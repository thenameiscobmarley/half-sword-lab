"""Find Half Sword on disk, copy the Lab DLL into it (with backups), manage save backups."""
from __future__ import annotations

import json
import os
import re
import shutil
import struct
import tarfile
import time
from dataclasses import dataclass
from pathlib import Path

HOME = Path.home()
REPO = Path(__file__).resolve().parent.parent
DLL_SRC = REPO / "native" / "dist" / "version.dll"
PROXY = "version.dll"
CONFIG = HOME / ".config" / "half-sword-lab" / "config.json"
BACKUPS = HOME / "HalfSwordLab-backups"
LAUNCH_OPTION = 'WINEDLLOVERRIDES="version=n,b" %command%'
MARKER = b"Half Sword Lab"  # string inside our DLL, used to tell it apart from a real version.dll

STEAM_ROOTS = [
    HOME / ".steam/steam",
    HOME / ".steam/root",
    HOME / ".local/share/Steam",
    HOME / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
    HOME / "snap/steam/common/.local/share/Steam",
]
WINDOWS = os.name == "nt"
if WINDOWS:
    STEAM_ROOTS = [Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Steam",
                   Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Steam"]


def load_config() -> dict:
    try:
        return json.loads(CONFIG.read_text())
    except (OSError, ValueError):
        return {}


def save_config(cfg: dict) -> None:
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps(cfg, indent=2))


def steam_libraries() -> list[Path]:
    libs: list[Path] = []
    for root in STEAM_ROOTS:
        vdf = root / "steamapps" / "libraryfolders.vdf"
        if root.is_dir():
            libs.append(root)
        if vdf.is_file():
            libs += [Path(p) for p in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(errors="replace"))]
    seen, out = set(), []
    for lib in libs:
        key = lib.resolve() if lib.exists() else lib
        if key not in seen and (lib / "steamapps").is_dir():
            seen.add(key)
            out.append(lib)
    return out


def pe_imports(exe: Path) -> set[str]:
    """Lower-case names of the DLLs an exe imports (normal + delay-load), read from its PE header."""
    names: set[str] = set()
    try:
        with exe.open("rb") as f:
            def read(off, n):
                f.seek(off)
                return f.read(n)
            pe = struct.unpack_from("<I", read(0x3C, 4))[0]
            nsec, optsize = struct.unpack_from("<H12xH", read(pe + 6, 16))
            opt = pe + 24
            dirs = opt + (112 if struct.unpack_from("<H", read(opt, 2))[0] == 0x20B else 96)
            sections = [struct.unpack_from("<8xIII I", read(opt + optsize + i * 40, 40)) for i in range(nsec)]

            def off(rva):
                for vsize, va, rawsize, raw in sections:
                    if va <= rva < va + max(vsize, rawsize):
                        return raw + rva - va
                return None

            def cstr(rva):
                o = off(rva)
                return read(o, 256).split(b"\0", 1)[0].decode("ascii", "replace").lower() if o else ""

            for entry, size, name_at in ((1, 20, 12), (13, 32, 4)):
                rva = struct.unpack_from("<I", read(dirs + entry * 8, 4))[0]
                o = off(rva) if rva else None
                while o:
                    desc = read(o, size)
                    if len(desc) < size or not any(desc):
                        break
                    names.add(cstr(struct.unpack_from("<I", desc, name_at)[0]))
                    o += size
    except (OSError, struct.error):
        pass
    return names


@dataclass
class GameInstall:
    root: Path
    library: Path | None = None
    appid: str | None = None

    @property
    def name(self) -> str:
        return self.root.name

    @property
    def exe(self) -> Path | None:
        hits = sorted(self.root.glob("*/Binaries/Win64/*-Win64-Shipping.exe"))
        return hits[0] if hits else None

    @property
    def win64(self) -> Path | None:
        return self.exe.parent if self.exe else None

    @property
    def project(self) -> str | None:
        return self.exe.parents[2].name if self.exe else None

    @property
    def lab_dir(self) -> Path | None:
        return self.win64 / "HalfSwordLab" if self.win64 else None

    @property
    def bridge_dir(self) -> Path | None:
        return self.lab_dir / "bridge" if self.lab_dir else None

    @property
    def dll_path(self) -> Path | None:
        return self.win64 / PROXY if self.win64 else None

    @property
    def injected(self) -> bool:
        p = self.dll_path
        return bool(p and p.exists() and MARKER in p.read_bytes())

    def loads_proxy(self) -> bool | None:
        return PROXY in pe_imports(self.exe) if self.exe else None

    @property
    def prefix(self) -> Path | None:
        if WINDOWS or not (self.library and self.appid):
            return None
        p = self.library / "steamapps" / "compatdata" / self.appid / "pfx"
        return p if p.is_dir() else None

    @property
    def local_appdata(self) -> Path | None:
        if WINDOWS:
            return Path(os.environ.get("LOCALAPPDATA", HOME / "AppData/Local"))
        return self.prefix / "drive_c/users/steamuser/AppData/Local" if self.prefix else None

    def save_dirs(self) -> list[Path]:
        local = self.local_appdata
        if not local or not local.is_dir():
            return []
        want = {(self.project or "").lower(), "halfsworduE5".lower()}
        return sorted(p for p in local.glob("*/Saved/SaveGames")
                      if p.is_dir() and (p.parts[-3].lower() in want or "halfsword" in p.parts[-3].lower()))

    # -- Wine/Proton DLL override (per game exe, inside the game's own prefix)
    @property
    def user_reg(self) -> Path | None:
        return self.prefix / "user.reg" if self.prefix else None

    def override_set(self) -> bool | None:
        if WINDOWS:
            return True  # Windows loads a version.dll next to the exe by itself
        reg = self.user_reg
        if not reg or not reg.exists() or not self.exe:
            return None
        return _reg_has_override(reg.read_text(errors="replace"), self.exe.name)

    def running(self) -> bool:
        if WINDOWS or not self.exe:
            return False
        name = self.exe.name.lower().encode()
        for cmd in Path("/proc").glob("[0-9]*/cmdline"):
            try:
                argv0 = cmd.read_bytes().split(b"\0", 1)[0].lower()
            except OSError:
                continue
            # Wine shows the Windows path of the exe as the program name.
            if argv0.replace(b"\\", b"/").rsplit(b"/", 1)[-1] == name:
                return True
        return False


def _manifests(lib: Path) -> list[tuple[str, str, str]]:
    out = []
    for acf in (lib / "steamapps").glob("appmanifest_*.acf"):
        text = acf.read_text(errors="replace")
        get = lambda k: (re.search(r'"%s"\s+"([^"]*)"' % k, text) or [None, ""])[1]
        out.append((get("appid"), get("name"), get("installdir")))
    return out


def find_games() -> list[GameInstall]:
    found: list[GameInstall] = []
    override = os.environ.get("HALFSWORD_DIR") or load_config().get("game_dir")
    if override and Path(override).is_dir():
        found.append(GameInstall(Path(override)))
    for lib in steam_libraries():
        common = lib / "steamapps" / "common"
        manifests = _manifests(lib)
        for appid, name, installdir in manifests:
            if "half sword" in name.lower() and (common / installdir).is_dir():
                found.append(GameInstall(common / installdir, lib, appid))
        if common.is_dir():
            for d in common.iterdir():
                if "halfsword" in d.name.lower().replace(" ", "") and not any(g.root == d for g in found):
                    appid = next((a for a, _, i in manifests if i == d.name), None)
                    found.append(GameInstall(d, lib, appid))
    found.sort(key=lambda g: (g.exe is None, "demo" in g.name.lower()))
    return found


# ---------------------------------------------------------------- Wine registry

def _reg_section(exe_name: str) -> str:
    return "[Software\\\\Wine\\\\AppDefaults\\\\%s\\\\DllOverrides]" % exe_name


def _reg_has_override(text: str, exe_name: str) -> bool:
    i = text.find(_reg_section(exe_name))
    if i < 0:
        return False
    block = text[i:].split("\n\n", 1)[0]
    return re.search(r'^"version"="native,builtin"$', block, re.M) is not None


def _reg_add_override(text: str, exe_name: str) -> str:
    sec = _reg_section(exe_name)
    i = text.find(sec)
    line = '"version"="native,builtin"'
    if i < 0:
        return text.rstrip("\n") + f"\n\n{sec} {int(time.time())}\n{line}\n"
    end = text.find("\n\n", i)
    end = len(text) if end < 0 else end
    block = re.sub(r'^"version"=.*\n?', "", text[i:end], flags=re.M).rstrip("\n")
    return text[:i] + block + "\n" + line + text[end:]


def _reg_remove_override(text: str, exe_name: str) -> str:
    sec = _reg_section(exe_name)
    i = text.find(sec)
    if i < 0:
        return text
    end = text.find("\n\n", i)
    end = len(text) if end < 0 else end
    block = re.sub(r'^"version"=.*\n?', "", text[i:end], flags=re.M).rstrip("\n")
    if re.search(r'^"', block, re.M):
        return text[:i] + block + text[end:]
    # Nothing else in the section: drop it entirely.
    rest = text[end:].lstrip("\n")
    return text[:i].rstrip("\n") + ("\n\n" + rest if rest else "\n")


# ---------------------------------------------------------------- backups

def _now() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


def _write_manifest(archive: Path, **info) -> None:
    info.setdefault("created", time.strftime("%Y-%m-%dT%H:%M:%S"))
    archive.with_name(archive.name.split(".tar.gz")[0] + ".json").write_text(json.dumps(info, indent=2))


def backup_saves(game: GameInstall | None, reason: str = "manual") -> Path | None:
    dirs = game.save_dirs() if game else []
    if not dirs:
        return None
    BACKUPS.mkdir(parents=True, exist_ok=True)
    out = BACKUPS / f"saves-{_now()}-{reason}.tar.gz"
    with tarfile.open(out, "w:gz") as tar:
        for d in dirs:
            tar.add(d, arcname=str(d.relative_to(d.parents[2])))
    _write_manifest(out, kind="saves", reason=reason, format="tar", game=str(game.root))
    return out


def backup_files(files: list[Path], reason: str, game: GameInstall | None) -> Path | None:
    """Back up specific game files (absolute paths are recorded so they can be restored)."""
    files = [f for f in files if f.exists()]
    if not files:
        return None
    BACKUPS.mkdir(parents=True, exist_ok=True)
    out = BACKUPS / f"gamefiles-{_now()}-{reason}.tar.gz"
    entries = []
    with tarfile.open(out, "w:gz") as tar:
        for i, f in enumerate(files):
            arc = f"{i}_{f.name}"
            tar.add(f, arcname=arc)
            entries.append({"arc": arc, "dest": str(f)})
    _write_manifest(out, kind="gamefiles", reason=reason, format="tar", files=entries,
                    game=str(game.root) if game else "")
    return out


@dataclass
class Backup:
    path: Path        # .tar.gz file or folder
    kind: str         # saves / gamefiles / demo
    reason: str
    created: float    # epoch seconds
    fmt: str
    info: dict

    @property
    def size(self) -> int:
        if self.path.is_dir():
            return sum(f.stat().st_size for f in self.path.rglob("*") if f.is_file())
        return self.path.stat().st_size if self.path.exists() else 0


def list_backups(folder: Path | None = None) -> list[Backup]:
    """All backups, newest first."""
    folder = folder or BACKUPS
    out: list[Backup] = []
    if not folder.is_dir():
        return out
    for item in folder.iterdir():
        if item.suffix == ".json" or item.name.startswith("."):
            continue
        stem = item.name.split(".tar.gz")[0]
        meta = folder / f"{stem}.json"
        try:
            info = json.loads(meta.read_text()) if meta.exists() else {}
        except ValueError:
            info = {}
        created = item.stat().st_mtime
        if "created" in info:
            try:
                created = time.mktime(time.strptime(info["created"], "%Y-%m-%dT%H:%M:%S"))
            except ValueError:
                pass
        kind = info.get("kind") or stem.split("-", 1)[0]
        parts = stem.split("-")
        reason = info.get("reason") or (parts[3] if len(parts) > 3 else "manual")
        out.append(Backup(item, kind, reason, created, info.get("format", "dir" if item.is_dir() else "tar"), info))
    out.sort(key=lambda b: b.created, reverse=True)
    return out


def restore_backup(b: Backup, game: GameInstall | None) -> str:
    if b.kind == "saves":
        dest = game.local_appdata if game else None
        if not dest:
            return "Can't restore saves: the game's AppData folder wasn't found."
        if game and game.running():
            return "Quit Half Sword before restoring saves."
        backup_saves(game, "pre-restore")
        if b.fmt == "tar":
            with tarfile.open(b.path) as tar:
                tar.extractall(dest, filter="data")
        else:
            shutil.copytree(b.path, dest, dirs_exist_ok=True)
        return f"Saves restored into {dest}"
    if b.kind == "gamefiles":
        if game and game.running():
            return "Quit Half Sword before restoring game files."
        with tarfile.open(b.path) as tar:
            for e in b.info.get("files", []):
                member = tar.getmember(e["arc"])
                data = tar.extractfile(member)
                if data:
                    Path(e["dest"]).write_bytes(data.read())
        return "Game files restored: " + ", ".join(Path(e["dest"]).name for e in b.info.get("files", []))
    return "Nothing to restore for this backup."


def delete_backup(b: Backup) -> None:
    if b.path.is_dir():
        shutil.rmtree(b.path)
    else:
        b.path.unlink(missing_ok=True)
    meta = b.path.with_name(b.path.name.split(".tar.gz")[0] + ".json")
    meta.unlink(missing_ok=True)


# ---------------------------------------------------------------- inject / remove

def _winpath(p: Path) -> str:
    return str(p) if WINDOWS else "Z:" + str(p).replace("/", "\\")


def inject(game: GameInstall, log=print) -> None:
    """Copy the Lab DLL into the game folder. Anything it replaces is backed up first."""
    if not DLL_SRC.exists():
        raise RuntimeError(f"{DLL_SRC} is missing (build it with native/build.sh)")
    if not game.win64:
        raise RuntimeError("Game binary folder not found")
    if game.running():
        raise RuntimeError("Quit Half Sword first (Wine rewrites its registry when the game exits)")
    touched = [p for p in (game.dll_path, game.user_reg) if p and p.exists() and not (p == game.dll_path and game.injected)]
    b = backup_files(touched, "pre-inject", game)
    if b:
        log(f"Backed up {', '.join(p.name for p in touched)} -> {b.name}")
    shutil.copy2(DLL_SRC, game.dll_path)
    game.lab_dir.mkdir(exist_ok=True)
    game.bridge_dir.mkdir(exist_ok=True)
    (game.lab_dir / "lab.ini").write_text(
        "; Half Sword Lab settings\n"
        "overlay=1\n"
        "; in-game menu hotkey (virtual-key code). 0xDC is the \\ key.\n"
        "toggle_key=0xDC\n"
        f"backup_dir={_winpath(BACKUPS)}\n")
    (game.lab_dir / "install.json").write_text(json.dumps({
        "installed": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "replaced_dll": bool(touched and game.dll_path in touched),
        "backup": str(b) if b else None,
    }, indent=2))
    if game.user_reg and game.user_reg.exists():
        text = game.user_reg.read_text(errors="replace")
        if not _reg_has_override(text, game.exe.name):
            game.user_reg.write_text(_reg_add_override(text, game.exe.name))
            log(f"Proton prefix: {PROXY} set to native for {game.exe.name}")
    elif not WINDOWS:
        log(f"No Proton prefix yet (start the game once), or use the launch option {LAUNCH_OPTION}")
    log(f"Copied {PROXY} into {game.win64}")


def remove_injection(game: GameInstall, log=print) -> None:
    if game.running():
        raise RuntimeError("Quit Half Sword first")
    info = {}
    try:
        info = json.loads((game.lab_dir / "install.json").read_text())
    except (OSError, ValueError, TypeError):
        pass
    if game.injected:
        game.dll_path.unlink()
        log(f"Removed {PROXY}")
    archive = Path(info.get("backup") or "")
    if info.get("replaced_dll") and archive.is_file():
        manifest = json.loads(archive.with_name(archive.name.split(".tar.gz")[0] + ".json").read_text())
        with tarfile.open(archive) as tar:
            for e in manifest.get("files", []):
                if Path(e["dest"]).name == PROXY:
                    game.dll_path.write_bytes(tar.extractfile(tar.getmember(e["arc"])).read())
                    log(f"Restored the original {PROXY}")
    if game.user_reg and game.user_reg.exists():
        text = game.user_reg.read_text(errors="replace")
        if _reg_has_override(text, game.exe.name):
            game.user_reg.write_text(_reg_remove_override(text, game.exe.name))
            log("Removed the Proton DLL override")
    if game.lab_dir and game.lab_dir.exists():
        shutil.rmtree(game.lab_dir)
        log("Removed the HalfSwordLab folder")

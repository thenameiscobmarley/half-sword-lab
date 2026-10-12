"""Find Half Sword on disk, install UE4SS + the Lab mod, back up saves."""
from __future__ import annotations

import io
import json
import os
import re
import shutil
import tarfile
import time
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

HOME = Path.home()
REPO = Path(__file__).resolve().parent.parent
MOD_SRC = REPO / "mod" / "HalfSwordLab"
CONFIG = HOME / ".config" / "half-sword-lab" / "config.json"
BACKUPS = HOME / "HalfSwordLab-backups"
LAUNCH_OPTION = 'WINEDLLOVERRIDES="dwmapi=n,b" %command%'
UE4SS_API = "https://api.github.com/repos/UE4SS-RE/RE-UE4SS/releases"

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
    def ue4ss_installed(self) -> bool:
        w = self.win64
        return bool(w) and (w / "dwmapi.dll").exists() and (
            (w / "ue4ss" / "UE4SS.dll").exists() or (w / "UE4SS.dll").exists())

    @property
    def mods_dir(self) -> Path | None:
        w = self.win64
        if not w:
            return None
        return w / "ue4ss" / "Mods" if (w / "ue4ss").is_dir() else w / "Mods"

    @property
    def mod_dir(self) -> Path | None:
        return self.mods_dir / "HalfSwordLab" if self.mods_dir else None

    @property
    def bridge_dir(self) -> Path | None:
        return self.mod_dir / "bridge" if self.mod_dir else None

    @property
    def mod_deployed(self) -> bool:
        return bool(self.mod_dir) and (self.mod_dir / "Scripts" / "main.lua").exists()

    @property
    def prefix(self) -> Path | None:
        if not (self.library and self.appid):
            return None
        p = self.library / "steamapps" / "compatdata" / self.appid / "pfx"
        return p if p.is_dir() else None

    def save_dirs(self) -> list[Path]:
        if WINDOWS:
            local = Path(os.environ.get("LOCALAPPDATA", HOME / "AppData/Local"))
        elif self.prefix:
            local = self.prefix / "drive_c/users/steamuser/AppData/Local"
        else:
            return []
        return sorted(p for p in local.glob("*/Saved/SaveGames") if p.is_dir() and "halfsword" in p.parts[-3].lower().replace(" ", ""))

    def launch_option_set(self) -> bool | None:
        """True/False if Steam's localconfig says so, None if it can't be told."""
        if WINDOWS:
            return True  # dwmapi.dll is picked up natively, no override needed
        if not self.appid:
            return None
        for root in STEAM_ROOTS:
            for cfg in root.glob("userdata/*/config/localconfig.vdf"):
                text = cfg.read_text(errors="replace")
                m = re.search(r'"%s"\s*\{' % re.escape(self.appid), text)
                if m:
                    block = text[m.end(): m.end() + 4000]
                    lo = re.search(r'"LaunchOptions"\s+"((?:[^"\\]|\\.)*)"', block.split("\n\t\t\t\t}", 1)[0])
                    return bool(lo and "dwmapi" in lo.group(1))
        return None


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
    # Prefer installs that actually contain the game binary; full game before demo.
    found.sort(key=lambda g: (g.exe is None, "demo" in g.name.lower()))
    return found


# ---------------------------------------------------------------- UE4SS

def _get_json(url: str):
    req = urllib.request.Request(url, headers={"User-Agent": "half-sword-lab", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def ue4ss_release(channel: str = "experimental") -> tuple[str, str]:
    """(asset name, download url) for the newest UE4SS build."""
    urls = [f"{UE4SS_API}/tags/experimental-latest", f"{UE4SS_API}/latest"]
    if channel == "stable":
        urls.reverse()
    for url in urls:
        try:
            data = _get_json(url)
        except OSError:
            continue
        for a in data.get("assets", []):
            n = a["name"]
            if n.startswith("UE4SS_v") and n.endswith(".zip"):
                return n, a["browser_download_url"]
    raise RuntimeError("Could not find a UE4SS release on GitHub")


KEEP_IF_PRESENT = ("UE4SS-settings.ini", "mods.txt", "mods.json")


def install_ue4ss(game: GameInstall, channel: str = "experimental", log=print) -> None:
    """Download UE4SS into Binaries/Win64. Never overwrites existing settings or mods."""
    w = game.win64
    if not w:
        raise RuntimeError("Game binary folder not found")
    name, url = ue4ss_release(channel)
    log(f"Downloading {name} ...")
    req = urllib.request.Request(url, headers={"User-Agent": "half-sword-lab"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for m in z.infolist():
            dest = (w / m.filename).resolve()
            if not dest.is_relative_to(w.resolve()):
                raise RuntimeError(f"Unsafe path in archive: {m.filename}")
            if m.is_dir():
                dest.mkdir(parents=True, exist_ok=True)
                continue
            rel = Path(m.filename)
            in_mods = "Mods" in rel.parts[:-1]
            if dest.exists() and (rel.name in KEEP_IF_PRESENT or in_mods):
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(z.read(m))
    log(f"UE4SS installed into {w}")


def deploy_mod(game: GameInstall, log=print) -> None:
    mods = game.mods_dir
    if not mods:
        raise RuntimeError("Game binary folder not found")
    dest = mods / "HalfSwordLab"
    (dest / "Scripts").mkdir(parents=True, exist_ok=True)
    (dest / "bridge").mkdir(exist_ok=True)
    shutil.copy2(MOD_SRC / "Scripts" / "main.lua", dest / "Scripts" / "main.lua")
    (dest / "enabled.txt").touch()

    txt = mods / "mods.txt"
    if txt.exists():
        lines = txt.read_text(errors="replace").splitlines()
        if not any(l.split(":")[0].strip() == "HalfSwordLab" for l in lines):
            lines.insert(0, "HalfSwordLab : 1")
            txt.write_text("\n".join(lines) + "\n")
    js = mods / "mods.json"
    if js.exists():
        try:
            entries = json.loads(js.read_text())
            if isinstance(entries, list) and not any(e.get("mod_name") == "HalfSwordLab" for e in entries):
                entries.insert(0, {"mod_name": "HalfSwordLab", "mod_enabled": True})
                js.write_text(json.dumps(entries, indent=4))
        except ValueError:
            log("mods.json is not valid JSON, left it alone (enabled.txt still turns the mod on)")
    log(f"Lab mod deployed to {dest}")


# ---------------------------------------------------------------- saves

def backup_saves(game: GameInstall, reason: str = "manual") -> Path | None:
    dirs = game.save_dirs()
    if not dirs:
        return None
    BACKUPS.mkdir(parents=True, exist_ok=True)
    out = BACKUPS / f"saves-{time.strftime('%Y%m%d-%H%M%S')}-{reason}.tar.gz"
    with tarfile.open(out, "w:gz") as tar:
        for d in dirs:
            tar.add(d, arcname=str(d.relative_to(d.parents[2])))
    return out


def list_backups() -> list[Path]:
    return sorted(BACKUPS.glob("saves-*.tar.gz"), reverse=True) if BACKUPS.is_dir() else []

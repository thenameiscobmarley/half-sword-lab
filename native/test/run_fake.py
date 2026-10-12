#!/usr/bin/env python3
"""Run the Lab DLL inside the fake UE5 game under Wine and check every feature end to end.

    python3 native/test/run_fake.py <work-dir> [--menu]

--menu also opens the in-game menu ('\\') and saves a screenshot to <work-dir>/menu.bmp.
--d3d12 renders the fake game with D3D12 (UE5's default) instead of D3D11; screenshot goes to menu12.bmp.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent


def winpath(p: Path) -> str:
    return "Z:" + str(p).replace("/", "\\")


def build_fake() -> None:
    exe = HERE / "fakeue.exe"
    if not exe.exists() or exe.stat().st_mtime < (HERE / "fakeue.cpp").stat().st_mtime:
        subprocess.run(["x86_64-w64-mingw32-g++", "-O1", "-std=c++17", "-static", "-o", str(exe), str(HERE / "fakeue.cpp"),
                        "-ld3d11", "-ld3d12", "-ldxgi", "-lversion", "-luuid", "-ldxguid"], check=True)


def main() -> int:
    build_fake()
    work = Path(sys.argv[1]).resolve()
    menu = "--menu" in sys.argv
    d3d12 = "--d3d12" in sys.argv
    game = work / "FakeGame" / "Binaries" / "Win64"
    lab = game / "HalfSwordLab"
    bridge = lab / "bridge"
    shutil.rmtree(work / "FakeGame", ignore_errors=True)
    bridge.mkdir(parents=True)
    shutil.copy2(HERE / "fakeue.exe", game / "FakeGame-Win64-Shipping.exe")
    shutil.copy2(NATIVE / "dist" / "version.dll", game / "version.dll")
    (lab / "lab.ini").write_text(f"overlay=1\ntoggle_key=0xDC\nbackup_dir={winpath(work / 'backups')}\n")

    env = dict(os.environ, WINEPREFIX=str(work / "prefix"), WINEDEBUG="-all",
               WINEDLLOVERRIDES="version=n,b;mscoree=;mshtml=", HSL_SECONDS="45")
    if menu:
        env.update(HSL_MENU_AT="12", HSL_SCREENSHOT=winpath(work / "menu.bmp"))
    if "--click" in sys.argv:
        env.update(HSL_CLICK=sys.argv[sys.argv.index("--click") + 1])
        saves = work / "prefix" / "drive_c" / "users" / os.environ.get("USER", "user") / "AppData" / "Local" / "FakeGame" / "Saved" / "SaveGames"
        saves.mkdir(parents=True, exist_ok=True)
        (saves / "Slot1.sav").write_bytes(b"GVAS fake save")
    if d3d12:
        env.update(HSL_D3D12="1", HSL_SHOT12=winpath(work / "menu12.bmp"))
    proc = subprocess.Popen(["wine", "FakeGame-Win64-Shipping.exe"], cwd=game, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def state(timeout=90):
        end = time.time() + timeout
        while time.time() < end:
            try:
                return json.loads((bridge / "state.json").read_text())
            except (OSError, ValueError):
                time.sleep(0.5)
        return None

    s = state()
    if not s:
        proc.kill()
        print("FAIL: no state.json; lab.log:\n" + (lab / "lab.log").read_text(errors="replace") if (lab / "lab.log").exists() else "FAIL: no lab.log either")
        return 1
    chars = {c["id"]: c for c in s["characters"]}
    print("characters:", [(c["name"], c["role"], [g["name"] for g in c["gear"]]) for c in chars.values()])
    print("economy:", s["economy"])
    print("classes:", {k: [c["name"] for c in v] for k, v in s["classes"].items()})

    time.sleep(3.5)  # let the gear scan run
    s = state()
    role = lambda r: next(c for c in s["characters"] if c["role"] == r)
    merc, opp = role("mercenary"), role("opponent")
    helmet = next(g for g in opp["gear"] if g["kind"] == "armor")
    merc_helmet = next(g for g in merc["gear"] if g["kind"] == "armor")
    gold = next(e for e in s["economy"] if e["n"] == "Gold")

    (bridge / "mods.cfg").write_text("random_sizes=1\nrandom_min=0.5\nrandom_max=0.6\ngravity=moon\nmatrix=1\n"
                                     "feather_opp=1\ngiant_weapons=1\nweapon_factor=2.5\n")
    base = int(time.time() * 1000)
    cmds = [
        ("set", gold["owner_id"], "Gold", "99999"),
        ("scale", merc["id"], "3"),
        ("clone", opp["id"], "2"),
        ("launch", "all", "2000"),
        ("hide", helmet["id"]),
        ("scale", merc_helmet["id"], "2.5"),
        ("set", role("player")["id"], "Health", "250"),
        ("dump",),
    ]
    (bridge / "commands.txt").write_text("".join("\t".join([str(base + i), *c]) + "\n" for i, c in enumerate(cmds)))
    time.sleep(6)
    s = state()
    print("after commands:", [(c["name"], c["role"], round(c["scale"], 2), round(c["mass"], 2), round(c["speed"], 2)) for c in s["characters"]])
    print("gravity now:", s["world"]["gravity"])
    print("log:", [l["msg"] for l in s["log"]][-10:])
    print("dump written:", (bridge / "dump.txt").exists(), (bridge / "dump.txt").stat().st_size if (bridge / "dump.txt").exists() else 0)

    out, _ = proc.communicate(timeout=120)
    print("---- fake game output\n" + out.strip())
    print("---- lab.log\n" + (lab / "lab.log").read_text(errors="replace").strip())
    if menu:
        backups = sorted((work / "backups").glob("saves-*")) if (work / "backups").exists() else []
        print("in-game backups:", [b.name for b in backups])
        print("menu screenshot:", (work / ("menu12.bmp" if d3d12 else "menu.bmp")).exists())
    return 0


if __name__ == "__main__":
    sys.exit(main())

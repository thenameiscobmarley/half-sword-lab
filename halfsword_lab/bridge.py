"""File bridge between the terminal GUI and the in-game Lua mod."""
from __future__ import annotations

import json
import os
import time
from collections import deque
from pathlib import Path

STALE_AFTER = 6  # seconds without a state write = game not running


class Bridge:
    demo = False

    def __init__(self, folder: Path):
        self.folder = folder
        self.folder.mkdir(parents=True, exist_ok=True)
        self._state: dict | None = None
        self._mtime = 0.0
        self._sent: deque[str] = deque(maxlen=60)
        self._last_id = 0

    # -- state from the game ------------------------------------------------
    def read_state(self) -> tuple[dict | None, bool]:
        path = self.folder / "state.json"
        try:
            mtime = path.stat().st_mtime
            if mtime != self._mtime:
                self._state = json.loads(path.read_text(encoding="utf-8", errors="replace"))
                self._mtime = mtime
        except (OSError, ValueError):
            pass  # mid-write or not there yet; keep the last good state
        live = self._state is not None and time.time() - self._mtime < STALE_AFTER
        return self._state, live

    # -- commands to the game -----------------------------------------------
    def send(self, verb: str, *args) -> None:
        cid = max(time.time_ns() // 1_000_000, self._last_id + 1)
        self._last_id = cid
        clean = [str(a).replace("\t", " ").replace("\n", " ") for a in args]
        self._sent.append("\t".join([str(cid), verb, *clean]))
        self._atomic("commands.txt", "\n".join(self._sent) + "\n")

    # -- mod toggles --------------------------------------------------------
    def read_mods(self) -> dict[str, str]:
        out = {}
        try:
            for line in (self.folder / "mods.cfg").read_text().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    out[k.strip()] = v.strip()
        except OSError:
            pass
        return out

    def write_mods(self, cfg: dict[str, str]) -> None:
        self._atomic("mods.cfg", "".join(f"{k}={v}\n" for k, v in sorted(cfg.items())))

    def _atomic(self, name: str, text: str) -> None:
        tmp = self.folder / (name + ".tmp")
        tmp.write_text(text)
        os.replace(tmp, self.folder / name)

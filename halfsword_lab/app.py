"""Half Sword Lab terminal GUI (Textual)."""
from __future__ import annotations

import io
import json
import shutil
import subprocess
import tarfile
import time
from pathlib import Path
from typing import Callable

from rich.text import Text
from textual import work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Grid, Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.theme import Theme
from textual.widgets import (Button, DataTable, Footer, Header, Input, Label, OptionList, RichLog, Select,
                             Static, Switch, TabbedContent, TabPane)
from textual.widgets.option_list import Option

from . import game as g
from .bridge import Bridge
from .demo import DemoBridge
from .mods import EXCLUSIVE, GRAVITY, MODS
from .tags import CYAN, DIM, MUTED, VIOLET, char_tags, group_of, tag, tagged

THEME = Theme(
    name="ui-vault", primary=VIOLET, secondary=CYAN, accent=CYAN, background="#08080d", surface="#12121b",
    panel="#181824", foreground="#ececf4", warning="#facc15", error="#f43f5e", success="#34d399", dark=True,
)

BODY = [
    ("scale", "Size", [("Pocket", 0.35), ("Small", 0.7), ("Normal", 1), ("Big", 1.5), ("Giant", 3)]),
    ("mass", "Weight", [("Feather", 0.15), ("Light", 0.5), ("Normal", 1), ("Heavy", 2.5), ("Anvil", 6)]),
    ("speed", "Speed", [("Slug", 0.4), ("Normal", 1), ("Quick", 1.5), ("Blitz", 2.5)]),
]


def human_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.0f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def human_age(t: float) -> str:
    s = max(0, time.time() - t)
    for size, unit in ((86400, "d"), (3600, "h"), (60, "min")):
        if s >= size:
            return f"{int(s // size)} {unit} ago"
    return "just now"


class BackupPrompt(ModalScreen[str]):
    """Asked before every change: back up first, apply without a backup, or cancel."""
    BINDINGS = [Binding("y", "pick('yes')", "Yes"), Binding("n", "pick('no')", "No"),
                Binding("escape", "pick('cancel')", "Cancel")]

    def __init__(self, change: str):
        super().__init__()
        self.change = change

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog"):
            yield Label(Text("Make Backup Before Applying Changes?", style=f"bold {CYAN}"))
            yield Static(Text.assemble(("Change: ", MUTED), (self.change, "bold")), classes="change")
            with Horizontal(classes="row"):
                yield Button("Yes, back up first (Y)", id="yes", variant="primary")
                yield Button("No, just apply (N)", id="no")
                yield Button("Cancel (Esc)", id="cancel")

    def on_button_pressed(self, e: Button.Pressed) -> None:
        self.dismiss(e.button.id)

    def action_pick(self, choice: str) -> None:
        self.dismiss(choice)


class EditValue(ModalScreen[str | None]):
    def __init__(self, title: str, value, kind: str):
        super().__init__()
        self.title_text, self.value, self.kind = title, value, kind

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog"):
            yield Label(Text.assemble(("Edit  ", MUTED), (self.title_text, f"bold {CYAN}")))
            if self.kind == "bool":
                with Horizontal(classes="row"):
                    yield Button("true", id="v-true", variant="success")
                    yield Button("false", id="v-false", variant="error")
                    yield Button("Cancel", id="cancel")
            else:
                yield Input(str(self.value), id="value", type="number")
                with Horizontal(classes="row"):
                    yield Button("Save", id="save", variant="primary")
                    yield Button("Cancel", id="cancel")

    def on_button_pressed(self, e: Button.Pressed) -> None:
        if e.button.id == "save":
            self.dismiss(self.query_one("#value", Input).value)
        elif e.button.id in ("v-true", "v-false"):
            self.dismiss(e.button.id[2:])
        else:
            self.dismiss(None)

    def on_input_submitted(self, e: Input.Submitted) -> None:
        self.dismiss(e.value)

    def key_escape(self) -> None:
        self.dismiss(None)


class Confirm(ModalScreen[bool]):
    def __init__(self, question: str):
        super().__init__()
        self.question = question

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog"):
            yield Label(self.question)
            with Horizontal(classes="row"):
                yield Button("Yes", id="yes", variant="error")
                yield Button("No", id="no")

    def on_button_pressed(self, e: Button.Pressed) -> None:
        self.dismiss(e.button.id == "yes")

    def key_escape(self) -> None:
        self.dismiss(False)


class LabApp(App):
    TITLE = "⚔  HALF SWORD LAB"
    CSS_PATH = "lab.tcss"
    BINDINGS = [
        Binding("q", "quit", "Quit"),
        Binding("b", "backup", "Backup saves"),
        Binding("d", "dump", "Dump for Claude"),
        Binding("f8", "panic", "Panic reset"),
    ]

    def __init__(self, demo: bool = False):
        super().__init__()
        self.demo = demo
        self.backups_dir = g.BACKUPS / "demo" if demo else g.BACKUPS
        self.game: g.GameInstall | None = None
        self.bridge: Bridge | DemoBridge | None = None
        self.state: dict | None = None
        self.live = False
        self.selected: str | None = None
        self.sel_gear: dict[str, str | None] = {"armor": None, "weapons": None}
        self.sel_backup: str | None = None
        self.cfg: dict[str, str] = {}
        self.sigs: dict[str, object] = {}
        self.actions: dict[str, Callable] = {}
        self.log_seq = 0
        self.backed_up = False

    # ------------------------------------------------------------------ layout
    def btn(self, label: str, bid: str, fn, variant: str = "default", classes: str = "") -> Button:
        self.actions[bid] = fn
        return Button(label, id=bid, variant=variant, classes=classes)

    def compose(self) -> ComposeResult:
        yield Header()
        with TabbedContent(id="main"):
            with TabPane("Characters", id="tab-chars"):
                with Horizontal():
                    with Vertical(id="left"):
                        yield Static(Text("CHARACTERS", style=f"bold {MUTED}"), classes="h")
                        yield OptionList(id="chars")
                        yield Static("", id="world")
                    with Vertical(id="right"):
                        yield Static("", id="char-title")
                        with TabbedContent(id="char-tabs"):
                            with TabPane("Body", id="t-body"):
                                yield from self.compose_body()
                            with TabPane("Stats", id="t-stats"):
                                yield Input(placeholder="filter stats (health, team, armor...)", id="stat-filter")
                                yield DataTable(id="stats", cursor_type="row", zebra_stripes=True)
                                yield Static("Enter on a stat to edit it. Values are live from the game.", classes="hint")
                            with TabPane("Armor", id="t-armor"):
                                yield from self.compose_gear("armor", "armor")
                            with TabPane("Weapons", id="t-weapons"):
                                yield from self.compose_gear("weapons", "weapon")
                            with TabPane("Coins", id="t-coins"):
                                yield from self.compose_coins()
                            with TabPane("Goofy", id="t-goofy"):
                                yield from self.compose_goofy()
            with TabPane("Mods", id="tab-mods"):
                yield from self.compose_mods()
            with TabPane("Backups", id="tab-backups"):
                yield from self.compose_backups()
            with TabPane("Setup", id="tab-setup"):
                yield from self.compose_setup()
            with TabPane("Log", id="tab-log"):
                yield RichLog(id="log", wrap=True, markup=False)
        yield Footer()

    def compose_body(self) -> ComposeResult:
        with VerticalScroll():
            yield Static("", id="body-info")
            for verb, label, presets in BODY:
                with Horizontal(classes="row"):
                    yield Label(label, classes="lbl")
                    yield Input(placeholder="1.0", id=f"in-{verb}", classes="num", type="number", valid_empty=True)
                    yield self.btn("Set", f"set-{verb}", lambda v=verb: self.apply_input(v), "primary")
                    for name, val in presets:
                        yield self.btn(name, f"p-{verb}-{name.lower()}",
                                       lambda v=verb, x=val, n=name, l=label: self.char_cmd(f"{l}: {n}", v, x))
            yield Static("Weight is a physics mass multiplier: heavier hits harder and is harder to push. "
                         "Speed is per-character time, so 2.0 means twice as fast at everything.", classes="hint")
            with Horizontal(classes="row"):
                yield self.btn("Reset this character", "reset-char", lambda: self.char_cmd("Reset character", "reset"),
                               "warning")

    def compose_gear(self, key: str, kind: str) -> ComposeResult:
        yield DataTable(id=f"gear-{key}", cursor_type="row", zebra_stripes=True)
        with Horizontal(classes="row"):
            big = 2.5 if kind == "armor" else 3
            yield self.btn("Giant", f"{key}-huge", lambda: self.gear_cmd(key, "Giant", "scale", big))
            yield self.btn("Tiny", f"{key}-tiny", lambda: self.gear_cmd(key, "Tiny", "scale", 0.4))
            if kind == "weapon":
                yield self.btn("Feather", f"{key}-feather", lambda: self.gear_cmd(key, "Feather", "mass", 0.1))
                yield self.btn("Heavy", f"{key}-heavy", lambda: self.gear_cmd(key, "Heavy", "mass", 5))
            yield self.btn("Normal", f"{key}-normal", lambda: self.gear_cmd(key, "Normal size", "scale", 1))
            yield self.btn("Hide", f"{key}-hide", lambda: self.gear_cmd(key, "Hide", "hide"))
            yield self.btn("Show", f"{key}-show", lambda: self.gear_cmd(key, "Show", "show"))
            yield self.btn("Remove", f"{key}-vanish", lambda: self.gear_cmd(key, "Remove", "vanish"), "error")
        with Horizontal(classes="row"):
            yield Select([], prompt=f"spawn {kind}...", id=f"spawn-{key}", classes="spawn")
            yield Input("1", id=f"count-{key}", classes="count", type="integer", valid_empty=True)
            yield self.btn("Spawn near", f"{key}-spawn", lambda: self.spawn(key), "primary")
        yield Static(f"{kind.title()} on this character. Remove deletes the piece, Hide only hides it. "
                     "Spawned items drop next to the character.", classes="hint")

    def compose_coins(self) -> ComposeResult:
        yield DataTable(id="coins", cursor_type="row", zebra_stripes=True)
        with Horizontal(classes="row"):
            yield self.btn("+100", "coin-100", lambda: self.coin_add(100))
            yield self.btn("+1,000", "coin-1000", lambda: self.coin_add(1000))
            yield self.btn("Set 99,999", "coin-max", lambda: self.coin_set(99999), "primary")
            yield self.btn("Edit...", "coin-edit", self.coin_edit)
        yield Static("Coin values are found by name (gold, coins, money, florins...) on the game instance, "
                     "game state, player and save objects. Save in-game afterwards to keep them.", classes="hint")

    def compose_goofy(self) -> ComposeResult:
        with Grid(id="goofy"):
            for i, (label, verb, *args) in enumerate([
                ("🚀 Launch to the sky", "launch", 2500), ("🧬 Clone", "clone", 1), ("👥 Clone army (x6)", "clone", 6),
                ("🧲 Bring to me", "bring"), ("🌀 Teleport me there", "visit"), ("🗿 Freeze", "speed", 0.02),
                ("☕ Hyper", "speed", 3), ("🐜 Ant", "scale", 0.2), ("🏔 Colossus", "scale", 4),
                ("🎈 Balloon (weightless)", "mass", 0.05), ("👻 Invisible", "hide"), ("👁 Visible", "show"),
            ]):
                yield self.btn(label, f"g-{i}", lambda l=label, v=verb, a=args: self.char_cmd(l[2:], v, *a))
            yield self.btn("💨 Vanish", "g-vanish", self.vanish_char, "error")

    def compose_mods(self) -> ComposeResult:
        with VerticalScroll(id="mods"):
            for m in MODS:
                with Horizontal(classes="mod"):
                    yield Switch(id=f"mod-{m.key}")
                    with Vertical(classes="mod-text"):
                        yield Static(tagged(m.title, m.tags))
                        yield Static(Text(m.desc, style=MUTED))
                    for pk, plabel, default in m.params:
                        yield Label(plabel, classes="plabel")
                        yield Input(default, id=f"param-{pk}", classes="param", type="number", valid_empty=True)
            with Horizontal(classes="mod"):
                with Vertical(classes="mod-text"):
                    yield Static(tagged("Gravity", ["world", "chaos"]))
                    yield Static(Text("Changes gravity for the whole world. Reverse makes everything fall up.",
                                      style=MUTED))
                yield Select(GRAVITY, value="normal", allow_blank=False, id="gravity")
                yield Label("custom", classes="plabel")
                yield Input("-980", id="param-gravity_custom", classes="param", type="number", valid_empty=True)
            yield Static("Numbers apply when you press Enter in their box.", classes="hint")
            with Horizontal(classes="row"):
                yield self.btn("🚀 Launch everyone", "launch-all",
                               lambda: self.confirm("Launch everyone", lambda: self.send("launch", "all", 2200)))
                yield self.btn("🛑 Panic reset (F8)", "panic", self.action_panic, "error")

    def compose_backups(self) -> ComposeResult:
        yield Static("", id="backup-head")
        yield DataTable(id="backups", cursor_type="row", zebra_stripes=True)
        with Horizontal(classes="row"):
            yield self.btn("Back up saves now", "bk-now", self.action_backup, "primary")
            yield self.btn("Restore selected", "bk-restore", self.restore_selected, "warning")
            yield self.btn("Delete selected", "bk-delete", self.delete_selected, "error")
            yield self.btn("Open backups folder", "bk-open", self.open_backups)
        yield Static("Restoring always makes a safety backup of the current saves first. "
                     "Game-file backups hold anything the DLL install replaced.", classes="hint")

    def compose_setup(self) -> ComposeResult:
        with VerticalScroll():
            yield Static("", id="setup-status")
            with Horizontal(classes="row"):
                yield self.btn("Copy DLL to game files (with backup)", "s-inject", self.inject, "primary")
                yield self.btn("Remove DLL (restore originals)", "s-remove", self.remove, "warning")
                yield self.btn("Re-detect game", "s-detect", self.detect)
            with Horizontal(classes="row"):
                yield self.btn("Dump game data for Claude", "s-dump", self.action_dump)
                yield self.btn("Rescan spawnable items", "s-rescan", lambda: self.send("scan_classes"))
                yield self.btn("Copy launch option (alternative)", "s-launch", self.copy_launch)
            with Horizontal(classes="row"):
                yield Input(placeholder="game folder, if not found automatically", id="game-path")
                yield self.btn("Use this folder", "s-path", self.use_path)
            yield Static(self.setup_help(), classes="hint")

    # ------------------------------------------------------------------ lifecycle
    def on_mount(self) -> None:
        self.register_theme(THEME)
        self.theme = "ui-vault"
        self.query_one("#stats", DataTable).add_columns("Stat", "Value", "Tag")
        for key in ("armor", "weapons"):
            self.query_one(f"#gear-{key}", DataTable).add_columns("Piece", "Asset", "Kind", "Scale")
        self.query_one("#coins", DataTable).add_columns("Where", "Name", "Value")
        self.query_one("#backups", DataTable).add_columns("When", "Age", "Kind", "Reason", "Size", "Name")
        self.detect()
        self.set_interval(1.0, self.poll)

    def detect(self) -> None:
        if self.demo:
            self.bridge = DemoBridge()
            if not g.list_backups(self.backups_dir):
                self.make_backup("demo")
        else:
            games = g.find_games()
            self.game = games[0] if games else None
            bd = self.game.bridge_dir if self.game else None
            self.bridge = Bridge(bd) if bd and bd.parent.exists() else None
        self.load_mods()
        self.render_setup()
        self.sigs.pop("backups", None)
        self.poll()

    def load_mods(self) -> None:
        if not self.bridge:
            return
        self.cfg = self.bridge.read_mods()
        for m in MODS:
            self.set_quiet(f"#mod-{m.key}", self.cfg.get(m.key) == "1")
            for pk, _, default in m.params:
                self.cfg.setdefault(pk, default)
                self.set_quiet(f"#param-{pk}", self.cfg[pk])
        self.set_quiet("#gravity", self.cfg.get("gravity", "normal"))
        self.set_quiet("#param-gravity_custom", self.cfg.get("gravity_custom", "-980"))

    def set_quiet(self, selector: str, value) -> None:
        """Set a widget's value without triggering our change handlers."""
        w = self.query_one(selector)
        with w.prevent(Switch.Changed, Select.Changed, Input.Changed):
            w.value = value

    # ------------------------------------------------------------------ "Make Backup Before Applying Changes?"
    def confirm(self, change: str, apply: Callable[[], object], on_cancel: Callable[[], object] | None = None) -> None:
        def done(choice: str | None) -> None:
            if choice not in ("yes", "no"):
                self.note(f"Cancelled: {change}")
                if on_cancel:
                    on_cancel()
                return
            if choice == "yes":
                self.make_backup("change")
            apply()

        self.push_screen(BackupPrompt(change), done)

    def make_backup(self, reason: str) -> Path | None:
        if self.demo:
            self.backups_dir.mkdir(parents=True, exist_ok=True)
            out = self.backups_dir / f"saves-{time.strftime('%Y%m%d-%H%M%S')}-{reason}.tar.gz"
            data = json.dumps((self.state or {}).get("economy", [])).encode()
            with tarfile.open(out, "w:gz") as tar:
                info = tarfile.TarInfo("demo/state.json")
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
            out.with_name(out.name[:-7] + ".json").write_text(json.dumps(
                {"kind": "saves", "reason": reason, "format": "tar", "created": time.strftime("%Y-%m-%dT%H:%M:%S")}))
        else:
            out = g.backup_saves(self.game, reason)
        self.note(f"Backup made: {out.name}" if out else "No save folder found to back up (has the game been played yet?)")
        self.sigs.pop("backups", None)
        self.refresh_backups()
        return out

    # ------------------------------------------------------------------ polling
    def poll(self) -> None:
        self.refresh_backups()
        if not self.bridge:
            self.sub_title = "Lab DLL not in the game yet · open the Setup tab"
            return
        self.state, self.live = self.bridge.read_state()
        if self.demo:
            self.sub_title = "DEMO MODE · fake data"
        elif self.live:
            w = (self.state or {}).get("world", {})
            self.sub_title = f"● LIVE · {w.get('map') or 'in game'}" + (" · mods paused (panic)" if w.get("paused") else "")
        else:
            self.sub_title = "○ waiting for the game (start Half Sword)"
        if self.live and not self.demo and not self.backed_up:
            self.backed_up = True
            self.backup_thread("session")
        self.refresh_chars()
        self.refresh_detail()
        self.refresh_logs()

    def chars(self) -> list[dict]:
        if not (self.state and (self.live or self.demo)):
            return []
        order = {"player": 0, "mercenary": 1, "opponent": 2}
        return sorted(self.state.get("characters", []), key=lambda c: (order.get(c["role"], 3), c["name"]))

    def current(self) -> dict | None:
        return next((c for c in self.chars() if c["id"] == self.selected), None)

    def changed(self, key: str, sig) -> bool:
        if self.sigs.get(key) == sig:
            return False
        self.sigs[key] = sig
        return True

    def refresh_chars(self) -> None:
        chars = self.chars()
        ol = self.query_one("#chars", OptionList)
        if not self.changed("chars", [(c["id"], c["name"], tuple(char_tags(c))) for c in chars]):
            return
        ol.clear_options()
        for c in chars:
            label = Text(c["name"], style="bold")
            label.append_text(tagged("\n ", char_tags(c)))
            label.append(f"  {c['class']}", style=DIM)
            ol.add_option(Option(label, id=c["id"]))
        if not chars:
            self.selected = None
            ol.add_option(Option(Text("No characters yet.\nStart a fight in-game\nor check Setup.", style=MUTED),
                                 disabled=True))
        elif self.selected not in {c["id"] for c in chars}:
            self.selected = chars[0]["id"]
        if self.selected:
            ol.highlighted = ol.get_option_index(self.selected)
        world = (self.state or {}).get("world", {})
        self.query_one("#world", Static).update(Text.assemble(
            ("gravity ", DIM), (str(world.get("gravity", "?")), MUTED), ("   chars ", DIM), (str(len(chars)), MUTED)))

    def on_option_list_option_highlighted(self, e: OptionList.OptionHighlighted) -> None:
        if e.option_id and e.option_id != self.selected:
            self.selected = e.option_id
            self.sigs = {k: v for k, v in self.sigs.items() if k in ("chars", "backups")}
            self.refresh_detail()

    def refresh_detail(self) -> None:
        c = self.current()
        title = self.query_one("#char-title", Static)
        if not c:
            title.update(Text("Pick a character on the left.", style=MUTED))
            for tid in ("stats", "gear-armor", "gear-weapons"):
                if self.changed(tid, None):
                    self.query_one(f"#{tid}", DataTable).clear()
        else:
            t = tagged(c["name"], char_tags(c), f"bold {CYAN}")
            t.append(f"   {c['class']}", style=DIM)
            title.update(t)
            self.query_one("#body-info", Static).update(Text.assemble(
                ("Size ", MUTED), (f"{c['scale']:.2f}x", "bold"), ("    Weight ", MUTED), (f"{c['mass']:.2f}x", "bold"),
                ("    Speed ", MUTED), (f"{c['speed']:.2f}x", "bold")))
            self.refresh_stats(c)
            for key, kind in (("armor", "armor"), ("weapons", "weapon")):
                self.refresh_gear(key, [x for x in c.get("gear", []) if x["kind"] == kind])
        self.refresh_coins()
        self.refresh_spawn_lists()

    def refresh_stats(self, c: dict) -> None:
        flt = self.query_one("#stat-filter", Input).value.lower().strip()
        rows = [p for p in c.get("props", []) if not flt or flt in p["n"].lower() or flt in group_of(p["n"])]
        if not self.changed("stats", (c["id"], flt, [(p["n"], p["v"]) for p in rows])):
            return
        table = self.query_one("#stats", DataTable)
        row = table.cursor_row
        table.clear()
        for p in rows:
            v = p["v"]
            shown = Text(str(v).lower(), style="#38bdf8") if p["t"] == "bool" else (
                Text(f"{v:.4g}" if isinstance(v, float) else str(v), style="bold"))
            table.add_row(p["n"], shown, tag(group_of(p["n"])), key=p["n"])
        if rows:
            table.move_cursor(row=min(row, len(rows) - 1))

    def refresh_gear(self, key: str, items: list[dict]) -> None:
        if not self.changed(f"gear-{key}", [(i["id"], round(i["scale"], 2)) for i in items]):
            return
        table = self.query_one(f"#gear-{key}", DataTable)
        row = table.cursor_row
        table.clear()
        for i in items:
            table.add_row(Text(i["name"], style="bold"), Text(i["class"], style=MUTED), tag(i["src"]),
                          f"{i['scale']:.2f}x", key=i["id"])
        if items:
            table.move_cursor(row=min(row, len(items) - 1))
            self.sel_gear[key] = items[min(max(row, 0), len(items) - 1)]["id"]
        else:
            self.sel_gear[key] = None

    def refresh_coins(self) -> None:
        eco = (self.state or {}).get("economy", []) if (self.live or self.demo) else []
        if not self.changed("coins", [(e["owner_id"], e["n"], e["v"]) for e in eco]):
            return
        table = self.query_one("#coins", DataTable)
        row = table.cursor_row
        table.clear()
        for e in eco:
            table.add_row(Text(e["owner"], style=MUTED), Text(e["n"], style="bold #f5c542"), f"{e['v']:,}"
                          if isinstance(e["v"], int) else str(e["v"]), key=f"{e['owner_id']}|{e['n']}")
        if eco:
            table.move_cursor(row=min(row, len(eco) - 1))

    def refresh_spawn_lists(self) -> None:
        classes = (self.state or {}).get("classes", {})
        for key in ("armor", "weapons"):
            items = classes.get(key, [])
            if self.changed(f"spawn-{key}", [i["path"] for i in items]):
                self.query_one(f"#spawn-{key}", Select).set_options([(i["name"], i["path"]) for i in items])

    def refresh_backups(self) -> None:
        backups = g.list_backups(self.backups_dir)
        if not self.changed("backups", [(str(b.path), int(b.created // 60)) for b in backups]):
            return
        table = self.query_one("#backups", DataTable)
        row = table.cursor_row
        table.clear()
        for b in backups:
            table.add_row(time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(b.created)), Text(human_age(b.created), style=MUTED),
                          tag(b.kind), tag(b.reason), human_size(b.size), Text(b.path.name, style=DIM), key=str(b.path))
        if backups:
            table.move_cursor(row=min(max(row, 0), len(backups) - 1))
            self.sel_backup = str(backups[min(max(row, 0), len(backups) - 1)].path)
        else:
            self.sel_backup = None
        head = Text.assemble((f"{len(backups)} backups", "bold"), ("  ·  newest first  ·  ", MUTED),
                             (str(self.backups_dir), DIM))
        self.query_one("#backup-head", Static).update(head)

    def refresh_logs(self) -> None:
        out = self.query_one("#log", RichLog)
        for entry in (self.state or {}).get("log", []):
            if entry["seq"] > self.log_seq:
                self.log_seq = entry["seq"]
                out.write(Text.assemble(("game  ", CYAN), entry["msg"]))

    def note(self, msg: str, severity: str = "information") -> None:
        self.query_one("#log", RichLog).write(Text.assemble(("lab   ", VIOLET), msg))
        self.notify(msg, severity=severity)

    # ------------------------------------------------------------------ actions
    def ready(self) -> bool:
        if not self.bridge:
            self.note("The Lab DLL isn't in the game yet. See the Setup tab.", "warning")
            return False
        if not (self.live or self.demo):
            self.note("Half Sword isn't running (or the DLL didn't load). Start the game first.", "warning")
            return False
        return True

    def send(self, verb: str, *args) -> None:
        if not self.ready():
            return
        self.bridge.send(verb, *args)
        self.sigs = {k: v for k, v in self.sigs.items() if k == "backups"}

    def char_cmd(self, label: str, verb: str, *args) -> None:
        c = self.current()
        if not c:
            self.note("Pick a character first.", "warning")
            return
        if not self.ready():
            return
        detail = f" {args[0]}" if args and verb in ("scale", "mass", "speed") else ""
        cid = c["id"]
        self.confirm(f"{label}{detail} on {c['name']}", lambda: self.send(verb, cid, *args))

    def apply_input(self, verb: str) -> None:
        raw = self.query_one(f"#in-{verb}", Input).value
        try:
            val = float(raw)
        except ValueError:
            self.note(f"'{raw}' isn't a number.", "warning")
            return
        if not 0.01 <= val <= 20:
            self.note("Keep it between 0.01 and 20; past that the physics explodes.", "warning")
            return
        self.char_cmd(dict((v, l) for v, l, _ in BODY)[verb], verb, val)

    def gear_cmd(self, key: str, label: str, verb: str, *args) -> None:
        gid = self.sel_gear.get(key)
        c = self.current()
        item = next((i for i in (c or {}).get("gear", []) if i["id"] == gid), None)
        if not item:
            self.note(f"No {key} selected.", "warning")
            return
        if self.ready():
            self.confirm(f"{label}: {item['name']}", lambda: self.send(verb, gid, *args))

    def spawn(self, key: str) -> None:
        path = self.query_one(f"#spawn-{key}", Select).value
        if not isinstance(path, str):
            self.note("Pick something to spawn first (Rescan spawnable items in Setup if the list is empty).",
                      "warning")
            return
        try:
            n = max(1, min(12, int(self.query_one(f"#count-{key}", Input).value or 1)))
        except ValueError:
            n = 1
        if self.ready():
            sel = self.selected or ""
            self.confirm(f"Spawn {n}x {path.rsplit('.', 1)[-1]}", lambda: self.send("spawn", path, n, sel))

    def coin_row(self) -> dict | None:
        table = self.query_one("#coins", DataTable)
        eco = (self.state or {}).get("economy", [])
        if not eco or table.cursor_row < 0:
            self.note("No coin values found yet. Try a dump (D) and hand it to Claude.", "warning")
            return None
        return eco[min(table.cursor_row, len(eco) - 1)]

    def coin_set(self, value) -> None:
        e = self.coin_row()
        if e and self.ready():
            self.confirm(f"{e['n']} = {value}", lambda: self.send("set", e["owner_id"], e["n"], value))

    def coin_add(self, amount: int) -> None:
        e = self.coin_row()
        if e and self.ready():
            new = int(e["v"]) + amount
            self.confirm(f"{e['n']} +{amount:,} ({new:,})", lambda: self.send("set", e["owner_id"], e["n"], new))

    def coin_edit(self) -> None:
        e = self.coin_row()
        if e:
            self.push_screen(EditValue(f"{e['owner']} · {e['n']}", e["v"], e["t"]),
                             lambda v: v is not None and self.coin_set(v))

    def vanish_char(self) -> None:
        c = self.current()
        if not c:
            return
        if c["role"] == "player":
            self.note("Not deleting yourself. Use Invisible instead.", "warning")
            return
        self.char_cmd("Delete from the world", "vanish")

    def on_button_pressed(self, e: Button.Pressed) -> None:
        fn = self.actions.get(e.button.id or "")
        if fn:
            fn()

    def on_data_table_row_highlighted(self, e: DataTable.RowHighlighted) -> None:
        tid = e.data_table.id or ""
        if e.row_key is None:
            return
        if tid.startswith("gear-"):
            self.sel_gear[tid[5:]] = e.row_key.value
        elif tid == "backups":
            self.sel_backup = e.row_key.value

    def on_data_table_row_selected(self, e: DataTable.RowSelected) -> None:
        c = self.current()
        if e.data_table.id == "stats" and c:
            p = next((p for p in c["props"] if p["n"] == e.row_key.value), None)
            if p:
                cid = c["id"]
                self.push_screen(EditValue(p["n"], p["v"], p["t"]), lambda v: v is not None and self.confirm(
                    f"{p['n']} = {v} on {c['name']}", lambda: self.send("set", cid, p["n"], v)))
        elif e.data_table.id == "coins":
            self.coin_edit()

    def on_input_changed(self, e: Input.Changed) -> None:
        if e.input.id == "stat-filter":
            self.refresh_detail()

    def on_input_submitted(self, e: Input.Submitted) -> None:
        iid = e.input.id or ""
        if iid.startswith("param-"):
            key, value = iid[6:], e.value
            old = self.cfg.get(key, "")
            if value == old:
                return
            self.confirm(f"{key} = {value}", lambda: self.set_cfg({key: value}),
                         lambda: self.set_quiet(f"#{iid}", old))
        elif iid.startswith("in-"):
            self.apply_input(iid[3:])

    def on_switch_changed(self, e: Switch.Changed) -> None:
        key, want = (e.switch.id or "")[4:], e.value
        self.set_quiet(f"#mod-{key}", not want)  # stays as it was until confirmed
        title = next((m.title for m in MODS if m.key == key), key)

        def apply() -> None:
            changes = {key: "1" if want else "0"}
            if want:  # mods that fight over the same thing switch each other off
                for group in EXCLUSIVE:
                    if key in group:
                        changes.update({o: "0" for o in group if o != key and self.cfg.get(o) == "1"})
            self.set_cfg(changes)

        self.confirm(f"{title} {'ON' if want else 'off'}", apply)

    def on_select_changed(self, e: Select.Changed) -> None:
        if e.select.id != "gravity" or not isinstance(e.value, str):
            return
        old, new = self.cfg.get("gravity", "normal"), e.value
        if new == old:
            return
        self.set_quiet("#gravity", old)
        label = dict((v, l) for l, v in GRAVITY)[new]
        self.confirm(f"Gravity: {label}", lambda: self.set_cfg({"gravity": new}))

    def set_cfg(self, changes: dict[str, str]) -> None:
        if not self.bridge:
            self.note("The Lab DLL isn't in the game yet, so mods can't be saved. See Setup.", "warning")
            return
        self.cfg.update(changes)
        self.bridge.write_mods(self.cfg)
        for k, v in changes.items():
            if any(m.key == k for m in MODS):
                self.set_quiet(f"#mod-{k}", v == "1")
            elif k == "gravity":
                self.set_quiet("#gravity", v)
            else:
                self.set_quiet(f"#param-{k}", v)
        self.note("Applied: " + ", ".join(f"{k}={v}" for k, v in changes.items()))

    def action_panic(self) -> None:
        if self.ready():
            self.confirm("Panic reset (undo everything the Lab changed)", lambda: self.send("panic"))

    def action_dump(self) -> None:
        if self.demo:
            self.note("Dump needs the real game.", "warning")
            return
        if self.ready():
            self.send("dump")
            self.note("Dumping game data...")
            self.set_timer(4, self.collect_dump)

    def collect_dump(self) -> None:
        src = self.game.bridge_dir / "dump.txt" if self.game else None
        if not src or not src.exists():
            self.note("No dump appeared. Is the game running with the DLL loaded?", "error")
            return
        dest = g.REPO / "dumps" / f"dump-{time.strftime('%Y%m%d-%H%M%S')}.txt"
        dest.parent.mkdir(exist_ok=True)
        shutil.copy2(src, dest)
        self.note(f"Dump saved: {dest}")

    def action_backup(self) -> None:
        self.backup_thread("manual")

    @work(thread=True)
    def backup_thread(self, reason: str) -> None:
        self.call_from_thread(self.make_backup, reason)

    # ------------------------------------------------------------------ backups tab
    def selected_backup(self) -> g.Backup | None:
        b = next((x for x in g.list_backups(self.backups_dir) if str(x.path) == self.sel_backup), None)
        if not b:
            self.note("Pick a backup in the list first.", "warning")
        return b

    def restore_selected(self) -> None:
        b = self.selected_backup()
        if not b:
            return

        def apply() -> None:
            if self.demo:
                self.note(f"Demo: would restore {b.path.name}")
                return
            try:
                self.note(g.restore_backup(b, self.game))
            except (OSError, tarfile.TarError, KeyError) as ex:
                self.note(f"Restore failed: {ex}", "error")
            self.sigs.pop("backups", None)

        self.confirm(f"Restore {b.kind} backup from {time.strftime('%Y-%m-%d %H:%M', time.localtime(b.created))}", apply)

    def delete_selected(self) -> None:
        b = self.selected_backup()
        if not b:
            return

        def done(ok: bool) -> None:
            if ok:
                g.delete_backup(b)
                self.note(f"Deleted {b.path.name}")
                self.sigs.pop("backups", None)
                self.refresh_backups()

        self.push_screen(Confirm(f"Delete backup {b.path.name}? This can't be undone."), done)

    def open_backups(self) -> None:
        self.backups_dir.mkdir(parents=True, exist_ok=True)
        opener = "xdg-open" if shutil.which("xdg-open") else None
        if opener:
            subprocess.Popen([opener, str(self.backups_dir)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.note(f"Backups folder: {self.backups_dir}")

    # ------------------------------------------------------------------ setup tab
    def setup_help(self) -> Text:
        return Text(
            "How it works: \"Copy DLL\" puts the Lab's own version.dll next to the game's exe (nothing else is "
            "installed). Half Sword loads it on start. It finds the game's characters and stats by name through "
            "Unreal's reflection, draws the in-game menu (press \\), and talks to this window through files in "
            "HalfSwordLab/bridge.\n"
            "On Linux the DLL is switched on inside the game's own Proton prefix, so no launch options are "
            "needed. Anything it replaces is backed up first, and Remove puts it all back.\n"
            f"Saves are also backed up the first time the game connects each session ({g.BACKUPS}).", style=MUTED)

    def render_setup(self) -> None:
        def row(ok, label, detail=""):
            mark = ("✔", "#34d399") if ok else ("?", "#facc15") if ok is None else ("✘", "#f43f5e")
            return Text.assemble((f" {mark[0]} ", f"bold {mark[1]}"), (label, "bold"), ("  " + detail, MUTED), "\n")

        t = Text()
        if self.demo:
            t.append_text(row(True, "Demo mode", "fake data, run without --demo for the real game"))
        gm = self.game
        t.append_text(row(bool(gm), "Half Sword", str(gm.root) if gm else "not found in any Steam library"))
        if gm:
            t.append_text(row(bool(gm.exe), "Game binary", gm.exe.name if gm.exe else "no *-Win64-Shipping.exe"))
            loads = gm.loads_proxy()
            t.append_text(row(loads, "Game loads version.dll", "yes" if loads else
                              "not in its import table: tell Claude, another proxy name is needed" if loads is False else ""))
            t.append_text(row(gm.injected, "Lab DLL", str(gm.dll_path) if gm.injected else "not copied yet"))
            if not g.WINDOWS:
                ov = gm.override_set()
                t.append_text(row(ov, "DLL switched on in Proton prefix", "yes" if ov else
                                  "start the game once so Proton creates its prefix" if ov is None else "no"))
            saves = gm.save_dirs()
            t.append_text(row(bool(saves), "Saves", ", ".join(str(s) for s in saves) or "none found yet"))
        backups = g.list_backups(self.backups_dir)
        t.append_text(row(bool(backups), "Backups", f"{len(backups)} in {self.backups_dir}"))
        self.query_one("#setup-status", Static).update(t)

    def inject(self) -> None:
        if self.demo:
            self.confirm("Copy DLL to game files", lambda: self.note("Demo: nothing copied."))
            return
        if not self.game or not self.game.win64:
            self.note("Find the game first.", "warning")
            return

        def apply() -> None:
            try:
                g.inject(self.game, log=self.note)
            except (OSError, RuntimeError) as ex:
                self.note(f"Copy failed: {ex}", "error")
            self.detect()

        self.confirm(f"Copy version.dll into {self.game.win64}", apply)

    def remove(self) -> None:
        if self.demo:
            self.confirm("Remove DLL from game files", lambda: self.note("Demo: nothing removed."))
            return
        if not self.game:
            return

        def apply() -> None:
            try:
                g.remove_injection(self.game, log=self.note)
            except (OSError, RuntimeError) as ex:
                self.note(f"Remove failed: {ex}", "error")
            self.detect()

        self.confirm("Remove the Lab DLL and restore the original files", apply)

    def copy_launch(self) -> None:
        self.copy_to_clipboard(g.LAUNCH_OPTION)
        for cmd in (["wl-copy"], ["xclip", "-selection", "clipboard"]):
            if shutil.which(cmd[0]):
                subprocess.run(cmd, input=g.LAUNCH_OPTION.encode(), check=False)
                break
        self.note(f"Copied: {g.LAUNCH_OPTION}  (only needed if the prefix switch doesn't work)")

    def use_path(self) -> None:
        raw = self.query_one("#game-path", Input).value.strip()
        path = Path(raw).expanduser()
        if not path.is_dir():
            self.note(f"Not a folder: {raw}", "error")
            return
        cfg = g.load_config()
        cfg["game_dir"] = str(path)
        g.save_config(cfg)
        self.detect()

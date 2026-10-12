"""Half Sword Lab terminal GUI (Textual)."""
from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

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
from .mods import GRAVITY, MODS
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


class EditValue(ModalScreen[str | None]):
    def __init__(self, title: str, value, kind: str):
        super().__init__()
        self.title_text, self.value, self.kind = title, value, kind

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog"):
            yield Label(Text.assemble((f"Edit  ", MUTED), (self.title_text, f"bold {CYAN}")))
            if self.kind == "bool":
                with Horizontal(classes="row"):
                    yield Button("true", id="v-true", variant="success")
                    yield Button("false", id="v-false", variant="error")
                    yield Button("Cancel", id="cancel")
            else:
                yield Input(str(self.value), id="value", type="number", valid_empty=True)
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
        self.game: g.GameInstall | None = None
        self.bridge: Bridge | DemoBridge | None = None
        self.state: dict | None = None
        self.live = False
        self.selected: str | None = None
        self.sel_gear: dict[str, str | None] = {"armor": None, "weapons": None}
        self.sel_coin: str | None = None
        self.cfg: dict[str, str] = {}
        self.sigs: dict[str, object] = {}
        self.actions: dict[str, callable] = {}
        self.log_seq = 0
        self.backed_up = False
        self.loading = False

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
                        yield self.btn(name, f"p-{verb}-{name.lower()}", lambda v=verb, x=val: self.char_cmd(v, x))
            yield Static("Weight is a physics mass multiplier: heavier hits harder and is harder to push. "
                         "Speed is per-character time, so 2.0 means twice as fast at everything.", classes="hint")
            with Horizontal(classes="row"):
                yield self.btn("Reset this character", "reset-char", lambda: self.char_cmd("reset"), "warning")

    def compose_gear(self, key: str, kind: str) -> ComposeResult:
        yield DataTable(id=f"gear-{key}", cursor_type="row", zebra_stripes=True)
        with Horizontal(classes="row"):
            if kind == "armor":
                yield self.btn("Huge", f"{key}-huge", lambda: self.gear_cmd(key, "scale", 2.5))
                yield self.btn("Tiny", f"{key}-tiny", lambda: self.gear_cmd(key, "scale", 0.4))
            else:
                yield self.btn("Giant", f"{key}-huge", lambda: self.gear_cmd(key, "scale", 3))
                yield self.btn("Tiny", f"{key}-tiny", lambda: self.gear_cmd(key, "scale", 0.35))
                yield self.btn("Feather", f"{key}-feather", lambda: self.gear_cmd(key, "mass", 0.1))
                yield self.btn("Heavy", f"{key}-heavy", lambda: self.gear_cmd(key, "mass", 5))
            yield self.btn("Normal", f"{key}-normal", lambda: self.gear_cmd(key, "scale", 1))
            yield self.btn("Hide", f"{key}-hide", lambda: self.gear_cmd(key, "hide"))
            yield self.btn("Show", f"{key}-show", lambda: self.gear_cmd(key, "show"))
            yield self.btn("Remove", f"{key}-vanish", lambda: self.gear_cmd(key, "vanish"), "error")
        with Horizontal(classes="row"):
            yield Select([], prompt=f"spawn {kind}...", id=f"spawn-{key}", classes="spawn")
            yield Input("1", id=f"count-{key}", classes="count", type="integer", valid_empty=True)
            yield self.btn("Spawn near", f"{key}-spawn", lambda: self.spawn(key), "primary")
        yield Static(f"{kind.title()} found on this character. Remove deletes the piece, Hide only hides it. "
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
            yield self.btn("🚀 Launch to the sky", "g-launch", lambda: self.char_cmd("launch", 2500))
            yield self.btn("🧬 Clone", "g-clone", lambda: self.char_cmd("clone", 1))
            yield self.btn("👥 Clone army (x6)", "g-army", lambda: self.char_cmd("clone", 6))
            yield self.btn("🧲 Bring to me", "g-bring", lambda: self.char_cmd("bring"))
            yield self.btn("🌀 Teleport me there", "g-visit", lambda: self.char_cmd("visit"))
            yield self.btn("🗿 Freeze", "g-freeze", lambda: self.char_cmd("speed", 0.02))
            yield self.btn("☕ Hyper", "g-hyper", lambda: self.char_cmd("speed", 3))
            yield self.btn("🐜 Ant", "g-ant", lambda: self.char_cmd("scale", 0.2))
            yield self.btn("🏔 Colossus", "g-colossus", lambda: self.char_cmd("scale", 4))
            yield self.btn("🎈 Balloon (weightless)", "g-balloon", lambda: self.char_cmd("mass", 0.05))
            yield self.btn("👻 Invisible", "g-hide", lambda: self.char_cmd("hide"))
            yield self.btn("👁 Visible", "g-show", lambda: self.char_cmd("show"))
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
            with Horizontal(classes="row"):
                yield self.btn("🚀 Launch everyone", "launch-all", lambda: self.send("launch", "all", 2200))
                yield self.btn("🛑 Panic reset (F8)", "panic", self.action_panic, "error")

    def compose_setup(self) -> ComposeResult:
        with VerticalScroll():
            yield Static("", id="setup-status")
            with Horizontal(classes="row"):
                yield self.btn("Re-detect game", "s-detect", self.detect, "primary")
                yield self.btn("Install UE4SS", "s-ue4ss", self.install_ue4ss)
                yield self.btn("Deploy Lab mod", "s-deploy", self.deploy)
                yield self.btn("Copy Steam launch option", "s-launch", self.copy_launch)
            with Horizontal(classes="row"):
                yield self.btn("Backup saves now", "s-backup", self.action_backup)
                yield self.btn("Dump game data for Claude", "s-dump", self.action_dump)
                yield self.btn("Rescan spawnable items", "s-rescan", lambda: self.send("scan_classes"))
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
        self.detect()
        self.set_interval(1.0, self.poll)

    def detect(self) -> None:
        if self.demo:
            self.bridge = DemoBridge()
        else:
            games = g.find_games()
            self.game = games[0] if games else None
            self.bridge = Bridge(self.game.bridge_dir) if self.game and self.game.mod_deployed else None
        self.load_mods()
        self.render_setup()
        self.poll()

    def load_mods(self) -> None:
        if not self.bridge:
            return
        self.cfg = self.bridge.read_mods()
        self.loading = True
        for m in MODS:
            self.query_one(f"#mod-{m.key}", Switch).value = self.cfg.get(m.key) == "1"
            for pk, _, default in m.params:
                self.query_one(f"#param-{pk}", Input).value = self.cfg.get(pk, default)
                self.cfg.setdefault(pk, default)
        self.query_one("#gravity", Select).value = self.cfg.get("gravity", "normal")
        self.query_one("#param-gravity_custom", Input).value = self.cfg.get("gravity_custom", "-980")
        self.call_after_refresh(setattr, self, "loading", False)

    # ------------------------------------------------------------------ polling
    def poll(self) -> None:
        if not self.bridge:
            self.sub_title = "game not set up · open the Setup tab"
            return
        self.state, self.live = self.bridge.read_state()
        if self.demo:
            self.sub_title = "DEMO MODE · fake data"
        elif self.live:
            w = (self.state or {}).get("world", {})
            self.sub_title = f"● LIVE · {w.get('map') or 'in game'}" + (" · mods paused (panic)" if w.get("paused") else "")
        else:
            self.sub_title = "○ waiting for the game (launch Half Sword with the launch option)"
        if self.live and not self.demo and not self.backed_up:
            self.backed_up = True
            self.backup("session")
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
            ("gravity ", DIM), (str(world.get("gravity", "?")), MUTED), (f"   chars ", DIM), (str(len(chars)), MUTED)))

    def on_option_list_option_highlighted(self, e: OptionList.OptionHighlighted) -> None:
        if e.option_id and e.option_id != self.selected:
            self.selected = e.option_id
            self.sigs = {"chars": self.sigs.get("chars")}
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
                ("Size ", MUTED), (f"{c['scale']:.2f}x", "bold"), (f"    Weight ", MUTED), (f"{c['mass']:.2f}x", "bold"),
                (f"    Speed ", MUTED), (f"{c['speed']:.2f}x", "bold")))
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

    def refresh_logs(self) -> None:
        out = self.query_one("#log", RichLog)
        for entry in (self.state or {}).get("log", []):
            if entry["seq"] > self.log_seq:
                self.log_seq = entry["seq"]
                out.write(Text.assemble((f"game  ", CYAN), entry["msg"]))

    def note(self, msg: str, severity: str = "information") -> None:
        self.query_one("#log", RichLog).write(Text.assemble((f"lab   ", VIOLET), msg))
        self.notify(msg, severity=severity)

    # ------------------------------------------------------------------ actions
    def send(self, verb: str, *args) -> bool:
        if not self.bridge:
            self.note("Game isn't set up yet, see the Setup tab.", "warning")
            return False
        if not (self.live or self.demo):
            self.note("Half Sword isn't running (or the mod isn't loaded). Start the game first.", "warning")
            return False
        self.bridge.send(verb, *args)
        self.sigs = {}
        return True

    def char_cmd(self, verb: str, *args) -> None:
        if not self.selected:
            self.note("Pick a character first.", "warning")
            return
        self.send(verb, self.selected, *args)

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
        self.char_cmd(verb, val)

    def gear_cmd(self, key: str, verb: str, *args) -> None:
        gid = self.sel_gear.get(key)
        if not gid:
            self.note(f"No {key} selected.", "warning")
            return
        self.send(verb, gid, *args)

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
        self.send("spawn", path, n, self.selected or "")

    def coin_row(self) -> dict | None:
        table = self.query_one("#coins", DataTable)
        eco = (self.state or {}).get("economy", [])
        if not eco or table.cursor_row < 0:
            self.note("No coin values found yet. Try a dump (D) and hand it to Claude.", "warning")
            return None
        return eco[min(table.cursor_row, len(eco) - 1)]

    def coin_set(self, value) -> None:
        e = self.coin_row()
        if e:
            self.send("set", e["owner_id"], e["n"], value)

    def coin_add(self, amount: int) -> None:
        e = self.coin_row()
        if e:
            self.coin_set(int(e["v"]) + amount)

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
        self.push_screen(Confirm(f"Delete {c['name']} from the world?"),
                         lambda ok: ok and self.char_cmd("vanish"))

    def on_button_pressed(self, e: Button.Pressed) -> None:
        fn = self.actions.get(e.button.id or "")
        if fn:
            fn()

    def on_data_table_row_highlighted(self, e: DataTable.RowHighlighted) -> None:
        tid = e.data_table.id or ""
        if tid.startswith("gear-") and e.row_key is not None:
            self.sel_gear[tid[5:]] = e.row_key.value

    def on_data_table_row_selected(self, e: DataTable.RowSelected) -> None:
        c = self.current()
        if e.data_table.id == "stats" and c:
            p = next((p for p in c["props"] if p["n"] == e.row_key.value), None)
            if p:
                cid = c["id"]
                self.push_screen(EditValue(p["n"], p["v"], p["t"]),
                                 lambda v: v is not None and self.send("set", cid, p["n"], v))
        elif e.data_table.id == "coins":
            self.coin_edit()

    def on_input_changed(self, e: Input.Changed) -> None:
        if e.input.id == "stat-filter":
            self.refresh_detail()
        elif (e.input.id or "").startswith("param-") and not self.loading:
            self.cfg[e.input.id[6:]] = e.value
            self.write_mods()

    def on_switch_changed(self, e: Switch.Changed) -> None:
        if self.loading:
            return
        key = (e.switch.id or "")[4:]
        self.cfg[key] = "1" if e.value else "0"
        if e.value:
            # Size and speed mods that fight over the same thing switch each other off.
            for group in (("random_sizes", "opp_scale_on", "ever_growing"), ("freeze_opp", "hyper_opp", "caffeine"),
                          ("matrix", "slowmo")):
                if key in group:
                    for other in group:
                        if other != key and self.cfg.get(other) == "1":
                            self.cfg[other] = "0"
                            self.query_one(f"#mod-{other}", Switch).value = False
        title = next((m.title for m in MODS if m.key == key), key)
        self.note(f"{title} {'ON' if e.value else 'off'}")
        self.write_mods()

    def on_select_changed(self, e: Select.Changed) -> None:
        if e.select.id == "gravity" and not self.loading and isinstance(e.value, str):
            self.cfg["gravity"] = e.value
            self.note(f"Gravity: {e.value}")
            self.write_mods()

    def write_mods(self) -> None:
        if self.bridge:
            self.bridge.write_mods(self.cfg)
        else:
            self.note("Game isn't set up yet, so mods can't be saved. See Setup.", "warning")

    def action_panic(self) -> None:
        if self.send("panic"):
            self.note("Panic reset sent: everything the Lab changed goes back to normal.")

    def action_dump(self) -> None:
        if self.demo:
            self.note("Dump needs the real game.", "warning")
            return
        if self.send("dump"):
            self.note("Dumping game data...")
            self.set_timer(4, self.collect_dump)

    def collect_dump(self) -> None:
        src = self.game.bridge_dir / "dump.txt" if self.game else None
        if not src or not src.exists():
            self.note("No dump appeared. Is the game running with the mod loaded?", "error")
            return
        dest = g.REPO / "dumps" / f"dump-{time.strftime('%Y%m%d-%H%M%S')}.txt"
        dest.parent.mkdir(exist_ok=True)
        shutil.copy2(src, dest)
        self.note(f"Dump saved: {dest}")

    def action_backup(self) -> None:
        self.backup("manual")

    @work(thread=True)
    def backup(self, reason: str) -> None:
        if not self.game:
            return
        out = g.backup_saves(self.game, reason)
        msg = f"Saves backed up: {out}" if out else "No save folder found to back up (has the game been played yet?)"
        self.call_from_thread(self.note, msg)
        self.call_from_thread(self.render_setup)

    # ------------------------------------------------------------------ setup tab
    def setup_help(self) -> Text:
        return Text(
            "How it works: UE4SS loads the Lab mod inside Half Sword. The mod reads stats with Unreal reflection "
            "and talks to this window through files in its bridge folder.\n"
            "1) Install UE4SS  2) Deploy Lab mod  3) In Steam: Half Sword → Properties → Launch Options, paste the "
            "launch option  4) Start the game.\n"
            "Saves are backed up automatically the first time the game connects each session "
            f"({g.BACKUPS}).", style=MUTED)

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
            t.append_text(row(bool(gm.exe), "Game binary", str(gm.exe.name) if gm.exe else "no *-Win64-Shipping.exe"))
            t.append_text(row(gm.ue4ss_installed, "UE4SS", str(gm.win64) if gm.ue4ss_installed else "not installed"))
            t.append_text(row(gm.mod_deployed, "Lab mod", str(gm.mod_dir) if gm.mod_deployed else "not deployed"))
            opt = gm.launch_option_set()
            t.append_text(row(opt, "Steam launch option", g.LAUNCH_OPTION if opt is not True else "set"))
            saves = gm.save_dirs()
            t.append_text(row(bool(saves), "Saves", ", ".join(str(s) for s in saves) or "none found yet"))
        backups = g.list_backups()
        t.append_text(row(bool(backups), "Backups", f"{len(backups)} in {g.BACKUPS}" + (
            f", latest {backups[0].name}" if backups else "")))
        self.query_one("#setup-status", Static).update(t)

    @work(thread=True, exclusive=True)
    def install_ue4ss(self) -> None:
        if not self.game:
            self.call_from_thread(self.note, "Find the game first.", "warning")
            return
        if self.game.ue4ss_installed:
            self.call_from_thread(self.note, "UE4SS is already installed; leaving your existing setup alone.")
            return
        try:
            g.install_ue4ss(self.game, log=lambda m: self.call_from_thread(self.note, m))
        except Exception as ex:  # network, permissions, bad zip
            self.call_from_thread(self.note, f"UE4SS install failed: {ex}", "error")
        self.call_from_thread(self.render_setup)

    def deploy(self) -> None:
        if not self.game or not self.game.win64:
            self.note("Find the game first.", "warning")
            return
        if self.game.save_dirs():
            g.backup_saves(self.game, "pre-deploy")
        try:
            g.deploy_mod(self.game, log=self.note)
        except OSError as ex:
            self.note(f"Deploy failed: {ex}", "error")
        self.detect()

    def copy_launch(self) -> None:
        self.copy_to_clipboard(g.LAUNCH_OPTION)
        for cmd in (["wl-copy"], ["xclip", "-selection", "clipboard"]):
            if shutil.which(cmd[0]):
                subprocess.run(cmd, input=g.LAUNCH_OPTION.encode(), check=False)
                break
        self.note(f"Copied: {g.LAUNCH_OPTION}")

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

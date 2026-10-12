"""Save Editor page: browse save slots, edit numbers and flags, write them back (with a backup prompt)."""
from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, DataTable, Input, OptionList, Select, Static
from textual.widgets.option_list import Option

from .save import Leaf, SaveError, SaveFile
from .tags import CYAN, DIM, MUTED, VIOLET, groups_of, tag
from .widgets import EditValue

FILTERS = [("All values", "all"), ("Editable only", "edit"), ("Coins", "coins"), ("Mercenaries", "mercenary"),
           ("Health & body", "health"), ("Armor", "armor"), ("Weapons", "weapon"), ("Changed", "changed")]


def age(t: float) -> str:
    s = max(0, time.time() - t)
    for size, unit in ((86400, "d"), (3600, "h"), (60, "min")):
        if s >= size:
            return f"{int(s // size)} {unit} ago"
    return "just now"


class SaveEditor(Horizontal):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.files: list[Path] = []
        self.sf: SaveFile | None = None
        self.changed: dict[int, object] = {}  # leaf index -> original value
        self.rows: list[int] = []

    def compose(self) -> ComposeResult:
        with Vertical(id="slots", classes="panel"):
            yield Static(Text("SAVE FILES", style=f"bold {MUTED}"), classes="section")
            yield OptionList(id="slot-list")
            with Horizontal(classes="bar"):
                yield Button("Reload", id="se-reload", compact=True)
                yield Button("Open folder", id="se-open", compact=True)
            yield Input(placeholder="other save folder…", id="se-folder", compact=True)
        with Vertical(id="save-main"):
            yield Static("", id="save-head")
            with Horizontal(classes="bar"):
                yield Input(placeholder="filter: gold, merc, health…", id="se-filter", compact=True)
                yield Select(FILTERS, value="all", allow_blank=False, id="se-group", compact=True)
            yield DataTable(id="se-table", cursor_type="row", zebra_stripes=True)
            with Horizontal(classes="bar"):
                yield Button("Edit value", id="se-edit", compact=True)
                yield Button("+1,000", id="se-add", compact=True)
                yield Button("Write changes to save", id="se-write", variant="primary", compact=True)
                yield Button("Discard changes", id="se-discard", compact=True)
            yield Static("Enter edits a value. Changes stay here until you write them. Only numbers and true/false "
                         "values are edited, in place, so the rest of the save is never touched.", classes="hint")

    def on_mount(self) -> None:
        self.query_one("#se-table", DataTable).add_columns("Value path", "Value", "Type", "Tags")
        self.reload()

    # ------------------------------------------------------------------ files
    def reload(self) -> None:
        dirs = self.app.save_dirs()
        self.files = sorted((f for d in dirs for f in d.glob("*.sav")), key=lambda f: f.stat().st_mtime, reverse=True)
        ol = self.query_one("#slot-list", OptionList)
        ol.clear_options()
        for i, f in enumerate(self.files):
            label = Text(f.name, style="bold")
            label.append(f"\n {age(f.stat().st_mtime)} · {f.stat().st_size / 1024:.1f} KB", style=DIM)
            if i == 0:
                label.append("  #newest", style=f"bold {CYAN}")
            if "auto" in f.name.lower():
                label.append("  #autosave", style=MUTED)
            ol.add_option(Option(label, id=str(f)))
        if not self.files:
            where = ", ".join(str(d) for d in dirs) or "no save folder found"
            ol.add_option(Option(Text(f"No .sav files.\n{where}", style=MUTED), disabled=True))
            self.sf = None
            self.render_head()
            self.render_table()
        elif not self.sf or self.sf.path not in self.files:
            self.open(self.files[0])
        else:
            self.open(self.sf.path)

    def open(self, path: Path) -> None:
        if self.changed and self.sf and self.sf.path != path:
            self.app.note("Unwritten changes were discarded.", "warning")
        self.changed.clear()
        try:
            self.sf = SaveFile.load(path)
        except (SaveError, OSError) as ex:
            self.sf = None
            self.app.note(f"Can't read {path.name}: {ex}", "error")
        ol = self.query_one("#slot-list", OptionList)
        try:
            ol.highlighted = ol.get_option_index(str(path))
        except Exception:
            pass
        self.render_head()
        self.render_table()

    def on_option_list_option_selected(self, e: OptionList.OptionSelected) -> None:
        if e.option_list.id == "slot-list" and e.option_id:
            self.open(Path(e.option_id))

    # ------------------------------------------------------------------ rendering
    def render_head(self) -> None:
        head = self.query_one("#save-head", Static)
        if not self.sf:
            head.update(Text("Pick a save on the left.", style=MUTED))
            return
        sf = self.sf
        t = Text.assemble((sf.path.name, f"bold {CYAN}"), ("   ", ""), (sf.class_name.rsplit(".", 1)[-1], MUTED))
        fmt = "UE5.4+ tags" if sf.new_tags else "classic tags"
        t.append(f"\nUE {sf.engine[0]}.{sf.engine[1]} · {fmt} · {len(sf.leaves)} values · "
                 f"{sum(l.editable for l in sf.leaves)} editable", style=DIM)
        if sf.compressed:
            t.append(" · compressed", style=DIM)
        if sf.warnings:
            t.append(f" · {len(sf.warnings)} parts left as-is", style="#facc15")
        if self.changed:
            t.append(f"\n● {len(self.changed)} unwritten change(s)", style=f"bold {VIOLET}")
        head.update(t)

    def render_table(self) -> None:
        table = self.query_one("#se-table", DataTable)
        row = table.cursor_row
        table.clear()
        self.rows = []
        if not self.sf:
            return
        flt = self.query_one("#se-filter", Input).value.lower().strip()
        grp = self.query_one("#se-group", Select).value
        for i, leaf in enumerate(self.sf.leaves):
            tags = groups_of(leaf.path)
            if flt and flt not in leaf.path.lower() and not any(flt in t for t in tags):
                continue
            if grp == "edit" and not leaf.editable:
                continue
            if grp == "changed" and i not in self.changed:
                continue
            if grp not in ("all", "edit", "changed") and grp not in tags:
                continue
            self.rows.append(i)
            path = Text(leaf.path, style=f"bold {VIOLET}" if i in self.changed else "")
            if i in self.changed:
                path.append(" ●", style=VIOLET)
            value = self.fmt(leaf)
            tag_text = Text()
            for t in tags:
                tag_text.append_text(tag(t))
                tag_text.append(" ")
            table.add_row(path, value, Text(leaf.type.replace("Property", ""), style=DIM), tag_text, key=str(i))
            if len(self.rows) >= 3000:
                break
        if self.rows:
            table.move_cursor(row=min(max(row, 0), len(self.rows) - 1))

    @staticmethod
    def fmt(leaf: Leaf) -> Text:
        v = leaf.value
        if leaf.kind == "bool":
            return Text("true" if v else "false", style="#38bdf8")
        if leaf.kind == "float":
            return Text(f"{v:.6g}", style="bold")
        if leaf.kind == "int":
            return Text(f"{v:,}", style="bold")
        return Text(str(v)[:60], style=MUTED)

    def on_input_changed(self, e: Input.Changed) -> None:
        if e.input.id == "se-filter":
            self.render_table()

    def on_select_changed(self, e: Select.Changed) -> None:
        if e.select.id == "se-group":
            self.render_table()

    # ------------------------------------------------------------------ editing
    def current_leaf(self) -> tuple[int, Leaf] | None:
        table = self.query_one("#se-table", DataTable)
        if not self.sf or not self.rows or table.cursor_row < 0:
            return None
        i = self.rows[min(table.cursor_row, len(self.rows) - 1)]
        return i, self.sf.leaves[i]

    def edit(self) -> None:
        cur = self.current_leaf()
        if not cur:
            return
        i, leaf = cur
        if not leaf.editable:
            self.app.note(f"{leaf.path} is text; only numbers and true/false values can be edited.", "warning")
            return
        self.app.push_screen(EditValue(leaf.path, leaf.value, leaf.kind), lambda v: v is not None and self.stage(i, v))

    def stage(self, i: int, value) -> None:
        leaf = self.sf.leaves[i]
        old = leaf.value
        try:
            self.sf.set(leaf, value)
        except (SaveError, ValueError) as ex:
            self.app.note(str(ex), "error")
            return
        self.changed.setdefault(i, old)
        if self.changed[i] == leaf.value:
            del self.changed[i]
        self.render_head()
        self.render_table()

    def add(self, amount: int) -> None:
        cur = self.current_leaf()
        if cur and cur[1].kind in ("int", "float"):
            self.stage(cur[0], cur[1].value + amount)

    def write(self) -> None:
        if not self.sf or not self.changed:
            self.app.note("Nothing to write.", "warning")
            return
        game = self.app.game
        if game and game.running():
            self.app.note("Quit Half Sword first: it would overwrite the save when it closes.", "error")
            return
        sf, n = self.sf, len(self.changed)

        def apply() -> None:
            try:
                sf.save()
            except OSError as ex:
                self.app.note(f"Write failed: {ex}", "error")
                return
            self.app.note(f"Wrote {n} change(s) to {sf.path.name}")
            self.changed.clear()
            self.reload()

        names = ", ".join(self.sf.leaves[i].path for i in list(self.changed)[:3]) + (" …" if n > 3 else "")
        self.app.confirm(f"Write {n} change(s) to {sf.path.name}: {names}", apply)

    def discard(self) -> None:
        if self.sf:
            self.open(self.sf.path)
            self.app.note("Changes discarded.")

    def on_data_table_row_selected(self, e: DataTable.RowSelected) -> None:
        if e.data_table.id == "se-table":
            self.edit()

    def on_input_submitted(self, e: Input.Submitted) -> None:
        if e.input.id == "se-folder":
            path = Path(e.value).expanduser()
            if not path.is_dir():
                self.app.note(f"Not a folder: {e.value}", "error")
                return
            self.app.custom_save_dir = path
            self.reload()

    def on_button_pressed(self, e: Button.Pressed) -> None:
        bid = e.button.id
        e.stop()
        if bid == "se-reload":
            self.reload()
        elif bid == "se-open":
            dirs = self.app.save_dirs()
            if dirs and shutil.which("xdg-open"):
                subprocess.Popen(["xdg-open", str(dirs[0])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.app.note(f"Save folder: {dirs[0] if dirs else 'none found'}")
        elif bid == "se-edit":
            self.edit()
        elif bid == "se-add":
            self.add(1000)
        elif bid == "se-write":
            self.write()
        elif bid == "se-discard":
            self.discard()

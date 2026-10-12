"""Dialogs shared by every page."""
from __future__ import annotations

from rich.text import Text
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.screen import ModalScreen
from textual.widgets import Button, Input, Label, Static

from .tags import CYAN, MUTED


class BackupPrompt(ModalScreen[str]):
    """Asked before every change: back up first, apply without a backup, or cancel."""
    BINDINGS = [Binding("y", "pick('yes')", "Yes"), Binding("n", "pick('no')", "No"),
                Binding("escape", "pick('cancel')", "Cancel")]

    def __init__(self, change: str):
        super().__init__()
        self.change = change

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog", classes="prompt"):
            yield Label(Text("Make Backup Before Applying Changes?", style=f"bold {CYAN}"))
            yield Static(Text.assemble(("Change  ", MUTED), (self.change, "bold")), classes="change")
            with Horizontal(classes="bar"):
                yield Button("Yes, back up first  Y", id="yes", variant="primary", compact=True)
                yield Button("No, just apply  N", id="no", compact=True)
                yield Button("Cancel  Esc", id="cancel", compact=True)

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
                with Horizontal(classes="bar"):
                    yield Button("true", id="v-true", variant="success", compact=True)
                    yield Button("false", id="v-false", variant="error", compact=True)
                    yield Button("Cancel", id="cancel", compact=True)
            else:
                yield Input(str(self.value), id="value", type="number", compact=True)
                with Horizontal(classes="bar"):
                    yield Button("Save", id="save", variant="primary", compact=True)
                    yield Button("Cancel", id="cancel", compact=True)

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
            with Horizontal(classes="bar"):
                yield Button("Yes", id="yes", variant="error", compact=True)
                yield Button("No", id="no", compact=True)

    def on_button_pressed(self, e: Button.Pressed) -> None:
        self.dismiss(e.button.id == "yes")

    def key_escape(self) -> None:
        self.dismiss(False)

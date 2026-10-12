"""Hashtag labels and their colours (palette from ui-vault)."""
from __future__ import annotations

from rich.text import Text

VIOLET, CYAN = "#8b5cf6", "#22d3ee"
DIM, MUTED, TEXT = "#66667a", "#9393a8", "#ececf4"

COLORS = {
    "player": "#f5c542", "mercenary": CYAN, "opponent": "#f43f5e", "dead": DIM, "modded": VIOLET,
    "health": "#f43f5e", "armor": "#94a3b8", "weapon": "#fb923c", "coins": "#f5c542",
    "team": CYAN, "speed": "#34d399", "physics": "#a78bfa", "misc": DIM,
    "opponents": "#f43f5e", "world": VIOLET, "chaos": "#e879f9", "weapons": "#fb923c",
    "experimental": "#facc15", "actor": MUTED, "component": MUTED, "bool": "#38bdf8",
    # backups
    "saves": CYAN, "gamefiles": "#fb923c", "manual": "#34d399", "in-game": VIOLET, "change": "#a78bfa",
    "session": MUTED, "pre-inject": "#fb923c", "pre-restore": "#facc15", "demo": DIM,
}

GROUPS = [
    ("coins", "gold coin money cash currency florin denar silver wealth funds purse"),
    ("mercenary", "merc hire ally companion follower recruit squad retinue"),
    ("health", "health hp blood conscious stamina energy life vital pain bleed wound injur fatigue dead"),
    ("armor", "armor armour helm plate mail gambeson protection"),
    ("weapon", "weapon sword blade damage sharp edge grip"),
    ("team", "team faction"),
    ("speed", "speed walk run sprint accel"),
    ("physics", "mass weight scale size gravity strength force muscle"),
]


def group_of(name: str) -> str:
    low = name.lower()
    for tag, words in GROUPS:
        if any(w in low for w in words.split()):
            return tag
    return "misc"


def tag(name: str) -> Text:
    return Text(f"#{name}", style=f"bold {COLORS.get(name, MUTED)}")


def tagged(label: str, tags: list[str], style: str = f"bold {TEXT}") -> Text:
    t = Text(label, style=style)
    for name in tags:
        t.append("  ")
        t.append_text(tag(name))
    return t


def is_modded(c: dict) -> bool:
    return any(abs((c.get(k) or 1) - 1) > 0.01 for k in ("scale", "mass", "speed"))


def char_tags(c: dict) -> list[str]:
    tags = [c.get("role", "opponent")]
    if c.get("dead"):
        tags.append("dead")
    if is_modded(c):
        tags.append("modded")
    return tags


def groups_of(name: str, limit: int = 2) -> list[str]:
    """All matching group tags for a (possibly dotted) name, e.g. Mercenaries[0].Health -> mercenary, health."""
    low = name.lower()
    out = [t for t, words in GROUPS if any(w in low for w in words.split())]
    return out[:limit] or ["misc"]

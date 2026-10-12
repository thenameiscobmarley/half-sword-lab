"""World mods. Keys match mods.cfg entries read by the Lua side."""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Mod:
    key: str
    title: str
    desc: str
    tags: list[str]
    params: list[tuple[str, str, str]] = field(default_factory=list)  # (cfg key, label, default)


MODS = [
    Mod("random_sizes", "Randomised Sizes Opponents", "Every opponent rolls a random size when it appears.",
        ["opponents", "chaos"], [("random_min", "min", "0.4"), ("random_max", "max", "2.2")]),
    Mod("opp_scale_on", "Opponent Sizes", "All opponents locked to one size (ignored while Randomised is on).",
        ["opponents"], [("opp_scale", "size", "1.8")]),
    Mod("ever_growing", "Ever-Growing Opponents", "Opponents grow a little every half second, up to 3x. Be quick.",
        ["opponents", "chaos"]),
    Mod("growth_spurt", "Growth Spurt", "You slowly grow while this is on.",
        ["player"], [("growth_cap", "cap", "2.5")]),
    Mod("caffeine", "Caffeine Lottery", "Each opponent gets a random speed between 0.5x and 1.8x.",
        ["opponents", "chaos"]),
    Mod("freeze_opp", "Statues", "Opponents slowed to almost a standstill.", ["opponents"]),
    Mod("hyper_opp", "Espresso Opponents", "Opponents move at 2.5x speed.", ["opponents", "chaos"]),
    Mod("feather_opp", "Paper Knights", "Opponents weigh almost nothing; hits send them flying.",
        ["opponents", "physics"]),
    Mod("anvil", "Anvil Mode", "You weigh 4x as much. Shove through anything.", ["player", "physics"]),
    Mod("giant_weapons", "Giant Weapons", "Weapons in your hands get scaled up.",
        ["weapons", "player"], [("weapon_factor", "x", "2.5")]),
    Mod("matrix", "Matrix Mode", "The world runs at 30% speed while you stay at full speed.", ["world", "player"]),
    Mod("slowmo", "Slow Motion", "Everything, including you, in slow motion.",
        ["world"], [("slowmo_value", "speed", "0.4")]),
    Mod("god_mode", "Stubborn", "Health/blood/consciousness-style stats can't drop below their best value. "
        "Uses auto-detected stat names.", ["player", "experimental"]),
]

GRAVITY = [
    ("Normal", "normal"), ("Moon (1/6 g)", "moon"), ("Mars", "mars"), ("Zero-G", "zero"),
    ("Heavy (3 g)", "heavy"), ("Reverse (fall up!)", "reverse"), ("Custom", "custom"),
]

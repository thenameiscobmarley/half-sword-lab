"""Writes a Half-Sword-style GVAS save for demo mode (and for testing both tag formats)."""
from __future__ import annotations

import struct
from pathlib import Path


def fstr(s: str) -> bytes:
    if not s:
        return struct.pack("<i", 0)
    b = s.encode("latin-1") + b"\0"
    return struct.pack("<i", len(b)) + b


class Writer:
    """Minimal GVAS writer. new_tags=True writes the UE5.4+ complete-type-name property tags."""

    def __init__(self, new_tags: bool = True):
        self.new = new_tags

    # -- type descriptions: ("IntProperty",) / ("StructProperty", "Vector") / ("ArrayProperty", inner) ...
    def _node(self, t) -> bytes:
        kind = t[0]
        if kind == "StructProperty":
            pkg = "/Script/CoreUObject" if t[1] in ("Vector", "Guid", "LinearColor") else "/Script/HalfSword"
            return fstr(kind) + struct.pack("<I", 1) + fstr(t[1]) + struct.pack("<I", 1) + fstr(pkg) + struct.pack("<I", 0)
        if kind in ("ArrayProperty", "SetProperty"):
            return fstr(kind) + struct.pack("<I", 1) + self._node(t[1])
        if kind == "MapProperty":
            return fstr(kind) + struct.pack("<I", 2) + self._node(t[1]) + self._node(t[2])
        return fstr(kind) + struct.pack("<I", 0)

    def prop(self, name: str, t, value: bytes, bool_value: bool = False) -> bytes:
        kind = t[0]
        if self.new:
            flags = 0x10 if (kind == "BoolProperty" and bool_value) else 0
            if kind == "StructProperty" and t[1] in ("Vector", "Guid", "LinearColor"):
                flags |= 0x08
            return fstr(name) + self._node(t) + struct.pack("<IB", len(value), flags) + value
        head = fstr(name) + fstr(kind) + struct.pack("<II", len(value), 0)
        if kind == "StructProperty":
            head += fstr(t[1]) + b"\0" * 16
        elif kind == "BoolProperty":
            head += bytes([1 if bool_value else 0])
        elif kind in ("ArrayProperty", "SetProperty"):
            head += fstr(t[1][0])
        elif kind == "MapProperty":
            head += fstr(t[1][0]) + fstr(t[2][0])
        return head + b"\0" + value

    # -- values
    def int_(self, name, v): return self.prop(name, ("IntProperty",), struct.pack("<i", v))
    def float_(self, name, v): return self.prop(name, ("FloatProperty",), struct.pack("<f", v))
    def bool_(self, name, v): return self.prop(name, ("BoolProperty",), b"", v)
    def str_(self, name, v): return self.prop(name, ("StrProperty",), fstr(v))
    def name_(self, name, v): return self.prop(name, ("NameProperty",), fstr(v))
    def vector(self, name, x, y, z): return self.prop(name, ("StructProperty", "Vector"), struct.pack("<ddd", x, y, z))

    def struct_(self, name, sname, props: list[bytes]) -> bytes:
        return self.prop(name, ("StructProperty", sname), b"".join(props) + fstr("None"))

    def struct_array(self, name, sname, items: list[list[bytes]]) -> bytes:
        body = b"".join(b"".join(p) + fstr("None") for p in items)
        value = struct.pack("<I", len(items))
        if not self.new:
            value += fstr(name) + fstr("StructProperty") + struct.pack("<II", len(body), 0) + fstr(sname) + b"\0" * 17
        return self.prop(name, ("ArrayProperty", ("StructProperty", sname)), value + body)

    def name_array(self, name, names: list[str]) -> bytes:
        return self.prop(name, ("ArrayProperty", ("NameProperty",)),
                         struct.pack("<I", len(names)) + b"".join(fstr(n) for n in names))

    def str_int_map(self, name, pairs: dict[str, int]) -> bytes:
        body = struct.pack("<II", 0, len(pairs)) + b"".join(fstr(k) + struct.pack("<i", v) for k, v in pairs.items())
        return self.prop(name, ("MapProperty", ("StrProperty",), ("IntProperty",)), body)

    def file(self, class_name: str, props: list[bytes]) -> bytes:
        engine = (5, 4, 4) if self.new else (5, 1, 1)
        head = b"GVAS" + struct.pack("<III", 3, 522, 1012 if self.new else 1008)
        head += struct.pack("<HHHI", *engine, 0) + fstr("++UE5+Release-%d.%d" % engine[:2])
        head += struct.pack("<II", 3, 0) + fstr(class_name) + (b"\0" if self.new else b"")
        return head + b"".join(props) + fstr("None") + b"\0\0\0\0"


def make_demo_save(path: Path, new_tags: bool = True) -> Path:
    w = Writer(new_tags)
    merc = lambda n, hp, lvl, hired, weapon: [w.str_("Name", n), w.float_("Health", hp), w.int_("Level", lvl),
                                              w.bool_("Hired", hired), w.name_("Weapon", weapon)]
    data = w.file("/Game/Core/HS_SaveGame.HS_SaveGame_C", [
        w.int_("Gold", 240),
        w.str_("PlayerName", "Willie"),
        w.struct_("PlayerStats", "PlayerStats", [
            w.float_("Health", 100.0), w.float_("MaxHealth", 100.0), w.float_("Blood", 5000.0),
            w.float_("Strength", 1.0), w.int_("Level", 3), w.int_("Kills", 42),
        ]),
        w.struct_("Armor", "ArmorState", [
            w.float_("HelmetDurability", 0.8), w.float_("ChestplateDurability", 0.55), w.bool_("HasGauntlets", True),
        ]),
        w.struct_array("Mercenaries", "MercenaryData", [
            merc("Bertram", 80.0, 2, True, "BP_Spear_C"),
            merc("Agnes", 120.0, 4, True, "BP_Longsword_C"),
            merc("Hob", 60.0, 1, False, "BP_Dagger_C"),
        ]),
        w.name_array("Inventory", ["BP_Longsword_C", "BP_Armor_Sallet_C", "BP_Buckler_C"]),
        w.str_int_map("ArenaWins", {"Pit": 5, "Courtyard": 2, "Tournament": 1}),
        w.vector("LastPosition", 1200.0, -340.5, 96.0),
        w.bool_("bHardcore", False),
        w.int_("Day", 17),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path

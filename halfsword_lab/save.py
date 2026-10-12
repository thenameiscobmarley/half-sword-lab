"""Unreal Engine save files (GVAS): read every value, edit numbers and flags in place.

Only fixed-size values (ints, floats, bools) are edited, and they are patched where they sit, so the file
keeps its exact layout and length. Anything the parser doesn't understand is skipped using the size every
property records, so unknown parts of a save are left byte-for-byte untouched.

Handles both property-tag formats: classic (UE4 / UE5.0-5.3) and "complete type name" (UE5.4+).
"""
from __future__ import annotations

import os
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

MAX_LEAVES = 50000

# Natively serialized structs: (field names, element format) per large-world-coordinates mode.
_VEC = {"Vector": "XYZ", "Vector2D": "XY", "Vector4": "XYZW", "Quat": "XYZW", "Rotator": "PYR"}


@dataclass
class Leaf:
    path: str
    type: str            # IntProperty, FloatProperty, BoolProperty, StrProperty, ...
    value: object
    offset: int          # where the value bytes live in the (decompressed) file
    fmt: str = ""        # struct format for numbers ("<i", "<f", ...); "" = read-only
    bool_mask: int = 0   # for bools: bit to flip at offset

    @property
    def editable(self) -> bool:
        return bool(self.fmt) or bool(self.bool_mask)

    @property
    def kind(self) -> str:
        if self.bool_mask:
            return "bool"
        if self.fmt in ("<f", "<d"):
            return "float"
        if self.fmt:
            return "int"
        return "text"


class SaveError(Exception):
    pass


class _R:
    def __init__(self, data: bytes, pos: int = 0):
        self.d, self.p = data, pos

    def take(self, fmt: str):
        v = struct.unpack_from(fmt, self.d, self.p)
        self.p += struct.calcsize(fmt)
        return v[0] if len(v) == 1 else v

    def i32(self): return self.take("<i")
    def u32(self): return self.take("<I")
    def u8(self): return self.take("<B")

    def fstring(self) -> str:
        n = self.i32()
        if n == 0:
            return ""
        if n > 0:
            if n > 1 << 20:
                raise SaveError(f"string too long at {self.p}")
            s = self.d[self.p:self.p + n]
            self.p += n
            return s[:-1].decode("latin-1")
        n = -n
        if n > 1 << 20:
            raise SaveError(f"string too long at {self.p}")
        s = self.d[self.p:self.p + n * 2]
        self.p += n * 2
        return s[:-2].decode("utf-16-le", "replace")

    def skip(self, n: int):
        self.p += n


@dataclass
class _Tag:
    name: str
    type: str
    size: int
    struct: str = ""       # struct type name
    inner: "_Tag | None" = None   # array/set element type, map key type
    value: "_Tag | None" = None   # map value type
    enum: str = ""
    bool_at: int = -1      # where a bool's value lives
    bool_mask: int = 0
    raw: bool = False      # UE5.4+ HasBinaryOrNativeSerialize


@dataclass
class SaveFile:
    path: Path
    data: bytearray = field(default_factory=bytearray)
    compressed: bool = False
    engine: tuple = (0, 0, 0)
    ue5: int = 0
    class_name: str = ""
    leaves: list[Leaf] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    # ------------------------------------------------------------------ loading
    @classmethod
    def load(cls, path: Path) -> "SaveFile":
        raw = Path(path).read_bytes()
        sf = cls(Path(path))
        if raw[:4] != b"GVAS":
            try:
                raw = zlib.decompress(raw)
                sf.compressed = True
            except zlib.error:
                raise SaveError("not an Unreal save file (no GVAS header)")
            if raw[:4] != b"GVAS":
                raise SaveError("compressed, but not an Unreal save inside")
        sf.data = bytearray(raw)
        sf._parse()
        return sf

    @property
    def new_tags(self) -> bool:
        return self.engine[:2] >= (5, 4)

    @property
    def lwc(self) -> bool:
        return self.ue5 >= 1004  # LARGE_WORLD_COORDINATES: vectors are doubles

    def _parse(self) -> None:
        r = _R(self.data, 4)
        save_ver = r.u32()
        r.u32()  # UE4 package version
        if save_ver >= 3 and save_ver != 34:
            self.ue5 = r.u32()
        self.engine = (r.take("<H"), r.take("<H"), r.take("<H"))
        r.u32()
        r.fstring()  # branch
        if self.engine[:2] >= (4, 12):
            r.u32()  # custom version format
            for _ in range(r.u32()):
                r.skip(20)
        self.class_name = r.fstring()
        if self.new_tags:
            r.u8()
        self._props(r, "", len(self.data))

    # ------------------------------------------------------------------ property lists
    def _props(self, r: _R, prefix: str, end: int) -> None:
        while r.p < end:
            name = r.fstring()
            if name == "None":
                return
            tag = self._tag(r, name)
            start = r.p
            path = f"{prefix}.{name}" if prefix else name
            try:
                self._value(r, tag, path, start + tag.size)
            except (SaveError, struct.error, UnicodeDecodeError, IndexError) as ex:
                self.warnings.append(f"{path}: skipped ({ex})")
            r.p = start + tag.size  # trust the recorded size no matter what we parsed

    def _tag(self, r: _R, name: str) -> _Tag:
        if self.new_tags:
            node = self._node(r)
            size = r.u32()
            flags_at = r.p
            flags = r.u8()
            tag = self._from_node(name, node, size, flags)
            if tag.type == "BoolProperty":
                tag.bool_at, tag.bool_mask = flags_at, 0x10
            if flags & 0x01:
                r.u32()
            if flags & 0x02:
                r.skip(16)
            if flags & 0x04:
                ext = r.u8()
                if ext & 0x02:
                    r.u8()
            return tag
        ttype = r.fstring()
        size = r.u32()
        r.u32()  # array index
        tag = _Tag(name, ttype, size)
        if ttype == "StructProperty":
            tag.struct = r.fstring()
            r.skip(16)
        elif ttype == "BoolProperty":
            tag.bool_at, tag.bool_mask = r.p, 0xFF
            r.u8()
        elif ttype in ("ByteProperty", "EnumProperty"):
            tag.enum = r.fstring()
        elif ttype in ("ArrayProperty", "SetProperty"):
            tag.inner = _Tag("", r.fstring(), 0)
        elif ttype == "MapProperty":
            tag.inner = _Tag("", r.fstring(), 0)
            tag.value = _Tag("", r.fstring(), 0)
        if r.u8():
            r.skip(16)
        return tag

    def _node(self, r: _R) -> tuple:
        name = r.fstring()
        return name, [self._node(r) for _ in range(r.u32())]

    def _from_node(self, name: str, node: tuple, size: int, flags: int) -> _Tag:
        t, inner = node
        tag = _Tag(name, t, size, raw=bool(flags & 0x08))
        if t == "StructProperty" and inner:
            tag.struct = inner[0][0]
        elif t in ("ArrayProperty", "SetProperty") and inner:
            tag.inner = self._from_node("", inner[0], 0, flags)
        elif t == "MapProperty" and len(inner) >= 2:
            tag.inner = self._from_node("", inner[0], 0, flags)
            tag.value = self._from_node("", inner[1], 0, flags)
        elif t in ("ByteProperty", "EnumProperty") and inner:
            tag.enum = inner[0][0]
        return tag

    # ------------------------------------------------------------------ values
    def _leaf(self, path: str, type_: str, value, offset: int, fmt: str = "", mask: int = 0) -> None:
        if len(self.leaves) < MAX_LEAVES:
            self.leaves.append(Leaf(path, type_, value, offset, fmt, mask))

    _NUM = {"IntProperty": "<i", "Int8Property": "<b", "Int16Property": "<h", "Int64Property": "<q",
            "UInt8Property": "<B", "UInt16Property": "<H", "UInt32Property": "<I", "UInt64Property": "<Q",
            "FloatProperty": "<f", "DoubleProperty": "<d"}

    def _value(self, r: _R, tag: _Tag, path: str, end: int, in_container: bool = False) -> None:
        t = tag.type
        if t in self._NUM:
            fmt = self._NUM[t]
            self._leaf(path, t, r.take(fmt), r.p - struct.calcsize(fmt), fmt)
        elif t == "BoolProperty":
            if in_container:
                self._leaf(path, t, bool(r.u8()), r.p - 1, mask=0xFF)
            else:
                self._leaf(path, t, bool(self.data[tag.bool_at] & tag.bool_mask), tag.bool_at, mask=tag.bool_mask)
        elif t == "ByteProperty":
            if (not tag.enum or tag.enum == "None") and (in_container or tag.size == 1):
                self._leaf(path, t, r.u8(), r.p - 1, "<B")
            else:
                self._leaf(path, t, r.fstring(), r.p)
        elif t in ("EnumProperty", "StrProperty", "NameProperty", "ObjectProperty", "SoftObjectProperty"):
            self._leaf(path, t, r.fstring(), r.p)
            if t == "SoftObjectProperty" and not in_container:
                r.p = end
        elif t == "TextProperty":
            self._leaf(path, t, self._text(r, end), r.p)
        elif t == "StructProperty":
            self._struct(r, tag, path, end)
        elif t in ("ArrayProperty", "SetProperty"):
            if t == "SetProperty" and r.u32():
                raise SaveError("sets with removed items aren't supported")
            self._array(r, tag, path, end)
        elif t == "MapProperty":
            self._map(r, tag, path, end)
        else:
            self._leaf(path, t, f"<{t}>", r.p)
            r.p = end

    def _text(self, r: _R, end: int) -> str:
        r.u32()  # flags
        history = r.take("<b")
        if history == -1:
            return r.fstring() if r.u32() else ""
        if history == 0:
            r.fstring()
            r.fstring()
            return r.fstring()
        r.p = end
        return "<text>"

    def _looks_like_props(self, r: _R) -> bool:
        if r.p + 4 > len(self.data):
            return False
        n = struct.unpack_from("<i", self.data, r.p)[0]
        name = self.data[r.p + 4:r.p + 4 + n] if 1 < n < 256 else b""
        return bool(name) and name[-1] == 0 and all(32 <= c < 127 for c in name[:-1])

    def _struct(self, r: _R, tag: _Tag, path: str, end: int) -> None:
        s = tag.struct.rsplit(".", 1)[-1]
        if not s:
            # Sets and maps in the classic format don't say which struct they hold: guess.
            s = "" if self._looks_like_props(r) else "Guid"
        if s in _VEC:
            fmt = "<d" if self.lwc else "<f"
            for axis in _VEC[s]:
                self._leaf(f"{path}.{axis}", "DoubleProperty" if self.lwc else "FloatProperty", r.take(fmt),
                           r.p - struct.calcsize(fmt), fmt)
        elif s == "LinearColor":
            for axis in "RGBA":
                self._leaf(f"{path}.{axis}", "FloatProperty", r.take("<f"), r.p - 4, "<f")
        elif s in ("IntPoint", "IntVector"):
            for axis in "XY" if s == "IntPoint" else "XYZ":
                self._leaf(f"{path}.{axis}", "IntProperty", r.i32(), r.p - 4, "<i")
        elif s == "Color":
            for axis in "BGRA":
                self._leaf(f"{path}.{axis}", "ByteProperty", r.u8(), r.p - 1, "<B")
        elif s in ("DateTime", "Timespan"):
            self._leaf(path, s, r.take("<q"), r.p - 8)
        elif s == "Guid":
            self._leaf(path, "Guid", self.data[r.p:r.p + 16].hex(), r.p)
            r.skip(16)
        elif s in ("SoftObjectPath", "SoftClassPath"):
            self._leaf(path, s, r.fstring(), r.p)
            r.fstring()
        elif s == "GameplayTagContainer":
            tags = [r.fstring() for _ in range(r.u32())]
            self._leaf(path, s, ", ".join(tags), r.p)
        elif tag.raw:
            raise SaveError(f"native struct {s}")
        else:
            self._props(r, path, end)

    def _elem_tag(self, r: _R, inner: _Tag) -> _Tag:
        if inner.type == "StructProperty" and not self.new_tags:
            # Classic format: struct arrays carry one inner tag describing the element struct.
            r.fstring()
            r.fstring()
            r.u32()
            r.u32()
            st = r.fstring()
            r.skip(16)
            if r.u8():
                r.skip(16)
            return _Tag("", "StructProperty", 0, struct=st)
        return inner

    def _array(self, r: _R, tag: _Tag, path: str, end: int) -> None:
        count = r.u32()
        # Only arrays (not sets) carry an inner struct tag in the classic format.
        inner = self._elem_tag(r, tag.inner) if count and tag.type == "ArrayProperty" else tag.inner
        if inner.type in ("ByteProperty", "BoolProperty") and count > 4096:
            self._leaf(path, f"{inner.type}[{count}]", f"<{count} bytes>", r.p)
            r.p = end
            return
        for i in range(count):
            if r.p >= end:
                raise SaveError("array ran past its size")
            self._value(r, inner, f"{path}[{i}]", end, True)

    def _map(self, r: _R, tag: _Tag, path: str, end: int) -> None:
        if r.u32():
            raise SaveError("maps with removed keys aren't supported")
        for i in range(r.u32()):
            key_start = len(self.leaves)
            self._value(r, tag.inner, f"{path}{{{i}}}", end, True)
            # The key only labels the entry; it isn't offered for editing (that could create duplicates).
            keys = self.leaves[key_start:]
            del self.leaves[key_start:]
            key = keys[0].value if len(keys) == 1 else i
            self._value(r, tag.value, f"{path}{{{key}}}", end, True)

    # ------------------------------------------------------------------ editing
    def set(self, leaf: Leaf, value) -> None:
        if leaf.bool_mask:
            on = value in (True, 1, "1", "true", "True")
            if on:
                self.data[leaf.offset] |= leaf.bool_mask if leaf.bool_mask != 0xFF else 1
            else:
                self.data[leaf.offset] &= ~leaf.bool_mask & 0xFF
            leaf.value = on
            return
        if not leaf.fmt:
            raise SaveError(f"{leaf.path} isn't editable")
        v = float(value) if leaf.fmt in ("<f", "<d") else int(float(value))
        struct.pack_into(leaf.fmt, self.data, leaf.offset, v)
        leaf.value = struct.unpack_from(leaf.fmt, self.data, leaf.offset)[0]

    def save(self, path: Path | None = None) -> Path:
        out = Path(path or self.path)
        blob = zlib.compress(bytes(self.data)) if self.compressed else bytes(self.data)
        tmp = out.with_name(out.name + ".tmp")
        tmp.write_bytes(blob)
        os.replace(tmp, out)
        return out

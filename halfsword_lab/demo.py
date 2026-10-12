"""Fake game state so the GUI can be explored without Half Sword running."""
from __future__ import annotations

import copy
import random
import time


def _props(**kw):
    return [{"n": k.replace("_", " ") if " " in k else k, "t": "bool" if isinstance(v, bool) else
             ("int" if isinstance(v, int) else "float"), "v": v} for k, v in kw.items()]


def _char(cid, name, cls, role, scale=1.0, **props):
    return {"id": cid, "name": name, "class": cls, "role": role, "dead": False,
            "scale": scale, "mass": 1.0, "speed": 1.0, "props": _props(**props), "gear": []}


class DemoBridge:
    demo = True

    def __init__(self):
        self.mods: dict[str, str] = {}
        self.log: list[dict] = []
        self.seq = 0
        self.chars = [
            _char("A1", "Player_Willie_C_0", "Player_Willie_C", "player", Health=100.0, Blood=5000.0,
                  Consciousness=1.0, Stamina=1.0, Team=0, Strength=1.0),
            _char("B2", "Merc_Hired_C_3", "Merc_Hired_C", "mercenary", Health=80.0, Blood=4200.0,
                  Consciousness=1.0, Team=0, IsHired=True),
            _char("C3", "Willie_NPC_C_7", "Willie_NPC_C", "opponent", 1.3, Health=120.0, Blood=5000.0,
                  Consciousness=1.0, Team=1, Aggression=0.8),
            _char("D4", "Willie_NPC_C_8", "Willie_NPC_C", "opponent", Health=60.0, Blood=900.0,
                  Consciousness=0.2, Team=1, Aggression=0.5),
        ]
        gear = {"A1": [("BP_Longsword_C_1", "BP_Longsword_C", "weapon"), ("Helmet_Sallet", "SM_Sallet", "armor"),
                       ("Gambeson", "SK_Gambeson", "armor")],
                "B2": [("BP_Spear_C_2", "BP_Spear_C", "weapon"), ("Mail_Shirt", "SK_MailShirt", "armor")],
                "C3": [("BP_Mace_C_4", "BP_Mace_C", "weapon")],
                "D4": [("BP_Dagger_C_9", "BP_Dagger_C", "weapon"), ("Bascinet", "SM_Bascinet", "armor")]}
        for c in self.chars:
            c["gear"] = [{"id": f"{c['id']}-{i}", "name": n, "class": k, "kind": kind,
                          "src": "component" if kind == "armor" else "actor", "scale": 1.0}
                         for i, (n, k, kind) in enumerate(gear[c["id"]])]
        self.economy = [{"owner": "GameInstance", "owner_id": "G1", "n": "Gold", "t": "int", "v": 240},
                        {"owner": "Save: HS_Save_0", "owner_id": "S1", "n": "Coins", "t": "int", "v": 240}]
        self.classes = {
            "weapons": [{"name": n, "path": f"/Game/Weapons/{n[:-2]}.{n}"} for n in
                        ("BP_Longsword_C", "BP_Spear_C", "BP_Mace_C", "BP_Dagger_C", "BP_Poleaxe_C", "BP_Buckler_C")],
            "armor": [{"name": n, "path": f"/Game/Armor/{n[:-2]}.{n}"} for n in
                      ("BP_Armor_Sallet_C", "BP_Armor_Cuirass_C", "BP_Armor_Gauntlets_C")],
            "characters": [{"name": "Willie_NPC_C", "path": "/Game/Characters/Willie_NPC.Willie_NPC_C"}],
        }
        self._say("demo mode: nothing here touches a real game")

    def _say(self, msg):
        self.seq += 1
        self.log = (self.log + [{"seq": self.seq, "msg": msg}])[-30:]

    def _find(self, oid):
        for c in self.chars:
            if c["id"] == oid:
                return c
            for g in c["gear"]:
                if g["id"] == oid:
                    return g
        for e in self.economy:
            if e["owner_id"] == oid:
                return e
        return None

    def read_state(self):
        for c in self.chars:
            for p in c["props"]:
                if p["n"] == "Stamina":
                    p["v"] = round(min(1.0, max(0.0, p["v"] + random.uniform(-0.05, 0.05))), 2)
        state = {"version": "demo", "t": time.time(), "world": {"map": "Arena_Demo", "gravity": -980.0},
                 "characters": self.chars, "economy": self.economy, "classes": self.classes, "log": self.log}
        return copy.deepcopy(state), True

    def send(self, verb, *args):
        a = [str(x) for x in args]
        o = self._find(a[0]) if a else None
        if verb == "set" and o:
            if "props" in o:
                for p in o["props"]:
                    if p["n"] == a[1]:
                        p["v"] = a[2] == "true" if p["t"] == "bool" else float(a[2]) if p["t"] == "float" else int(float(a[2]))
            else:
                for e in self.economy:
                    if e["owner_id"] == a[0] and e["n"] == a[1]:
                        e["v"] = int(float(a[2]))
        elif verb in ("scale", "mass", "speed") and o:
            o[verb] = float(a[1])
        elif verb == "reset" and o:
            o.update(scale=1.0, mass=1.0, speed=1.0)
        elif verb == "vanish":
            for c in self.chars:
                c["gear"] = [g for g in c["gear"] if g["id"] != a[0]]
            self.chars = [c for c in self.chars if c["id"] != a[0]]
        elif verb == "clone" and o:
            for _ in range(int(a[1])):
                n = copy.deepcopy(o)
                n["id"] = f"X{random.randint(100, 999)}"
                n["name"] = o["name"].rsplit("_", 1)[0] + f"_{random.randint(10, 99)}"
                n["role"] = "opponent" if o["role"] == "player" else o["role"]
                self.chars.append(n)
        self._say(f"{verb} {' '.join(a)}")

    def read_mods(self):
        return dict(self.mods)

    def write_mods(self, cfg):
        changed = {k: v for k, v in cfg.items() if self.mods.get(k) != v}
        self.mods = dict(cfg)
        for k, v in changed.items():
            self._say(f"mod {k} = {v}")

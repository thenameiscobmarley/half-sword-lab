# Handoff: finish Half Sword Lab on a machine that has the game

Paste this into Claude Code on the PC with Half Sword installed:

```text
Clone https://github.com/thenameiscobmarley/half-sword-lab and read README.md and CLAUDE_HANDOFF.md,
then follow the handoff plan in CLAUDE_HANDOFF.md to make every feature verified against the real game.
```

---

## Context for Claude

Half Sword Lab was built on a machine **without** the game. Everything except the in-game check is done
and tested: the terminal GUI (`halfsword_lab/`, Textual), the launchers (`run.sh`, `run.bat`), the UE4SS
installer/deployer, save backups, and the Lua mod (`mod/HalfSwordLab/Scripts/main.lua`). The Lua mod was
tested against a mock of the UE4SS API (character scan, mercenary detection, coins, gear, every world mod,
command bridge, panic reset, dump).

What is **not** verified, because nobody could see the game's real names:

| Area | Current approach in `main.lua` | Verify / replace with |
| --- | --- | --- |
| Characters | `FindAllOf("Character")`, fallback `Pawn` | the real player/NPC class names |
| Mercenaries | class name contains merc/hire/ally..., or a true bool like that, or same `Team` int as the player | the real flag/array that marks hired mercs |
| Stats | all numeric/bool props declared by game classes | confirm Health/Blood/Consciousness names, which ones are "max" |
| Coins | props named gold/coin/money/florin... on GameInstance, GameState, GameMode, PlayerState, PC, pawn, SaveGame | the real currency variable and the object it lives on |
| Weapons / armor | BP classes with weapon/armor words; actors attached to (or owned by) a character; mesh components with those words | how Half Sword really attaches gear |
| Spawning | `BeginDeferredActorSpawnFromClass` + `FinishSpawningActor` | whether spawned NPCs get an AI controller and fight |
| Weight | `Mesh:SetAllMassScale` | that the active-ragdoll still behaves |

## ⚠ The user's current game state

**One of the characters on this PC is already modded.** Before changing anything:

1. **Back up the saves first** (`./run.sh` → Setup → *Backup saves now*, or copy the `SaveGames` folder
   by hand). Check the backup exists before continuing.
2. Ask the user **how** it was modded (save editor? another UE4SS / Lua mod? Cheat Engine? a mod manager?).
   - If UE4SS is already installed, **do not reinstall it** and don't touch their other mods. The Lab's
     installer skips existing `UE4SS-settings.ini`, `mods.txt` and `mods.json`, and `deploy_mod` only adds
     its own line.
   - If another mod keeps re-applying values every tick, the Lab and that mod will fight. Spot it and tell
     the user.
3. Write down that character's current values (size, stats, coins) before testing. Never "reset to
   normal" the user's own modded values. Panic reset (F8) only undoes what the Lab itself changed, so keep
   it that way.

## Plan

1. **Find the install.** Note the game version (Steam build / in-game version) and the UE version
   (`strings *-Win64-Shipping.exe | grep -m1 "++UE5+Release"`). Run `./run.sh` (or `run.bat`) and check
   that the Setup tab finds the game, binary, saves and Steam launch option.
2. **Install + deploy** from the Setup tab. On Linux/Proton set the launch option
   `WINEDLLOVERRIDES="dwmapi=n,b" %command%`. If the experimental UE4SS doesn't load, try stable:
   `install_ue4ss(game, channel="stable")`. Check the log at `…/Win64/ue4ss/UE4SS.log` and look for
   `[HalfSwordLab] … loaded`.
3. **Dump.** Have the user start a fight **with a mercenary and opponents present**, then press **D** in
   the GUI. The dump is saved to `dumps/dump-*.txt`. It lists every property of the player, controller,
   player state, game instance/mode/state, save objects and each character class, plus every loaded
   blueprint class.
4. **Lock in real names.** Add a `KNOWN` table at the top of `main.lua` (player class, NPC classes,
   mercenary flag, coin owner + property, health-type stats and their max counterparts, weapon/armor base
   classes). Use it first and keep the keyword heuristics as a fallback, so the mod still survives updates.
5. **Verify every feature in-game with the user**, one by one, and fix what doesn't work:
   - [ ] character list + correct `#player` / `#mercenary` / `#opponent` / `#dead` tags
   - [ ] Body: size, weight, speed (check the ragdoll after scaling)
   - [ ] Stats edit (Health etc.), coins edit, **and that coins survive save + reload**
   - [ ] Armor/Weapons list, giant/tiny, hide/remove, spawn
   - [ ] Goofy buttons (launch, clone (do clones fight?), bring, teleport, freeze, invisible)
   - [ ] every mod in the Mods tab, gravity presets, Matrix Mode
   - [ ] F8 panic reset, both in the GUI and in-game
6. **Add Half-Sword-specific mods** once the real names are known. Ideas: free mercenary hiring, spawn a
   chosen enemy type, an "arena of clones" wave mode, permanent bleeding immunity, gear randomiser.
   Every new mod gets a row in `halfsword_lab/mods.py` with tags (`#opponents` `#world` `#player`
   `#chaos` `#weapons` `#physics` `#experimental`).
7. Update the README *Status* section (game version it was verified on), commit, and push.

## Repo map

```
run.sh / run.bat              launchers (python + venv + deps, then python -m halfsword_lab)
halfsword_lab/app.py          Textual GUI (tabs: Characters[Body/Stats/Armor/Weapons/Coins/Goofy], Mods, Setup, Log)
halfsword_lab/game.py         Steam/library/appid detection, UE4SS install, mod deploy, save backups
halfsword_lab/bridge.py       state.json / commands.txt / mods.cfg file bridge
halfsword_lab/mods.py         world mod definitions (keys = mods.cfg keys used in main.lua applyMods)
halfsword_lab/demo.py         fake game for --demo
halfsword_lab/tags.py         #tags + colours (ui-vault palette)
mod/HalfSwordLab/Scripts/main.lua   the in-game UE4SS Lua mod
```

Bridge protocol: commands are `id<TAB>verb<TAB>args…` lines. Ids only ever go up, and the mod skips ids
it has already run (and anything left over from before it loaded). Verbs: `set scale mass speed launch
bring visit clone spawn vanish hide show reset scan_classes dump panic`.

## User preferences

- Don't spawn subagents; they drain the user's quota.
- End every reply with a progress tag like `(50% Completed Response)`.
- GUI style follows https://github.com/thenameiscobmarley/ui-vault (dark, violet `#8b5cf6` / cyan `#22d3ee`).

# Handoff: finish Half Sword Lab on a machine that has the game

Paste this into Claude Code on the PC with Half Sword installed:

```text
Clone https://github.com/thenameiscobmarley/half-sword-lab and read README.md and CLAUDE_HANDOFF.md,
then follow the handoff plan in CLAUDE_HANDOFF.md to make every feature verified against the real game.
```

---

## Context for Claude

Half Sword Lab was built on a laptop **without** the game. The user does **not** want UE4SS, other mod
loaders, or memory scanners (Cheat Engine style). The Lab is a self-contained proxy `version.dll`
(`native/src`), plus a Python/Textual terminal app (`halfsword_lab/`). They talk through files in
`<Win64>/HalfSwordLab/bridge/`.

Verified on the laptop with `native/test/fakeue.cpp`, a fake UE5 game laid out like a real UE5 shipping
build, run under Wine (`python3 native/test/run_fake.py /tmp/hsl --menu [--d3d12]`):
- GUObjectArray / FName pool found by shape, and layout calibration
- ProcessEvent found by its FunctionFlags tests, then hooked
- game-thread commands and every mod
- the ImGui menu on D3D11 and D3D12
- the "Make Backup Before Applying Changes?" popup and in-game backups
- install / remove through the Proton prefix (`user.reg` AppDefaults override)

Also built: a **Save Editor** (`halfsword_lab/save.py` + `saveui.py`, `./save-editor.sh` for the save-only
edition). It parses GVAS (classic + UE5.4 tags) and patches numbers/bools in place. It was checked against a
real Deep Rock Galactic save (UE4.27, 0 parse warnings, edit = only the edited bytes change) and against
uesave-rs reading the demo saves it writes.

**Not verified, because nobody could see the real game:**

| Area | Current approach (`native/src/lab.cpp`) | Verify / fix |
| --- | --- | --- |
| Attaching | `ue.cpp` calibration against Actor/Pawn/SceneComponent/K2_DestroyActor | read `HalfSwordLab/lab.log`: the `layout:` line must have no `-1` |
| Game loads version.dll | PE import check in Setup tab | if it doesn't, add another proxy (`dwmapi`, `winmm`, `dinput8`...) in `dllmain.cpp` + `game.py PROXY` |
| Mercenaries | class/name contains merc/hire/ally..., a true bool like that, or same `Team` int as the player | **the team rule misfires when everyone's team is 0** (seen in the fake game). Find the real flag. |
| Stats | numeric/bool props declared by game classes | confirm Health/Blood/Consciousness names and which are "max" values |
| Coins | props named gold/coin/money/florin... on GameInstance, GameState, GameMode, PlayerState, PC, pawn, SaveGame | the real currency variable and the object it lives on; check it survives save + reload |
| Weapons / armor | BP classes with weapon/armor words; actors whose attach parent/Owner is a character; mesh components on the character | how Half Sword really attaches gear |
| Spawning / clones | `BeginDeferredActorSpawnFromClass` + `FinishSpawningActor` | do spawned NPCs get an AI controller and fight? |
| Weight | `Mesh:SetAllMassScale` | the active ragdoll still behaves |
| Save Editor | generic GVAS parse | open a **copy** of a real Half Sword save first: the header must show no "parts left as-is"; find gold / mercenary values; edit the copy, load it in-game |
| Overlay | Present/Present1/ResizeBuffers + ExecuteCommandLists hooks, input via raw mouse deltas | mouse in the menu while Half Sword has the cursor captured |

## ⚠ The user's current game state

**One of the characters on this PC is already modded.** Before changing anything:

1. **Back up the saves first** (`./run.sh` → Backups → *Back up saves now*). Check that the backup shows
   up in the list.
2. Ask the user **how** it was modded (save editor? UE4SS / another DLL mod? a mod manager?).
   - If another mod already uses `version.dll`, *Copy DLL* backs it up and *Remove* puts it back, but the
     two can't both be `version.dll`. Switch the Lab to a different proxy name in that case.
   - If another mod rewrites values every tick, it will fight the Lab. Spot it and tell the user.
3. Write down that character's current values (size, stats, coins) before testing. Never "reset to normal"
   the user's own modded values. F8 / panic only undoes what the Lab itself changed; keep it that way.

## Plan

1. **Install check.** `./run.sh` → Setup: game found, binary found, "Game loads version.dll" ✔,
   saves found. Note the game version and UE version
   (`strings *-Win64-Shipping.exe | grep -m1 "++UE5+Release"`).
2. **Copy DLL to game files**, start the game, and look for the in-game toast. Read
   `…/Win64/HalfSwordLab/lab.log`: it must say `layout: …` with no `-1`, then `ProcessEvent hooked` and
   `ready`. If calibration fails, the log names the step. Fix it in `native/src/ue.cpp`, rebuild with
   `native/build.sh`, and copy again.
3. **Dump.** Start a fight **with a mercenary and opponents present**, then press **D** in the terminal
   app. The dump is saved to `dumps/dump-*.txt`. It lists every property of the player, controller,
   player state, game instance/mode/state, save objects and each character class, plus all loaded
   blueprint classes.
4. **Lock in real names.** Add a `KNOWN` table at the top of `native/src/lab.cpp` (mercenary flag, coin
   owner + property, health-type stats and their max counterparts, weapon/armor classes). Use it first and
   keep the keyword guesses as a fallback.
5. **Verify every feature in-game with the user**, one by one:
   - [ ] character list + correct `#player` / `#mercenary` / `#opponent` / `#dead` tags
   - [ ] Body: size, weight, speed (check the ragdoll after scaling)
   - [ ] Stats edit; coins edit, **and that coins survive save + reload**
   - [ ] Armor/Weapons list, giant/tiny, hide/remove, spawn
   - [ ] Goofy buttons (launch, clone (do clones fight?), bring, teleport, freeze, invisible)
   - [ ] every mod in the Mods tab, gravity presets, Matrix Mode
   - [ ] in-game `\` menu: mouse works, backup popup works, F8 panic
   - [ ] Remove DLL restores everything
   - [ ] Save Editor: real saves parse, gold/mercenary edits stick after loading the save in-game
6. **Add Half-Sword-specific mods** once the real names are known. Ideas: free mercenary hiring, spawn a
   chosen enemy type, an "arena of clones" wave mode, bleeding immunity, a gear randomiser. Add each in
   `halfsword_lab/mods.py` **and** `native/src/overlay.cpp MODS`, with tags.
7. Rebuild the DLL, re-run `native/test/run_fake.py`, update the README *Status* section (game version
   it was verified on), commit, and push.

## Repo map

```
run.sh / run.bat                 launchers (list what's missing, ask y/n, then python -m halfsword_lab)
halfsword_lab/app.py             Textual dashboard: sidebar + Home landing page, Save Editor, Live Lab, Mods, Backups, Setup, Log
halfsword_lab/game.py            Steam/appid/prefix detection, PE import check, inject/remove, Wine registry, backups
halfsword_lab/bridge.py          state.json / commands.txt / mods.cfg
halfsword_lab/mods.py            world mod definitions (keys = mods.cfg keys used by lab.cpp applyMods)
halfsword_lab/demo.py            fake game for --demo
halfsword_lab/save.py            GVAS reader + in-place editor; saveui.py = Save Editor page; demo_save.py = test/demo saves
halfsword_lab/widgets.py         shared dialogs (BackupPrompt, EditValue, Confirm)
native/src/dllmain.cpp           version.dll export forwarding, loader thread, ProcessEvent hook (game thread)
native/src/ue.cpp                engine discovery + reflection (+ calibration)
native/src/lab.cpp               characters, gear, coins, mods, commands, state.json, in-game backups
native/src/overlay.cpp           D3D11/D3D12 Present hooks + ImGui menu (\ toggles, F8 panic)
native/test/fakeue.cpp           fake UE5 game for testing under Wine; run_fake.py drives it
native/dist/version.dll          prebuilt DLL that Setup copies into the game
```

Bridge protocol: commands are `id<TAB>verb<TAB>args…` lines. Ids only ever go up, and the DLL skips ids it
has already run (and anything left over from before it loaded). Verbs: `set scale mass speed launch bring
visit clone spawn vanish hide show reset scan_classes dump panic`.

## User preferences

- Don't spawn subagents; they drain the user's quota.
- End every reply with a progress tag like `(50% Completed Response)`.
- No UE4SS / mod loaders / Cheat-Engine-style memory scanning. Self-contained DLL only.
- Every change asks "Make Backup Before Applying Changes?"; `run.sh` asks y/n before installing anything.
- Style: "clean dashboard" (sidebar nav, landing page, flat cards, compact controls), in both the terminal app
  and the in-game menu; palette from https://github.com/thenameiscobmarley/ui-vault (violet `#8b5cf6` / cyan `#22d3ee`).

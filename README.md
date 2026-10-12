# ⚔ Half Sword Lab

A trainer and mod lab for **Half Sword**, with a terminal GUI and an in-game menu (press **`\`**).
Pick any character (you, your mercenaries, opponents) and change their size, weight, speed, stats, armor,
weapons and coins. Or switch on world mods like **Randomised Sizes Opponents**, **Matrix Mode** or
**Reverse Gravity**.

```
./run.sh          # Linux (KDE, Linux Mint, Steam Deck...), game running through Proton
run.bat           # Windows
./run.sh --demo   # look around with fake data, no game needed
```

The launcher lists what it needs (Python if missing, a private `.venv`, the
[Textual](https://textual.textualize.io) package) and asks **y/n before installing anything**.

## How it works

```
 terminal GUI (Python)  ⇄  HalfSwordLab/bridge files  ⇄  version.dll (the Lab)  →  Half Sword
                            state.json / commands.txt      loaded by the game,
                            mods.cfg                       draws the \ menu
```

* **No mod loader, no UE4SS, no Cheat Engine.** The Lab is one self-contained DLL
  ([`native/dist/version.dll`](native/dist)), built from the source in [`native/src`](native/src). The
  game loads it like a normal `version.dll`, and it forwards every real `version.dll` call to Windows/Wine.
* **Built to survive updates.** It doesn't use hardcoded memory addresses. When the game starts, the DLL
  locates Unreal's object table and name table by their shape, and works out the engine's layout
  from classes every UE5 game has. It then finds characters, stats, gear and coins by name through
  Unreal's own reflection.
* **In-game menu** (Dear ImGui over DirectX 11 or 12). It uses the same tags, colors and actions as the terminal app.

## First run

Open the **Setup** tab:

1. **Copy DLL to game files (with backup)**. Puts `version.dll` + a `HalfSwordLab` folder next to
   `HalfSwordUE5-Win64-Shipping.exe`. If a `version.dll` from another mod is already there, it is backed up
   first. On Linux it also switches the DLL on inside the game's own Proton prefix (after a backup of
   `user.reg`), so **no Steam launch options are needed**.
2. Start Half Sword. A "Half Sword Lab loaded - press \\" toast appears in the top-left, and the terminal
   header turns **● LIVE**.

**Remove DLL (restore originals)** undoes all of it.

## Backups

* Every change, in the terminal app and in the in-game menu, first asks
  **"Make Backup Before Applying Changes?"**: *Yes, back up first* / *No, just apply* / *Cancel*.
* Saves are also backed up automatically the first time the game connects each session, and before any
  restore.
* The **Backups** tab lists them all, **newest first**, tagged `#saves` `#gamefiles` and `#manual`
  `#change` `#in-game` `#session` `#pre-inject` `#pre-restore`, with Restore and Delete.
  They live in `~/HalfSwordLab-backups/`.

## What's in it

| Tab | What you can do |
| --- | --- |
| **Characters** | Everyone in the world, tagged `#player` `#mercenary` `#opponent` `#dead` `#modded` |
| Body | Size, Weight (physics mass), Speed (per-character time), presets from *Pocket* to *Giant* |
| Stats | Every number/flag on the character's blueprint (health, blood, consciousness, team...), tagged, editable |
| Armor / Weapons | Gear on the character: giant/tiny, feather/heavy, hide, remove, spawn more |
| Coins | Gold/coin values on the game instance, player and save objects |
| Goofy | Launch to the sky, clone army, bring to me, teleport, freeze, hyper, ant, colossus, balloon, invisible |
| **Mods** | Randomised Sizes Opponents, Opponent Sizes, Ever-Growing Opponents, Growth Spurt, Caffeine Lottery, Statues, Espresso Opponents, Paper Knights, Anvil Mode, Giant Weapons, Matrix Mode, Slow Motion, Stubborn, Gravity (Moon/Mars/Zero-G/Heavy/Reverse/Custom) |
| **Backups** | All backups, newest first: restore, delete, back up now |
| **Setup** | Copy/remove the DLL, status checks, data dump for Claude |

**F8** (in-game) is a panic button. It puts back everything the Lab changed and pauses mods until you
touch one again. Characters you modded some other way are left alone.

## Status

Tested without Half Sword, against [`native/test/fakeue.cpp`](native/test/fakeue.cpp). That's a fake
"UE5 game" laid out in memory like a real UE5 shipping build, run under Wine with the real DLL.
Verified there:
- the DLL finds the engine and calibrates
- every command and mod works through the bridge
- the in-game menu renders on both D3D11 and D3D12
- the backup prompt works, and in-game backups are written
- the Proton-prefix switch, the install, and the removal

Size, weight, speed, gravity, time and spawning use standard Unreal Engine functions. Recognising
mercenaries, and finding coin, health and gear names, is done by name matching and **still has to be
confirmed in the real game**. [`CLAUDE_HANDOFF.md`](CLAUDE_HANDOFF.md) covers that step.

## Building the DLL yourself

```
sudo pacman -S mingw-w64-gcc            # Arch / CachyOS
sudo apt install g++-mingw-w64-x86-64   # Debian / Linux Mint
native/build.sh                          # -> native/dist/version.dll
python3 native/test/run_fake.py /tmp/hsl --menu [--d3d12]   # end-to-end test under Wine
```

Bundled source (compiled in, nothing downloaded at runtime): [Dear ImGui](https://github.com/ocornut/imgui)
1.92.9 (MIT) and [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (BSD-2).

Single-player only. Don't use it anywhere it would ruin someone else's game.

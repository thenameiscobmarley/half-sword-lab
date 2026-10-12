# ⚔ Half Sword Lab

A terminal GUI trainer and mod lab for **Half Sword**. Pick any character (you, your mercenaries, opponents)
and change their size, weight, speed, stats, armor, weapons and coins. Or switch on world mods like
**Randomised Sizes Opponents**, **Matrix Mode** or **Reverse Gravity**.

```
./run.sh          # Linux / Steam Deck (Proton)
run.bat           # Windows
./run.sh --demo   # look around with fake data, no game needed
```

The launcher installs Python if it's missing (pacman / apt / dnf / zypper / apk / xbps, falling back to
[uv](https://github.com/astral-sh/uv); winget on Windows). It then sets up a private virtualenv with
[Textual](https://textual.textualize.io) and opens the GUI.

## How it works

```
 terminal GUI (Python/Textual)  ⇄  bridge files  ⇄  HalfSwordLab mod (Lua)  →  Half Sword
                                    state.json        runs inside the game
                                    commands.txt      through UE4SS
                                    mods.cfg
```

* [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) loads the `HalfSwordLab` Lua mod inside the game.
* The mod finds characters, stats, gear and coins through Unreal reflection, by **name** rather than memory
  addresses, so game updates don't break it. It writes everything to `state.json` once a second.
* The GUI sends commands back (`commands.txt`) and stores your mod toggles (`mods.cfg`). The mod applies
  them every half second.

## First run

Open the **Setup** tab:

1. **Install UE4SS**: downloads the latest build into `…/Binaries/Win64`. If you already have UE4SS, it is
   left alone.
2. **Deploy Lab mod**: copies the mod and enables it in `mods.txt` / `mods.json`. Your other mods are kept.
3. **Linux only:** Steam → Half Sword → Properties → Launch Options:
   `WINEDLLOVERRIDES="dwmapi=n,b" %command%` (the **Copy Steam launch option** button copies it for you).
4. Start the game. The header turns **● LIVE**.

Your saves are backed up to `~/HalfSwordLab-backups/` before deploying and the first time the game connects
each session. Press **B** to back up any time.

## What's in it

| Tab | What you can do |
| --- | --- |
| **Characters** | Everyone in the world, tagged `#player` `#mercenary` `#opponent` `#dead` `#modded` |
| Body | Size, Weight (physics mass), Speed (per-character time), with presets from *Pocket* to *Giant* |
| Stats | Every number/flag the character's blueprint has (health, blood, consciousness, team...), tagged and editable |
| Armor / Weapons | Gear on the character: make it giant or tiny, feather or heavy, hide it, remove it, spawn more |
| Coins | Gold/coin values found on the game instance, player and save objects |
| Goofy | Launch to the sky, clone army, bring to me, teleport, freeze, hyper, ant, colossus, balloon, invisible |
| **Mods** | Randomised Sizes Opponents, Opponent Sizes, Ever-Growing Opponents, Growth Spurt, Caffeine Lottery, Statues, Espresso Opponents, Paper Knights, Anvil Mode, Giant Weapons, Matrix Mode, Slow Motion, Stubborn, Gravity (Moon/Mars/Zero-G/Heavy/Reverse/Custom) |
| **Setup** | Install, deploy, backups, data dump |

**F8** (in the GUI *or* in-game) is a panic button. It puts back everything the Lab changed and pauses mods
until you touch one again. Characters you modded some other way are not touched.

## Status

Built and tested without the game: the GUI in demo mode, and the Lua mod against a mock of the UE4SS API.
Size, weight, speed, gravity, time and spawning use standard Unreal Engine functions. Stat, coin, gear and
mercenary detection is done by name matching, and still needs to be confirmed on a real install.
That's what [`CLAUDE_HANDOFF.md`](CLAUDE_HANDOFF.md) is for.

To restore a backup: quit the game, then
`tar -xzf ~/HalfSwordLab-backups/<file>.tar.gz -C <AppData/Local folder>`
(on Proton that's `…/compatdata/<appid>/pfx/drive_c/users/steamuser/AppData/Local`).

Single-player only. Don't use it anywhere it would ruin someone else's game.

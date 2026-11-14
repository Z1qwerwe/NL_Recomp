<img width="708" height="711" alt="image" src="https://github.com/user-attachments/assets/148bec71-1191-4882-9dfa-3f970b308871" />




# Nintendo Land Recomp

An unofficial project that runs **Nintendo Land (Wii U)** natively on PC — its own `NintendoLand.exe` with its own window, in-game settings menu, controller support and **online play with friends over Steam**.

> **No game files and no code generated from the game are included — not in this repo and not in any release.**
> You need your own dump of Nintendo Land (e.g. made with dumpling from your Wii U). The launcher builds the game on your PC from your dump.

## Quick start

1. Install **Python 3.8+** (python.org, tick "Add to PATH").
2. Download the release ZIP, extract it, run **`start.bat`**.
3. Pick your extracted game folder (the one with `code/`, `content/`, `meta/`).
4. Click **Build game**. If Git or Visual Studio Build Tools are missing, the launcher offers to install them (via winget, ~6–8 GB, asks for admin rights once). The first build takes a while.
5. Click **PLAY**.

## In-game

- **Esc** (or the Home/Guide button) opens the menu: Screen, Game, Sound, Controls, **Online**
- **F11** / **Alt+Enter** fullscreen · **Tab** swap TV/GamePad image · click into the GamePad image = touch
- Click into the big image to capture the mouse as camera (Esc releases it)
- Controllers: Xbox, PlayStation (incl. gyro), Switch Pro … The first one is the GamePad, every further one becomes a Wiimote (with Nunchuk)

## Online with friends

The host runs the game; friends see the host's TV image and play as **Wiimote players 2–5**. Friends **don't need the game**.

- **Host:** Esc → **Online** → *Open room* → share the 6-letter code (or invite friends from the friend list there).
- **Friends:** run **NLOnline.exe** (from `NLOnline_fuer_Freunde.zip` in the release or the host's `bin` folder), enter the code or click *Join* next to the host in the friend list.
- Needs Steam running on both sides (uses Steam's test app "Spacewar", AppID 480, for friends, codes and NAT traversal).
- Upload per friend: ~3.5 / 6 / 12 Mbit/s (Low / Medium / High in the Online menu).

## How it works

- `recomp/` statically recompiles the game's PowerPC code to C++ (on your PC, from your dump).
- Cemu's core libraries (MPL-2.0) provide the Wii U OS, graphics (Vulkan), audio and input; Cemu's UI isn't used. `nlrecomp/cemu.py` fetches a pinned Cemu version and applies small patches.
- `frontend/` is the game program (window, menu, input, online), `frontend/client/` is NLOnline.exe.

| Folder | Contents |
|---|---|
| `launcher.py`, `nlrecomp/` | Launcher: detect the game, recompile, install build tools, build, launch |
| `recomp/` | Recompiler `.rpx` → C++ |
| `cemu/aot/` | Integration of the recompiled code into Cemu |
| `frontend/` | NintendoLand.exe and NLOnline.exe |
| `third_party/steam/` | `steam_api64.dll` (Valve redistributable) |
| `runtime/`, `aroma-plugin/`, `tools/` | Experiments and helper tools |

## Legal

Nintendo Land is © Nintendo. This project is not affiliated with Nintendo. It contains no Nintendo code or assets; you must own the game. Cemu is licensed under MPL-2.0. `steam_api64.dll` is redistributed under the Steamworks SDK terms.

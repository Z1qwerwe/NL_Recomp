# Nintendo Land Recomp

An unofficial attempt to run **Nintendo Land (Wii U)** natively on PC via static recompilation, with the goal of adding online multiplayer.

> This repo contains **no** game files and no code generated from the game.
> You need your own dump of Nintendo Land (e.g. made with dumpling from your Wii U).

## Two approaches

1. **Cemu version (recommended):** Cemu (MPL-2.0) provides the operating system, graphics (Vulkan/OpenGL), audio, controllers and microphone. Instead of Cemu's CPU emulation, the recompiled native game functions (`cemu/aot/`) are run. In the launcher: **Build Cemu version**, then **PLAY**.
2. **Custom runtime:** a fully custom implementation (`runtime/`), good for testing and debugging, but no real graphics yet.

## Structure

| Folder | Contents |
|---|---|
| `launcher.py`, `nlrecomp/` | Launcher (Python/Tkinter): detect the game, analyze, collect shaders, recompile, build, launch |
| `recomp/` | Recompiler: `.rpx` → C++ (PowerPC/Espresso incl. paired singles) |
| `runtime/` | Custom C++ runtime: memory, HLE of the Wii U system libraries (coreinit, FS, threads, …) |
| `cemu/aot/` | Cemu integration: headers for the generated code, `nl_aot_glue.cpp`, CMake integration |
| `nlrecomp/cemu.py` | Fetches Cemu (pinned version), applies the small patches and builds everything |
| `aroma-plugin/` | Experimental Aroma plugin for netplay on real hardware |
| `tools/` | Helper tools (RPX import analysis) |

## Usage

1. Install Python 3.8+ and Visual Studio 2022 with "Desktop development with C++".
2. Run `start.bat` and select the extracted game folder (containing `code/`, `content/`, `meta/`).
3. **Recompile + Build**, then **PLAY**.

## Status

- [x] Load RPX, relocations, 99,749 functions to C++
- [x] Game runs up to graphics initialization
- [x] Threads, mutexes, events, message queues, alarms
- [x] File system (`/vol/content`, `/vol/save`)
- [x] Real MEM heaps (Expanded/Frame/Default)
- [x] Window (SDL3), TV image + GamePad image, F2 swaps the view
- [x] Input: controllers, mouse/keyboard, remapping in-game (F1), additional controllers as Wiimotes
- [x] Microphone (GamePad mic via the PC microphone)
- [~] GX2: memory sizes, clear, scan buffer, VSync (no real draw calls yet)
- [ ] GX2 → Vulkan/OpenGL with Latte shader translation (proper image)
- [ ] Online multiplayer

## Controls (default)

| GamePad | Keyboard | Controller |
|---|---|---|
| Left stick | W A S D | Left stick |
| Right stick | I J K L | Right stick |
| A / B / X / Y | Enter / Backspace / X / Y | Right / Down / Up / Left |
| L / R / ZL / ZR | Q / E / 1 / 3 | Shoulder buttons / Triggers |
| + / − / HOME | P / M / H | Start / Back / Guide |
| Touchscreen | Mouse on the GamePad image | – |

**F1** opens the in-game remapping menu (saved to `controls.ini`), where you can also toggle the microphone on/off. **F2** shows the GamePad image large. Additional controllers are detected as Wiimotes (with Nunchuk).

## Debugging

- `NL_TRACE_HLE=1`: log every system function call
- `NL_TRACE_FS=1`: log file accesses
- CMake `-DNL_TRACE_FUNCS=ON` + `NL_DUMP_ON=<function name>`: print the last 64 game functions when a system function is called


## NintendoLand.exe (standalone program)

Like Zelda64Recomp: its own .exe with its own window, its own icon and its own settings.
The launcher builds it via **Build game** and launches it straight into the game with **PLAY**.

- Code: `frontend/main.cpp` (Win32 window, settings, input), build: `frontend/nl_frontend.cmake`
- Foundation: Cemu's core libraries (MPL-2.0) for Cafe OS functions, Vulkan graphics, audio and controllers.
  Cemu's UI (wxWidgets) is not used.
- Settings: `nl_data/nl_settings.ini` next to the .exe (written by the launcher under *Settings*)
- In-game keys: **F11** / **Alt+Enter** fullscreen, **Tab** switch between TV/GamePad image, mouse = touchscreen
- Controllers are detected automatically: the first one is the GamePad, any others become Wiimotes.

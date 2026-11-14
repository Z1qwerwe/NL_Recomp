# Nintendo Land Recomp

Inoffizieller Versuch, **Nintendo Land (Wii U)** per Static Recompilation nativ auf dem PC laufen zu lassen, mit dem Ziel, einen Online-Multiplayer einzubauen.

> Dieses Repo enthält **keine** Spieldateien und keinen aus dem Spiel erzeugten Code.
> Du brauchst deinen eigenen Dump von Nintendo Land (z. B. mit dumpling von deiner Wii U).

## Aufbau

| Ordner | Inhalt |
|---|---|
| `launcher.py`, `nlrecomp/` | Launcher (Python/Tkinter): Spiel erkennen, analysieren, Shader sammeln, recompilen, bauen, starten |
| `recomp/` | Recompiler: `.rpx` → C++ (PowerPC/Espresso inkl. Paired Singles) |
| `runtime/` | C++-Runtime: Speicher, HLE der Wii-U-Systembibliotheken (coreinit, FS, Threads, …) |
| `aroma-plugin/` | Experimentelles Aroma-Plugin für Netplay auf echter Hardware |
| `tools/` | Hilfstools (RPX-Import-Analyse) |

## Benutzung

1. Python 3.8+ und Visual Studio 2022 mit „Desktopentwicklung mit C++“ installieren.
2. `start.bat` starten und den entpackten Spielordner (mit `code/`, `content/`, `meta/`) auswählen.
3. **Recompilen + Bauen**, dann **SPIELEN**.

## Stand

- [x] RPX laden, Relocations, 99.749 Funktionen nach C++
- [x] Spiel läuft bis zur Grafik-Initialisierung
- [x] Threads, Mutexe, Events, Message-Queues, Alarme
- [x] Dateisystem (`/vol/content`, `/vol/save`)
- [ ] GX2 → Vulkan/OpenGL (Bild)
- [ ] Eingabe (Controller, Maus/Tastatur, Remapping), Mikrofon
- [ ] Online-Multiplayer

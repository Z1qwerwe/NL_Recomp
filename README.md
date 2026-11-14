# Nintendo Land Recomp

Inoffizieller Versuch, **Nintendo Land (Wii U)** per Static Recompilation nativ auf dem PC laufen zu lassen, mit dem Ziel, einen Online-Multiplayer einzubauen.

> Dieses Repo enthält **keine** Spieldateien und keinen aus dem Spiel erzeugten Code.
> Du brauchst deinen eigenen Dump von Nintendo Land (z. B. mit dumpling von deiner Wii U).

## Zwei Wege

1. **Cemu-Version (empfohlen):** Cemu (MPL-2.0) liefert Betriebssystem, Grafik (Vulkan/OpenGL), Audio, Controller und Mikrofon. Statt Cemus CPU-Emulation laufen die recompilierten, nativen Spielfunktionen (`cemu/aot/`). Im Launcher: **Cemu-Version bauen**, dann **SPIELEN**.
2. **Eigene Runtime:** komplett eigene Umsetzung (`runtime/`), gut zum Testen und Debuggen, aber noch ohne echte Grafik.

## Aufbau

| Ordner | Inhalt |
|---|---|
| `launcher.py`, `nlrecomp/` | Launcher (Python/Tkinter): Spiel erkennen, analysieren, Shader sammeln, recompilen, bauen, starten |
| `recomp/` | Recompiler: `.rpx` → C++ (PowerPC/Espresso inkl. Paired Singles) |
| `runtime/` | Eigene C++-Runtime: Speicher, HLE der Wii-U-Systembibliotheken (coreinit, FS, Threads, …) |
| `cemu/aot/` | Anbindung an Cemu: Header für den generierten Code, `nl_aot_glue.cpp`, CMake-Einbindung |
| `nlrecomp/cemu.py` | Holt Cemu (fester Stand), wendet die kleinen Anpassungen an und baut alles |
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
- [x] Echte MEM-Heaps (Expanded/Frame/Default)
- [x] Fenster (SDL3), TV-Bild + GamePad-Bild, F2 tauscht die Ansicht
- [x] Eingabe: Controller, Maus/Tastatur, Belegung im Spiel ändern (F1), weitere Controller als Wiimotes
- [x] Mikrofon (GamePad-Mikro über das PC-Mikrofon)
- [~] GX2: Speichergrößen, Clear, Scan-Buffer, VSync (noch keine echten Draw-Calls)
- [ ] GX2 → Vulkan/OpenGL mit Latte-Shader-Übersetzung (richtiges Bild)
- [ ] Online-Multiplayer

## Steuerung (Standard)

| GamePad | Tastatur | Controller |
|---|---|---|
| Linker Stick | W A S D | linker Stick |
| Rechter Stick | I J K L | rechter Stick |
| A / B / X / Y | Enter / Rücktaste / X / Y | rechts / unten / oben / links |
| L / R / ZL / ZR | Q / E / 1 / 3 | Schultertasten / Trigger |
| + / − / HOME | P / M / H | Start / Back / Guide |
| Touchscreen | Maus auf dem GamePad-Bild | – |

**F1** öffnet im Spiel das Menü zum Umbelegen (wird in `controls.ini` gespeichert), dort lässt sich auch das Mikrofon an/aus schalten. **F2** zeigt das GamePad-Bild groß. Weitere Controller werden als Wiimotes (mit Nunchuk) erkannt.

## Debuggen

- `NL_TRACE_HLE=1`: jeden Aufruf einer Systemfunktion loggen
- `NL_TRACE_FS=1`: Dateizugriffe loggen
- CMake `-DNL_TRACE_FUNCS=ON` + `NL_DUMP_ON=<Funktionsname>`: die letzten 64 Spielfunktionen ausgeben, wenn eine Systemfunktion aufgerufen wird

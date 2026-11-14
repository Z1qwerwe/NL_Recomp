"""Die einzelnen Schritte vom Launcher, ohne GUI (auch per Kommandozeile nutzbar)."""
import json
import os
import shutil
import subprocess

from . import rpx as rpxmod
from . import shaders
from .game import find_game

APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SETTINGS_PATH = os.path.join(APP_DIR, "settings.json")

DEFAULT_SETTINGS = {
    "base_dir": "",
    "update_dir": "",
    "work_dir": os.path.join(APP_DIR, "work"),
    # Eigener Recompiler-Befehl ({rpx} und {out} werden ersetzt). Leer = eingebauter Recompiler.
    "recompiler_cmd": "",
    # Shader-Translator der Runtime (kommt spaeter), siehe shaders.translate_shaders
    "shader_translator": "",
    # Ordner mit dem CMake-Projekt der Runtime. Leer = mitgelieferte Runtime.
    "runtime_dir": "",
    "exe_name": "NintendoLand.exe",
}


def load_settings():
    s = dict(DEFAULT_SETTINGS)
    if os.path.isfile(SETTINGS_PATH):
        s.update(json.load(open(SETTINGS_PATH, encoding="utf-8")))
    return s


def save_settings(s):
    json.dump(s, open(SETTINGS_PATH, "w", encoding="utf-8"), indent=2)


def _game_work_dir(settings, game):
    tid = game.base.title_id or "unknown"
    d = os.path.join(settings["work_dir"], tid)
    os.makedirs(d, exist_ok=True)
    return d


def step_detect(settings, log):
    game = find_game(settings["base_dir"], settings["update_dir"] or None)
    b = game.base
    log(f"Spiel: {b.name or '?'}  [{b.title_id or '?'}]  v{b.version}  ({b.kind})")
    if game.update:
        u = game.update
        log(f"Update: [{u.title_id or '?'}]  v{u.version}  ({u.kind})")
    for w in game.warnings:
        log(f"WARNUNG: {w}")
    if game.rpx:
        log(f"RPX: {game.rpx}")
    return game


def step_analyze(settings, game, log):
    if not game.rpx:
        raise RuntimeError("Keine entschluesselte .rpx gefunden.")
    out = _game_work_dir(settings, game)
    r = rpxmod.analyze(game.rpx)
    json.dump(r, open(os.path.join(out, "imports.json"), "w"), indent=2)
    open(os.path.join(out, "imports.md"), "w", encoding="utf-8").write(rpxmod.to_markdown(r))
    log(f"~{r['approx_instructions']:,} PowerPC-Instruktionen, "
        f"{sum(len(d['functions']) for d in r['imports'].values())} importierte Funktionen")
    for lib, d in sorted(r["imports"].items(), key=lambda x: -len(x[1]["functions"])):
        log(f"  {lib:<12} {len(d['functions']):>4}")
    return r


def step_shaders(settings, game, log, progress, stop):
    cache = os.path.join(_game_work_dir(settings, game), "shader_cache")
    roots = game.content_roots()
    if not roots:
        raise RuntimeError("Kein content/-Ordner gefunden.")
    log("Shader werden aus den Spieldateien geholt (beim ersten Mal dauert's ein paar Minuten)...")
    shaders.extract_shaders(roots, cache, log, progress, stop)
    shaders.translate_shaders(cache, settings["shader_translator"], log, progress, stop)


def _run(cmd, log, cwd=None, stop=lambda: False):
    log(f"> {cmd if isinstance(cmd, str) else ' '.join(cmd)}")
    p = subprocess.Popen(cmd, cwd=cwd, shell=isinstance(cmd, str),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in p.stdout:
        log("  " + line.rstrip())
        if stop():
            p.terminate()
            break
    if p.wait() != 0:
        raise RuntimeError(f"Befehl fehlgeschlagen (Code {p.returncode})")


def step_recompile(settings, game, log, stop):
    out = os.path.join(_game_work_dir(settings, game), "generated")
    os.makedirs(out, exist_ok=True)
    if settings["recompiler_cmd"]:
        cmd = settings["recompiler_cmd"].format(rpx=f'"{game.rpx}"', out=f'"{out}"')
        _run(cmd, log, stop=stop)
        return
    # Eingebauter Recompiler
    import sys
    sys.path.insert(0, os.path.join(APP_DIR, "recomp"))
    from rpx_loader import load, write_image
    from emit import generate, add_secondary_entries
    log(f"Recompiler: lade {os.path.basename(game.rpx)} ...")
    rpx = load(game.rpx)
    log(f"  {len(rpx.func_syms)} Funktionen, {len(rpx.imports)} Imports")
    add_secondary_entries(rpx, log)
    write_image(rpx, os.path.join(out, "image.bin"))
    log("Erzeuge C++-Code (dauert ca. 20-60 Sekunden) ...")
    _files, unimpl = generate(rpx, out, log=log)
    if unimpl:
        log(f"WARNUNG: nicht uebersetzte Instruktionen: {unimpl}")
    log(f"C++-Code liegt in {out}")


def _find_cmake():
    c = shutil.which("cmake")
    if c:
        return c
    # CMake, das bei Visual Studio dabei ist
    import glob
    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"),
               os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        hits = glob.glob(os.path.join(pf, "Microsoft Visual Studio", "*", "*", "Common7", "IDE",
                                      "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe"))
        if hits:
            return sorted(hits)[-1]
    return None


def step_build(settings, game, log, stop):
    rt = settings["runtime_dir"] or os.path.join(APP_DIR, "runtime")
    if not os.path.isfile(os.path.join(rt, "CMakeLists.txt")):
        raise RuntimeError(f"Kein Runtime-Projekt (CMakeLists.txt) in {rt}.")
    cmake = _find_cmake()
    if not cmake:
        raise RuntimeError("CMake nicht gefunden. Installier Visual Studio 2022 mit "
                           "'Desktopentwicklung mit C++' (da ist CMake dabei).")
    wd = _game_work_dir(settings, game)
    build = os.path.join(wd, "build")
    gen = os.path.join(wd, "generated")
    _kill_running(settings, log)
    log("Kompiliere (beim ersten Mal ein paar Minuten, ~150 MB C++) ...")
    # Jeder Compiler-Prozess braucht bei den grossen Dateien ~1-2 GB RAM, deshalb begrenzt
    jobs = str(max(1, min(4, (os.cpu_count() or 2) // 2)))
    gen_cm = gen.replace("\\", "/")
    _run([cmake, "-S", rt, "-B", build, f"-DGENERATED_DIR={gen_cm}", f"-DNL_JOBS={jobs}"], log, stop=stop)
    _run([cmake, "--build", build, "--config", "Release", "--parallel", jobs], log, stop=stop)


def find_exe(settings, game):
    build = os.path.join(_game_work_dir(settings, game), "build")
    for dirpath, _, files in os.walk(build):
        if settings["exe_name"] in files:
            return os.path.join(dirpath, settings["exe_name"])
    return None


def _kill_running(settings, log):
    """Beendet eine noch laufende NintendoLand.exe (sonst kann der Linker sie nicht ueberschreiben)."""
    if os.name != "nt":
        return
    r = subprocess.run(["taskkill", "/F", "/IM", settings["exe_name"]], capture_output=True, text=True)
    if r.returncode == 0:
        log("Laufende Spiel-Instanz beendet.")


def step_launch(settings, game, log):
    exe = find_exe(settings, game)
    if not exe:
        raise RuntimeError("Noch keine fertige .exe. Erst recompilen und bauen.")
    _kill_running(settings, log)
    wd = _game_work_dir(settings, game)
    cache = os.path.join(wd, "shader_cache")
    env = dict(os.environ,
               NL_CONTENT_DIRS=os.pathsep.join(game.content_roots()),
               NL_SHADER_CACHE=cache,
               NL_SAVE_DIR=os.path.join(wd, "save"))
    image = os.path.join(wd, "generated", "image.bin")
    log_path = os.path.join(wd, "game.log")
    log(f"Starte {exe}")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    p = subprocess.Popen([exe, image], cwd=os.path.dirname(exe), env=env, creationflags=flags,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace", bufsize=1)

    def pump():
        with open(log_path, "w", encoding="utf-8") as f:
            for line in p.stdout:
                f.write(line)
                f.flush()
                log("  " + line.rstrip())
            code = p.wait()
            f.write(f"\n[Beendet mit Code {code}]\n")
        log(f"Spiel beendet (Code {code}). Log: {log_path}")

    import threading
    threading.Thread(target=pump, daemon=True).start()

"""Cemu als Basis: Quellcode holen, fuer Nintendo Land Recomp anpassen, bauen, starten.

Cemu (MPL-2.0, https://github.com/cemu-project/Cemu) liefert Betriebssystem, Grafik,
Audio und Eingabe. Unser recompilierter Spielcode ersetzt Cemus CPU-Emulation.
"""
import glob
import os
import shutil
import subprocess

CEMU_URL = "https://github.com/cemu-project/Cemu"
CEMU_COMMIT = "d4f720597e022737782e6215c0ec3ab1d9ae6636"   # getesteter Stand
MARK = "# --- Nintendo Land Recomp ---"


def find_git():
    g = shutil.which("git")
    if g:
        return g
    # Visual Studio bringt ein eigenes Git mit
    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"),
               os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        hits = glob.glob(os.path.join(pf, "Microsoft Visual Studio", "*", "*", "Common7", "IDE",
                                      "CommonExtensions", "Microsoft", "TeamFoundation",
                                      "Team Explorer", "Git", "cmd", "git.exe"))
        if hits:
            return sorted(hits)[-1]
    return None


def git_env(git):
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(git) + os.pathsep + env.get("PATH", "")
    return env


def fetch(cemu_dir, run, log):
    git = find_git()
    if not git:
        raise RuntimeError("Git nicht gefunden. Installier 'Git for Windows' oder in Visual Studio "
                           "die Komponente 'Git fuer Windows'.")
    env = git_env(git)
    if not os.path.isdir(os.path.join(cemu_dir, ".git")):
        log("Lade Cemu-Quellcode (einmalig, ein paar hundert MB) ...")
        run([git, "clone", CEMU_URL, cemu_dir], env=env)
    run([git, "-C", cemu_dir, "fetch", "--depth", "1", "origin", CEMU_COMMIT], env=env, check=False)
    run([git, "-C", cemu_dir, "checkout", "-f", CEMU_COMMIT], env=env)
    log("Lade Cemu-Untermodule (vcpkg, imgui, ...) ...")
    run([git, "-C", cemu_dir, "submodule", "update", "--init", "--recursive"], env=env)


def _patch_file(path, old, new, log):
    s = open(path, encoding="utf-8").read()
    if new in s:
        return
    if old not in s:
        raise RuntimeError(f"Cemu-Patch passt nicht mehr: {os.path.basename(path)}")
    s = s.replace(old, new, 1)
    open(path, "w", encoding="utf-8", newline="\n").write(s)
    log(f"  gepatcht: {os.path.relpath(path, os.path.dirname(os.path.dirname(path)))}")


def patch(cemu_dir, log):
    """Kleine Aenderungen an Cemu (bleiben MPL-2.0)."""
    # 1. CMake: unseren AOT-Code einbinden
    _patch_file(os.path.join(cemu_dir, "CMakeLists.txt"),
                "add_subdirectory(src)\n",
                "add_subdirectory(src)\n\n" + MARK + "\nif (NL_AOT_DIR)\n"
                "\tinclude(\"${NL_AOT_DIR}/nl_aot.cmake\")\nendif()\n", log)
    # 2. Nach dem Laden und Linken der RPX die nativen Funktionen aktivieren
    cs = os.path.join(cemu_dir, "src", "Cafe", "CafeSystem.cpp")
    _patch_file(cs, "\tRPLLoader_Link();\n",
                "\tRPLLoader_Link();\n#ifdef NL_AOT\n\t{ void NL_AOT_Install(); NL_AOT_Install(); } "
                "// Nintendo Land Recomp\n#endif\n", log)
    # 3. Groessere Fiber-Stacks: der native Code braucht mehr Stack als Cemus Interpreter
    fw = os.path.join(cemu_dir, "src", "util", "Fiber", "FiberWin.cpp")
    _patch_file(fw, "HANDLE fiberHandle = CreateFiber(2 * 1024 * 1024, ",
                "HANDLE fiberHandle = CreateFiberEx(256 * 1024, 64 * 1024 * 1024, 0, ", log)
    fu = os.path.join(cemu_dir, "src", "util", "Fiber", "FiberUnix.cpp")
    if os.path.isfile(fu):
        _patch_file(fu, "const size_t stackSize = 2 * 1024 * 1024;",
                    "const size_t stackSize = 64 * 1024 * 1024;", log)


def configure_and_build(cemu_dir, build_dir, aot_dir, generated_dir, cmake, run, log, jobs="4"):
    git = find_git()
    env = git_env(git) if git else dict(os.environ)
    to_cm = lambda p: p.replace("\\", "/")  # noqa: E731
    args = [cmake, "-S", cemu_dir, "-B", build_dir,
            f"-DNL_AOT_DIR={to_cm(aot_dir)}", f"-DNL_AOT_GENERATED={to_cm(generated_dir)}",
            "-DENABLE_DISCORD_RPC=OFF"]
    if os.name == "nt":
        args += ["-G", "Visual Studio 17 2022", "-A", "x64"]
    log("Konfiguriere Cemu (beim ersten Mal laedt vcpkg alle Abhaengigkeiten, 20-60 Minuten) ...")
    run(args, env=env)
    log("Baue NintendoLand.exe (eigenes Programm mit nativem Spielcode) ...")
    run([cmake, "--build", build_dir, "--config", "Release", "--target", "NintendoLand",
         "--parallel", jobs], env=env)


def find_game_exe(cemu_dir):
    """Unser eigenes Programm (eigenes Fenster, eigene Einstellungen)."""
    for name in ("NintendoLand.exe", "NintendoLand"):
        p = os.path.join(cemu_dir, "bin", name)
        if os.path.isfile(p):
            return p
    return None


def find_exe(cemu_dir):
    for name in ("Cemu_release.exe", "Cemu_release", "Cemu.exe"):
        p = os.path.join(cemu_dir, "bin", name)
        if os.path.isfile(p):
            return p
    return None

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
    head = subprocess.run([git, "-C", cemu_dir, "rev-parse", "HEAD"], env=env, capture_output=True, text=True).stdout.strip()
    if head == CEMU_COMMIT:
        # Schon auf dem richtigen Stand: nichts zuruecksetzen, sonst wird alles neu kompiliert
        return
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


NL_COMBINED = """\t// Nintendo Land Recomp: TV- und GamePad-Bild zusammen im Hauptfenster
\tif (g_nl_screens.mode != 0 && g_nl_screens.layout && !g_renderer->IsPadWindowActive())
\t{
\t\tstruct NLDrc { MPTR ptr; uint32 w, h, fmt, pitch, swz; Latte::E_HWTILEMODE tm; };
\t\tstatic NLDrc s_drc{};
\t\tif (renderTarget & RENDER_TARGET_DRC)
\t\t{
\t\t\ts_drc = { colorBufferPtr, colorBufferWidth, colorBufferHeight, colorBufferFormat, colorBufferPitch, colorBufferSwizzle, colorBufferTilemode };
\t\t\tif (!(renderTarget & RENDER_TARGET_TV))
\t\t\t\treturn;
\t\t}
\t\tLatteTextureView* drcView = nullptr;
\t\tif (s_drc.ptr)
\t\t\tdrcView = LatteTC_GetTextureSliceViewOrTryCreate(s_drc.ptr, MPTR_NULL, (Latte::E_GX2SURFFMT)s_drc.fmt, s_drc.tm, s_drc.w, s_drc.h, 1, s_drc.pitch, s_drc.swz, 0, 0, true);
\t\tint w = 0, h = 0;
\t\tWindowSystem::GetWindowPhysSize(w, h);
\t\tint rTV[4], rDRC[4];
\t\tg_nl_screens.layout(w, h, LatteGPUState.isDRCPrimary, rTV, rDRC);
\t\tconst bool drcBig = LatteGPUState.isDRCPrimary;
\t\t// erst das grosse Bild (mit Hintergrund loeschen), dann das kleine, danach das Overlay
\t\tLatteTextureView* first = drcBig && drcView ? drcView : texView;
\t\tLatteTextureView* second = drcBig ? texView : drcView;
\t\tg_nl_screens.rect = drcBig && drcView ? rDRC : rTV;
\t\tg_nl_screens.clear = true;
\t\tg_nl_screens.skip_overlay = second != nullptr && second != first;
\t\tLatteRenderTarget_copyToBackbuffer(first, false);
\t\tif (second && second != first)
\t\t{
\t\t\tg_nl_screens.rect = drcBig ? rTV : rDRC;
\t\t\tg_nl_screens.clear = false;
\t\t\tg_nl_screens.skip_overlay = false;
\t\t\tLatteRenderTarget_copyToBackbuffer(second, false);
\t\t}
\t\tg_nl_screens.rect = nullptr;
\t\tg_nl_screens.clear = false;
\t\tg_nl_screens.skip_overlay = false;
\t\treturn;
\t}
"""


def patch(cemu_dir, log):
    """Kleine Aenderungen an Cemu (bleiben MPL-2.0)."""
    # 1. CMake: unseren AOT-Code einbinden
    _patch_file(os.path.join(cemu_dir, "CMakeLists.txt"),
                "add_subdirectory(src)\n",
                "add_subdirectory(src)\n\n" + MARK + "\nif (NL_AOT_DIR)\n"
                "\tinclude(\"${NL_AOT_DIR}/nl_aot.cmake\")\nendif()\n", log)
    # 1b. Keine Link-Time-Optimierung: der MSVC-Linker stuerzt bei unserer Programmgroesse sonst ab (LNK1000)
    #     und braucht ewig. Der generierte Spielcode hatte sie ohnehin schon aus.
    _patch_file(os.path.join(cemu_dir, "CMakeLists.txt"),
                "set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)\nset(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)\n",
                "set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF) # Nintendo Land Recomp\n"
                "set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)\n", log)
    # 2. Nach dem Laden und Linken der RPX die nativen Funktionen aktivieren
    cs = os.path.join(cemu_dir, "src", "Cafe", "CafeSystem.cpp")
    _patch_file(cs, "\tRPLLoader_Link();\n",
                "\tRPLLoader_Link();\n#ifdef NL_AOT\n\t{ void NL_AOT_Install(); NL_AOT_Install(); } "
                "// Nintendo Land Recomp\n#endif\n", log)
    # 3. Groessere Fiber-Stacks: der native Code braucht mehr Stack als Cemus Interpreter
    fw = os.path.join(cemu_dir, "src", "util", "Fiber", "FiberWin.cpp")
    _patch_file(fw, "HANDLE fiberHandle = CreateFiber(2 * 1024 * 1024, ",
                "HANDLE fiberHandle = CreateFiberEx(256 * 1024, 64 * 1024 * 1024, 0, ", log)
    # 4. Haken fuer unser Menue im Spielbild (jedes Bild, vor Cemus eigenem Overlay)
    lo = os.path.join(cemu_dir, "src", "Cafe", "HW", "Latte", "Core", "LatteOverlay.cpp")
    _patch_file(lo, "void LatteOverlay_render(bool pad_view)\n{\n",
                "void (*g_nl_overlay_hook)(bool pad_view) = nullptr; // Nintendo Land Recomp\n"
                "void LatteOverlay_render(bool pad_view)\n{\n\tif (g_nl_overlay_hook) g_nl_overlay_hook(pad_view);\n", log)
    # 5. TV- und GamePad-Bild in einem Fenster (nl_screens.h liegt im Frontend)
    rt = os.path.join(cemu_dir, "src", "Cafe", "HW", "Latte", "Core", "LatteRenderTarget.cpp")
    _patch_file(rt, '#include "Cafe/OS/libs/swkbd/swkbd.h"\n',
                '#include "Cafe/OS/libs/swkbd/swkbd.h"\n#include "nl_screens.h" // Nintendo Land Recomp\nNLScreens g_nl_screens;\n', log)
    _patch_file(rt, "\tbool clearBackground = false;\n\tif (imageWidth != fullscreenWidth || imageHeight != fullscreenHeight)\n\t\tclearBackground = true;\n",
                "\tbool clearBackground = false;\n\tif (imageWidth != fullscreenWidth || imageHeight != fullscreenHeight)\n\t\tclearBackground = true;\n"
                "\tif (g_nl_screens.rect) { imageX = g_nl_screens.rect[0]; imageY = g_nl_screens.rect[1]; imageWidth = g_nl_screens.rect[2]; "
                "imageHeight = g_nl_screens.rect[3]; clearBackground = g_nl_screens.clear; } // Nintendo Land Recomp\n", log)
    _patch_file(rt, "\tg_renderer->HandleScreenshotRequest(textureView, isPadView);\n\tif (!g_renderer->ImguiBegin(!isPadView))\n",
                "\tg_renderer->HandleScreenshotRequest(textureView, isPadView);\n\tif (g_nl_screens.skip_overlay) return; // Nintendo Land Recomp\n"
                "\tif (!g_renderer->ImguiBegin(!isPadView))\n", log)
    _patch_file(rt, "\tif (!texView)\n\t{\n\t\treturn;\n\t}\n",
                "\tif (!texView)\n\t{\n\t\treturn;\n\t}\n" + NL_COMBINED, log)
    # 6. Keine Shader-Meldungen und kein Emulator-Name im Log
    cfg = os.path.join(cemu_dir, "src", "config", "CemuConfig.h")
    _patch_file(cfg, "\t\tbool shader_compiling = true;\n", "\t\tbool shader_compiling = false; // Nintendo Land Recomp\n", log)
    csys = os.path.join(cemu_dir, "src", "Cafe", "CafeSystem.cpp")
    _patch_file(csys, 'cemuLog_log(LogType::Force, "------- Init {} -------", BUILD_VERSION_WITH_NAME_STRING);',
                'cemuLog_log(LogType::Force, "------- Init NL-Engine (Nintendo Land Recomp) -------");', log)
    # 7. Virtuelles GamePad-Neigen (Maus/rechter Stick) statt Rechtsklick-Ziehen
    vp = os.path.join(cemu_dir, "src", "input", "emulated", "VPADController.cpp")
    _patch_file(vp, '#include "input/emulated/VPADController.h"\n',
                '#include "input/emulated/VPADController.h"\n'
                'bool (*g_nl_virtual_gyro)(float& wx, float& wy) = nullptr; // Nintendo Land Recomp\n', log)
    _patch_file(vp,
                "\tif (const auto right_mouse = input_manager.get_right_down_mouse_info(&pad_view))\n\t{\n"
                "\t\tconst Vector2<float> mousePos(right_mouse->x, right_mouse->y);\n\n"
                "\t\tint w, h;\n\t\tif (pad_view)\n\t\t\tWindowSystem::GetPadWindowPhysSize(w, h);\n\t\telse\n"
                "\t\t\tWindowSystem::GetWindowPhysSize(w, h);\n\n"
                "\t\tfloat wx = mousePos.x / w;\n\t\tfloat wy = mousePos.y / h;\n",
                "\t// Nintendo Land Recomp: virtuelles Neigen (Maus/rechter Stick) hat Vorrang\n"
                "\tfloat nl_wx = 0.5f, nl_wy = 0.5f;\n"
                "\tconst bool nl_gyro = g_nl_virtual_gyro && g_nl_virtual_gyro(nl_wx, nl_wy);\n"
                "\tconst auto right_mouse = nl_gyro ? std::optional<glm::ivec2>() : input_manager.get_right_down_mouse_info(&pad_view);\n"
                "\tif (nl_gyro || right_mouse)\n\t{\n"
                "\t\tfloat wx = nl_wx, wy = nl_wy;\n"
                "\t\tif (!nl_gyro)\n\t\t{\n"
                "\t\t\tconst Vector2<float> mousePos(right_mouse->x, right_mouse->y);\n"
                "\t\t\tint w, h;\n\t\t\tif (pad_view)\n\t\t\t\tWindowSystem::GetPadWindowPhysSize(w, h);\n\t\t\telse\n"
                "\t\t\t\tWindowSystem::GetWindowPhysSize(w, h);\n"
                "\t\t\twx = mousePos.x / w;\n\t\t\twy = mousePos.y / h;\n\t\t}\n", log)
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

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


_UNIX_DIRS = ("devkitpro", "msys64", "msys2", "cygwin", "mingw64", "mingw32")


def find_git():
    # Git aus MSYS2/devkitPro/Cygwin meiden: das versteht Windows-Pfade nicht richtig
    for d in os.environ.get("PATH", "").split(os.pathsep):
        g = os.path.join(d.strip('"'), "git.exe")
        if os.path.isfile(g) and not any(t in g.lower() for t in _UNIX_DIRS):
            return g
    for pf in (os.environ.get("ProgramFiles", r"C:\Program Files"),
               os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        g = os.path.join(pf, "Git", "cmd", "git.exe")
        if os.path.isfile(g):
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
        if os.path.isdir(cemu_dir) and os.listdir(cemu_dir):
            log(f"{cemu_dir} ist von einem abgebrochenen Versuch uebrig - wird geleert.")
            shutil.rmtree(cemu_dir, ignore_errors=True)
        log("Lade Cemu-Quellcode (einmalig, ein paar hundert MB) ...")
        run([git, "clone", CEMU_URL, cemu_dir], env=env)
    head = subprocess.run([git, "-C", cemu_dir, "rev-parse", "HEAD"], env=env, capture_output=True, text=True).stdout.strip()
    if head != CEMU_COMMIT:
        run([git, "-C", cemu_dir, "fetch", "--depth", "1", "origin", CEMU_COMMIT], env=env, check=False)
        run([git, "-C", cemu_dir, "checkout", "-f", CEMU_COMMIT], env=env)
        log("Lade Cemu-Untermodule (vcpkg, imgui, ...) ...")
        run([git, "-C", cemu_dir, "submodule", "update", "--init", "--recursive"], env=env)
    # Untermodule pruefen: ein abgebrochener frueherer Versuch kann sie leer zuruecklassen
    if not submodules_ok(cemu_dir):
        log("Cemu-Untermodule fehlen oder sind unvollstaendig - lade sie nach ...")
        run([git, "-C", cemu_dir, "submodule", "sync", "--recursive"], env=env, check=False)
        run([git, "-C", cemu_dir, "submodule", "update", "--init", "--recursive", "--force"], env=env)
        if not submodules_ok(cemu_dir):
            raise RuntimeError("Die Cemu-Untermodule (vcpkg, imgui ...) konnten nicht geladen werden. "
                               f"Loesch den Ordner {cemu_dir} und starte 'Spiel bauen' nochmal "
                               "(Internet muss gehen, Git for Windows muss installiert sein).")


_SUBMODULE_MARKERS = (
    ("dependencies", "vcpkg", "scripts", "buildsystems", "vcpkg.cmake"),
    ("dependencies", "imgui", "imgui.h"),
)


def submodules_ok(cemu_dir):
    return all(os.path.isfile(os.path.join(cemu_dir, *m)) for m in _SUBMODULE_MARKERS)


# Patches werden immer auf das Original aus Git angewendet (nicht auf die Datei auf der Platte).
# So passen sie auch, wenn sich ein Patch geaendert hat, und Dateien werden nur geschrieben,
# wenn sich ihr Inhalt wirklich aendert (sonst unnoetiges Neukompilieren).
_state = {"root": None, "files": {}}


def _original(path):
    root = _state["root"]
    if root:
        rel = os.path.relpath(path, root).replace("\\", "/")
        git = find_git()
        if git:
            r = subprocess.run([git, "-C", root, "show", "HEAD:" + rel], capture_output=True)
            if r.returncode == 0:
                return r.stdout.decode("utf-8").replace("\r\n", "\n")
    return open(path, encoding="utf-8").read()


def _patch_file(path, old, new, log):
    files = _state["files"]
    if path not in files:
        files[path] = _original(path)
    s = files[path]
    if new in s:
        return
    if old not in s:
        raise RuntimeError(f"Cemu-Patch passt nicht mehr: {os.path.basename(path)}")
    files[path] = s.replace(old, new, 1)


def _flush(log):
    for path, s in _state["files"].items():
        try:
            cur = open(path, encoding="utf-8").read()
        except OSError:
            cur = None
        if cur != s:
            open(path, "w", encoding="utf-8", newline="\n").write(s)
            log(f"  gepatcht: {os.path.relpath(path, os.path.dirname(os.path.dirname(path)))}")
    _state["files"] = {}

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
    _state["root"] = cemu_dir if os.path.isdir(os.path.join(cemu_dir, ".git")) else None
    _state["files"] = {}
    try:
        _patch_all(cemu_dir, log)
        _flush(log)
    finally:
        _state["files"] = {}


def _patch_all(cemu_dir, log):
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
    # 8. Deterministischer Modus (Netplay): virtuelle Uhr statt echter Zeit, alles auf einem Kern
    src = os.path.join(cemu_dir, "src")
    EXT = ("extern bool g_nl_det; extern std::atomic<uint64> g_nl_vclock; extern std::atomic<uint64> g_nl_cnt[8]; "
           "extern void (*g_nl_det_tick)(); extern void (*g_nl_det_idle)(); // Nintendo Land Recomp\n")
    tm = os.path.join(src, "Cafe", "HW", "Espresso", "PPCTimer.cpp")
    _patch_file(tm, "FSpinlock sTimerSpinlock;\n",
                "FSpinlock sTimerSpinlock;\n"
                "// Nintendo Land Recomp: deterministischer Modus\n"
                "bool g_nl_det = false;\nstd::atomic<uint64> g_nl_vclock{0};\n"
                "uint64 g_nl_det_epoch = 820454400; // Sekunden seit 2000 (1.1.2026), vom Host vorgegeben\n"
                "void (*g_nl_det_tick)() = nullptr;\nvoid (*g_nl_det_idle)() = nullptr;\n"
                "std::atomic<uint64> g_nl_cnt[8]{}; // Zaehler zur Fehlersuche: 0 IPC, 1 IPC-Timeout, 2 GPU, 3 Leerlauf, 4 Audio\n", log)
    _patch_file(tm, "uint64 PPCTimer_getFromRDTSC()\n{\n",
                "uint64 PPCTimer_getFromRDTSC()\n{\n\tif (g_nl_det) return g_nl_vclock.load(std::memory_order_relaxed); // Nintendo Land Recomp\n", log)
    th = os.path.join(src, "Cafe", "OS", "libs", "coreinit", "coreinit_Thread.cpp")
    _patch_file(th, '#include "Cafe/OS/common/OSCommon.h"\n', '#include "Cafe/OS/common/OSCommon.h"\n' + EXT, log)
    _patch_file(th, "\t\tthread->totalCycles += (uint64)executedCycles;\n",
                "\t\tthread->totalCycles += (uint64)executedCycles;\n"
                "\t\tif (g_nl_det) g_nl_vclock.fetch_add((uint64)executedCycles); // Nintendo Land Recomp: virtuelle Uhr\n", log)
    _patch_file(th, "\t\t// AX update\n\t\tsnd_core::AXOut_update();\n",
                "\t\tif (g_nl_det && g_nl_det_tick) g_nl_det_tick(); // Nintendo Land Recomp: VSync usw. nach virtueller Uhr\n"
                "\t\t// AX update\n\t\tsnd_core::AXOut_update();\n", log)
    _patch_file(th, "\t\t\t\t__OSCheckSystemEvents();\n\t\t\t\tif(g_isMulticoreMode == false)\n\t\t\t\t\tcoreIndex = (coreIndex + 1) % 3;\n",
                "\t\t\t\t__OSCheckSystemEvents();\n\t\t\t\tif(g_isMulticoreMode == false)\n\t\t\t\t\tcoreIndex = (coreIndex + 1) % 3;\n"
                "\t\t\t\t// Nintendo Land Recomp: nichts zu tun -> virtuelle Uhr vorspulen\n"
                "\t\t\t\tif (g_nl_det && g_nl_det_idle && g_coreRunQueueThreadCount[0].isZero() && g_coreRunQueueThreadCount[1].isZero() && g_coreRunQueueThreadCount[2].isZero())\n"
                "\t\t\t\t\tg_nl_det_idle();\n", log)
    ax = os.path.join(src, "Cafe", "OS", "libs", "snd_core", "ax_out.cpp")
    _patch_file(ax, '#include "Cafe/OS/libs/snd_core/ax.h"\n', '#include "Cafe/OS/libs/snd_core/ax.h"\n' + EXT, log)
    _patch_file(ax, "\tvoid AXOut_update()\n\t{\n",
                "\tvoid AXOut_update()\n\t{\n"
                "\t\tif (g_nl_det) // Nintendo Land Recomp: Audio-Takt alle 3 ms virtueller Zeit\n\t\t{\n"
                "\t\t\tstatic uint64 s_nl_next = 0;\n\t\t\tconst uint64 step = 1243125000ull * 3 / 1000;\n"
                "\t\t\tconst uint64 now = g_nl_vclock.load();\n\t\t\tif (s_nl_next == 0) s_nl_next = now + step;\n"
                "\t\t\tif (now < s_nl_next) return;\n\t\t\ts_nl_next += step;\n"
                "\t\t\tif (snd_core::isInitialized() && numQueuedFramesSndGeneric == snd_core::getNumProcessedFrames())\n\t\t\t{\n"
                "\t\t\t\tAXOut_updateDevicePlayState(true);\n\t\t\t\tsnd_core::AXIst_QueueFrame();\n\t\t\t\tnumQueuedFramesSndGeneric++;\n\t\t\t\tg_nl_cnt[4]++;\n\t\t\t}\n"
                "\t\t\treturn;\n\t\t}\n", log)
    lt = os.path.join(src, "Cafe", "HW", "Latte", "Core", "LatteTiming.cpp")
    _patch_file(lt, '#include "Cafe/OS/libs/gx2/GX2_Event.h"\n', '#include "Cafe/OS/libs/gx2/GX2_Event.h"\n' + EXT, log)
    _patch_file(lt, "void LatteTiming_signalVsync()\n{\n",
                "static thread_local bool t_nl_det_vsync = false; // Nintendo Land Recomp\n"
                "void LatteTiming_signalVsync();\n"
                "void NL_Det_SignalVsync() { t_nl_det_vsync = true; LatteTiming_signalVsync(); t_nl_det_vsync = false; }\n"
                "void LatteTiming_signalVsync()\n{\n"
                "\tif (g_nl_det && !t_nl_det_vsync) return; // im deterministischen Modus nur nach virtueller Uhr\n", log)
    _patch_file(lt, "\t\t\telse\n\t\t\t{\n\t\t\t\t// old code for all other games\n",
                "\t\t\telse if (g_nl_det) // Nintendo Land Recomp: Flip nach Anfragen des Spiels, nicht nach GPU-Fortschritt\n\t\t\t{\n"
                "\t\t\t\tuint32 req = _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE);\n"
                "\t\t\t\tuint32 exe = _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE);\n"
                "\t\t\t\tif (exe != req)\n\t\t\t\t\tLatteGPUState.sharedArea->flipExecuteCountBE = _swapEndianU32(exe + 1);\n\t\t\t}\n"
                "\t\t\telse\n\t\t\t{\n\t\t\t\t// old code for all other games\n", log)
    ipc = os.path.join(src, "Cafe", "OS", "libs", "coreinit", "coreinit_IPC.cpp")
    _patch_file(ipc, '#include "coreinit_MessageQueue.h"\n',
                '#include "coreinit_MessageQueue.h"\n' + EXT +
                "#include <set>\n// Nintendo Land Recomp: offene IPC-Anfragen im deterministischen Modus\n"
                "static std::mutex s_nl_ipc_mtx;\nstatic std::set<const void*> s_nl_ipc_pending, s_nl_ipc_long;\n"
                "int NL_IPC_PendingCount() { std::lock_guard l(s_nl_ipc_mtx); return (int)s_nl_ipc_pending.size(); }\n", log)
    _patch_file(ipc, "\t\tiosu::kernel::IPCSubmitFromCOS(coreIndex, command);\n\t}\n",
                "\t\tif (g_nl_det) { std::lock_guard l(s_nl_ipc_mtx); s_nl_ipc_pending.insert(command); g_nl_cnt[0]++; }\n"
                "\t\tiosu::kernel::IPCSubmitFromCOS(coreIndex, command);\n"
                "\t\tif (g_nl_det) // Nintendo Land Recomp: auf die Antwort warten (max. 100 ms), damit sie an fester Stelle ankommt\n\t\t{\n"
                "\t\t\tauto t0 = std::chrono::steady_clock::now();\n\t\t\twhile (true)\n\t\t\t{\n"
                "\t\t\t\t{\n\t\t\t\t\tstd::lock_guard l(s_nl_ipc_mtx);\n\t\t\t\t\tif (!s_nl_ipc_pending.count(command)) break;\n"
                "\t\t\t\t\tif (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(100)) { s_nl_ipc_pending.erase(command); s_nl_ipc_long.insert(command); g_nl_cnt[1]++; "
                "cemuLog_log(LogType::Force, \"NL-DET: IPC-Anfrage dauert lange (cmd {} dev {} t={})\", (uint32)command->cmdId.value(), (uint32)command->devHandle.value(), g_nl_vclock.load()); break; }\n"
                "\t\t\t\t}\n\t\t\t\tstd::this_thread::yield();\n\t\t\t}\n\t\t}\n\t}\n", log)
    _patch_file(ipc, "\t\tIPCDriver_KernelCallback(ipcDriver);\n\n\t\tlock.lock();\n",
                "\t\tIPCDriver_KernelCallback(ipcDriver);\n"
                "\t\tif (g_nl_det) { std::lock_guard l(s_nl_ipc_mtx); for (uint32 i = 0; i < numResponses; i++) if (!s_nl_ipc_pending.erase(responseArray[i]) && s_nl_ipc_long.erase(responseArray[i])) "
                "cemuLog_log(LogType::Force, \"NL-DET: Dauer-Anfrage beantwortet (dev {} t={})\", (uint32)responseArray[i]->devHandle.value(), g_nl_vclock.load()); } // Nintendo Land Recomp\n"
                "\n\t\tlock.lock();\n", log)
    acs = os.path.join(src, "config", "ActiveSettings.cpp")
    _patch_file(acs, "CPUMode ActiveSettings::GetCPUMode()\n{\n",
                "extern bool g_nl_det; // Nintendo Land Recomp\nCPUMode ActiveSettings::GetCPUMode()\n{\n"
                "\tif (g_nl_det) return CPUMode::SinglecoreRecompiler; // deterministischer Modus: ein Kern\n", log)
    cs2 = os.path.join(src, "Cafe", "CafeSystem.cpp")
    _patch_file(cs2, "\tppcCyclesSince2000TimerClock = ppcCyclesSince2000 / 20ULL;\n\tPPCTimer_start();\n",
                "\tppcCyclesSince2000TimerClock = ppcCyclesSince2000 / 20ULL;\n"
                "\t{ extern bool g_nl_det; extern uint64 g_nl_det_epoch; // Nintendo Land Recomp: feste Uhrzeit fuer alle Spieler\n"
                "\t\tif (g_nl_det) { ppcCyclesSince2000 = ppcCyclesSince2000_UTC = g_nl_det_epoch * (uint64)ESPRESSO_CORE_CLOCK; "
                "ppcCyclesSince2000TimerClock = ppcCyclesSince2000 / 20ULL; } }\n"
                "\tPPCTimer_start();\n", log)
    # 9. Deterministischer Modus: GPU-Auftraege synchron (Fertig-Meldung der GPU an fester Stelle)
    tcl = os.path.join(src, "Cafe", "OS", "libs", "TCL", "TCL.cpp")
    _patch_file(tcl, '#include "Common/CafeStaticVar.h"\n', '#include "Common/CafeStaticVar.h"\n' + EXT, log)
    _patch_file(tcl, "\tuint64 s_currentRetireMarker = 0;\n",
                "\tuint64 s_currentRetireMarker = 0;\n"
                "\tstatic std::atomic<uint64> s_nl_notified{0}; // Nintendo Land Recomp: zuletzt gemeldeter Retire-Marker\n", log)
    _patch_file(tcl, "\t\tcoreinit::OSSignalEventAllInternal(s_updateRetirementEvent.GetPtr());\n\t\t__OSUnlockScheduler();\n",
                "\t\tcoreinit::OSSignalEventAllInternal(s_updateRetirementEvent.GetPtr());\n\t\t__OSUnlockScheduler();\n"
                "\t\ts_nl_notified.store((uint64)s_tclStatePPC->gpuRetireMarker); // Nintendo Land Recomp\n", log)
    _patch_file(tcl, "\t\t\t*timestampValueOut = s_currentRetireMarker; // incremented before each submit\n",
                "\t\t\t*timestampValueOut = s_currentRetireMarker; // incremented before each submit\n"
                "\t\t\tif (g_nl_det) // Nintendo Land Recomp: warten, bis die GPU fertig ist und es gemeldet hat\n\t\t\t{\n\t\t\t\tg_nl_cnt[2]++;\n"
                "\t\t\t\tauto t0 = std::chrono::steady_clock::now();\n"
                "\t\t\t\twhile (s_nl_notified.load() < s_currentRetireMarker)\n\t\t\t\t{\n"
                "\t\t\t\t\tif (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) { cemuLog_log(LogType::Force, \"NL-DET: GPU-Wartezeit ueberschritten\"); break; }\n"
                "\t\t\t\t\tstd::this_thread::yield();\n\t\t\t\t}\n\t\t\t}\n", log)
    # 9b. Fehlersuche: welches Systemgeraet hat welche Nummer
    _patch_file(ipc, "\t\t\t\t\tif (response.cmdId == IPCCommandId::IOS_OPEN)\n\t\t\t\t\t\ts_openIOSHandles.push_back(response.result);\n",
                "\t\t\t\t\tif (response.cmdId == IPCCommandId::IOS_OPEN)\n\t\t\t\t\t\ts_openIOSHandles.push_back(response.result);\n"
                "\t\t\t\t\tif (g_nl_det && response.cmdId == IPCCommandId::IOS_OPEN) // Nintendo Land Recomp\n"
                "\t\t\t\t\t\tcemuLog_log(LogType::Force, \"NL-DET: IOS_Open {} -> {}\", (const char*)memory_getPointerFromVirtualOffsetAllowNull(response.args[0]), (uint32)response.result.value());\n", log)
    # 10. Deterministischer Modus: aeltere IOSU-Anfragen (ACT/BOSS/MCP...) mitzaehlen, Uhr wartet auf sie
    io = os.path.join(src, "Cafe", "IOSU", "legacy", "iosu_ioctl.cpp")
    _patch_file(io, '#include "util/helpers/Semaphore.h"\n',
                '#include "util/helpers/Semaphore.h"\n' + EXT +
                "std::atomic<int> g_nl_legacy_pending{0}; // Nintendo Land Recomp\n"
                "int NL_Legacy_PendingCount() { return g_nl_legacy_pending.load(); }\n", log)
    _patch_file(io, "\t_ioctlRingbufferSemaphore[ioctlHandle].increment();\n\tcoreinit::__OSSuspendThreadInternal(coreinit::OSGetCurrentThread());\n",
                "\tif (g_nl_det) g_nl_legacy_pending++; // Nintendo Land Recomp\n"
                "\t_ioctlRingbufferSemaphore[ioctlHandle].increment();\n\tcoreinit::__OSSuspendThreadInternal(coreinit::OSGetCurrentThread());\n", log)
    _patch_file(io, "\tcoreinit::OSResumeThread(ioQueueEntry->ppcThread);\n}\n",
                "\tcoreinit::OSResumeThread(ioQueueEntry->ppcThread);\n\tif (g_nl_det) g_nl_legacy_pending--; // Nintendo Land Recomp\n}\n", log)
    # 11. Deterministischer Modus: JIT kompiliert synchron statt im Hintergrund-Thread
    #     (sonst laeuft eine Funktion je nach Timing mal interpretiert, mal kompiliert -> andere Zyklen)
    rc = os.path.join(src, "Cafe", "HW", "Espresso", "Recompiler", "PPCRecompiler.cpp")
    _patch_file(rc, "#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION\nstatic std::mutex s_singleRecompilationMutex;\n#endif\n",
                "static std::mutex s_singleRecompilationMutex;\nextern bool g_nl_det; // Nintendo Land Recomp\n", log)
    _patch_file(rc, "{\n#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION\n\tif (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)\n\t\treturn;\n",
                "{\n\tif (g_nl_det) { // Nintendo Land Recomp: synchron kompilieren\n\tif (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)\n\t\treturn;\n", log)
    _patch_file(rc, "\ts_singleRecompilationMutex.unlock();\n\treturn;\n#endif\n",
                "\ts_singleRecompilationMutex.unlock();\n\treturn;\n\t} // Nintendo Land Recomp\n", log)
    # 12. Online-Streaming: TV-Bild und TV-Ton abgreifen
    rh = os.path.join(src, "Cafe", "HW", "Latte", "Renderer", "Renderer.h")
    _patch_file(rh, "\tvirtual void HandleScreenshotRequest(LatteTextureView* texView, bool padView){}\n",
                "\tvirtual void HandleScreenshotRequest(LatteTextureView* texView, bool padView){}\n"
                "\tvirtual void NL_StreamCapture(LatteTextureView* texView, bool pad) {} // Nintendo Land Recomp\n", log)
    vh = os.path.join(src, "Cafe", "HW", "Latte", "Renderer", "Vulkan", "VulkanRenderer.h")
    _patch_file(vh, "\tvoid HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;\n",
                "\tvoid HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;\n"
                "\tvoid NL_StreamCapture(LatteTextureView* texView, bool pad) override; // Nintendo Land Recomp\n", log)
    _patch_file(rt, "\tif (!texView)\n\t{\n\t\treturn;\n\t}\n" + NL_COMBINED,
                "\tif (!texView)\n\t{\n\t\treturn;\n\t}\n"
                "\tif (renderTarget & RENDER_TARGET_TV) g_renderer->NL_StreamCapture(texView, false); // Nintendo Land Recomp: Online\n"
                "\tif (renderTarget & RENDER_TARGET_DRC) g_renderer->NL_StreamCapture(texView, true);\n" + NL_COMBINED, log)
    _patch_file(ax, '#include "Cafe/OS/libs/snd_core/ax.h"\n',
                '#include "Cafe/OS/libs/snd_core/ax.h"\n'
                'extern void (*g_nl_stream_audio)(const sint16* samples, int frames, int channels); // Nintendo Land Recomp\n', log)
    _patch_file(ax, "\t\t\tif(g_tvAudio)\n\t\t\t\tg_tvAudio->FeedBlock(tempTVChannelData);\n",
                "\t\t\tif(g_tvAudio)\n\t\t\t\tg_tvAudio->FeedBlock(tempTVChannelData);\n"
                "\t\t\tif (g_nl_stream_audio) g_nl_stream_audio(tempTVChannelData, AX_SAMPLES_PER_3MS_48KHZ * AX_FRAMES_PER_GROUP, (int)channels); // Nintendo Land Recomp\n", log)
    # 13. Discord: welche Dateien das Spiel oeffnet (daran erkennt man das Minispiel)
    fsc = os.path.join(src, "Cafe", "Filesystem", "fsc.cpp")
    _patch_file(fsc, "FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG accessFlags, sint32* fscStatus, sint32 maxPriority)\n{\n",
                "extern void (*g_nl_file_open_hook)(const char* path); // Nintendo Land Recomp\n"
                "FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG accessFlags, sint32* fscStatus, sint32 maxPriority)\n{\n"
                "\tif (g_nl_file_open_hook) g_nl_file_open_hook(path);\n", log)
    fu = os.path.join(cemu_dir, "src", "util", "Fiber", "FiberUnix.cpp")
    if os.path.isfile(fu):
        _patch_file(fu, "const size_t stackSize = 2 * 1024 * 1024;",
                    "const size_t stackSize = 64 * 1024 * 1024;", log)


def configure_and_build(cemu_dir, build_dir, aot_dir, generated_dir, cmake, run, log, jobs="4"):
    git = find_git()
    env = git_env(git) if git else dict(os.environ)
    # MSYS2/devkitPro/Cygwin aus dem PATH: deren cmake/link/gcc stoeren den Visual-Studio-Build
    bad = ("devkitpro", "msys64", "msys2", "cygwin", "mingw64", "mingw32")
    env["PATH"] = os.pathsep.join(d for d in env.get("PATH", "").split(os.pathsep)
                                  if not any(t in d.lower() for t in bad))
    to_cm = lambda p: p.replace("\\", "/")  # noqa: E731
    args = [cmake, "-S", cemu_dir, "-B", build_dir,
            f"-DNL_AOT_DIR={to_cm(aot_dir)}", f"-DNL_AOT_GENERATED={to_cm(generated_dir)}",
            "-DENABLE_DISCORD_RPC=OFF"]
    if os.name == "nt":
        args += ["-G", "Visual Studio 17 2022", "-A", "x64"]
        # Ein frueherer Versuch mit falschem CMake kann einen kaputten Cache hinterlassen
        cache = os.path.join(build_dir, "CMakeCache.txt")
        if os.path.isfile(cache):
            with open(cache, encoding="utf-8", errors="replace") as f:
                txt = f.read()
            if "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022" not in txt:
                log("Alter CMake-Cache passt nicht (anderer Generator) - wird geloescht.")
                os.remove(cache)
                shutil.rmtree(os.path.join(build_dir, "CMakeFiles"), ignore_errors=True)
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

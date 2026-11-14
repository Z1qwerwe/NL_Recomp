// Nintendo Land Recomp - eigenes Programm.
//
// Eigenes Fenster, eigene Einstellungen (nl_settings.ini), startet direkt ins Spiel.
// Der recompilierte Spielcode laeuft nativ; fuer Betriebssystem-Funktionen, Grafik
// (Vulkan), Audio und Eingabe werden Cemus Bibliotheken (MPL-2.0) als Unterbau genutzt,
// ohne Cemus Oberflaeche.
#include "WindowSystem.h"
#include "nl_app.h"
#include "nl_screens.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "input/ControllerFactory.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/VPADController.h"
#include "util/crypto/aes128.h"

#if !BOOST_OS_WINDOWS
#error "NintendoLand.exe gibt es (noch) nur fuer Windows"
#endif
#include <WinSock2.h>
#include <Windows.h>
#include <objbase.h>
#include <windowsx.h>
#pragma comment(lib, "Dbghelp.lib")


#include <fstream>
#include <set>
#include <map>

void PPCTimer_init();

// Symbole, die sonst Cemus eigene main.cpp liefert
std::atomic_bool g_isGPUInitFinished = false;
void requireConsole() {}

extern "C" {
// Laptops mit zwei Grafikkarten: die starke nehmen
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

// ===========================================================================
// WindowSystem: Cemus Schnittstelle zur Oberflaeche, hier mit Win32 umgesetzt
// ===========================================================================
static WindowSystem::WindowInfo g_window_info{};
static HWND s_window = nullptr;
static HWND s_pad_window = nullptr;
static std::atomic<double> s_fps{0};
static std::atomic<bool> s_loading{true};

void WindowSystem::Create() {}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory>) {
    std::wstring t = boost::nowide::widen(title.empty() ? std::string_view("Nintendo Land") : title);
    std::wstring m = boost::nowide::widen(message);
    MessageBoxW(s_window, m.c_str(), t.c_str(), MB_OK | MB_ICONERROR);
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo() { return g_window_info; }

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps) {
    s_fps = fps;
    s_loading = isLoading;
}

void WindowSystem::GetWindowSize(int& w, int& h) { w = g_window_info.width; h = g_window_info.height; }
void WindowSystem::GetPadWindowSize(int& w, int& h) {
    w = g_window_info.pad_open ? g_window_info.pad_width.load() : 0;
    h = g_window_info.pad_open ? g_window_info.pad_height.load() : 0;
}
void WindowSystem::GetWindowPhysSize(int& w, int& h) { w = g_window_info.phys_width; h = g_window_info.phys_height; }
void WindowSystem::GetPadWindowPhysSize(int& w, int& h) {
    w = g_window_info.pad_open ? g_window_info.phys_pad_width.load() : 0;
    h = g_window_info.pad_open ? g_window_info.phys_pad_height.load() : 0;
}
double WindowSystem::GetWindowDPIScale() { return g_window_info.dpi_scale; }
double WindowSystem::GetPadDPIScale() { return g_window_info.pad_open ? g_window_info.pad_dpi_scale.load() : 1.0; }
bool WindowSystem::IsPadWindowOpen() { return g_window_info.pad_open; }

bool WindowSystem::IsKeyDown(uint32 key) { return g_window_info.get_keystate(key); }

bool WindowSystem::IsKeyDown(PlatformKeyCodes platformKey) {
    switch (platformKey) {
    case PlatformKeyCodes::LCONTROL: return g_window_info.app_active && (GetAsyncKeyState(VK_LCONTROL) & 0x8000);
    case PlatformKeyCodes::RCONTROL: return g_window_info.app_active && (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
    case PlatformKeyCodes::TAB: return false;   // Bildwechsel laeuft ueber die SCREEN-Belegung (umschalten)
    case PlatformKeyCodes::ESCAPE: return IsKeyDown((uint32)VK_ESCAPE);
    default: return false;
    }
}

std::string WindowSystem::GetKeyCodeName(uint32 button) {
    LONG sc = MapVirtualKeyA((UINT)button, MAPVK_VK_TO_VSC_EX) << 16;
    char name[128];
    if (GetKeyNameTextA(sc, name, sizeof(name))) return name;
    return fmt::format("key_{}", button);
}

bool WindowSystem::InputConfigWindowHasFocus() { return false; }
void WindowSystem::NotifyGameLoaded() {}
void WindowSystem::NotifyGameExited() {}
void WindowSystem::RefreshGameList() {}
void WindowSystem::CaptureInput(const ControllerState&, const ControllerState&) {}
bool WindowSystem::IsFullScreen() { return g_window_info.is_fullscreen; }

// ===========================================================================
// Hilfen
// ===========================================================================
static void fatal(const std::string& msg) {
    cemuLog_log(LogType::Force, "Nintendo Land: {}", msg);
    WindowSystem::ShowErrorDialog(msg, "Nintendo Land");
    exit(1);
}

static WindowSystem::WindowHandleInfo handle_of(HWND w) {
    WindowSystem::WindowHandleInfo h{};
    h.backend = WindowSystem::WindowHandleInfo::Backend::Windows;
    h.surface = w;
    return h;
}

static void update_window_sizes() {
    auto measure = [](HWND w, int& lw, int& lh, int& pw, int& ph, double& scale) {
        RECT rc{};
        GetClientRect(w, &rc);
        pw = std::max<int>(1, rc.right - rc.left);
        ph = std::max<int>(1, rc.bottom - rc.top);
        UINT dpi = GetDpiForWindow(w);
        scale = dpi ? dpi / 96.0 : 1.0;
        lw = (int)(pw / scale);
        lh = (int)(ph / scale);
    };
    int lw, lh, pw, ph;
    double sc;
    measure(s_window, lw, lh, pw, ph, sc);
    g_window_info.width = lw;
    g_window_info.height = lh;
    g_window_info.phys_width = pw;
    g_window_info.phys_height = ph;
    g_window_info.dpi_scale = sc;
    if (s_pad_window) {
        measure(s_pad_window, lw, lh, pw, ph, sc);
        g_window_info.pad_width = lw;
        g_window_info.pad_height = lh;
        g_window_info.phys_pad_width = pw;
        g_window_info.phys_pad_height = ph;
        g_window_info.pad_dpi_scale = sc;
    }
}

static bool create_default_dirs(const fs::path& mlc) {
    std::error_code ec;
    const fs::path dirs[] = {
        mlc, mlc / "sys", mlc / "usr", mlc / "usr/title/00050000", mlc / "usr/title/0005000c",
        mlc / "usr/title/0005000e", mlc / "usr/save/00050010/1004a000/user/common/db",
        mlc / "usr/save/00050010/1004a100/user/common/db", mlc / "usr/save/00050010/1004a200/user/common/db",
        mlc / "sys/title/0005001b/1005c000/content",
        ActiveSettings::GetConfigPath("controllerProfiles"), ActiveSettings::GetUserDataPath("memorySearcher"),
    };
    for (auto& d : dirs)
        if (!fs::exists(d, ec) && !fs::create_directories(d, ec)) return false;
    return true;
}

static void setup_mic() {
    auto& config = GetConfig();
    if (!g_nl.mic) { config.input_device = L""; return; }
    if (!config.input_device.empty()) return;
    auto devices = IAudioInputAPI::GetDevices(IAudioInputAPI::Cubeb);
    if (!devices.empty()) {
        config.input_device = devices.front()->GetIdentifier();
        cemuLog_log(LogType::Force, "Nintendo Land: Mikrofon: {}", boost::nowide::narrow(devices.front()->GetName()));
    }
}

// Vollbild ohne Rahmen auf dem aktuellen Bildschirm
static WINDOWPLACEMENT s_prev_placement{sizeof(WINDOWPLACEMENT)};
static void set_fullscreen(bool on) {
    DWORD style = GetWindowLongW(s_window, GWL_STYLE);
    if (on) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(s_window, &s_prev_placement) &&
            GetMonitorInfoW(MonitorFromWindow(s_window, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(s_window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(s_window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLongW(s_window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(s_window, &s_prev_placement);
        SetWindowPos(s_window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_window_info.is_fullscreen = on;
    g_nl.fullscreen = on;
}

// Auftraege von Menue/Eingabe (anderer Thread) an den Fenster-Thread
static constexpr UINT WM_NL_REQUEST = WM_APP + 1;
void nl_request(NLRequest r) { if (s_window) PostMessageW(s_window, WM_NL_REQUEST, (WPARAM)r, 0); }
bool nl_key_down(uint32_t vk) { return g_window_info.get_keystate(vk); }

static void restart_self() {
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring exe(4096, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size()));
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Maus = Touchscreen des GamePads (Positionen in echten Pixeln, wie bei Cemu)
// Maus: im Hauptfenster entweder Kamera (Zeiger versteckt) oder Zeiger (Touch auf dem GamePad-Bild, Menue)
static bool s_mouse_left = false;
static void update_mouse(HWND win, LPARAM lp, std::optional<bool> left, std::optional<bool> right) {
    auto& im = InputManager::instance();
    const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    if (win == s_pad_window) {   // eigenes GamePad-Fenster: wie bisher (Cemu wertet es als Touch)
        auto& m = im.m_pad_mouse;
        std::scoped_lock lock(m.m_mutex);
        m.position = {x, y};
        if (left) { m.left_down = *left; if (*left) m.left_down_toggle = true; }
        return;
    }
    const bool pointer = nl_mouse_pointer_mode();
    if (left) s_mouse_left = *left;
    {
        // Menue (ImGui) bekommt Position immer, Klicks nur wenn es offen ist
        auto& m = im.m_main_mouse;
        std::scoped_lock lock(m.m_mutex);
        m.position = {x, y};
        bool menu = g_nl_menu_open;
        m.left_down = menu && s_mouse_left;
        if (left && *left && menu) m.left_down_toggle = true;
        if (right) m.right_down = menu && *right;
    }
    if (pointer) {
        nl_mouse_touch(s_mouse_left, x, y);
    } else {
        nl_mouse_touch(false, x, y);
        // Kamera-Modus: Maustasten wie Tasten (belegbar im Menue)
        if (left) g_window_info.set_keystate(VK_LBUTTON, *left);
        if (right) g_window_info.set_keystate(VK_RBUTTON, *right);
    }
}

// Zeiger zeigen/verstecken und im Kamera-Modus im Fenster festhalten
static bool s_cursor_hidden = false;
static void update_cursor() {
    const bool hide = !nl_mouse_pointer_mode() && GetForegroundWindow() == s_window;
    if (hide != s_cursor_hidden) {
        s_cursor_hidden = hide;
        if (hide) {
            RECT rc;
            GetClientRect(s_window, &rc);
            MapWindowPoints(s_window, nullptr, (POINT*)&rc, 2);
            ClipCursor(&rc);
            nl_mouse_touch(false, 0, 0);
        } else {
            ClipCursor(nullptr);
            g_window_info.set_keystate(VK_LBUTTON, false);
            g_window_info.set_keystate(VK_RBUTTON, false);
        }
        POINT p;
        GetCursorPos(&p);
        SetCursorPos(p.x, p.y);   // loest WM_SETCURSOR aus
    }
}

// Bildflaechen fuer TV + GamePad in einem Fenster
static void screens_layout(int w, int h, bool drc_big, int* tv, int* drc) {
    int bg[4], sml[4];
    if (g_nl.screen_layout == 2) {
        // nebeneinander: grosses Bild 2/3, kleines 1/3 der Breite
        int u = std::min(w / 3, h * 16 / 18);
        int bw = 2 * u, bh = bw * 9 / 16, sw = u, sh = sw * 9 / 16;
        int x0 = (w - bw - sw) / 2;
        bg[0] = x0; bg[1] = (h - bh) / 2; bg[2] = bw; bg[3] = bh;
        sml[0] = x0 + bw; sml[1] = (h - sh) / 2; sml[2] = sw; sml[3] = sh;
    } else {
        // Bild-in-Bild: grosses Bild fuellt das Fenster, kleines unten rechts
        int bw = w, bh = w * 9 / 16;
        if (bh > h) { bh = h; bw = h * 16 / 9; }
        bg[0] = (w - bw) / 2; bg[1] = (h - bh) / 2; bg[2] = bw; bg[3] = bh;
        int sw = bw * std::clamp(g_nl.pip_size, 15, 60) / 100, sh = sw * 9 / 16, m = bw / 60;
        sml[0] = bg[0] + bw - sw - m; sml[1] = bg[1] + bh - sh - m; sml[2] = sw; sml[3] = sh;
    }
    for (int k = 0; k < 4; k++) {
        tv[k] = drc_big ? sml[k] : bg[k];
        drc[k] = drc_big ? bg[k] : sml[k];
        g_nl_screens.drc_rect[k] = drc[k];
    }
}


static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
    case WM_DPICHANGED:
        if (msg == WM_DPICHANGED) {
            auto* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (s_window) update_window_sizes();
        if (s_cursor_hidden) { s_cursor_hidden = false; ClipCursor(nullptr); update_cursor(); }
        return msg == WM_SIZE ? 0 : DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ACTIVATEAPP:
        g_window_info.app_active = wp != 0;
        if (!wp) g_window_info.set_keystatesup();
        update_cursor();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hwnd == s_window) {
            SetCursor(s_cursor_hidden ? nullptr : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && s_cursor_hidden)
            nl_mouse_add_delta(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F11 || (wp == VK_RETURN && (lp & (1 << 29)))) {
            if (!(lp & (1 << 30))) {
                set_fullscreen(!g_window_info.is_fullscreen);
                nl_settings_save();
            }
            return 0;
        }
        if (wp == VK_F4 && (lp & (1 << 29))) break;   // Alt+F4 normal
        g_window_info.set_keystate((uint32)wp, true);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        g_window_info.set_keystate((uint32)wp, false);
        return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd); update_mouse(hwnd, lp, true, {}); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); update_mouse(hwnd, lp, false, {}); return 0;
    case WM_RBUTTONDOWN: update_mouse(hwnd, lp, {}, true); return 0;
    case WM_RBUTTONUP: update_mouse(hwnd, lp, {}, false); return 0;
    case WM_MOUSEMOVE: update_mouse(hwnd, lp, {}, {}); return 0;
    case WM_MBUTTONDOWN: if (!nl_mouse_pointer_mode()) g_window_info.set_keystate(VK_MBUTTON, true); return 0;
    case WM_MBUTTONUP: g_window_info.set_keystate(VK_MBUTTON, false); return 0;
    case WM_MOUSEWHEEL:
        InputManager::instance().m_mouse_wheel = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;
    case WM_ERASEBKGND:
        return 1;   // Vulkan malt alles selbst
    case WM_NL_REQUEST:
        switch ((NLRequest)wp) {
        case NLRequest::SaveSettings: nl_settings_save(); break;
        case NLRequest::ApplyFullscreen: set_fullscreen(g_nl.fullscreen); nl_settings_save(); break;
        case NLRequest::Restart:
            nl_settings_save();
            nl_input_save();
            restart_self();
            _exit(0);
        case NLRequest::Quit:
            nl_settings_save();
            nl_input_save();
            _exit(0);
        }
        return 0;
    case WM_TIMER: {
        if (wp == 2) {   // schnell: Zeiger und Bild-Modus aktuell halten
            g_nl_screens.mode = WindowSystem::IsPadWindowOpen() ? 0 : g_nl.screen_layout;
            update_cursor();
            return 0;
        }
        nl_input_hotplug();
        const char* view = g_nl_screens.mode ? (LatteGPUState.isDRCPrimary ? "GamePad gross" : "TV gross")
                                             : (LatteGPUState.isDRCPrimary ? "GamePad-Bild" : "TV-Bild");
        std::string t = s_loading ? std::string("Nintendo Land - laedt ...")
                                  : fmt::format("Nintendo Land - {:.0f} FPS - {}   |   Tab: Bilder tauschen   Alt: Mauszeiger   Esc: Menue",
                                                s_fps.load(), view);
        SetWindowTextW(s_window, boost::nowide::widen(t).c_str());
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_window(const wchar_t* title, int w, int h) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"NintendoLandWindow";
        RegisterClassExW(&wc);
        registered = true;
    }
    // Groesse ist die Innenflaeche, an DPI angepasst
    UINT dpi = GetDpiForSystem();
    RECT rc{0, 0, MulDiv(w, dpi, 96), MulDiv(h, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    return CreateWindowExW(0, L"NintendoLandWindow", title, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                           nullptr, nullptr, inst, nullptr);
}

// ===========================================================================
int main(int argc, char* argv[]) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    // Spiel-Pfad: erstes Argument (Lunch.rpx oder Spielordner)
    fs::path game_path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-g" || a == "--game") && i + 1 < argc) game_path = _utf8ToPath(argv[++i]);
        else if (a[0] != '-') game_path = _utf8ToPath(a);
    }

    // Pfade: alles liegt neben der .exe im Ordner "nl_data"
    fs::path exe_path;
    std::wstring buf(4096, L'\0');
    buf.resize(GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size()));
    exe_path = buf;
    SetCurrentDirectoryW(exe_path.parent_path().c_str());
    fs::path data_dir = exe_path.parent_path() / "nl_data";
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    std::set<fs::path> failed_write;
    ActiveSettings::SetPaths(true, exe_path, data_dir, data_dir, data_dir, exe_path.parent_path(), failed_write);
    nl_settings_load(data_dir / "nl_settings.ini");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    // Konfiguration (intern im Cemu-Format, aber von uns gesteuert)
    GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
    if (fs::exists(ActiveSettings::GetConfigPath("settings.xml"), ec))
        GetConfigHandle().Load();
    NetworkConfig::LoadOnce();
    auto& config = GetConfig();
    config.graphic_api = GraphicAPI::kVulkan;
    config.vsync = g_nl.vsync;
    config.async_compile = g_nl.async_shaders;
    config.console_language = (CafeConsoleLanguage)g_nl.language;
    nl_settings_apply_live();
    GetConfigHandle().Save();

    if (!create_default_dirs(ActiveSettings::GetMlcPath()))
        fatal("Konnte den Datenordner nicht anlegen: " + _pathToUtf8(data_dir));
    ActiveSettings::Init();
    LatteOverlay_init();

    // Grundsystem (entspricht CemuCommonInit)
    AES128_init();
    PPCTimer_init();
    ExceptionHandler_Init();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    IAudioAPI::InitializeStatic();
    IAudioInputAPI::InitializeStatic();
    // Eigenes Grafikpaket (Aufloesung, Texturfilter) schreiben und einschalten
    nl_write_resolution_pack(data_dir);
    GraphicPack2::LoadAll();
    for (auto& gp : GraphicPack2::GetGraphicPacks())
        if (_pathToUtf8(gp->GetRulesPath()).find("NintendoLandRecomp") != std::string::npos) gp->SetEnabled(true);
    InputManager::instance().load();
    CafeSystem::Initialize();
    CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
    CafeTitleList::SetMLCPath(ActiveSettings::GetMlcPath());
    CafeSaveList::Initialize();
    CafeSaveList::SetMLCPath(ActiveSettings::GetMlcPath());
    nl_input_init(data_dir / "controls.ini");
    for (int i = 0; i < 20 && nl_gamepad_name().empty(); i++) {   // SDL startet im Hintergrund
        nl_input_hotplug();
        if (nl_gamepad_name().empty()) Sleep(50);
    }
    setup_mic();
    nl_menu_install();

    if (game_path.empty())
        fatal("Kein Spiel angegeben. Bitte ueber den Launcher starten.");

    // Fenster
    s_window = make_window(L"Nintendo Land", g_nl.window_w, g_nl.window_h);
    if (!s_window) fatal("Fenster konnte nicht erstellt werden.");
    if (g_nl.gamepad_window) {
        s_pad_window = make_window(L"Nintendo Land - GamePad", 854, 480);
        g_window_info.pad_open = s_pad_window != nullptr;
    }
    g_window_info.app_active = true;
    g_window_info.window_main = handle_of(s_window);
    g_window_info.canvas_main = handle_of(s_window);
    if (s_pad_window) {
        g_window_info.window_pad = handle_of(s_pad_window);
        g_window_info.canvas_pad = handle_of(s_pad_window);
        ShowWindow(s_pad_window, SW_SHOW);
    }
    ShowWindow(s_window, SW_SHOW);
    SetForegroundWindow(s_window);
    update_window_sizes();
    if (g_nl.fullscreen) set_fullscreen(true);
    SetTimer(s_window, 1, 500, nullptr);
    SetTimer(s_window, 2, 30, nullptr);
    // TV + GamePad in einem Fenster
    g_nl_screens.layout = screens_layout;
    g_nl_screens.mode = s_pad_window ? 0 : g_nl.screen_layout;
    // Rohe Mausbewegung fuer die Maus-Kamera
    RAWINPUTDEVICE rid{0x01, 0x02, 0, s_window};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    // Renderer (Vulkan) an unser Fenster haengen
    if (!InitializeGlobalVulkan() || !g_vulkan_available)
        fatal("Vulkan ist nicht verfuegbar. Grafiktreiber aktualisieren.");
    try {
        g_renderer = std::make_unique<VulkanRenderer>();
        VulkanRenderer::GetInstance()->InitializeSurface({g_window_info.phys_width, g_window_info.phys_height}, true);
        if (s_pad_window)
            VulkanRenderer::GetInstance()->InitializeSurface({g_window_info.phys_pad_width, g_window_info.phys_pad_height}, false);
    } catch (const std::exception& ex) {
        fatal(std::string("Vulkan-Fehler: ") + ex.what());
    }

    // Spiel vorbereiten und starten
    TitleInfo title{game_path};
    if (title.IsValid()) {
        CafeTitleList::AddTitleFromPath(game_path);
        TitleId base_id;
        if (!CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), base_id))
            fatal("Spieldateien nicht gefunden: " + _pathToUtf8(game_path));
        if (CafeSystem::PrepareForegroundTitle(base_id) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
            fatal("Spiel konnte nicht geladen werden: " + _pathToUtf8(game_path));
    } else if (CafeSystem::PrepareForegroundTitleFromStandaloneRPX(game_path) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS) {
        fatal("Spiel konnte nicht geladen werden: " + _pathToUtf8(game_path));
    }
    // Beim Start fragt Nintendo Land auf dem GamePad nach Dingen -> erst das GamePad-Bild zeigen
    if (!s_pad_window) LatteGPUState.isDRCPrimary = g_nl.start_view == 1;
    CafeSystem::LaunchForegroundTitle();

    // Hauptschleife: Windows-Nachrichten (Fenster, Tastatur, Maus)
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    cemuLog_log(LogType::Force, "Nintendo Land: wird beendet");
    if (CafeSystem::IsTitleRunning()) CafeSystem::ShutdownTitle();
    _exit(0);
}

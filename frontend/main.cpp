// Nintendo Land Recomp - eigenes Programm.
//
// Eigenes Fenster, eigene Einstellungen (nl_settings.ini), startet direkt ins Spiel.
// Der recompilierte Spielcode laeuft nativ; fuer Betriebssystem-Funktionen, Grafik
// (Vulkan), Audio und Eingabe werden Cemus Bibliotheken (MPL-2.0) als Unterbau genutzt,
// ohne Cemus Oberflaeche.
#include "WindowSystem.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Espresso/PPCState.h"
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
// Einstellungen (nl_settings.ini, wird vom Launcher geschrieben)
// ===========================================================================
struct NLSettings {
    bool fullscreen = false;
    bool vsync = true;
    bool gamepad_window = false;   // GamePad-Bild in eigenem Fenster
    int volume = 60;
    bool mic = true;
    int window_w = 1280, window_h = 720;
    std::map<std::string, uint32> keys;   // Aktion -> Windows-Tastencode
};
static NLSettings s_settings;

static void load_settings(const fs::path& path) {
    // Standard-Tastenbelegung
    s_settings.keys = {
        {"A", VK_RETURN}, {"B", VK_BACK}, {"X", 'X'}, {"Y", 'Y'},
        {"L", 'Q'}, {"R", 'E'}, {"ZL", '1'}, {"ZR", '3'},
        {"PLUS", 'P'}, {"MINUS", 'M'}, {"HOME", 'H'},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"LS_UP", 'W'}, {"LS_DOWN", 'S'}, {"LS_LEFT", 'A'}, {"LS_RIGHT", 'D'},
        {"RS_UP", 'I'}, {"RS_DOWN", 'K'}, {"RS_LEFT", 'J'}, {"RS_RIGHT", 'L'},
        {"LS_CLICK", 'C'}, {"RS_CLICK", 'V'}, {"MIC", 'T'}, {"SCREEN", VK_TAB},
    };
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
            while (!s.empty() && isspace((unsigned char)s.front())) s.erase(0, 1);
        };
        trim(k);
        trim(v);
        auto b = [&] { return v == "1" || v == "true" || v == "an"; };
        if (k == "fullscreen") s_settings.fullscreen = b();
        else if (k == "vsync") s_settings.vsync = b();
        else if (k == "gamepad_window") s_settings.gamepad_window = b();
        else if (k == "volume") s_settings.volume = std::clamp(atoi(v.c_str()), 0, 100);
        else if (k == "mic") s_settings.mic = b();
        else if (k == "window_w") s_settings.window_w = std::max(320, atoi(v.c_str()));
        else if (k == "window_h") s_settings.window_h = std::max(240, atoi(v.c_str()));
        else if (k.rfind("key_", 0) == 0) s_settings.keys[k.substr(4)] = (uint32)strtoul(v.c_str(), nullptr, 0);
    }
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
    case PlatformKeyCodes::TAB: return IsKeyDown((uint32)VK_TAB);
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

==========
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

// Eingabe: Tastatur + erster Controller = GamePad, weitere Controller = Wiimotes
static void setup_input() {
    auto& im = InputManager::instance();
    auto vpad = im.set_controller(0, EmulatedController::Type::VPAD);

    auto kb = ControllerFactory::CreateController(InputAPI::Keyboard, "keyboard", "Tastatur");
    vpad->add_controller(kb);
    const std::pair<const char*, uint64> ids[] = {
        {"A", VPADController::kButtonId_A}, {"B", VPADController::kButtonId_B},
        {"X", VPADController::kButtonId_X}, {"Y", VPADController::kButtonId_Y},
        {"L", VPADController::kButtonId_L}, {"R", VPADController::kButtonId_R},
        {"ZL", VPADController::kButtonId_ZL}, {"ZR", VPADController::kButtonId_ZR},
        {"PLUS", VPADController::kButtonId_Plus}, {"MINUS", VPADController::kButtonId_Minus},
        {"HOME", VPADController::kButtonId_Home},
        {"UP", VPADController::kButtonId_Up}, {"DOWN", VPADController::kButtonId_Down},
        {"LEFT", VPADController::kButtonId_Left}, {"RIGHT", VPADController::kButtonId_Right},
        {"LS_UP", VPADController::kButtonId_StickL_Up}, {"LS_DOWN", VPADController::kButtonId_StickL_Down},
        {"LS_LEFT", VPADController::kButtonId_StickL_Left}, {"LS_RIGHT", VPADController::kButtonId_StickL_Right},
        {"RS_UP", VPADController::kButtonId_StickR_Up}, {"RS_DOWN", VPADController::kButtonId_StickR_Down},
        {"RS_LEFT", VPADController::kButtonId_StickR_Left}, {"RS_RIGHT", VPADController::kButtonId_StickR_Right},
        {"LS_CLICK", VPADController::kButtonId_StickL}, {"RS_CLICK", VPADController::kButtonId_StickR},
        {"MIC", VPADController::kButtonId_Mic}, {"SCREEN", VPADController::kButtonId_Screen},
    };
    for (auto& [name, id] : ids) {
        auto it = s_settings.keys.find(name);
        if (it != s_settings.keys.end() && it->second) vpad->set_mapping(id, kb, it->second);
    }

}

// Controller ueber SDL (Xbox, PlayStation, Switch Pro, ...). Wird regelmaessig aufgerufen,
// damit auch spaeter eingesteckte Controller erkannt werden.
// Erster Controller = GamePad (zusaetzlich zur Tastatur), weitere = Wiimotes.
static std::set<std::string> s_assigned_pads;
static size_t s_next_wiimote = 1;
static void assign_gamepads() {
    auto& im = InputManager::instance();
    auto provider = im.get_api_provider(InputAPI::SDLController);
    if (!provider) return;
    for (auto& pad : provider->get_controllers()) {
        std::string id = pad->uuid() + "|" + pad->display_name();
        if (s_assigned_pads.count(id)) continue;
        s_assigned_pads.insert(id);
        if (s_assigned_pads.size() == 1) {
            auto vpad = im.get_controller(0);
            if (!vpad) continue;
            vpad->add_controller(pad);
            vpad->set_default_mapping(pad);
            cemuLog_log(LogType::Force, "Nintendo Land: Controller als GamePad: {}", pad->display_name());
        } else if (s_next_wiimote <= 4) {
            auto wiimote = im.set_controller(s_next_wiimote, EmulatedController::Type::Wiimote, pad);
            wiimote->set_default_mapping(pad);
            cemuLog_log(LogType::Force, "Nintendo Land: Controller als Wiimote {}: {}", s_next_wiimote, pad->display_name());
            s_next_wiimote++;
        }
    }
}

static void setup_mic() {
    auto& config = GetConfig();
    if (!s_settings.mic) { config.input_device = L""; return; }
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
}

// Maus = Touchscreen des GamePads (Positionen in echten Pixeln, wie bei Cemu)
static void update_mouse(HWND win, LPARAM lp, std::optional<bool> left, std::optional<bool> right) {
    auto& im = InputManager::instance();
    auto& m = (win == s_pad_window) ? im.m_pad_mouse : im.m_main_mouse;
    std::scoped_lock lock(m.m_mutex);
    m.position = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (left) {
        m.left_down = *left;
        if (*left) m.left_down_toggle = true;
    }
    if (right) {
        m.right_down = *right;
        if (*right) m.right_down_toggle = true;
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
        return msg == WM_SIZE ? 0 : DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ACTIVATEAPP:
        g_window_info.app_active = wp != 0;
        if (!wp) g_window_info.set_keystatesup();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F11 || (wp == VK_RETURN && (lp & (1 << 29)))) {
            if (!(lp & (1 << 30))) set_fullscreen(!g_window_info.is_fullscreen);
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
    case WM_MOUSEWHEEL:
        InputManager::instance().m_mouse_wheel = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;
    case WM_ERASEBKGND:
        return 1;   // Vulkan malt alles selbst
    case WM_TIMER: {
        assign_gamepads();
        std::string t = s_loading ? "Nintendo Land - laedt ..." : fmt::format("Nintendo Land - {:.0f} FPS", s_fps.load());
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
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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
    load_settings(data_dir / "nl_settings.ini");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    // Konfiguration (intern im Cemu-Format, aber von uns gesteuert)
    GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
    if (fs::exists(ActiveSettings::GetConfigPath("settings.xml"), ec))
        GetConfigHandle().Load();
    NetworkConfig::LoadOnce();
    auto& config = GetConfig();
    config.graphic_api = GraphicAPI::kVulkan;
    config.vsync = s_settings.vsync ? 1 : 0;
    config.tv_volume = s_settings.volume;
    config.pad_volume = s_settings.gamepad_window ? s_settings.volume : 0;
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
    GraphicPack2::LoadAll();
    InputManager::instance().load();
    CafeSystem::Initialize();
    CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
    CafeTitleList::SetMLCPath(ActiveSettings::GetMlcPath());
    CafeSaveList::Initialize();
    CafeSaveList::SetMLCPath(ActiveSettings::GetMlcPath());
    setup_input();
    for (int i = 0; i < 20 && s_assigned_pads.empty(); i++) {   // SDL startet im Hintergrund
        assign_gamepads();
        if (s_assigned_pads.empty()) Sleep(50);
    }
    setup_mic();

    if (game_path.empty())
        fatal("Kein Spiel angegeben. Bitte ueber den Launcher starten.");

    // Fenster
    s_window = make_window(L"Nintendo Land", s_settings.window_w, s_settings.window_h);
    if (!s_window) fatal("Fenster konnte nicht erstellt werden.");
    if (s_settings.gamepad_window) {
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
    if (s_settings.fullscreen) set_fullscreen(true);
    SetTimer(s_window, 1, 500, nullptr);

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

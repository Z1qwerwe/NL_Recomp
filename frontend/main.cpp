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
// Einstellungen (nl_settings.ini, wird vom Launcher geschrieben)
// ===========================================================================
struct NLSettings {
    bool fullscreen = false;
    bool vsync = true;
    bool gamepad_window = false;   // GamePad-Bild in eigenem Fenster
    int volume = 60;
    bool mic = true;
    int window_w = 1280, window_h = 720;
};
static NLSettings s_settings;

static void load_settings(const fs::path& path) {
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

// ===========================================================================
// Eingabe
//
// Cemu erlaubt pro Aktion genau eine Quelle (Taste ODER Controller-Knopf).
// Regel: Ist ein Controller da, nutzt jede Aktion seine Belegung, ausser man hat ihr im
// Menue (F1) eine Taste gegeben. Ohne Controller gilt die Tastatur.
// Die Belegung gehoert dem Spiel und liegt in nl_data/controls.ini.
// ===========================================================================
struct Action { const char* name; const char* label; uint64 id; uint32 default_key; };
static const Action kActions[] = {
    {"A", "A-Knopf", VPADController::kButtonId_A, VK_RETURN},
    {"B", "B-Knopf", VPADController::kButtonId_B, VK_BACK},
    {"X", "X-Knopf", VPADController::kButtonId_X, 'X'},
    {"Y", "Y-Knopf", VPADController::kButtonId_Y, 'Y'},
    {"L", "L", VPADController::kButtonId_L, 'Q'},
    {"R", "R", VPADController::kButtonId_R, 'E'},
    {"ZL", "ZL", VPADController::kButtonId_ZL, '1'},
    {"ZR", "ZR", VPADController::kButtonId_ZR, '3'},
    {"PLUS", "Plus (+)", VPADController::kButtonId_Plus, 'P'},
    {"MINUS", "Minus (-)", VPADController::kButtonId_Minus, 'M'},
    {"HOME", "Home", VPADController::kButtonId_Home, 'H'},
    {"UP", "Steuerkreuz hoch", VPADController::kButtonId_Up, VK_UP},
    {"DOWN", "Steuerkreuz runter", VPADController::kButtonId_Down, VK_DOWN},
    {"LEFT", "Steuerkreuz links", VPADController::kButtonId_Left, VK_LEFT},
    {"RIGHT", "Steuerkreuz rechts", VPADController::kButtonId_Right, VK_RIGHT},
    {"LS_UP", "Linker Stick hoch", VPADController::kButtonId_StickL_Up, 'W'},
    {"LS_DOWN", "Linker Stick runter", VPADController::kButtonId_StickL_Down, 'S'},
    {"LS_LEFT", "Linker Stick links", VPADController::kButtonId_StickL_Left, 'A'},
    {"LS_RIGHT", "Linker Stick rechts", VPADController::kButtonId_StickL_Right, 'D'},
    {"RS_UP", "Rechter Stick hoch", VPADController::kButtonId_StickR_Up, 'I'},
    {"RS_DOWN", "Rechter Stick runter", VPADController::kButtonId_StickR_Down, 'K'},
    {"RS_LEFT", "Rechter Stick links", VPADController::kButtonId_StickR_Left, 'J'},
    {"RS_RIGHT", "Rechter Stick rechts", VPADController::kButtonId_StickR_Right, 'L'},
    {"LS_CLICK", "Linker Stick druecken", VPADController::kButtonId_StickL, 'C'},
    {"RS_CLICK", "Rechter Stick druecken", VPADController::kButtonId_StickR, 'V'},
    {"MIC", "Mikrofon (halten)", VPADController::kButtonId_Mic, 'T'},
    {"SCREEN", "TV/GamePad-Bild wechseln", VPADController::kButtonId_Screen, VK_TAB},
};
constexpr size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

// Pro Aktion: Taste (0 = Standard) und Controller-Knopf (-1 = Standard, 0 = Tastatur benutzen)
struct Binding { uint32 key = 0; int64 pad = -1; };
static std::map<std::string, Binding> s_bind;
static fs::path s_controls_path;
static std::shared_ptr<ControllerBase> s_keyboard, s_gamepad;
static std::mutex s_input_mutex;

static uint32 key_of(const Action& a) {
    auto it = s_bind.find(a.name);
    return (it != s_bind.end() && it->second.key) ? it->second.key : a.default_key;
}

static void load_controls() {
    std::ifstream f(s_controls_path);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k.rfind("key_", 0) == 0) s_bind[k.substr(4)].key = (uint32)strtoul(v.c_str(), nullptr, 0);
        else if (k.rfind("pad_", 0) == 0) s_bind[k.substr(4)].pad = strtoll(v.c_str(), nullptr, 0);
    }
}

static void save_controls() {
    std::ofstream f(s_controls_path);
    f << "# Steuerung von Nintendo Land (im Spiel mit F1 aendern)\n";
    for (auto& [name, b] : s_bind) {
        if (b.key) f << "key_" << name << "=0x" << std::hex << b.key << std::dec << "\n";
        if (b.pad != -1) f << "pad_" << name << "=" << b.pad << "\n";
    }
}

// Baut die GamePad-Belegung komplett neu auf
static void apply_controls() {
    std::scoped_lock lock(s_input_mutex);
    auto& im = InputManager::instance();
    auto old = im.get_vpad_controller(0);
    bool toggle = true;
    auto vpad = std::static_pointer_cast<VPADController>(im.set_controller(0, EmulatedController::Type::VPAD));
    vpad->set_screen_toggle(toggle);   // Bild-Wechsel schaltet um statt nur beim Halten
    vpad->add_controller(s_keyboard);
    if (s_gamepad) {
        vpad->add_controller(s_gamepad);
        vpad->set_default_mapping(s_gamepad);
    }
    for (auto& a : kActions) {
        auto it = s_bind.find(a.name);
        Binding b = it != s_bind.end() ? it->second : Binding{};
        bool use_pad = s_gamepad && b.pad != 0 && (b.pad > 0 || vpad->get_mapping_controller(a.id));
        if (use_pad && b.pad > 0) vpad->set_mapping(a.id, s_gamepad, (uint64)b.pad);
        else if (!use_pad) vpad->set_mapping(a.id, s_keyboard, key_of(a));
    }
}

static std::string binding_text(const Action& a) {
    auto vpad = InputManager::instance().get_vpad_controller(0);
    if (!vpad) return "-";
    auto ctrl = vpad->get_mapping_controller(a.id);
    if (!ctrl) return "-";
    std::string name = vpad->get_mapping_name(a.id);
    if (ctrl == s_keyboard) return "Taste " + name;
    return "Controller " + name;
}

static void setup_input() {
    s_keyboard = ControllerFactory::CreateController(InputAPI::Keyboard, "keyboard", "Tastatur");
    load_controls();
    apply_controls();
}

// Controller ueber SDL (Xbox, PlayStation, Switch Pro, ...). Wird regelmaessig aufgerufen,
// damit auch spaeter eingesteckte Controller erkannt werden.
// Erster Controller = GamePad, weitere = Wiimotes.
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
        if (!s_gamepad) {
            s_gamepad = pad;
            apply_controls();
            cemuLog_log(LogType::Force, "Nintendo Land: Controller als GamePad: {}", pad->display_name());
        } else if (s_next_wiimote <= 4) {
            auto wiimote = im.set_controller(s_next_wiimote, EmulatedController::Type::Wiimote, pad);
            wiimote->set_default_mapping(pad);
            cemuLog_log(LogType::Force, "Nintendo Land: Controller als Wiimote {}: {}", s_next_wiimote, pad->display_name());
            s_next_wiimote++;
        }
    }
}

// ===========================================================================
// Steuerungs-Menue im Spiel (F1)
// ===========================================================================
static HWND s_ctrl_win = nullptr, s_ctrl_list = nullptr, s_ctrl_hint = nullptr;
static int s_capture = -1;            // Index der Aktion, die gerade neu belegt wird
static uint64 s_capture_start = 0;
static ControllerState s_pad_rest{};  // Controller-Zustand beim Start der Aufnahme

static void ctrl_refresh() {
    if (!s_ctrl_list) return;
    int top = (int)SendMessageW(s_ctrl_list, LB_GETTOPINDEX, 0, 0);
    int sel = (int)SendMessageW(s_ctrl_list, LB_GETCURSEL, 0, 0);
    SendMessageW(s_ctrl_list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < kActionCount; i++) {
        std::string t = fmt::format("{:<26}\t{}", kActions[i].label,
                                    (int)i == s_capture ? std::string(">>> Taste oder Controller-Knopf druecken ...") : binding_text(kActions[i]));
        SendMessageW(s_ctrl_list, LB_ADDSTRING, 0, (LPARAM)boost::nowide::widen(t).c_str());
    }
    SendMessageW(s_ctrl_list, LB_SETTOPINDEX, top, 0);
    SendMessageW(s_ctrl_list, LB_SETCURSEL, sel, 0);
    std::string hint = s_gamepad ? fmt::format("Controller: {}", s_gamepad->display_name()) : std::string("Kein Controller erkannt (nur Tastatur)");
    hint += "\nAktion anklicken, dann Taste oder Controller-Knopf druecken. Esc = abbrechen.";
    SetWindowTextW(s_ctrl_hint, boost::nowide::widen(hint).c_str());
}

static void ctrl_finish(int64 pad_code, uint32 key) {
    const Action& a = kActions[s_capture];
    auto& b = s_bind[a.name];
    if (key) { b.key = key; b.pad = 0; }     // Taste: diese Aktion nutzt die Tastatur
    else b.pad = pad_code;                    // Controller-Knopf
    s_capture = -1;
    apply_controls();
    save_controls();
    ctrl_refresh();
}

// Wird alle 30 ms aufgerufen, solange eine Aufnahme laeuft
static void ctrl_poll() {
    if (s_capture < 0) return;
    if (GetTickCount64() - s_capture_start < 250) return;   // Klick nicht als Eingabe werten
    if (GetForegroundWindow() != s_ctrl_win) return;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { s_capture = -1; ctrl_refresh(); return; }
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        if (vk == VK_ESCAPE || vk == VK_F1 || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
        if (GetAsyncKeyState(vk) & 0x8000) { ctrl_finish(0, (uint32)vk); return; }
    }
    if (s_gamepad) {
        const auto& st = s_gamepad->update_state();
        for (uint32 btn : st.buttons.GetButtonList()) {
            if (!s_pad_rest.buttons.GetButtonState(btn)) { ctrl_finish((int64)btn, 0); return; }
        }
        auto axis_code = [&](const glm::vec2& v, const glm::vec2& rest, uint32 xp, uint32 yp, uint32 xn, uint32 yn) -> int64 {
            if (v.x - rest.x > 0.6f) return xp;
            if (v.y - rest.y > 0.6f) return yp;
            if (v.x - rest.x < -0.6f) return xn;
            if (v.y - rest.y < -0.6f) return yn;
            return -1;
        };
        int64 c = axis_code(st.axis, s_pad_rest.axis, kAxisXP, kAxisYP, kAxisXN, kAxisYN);
        if (c < 0) c = axis_code(st.rotation, s_pad_rest.rotation, kRotationXP, kRotationYP, kRotationXN, kRotationYN);
        if (c < 0) c = axis_code(st.trigger, s_pad_rest.trigger, kTriggerXP, kTriggerYP, kTriggerXN, kTriggerYN);
        if (c >= 0) { ctrl_finish(c, 0); return; }
    }
}

static LRESULT CALLBACK ctrl_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == 100 && HIWORD(wp) == LBN_SELCHANGE) {
            s_capture = (int)SendMessageW(s_ctrl_list, LB_GETCURSEL, 0, 0);
            s_capture_start = GetTickCount64();
            if (s_gamepad) s_pad_rest = ControllerState(s_gamepad->update_state());
            ctrl_refresh();
        } else if (LOWORD(wp) == 101) {   // Standard
            s_bind.clear();
            s_capture = -1;
            apply_controls();
            save_controls();
            ctrl_refresh();
        } else if (LOWORD(wp) == 102) {   // Fertig
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_TIMER:
        ctrl_poll();
        return 0;
    case WM_DESTROY:
        s_ctrl_win = s_ctrl_list = s_ctrl_hint = nullptr;
        s_capture = -1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void open_controls_menu(HWND owner) {
    if (s_ctrl_win) { SetForegroundWindow(s_ctrl_win); return; }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = ctrl_proc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"NintendoLandControls";
        RegisterClassExW(&wc);
        registered = true;
    }
    UINT dpi = GetDpiForWindow(owner);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    s_ctrl_win = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NintendoLandControls", L"Steuerung",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, S(560), S(640),
                                 owner, nullptr, inst, nullptr);
    HFONT font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_ctrl_hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, S(12), S(10), S(520), S(44), s_ctrl_win, nullptr, inst, nullptr);
    s_ctrl_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_USETABSTOPS,
                                  S(12), S(58), S(520), S(480), s_ctrl_win, (HMENU)100, inst, nullptr);
    HWND reset = CreateWindowExW(0, L"BUTTON", L"Standard", WS_CHILD | WS_VISIBLE, S(12), S(548), S(120), S(32), s_ctrl_win, (HMENU)101, inst, nullptr);
    HWND done = CreateWindowExW(0, L"BUTTON", L"Fertig", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, S(412), S(548), S(120), S(32), s_ctrl_win, (HMENU)102, inst, nullptr);
    for (HWND h : {s_ctrl_hint, s_ctrl_list, reset, done}) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    int tabs[] = {130};
    SendMessageW(s_ctrl_list, LB_SETTABSTOPS, 1, (LPARAM)tabs);
    // mittig ueber dem Spielfenster
    RECT o{}, c{};
    GetWindowRect(owner, &o);
    GetWindowRect(s_ctrl_win, &c);
    SetWindowPos(s_ctrl_win, nullptr, (o.left + o.right - (c.right - c.left)) / 2, (o.top + o.bottom - (c.bottom - c.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ctrl_refresh();
    SetTimer(s_ctrl_win, 1, 30, nullptr);
    ShowWindow(s_ctrl_win, SW_SHOW);
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
        if (wp == VK_F1) {
            if (!(lp & (1 << 30))) open_controls_menu(s_window);
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
        const char* view = LatteGPUState.isDRCPrimary ? "GamePad-Bild" : "TV-Bild";
        std::string t = s_loading ? std::string("Nintendo Land - laedt ...")
                                  : fmt::format("Nintendo Land - {:.0f} FPS - {}   |   Tab: Bild wechseln   F1: Steuerung   F11: Vollbild",
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
    load_settings(data_dir / "nl_settings.ini");
    s_controls_path = data_dir / "controls.ini";

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
    // Beim Start fragt Nintendo Land auf dem GamePad nach Dingen -> erst das GamePad-Bild zeigen
    if (!s_pad_window) LatteGPUState.isDRCPrimary = true;
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

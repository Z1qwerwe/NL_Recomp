// Plattform mit SDL3: Fenster (TV + GamePad-Bild), Controller, Maus/Tastatur,
// Tastenbelegung im Spiel (F1) und Mikrofon.
#include "platform.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Belegung
// ---------------------------------------------------------------------------
// VPAD-Knoepfe (Wii U GamePad)
enum : uint32_t {
    VB_SYNC = 0x0001, VB_HOME = 0x0002, VB_MINUS = 0x0004, VB_PLUS = 0x0008,
    VB_R = 0x0010, VB_L = 0x0020, VB_ZR = 0x0040, VB_ZL = 0x0080,
    VB_DOWN = 0x0100, VB_UP = 0x0200, VB_RIGHT = 0x0400, VB_LEFT = 0x0800,
    VB_Y = 0x1000, VB_X = 0x2000, VB_B = 0x4000, VB_A = 0x8000,
    VB_TV = 0x10000, VB_STICK_R = 0x20000, VB_STICK_L = 0x40000,
};

struct Action {
    const char* id;       // Name in controls.ini
    const char* label;    // Anzeige im Menue
    uint32_t button;      // VPAD-Bit, 0 = Stick-Richtung
    int stick;            // 0 = kein, 1 = links, 2 = rechts
    float dx, dy;         // Richtung bei Sticks
    SDL_Scancode key;     // Tastatur
    std::string pad;      // Controller: "button:a" oder "axis:leftx+" usw.
};

static std::vector<Action> g_actions = {
    {"A", "A", VB_A, 0, 0, 0, SDL_SCANCODE_RETURN, "button:b"},
    {"B", "B", VB_B, 0, 0, 0, SDL_SCANCODE_BACKSPACE, "button:a"},
    {"X", "X", VB_X, 0, 0, 0, SDL_SCANCODE_X, "button:y"},
    {"Y", "Y", VB_Y, 0, 0, 0, SDL_SCANCODE_Y, "button:x"},
    {"L", "L", VB_L, 0, 0, 0, SDL_SCANCODE_Q, "button:leftshoulder"},
    {"R", "R", VB_R, 0, 0, 0, SDL_SCANCODE_E, "button:rightshoulder"},
    {"ZL", "ZL", VB_ZL, 0, 0, 0, SDL_SCANCODE_1, "axis:lefttrigger+"},
    {"ZR", "ZR", VB_ZR, 0, 0, 0, SDL_SCANCODE_3, "axis:righttrigger+"},
    {"PLUS", "+ (Start)", VB_PLUS, 0, 0, 0, SDL_SCANCODE_P, "button:start"},
    {"MINUS", "- (Select)", VB_MINUS, 0, 0, 0, SDL_SCANCODE_M, "button:back"},
    {"HOME", "HOME", VB_HOME, 0, 0, 0, SDL_SCANCODE_H, "button:guide"},
    {"UP", "Steuerkreuz hoch", VB_UP, 0, 0, 0, SDL_SCANCODE_UP, "button:dpup"},
    {"DOWN", "Steuerkreuz runter", VB_DOWN, 0, 0, 0, SDL_SCANCODE_DOWN, "button:dpdown"},
    {"LEFT", "Steuerkreuz links", VB_LEFT, 0, 0, 0, SDL_SCANCODE_LEFT, "button:dpleft"},
    {"RIGHT", "Steuerkreuz rechts", VB_RIGHT, 0, 0, 0, SDL_SCANCODE_RIGHT, "button:dpright"},
    {"LS_UP", "Linker Stick hoch", 0, 1, 0, 1, SDL_SCANCODE_W, "axis:lefty-"},
    {"LS_DOWN", "Linker Stick runter", 0, 1, 0, -1, SDL_SCANCODE_S, "axis:lefty+"},
    {"LS_LEFT", "Linker Stick links", 0, 1, -1, 0, SDL_SCANCODE_A, "axis:leftx-"},
    {"LS_RIGHT", "Linker Stick rechts", 0, 1, 1, 0, SDL_SCANCODE_D, "axis:leftx+"},
    {"RS_UP", "Rechter Stick hoch", 0, 2, 0, 1, SDL_SCANCODE_I, "axis:righty-"},
    {"RS_DOWN", "Rechter Stick runter", 0, 2, 0, -1, SDL_SCANCODE_K, "axis:righty+"},
    {"RS_LEFT", "Rechter Stick links", 0, 2, -1, 0, SDL_SCANCODE_J, "axis:rightx-"},
    {"RS_RIGHT", "Rechter Stick rechts", 0, 2, 1, 0, SDL_SCANCODE_L, "axis:rightx+"},
    {"LS_CLICK", "Linker Stick druecken", VB_STICK_L, 0, 0, 0, SDL_SCANCODE_C, "button:leftstick"},
    {"RS_CLICK", "Rechter Stick druecken", VB_STICK_R, 0, 0, 0, SDL_SCANCODE_V, "button:rightstick"},
};

static bool g_mic_enabled = true;
static std::string g_config_path = "controls.ini";

static void save_config() {
    std::ofstream f(g_config_path);
    f << "# Nintendo Land Recomp - Tastenbelegung (im Spiel mit F1 aendern)\n";
    f << "mic = " << (g_mic_enabled ? "on" : "off") << "\n";
    for (auto& a : g_actions)
        f << a.id << " = key:" << SDL_GetScancodeName(a.key) << ", pad:" << a.pad << "\n";
}

static void load_config() {
    std::ifstream f(g_config_path);
    if (!f) { save_config(); return; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
            while (!s.empty() && isspace((unsigned char)s.front())) s.erase(0, 1);
            return s;
        };
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "mic") { g_mic_enabled = (v == "on"); continue; }
        for (auto& a : g_actions) {
            if (k != a.id) continue;
            std::stringstream ss(v);
            std::string part;
            while (std::getline(ss, part, ',')) {
                part = trim(part);
                if (part.rfind("key:", 0) == 0) {
                    SDL_Scancode sc = SDL_GetScancodeFromName(part.substr(4).c_str());
                    a.key = sc;
                } else if (part.rfind("pad:", 0) == 0) {
                    a.pad = part.substr(4);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Zustand
// ---------------------------------------------------------------------------
struct Frame {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    uint64_t version = 0;
};

static SDL_Window* g_win = nullptr;
static SDL_Renderer* g_ren = nullptr;
static SDL_Texture* g_tex[2] = {nullptr, nullptr};
static int g_tex_w[2] = {0, 0}, g_tex_h[2] = {0, 0};
static uint64_t g_tex_version[2] = {0, 0};

static std::mutex g_frame_mutex;
static Frame g_frames[2];
static std::atomic<uint64_t> g_stat_frames{0};
static std::atomic<uint32_t> g_stat_draws{0};

static std::mutex g_input_mutex;
static VpadState g_vpad;
static WiimoteState g_wii[4];

static std::vector<SDL_Gamepad*> g_pads;
static std::atomic<bool> g_quit{false};

// Menue
static bool g_menu = false;
static int g_menu_sel = 0;
static bool g_waiting = false;
static bool g_drc_big = false;  // F2: GamePad-Bild gross

// Mikrofon
static SDL_AudioStream* g_mic = nullptr;
static std::mutex g_mic_mutex;

// Wo das GamePad-Bild im Fenster liegt (fuer Touch)
static SDL_FRect g_drc_rect{0, 0, 0, 0};

// ---------------------------------------------------------------------------
static float pad_value(SDL_Gamepad* gp, const std::string& spec) {
    if (!gp || spec.empty()) return 0;
    if (spec.rfind("button:", 0) == 0) {
        SDL_GamepadButton b = SDL_GetGamepadButtonFromString(spec.substr(7).c_str());
        return (b != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(gp, b)) ? 1.f : 0.f;
    }
    if (spec.rfind("axis:", 0) == 0) {
        std::string ax = spec.substr(5);
        char sign = ax.empty() ? '+' : ax.back();
        if (sign == '+' || sign == '-') ax.pop_back();
        SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(ax.c_str());
        if (a == SDL_GAMEPAD_AXIS_INVALID) return 0;
        float v = SDL_GetGamepadAxis(gp, a) / 32767.f;
        v = sign == '-' ? -v : v;
        return v > 0.15f ? std::fmin(v, 1.f) : 0.f;   // Deadzone
    }
    return 0;
}

static void update_input() {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    SDL_Gamepad* gp0 = g_pads.empty() ? nullptr : g_pads[0];

    VpadState v;
    if (!g_menu) {
        for (auto& a : g_actions) {
            float val = std::fmax(keys[a.key] ? 1.f : 0.f, pad_value(gp0, a.pad));
            if (a.stick == 0) {
                if (val > 0.5f) v.buttons |= a.button;
            } else if (a.stick == 1) {
                v.lx += a.dx * val; v.ly += a.dy * val;
            } else {
                v.rx += a.dx * val; v.ry += a.dy * val;
            }
        }
        auto clamp1 = [](float& f) { f = f < -1 ? -1 : f > 1 ? 1 : f; };
        clamp1(v.lx); clamp1(v.ly); clamp1(v.rx); clamp1(v.ry);
    }

    // Maus auf dem GamePad-Bild = Touchscreen
    float mx = 0, my = 0;
    SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    float sx = 1, sy = 1;
    if (g_ren) {
        int ww = 0, wh = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(g_win, &ww, &wh);
        SDL_GetRenderOutputSize(g_ren, &ow, &oh);
        if (ww && wh) { sx = (float)ow / ww; sy = (float)oh / wh; }
    }
    mx *= sx; my *= sy;
    if (!g_menu && (mb & SDL_BUTTON_LMASK) && g_drc_rect.w > 0 &&
        mx >= g_drc_rect.x && my >= g_drc_rect.y &&
        mx < g_drc_rect.x + g_drc_rect.w && my < g_drc_rect.y + g_drc_rect.h) {
        v.touch = true;
        v.touch_x = (mx - g_drc_rect.x) / g_drc_rect.w;
        v.touch_y = (my - g_drc_rect.y) / g_drc_rect.h;
    }

    // Weitere Controller = Wiimotes (mit Nunchuk)
    WiimoteState w[4];
    for (int i = 0; i < 4; i++) {
        SDL_Gamepad* gp = (size_t)(i + 1) < g_pads.size() ? g_pads[i + 1] : nullptr;
        if (!gp) continue;
        w[i].connected = true;
        w[i].nunchuk = true;
        auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gp, b); };
        auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(gp, a) / 32767.f; };
        uint32_t b = 0;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= 0x0001;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= 0x0002;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= 0x0004;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= 0x0008;
        if (btn(SDL_GAMEPAD_BUTTON_START)) b |= 0x0010;
        if (btn(SDL_GAMEPAD_BUTTON_NORTH)) b |= 0x0100;   // 2
        if (btn(SDL_GAMEPAD_BUTTON_WEST)) b |= 0x0200;    // 1
        if (axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f) b |= 0x0400;  // B
        if (btn(SDL_GAMEPAD_BUTTON_SOUTH)) b |= 0x0800;   // A
        if (btn(SDL_GAMEPAD_BUTTON_BACK)) b |= 0x1000;
        if (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= 0x4000;        // C (Nunchuk)
        if (axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f) b |= 0x2000;   // Z (Nunchuk)
        if (btn(SDL_GAMEPAD_BUTTON_GUIDE)) b |= 0x8000;
        w[i].buttons = b;
        w[i].pointer_x = axis(SDL_GAMEPAD_AXIS_RIGHTX);
        w[i].pointer_y = axis(SDL_GAMEPAD_AXIS_RIGHTY);
        w[i].nx = axis(SDL_GAMEPAD_AXIS_LEFTX);
        w[i].ny = -axis(SDL_GAMEPAD_AXIS_LEFTY);
    }

    std::lock_guard<std::mutex> lk(g_input_mutex);
    g_vpad = v;
    for (int i = 0; i < 4; i++) g_wii[i] = w[i];
}

// ---------------------------------------------------------------------------
static void draw_text(float x, float y, const char* s) { SDL_RenderDebugText(g_ren, x, y, s); }

static std::string binding_text(const Action& a) {
    std::string k = SDL_GetScancodeName(a.key);
    if (k.empty()) k = "-";
    return k + "  |  " + (a.pad.empty() ? "-" : a.pad);
}

static void draw_menu(int ow, int oh) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, 10, 10, 18, 225);
    SDL_FRect bg{0, 0, (float)ow, (float)oh};
    SDL_RenderFillRect(g_ren, &bg);

    SDL_SetRenderScale(g_ren, 2.f, 2.f);
    SDL_SetRenderDrawColor(g_ren, 255, 255, 255, 255);
    draw_text(16, 12, "STEUERUNG  (F1 schliessen)");
    SDL_SetRenderDrawColor(g_ren, 150, 150, 170, 255);
    draw_text(16, 26, "Hoch/Runter: waehlen   Enter: neu belegen   Entf: loeschen");
    float y = 46;
    int n = (int)g_actions.size() + 1;
    for (int i = 0; i < n; i++, y += 11) {
        bool sel = i == g_menu_sel;
        if (sel) {
            SDL_SetRenderDrawColor(g_ren, 228, 0, 43, 255);
            SDL_FRect r{12, y - 2, (float)ow / 2.f - 24, 11};
            SDL_RenderFillRect(g_ren, &r);
        }
        SDL_SetRenderDrawColor(g_ren, 255, 255, 255, 255);
        char line[256];
        if (i < (int)g_actions.size()) {
            const Action& a = g_actions[i];
            std::string b = (sel && g_waiting) ? "...Taste oder Controller-Knopf druecken..." : binding_text(a);
            snprintf(line, sizeof line, "%-22s %s", a.label, b.c_str());
        } else {
            snprintf(line, sizeof line, "%-22s %s", "Mikrofon", g_mic_enabled ? "AN" : "AUS");
        }
        draw_text(16, y, line);
    }
    SDL_SetRenderScale(g_ren, 1.f, 1.f);
}

static void menu_key(SDL_Scancode sc) {
    int n = (int)g_actions.size() + 1;
    if (g_waiting) {
        if (sc != SDL_SCANCODE_ESCAPE && g_menu_sel < (int)g_actions.size()) g_actions[g_menu_sel].key = sc;
        g_waiting = false;
        save_config();
        return;
    }
    if (sc == SDL_SCANCODE_UP) g_menu_sel = (g_menu_sel + n - 1) % n;
    else if (sc == SDL_SCANCODE_DOWN) g_menu_sel = (g_menu_sel + 1) % n;
    else if (sc == SDL_SCANCODE_RETURN) {
        if (g_menu_sel < (int)g_actions.size()) g_waiting = true;
        else { g_mic_enabled = !g_mic_enabled; save_config(); }
    } else if (sc == SDL_SCANCODE_DELETE && g_menu_sel < (int)g_actions.size()) {
        g_actions[g_menu_sel].key = SDL_SCANCODE_UNKNOWN;
        g_actions[g_menu_sel].pad.clear();
        save_config();
    } else if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F1) {
        g_menu = false;
    }
}

static void menu_pad_button(SDL_GamepadButton b) {
    if (!g_waiting || g_menu_sel >= (int)g_actions.size()) return;
    g_actions[g_menu_sel].pad = std::string("button:") + SDL_GetGamepadStringForButton(b);
    g_waiting = false;
    save_config();
}

static void menu_pad_axis(SDL_GamepadAxis a, int16_t value) {
    if (!g_waiting || g_menu_sel >= (int)g_actions.size() || std::abs((int)value) < 24000) return;
    g_actions[g_menu_sel].pad = std::string("axis:") + SDL_GetGamepadStringForAxis(a) + (value > 0 ? "+" : "-");
    g_waiting = false;
    save_config();
}

// ---------------------------------------------------------------------------
static void upload(int t) {
    std::lock_guard<std::mutex> lk(g_frame_mutex);
    Frame& f = g_frames[t];
    if (!f.w || f.version == g_tex_version[t]) return;
    if (!g_tex[t] || g_tex_w[t] != f.w || g_tex_h[t] != f.h) {
        if (g_tex[t]) SDL_DestroyTexture(g_tex[t]);
        g_tex[t] = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, f.w, f.h);
        g_tex_w[t] = f.w;
        g_tex_h[t] = f.h;
    }
    SDL_UpdateTexture(g_tex[t], nullptr, f.px.data(), f.w * 4);
    g_tex_version[t] = f.version;
}

static SDL_FRect fit(float x, float y, float w, float h, float aspect) {
    float rw = w, rh = w / aspect;
    if (rh > h) { rh = h; rw = h * aspect; }
    return SDL_FRect{x + (w - rw) / 2, y + (h - rh) / 2, rw, rh};
}

static void render() {
    upload(TARGET_TV);
    upload(TARGET_DRC);
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(g_ren, &ow, &oh);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);

    int big = g_drc_big ? TARGET_DRC : TARGET_TV, small = g_drc_big ? TARGET_TV : TARGET_DRC;
    SDL_FRect bigr = fit(0, 0, (float)ow, (float)oh, 16.f / 9.f);
    float sw = ow * 0.28f, sh = sw * 9.f / 16.f;
    SDL_FRect smallr{(float)ow - sw - 12, (float)oh - sh - 12, sw, sh};
    if (g_tex[big]) SDL_RenderTexture(g_ren, g_tex[big], nullptr, &bigr);
    if (g_tex[small]) {
        SDL_SetRenderDrawColor(g_ren, 40, 40, 50, 255);
        SDL_FRect border{smallr.x - 2, smallr.y - 2, smallr.w + 4, smallr.h + 4};
        SDL_RenderFillRect(g_ren, &border);
        SDL_RenderTexture(g_ren, g_tex[small], nullptr, &smallr);
    }
    g_drc_rect = g_drc_big ? bigr : (g_tex[TARGET_DRC] ? smallr : SDL_FRect{0, 0, 0, 0});

    // Statuszeile
    char status[200];
    snprintf(status, sizeof status, "Frames: %llu  Draws: %u  Controller: %zu  Mikro: %s   F1 Steuerung  F2 GamePad gross",
             (unsigned long long)g_stat_frames.load(), g_stat_draws.load(), g_pads.size(),
             g_mic ? "an" : (g_mic_enabled ? "bereit" : "aus"));
    SDL_SetRenderDrawColor(g_ren, 200, 200, 210, 255);
    draw_text(8, 8, status);
    if (!g_tex[TARGET_TV] && !g_tex[TARGET_DRC]) {
        SDL_SetRenderScale(g_ren, 2.f, 2.f);
        draw_text(ow / 4.f - 120, oh / 4.f, "Warte auf das erste Bild vom Spiel...");
        SDL_SetRenderScale(g_ren, 1.f, 1.f);
    }
    if (g_menu) draw_menu(ow, oh);
    SDL_RenderPresent(g_ren);
}

// ---------------------------------------------------------------------------
bool platform_init() {
    if (const char* p = getenv("NL_CONTROLS")) g_config_path = p;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init fehlgeschlagen: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_CreateWindowAndRenderer("Nintendo Land Recomp", 1280, 720, SDL_WINDOW_RESIZABLE, &g_win, &g_ren)) {
        fprintf(stderr, "Fenster fehlgeschlagen: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(g_ren, 1);
    load_config();
    printf("Fenster offen. F1 = Steuerung, F2 = GamePad-Bild gross\n");
    return true;
}

int platform_run(const std::function<int()>& game) {
    std::atomic<int> result{0};
    std::atomic<bool> game_done{false};
    std::thread t([&] { result = game(); game_done = true; });
    t.detach();

    while (!g_quit && !game_done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if (e.key.scancode == SDL_SCANCODE_F1) { g_menu = !g_menu; g_waiting = false; break; }
                if (e.key.scancode == SDL_SCANCODE_F2) { g_drc_big = !g_drc_big; break; }
                if (g_menu) menu_key(e.key.scancode);
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                SDL_Gamepad* gp = SDL_OpenGamepad(e.gdevice.which);
                if (gp) { g_pads.push_back(gp); printf("Controller verbunden: %s\n", SDL_GetGamepadName(gp)); }
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (size_t i = 0; i < g_pads.size(); i++)
                    if (SDL_GetGamepadID(g_pads[i]) == e.gdevice.which) {
                        SDL_CloseGamepad(g_pads[i]);
                        g_pads.erase(g_pads.begin() + i);
                        break;
                    }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (g_menu) {
                    if (g_waiting) menu_pad_button((SDL_GamepadButton)e.gbutton.button);
                    else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) menu_key(SDL_SCANCODE_UP);
                    else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu_key(SDL_SCANCODE_DOWN);
                    else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) menu_key(SDL_SCANCODE_RETURN);
                } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
                    g_menu = true;
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (g_menu) menu_pad_axis((SDL_GamepadAxis)e.gaxis.axis, e.gaxis.value);
                break;
            default: break;
            }
        }
        update_input();
        render();   // VSync begrenzt auf die Bildwiederholrate
    }
    platform_mic_stop();
    SDL_Quit();
    return result;
}

void platform_present(int target, int width, int height, const uint32_t* rgba) {
    if (target < 0 || target > 1 || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lk(g_frame_mutex);
    Frame& f = g_frames[target];
    f.w = width;
    f.h = height;
    f.px.assign(rgba, rgba + (size_t)width * height);
    f.version++;
}

void platform_set_stats(uint64_t frames, uint32_t draws) {
    g_stat_frames = frames;
    g_stat_draws = draws;
}

VpadState platform_vpad() {
    std::lock_guard<std::mutex> lk(g_input_mutex);
    return g_vpad;
}

WiimoteState platform_wiimote(int i) {
    std::lock_guard<std::mutex> lk(g_input_mutex);
    return (i >= 0 && i < 4) ? g_wii[i] : WiimoteState{};
}

bool platform_mic_start() {
    std::lock_guard<std::mutex> lk(g_mic_mutex);
    if (!g_mic_enabled) return false;
    if (g_mic) return true;
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = 32000;
    g_mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (!g_mic) {
        fprintf(stderr, "Mikrofon konnte nicht geoeffnet werden: %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_mic);
    printf("Mikrofon an (32 kHz)\n");
    return true;
}

void platform_mic_stop() {
    std::lock_guard<std::mutex> lk(g_mic_mutex);
    if (g_mic) { SDL_DestroyAudioStream(g_mic); g_mic = nullptr; }
}

size_t platform_mic_read(int16_t* out, size_t max_samples) {
    std::lock_guard<std::mutex> lk(g_mic_mutex);
    if (!g_mic) return 0;
    int got = SDL_GetAudioStreamData(g_mic, out, (int)(max_samples * 2));
    return got > 0 ? (size_t)got / 2 : 0;
}

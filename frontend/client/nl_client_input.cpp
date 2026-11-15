// NL Recomp - Eingaben von NLOnline (siehe nl_client_input.h)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "nl_client_input.h"
#include "nl_hid_wiimote.h"

#include <SDL3/SDL.h>
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>

using namespace nlnet;
using clk = std::chrono::steady_clock;

namespace {
// ---------------------------------------------------------------------------
// Aktionen und Belegung
// ---------------------------------------------------------------------------
enum Act {
    A_A, A_B, A_1, A_2, A_PLUS, A_MINUS, A_HOME, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_C, A_Z,
    A_NUP, A_NDOWN, A_NLEFT, A_NRIGHT, A_SHAKE, A_TILT_L, A_TILT_R, A_RECENTER, A_COUNT
};
struct ActInfo { const char* key; const char* label; uint16_t wii; };
const ActInfo kActs[A_COUNT] = {
    {"a", "A", NLW_A}, {"b", "B (Abzug)", NLW_B}, {"1", "1", NLW_1}, {"2", "2", NLW_2},
    {"plus", "+", NLW_PLUS}, {"minus", "-", NLW_MINUS}, {"home", "Home", NLW_HOME},
    {"up", "Steuerkreuz hoch", NLW_UP}, {"down", "Steuerkreuz runter", NLW_DOWN},
    {"left", "Steuerkreuz links", NLW_LEFT}, {"right", "Steuerkreuz rechts", NLW_RIGHT},
    {"c", "Nunchuk C", NLW_C}, {"z", "Nunchuk Z", NLW_Z},
    {"nup", "Nunchuk-Stick hoch", 0}, {"ndown", "Nunchuk-Stick runter", 0},
    {"nleft", "Nunchuk-Stick links", 0}, {"nright", "Nunchuk-Stick rechts", 0},
    {"shake", "Schuetteln", 0}, {"tiltl", "Wiimote nach links kippen", 0}, {"tiltr", "Wiimote nach rechts kippen", 0},
    {"recenter", "Zeiger/Bewegung zentrieren", 0},
};
// Controller-Code: 0 = nichts, 1..32 = Knopf+1, 100 + Achse*2 (+1 = negative Richtung)
constexpr int kPadAxis = 100;
struct Bind { int key[2] = {0, 0}; int pad = 0; };
Bind s_binds[A_COUNT];

void default_binds() {
    auto set = [](Act a, int k1, int k2, int pad) { s_binds[a] = {{k1, k2}, pad}; };
    set(A_A, VK_LBUTTON, VK_SPACE, 1 + SDL_GAMEPAD_BUTTON_SOUTH);
    set(A_B, VK_RBUTTON, VK_CONTROL, kPadAxis + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER * 2);
    set(A_1, '1', 'K', 1 + SDL_GAMEPAD_BUTTON_EAST);
    set(A_2, '2', 'L', 1 + SDL_GAMEPAD_BUTTON_WEST);
    set(A_PLUS, VK_RETURN, 0, 1 + SDL_GAMEPAD_BUTTON_START);
    set(A_MINUS, VK_BACK, 0, 1 + SDL_GAMEPAD_BUTTON_BACK);
    set(A_HOME, 'H', 0, 1 + SDL_GAMEPAD_BUTTON_GUIDE);
    set(A_UP, VK_UP, 0, 1 + SDL_GAMEPAD_BUTTON_DPAD_UP);
    set(A_DOWN, VK_DOWN, 0, 1 + SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    set(A_LEFT, VK_LEFT, 0, 1 + SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    set(A_RIGHT, VK_RIGHT, 0, 1 + SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    set(A_C, 'Q', 0, 1 + SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    set(A_Z, VK_SHIFT, 0, kPadAxis + SDL_GAMEPAD_AXIS_LEFT_TRIGGER * 2);
    set(A_NUP, 'W', 0, 0);
    set(A_NDOWN, 'S', 0, 0);
    set(A_NLEFT, 'A', 0, 0);
    set(A_NRIGHT, 'D', 0, 0);
    set(A_SHAKE, 'F', VK_MBUTTON, 1 + SDL_GAMEPAD_BUTTON_NORTH);
    set(A_TILT_L, 'Z', 0, 0);
    set(A_TILT_R, 'X', 0, 1 + SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    set(A_RECENTER, 'R', 0, 1 + SDL_GAMEPAD_BUTTON_RIGHT_STICK);
}

// ---------------------------------------------------------------------------
// Einstellungen und Zustand
// ---------------------------------------------------------------------------
std::mutex s_mx;
bool s_sideways = true;
bool s_use_pad_gyro = true;
bool s_use_wiimote = true;
int s_wiimote_index = 0;
int s_acc_x_sign = -1;            // Wiimote-x (wiibrew: nach links) -> SDL-x (nach rechts)
int s_mpls_sign[3] = {1, 1, 1};   // pitch, yaw, roll
bool s_show_pointer = true;
// Sensorleiste mit Gyro nachbauen (wie Dolphin): drehen = links/rechts, neigen = hoch/runter
bool s_gyro_pointer = true;
float s_gyro_yaw_range = 25.0f;     // Grad fuer die ganze Bildbreite
float s_gyro_pitch_range = 20.0f;   // Grad fuer die ganze Bildhoehe
float s_gp_yaw = 0, s_gp_pitch = 0, s_gp_pitch_center = 0;
bool s_gp_init = false;
clk::time_point s_last_mouse_move{};
bool s_hid = true;   // eigener Wiimote-Treiber (nur NLOnline)

SDL_Gamepad* s_pad = nullptr;
std::string s_pad_name;
bool s_pad_gyro = false;

// Zeiger
enum PtrSrc { PTR_NONE, PTR_MOUSE, PTR_STICK, PTR_IR, PTR_GYRO };
PtrSrc s_ptr_src = PTR_NONE;
float s_ptr_u = 0.5f, s_ptr_v = 0.5f;
float s_last_mouse_u = -1, s_last_mouse_v = -1;
clk::time_point s_last_poll = clk::now();
// Bewegung (nachgebaut)
float s_roll = 0;
uint8_t s_recenter = 0;
bool s_recenter_held = false;
// Status fuer die Anzeige
Input s_last_in;
int s_last_source = SRC_KEYBOARD;
NLHidWiimoteState s_wm;
bool s_wm_ok = false;

// Neu belegen
int s_cap_act = -1, s_cap_slot = -1;   // slot 0/1 = Taste, 2 = Controller
bool s_cap_armed = false;              // erst wenn alles losgelassen ist
clk::time_point s_cap_start;

uint32_t now_us() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(clk::now().time_since_epoch()).count();
}

bool key_down(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

float pad_axis(SDL_GamepadAxis a) {
    if (!s_pad) return 0;
    return SDL_GetGamepadAxis(s_pad, a) / 32767.0f;
}

bool pad_code_down(int code) {
    if (!s_pad || code <= 0) return false;
    if (code < kPadAxis) return SDL_GetGamepadButton(s_pad, (SDL_GamepadButton)(code - 1));
    const int axis = (code - kPadAxis) / 2;
    const bool neg = ((code - kPadAxis) & 1) != 0;
    const float v = pad_axis((SDL_GamepadAxis)axis);
    return neg ? v < -0.5f : v > 0.5f;
}

std::string key_name(int vk) {
    switch (vk) {
    case 0: return "-";
    case VK_LBUTTON: return "Linksklick";
    case VK_RBUTTON: return "Rechtsklick";
    case VK_MBUTTON: return "Mittelklick";
    case VK_XBUTTON1: return "Maus 4";
    case VK_XBUTTON2: return "Maus 5";
    case VK_SPACE: return "Leertaste";
    case VK_CONTROL: return "Strg";
    case VK_SHIFT: return "Shift";
    case VK_MENU: return "Alt";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Ruecktaste";
    case VK_TAB: return "Tab";
    case VK_UP: return "Pfeil hoch";
    case VK_DOWN: return "Pfeil runter";
    case VK_LEFT: return "Pfeil links";
    case VK_RIGHT: return "Pfeil rechts";
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t buf[64] = {};
    if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0) {
        char out[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
        return out;
    }
    return "Taste " + std::to_string(vk);
}

std::string pad_name(int code) {
    if (code <= 0) return "-";
    if (code < kPadAxis) {
        static const char* kNames[] = {"Unten (A/Kreuz)", "Rechts (B/Kreis)", "Links (X/Viereck)", "Oben (Y/Dreieck)", "Back/Select",
                                       "Guide/Home", "Start", "Linker Stick drueck.", "Rechter Stick drueck.", "LB/L1", "RB/R1",
                                       "Steuerkreuz hoch", "Steuerkreuz runter", "Steuerkreuz links", "Steuerkreuz rechts", "Misc",
                                       "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
        const int b = code - 1;
        return b < (int)(sizeof(kNames) / sizeof(kNames[0])) ? kNames[b] : "Knopf " + std::to_string(b);
    }
    const int axis = (code - kPadAxis) / 2;
    const bool neg = ((code - kPadAxis) & 1) != 0;
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT/L2";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT/R2";
    case SDL_GAMEPAD_AXIS_LEFTX: return neg ? "Linker Stick links" : "Linker Stick rechts";
    case SDL_GAMEPAD_AXIS_LEFTY: return neg ? "Linker Stick hoch" : "Linker Stick runter";
    case SDL_GAMEPAD_AXIS_RIGHTX: return neg ? "Rechter Stick links" : "Rechter Stick rechts";
    case SDL_GAMEPAD_AXIS_RIGHTY: return neg ? "Rechter Stick hoch" : "Rechter Stick runter";
    }
    return "Achse " + std::to_string(axis);
}

bool act_down(int a, bool allow_mouse_buttons) {
    for (int k : s_binds[a].key) {
        if (!k) continue;
        const bool mouse = k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON || k == VK_XBUTTON1 || k == VK_XBUTTON2;
        if (mouse && !allow_mouse_buttons) continue;
        if (key_down(k)) return true;
    }
    return pad_code_down(s_binds[a].pad);
}

// Neu belegen: auf die naechste Taste / den naechsten Knopf warten
void capture_poll() {
    if (s_cap_act < 0) return;
    if (key_down(VK_ESCAPE)) { s_cap_act = -1; return; }
    bool any = false;
    for (int vk = 1; vk < 255; vk++) any |= key_down(vk);
    if (s_pad) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) any |= SDL_GetGamepadButton(s_pad, (SDL_GamepadButton)b);
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) any |= std::abs(pad_axis((SDL_GamepadAxis)a)) > 0.5f;
    }
    if (!s_cap_armed) {   // erst alles loslassen (der Klick auf "aendern" zaehlt nicht)
        if (!any) s_cap_armed = true;
        return;
    }
    if (s_cap_slot < 2) {
        for (int vk = 1; vk < 255; vk++) {
            if (vk == VK_ESCAPE || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
                vk == VK_RMENU)
                continue;   // links/rechts-Varianten: wir nehmen die allgemeine Taste
            if (key_down(vk)) {
                s_binds[s_cap_act].key[s_cap_slot] = vk;
                s_cap_act = -1;
                return;
            }
        }
    } else if (s_pad) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(s_pad, (SDL_GamepadButton)b)) {
                s_binds[s_cap_act].pad = 1 + b;
                s_cap_act = -1;
                return;
            }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
            const float v = pad_axis((SDL_GamepadAxis)a);
            if (std::abs(v) > 0.6f) {
                s_binds[s_cap_act].pad = kPadAxis + a * 2 + (v < 0 ? 1 : 0);
                s_cap_act = -1;
                return;
            }
        }
    }
}

std::string buttons_text(uint16_t b) {
    static const struct { uint16_t bit; const char* n; } k[] = {
        {NLW_A, "A"}, {NLW_B, "B"}, {NLW_1, "1"}, {NLW_2, "2"}, {NLW_PLUS, "+"}, {NLW_MINUS, "-"}, {NLW_HOME, "Home"},
        {NLW_UP, "Hoch"}, {NLW_DOWN, "Runter"}, {NLW_LEFT, "Links"}, {NLW_RIGHT, "Rechts"}, {NLW_C, "C"}, {NLW_Z, "Z"}};
    std::string s;
    for (auto& e : k)
        if (b & e.bit) s += std::string(s.empty() ? "" : " ") + e.n;
    return s.empty() ? "-" : s;
}
}  // namespace

// ===========================================================================
void nlin_init(bool with_hid) {
    default_binds();
    s_hid = with_hid;
    if (with_hid) nl_hidwii_start();
}

void nlin_shutdown() {
    if (s_hid) nl_hidwii_stop();
}

void nlin_set_gamepad(SDL_Gamepad* pad, const std::string& name) {
    std::scoped_lock l(s_mx);
    s_pad = pad;
    s_pad_name = name;
    s_pad_gyro = false;
    if (pad && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
        s_pad_gyro = true;
    }
}

bool nlin_sideways() { std::scoped_lock l(s_mx); return s_sideways; }
void nlin_set_sideways(bool on) { std::scoped_lock l(s_mx); s_sideways = on; }
bool nlin_capturing() { return s_cap_act >= 0; }

bool nlin_pointer(float& u, float& v, bool& from_wiimote) {
    std::scoped_lock l(s_mx);
    if (s_ptr_src == PTR_NONE || !s_show_pointer) return false;
    u = s_ptr_u;
    v = s_ptr_v;
    from_wiimote = s_ptr_src == PTR_IR;
    return true;
}

Input nlin_poll(const NLInFrame& f) {
    std::scoped_lock l(s_mx);
    capture_poll();
    const auto now = clk::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - s_last_poll).count(), 0.0f, 0.1f);
    s_last_poll = now;

    Input in;
    in.flags = s_sideways ? IN_SIDEWAYS : 0;
    // echte Wiimote
    s_wm_ok = s_hid && s_use_wiimote && nl_hidwii_get(s_wiimote_index, s_wm);
    if (!s_wm_ok && s_hid && s_use_wiimote) s_wm_ok = nl_hidwii_get(0, s_wm);
    if (s_wm_ok) {
        in.battery = (uint8_t)std::clamp(s_wm.battery, 0, 100);
        in.ext = (uint8_t)((s_wm.nunchuk ? 1 : 0) | (s_wm.motion_plus ? 2 : 0));
    }
    if (!f.playing || f.blocked || s_cap_act >= 0) {
        s_last_in = in;
        return in;   // nichts ans Spiel
    }
    const bool keys = f.focused;
    const bool mouse_btn = keys && f.mouse_on_tv;   // Mausknoepfe nur ueber dem TV-Bild
    auto down = [&](int a) { return (keys && act_down(a, mouse_btn)) || (!keys && pad_code_down(s_binds[a].pad)); };

    // ---- Knoepfe
    uint16_t wb = 0;
    for (int a = 0; a < A_COUNT; a++)
        if (kActs[a].wii && down(a)) wb |= kActs[a].wii;
    if (s_wm_ok) wb |= s_wm.buttons;

    // ---- Nunchuk-Stick: Controller-Stick, Tasten, echter Nunchuk
    float nx = pad_axis(SDL_GAMEPAD_AXIS_LEFTX), ny = -pad_axis(SDL_GAMEPAD_AXIS_LEFTY);
    if (std::abs(nx) < 0.15f) nx = 0;
    if (std::abs(ny) < 0.15f) ny = 0;
    if (down(A_NLEFT)) nx = -1;
    if (down(A_NRIGHT)) nx = 1;
    if (down(A_NUP)) ny = 1;
    if (down(A_NDOWN)) ny = -1;
    if (s_wm_ok && s_wm.nunchuk && (std::abs(s_wm.nx) > 0.1f || std::abs(s_wm.ny) > 0.1f)) { nx = s_wm.nx; ny = s_wm.ny; }
    // Wiimote quer: Steuerkreuz gedreht (Bild-hoch = Wiimote-rechts), Stick lenkt mit
    if (s_sideways && !s_wm_ok) {
        const bool up = (wb & NLW_UP) || ny > 0.5f, dn = (wb & NLW_DOWN) || ny < -0.5f;
        const bool lf = (wb & NLW_LEFT) || nx < -0.5f, rt = (wb & NLW_RIGHT) || nx > 0.5f;
        wb &= (uint16_t)~(NLW_UP | NLW_DOWN | NLW_LEFT | NLW_RIGHT);
        if (up) wb |= NLW_RIGHT;
        if (dn) wb |= NLW_LEFT;
        if (lf) wb |= NLW_UP;
        if (rt) wb |= NLW_DOWN;
    }
    in.wbuttons = wb;
    in.nx = (int16_t)std::clamp((int)(nx * 32767), -32767, 32767);
    in.ny = (int16_t)std::clamp((int)(ny * 32767), -32767, 32767);

    // ---- Zeiger: Kamera der Wiimote > Maus auf dem TV-Bild > rechter Stick
    const float rx = pad_axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = pad_axis(SDL_GAMEPAD_AXIS_RIGHTY);
    const bool stick_moved = std::abs(rx) > 0.2f || std::abs(ry) > 0.2f;
    const bool mouse_moved = f.mouse_on_tv && (std::abs(f.mouse_u - s_last_mouse_u) > 0.002f || std::abs(f.mouse_v - s_last_mouse_v) > 0.002f);
    if (f.mouse_on_tv) { s_last_mouse_u = f.mouse_u; s_last_mouse_v = f.mouse_v; }
    const bool recenter = down(A_RECENTER);
    if (recenter && !s_recenter_held) {
        s_recenter++;
        if (s_ptr_src == PTR_STICK) s_ptr_u = s_ptr_v = 0.5f;
    }
    s_recenter_held = recenter;
    if (s_wm_ok && s_wm.ir_seen) {
        s_ptr_src = PTR_IR;
        s_ptr_u = s_wm.px;
        s_ptr_v = s_wm.py;
    } else if (keys && mouse_moved) {
        s_ptr_src = PTR_MOUSE;
    } else if (stick_moved) {
        if (s_ptr_src != PTR_STICK) { s_ptr_src = PTR_STICK; }
        s_ptr_u = std::clamp(s_ptr_u + rx * 0.9f * dt, 0.0f, 1.0f);
        s_ptr_v = std::clamp(s_ptr_v + ry * 0.9f * dt, 0.0f, 1.0f);
    } else if (s_ptr_src == PTR_IR) {
        s_ptr_src = PTR_NONE;   // Wiimote zeigt weg von der Sensorleiste
    }
    if (s_ptr_src == PTR_MOUSE) {
        if (f.mouse_on_tv) { s_ptr_u = f.mouse_u; s_ptr_v = f.mouse_v; }
        else s_ptr_src = PTR_NONE;   // Maus neben dem Bild = Zeiger weg
    }
    if (mouse_moved && keys) s_last_mouse_move = now;
    const bool recenter_edge = s_recenter != s_last_in.recenter;

    // ---- Bewegung
    in.recenter = s_recenter;
    in.sensor_us = now_us();
    if (s_wm_ok) {
        // echte Wiimote: Wiimote-Achsen (x seitlich, y vorne, z oben) -> SDL (x rechts, y oben, z zum Spieler)
        in.source = SRC_WIIMOTE;
        in.flags |= IN_IMU_REAL;
        in.acc[0] = s_acc_x_sign * s_wm.acc[0] * 9.81f;
        in.acc[1] = s_wm.acc[2] * 9.81f;
        in.acc[2] = -s_wm.acc[1] * 9.81f;
        if (s_wm.gyro_valid) {
            const float d2r = 3.14159265f / 180.0f;
            in.gyro[0] = s_mpls_sign[0] * s_wm.pitch_rate * d2r;    // Spitze hoch
            in.gyro[1] = s_mpls_sign[1] * s_wm.yaw_rate * d2r;      // nach links drehen
            in.gyro[2] = -s_mpls_sign[2] * s_wm.roll_rate * d2r;    // SDL z: nach links kippen
        }
    } else if (s_pad && s_pad_gyro && s_use_pad_gyro) {
        in.source = SRC_CONTROLLER;
        in.flags |= IN_IMU_REAL;
        SDL_GetGamepadSensorData(s_pad, SDL_SENSOR_ACCEL, in.acc, 3);
        SDL_GetGamepadSensorData(s_pad, SDL_SENSOR_GYRO, in.gyro, 3);
    } else {
        // nachgebaut: Wiimote zeigt dahin, wo der Zeiger ist; Tasten kippen; Schuetteln
        in.source = (s_pad && (stick_moved || std::abs(nx) > 0.2f)) ? SRC_CONTROLLER : SRC_KEYBOARD;
        const float target_roll = down(A_TILT_L) ? -50.0f : down(A_TILT_R) ? 50.0f : 0.0f;
        s_roll += (target_roll - s_roll) * std::min(1.0f, dt * 10.0f);
        const float u = s_ptr_src == PTR_NONE ? 0.5f : s_ptr_u, v = s_ptr_src == PTR_NONE ? 0.5f : s_ptr_v;
        in.yaw = (int16_t)((u - 0.5f) * 60.0f * 100);
        in.pitch = (int16_t)((0.5f - v) * 40.0f * 100);
        in.roll = (int16_t)(s_roll * 100);
        in.shake = down(A_SHAKE) ? 255 : 0;
    }
    if (s_pad && in.source == SRC_KEYBOARD && (std::abs(nx) > 0.2f || stick_moved)) in.source = SRC_CONTROLLER;

    // ---- Sensorleiste mit Gyro nachbauen (echte Wiimote mit MotionPlus ohne Sensorleiste, Gyro-Controller)
    const bool has_gyro = (in.flags & IN_IMU_REAL) && ((s_wm_ok && s_wm.gyro_valid) || (!s_wm_ok && s_pad_gyro));
    if (s_gyro_pointer && has_gyro && !(s_wm_ok && s_wm.ir_seen)) {
        // "oben" im Geraet aus der Schwerkraft (SDL-Achsen: x rechts, y oben, z zum Spieler)
        float up[3] = {in.acc[0], in.acc[1], in.acc[2]};
        const float len = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (len > 1.0f) {
            for (float& c : up) c /= len;
            const float r2d = 57.2957795f;
            // links/rechts drehen = Drehung um die Senkrechte (unabhaengig davon, wie man die Wiimote kippt)
            const float yaw_rate = (in.gyro[0] * up[0] + in.gyro[1] * up[1] + in.gyro[2] * up[2]) * r2d;   // + = nach links
            // hoch/runter: Drehung um die waagrechte Querachse, dazu langsam an die Schwerkraft angleichen (kein Wegdriften)
            float right[3] = {up[1], -up[0], 0.0f};   // Kreuzprodukt vorne(0,0,-1) x oben
            const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1]);
            if (rl > 0.1f) { right[0] /= rl; right[1] /= rl; }
            const float pitch_rate = (in.gyro[0] * right[0] + in.gyro[1] * right[1]) * r2d;   // + = Spitze hoch
            const float acc_pitch = std::asin(std::clamp(-up[2], -1.0f, 1.0f)) * r2d;
            if (!s_gp_init || recenter_edge) {
                s_gp_init = true;
                s_gp_yaw = 0;
                s_gp_pitch = acc_pitch;
                s_gp_pitch_center = acc_pitch;
            }
            s_gp_yaw += yaw_rate * dt;
            s_gp_pitch = (s_gp_pitch + pitch_rate * dt) * 0.98f + acc_pitch * 0.02f;
            // nicht ewig weit raus zaehlen: am Rand festhalten (wie ein Zeiger am Bildschirmrand)
            const float ylim = s_gyro_yaw_range * 0.75f;
            s_gp_yaw = std::clamp(s_gp_yaw, -ylim, ylim);
            const bool mouse_recent = now - s_last_mouse_move < std::chrono::seconds(1);
            if (!mouse_recent && !stick_moved) {
                s_ptr_src = PTR_GYRO;
                s_ptr_u = std::clamp(0.5f - s_gp_yaw / s_gyro_yaw_range, 0.0f, 1.0f);
                s_ptr_v = std::clamp(0.5f - (s_gp_pitch - s_gp_pitch_center) / s_gyro_pitch_range, 0.0f, 1.0f);
            }
        }
    } else {
        s_gp_init = false;
        if (s_ptr_src == PTR_GYRO) s_ptr_src = PTR_NONE;
    }

    if (s_ptr_src != PTR_NONE) {
        in.flags |= IN_POINTER;
        if (s_ptr_src == PTR_IR) in.flags |= IN_PTR_IR;
        in.px = (uint16_t)std::clamp((int)(s_ptr_u * 65535), 0, 65535);
        in.py = (uint16_t)std::clamp((int)(s_ptr_v * 65535), 0, 65535);
    }
    s_last_source = in.source;
    s_last_in = in;
    return in;
}

// ===========================================================================
// Einstellungen
// ===========================================================================
bool nlin_load_kv(const std::string& k, const std::string& v) {
    std::scoped_lock l(s_mx);
    if (k == "sideways") { s_sideways = v != "0"; return true; }
    if (k == "pad_gyro") { s_use_pad_gyro = v != "0"; return true; }
    if (k == "use_wiimote") { s_use_wiimote = v != "0"; return true; }
    if (k == "wiimote_index") { s_wiimote_index = std::clamp(atoi(v.c_str()), 0, 3); return true; }
    if (k == "show_pointer") { s_show_pointer = v != "0"; return true; }
    if (k == "gyro_pointer") { s_gyro_pointer = v != "0"; return true; }
    if (k == "gyro_yaw_range") { s_gyro_yaw_range = std::clamp((float)atof(v.c_str()), 5.0f, 90.0f); return true; }
    if (k == "gyro_pitch_range") { s_gyro_pitch_range = std::clamp((float)atof(v.c_str()), 5.0f, 90.0f); return true; }
    if (k == "wiimote_acc_x") { s_acc_x_sign = atoi(v.c_str()) < 0 ? -1 : 1; return true; }
    if (k == "mpls_signs") {
        int p = 1, y = 1, r = 1;
        if (sscanf(v.c_str(), "%d,%d,%d", &p, &y, &r) == 3) {
            s_mpls_sign[0] = p < 0 ? -1 : 1; s_mpls_sign[1] = y < 0 ? -1 : 1; s_mpls_sign[2] = r < 0 ? -1 : 1;
        }
        return true;
    }
    if (k.rfind("bind_", 0) == 0) {
        const std::string name = k.substr(5);
        for (int a = 0; a < A_COUNT; a++)
            if (name == kActs[a].key) {
                int k1 = 0, k2 = 0, p = 0;
                if (sscanf(v.c_str(), "%d,%d,%d", &k1, &k2, &p) == 3) s_binds[a] = {{k1, k2}, p};
            }
        return true;
    }
    return false;
}

void nlin_save(std::ostream& out) {
    std::scoped_lock l(s_mx);
    out << "sideways=" << (s_sideways ? 1 : 0) << "\npad_gyro=" << (s_use_pad_gyro ? 1 : 0) << "\nuse_wiimote=" << (s_use_wiimote ? 1 : 0)
        << "\nwiimote_index=" << s_wiimote_index << "\nshow_pointer=" << (s_show_pointer ? 1 : 0) << "\ngyro_pointer=" << (s_gyro_pointer ? 1 : 0)
        << "\ngyro_yaw_range=" << s_gyro_yaw_range << "\ngyro_pitch_range=" << s_gyro_pitch_range << "\nwiimote_acc_x=" << s_acc_x_sign
        << "\nmpls_signs=" << s_mpls_sign[0] << "," << s_mpls_sign[1] << "," << s_mpls_sign[2] << "\n";
    for (int a = 0; a < A_COUNT; a++)
        out << "bind_" << kActs[a].key << "=" << s_binds[a].key[0] << "," << s_binds[a].key[1] << "," << s_binds[a].pad << "\n";
}

// ===========================================================================
// Steuerungs-Seite
// ===========================================================================
bool nlin_ui(ImFont* big) {
    bool changed = false;
    const ImVec4 accent(0.0f, 0.68f, 0.94f, 1);
    const ImVec4 ok(0.20f, 0.75f, 0.35f, 1), bad(0.90f, 0.35f, 0.30f, 1), dim(0.55f, 0.60f, 0.66f, 1);
    auto yes_no = [&](bool v, const char* y, const char* n) { ImGui::TextColored(v ? ok : bad, "%s", v ? y : n); };

    if (big) ImGui::PushFont(big);
    ImGui::TextColored(accent, "Steuerung");
    if (big) ImGui::PopFont();
    ImGui::TextDisabled("Du spielst als Wiimote (mit Nunchuk). Hier stellst du ein, womit.");
    ImGui::Separator();

    // ---------------------------------------------------------------- echte Wiimotes
    ImGui::TextColored(accent, "Echte Wiimotes (Bluetooth)");
    if (!s_hid) {
        ImGui::TextWrapped("Im Spiel werden echte Wiimotes automatisch als eigene Spieler eingehaengt (Bluetooth koppeln reicht). "
                           "Ob sie gesehen werden, Zeiger und Akku: im Menue unter Steuerung > Wiimotes.");
        ImGui::Spacing();
    }
    const auto list = s_hid ? nl_hidwii_list() : std::vector<NLHidWiimoteState>{};
    if (s_hid) {
    {
        std::scoped_lock l(s_mx);
        if (ImGui::Checkbox("Echte Wiimote benutzen, wenn eine verbunden ist", &s_use_wiimote)) changed = true;
    }
    if (list.empty()) {
        ImGui::TextColored(dim, "Keine Wiimote verbunden.");
    } else {
        ImGui::Text("%d verbunden", (int)list.size());
        if (ImGui::BeginTable("##wiimotes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Wiimote");
            ImGui::TableSetupColumn("Akku");
            ImGui::TableSetupColumn("Nunchuk");
            ImGui::TableSetupColumn("MotionPlus");
            ImGui::TableSetupColumn("Sensorleiste");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)list.size(); i++) {
                const auto& w = list[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", w.name.c_str());
                ImGui::TableNextColumn();
                if (w.battery >= 0) ImGui::TextColored(w.battery <= 15 ? bad : ok, "%d %%", w.battery);
                else ImGui::TextDisabled("?");
                ImGui::TableNextColumn();
                yes_no(w.nunchuk, "ja", "nein");
                ImGui::TableNextColumn();
                yes_no(w.motion_plus, "ja", "nein");
                ImGui::TableNextColumn();
                if (w.ir_seen) ImGui::TextColored(ok, "gesehen (%d Punkte)", w.ir_dots);
                else ImGui::TextColored(bad, "nicht gesehen");
                ImGui::TableNextColumn();
                std::scoped_lock l(s_mx);
                const bool active = s_use_wiimote && s_wiimote_index == i;
                ImGui::PushID(i);
                if (active) ImGui::TextColored(accent, "in Benutzung");
                else if (ImGui::SmallButton("Diese benutzen")) { s_wiimote_index = i; s_use_wiimote = true; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::TreeNode("So verbindest du eine Wiimote")) {
        ImGui::TextWrapped("1. Windows-Einstellungen > Bluetooth und Geraete > Geraet hinzufuegen > Bluetooth.\n"
                           "2. An der Wiimote 1 und 2 gleichzeitig druecken (oder den roten Sync-Knopf im Batteriefach). "
                           "Die Lampen blinken.\n"
                           "3. 'Nintendo RVL-CNT-01' auswaehlen. Fragt Windows nach einer PIN: einfach leer lassen und weiter.\n"
                           "4. Sie taucht hier nach ein paar Sekunden auf. Danach reicht ein Knopfdruck an der Wiimote.\n\n"
                           "Am einfachsten geht es mit einer DolphinBar (Modus 4) - die ist auch gleich die Sensorleiste.\n"
                           "Fuer den Zeiger braucht die Wiimote eine Sensorleiste (oder zwei Kerzen) ueber oder unter dem Bildschirm. "
                           "Ohne Sensorleiste zielst du mit der Maus.");
        ImGui::TreePop();
    }
    }   // s_hid
    ImGui::Spacing();

    // ---------------------------------------------------------------- Controller / Maus
    ImGui::TextColored(accent, "Controller, Maus und Tastatur");
    {
        std::scoped_lock l(s_mx);
        if (s_pad) {
            ImGui::Text("Controller: %s", s_pad_name.c_str());
            if (s_pad_gyro) {
                if (ImGui::Checkbox("Gyro des Controllers als Wiimote-Bewegung", &s_use_pad_gyro)) changed = true;
            } else {
                ImGui::TextColored(dim, "Dieser Controller hat keinen Bewegungssensor - Bewegung kommt vom Zeiger.");
            }
        } else {
            ImGui::TextColored(dim, "Kein Controller. Xbox, PlayStation, Switch Pro ... einfach einstecken.");
        }
        if (ImGui::Checkbox("Wiimote quer halten (Steuerkreuz gedreht, z.B. Mario Chase)", &s_sideways)) changed = true;
        if (ImGui::Checkbox("Meinen Zeiger anzeigen", &s_show_pointer)) changed = true;
        if (ImGui::Checkbox("Sensorleiste mit Gyro nachbauen (wie Dolphin)", &s_gyro_pointer)) changed = true;
        ImGui::TextColored(dim, "Echte Wiimote mit MotionPlus ohne Sensorleiste oder Controller mit Gyro: drehen = Zeiger links/rechts, "
                                "neigen = hoch/runter. Zeigt er daneben: auf die Bildmitte zielen und 'Zentrieren' druecken.");
        if (s_gyro_pointer) {
            ImGui::SetNextItemWidth(320);
            if (ImGui::SliderFloat("Drehen fuer die ganze Breite", &s_gyro_yaw_range, 5.0f, 90.0f, "%.0f Grad")) changed = true;
            ImGui::SetNextItemWidth(320);
            if (ImGui::SliderFloat("Neigen fuer die ganze Hoehe", &s_gyro_pitch_range, 5.0f, 90.0f, "%.0f Grad")) changed = true;
        }
    }
    ImGui::TextColored(dim, "Ohne echte Sensoren dreht sich die Wiimote dahin, wo dein Zeiger ist: Maus schnell bewegen = Schwung "
                            "(z.B. das Schwert in Battle Quest). Kippen und Schuetteln gehen ueber Tasten.");
    ImGui::Spacing();

    // ---------------------------------------------------------------- Live-Test
    ImGui::TextColored(accent, "Live-Test");
    {
        std::scoped_lock l(s_mx);
        static const char* kSrc[] = {"Maus + Tastatur", "Controller", "echte Wiimote"};
        ImGui::Text("Spielt mit: %s", kSrc[std::clamp(s_last_source, 0, 2)]);
        ImGui::Text("Knoepfe: %s", buttons_text(s_last_in.wbuttons).c_str());
        ImGui::Text("Nunchuk-Stick: %.2f / %.2f", s_last_in.nx / 32767.0f, s_last_in.ny / 32767.0f);
        if (s_last_in.flags & IN_POINTER)
            ImGui::Text("Zeiger: %.0f %% / %.0f %%  (%s)", s_last_in.px / 655.35f, s_last_in.py / 655.35f,
                        (s_last_in.flags & IN_PTR_IR) ? "Wiimote-Kamera" : s_ptr_src == PTR_GYRO ? "Gyro (nachgebaute Sensorleiste)"
                        : s_ptr_src == PTR_STICK ? "Stick" : "Maus");
        else ImGui::TextColored(dim, "Zeiger: keiner (Maus aufs TV-Bild, rechter Stick oder Wiimote auf die Sensorleiste)");
        if (s_last_in.flags & IN_IMU_REAL)
            ImGui::Text("Bewegung: echte Sensoren  -  Gyro %.2f %.2f %.2f rad/s", s_last_in.gyro[0], s_last_in.gyro[1], s_last_in.gyro[2]);
        else
            ImGui::Text("Bewegung: nachgebaut  -  drehen %.0f, neigen %.0f, kippen %.0f Grad%s", s_last_in.yaw / 100.0f,
                        s_last_in.pitch / 100.0f, s_last_in.roll / 100.0f, s_last_in.shake ? ", schuetteln" : "");
    }
    ImGui::Spacing();

    // ---------------------------------------------------------------- Belegung
    ImGui::TextColored(accent, "Belegung");
    ImGui::TextColored(dim, "Klick auf ein Feld und druecke die neue Taste / den Knopf. Rechtsklick loescht. Esc bricht ab.");
    if (ImGui::BeginTable("##binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Wiimote", 0, 1.4f);
        ImGui::TableSetupColumn("Taste 1", 0, 1.0f);
        ImGui::TableSetupColumn("Taste 2", 0, 1.0f);
        ImGui::TableSetupColumn("Controller", 0, 1.2f);
        ImGui::TableHeadersRow();
        std::scoped_lock l(s_mx);
        for (int a = 0; a < A_COUNT; a++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", kActs[a].label);
            for (int slot = 0; slot < 3; slot++) {
                ImGui::TableNextColumn();
                ImGui::PushID(a * 4 + slot);
                const bool cap = s_cap_act == a && s_cap_slot == slot;
                std::string txt = cap ? (slot == 2 ? "Knopf druecken ..." : "Taste druecken ...")
                                      : slot == 2 ? pad_name(s_binds[a].pad) : key_name(s_binds[a].key[slot]);
                if (ImGui::Button(txt.c_str(), ImVec2(-1, 0)) && !cap) {
                    s_cap_act = a;
                    s_cap_slot = slot;
                    s_cap_armed = false;
                    s_cap_start = clk::now();
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    if (slot == 2) s_binds[a].pad = 0;
                    else s_binds[a].key[slot] = 0;
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    {
        std::scoped_lock l(s_mx);
        static int s_prev_cap = -1;
        if (s_prev_cap >= 0 && s_cap_act < 0) changed = true;   // gerade fertig belegt
        s_prev_cap = s_cap_act;
    }
    if (ImGui::Button("Alles auf Standard")) {
        std::scoped_lock l(s_mx);
        default_binds();
        changed = true;
    }
    if (ImGui::TreeNode("Erweitert: Achsen der echten Wiimote")) {
        ImGui::TextWrapped("Falls sich beim Kippen oder Drehen mit einer echten Wiimote etwas falsch herum bewegt, hier umdrehen.");
        std::scoped_lock l(s_mx);
        bool x = s_acc_x_sign > 0, p = s_mpls_sign[0] < 0, y = s_mpls_sign[1] < 0, r = s_mpls_sign[2] < 0;
        if (ImGui::Checkbox("Kippen links/rechts umdrehen", &x)) { s_acc_x_sign = x ? 1 : -1; changed = true; }
        if (ImGui::Checkbox("MotionPlus hoch/runter umdrehen", &p)) { s_mpls_sign[0] = p ? -1 : 1; changed = true; }
        if (ImGui::Checkbox("MotionPlus links/rechts drehen umdrehen", &y)) { s_mpls_sign[1] = y ? -1 : 1; changed = true; }
        if (ImGui::Checkbox("MotionPlus kippen umdrehen", &r)) { s_mpls_sign[2] = r ? -1 : 1; changed = true; }
        ImGui::TreePop();
    }
    return changed;
}

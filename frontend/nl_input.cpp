// Nintendo Land Recomp - Eingabe.
//
// Ein eigener "Controller" (NLInputController) fasst Tastatur und Controller zusammen.
// Cemus GamePad-Emulation (VPAD) erlaubt pro Aktion nur EINE Quelle; deshalb zeigt
// jede Aktion auf einen virtuellen Knopf unseres Controllers, und wir entscheiden selbst,
// welche Tasten/Knoepfe ihn ausloesen: 2 Tasten + 1 Controller-Knopf pro Aktion.
#include "nl_app.h"
#include "nl_screens.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include "WindowSystem.h"
#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/VPADController.h"

#include <Windows.h>
#include <fstream>
#include <map>
#include <set>

const NLAction kNLActions[] = {
    {"A", "A", "Bestaetigen, springen, angreifen - je nach Attraktion.", VPADController::kButtonId_A, false},
    {"B", "B", "Zurueck, abbrechen.", VPADController::kButtonId_B, false},
    {"X", "X", "X-Knopf des GamePads.", VPADController::kButtonId_X, false},
    {"Y", "Y", "Y-Knopf des GamePads.", VPADController::kButtonId_Y, false},
    {"L", "L", "Linke Schultertaste.", VPADController::kButtonId_L, false},
    {"R", "R", "Rechte Schultertaste.", VPADController::kButtonId_R, false},
    {"ZL", "ZL", "Linker Trigger.", VPADController::kButtonId_ZL, false},
    {"ZR", "ZR", "Rechter Trigger.", VPADController::kButtonId_ZR, false},
    {"PLUS", "Plus (+)", "Pause in den Attraktionen.", VPADController::kButtonId_Plus, false},
    {"MINUS", "Minus (-)", "Minus-Knopf des GamePads.", VPADController::kButtonId_Minus, false},
    {"UP", "Steuerkreuz hoch", "", VPADController::kButtonId_Up, false},
    {"DOWN", "Steuerkreuz runter", "", VPADController::kButtonId_Down, false},
    {"LEFT", "Steuerkreuz links", "", VPADController::kButtonId_Left, false},
    {"RIGHT", "Steuerkreuz rechts", "", VPADController::kButtonId_Right, false},
    {"LS_UP", "Linker Stick hoch", "Laufen.", VPADController::kButtonId_StickL_Up, true},
    {"LS_DOWN", "Linker Stick runter", "Laufen.", VPADController::kButtonId_StickL_Down, true},
    {"LS_LEFT", "Linker Stick links", "Laufen.", VPADController::kButtonId_StickL_Left, true},
    {"LS_RIGHT", "Linker Stick rechts", "Laufen.", VPADController::kButtonId_StickL_Right, true},
    {"RS_UP", "Rechter Stick hoch", "Kamera, zielen.", VPADController::kButtonId_StickR_Up, true},
    {"RS_DOWN", "Rechter Stick runter", "Kamera, zielen.", VPADController::kButtonId_StickR_Down, true},
    {"RS_LEFT", "Rechter Stick links", "Kamera, zielen.", VPADController::kButtonId_StickR_Left, true},
    {"RS_RIGHT", "Rechter Stick rechts", "Kamera, zielen.", VPADController::kButtonId_StickR_Right, true},
    {"LS_CLICK", "Linken Stick druecken", "", VPADController::kButtonId_StickL, false},
    {"RS_CLICK", "Rechten Stick druecken", "", VPADController::kButtonId_StickR, false},
    {"MIC", "Mikrofon (halten)", "Solange gedrueckt, hoert das Spiel dein Mikrofon - z. B. zum Pusten.", VPADController::kButtonId_Mic, false},
    {"SCREEN", "TV/GamePad-Bild wechseln", "Schaltet zwischen TV-Bild und GamePad-Bild um. Nintendo Land zeigt wichtige Sachen oft auf dem GamePad.", VPADController::kButtonId_Screen, false},
    {"HOME", "Home", "Home-Knopf des GamePads.", VPADController::kButtonId_Home, false},
};
const int kNLActionCount = (int)(sizeof(kNLActions) / sizeof(kNLActions[0]));

// SDL-Gamepad: Knopf-Nummern (wie Cemus SDLController) und Achsen-Codes aus Controller.h
enum PadBtn { P_SOUTH = 0, P_EAST, P_WEST, P_NORTH, P_BACK, P_GUIDE, P_START, P_LSTICK, P_RSTICK, P_LB, P_RB,
              P_UP, P_DOWN, P_LEFT, P_RIGHT, P_MISC, P_PADDLE1, P_PADDLE2, P_PADDLE3, P_PADDLE4, P_TOUCHPAD };

static NLBinding s_bind[64];
static int action_index(const std::string& name);
static std::filesystem::path s_path;
static std::shared_ptr<ControllerBase> s_gamepad;
static std::mutex s_pad_mutex;
static NLPadSnapshot s_snap;
static std::atomic<bool> s_menu_btn_edge{false};
static bool s_menu_btn_last = false;

// Virtuelles Neigen des GamePads: 0..1 wie Cemus Maus-Gyro (0.5 = gerade)
extern bool (*g_nl_virtual_gyro)(float& wx, float& wy);
static std::atomic<float> s_gyro_wx{0.5f}, s_gyro_wy{0.5f};
bool nl_pad_has_gyro() {
    std::scoped_lock l(s_pad_mutex);
    return s_gamepad && s_gamepad->is_connected() && s_gamepad->has_motion();
}
bool nl_pad_gyro_active() { return g_nl.pad_gyro && nl_pad_has_gyro(); }

static bool gyro_hook(float& wx, float& wy) {
    if (!g_nl.free_camera || g_nl_menu_open) return false;
    wx = s_gyro_wx;
    wy = s_gyro_wy;
    return true;
}

// Maus (vom Fenster-Thread geschrieben)
static std::atomic<int> s_mouse_dx{0}, s_mouse_dy{0};
static std::atomic<bool> s_touch_down{false};
static std::atomic<int> s_touch_x{0}, s_touch_y{0};

void nl_mouse_add_delta(int dx, int dy) { s_mouse_dx += dx; s_mouse_dy += dy; }
void nl_mouse_touch(bool down, int x, int y) { s_touch_x = x; s_touch_y = y; s_touch_down = down; }
std::atomic<bool> g_nl_mouse_captured{false};
bool nl_mouse_pointer_mode() {
    return !g_nl.mouse_camera || !g_nl_mouse_captured || g_nl_menu_open || nl_key_down(VK_MENU) ||
           !WindowSystem::GetWindowInfo().app_active;
}

NLPadType nl_pad_type() {
    std::string n = nl_gamepad_name();
    if (n.empty()) return NLPadType::None;
    for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
    auto has = [&](const char* t) { return n.find(t) != std::string::npos; };
    if (has("dualsense") || has("ps5") || has("ps4") || has("ps3") || has("dualshock") || has("playstation")) return NLPadType::PlayStation;
    if (has("xbox") || has("xinput")) return NLPadType::Xbox;
    if (has("nintendo") || has("switch") || has("pro controller") || has("joy-con") || has("wii u")) return NLPadType::Nintendo;
    return NLPadType::Other;
}

bool nl_a_is_bottom() {
    if (g_nl.button_layout == 1) return false;
    if (g_nl.button_layout == 2) return true;
    return nl_pad_type() != NLPadType::Nintendo;   // automatisch: Xbox/PlayStation = A unten
}

static NLBinding default_binding(int i) {
    const bool xbox = nl_a_is_bottom();
    static const std::map<std::string, std::tuple<uint32_t, uint32_t, int>> d = {
        // Aktion        Taste 1     Taste 2      Controller
        {"A", {VK_RETURN, 'E', -100}}, {"B", {VK_BACK, VK_LCONTROL, -101}},
        {"X", {VK_SPACE, 0, -102}}, {"Y", {'F', 0, -103}},
        {"L", {'Q', 0, P_LB}}, {"R", {'R', 0, P_RB}},
        {"ZL", {'1', 0, kTriggerXP}}, {"ZR", {'3', 0, kTriggerYP}},
        {"PLUS", {'P', 0, P_START}}, {"MINUS", {'M', 0, P_BACK}},
        {"UP", {VK_UP, 0, P_UP}}, {"DOWN", {VK_DOWN, 0, P_DOWN}},
        {"LEFT", {VK_LEFT, 0, P_LEFT}}, {"RIGHT", {VK_RIGHT, 0, P_RIGHT}},
        {"LS_UP", {'W', 0, kAxisYN}}, {"LS_DOWN", {'S', 0, kAxisYP}},
        {"LS_LEFT", {'A', 0, kAxisXN}}, {"LS_RIGHT", {'D', 0, kAxisXP}},
        {"RS_UP", {'I', 0, kRotationYN}}, {"RS_DOWN", {'K', 0, kRotationYP}},
        {"RS_LEFT", {'J', 0, kRotationXN}}, {"RS_RIGHT", {'L', 0, kRotationXP}},
        {"LS_CLICK", {'C', 0, P_LSTICK}}, {"RS_CLICK", {'V', 0, P_RSTICK}},
        {"MIC", {'T', 0, P_MISC}}, {"SCREEN", {VK_TAB, 0, -1}}, {"HOME", {'H', 0, -1}},
    };
    NLBinding b;
    auto it = d.find(kNLActions[i].name);
    if (it == d.end()) return b;
    auto [k1, k2, p] = it->second;
    // Gesichtstasten je nach Layout: Nintendo = A rechts, B unten, X oben, Y links
    if (p <= -100) {
        const int nin[] = {P_EAST, P_SOUTH, P_NORTH, P_WEST};
        const int xb[] = {P_SOUTH, P_EAST, P_WEST, P_NORTH};
        p = (xbox ? xb : nin)[-p - 100];
    }
    b.key1 = k1;
    b.key2 = k2;
    b.pad = p;
    return b;
}

NLBinding& nl_binding(int action) { return s_bind[action]; }

// Gesichtstasten an das Knopf-Layout anpassen (A/B/X/Y), ohne eigene Aenderungen zu ueberschreiben:
// nur Aktionen, die noch die Standardbelegung des ANDEREN Layouts haben, werden umgestellt.
static void fix_face_buttons() {
    const bool bottom = nl_a_is_bottom();
    const int nin[] = {P_EAST, P_SOUTH, P_NORTH, P_WEST};
    const int xb[] = {P_SOUTH, P_EAST, P_WEST, P_NORTH};
    const char* names[] = {"A", "B", "X", "Y"};
    bool changed = false;
    for (int k = 0; k < 4; k++) {
        int i = action_index(names[k]);
        if (i < 0) continue;
        int want = (bottom ? xb : nin)[k], other = (bottom ? nin : xb)[k];
        if (s_bind[i].pad == other) { s_bind[i].pad = want; changed = true; }
    }
    if (changed) nl_input_save();
}

void nl_input_reset_defaults() {
    for (int i = 0; i < kNLActionCount; i++) s_bind[i] = default_binding(i);
}

static int action_index(const std::string& name) {
    for (int i = 0; i < kNLActionCount; i++)
        if (name == kNLActions[i].name) return i;
    return -1;
}

void nl_input_save() {
    std::ofstream f(s_path);
    f << "# Nintendo Land Steuerung (im Spiel: Esc -> Steuerung)\n";
    for (int i = 0; i < kNLActionCount; i++)
        f << kNLActions[i].name << "=" << s_bind[i].key1 << "," << s_bind[i].key2 << "," << s_bind[i].pad << "\n";
}

static void load_bindings() {
    nl_input_reset_defaults();
    std::ifstream f(s_path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        int i = action_index(line.substr(0, eq));
        if (i < 0) continue;
        unsigned k1 = 0, k2 = 0;
        int p = -1;
        if (sscanf(line.c_str() + eq + 1, "%u,%u,%d", &k1, &k2, &p) == 3) s_bind[i] = {k1, k2, p};
    }
}

// ---------------------------------------------------------------------------
// Namen fuer die Anzeige
// ---------------------------------------------------------------------------
std::string nl_key_name(uint32_t vk) {
    if (!vk) return "-";
    switch (vk) {
    case VK_LBUTTON: return "Linke Maustaste";
    case VK_RBUTTON: return "Rechte Maustaste";
    case VK_MBUTTON: return "Mittlere Maustaste";
    case VK_LCONTROL: return "Strg links";
    case VK_RCONTROL: return "Strg rechts";
    case VK_LSHIFT: return "Shift links";
    case VK_RSHIFT: return "Shift rechts";
    case VK_SPACE: return "Leertaste";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Ruecktaste";
    case VK_TAB: return "Tab";
    case VK_UP: return "Pfeil hoch";
    case VK_DOWN: return "Pfeil runter";
    case VK_LEFT: return "Pfeil links";
    case VK_RIGHT: return "Pfeil rechts";
    }
    LONG sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC_EX) << 16;
    char name[64];
    if (GetKeyNameTextA(sc, name, sizeof(name)) > 0) return name;
    return "Taste " + std::to_string(vk);
}

std::string nl_pad_name(int code) {
    const NLPadType t = nl_pad_type();
    const bool ps = t == NLPadType::PlayStation, xb = t == NLPadType::Xbox, nin = t == NLPadType::Nintendo;
    switch (code) {
    case -1: return "-";
    case P_SOUTH: return ps ? "Kreuz" : xb ? "A" : nin ? "B (unten)" : "Unten-Knopf";
    case P_EAST: return ps ? "Kreis" : xb ? "B" : nin ? "A (rechts)" : "Rechts-Knopf";
    case P_WEST: return ps ? "Viereck" : xb ? "X" : nin ? "Y (links)" : "Links-Knopf";
    case P_NORTH: return ps ? "Dreieck" : xb ? "Y" : nin ? "X (oben)" : "Oben-Knopf";
    case P_BACK: return ps ? "Create" : xb ? "View" : nin ? "Minus" : "Back";
    case P_GUIDE: return ps ? "PS-Taste" : xb ? "Xbox-Taste" : "Home";
    case P_START: return ps ? "Options" : xb ? "Menu" : nin ? "Plus" : "Start";
    case P_LSTICK: return ps ? "L3" : "L-Stick druecken";
    case P_RSTICK: return ps ? "R3" : "R-Stick druecken";
    case P_LB: return ps ? "L1" : xb ? "LB" : "L";
    case P_RB: return ps ? "R1" : xb ? "RB" : "R";
    case P_UP: return "Steuerkreuz hoch";
    case P_DOWN: return "Steuerkreuz runter";
    case P_LEFT: return "Steuerkreuz links";
    case P_RIGHT: return "Steuerkreuz rechts";
    case P_MISC: return ps ? "Mikrofon-Taste" : xb ? "Share" : "Capture";
    case P_PADDLE1: case P_PADDLE2: case P_PADDLE3: case P_PADDLE4: return "Paddle " + std::to_string(code - P_PADDLE1 + 1);
    case P_TOUCHPAD: return "Touchpad";
    case kAxisXP: return "L-Stick rechts";
    case kAxisXN: return "L-Stick links";
    case kAxisYP: return "L-Stick runter";
    case kAxisYN: return "L-Stick hoch";
    case kRotationXP: return "R-Stick rechts";
    case kRotationXN: return "R-Stick links";
    case kRotationYP: return "R-Stick runter";
    case kRotationYN: return "R-Stick hoch";
    case kTriggerXP: return ps ? "L2" : xb ? "LT" : "ZL";
    case kTriggerYP: return ps ? "R2" : xb ? "RT" : "ZR";
    }
    return "Knopf " + std::to_string(code);
}

std::string nl_gamepad_name() {
    std::scoped_lock l(s_pad_mutex);
    return s_gamepad ? s_gamepad->display_name() : std::string();
}

// ---------------------------------------------------------------------------
// Unser zusammengefasster Controller
// ---------------------------------------------------------------------------
static float pad_value(const ControllerState& st, int code) {
    if (code < 0) return 0.0f;
    if (code <= kButtonNoneAxisMAX) return st.buttons.GetButtonState((uint32)code) ? 1.0f : 0.0f;
    switch (code) {
    case kAxisXP: return std::max(0.0f, st.axis.x);
    case kAxisXN: return std::max(0.0f, -st.axis.x);
    case kAxisYP: return std::max(0.0f, st.axis.y);
    case kAxisYN: return std::max(0.0f, -st.axis.y);
    case kRotationXP: return std::max(0.0f, st.rotation.x);
    case kRotationXN: return std::max(0.0f, -st.rotation.x);
    case kRotationYP: return std::max(0.0f, st.rotation.y);
    case kRotationYN: return std::max(0.0f, -st.rotation.y);
    case kTriggerXP: return std::max(0.0f, st.trigger.x);
    case kTriggerYP: return std::max(0.0f, st.trigger.y);
    }
    return 0.0f;
}

// Wo ist das GamePad-Bild im Hauptfenster?
static bool drc_rect_now(int* r) {
    if (WindowSystem::IsPadWindowOpen()) return false;   // eigenes GamePad-Fenster: Cemu-Weg
    if (g_nl_screens.mode != 0) {
        for (int k = 0; k < 4; k++) r[k] = g_nl_screens.drc_rect[k];
        return r[2] > 0 && r[3] > 0;
    }
    if (!LatteGPUState.isDRCPrimary) return false;   // nur TV sichtbar
    int w = 0, h = 0;
    WindowSystem::GetWindowPhysSize(w, h);
    int iw = w, ih = w * 9 / 16;
    if (ih > h) { ih = h; iw = h * 16 / 9; }
    r[0] = (w - iw) / 2; r[1] = (h - ih) / 2; r[2] = iw; r[3] = ih;
    return iw > 0;
}

bool nl_point_in_drc(int x, int y) {
    int r[4];
    return drc_rect_now(r) && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
}

class NLInputController : public ControllerBase {
public:
    NLInputController() : ControllerBase("nl_input", "Nintendo Land") {
        set_axis_settings(AxisSetting(0.0f));       // Totzone macht schon der echte Controller
        set_rotation_settings(AxisSetting(0.0f));
        set_trigger_settings(AxisSetting(0.0f));
        set_use_motion(true);   // Bewegungsdaten werden genutzt, sobald has_motion() ja sagt
    }
    std::string_view api_name() const override { return "NintendoLand"; }
    InputAPI::Type api() const override { return InputAPI::Keyboard; }
    bool is_connected() override { return true; }
    std::string get_button_name(uint64 button) const override { return "NL " + std::to_string(button); }

    // Echter Gyro vom Controller (PS5/PS4/Switch Pro ...) -> GamePad-Neigen
    bool has_motion() override { return nl_pad_gyro_active(); }
    MotionSample get_motion_sample() override {
        std::scoped_lock l(s_pad_mutex);
        return s_gamepad ? s_gamepad->get_motion_sample() : MotionSample{};
    }

    // Touchscreen: Maus-Klick auf die GamePad-Flaeche im Fenster
    bool has_position() override {
        if (g_nl_menu_open || !s_touch_down || !nl_mouse_pointer_mode()) return false;
        int r[4];
        if (!drc_rect(r)) return false;
        int x = s_touch_x, y = s_touch_y;
        return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
    }
    glm::vec2 get_position() override {
        int r[4];
        if (!drc_rect(r)) return {};
        return {std::clamp((s_touch_x - r[0]) / (float)r[2], 0.0f, 1.0f), std::clamp((s_touch_y - r[1]) / (float)r[3], 0.0f, 1.0f)};
    }
    glm::vec2 get_prev_position() override { return get_position(); }

    static bool drc_rect(int* r) { return drc_rect_now(r); }

    ControllerState raw_state() override {
        ControllerState pad{};
        {
            std::scoped_lock l(s_pad_mutex);
            if (s_gamepad) {
                pad = ControllerState(s_gamepad->update_state());
                NLPadSnapshot s;
                s.connected = true;
                for (uint32 b : pad.buttons.GetButtonList())
                    if (b < 64) s.buttons |= 1ull << b;
                s.lx = pad.axis.x; s.ly = pad.axis.y;
                s.rx = pad.rotation.x; s.ry = pad.rotation.y;
                s.lt = pad.trigger.x; s.rt = pad.trigger.y;
                s_snap = s;
                bool menu = (s.buttons >> P_GUIDE) & 1;
                if (menu && !s_menu_btn_last) s_menu_btn_edge = true;
                s_menu_btn_last = menu;
            } else {
                s_snap = {};
            }
        }
        ControllerState out{};
        if (g_nl_menu_open) return out;   // Menue offen: Spiel bekommt keine Eingaben

        float v[64] = {};
        for (int i = 0; i < kNLActionCount; i++) {
            const auto& b = s_bind[i];
            float x = 0.0f;
            if ((b.key1 && nl_key_down(b.key1)) || (b.key2 && nl_key_down(b.key2))) x = 1.0f;
            x = std::max(x, pad_value(pad, b.pad));
            v[i] = x;
        }
        auto val = [&](const char* name) { int i = action_index(name); return i >= 0 ? v[i] : 0.0f; };
        for (int i = 0; i < kNLActionCount; i++)
            if (!kNLActions[i].stick) out.buttons.SetButtonState(kButton0 + i, v[i] > 0.5f);
        out.axis = {val("LS_RIGHT") - val("LS_LEFT"), val("LS_UP") - val("LS_DOWN")};
        out.rotation = {val("RS_RIGHT") - val("RS_LEFT"), val("RS_UP") - val("RS_DOWN")};
        // Maus-Kamera: Bewegung seit der letzten Abfrage wirkt wie der rechte Stick
        int dx = s_mouse_dx.exchange(0), dy = s_mouse_dy.exchange(0);
        const bool mouse_active = g_nl.mouse_camera && !nl_mouse_pointer_mode();
        if (g_nl.free_camera) {
            // Freie Kamera: Maus und rechter Stick neigen das virtuelle GamePad (hoch/runter/links/rechts)
            float wx = s_gyro_wx, wy = s_gyro_wy;
            const float inv = g_nl.mouse_invert_y ? -1.0f : 1.0f;
            if (mouse_active) {
                const float km = 0.0006f * g_nl.mouse_sens / 100.0f;
                wx += dx * km;
                wy += dy * km * inv;
            }
            const float ks = 0.010f * g_nl.stick_camera_speed / 100.0f;
            wx += out.rotation.x * ks;
            wy -= out.rotation.y * ks * inv;
            // zentrieren: Z, mittlere Maustaste oder Touchpad/Share am Controller
            if (nl_key_down('Z') || nl_key_down(VK_MBUTTON) || (pad.buttons.GetButtonState(P_TOUCHPAD))) wx = wy = 0.5f;
            s_gyro_wx = std::clamp(wx, 0.02f, 0.98f);
            s_gyro_wy = std::clamp(wy, 0.10f, 0.90f);
        }
        if (mouse_active) {
            const float k = 0.30f * g_nl.mouse_sens / 100.0f;
            const float ky = g_nl.mouse_invert_y ? -k : k;
            out.rotation.x = std::clamp(out.rotation.x + dx * k, -1.0f, 1.0f);
            if (!g_nl.free_camera) out.rotation.y = std::clamp(out.rotation.y - dy * ky, -1.0f, 1.0f);
        }
        return out;
    }
};

static std::shared_ptr<NLInputController> s_nl_controller;

static void apply_mapping() {
    auto& im = InputManager::instance();
    auto vpad = std::static_pointer_cast<VPADController>(im.set_controller(0, EmulatedController::Type::VPAD));
    vpad->set_screen_toggle(true);   // Bildwechsel schaltet um statt nur beim Halten
    vpad->add_controller(s_nl_controller);
    for (int i = 0; i < kNLActionCount; i++) {
        const auto& a = kNLActions[i];
        uint64 code = kButton0 + i;
        if (a.stick) {
            std::string n = a.name;
            if (n == "LS_UP") code = kAxisYP;
            else if (n == "LS_DOWN") code = kAxisYN;
            else if (n == "LS_LEFT") code = kAxisXN;
            else if (n == "LS_RIGHT") code = kAxisXP;
            else if (n == "RS_UP") code = kRotationYP;
            else if (n == "RS_DOWN") code = kRotationYN;
            else if (n == "RS_LEFT") code = kRotationXN;
            else if (n == "RS_RIGHT") code = kRotationXP;
        }
        vpad->set_mapping(a.id, s_nl_controller, code);
    }
}

void nl_input_init(const std::filesystem::path& controls_path) {
    s_path = controls_path;
    load_bindings();
    s_nl_controller = std::make_shared<NLInputController>();
    g_nl_virtual_gyro = gyro_hook;
    apply_mapping();
}

// Controller ueber SDL (Xbox, PlayStation, Switch Pro, ...). Erster = GamePad, weitere = Wiimotes.
static std::set<std::string> s_assigned;
void nl_input_hotplug() {
    auto& im = InputManager::instance();
    static int s_native_tick = 0;
    if (++s_native_tick % 4 == 1) nl_wii_update_native();   // echte Wiimotes alle ~2 s
    auto provider = im.get_api_provider(InputAPI::SDLController);
    if (!provider) return;
    for (auto& pad : provider->get_controllers()) {
        std::string id = pad->uuid() + "|" + pad->display_name();
        if (s_assigned.count(id)) continue;
        s_assigned.insert(id);
        bool as_gamepad = false;
        {
            std::scoped_lock l(s_pad_mutex);
            if (!s_gamepad) {
                s_gamepad = pad;
                as_gamepad = true;
            }
        }
        float dz = g_nl.deadzone / 100.0f;
        pad->set_axis_settings(ControllerBase::AxisSetting(dz));
        pad->set_rotation_settings(ControllerBase::AxisSetting(dz));
        if (as_gamepad) {
            pad->connect();   // SDL-Gamepad oeffnen (macht Cemu sonst beim Einhaengen ins GamePad)
            fix_face_buttons();
            cemuLog_log(LogType::Force, "Nintendo Land: Controller als GamePad: {}", pad->display_name());
        } else if (nl_wii_free_slots() > 0) {
            pad->connect();   // wie beim GamePad: SDL-Pad wirklich oeffnen
            std::weak_ptr<ControllerBase> weak = pad;
            nl_wii_attach(pad->display_name(), false, [weak]() {
                NLWiiIn in;
                auto p = weak.lock();
                if (!p || !p->is_connected()) { in.connected = false; return in; }
                ControllerState st = p->update_state();
                for (uint32 b : st.buttons.GetButtonList())
                    if (b < 32) in.buttons |= 1u << b;
                in.lx = st.axis.x; in.ly = st.axis.y;
                in.rx = st.rotation.x; in.ry = st.rotation.y;
                in.lt = st.trigger.x; in.rt = st.trigger.y;
                in.sideways = g_nl.wiimote_sideways;
                return in;
            });
        }
    }
    // Totzone live aktualisieren, Verbindung pruefen (z. B. Kabel kurz raus)
    std::scoped_lock l(s_pad_mutex);
    if (s_gamepad) {
        if (!s_gamepad->is_connected()) s_gamepad->connect();
        float dz = g_nl.deadzone / 100.0f;
        s_gamepad->set_axis_settings(ControllerBase::AxisSetting(dz));
        s_gamepad->set_rotation_settings(ControllerBase::AxisSetting(dz));
    }
}

NLPadSnapshot nl_pad_snapshot() {
    std::scoped_lock l(s_pad_mutex);
    return s_snap;
}

bool nl_pad_menu_pressed() { return s_menu_btn_edge.exchange(false); }

int nl_pad_capture(const NLPadSnapshot& rest) {
    NLPadSnapshot s = nl_pad_snapshot();
    if (!s.connected) return -1;
    uint64_t pressed = s.buttons & ~rest.buttons;
    for (int b = 0; b <= P_TOUCHPAD; b++)
        if (b != P_GUIDE && ((pressed >> b) & 1)) return b;
    auto axis = [](float v, float r, int pos, int neg) { return v - r > 0.6f ? pos : (v - r < -0.6f ? neg : -1); };
    int c;
    if ((c = axis(s.lx, rest.lx, kAxisXP, kAxisXN)) >= 0) return c;
    if ((c = axis(s.ly, rest.ly, kAxisYP, kAxisYN)) >= 0) return c;
    if ((c = axis(s.rx, rest.rx, kRotationXP, kRotationXN)) >= 0) return c;
    if ((c = axis(s.ry, rest.ry, kRotationYP, kRotationYN)) >= 0) return c;
    if (s.lt - rest.lt > 0.5f) return kTriggerXP;
    if (s.rt - rest.rt > 0.5f) return kTriggerYP;
    return -1;
}

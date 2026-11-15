// Nintendo Land Recomp - Wiimote-Spieler (2-5) aus normalen Controllern.
// Cemu hat eine Standard-Belegung nur fuer echte Wiimotes. Hier bekommt jeder Controller
// (lokal per SDL oder ueber das Netz von einem Freund) eine feste Wiimote+Nunchuk-Belegung:
//
//   Unten (A/Kreuz) = A und 2      Rechts (B/Kreis) = 1      Links (X/Viereck) = B
//   Oben (Y/Dreieck) = C           LB = C   LT = Z           RB/RT = B
//   Start = +   Back = -   Guide = Home
//   Linker Stick = Nunchuk-Stick (und quer gehalten: Steuerkreuz)
//   Steuerkreuz = Wiimote-Steuerkreuz (quer gehalten gedreht, wie in Mario Chase/Luigi's Mansion)
//   Rechter Stick = Zeiger aufs Bild, Klick = Zeiger zur Mitte
#include "nl_app.h"

#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/WiimoteController.h"
#include "input/motion/MotionSample.h"
#include "util/math/quaternion.h"
#if HAS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

#include <chrono>
#include <mutex>

namespace {
enum WiiAct { W_A, W_B, W_1, W_2, W_PLUS, W_MINUS, W_HOME, W_UP, W_DOWN, W_LEFT, W_RIGHT, W_Z, W_C, W_COUNT };
enum PadBit { B_SOUTH = 0, B_EAST, B_WEST, B_NORTH, B_BACK, B_GUIDE, B_START, B_LSTICK, B_RSTICK, B_LB, B_RB, B_UP, B_DOWN, B_LEFT, B_RIGHT };

class NLWiiPad : public ControllerBase {
public:
    NLWiiPad(int slot, NLWiiSource src)
        : ControllerBase("nl_wii_" + std::to_string(slot), "Wiimote " + std::to_string(slot)), m_src(std::move(src)) {
        set_axis_settings(AxisSetting(0.0f));
        set_rotation_settings(AxisSetting(0.0f));
        set_trigger_settings(AxisSetting(0.0f));
        set_use_motion(true);   // Ausrichtung aus dem Zeiger (siehe get_motion_sample)
        m_last = std::chrono::steady_clock::now();
    }
    std::string_view api_name() const override { return "NintendoLand"; }
    InputAPI::Type api() const override { return InputAPI::Keyboard; }
    bool is_connected() override { return true; }
    std::string get_button_name(uint64 button) const override { return "NL " + std::to_string(button); }

    bool has_position() override { return true; }
    glm::vec2 get_position() override { std::scoped_lock l(m_mx); return m_ptr; }
    glm::vec2 get_prev_position() override { std::scoped_lock l(m_mx); return m_prev; }
    PositionVisibility GetPositionVisibility() override { return PositionVisibility::FULL; }

    // Nintendo Land rechnet den Wiimote-Zeiger mit MotionPlus aus der Ausrichtung der Wiimote.
    // Deshalb "dreht" der Zeiger (Maus des Gastes / rechter Stick) die Wiimote mit: nach oben zeigen =
    // nach oben neigen, nach links zeigen = nach links drehen. Muster wie Cemus Maus-Gyro beim GamePad.
    bool has_motion() override { return true; }
    MotionSample get_motion_sample() override {
        std::scoped_lock l(m_mx);
        const float rotX = (m_ptr.y * 2.0f - 1.0f) * 25.0f;    // Grad, hoch/runter
        const float rotY = (m_ptr.x * 2.0f - 1.0f) * -35.0f;   // Grad, links/rechts
        Quaternion<float> q(rotX, rotY, 0.0f);
        float quat[4] = {q.w, q.x, q.y, q.z};
        const float pitch = rotX * 3.14159265f / 180.0f;
        float acc[3] = {0.0f, std::sin(pitch), std::cos(pitch)};   // Schwerkraft, ruhig gehalten
        float gyro[3] = {std::clamp((rotX - m_last_rot[0]) / 360.0f, -1.0f, 1.0f),
                         std::clamp((rotY - m_last_rot[1]) / 360.0f * 15.0f, -1.0f, 1.0f), 0.0f};
        float orient[3] = {rotX / 360.0f, rotY / 360.0f, 0.0f};
        m_last_rot[0] = rotX;
        m_last_rot[1] = rotY;
        return MotionSample(acc, 0.0f, gyro, orient, quat);
    }

    ControllerState raw_state() override {
        NLWiiIn in = m_src ? m_src() : NLWiiIn{};
        ControllerState out{};
        if (!in.connected || g_nl_menu_open) return out;
        auto b = [&](int bit) { return ((in.buttons >> bit) & 1) != 0; };
        bool act[W_COUNT] = {};
        act[W_A] = b(B_SOUTH);
        act[W_2] = in.sideways && b(B_SOUTH);
        act[W_1] = b(B_EAST);
        act[W_B] = b(B_WEST) || b(B_RB) || in.rt > 0.5f;
        act[W_C] = b(B_NORTH) || b(B_LB);
        act[W_Z] = in.lt > 0.5f;
        act[W_PLUS] = b(B_START);
        act[W_MINUS] = b(B_BACK);
        act[W_HOME] = b(B_GUIDE);
        bool up = b(B_UP), down = b(B_DOWN), left = b(B_LEFT), right = b(B_RIGHT);
        if (in.sideways) {
            // Wiimote quer (Steuerkreuz links): Bild-hoch = Wiimote-rechts usw.
            up = up || in.ly < -0.5f;
            down = down || in.ly > 0.5f;
            left = left || in.lx < -0.5f;
            right = right || in.lx > 0.5f;
            act[W_RIGHT] = up;
            act[W_LEFT] = down;
            act[W_UP] = left;
            act[W_DOWN] = right;
        } else {
            act[W_UP] = up;
            act[W_DOWN] = down;
            act[W_LEFT] = left;
            act[W_RIGHT] = right;
        }
        for (int i = 0; i < W_COUNT; i++) out.buttons.SetButtonState(kButton0 + i, act[i]);
        out.axis = {in.lx, in.ly};

        // Zeiger mit dem rechten Stick
        auto now = std::chrono::steady_clock::now();
        float dt = std::min(0.1f, std::chrono::duration<float>(now - m_last).count());
        m_last = now;
        auto dz = [](float v) { return std::abs(v) < 0.18f ? 0.0f : v; };
        std::scoped_lock l(m_mx);
        m_prev = m_ptr;
        if (in.has_ptr) {   // Maus des Online-Gastes: Position direkt uebernehmen
            m_ptr = {std::clamp(in.px, 0.0f, 1.0f), std::clamp(in.py, 0.0f, 1.0f)};
            return out;
        }
        if (b(B_RSTICK)) m_ptr = {0.5f, 0.5f};
        m_ptr.x = std::clamp(m_ptr.x + dz(in.rx) * 0.9f * dt, 0.0f, 1.0f);
        m_ptr.y = std::clamp(m_ptr.y + dz(in.ry) * 0.9f * dt, 0.0f, 1.0f);
        return out;
    }

private:
    NLWiiSource m_src;
    std::mutex m_mx;
    glm::vec2 m_ptr{0.5f, 0.5f}, m_prev{0.5f, 0.5f};
    float m_last_rot[2] = {0, 0};
    std::chrono::steady_clock::time_point m_last;
};

struct Slot {
    bool used = false;
    bool remote = false;
    std::string name;
    std::shared_ptr<NLWiiPad> pad;
    std::shared_ptr<ControllerBase> native;   // echte Wiimote (Bluetooth)
    WPADDeviceType type = kWAPDevCore;
};
std::mutex s_mx;
Slot s_slots[5];   // 1-4 benutzt
}  // namespace

int nl_wii_attach(const std::string& name, bool remote, NLWiiSource src) {
    std::scoped_lock l(s_mx);
    for (int i = 1; i <= 4; i++) {
        if (s_slots[i].used) continue;
        auto pad = std::make_shared<NLWiiPad>(i, std::move(src));
        auto emu = InputManager::instance().set_controller(i, EmulatedController::Type::Wiimote);
        auto wm = std::dynamic_pointer_cast<WiimoteController>(emu);
        if (!wm) return -1;
        wm->add_controller(pad);
        const uint64 ids[W_COUNT] = {WiimoteController::kButtonId_A, WiimoteController::kButtonId_B, WiimoteController::kButtonId_1,
                                     WiimoteController::kButtonId_2, WiimoteController::kButtonId_Plus, WiimoteController::kButtonId_Minus,
                                     WiimoteController::kButtonId_Home, WiimoteController::kButtonId_Up, WiimoteController::kButtonId_Down,
                                     WiimoteController::kButtonId_Left, WiimoteController::kButtonId_Right,
                                     WiimoteController::kButtonId_Nunchuck_Z, WiimoteController::kButtonId_Nunchuck_C};
        for (int a = 0; a < W_COUNT; a++) wm->set_mapping(ids[a], pad, kButton0 + a);
        wm->set_mapping(WiimoteController::kButtonId_Nunchuck_Up, pad, kAxisYN);
        wm->set_mapping(WiimoteController::kButtonId_Nunchuck_Down, pad, kAxisYP);
        wm->set_mapping(WiimoteController::kButtonId_Nunchuck_Left, pad, kAxisXN);
        wm->set_mapping(WiimoteController::kButtonId_Nunchuck_Right, pad, kAxisXP);
        wm->set_device_type(kWAPDevMPLSFreeStyle);   // Wiimote + MotionPlus + Nunchuk
        s_slots[i] = {true, remote, name, pad};
        cemuLog_log(LogType::Force, "Nintendo Land: {} ist Wiimote {}{}", name, i, remote ? " (online)" : "");
        return i;
    }
    return -1;
}

void nl_wii_detach(int slot) {
    std::scoped_lock l(s_mx);
    if (slot < 1 || slot > 4 || !s_slots[slot].used) return;
    InputManager::instance().delete_controller(slot);
    s_slots[slot] = {};
}

NLWiiSlotInfo nl_wii_slot(int slot) {
    std::scoped_lock l(s_mx);
    if (slot < 1 || slot > 4) return {};
    return {s_slots[slot].used, s_slots[slot].remote, s_slots[slot].name};
}

int nl_wii_free_slots() {
    std::scoped_lock l(s_mx);
    int n = 0;
    for (int i = 1; i <= 4; i++) n += !s_slots[i].used;
    return n;
}

// ===========================================================================
// Echte Wiimotes (Bluetooth, ueber Cemus HID-Anbindung)
// Koppeln: Windows-Bluetooth -> "Geraet hinzufuegen" -> 1+2 an der Wiimote druecken,
// oder eine DolphinBar im Modus 4. Fuer den Zeiger braucht man eine Sensorleiste.
// ===========================================================================
#if HAS_WIIMOTE
static WPADDeviceType native_type(const std::shared_ptr<ControllerBase>& c) {
    auto n = std::static_pointer_cast<NativeWiimoteController>(c);
    const bool mpls = n->is_mpls_attached();
    switch (n->get_extension()) {
    case NativeWiimoteController::Nunchuck: return mpls ? kWAPDevMPLSFreeStyle : kWAPDevFreestyle;
    case NativeWiimoteController::Classic: return mpls ? kWAPDevMPLSClassic : kWAPDevClassic;
    default: return mpls ? kWAPDevMPLS : kWAPDevCore;
    }
}
#endif

void nl_wii_update_native() {
#if HAS_WIIMOTE
    auto& im = InputManager::instance();
    auto provider = im.get_api_provider(InputAPI::Wiimote);
    if (!provider) return;
    std::scoped_lock l(s_mx);
    // getrennte Wiimotes freigeben, Erweiterung (Nunchuk/MotionPlus) nachfuehren
    for (int i = 1; i <= 4; i++) {
        Slot& sl = s_slots[i];
        if (!sl.used || !sl.native) continue;
        if (!sl.native->is_connected()) {
            cemuLog_log(LogType::Force, "NL: echte Wiimote {} getrennt", i);
            InputManager::instance().delete_controller(i);
            sl = {};
            continue;
        }
        const WPADDeviceType t = native_type(sl.native);
        if (t != sl.type) {
            if (auto wm = std::dynamic_pointer_cast<WiimoteController>(im.get_controller(i))) wm->set_device_type(t);
            sl.type = t;
        }
    }
    // neue Wiimotes einhaengen
    for (auto& c : provider->get_controllers()) {
        if (!c->is_connected()) continue;
        bool known = false;
        for (int i = 1; i <= 4; i++) known |= s_slots[i].native && s_slots[i].native->uuid() == c->uuid();
        if (known) continue;
        for (int i = 1; i <= 4; i++) {
            if (s_slots[i].used) continue;
            auto wm = std::dynamic_pointer_cast<WiimoteController>(im.set_controller(i, EmulatedController::Type::Wiimote));
            if (!wm) break;
            wm->add_controller(c);
            wm->set_default_mapping(c);
            const WPADDeviceType t = native_type(c);
            wm->set_device_type(t);
            s_slots[i] = {true, false, "Wiimote (Bluetooth)", nullptr, c, t};
            cemuLog_log(LogType::Force, "NL: echte Wiimote ist Spieler {}", i);
            break;
        }
    }
#endif
}

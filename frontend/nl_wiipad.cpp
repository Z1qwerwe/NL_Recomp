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
// Online-Gaeste schicken fertige Wiimote-Eingaben (eigene Belegung in NLOnline).
#include "nl_app.h"

#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/emulated/WiimoteController.h"
#include "input/motion/MotionSample.h"
#include "input/motion/MotionHandler.h"
#include "net/nl_proto.h"
#include "util/math/quaternion.h"
#if HAS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace {
enum WiiAct { W_A, W_B, W_1, W_2, W_PLUS, W_MINUS, W_HOME, W_UP, W_DOWN, W_LEFT, W_RIGHT, W_Z, W_C, W_COUNT };
enum PadBit { B_SOUTH = 0, B_EAST, B_WEST, B_NORTH, B_BACK, B_GUIDE, B_START, B_LSTICK, B_RSTICK, B_LB, B_RB, B_UP, B_DOWN, B_LEFT, B_RIGHT };

// ---------------------------------------------------------------------------
// Kleine Quaternion-Rechnung fuer die nachgebaute Bewegung
// Achsen wie in Cemus Bewegungs-Filter (Mahony): Wiimote/Controller-Koerper x rechts, y unten, z vorne;
// Welt X rechts, Y hinten, Z unten. q dreht Koerper -> Welt.
// ---------------------------------------------------------------------------
struct Q {
    float w = 1, x = 0, y = 0, z = 0;
    Q operator*(const Q& b) const {
        return {w * b.w - x * b.x - y * b.y - z * b.z, w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x, w * b.z + x * b.y - y * b.x + z * b.w};
    }
    Q conj() const { return {w, -x, -y, -z}; }
    static Q axis(float ax, float ay, float az, float rad) {
        const float s = std::sin(rad / 2);
        return {std::cos(rad / 2), ax * s, ay * s, az * s};
    }
    void rotate(const float v[3], float out[3]) const {   // q * v * q^-1
        Q p{0, v[0], v[1], v[2]};
        Q r = (*this) * p * conj();
        out[0] = r.x; out[1] = r.y; out[2] = r.z;
    }
};
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;
const Q kFlat = Q::axis(1, 0, 0, kPi / 2);   // Cemus Grundhaltung: flach, Knoepfe oben

class NLWiiPad : public ControllerBase {
public:
    NLWiiPad(int slot, NLWiiSource src)
        : ControllerBase("nl_wii_" + std::to_string(slot), "Wiimote " + std::to_string(slot)), m_src(std::move(src)) {
        set_axis_settings(AxisSetting(0.0f));
        set_rotation_settings(AxisSetting(0.0f));
        set_trigger_settings(AxisSetting(0.0f));
        set_use_motion(true);
        m_last = m_motion_t = std::chrono::steady_clock::now();
    }
    std::string_view api_name() const override { return "NintendoLand"; }
    InputAPI::Type api() const override { return InputAPI::Keyboard; }
    bool is_connected() override { return true; }
    std::string get_button_name(uint64 button) const override { return "NL " + std::to_string(button); }

    bool has_position() override { return true; }
    glm::vec2 get_position() override { std::scoped_lock l(m_mx); return m_ptr; }
    glm::vec2 get_prev_position() override { std::scoped_lock l(m_mx); return m_prev; }
    PositionVisibility GetPositionVisibility() override {
        std::scoped_lock l(m_mx);
        return m_ptr_visible ? PositionVisibility::FULL : PositionVisibility::NONE;
    }

    // Bewegung (Beschleunigung + MotionPlus). Nintendo Land liest die Haltung der Wiimote aus MotionPlus
    // (z.B. das Schwert in Battle Quest). Zwei Wege:
    //  - echte Sensoren (Gyro-Controller oder echte Wiimote des Gastes): durch Cemus Bewegungs-Filter,
    //    genau wie ein Gyro-Controller in Cemu;
    //  - sonst eine nachgebaute Wiimote, die sich nach Zeiger/Tasten dreht (Maus nach links = Wiimote
    //    nach links drehen, schnell bewegen = Schwung).
    bool has_motion() override { return true; }
    MotionSample get_motion_sample() override {
        std::scoped_lock l(m_mx);
        if (m_real) return m_handler.getMotionSample();
        return virtual_sample_locked();
    }

    // Uebersicht im Menue
    struct Status { bool pointer = false, motion = false, real_imu = false; int source = -1, battery = -1; uint8_t ext = 0; };
    Status status() {
        std::scoped_lock l(m_mx);
        Status st;
        st.pointer = m_ptr_visible && std::chrono::steady_clock::now() - m_ptr_time < std::chrono::seconds(1);
        st.motion = m_real || m_has_angles;
        st.real_imu = m_real;
        st.source = m_source;
        st.battery = m_battery;
        st.ext = m_ext;
        return st;
    }

    ControllerState raw_state() override {
        NLWiiIn in = m_src ? m_src() : NLWiiIn{};
        ControllerState out{};
        if (!in.connected || g_nl_menu_open) return out;
        bool act[W_COUNT] = {};
        if (in.native) {
            // Online-Gast: kommt schon als Wiimote an (Belegung macht der Gast selbst)
            auto w = [&](uint16_t bit) { return (in.wbuttons & bit) != 0; };
            act[W_A] = w(nlnet::NLW_A); act[W_B] = w(nlnet::NLW_B); act[W_1] = w(nlnet::NLW_1); act[W_2] = w(nlnet::NLW_2);
            act[W_PLUS] = w(nlnet::NLW_PLUS); act[W_MINUS] = w(nlnet::NLW_MINUS); act[W_HOME] = w(nlnet::NLW_HOME);
            act[W_UP] = w(nlnet::NLW_UP); act[W_DOWN] = w(nlnet::NLW_DOWN); act[W_LEFT] = w(nlnet::NLW_LEFT);
            act[W_RIGHT] = w(nlnet::NLW_RIGHT); act[W_C] = w(nlnet::NLW_C); act[W_Z] = w(nlnet::NLW_Z);
            out.axis = {std::clamp(in.nx, -1.0f, 1.0f), std::clamp(-in.ny, -1.0f, 1.0f)};
        } else {
            map_local_pad(in, act);
            out.axis = {in.lx, in.ly};
        }
        for (int i = 0; i < W_COUNT; i++) out.buttons.SetButtonState(kButton0 + i, act[i]);

        auto now = std::chrono::steady_clock::now();
        float dt = std::min(0.1f, std::chrono::duration<float>(now - m_last).count());
        m_last = now;
        std::scoped_lock l(m_mx);
        m_source = in.source;
        m_battery = in.battery;
        m_ext = in.ext;
        // ---- Zeiger
        m_prev = m_ptr;
        if (in.native) {
            if (in.has_ptr) {
                m_ptr = {std::clamp(in.px, 0.0f, 1.0f), std::clamp(in.py, 0.0f, 1.0f)};
                m_ptr_visible = true;
                m_ptr_time = now;
            } else if (in.source == nlnet::SRC_WIIMOTE) {
                m_ptr_visible = false;   // echte Wiimote sieht die Sensorleiste nicht
            }
        } else {
            auto dz = [](float v) { return std::abs(v) < 0.18f ? 0.0f : v; };
            if (in.has_ptr) m_ptr = {std::clamp(in.px, 0.0f, 1.0f), std::clamp(in.py, 0.0f, 1.0f)};
            if (in.buttons & (1u << B_RSTICK)) m_ptr = {0.5f, 0.5f};
            m_ptr.x = std::clamp(m_ptr.x + dz(in.rx) * 0.9f * dt, 0.0f, 1.0f);
            m_ptr.y = std::clamp(m_ptr.y + dz(in.ry) * 0.9f * dt, 0.0f, 1.0f);
            m_ptr_visible = true;
            m_ptr_time = now;
        }
        // ---- Bewegung
        if (in.recenter != m_recenter) {   // Gast hat "Zentrieren" gedrueckt
            m_recenter = in.recenter;
            m_handler = WiiUMotionHandler{};
            m_last_sensor_us = 0;
        }
        if (in.imu_real) {
            if (!m_real) { m_handler = WiiUMotionHandler{}; m_last_sensor_us = 0; }
            m_real = true;
            if (in.sensor_us != m_last_sensor_us) {
                float sdt = m_last_sensor_us ? (in.sensor_us - m_last_sensor_us) / 1e6f : 0.008f;
                sdt = std::clamp(sdt, 0.001f, 0.2f);
                m_last_sensor_us = in.sensor_us;
                // gleiche Umrechnung wie Cemus SDL-Controller (input/api/SDL/SDLControllerProvider.cpp)
                const float ax = -in.acc[0] / 9.81f, ay = -in.acc[1] / 9.81f, az = -in.acc[2] / 9.81f;
                m_handler.processMotionSample(sdt, in.gyro[0], -in.gyro[1], -in.gyro[2], ax, -ay, -az);
            }
        } else {
            m_real = false;
            if (in.has_angles) {
                m_has_angles = true;
                m_t_yaw = in.yaw; m_t_pitch = in.pitch; m_t_roll = in.roll;
                m_shake = in.shake;
            } else {
                // lokaler Controller ohne Sensoren: Haltung folgt dem Zeiger
                m_has_angles = true;
                m_t_yaw = (m_ptr.x - 0.5f) * 60.0f;
                m_t_pitch = (0.5f - m_ptr.y) * 40.0f;
                m_t_roll = 0;
                m_shake = 0;
            }
        }
        return out;
    }

private:
    static void map_local_pad(const NLWiiIn& in, bool act[W_COUNT]) {
        auto b = [&](int bit) { return ((in.buttons >> bit) & 1) != 0; };
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
            act[W_RIGHT] = up; act[W_LEFT] = down; act[W_UP] = left; act[W_DOWN] = right;
        } else {
            act[W_UP] = up; act[W_DOWN] = down; act[W_LEFT] = left; act[W_RIGHT] = right;
        }
    }

    // Nachgebaute Wiimote: Haltung aus yaw/pitch/roll, daraus Beschleunigung, Drehrate und Lage
    // in genau der Form, die Cemus Bewegungs-Filter fuer einen echten Gyro-Controller liefern wuerde.
    MotionSample virtual_sample_locked() {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - m_motion_t).count();
        m_motion_t = now;
        dt = std::clamp(dt, 0.001f, 0.1f);
        // weich nachfuehren (Netz liefert in Stufen), aber schnell genug fuer Schwuenge
        const float k = 1.0f - std::exp(-dt * 30.0f);
        m_yaw += (m_t_yaw - m_yaw) * k;
        m_pitch += (m_t_pitch - m_pitch) * k;
        m_roll += (m_t_roll - m_roll) * k;
        const Q q = Q::axis(0, 0, 1, m_yaw * kDeg) * kFlat * Q::axis(1, 0, 0, m_pitch * kDeg) * Q::axis(0, 0, 1, m_roll * kDeg);
        // Drehrate im Koerper aus der Aenderung der Haltung
        Q d = m_q.conj() * q;
        if (d.w < 0) d = {-d.w, -d.x, -d.y, -d.z};
        float gyro[3] = {2 * d.x / dt, 2 * d.y / dt, 2 * d.z / dt};
        m_q = q;
        // Schwerkraft (nach unten) im Koerper + Schuetteln
        const float down[3] = {0, 0, 1};
        float acc[3];
        q.conj().rotate(down, acc);
        if (m_shake > 0.01f) {
            m_shake_phase += dt * 2 * kPi * 9.0f;
            acc[1] += std::sin(m_shake_phase) * 2.5f * m_shake;
        }
        float acc_acc = MotionSample::calculateAccAcceleration(m_prev_acc, acc);
        m_prev_acc[0] = acc[0]; m_prev_acc[1] = acc[1]; m_prev_acc[2] = acc[2];
        // Winkel genau wie MahonySensorFusion::calcOrientation, dann wie WiiUMotionHandler
        const float roll = std::atan2(2 * (q.z * q.w + q.x * q.y), 1 - 2 * (q.w * q.w + q.x * q.x));
        const float sp = std::clamp(2 * (q.z * q.x - q.y * q.w), -1.0f, 1.0f);
        const float pitch = std::asin(sp);
        const float yaw = std::atan2(2 * (q.z * q.y + q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y));
        auto r2o = [](float r) { return r / (2 * kPi); };
        float orient[3] = {r2o(-yaw) - 0.5f, r2o(-pitch) - 0.5f, r2o(roll)};
        float quat[4] = {q.w, q.x, q.y, q.z};
        return MotionSample(acc, acc_acc, gyro, orient, quat);
    }

    NLWiiSource m_src;
    std::mutex m_mx;
    glm::vec2 m_ptr{0.5f, 0.5f}, m_prev{0.5f, 0.5f};
    bool m_ptr_visible = true;
    std::chrono::steady_clock::time_point m_last, m_motion_t, m_ptr_time{};
    // Bewegung
    bool m_real = false, m_has_angles = false;
    WiiUMotionHandler m_handler;
    uint32_t m_last_sensor_us = 0;
    uint8_t m_recenter = 0;
    float m_t_yaw = 0, m_t_pitch = 0, m_t_roll = 0, m_shake = 0;
    float m_yaw = 0, m_pitch = 0, m_roll = 0, m_shake_phase = 0;
    Q m_q = kFlat;
    float m_prev_acc[3] = {0, 1, 0};
    // Status
    int m_source = -1, m_battery = -1;
    uint8_t m_ext = 0;
};

// MotionPlus fuer nachgebaute Wiimotes (Controller, Online-Gaeste, Wiimote-Test):
// Sobald das Spiel MotionPlus sieht, rechnet Nintendo Land den Zeiger aus der Haltung der Wiimote statt aus
// der Zeigerposition - mit Maus/Stick landet der Klick dann woanders als der Zeiger. Darum MotionPlus nur dort,
// wo man ihn braucht (Battle Quest: Schwert), sonst eine normale Wiimote + Nunchuk mit Zeiger wie in Cemu.
static WPADDeviceType wanted_virtual_type() {
    const int mode = g_nl.wii_motionplus;
    if (mode == 1) return kWAPDevMPLSFreeStyle;
    if (mode == 2) return kWAPDevFreestyle;
    const std::string a = nl_current_attraction();
    return a.find("Battle Quest") != std::string::npos ? kWAPDevMPLSFreeStyle : kWAPDevFreestyle;
}

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
        const WPADDeviceType t = wanted_virtual_type();
        wm->set_device_type(t);   // Wiimote + Nunchuk (+ MotionPlus nur wo noetig)
        s_slots[i] = {true, remote, name, pad, nullptr, t};
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
    if (slot < 1 || slot > 4 || !s_slots[slot].used) return {};
    const Slot& sl = s_slots[slot];
    NLWiiSlotInfo info;
    info.used = true;
    info.remote = sl.remote;
    info.name = sl.name;
    if (sl.pad) {
        const auto st = sl.pad->status();
        info.kind = sl.remote ? "Online-Gast" : "Controller";
        static const char* kSrc[] = {"Maus + Tastatur", "Controller", "echte Wiimote"};
        info.device = (st.source >= 0 && st.source <= 2) ? kSrc[st.source] : "Controller (dieser PC)";
        if (st.real_imu && st.source == nlnet::SRC_CONTROLLER) info.device += " mit Gyro";
        info.pointer_seen = st.pointer;
        info.motion = st.motion;
        info.nunchuk = (st.ext & 1) != 0;
        info.motion_plus = (st.ext & 2) != 0;
        info.battery = st.battery;
    }
#if HAS_WIIMOTE
    if (sl.native) {
        auto n = std::static_pointer_cast<NativeWiimoteController>(sl.native);
        info.kind = "Wiimote (Bluetooth)";
        info.device = "echte Wiimote";
        info.pointer_seen = n->GetPositionVisibility() != PositionVisibility::NONE;
        info.nunchuk = n->get_extension() == NativeWiimoteController::Nunchuck;
        info.motion_plus = n->is_mpls_attached();
        info.motion = true;
        info.battery = n->has_low_battery() ? 10 : -1;
    }
#endif
    return info;
}

void nl_wii_update_motion_mode() {
    const WPADDeviceType t = wanted_virtual_type();
    auto& im = InputManager::instance();
    std::scoped_lock l(s_mx);
    for (int i = 1; i <= 4; i++) {
        Slot& sl = s_slots[i];
        if (!sl.used || !sl.pad || sl.type == t) continue;
        if (auto wm = std::dynamic_pointer_cast<WiimoteController>(im.get_controller(i))) wm->set_device_type(t);
        sl.type = t;
        cemuLog_log(LogType::Force, "NL: Wiimote {} jetzt {}", i, t == kWAPDevMPLSFreeStyle ? "mit MotionPlus" : "ohne MotionPlus");
    }
    // alle ~10 s eine Zeile pro nachgebauter Wiimote ins Log (hilft, wenn der Zeiger nicht trifft)
    static int s_tick = 0;
    if (++s_tick % 20 != 0) return;
    for (int i = 1; i <= 4; i++) {
        Slot& sl = s_slots[i];
        if (!sl.used || !sl.pad) continue;
        const glm::vec2 p = sl.pad->get_position();
        const auto st = sl.pad->status();
        cemuLog_log(LogType::Force, "NL: Wiimote {} ({}) Zeiger {:.2f}/{:.2f} {} | {} | Quelle {}", i, sl.name, p.x, p.y,
                    st.pointer ? "sichtbar" : "weg", sl.type == kWAPDevMPLSFreeStyle ? "MotionPlus" : "ohne MotionPlus", st.source);
    }
}

// Anzahl wird im Eingabe-Thread (nl_wii_update_native, alle ~2 s) ermittelt. Nicht im Menue abfragen:
// Cemus get_controllers() legt jedes Mal neue Controller an und spricht die Wiimotes an.
static std::atomic<int> s_bt_count{0};
int nl_wii_bluetooth_count() { return s_bt_count; }

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
    const auto controllers = provider->get_controllers();
    int connected = 0;
    for (auto& c : controllers) connected += c->is_connected() ? 1 : 0;
    s_bt_count = connected;
    for (auto& c : controllers) {
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

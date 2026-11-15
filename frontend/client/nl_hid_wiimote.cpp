// NL Recomp - echte Wiimotes fuer NLOnline (Windows-HID).
// Protokoll nach wiibrew.org (Wiimote-Reports, Kamera, Nunchuk, MotionPlus).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "nl_hid_wiimote.h"
#include "../net/nl_proto.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace {
using clk = std::chrono::steady_clock;

uint32_t now_us() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(clk::now().time_since_epoch()).count();
}

std::atomic<int> s_sign_pitch{1}, s_sign_yaw{1}, s_sign_roll{1};

enum : uint8_t {
    OUT_RUMBLE = 0x10, OUT_LEDS = 0x11, OUT_MODE = 0x12, OUT_IR1 = 0x13, OUT_STATUS = 0x15, OUT_WRITE = 0x16,
    OUT_READ = 0x17, OUT_IR2 = 0x1A,
    IN_STATUS = 0x20, IN_READ = 0x21, IN_ACK = 0x22,
};

class Device {
public:
    Device(std::wstring path, HANDLE h, int index) : m_path(std::move(path)), m_h(h), m_index(index) {
        m_evt_r = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_evt_w = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(m_h, &pp)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                m_out_len = std::max<int>(caps.OutputReportByteLength, 22);
                m_in_len = std::max<int>(caps.InputReportByteLength, 22);
            }
            HidD_FreePreparsedData(pp);
        }
        m_state.name = "Wiimote " + std::to_string(index + 1);
    }
    ~Device() {
        m_quit = true;
        if (m_thread.joinable()) m_thread.join();
        CancelIo(m_h);
        CloseHandle(m_h);
        CloseHandle(m_evt_r);
        CloseHandle(m_evt_w);
    }
    void start() { m_thread = std::thread([this] { run(); }); }
    bool dead() const { return m_dead; }
    const std::wstring& path() const { return m_path; }
    int index() const { return m_index; }
    NLHidWiimoteState state() { std::scoped_lock l(m_mx); return m_state; }
    void set_rumble(bool on) { m_rumble = on; m_rumble_dirty = true; }

private:
    // ------------------------------------------------------------------ Senden/Empfangen
    bool write(std::initializer_list<uint8_t> bytes) {
        std::vector<uint8_t> buf(m_out_len, 0);
        size_t i = 0;
        for (uint8_t b : bytes) if (i < buf.size()) buf[i++] = b;
        return write_raw(buf);
    }
    bool write_mem(uint8_t space, uint32_t addr, std::initializer_list<uint8_t> data) {
        std::vector<uint8_t> v = {OUT_WRITE, space, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)data.size()};
        for (uint8_t b : data) v.push_back(b);
        while (v.size() < 22) v.push_back(0);
        std::vector<uint8_t> buf(m_out_len, 0);
        memcpy(buf.data(), v.data(), std::min(buf.size(), v.size()));
        m_ack = -1;
        if (!write_raw(buf)) return false;
        const auto until = clk::now() + std::chrono::milliseconds(400);
        while (m_ack < 0 && clk::now() < until && !m_quit) pump(20);
        return m_ack == 0;
    }
    bool write_raw(std::vector<uint8_t>& buf) {
        buf[1] |= m_rumble ? 1 : 0;
        OVERLAPPED ov{};
        ov.hEvent = m_evt_w;
        ResetEvent(m_evt_w);
        DWORD done = 0;
        BOOL ok = WriteFile(m_h, buf.data(), (DWORD)buf.size(), nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(m_evt_w, 200) == WAIT_OBJECT_0) ok = GetOverlappedResult(m_h, &ov, &done, FALSE);
            else { CancelIo(m_h); ok = FALSE; m_read_pending = false; }
        }
        if (ok) return true;
        return HidD_SetOutputReport(m_h, buf.data(), (ULONG)buf.size()) != FALSE;
    }
    bool read_mem(uint8_t space, uint32_t addr, uint16_t size, uint8_t* out) {
        m_read_ok = false;
        m_read_err = false;
        m_read_want = size;
        m_read_got = 0;
        m_read_buf = out;
        if (!write({OUT_READ, space, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(size >> 8), (uint8_t)size}))
            return false;
        const auto until = clk::now() + std::chrono::milliseconds(500);
        while (!m_read_ok && !m_read_err && clk::now() < until && !m_quit) pump(20);
        m_read_buf = nullptr;
        return m_read_ok;
    }

    // Einen Report lesen und verarbeiten (wartet hoechstens ms)
    bool pump(DWORD ms) {
        if (!m_read_pending) {
            memset(m_in, 0, sizeof(m_in));
            m_ov = {};
            m_ov.hEvent = m_evt_r;
            ResetEvent(m_evt_r);
            DWORD got = 0;
            if (ReadFile(m_h, m_in, (DWORD)std::min<int>(m_in_len, sizeof(m_in)), &got, &m_ov)) {
                handle(m_in, got);
                return true;
            }
            if (GetLastError() != ERROR_IO_PENDING) { m_io_error = true; return false; }
            m_read_pending = true;
        }
        if (WaitForSingleObject(m_evt_r, ms) != WAIT_OBJECT_0) return false;
        DWORD got = 0;
        m_read_pending = false;
        if (!GetOverlappedResult(m_h, &m_ov, &got, FALSE)) { m_io_error = true; return false; }
        handle(m_in, got);
        return true;
    }

    // ------------------------------------------------------------------ Auswertung
    void handle(const uint8_t* d, DWORD n) {
        if (n < 3) return;
        m_last_data = clk::now();
        const uint8_t id = d[0];
        parse_buttons(d + 1);
        if (id == IN_STATUS && n >= 7) {
            const bool ext = (d[3] & 0x02) != 0;
            {
                std::scoped_lock l(m_mx);
                m_state.battery = std::min(100, d[6] * 100 / 0xC8);
            }
            m_status_seen = true;
            if (ext != m_ext_flag) { m_ext_flag = ext; m_ext_changed = true; }
            m_need_mode = true;   // nach einem Status-Report schickt die Wiimote erst nach neuem Modus wieder Daten
        } else if (id == IN_READ && n >= 22) {
            if (!m_read_buf) return;
            const int err = d[3] & 0x0F, sz = (d[3] >> 4) + 1;
            if (err) { m_read_err = true; return; }
            for (int i = 0; i < sz && m_read_got < m_read_want; i++) m_read_buf[m_read_got++] = d[6 + i];
            if (m_read_got >= m_read_want) m_read_ok = true;
        } else if (id == IN_ACK && n >= 5) {
            if (d[3] == OUT_WRITE) m_ack = d[4];
        } else if (id == 0x37 && n >= 22) {
            parse_acc(d);
            parse_ir(d + 6);
            parse_ext(d + 16);
            std::scoped_lock l(m_mx);
            m_state.sample_us = now_us();
        } else if (id >= 0x30 && id <= 0x3F) {
            std::scoped_lock l(m_mx);
            m_state.sample_us = now_us();
        }
    }

    void parse_buttons(const uint8_t* b) {
        uint16_t w = 0;
        if (b[0] & 0x01) w |= nlnet::NLW_LEFT;
        if (b[0] & 0x02) w |= nlnet::NLW_RIGHT;
        if (b[0] & 0x04) w |= nlnet::NLW_DOWN;
        if (b[0] & 0x08) w |= nlnet::NLW_UP;
        if (b[0] & 0x10) w |= nlnet::NLW_PLUS;
        if (b[1] & 0x01) w |= nlnet::NLW_2;
        if (b[1] & 0x02) w |= nlnet::NLW_1;
        if (b[1] & 0x04) w |= nlnet::NLW_B;
        if (b[1] & 0x08) w |= nlnet::NLW_A;
        if (b[1] & 0x10) w |= nlnet::NLW_MINUS;
        if (b[1] & 0x80) w |= nlnet::NLW_HOME;
        std::scoped_lock l(m_mx);
        m_state.buttons = (uint16_t)((m_state.buttons & (nlnet::NLW_C | nlnet::NLW_Z)) | w);
    }

    void parse_acc(const uint8_t* d) {
        const int x = (d[3] << 2) | ((d[1] >> 5) & 3);
        const int y = (d[4] << 2) | ((d[2] >> 4) & 2);
        const int z = (d[5] << 2) | ((d[2] >> 5) & 2);
        const int raw[3] = {x, y, z};
        std::scoped_lock l(m_mx);
        for (int i = 0; i < 3; i++) {
            const float span = (float)std::max(1, m_cal_one[i] - m_cal_zero[i]);
            m_state.acc[i] = (raw[i] - m_cal_zero[i]) / span;
        }
    }

    void parse_ir(const uint8_t* d) {
        struct Dot { int x, y; };
        Dot dots[4];
        int n = 0;
        for (int p = 0; p < 2; p++) {
            const uint8_t* o = d + p * 5;
            const int x1 = o[0] | ((o[2] >> 4) & 3) << 8, y1 = o[1] | ((o[2] >> 6) & 3) << 8;
            const int x2 = o[3] | (o[2] & 3) << 8, y2 = o[4] | ((o[2] >> 2) & 3) << 8;
            if (x1 != 1023 || y1 != 1023) dots[n++] = {x1, y1};
            if (x2 != 1023 || y2 != 1023) dots[n++] = {x2, y2};
        }
        float mx, my;
        if (n >= 2) {
            // die zwei Punkte, die am weitesten auseinander liegen = beide Enden der Sensorleiste
            int a = 0, b = 1, best = -1;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++) {
                    const int dd = (dots[i].x - dots[j].x) * (dots[i].x - dots[j].x) + (dots[i].y - dots[j].y) * (dots[i].y - dots[j].y);
                    if (dd > best) { best = dd; a = i; b = j; }
                }
            if (dots[a].x > dots[b].x) std::swap(a, b);
            mx = (dots[a].x + dots[b].x) / 2.0f;
            my = (dots[a].y + dots[b].y) / 2.0f;
            m_half_x = (dots[b].x - dots[a].x) / 2.0f;
            m_half_y = (dots[b].y - dots[a].y) / 2.0f;
            m_last_mid_x = mx;
        } else if (n == 1 && m_half_x > 0) {
            // nur ein Punkt sichtbar: mit dem letzten Abstand die Mitte schaetzen
            const bool is_left = dots[0].x < m_last_mid_x;
            mx = dots[0].x + (is_left ? m_half_x : -m_half_x);
            my = dots[0].y + (is_left ? m_half_y : -m_half_y);
        } else {
            std::scoped_lock l(m_mx);
            m_state.ir_seen = false;
            m_state.ir_dots = n;
            return;
        }
        // Kamera sieht gespiegelt: Wiimote nach rechts -> Punkte wandern nach links
        float u = 1.0f - mx / 1024.0f, v = my / 768.0f;
        u = 0.5f + (u - 0.5f) * 1.6f;
        v = 0.5f + (v - 0.5f) * 1.6f;
        std::scoped_lock l(m_mx);
        m_state.ir_seen = true;
        m_state.ir_dots = n;
        m_state.px = std::clamp(u, 0.0f, 1.0f);
        m_state.py = std::clamp(v, 0.0f, 1.0f);
    }

    void parse_nunchuk(int sx, int sy, bool c, bool z) {
        std::scoped_lock l(m_mx);
        m_state.nx = std::clamp((sx - 128) / 100.0f, -1.0f, 1.0f);
        m_state.ny = std::clamp((sy - 128) / 100.0f, -1.0f, 1.0f);
        m_state.buttons = (uint16_t)(m_state.buttons & ~(nlnet::NLW_C | nlnet::NLW_Z));
        if (c) m_state.buttons |= nlnet::NLW_C;
        if (z) m_state.buttons |= nlnet::NLW_Z;
    }

    void parse_ext(const uint8_t* e) {
        if (m_mpls_active) {
            const bool mpls_frame = (e[5] & 0x02) != 0;
            if (mpls_frame) {
                const int yaw = e[0] | (e[3] >> 2) << 8, roll = e[1] | (e[4] >> 2) << 8, pitch = e[2] | (e[5] >> 2) << 8;
                const bool yaw_slow = e[3] & 2, pitch_slow = e[3] & 1, roll_slow = e[4] & 2;
                auto dps = [](int raw, bool slow) { return (raw - 8192) / 20.0f * (slow ? 1.0f : 4.545f); };
                std::scoped_lock l(m_mx);
                m_state.pitch_rate = dps(pitch, pitch_slow) * s_sign_pitch;
                m_state.yaw_rate = dps(yaw, yaw_slow) * s_sign_yaw;
                m_state.roll_rate = dps(roll, roll_slow) * s_sign_roll;
                m_state.gyro_valid = true;
                m_state.nunchuk = m_passthrough && (e[4] & 1);
            } else if (m_passthrough) {
                // Nunchuk durchgereicht (komprimiert)
                parse_nunchuk(e[0], e[1], !(e[5] & 0x08), !(e[5] & 0x04));
            }
            return;
        }
        if (m_nunchuk) parse_nunchuk(e[0], e[1], !(e[5] & 0x02), !(e[5] & 0x01));
    }

    // ------------------------------------------------------------------ Einrichten
    void read_calibration() {
        uint8_t c[10] = {};
        if (!read_mem(0x00, 0x0016, 10, c)) return;
        m_cal_zero[0] = c[0] << 2 | ((c[3] >> 4) & 3);
        m_cal_zero[1] = c[1] << 2 | ((c[3] >> 2) & 3);
        m_cal_zero[2] = c[2] << 2 | (c[3] & 3);
        m_cal_one[0] = c[4] << 2 | ((c[7] >> 4) & 3);
        m_cal_one[1] = c[5] << 2 | ((c[7] >> 2) & 3);
        m_cal_one[2] = c[6] << 2 | (c[7] & 3);
        for (int i = 0; i < 3; i++)
            if (m_cal_one[i] - m_cal_zero[i] < 20) { m_cal_zero[i] = 512; m_cal_one[i] = 616; }
    }

    void setup_ir() {
        write({OUT_IR1, 0x04});
        write({OUT_IR2, 0x04});
        write_mem(0x04, 0xB00030, {0x08});
        write_mem(0x04, 0xB00000, {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64});
        write_mem(0x04, 0xB0001A, {0x63, 0x03});
        write_mem(0x04, 0xB00033, {0x01});   // Basis-Modus (10 Bytes, passt zu Report 0x37)
        write_mem(0x04, 0xB00030, {0x08});
    }

    void setup_extension() {
        m_mpls_active = m_passthrough = m_nunchuk = false;
        // normale Erweiterung (Nunchuk) unverschluesselt einschalten
        bool nunchuk = false;
        if (m_ext_flag) {
            write_mem(0x04, 0xA400F0, {0x55});
            write_mem(0x04, 0xA400FB, {0x00});
            uint8_t id[6] = {};
            if (read_mem(0x04, 0xA400FA, 6, id)) nunchuk = id[2] == 0xA4 && id[3] == 0x20 && id[4] == 0x00 && id[5] == 0x00;
        }
        // MotionPlus (Adapter oder Wiimote Plus) - eingebaut ist er auf 0xA600FA ansprechbar
        uint8_t mp[6] = {};
        const bool mpls = read_mem(0x04, 0xA600FA, 6, mp) && mp[2] == 0xA6 && mp[3] == 0x20 && mp[5] == 0x05;
        if (mpls) {
            write_mem(0x04, 0xA600F0, {0x55});
            m_ext_flag = true;   // nach dem Einschalten meldet die Wiimote "Erweiterung da" - kein neues Einrichten
            write_mem(0x04, 0xA600FE, {(uint8_t)(nunchuk ? 0x05 : 0x04)});
            m_mpls_active = true;
            m_passthrough = nunchuk;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        m_nunchuk = nunchuk && !mpls;
        std::scoped_lock l(m_mx);
        m_state.motion_plus = mpls;
        m_state.nunchuk = nunchuk;
        if (!mpls) m_state.gyro_valid = false;
        if (!nunchuk) { m_state.nx = m_state.ny = 0; }
    }

    void set_mode() { write({OUT_MODE, 0x04, 0x37}); m_need_mode = false; }

    void run() {
        // Lampe = Nummer, Status abfragen (sagt auch, ob ein Nunchuk steckt)
        write({OUT_LEDS, (uint8_t)(0x10 << (m_index & 3))});
        m_last_data = clk::now();
        write({OUT_STATUS, 0x00});
        const auto until = clk::now() + std::chrono::milliseconds(1500);
        while (!m_status_seen && clk::now() < until && !m_io_error && !m_quit) pump(50);
        if (!m_status_seen) { m_dead = true; return; }   // gekoppelt, aber aus
        read_calibration();
        setup_ir();
        m_ext_changed = false;
        setup_extension();
        set_mode();
        {
            std::scoped_lock l(m_mx);
            m_state.connected = true;
        }
        auto last_status = clk::now();
        while (!m_quit && !m_io_error) {
            pump(30);
            if (m_ext_changed) {   // Nunchuk/MotionPlus ein- oder ausgesteckt
                m_ext_changed = false;
                setup_extension();
                m_need_mode = true;
            }
            if (m_need_mode) set_mode();
            if (m_rumble_dirty) { m_rumble_dirty = false; write({OUT_RUMBLE, 0x00}); }
            if (clk::now() - last_status > std::chrono::seconds(20)) {   // Akku
                last_status = clk::now();
                write({OUT_STATUS, 0x00});
            }
            if (clk::now() - m_last_data > std::chrono::seconds(4)) break;   // ausgeschaltet / ausser Reichweite
        }
        std::scoped_lock l(m_mx);
        m_state.connected = false;
        m_dead = true;
    }

    std::wstring m_path;
    HANDLE m_h;
    HANDLE m_evt_r = nullptr, m_evt_w = nullptr;
    OVERLAPPED m_ov{};
    uint8_t m_in[64] = {};
    int m_out_len = 22, m_in_len = 22;
    int m_index;
    std::thread m_thread;
    std::atomic<bool> m_quit{false}, m_dead{false};
    std::atomic<bool> m_rumble{false}, m_rumble_dirty{false};
    bool m_read_pending = false, m_io_error = false;
    clk::time_point m_last_data{};
    // Antworten
    int m_ack = -1;
    bool m_read_ok = false, m_read_err = false;
    uint16_t m_read_want = 0, m_read_got = 0;
    uint8_t* m_read_buf = nullptr;
    bool m_status_seen = false, m_ext_flag = false, m_ext_changed = false, m_need_mode = false;
    bool m_mpls_active = false, m_passthrough = false, m_nunchuk = false;
    int m_cal_zero[3] = {512, 512, 512}, m_cal_one[3] = {616, 616, 616};
    float m_half_x = 0, m_half_y = 0, m_last_mid_x = 512;
    std::mutex m_mx;
    NLHidWiimoteState m_state;
};

std::mutex s_mx;
std::vector<std::unique_ptr<Device>> s_devs;
std::thread s_scan;
std::atomic<bool> s_quit{false};

std::vector<std::wstring> enumerate_wiimotes() {
    std::vector<std::wstring> out;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &guid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<uint8_t> buf(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifd, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{sizeof(a)};
        if (HidD_GetAttributes(h, &a) && a.VendorID == 0x057E && (a.ProductID == 0x0306 || a.ProductID == 0x0330))
            out.push_back(det->DevicePath);
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(info);
    return out;
}

void scan_loop() {
    while (!s_quit) {
        // tote Geraete aufraeumen
        {
            std::vector<std::unique_ptr<Device>> dead;
            {
                std::scoped_lock l(s_mx);
                for (auto it = s_devs.begin(); it != s_devs.end();) {
                    if ((*it)->dead()) { dead.push_back(std::move(*it)); it = s_devs.erase(it); }
                    else ++it;
                }
            }
            dead.clear();   // Threads ausserhalb der Sperre beenden
        }
        for (auto& path : enumerate_wiimotes()) {
            std::scoped_lock l(s_mx);
            bool known = false;
            for (auto& d : s_devs) known |= d->path() == path;
            if (known || s_devs.size() >= 4) continue;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            // freie Nummer (Lampe) suchen
            int idx = 0;
            for (;; idx++) {
                bool used = false;
                for (auto& d : s_devs) used |= d->index() == idx;
                if (!used) break;
            }
            s_devs.push_back(std::make_unique<Device>(path, h, idx));
            s_devs.back()->start();
        }
        for (int i = 0; i < 20 && !s_quit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
}  // namespace

void nl_hidwii_start() {
    if (s_scan.joinable()) return;
    s_quit = false;
    s_scan = std::thread(scan_loop);
}

void nl_hidwii_stop() {
    s_quit = true;
    if (s_scan.joinable()) s_scan.join();
    std::vector<std::unique_ptr<Device>> all;
    {
        std::scoped_lock l(s_mx);
        all.swap(s_devs);
    }
    all.clear();
}

std::vector<NLHidWiimoteState> nl_hidwii_list() {
    std::vector<NLHidWiimoteState> out;
    std::scoped_lock l(s_mx);
    std::vector<Device*> sorted;
    for (auto& d : s_devs) sorted.push_back(d.get());
    std::sort(sorted.begin(), sorted.end(), [](Device* a, Device* b) { return a->index() < b->index(); });
    for (auto* d : sorted) {
        NLHidWiimoteState st = d->state();
        if (st.connected) out.push_back(st);
    }
    return out;
}

bool nl_hidwii_get(int index, NLHidWiimoteState& out) {
    auto list = nl_hidwii_list();
    if (index < 0 || index >= (int)list.size()) return false;
    out = list[index];
    return true;
}

void nl_hidwii_rumble(int index, bool on) {
    std::scoped_lock l(s_mx);
    int i = 0;
    for (auto& d : s_devs) {
        if (!d->state().connected) continue;
        if (i++ == index) d->set_rumble(on);
    }
}

void nl_hidwii_set_mpls_signs(int pitch, int yaw, int roll) {
    s_sign_pitch = pitch < 0 ? -1 : 1;
    s_sign_yaw = yaw < 0 ? -1 : 1;
    s_sign_roll = roll < 0 ? -1 : 1;
}

// NL Recomp - Discord Rich Presence (siehe nl_discord.h)
//
// Protokoll der Discord-App: Named Pipe \\.\pipe\discord-ipc-N, jede Nachricht =
//   uint32 Opcode (0 Handshake, 1 Frame, 2 Close, 3 Ping, 4 Pong) + uint32 Laenge + JSON.
#include "nl_discord.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::thread s_thread;
std::atomic<bool> s_quit{false};
std::mutex s_mx;
struct Activity {
    std::string details, state;
    int party_size = 0, party_max = 0;
    int64_t start = 0;
};
Activity s_want;
bool s_dirty = false;
HANDLE s_pipe = INVALID_HANDLE_VALUE;
uint64_t s_nonce = 0;

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': break;
        case '\t': o += ' '; break;
        default:
            if (c < 0x20) break;
            o += (char)c;
        }
    }
    return o;
}

bool write_frame(uint32_t op, const std::string& json) {
    if (s_pipe == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> buf(8 + json.size());
    const uint32_t len = (uint32_t)json.size();
    memcpy(buf.data(), &op, 4);
    memcpy(buf.data() + 4, &len, 4);
    memcpy(buf.data() + 8, json.data(), json.size());
    DWORD w = 0;
    return WriteFile(s_pipe, buf.data(), (DWORD)buf.size(), &w, nullptr) && w == buf.size();
}

// alles lesen, was Discord geschickt hat (sonst laeuft die Leitung irgendwann voll); Ping beantworten
bool drain() {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(s_pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail < 8) return true;
        uint32_t hdr[2];
        DWORD r = 0;
        if (!ReadFile(s_pipe, hdr, 8, &r, nullptr) || r != 8) return false;
        std::string body(hdr[1], '\0');
        if (hdr[1] && (!ReadFile(s_pipe, body.data(), hdr[1], &r, nullptr) || r != hdr[1])) return false;
        if (hdr[0] == 3) write_frame(4, body);   // Ping -> Pong
        if (hdr[0] == 2) return false;            // Discord schliesst
    }
}

void disconnect() {
    if (s_pipe != INVALID_HANDLE_VALUE) CloseHandle(s_pipe);
    s_pipe = INVALID_HANDLE_VALUE;
}

bool connect() {
    for (int i = 0; i < 10; i++) {
        std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        s_pipe = h;
        if (!write_frame(0, std::string("{\"v\":1,\"client_id\":\"") + kNLDiscordAppId + "\"}")) { disconnect(); continue; }
        // auf die Antwort (READY) warten, max. 2 s
        for (int t = 0; t < 40; t++) {
            DWORD avail = 0;
            if (!PeekNamedPipe(s_pipe, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail >= 8) return drain();
            Sleep(50);
        }
        disconnect();
    }
    return false;
}

bool send_activity(const Activity& a) {
    std::string act = "{";
    if (!a.details.empty()) act += "\"details\":\"" + json_escape(a.details) + "\",";
    if (!a.state.empty()) act += "\"state\":\"" + json_escape(a.state) + "\",";
    if (a.start > 0) act += "\"timestamps\":{\"start\":" + std::to_string(a.start) + "},";
    act += "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"NL Recomp\"}";
    if (a.party_max > 0)
        act += ",\"party\":{\"id\":\"nl-" + std::to_string(GetCurrentProcessId()) + "\",\"size\":[" + std::to_string(a.party_size) + "," +
               std::to_string(a.party_max) + "]}";
    act += "}";
    const std::string json = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                             ",\"activity\":" + act + "},\"nonce\":\"" + std::to_string(++s_nonce) + "\"}";
    return write_frame(1, json);
}

void loop() {
    auto last_try = std::chrono::steady_clock::time_point{};
    auto last_send = std::chrono::steady_clock::time_point{};
    while (!s_quit) {
        const auto now = std::chrono::steady_clock::now();
        if (s_pipe == INVALID_HANDLE_VALUE) {
            if (now - last_try > std::chrono::seconds(10)) {
                last_try = now;
                if (connect()) {
                    std::scoped_lock l(s_mx);
                    s_dirty = true;   // nach (Wieder-)Verbinden alles neu schicken
                }
            }
        } else {
            if (!drain()) { disconnect(); continue; }
            Activity a;
            bool send = false;
            {
                std::scoped_lock l(s_mx);
                // Discord erlaubt etwa 5 Aenderungen pro 20 s
                if (s_dirty && now - last_send > std::chrono::seconds(4)) { a = s_want; s_dirty = false; send = true; }
            }
            if (send) {
                last_send = now;
                if (!send_activity(a)) disconnect();
            }
        }
        Sleep(250);
    }
    if (s_pipe != INVALID_HANDLE_VALUE) {
        write_frame(1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) + "},\"nonce\":\"end\"}");
        disconnect();
    }
}
}  // namespace

void nl_discord_start() {
    if (s_thread.joinable()) return;
    s_quit = false;
    s_thread = std::thread(loop);
}

void nl_discord_stop() {
    s_quit = true;
    if (s_thread.joinable()) s_thread.join();
}

void nl_discord_set(const std::string& details, const std::string& state, int party_size, int party_max, int64_t start_unix) {
    std::scoped_lock l(s_mx);
    if (s_want.details == details && s_want.state == state && s_want.party_size == party_size && s_want.party_max == party_max &&
        s_want.start == start_unix)
        return;
    s_want = {details, state, party_size, party_max, start_unix};
    s_dirty = true;
}

std::string nl_attraction_from_path(const std::string& path) {
    static const struct { const char* key; const char* name; } kMap[] = {
        {"Zld_", "The Legend of Zelda: Battle Quest"},
        {"Mtr_", "Metroid Blast"},
        {"Pkm_", "Pikmin Adventure"},
        {"Mro_", "Mario Chase"},
        {"Lig_", "Luigi's Ghost Mansion"},
        {"Dbt_", "Animal Crossing: Sweet Day"},
        {"Fzr_", "Captain Falcon's Twister Race"},
        {"Nzm_", "Takamaru's Ninja Castle"},
        {"Dkb_", "Donkey Kong's Crash Course"},
        {"Ysi_", "Yoshi's Fruit Cart"},
        {"Blf_", "Balloon Trip Breeze"},
        {"Gwr_", "Octopus Dance"},
    };
    const size_t p = path.find("/Package/");
    if (p == std::string::npos) return "";
    const std::string file = path.substr(p + 9);
    for (const auto& m : kMap)
        if (file.compare(0, strlen(m.key), m.key) == 0) return m.name;
    return "";
}

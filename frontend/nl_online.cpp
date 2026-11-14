// Nintendo Land Recomp - Online als Host: Raum mit Code ueber Steam, TV-Bild und Ton an die
// Freunde streamen, ihre Controller als Wiimote-Spieler 2-5 einspeisen.
#include "nl_app.h"
#include "net/nl_proto.h"
#include "net/nl_steam.h"
#include "net/nl_video.h"

#include <Windows.h>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

// aus dem Unterbau (nl_capture_vk.cpp / ax_out.cpp)
extern std::atomic<bool> g_nl_stream_active;
extern std::atomic<int> g_nl_stream_w, g_nl_stream_h, g_nl_stream_fps;
extern void (*g_nl_stream_frame)(const uint8* rgba, int w, int h, int stride, bool srgb);
extern void (*g_nl_stream_audio)(const sint16* samples, int frames, int channels);

using namespace nlnet;
using clk = std::chrono::steady_clock;

namespace {
struct Client {
    uint64_t id = 0;
    int slot = 0;
    std::string name;
    uint32_t input_seq = 0;
    clk::time_point last_seen;
};

struct Quality { int w, h, fps, kbps; };
constexpr Quality kQuality[] = {{960, 540, 60, 3500}, {1280, 720, 60, 6000}, {1280, 720, 60, 12000}};

// Sperr-Reihenfolge: s_mx -> (Steam / InputManager). s_in_mx und s_peer_mx werden nie mit anderen
// Sperren verschachtelt genommen (Eingabe-Thread von Cemu, Encoder- und Ton-Thread nutzen nur diese).
std::mutex s_mx;                       // Raum, Spielerliste
std::map<uint64_t, Client> s_clients;
std::mutex s_in_mx;                    // Eingaben der Spieler (liest Cemus Eingabe-Thread)
std::map<uint64_t, NLWiiIn> s_inputs;
std::mutex s_peer_mx;                  // Empfaenger fuer Bild und Ton
std::vector<uint64_t> s_peers;
// Ergebnis von "Raum erstellen" (kommt aus einem Steam-Callback)
std::mutex s_evt_mx;
int s_room_result = 0;                 // 0 nichts, 1 ok, -1 Fehler
std::string s_room_code;
bool s_hosting = false, s_opening = false;
std::string s_status;
NLVideoEncoder s_encoder;
std::atomic<uint32_t> s_frame_no{0}, s_audio_seq{0};
clk::time_point s_last_key_req{};
std::vector<int16_t> s_audio_buf;

std::thread s_thread;
std::atomic<bool> s_quit{false};
bool s_inited = false;

uint64_t now_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(clk::now().time_since_epoch()).count(); }

void set_status(const std::string& s) {
    s_status = s;
    cemuLog_log(LogType::Force, "Online: {}", s);
}

void update_peers_locked() {
    std::scoped_lock l(s_peer_mx);
    s_peers.clear();
    for (auto& [id, c] : s_clients) s_peers.push_back(id);
}

void update_presence_locked() {
    if (!s_hosting) { nl_steam_set_status("Spielt Nintendo Land"); return; }
    nl_steam_set_status("Hostet einen Raum (" + std::to_string(s_clients.size() + 1) + "/5)");
}

// ---------------------------------------------------------------- Bild und Ton
void on_frame(const uint8* rgba, int w, int h, int stride, bool srgb) {
    (void)stride;
    s_encoder.submit_rgba(rgba, w, h, w * 4, srgb);
}

void on_packet(const uint8_t* data, size_t size, bool key, uint64_t) {
    std::vector<uint8_t> msg(sizeof(VideoHeader) + size);
    VideoHeader hdr;
    hdr.frame = ++s_frame_no;
    hdr.keyframe = key ? 1 : 0;
    hdr.width = (uint16_t)s_encoder.width();
    hdr.height = (uint16_t)s_encoder.height();
    hdr.t_host_us = now_us();
    memcpy(msg.data(), &hdr, sizeof(hdr));
    memcpy(msg.data() + sizeof(hdr), data, size);
    std::scoped_lock l(s_peer_mx);
    for (uint64_t id : s_peers) nl_net_send(id, CH_VIDEO, msg.data(), msg.size(), false);
}

void on_audio(const sint16* samples, int frames, int channels) {
    if (!g_nl_stream_active) return;
    std::vector<uint8_t> msg(sizeof(AudioHeader) + (size_t)frames * 4);
    AudioHeader hdr;
    hdr.frames = (uint16_t)frames;
    int16_t* out = (int16_t*)(msg.data() + sizeof(hdr));
    for (int i = 0; i < frames; i++) {
        const sint16* f = samples + (size_t)i * channels;
        int l = f[0], r = channels > 1 ? f[1] : f[0];
        if (channels >= 6) {   // 5.1 -> Stereo
            l = (int)(f[0] + 0.7f * f[2] + 0.7f * f[4]);
            r = (int)(f[1] + 0.7f * f[2] + 0.7f * f[5]);
        }
        out[i * 2] = (int16_t)std::clamp(l, -32768, 32767);
        out[i * 2 + 1] = (int16_t)std::clamp(r, -32768, 32767);
    }
    hdr.seq = ++s_audio_seq;
    memcpy(msg.data(), &hdr, sizeof(hdr));
    std::scoped_lock l(s_peer_mx);
    for (uint64_t id : s_peers) nl_net_send(id, CH_AUDIO, msg.data(), msg.size(), false);
}

// ---------------------------------------------------------------- Spieler
void remove_client_locked(uint64_t id, bool send_bye, uint8_t type = MSG_BYE) {
    auto it = s_clients.find(id);
    if (it == s_clients.end()) return;
    if (send_bye) {
        Simple m;
        m.type = type;
        nl_net_send(id, CH_CONTROL, &m, sizeof(m), true);
    }
    const int slot = it->second.slot;
    set_status(it->second.name + " ist gegangen.");
    s_clients.erase(it);
    update_peers_locked();
    {
        std::scoped_lock li(s_in_mx);
        s_inputs.erase(id);
    }
    nl_wii_detach(slot);
    if (!send_bye) nl_net_close(id);   // nach einem Tschuess die Sitzung offen lassen, damit es noch ankommt
    if (s_clients.empty()) g_nl_stream_active = false;
    update_presence_locked();
}

bool ensure_encoder_locked() {
    if (s_encoder.running()) return true;
    const Quality q = kQuality[std::clamp(g_nl.stream_quality, 0, 2)];
    g_nl_stream_w = q.w;
    g_nl_stream_h = q.h;
    g_nl_stream_fps = q.fps;
    if (!s_encoder.start(q.w, q.h, q.fps, q.kbps, on_packet)) {
        set_status("Video-Encoder konnte nicht starten.");
        return false;
    }
    cemuLog_log(LogType::Force, "Online: Video {}x{} {} fps {} kbit/s ueber {}", q.w, q.h, q.fps, q.kbps, s_encoder.encoder_name());
    return true;
}

void handle_hello_locked(uint64_t from, const Hello& h) {
    auto reply_full = [&](uint8_t reason) {
        Full f;
        f.reason = reason;
        nl_net_send(from, CH_CONTROL, &f, sizeof(f), true);
    };
    if (!s_hosting) { reply_full(2); return; }
    if (h.version != kVersion) { reply_full(1); return; }
    auto it = s_clients.find(from);
    if (it == s_clients.end()) {
        std::string name(h.name, strnlen(h.name, sizeof(h.name)));
        if (name.empty()) name = nl_steam_name_of(from);
        {
            std::scoped_lock li(s_in_mx);
            NLWiiIn idle;
            idle.sideways = true;
            s_inputs[from] = idle;
        }
        const int slot = nl_wii_attach(name, true, [from]() {
            std::scoped_lock li(s_in_mx);
            auto c = s_inputs.find(from);
            if (c == s_inputs.end()) { NLWiiIn none; none.connected = false; return none; }
            return c->second;
        });
        if (slot < 0) {
            std::scoped_lock li(s_in_mx);
            s_inputs.erase(from);
        }
        if (slot < 0) { reply_full(0); return; }
        Client c;
        c.id = from;
        c.slot = slot;
        c.name = name;
        c.last_seen = clk::now();
        it = s_clients.emplace(from, c).first;
        update_peers_locked();
        set_status(name + " spielt jetzt als Wiimote " + std::to_string(slot) + " mit.");
        update_presence_locked();
    }
    if (!ensure_encoder_locked()) return;
    Welcome w;
    w.slot = (uint8_t)it->second.slot;
    w.width = (uint16_t)s_encoder.width();
    w.height = (uint16_t)s_encoder.height();
    w.fps = (uint16_t)s_encoder.fps();
    nl_net_send(from, CH_CONTROL, &w, sizeof(w), true);
    s_encoder.request_keyframe();
    g_nl_stream_active = true;
}

// ---------------------------------------------------------------- Netz-Thread
void net_loop() {
    while (!s_quit) {
        nl_steam_tick();
        {
            int res = 0;
            std::string code;
            {
                std::scoped_lock le(s_evt_mx);
                res = s_room_result;
                code = s_room_code;
                s_room_result = 0;
            }
            if (res != 0) {
                std::scoped_lock l(s_mx);
                s_opening = false;
                if (res < 0) set_status("Raum konnte nicht erstellt werden. Ist Steam online?");
                else {
                    s_hosting = true;
                    nl_net_accept_all(true);
                    set_status("Raum offen. Code: " + code);
                    update_presence_locked();
                }
            }
        }
        nl_net_receive(CH_CONTROL, [](uint64_t from, const uint8_t* d, size_t n) {
            if (n < 1) return;
            std::scoped_lock l(s_mx);
            auto it = s_clients.find(from);
            if (it != s_clients.end()) it->second.last_seen = clk::now();
            switch (d[0]) {
            case MSG_HELLO:
                if (n >= sizeof(Hello)) { Hello h; memcpy(&h, d, sizeof(h)); handle_hello_locked(from, h); }
                break;
            case MSG_BYE: remove_client_locked(from, false); break;
            case MSG_KEYFRAME:
                if (clk::now() - s_last_key_req > std::chrono::milliseconds(250)) {
                    s_last_key_req = clk::now();
                    s_encoder.request_keyframe();
                }
                break;
            case MSG_PING:
                if (n >= sizeof(Ping)) {
                    Ping p;
                    memcpy(&p, d, sizeof(p));
                    p.type = MSG_PONG;
                    nl_net_send(from, CH_CONTROL, &p, sizeof(p), false);
                }
                break;
            }
        });
        nl_net_receive(CH_INPUT, [](uint64_t from, const uint8_t* d, size_t n) {
            if (n < sizeof(Input)) return;
            Input in;
            memcpy(&in, d, sizeof(in));
            std::scoped_lock l(s_mx);
            auto it = s_clients.find(from);
            if (it == s_clients.end()) return;
            Client& c = it->second;
            c.last_seen = clk::now();
            if (in.seq <= c.input_seq && c.input_seq - in.seq < 1000) return;   // altes Paket
            c.input_seq = in.seq;
            NLWiiIn w;
            w.buttons = in.buttons;
            w.lx = in.lx / 32767.0f; w.ly = in.ly / 32767.0f;
            w.rx = in.rx / 32767.0f; w.ry = in.ry / 32767.0f;
            w.lt = in.lt / 255.0f; w.rt = in.rt / 255.0f;
            w.sideways = (in.flags & IN_SIDEWAYS) != 0;
            w.connected = true;
            std::scoped_lock li(s_in_mx);
            s_inputs[from] = w;
        });
        // Verbindungen ohne Lebenszeichen entfernen
        {
            std::scoped_lock l(s_mx);
            std::vector<uint64_t> dead;
            for (auto& [id, c] : s_clients)
                if (clk::now() - c.last_seen > std::chrono::seconds(8)) dead.push_back(id);
            for (uint64_t id : dead) remove_client_locked(id, true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(s_hosting ? 1 : 20));
    }
}
}  // namespace

// ===========================================================================
bool nl_online_available() { return nl_steam_available(); }
bool nl_online_steam_ok() { return nl_steam_ok(); }

void nl_online_init() {
    if (s_inited) return;
    s_inited = true;
    g_nl_stream_frame = on_frame;
    g_nl_stream_audio = on_audio;
    if (!nl_steam_available()) {
        s_status = "Online ist in diesem Build nicht dabei (Steamworks SDK fehlt).";
        return;
    }
    SetEnvironmentVariableW(L"SteamAppId", L"480");
    SetEnvironmentVariableW(L"SteamGameId", L"480");
    if (!nl_steam_init()) {
        s_status = "Steam laeuft nicht - starte Steam und dann das Spiel neu.";
        return;
    }
    nl_steam_set_status("Spielt Nintendo Land");
    // Einladung angenommen, waehrend das Spiel laeuft -> als Gast beitreten
    nl_steam_on_join_request([](std::string code) { nl_online_join(code); });
    s_status = "Mit Steam verbunden als " + nl_steam_my_name() + ".";
    s_quit = false;
    s_thread = std::thread(net_loop);
}

void nl_online_shutdown() {
    nl_online_close_room();
    s_quit = true;
    if (s_thread.joinable()) s_thread.join();
    g_nl_stream_frame = nullptr;
    g_nl_stream_audio = nullptr;
    nl_steam_shutdown();
}

void nl_online_open_room() {
    {
        std::scoped_lock l(s_mx);
        if (!nl_steam_ok() || s_hosting || s_opening) return;
        s_opening = true;
        s_status = "Raum wird erstellt ...";
    }
    // Achtung: der Rueckruf kommt aus Steam (mit Steam-Sperre) -> nur Ergebnis ablegen
    nl_steam_host_room(5, [](bool ok, std::string code) {
        std::scoped_lock le(s_evt_mx);
        s_room_result = ok ? 1 : -1;
        s_room_code = code;
    });
}

static void close_room_locked();
void nl_online_close_room() {
    {
        std::scoped_lock l(s_mx);
        if (!s_hosting && !s_opening) return;
        close_room_locked();
    }
    s_encoder.stop();
}

static void close_room_locked() {
    std::vector<uint64_t> ids;
    for (auto& [id, c] : s_clients) ids.push_back(id);
    for (uint64_t id : ids) remove_client_locked(id, true);
    s_hosting = false;
    s_opening = false;
    g_nl_stream_active = false;
    nl_net_accept_all(false);
    nl_steam_close_room();
    set_status("Raum geschlossen.");
    update_presence_locked();
}

bool nl_online_hosting() { std::scoped_lock l(s_mx); return s_hosting; }
bool nl_online_opening() { std::scoped_lock l(s_mx); return s_opening; }
std::string nl_online_code() { return nl_steam_room_code(); }
std::string nl_online_status() { std::scoped_lock l(s_mx); return s_status; }
std::string nl_online_encoder() { return s_encoder.running() ? s_encoder.encoder_name() : ""; }

std::vector<NLOnlinePlayer> nl_online_players() {
    std::scoped_lock l(s_mx);
    std::vector<NLOnlinePlayer> out;
    for (auto& [id, c] : s_clients) out.push_back({id, c.slot, c.name, nl_net_ping_ms(id)});
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.slot < b.slot; });
    return out;
}

void nl_online_kick(uint64_t id) {
    std::scoped_lock l(s_mx);
    remove_client_locked(id, true, MSG_KICK);
}

void nl_online_invite(uint64_t friend_id) { nl_steam_invite(friend_id); }

void nl_online_join(const std::string& code) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path client = std::filesystem::path(exe).parent_path() / "NLOnline.exe";
    if (!std::filesystem::exists(client)) {
        std::scoped_lock l(s_mx);
        set_status("NLOnline.exe fehlt neben dem Spiel.");
        return;
    }
    std::wstring cmd = L"\"" + client.wstring() + L"\" --join " + std::wstring(code.begin(), code.end());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, client.parent_path().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::scoped_lock l(s_mx);
        set_status("NLOnline wird gestartet ...");
    }
}

// Nintendo Land Recomp - Online als Host: Raum mit Code ueber Steam, TV-Bild und Ton an die
// Freunde streamen, ihre Controller als Wiimote-Spieler 2-5 einspeisen.
#include "nl_app.h"
#include "net/nl_proto.h"
#include "net/nl_steam.h"
#include "net/nl_video.h"
#include "net/nl_adpcm.h"
#include "net/nl_discord.h"

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
extern void (*g_nl_file_open_hook)(const char* path);

using namespace nlnet;
using clk = std::chrono::steady_clock;

namespace {
struct Client {
    uint64_t id = 0;
    int slot = 0;
    std::string name;
    uint32_t input_seq = 0;
    clk::time_point last_seen;
    // fuer die Bitrate-Regelung
    float loss = 0;            // zuletzt gemeldeter Bildverlust (0..1)
    bool ptr_logged = false;
    int ping_base = 0;         // niedrigster Ping (ohne Stau)
    clk::time_point base_time{};
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

// ---------------------------------------------------------------- Chat
std::mutex s_chat_mx;
std::vector<NLChatLine> s_chat;          // letzte Nachrichten (fuer die Anzeige)
std::string s_chat_input;

uint64_t now_ms() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now().time_since_epoch()).count(); }

void chat_add(const std::string& name, const std::string& text) {
    std::scoped_lock l(s_chat_mx);
    s_chat.push_back({name, text, now_ms(), name.empty()});
    if (s_chat.size() > 50) s_chat.erase(s_chat.begin());
}

// an alle Gaeste schicken (leerer Name = Systemmeldung). Ruft man mit s_mx gehalten auf.
void chat_broadcast_locked(const std::string& name, const std::string& text) {
    Chat c;
    strncpy(c.name, name.c_str(), sizeof(c.name) - 1);
    strncpy(c.text, text.c_str(), sizeof(c.text) - 1);
    for (auto& [id, cl] : s_clients) nl_net_send(id, CH_CONTROL, &c, sizeof(c), true);
}

void system_msg_locked(const std::string& text) {
    chat_add("", text);
    chat_broadcast_locked("", text);
}

// ---------------------------------------------------------------- Discord / Minispiel
std::mutex s_act_mx;
std::string s_attraction;            // gerade gespieltes Minispiel (leer = Plaza)
bool s_act_dirty = true;
const int64_t s_start_unix = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

void on_file_open(const char* path) {
    if (!path || !strstr(path, "/Package/")) return;
    cemuLog_log(LogType::Force, "NL: Paket geladen: {}", path);   // zum Lernen, in welcher Reihenfolge das Spiel laedt
    std::string a = nl_attraction_from_path(path);
    std::scoped_lock l(s_act_mx);
    if (!a.empty() && a != s_attraction) { s_attraction = a; s_act_dirty = true; }
    else if (a.empty() && strstr(path, "/Ttl_") && !s_attraction.empty()) { s_attraction.clear(); s_act_dirty = true; }
}

int local_players() {
    int n = 1;   // GamePad
    for (int i = 1; i <= 4; i++) n += nl_wii_slot(i).used ? 1 : 0;
    return n;
}

// Discord und Gaeste auf den neuesten Stand bringen (Netz-Thread, mit s_mx)
void update_activity_locked(bool force) {
    std::string a;
    {
        std::scoped_lock l(s_act_mx);
        if (!s_act_dirty && !force) return;
        s_act_dirty = false;
        a = s_attraction;
    }
    const int players = local_players();
    const std::string details = a.empty() ? "In the Plaza" : a;
    std::string state = s_hosting ? (s_clients.empty() ? "Online room open" : "Online with friends") : (players > 1 ? "Local multiplayer" : "Playing solo");
    nl_discord_set(details, state, s_hosting ? players : 0, s_hosting ? 5 : 0, s_start_unix);
    Activity m;
    strncpy(m.attraction, a.c_str(), sizeof(m.attraction) - 1);
    m.players = (uint8_t)players;
    m.start_unix = s_start_unix;
    for (auto& [id, c] : s_clients) nl_net_send(id, CH_CONTROL, &m, sizeof(m), true);
}

void update_peers_locked() {
    std::scoped_lock l(s_peer_mx);
    s_peers.clear();
    for (auto& [id, c] : s_clients) s_peers.push_back(id);
}

void update_presence_locked() {
    if (!s_hosting) { nl_steam_set_status("Spielt NL"); return; }
    nl_steam_set_status("Hostet einen Raum (" + std::to_string(s_clients.size() + 1) + "/5)");
}

// ---------------------------------------------------------------- Bild und Ton
void on_frame(const uint8* rgba, int w, int h, int stride, bool srgb) {
    (void)stride;
    s_encoder.submit_rgba(rgba, w, h, w * 4, srgb);
}

// Ein Bild in Stuecke <= kChunk teilen: grosse unzuverlaessige Nachrichten gehen sonst ganz verloren,
// sobald ein einziges UDP-Paket fehlt.
void on_packet(const uint8_t* data, size_t size, bool key, uint64_t) {
    VideoHeader hdr;
    hdr.frame = ++s_frame_no;
    hdr.count = (uint16_t)((size + kChunk - 1) / kChunk);
    hdr.total = (uint32_t)size;
    hdr.keyframe = key ? 1 : 0;
    hdr.width = (uint16_t)s_encoder.width();
    hdr.height = (uint16_t)s_encoder.height();
    hdr.t_host_us = now_us();
    uint8_t msg[sizeof(VideoHeader) + kChunk];
    std::scoped_lock l(s_peer_mx);
    for (uint16_t i = 0; i < hdr.count; i++) {
        const size_t off = (size_t)i * kChunk, n = std::min<size_t>(kChunk, size - off);
        hdr.index = i;
        memcpy(msg, &hdr, sizeof(hdr));
        memcpy(msg + sizeof(hdr), data + off, n);
        for (uint64_t id : s_peers) nl_net_send(id, CH_VIDEO, msg, sizeof(hdr) + n, false);
    }
}

// Bitrate an die schlechteste Verbindung anpassen.
// Stau erkennt man am steigenden Ping (der Router puffert), noch bevor Pakete verloren gehen.
int s_kbps_max = 6000, s_kbps_now = 3000;
clk::time_point s_last_down{}, s_last_up{}, s_last_adapt{};

void adapt_bitrate_locked() {
    const auto now = clk::now();
    if (!s_encoder.running() || s_clients.empty() || now - s_last_adapt < std::chrono::milliseconds(500)) return;
    s_last_adapt = now;
    float loss = 0;
    int over = 0;   // wie weit der Ping ueber dem Grundwert liegt (ms), schlechtester Gast
    for (auto& [id, c] : s_clients) {
        loss = std::max(loss, c.loss);
        c.loss = 0;
        const int p = nl_net_ping_ms(id);
        if (p < 0) continue;
        // Grundwert: kleinster Ping; alle 10 s darf er leicht steigen (Route kann sich aendern)
        if (c.ping_base == 0 || p < c.ping_base) { c.ping_base = p; c.base_time = now; }
        else if (now - c.base_time > std::chrono::seconds(10)) { c.ping_base += 3; c.base_time = now; }
        over = std::max(over, p - c.ping_base);
    }
    int k = s_kbps_now;
    if ((over > 45 || loss > 0.03f) && now - s_last_down > std::chrono::milliseconds(800)) {
        k = std::max(700, (int)(k * (over > 120 ? 0.6f : 0.78f)));
        s_last_down = now;
    } else if (over < 15 && loss < 0.01f && now - s_last_down > std::chrono::seconds(5) && now - s_last_up > std::chrono::seconds(2)) {
        k = std::min(s_kbps_max, (int)(k * 1.08f) + 50);
        s_last_up = now;
    }
    if (k != s_kbps_now) {
        s_kbps_now = k;
        s_encoder.set_bitrate(k);
        cemuLog_log(LogType::Force, "Online: Bitrate {} kbit/s (Verlust {:.0f}%, Ping +{} ms)", k, loss * 100, over);
    }
}

void on_audio(const sint16* samples, int frames, int channels) {
    if (!g_nl_stream_active) return;
    static std::vector<int16_t> pcm;
    static std::vector<uint8_t> packed, msg;
    static nladpcm::State st[2];   // laeuft nur im Ton-Thread
    pcm.resize((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        const sint16* f = samples + (size_t)i * channels;
        int l = f[0], r = channels > 1 ? f[1] : f[0];
        if (channels >= 6) {   // 5.1 -> Stereo
            l = (int)(f[0] + 0.7f * f[2] + 0.7f * f[4]);
            r = (int)(f[1] + 0.7f * f[2] + 0.7f * f[5]);
        }
        pcm[i * 2] = (int16_t)std::clamp(l, -32768, 32767);
        pcm[i * 2 + 1] = (int16_t)std::clamp(r, -32768, 32767);
    }
    nladpcm::encode_stereo(st, pcm.data(), frames, packed);   // 4x kleiner als rohes PCM
    AudioHeader hdr;
    hdr.frames = (uint16_t)frames;
    hdr.codec = AUDIO_ADPCM;
    hdr.seq = ++s_audio_seq;
    msg.resize(sizeof(hdr) + packed.size());
    memcpy(msg.data(), &hdr, sizeof(hdr));
    memcpy(msg.data() + sizeof(hdr), packed.data(), packed.size());
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
    { std::scoped_lock la(s_act_mx); s_act_dirty = true; }
    system_msg_locked(it->second.name + " ist gegangen.");
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
    s_kbps_max = q.kbps;
    s_kbps_now = q.kbps / 2;   // vorsichtig anfangen, dann hochregeln
    // Leinwand: TV (q.w x q.h) + GamePad (halbe Groesse) rechts daneben
    if (!s_encoder.start(q.w + q.w / 2, q.h, q.fps, s_kbps_now, on_packet)) {
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
            idle.native = true;
            idle.source = SRC_KEYBOARD;
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
        { std::scoped_lock la(s_act_mx); s_act_dirty = true; }
        system_msg_locked(name + " ist dabei (Wiimote " + std::to_string(slot) + ").");
        update_presence_locked();
    }
    if (!ensure_encoder_locked()) return;
    Welcome w;
    w.slot = (uint8_t)it->second.slot;
    w.width = (uint16_t)s_encoder.width();
    w.height = (uint16_t)s_encoder.height();
    w.fps = (uint16_t)s_encoder.fps();
    w.tv_w = (uint16_t)(g_nl_stream_w & ~3);
    w.tv_h = (uint16_t)(g_nl_stream_h & ~3);
    w.pad_x = w.tv_w;
    w.pad_y = 0;
    w.pad_w = (uint16_t)(w.tv_w / 2);
    w.pad_h = (uint16_t)(w.tv_h / 2);
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
            case MSG_CHAT:
                if (n >= sizeof(Chat) && it != s_clients.end()) {
                    Chat c;
                    memcpy(&c, d, sizeof(c));
                    std::string text(c.text, strnlen(c.text, sizeof(c.text)));
                    if (!text.empty()) {
                        chat_add(it->second.name, text);                   // Name vom Host, nicht vom Gast
                        chat_broadcast_locked(it->second.name, text);
                    }
                }
                break;
            case MSG_STATS:
                if (n >= sizeof(Stats) && it != s_clients.end()) {
                    Stats st;
                    memcpy(&st, d, sizeof(st));
                    const uint32_t total = st.frames_ok + st.frames_lost;
                    if (total >= 5) it->second.loss = std::max(it->second.loss, (float)st.frames_lost / total);
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
            w.native = true;
            w.wbuttons = in.wbuttons;
            w.nx = in.nx / 32767.0f;
            w.ny = in.ny / 32767.0f;
            w.sideways = (in.flags & IN_SIDEWAYS) != 0;
            w.has_ptr = (in.flags & IN_POINTER) != 0;
            w.ptr_ir = (in.flags & IN_PTR_IR) != 0;
            w.px = in.px / 65535.0f;
            w.py = in.py / 65535.0f;
            w.source = in.source;
            w.has_angles = true;
            w.yaw = in.yaw / 100.0f;
            w.pitch = in.pitch / 100.0f;
            w.roll = in.roll / 100.0f;
            w.shake = in.shake / 255.0f;
            w.recenter = in.recenter;
            w.imu_real = (in.flags & IN_IMU_REAL) != 0;
            for (int k = 0; k < 3; k++) { w.acc[k] = in.acc[k]; w.gyro[k] = in.gyro[k]; }
            w.sensor_us = in.sensor_us;
            w.battery = in.battery == 255 ? -1 : in.battery;
            w.ext = in.ext;
            if (!c.ptr_logged && (w.has_ptr || w.imu_real)) {   // einmal ins Log: hilft beim Testen
                c.ptr_logged = true;
                static const char* kSrc[] = {"Maus + Tastatur", "Controller", "echte Wiimote"};
                cemuLog_log(LogType::Force, "Online: {} spielt mit {} (Wiimote {}){}{}", c.name, kSrc[std::min<int>(in.source, 2)], c.slot,
                            w.has_ptr ? ", Zeiger kommt an" : "", w.imu_real ? ", echte Bewegungssensoren" : "");
            }
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
            adapt_bitrate_locked();
            static auto s_last_act = clk::now();
            if (clk::now() - s_last_act > std::chrono::seconds(2)) {   // auch lokale Wiimotes / Raum auf/zu
                s_last_act = clk::now();
                update_activity_locked(true);
            }
            update_activity_locked(false);
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
    g_nl_file_open_hook = on_file_open;
    nl_discord_start();
    s_quit = false;
    s_thread = std::thread(net_loop);   // laeuft auch ohne Steam (Discord, Minispiel-Erkennung)
    if (!nl_steam_available()) {
        s_status = "steam_api64.dll fehlt neben dem Spiel - im Launcher neu bauen.";
        return;
    }
    SetEnvironmentVariableW(L"SteamAppId", L"480");
    SetEnvironmentVariableW(L"SteamGameId", L"480");
    if (!nl_steam_init()) {
        s_status = "Steam laeuft nicht - starte Steam und dann das Spiel neu.";
        return;
    }
    nl_steam_set_status("Spielt NL");
    // Einladung angenommen, waehrend das Spiel laeuft -> als Gast beitreten
    nl_steam_on_join_request([](std::string code) { nl_online_join(code); });
    s_status = "Mit Steam verbunden als " + nl_steam_my_name() + ".";
}

void nl_online_shutdown() {
    nl_online_close_room();
    s_quit = true;
    if (s_thread.joinable()) s_thread.join();
    g_nl_stream_frame = nullptr;
    g_nl_stream_audio = nullptr;
    g_nl_file_open_hook = nullptr;
    nl_discord_stop();
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

// ===========================================================================
// Chat (Taste Y im Spiel)
// ===========================================================================
bool g_nl_chat_open = false;

bool nl_chat_available() {
    std::scoped_lock l(s_mx);
    return s_hosting;
}

void nl_chat_open() {
    if (!nl_chat_available()) return;
    std::scoped_lock l(s_chat_mx);
    s_chat_input.clear();
    g_nl_chat_open = true;
}

void nl_chat_close() {
    std::scoped_lock l(s_chat_mx);
    g_nl_chat_open = false;
    s_chat_input.clear();
}

std::string nl_chat_input() {
    std::scoped_lock l(s_chat_mx);
    return s_chat_input;
}

static void append_utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

void nl_chat_char(uint32_t ch) {
    if (!g_nl_chat_open || ch < 32 || ch == 127 || (ch >= 0xD800 && ch <= 0xDFFF)) return;
    std::scoped_lock l(s_chat_mx);
    if (s_chat_input.size() < sizeof(Chat::text) - 4) append_utf8(s_chat_input, ch);
}

void nl_chat_key(uint32_t vk) {
    if (!g_nl_chat_open) return;
    if (vk == VK_ESCAPE) { nl_chat_close(); return; }
    if (vk == VK_BACK) {
        std::scoped_lock l(s_chat_mx);
        while (!s_chat_input.empty()) {   // ganzes UTF-8-Zeichen loeschen
            const unsigned char c = (unsigned char)s_chat_input.back();
            s_chat_input.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        return;
    }
    if (vk == VK_RETURN) {
        std::string text = nl_chat_input();
        nl_chat_close();
        if (!text.empty()) nl_online_send_chat(text);
    }
}

void nl_online_send_chat(const std::string& text) {
    const std::string me = nl_steam_my_name();
    chat_add(me, text);
    std::scoped_lock l(s_mx);
    chat_broadcast_locked(me, text);
}

std::vector<NLChatLine> nl_online_chat_lines() {
    std::scoped_lock l(s_chat_mx);
    const size_t n = std::min<size_t>(s_chat.size(), 8);
    return std::vector<NLChatLine>(s_chat.end() - n, s_chat.end());
}

uint64_t nl_online_now_ms() { return now_ms(); }

// Nintendo Land Recomp - Steam-Anbindung (siehe nl_steam.h)
#include "nl_steam.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>

static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // ohne I, O, 0, 1

std::string nl_make_code() {
    static std::mt19937 rng{std::random_device{}()};
    std::string c;
    for (int i = 0; i < 6; i++) c += kAlphabet[rng() % (sizeof(kAlphabet) - 1)];
    return c;
}

std::string nl_normalize_code(const std::string& in) {
    std::string c;
    for (char ch : in) {
        ch = (char)toupper((unsigned char)ch);
        if (ch && strchr(kAlphabet, ch)) c += ch;   // Leerzeichen, Bindestriche usw. fallen weg
    }
    return c.substr(0, 6);
}

// ===========================================================================
// Steam ueber die flache C-API von steam_api64.dll (zur Laufzeit geladen, kein SDK zum Bauen noetig).
// Die Strukturen unten sind nur die Teile, die wir lesen; Groessen/Offsets wurden gegen die
// offiziellen Header (Windows, 64 Bit) geprueft.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <map>

namespace {
using SteamAPICall = uint64_t;
using HPipe = int32_t;
struct Ident { alignas(4) uint8_t raw[136]; };              // SteamNetworkingIdentity
struct NetMsg {                                              // SteamNetworkingMessage_t (Anfang)
    void* data;
    int32_t size;
    uint32_t conn;
    Ident peer;
};
static_assert(offsetof(NetMsg, size) == 8 && offsetof(NetMsg, peer) == 16, "Layout");
struct CallbackMsg { int32_t user; int32_t id; uint8_t* param; int32_t size; };
static_assert(sizeof(CallbackMsg) == 24, "Layout");
struct CallCompleted { uint64_t call; int32_t id; uint32_t size; };

constexpr int CB_LOBBY_JOIN_REQUESTED = 333, CB_RICH_JOIN_REQUESTED = 337;
constexpr int CB_SESSION_REQUEST = 1251, CB_CALL_COMPLETED = 703;
constexpr int SEND_UNRELIABLE = 0, SEND_NO_NAGLE = 1, SEND_RELIABLE = 8, SEND_AUTORESTART = 32;
constexpr int CFG_SEND_BUFFER = 9, CFG_SEND_RATE_MIN = 10, CFG_SEND_RATE_MAX = 11;
constexpr int FRIEND_IMMEDIATE = 4, LOBBY_PUBLIC = 2, CMP_EQUAL = 0, DIST_WORLDWIDE = 3, STATE_CONNECTED = 3;

// Funktionszeiger
HMODULE s_dll = nullptr;
#define NLF(ret, name, ...) ret (*name)(__VA_ARGS__) = nullptr
NLF(int, InitFlat, char*);
NLF(bool, InitOld);
NLF(void, Shutdown);
NLF(HPipe, GetPipe);
NLF(void, MD_Init);
NLF(void, MD_RunFrame, HPipe);
NLF(bool, MD_Next, HPipe, CallbackMsg*);
NLF(void, MD_Free, HPipe);
NLF(bool, MD_CallResult, HPipe, SteamAPICall, void*, int, int, bool*);
NLF(void*, A_Friends);
NLF(void*, A_User);
NLF(void*, A_Matchmaking);
NLF(void*, A_Messages);
NLF(void*, A_Utils);
NLF(const char*, F_GetPersonaName, void*);
NLF(int, F_GetFriendCount, void*, int);
NLF(uint64_t, F_GetFriendByIndex, void*, int, int);
NLF(const char*, F_GetFriendPersonaName, void*, uint64_t);
NLF(int, F_GetFriendPersonaState, void*, uint64_t);
NLF(bool, F_GetFriendGamePlayed, void*, uint64_t, void*);
NLF(const char*, F_GetFriendRichPresence, void*, uint64_t, const char*);
NLF(void, F_RequestFriendRichPresence, void*, uint64_t);
NLF(bool, F_SetRichPresence, void*, const char*, const char*);
NLF(void, F_ClearRichPresence, void*);
NLF(uint64_t, U_GetSteamID, void*);
NLF(SteamAPICall, M_CreateLobby, void*, int, int);
NLF(bool, M_SetLobbyData, void*, uint64_t, const char*, const char*);
NLF(const char*, M_GetLobbyData, void*, uint64_t, const char*);
NLF(void, M_AddStringFilter, void*, const char*, const char*, int);
NLF(void, M_AddDistanceFilter, void*, int);
NLF(void, M_AddCountFilter, void*, int);
NLF(SteamAPICall, M_RequestLobbyList, void*);
NLF(uint64_t, M_GetLobbyByIndex, void*, int);
NLF(SteamAPICall, M_JoinLobby, void*, uint64_t);
NLF(void, M_LeaveLobby, void*, uint64_t);
NLF(bool, M_InviteUserToLobby, void*, uint64_t, uint64_t);
NLF(int, N_Send, void*, const Ident*, const void*, uint32_t, int, int);
NLF(int, N_Receive, void*, int, NetMsg**, int);
NLF(bool, N_Accept, void*, const Ident*);
NLF(bool, N_Close, void*, const Ident*);
NLF(int, N_SessionInfo, void*, const Ident*, void*, void*);
NLF(void, U_InitRelay, void*);
NLF(bool, U_SetGlobalInt, void*, int, int32_t);
NLF(void, Msg_Release, NetMsg*);
NLF(void, Id_SetSteamID64, Ident*, uint64_t);
NLF(uint64_t, Id_GetSteamID64, Ident*);
#undef NLF

template <typename T> bool load(T& fn, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if ((fn = (T)GetProcAddress(s_dll, n))) return true;
    return false;
}

bool load_dll() {
    if (s_dll) return true;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    s_dll = LoadLibraryW((dir + L"steam_api64.dll").c_str());
    if (!s_dll) return false;
    bool ok = true;
    load(InitFlat, {"SteamAPI_InitFlat"});
    load(InitOld, {"SteamAPI_Init"});
    ok &= InitFlat || InitOld;
    ok &= load(Shutdown, {"SteamAPI_Shutdown"});
    ok &= load(GetPipe, {"SteamAPI_GetHSteamPipe"});
    ok &= load(MD_Init, {"SteamAPI_ManualDispatch_Init"});
    ok &= load(MD_RunFrame, {"SteamAPI_ManualDispatch_RunFrame"});
    ok &= load(MD_Next, {"SteamAPI_ManualDispatch_GetNextCallback"});
    ok &= load(MD_Free, {"SteamAPI_ManualDispatch_FreeLastCallback"});
    ok &= load(MD_CallResult, {"SteamAPI_ManualDispatch_GetAPICallResult"});
    ok &= load(A_Friends, {"SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v017"});
    ok &= load(A_User, {"SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021", "SteamAPI_SteamUser_v020"});
    ok &= load(A_Matchmaking, {"SteamAPI_SteamMatchmaking_v009"});
    ok &= load(A_Messages, {"SteamAPI_SteamNetworkingMessages_SteamAPI_v002"});
    ok &= load(A_Utils, {"SteamAPI_SteamNetworkingUtils_SteamAPI_v004", "SteamAPI_SteamNetworkingUtils_SteamAPI_v003"});
    ok &= load(F_GetPersonaName, {"SteamAPI_ISteamFriends_GetPersonaName"});
    ok &= load(F_GetFriendCount, {"SteamAPI_ISteamFriends_GetFriendCount"});
    ok &= load(F_GetFriendByIndex, {"SteamAPI_ISteamFriends_GetFriendByIndex"});
    ok &= load(F_GetFriendPersonaName, {"SteamAPI_ISteamFriends_GetFriendPersonaName"});
    ok &= load(F_GetFriendPersonaState, {"SteamAPI_ISteamFriends_GetFriendPersonaState"});
    ok &= load(F_GetFriendGamePlayed, {"SteamAPI_ISteamFriends_GetFriendGamePlayed"});
    ok &= load(F_GetFriendRichPresence, {"SteamAPI_ISteamFriends_GetFriendRichPresence"});
    ok &= load(F_RequestFriendRichPresence, {"SteamAPI_ISteamFriends_RequestFriendRichPresence"});
    ok &= load(F_SetRichPresence, {"SteamAPI_ISteamFriends_SetRichPresence"});
    ok &= load(F_ClearRichPresence, {"SteamAPI_ISteamFriends_ClearRichPresence"});
    ok &= load(U_GetSteamID, {"SteamAPI_ISteamUser_GetSteamID"});
    ok &= load(M_CreateLobby, {"SteamAPI_ISteamMatchmaking_CreateLobby"});
    ok &= load(M_SetLobbyData, {"SteamAPI_ISteamMatchmaking_SetLobbyData"});
    ok &= load(M_GetLobbyData, {"SteamAPI_ISteamMatchmaking_GetLobbyData"});
    ok &= load(M_AddStringFilter, {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter"});
    ok &= load(M_AddDistanceFilter, {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter"});
    ok &= load(M_AddCountFilter, {"SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter"});
    ok &= load(M_RequestLobbyList, {"SteamAPI_ISteamMatchmaking_RequestLobbyList"});
    ok &= load(M_GetLobbyByIndex, {"SteamAPI_ISteamMatchmaking_GetLobbyByIndex"});
    ok &= load(M_JoinLobby, {"SteamAPI_ISteamMatchmaking_JoinLobby"});
    ok &= load(M_LeaveLobby, {"SteamAPI_ISteamMatchmaking_LeaveLobby"});
    ok &= load(M_InviteUserToLobby, {"SteamAPI_ISteamMatchmaking_InviteUserToLobby"});
    ok &= load(N_Send, {"SteamAPI_ISteamNetworkingMessages_SendMessageToUser"});
    ok &= load(N_Receive, {"SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"});
    ok &= load(N_Accept, {"SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"});
    ok &= load(N_Close, {"SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser"});
    ok &= load(N_SessionInfo, {"SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo"});
    ok &= load(U_InitRelay, {"SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess"});
    ok &= load(U_SetGlobalInt, {"SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32"});
    ok &= load(Msg_Release, {"SteamAPI_SteamNetworkingMessage_t_Release"});
    ok &= load(Id_SetSteamID64, {"SteamAPI_SteamNetworkingIdentity_SetSteamID64"});
    ok &= load(Id_GetSteamID64, {"SteamAPI_SteamNetworkingIdentity_GetSteamID64"});
    if (!ok) {
        FreeLibrary(s_dll);
        s_dll = nullptr;
    }
    return ok;
}

std::recursive_mutex s_mx;
bool s_ok = false;
bool s_accept_all = false;
HPipe s_pipe = 0;
void *s_friends = nullptr, *s_user = nullptr, *s_mm = nullptr, *s_msgs = nullptr;
uint64_t s_lobby = 0;
std::string s_code;
std::function<void(std::string)> s_on_join;
std::string s_pending_join;
std::map<SteamAPICall, std::function<void(const uint8_t*, bool)>> s_calls;   // offene Anfragen

Ident ident(uint64_t id) {
    Ident i{};
    Id_SetSteamID64(&i, id);
    return i;
}
std::string str(const char* p) { return p ? p : ""; }

void on_callback(int id, const uint8_t* p) {
    if (id == CB_LOBBY_JOIN_REQUESTED) {   // Einladung im Steam-Overlay angenommen
        uint64_t lobby;
        memcpy(&lobby, p, 8);
        SteamAPICall c = M_JoinLobby(s_mm, lobby);
        s_calls[c] = [](const uint8_t* r, bool fail) {
            if (fail) return;
            uint64_t l;
            memcpy(&l, r, 8);
            std::string code = str(M_GetLobbyData(s_mm, l, "code"));
            M_LeaveLobby(s_mm, l);   // wir brauchen nur den Code
            if (!code.empty()) s_pending_join = code;
        };
    } else if (id == CB_RICH_JOIN_REQUESTED) {
        std::string c((const char*)p + 8, strnlen((const char*)p + 8, 256));
        auto pos = c.find("+nl_join ");
        if (pos != std::string::npos) s_pending_join = nl_normalize_code(c.substr(pos + 9));
    } else if (id == CB_SESSION_REQUEST) {
        if (s_accept_all) N_Accept(s_msgs, (const Ident*)p);
    }
}
}  // namespace

bool nl_steam_available() { return load_dll(); }

bool nl_steam_init() {
    std::scoped_lock l(s_mx);
    if (s_ok) return true;
    if (!load_dll()) return false;
    char err[1024] = {};
    bool ok = InitFlat ? InitFlat(err) == 0 : InitOld();
    if (!ok) return false;
    MD_Init();
    s_pipe = GetPipe();
    s_friends = A_Friends();
    s_user = A_User();
    s_mm = A_Matchmaking();
    s_msgs = A_Messages();
    if (!s_friends || !s_user || !s_mm || !s_msgs) { Shutdown(); return false; }
    s_ok = true;
    void* utils = A_Utils();
    U_InitRelay(utils);
    // Standard ist nur 256 KB/s (~2 Mbit/s): der Stream fror ein. Min = Max (so empfiehlt es Valve) ergibt
    // gleichmaessiges Senden; die eigentliche Datenmenge regelt der Host ueber die Bitrate. Kleiner Puffer,
    // damit sich bei langsamer Leitung keine Sekunden an Bild aufstauen.
    U_SetGlobalInt(utils, CFG_SEND_RATE_MIN, 2 * 1024 * 1024);
    U_SetGlobalInt(utils, CFG_SEND_RATE_MAX, 2 * 1024 * 1024);
    U_SetGlobalInt(utils, CFG_SEND_BUFFER, 1024 * 1024);
    F_SetRichPresence(s_friends, "nlr", "1");
    F_SetRichPresence(s_friends, "status", "NL Recomp");
    nl_steam_friends();
    return true;
}

void nl_steam_shutdown() {
    std::scoped_lock l(s_mx);
    if (!s_ok) return;
    nl_steam_close_room();
    F_ClearRichPresence(s_friends);
    Shutdown();
    s_ok = false;
}

bool nl_steam_ok() { return s_ok; }

void nl_steam_tick() {
    std::function<void(std::string)> cb;
    std::string code;
    {
        std::scoped_lock l(s_mx);
        if (!s_ok) return;
        MD_RunFrame(s_pipe);
        CallbackMsg m{};
        while (MD_Next(s_pipe, &m)) {
            if (m.id == CB_CALL_COMPLETED) {
                CallCompleted cc;
                memcpy(&cc, m.param, sizeof(cc));
                auto it = s_calls.find(cc.call);
                if (it != s_calls.end()) {
                    std::vector<uint8_t> buf(std::max<uint32_t>(cc.size, 8));
                    bool failed = false;
                    bool got = MD_CallResult(s_pipe, cc.call, buf.data(), (int)cc.size, cc.id, &failed);
                    auto fn = std::move(it->second);
                    s_calls.erase(it);
                    fn(buf.data(), !got || failed);
                }
            } else {
                on_callback(m.id, m.param);
            }
            MD_Free(s_pipe);
        }
        if (!s_pending_join.empty() && s_on_join) {
            cb = s_on_join;
            code = s_pending_join;
            s_pending_join.clear();
        }
    }
    if (cb) cb(code);
}

uint64_t nl_steam_me() {
    std::scoped_lock l(s_mx);
    return s_ok ? U_GetSteamID(s_user) : 0;
}

std::string nl_steam_my_name() {
    std::scoped_lock l(s_mx);
    return s_ok ? str(F_GetPersonaName(s_friends)) : "Spieler";
}

std::string nl_steam_name_of(uint64_t id) {
    std::scoped_lock l(s_mx);
    return s_ok ? str(F_GetFriendPersonaName(s_friends, id)) : "?";
}

std::vector<NLFriend> nl_steam_friends() {
    std::scoped_lock l(s_mx);
    std::vector<NLFriend> out;
    if (!s_ok) return out;
    static auto s_last_request = std::chrono::steady_clock::time_point{};
    const bool request = std::chrono::steady_clock::now() - s_last_request > std::chrono::seconds(10);
    if (request) s_last_request = std::chrono::steady_clock::now();
    const int n = F_GetFriendCount(s_friends, FRIEND_IMMEDIATE);
    for (int i = 0; i < n; i++) {
        uint64_t id = F_GetFriendByIndex(s_friends, i, FRIEND_IMMEDIATE);
        NLFriend f;
        f.id = id;
        f.name = str(F_GetFriendPersonaName(s_friends, id));
        f.online = F_GetFriendPersonaState(s_friends, id) != 0;
        alignas(8) uint8_t gi[24] = {};   // FriendGameInfo_t
        if (F_GetFriendGamePlayed(s_friends, id, gi)) {
            uint64_t game_id;
            memcpy(&game_id, gi, 8);
            if ((game_id & 0xFFFFFF) == 480) {
                if (request) F_RequestFriendRichPresence(s_friends, id);
                if (str(F_GetFriendRichPresence(s_friends, id, "nlr")) == "1") {
                    f.in_nl = true;
                    f.code = str(F_GetFriendRichPresence(s_friends, id, "nl_code"));
                    f.status = str(F_GetFriendRichPresence(s_friends, id, "status"));
                }
            }
        }
        if (f.online) out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const NLFriend& a, const NLFriend& b) {
        if (a.in_nl != b.in_nl) return a.in_nl;
        if (a.code.empty() != b.code.empty()) return !a.code.empty();
        std::string x = a.name, y = b.name;
        for (auto& c : x) c = (char)tolower((unsigned char)c);
        for (auto& c : y) c = (char)tolower((unsigned char)c);
        return x < y;
    });
    return out;
}

void nl_steam_host_room(int max_players, std::function<void(bool, std::string)> done) {
    std::scoped_lock l(s_mx);
    if (!s_ok) { if (done) done(false, ""); return; }
    if (s_lobby) { if (done) done(true, s_code); return; }
    SteamAPICall c = M_CreateLobby(s_mm, LOBBY_PUBLIC, max_players);
    s_calls[c] = [done](const uint8_t* r, bool fail) {
        int32_t result;
        uint64_t lobby;
        memcpy(&result, r, 4);
        memcpy(&lobby, r + 8, 8);
        if (fail || result != 1 /*k_EResultOK*/ || !lobby) { if (done) done(false, ""); return; }
        s_lobby = lobby;
        s_code = nl_make_code();
        M_SetLobbyData(s_mm, lobby, "nlr", "1");
        M_SetLobbyData(s_mm, lobby, "nlv", "1");
        M_SetLobbyData(s_mm, lobby, "code", s_code.c_str());
        M_SetLobbyData(s_mm, lobby, "host", std::to_string(U_GetSteamID(s_user)).c_str());
        M_SetLobbyData(s_mm, lobby, "name", F_GetPersonaName(s_friends));
        F_SetRichPresence(s_friends, "nl_code", s_code.c_str());
        F_SetRichPresence(s_friends, "connect", ("+nl_join " + s_code).c_str());
        if (done) done(true, s_code);
    };
}

void nl_steam_close_room() {
    std::scoped_lock l(s_mx);
    if (!s_ok) return;
    if (s_lobby) M_LeaveLobby(s_mm, s_lobby);
    s_lobby = 0;
    s_code.clear();
    F_SetRichPresence(s_friends, "nl_code", nullptr);
    F_SetRichPresence(s_friends, "connect", nullptr);
}

std::string nl_steam_room_code() {
    std::scoped_lock l(s_mx);
    return s_code;
}

void nl_steam_invite(uint64_t friend_id) {
    std::scoped_lock l(s_mx);
    if (s_ok && s_lobby) M_InviteUserToLobby(s_mm, s_lobby, friend_id);
}

void nl_steam_find_code(const std::string& code_in, std::function<void(bool, uint64_t, std::string)> done) {
    std::scoped_lock l(s_mx);
    if (!s_ok) { if (done) done(false, 0, "Steam laeuft nicht."); return; }
    std::string code = nl_normalize_code(code_in);
    if (code.size() != 6) { if (done) done(false, 0, "Der Code hat 6 Zeichen."); return; }
    M_AddStringFilter(s_mm, "nlr", "1", CMP_EQUAL);
    M_AddStringFilter(s_mm, "code", code.c_str(), CMP_EQUAL);
    M_AddDistanceFilter(s_mm, DIST_WORLDWIDE);
    M_AddCountFilter(s_mm, 5);
    SteamAPICall c = M_RequestLobbyList(s_mm);
    s_calls[c] = [done, code](const uint8_t* r, bool fail) {
        if (!done) return;
        if (fail) { done(false, 0, "Steam antwortet nicht."); return; }
        uint32_t count;
        memcpy(&count, r, 4);
        for (uint32_t i = 0; i < count; i++) {
            uint64_t lobby = M_GetLobbyByIndex(s_mm, (int)i);
            if (code != str(M_GetLobbyData(s_mm, lobby, "code"))) continue;
            uint64_t host = strtoull(str(M_GetLobbyData(s_mm, lobby, "host")).c_str(), nullptr, 10);
            if (host) { done(true, host, ""); return; }
        }
        done(false, 0, "Kein Raum mit diesem Code gefunden.");
    };
}

void nl_steam_on_join_request(std::function<void(std::string)> fn) {
    std::scoped_lock l(s_mx);
    s_on_join = std::move(fn);
}

void nl_steam_set_status(const std::string& text) {
    std::scoped_lock l(s_mx);
    if (s_ok) F_SetRichPresence(s_friends, "status", text.c_str());
}

bool nl_net_send(uint64_t to, int channel, const void* data, size_t size, bool reliable) {
    if (!s_ok) return false;
    Ident id = ident(to);
    int flags = (reliable ? SEND_RELIABLE : SEND_UNRELIABLE) | SEND_NO_NAGLE | SEND_AUTORESTART;
    return N_Send(s_msgs, &id, data, (uint32_t)size, flags, channel) == 1 /*k_EResultOK*/;
}

int nl_net_receive(int channel, const std::function<void(uint64_t, const uint8_t*, size_t)>& fn) {
    if (!s_ok) return 0;
    NetMsg* msgs[32];
    int total = 0;
    for (;;) {
        int n = N_Receive(s_msgs, channel, msgs, 32);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            fn(Id_GetSteamID64(&msgs[i]->peer), (const uint8_t*)msgs[i]->data, (size_t)msgs[i]->size);
            Msg_Release(msgs[i]);
        }
        total += n;
        if (n < 32) break;
    }
    return total;
}

void nl_net_close(uint64_t peer) {
    if (!s_ok) return;
    Ident id = ident(peer);
    N_Close(s_msgs, &id);
}

void nl_net_accept_all(bool accept) { s_accept_all = accept; }

int nl_net_ping_ms(uint64_t peer) {
    if (!s_ok) return -1;
    Ident id = ident(peer);
    alignas(8) uint8_t info[2048] = {};   // SteamNetConnectionInfo_t (696 Byte)
    alignas(8) uint8_t st[512] = {};      // SteamNetConnectionRealTimeStatus_t (120 Byte), m_nPing bei 4
    if (N_SessionInfo(s_msgs, &id, info, st) != STATE_CONNECTED) return -1;
    int32_t ping;
    memcpy(&ping, st + 4, 4);
    return ping;
}

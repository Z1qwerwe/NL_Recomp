// Nintendo Land Recomp - Steam-Anbindung (siehe nl_steam.h)
#include "nl_steam.h"

#include <algorithm>
#include <cctype>
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

#ifndef NL_STEAM
// ---------------------------------------------------------------- ohne SDK
bool nl_steam_available() { return false; }
bool nl_steam_init() { return false; }
void nl_steam_shutdown() {}
bool nl_steam_ok() { return false; }
void nl_steam_tick() {}
uint64_t nl_steam_me() { return 0; }
std::string nl_steam_my_name() { return "Spieler"; }
std::string nl_steam_name_of(uint64_t) { return "?"; }
std::vector<NLFriend> nl_steam_friends() { return {}; }
void nl_steam_host_room(int, std::function<void(bool, std::string)> done) { if (done) done(false, ""); }
void nl_steam_close_room() {}
std::string nl_steam_room_code() { return ""; }
void nl_steam_invite(uint64_t) {}
void nl_steam_find_code(const std::string&, std::function<void(bool, uint64_t, std::string)> done) {
    if (done) done(false, 0, "Online ist in diesem Build nicht dabei (Steamworks SDK fehlt).");
}
void nl_steam_on_join_request(std::function<void(std::string)>) {}
void nl_steam_set_status(const std::string&) {}
bool nl_net_send(uint64_t, int, const void*, size_t, bool) { return false; }
int nl_net_receive(int, const std::function<void(uint64_t, const uint8_t*, size_t)>&) { return 0; }
void nl_net_close(uint64_t) {}
void nl_net_accept_all(bool) {}
int nl_net_ping_ms(uint64_t) { return -1; }

#else
// ---------------------------------------------------------------- mit Steamworks SDK
#include "steam/steam_api.h"
#include "steam/isteamnetworkingmessages.h"
#include "steam/isteamnetworkingutils.h"

static std::recursive_mutex s_mx;
static bool s_ok = false;
static bool s_accept_all = false;
static CSteamID s_lobby;
static std::string s_code;
static std::function<void(std::string)> s_on_join;
static std::string s_pending_join;   // Code, der beim naechsten tick gemeldet wird

class NLSteam {
public:
    // Raum erstellen
    CCallResult<NLSteam, LobbyCreated_t> cr_created;
    std::function<void(bool, std::string)> created_done;
    int max_players = 5;
    void OnCreated(LobbyCreated_t* p, bool io_fail) {
        if (io_fail || p->m_eResult != k_EResultOK) {
            if (created_done) created_done(false, "");
            return;
        }
        s_lobby = CSteamID(p->m_ulSteamIDLobby);
        s_code = nl_make_code();
        auto* mm = SteamMatchmaking();
        mm->SetLobbyData(s_lobby, "nlr", "1");
        mm->SetLobbyData(s_lobby, "nlv", "1");
        mm->SetLobbyData(s_lobby, "code", s_code.c_str());
        mm->SetLobbyData(s_lobby, "host", std::to_string(SteamUser()->GetSteamID().ConvertToUint64()).c_str());
        mm->SetLobbyData(s_lobby, "name", SteamFriends()->GetPersonaName());
        SteamFriends()->SetRichPresence("nl_code", s_code.c_str());
        SteamFriends()->SetRichPresence("connect", ("+nl_join " + s_code).c_str());
        if (created_done) created_done(true, s_code);
    }

    // Code suchen
    CCallResult<NLSteam, LobbyMatchList_t> cr_list;
    std::function<void(bool, uint64_t, std::string)> find_done;
    std::string find_code;
    void OnList(LobbyMatchList_t* p, bool io_fail) {
        auto done = find_done;
        find_done = nullptr;
        if (!done) return;
        if (io_fail) { done(false, 0, "Steam antwortet nicht."); return; }
        for (uint32 i = 0; i < p->m_nLobbiesMatching; i++) {
            CSteamID l = SteamMatchmaking()->GetLobbyByIndex(i);
            if (find_code != SteamMatchmaking()->GetLobbyData(l, "code")) continue;
            uint64_t host = strtoull(SteamMatchmaking()->GetLobbyData(l, "host"), nullptr, 10);
            if (host) { done(true, host, ""); return; }
        }
        done(false, 0, "Kein Raum mit diesem Code gefunden.");
    }

    // Einladung angenommen (Steam-Overlay / Chat)
    STEAM_CALLBACK(NLSteam, OnLobbyJoinRequested, GameLobbyJoinRequested_t);
    CCallResult<NLSteam, LobbyEnter_t> cr_enter;
    void OnEnter(LobbyEnter_t* p, bool io_fail) {
        if (io_fail) return;
        CSteamID l(p->m_ulSteamIDLobby);
        std::string code = SteamMatchmaking()->GetLobbyData(l, "code");
        SteamMatchmaking()->LeaveLobby(l);   // wir brauchen nur den Code
        if (!code.empty()) s_pending_join = code;
    }
    STEAM_CALLBACK(NLSteam, OnRichJoin, GameRichPresenceJoinRequested_t);

    // P2P: Verbindungswunsch
    STEAM_CALLBACK(NLSteam, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
};

void NLSteam::OnLobbyJoinRequested(GameLobbyJoinRequested_t* p) {
    cr_enter.Set(SteamMatchmaking()->JoinLobby(p->m_steamIDLobby), this, &NLSteam::OnEnter);
}
void NLSteam::OnRichJoin(GameRichPresenceJoinRequested_t* p) {
    std::string c = p->m_rgchConnect;
    auto pos = c.find("+nl_join ");
    if (pos != std::string::npos) s_pending_join = nl_normalize_code(c.substr(pos + 9));
}
void NLSteam::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* p) {
    if (s_accept_all) SteamNetworkingMessages()->AcceptSessionWithUser(p->m_identityRemote);
}

static NLSteam* s_steam = nullptr;

bool nl_steam_available() { return true; }

bool nl_steam_init() {
    std::scoped_lock l(s_mx);
    if (s_ok) return true;
    if (!SteamAPI_Init()) return false;
    s_ok = true;
    s_steam = new NLSteam();
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    SteamFriends()->SetRichPresence("nlr", "1");
    SteamFriends()->SetRichPresence("status", "Nintendo Land Recomp");
    // Rich Presence der Freunde anfordern, die gerade Spacewar (= uns) laufen haben
    nl_steam_friends();
    return true;
}

void nl_steam_shutdown() {
    std::scoped_lock l(s_mx);
    if (!s_ok) return;
    nl_steam_close_room();
    SteamFriends()->ClearRichPresence();
    delete s_steam;
    s_steam = nullptr;
    SteamAPI_Shutdown();
    s_ok = false;
}

bool nl_steam_ok() { return s_ok; }

void nl_steam_tick() {
    std::function<void(std::string)> cb;
    std::string code;
    {
        std::scoped_lock l(s_mx);
        if (!s_ok) return;
        SteamAPI_RunCallbacks();
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
    return s_ok ? SteamUser()->GetSteamID().ConvertToUint64() : 0;
}

std::string nl_steam_my_name() {
    std::scoped_lock l(s_mx);
    return s_ok ? SteamFriends()->GetPersonaName() : "Spieler";
}

std::string nl_steam_name_of(uint64_t id) {
    std::scoped_lock l(s_mx);
    if (!s_ok) return "?";
    return SteamFriends()->GetFriendPersonaName(CSteamID((uint64)id));
}

std::vector<NLFriend> nl_steam_friends() {
    std::scoped_lock l(s_mx);
    std::vector<NLFriend> out;
    if (!s_ok) return out;
    auto* fr = SteamFriends();
    static auto s_last_request = std::chrono::steady_clock::time_point{};
    const bool request = std::chrono::steady_clock::now() - s_last_request > std::chrono::seconds(10);
    if (request) s_last_request = std::chrono::steady_clock::now();
    const int n = fr->GetFriendCount(k_EFriendFlagImmediate);
    for (int i = 0; i < n; i++) {
        CSteamID id = fr->GetFriendByIndex(i, k_EFriendFlagImmediate);
        NLFriend f;
        f.id = id.ConvertToUint64();
        f.name = fr->GetFriendPersonaName(id);
        f.online = fr->GetFriendPersonaState(id) != k_EPersonaStateOffline;
        FriendGameInfo_t gi{};
        if (fr->GetFriendGamePlayed(id, &gi) && gi.m_gameID.AppID() == 480) {
            if (request) fr->RequestFriendRichPresence(id);
            if (std::string(fr->GetFriendRichPresence(id, "nlr")) == "1") {
                f.in_nl = true;
                f.code = fr->GetFriendRichPresence(id, "nl_code");
                f.status = fr->GetFriendRichPresence(id, "status");
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
    if (s_lobby.IsValid()) { if (done) done(true, s_code); return; }
    s_steam->created_done = done;
    s_steam->max_players = max_players;
    s_steam->cr_created.Set(SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, max_players), s_steam, &NLSteam::OnCreated);
}

void nl_steam_close_room() {
    std::scoped_lock l(s_mx);
    if (!s_ok) return;
    if (s_lobby.IsValid()) SteamMatchmaking()->LeaveLobby(s_lobby);
    s_lobby = CSteamID();
    s_code.clear();
    SteamFriends()->SetRichPresence("nl_code", nullptr);
    SteamFriends()->SetRichPresence("connect", nullptr);
}

std::string nl_steam_room_code() {
    std::scoped_lock l(s_mx);
    return s_code;
}

void nl_steam_invite(uint64_t friend_id) {
    std::scoped_lock l(s_mx);
    if (s_ok && s_lobby.IsValid()) SteamMatchmaking()->InviteUserToLobby(s_lobby, CSteamID((uint64)friend_id));
}

void nl_steam_find_code(const std::string& code_in, std::function<void(bool, uint64_t, std::string)> done) {
    std::scoped_lock l(s_mx);
    if (!s_ok) { if (done) done(false, 0, "Steam laeuft nicht."); return; }
    std::string code = nl_normalize_code(code_in);
    if (code.size() != 6) { if (done) done(false, 0, "Der Code hat 6 Zeichen."); return; }
    auto* mm = SteamMatchmaking();
    mm->AddRequestLobbyListStringFilter("nlr", "1", k_ELobbyComparisonEqual);
    mm->AddRequestLobbyListStringFilter("code", code.c_str(), k_ELobbyComparisonEqual);
    mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    mm->AddRequestLobbyListResultCountFilter(5);
    s_steam->find_code = code;
    s_steam->find_done = done;
    s_steam->cr_list.Set(mm->RequestLobbyList(), s_steam, &NLSteam::OnList);
}

void nl_steam_on_join_request(std::function<void(std::string)> fn) {
    std::scoped_lock l(s_mx);
    s_on_join = std::move(fn);
}

void nl_steam_set_status(const std::string& text) {
    std::scoped_lock l(s_mx);
    if (s_ok) SteamFriends()->SetRichPresence("status", text.c_str());
}

static SteamNetworkingIdentity ident(uint64_t id) {
    SteamNetworkingIdentity i;
    i.SetSteamID64((uint64)id);
    return i;
}

bool nl_net_send(uint64_t to, int channel, const void* data, size_t size, bool reliable) {
    if (!s_ok) return false;
    int flags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable) | k_nSteamNetworkingSend_AutoRestartBrokenSession;
    return SteamNetworkingMessages()->SendMessageToUser(ident(to), data, (uint32)size, flags, channel) == k_EResultOK;
}

int nl_net_receive(int channel, const std::function<void(uint64_t, const uint8_t*, size_t)>& fn) {
    if (!s_ok) return 0;
    SteamNetworkingMessage_t* msgs[32];
    int total = 0;
    for (;;) {
        int n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(channel, msgs, 32);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            fn(msgs[i]->m_identityPeer.GetSteamID64(), (const uint8_t*)msgs[i]->m_pData, (size_t)msgs[i]->m_cbSize);
            msgs[i]->Release();
        }
        total += n;
        if (n < 32) break;
    }
    return total;
}

void nl_net_close(uint64_t peer) {
    if (s_ok) SteamNetworkingMessages()->CloseSessionWithUser(ident(peer));
}

void nl_net_accept_all(bool accept) { s_accept_all = accept; }

int nl_net_ping_ms(uint64_t peer) {
    if (!s_ok) return -1;
    SteamNetConnectionInfo_t info{};
    SteamNetConnectionRealTimeStatus_t st{};
    auto state = SteamNetworkingMessages()->GetSessionConnectionInfo(ident(peer), &info, &st);
    if (state != k_ESteamNetworkingConnectionState_Connected) return -1;
    return st.m_nPing;
}
#endif

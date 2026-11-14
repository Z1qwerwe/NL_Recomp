// Nintendo Land Recomp - Steam: Freundesliste, Raeume mit Code, Einladungen, P2P-Nachrichten.
// Laeuft ueber die Steam-Test-App "Spacewar" (AppID 480). Ohne Steamworks SDK (NL_STEAM nicht
// definiert) sind alle Funktionen harmlose Platzhalter und nl_steam_available() ist false.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct NLFriend {
    uint64_t id = 0;
    std::string name;
    bool online = false;
    bool in_nl = false;      // hat Nintendo Land Recomp offen
    std::string code;        // hostet gerade einen Raum mit diesem Code
    std::string status;      // Text wie "Hostet einen Raum"
};

bool nl_steam_available();                 // mit SDK gebaut?
bool nl_steam_init();                      // Steam verbinden (false: Steam laeuft nicht)
void nl_steam_shutdown();
bool nl_steam_ok();                        // verbunden
void nl_steam_tick();                      // Callbacks verarbeiten (regelmaessig aufrufen)
uint64_t nl_steam_me();
std::string nl_steam_my_name();
std::string nl_steam_name_of(uint64_t id);

std::vector<NLFriend> nl_steam_friends();  // online zuerst, Nintendo-Land-Spieler ganz oben

// Raum (Host)
void nl_steam_host_room(int max_players, std::function<void(bool ok, std::string code)> done);
void nl_steam_close_room();
std::string nl_steam_room_code();          // leer = kein Raum
void nl_steam_invite(uint64_t friend_id);

// Beitreten (Client): Code -> Steam-ID des Hosts
void nl_steam_find_code(const std::string& code, std::function<void(bool ok, uint64_t host, std::string err)> done);
// Einladung oder "Beitreten" im Steam-Overlay -> Code (wird bei nl_steam_tick ausgeloest)
void nl_steam_on_join_request(std::function<void(std::string code)> fn);

void nl_steam_set_status(const std::string& text);   // Rich Presence

// P2P (verbindungslos, Steam kuemmert sich um NAT/Relay)
bool nl_net_send(uint64_t to, int channel, const void* data, size_t size, bool reliable);
// ruft fn fuer jede wartende Nachricht auf dem Kanal auf, liefert die Anzahl
int nl_net_receive(int channel, const std::function<void(uint64_t from, const uint8_t* data, size_t size)>& fn);
void nl_net_close(uint64_t peer);
void nl_net_accept_all(bool accept);       // Host: Verbindungswuensche annehmen
int nl_net_ping_ms(uint64_t peer);         // -1 = unbekannt

std::string nl_make_code();                // 6 Zeichen, ohne verwechselbare Zeichen
std::string nl_normalize_code(const std::string& in);

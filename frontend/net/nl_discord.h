// NL Recomp - Discord Rich Presence ueber die lokale Discord-Schnittstelle (Named Pipe), ohne Zusatz-Bibliothek.
// Laeuft in einem eigenen Thread; ist Discord nicht offen, passiert einfach nichts (es wird alle paar
// Sekunden neu versucht).
#pragma once
#include <cstdint>
#include <string>

constexpr const char* kNLDiscordAppId = "1571560465971155074";

void nl_discord_start();
void nl_discord_stop();
// details = obere Zeile (z. B. Minispiel), state = untere Zeile (z. B. "Online: 3 von 5")
// party_size/party_max = 0 -> keine Gruppe anzeigen; start_unix = 0 -> keine Zeit anzeigen
void nl_discord_set(const std::string& details, const std::string& state, int party_size = 0, int party_max = 0,
                    int64_t start_unix = 0);

// Minispiel aus einem Dateipfad erkennen (".../Package/Mro_Cmn.pack" -> "Mario Chase"); leer = unbekannt
std::string nl_attraction_from_path(const std::string& path);

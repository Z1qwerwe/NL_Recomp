// Nintendo Land Recomp - Online-Protokoll zwischen Host (NintendoLand.exe) und Client (NLOnline.exe)
//
// Prinzip: Nur der Host laesst das Spiel laufen. Er schickt das TV-Bild (H.264) und den TV-Ton
// an die Clients; die Clients schicken nur ihre Controller-Eingaben zurueck und werden beim Host
// zu Wiimote-Spielern (Spieler 2-5).
#pragma once
#include <cstdint>

namespace nlnet {

constexpr uint32_t kVersion = 1;
constexpr int kMaxRemote = 4;   // Wiimote 1-4

// Kanaele (ISteamNetworkingMessages)
enum Channel : int {
    CH_CONTROL = 0,   // zuverlaessig: Hallo, Willkommen, Tschuess, Keyframe-Wunsch, Ping
    CH_VIDEO = 1,     // unzuverlaessig: H.264-Bilder
    CH_AUDIO = 2,     // unzuverlaessig: PCM-Ton
    CH_INPUT = 3,     // unzuverlaessig: Controller-Zustand (Client -> Host)
};

enum MsgType : uint8_t {
    MSG_HELLO = 1,      // Client -> Host
    MSG_WELCOME = 2,    // Host -> Client
    MSG_FULL = 3,       // Host -> Client: kein Platz / falsche Version
    MSG_BYE = 4,        // beide Richtungen
    MSG_KEYFRAME = 5,   // Client -> Host: bitte neues Schluesselbild
    MSG_PING = 6,       // Client -> Host
    MSG_PONG = 7,       // Host -> Client
    MSG_KICK = 8,       // Host -> Client
};

#pragma pack(push, 1)
struct Hello {
    uint8_t type = MSG_HELLO;
    uint32_t version = kVersion;
    char name[48] = {};
};

struct Welcome {
    uint8_t type = MSG_WELCOME;
    uint32_t version = kVersion;
    uint8_t slot = 0;          // 1-4 = Wiimote-Nummer
    uint16_t width = 0, height = 0, fps = 0;
    uint32_t sample_rate = 48000;
    uint8_t channels = 2;
};

struct Full {
    uint8_t type = MSG_FULL;
    uint8_t reason = 0;   // 0 = voll, 1 = Version, 2 = Raum zu
};

struct Simple {
    uint8_t type = 0;
    uint32_t value = 0;
};

struct Ping {
    uint8_t type = MSG_PING;
    uint64_t t_client_us = 0;
};

// Kopf jedes Video-Pakets, danach H.264 (Annex B)
struct VideoHeader {
    uint32_t frame = 0;       // laufende Nummer
    uint8_t keyframe = 0;
    uint16_t width = 0, height = 0;
    uint64_t t_host_us = 0;
};

// Kopf jedes Ton-Pakets, danach int16 stereo, 48 kHz
struct AudioHeader {
    uint32_t seq = 0;
    uint16_t frames = 0;
};

// Controller-Zustand eines Clients. Knoepfe = SDL3-Gamepad-Nummern als Bits
// (0 unten, 1 rechts, 2 links, 3 oben, 4 Back, 5 Guide, 6 Start, 7/8 Stick-Klick,
//  9 LB, 10 RB, 11-14 Steuerkreuz hoch/runter/links/rechts, 20 Touchpad)
struct Input {
    uint32_t seq = 0;
    uint32_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;   // SDL-Konvention: y nach unten positiv
    uint8_t lt = 0, rt = 0;                   // 0..255
    uint8_t flags = 0;                        // Bit 0: Wiimote quer halten
};
#pragma pack(pop)

enum InputFlags : uint8_t { IN_SIDEWAYS = 1 };

}  // namespace nlnet

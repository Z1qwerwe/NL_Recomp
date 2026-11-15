// Nintendo Land Recomp - Online-Protokoll zwischen Host (NintendoLand.exe) und Client (NLOnline.exe)
//
// Prinzip: Nur der Host laesst das Spiel laufen. Er schickt das TV-Bild (H.264) und den TV-Ton
// an die Clients; die Clients schicken nur ihre Controller-Eingaben zurueck und werden beim Host
// zu Wiimote-Spielern (Spieler 2-5).
#pragma once
#include <cstdint>

namespace nlnet {

constexpr uint32_t kVersion = 4;
constexpr int kChunk = 1100;   // Nutzdaten pro Video-Stueck (passt in ein UDP-Paket)
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
    MSG_STATS = 9,      // Client -> Host: Empfangsstatistik (fuer die Bitrate)
    MSG_CHAT = 10,      // beide Richtungen: Chatnachricht (Host verteilt an alle)
    MSG_ACTIVITY = 11,  // Host -> Client: was gerade gespielt wird (fuer Discord)
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
    // Aufteilung der Leinwand (siehe nl_capture_vk.cpp): TV links, GamePad rechts oben
    uint16_t tv_w = 0, tv_h = 0;
    uint16_t pad_x = 0, pad_y = 0, pad_w = 0, pad_h = 0;
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

// Kopf jedes Video-Stuecks, danach bis zu kChunk Byte H.264 (Annex B).
// Ein Bild wird in 'count' Stuecke geteilt; der Client setzt sie wieder zusammen.
struct VideoHeader {
    uint32_t frame = 0;       // laufende Nummer
    uint16_t index = 0;       // Stueck-Nummer
    uint16_t count = 1;       // Stuecke in diesem Bild
    uint32_t total = 0;       // Bytes im ganzen Bild
    uint8_t keyframe = 0;
    uint16_t width = 0, height = 0;
    uint64_t t_host_us = 0;
};

struct Stats {
    uint8_t type = MSG_STATS;
    uint32_t frames_ok = 0;     // seit der letzten Meldung komplett empfangen
    uint32_t frames_lost = 0;   // seit der letzten Meldung verloren
    uint32_t audio_underruns = 0;
};

struct Activity {
    uint8_t type = MSG_ACTIVITY;
    char attraction[64] = {};   // leer = Plaza / Menue
    uint8_t players = 1;        // alle Spieler im Raum (GamePad + Wiimotes)
    int64_t start_unix = 0;     // seit wann gespielt wird
};

struct Chat {
    uint8_t type = MSG_CHAT;
    char name[48] = {};
    char text[200] = {};
};

// Kopf jedes Ton-Pakets, danach Ton (48 kHz stereo): roh int16 oder IMA-ADPCM (nl_adpcm.h)
enum AudioCodec : uint8_t { AUDIO_PCM = 0, AUDIO_ADPCM = 1 };
struct AudioHeader {
    uint32_t seq = 0;
    uint16_t frames = 0;
    uint8_t codec = AUDIO_PCM;
};

// Controller-Zustand eines Clients. Knoepfe = SDL3-Gamepad-Nummern als Bits
// (0 unten, 1 rechts, 2 links, 3 oben, 4 Back, 5 Guide, 6 Start, 7/8 Stick-Klick,
//  9 LB, 10 RB, 11-14 Steuerkreuz hoch/runter/links/rechts, 20 Touchpad)
// Wiimote-Knoepfe so, wie das Spiel sie kennt (Gast schickt fertige Wiimote-Eingaben,
// die Belegung stellt jeder Gast selbst in NLOnline ein)
enum WiiButton : uint16_t {
    NLW_A = 1 << 0, NLW_B = 1 << 1, NLW_1 = 1 << 2, NLW_2 = 1 << 3, NLW_PLUS = 1 << 4, NLW_MINUS = 1 << 5,
    NLW_HOME = 1 << 6, NLW_UP = 1 << 7, NLW_DOWN = 1 << 8, NLW_LEFT = 1 << 9, NLW_RIGHT = 1 << 10,
    NLW_C = 1 << 11, NLW_Z = 1 << 12,
};

// Woher die Eingaben des Gastes kommen (fuer die Uebersicht beim Host)
enum InputSource : uint8_t { SRC_KEYBOARD = 0, SRC_CONTROLLER = 1, SRC_WIIMOTE = 2 };

struct Input {
    uint32_t seq = 0;
    uint16_t wbuttons = 0;                    // WiiButton-Bits
    int16_t nx = 0, ny = 0;                   // Nunchuk-Stick, y nach OBEN positiv
    uint16_t px = 32768, py = 32768;          // Zeiger auf dem TV-Bild, 0..65535
    uint8_t flags = 0;                        // InputFlags
    uint8_t source = SRC_KEYBOARD;
    // Bewegung ohne echte Sensoren (Maus/Stick/Tasten): gewuenschte Haltung der Wiimote
    int16_t yaw = 0, pitch = 0, roll = 0;     // 1/100 Grad; yaw + = rechts, pitch + = hoch, roll + = rechts
    uint8_t shake = 0;                        // 0..255 Schuetteln
    uint8_t recenter = 0;                     // zaehlt hoch, wenn der Gast "Zentrieren" drueckt
    // echte Sensoren (Controller-Gyro oder echte Wiimote), SDL-Achsen: x rechts, y oben, z zum Spieler
    float acc[3] = {0, 0, 0};                 // m/s^2 (in Ruhe flach: 0, 9.81, 0)
    float gyro[3] = {0, 0, 0};                // rad/s
    uint32_t sensor_us = 0;                   // Zeitstempel der Sensorwerte (Gast-Uhr)
    // Status der echten Wiimote des Gastes (Uebersicht beim Host)
    uint8_t battery = 255;                    // 0..100, 255 = unbekannt/keine Wiimote
    uint8_t ext = 0;                          // Bit 0 Nunchuk, Bit 1 MotionPlus
};
#pragma pack(pop)

enum InputFlags : uint8_t {
    IN_SIDEWAYS = 1,    // quer gehalten (Steuerkreuz gedreht)
    IN_POINTER = 2,     // Zeiger gueltig (Maus auf dem Bild / Sensorleiste gesehen / Stick)
    IN_IMU_REAL = 4,    // acc/gyro sind echte Sensorwerte
    IN_PTR_IR = 8,      // Zeiger kommt von der Kamera einer echten Wiimote
};

}  // namespace nlnet

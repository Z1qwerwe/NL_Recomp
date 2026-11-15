// Nintendo Land Recomp - gemeinsame Sachen von Fenster, Eingabe und Menue.
#pragma once

#include <string>
#include <atomic>
#include <filesystem>
#include <functional>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Einstellungen (nl_data/nl_settings.ini, gehoert dem Spiel, wird im Menue geaendert)
// ---------------------------------------------------------------------------
struct NLSettings {
    // Bild
    bool fullscreen = false;
    int vsync = 1;               // 0 aus, 1 an, 2 Mailbox (wenig Verzoegerung)
    int perf_overlay = 0;        // 0 aus, 1 FPS, 2 ausfuehrlich
    bool gamepad_window = false; // GamePad-Bild in eigenem Fenster (Neustart)
    int screen_layout = 1;       // 0 eins von beiden (Tab), 1 Bild-in-Bild, 2 nebeneinander
    int pip_size = 33;           // Groesse des kleinen Bildes in Prozent
    int res_scale = 0;           // Index in kResScales (Neustart)
    int upscale = 1;             // 0 bilinear, 1 bikubisch, 2 Hermite, 3 Pixel
    int downscale = 0;
    int aspect = 0;              // 0 Seitenverhaeltnis behalten, 1 strecken
    int aniso = 0;               // Texturfilter: 0 Spiel, 1 2x, 2 4x, 3 8x, 4 16x (Neustart)
    int window_w = 1280, window_h = 720;
    // Spiel
    int language = 3;            // Konsolensprache (3 = Deutsch), Neustart
    bool async_shaders = true;   // Shader im Hintergrund kompilieren (Neustart)
    int start_view = 1;          // 0 TV, 1 GamePad
    // Ton
    int volume = 60;
    int pad_volume = 60;         // nur mit eigenem GamePad-Fenster
    bool mic = true;             // (Neustart)
    int mic_volume = 50;
    // Steuerung
    int button_layout = 0;       // 0 automatisch, 1 Nintendo (A rechts), 2 A unten (Xbox/PlayStation)
    bool mouse_camera = true;    // Maus dreht die Kamera (rechter Stick), Zeiger versteckt
    int mouse_sens = 100;        // Prozent
    bool mouse_invert_y = false; // Maus hoch = nach unten schauen
    bool free_camera = true;     // Maus + rechter Stick simulieren das Neigen des GamePads (freie Kamera)
    int stick_camera_speed = 100;// Prozent, wie schnell der rechte Stick neigt
    bool pad_gyro = true;        // Gyro des Controllers (PS5, PS4, Switch Pro ...) als GamePad-Neigen benutzen
    int deadzone = 20;           // Prozent
    // Online
    bool wiimote_sideways = true;// Controller als quer gehaltene Wiimote (Steuerkreuz gedreht, Stick steuert mit)
    int stream_quality = 1;      // 0 niedrig (540p), 1 mittel (720p), 2 hoch (720p, mehr Bitrate)
};
extern NLSettings g_nl;

struct NLResScale { const char* name; float factor; };
inline constexpr NLResScale kResScales[] = {
    {"1280 x 720 (Original)", 1.0f}, {"1920 x 1080", 1.5f}, {"2560 x 1440", 2.0f},
    {"3840 x 2160 (4K)", 3.0f}, {"5120 x 2880 (5K)", 4.0f},
};
inline constexpr int kResScaleCount = (int)(sizeof(kResScales) / sizeof(kResScales[0]));

void nl_settings_load(const std::filesystem::path& path);
void nl_settings_save();               // schreibt die Datei (ruft nur der Fenster-Thread)
void nl_settings_apply_live();         // Werte, die sofort wirken, in den Unterbau uebernehmen
void nl_write_resolution_pack(const std::filesystem::path& data_dir);

// ---------------------------------------------------------------------------
// Eingabe (nl_input.cpp)
// ---------------------------------------------------------------------------
struct NLAction { const char* name; const char* label; const char* help; uint64_t id; bool stick; };
extern const NLAction kNLActions[];
extern const int kNLActionCount;

struct NLBinding { uint32_t key1 = 0, key2 = 0; int pad = -1; };   // pad -1 = keiner
NLBinding& nl_binding(int action);
void nl_input_init(const std::filesystem::path& controls_path);
void nl_input_hotplug();               // neue Controller erkennen (Fenster-Thread, alle 500 ms)
void nl_input_save();
void nl_input_reset_defaults();
std::string nl_key_name(uint32_t vk);
std::string nl_pad_name(int code);
std::string nl_gamepad_name();         // leer = kein Controller
enum class NLPadType { None, Nintendo, Xbox, PlayStation, Other };
NLPadType nl_pad_type();
bool nl_pad_has_gyro();                 // hat der Controller einen Gyro-Sensor?
bool nl_pad_gyro_active();              // wird er gerade benutzt?
bool nl_a_is_bottom();                  // Knopf-Layout: A = unterer Knopf?

// Maus (Fenster-Thread schreibt, Spiel liest)
void nl_mouse_add_delta(int dx, int dy);           // relative Bewegung (Maus-Kamera)
void nl_mouse_touch(bool down, int x, int y);       // Klick/Bewegung in echten Fensterpixeln
bool nl_mouse_pointer_mode();                       // Zeiger sichtbar (nicht eingefangen, Alt, Menue offen, ...)
extern std::atomic<bool> g_nl_mouse_captured;       // Maus fuer die Kamera eingefangen (Klick ins Spielbild)
bool nl_point_in_drc(int x, int y);                 // liegt der Punkt auf dem GamePad-Bild?

// Rohzustand fuer das Menue (Navigation und Aufnahme)
struct NLPadSnapshot { bool connected = false; uint64_t buttons = 0; float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; };
NLPadSnapshot nl_pad_snapshot();
int nl_pad_capture(const NLPadSnapshot& rest);   // gedrueckter Knopf/Achse oder -1
bool nl_pad_menu_pressed();                       // Menue-Knopf am Controller (Flanke)

// ---------------------------------------------------------------------------
// Menue (nl_menu.cpp)
// ---------------------------------------------------------------------------
extern std::atomic<bool> g_nl_menu_open;
void nl_menu_install();
void nl_menu_toggle();

// ---------------------------------------------------------------------------
// Deterministischer Modus (nl_det.cpp), Grundlage fuer Netplay
// ---------------------------------------------------------------------------
bool nl_det_init();   // NL_DET=1 -> an

// ---------------------------------------------------------------------------
// Fenster (main.cpp) - Auftraege an den Fenster-Thread
// ---------------------------------------------------------------------------
enum class NLRequest { SaveSettings, ApplyFullscreen, Restart, Quit };
void nl_request(NLRequest r);
bool nl_key_down(uint32_t vk);

// ---------------------------------------------------------------------------
// Wiimote-Spieler 2-5 aus normalen Controllern (nl_wiipad.cpp)
// ---------------------------------------------------------------------------
struct NLWiiIn {
    uint32_t buttons = 0;                 // SDL3-Gamepad-Knopfnummern als Bits
    float lx = 0, ly = 0, rx = 0, ry = 0; // -1..1, y nach unten positiv (wie SDL)
    float lt = 0, rt = 0;                 // 0..1
    bool sideways = true;                 // Wiimote quer halten
    bool has_ptr = false;                 // Zeiger direkt gesetzt (Maus des Gastes), 0..1
    float px = 0.5f, py = 0.5f;
    bool connected = true;
};
using NLWiiSource = std::function<NLWiiIn()>;
struct NLWiiSlotInfo { bool used = false; bool remote = false; std::string name; };
int nl_wii_attach(const std::string& name, bool remote, NLWiiSource src);   // 1-4 oder -1 (voll)
void nl_wii_detach(int slot);
NLWiiSlotInfo nl_wii_slot(int slot);
int nl_wii_free_slots();

// ---------------------------------------------------------------------------
// Online (nl_online.cpp): Raum hosten, Bild/Ton streamen, Freunde als Wiimote-Spieler
// ---------------------------------------------------------------------------
void nl_online_init();                    // Steam verbinden, Hooks setzen (nach dem Spielstart)
void nl_online_shutdown();
bool nl_online_steam_ok();
bool nl_online_available();               // steam_api64.dll vorhanden?
void nl_online_open_room();               // Raum oeffnen -> Code
void nl_online_close_room();
bool nl_online_hosting();
bool nl_online_opening();                 // Raum wird gerade erstellt
std::string nl_online_code();
std::string nl_online_status();           // letzte Meldung fuer das Menue
struct NLOnlinePlayer { uint64_t id = 0; int slot = 0; std::string name; int ping = -1; };
std::vector<NLOnlinePlayer> nl_online_players();
void nl_online_kick(uint64_t id);
void nl_online_invite(uint64_t friend_id);
void nl_online_join(const std::string& code);   // startet NLOnline.exe mit dem Code
std::string nl_online_encoder();          // Name des Video-Encoders

// Chat (Taste Y, nur mit offenem Raum)
struct NLChatLine { std::string name, text; uint64_t ms = 0; bool system = false; };
extern bool g_nl_chat_open;
bool nl_chat_available();
void nl_chat_open();
void nl_chat_close();
void nl_chat_char(uint32_t ch);           // WM_CHAR
void nl_chat_key(uint32_t vk);            // Enter / Esc / Ruecktaste
std::string nl_chat_input();
void nl_online_send_chat(const std::string& text);
std::vector<NLChatLine> nl_online_chat_lines();   // die letzten 8
uint64_t nl_online_now_ms();

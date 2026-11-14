// Nintendo Land Recomp - Einstellungen und Menue im Spiel (Esc oder Home/Guide am Controller).
//
// Das Menue wird mit ImGui direkt ins Spielbild gezeichnet (ueber den Overlay-Haken im
// Unterbau, siehe nl_menu_install). Bedienbar mit Maus, Tastatur und Controller.
#include "nl_app.h"
#include "net/nl_steam.h"

#include "WindowSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "config/CemuConfig.h"
#include "imgui/imgui_extension.h"
#include "input/api/Controller.h"

#include <imgui.h>
#include <Windows.h>

#include <fstream>
#include <functional>
#include <vector>

NLSettings g_nl;
static NLSettings s_startup;   // Werte beim Start (fuer "gilt nach Neustart")
static std::filesystem::path s_settings_path;
std::atomic<bool> g_nl_menu_open{false};

// Haken im Unterbau (LatteOverlay.cpp): wird jedes Bild aufgerufen
extern void (*g_nl_overlay_hook)(bool pad_view);

// ===========================================================================
// Einstellungen laden/speichern/anwenden
// ===========================================================================
static const char* kLanguages[] = {"Japanisch", "Englisch", "Franzoesisch", "Deutsch", "Italienisch", "Spanisch",
                                   "Chinesisch", "Koreanisch", "Niederlaendisch", "Portugiesisch", "Russisch"};
static const char* kFilters[] = {"Bilinear", "Bikubisch", "Hermite (scharf)", "Pixelgenau"};

void nl_settings_load(const std::filesystem::path& path) {
    s_settings_path = path;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        int v = atoi(line.c_str() + eq + 1);
        auto& n = g_nl;
        if (k == "fullscreen") n.fullscreen = v != 0;
        else if (k == "vsync") n.vsync = std::clamp(v, 0, 2);
        else if (k == "perf_overlay") n.perf_overlay = std::clamp(v, 0, 2);
        else if (k == "gamepad_window") n.gamepad_window = v != 0;
        else if (k == "res_scale") n.res_scale = std::clamp(v, 0, kResScaleCount - 1);
        else if (k == "upscale") n.upscale = std::clamp(v, 0, 3);
        else if (k == "downscale") n.downscale = std::clamp(v, 0, 3);
        else if (k == "aspect") n.aspect = std::clamp(v, 0, 1);
        else if (k == "aniso") n.aniso = std::clamp(v, 0, 4);
        else if (k == "window_w") n.window_w = std::max(320, v);
        else if (k == "window_h") n.window_h = std::max(240, v);
        else if (k == "language") n.language = std::clamp(v, 0, 10);
        else if (k == "async_shaders") n.async_shaders = v != 0;
        else if (k == "start_view") n.start_view = std::clamp(v, 0, 1);
        else if (k == "volume") n.volume = std::clamp(v, 0, 100);
        else if (k == "pad_volume") n.pad_volume = std::clamp(v, 0, 100);
        else if (k == "mic") n.mic = v != 0;
        else if (k == "mic_volume") n.mic_volume = std::clamp(v, 0, 100);
        else if (k == "deadzone") n.deadzone = std::clamp(v, 0, 60);
        else if (k == "screen_layout") n.screen_layout = std::clamp(v, 0, 2);
        else if (k == "pip_size") n.pip_size = std::clamp(v, 15, 60);
        else if (k == "mouse_camera") n.mouse_camera = v != 0;
        else if (k == "mouse_sens2") n.mouse_sens = std::clamp(v, 10, 1000);
        else if (k == "mouse_invert_y") n.mouse_invert_y = v != 0;
        else if (k == "free_camera") n.free_camera = v != 0;
        else if (k == "pad_gyro") n.pad_gyro = v != 0;
        else if (k == "wiimote_sideways") n.wiimote_sideways = v != 0;
        else if (k == "stream_quality") n.stream_quality = std::clamp(v, 0, 2);
        else if (k == "stick_camera_speed") n.stick_camera_speed = std::clamp(v, 10, 400);
        else if (k == "button_layout2") n.button_layout = std::clamp(v, 0, 2);
    }
    s_startup = g_nl;
}

void nl_settings_save() {
    const auto& n = g_nl;
    std::ofstream f(s_settings_path);
    f << "# Nintendo Land Einstellungen (im Spiel: Esc)\n"
      << "fullscreen=" << n.fullscreen << "\nvsync=" << n.vsync << "\nperf_overlay=" << n.perf_overlay
      << "\ngamepad_window=" << n.gamepad_window << "\nres_scale=" << n.res_scale << "\nupscale=" << n.upscale
      << "\ndownscale=" << n.downscale << "\naspect=" << n.aspect << "\naniso=" << n.aniso
      << "\nwindow_w=" << n.window_w << "\nwindow_h=" << n.window_h << "\nlanguage=" << n.language
      << "\nasync_shaders=" << n.async_shaders << "\nstart_view=" << n.start_view << "\nvolume=" << n.volume
      << "\npad_volume=" << n.pad_volume << "\nmic=" << n.mic << "\nmic_volume=" << n.mic_volume
      << "\nbutton_layout2=" << n.button_layout << "\ndeadzone=" << n.deadzone << "\nscreen_layout=" << n.screen_layout
      << "\npip_size=" << n.pip_size << "\nmouse_camera=" << n.mouse_camera << "\nmouse_sens2=" << n.mouse_sens << "\nmouse_invert_y=" << n.mouse_invert_y << "\nfree_camera=" << n.free_camera
      << "\nstick_camera_speed=" << n.stick_camera_speed << "\npad_gyro=" << n.pad_gyro
      << "\nwiimote_sideways=" << n.wiimote_sideways << "\nstream_quality=" << n.stream_quality << "\n";
    GetConfigHandle().Save();
}

void nl_settings_apply_live() {
    auto& c = GetConfig();
    const auto& n = g_nl;
    c.upscale_filter = n.upscale;
    c.downscale_filter = n.downscale;
    c.fullscreen_scaling = n.aspect == 0 ? kKeepAspectRatio : kStretch;
    c.overlay.position = n.perf_overlay ? ScreenPosition::kTopLeft : ScreenPosition::kDisabled;
    c.overlay.fps = true;
    c.overlay.cpu_usage = c.overlay.ram_usage = c.overlay.vram_usage = c.overlay.drawcalls = (n.perf_overlay == 2);
    c.tv_volume = n.volume;
    c.pad_volume = n.gamepad_window ? n.pad_volume : 0;
    c.input_volume = n.mic_volume;
    if (g_tvAudio) g_tvAudio->SetVolume(n.volume);
    if (g_padAudio) g_padAudio->SetVolume(n.gamepad_window ? n.pad_volume : 0);
    if (g_inputAudio) g_inputAudio->SetVolume(n.mic_volume);
}

// Eigenes Grafikpaket fuer Aufloesung und Texturfilter (wirkt nach Neustart)
void nl_write_resolution_pack(const std::filesystem::path& data_dir) {
    std::error_code ec;
    auto dir = data_dir / "graphicPacks" / "NintendoLandRecomp";
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(dir / "rules.txt");
    f << "# Wird von NintendoLand.exe geschrieben - im Spiel unter Esc -> Bild einstellen\n"
         "[Definition]\n"
         "titleIds = 0005000010101E00,0005000010101F00,0005000010102100\n"
         "name = Nintendo Land Recomp\n"
         "version = 6\n\n";
    const float s = kResScales[g_nl.res_scale].factor;
    if (s != 1.0f) {
        // TV (1280x720) und GamePad (854x480) samt halben/viertel Puffern (Bloom, Schatten usw.)
        const int sizes[][2] = {{1280, 720}, {640, 360}, {320, 180}, {160, 90}, {854, 480}, {427, 240}};
        for (auto& sz : sizes) {
            f << "[TextureRedefine]\nwidth = " << sz[0] << "\nheight = " << sz[1]
              << "\ntilemodesExcluded = 0x001\noverwriteWidth = " << (int)(sz[0] * s + 0.5f)
              << "\noverwriteHeight = " << (int)(sz[1] * s + 0.5f) << "\n\n";
        }
    }
    if (g_nl.aniso > 0) {
        const int v[] = {1, 2, 4, 8, 16};
        f << "[TextureRedefine]\noverwriteAnisotropy = " << v[g_nl.aniso] << "\n\n";
    }
}

// ===========================================================================
// Menue-Aufbau
// ===========================================================================
struct Row {
    std::string label;
    std::string help;
    std::function<std::string()> value;      // angezeigter Wert
    std::function<void(int)> change;         // -1 / +1
    std::function<bool()> is_default;
    bool restart = false;                     // wirkt erst nach Neustart
    bool header = false;                      // Zwischenueberschrift
    int action = -1;                          // Steuerungs-Zeile (Index in kNLActions)
    std::function<void()> press;              // Knopf-Zeile (Enter / A / Klick)
    bool text_entry = false;                  // Code-Eingabe (Tastatur)
    int dot = 0;                              // 1 = gruener Punkt (spielt gerade Nintendo Land), 2 = blau (hostet)
};

static Row info(std::string label, std::string value, std::string help) {
    Row r;
    r.label = std::move(label);
    r.help = std::move(help);
    r.value = [value] { return value; };
    return r;
}

static Row button(std::string label, std::string value, std::string help, std::function<void()> press) {
    Row r = info(std::move(label), std::move(value), std::move(help));
    r.press = std::move(press);
    return r;
}

static void save_later() { nl_request(NLRequest::SaveSettings); }

template <typename T>
static Row choice(const char* label, const char* help, T* field, std::vector<std::string> names, T def, bool restart = false) {
    Row r;
    r.label = label;
    r.help = help;
    r.restart = restart;
    r.value = [field, names] { return names[std::clamp((int)*field, 0, (int)names.size() - 1)]; };
    r.change = [field, names](int d) {
        int v = std::clamp((int)*field + d, 0, (int)names.size() - 1);
        *field = (T)v;
        nl_settings_apply_live();
        save_later();
    };
    r.is_default = [field, def] { return *field == def; };
    return r;
}

static Row onoff(const char* label, const char* help, bool* field, bool def, bool restart = false,
                 std::function<void()> after = {}) {
    Row r;
    r.label = label;
    r.help = help;
    r.restart = restart;
    r.value = [field] { return std::string(*field ? "An" : "Aus"); };
    r.change = [field, after](int) {
        *field = !*field;
        nl_settings_apply_live();
        if (after) after();
        save_later();
    };
    r.is_default = [field, def] { return *field == def; };
    return r;
}

static Row percent(const char* label, const char* help, int* field, int def, int lo, int hi, int step) {
    Row r;
    r.label = label;
    r.help = help;
    r.value = [field] { return std::to_string(*field) + " %"; };
    r.change = [field, lo, hi, step](int d) {
        *field = std::clamp(*field + d * step, lo, hi);
        nl_settings_apply_live();
        save_later();
    };
    r.is_default = [field, def] { return *field == def; };
    return r;
}

static Row header(const char* label) {
    Row r;
    r.label = label;
    r.header = true;
    return r;
}

// ---------------------------------------------------------------------------
// Online-Reiter
// ---------------------------------------------------------------------------
static std::string s_join_code;                 // getippter Code
static std::vector<NLFriend> s_friends;         // Freundesliste (jede Sekunde neu)
static uint64_t s_friends_time = 0;
static std::string s_online_note;               // kurze Rueckmeldung ("Einladung geschickt")

static void copy_to_clipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (h) {
        memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

static std::string clipboard_text() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* p = (const char*)GlobalLock(h)) { out = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    return out;
}

static void build_online(std::vector<Row>& rows) {
    auto& n = g_nl;
    const NLSettings d{};
    if (!nl_online_available()) {
        rows.push_back(header("Online"));
        rows.push_back(info("Online", "nicht verfuegbar", "steam_api64.dll fehlt neben NintendoLand.exe. Baue das Spiel im Launcher neu - die Datei wird dabei mitkopiert."));
        return;
    }
    if (!nl_online_steam_ok()) {
        rows.push_back(header("Online"));
        rows.push_back(info("Steam", "nicht verbunden", nl_online_status() + "\n\nOnline laeuft ueber Steam: Freundesliste, Einladungen und die Verbindung. "
                            "Starte Steam und danach das Spiel neu."));
        return;
    }
    const bool hosting = nl_online_hosting();
    const std::string room_help = "Du hostest: das Spiel laeuft bei dir, deine Freunde sehen dein TV-Bild und spielen als Wiimote 1-4 mit. "
                                  "Sie brauchen dafuer nur NLOnline.exe und Steam - nicht das Spiel. Du selbst spielst mit dem GamePad.";
    rows.push_back(header("Dein Raum"));
    if (hosting) {
        const std::string code = nl_online_code();
        rows.push_back(button("Code", code, "Gib diesen Code deinen Freunden. Sie tippen ihn in NLOnline.exe ein. "
                              "Enter / A kopiert ihn in die Zwischenablage.\n\n" + room_help,
                              [code] { copy_to_clipboard(code); s_online_note = "Code kopiert."; }));
        rows.push_back(button("Raum schliessen", "Schliessen", "Alle Mitspieler werden getrennt.", [] { nl_online_close_room(); }));
    } else if (nl_online_opening()) {
        rows.push_back(info("Raum", "wird erstellt ...", room_help));
    } else {
        rows.push_back(button("Raum oeffnen", "Oeffnen", "Erstellt einen Raum mit einem 6-stelligen Code.\n\n" + room_help,
                              [] { nl_online_open_room(); }));
    }
    rows.push_back(choice("Bildqualitaet fuer Freunde", "Wie gut das Bild bei deinen Freunden ankommt. Hoeher braucht mehr Upload: "
                          "Niedrig ca. 3,5 Mbit/s, Mittel 6, Hoch 12 - pro Freund. Gilt ab dem naechsten geoeffneten Raum.",
                          &n.stream_quality, {"Niedrig (540p)", "Mittel (720p)", "Hoch (720p+)"}, d.stream_quality));

    rows.push_back(header("Mitspieler"));
    rows.push_back(info("GamePad", nl_steam_my_name(), "Du bist Spieler 1 mit dem GamePad."));
    auto players = nl_online_players();
    for (int slot = 1; slot <= 4; slot++) {
        NLWiiSlotInfo si = nl_wii_slot(slot);
        if (!si.used) continue;
        std::string label = "Wiimote " + std::to_string(slot) + ": " + si.name;
        if (!si.remote) {
            rows.push_back(info(label, "hier am PC", "Ein weiterer Controller an diesem PC."));
            continue;
        }
        for (auto& p : players) {
            if (p.slot != slot) continue;
            std::string v = (p.ping >= 0 ? std::to_string(p.ping) + " ms  " : std::string()) + "Rauswerfen";
            uint64_t id = p.id;
            rows.push_back(button(label, v, "Online-Mitspieler. Enter / A wirft ihn aus dem Raum.", [id] { nl_online_kick(id); }));
            rows.back().dot = 1;
        }
    }
    rows.push_back(onoff("Wiimote quer halten", "Fuer Controller an diesem PC: das Steuerkreuz und der linke Stick steuern wie eine quer "
                         "gehaltene Wiimote (Mario Chase, Luigi's Mansion, Animal Crossing). Aus: wie eine aufrecht gehaltene Wiimote. "
                         "Online-Freunde stellen das in NLOnline selbst ein.", &n.wiimote_sideways, d.wiimote_sideways));

    rows.push_back(header("Beitreten"));
    {
        Row r;
        r.label = "Code eingeben";
        r.help = "Tippe den 6-stelligen Code eines Freundes und druecke Enter. Strg+V fuegt ihn ein, Ruecktaste loescht.\n\n"
                 "Das Spiel bleibt offen; NLOnline.exe startet und verbindet dich mit dem Raum deines Freundes.";
        r.text_entry = true;
        r.value = [] { return s_join_code.size() < 6 ? s_join_code + "_" : s_join_code; };
        r.press = [] {
            if (s_join_code.size() == 6) nl_online_join(s_join_code);
            else s_online_note = "Der Code hat 6 Zeichen.";
        };
        rows.push_back(r);
    }

    rows.push_back(header("Freunde"));
    const uint64_t now = GetTickCount64();
    if (now - s_friends_time > 1000) {
        s_friends = nl_steam_friends();
        s_friends_time = now;
    }
    if (s_friends.empty()) rows.push_back(info("Keine Freunde online", "", "Hier stehen deine Steam-Freunde, die gerade online sind."));
    for (auto& f : s_friends) {
        std::string v, help;
        std::function<void()> press;
        const uint64_t id = f.id;
        const std::string name = f.name;
        if (!f.code.empty()) {
            v = "hostet  Beitreten";
            help = name + " hostet gerade einen Raum. Enter / A: beitreten.";
            const std::string code = f.code;
            press = [code] { nl_online_join(code); };
        } else {
            v = f.in_nl ? "spielt Nintendo Land" : "online";
            help = f.in_nl ? name + " hat Nintendo Land Recomp offen." : name + " ist auf Steam online.";
            if (hosting) {
                v += "  Einladen";
                help += " Enter / A: in deinen Raum einladen (kommt als Steam-Einladung an).";
                press = [id, name] { nl_online_invite(id); s_online_note = "Einladung an " + name + " geschickt."; };
            } else {
                help += " Oeffne einen Raum, um Freunde einzuladen.";
            }
        }
        Row r = press ? button(name, v, help, press) : info(name, v, help);
        r.dot = !f.code.empty() ? 2 : f.in_nl ? 1 : 0;
        rows.push_back(r);
    }
}

static std::vector<Row> build_tab(int tab) {
    std::vector<Row> rows;
    auto& n = g_nl;
    const NLSettings d{};
    if (tab == 0) {   // Bild
        rows.push_back(onoff("Vollbild", "Spiel im Vollbild ohne Rahmen. Auch mit F11 oder Alt+Enter.", &n.fullscreen, d.fullscreen,
                             false, [] { nl_request(NLRequest::ApplyFullscreen); }));
        rows.push_back(choice("VSync", "Wartet auf den Bildschirm, damit das Bild nicht reisst. 'Schnell' (Mailbox) hat weniger Verzoegerung.",
                              &n.vsync, {"Aus", "An", "Schnell"}, d.vsync, true));
        rows.push_back(choice("Leistungsanzeige", "Zeigt oben links die Bilder pro Sekunde, ausfuehrlich auch CPU-, RAM- und Grafikspeicher-Last.",
                              &n.perf_overlay, {"Aus", "FPS", "Ausfuehrlich"}, d.perf_overlay));
        rows.push_back(choice("Bildschirme", "Wie TV- und GamePad-Bild gezeigt werden. Bild-in-Bild: das GamePad-Bild klein in der Ecke. "
                              "Nebeneinander: beide gleichzeitig. Mit Tab tauschst du, welches gross ist. Ein Klick auf das GamePad-Bild ist ein Touch.",
                              &n.screen_layout, {"Eins (Tab wechselt)", "Bild-in-Bild", "Nebeneinander"}, d.screen_layout));
        rows.push_back(percent("Groesse kleines Bild", "Wie gross das kleine Bild bei Bild-in-Bild ist.", &n.pip_size, d.pip_size, 15, 60, 5));
        rows.push_back(onoff("GamePad-Fenster", "Zeigt das GamePad-Bild stattdessen in einem zweiten Fenster - praktisch mit zwei Bildschirmen.",
                             &n.gamepad_window, d.gamepad_window, true));
        rows.push_back(header("Bildqualitaet"));
        {
            std::vector<std::string> names;
            for (auto& r : kResScales) names.push_back(r.name);
            rows.push_back(choice("Aufloesung", "Interne Aufloesung, in der das Spiel gerendert wird. Hoeher = schaerfer, braucht mehr Grafikkarte. Original ist 720p.",
                                  &n.res_scale, names, d.res_scale, true));
        }
        rows.push_back(choice("Hochskalierung", "Filter, wenn das Bild groesser als die interne Aufloesung angezeigt wird.",
                              &n.upscale, {kFilters[0], kFilters[1], kFilters[2], kFilters[3]}, d.upscale));
        rows.push_back(choice("Runterskalierung", "Filter, wenn die interne Aufloesung groesser als das Fenster ist.",
                              &n.downscale, {kFilters[0], kFilters[1], kFilters[2], kFilters[3]}, d.downscale));
        rows.push_back(choice("Texturfilter", "Anisotrope Filterung: Boden und Waende bleiben auch schraeg betrachtet scharf.",
                              &n.aniso, {"Wie im Spiel", "2x", "4x", "8x", "16x"}, d.aniso, true));
        rows.push_back(choice("Bildformat", "16:9 mit schwarzen Balken behalten oder das Bild aufs ganze Fenster strecken.",
                              &n.aspect, {"16:9 behalten", "Strecken"}, d.aspect));
    } else if (tab == 1) {   // Spiel
        rows.push_back(choice("Grosses Bild beim Start", "Welches Bild beim Start gross ist. Nintendo Land fragt am Anfang vieles auf dem GamePad.",
                              &n.start_view, {"TV", "GamePad"}, d.start_view));
        {
            std::vector<std::string> names(std::begin(kLanguages), std::end(kLanguages));
            rows.push_back(choice("Sprache", "Sprache der Konsole, die das Spiel benutzt.", &n.language, names, d.language, true));
        }
        rows.push_back(onoff("Shader im Hintergrund", "Neue Shader werden im Hintergrund vorbereitet: kein Ruckeln, aber manche Objekte tauchen kurz spaeter auf.",
                             &n.async_shaders, d.async_shaders, true));
    } else if (tab == 2) {   // Ton
        rows.push_back(percent("Lautstaerke", "Gesamtlautstaerke des Spiels (TV-Ton).", &n.volume, d.volume, 0, 100, 5));
        rows.push_back(percent("GamePad-Lautstaerke", "Ton des GamePad-Lautsprechers - nur mit eigenem GamePad-Fenster.", &n.pad_volume, d.pad_volume, 0, 100, 5));
        rows.push_back(onoff("Mikrofon", "Mikrofon des PCs als GamePad-Mikrofon. Im Spiel die Mikrofon-Taste halten.", &n.mic, d.mic, true));
        rows.push_back(percent("Mikrofon-Lautstaerke", "Wie laut das Spiel dein Mikrofon hoert.", &n.mic_volume, d.mic_volume, 0, 100, 5));
    } else if (tab == 3) {   // Steuerung
        rows.push_back(choice("Knopf-Layout", "Automatisch: Xbox- und PlayStation-Controller bekommen A auf den unteren Knopf (Kreuz), "
                              "Nintendo-Controller auf den rechten. Setzt die Controller-Belegung auf Standard.",
                              &n.button_layout, {"Automatisch", "Nintendo (A rechts)", "A unten (Xbox/PS)"}, d.button_layout));
        rows.back().change = [](int dir) {
            g_nl.button_layout = std::clamp(g_nl.button_layout + dir, 0, 2);
            nl_input_reset_defaults();
            nl_input_save();
            save_later();
        };
        rows.push_back(onoff("Controller-Gyro", "Hat dein Controller einen Bewegungssensor (PS5, PS4, Switch Pro ...), "
                             "neigst du damit das GamePad wie im Original: Controller kippen = zielen und umschauen. "
                             "Solange der Gyro an ist, uebernimmt er das Neigen statt Maus und rechtem Stick.", &n.pad_gyro, d.pad_gyro));
        rows.push_back(onoff("Freie Kamera (statt Neigen)", "Im Original zielt und schaut man oft, indem man das GamePad neigt. "
                             "Hier uebernehmen Maus und rechter Stick das: hoch, runter, links, rechts. "
                             "Z, mittlere Maustaste oder das Touchpad am Controller zentriert die Kamera wieder.", &n.free_camera, d.free_camera));
        rows.push_back(percent("Stick-Kamera-Tempo", "Wie schnell der rechte Stick die Kamera dreht (freie Kamera).", &n.stick_camera_speed,
                               d.stick_camera_speed, 10, 400, 10));
        rows.push_back(onoff("Maus-Kamera", "Klick ins grosse Spielbild faengt die Maus ein: dann dreht sie die Kamera wie der rechte Stick und der Zeiger ist weg. "
                             "Esc oder Alt gibt sie wieder frei. Ein Klick aufs GamePad-Bild ist immer ein Touch.", &n.mouse_camera, d.mouse_camera));
        rows.push_back(percent("Maus-Empfindlichkeit", "Wie schnell sich die Kamera mit der Maus dreht.", &n.mouse_sens, d.mouse_sens, 10, 1000, 10));
        rows.push_back(onoff("Maus hoch/runter umkehren", "Maus nach oben = nach unten schauen (wie bei Flugspielen).", &n.mouse_invert_y, d.mouse_invert_y));
        rows.push_back(percent("Stick-Totzone", "Wie weit der Stick bewegt werden muss, bevor das Spiel reagiert.", &n.deadzone, d.deadzone, 0, 60, 5));
        rows.push_back(header("Belegung"));
        for (int i = 0; i < kNLActionCount; i++) {
            Row r;
            r.label = kNLActions[i].label;
            r.help = kNLActions[i].help;
            r.action = i;
            rows.push_back(r);
        }
    } else {   // Online
        build_online(rows);
    }
    return rows;
}

// ===========================================================================
// Eingabe im Menue
// ===========================================================================
struct NavState {
    bool held[16] = {};
    uint64_t next_repeat[16] = {};
};
static NavState s_nav;
enum Nav { N_UP, N_DOWN, N_LEFT, N_RIGHT, N_OK, N_BACK, N_TAB_L, N_TAB_R, N_CLEAR, N_COUNT };

static bool nav_edge(int which, bool down, bool repeat) {
    uint64_t now = GetTickCount64();
    bool fire = false;
    if (down && !s_nav.held[which]) {
        fire = true;
        s_nav.next_repeat[which] = now + 380;
    } else if (down && repeat && now >= s_nav.next_repeat[which]) {
        fire = true;
        s_nav.next_repeat[which] = now + 70;
    }
    s_nav.held[which] = down;
    return fire;
}

// ===========================================================================
// Zustand
// ===========================================================================
static constexpr int kTabCount = 5;   // Bild, Spiel, Ton, Steuerung, Online
static int s_tab = 0, s_sel = 0, s_col = 0, s_scroll = 0;
static bool s_type_prev[256] = {};
static bool s_was_typing = false;

// Code-Eingabe im Online-Reiter: Buchstaben, Ziffern, Ruecktaste, Strg+V
static void poll_typing() {
    if (!s_was_typing)   // gerade erst auf die Zeile gekommen: gehaltene Tasten nicht als Eingabe werten
        for (int vk = 0; vk < 256; vk++) s_type_prev[vk] = nl_key_down(vk);
    auto edge = [](int vk) {
        bool d = nl_key_down(vk);
        bool e = d && !s_type_prev[vk];
        s_type_prev[vk] = d;
        return e;
    };
    const bool ctrl = nl_key_down(VK_CONTROL);
    if (ctrl && edge('V')) { s_join_code = nl_normalize_code(s_join_code + clipboard_text()); return; }
    for (int vk = 'A'; vk <= 'Z'; vk++)
        if (edge(vk) && !ctrl) s_join_code = nl_normalize_code(s_join_code + (char)vk);
    for (int vk = '0'; vk <= '9'; vk++)
        if (edge(vk)) s_join_code = nl_normalize_code(s_join_code + (char)vk);
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; vk++)
        if (edge(vk)) s_join_code = nl_normalize_code(s_join_code + (char)('0' + vk - VK_NUMPAD0));
    if (edge(VK_BACK) && !s_join_code.empty()) s_join_code.pop_back();
}
static int s_capture_action = -1, s_capture_col = 0;
static uint64_t s_capture_start = 0;
static NLPadSnapshot s_capture_rest;
static bool s_capture_armed = false;
static int s_bottom = -1;             // Fokus auf unteren Knoepfen (0 Weiter, 1 Neustart, 2 Beenden)
static bool s_quit_confirm = false;
static bool s_esc_last = false;

void nl_menu_toggle() {
    bool open = !g_nl_menu_open;
    s_capture_action = -1;
    s_bottom = -1;
    s_quit_confirm = false;
    g_nl_menu_open = open;
}

static void start_capture(int action, int col) {
    s_capture_action = action;
    s_capture_col = col;
    s_capture_start = GetTickCount64();
    s_capture_rest = nl_pad_snapshot();
    s_capture_armed = false;
}

static void assign_key(int action, int col, uint32_t vk) {
    // Eine Taste gibt es nur einmal: von anderen Aktionen entfernen
    for (int i = 0; i < kNLActionCount; i++) {
        auto& b = nl_binding(i);
        if (b.key1 == vk) b.key1 = 0;
        if (b.key2 == vk) b.key2 = 0;
    }
    auto& b = nl_binding(action);
    (col == 0 ? b.key1 : b.key2) = vk;
}

static void assign_pad(int action, int code) {
    // Controller-Knopf: mit der Aktion tauschen, die ihn bisher hatte
    auto& b = nl_binding(action);
    for (int i = 0; i < kNLActionCount; i++)
        if (i != action && nl_binding(i).pad == code) nl_binding(i).pad = b.pad;
    b.pad = code;
}

static void poll_capture() {
    if (s_capture_action < 0) return;
    if (GetTickCount64() - s_capture_start < 200) return;
    // Erst loslegen, wenn alles losgelassen ist (sonst zaehlt Enter/A vom Auswaehlen)
    bool any = false;
    for (int vk = 1; vk < 0xFF; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) { any = true; break; }
    NLPadSnapshot ps = nl_pad_snapshot();
    if (!s_capture_armed) {
        if (!any && (ps.buttons & ~s_capture_rest.buttons) == 0) {
            s_capture_armed = true;
            s_capture_rest = ps;
        }
        return;
    }
    if (!WindowSystem::GetWindowInfo().app_active) return;
    if (s_capture_col < 2) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { s_capture_action = -1; return; }
        for (int vk = 1; vk < 0xFF; vk++) {
            if (vk == VK_ESCAPE || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) continue;
            if (GetAsyncKeyState(vk) & 0x8000) {
                assign_key(s_capture_action, s_capture_col, (uint32_t)vk);
                s_capture_action = -1;
                nl_input_save();
                return;
            }
        }
    } else {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { s_capture_action = -1; return; }
        int code = nl_pad_capture(s_capture_rest);
        if (code >= 0) {
            assign_pad(s_capture_action, code);
            s_capture_action = -1;
            nl_input_save();
        }
    }
}

// ===========================================================================
// Zeichnen
// ===========================================================================
static ImU32 col(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
// Wii-U-Look: helles Glas, feines Raster, weisse Kacheln mit Glanz, Cyan als Akzent
static const ImU32 C_PANEL = col(246, 248, 250, 236), C_ROW = col(255, 255, 255, 240), C_ROW_HOT = col(236, 248, 253, 250),
                   C_SEL = col(0, 174, 239, 255), C_TEXT = col(62, 66, 74), C_DIM = col(140, 146, 156),
                   C_ACCENT = col(0, 174, 239), C_HELP = col(236, 240, 244, 235), C_EDGE = col(208, 213, 220),
                   C_ON_ACCENT = col(255, 255, 255);

// Glas-Kachel: weisse Flaeche, Rand, Glanz oben, weicher Schatten
static void glass_tile(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 fill, ImU32 edge, float u, bool shadow = true) {
    if (shadow)
        for (int i = 3; i >= 1; i--)
            dl->AddRectFilled(ImVec2(a.x - i * u, a.y + (i + 1) * u), ImVec2(b.x + i * u, b.y + (i + 2) * u), IM_COL32(40, 60, 90, 10), r + i * u);
    dl->AddRectFilled(a, b, fill, r);
    dl->AddRectFilled(ImVec2(a.x + 2 * u, a.y + 2 * u), ImVec2(b.x - 2 * u, a.y + (b.y - a.y) * 0.48f), IM_COL32(255, 255, 255, 70), r);
    dl->AddRect(a, b, edge, r, 0, 1.5f * u);
}
// Akzent-Pille (aktiver Reiter, fokussierter Knopf): Cyan mit Verlauf und Glanz
static void accent_pill(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float u) {
    dl->AddRectFilled(ImVec2(a.x, a.y + 3 * u), ImVec2(b.x, b.y + 3 * u), IM_COL32(0, 120, 170, 60), r);
    dl->AddRectFilled(a, b, IM_COL32(0, 168, 232, 255), r);
    dl->AddRectFilled(ImVec2(a.x + 2 * u, a.y + 2 * u), ImVec2(b.x - 2 * u, a.y + (b.y - a.y) * 0.5f), IM_COL32(255, 255, 255, 60), r);
    dl->AddRect(a, b, IM_COL32(0, 140, 200, 255), r, 0, 1.5f * u);
}
// runder Pfeil-Knopf wie im Wii-U-Menue
static void round_arrow(ImDrawList* dl, ImVec2 c, float rad, bool right, bool hot) {
    dl->AddCircleFilled(c, rad, hot ? IM_COL32(0, 174, 239, 255) : IM_COL32(232, 236, 241, 255), 24);
    dl->AddCircle(c, rad, hot ? IM_COL32(0, 140, 200, 255) : IM_COL32(205, 210, 217, 255), 24, 1.5f);
    ImU32 t = hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(110, 116, 126, 255);
    float s = rad * 0.75f;
    if (right) dl->AddTriangleFilled(ImVec2(c.x - s * 0.35f, c.y - s * 0.55f), ImVec2(c.x - s * 0.35f, c.y + s * 0.55f), ImVec2(c.x + s * 0.55f, c.y), t);
    else dl->AddTriangleFilled(ImVec2(c.x + s * 0.35f, c.y - s * 0.55f), ImVec2(c.x + s * 0.35f, c.y + s * 0.55f), ImVec2(c.x - s * 0.55f, c.y), t);
}

static void text_at(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 c, const std::string& t) {
    dl->AddText(f, size, p, c, t.c_str());
}
static ImVec2 measure(ImFont* f, float size, const std::string& t) {
    return f->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str());
}
static void text_center(ImDrawList* dl, ImFont* f, float size, ImVec2 a, ImVec2 b, ImU32 c, const std::string& t) {
    ImVec2 m = measure(f, size, t);
    dl->AddText(f, size, ImVec2((a.x + b.x - m.x) / 2, (a.y + b.y - m.y) / 2), c, t.c_str());
}
static void arrow(ImDrawList* dl, ImVec2 c, float s, bool right, ImU32 color) {
    if (right) dl->AddTriangleFilled(ImVec2(c.x - s * 0.4f, c.y - s * 0.5f), ImVec2(c.x - s * 0.4f, c.y + s * 0.5f), ImVec2(c.x + s * 0.5f, c.y), color);
    else dl->AddTriangleFilled(ImVec2(c.x + s * 0.4f, c.y - s * 0.5f), ImVec2(c.x + s * 0.4f, c.y + s * 0.5f), ImVec2(c.x - s * 0.5f, c.y), color);
}
static bool hit(ImVec2 a, ImVec2 b) {
    ImGuiIO& io = ImGui::GetIO();
    return io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y && io.MousePos.y < b.y;
}
static bool clicked(ImVec2 a, ImVec2 b) { return hit(a, b) && ImGui::IsMouseClicked(0); }

static void render_menu(int w, int h) {
    const float u = std::min(w / 1600.0f, h / 900.0f);   // Grundmass
    const float fs_big = std::round(30 * u), fs = std::round(22 * u), fs_small = std::round(16 * u);
    ImFont* f_big = ImGui_GetFont(fs_big);
    ImFont* f = ImGui_GetFont(fs);
    ImFont* f_small = ImGui_GetFont(fs_small);
    if (!f_big || !f || !f_small) return;   // Schriften werden im naechsten Bild bereitgestellt

    std::vector<Row> rows = build_tab(s_tab);
    const int row_count = (int)rows.size();
    auto selectable = [&](int i) { return i >= 0 && i < row_count && !rows[i].header; };
    if (!selectable(s_sel)) {
        s_sel = 0;
        while (s_sel < row_count && !selectable(s_sel)) s_sel++;
    }

    // ---------------- Eingabe ----------------
    const bool typing = s_bottom < 0 && selectable(s_sel) && rows[s_sel].text_entry && s_capture_action < 0;
    if (typing) poll_typing();
    s_was_typing = typing;
    NLPadSnapshot ps = nl_pad_snapshot();
    const bool xb = nl_a_is_bottom();
    auto pb = [&](int b) { return ps.connected && ((ps.buttons >> b) & 1); };
    const int PAD_OK = xb ? 0 : 1, PAD_BACK = xb ? 1 : 0;   // Nintendo: A rechts, B unten
    bool in[N_COUNT] = {};
    if (s_capture_action < 0) {
        const bool letters = !typing;   // beim Code-Tippen sind W/A/S/D/Q/E und Ruecktaste Buchstaben
        in[N_UP] = nav_edge(N_UP, nl_key_down(VK_UP) || (letters && nl_key_down('W')) || pb(11) || ps.ly < -0.6f, true);
        in[N_DOWN] = nav_edge(N_DOWN, nl_key_down(VK_DOWN) || (letters && nl_key_down('S')) || pb(12) || ps.ly > 0.6f, true);
        in[N_LEFT] = nav_edge(N_LEFT, nl_key_down(VK_LEFT) || (letters && nl_key_down('A')) || pb(13) || ps.lx < -0.6f, true);
        in[N_RIGHT] = nav_edge(N_RIGHT, nl_key_down(VK_RIGHT) || (letters && nl_key_down('D')) || pb(14) || ps.lx > 0.6f, true);
        in[N_OK] = nav_edge(N_OK, nl_key_down(VK_RETURN) || (letters && nl_key_down(VK_SPACE)) || pb(PAD_OK), false);
        in[N_BACK] = nav_edge(N_BACK, (letters && nl_key_down(VK_BACK)) || pb(PAD_BACK), false);
        in[N_TAB_L] = nav_edge(N_TAB_L, (letters && nl_key_down('Q')) || pb(9), false);
        in[N_TAB_R] = nav_edge(N_TAB_R, (letters && nl_key_down('E')) || pb(10), false);
        in[N_CLEAR] = nav_edge(N_CLEAR, nl_key_down(VK_DELETE) || pb(xb ? 2 : 3), false);
    } else {
        for (int i = 0; i < N_COUNT; i++) s_nav.held[i] = true;   // nach der Aufnahme nichts ausloesen
    }
    poll_capture();

    if (in[N_TAB_L] || in[N_TAB_R]) {
        s_tab = (s_tab + (in[N_TAB_R] ? 1 : kTabCount - 1)) % kTabCount;
        s_sel = 0;
        s_scroll = 0;
        s_bottom = -1;
        return;
    }
    if (in[N_BACK]) { nl_menu_toggle(); return; }
    if (s_bottom >= 0) {
        if (in[N_LEFT]) s_bottom = std::max(0, s_bottom - 1);
        if (in[N_RIGHT]) s_bottom = std::min(2, s_bottom + 1);
        if (in[N_UP]) s_bottom = -1;
    } else {
        if (in[N_UP]) { int i = s_sel - 1; while (i >= 0 && !selectable(i)) i--; if (i >= 0) s_sel = i; }
        if (in[N_DOWN]) {
            int i = s_sel + 1;
            while (i < row_count && !selectable(i)) i++;
            if (i < row_count) s_sel = i; else s_bottom = 0;
        }
        if (selectable(s_sel)) {
            Row& r = rows[s_sel];
            if (r.action >= 0) {
                if (in[N_LEFT]) s_col = std::max(0, s_col - 1);
                if (in[N_RIGHT]) s_col = std::min(2, s_col + 1);
                if (in[N_OK]) start_capture(r.action, s_col);
                if (in[N_CLEAR]) {
                    auto& b = nl_binding(r.action);
                    if (s_col == 0) b.key1 = 0; else if (s_col == 1) b.key2 = 0; else b.pad = -1;
                    nl_input_save();
                }
            } else if (r.press) {
                if (in[N_OK] || (in[N_RIGHT] && !r.text_entry)) r.press();
            } else if (r.change) {
                if (in[N_LEFT]) r.change(-1);
                if (in[N_RIGHT] || in[N_OK]) r.change(+1);
            }
        }
    }

    // ---------------- Zeichnen ----------------
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##nl_menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2((float)w, (float)h), col(220, 228, 236, 150), col(220, 228, 236, 150),
                                col(150, 165, 180, 170), col(150, 165, 180, 170));

    const float pw = 1400 * u, ph = 800 * u;
    const ImVec2 p0((w - pw) / 2, (h - ph) / 2), p1(p0.x + pw, p0.y + ph);
    // Panel: Schatten, Glas, feines Raster wie im Wii-U-Menue, weisser Rand
    for (int i = 6; i >= 1; i--)
        dl->AddRectFilled(ImVec2(p0.x - i * 3 * u, p0.y + i * 2 * u), ImVec2(p1.x + i * 3 * u, p1.y + i * 4 * u), col(30, 50, 80, 9), (28 + i * 3) * u);
    dl->AddRectFilled(p0, p1, C_PANEL, 28 * u);
    dl->PushClipRect(ImVec2(p0.x + 14 * u, p0.y + 14 * u), ImVec2(p1.x - 14 * u, p1.y - 14 * u), true);
    for (float gx = p0.x; gx < p1.x; gx += 26 * u) dl->AddLine(ImVec2(gx, p0.y), ImVec2(gx, p1.y), col(170, 180, 192, 22), 1.0f);
    for (float gy = p0.y; gy < p1.y; gy += 26 * u) dl->AddLine(ImVec2(p0.x, gy), ImVec2(p1.x, gy), col(170, 180, 192, 22), 1.0f);
    dl->PopClipRect();
    dl->AddRectFilled(ImVec2(p0.x + 4 * u, p0.y + 4 * u), ImVec2(p1.x - 4 * u, p0.y + 120 * u), col(255, 255, 255, 90), 26 * u);
    dl->AddRect(p0, p1, col(255, 255, 255, 255), 28 * u, 0, 4 * u);
    dl->AddRect(ImVec2(p0.x - 1 * u, p0.y - 1 * u), ImVec2(p1.x + 1 * u, p1.y + 1 * u), C_EDGE, 29 * u, 0, 1.0f * u);

    const float pad = 36 * u;
    // kleines rundes Logo wie das Mii-Bild oben links im Wii-U-Menue
    {
        ImVec2 lc(p0.x + pad + 20 * u, p0.y + 50 * u);
        const ImU32 ring[] = {col(230, 60, 60), col(250, 150, 40), col(250, 215, 50), col(80, 190, 90), col(60, 150, 230), col(150, 90, 210)};
        for (int i = 0; i < 6; i++)
            dl->PathArcTo(lc, 19 * u, -1.5708f + i * 1.0472f, -1.5708f + (i + 1) * 1.0472f, 8), dl->PathLineTo(lc), dl->PathFillConvex(ring[i]);
        dl->AddCircleFilled(lc, 11 * u, col(255, 255, 255), 24);
    }
    const float title_x = p0.x + pad + 50 * u;
    text_at(dl, f_big, fs_big, ImVec2(title_x, p0.y + 32 * u), C_ACCENT, "Nintendo Land");
    float tx = title_x + measure(f_big, fs_big, "Nintendo Land").x + 14 * u;
    text_at(dl, f_big, fs_big, ImVec2(tx, p0.y + 32 * u), C_TEXT, "|  Einstellungen");
    std::string ver = "NL Recomp 0.2";
    text_at(dl, f_small, fs_small, ImVec2(p1.x - pad - measure(f_small, fs_small, ver).x, p0.y + 42 * u), C_DIM, ver);

    // Reiter
    const char* tabs[kTabCount] = {"Bild", "Spiel", "Ton", "Steuerung", "Online"};
    float tab_x = p0.x + pad;
    const float tab_y = p0.y + 86 * u, tab_h = 46 * u;
    for (int i = 0; i < kTabCount; i++) {
        float tw = measure(f, fs, tabs[i]).x + 44 * u;
        ImVec2 a(tab_x, tab_y), b(tab_x + tw, tab_y + tab_h);
        bool active = i == s_tab;
        if (active) accent_pill(dl, a, b, tab_h / 2, u);
        else glass_tile(dl, a, b, tab_h / 2, hit(a, b) ? C_ROW_HOT : C_ROW, C_EDGE, u);
        text_center(dl, f, fs, a, b, active ? C_ON_ACCENT : C_TEXT, tabs[i]);
        if (clicked(a, b)) { s_tab = i; s_sel = 0; s_scroll = 0; s_bottom = -1; }
        tab_x += tw + 12 * u;
    }
    text_at(dl, f_small, fs_small, ImVec2(tab_x + 10 * u, tab_y + 14 * u), C_DIM, "Q / E  oder  LB / RB");

    // Bereiche
    const float list_x0 = p0.x + pad, list_x1 = p0.x + pw * 0.62f;
    const float list_y0 = tab_y + tab_h + 24 * u, list_y1 = p1.y - 96 * u;
    const float help_x0 = list_x1 + 24 * u, help_x1 = p1.x - pad;
    glass_tile(dl, ImVec2(help_x0, list_y0), ImVec2(help_x1, list_y1), 18 * u, C_HELP, C_EDGE, u, false);

    const float row_h = 54 * u, gap = 8 * u;
    const bool controls = s_tab == 3;
    float y = list_y0;
    // Kopf der Belegungs-Tabelle
    const float cell_w = (list_x1 - list_x0) * 0.22f;
    const float cells_x = list_x1 - cell_w * 3 - 16 * u;

    // sichtbaren Bereich so schieben, dass die Auswahl drin bleibt
    const int visible = (int)((list_y1 - list_y0) / (row_h + gap));
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + visible) s_scroll = s_sel - visible + 1;
    ImGuiIO& io = ImGui::GetIO();
    if (hit(ImVec2(list_x0, list_y0), ImVec2(list_x1, list_y1)) && io.MouseWheel != 0.0f)
        s_scroll = std::clamp(s_scroll - (int)io.MouseWheel, 0, std::max(0, row_count - visible));

    dl->PushClipRect(ImVec2(list_x0, list_y0), ImVec2(list_x1, list_y1), true);
    for (int i = s_scroll; i < row_count && y + row_h <= list_y1 + 1; i++) {
        Row& r = rows[i];
        ImVec2 a(list_x0, y), b(list_x1, y + row_h);
        if (r.header) {
            text_at(dl, f, fs, ImVec2(a.x + 4 * u, y + row_h - fs - 10 * u), C_ACCENT, r.label);
            if (controls && std::string(r.label) == "Belegung") {
                const char* heads[] = {"Taste 1", "Taste 2", "Controller"};
                for (int c = 0; c < 3; c++)
                    text_center(dl, f_small, fs_small, ImVec2(cells_x + c * (cell_w + 8 * u), y), ImVec2(cells_x + c * (cell_w + 8 * u) + cell_w, y + row_h),
                                C_DIM, heads[c]);
            }
            y += row_h + gap;
            continue;
        }
        const bool sel = i == s_sel && s_bottom < 0;
        glass_tile(dl, a, b, 14 * u, hit(a, b) ? C_ROW_HOT : C_ROW, C_EDGE, u);
        if (sel && r.action < 0) {
            dl->AddRect(ImVec2(a.x - 3 * u, a.y - 3 * u), ImVec2(b.x + 3 * u, b.y + 3 * u), col(0, 174, 239, 70), 17 * u, 0, 4 * u);
            dl->AddRect(a, b, C_SEL, 14 * u, 0, 2.5f * u);
        }
        float label_x = a.x + 20 * u;
        if (r.dot) {
            dl->AddCircleFilled(ImVec2(a.x + 24 * u, y + row_h / 2), 7 * u, r.dot == 2 ? C_ACCENT : col(70, 200, 90), 16);
            label_x += 22 * u;
        }
        text_at(dl, f, fs, ImVec2(label_x, y + (row_h - fs) / 2), C_TEXT, r.label);
        if (r.press || (!r.change && r.action < 0)) {
            // Knopf- oder Info-Zeile: Wert rechts, bei Knoepfen in einer Glas-Pille
            std::string v = r.value ? r.value() : std::string();
            ImVec2 tsz = measure(f, fs, v);
            const float vx1 = b.x - 16 * u, vx0 = vx1 - tsz.x - 40 * u;
            if (r.press && !v.empty()) {
                ImVec2 pa(vx0, y + 8 * u), pb2(vx1, y + row_h - 8 * u);
                if (sel || hit(a, b)) accent_pill(dl, pa, pb2, (row_h - 16 * u) / 2, u);
                else glass_tile(dl, pa, pb2, (row_h - 16 * u) / 2, col(242, 245, 248), C_EDGE, u, false);
                text_center(dl, f, fs, pa, pb2, (sel || hit(a, b)) ? C_ON_ACCENT : C_TEXT, v);
            } else {
                text_at(dl, f, fs, ImVec2(vx1 - tsz.x - 4 * u, y + (row_h - fs) / 2), r.text_entry ? C_ACCENT : C_DIM, v);
            }
            if (clicked(a, b)) { s_sel = i; s_bottom = -1; if (r.press && !r.text_entry) r.press(); }
        } else if (r.action >= 0) {
            const auto& bind = nl_binding(r.action);
            for (int c = 0; c < 3; c++) {
                ImVec2 ca(cells_x + c * (cell_w + 8 * u), y + 6 * u), cb(ca.x + cell_w, y + row_h - 6 * u);
                bool cs = sel && s_col == c;
                bool capturing = s_capture_action == r.action && s_capture_col == c;
                if (capturing) accent_pill(dl, ca, cb, 10 * u, u);
                else {
                    dl->AddRectFilled(ca, cb, hit(ca, cb) ? C_ROW_HOT : col(242, 245, 248), 10 * u);
                    dl->AddRect(ca, cb, C_EDGE, 10 * u, 0, 1.2f * u);
                }
                if (cs) dl->AddRect(ImVec2(ca.x - 2 * u, ca.y - 2 * u), ImVec2(cb.x + 2 * u, cb.y + 2 * u), C_SEL, 12 * u, 0, 2.5f * u);
                std::string t = capturing ? (c < 2 ? "Taste druecken ..." : "Knopf druecken ...")
                                          : (c == 0 ? nl_key_name(bind.key1) : c == 1 ? nl_key_name(bind.key2) : nl_pad_name(bind.pad));
                ImFont* cf = measure(f, fs, t).x > cell_w - 12 * u ? f_small : f;
                float cfs = cf == f ? fs : fs_small;
                text_center(dl, cf, cfs, ca, cb, capturing ? C_ON_ACCENT : C_TEXT, t);
                if (clicked(ca, cb) && s_capture_action < 0) { s_sel = i; s_col = c; s_bottom = -1; start_capture(r.action, c); }
            }
        } else {
            // < Wert >
            const float vx1 = b.x - 20 * u, vx0 = vx1 - (list_x1 - list_x0) * 0.42f;
            const float cy = y + row_h / 2 - (r.is_default() ? 6 * u : 0);
            std::string v = r.value();
            text_center(dl, f, fs, ImVec2(vx0, cy - fs / 2), ImVec2(vx1, cy + fs / 2), C_TEXT, v);
            if (r.is_default()) text_center(dl, f_small, fs_small, ImVec2(vx0, cy + fs / 2 - 2 * u), ImVec2(vx1, cy + fs / 2 + fs_small), C_DIM, "Standard");
            const float as = 14 * u;
            ImVec2 la(vx0, y), lb(vx0 + 40 * u, y + row_h), ra(vx1 - 40 * u, y), rb(vx1, y + row_h);
            (void)as;
            round_arrow(dl, ImVec2(vx0 + 18 * u, y + row_h / 2), 15 * u, false, hit(la, lb) || sel);
            round_arrow(dl, ImVec2(vx1 - 18 * u, y + row_h / 2), 15 * u, true, hit(ra, rb) || sel);
            if (clicked(la, lb)) { s_sel = i; r.change(-1); }
            else if (clicked(ra, rb) || (clicked(a, b) && !hit(la, lb))) { s_sel = i; r.change(+1); }
            if (r.restart) text_at(dl, f_small, fs_small, ImVec2(a.x + 20 * u + measure(f, fs, r.label).x + 12 * u, y + (row_h - fs_small) / 2 + 2 * u),
                                   C_DIM, "(Neustart)");
        }
        if (hit(a, b) && io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y > 0.5f) { s_sel = i; s_bottom = -1; }
        y += row_h + gap;
    }
    dl->PopClipRect();

    // Erklaerung rechts
    std::string help;
    if (s_capture_action >= 0)
        help = s_capture_col < 2 ? "Jetzt die Taste druecken, die du fuer diese Aktion willst.\n\nEsc bricht ab."
                                 : "Jetzt den Controller-Knopf, Stick oder Trigger druecken.\n\nEsc bricht ab.";
    else if (selectable(s_sel)) {
        const Row& r = rows[s_sel];
        help = r.help;
        if (r.action >= 0)
            help += std::string(help.empty() ? "" : "\n\n") +
                    "Feld waehlen und Enter / A druecken, dann die Taste oder den Controller-Knopf. Entf / X leert das Feld. "
                    "Eine Taste gibt es nur einmal: sie wandert hierher. Ein Controller-Knopf tauscht mit der Aktion, die ihn vorher hatte.";
        if (r.restart) help += "\n\nWirkt nach einem Neustart.";
    }
    if (s_tab == 4) {
        if (!s_online_note.empty()) help += "\n\n" + s_online_note;
        std::string st = nl_online_status();
        if (!st.empty() && nl_online_steam_ok()) help += "\n\n" + st;
        std::string enc = nl_online_encoder();
        if (!enc.empty()) help += "\nVideo: " + enc;
    }
    if (controls) {
        std::string gp = nl_gamepad_name();
        help += "\n\n" + (gp.empty() ? std::string("Kein Controller erkannt.") : "Controller: " + gp);
        if (!gp.empty())
            help += nl_pad_has_gyro() ? (nl_pad_gyro_active() ? "\nGyro: aktiv" : "\nGyro: vorhanden, aus") : "\nGyro: keiner";
    }
    {
        ImVec2 tp(help_x0 + 20 * u, list_y0 + 20 * u);
        dl->AddText(f, fs, tp, C_TEXT, help.c_str(), nullptr, help_x1 - help_x0 - 40 * u);
    }

    // Untere Leiste
    bool restart_needed = g_nl.vsync != s_startup.vsync || g_nl.gamepad_window != s_startup.gamepad_window ||
                          g_nl.res_scale != s_startup.res_scale || g_nl.aniso != s_startup.aniso || g_nl.language != s_startup.language ||
                          g_nl.async_shaders != s_startup.async_shaders || g_nl.mic != s_startup.mic;
    if (restart_needed)
        text_at(dl, f_small, fs_small, ImVec2(list_x0, p1.y - 64 * u), C_ACCENT, "Einige Aenderungen wirken erst nach einem Neustart.");
    else
        text_at(dl, f_small, fs_small, ImVec2(list_x0, p1.y - 64 * u), C_DIM, "Esc / B: zurueck zum Spiel      Pfeile oder Stick: auswaehlen");
    const char* btns[] = {"Weiter", s_quit_confirm ? "Wirklich?" : "Neustart", s_quit_confirm ? "Ja, beenden" : "Spiel beenden"};
    if (s_quit_confirm) btns[1] = "Neustart";
    const float bw = 210 * u, bh = 50 * u;
    for (int i = 0; i < 3; i++) {
        ImVec2 a(p1.x - pad - (3 - i) * (bw + 14 * u) + 14 * u, p1.y - 76 * u), b(a.x + bw, a.y + bh);
        bool focus = s_bottom == i;
        if (focus) accent_pill(dl, a, b, bh / 2, u);
        else glass_tile(dl, a, b, bh / 2, hit(a, b) ? C_ROW_HOT : C_ROW, C_EDGE, u);
        text_center(dl, f, fs, a, b, focus ? C_ON_ACCENT : C_TEXT, btns[i]);
        bool press = clicked(a, b) || (focus && in[N_OK]);
        if (press) {
            if (i == 0) nl_menu_toggle();
            else if (i == 1) nl_request(NLRequest::Restart);
            else if (s_quit_confirm) nl_request(NLRequest::Quit);
            else s_quit_confirm = true;
        }
    }
    ImGui::End();
}

// Wird jedes Bild vom Unterbau aufgerufen (pad_view = Bild fuer das GamePad-Fenster)
static void overlay_hook(bool pad_view) {
    if (pad_view && WindowSystem::IsPadWindowOpen()) return;   // Menue nur im Hauptfenster
    // Esc und Home/Guide oeffnen/schliessen das Menue
    bool esc = nl_key_down(VK_ESCAPE);
    bool esc_edge = esc && !s_esc_last;
    s_esc_last = esc;
    if (nl_pad_menu_pressed() || (esc_edge && s_capture_action < 0)) nl_menu_toggle();
    if (!g_nl_menu_open) return;
    int w = 0, h = 0;
    WindowSystem::GetWindowPhysSize(w, h);
    if (w > 0 && h > 0) render_menu(w, h);
}

void nl_menu_install() { g_nl_overlay_hook = overlay_hook; }

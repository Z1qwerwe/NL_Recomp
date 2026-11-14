// Nintendo Land Recomp - Einstellungen und Menue im Spiel (Esc oder Home/Guide am Controller).
//
// Das Menue wird mit ImGui direkt ins Spielbild gezeichnet (ueber den Overlay-Haken im
// Unterbau, siehe nl_menu_install). Bedienbar mit Maus, Tastatur und Controller.
#include "nl_app.h"

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
        else if (k == "button_layout") n.button_layout = std::clamp(v, 0, 1);
        else if (k == "deadzone") n.deadzone = std::clamp(v, 0, 60);
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
      << "\nbutton_layout=" << n.button_layout << "\ndeadzone=" << n.deadzone << "\n";
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
    const char* label = "";
    const char* help = "";
    std::function<std::string()> value;      // angezeigter Wert
    std::function<void(int)> change;         // -1 / +1
    std::function<bool()> is_default;
    bool restart = false;                     // wirkt erst nach Neustart
    bool header = false;                      // Zwischenueberschrift
    int action = -1;                          // Steuerungs-Zeile (Index in kNLActions)
};

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
        rows.push_back(onoff("GamePad-Fenster", "Zeigt das GamePad-Bild in einem zweiten Fenster - praktisch mit zwei Bildschirmen.",
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
        rows.push_back(choice("Startbild", "Welches Bild beim Start im Hauptfenster gezeigt wird. Nintendo Land fragt am Anfang vieles auf dem GamePad.",
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
    } else {   // Steuerung
        rows.push_back(choice("Knopf-Layout", "Nintendo: A ist der rechte Knopf (wie am GamePad). Xbox: A ist der untere Knopf. Gilt fuer die Standard-Belegung.",
                              &n.button_layout, {"Nintendo (A rechts)", "Xbox (A unten)"}, d.button_layout));
        rows.back().change = [](int dir) {
            g_nl.button_layout = std::clamp(g_nl.button_layout + dir, 0, 1);
            nl_input_reset_defaults();
            nl_input_save();
            save_later();
        };
        rows.push_back(percent("Stick-Totzone", "Wie weit der Stick bewegt werden muss, bevor das Spiel reagiert.", &n.deadzone, d.deadzone, 0, 60, 5));
        rows.push_back(header("Belegung"));
        for (int i = 0; i < kNLActionCount; i++) {
            Row r;
            r.label = kNLActions[i].label;
            r.help = kNLActions[i].help;
            r.action = i;
            rows.push_back(r);
        }
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
static int s_tab = 0, s_sel = 0, s_col = 0, s_scroll = 0;
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
static const ImU32 C_PANEL = col(18, 20, 30, 242), C_ROW = col(34, 38, 54, 255), C_ROW_HOT = col(48, 54, 76, 255),
                   C_SEL = col(255, 255, 255, 255), C_TEXT = col(240, 242, 250), C_DIM = col(150, 156, 178),
                   C_ACCENT = col(255, 196, 40), C_HELP = col(26, 29, 42, 255);
static const ImU32 kRainbow[] = {col(230, 60, 60), col(250, 150, 40), col(250, 215, 50), col(80, 190, 90), col(60, 150, 230), col(150, 90, 210)};

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
    NLPadSnapshot ps = nl_pad_snapshot();
    const bool xb = g_nl.button_layout == 1;
    auto pb = [&](int b) { return ps.connected && ((ps.buttons >> b) & 1); };
    const int PAD_OK = xb ? 0 : 1, PAD_BACK = xb ? 1 : 0;   // Nintendo: A rechts, B unten
    bool in[N_COUNT] = {};
    if (s_capture_action < 0) {
        in[N_UP] = nav_edge(N_UP, nl_key_down(VK_UP) || nl_key_down('W') || pb(11) || ps.ly < -0.6f, true);
        in[N_DOWN] = nav_edge(N_DOWN, nl_key_down(VK_DOWN) || nl_key_down('S') || pb(12) || ps.ly > 0.6f, true);
        in[N_LEFT] = nav_edge(N_LEFT, nl_key_down(VK_LEFT) || nl_key_down('A') || pb(13) || ps.lx < -0.6f, true);
        in[N_RIGHT] = nav_edge(N_RIGHT, nl_key_down(VK_RIGHT) || nl_key_down('D') || pb(14) || ps.lx > 0.6f, true);
        in[N_OK] = nav_edge(N_OK, nl_key_down(VK_RETURN) || nl_key_down(VK_SPACE) || pb(PAD_OK), false);
        in[N_BACK] = nav_edge(N_BACK, nl_key_down(VK_BACK) || pb(PAD_BACK), false);
        in[N_TAB_L] = nav_edge(N_TAB_L, nl_key_down('Q') || pb(9), false);
        in[N_TAB_R] = nav_edge(N_TAB_R, nl_key_down('E') || pb(10), false);
        in[N_CLEAR] = nav_edge(N_CLEAR, nl_key_down(VK_DELETE) || pb(xb ? 2 : 3), false);
    } else {
        for (int i = 0; i < N_COUNT; i++) s_nav.held[i] = true;   // nach der Aufnahme nichts ausloesen
    }
    poll_capture();

    if (in[N_TAB_L] || in[N_TAB_R]) {
        s_tab = (s_tab + (in[N_TAB_R] ? 1 : 3)) % 4;
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
            } else {
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
    dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), col(0, 0, 0, 120));

    const float pw = 1400 * u, ph = 800 * u;
    const ImVec2 p0((w - pw) / 2, (h - ph) / 2), p1(p0.x + pw, p0.y + ph);
    dl->AddRectFilled(p0, p1, C_PANEL, 18 * u);
    // Regenbogen-Streifen oben (Nintendo-Land-Farben)
    const float stripe = 8 * u, seg = (pw - 36 * u) / 6;
    for (int i = 0; i < 6; i++)
        dl->AddRectFilled(ImVec2(p0.x + 18 * u + i * seg, p0.y + 10 * u), ImVec2(p0.x + 18 * u + (i + 1) * seg, p0.y + 10 * u + stripe), kRainbow[i],
                          i == 0 || i == 5 ? 4 * u : 0);

    const float pad = 36 * u;
    text_at(dl, f_big, fs_big, ImVec2(p0.x + pad, p0.y + 32 * u), C_ACCENT, "Nintendo Land");
    float tx = p0.x + pad + measure(f_big, fs_big, "Nintendo Land").x + 14 * u;
    text_at(dl, f_big, fs_big, ImVec2(tx, p0.y + 32 * u), C_TEXT, "|  Einstellungen");
    std::string ver = "NL Recomp 0.2";
    text_at(dl, f_small, fs_small, ImVec2(p1.x - pad - measure(f_small, fs_small, ver).x, p0.y + 42 * u), C_DIM, ver);

    // Reiter
    const char* tabs[] = {"Bild", "Spiel", "Ton", "Steuerung"};
    float tab_x = p0.x + pad;
    const float tab_y = p0.y + 86 * u, tab_h = 46 * u;
    for (int i = 0; i < 4; i++) {
        float tw = measure(f, fs, tabs[i]).x + 44 * u;
        ImVec2 a(tab_x, tab_y), b(tab_x + tw, tab_y + tab_h);
        bool active = i == s_tab;
        dl->AddRectFilled(a, b, active ? kRainbow[i + 1] : (hit(a, b) ? C_ROW_HOT : C_ROW), tab_h / 2);
        text_center(dl, f, fs, a, b, active ? col(20, 20, 28) : C_TEXT, tabs[i]);
        if (clicked(a, b)) { s_tab = i; s_sel = 0; s_scroll = 0; s_bottom = -1; }
        tab_x += tw + 12 * u;
    }
    text_at(dl, f_small, fs_small, ImVec2(tab_x + 10 * u, tab_y + 14 * u), C_DIM, "Q / E  oder  LB / RB");

    // Bereiche
    const float list_x0 = p0.x + pad, list_x1 = p0.x + pw * 0.62f;
    const float list_y0 = tab_y + tab_h + 24 * u, list_y1 = p1.y - 96 * u;
    const float help_x0 = list_x1 + 24 * u, help_x1 = p1.x - pad;
    dl->AddRectFilled(ImVec2(help_x0, list_y0), ImVec2(help_x1, list_y1), C_HELP, 12 * u);

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
        dl->AddRectFilled(a, b, hit(a, b) ? C_ROW_HOT : C_ROW, 10 * u);
        if (sel && r.action < 0) dl->AddRect(a, b, C_SEL, 10 * u, 0, 3 * u);
        text_at(dl, f, fs, ImVec2(a.x + 20 * u, y + (row_h - fs) / 2), C_TEXT, r.label);
        if (r.action >= 0) {
            const auto& bind = nl_binding(r.action);
            for (int c = 0; c < 3; c++) {
                ImVec2 ca(cells_x + c * (cell_w + 8 * u), y + 6 * u), cb(ca.x + cell_w, y + row_h - 6 * u);
                bool cs = sel && s_col == c;
                bool capturing = s_capture_action == r.action && s_capture_col == c;
                dl->AddRectFilled(ca, cb, capturing ? C_ACCENT : (hit(ca, cb) ? C_ROW_HOT : col(24, 27, 40)), 8 * u);
                if (cs) dl->AddRect(ca, cb, C_SEL, 8 * u, 0, 3 * u);
                std::string t = capturing ? (c < 2 ? "Taste druecken ..." : "Knopf druecken ...")
                                          : (c == 0 ? nl_key_name(bind.key1) : c == 1 ? nl_key_name(bind.key2) : nl_pad_name(bind.pad));
                ImFont* cf = measure(f, fs, t).x > cell_w - 12 * u ? f_small : f;
                float cfs = cf == f ? fs : fs_small;
                text_center(dl, cf, cfs, ca, cb, capturing ? col(20, 20, 28) : C_TEXT, t);
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
            arrow(dl, ImVec2(vx0 + 18 * u, y + row_h / 2), as, false, hit(la, lb) ? C_ACCENT : C_DIM);
            arrow(dl, ImVec2(vx1 - 18 * u, y + row_h / 2), as, true, hit(ra, rb) ? C_ACCENT : C_DIM);
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
    if (controls) {
        std::string gp = nl_gamepad_name();
        help += "\n\n" + (gp.empty() ? std::string("Kein Controller erkannt.") : "Controller: " + gp);
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
        dl->AddRectFilled(a, b, focus ? C_ACCENT : (hit(a, b) ? C_ROW_HOT : C_ROW), bh / 2);
        text_center(dl, f, fs, a, b, focus ? col(20, 20, 28) : C_TEXT, btns[i]);
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

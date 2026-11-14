// NLOnline.exe - Mitspielen bei einem Freund, der Nintendo Land Recomp hostet.
// Enthaelt keinen Spielcode: zeigt nur das gestreamte TV-Bild, spielt den Ton ab und schickt die
// Controller-Eingaben zum Host. Man wird dort Wiimote-Spieler (2-5).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX   // sonst macht windows.h aus std::max ein Makro
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <xaudio2.h>
#include <mfapi.h>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "../net/nl_proto.h"
#include "../net/nl_steam.h"
#include "../net/nl_video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "xaudio2.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace nlnet;
using clk = std::chrono::steady_clock;

// ===========================================================================
// Zustand
// ===========================================================================
enum class Screen { Home, Finding, Connecting, Playing, Error };

static std::atomic<Screen> s_screen{Screen::Home};
static std::mutex s_mx;                     // schuetzt die Texte und das Bild unten
static std::string s_error;
static std::string s_host_name;
static std::atomic<uint64_t> s_host{0};
static std::atomic<int> s_slot{0};
static std::atomic<int> s_ping_ms{-1};
static std::atomic<bool> s_quit{false};

// Ergebnis der Code-Suche (aus einem Steam-Rueckruf)
static std::mutex s_find_mx;
static int s_find_result = 0;   // 0 nichts, 1 ok, -1 Fehler
static uint64_t s_find_host = 0;
static std::string s_find_err;

// Neuestes dekodiertes Bild
static std::vector<uint8_t> s_frame;
static int s_frame_w = 0, s_frame_h = 0;
static bool s_frame_new = false;

// Eingabe (Hauptthread schreibt, Netz-Thread schickt)
static std::mutex s_in_mx;
static Input s_input;
static std::atomic<bool> s_sideways{true};
static std::atomic<bool> s_overlay{false};   // Esc-Menue offen -> keine Eingaben ans Spiel

static std::string s_code_buf;              // getippter Code
static std::filesystem::path s_ini;

// ===========================================================================
// Einstellungen
// ===========================================================================
static void load_settings() {
    std::ifstream f(s_ini);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "sideways") s_sideways = v != "0";
        else if (k == "last_code") s_code_buf = nl_normalize_code(v);
    }
}
static void save_settings() {
    std::ofstream f(s_ini);
    f << "sideways=" << (s_sideways ? 1 : 0) << "\nlast_code=" << s_code_buf << "\n";
}

// ===========================================================================
// Ton (XAudio2, in Windows eingebaut)
// ===========================================================================
static IXAudio2* s_xa = nullptr;
static IXAudio2MasteringVoice* s_master = nullptr;
static IXAudio2SourceVoice* s_voice = nullptr;
static constexpr int kAudioPool = 48;
static std::vector<uint8_t> s_audio_pool[kAudioPool];
static int s_audio_next = 0;

static bool audio_init() {
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    if (FAILED(s_xa->CreateMasteringVoice(&s_master))) return false;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 48000 * 4;
    if (FAILED(s_xa->CreateSourceVoice(&s_voice, &wf))) return false;
    s_voice->Start();
    return true;
}

static void audio_play(const uint8_t* pcm, size_t bytes) {
    if (!s_voice || bytes == 0) return;
    XAUDIO2_VOICE_STATE st{};
    s_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued > 8) return;   // zu viel im Puffer -> verwerfen, damit die Verzoegerung klein bleibt
    auto& buf = s_audio_pool[s_audio_next];
    s_audio_next = (s_audio_next + 1) % kAudioPool;
    buf.assign(pcm, pcm + bytes);
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = (UINT32)buf.size();
    xb.pAudioData = buf.data();
    s_voice->SubmitSourceBuffer(&xb);
}

// ===========================================================================
// Netz-Thread
// ===========================================================================
static void set_error(const std::string& e) {
    {
        std::scoped_lock l(s_mx);
        s_error = e;
    }
    s_screen = Screen::Error;
}

static void send_simple(uint8_t type) {
    Simple m;
    m.type = type;
    nl_net_send(s_host, CH_CONTROL, &m, sizeof(m), true);
}

static void leave_session(bool tell_host) {
    if (s_host && tell_host) send_simple(MSG_BYE);
    s_host = 0;
    s_slot = 0;
}

static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(clk::now().time_since_epoch()).count();
}

static void net_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    NLVideoDecoder decoder;
    std::vector<uint8_t> bgra;
    uint32_t last_frame = 0;
    bool need_key = true;
    clk::time_point last_hello{}, connect_start{}, last_ping{}, last_input{}, last_key_req{};
    uint32_t input_seq = 0;
    Screen prev = Screen::Home;

    while (!s_quit) {
        nl_steam_tick();
        const Screen scr = s_screen;
        if (scr != prev) {
            if (scr == Screen::Connecting) {
                connect_start = clk::now();
                last_hello = {};
                need_key = true;
                last_frame = 0;
                decoder.reset();
            }
            prev = scr;
        }

        // Code-Suche fertig?
        {
            int res = 0;
            uint64_t host = 0;
            std::string err;
            {
                std::scoped_lock l(s_find_mx);
                res = s_find_result;
                host = s_find_host;
                err = s_find_err;
                s_find_result = 0;
            }
            if (res > 0 && s_screen == Screen::Finding) {
                s_host = host;
                {
                    std::scoped_lock l(s_mx);
                    s_host_name = nl_steam_name_of(host);
                }
                s_screen = Screen::Connecting;
                continue;
            }
            if (res < 0 && s_screen == Screen::Finding) set_error(err);
        }

        if (scr == Screen::Connecting) {
            if (clk::now() - last_hello > std::chrono::seconds(1)) {
                last_hello = clk::now();
                Hello h;
                std::string me = nl_steam_my_name();
                strncpy(h.name, me.c_str(), sizeof(h.name) - 1);
                nl_net_send(s_host, CH_CONTROL, &h, sizeof(h), true);
            }
            if (clk::now() - connect_start > std::chrono::seconds(20)) {
                leave_session(false);
                set_error("Der Host antwortet nicht. Laeuft sein Spiel noch und ist der Raum offen?");
            }
        }

        const uint64_t host = s_host;
        nl_net_receive(CH_CONTROL, [&](uint64_t from, const uint8_t* d, size_t n) {
            if (from != host || n < 1) return;
            switch (d[0]) {
            case MSG_WELCOME:
                if (n >= sizeof(Welcome) && s_screen == Screen::Connecting) {
                    Welcome w;
                    memcpy(&w, d, sizeof(w));
                    s_slot = w.slot;
                    s_screen = Screen::Playing;
                    send_simple(MSG_KEYFRAME);
                }
                break;
            case MSG_FULL:
                if (n >= sizeof(Full)) {
                    const uint8_t reason = d[1];
                    leave_session(false);
                    set_error(reason == 1 ? "Der Host hat eine andere Version von Nintendo Land Recomp."
                                          : reason == 2 ? "Der Raum ist nicht mehr offen." : "Der Raum ist voll (4 Wiimote-Spieler).");
                }
                break;
            case MSG_BYE:
                leave_session(false);
                set_error("Der Host hat den Raum geschlossen.");
                break;
            case MSG_KICK:
                leave_session(false);
                set_error("Der Host hat dich aus dem Raum genommen.");
                break;
            case MSG_PONG:
                if (n >= sizeof(Ping)) {
                    Ping p;
                    memcpy(&p, d, sizeof(p));
                    s_ping_ms = (int)((now_us() - p.t_client_us) / 1000);
                }
                break;
            }
        });

        nl_net_receive(CH_VIDEO, [&](uint64_t from, const uint8_t* d, size_t n) {
            if (from != host || n <= sizeof(VideoHeader) || s_screen != Screen::Playing) return;
            VideoHeader hdr;
            memcpy(&hdr, d, sizeof(hdr));
            const bool gap = last_frame != 0 && hdr.frame != last_frame + 1;
            last_frame = hdr.frame;
            if (gap) need_key = true;
            if (need_key && !hdr.keyframe) {
                if (clk::now() - last_key_req > std::chrono::milliseconds(300)) {
                    last_key_req = clk::now();
                    send_simple(MSG_KEYFRAME);
                }
                return;
            }
            if (hdr.keyframe) need_key = false;
            int w = 0, h = 0;
            if (decoder.decode(d + sizeof(hdr), n - sizeof(hdr), hdr.t_host_us, bgra, w, h)) {
                std::scoped_lock l(s_mx);
                s_frame.swap(bgra);
                s_frame_w = w;
                s_frame_h = h;
                s_frame_new = true;
            }
        });

        nl_net_receive(CH_AUDIO, [&](uint64_t from, const uint8_t* d, size_t n) {
            if (from != host || n <= sizeof(AudioHeader) || s_screen != Screen::Playing) return;
            audio_play(d + sizeof(AudioHeader), n - sizeof(AudioHeader));
        });

        if (s_screen == Screen::Playing && host) {
            if (clk::now() - last_input > std::chrono::milliseconds(8)) {
                last_input = clk::now();
                Input in;
                {
                    std::scoped_lock l(s_in_mx);
                    in = s_input;
                }
                if (s_overlay) in = Input{};
                in.seq = ++input_seq;
                in.flags = s_sideways ? IN_SIDEWAYS : 0;
                nl_net_send(host, CH_INPUT, &in, sizeof(in), false);
            }
            if (clk::now() - last_ping > std::chrono::seconds(1)) {
                last_ping = clk::now();
                Ping p;
                p.t_client_us = now_us();
                nl_net_send(host, CH_CONTROL, &p, sizeof(p), false);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    decoder.reset();
    MFShutdown();
    CoUninitialize();
}

static void start_join(const std::string& code_in) {
    std::string code = nl_normalize_code(code_in);
    if (code.size() != 6) {
        set_error("Der Code hat 6 Zeichen (Buchstaben und Ziffern).");
        return;
    }
    s_code_buf = code;
    save_settings();
    s_screen = Screen::Finding;
    nl_steam_find_code(code, [](bool ok, uint64_t host, std::string err) {
        std::scoped_lock l(s_find_mx);   // kommt aus einem Steam-Rueckruf: nur ablegen
        s_find_result = ok ? 1 : -1;
        s_find_host = host;
        s_find_err = err;
    });
}

// ===========================================================================
// Controller (SDL3) und Tastatur
// ===========================================================================
static SDL_Gamepad* s_pad = nullptr;
static std::string s_pad_name;

static void pad_open_first() {
    if (s_pad) return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) {
        s_pad = SDL_OpenGamepad(ids[0]);
        const char* n = s_pad ? SDL_GetGamepadName(s_pad) : nullptr;
        s_pad_name = n ? n : "Controller";
    }
    SDL_free(ids);
}

static void poll_input(HWND hwnd) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED) pad_open_first();
        if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && s_pad && SDL_GetGamepadID(s_pad) == ev.gdevice.which) {
            SDL_CloseGamepad(s_pad);
            s_pad = nullptr;
            s_pad_name.clear();
            pad_open_first();
        }
    }
    SDL_UpdateGamepads();
    Input in;
    if (s_pad) {
        for (int b = 0; b < 21; b++)
            if (SDL_GetGamepadButton(s_pad, (SDL_GamepadButton)b)) in.buttons |= 1u << b;
        in.lx = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX);
        in.ly = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY);
        in.rx = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTX);
        in.ry = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTY);
        in.lt = (uint8_t)std::clamp(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128, 0, 255);
        in.rt = (uint8_t)std::clamp(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128, 0, 255);
    }
    // Tastatur, solange das Fenster vorne ist
    if (GetForegroundWindow() == hwnd) {
        auto k = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        auto btn = [&](int bit, bool on) { if (on) in.buttons |= 1u << bit; };
        btn(0, k(VK_SPACE) || k('J'));       // unten  -> A / 2
        btn(1, k('K'));                      // rechts -> 1
        btn(2, k('L'));                      // links  -> B
        btn(3, k('I'));                      // oben   -> C
        btn(4, k(VK_BACK));                  // -
        btn(6, k(VK_RETURN));                // +
        btn(9, k('Q'));                      // C
        btn(10, k('E'));                     // B
        btn(11, k(VK_UP));
        btn(12, k(VK_DOWN));
        btn(13, k(VK_LEFT));
        btn(14, k(VK_RIGHT));
        if (k('W')) in.ly = -32767;
        if (k('S')) in.ly = 32767;
        if (k('A')) in.lx = -32767;
        if (k('D')) in.lx = 32767;
        if (k(VK_SHIFT)) in.lt = 255;        // Z
        if (k(VK_CONTROL)) in.rt = 255;      // B
    }
    std::scoped_lock l(s_in_mx);
    s_input = in;
}

// ===========================================================================
// Fenster, Direct3D 11, ImGui
// ===========================================================================
static ID3D11Device* s_dev = nullptr;
static ID3D11DeviceContext* s_ctx = nullptr;
static IDXGISwapChain* s_swap = nullptr;
static ID3D11RenderTargetView* s_rtv = nullptr;
static ID3D11Texture2D* s_tex = nullptr;
static ID3D11ShaderResourceView* s_srv = nullptr;
static int s_tex_w = 0, s_tex_h = 0;
static UINT s_resize_w = 0, s_resize_h = 0;
static bool s_fullscreen = false;
static WINDOWPLACEMENT s_prev_place{sizeof(WINDOWPLACEMENT)};

static void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    s_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        s_dev->CreateRenderTargetView(back, nullptr, &s_rtv);
        back->Release();
    }
}
static void release_rtv() {
    if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
}

static bool d3d_init(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &s_swap, &s_dev, &fl, &s_ctx)) &&
        FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &s_swap, &s_dev, &fl, &s_ctx)))
        return false;
    create_rtv();
    return true;
}

static void upload_frame() {
    std::scoped_lock l(s_mx);
    if (!s_frame_new || s_frame_w <= 0) return;
    s_frame_new = false;
    if (s_frame_w != s_tex_w || s_frame_h != s_tex_h) {
        if (s_srv) { s_srv->Release(); s_srv = nullptr; }
        if (s_tex) { s_tex->Release(); s_tex = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = s_frame_w;
        td.Height = s_frame_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s_dev->CreateTexture2D(&td, nullptr, &s_tex))) return;
        s_dev->CreateShaderResourceView(s_tex, nullptr, &s_srv);
        s_tex_w = s_frame_w;
        s_tex_h = s_frame_h;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(s_ctx->Map(s_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int y = 0; y < s_frame_h; y++)
            memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, s_frame.data() + (size_t)y * s_frame_w * 4, (size_t)s_frame_w * 4);
        s_ctx->Unmap(s_tex, 0);
    }
}

static void toggle_fullscreen(HWND hwnd) {
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!s_fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(hwnd, &s_prev_place) && GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &s_prev_place);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    s_fullscreen = !s_fullscreen;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { s_resize_w = LOWORD(lp); s_resize_h = HIWORD(lp); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11) toggle_fullscreen(hwnd);
        if (wp == VK_ESCAPE && s_screen == Screen::Playing) s_overlay = !s_overlay;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { toggle_fullscreen(hwnd); return 0; }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Oberflaeche (Wii-U-Glas-Look wie im Spielmenue)
// ===========================================================================
static ImU32 col(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
static const ImU32 C_ACCENT = IM_COL32(0, 174, 239, 255);

static void style_glass() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 26;
    st.ChildRounding = 18;
    st.FrameRounding = 14;
    st.GrabRounding = 14;
    st.PopupRounding = 14;
    st.WindowBorderSize = 2;
    st.FrameBorderSize = 1;
    st.WindowPadding = ImVec2(28, 24);
    st.FramePadding = ImVec2(16, 10);
    st.ItemSpacing = ImVec2(12, 12);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImVec4(0.12f, 0.16f, 0.22f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.50f, 0.56f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.965f, 0.972f, 0.98f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(1, 1, 1, 0.75f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.95f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.92f, 0.97f, 0.99f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.86f, 0.95f, 0.99f, 1);
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.95f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.0f, 0.68f, 0.94f, 0.85f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.0f, 0.60f, 0.85f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.0f, 0.68f, 0.94f, 1);
    c[ImGuiCol_Header] = ImVec4(0.0f, 0.68f, 0.94f, 0.25f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.68f, 0.94f, 0.35f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.75f, 0.80f, 0.86f, 1);
}

static void draw_background(ImDrawList* dl, ImVec2 size) {
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, col(232, 238, 244), col(232, 238, 244), col(196, 208, 220), col(196, 208, 220));
    for (float x = 0; x < size.x; x += 28) dl->AddLine(ImVec2(x, 0), ImVec2(x, size.y), col(170, 180, 192, 26));
    for (float y = 0; y < size.y; y += 28) dl->AddLine(ImVec2(0, y), ImVec2(size.x, y), col(170, 180, 192, 26));
}

static void logo(ImDrawList* dl, ImVec2 c, float r) {
    const ImU32 ring[] = {col(230, 60, 60), col(250, 150, 40), col(250, 215, 50), col(80, 190, 90), col(60, 150, 230), col(150, 90, 210)};
    for (int i = 0; i < 6; i++) {
        dl->PathArcTo(c, r, -1.5708f + i * 1.0472f, -1.5708f + (i + 1) * 1.0472f, 8);
        dl->PathLineTo(c);
        dl->PathFillConvex(ring[i]);
    }
    dl->AddCircleFilled(c, r * 0.58f, col(255, 255, 255), 24);
}

static bool accent_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.68f, 0.94f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.75f, 0.98f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

static int code_filter(ImGuiInputTextCallbackData* d) {
    if (d->EventChar < 256) {
        char ch = (char)toupper((int)d->EventChar);
        if (nl_normalize_code(std::string(1, ch)).empty()) return 1;
        d->EventChar = (ImWchar)ch;
    }
    return 0;
}

static std::vector<NLFriend> s_friends;
static clk::time_point s_friends_time{};

static void ui_home(ImVec2 size, ImFont* big) {
    const float pw = (std::min)(1100.0f, size.x - 60), ph = (std::min)(640.0f, size.y - 60);
    ImGui::SetNextWindowPos(ImVec2((size.x - pw) / 2, (size.y - ph) / 2));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##home", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    logo(dl, ImVec2(wp.x + 50, wp.y + 52), 20);
    ImGui::SetCursorPos(ImVec2(84, 30));
    ImGui::PushFont(big);
    ImGui::TextColored(ImVec4(0.0f, 0.68f, 0.94f, 1), "Nintendo Land");
    ImGui::SameLine();
    ImGui::TextUnformatted("|  Online mitspielen");
    ImGui::PopFont();
    ImGui::SetCursorPosY(96);

    const float left_w = pw * 0.42f;
    ImGui::BeginChild("##left", ImVec2(left_w, ph - 150), true);
    ImGui::TextDisabled("Mit Code beitreten");
    ImGui::SetNextItemWidth(-1);
    ImGui::PushFont(big);
    char buf[16];
    strncpy(buf, s_code_buf.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    bool enter = ImGui::InputTextWithHint("##code", "CODE", buf, 7, ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
                                          code_filter);
    s_code_buf = nl_normalize_code(buf);
    ImGui::PopFont();
    if (accent_button("Beitreten", ImVec2(-1, 52)) || enter) start_join(s_code_buf);
    ImGui::Spacing();
    ImGui::TextWrapped("Den Code siehst du beim Host im Spielmenue unter Online. Du spielst als Wiimote mit - das Spiel brauchst du dafuer nicht.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    bool sw = s_sideways;
    if (ImGui::Checkbox("Wiimote quer halten", &sw)) { s_sideways = sw; save_settings(); }
    ImGui::TextDisabled("An: fuer Mario Chase, Luigi's Mansion, Animal Crossing.\nAus: Wiimote aufrecht mit Nunchuk.");
    ImGui::Spacing();
    ImGui::TextDisabled("Controller: %s", s_pad_name.empty() ? "keiner (Tastatur: WASD, Leertaste, K, L, I)" : s_pad_name.c_str());
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0, ph - 150), true);
    ImGui::TextDisabled("Steam-Freunde");
    if (clk::now() - s_friends_time > std::chrono::seconds(1)) {
        s_friends = nl_steam_friends();
        s_friends_time = clk::now();
    }
    if (s_friends.empty()) ImGui::TextWrapped("Gerade ist keiner deiner Steam-Freunde online.");
    for (auto& f : s_friends) {
        ImGui::PushID((int)(f.id & 0x7fffffff));
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float row_h = 48;
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + row_h), col(255, 255, 255, 235), 12);
        if (f.in_nl)
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 18, p.y + row_h / 2), 7, f.code.empty() ? col(70, 200, 90) : C_ACCENT, 16);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 34, p.y + 6));
        ImGui::TextUnformatted(f.name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x + 34, p.y + 26));
        ImGui::TextDisabled("%s", !f.code.empty() ? "hostet einen Raum" : f.in_nl ? "spielt Nintendo Land" : "online");
        if (!f.code.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetContentRegionAvail().x - 150, p.y + 6));
            if (accent_button("Beitreten", ImVec2(140, 36))) start_join(f.code);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h + 8));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Angemeldet als %s   |   F11: Vollbild   |   Esc im Spiel: Menue", nl_steam_my_name().c_str());
    ImGui::End();
}

static void ui_message(ImVec2 size, ImFont* big, const std::string& title, const std::string& text, bool cancel, bool back) {
    const float pw = (std::min)(720.0f, size.x - 60), ph = 300;
    ImGui::SetNextWindowPos(ImVec2((size.x - pw) / 2, (size.y - ph) / 2));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##msg", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(big);
    ImGui::TextColored(ImVec4(0.0f, 0.68f, 0.94f, 1), "%s", title.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::SetCursorPosY(ph - 84);
    if (cancel && ImGui::Button("Abbrechen", ImVec2(200, 50))) {
        leave_session(true);
        s_screen = Screen::Home;
    }
    if (back && accent_button("Zurueck", ImVec2(200, 50))) s_screen = Screen::Home;
    ImGui::End();
}

static void ui_playing(ImVec2 size, ImFont* big) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), size, col(0, 0, 0));
    if (s_srv && s_tex_w > 0) {
        float scale = (std::min)(size.x / s_tex_w, size.y / s_tex_h);
        float w = s_tex_w * scale, h = s_tex_h * scale;
        ImVec2 p0((size.x - w) / 2, (size.y - h) / 2);
        bg->AddImage((ImTextureID)s_srv, p0, ImVec2(p0.x + w, p0.y + h));
    } else {
        const char* t = "Warte auf das Bild ...";
        ImVec2 ts = ImGui::CalcTextSize(t);
        bg->AddText(ImVec2((size.x - ts.x) / 2, (size.y - ts.y) / 2), col(220, 225, 230), t);
    }
    // kleine Info oben links
    {
        std::string info = "Wiimote " + std::to_string(s_slot.load());
        if (s_ping_ms >= 0) info += "   " + std::to_string(s_ping_ms.load()) + " ms";
        ImVec2 ts = ImGui::CalcTextSize(info.c_str());
        bg->AddRectFilled(ImVec2(12, 12), ImVec2(36 + ts.x, 24 + ts.y + 12), col(255, 255, 255, 170), 14);
        bg->AddText(ImVec2(24, 18), col(30, 40, 55), info.c_str());
    }
    if (!s_overlay) return;
    const float pw = 520, ph = 400;
    ImGui::SetNextWindowPos(ImVec2((size.x - pw) / 2, (size.y - ph) / 2));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##pause", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(big);
    ImGui::TextColored(ImVec4(0.0f, 0.68f, 0.94f, 1), "Pause");
    ImGui::PopFont();
    std::string host_name;
    {
        std::scoped_lock l(s_mx);
        host_name = s_host_name;
    }
    ImGui::TextDisabled("Bei %s - du bist Wiimote %d", host_name.c_str(), s_slot.load());
    ImGui::Spacing();
    if (accent_button("Weiterspielen", ImVec2(-1, 52))) s_overlay = false;
    bool sw = s_sideways;
    if (ImGui::Checkbox("Wiimote quer halten", &sw)) { s_sideways = sw; save_settings(); }
    ImGui::Spacing();
    if (ImGui::Button("Vollbild an/aus (F11)", ImVec2(-1, 48))) toggle_fullscreen(GetActiveWindow());
    if (ImGui::Button("Raum verlassen", ImVec2(-1, 48))) {
        leave_session(true);
        s_overlay = false;
        s_screen = Screen::Home;
    }
    ImGui::End();
}

// ===========================================================================
int main(int argc, char* argv[]) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    s_ini = std::filesystem::path(exe).parent_path() / "NLOnline.ini";
    load_settings();

    std::string join_arg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "--join" || a == "+nl_join") && i + 1 < argc) join_arg = argv[++i];
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetEnvironmentVariableW(L"SteamAppId", L"480");
    SetEnvironmentVariableW(L"SteamGameId", L"480");
    const bool steam = nl_steam_available() && nl_steam_init();
    if (steam) {
        nl_steam_set_status("Spielt online mit");
        nl_steam_on_join_request([](std::string code) {
            if (s_screen == Screen::Home || s_screen == Screen::Error) start_join(code);
        });
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_Init(SDL_INIT_GAMEPAD);
    pad_open_first();
    audio_init();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"NLOnline";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Nintendo Land - Online", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!d3d_init(hwnd)) {
        MessageBoxW(nullptr, L"Direct3D 11 konnte nicht gestartet werden.", L"Nintendo Land - Online", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFont* font = nullptr;
    ImFont* big = nullptr;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::filesystem::path seg = std::filesystem::path(windir) / "Fonts" / "segoeui.ttf";
    if (std::filesystem::exists(seg)) {
        font = io.Fonts->AddFontFromFileTTF(seg.string().c_str(), 21.0f);
        big = io.Fonts->AddFontFromFileTTF(seg.string().c_str(), 34.0f);
    }
    if (!font) font = io.Fonts->AddFontDefault();
    if (!big) big = font;
    style_glass();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(s_dev, s_ctx);

    std::thread net;
    if (steam) {
        net = std::thread(net_loop);
        if (!join_arg.empty()) start_join(join_arg);
    }

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (s_resize_w) {
            release_rtv();
            s_swap->ResizeBuffers(0, s_resize_w, s_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            s_resize_w = s_resize_h = 0;
            create_rtv();
        }
        poll_input(hwnd);
        upload_frame();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const ImVec2 size = io.DisplaySize;
        const Screen scr = s_screen;
        if (scr != Screen::Playing) draw_background(ImGui::GetBackgroundDrawList(), size);
        if (!steam) {
            ui_message(size, big, "Steam laeuft nicht",
                       nl_steam_available() ? "Online laeuft ueber Steam (Freundesliste, Codes, Verbindung). Starte Steam und dann NLOnline neu."
                                            : "steam_api64.dll fehlt neben NLOnline.exe. Entpacke das ZIP bitte komplett.",
                       false, false);
        } else if (scr == Screen::Home) {
            ui_home(size, big);
        } else if (scr == Screen::Finding) {
            ui_message(size, big, "Suche Raum ...", "Code " + s_code_buf, true, false);
        } else if (scr == Screen::Connecting) {
            std::string hn;
            {
                std::scoped_lock l(s_mx);
                hn = s_host_name;
            }
            ui_message(size, big, "Verbinde ...", "Mit " + hn + ". Das kann ein paar Sekunden dauern.", true, false);
        } else if (scr == Screen::Playing) {
            ui_playing(size, big);
        } else {
            std::string e;
            {
                std::scoped_lock l(s_mx);
                e = s_error;
            }
            ui_message(size, big, "Hat nicht geklappt", e, false, true);
        }
        ImGui::Render();
        const float clear[4] = {0, 0, 0, 1};
        s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
        s_ctx->ClearRenderTargetView(s_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        s_swap->Present(1, 0);
    }

    leave_session(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // Tschuess noch rausschicken
    s_quit = true;
    if (net.joinable()) net.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (s_voice) s_voice->DestroyVoice();
    if (s_master) s_master->DestroyVoice();
    if (s_xa) s_xa->Release();
    if (s_pad) SDL_CloseGamepad(s_pad);
    SDL_Quit();
    nl_steam_shutdown();
    return 0;
}

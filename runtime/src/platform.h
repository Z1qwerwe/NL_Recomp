// Plattform-Schicht: Fenster, Eingabe, Mikrofon.
// Mit SDL3 (Windows) oder ohne Fenster (Tests, NL_HEADLESS).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

enum : int { TARGET_TV = 0, TARGET_DRC = 1 };

// Was das Spiel als GamePad (VPAD) sieht
struct VpadState {
    uint32_t buttons = 0;          // VPAD_BUTTON_*
    float lx = 0, ly = 0, rx = 0, ry = 0;
    bool touch = false;
    float touch_x = 0, touch_y = 0; // 0..1 auf dem GamePad-Bildschirm
};

// Was das Spiel als Wiimote (KPAD) sieht
struct WiimoteState {
    bool connected = false;
    uint32_t buttons = 0;          // WPAD_BUTTON_*
    float pointer_x = 0, pointer_y = 0;  // -1..1
    bool nunchuk = false;
    float nx = 0, ny = 0;
};

// Startet die Plattform. Gibt false zurueck, wenn kein Fenster moeglich ist.
bool platform_init();
// Laesst `game` in einem eigenen Thread laufen und kuemmert sich im Hauptthread
// um Fenster und Eingabe, bis das Fenster geschlossen wird.
int platform_run(const std::function<int()>& game);

// Vom Spiel-Thread: neues Bild fuer TV oder GamePad (RGBA8, Zeile fuer Zeile).
void platform_present(int target, int width, int height, const uint32_t* rgba);
void platform_set_stats(uint64_t frames, uint32_t draws_last_frame);

VpadState platform_vpad();
WiimoteState platform_wiimote(int index);

// Mikrofon: 16-bit Mono, 32 kHz. Liefert, wie viele Samples gelesen wurden.
bool platform_mic_start();
void platform_mic_stop();
size_t platform_mic_read(int16_t* out, size_t max_samples);

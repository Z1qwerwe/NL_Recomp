// Plattform ohne Fenster (fuer Tests auf Servern / ohne SDL).
#include "platform.h"
#include <cstdio>

bool platform_init() { return false; }
int platform_run(const std::function<int()>& game) { return game(); }

static uint64_t g_frames = 0;
void platform_present(int target, int width, int height, const uint32_t* rgba) {
    if (target == TARGET_TV && (g_frames++ % 60) == 0 && rgba)
        printf("[Bild] TV-Frame %llu: %dx%d, Pixel(0,0)=0x%08X\n",
               (unsigned long long)g_frames, width, height, rgba[0]);
}
void platform_set_stats(uint64_t, uint32_t) {}
VpadState platform_vpad() { return {}; }
WiimoteState platform_wiimote(int) { return {}; }
bool platform_mic_start() { return false; }
void platform_mic_stop() {}
size_t platform_mic_read(int16_t*, size_t) { return 0; }

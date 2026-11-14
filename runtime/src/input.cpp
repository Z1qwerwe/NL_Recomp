// Eingabe: VPAD (GamePad), KPAD/WPAD (Wiimotes), Mikrofon, ProcUI.
#include "hle.h"
#include "input.h"
#include "platform.h"

#include <cstdio>
#include <vector>

static void stf(uint32_t a, float v) { stf32(a, v); }

// --- VPAD ------------------------------------------------------------------
static uint32_t g_vpad_prev = 0;

// int32 VPADRead(chan, VPADStatus* buffers, count, VPADReadError* err)
static void h_VPADRead(PPCContext& c) {
    uint32_t out = c.gpr[4], count = c.gpr[5], err = c.gpr[6];
    if (c.gpr[3] != 0 || !count) {
        if (err) st32(err, (uint32_t)-2);   // VPAD_READ_INVALID_CONTROLLER
        c.gpr[3] = 0;
        return;
    }
    VpadState v = platform_vpad();
    memset(g_base + out, 0, 0xAC);
    st32(out + 0x00, v.buttons);
    st32(out + 0x04, v.buttons & ~g_vpad_prev);
    st32(out + 0x08, g_vpad_prev & ~v.buttons);
    g_vpad_prev = v.buttons;
    stf(out + 0x0C, v.lx); stf(out + 0x10, v.ly);
    stf(out + 0x14, v.rx); stf(out + 0x18, v.ry);
    stf(out + 0x1C, 0); stf(out + 0x20, -1.f); stf(out + 0x24, 0);   // Beschleunigung: GamePad liegt flach
    stf(out + 0x28, 1.f);
    // Touch (Rohwerte 0..4095); VPADGetTPCalibratedPoint rechnet um
    for (uint32_t tp : {0x52u, 0x5Au, 0x62u}) {
        st16(out + tp + 0, v.touch ? (uint32_t)(v.touch_x * 4095.f) : 0);
        st16(out + tp + 2, v.touch ? (uint32_t)(v.touch_y * 4095.f) : 0);
        st16(out + tp + 4, v.touch ? 1 : 0);
        st16(out + tp + 6, 0);
    }
    // Richtung (Gyro): Einheitsmatrix
    stf(out + 0x6C, 1.f); stf(out + 0x7C, 1.f); stf(out + 0x8C, 1.f);
    st8(out + 0xA0, 0xFF);   // Lautstaerke-Regler
    st8(out + 0xA1, 0xC0);   // Akku
    if (err) st32(err, 0);
    c.gpr[3] = 1;
}

// VPADGetTPCalibratedPoint(chan, VPADTouchData* out, const VPADTouchData* in)  -> 1280x720
static void h_VPADGetTPCalibratedPoint(PPCContext& c) {
    uint32_t out = c.gpr[4], in = c.gpr[5];
    uint32_t x = ld16(in), y = ld16(in + 2);
    st16(out + 0, x * 1280 / 4096);
    st16(out + 2, y * 720 / 4096);
    st16(out + 4, ld16(in + 4));
    st16(out + 6, ld16(in + 6));
}

// --- WPAD / KPAD (Wiimotes) -------------------------------------------------
static uint32_t g_kpad_prev[4] = {0, 0, 0, 0};

// int32 WPADProbe(chan, u32* type) -> 0 = verbunden, -1 = keiner
static void h_WPADProbe(PPCContext& c) {
    WiimoteState w = platform_wiimote((int)c.gpr[3]);
    if (!w.connected) { c.gpr[3] = (uint32_t)-1; return; }
    if (c.gpr[4]) st32(c.gpr[4], w.nunchuk ? 1 : 0);   // WPAD_EXT_CORE / NUNCHUK
    c.gpr[3] = 0;
}

// int32 KPADReadEx(chan, KPADStatus* data, size, KPADError* err)
static void h_KPADReadEx(PPCContext& c) {
    int ch = (int)c.gpr[3];
    uint32_t out = c.gpr[4], count = c.gpr[5], err = c.gpr[6];
    WiimoteState w = platform_wiimote(ch);
    if (!w.connected || !count || ch < 0 || ch > 3) {
        if (err) st32(err, (uint32_t)-2);   // KPAD_ERROR_INVALID_CONTROLLER
        c.gpr[3] = 0;
        return;
    }
    memset(g_base + out, 0, 0xF0);
    uint32_t prev = g_kpad_prev[ch];
    st32(out + 0x00, w.buttons);
    st32(out + 0x04, w.buttons & ~prev);
    st32(out + 0x08, prev & ~w.buttons);
    g_kpad_prev[ch] = w.buttons;
    stf(out + 0x0C, 0); stf(out + 0x10, -1.f); stf(out + 0x14, 0);   // Beschleunigung
    stf(out + 0x18, 1.f);
    stf(out + 0x20, w.pointer_x); stf(out + 0x24, w.pointer_y);       // Zeiger
    stf(out + 0x34, 1.f); stf(out + 0x38, 0);                         // Winkel
    stf(out + 0x48, 1.5f);                                            // Abstand
    st8(out + 0x5C, w.nunchuk ? 1 : 0);                               // Erweiterung
    st8(out + 0x5D, 0);                                               // kein Fehler
    st8(out + 0x5E, 1);                                               // Zeiger gueltig
    if (w.nunchuk) {
        stf(out + 0x60, w.nx); stf(out + 0x64, w.ny);                 // Nunchuk-Stick
    }
    if (err) st32(err, 0);
    c.gpr[3] = 1;
}

static void h_KPADGetMplsWorkSize(PPCContext& c) { c.gpr[3] = 0x5FE0; }
static void h_WPADGetBatteryLevel(PPCContext& c) { c.gpr[3] = 4; }
static void h_ret0(PPCContext& c) { c.gpr[3] = 0; }
static void h_ret1(PPCContext& c) { c.gpr[3] = 1; }
static void h_nop(PPCContext&) {}

// --- Mikrofon (GamePad) -------------------------------------------------------
// MICInit(instance, unused, MICWorkMemory* {u32 size; void* buffer}, MICError* err) -> Handle
static uint32_t g_mic_buf = 0, g_mic_size = 0, g_mic_write = 0, g_mic_avail = 0;
static bool g_mic_open = false;

static void h_MICInit(PPCContext& c) {
    uint32_t wm = c.gpr[5];
    if (wm) {
        g_mic_size = ld32(wm + 0);
        g_mic_buf = ld32(wm + 4);
    }
    if (c.gpr[6]) st32(c.gpr[6], 0);
    printf("[Mikro] MICInit: Puffer 0x%08X, %u Bytes\n", g_mic_buf, g_mic_size);
    c.gpr[3] = 1;   // Handle
}
static void h_MICOpen(PPCContext& c) {
    g_mic_open = platform_mic_start();
    g_mic_write = g_mic_avail = 0;
    c.gpr[3] = 0;
}
static void h_MICClose(PPCContext& c) { platform_mic_stop(); g_mic_open = false; c.gpr[3] = 0; }

// Neue Samples vom PC-Mikrofon in den Ringpuffer des Spiels schreiben (Big-Endian 16 bit)
static void mic_pump() {
    if (!g_mic_open || !g_mic_buf || g_mic_size < 2) return;
    int16_t tmp[2048];
    size_t n = platform_mic_read(tmp, 2048);
    uint32_t samples = g_mic_size / 2;
    for (size_t i = 0; i < n; i++) {
        st16(g_mic_buf + g_mic_write * 2, (uint16_t)tmp[i]);
        g_mic_write = (g_mic_write + 1) % samples;
        if (g_mic_avail < samples) g_mic_avail++;
    }
}

// MICGetStatus(handle, MICStatus* {u32 state; u32 available; u32 readPos})
static void h_MICGetStatus(PPCContext& c) {
    mic_pump();
    uint32_t st = c.gpr[4];
    uint32_t samples = g_mic_size / 2 ? g_mic_size / 2 : 1;
    if (st) {
        st32(st + 0, g_mic_open ? 1 : 0);
        st32(st + 4, g_mic_avail);
        st32(st + 8, (g_mic_write + samples - g_mic_avail) % samples);
    }
    c.gpr[3] = 0;
}
// MICSetDataConsumed(handle, samples)
static void h_MICSetDataConsumed(PPCContext& c) {
    g_mic_avail = c.gpr[4] > g_mic_avail ? 0 : g_mic_avail - c.gpr[4];
    c.gpr[3] = 0;
}

// --- ProcUI ---------------------------------------------------------------------
// Das Spiel ist immer im Vordergrund
static void h_ProcUIProcessMessages(PPCContext& c) { c.gpr[3] = 0; }   // PROCUI_STATUS_IN_FOREGROUND

void input_register(void (*reg)(const char*, const char*, void (*)(PPCContext&))) {
    reg("vpad", "VPADRead", h_VPADRead);
    reg("vpad", "VPADGetTPCalibratedPoint", h_VPADGetTPCalibratedPoint);
    for (const char* n : {"VPADControlMotor", "VPADStopMotor", "VPADDisableGyroDirRevise",
                          "VPADEnableGyroDirRevise", "VPADSetGyroDirReviseBase", "VPADSetGyroDirReviseParam",
                          "VPADSetGyroDirection", "VPADSetGyroZeroDriftMode"})
        reg("vpad", n, h_nop);
    reg("vpad", "VPADGetGyroZeroDriftMode", h_nop);
    reg("vpadbase", "VPADBASEGetHeadphoneStatus", h_ret0);

    const char* p = "padscore";
    reg(p, "WPADProbe", h_WPADProbe);
    reg(p, "KPADReadEx", h_KPADReadEx);
    reg(p, "KPADGetMplsWorkSize", h_KPADGetMplsWorkSize);
    reg(p, "WPADGetBatteryLevel", h_WPADGetBatteryLevel);
    reg(p, "WPADIsMplsAttached", h_ret0);
    reg(p, "WPADIsMotorEnabled", h_ret1);
    reg(p, "WPADCanSendStreamData", h_ret0);
    reg(p, "KPADIsEnableMplsDpdRevise", h_ret0);
    for (const char* n : {"KPADInitEx", "KPADSetMplsWorkarea", "KPADDisableMpls", "KPADEnableMpls",
                          "KPADDisableMplsDirRevise", "KPADEnableMplsDirRevise", "KPADDisableMplsDpdRevise",
                          "KPADEnableMplsDpdRevise", "KPADEnableStickCrossClamp", "KPADGetMplsZeroDriftMode",
                          "KPADSetMplsDirReviseBase", "KPADSetMplsDirReviseParam", "KPADSetMplsDirection",
                          "KPADSetMplsDpdReviseParam", "KPADSetMplsZeroDriftMode", "KPADSetPosParam",
                          "WPADControlMotor", "WPADControlSpeaker", "WPADEnableURCC", "WPADSendStreamData",
                          "WPADGetSpeakerVolume", "WENCGetEncodeData"})
        reg(p, n, h_nop);

    reg("mic", "MICInit", h_MICInit);
    reg("mic", "MICOpen", h_MICOpen);
    reg("mic", "MICClose", h_MICClose);
    reg("mic", "MICUninit", h_MICClose);
    reg("mic", "MICGetStatus", h_MICGetStatus);
    reg("mic", "MICSetDataConsumed", h_MICSetDataConsumed);
    reg("mic", "MICGetState", h_ret0);
    reg("mic", "MICSetState", h_ret0);

    reg("proc_ui", "ProcUIInit", h_nop);
    reg("proc_ui", "ProcUIRegisterCallback", h_nop);
    reg("proc_ui", "ProcUIProcessMessages", h_ProcUIProcessMessages);
    reg("proc_ui", "ProcUIInShutdown", h_ret0);
    reg("proc_ui", "ProcUIDrawDoneRelease", h_nop);
    reg("proc_ui", "ProcUIShutdown", h_nop);
}

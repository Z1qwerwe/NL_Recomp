// GX2 (Grafik der Wii U) - erste Stufe.
//
// Was schon geht: Speichergroessen/Formate berechnen, Bildschirm-Puffer,
// Clear (ClearColor/ClearBuffersEx), Kopieren in den Scan-Buffer und Anzeigen
// im Fenster (TV + GamePad), Frame-Takt (VSync), Draw-Calls zaehlen.
// Was noch fehlt: echte Draw-Calls (Latte-Shader -> GLSL/SPIR-V, Texturen).
#include "hle.h"
#include "gx2.h"
#include "platform.h"
#include "threads.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

// GX2Surface (0x74 Bytes)
enum : uint32_t {
    SURF_DIM = 0x00, SURF_W = 0x04, SURF_H = 0x08, SURF_DEPTH = 0x0C, SURF_MIPS = 0x10,
    SURF_FORMAT = 0x14, SURF_AA = 0x18, SURF_USE = 0x1C, SURF_IMAGESIZE = 0x20, SURF_IMAGE = 0x24,
    SURF_MIPSIZE = 0x28, SURF_MIPMAPS = 0x2C, SURF_TILEMODE = 0x30, SURF_SWIZZLE = 0x34,
    SURF_ALIGN = 0x38, SURF_PITCH = 0x3C, SURF_MIPOFS = 0x40,
};

static uint64_t g_frames = 0;
static uint32_t g_draws = 0, g_draws_last = 0;
static uint32_t g_swap_count = 0;
static std::chrono::steady_clock::time_point g_last_vsync = std::chrono::steady_clock::now();

struct ScanTarget { int w = 0, h = 0; std::vector<uint32_t> px; bool dirty = false; };
static ScanTarget g_scan[2];

// --- Formate --------------------------------------------------------------------
static bool is_bc(uint32_t fmt) { uint32_t f = fmt & 0x3F; return f >= 0x31 && f <= 0x35; }

static uint32_t bytes_per_element(uint32_t fmt) {
    switch (fmt & 0x3F) {
    case 0x01: case 0x02: case 0x03: return 1;
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x0A: case 0x0B: case 0x0C: return 2;
    case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x13: case 0x16:
    case 0x19: case 0x1A: case 0x1B: return 4;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: return 8;
    case 0x22: case 0x23: return 16;
    case 0x31: case 0x34: return 8;     // BC1, BC4 (pro 4x4-Block)
    case 0x32: case 0x33: case 0x35: return 16;  // BC2, BC3, BC5
    default: return 4;
    }
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// GX2CalcSurfaceSizeAndAlignment(GX2Surface*)
static void h_GX2CalcSurfaceSizeAndAlignment(PPCContext& c) {
    uint32_t s = c.r[3];
    uint32_t w = std::max(1u, ld32(s + SURF_W)), h = std::max(1u, ld32(s + SURF_H));
    uint32_t depth = std::max(1u, ld32(s + SURF_DEPTH)), mips = std::max(1u, ld32(s + SURF_MIPS));
    uint32_t fmt = ld32(s + SURF_FORMAT), tile = ld32(s + SURF_TILEMODE);
    uint32_t bpe = bytes_per_element(fmt);
    bool bc = is_bc(fmt);
    bool linear = tile == 1 || tile == 16;   // LINEAR_ALIGNED / LINEAR_SPECIAL
    if (tile == 0) { tile = 4; st32(s + SURF_TILEMODE, tile); linear = false; }  // DEFAULT -> 2D tiled

    uint32_t pitch_align = linear ? std::max(64u / std::max(bpe, 1u), 8u) : 32u;
    uint32_t total = 0, base_size = 0, mip_total = 0, base_pitch = 0;
    for (uint32_t m = 0; m < mips && m < 14; m++) {
        uint32_t mw = std::max(1u, w >> m), mh = std::max(1u, h >> m);
        uint32_t ew = bc ? (mw + 3) / 4 : mw, eh = bc ? (mh + 3) / 4 : mh;
        uint32_t pitch = align_up(ew, pitch_align);
        uint32_t height = linear ? eh : align_up(eh, 8u);
        uint32_t size = align_up(pitch * height * bpe * depth, 0x800);
        if (m == 0) { base_size = size; base_pitch = pitch; }
        else {
            if (m - 1 < 13) st32(s + SURF_MIPOFS + (m - 1) * 4, mip_total);
            mip_total += size;
        }
        total += size;
    }
    (void)total;
    st32(s + SURF_IMAGESIZE, base_size);
    st32(s + SURF_MIPSIZE, mip_total);
    st32(s + SURF_ALIGN, 0x800);
    st32(s + SURF_PITCH, base_pitch);
}

// --- Bildschirm ---------------------------------------------------------------
static void tv_size(uint32_t mode, int& w, int& h) {
    switch (mode) {
    case 1: w = 640; h = 480; break;
    case 2: w = 854; h = 480; break;
    case 5: w = 1920; h = 1080; break;
    default: w = 1280; h = 720; break;
    }
}

// GX2CalcTVSize(mode, format, buffering, u32* size, BOOL* scaleNeeded)
static void h_GX2CalcTVSize(PPCContext& c) {
    int w, h;
    tv_size(c.r[3], w, h);
    uint32_t bufs = c.r[5] ? c.r[5] : 2;
    if (c.r[6]) st32(c.r[6], align_up((uint32_t)(w * h * 4), 0x1000) * bufs);
    if (c.r[7]) st32(c.r[7], 0);
}
// GX2CalcDRCSize(mode, format, buffering, u32* size, BOOL* scaleNeeded)
static void h_GX2CalcDRCSize(PPCContext& c) {
    uint32_t bufs = c.r[5] ? c.r[5] : 2;
    if (c.r[6]) st32(c.r[6], align_up(854 * 480 * 4, 0x1000) * bufs);
    if (c.r[7]) st32(c.r[7], 0);
}

static void fill_surface(uint32_t surf, float r, float g, float b, float a) {
    uint32_t img = ld32(surf + SURF_IMAGE), size = ld32(surf + SURF_IMAGESIZE);
    uint32_t fmt = ld32(surf + SURF_FORMAT);
    if (!img || !size || size > 0x4000000) return;
    auto u8 = [](float f) { return (uint8_t)(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
    uint32_t bpe = bytes_per_element(fmt);
    if (bpe != 4) { memset(g_base + img, 0, size); return; }
    uint8_t px[4] = {u8(r), u8(g), u8(b), u8(a)};
    uint8_t* p = g_base + img;
    for (uint32_t i = 0; i + 4 <= size; i += 4) memcpy(p + i, px, 4);
}

// GX2ClearColor(GX2ColorBuffer*, r, g, b, a)  -- Farben in f1..f4
static void h_GX2ClearColor(PPCContext& c) { fill_surface(c.r[3], (float)c.f[1], (float)c.f[2], (float)c.f[3], (float)c.f[4]); }

// GX2ClearBuffersEx(cb, db, r, g, b, a, depth, stencil, flags)
static void h_GX2ClearBuffersEx(PPCContext& c) {
    if (c.r[3]) fill_surface(c.r[3], (float)c.f[1], (float)c.f[2], (float)c.f[3], (float)c.f[4]);
}

// GX2CopyColorBufferToScanBuffer(GX2ColorBuffer*, target) -- target 1 = TV, 4 = DRC
static void h_GX2CopyColorBufferToScanBuffer(PPCContext& c) {
    uint32_t cb = c.r[3];
    int t = c.r[4] == 4 ? TARGET_DRC : TARGET_TV;
    uint32_t w = ld32(cb + SURF_W), h = ld32(cb + SURF_H);
    uint32_t img = ld32(cb + SURF_IMAGE), pitch = ld32(cb + SURF_PITCH);
    if (!w || !h || !img || w > 4096 || h > 4096) return;
    if (!pitch) pitch = w;
    ScanTarget& s = g_scan[t];
    s.w = (int)w;
    s.h = (int)h;
    s.px.resize((size_t)w * h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = g_base + img + (size_t)y * pitch * 4;
        uint32_t* out = s.px.data() + (size_t)y * w;
        memcpy(out, row, (size_t)w * 4);
        for (uint32_t x = 0; x < w; x++) out[x] |= 0xFF000000u;   // Alpha auf voll
    }
    s.dirty = true;
}

static void wait_vsync() {
    using namespace std::chrono;
    auto next = g_last_vsync + microseconds(16667);
    auto now = steady_clock::now();
    if (next > now) guest_sleep_ns(duration_cast<nanoseconds>(next - now).count());
    g_last_vsync = steady_clock::now();
}

// GX2SwapScanBuffers()
static void h_GX2SwapScanBuffers(PPCContext&) {
    for (int t = 0; t < 2; t++)
        if (g_scan[t].dirty) {
            platform_present(t, g_scan[t].w, g_scan[t].h, g_scan[t].px.data());
            g_scan[t].dirty = false;
        }
    g_frames++;
    g_swap_count++;
    g_draws_last = g_draws;
    g_draws = 0;
    platform_set_stats(g_frames, g_draws_last);
}

static void h_GX2WaitForVsync(PPCContext&) { wait_vsync(); }

// GX2GetSwapStatus(u32* swapCount, u32* flipCount, OSTime* lastFlip, OSTime* lastVsync)
static void h_GX2GetSwapStatus(PPCContext& c) {
    uint64_t now = ((uint64_t)ppc_mftb(269) << 32) | ppc_mftb(268);
    if (c.r[3]) st32(c.r[3], g_swap_count);
    if (c.r[4]) st32(c.r[4], g_swap_count);
    if (c.r[5]) st64(c.r[5], now);
    if (c.r[6]) st64(c.r[6], now);
}

static void h_draw(PPCContext&) { g_draws++; }
static void h_ret0(PPCContext& c) { c.r[3] = 0; }
static void h_ret1(PPCContext& c) { c.r[3] = 1; }

// GX2CopySurface(src, srcMip, srcSlice, dst, dstMip, dstSlice) -- grob: Basis-Ebene kopieren
static void h_GX2CopySurface(PPCContext& c) {
    uint32_t src = c.r[3], dst = c.r[6];
    uint32_t si = ld32(src + SURF_IMAGE), di = ld32(dst + SURF_IMAGE);
    uint32_t n = std::min(ld32(src + SURF_IMAGESIZE), ld32(dst + SURF_IMAGESIZE));
    if (si && di && n && n < 0x4000000) memmove(g_base + di, g_base + si, n);
}

// Hilfsgroessen
static void h_GX2CalcAuxInfo(PPCContext& c) {
    if (c.r[4]) st32(c.r[4], 0x2000);
    if (c.r[5]) st32(c.r[5], 0x800);
}
static void h_GX2CalcFetchShaderSizeEx(PPCContext& c) { c.r[3] = align_up(c.r[3] * 0x10 + 0x40, 0x20); }
// GX2InitFetchShaderEx(GX2FetchShader*, buffer, count, attribs, type, tess)
static void h_GX2InitFetchShaderEx(PPCContext& c) {
    uint32_t fs = c.r[3];
    st32(fs + 0x00, c.r[7]);
    st32(fs + 0x08, align_up(c.r[5] * 0x10 + 0x40, 0x20));
    st32(fs + 0x0C, c.r[4]);
    st32(fs + 0x10, c.r[5]);
}
static void h_ringsize(PPCContext& c) { c.r[3] = 0x40000; }
static void h_gprs(PPCContext& c) { c.r[3] = 64; }
static void h_stack(PPCContext& c) { c.r[3] = 32; }
static void h_GX2TempGetGPUVersion(PPCContext& c) { c.r[3] = 2; }
static void h_timeconv(PPCContext&) { /* r3:r4 bleiben gleich */ }
static void h_samplecycle(PPCContext& c) {
    if (c.r[3]) st64(c.r[3], ((uint64_t)ppc_mftb(269) << 32) | ppc_mftb(268));
}
// GX2GetCurrentDisplayList(void** list, u32* size) / GX2GetContextStateDisplayList(state, list*, size*)
static void h_GX2GetCurrentDisplayList(PPCContext& c) {
    if (c.r[3]) st32(c.r[3], 0);
    if (c.r[4]) st32(c.r[4], 0);
    c.r[3] = 0;
}
static void h_GX2GetContextStateDisplayList(PPCContext& c) {
    if (c.r[4]) st32(c.r[4], 0);
    if (c.r[5]) st32(c.r[5], 0);
}

void gx2_register(void (*reg)(const char*, const char*, void (*)(PPCContext&))) {
    const char* g = "gx2";
    reg(g, "GX2CalcSurfaceSizeAndAlignment", h_GX2CalcSurfaceSizeAndAlignment);
    reg(g, "GX2CalcTVSize", h_GX2CalcTVSize);
    reg(g, "GX2CalcDRCSize", h_GX2CalcDRCSize);
    reg(g, "GX2ClearColor", h_GX2ClearColor);
    reg(g, "GX2ClearBuffersEx", h_GX2ClearBuffersEx);
    reg(g, "GX2CopyColorBufferToScanBuffer", h_GX2CopyColorBufferToScanBuffer);
    reg(g, "GX2SwapScanBuffers", h_GX2SwapScanBuffers);
    reg(g, "GX2WaitForVsync", h_GX2WaitForVsync);
    reg(g, "GX2GetSwapStatus", h_GX2GetSwapStatus);
    reg(g, "GX2DrawEx", h_draw);
    reg(g, "GX2DrawIndexedEx", h_draw);
    reg(g, "GX2DrawDone", h_ret1);
    reg(g, "GX2GetLastFrame", h_ret0);
    reg(g, "GX2CopySurface", h_GX2CopySurface);
    reg(g, "GX2CalcColorBufferAuxInfo", h_GX2CalcAuxInfo);
    reg(g, "GX2CalcDepthBufferHiZInfo", h_GX2CalcAuxInfo);
    reg(g, "GX2CalcFetchShaderSizeEx", h_GX2CalcFetchShaderSizeEx);
    reg(g, "GX2InitFetchShaderEx", h_GX2InitFetchShaderEx);
    reg(g, "GX2CalcGeometryShaderInputRingBufferSize", h_ringsize);
    reg(g, "GX2CalcGeometryShaderOutputRingBufferSize", h_ringsize);
    for (const char* n : {"GX2GetVertexShaderGPRs", "GX2GetPixelShaderGPRs", "GX2GetGeometryShaderGPRs"})
        reg(g, n, h_gprs);
    for (const char* n : {"GX2GetVertexShaderStackEntries", "GX2GetPixelShaderStackEntries",
                          "GX2GetGeometryShaderStackEntries"})
        reg(g, n, h_stack);
    reg(g, "GX2TempGetGPUVersion", h_GX2TempGetGPUVersion);
    reg(g, "GX2CPUTimeToGPUTime", h_timeconv);
    reg(g, "GX2GPUTimeToCPUTime", h_timeconv);
    reg(g, "GX2SampleTopGPUCycle", h_samplecycle);
    reg(g, "GX2SampleBottomGPUCycle", h_samplecycle);
    reg(g, "GX2GetCurrentDisplayList", h_GX2GetCurrentDisplayList);
    reg(g, "GX2GetContextStateDisplayList", h_GX2GetContextStateDisplayList);
    reg(g, "GX2EndDisplayList", h_ret0);

    // Zustands-Funktionen: fuer die erste Stufe reicht "angenommen"
    static const char* state_fns[] = {
        "GX2Init", "GX2Shutdown", "GX2Flush", "GX2Invalidate", "GX2SetTVBuffer", "GX2SetDRCBuffer",
        "GX2SetTVScale", "GX2SetDRCScale", "GX2SetTVEnable", "GX2SetDRCEnable", "GX2SetTVGamma",
        "GX2SetDRCGamma", "GX2SetSwapInterval", "GX2SetupContextStateEx", "GX2SetContextState",
        "GX2SetDefaultState", "GX2InitColorBufferRegs", "GX2InitDepthBufferRegs",
        "GX2InitDepthBufferHiZEnable", "GX2InitTextureRegs", "GX2InitSampler",
        "GX2InitSamplerBorderType", "GX2InitSamplerClamping", "GX2InitSamplerDepthCompare",
        "GX2InitSamplerLOD", "GX2InitSamplerXYFilter", "GX2InitSamplerZMFilter",
        "GX2SetColorBuffer", "GX2SetDepthBuffer", "GX2SetViewport", "GX2SetScissor",
        "GX2SetFetchShader", "GX2SetVertexShader", "GX2SetPixelShader", "GX2SetGeometryShader",
        "GX2SetShaderModeEx", "GX2SetAttribBuffer", "GX2SetVertexTexture", "GX2SetPixelTexture",
        "GX2SetGeometryTexture", "GX2SetVertexSampler", "GX2SetPixelSampler", "GX2SetGeometrySampler",
        "GX2SetVertexUniformBlock", "GX2SetPixelUniformBlock", "GX2SetGeometryUniformBlock",
        "GX2SetVertexUniformReg", "GX2SetPixelUniformReg", "GX2SetDepthStencilControl",
        "GX2SetDepthStencilControlReg", "GX2SetStencilMask", "GX2SetPolygonControl",
        "GX2SetPolygonControlReg", "GX2SetPolygonOffsetReg", "GX2SetColorControl",
        "GX2SetColorControlReg", "GX2SetBlendControl", "GX2SetBlendControlReg",
        "GX2SetBlendConstantColor", "GX2SetBlendConstantColorReg", "GX2SetAlphaTest",
        "GX2SetAlphaTestReg", "GX2SetAlphaToMask", "GX2SetTargetChannelMasks", "GX2SetCullOnlyControl",
        "GX2SetDepthOnlyControl", "GX2SetLineWidth", "GX2SetPrimitiveRestartIndex",
        "GX2SetRasterizerClipControl", "GX2SetSurfaceSwizzle", "GX2SetClearDepthStencil",
        "GX2SetVertexSamplerBorderColor", "GX2SetPixelSamplerBorderColor",
        "GX2SetGeometrySamplerBorderColor", "GX2SetGeometryShaderInputRingBuffer",
        "GX2SetGeometryShaderOutputRingBuffer", "GX2ClearDepthStencilEx", "GX2ExpandAAColorBuffer",
        "GX2ExpandDepthBuffer", "GX2BeginDisplayListEx", "GX2CallDisplayList",
        "GX2DirectCallDisplayList", "GX2QueryBeginConditionalRender", "GX2QueryEndConditionalRender",
    };
    static auto nop = [](PPCContext&) {};
    for (const char* n : state_fns) reg(g, n, nop);
}

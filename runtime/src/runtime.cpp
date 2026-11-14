// Nintendo Land Recomp - Kern der Runtime: Speicher, Laden, Dispatch, CPU-Helfer.
#include "ppc_runtime.h"
#include "hle.h"
#include "threads.h"
#include "platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t* g_base = nullptr;

#if defined(NL_TRACE_FUNCS)
thread_local uint32_t g_trace_ring[1024];
thread_local uint32_t g_trace_pos = 0;
void nl_dump_trace(const char* why) {
    printf("--- Letzte Funktionen (%s) ---\n", why);
    for (uint32_t i = 64; i > 0; i--) printf(" %08X", g_trace_ring[(g_trace_pos - i) & 1023]);
    printf("\n");
}
#else
void nl_dump_trace(const char*) {}
#endif

// --- Speicher ---------------------------------------------------------------
static void map_address_space() {
    const uint64_t size = 0x100000000ull;
#if defined(_WIN32)
    g_base = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_base) { fprintf(stderr, "Konnte 4 GB Adressraum nicht reservieren\n"); exit(1); }
    // Bereiche, die die Wii-U-App benutzt, gleich committen (physisch erst bei Zugriff)
    VirtualAlloc(g_base + 0x01000000, 0x03000000, MEM_COMMIT, PAGE_READWRITE);  // Code
    VirtualAlloc(g_base + 0x10000000, 0x40000000, MEM_COMMIT, PAGE_READWRITE);  // MEM2
    VirtualAlloc(g_base + 0xC0000000, 0x01000000, MEM_COMMIT, PAGE_READWRITE);  // Imports
    VirtualAlloc(g_base + 0xF4000000, 0x02000000, MEM_COMMIT, PAGE_READWRITE);  // MEM1/LC
#else
    g_base = (uint8_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_base == MAP_FAILED) { perror("mmap"); exit(1); }
#endif
}

static bool load_image(const char* path, uint32_t& entry) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[4];
    uint32_t count = 0;
    fread(magic, 1, 4, f);
    fread(&entry, 4, 1, f);
    fread(&count, 4, 1, f);
    if (memcmp(magic, "NLIM", 4) != 0) { fclose(f); return false; }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr, size, fsize;
        fread(&addr, 4, 1, f); fread(&size, 4, 1, f); fread(&fsize, 4, 1, f);
        memset(g_base + addr, 0, size);
        if (fsize) fread(g_base + addr, 1, fsize, f);
    }
    fclose(f);
    return true;
}

// --- Dispatch ---------------------------------------------------------------
static PPCFunc find_func(uint32_t addr) {
    const FuncEntry* end = g_func_table + g_func_count;
    const FuncEntry* it = std::lower_bound(g_func_table, end, addr,
        [](const FuncEntry& e, uint32_t a) { return e.addr < a; });
    return (it != end && it->addr == addr) ? it->fn : nullptr;
}

void call_indirect(PPCContext& c, uint32_t addr) {
    if (hle_is_fake_addr(addr)) { hle_call_fake(c, addr); return; }
    if (addr >= 0xC0000000u && addr < 0xC1000000u) { hle_call(c, addr); return; }
    if (PPCFunc fn = find_func(addr)) { fn(c); return; }
    fprintf(stderr, "[CPU] Unbekanntes Sprungziel 0x%08X (LR=0x%08X)\n", addr, c.lr);
    nl_dump_trace("Absturz");
    exit(2);
}

// --- CPU-Helfer -------------------------------------------------------------
void ppc_unimpl(PPCContext& c, uint32_t addr, uint32_t raw) {
    fprintf(stderr, "[CPU] Nicht implementierte Instruktion 0x%08X bei 0x%08X\n", raw, addr);
    exit(3);
}

void ppc_trap(PPCContext& c, uint32_t addr) {
    fprintf(stderr, "[CPU] Trap bei 0x%08X (LR=0x%08X)\n", addr, c.lr);
    exit(4);
}

void ppc_syscall(PPCContext& c) {
    fprintf(stderr, "[CPU] Syscall r0=0x%08X\n", c.r[0]);
}

uint32_t ppc_mfspr(PPCContext& c, int spr) {
    switch (spr) {
    case 287: return 0x70010201;  // PVR Espresso
    case 1007: return 1;          // PIR / Core-ID
    default: return 0;
    }
}

void ppc_mtspr(PPCContext& c, int spr, uint32_t v) { (void)c; (void)spr; (void)v; }

uint32_t ppc_mftb(int spr) {
    using namespace std::chrono;
    // Timebase der Wii U: 248.625 MHz / 4
    uint64_t ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    uint64_t tb = (uint64_t)((double)ns * 0.06215625);
    return spr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

void ppc_dcbz(uint32_t ea) { memset(g_base + (ea & ~31u), 0, 32); }

static double dequant(uint32_t ea, int type, int scale, int& size) {
    double s = std::ldexp(1.0, -scale);
    switch (type) {
    case 4: size = 1; return ld8(ea) * s;
    case 5: size = 2; return ld16(ea) * s;
    case 6: size = 1; return (int8_t)ld8(ea) * s;
    case 7: size = 2; return (int16_t)ld16(ea) * s;
    default: size = 4; return ldf32(ea);
    }
}

void psq_load(PPCContext& c, uint32_t ea, int fd, int w, int gqr) {
    uint32_t g = c.gqr[gqr];
    int type = (g >> 16) & 7;
    int scale = (int)((g >> 24) & 0x3F);
    if (scale & 0x20) scale -= 0x40;
    int size = 4;
    c.f[fd] = dequant(ea, type, scale, size);
    c.ps1[fd] = w ? 1.0 : dequant(ea + size, type, scale, size);
}

static void quant(uint32_t ea, double v, int type, int scale, int& size) {
    double x = v * std::ldexp(1.0, scale);
    auto clamp = [](double d, double lo, double hi) { return d < lo ? lo : d > hi ? hi : d; };
    switch (type) {
    case 4: size = 1; st8(ea, (uint32_t)clamp(x, 0, 255)); break;
    case 5: size = 2; st16(ea, (uint32_t)clamp(x, 0, 65535)); break;
    case 6: size = 1; st8(ea, (uint32_t)(int32_t)clamp(x, -128, 127)); break;
    case 7: size = 2; st16(ea, (uint32_t)(int32_t)clamp(x, -32768, 32767)); break;
    default: size = 4; stf32(ea, (float)v); break;
    }
}

void psq_store(PPCContext& c, uint32_t ea, int fs, int w, int gqr) {
    uint32_t g = c.gqr[gqr];
    int type = g & 7;
    int scale = (int)((g >> 8) & 0x3F);
    if (scale & 0x20) scale -= 0x40;
    int size = 4;
    quant(ea, c.f[fs], type, scale, size);
    if (!w) quant(ea + size, c.ps1[fs], type, scale, size);
}

// --- Start ------------------------------------------------------------------
int main(int argc, char** argv) {
    // Ungepuffert, damit der Launcher die Ausgabe sofort sieht
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const char* image = argc > 1 ? argv[1] : "image.bin";
    map_address_space();
    uint32_t entry = 0;
    if (!load_image(image, entry)) {
        fprintf(stderr, "Konnte Speicherabbild '%s' nicht laden\n", image);
        return 1;
    }
    printf("Nintendo Land Recomp: %zu Funktionen, %zu Imports, Einstieg 0x%08X\n",
           g_func_count, g_import_count, entry);

    static PPCContext c;
    memset(&c, 0, sizeof(c));
    const uint32_t stack_top = 0x4F000000u;   // Hauptstack: 0x4E000000 - 0x4F000000
    c.r[1] = stack_top - 0x100;
    c.r[2] = g_sda2_base;
    c.r[13] = g_sda_base;
    hle_init(c);

    // argc/argv wie unter Cafe OS
    uint32_t argv_ptr = hle_alloc(16, 4);
    uint32_t name = hle_alloc(16, 4);
    memcpy(g_base + name, "Lunch.rpx", 10);
    st32(argv_ptr, name);
    st32(argv_ptr + 4, 0);
    c.r[3] = 1;
    c.r[4] = argv_ptr;

    // Das Spiel laeuft in einem eigenen Thread, das Fenster im Hauptthread
    bool window = getenv("NL_HEADLESS") == nullptr && platform_init();
    if (!window) printf("Kein Fenster (laeuft ohne Bild)\n");
    return platform_run([entry]() {
        threads_main_enter();
        call_indirect(c, entry);
        printf("Einstiegsfunktion ist zurueckgekehrt, r3=%d\n", (int32_t)c.r[3]);
        hle_report();
        return (int)c.r[3];
    });
}

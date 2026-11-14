// Nintendo Land Recomp - Runtime-Header fuer den generierten Code.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <climits>

#if defined(_MSC_VER)
#include <stdlib.h>
#include <intrin.h>
#define NL_INLINE __forceinline
static NL_INLINE uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v); }
static NL_INLINE uint16_t bswap16(uint16_t v) { return _byteswap_ushort(v); }
static NL_INLINE uint64_t bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
#define NL_INLINE inline __attribute__((always_inline))
static NL_INLINE uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static NL_INLINE uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static NL_INLINE uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

// Der komplette 32-bit Adressraum der Wii U liegt ab g_base im Host-Speicher.
extern uint8_t* g_base;

static NL_INLINE uint32_t ld8(uint32_t a) { return g_base[a]; }
static NL_INLINE uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return bswap16(v); }
static NL_INLINE uint32_t lds16(uint32_t a) { return (uint32_t)(int32_t)(int16_t)ld16(a); }
static NL_INLINE uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return bswap32(v); }
static NL_INLINE uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return bswap64(v); }
static NL_INLINE void st8(uint32_t a, uint32_t v) { g_base[a] = (uint8_t)v; }
static NL_INLINE void st16(uint32_t a, uint32_t v) { uint16_t x = bswap16((uint16_t)v); memcpy(g_base + a, &x, 2); }
#if defined(NL_TRACE_FUNCS)
extern uint32_t g_watch_addr;
void nl_watch_hit(uint32_t a, uint32_t v);
static NL_INLINE void st32(uint32_t a, uint32_t v) {
    if (a == g_watch_addr) nl_watch_hit(a, v);
    uint32_t x = bswap32(v); memcpy(g_base + a, &x, 4);
}
#else
static NL_INLINE void st32(uint32_t a, uint32_t v) { uint32_t x = bswap32(v); memcpy(g_base + a, &x, 4); }
#endif
static NL_INLINE void st64(uint32_t a, uint64_t v) { uint64_t x = bswap64(v); memcpy(g_base + a, &x, 8); }

static NL_INLINE uint64_t fbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static NL_INLINE double bitsf(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static NL_INLINE float ldf32(uint32_t a) { uint32_t u = ld32(a); float f; memcpy(&f, &u, 4); return f; }
static NL_INLINE double ldf64(uint32_t a) { return bitsf(ld64(a)); }
static NL_INLINE void stf32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); st32(a, u); }
static NL_INLINE void stf64(uint32_t a, double d) { st64(a, fbits(d)); }

static NL_INLINE uint32_t rotl32(uint32_t v, uint32_t s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }

static NL_INLINE uint32_t ppc_cntlzw(uint32_t v) {
    if (!v) return 32;
#if defined(_MSC_VER)
    unsigned long i; _BitScanReverse(&i, v); return 31 - i;
#else
    return (uint32_t)__builtin_clz(v);
#endif
}

static NL_INLINE uint32_t ppc_divw(uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0 || (x == INT32_MIN && y == -1)) return (x < 0) ? 0xFFFFFFFFu : 0;
    return (uint32_t)(x / y);
}

static NL_INLINE int32_t ppc_fctiwz(double d) {
    if (d != d) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}
static NL_INLINE int32_t ppc_fctiw(double d) {
    if (d != d) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)std::nearbyint(d);
}

// CPU-Zustand im selben Format wie Cemus PPCInterpreter_t (gleiche Feldnamen),
// damit derselbe generierte Code in der eigenen Runtime und in Cemu laeuft.
union FPR_t {
    double fpr;
    struct { double fp0; double fp1; };
    struct { uint64_t fp0int; uint64_t fp1int; };
};

struct PPCContext {
    uint32_t instructionPointer;
    uint32_t gpr[32];
    FPR_t fpr[32];
    uint32_t fpscr;
    uint8_t cr[32];
    uint8_t xer_ca, xer_so, xer_ov;
    int32_t remainingCycles;
    struct { uint32_t LR, CTR, XER, UPIR, UGQR[8]; } spr;
    uint32_t reservedMemAddr, reservedMemValue;
    uint32_t msr;
};

#define NL_LOOP(c) ((void)0)

static NL_INLINE void ppc_setcr_flags(PPCContext& c, int f, uint32_t nib) {
    c.cr[f * 4 + 0] = (nib >> 3) & 1; c.cr[f * 4 + 1] = (nib >> 2) & 1;
    c.cr[f * 4 + 2] = (nib >> 1) & 1; c.cr[f * 4 + 3] = nib & 1;
}
static NL_INLINE void ppc_setcr(PPCContext& c, int f, int32_t a, int32_t b) {
    c.cr[f * 4 + 0] = a < b; c.cr[f * 4 + 1] = a > b; c.cr[f * 4 + 2] = a == b; c.cr[f * 4 + 3] = c.xer_so;
}
static NL_INLINE void ppc_setcr_u(PPCContext& c, int f, uint32_t a, uint32_t b) {
    c.cr[f * 4 + 0] = a < b; c.cr[f * 4 + 1] = a > b; c.cr[f * 4 + 2] = a == b; c.cr[f * 4 + 3] = c.xer_so;
}
static NL_INLINE void ppc_setcr0(PPCContext& c, int32_t v) { ppc_setcr(c, 0, v, 0); }
static NL_INLINE void ppc_setcr_f(PPCContext& c, int f, double a, double b) {
    bool un = (a != a) || (b != b);
    c.cr[f * 4 + 0] = !un && a < b; c.cr[f * 4 + 1] = !un && a > b;
    c.cr[f * 4 + 2] = !un && a == b; c.cr[f * 4 + 3] = un;
}
static NL_INLINE void ppc_setcrbit(PPCContext& c, int bit, uint32_t v) { c.cr[bit] = v & 1; }
static NL_INLINE uint32_t ppc_get_cr(PPCContext& c) {
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) v |= (uint32_t)(c.cr[i] & 1) << (31 - i);
    return v;
}
static NL_INLINE void ppc_set_cr(PPCContext& c, uint32_t v) {
    for (int i = 0; i < 32; i++) c.cr[i] = (v >> (31 - i)) & 1;
}
static NL_INLINE void ppc_mcrf(PPCContext& c, int d, int s) {
    for (int k = 0; k < 4; k++) c.cr[d * 4 + k] = c.cr[s * 4 + k];
}
static NL_INLINE uint32_t ppc_get_xer(PPCContext& c) {
    return ((uint32_t)c.xer_so << 31) | ((uint32_t)c.xer_ov << 30) | ((uint32_t)c.xer_ca << 29);
}
static NL_INLINE void ppc_set_xer(PPCContext& c, uint32_t v) {
    c.xer_so = (v >> 31) & 1; c.xer_ov = (v >> 30) & 1; c.xer_ca = (v >> 29) & 1;
}
static NL_INLINE uint32_t ppc_get_msr(PPCContext& c) { return c.msr; }
static NL_INLINE void ppc_set_msr(PPCContext& c, uint32_t v) { c.msr = v; }
static NL_INLINE uint32_t ppc_lwarx(PPCContext& c, uint32_t ea) {
    c.reservedMemAddr = ea;
    c.reservedMemValue = ld32(ea);
    return c.reservedMemValue;
}
static NL_INLINE void ppc_stwcx(PPCContext& c, uint32_t ea, uint32_t v) {
    bool ok = c.reservedMemAddr == ea && ld32(ea) == c.reservedMemValue;
    if (ok) st32(ea, v);
    c.reservedMemAddr = 0;
    ppc_setcr_flags(c, 0, (ok ? 2u : 0u) | c.xer_so);
}

static NL_INLINE uint32_t ppc_sraw(PPCContext& c, uint32_t s, uint32_t b) {
    int32_t x = (int32_t)s;
    if (b & 0x20) { c.xer_ca = x < 0; return x < 0 ? 0xFFFFFFFFu : 0; }
    uint32_t n = b & 31;
    c.xer_ca = (x < 0) && n && (s & ((1u << n) - 1));
    return (uint32_t)(x >> n);
}

static inline void ppc_lswi(PPCContext& c, uint32_t ea, int rd, int nb) {
    int r = rd - 1, sh = 0;
    for (int k = 0; k < nb; k++, ea++) {
        if (sh == 0) { r = (r + 1) & 31; c.gpr[r] = 0; }
        c.gpr[r] |= ld8(ea) << (24 - sh);
        sh = (sh + 8) & 31;
    }
}
static inline void ppc_stswi(PPCContext& c, uint32_t ea, int rs, int nb) {
    int r = rs - 1, sh = 0;
    for (int k = 0; k < nb; k++, ea++) {
        if (sh == 0) r = (r + 1) & 31;
        st8(ea, c.gpr[r] >> (24 - sh));
        sh = (sh + 8) & 31;
    }
}

// Optionaler Funktions-Tracer (nur mit -DNL_TRACE_FUNCS): merkt sich die letzten Aufrufe
#if defined(NL_TRACE_FUNCS)
extern thread_local uint32_t g_trace_ring[1024];
extern thread_local uint32_t g_trace_pos;
#define NL_ENTER(a) (g_trace_ring[g_trace_pos++ & 1023] = (a))
#else
#define NL_ENTER(a) ((void)0)
#endif
void nl_dump_trace(const char* why);

typedef void (*PPCFunc)(PPCContext&);
struct FuncEntry { uint32_t addr; PPCFunc fn; };
struct ImportEntry { uint32_t addr; const char* lib; const char* name; bool is_func; };

extern const FuncEntry g_func_table[];
extern const size_t g_func_count;
extern const ImportEntry g_import_table[];
extern const size_t g_import_count;
extern const uint32_t g_entry, g_sda_base, g_sda2_base, g_stack_size;

// In der Runtime implementiert
void hle_call(PPCContext& c, uint32_t import_addr, uint32_t call_site = 0);
void call_indirect(PPCContext& c, uint32_t addr);
void ppc_unimpl(PPCContext& c, uint32_t addr, uint32_t raw);
void ppc_trap(PPCContext& c, uint32_t addr);
void ppc_syscall(PPCContext& c);
uint32_t ppc_mfspr(PPCContext& c, int spr);
void ppc_mtspr(PPCContext& c, int spr, uint32_t v);
uint32_t ppc_mftb(int spr);
void ppc_dcbz(uint32_t ea);
void psq_load(PPCContext& c, uint32_t ea, int fd, int w, int gqr);
void psq_store(PPCContext& c, uint32_t ea, int fs, int w, int gqr);

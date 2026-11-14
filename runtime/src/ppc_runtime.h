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

struct PPCContext {
    uint32_t r[32];
    double f[32];
    double ps1[32];
    uint32_t cr, lr, ctr;
    uint32_t ca, so, ov;
    uint32_t fpscr, msr;
    uint32_t gqr[8];
    uint32_t reserve;

    NL_INLINE void setcr_flags(int field, uint32_t nib) {
        int sh = 28 - 4 * field;
        cr = (cr & ~(0xFu << sh)) | ((nib & 0xF) << sh);
    }
    NL_INLINE void setcr(int field, int32_t a, int32_t b) {
        setcr_flags(field, (a < b ? 8 : a > b ? 4 : 2) | (so & 1));
    }
    NL_INLINE void setcr_u(int field, uint32_t a, uint32_t b) {
        setcr_flags(field, (a < b ? 8 : a > b ? 4 : 2) | (so & 1));
    }
    NL_INLINE void setcr0(int32_t v) { setcr(0, v, 0); }
    NL_INLINE void setcr_f(int field, double a, double b) {
        setcr_flags(field, (a != a || b != b) ? 1 : a < b ? 8 : a > b ? 4 : 2);
    }
    NL_INLINE void setcrbit(int bit, uint32_t v) {
        uint32_t m = 1u << (31 - bit);
        cr = v ? (cr | m) : (cr & ~m);
    }
    NL_INLINE uint32_t get_xer() const { return (so << 31) | (ov << 30) | (ca << 29); }
    NL_INLINE void set_xer(uint32_t v) { so = (v >> 31) & 1; ov = (v >> 30) & 1; ca = (v >> 29) & 1; }
};

static NL_INLINE uint32_t ppc_sraw(PPCContext& c, uint32_t s, uint32_t b) {
    int32_t x = (int32_t)s;
    if (b & 0x20) { c.ca = x < 0; return x < 0 ? 0xFFFFFFFFu : 0; }
    uint32_t n = b & 31;
    c.ca = (x < 0) && n && (s & ((1u << n) - 1));
    return (uint32_t)(x >> n);
}

static inline void ppc_lswi(PPCContext& c, uint32_t ea, int rd, int nb) {
    int r = rd - 1, sh = 0;
    for (int k = 0; k < nb; k++, ea++) {
        if (sh == 0) { r = (r + 1) & 31; c.r[r] = 0; }
        c.r[r] |= ld8(ea) << (24 - sh);
        sh = (sh + 8) & 31;
    }
}
static inline void ppc_stswi(PPCContext& c, uint32_t ea, int rs, int nb) {
    int r = rs - 1, sh = 0;
    for (int k = 0; k < nb; k++, ea++) {
        if (sh == 0) r = (r + 1) & 31;
        st8(ea, c.r[r] >> (24 - sh));
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
void hle_call(PPCContext& c, uint32_t import_addr);
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

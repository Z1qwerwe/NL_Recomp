// Nintendo Land Recomp - Verbindung zwischen Cemu und dem vorab kompilierten (AOT) Spielcode.
//
// Ablauf:
//  1. Cemu laedt Lunch.rpx ganz normal (Relocations, Imports, Betriebssystem).
//  2. NL_AOT_Install() schreibt an den Anfang jeder recompilierten Funktion einen
//     HLE-Befehl. Springt Cemu dorthin, ruft es aot_entry() auf.
//  3. aot_entry() fuehrt die native C++-Version der Funktion aus, direkt auf Cemus
//     CPU-Zustand (PPCInterpreter_t). Danach geht es bei LR weiter.
//  4. Ruft nativer Code etwas auf, das nicht nativ ist (Systemfunktionen, Trampoline),
//     laeuft das ueber Cemus PPCCore_executeCallbackInternal.
//
// Diese Datei wird im Cemu-Target (CemuCafe) mitgebaut und darf Cemus Header nutzen.
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cemu/Logging/CemuLogging.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <unordered_map>

// --- Gemeinsame Typen mit dem generierten Code (siehe cemu/aot/ppc_runtime.h) ----
struct PPCContext;   // ist im Speicher exakt ein PPCInterpreter_t
typedef void (*PPCFunc)(PPCContext&);
struct FuncEntry { uint32_t addr; PPCFunc fn; };
struct ImportEntry { uint32_t addr; const char* lib; const char* name; bool is_func; };

extern const FuncEntry g_func_table[];
extern const size_t g_func_count;
extern const uint32_t g_func_firstword[];
extern const uint32_t g_entry;

// Spiegel der Felder, die der generierte Code benutzt: muss mit PPCInterpreter_t uebereinstimmen
struct NLCtxMirror {
    uint32_t instructionPointer;
    uint32_t gpr[32];
    FPR_t fpr[32];
    uint32_t fpscr;
    uint8_t cr[32];
    uint8_t xer_ca, xer_so, xer_ov;
    int32_t remainingCycles;
    int32_t skippedCycles;
    struct { uint32_t LR, CTR, XER, UPIR, UGQR[8]; } spr;
    uint32_t reservedMemAddr, reservedMemValue;
};
#define NL_CHECK(f) static_assert(offsetof(NLCtxMirror, f) == offsetof(PPCInterpreter_t, f), "Layout von " #f " passt nicht zu Cemu")
NL_CHECK(instructionPointer); NL_CHECK(gpr); NL_CHECK(fpr); NL_CHECK(fpscr); NL_CHECK(cr);
NL_CHECK(xer_ca); NL_CHECK(xer_so); NL_CHECK(xer_ov); NL_CHECK(remainingCycles);
NL_CHECK(skippedCycles); NL_CHECK(spr); NL_CHECK(reservedMemAddr); NL_CHECK(reservedMemValue);

static inline PPCInterpreter_t* H(PPCContext& c) { return reinterpret_cast<PPCInterpreter_t*>(&c); }

extern uint32 ppcThreadQuantum;

// --- Funktionstabelle ---------------------------------------------------------
static bool s_installed = false;
static std::unordered_map<uint32_t, uint32_t> s_original_words;   // ueberschriebene Befehle

static PPCFunc find_func(uint32_t addr) {
    const FuncEntry* end = g_func_table + g_func_count;
    const FuncEntry* it = std::lower_bound(g_func_table, end, addr,
        [](const FuncEntry& e, uint32_t a) { return e.addr < a; });
    return (it != end && it->addr == addr) ? it->fn : nullptr;
}

static uint32_t read_be32(uint32_t addr) {
    auto it = s_original_words.find(addr);
    if (it != s_original_words.end()) return it->second;
    return _swapEndianU32(*(uint32*)(memory_base + addr));
}

// --- Vom generierten Code aufgerufen ---------------------------------------------
void call_indirect(PPCContext& c, uint32_t addr) {
    if (PPCFunc fn = find_func(addr)) { fn(c); return; }
    // Nicht nativ (Systemfunktion, Trampolin, ...): von Cemu ausfuehren lassen
    PPCCore_executeCallbackInternal(addr);
}

// Aufruf einer importierten Funktion. Das Ziel steht im (von Cemu relozierten) Sprungbefehl.
void hle_call(PPCContext& c, uint32_t /*import_addr*/, uint32_t call_site) {
    uint32_t ins = read_be32(call_site);
    uint32_t target;
    if ((ins >> 26) == 18) {
        int32_t off = ins & 0x03FFFFFC;
        if (off & 0x02000000) off -= 0x04000000;
        target = (ins & 2) ? (uint32_t)off : call_site + off;
    } else if ((ins >> 26) == 16) {
        int32_t off = ins & 0xFFFC;
        if (off & 0x8000) off -= 0x10000;
        target = (ins & 2) ? (uint32_t)off : call_site + off;
    } else {
        cemuLog_log(LogType::Force, "NL-AOT: Import-Aufruf bei 0x{:08x} ist kein Sprung (0x{:08x})", call_site, ins);
        return;
    }
    call_indirect(c, target);
}

void ppc_unimpl(PPCContext& c, uint32_t addr, uint32_t raw) {
    cemuLog_log(LogType::Force, "NL-AOT: nicht implementierte Instruktion 0x{:08x} bei 0x{:08x}", raw, addr);
    cemu_assert_debug(false);
}

void ppc_trap(PPCContext& c, uint32_t addr) {
    cemuLog_log(LogType::Force, "NL-AOT: Trap bei 0x{:08x} (LR 0x{:08x})", addr, H(c)->spr.LR);
}

void ppc_syscall(PPCContext& c) {
    cemuLog_log(LogType::Force, "NL-AOT: Syscall r0=0x{:08x}", H(c)->gpr[0]);
}

uint32_t ppc_mfspr(PPCContext& c, int spr) {
    switch (spr) {
    case 287: return 0x70010201;               // PVR Espresso
    case 1007: return H(c)->spr.UPIR;          // Kern-ID
    default:
        cemuLog_logDebug(LogType::Force, "NL-AOT: mfspr {}", spr);
        return 0;
    }
}

void ppc_mtspr(PPCContext& c, int spr, uint32_t v) {
    cemuLog_logDebug(LogType::Force, "NL-AOT: mtspr {} = 0x{:08x}", spr, v);
}

uint32_t ppc_mftb(int spr) {
    uint64 tb = coreinit::OSGetTime();
    return spr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

uint32_t ppc_get_msr(PPCContext& c) { return H(c)->sprExtended.msr; }
void ppc_set_msr(PPCContext& c, uint32_t v) { H(c)->sprExtended.msr = v; }

void ppc_dcbz(uint32_t ea) { memset(memory_base + (ea & ~31u), 0, 32); }

bool nl_atomic_cas32(uint32_t ea, uint32_t expected_be, uint32_t desired_be) {
    std::atomic_ref<uint32_t> ref(*reinterpret_cast<uint32_t*>(memory_base + ea));
    return ref.compare_exchange_strong(expected_be, desired_be);
}

void nl_aot_yield(PPCContext& c) {
    H(c)->remainingCycles = ppcThreadQuantum;
    coreinit::OSYieldThread();
}

void nl_dump_trace(const char*) {}

// --- Paired-Single Laden/Speichern mit GQR-Quantisierung --------------------------
static inline uint8_t ld8(uint32_t a) { return memory_base[a]; }
static inline uint16_t ld16(uint32_t a) { return _swapEndianU16(*(uint16*)(memory_base + a)); }
static inline float ldf32(uint32_t a) { uint32_t u = _swapEndianU32(*(uint32*)(memory_base + a)); float f; memcpy(&f, &u, 4); return f; }
static inline void st8(uint32_t a, uint32_t v) { memory_base[a] = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { *(uint16*)(memory_base + a) = _swapEndianU16((uint16)v); }
static inline void stf32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); *(uint32*)(memory_base + a) = _swapEndianU32(u); }

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
    PPCInterpreter_t* h = H(c);
    uint32_t g = h->spr.UGQR[gqr];
    int type = (g >> 16) & 7;
    int scale = (int)((g >> 24) & 0x3F);
    if (scale & 0x20) scale -= 0x40;
    int size = 4;
    h->fpr[fd].fp0 = dequant(ea, type, scale, size);
    h->fpr[fd].fp1 = w ? 1.0 : dequant(ea + size, type, scale, size);
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
    PPCInterpreter_t* h = H(c);
    uint32_t g = h->spr.UGQR[gqr];
    int type = g & 7;
    int scale = (int)((g >> 8) & 0x3F);
    if (scale & 0x20) scale -= 0x40;
    int size = 4;
    quant(ea, h->fpr[fs].fp0, type, scale, size);
    if (!w) quant(ea + size, h->fpr[fs].fp1, type, scale, size);
}

// --- Einstieg aus Cemu --------------------------------------------------------------
static void aot_entry(PPCInterpreter_t* hCPU) {
    uint32_t ip = hCPU->instructionPointer;
    PPCFunc fn = find_func(ip);
    if (!fn) {
        cemuLog_log(LogType::Force, "NL-AOT: keine native Funktion fuer 0x{:08x}", ip);
        cemu_assert(false);
        return;
    }
    fn(*reinterpret_cast<PPCContext*>(hCPU));
    hCPU->instructionPointer = hCPU->spr.LR;
}

// Wird nach dem Laden und Linken der RPX aufgerufen (CafeSystem.cpp)
void NL_AOT_Install() {
    if (s_installed) return;
    const char* off = getenv("NL_AOT_OFF");
    if (off && *off == '1') {
        cemuLog_log(LogType::Force, "NL-AOT: abgeschaltet (NL_AOT_OFF=1), Cemu emuliert die CPU");
        return;
    }
    // Pruefen, ob Cemu den Code an dieselben Adressen geladen hat wie der Recompiler
    size_t mismatch = 0;
    for (size_t i = 0; i < g_func_count; i++) {
        uint32_t w = _swapEndianU32(*(uint32*)(memory_base + g_func_table[i].addr));
        if (w != g_func_firstword[i] && ++mismatch <= 5)
            cemuLog_log(LogType::Force, "NL-AOT: Abweichung bei 0x{:08x}: erwartet 0x{:08x}, gefunden 0x{:08x}",
                        g_func_table[i].addr, g_func_firstword[i], w);
    }
    if (mismatch > g_func_count / 100) {
        cemuLog_log(LogType::Force, "NL-AOT: {} von {} Funktionen passen nicht - falsche Spielversion oder anderer Lade-Offset. AOT deaktiviert.",
                    mismatch, g_func_count);
        return;
    }
    HLEIDX idx = PPCInterpreter_registerHLECall(aot_entry, "nl_aot_entry");
    uint32_t opcode = (1u << 26) | (uint32_t)idx;
    for (size_t i = 0; i < g_func_count; i++) {
        uint32_t a = g_func_table[i].addr;
        uint32* p = (uint32*)(memory_base + a);
        s_original_words[a] = _swapEndianU32(*p);
        *p = _swapEndianU32(opcode);
    }
    s_installed = true;
    cemuLog_log(LogType::Force, "NL-AOT: {} native Funktionen aktiv ({} Abweichungen)", g_func_count, mismatch);
}

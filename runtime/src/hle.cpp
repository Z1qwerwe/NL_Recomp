// High-Level-Emulation: Jede importierte Systemfunktion bekommt hier eine
// C++-Implementierung. Was fehlt, wird einmal geloggt und gibt 0 zurueck.
#include "hle.h"
#include "threads.h"
#include "fs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using HleFn = void (*)(PPCContext&);

static std::unordered_map<std::string, HleFn> g_impl;          // "lib::name" -> Funktion
static std::unordered_map<uint32_t, int> g_import_slot;         // Import-Adresse -> Slot
static std::vector<std::pair<std::string, HleFn>> g_slots;      // Slot -> (Name, Funktion)
static std::map<std::string, uint64_t> g_calls;
static std::set<std::string> g_missing;
static bool g_trace = getenv("NL_TRACE_HLE") != nullptr;

static const uint32_t FAKE_BASE = 0xFF000000u;

// --- Gast-Speicher ----------------------------------------------------------
static uint32_t g_heap_ptr = 0x20000000u;
static const uint32_t g_heap_end = 0x4E000000u;

uint32_t hle_alloc(uint32_t size, uint32_t align) {
    if (align < 4) align = 4;
    uint32_t p = (g_heap_ptr + align - 1) & ~(align - 1);
    if (p + size > g_heap_end) {
        fprintf(stderr, "[HLE] Gast-Heap voll (%u Bytes angefragt)\n", size);
        return 0;
    }
    g_heap_ptr = p + size;
    memset(g_base + p, 0, size);
    return p;
}

// --- Hilfen fuer Argumente -------------------------------------------------
static std::string gstr(uint32_t a) {
    std::string s;
    if (!a) return "(null)";
    while (g_base[a] && s.size() < 4096) s.push_back((char)g_base[a++]);
    return s;
}

static void ret(PPCContext& c, uint32_t v) { c.r[3] = v; }
static void ret64(PPCContext& c, uint64_t v) { c.r[3] = (uint32_t)(v >> 32); c.r[4] = (uint32_t)v; }

// printf im Gast-Format: Argumente kommen aus r4..r10 und f1..f8
static std::string guest_format(PPCContext& c, uint32_t fmt_addr, int first_gpr) {
    std::string fmt = gstr(fmt_addr), out;
    int gpr = first_gpr, fpr = 1;
    char buf[512];
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0123456789.lhz", fmt[j])) j++;
        if (j >= fmt.size()) break;
        std::string spec = fmt.substr(i, j - i + 1);
        char t = fmt[j];
        uint32_t arg = gpr <= 10 ? c.r[gpr] : 0;
        if (t == '%') out.push_back('%');
        else if (t == 's') { snprintf(buf, sizeof buf, spec.c_str(), gstr(arg).c_str()); out += buf; gpr++; }
        else if (strchr("fFeEgG", t)) { snprintf(buf, sizeof buf, spec.c_str(), c.f[fpr++]); out += buf; }
        else {
            std::string s2;
            for (char ch : spec) if (ch != 'l' && ch != 'h' && ch != 'z') s2.push_back(ch);
            snprintf(buf, sizeof buf, s2.c_str(), arg); out += buf; gpr++;
        }
        i = j;
    }
    return out;
}

// --- coreinit ----------------------------------------------------------------
static void h_memcpy(PPCContext& c) { memmove(g_base + c.r[3], g_base + c.r[4], c.r[5]); }
static void h_memset(PPCContext& c) { memset(g_base + c.r[3], (int)c.r[4], c.r[5]); }
static void h_OSReport(PPCContext& c) { printf("[Spiel] %s", guest_format(c, c.r[3], 4).c_str()); fflush(stdout); }
static void h_OSConsoleWrite(PPCContext& c) { fwrite(g_base + c.r[3], 1, c.r[4], stdout); }
static void h_OSPanic(PPCContext& c) {
    fprintf(stderr, "[Spiel] PANIC %s:%u: %s\n", gstr(c.r[3]).c_str(), c.r[4],
            guest_format(c, c.r[5], 6).c_str());
    hle_report();
    exit(5);
}
static void h_exit(PPCContext& c) {
    printf("[Spiel] exit(%d)\n", (int32_t)c.r[3]);
    hle_report();
    exit((int)c.r[3]);
}

static uint64_t tb_now() { return ((uint64_t)ppc_mftb(269) << 32) | ppc_mftb(268); }
static void h_OSGetTime(PPCContext& c) { ret64(c, tb_now()); }
static void h_one(PPCContext& c) { ret(c, 1); }
static void h_zero(PPCContext& c) { ret(c, 0); }
static void h_nop(PPCContext&) {}


// Heaps: fuer den Anfang ein einfacher Bump-Allokator
static void h_MEMCreateHeap(PPCContext& c) { ret(c, c.r[3] ? c.r[3] : hle_alloc(0x40, 4)); }
static void h_MEMAllocFromHeapEx(PPCContext& c) {
    int32_t align = (int32_t)c.r[5];
    ret(c, hle_alloc(c.r[4], (uint32_t)(align < 0 ? -align : align)));
}
static void h_MEMGetAllocatableSize(PPCContext& c) { ret(c, g_heap_end - g_heap_ptr - 0x1000); }
static void h_MEMGetBaseHeapHandle(PPCContext& c) { ret(c, 0x7F000000u + c.r[3]); }
static void h_DefaultAlloc(PPCContext& c) { ret(c, hle_alloc(c.r[3], 32)); }
static void h_DefaultAllocEx(PPCContext& c) {
    int32_t align = (int32_t)c.r[4];
    ret(c, hle_alloc(c.r[3], (uint32_t)(align < 0 ? -align : align)));
}

// Dynamisches Laden: OSDynLoad_FindExport liefert eine Fake-Adresse
static int slot_for(const std::string& key);
static void h_OSDynLoad_Acquire(PPCContext& c) {
    std::string lib = gstr(c.r[3]);
    if (c.r[4]) st32(c.r[4], 0x7E000000u + (uint32_t)std::hash<std::string>{}(lib) % 0x1000);
    if (g_trace) printf("[HLE] OSDynLoad_Acquire(%s)\n", lib.c_str());
    ret(c, 0);
}
static void h_OSDynLoad_FindExport(PPCContext& c) {
    std::string name = gstr(c.r[5]);
    int s = slot_for("dyn::" + name);
    if (c.r[6]) st32(c.r[6], FAKE_BASE + s * 4);
    if (g_trace) printf("[HLE] OSDynLoad_FindExport(%s)\n", name.c_str());
    ret(c, 0);
}

// --- Registrierung ------------------------------------------------------------
static void reg(const char* lib, const char* name, HleFn fn) { g_impl[std::string(lib) + "::" + name] = fn; }

static void register_all() {
    const char* ci = "coreinit";
    reg(ci, "memcpy", h_memcpy); reg(ci, "memmove", h_memcpy); reg(ci, "OSBlockMove", h_memcpy);
    reg(ci, "memset", h_memset); reg(ci, "OSBlockSet", h_memset);
    reg(ci, "OSReport", h_OSReport); reg(ci, "OSConsoleWrite", h_OSConsoleWrite);
    reg(ci, "OSPanic", h_OSPanic); reg(ci, "exit", h_exit); reg(ci, "_Exit", h_exit);
    reg(ci, "OSGetTime", h_OSGetTime); reg(ci, "OSGetSystemTime", h_OSGetTime);
    reg(ci, "OSGetCoreId", h_one); reg(ci, "OSGetMainCoreId", h_one);
    reg(ci, "OSDisableInterrupts", h_one); reg(ci, "OSRestoreInterrupts", h_nop);
    reg(ci, "OSIsDebuggerPresent", h_zero); reg(ci, "OSIsDebuggerInitialized", h_zero);
    for (const char* n : {"DCFlushRange", "DCInvalidateRange", "DCStoreRange", "OSMemoryBarrier",
                          "OSInitThreadQueue", "OSSetThreadName", "OSEnforceInorderIO",
                          "OSEnableHomeButtonMenu", "OSSetScreenCapturePermission",
                          "__ghsLock", "__ghsUnlock", "__gh_set_errno", "__ghs_flock_file",
                          "__ghs_funlock_file", "OSBlockThreadsOnExit", "OSDynLoad_SetAllocator"})
        reg(ci, n, h_nop);
    reg(ci, "DCZeroRange", h_memset);  // r3=addr, r4=size -> unten korrigiert
    g_impl["coreinit::DCZeroRange"] = [](PPCContext& c) { memset(g_base + c.r[3], 0, c.r[4]); };
    reg(ci, "MEMCreateExpHeapEx", h_MEMCreateHeap); reg(ci, "MEMCreateFrmHeapEx", h_MEMCreateHeap);
    reg(ci, "MEMAllocFromExpHeapEx", h_MEMAllocFromHeapEx);
    reg(ci, "MEMAllocFromFrmHeapEx", h_MEMAllocFromHeapEx);
    reg(ci, "MEMFreeToExpHeap", h_nop); reg(ci, "MEMDestroyExpHeap", h_nop);
    reg(ci, "MEMDestroyFrmHeap", h_nop);
    reg(ci, "MEMGetAllocatableSizeForExpHeapEx", h_MEMGetAllocatableSize);
    reg(ci, "MEMGetAllocatableSizeForFrmHeapEx", h_MEMGetAllocatableSize);
    reg(ci, "MEMGetTotalFreeSizeForExpHeap", h_MEMGetAllocatableSize);
    reg(ci, "MEMGetBaseHeapHandle", h_MEMGetBaseHeapHandle);
    reg(ci, "MEMAllocFromDefaultHeap", h_DefaultAlloc);
    reg(ci, "MEMAllocFromDefaultHeapEx", h_DefaultAllocEx);
    reg(ci, "MEMFreeToDefaultHeap", h_nop);
    reg(ci, "OSDynLoad_Acquire", h_OSDynLoad_Acquire);
    reg(ci, "OSDynLoad_FindExport", h_OSDynLoad_FindExport);
    threads_register(reg);
    fs_register(reg);
}

static int slot_for(const std::string& key) {
    for (size_t i = 0; i < g_slots.size(); i++)
        if (g_slots[i].first == key) return (int)i;
    auto it = g_impl.find(key);
    g_slots.push_back({key, it != g_impl.end() ? it->second : nullptr});
    return (int)g_slots.size() - 1;
}

void hle_init(PPCContext& c) {
    (void)c;
    register_all();
    fs_init_paths();
    int data_imports = 0;
    for (size_t i = 0; i < g_import_count; i++) {
        const ImportEntry& e = g_import_table[i];
        int s = slot_for(std::string(e.lib) + "::" + e.name);
        g_import_slot[e.addr] = s;
        if (!e.is_func) {
            // Daten-Import: Zeiger-Variable. Funktionszeiger zeigen auf eine Fake-Adresse.
            st32(e.addr, FAKE_BASE + s * 4);
            data_imports++;
        }
    }
    printf("HLE: %zu Implementierungen, %zu Import-Slots, %d Daten-Imports\n",
           g_impl.size(), g_slots.size(), data_imports);
}

static void dispatch(PPCContext& c, int slot) {
    auto& [name, fn] = g_slots[slot];
    g_calls[name]++;
    if (g_trace) printf("[HLE] %s(0x%X, 0x%X, 0x%X) LR=0x%08X\n", name.c_str(), c.r[3], c.r[4], c.r[5], c.lr);
    if (fn) { fn(c); return; }
    if (g_missing.insert(name).second)
        printf("[HLE] fehlt: %s  (r3=0x%X r4=0x%X, LR=0x%08X)\n", name.c_str(), c.r[3], c.r[4], c.lr);
    c.r[3] = 0;
}

void hle_call(PPCContext& c, uint32_t import_addr) {
    auto it = g_import_slot.find(import_addr);
    if (it == g_import_slot.end()) {
        fprintf(stderr, "[HLE] Unbekannte Import-Adresse 0x%08X\n", import_addr);
        exit(6);
    }
    dispatch(c, it->second);
}

bool hle_is_fake_addr(uint32_t addr) { return addr >= FAKE_BASE && addr < FAKE_BASE + 0x100000; }
void hle_call_fake(PPCContext& c, uint32_t addr) { dispatch(c, (int)((addr - FAKE_BASE) / 4)); }

void hle_report() {
    printf("\n=== HLE-Bericht ===\n");
    printf("Aufgerufene Systemfunktionen: %zu, davon fehlen: %zu\n", g_calls.size(), g_missing.size());
    for (auto& [name, n] : g_calls)
        printf("  %-45s %8llu %s\n", name.c_str(), (unsigned long long)n,
               g_missing.count(name) ? "FEHLT" : "");
}

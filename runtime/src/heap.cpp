// MEM-Heaps von Cafe OS: Expanded Heaps (mit Freigeben) und Frame Heaps (Stapel).
#include "hle.h"
#include "heap.h"

#include <cstdio>
#include <map>

// Speicher-Aufteilung im Gast:
//   0x20000000 - 0x48000000  Standard-Heap des Spiels (MEMAllocFromDefaultHeap, Basis-Heap)
//   0x48000000 - 0x4E000000  interner Speicher der Runtime (hle_alloc)
//   0x4E000000 - 0x4F000000  Stack des Hauptthreads
static const uint32_t DEFAULT_HEAP_START = 0x20000000u;
static const uint32_t DEFAULT_HEAP_END = 0x48000000u;
static const uint32_t DEFAULT_HEAP_HANDLE = 0x7F000001u;

struct ExpHeap {
    std::map<uint32_t, uint32_t> free_blocks;   // Start -> Groesse
    std::map<uint32_t, uint32_t> used;          // Start -> Groesse
    uint32_t start = 0, end = 0;

    void init(uint32_t s, uint32_t e) { start = s; end = e; free_blocks.clear(); used.clear(); free_blocks[s] = e - s; }

    uint32_t alloc(uint32_t size, int32_t align) {
        if (!size) size = 1;
        size = (size + 3) & ~3u;
        uint32_t al = (uint32_t)(align < 0 ? -align : align);
        if (al < 4) al = 4;
        if (align >= 0) {
            for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
                uint32_t b = it->first, bs = it->second;
                uint32_t p = (b + al - 1) & ~(al - 1);
                if (p + size > b + bs) continue;
                take(it, p, size);
                return p;
            }
        } else {
            for (auto it = free_blocks.rbegin(); it != free_blocks.rend(); ++it) {
                uint32_t b = it->first, bs = it->second;
                if (bs < size) continue;
                uint32_t p = (b + bs - size) & ~(al - 1);
                if (p < b) continue;
                take(std::prev(it.base()), p, size);
                return p;
            }
        }
        return 0;
    }

    void take(std::map<uint32_t, uint32_t>::iterator it, uint32_t p, uint32_t size) {
        uint32_t b = it->first, bs = it->second;
        free_blocks.erase(it);
        if (p > b) free_blocks[b] = p - b;
        if (p + size < b + bs) free_blocks[p + size] = b + bs - (p + size);
        used[p] = size;
    }

    void free(uint32_t p) {
        auto u = used.find(p);
        if (u == used.end()) return;
        uint32_t size = u->second;
        used.erase(u);
        auto ins = free_blocks.emplace(p, size).first;
        auto next = std::next(ins);
        if (next != free_blocks.end() && ins->first + ins->second == next->first) {
            ins->second += next->second;
            free_blocks.erase(next);
        }
        if (ins != free_blocks.begin()) {
            auto prev = std::prev(ins);
            if (prev->first + prev->second == ins->first) {
                prev->second += ins->second;
                free_blocks.erase(ins);
            }
        }
    }

    uint32_t largest(int32_t align) const {
        uint32_t al = (uint32_t)(align < 0 ? -align : align), best = 0;
        if (al < 4) al = 4;
        for (auto& [b, bs] : free_blocks) {
            uint32_t p = (b + al - 1) & ~(al - 1);
            if (p < b + bs && b + bs - p > best) best = b + bs - p;
        }
        return best;
    }

    uint32_t total_free() const {
        uint32_t t = 0;
        for (auto& [b, bs] : free_blocks) t += bs;
        return t;
    }
};

struct FrmHeap { uint32_t start = 0, end = 0, head = 0, tail = 0; };

static std::map<uint32_t, ExpHeap> g_exp;
static std::map<uint32_t, FrmHeap> g_frm;

static ExpHeap& default_heap() {
    auto it = g_exp.find(DEFAULT_HEAP_HANDLE);
    if (it == g_exp.end()) {
        it = g_exp.emplace(DEFAULT_HEAP_HANDLE, ExpHeap{}).first;
        it->second.init(DEFAULT_HEAP_START, DEFAULT_HEAP_END);
    }
    return it->second;
}

static ExpHeap* exp_heap(uint32_t h) {
    if (h == DEFAULT_HEAP_HANDLE || h == 0) return &default_heap();
    auto it = g_exp.find(h);
    return it == g_exp.end() ? nullptr : &it->second;
}

// MEMCreateExpHeapEx(start, size, flags) -> Handle (der Header liegt am Anfang)
static void h_MEMCreateExpHeapEx(PPCContext& c) {
    uint32_t start = c.r[3], size = c.r[4];
    if (!start || size < 0x40) { c.r[3] = 0; return; }
    ExpHeap h;
    h.init(start + 0x40, start + size);   // 0x40 Bytes fuer den Header reservieren
    g_exp[start] = h;
    c.r[3] = start;
}
static void h_MEMDestroyExpHeap(PPCContext& c) { g_exp.erase(c.r[3]); c.r[3] = c.r[3]; }

// MEMAllocFromExpHeapEx(heap, size, align)
static void h_MEMAllocFromExpHeapEx(PPCContext& c) {
    ExpHeap* h = exp_heap(c.r[3]);
    uint32_t p = h ? h->alloc(c.r[4], (int32_t)c.r[5]) : 0;
    if (!p) fprintf(stderr, "[Heap] Alloc fehlgeschlagen: Heap 0x%08X, %u Bytes\n", c.r[3], c.r[4]);
    c.r[3] = p;
}
static void h_MEMFreeToExpHeap(PPCContext& c) {
    if (ExpHeap* h = exp_heap(c.r[3])) h->free(c.r[4]);
}
static void h_MEMGetAllocatableSizeForExpHeapEx(PPCContext& c) {
    ExpHeap* h = exp_heap(c.r[3]);
    c.r[3] = h ? h->largest((int32_t)c.r[4]) : 0;
}
static void h_MEMGetTotalFreeSizeForExpHeap(PPCContext& c) {
    ExpHeap* h = exp_heap(c.r[3]);
    c.r[3] = h ? h->total_free() : 0;
}

// Frame Heaps
static void h_MEMCreateFrmHeapEx(PPCContext& c) {
    uint32_t start = c.r[3], size = c.r[4];
    if (!start || size < 0x40) { c.r[3] = 0; return; }
    FrmHeap f{start + 0x40, start + size, start + 0x40, start + size};
    g_frm[start] = f;
    c.r[3] = start;
}
static void h_MEMDestroyFrmHeap(PPCContext& c) { g_frm.erase(c.r[3]); }
static void h_MEMAllocFromFrmHeapEx(PPCContext& c) {
    auto it = g_frm.find(c.r[3]);
    if (it == g_frm.end()) { c.r[3] = 0; return; }
    FrmHeap& f = it->second;
    uint32_t size = (c.r[4] + 3) & ~3u;
    int32_t align = (int32_t)c.r[5];
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    uint32_t p;
    if (align >= 0) {
        p = (f.head + al - 1) & ~(al - 1);
        if (p + size > f.tail) { c.r[3] = 0; return; }
        f.head = p + size;
    } else {
        p = (f.tail - size) & ~(al - 1);
        if (p < f.head) { c.r[3] = 0; return; }
        f.tail = p;
    }
    c.r[3] = p;
}
static void h_MEMGetAllocatableSizeForFrmHeapEx(PPCContext& c) {
    auto it = g_frm.find(c.r[3]);
    if (it == g_frm.end()) { c.r[3] = 0; return; }
    int32_t align = (int32_t)c.r[4];
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    uint32_t p = (it->second.head + al - 1) & ~(al - 1);
    c.r[3] = p < it->second.tail ? it->second.tail - p : 0;
}

// Standard-Heap und Basis-Heap
static void h_MEMGetBaseHeapHandle(PPCContext& c) { c.r[3] = DEFAULT_HEAP_HANDLE; }
static void h_MEMAllocFromDefaultHeap(PPCContext& c) { c.r[3] = default_heap().alloc(c.r[3], 0x40); }
static void h_MEMAllocFromDefaultHeapEx(PPCContext& c) { c.r[3] = default_heap().alloc(c.r[3], (int32_t)c.r[4]); }
static void h_MEMFreeToDefaultHeap(PPCContext& c) { default_heap().free(c.r[3]); }

void heap_register(void (*reg)(const char*, const char*, void (*)(PPCContext&))) {
    const char* ci = "coreinit";
    reg(ci, "MEMCreateExpHeapEx", h_MEMCreateExpHeapEx);
    reg(ci, "MEMDestroyExpHeap", h_MEMDestroyExpHeap);
    reg(ci, "MEMAllocFromExpHeapEx", h_MEMAllocFromExpHeapEx);
    reg(ci, "MEMFreeToExpHeap", h_MEMFreeToExpHeap);
    reg(ci, "MEMGetAllocatableSizeForExpHeapEx", h_MEMGetAllocatableSizeForExpHeapEx);
    reg(ci, "MEMGetTotalFreeSizeForExpHeap", h_MEMGetTotalFreeSizeForExpHeap);
    reg(ci, "MEMCreateFrmHeapEx", h_MEMCreateFrmHeapEx);
    reg(ci, "MEMDestroyFrmHeap", h_MEMDestroyFrmHeap);
    reg(ci, "MEMAllocFromFrmHeapEx", h_MEMAllocFromFrmHeapEx);
    reg(ci, "MEMGetAllocatableSizeForFrmHeapEx", h_MEMGetAllocatableSizeForFrmHeapEx);
    reg(ci, "MEMGetBaseHeapHandle", h_MEMGetBaseHeapHandle);
    reg(ci, "MEMAllocFromDefaultHeap", h_MEMAllocFromDefaultHeap);
    reg(ci, "MEMAllocFromDefaultHeapEx", h_MEMAllocFromDefaultHeapEx);
    reg(ci, "MEMFreeToDefaultHeap", h_MEMFreeToDefaultHeap);
}

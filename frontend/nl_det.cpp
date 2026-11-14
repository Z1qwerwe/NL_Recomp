// Nintendo Land Recomp - deterministischer Modus (Grundlage fuer Netplay wie bei Dolphin).
//
// Alle Spieler muessen das Spiel bit-genau gleich ausfuehren. Dafuer laeuft alles, was das
// Spiel von "aussen" mitbekommt, nach einer virtuellen Uhr statt nach echter Zeit:
//  - Spiel-Uhr (OSGetTime, mftb) = ausgefuehrte CPU-Takte        (Patch in PPCTimer.cpp)
//  - VSync/Flip alle 1/59.94 s virtueller Zeit                   (hier)
//  - Audio-Takt alle 3 ms virtueller Zeit                         (Patch in ax_out.cpp)
//  - System-Anfragen (Dateien usw.) kommen synchron zurueck       (Patch in coreinit_IPC.cpp)
//  - nur ein CPU-Kern, feste Uhrzeit/Datum                        (Patches ActiveSettings/CafeSystem)
// Hat das Spiel nichts zu tun, wird die virtuelle Uhr vorgespult und auf Echtzeit gebremst.
//
// Test: NL_DET=1 und NL_DET_HASH=1 setzen -> alle 300 VSyncs steht ein Fingerabdruck des
// Spielspeichers im Log ("NL-DET"). Zwei Laeufe ohne Eingaben muessen identisch sein.
#include "nl_app.h"

#include "Cafe/HW/MMU/MMU.h"

#include <chrono>
#include <thread>

extern bool g_nl_det;
extern std::atomic<uint64> g_nl_vclock;
extern uint64 g_nl_det_epoch;
extern std::atomic<uint64> g_nl_cnt[8];
extern void (*g_nl_det_tick)();
extern void (*g_nl_det_idle)();
void NL_Det_SignalVsync();
int NL_IPC_PendingCount();
int NL_Legacy_PendingCount();

static constexpr uint64 kClock = 1243125000ull;                 // Espresso-Takt
static constexpr uint64 kVsync = kClock * 1001ull / 60000ull;   // 59.94 Hz
static constexpr uint64 kIdleStep = kClock / 2000;              // 0.5 ms vorspulen pro Leerlauf-Runde

static uint64 s_next_vsync = 0;
static uint64 s_vsync_count = 0;
static bool s_hash = false;
static int s_hash_every = 300;
static uint64 s_quit_after = 0;
static bool s_trace = false;      // jede halbe Sekunde die virtuelle Uhr loggen   // Test: nach so vielen VSyncs beenden

// Echtzeit-Bremse
static std::chrono::steady_clock::time_point s_real0;
static uint64 s_v0 = 0;
static bool s_pace_init = false;

static bool s_hash_detail = false;   // zusaetzlich Fingerabdruck pro 16-MB-Block loggen

static uint64 hash_block(uint32 start, uint32 size) {
    uint64 h = 1469598103934665603ull;
    for (uint64 a = start; a < (uint64)start + size; a += 0x100000) {
        if (!memory_isAddressRangeAccessible((MPTR)a, 0x100000)) continue;
        const uint64* p = (const uint64*)(memory_base + a);
        for (size_t i = 0; i < 0x100000 / 8; i++) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    return h;
}

static void log_hash_detail(uint64 vsync) {
    std::string line;
    auto add = [&](uint32 start, uint32 end, uint32 step) {
        for (uint64 a = start; a < end; a += step)
            line += fmt::format(" {:08x}:{:016x}", (uint32)a, hash_block((uint32)a, step));
    };
    add(0x10000000, 0x50000000, 0x1000000);   // MEM2 in 16-MB-Bloecken
    add(0xF4000000, 0xF6000000, 0x200000);    // MEM1 in 2-MB-Bloecken
    cemuLog_log(LogType::Force, "NL-DET-B vsync={}{}", vsync, line);
}

static uint64 hash_guest_memory() {
    // FNV-1a ueber alle zugaenglichen 1-MB-Bloecke von MEM1 und MEM2
    uint64 h = 1469598103934665603ull;
    auto add_range = [&](uint32 start, uint32 end) {
        for (uint64 a = start; a < end; a += 0x100000) {
            if (!memory_isAddressRangeAccessible((MPTR)a, 0x100000)) continue;
            const uint64* p = (const uint64*)(memory_base + a);
            for (size_t i = 0; i < 0x100000 / 8; i++) {
                h ^= p[i];
                h *= 1099511628211ull;
            }
            h ^= a;
        }
    };
    add_range(0x10000000, 0x50000000);   // MEM2
    add_range(0xF4000000, 0xF6000000);   // MEM1
    return h;
}

// wird bei jedem Scheduler-Durchlauf aufgerufen (Spiel-Thread, ohne Scheduler-Lock)
static void det_tick() {
    const uint64 now = g_nl_vclock.load();
    if (s_next_vsync == 0) s_next_vsync = now + kVsync;
    while (now >= s_next_vsync) {
        s_next_vsync += kVsync;
        s_vsync_count++;
        NL_Det_SignalVsync();
        if (s_trace && (s_vsync_count <= 120 || s_vsync_count % 30 == 0))
            cemuLog_log(LogType::Force, "NL-DET-T vsync={} vclock={} ipc={} ipclang={} gpu={} leer={} audio={}", s_vsync_count,
                        g_nl_vclock.load(), g_nl_cnt[0].load(), g_nl_cnt[1].load(), g_nl_cnt[2].load(), g_nl_cnt[3].load(), g_nl_cnt[4].load());
        if (s_hash && s_vsync_count % s_hash_every == 0) {
            auto t0 = std::chrono::steady_clock::now();
            uint64 h = hash_guest_memory();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            cemuLog_log(LogType::Force, "NL-DET vsync={} vclock={} hash={:016x} ({} ms)", s_vsync_count, now, h, ms);
            if (s_hash_detail) log_hash_detail(s_vsync_count);
        }
        if (s_quit_after && s_vsync_count >= s_quit_after) {
            cemuLog_log(LogType::Force, "NL-DET: Testlauf fertig nach {} VSyncs", s_vsync_count);
            cemuLog_waitForFlush();
            nl_request(NLRequest::Quit);
            s_quit_after = 0;
        }
    }
}

// wird aufgerufen, wenn kein Spiel-Thread laufbereit ist
static void det_idle() {
    // Auf offene System-Antworten warten, ohne die Uhr weiterzudrehen. Dauer-Anfragen (warten
    // absichtlich auf ein Ereignis) blockieren nach 200 ms nicht mehr.
    static int s_last_pending = 0;
    static std::chrono::steady_clock::time_point s_pending_since;
    const int pending = NL_IPC_PendingCount() + NL_Legacy_PendingCount();
    if (pending != s_last_pending) {
        s_last_pending = pending;
        s_pending_since = std::chrono::steady_clock::now();
    }
    if (pending > 0 && std::chrono::steady_clock::now() - s_pending_since < std::chrono::milliseconds(200)) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        return;
    }
    const uint64 v = g_nl_vclock.fetch_add(kIdleStep) + kIdleStep;
    g_nl_cnt[3]++;
    // Echtzeit-Bremse: virtuelle Zeit darf der echten nicht davonlaufen
    auto real = std::chrono::steady_clock::now();
    if (!s_pace_init) {
        s_pace_init = true;
        s_real0 = real;
        s_v0 = v;
    }
    auto target = s_real0 + std::chrono::nanoseconds((v - s_v0) * 1000000000ull / kClock);
    if (target > real) {
        std::this_thread::sleep_until(target);
    } else if (real - target > std::chrono::milliseconds(250)) {
        // zu weit hinten (Ruckler) -> Bremse neu ansetzen statt aufzuholen
        s_real0 = real;
        s_v0 = v;
    }
}

bool nl_det_init() {
    const char* e = getenv("NL_DET");
    if (!e || *e != '1') return false;
    g_nl_det = true;
    g_nl.pad_gyro = false;   // Controller-Gyro rauscht -> im Test aus (beim Netplay werden Bewegungsdaten mit uebertragen)
    g_nl_vclock = kClock;   // 1 s, damit keine Zeitdifferenz negativ wird
    if (const char* ep = getenv("NL_DET_EPOCH")) g_nl_det_epoch = strtoull(ep, nullptr, 10);
    const char* hsh = getenv("NL_DET_HASH");
    s_hash = hsh && *hsh == '1';
    if (const char* ev = getenv("NL_DET_HASH_EVERY")) s_hash_every = std::max(1, atoi(ev));
    if (const char* q = getenv("NL_DET_QUIT_AFTER")) s_quit_after = strtoull(q, nullptr, 10);
    if (const char* t = getenv("NL_DET_TRACE")) s_trace = *t == '1';
    if (const char* hd = getenv("NL_DET_HASH_DETAIL")) s_hash_detail = *hd == '1';
    g_nl_det_tick = det_tick;
    g_nl_det_idle = det_idle;
    cemuLog_log(LogType::Force, "NL-DET: deterministischer Modus an (Fingerabdruck {})", s_hash ? "an" : "aus");
    return true;
}

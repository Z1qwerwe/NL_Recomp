// Threads, Mutexe, Events, Message-Queues und Alarme von Cafe OS.
//
// Jeder Gast-Thread ist ein echter Host-Thread. Damit der Spielcode sich nicht
// selbst in die Quere kommt, laeuft immer nur ein Gast-Thread gleichzeitig
// (globaler Lock). Abgegeben wird er bei allem, was blockiert oder wartet.
#include "hle.h"
#include "threads.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

static std::mutex g_gil;
static std::condition_variable_any g_cv;   // wird bei jeder Zustandsaenderung geweckt

struct GuestThread {
    uint32_t addr = 0;
    uint32_t entry = 0, argc = 0, argv = 0, stack_top = 0;
    PPCContext ctx{};
    bool started = false, finished = false;
    uint32_t exit_value = 0;
    uint32_t specific[16] = {};
    std::thread host;
};

static std::map<uint32_t, std::unique_ptr<GuestThread>> g_threads;
static thread_local GuestThread* t_current = nullptr;
static GuestThread g_main_thread;

// Den Lock haelt der aktuell laufende Gast-Thread. Waehrend er wartet, gibt er ihn ab.
static std::unique_lock<std::mutex>& gil() {
    static thread_local std::unique_lock<std::mutex> lk(g_gil, std::defer_lock);
    return lk;
}

void threads_main_enter() {
    gil().lock();
    t_current = &g_main_thread;
}

static GuestThread* cur() {
    if (!t_current) t_current = &g_main_thread;
    if (!g_main_thread.addr && t_current == &g_main_thread) g_main_thread.addr = hle_alloc(0x6A0, 8);
    return t_current;
}

template <class Pred>
static void wait_until(Pred p) { g_cv.wait(gil(), p); }

template <class Pred>
static bool wait_until_for(Pred p, std::chrono::nanoseconds ns) { return g_cv.wait_for(gil(), ns, p); }

static void notify() { g_cv.notify_all(); }

static std::chrono::nanoseconds ticks_to_ns(uint64_t ticks) {
    return std::chrono::nanoseconds((int64_t)((double)ticks / 0.06215625));
}

// --- Threads ------------------------------------------------------------------
// BOOL OSCreateThread(OSThread*, entry, argc, argv, stackTop, stackSize, prio, attr)
static void h_OSCreateThread(PPCContext& c) {
    auto t = std::make_unique<GuestThread>();
    t->addr = c.r[3];
    t->entry = c.r[4];
    t->argc = c.r[5];
    t->argv = c.r[6];
    t->stack_top = c.r[7];
    memcpy(t->ctx.gqr, c.gqr, sizeof c.gqr);
    printf("[Threads] OSCreateThread 0x%08X entry=0x%08X stack=0x%08X\n", t->addr, t->entry, t->stack_top);
    g_threads[t->addr] = std::move(t);
    c.r[3] = 1;
}

static void thread_body(GuestThread* t) {
    gil().lock();
    t_current = t;
    PPCContext& c = t->ctx;
    c.r[1] = (t->stack_top - 0x20) & ~0xFu;
    c.r[2] = g_sda2_base;
    c.r[13] = g_sda_base;
    c.r[3] = t->argc;
    c.r[4] = t->argv;
    call_indirect(c, t->entry);
    t->exit_value = c.r[3];
    t->finished = true;
    printf("[Threads] Thread 0x%08X beendet (%d)\n", t->addr, (int32_t)t->exit_value);
    notify();
    gil().unlock();
}

static void h_OSResumeThread(PPCContext& c) {
    auto it = g_threads.find(c.r[3]);
    if (it == g_threads.end()) { c.r[3] = 0; return; }
    GuestThread* t = it->second.get();
    if (!t->started) {
        t->started = true;
        t->host = std::thread(thread_body, t);
        t->host.detach();
    }
    c.r[3] = 1;  // vorheriger Suspend-Zaehler
}

static void h_OSJoinThread(PPCContext& c) {
    auto it = g_threads.find(c.r[3]);
    if (it == g_threads.end()) { c.r[3] = 0; return; }
    GuestThread* t = it->second.get();
    uint32_t out = c.r[4];
    wait_until([t] { return t->finished; });
    if (out) st32(out, t->exit_value);
    c.r[3] = 1;
}

static void h_OSGetCurrentThread(PPCContext& c) { c.r[3] = cur()->addr; }
static void h_OSGetThreadSpecific(PPCContext& c) { c.r[3] = cur()->specific[c.r[3] & 15]; }
static void h_OSSetThreadSpecific(PPCContext& c) { cur()->specific[c.r[3] & 15] = c.r[4]; }
static void h_OSGetThreadPriority(PPCContext& c) { c.r[3] = 16; }
static void h_ret1(PPCContext& c) { c.r[3] = 1; }

static void h_OSYieldThread(PPCContext&) {
    gil().unlock();
    std::this_thread::yield();
    gil().lock();
}

static void h_OSSleepTicks(PPCContext& c) {
    uint64_t ticks = ((uint64_t)c.r[3] << 32) | c.r[4];
    gil().unlock();
    if (ticks) std::this_thread::sleep_for(ticks_to_ns(ticks));
    else std::this_thread::yield();
    gil().lock();
}

// --- Mutex --------------------------------------------------------------------
struct Mutex { GuestThread* owner = nullptr; int count = 0; };
static std::map<uint32_t, Mutex> g_mutex;

static void h_OSInitMutex(PPCContext& c) { g_mutex[c.r[3]] = Mutex{}; }
static void h_OSLockMutex(PPCContext& c) {
    Mutex& m = g_mutex[c.r[3]];
    GuestThread* me = cur();
    if (m.owner != me) wait_until([&] { return m.owner == nullptr; });
    m.owner = me;
    m.count++;
}
static void h_OSTryLockMutex(PPCContext& c) {
    Mutex& m = g_mutex[c.r[3]];
    GuestThread* me = cur();
    if (m.owner && m.owner != me) { c.r[3] = 0; return; }
    m.owner = me;
    m.count++;
    c.r[3] = 1;
}
static void h_OSUnlockMutex(PPCContext& c) {
    Mutex& m = g_mutex[c.r[3]];
    if (m.owner == cur() && --m.count <= 0) { m.owner = nullptr; m.count = 0; notify(); }
}

// --- Events ---------------------------------------------------------------------
struct Event { bool signaled = false; bool autoreset = false; };
static std::map<uint32_t, Event> g_event;

static void h_OSInitEvent(PPCContext& c) { g_event[c.r[3]] = Event{c.r[4] != 0, c.r[5] == 1}; }
static void h_OSSignalEvent(PPCContext& c) { g_event[c.r[3]].signaled = true; notify(); }
static void h_OSResetEvent(PPCContext& c) { g_event[c.r[3]].signaled = false; }
static void h_OSWaitEvent(PPCContext& c) {
    Event& e = g_event[c.r[3]];
    wait_until([&] { return e.signaled; });
    if (e.autoreset) e.signaled = false;
}
static void h_OSWaitEventWithTimeout(PPCContext& c) {
    Event& e = g_event[c.r[3]];
    uint64_t ns = ((uint64_t)c.r[5] << 32) | c.r[6];
    bool ok = wait_until_for([&] { return e.signaled; }, std::chrono::nanoseconds(ns));
    if (ok && e.autoreset) e.signaled = false;
    c.r[3] = ok ? 1 : 0;
}

// --- Message-Queues -------------------------------------------------------------
struct MsgQueue { std::deque<std::array<uint32_t, 4>> msgs; uint32_t cap = 1; };
static std::map<uint32_t, MsgQueue> g_queue;

static void h_OSInitMessageQueue(PPCContext& c) {
    MsgQueue q;
    q.cap = c.r[5] ? c.r[5] : 1;
    g_queue[c.r[3]] = q;
}
static void h_OSSendMessage(PPCContext& c) {
    MsgQueue& q = g_queue[c.r[3]];
    std::array<uint32_t, 4> m;
    for (int k = 0; k < 4; k++) m[k] = ld32(c.r[4] + k * 4);
    bool block = c.r[5] & 1;
    if (q.msgs.size() >= q.cap) {
        if (!block) { c.r[3] = 0; return; }
        wait_until([&] { return q.msgs.size() < q.cap; });
    }
    if (c.r[5] & 2) q.msgs.push_front(m);  // OS_MESSAGE_FLAGS_HIGH_PRIORITY
    else q.msgs.push_back(m);
    notify();
    c.r[3] = 1;
}
static void h_OSReceiveMessage(PPCContext& c) {
    MsgQueue& q = g_queue[c.r[3]];
    uint32_t out = c.r[4];
    bool block = c.r[5] & 1;
    if (q.msgs.empty()) {
        if (!block) { c.r[3] = 0; return; }
        wait_until([&] { return !q.msgs.empty(); });
    }
    auto m = q.msgs.front();
    q.msgs.pop_front();
    for (int k = 0; k < 4; k++) st32(out + k * 4, m[k]);
    notify();
    c.r[3] = 1;
}

// --- Rendezvous -----------------------------------------------------------------
static std::map<uint32_t, uint32_t> g_rendezvous;
static void h_OSInitRendezvous(PPCContext& c) { g_rendezvous[c.r[3]] = 0; }
static void h_OSWaitRendezvous(PPCContext& c) {
    uint32_t& n = g_rendezvous[c.r[3]];
    n++;
    notify();
    // Alle Kerne, die das Spiel angibt (Bitmaske in r4), muessen ankommen
    uint32_t mask = c.r[4], need = 0;
    for (int k = 0; k < 3; k++) need += (mask >> k) & 1;
    wait_until_for([&] { return n >= need; }, std::chrono::milliseconds(50));
    c.r[3] = 1;
}

// --- Alarme ---------------------------------------------------------------------
struct Alarm {
    uint32_t addr, callback = 0, userdata = 0;
    uint64_t start_tb = 0, period = 0;
    bool active = false;
    uint64_t generation = 0;
};
static std::map<uint32_t, Alarm> g_alarm;
static uint32_t g_alarm_stack = 0;

static uint64_t tb_now() { return ((uint64_t)ppc_mftb(269) << 32) | ppc_mftb(268); }

static void alarm_worker(uint32_t addr, uint64_t gen, uint64_t delay, uint64_t period) {
    std::this_thread::sleep_for(ticks_to_ns(delay));
    PPCContext ctx{};
    while (true) {
        gil().lock();
        auto it = g_alarm.find(addr);
        if (it == g_alarm.end() || !it->second.active || it->second.generation != gen) {
            gil().unlock();
            return;
        }
        Alarm& a = it->second;
        if (!g_alarm_stack) g_alarm_stack = hle_alloc(0x10000, 16) + 0x10000;
        ctx.r[1] = g_alarm_stack - 0x40;
        ctx.r[2] = g_sda2_base;
        ctx.r[13] = g_sda_base;
        ctx.r[3] = a.addr;
        ctx.r[4] = 0;  // OSContext* (vereinfachtes Modell)
        uint32_t cb = a.callback;
        if (!period) a.active = false;
        call_indirect(ctx, cb);
        notify();
        gil().unlock();
        if (!period) return;
        std::this_thread::sleep_for(ticks_to_ns(period));
    }
}

static void h_OSCreateAlarm(PPCContext& c) { g_alarm[c.r[3]] = Alarm{c.r[3]}; }
static void h_OSSetAlarmUserData(PPCContext& c) { g_alarm[c.r[3]].userdata = c.r[4]; st32(c.r[3] + 0x30, c.r[4]); }
static void h_OSGetAlarmUserData(PPCContext& c) { c.r[3] = g_alarm[c.r[3]].userdata; }
static void h_OSCancelAlarm(PPCContext& c) {
    Alarm& a = g_alarm[c.r[3]];
    a.active = false;
    a.generation++;
    c.r[3] = 1;
}
// OSSetAlarm(alarm, u64 time (r5:r6), callback r7)
static void h_OSSetAlarm(PPCContext& c) {
    Alarm& a = g_alarm[c.r[3]];
    a.addr = c.r[3];
    a.callback = c.r[7];
    a.period = 0;
    a.active = true;
    a.generation++;
    uint64_t delay = ((uint64_t)c.r[5] << 32) | c.r[6];
    std::thread(alarm_worker, a.addr, a.generation, delay, 0).detach();
    c.r[3] = 1;
}
// OSSetPeriodicAlarm(alarm, u64 start (r5:r6), u64 period (r7:r8), callback r9)
static void h_OSSetPeriodicAlarm(PPCContext& c) {
    Alarm& a = g_alarm[c.r[3]];
    a.addr = c.r[3];
    a.callback = c.r[9];
    uint64_t start = ((uint64_t)c.r[5] << 32) | c.r[6];
    a.period = ((uint64_t)c.r[7] << 32) | c.r[8];
    a.active = true;
    a.generation++;
    uint64_t now = tb_now();
    uint64_t delay = start > now ? start - now : 0;
    std::thread(alarm_worker, a.addr, a.generation, delay, a.period ? a.period : 1).detach();
    c.r[3] = 1;
}

// --- System-Infos ---------------------------------------------------------------
static void h_OSGetSystemInfo(PPCContext& c) {
    static uint32_t info = 0;
    if (!info) {
        info = hle_alloc(0x20, 8);
        st32(info + 0x0, 248625000);    // Busfrequenz
        st32(info + 0x4, 1243125000);   // CPU-Frequenz
        st64(info + 0x8, 0);            // Basiszeit
    }
    c.r[3] = info;
}

// OSTicksToCalendarTime(OSTime ticks (r3:r4), OSCalendarTime* out (r5))
static void h_OSTicksToCalendarTime(PPCContext& c) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    uint32_t o = c.r[5];
    int32_t vals[10] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon,
                        tm.tm_year + 1900, tm.tm_wday, tm.tm_yday, 0, 0};
    for (int k = 0; k < 10; k++) st32(o + k * 4, (uint32_t)vals[k]);
}

uint32_t threads_specific(int i) { return cur()->specific[i & 15]; }

void guest_sleep_ns(int64_t ns) {
    gil().unlock();
    if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    else std::this_thread::yield();
    gil().lock();
}

void threads_register(void (*reg)(const char*, const char*, void (*)(PPCContext&))) {
    const char* ci = "coreinit";
    reg(ci, "OSCreateThread", h_OSCreateThread);
    reg(ci, "OSResumeThread", h_OSResumeThread);
    reg(ci, "OSJoinThread", h_OSJoinThread);
    reg(ci, "OSGetCurrentThread", h_OSGetCurrentThread);
    reg(ci, "OSGetThreadSpecific", h_OSGetThreadSpecific);
    reg(ci, "OSSetThreadSpecific", h_OSSetThreadSpecific);
    reg(ci, "OSGetThreadPriority", h_OSGetThreadPriority);
    reg(ci, "OSSetThreadAffinity", h_ret1);
    reg(ci, "OSYieldThread", h_OSYieldThread);
    reg(ci, "OSSleepTicks", h_OSSleepTicks);
    reg(ci, "OSInitMutex", h_OSInitMutex);
    reg(ci, "OSLockMutex", h_OSLockMutex);
    reg(ci, "OSTryLockMutex", h_OSTryLockMutex);
    reg(ci, "OSUnlockMutex", h_OSUnlockMutex);
    reg(ci, "OSInitEvent", h_OSInitEvent);
    reg(ci, "OSSignalEvent", h_OSSignalEvent);
    reg(ci, "OSResetEvent", h_OSResetEvent);
    reg(ci, "OSWaitEvent", h_OSWaitEvent);
    reg(ci, "OSWaitEventWithTimeout", h_OSWaitEventWithTimeout);
    reg(ci, "OSInitMessageQueue", h_OSInitMessageQueue);
    reg(ci, "OSSendMessage", h_OSSendMessage);
    reg(ci, "OSReceiveMessage", h_OSReceiveMessage);
    reg(ci, "OSInitRendezvous", h_OSInitRendezvous);
    reg(ci, "OSWaitRendezvous", h_OSWaitRendezvous);
    reg(ci, "OSCreateAlarm", h_OSCreateAlarm);
    reg(ci, "OSSetAlarm", h_OSSetAlarm);
    reg(ci, "OSSetPeriodicAlarm", h_OSSetPeriodicAlarm);
    reg(ci, "OSCancelAlarm", h_OSCancelAlarm);
    reg(ci, "OSSetAlarmUserData", h_OSSetAlarmUserData);
    reg(ci, "OSGetAlarmUserData", h_OSGetAlarmUserData);
    reg(ci, "OSGetSystemInfo", h_OSGetSystemInfo);
    reg(ci, "OSTicksToCalendarTime", h_OSTicksToCalendarTime);
}

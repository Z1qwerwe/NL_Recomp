// Nintendo Land Netplay - Aroma Plugin (Meilenstein 1: Remote-Wiimote)
//
// Idee: Ein Wiimote-Input kommt per UDP aus dem Netzwerk und wird
// in den Wiimote-Kanal 1 des Spiels injiziert. Wenn das klappt, kannst du
// spaeter dasselbe Prinzip zwischen zwei Konsolen (Lockstep-Netplay) bauen.

#include <wups.h>
#include <coreinit/title.h>
#include <coreinit/thread.h>
#include <padscore/kpad.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>

WUPS_PLUGIN_NAME("NintendoLandNetplay");
WUPS_PLUGIN_DESCRIPTION("Experimentelles Online-Netplay fuer Nintendo Land");
WUPS_PLUGIN_VERSION("v0.1");
WUPS_PLUGIN_AUTHOR("du");
WUPS_PLUGIN_LICENSE("GPL");

WUPS_USE_WUT_DEVOPTAB();

// ---------------------------------------------------------------------------
// Konfiguration
// ---------------------------------------------------------------------------
// TODO: Trag hier die Title-ID deiner Nintendo-Land-Version ein.
// Beim ersten Start loggt das Plugin die Title-ID jeder App, die startet.
static constexpr uint64_t NINTENDO_LAND_TID = 0x0000000000000000ULL;

static constexpr uint16_t NET_PORT       = 4405;
static constexpr int      REMOTE_CHANNEL = 1; // Wiimote-Slot, der "remote" ist

// Paket, das vom Netzwerk kommt (vom PC-Script oder spaeter von der 2. Konsole)
struct __attribute__((packed)) InputPacket {
    uint32_t magic;   // 'NLNP'
    uint32_t frame;   // Frame-Nummer (fuer spaeteres Lockstep)
    uint32_t hold;    // KPAD-Buttons (WPAD_BUTTON_*)
    float    posX;    // Pointer X (-1..1)
    float    posY;    // Pointer Y (-1..1)
};
static constexpr uint32_t PACKET_MAGIC = 0x4E4C4E50; // 'NLNP'

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool        gActive      = false;
static int         gSocket      = -1;
static InputPacket gRemote      = {};
static bool        gHaveRemote  = false;
static uint32_t    gPrevHold    = 0;

static void netOpen() {
    gSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gSocket < 0) {
        WHBLogPrintf("[NLNP] socket() fehlgeschlagen");
        return;
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(NET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(gSocket, (sockaddr *) &addr, sizeof(addr)) < 0) {
        WHBLogPrintf("[NLNP] bind() fehlgeschlagen");
        close(gSocket);
        gSocket = -1;
        return;
    }
    // Non-blocking, damit wir im Input-Hook nie haengen
    int flags = fcntl(gSocket, F_GETFL, 0);
    fcntl(gSocket, F_SETFL, flags | O_NONBLOCK);
    WHBLogPrintf("[NLNP] lausche auf UDP-Port %d", NET_PORT);
}

static void netClose() {
    if (gSocket >= 0) {
        close(gSocket);
        gSocket = -1;
    }
}

// Alle wartenden Pakete lesen, das neueste gewinnt
static void netPoll() {
    if (gSocket < 0) return;
    InputPacket pkt;
    while (true) {
        int n = recv(gSocket, &pkt, sizeof(pkt), 0);
        if (n != (int) sizeof(pkt)) break;
        if (pkt.magic != PACKET_MAGIC) continue;
        gRemote     = pkt;
        gHaveRemote = true;
    }
}

// ---------------------------------------------------------------------------
// Plugin-Lifecycle
// ---------------------------------------------------------------------------
INITIALIZE_PLUGIN() {
}

ON_APPLICATION_START() {
    WHBLogUdpInit();
    uint64_t tid = OSGetTitleID();
    WHBLogPrintf("[NLNP] App gestartet, Title-ID: %016llX", tid);

    gActive = (tid == NINTENDO_LAND_TID);
    if (gActive) {
        WHBLogPrintf("[NLNP] Nintendo Land erkannt, Netplay aktiv");
        netOpen();
    }
}

ON_APPLICATION_REQUESTS_EXIT() {
    netClose();
    gActive     = false;
    gHaveRemote = false;
}

ON_APPLICATION_ENDS() {
    WHBLogUdpDeinit();
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
// Wiimote-Input (KPAD). Nintendo Land koennte auch WPADRead oder KPADRead
// nutzen. Wenn hier nie was geloggt wird, haeng dich stattdessen dort ein.
DECL_FUNCTION(int32_t, KPADReadEx, KPADChan chan, KPADStatus *data, uint32_t size, KPADError *error) {
    int32_t count = real_KPADReadEx(chan, data, size, error);

    if (!gActive || chan != (KPADChan) REMOTE_CHANNEL) {
        return count;
    }

    netPoll();
    if (!gHaveRemote) {
        return count;
    }

    // Wenn am Kanal keine echte Wiimote haengt, liefern wir trotzdem 1 Sample
    if (count <= 0 && size > 0) {
        memset(&data[0], 0, sizeof(KPADStatus));
        count = 1;
        if (error) *error = KPAD_ERROR_OK;
    }

    for (int32_t i = 0; i < count; i++) {
        KPADStatus &s = data[i];
        uint32_t hold  = gRemote.hold;
        s.hold         = hold;
        s.trigger      = hold & ~gPrevHold;
        s.release      = gPrevHold & ~hold;
        s.pos.x        = gRemote.posX;
        s.pos.y        = gRemote.posY;
        s.extensionType = WPAD_EXT_CORE;
        gPrevHold = hold;
    }
    return count;
}
WUPS_MUST_REPLACE(KPADReadEx, WUPS_LOADER_LIBRARY_PADSCORE, KPADReadEx);

// GamePad-Input (VPAD). Nur ein Logger fuer den Anfang, damit du siehst,
// wie oft das Spiel pollt (wichtig fuers Frame-Timing beim Lockstep).
DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError) {
    int32_t res = real_VPADRead(chan, buffers, count, outError);
    static uint32_t calls = 0;
    if (gActive && (++calls % 600) == 0) {
        WHBLogPrintf("[NLNP] VPADRead calls: %u", calls);
    }
    return res;
}
WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);

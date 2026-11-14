// Dateisystem von Cafe OS (FS*). /vol/content wird auf die entpackten
// Spieldateien abgebildet (Update zuerst), /vol/save auf einen Ordner am PC.
#include "hle.h"
#include "fs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

enum : int32_t {
    FS_OK = 0, FS_END = -2, FS_NOT_FOUND = -6, FS_EXISTS = -5, FS_ERROR = -17
};

static std::vector<fs::path> g_content_roots;
static fs::path g_save_root;
static std::string g_cwd = "/vol/content";

struct OpenFile { FILE* f = nullptr; uint64_t size = 0; };
struct OpenDir { std::vector<fs::directory_entry> entries; size_t pos = 0; };
static std::map<uint32_t, OpenFile> g_files;
static std::map<uint32_t, OpenDir> g_dirs;
static uint32_t g_next_handle = 1;
static bool g_trace_fs = getenv("NL_TRACE_FS") != nullptr;

void fs_init_paths() {
    const char* env = getenv("NL_CONTENT_DIRS");
    if (env) {
        std::string s = env;
#if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t a = 0;
        while (a <= s.size()) {
            size_t b = s.find(sep, a);
            if (b == std::string::npos) b = s.size();
            if (b > a) g_content_roots.emplace_back(s.substr(a, b - a));
            a = b + 1;
        }
    }
    const char* save = getenv("NL_SAVE_DIR");
    g_save_root = save ? fs::path(save) : fs::current_path() / "save";
    std::error_code ec;
    fs::create_directories(g_save_root, ec);
    printf("FS: %zu Content-Ordner, Spielstaende in %s\n", g_content_roots.size(),
           g_save_root.string().c_str());
}

static std::string gstr(uint32_t a) {
    std::string s;
    while (a && g_base[a]) s.push_back((char)g_base[a++]);
    return s;
}

static std::string absolute_guest(const std::string& p) {
    if (!p.empty() && p[0] == '/') return p;
    return g_cwd + "/" + p;
}

// Gast-Pfad -> Host-Pfad. Bei mehreren Content-Ordnern gewinnt der erste, in dem es die Datei gibt.
static fs::path to_host(const std::string& guest, bool for_write = false) {
    std::string p = absolute_guest(guest);
    const std::string content = "/vol/content", save = "/vol/save";
    if (p.compare(0, content.size(), content) == 0) {
        std::string rel = p.substr(content.size());
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        for (auto& r : g_content_roots) {
            fs::path h = r / fs::u8path(rel);
            if (fs::exists(h)) return h;
        }
        return g_content_roots.empty() ? fs::path() : g_content_roots.back() / fs::u8path(rel);
    }
    if (p.compare(0, save.size(), save) == 0) {
        std::string rel = p.substr(save.size());
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        fs::path h = g_save_root / fs::u8path(rel);
        if (for_write) { std::error_code ec; fs::create_directories(h.parent_path(), ec); }
        return h;
    }
    return fs::path();
}

static void write_stat(uint32_t out, const fs::path& h) {
    memset(g_base + out, 0, 0x64);
    std::error_code ec;
    bool dir = fs::is_directory(h, ec);
    st32(out + 0x0, dir ? 0x80000000u : 0x01000000u);  // Flags: Ordner / Datei
    st32(out + 0x4, 0x666);                              // Rechte
    st32(out + 0x10, dir ? 0 : (uint32_t)fs::file_size(h, ec));
}

static void ret(PPCContext& c, int32_t v) { c.gpr[3] = (uint32_t)v; }

// FSInit / Client-Verwaltung
static void h_ok(PPCContext& c) { ret(c, FS_OK); }
static void h_FSGetVolumeState(PPCContext& c) { ret(c, 1); }  // FS_VOLUME_STATE_READY

// FSOpenFile(client, block, path, mode, FSFileHandle* out, errorMask)
static void h_FSOpenFile(PPCContext& c) {
    std::string path = gstr(c.gpr[5]), mode = gstr(c.gpr[6]);
    bool write = mode.find_first_of("wa+") != std::string::npos;
    fs::path h = to_host(path, write);
    std::string m = mode.find('b') == std::string::npos ? mode + "b" : mode;
    FILE* f = h.empty() ? nullptr : fopen(h.string().c_str(), m.c_str());
    if (g_trace_fs || !f) printf("[FS] open %s (%s) -> %s\n", path.c_str(), mode.c_str(), f ? "ok" : "FEHLT");
    if (!f) { ret(c, FS_NOT_FOUND); return; }
    uint32_t handle = g_next_handle++;
    std::error_code ec;
    g_files[handle] = OpenFile{f, (uint64_t)fs::file_size(h, ec)};
    st32(c.gpr[7], handle);
    ret(c, FS_OK);
}

static void h_FSCloseFile(PPCContext& c) {
    auto it = g_files.find(c.gpr[5]);
    if (it != g_files.end()) { fclose(it->second.f); g_files.erase(it); }
    ret(c, FS_OK);
}

// FSReadFile(client, block, buf, size, count, handle, flag, errorMask) -> gelesene Elemente
static void h_FSReadFile(PPCContext& c) {
    auto it = g_files.find(c.gpr[8]);
    if (it == g_files.end() || !c.gpr[6]) { ret(c, FS_ERROR); return; }
    size_t n = fread(g_base + c.gpr[5], 1, (size_t)c.gpr[6] * c.gpr[7], it->second.f);
    ret(c, (int32_t)(n / c.gpr[6]));
}

// FSReadFileWithPos(client, block, buf, size, count, pos, handle, flag, errorMask)
static void h_FSReadFileWithPos(PPCContext& c) {
    auto it = g_files.find(c.gpr[9]);
    if (it == g_files.end() || !c.gpr[6]) { ret(c, FS_ERROR); return; }
    fseek(it->second.f, (long)c.gpr[8], SEEK_SET);
    size_t n = fread(g_base + c.gpr[5], 1, (size_t)c.gpr[6] * c.gpr[7], it->second.f);
    ret(c, (int32_t)(n / c.gpr[6]));
}

// FSWriteFile(client, block, buf, size, count, handle, flag, errorMask)
static void h_FSWriteFile(PPCContext& c) {
    auto it = g_files.find(c.gpr[8]);
    if (it == g_files.end() || !c.gpr[6]) { ret(c, FS_ERROR); return; }
    size_t n = fwrite(g_base + c.gpr[5], 1, (size_t)c.gpr[6] * c.gpr[7], it->second.f);
    ret(c, (int32_t)(n / c.gpr[6]));
}

// FSSetPosFile(client, block, handle, pos)
static void h_FSSetPosFile(PPCContext& c) {
    auto it = g_files.find(c.gpr[5]);
    if (it == g_files.end()) { ret(c, FS_ERROR); return; }
    fseek(it->second.f, (long)c.gpr[6], SEEK_SET);
    ret(c, FS_OK);
}

// FSGetStatFile(client, block, handle, FSStat* out)
static void h_FSGetStatFile(PPCContext& c) {
    auto it = g_files.find(c.gpr[5]);
    if (it == g_files.end()) { ret(c, FS_ERROR); return; }
    memset(g_base + c.gpr[6], 0, 0x64);
    st32(c.gpr[6] + 0x0, 0x01000000u);
    st32(c.gpr[6] + 0x4, 0x666);
    st32(c.gpr[6] + 0x10, (uint32_t)it->second.size);
    ret(c, FS_OK);
}

// FSGetStat(client, block, path, FSStat* out)
static void h_FSGetStat(PPCContext& c) {
    std::string path = gstr(c.gpr[5]);
    fs::path h = to_host(path);
    std::error_code ec;
    if (h.empty() || !fs::exists(h, ec)) {
        if (g_trace_fs) printf("[FS] stat %s -> FEHLT\n", path.c_str());
        ret(c, FS_NOT_FOUND);
        return;
    }
    write_stat(c.gpr[6], h);
    ret(c, FS_OK);
}

// FSOpenDir(client, block, path, FSDirHandle* out)
static void h_FSOpenDir(PPCContext& c) {
    std::string path = gstr(c.gpr[5]);
    fs::path h = to_host(path);
    std::error_code ec;
    if (h.empty() || !fs::is_directory(h, ec)) { ret(c, FS_NOT_FOUND); return; }
    OpenDir d;
    for (auto& e : fs::directory_iterator(h, ec)) d.entries.push_back(e);
    uint32_t handle = g_next_handle++;
    g_dirs[handle] = std::move(d);
    st32(c.gpr[6], handle);
    ret(c, FS_OK);
}

// FSReadDir(client, block, handle, FSDirEntry* out): FSStat (0x64) + Name
static void h_FSReadDir(PPCContext& c) {
    auto it = g_dirs.find(c.gpr[5]);
    if (it == g_dirs.end() || it->second.pos >= it->second.entries.size()) { ret(c, FS_END); return; }
    auto& e = it->second.entries[it->second.pos++];
    write_stat(c.gpr[6], e.path());
    std::string name = e.path().filename().u8string();
    memset(g_base + c.gpr[6] + 0x64, 0, 256);
    memcpy(g_base + c.gpr[6] + 0x64, name.c_str(), std::min<size_t>(name.size(), 255));
    ret(c, FS_OK);
}

static void h_FSCloseDir(PPCContext& c) { g_dirs.erase(c.gpr[5]); ret(c, FS_OK); }

static void h_FSChangeDir(PPCContext& c) { g_cwd = absolute_guest(gstr(c.gpr[5])); ret(c, FS_OK); }

// FSGetCwd(client, block, char* out, len)
static void h_FSGetCwd(PPCContext& c) {
    uint32_t len = c.gpr[6];
    std::string s = g_cwd.substr(0, len ? len - 1 : 0);
    memcpy(g_base + c.gpr[5], s.c_str(), s.size() + 1);
    ret(c, FS_OK);
}

void fs_register(void (*reg)(const char*, const char*, void (*)(PPCContext&))) {
    const char* ci = "coreinit";
    for (const char* n : {"FSInit", "FSShutdown", "FSAddClient", "FSDelClient", "FSInitCmdBlock",
                          "FSSetStateChangeNotification", "FSGetLastError"})
        reg(ci, n, h_ok);
    reg(ci, "FSGetVolumeState", h_FSGetVolumeState);
    reg(ci, "FSOpenFile", h_FSOpenFile);
    reg(ci, "FSCloseFile", h_FSCloseFile);
    reg(ci, "FSReadFile", h_FSReadFile);
    reg(ci, "FSReadFileWithPos", h_FSReadFileWithPos);
    reg(ci, "FSWriteFile", h_FSWriteFile);
    reg(ci, "FSSetPosFile", h_FSSetPosFile);
    reg(ci, "FSGetStatFile", h_FSGetStatFile);
    reg(ci, "FSGetStat", h_FSGetStat);
    reg(ci, "FSOpenDir", h_FSOpenDir);
    reg(ci, "FSReadDir", h_FSReadDir);
    reg(ci, "FSCloseDir", h_FSCloseDir);
    reg(ci, "FSChangeDir", h_FSChangeDir);
    reg(ci, "FSGetCwd", h_FSGetCwd);
}

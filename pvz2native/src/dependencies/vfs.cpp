/* Guest path translation -- see vfs.h for why this layer exists at all. */

#include <pvz2native/presence.h>
#include <pvz2native/dependencies/vfs.h>

#include <pvz2native/config.h>
#include <pvz2native/credits_screen.h>

#include <cctype>
#include <fcntl.h>
#include <string>

#if defined(_WIN32)
#include <io.h>
#endif

namespace pvz2native {
namespace vfs {

/* Real on-disk resources (doc section 9.22). The game's own native loader
 * (ResStreamsManager/ResourceManager: sub_2BE120 -> sub_867740/sub_864950)
 * opens "main.rsb" via libc fopen/fread and decodes the outer "1bsr" RSB plus
 * the inner "pgsr"/RTON formats ITSELF -- so none of those formats are
 * reimplemented here. It gets real file I/O over the real .obb, which literally
 * IS a "1bsr" archive (magic confirmed). */
/* Both come from config.ini (see pvz2_config_load): the host path is used
 * verbatim, and the guest path is the same with a leading '/' so
 * ResStreamsManager::LoadRSB takes its verbatim branch (see vfs.h). The guest
 * form is cached on first use -- the config path is fixed for the run. */
const char *obb_host_path() { return pvz2_config()->obb_path; }

const char *obb_guest_path() {
    static const std::string guest = std::string("/") + pvz2_config()->obb_path;
    return guest.c_str();
}

std::string translate(GuestRuntime *rt, std::string p) {
    /* SexyAppFramework's VFS tags every resource lookup with a scheme prefix
     * before it reaches libc; on Android the AAssetManager layer consumes it,
     * so the bytes libc sees never carry it. Stripping it here is what unblocked
     * the RSB load: the engine logged "Loading main RSB from the path <obb>"
     * and then asked for "ASSET:<obb>", which no host open could satisfy. */
    if (p.compare(0, kAssetSchemeLen, kAssetScheme) == 0) p.erase(0, kAssetSchemeLen);
    for (char &c : p) if (c == '\\') c = '/';
    /* Undo the leading slash obb_guest_path() carries to look absolute to the
     * engine: "/E:/..." is not a path the host can open. */
    if (p.size() >= 3 && p[0] == '/' && std::isalpha((unsigned char)p[1]) && p[2] == ':') {
        p.erase(0, 1);
    }

    /* Save data lands in a folder the ENGINE names: "No_Backup/" is a string
     * literal inside libPVZ2.so (on iOS/Android it marks data excluded from
     * cloud backup, which means nothing on a PC). Renaming it here rather than
     * patching the .so keeps this independent of the game build -- and this is
     * the layer every path already passes through on its way to libc, so it
     * covers the mkdir that creates it as well as the reads and writes. */
    static const char kEngineDataDir[] = "No_Backup";
    static const char kHostDataDir[] = "userdata";
    for (std::size_t at = p.find(kEngineDataDir); at != std::string::npos;
         at = p.find(kEngineDataDir, at + sizeof(kHostDataDir) - 1)) {
        p.replace(at, sizeof(kEngineDataDir) - 1, kHostDataDir);
    }

    if (p.find("modern34") != std::string::npos || p.find("Modern34") != std::string::npos) {
        credits_set_modern34_active(1);
    } else if (p.find("save_Board") != std::string::npos || p.find("_modern") != std::string::npos) {
        credits_set_modern34_active(0);
    }

    std::size_t slash = p.find_last_of('/');
    std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
    for (char &c : base) c = (char)std::tolower((unsigned char)c);

    if (base == "main.rsb") {
        if (rt != nullptr) rt->rsb_touched = true;
        return std::string(obb_host_path());
    }
    /* The engine may also open the .obb by the full path it got from
     * FrameworkInfo_SysGetMainExpansionFilePath -- that counts as reaching the
     * RSB load too. */
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".obb") == 0) {
        if (rt != nullptr) rt->rsb_touched = true;
    }
    return p;
}

int translate_open_flags(std::uint32_t g) {
    int h = (int)(g & 3); /* O_RDONLY/O_WRONLY/O_RDWR share values 0/1/2 */
#if defined(_WIN32)
    if (g & 0x40)  h |= _O_CREAT;
    if (g & 0x80)  h |= _O_EXCL;
    if (g & 0x200) h |= _O_TRUNC;
    if (g & 0x400) h |= _O_APPEND;
    h |= _O_BINARY;
#else
    if (g & 0x40)  h |= O_CREAT;
    if (g & 0x80)  h |= O_EXCL;
    if (g & 0x200) h |= O_TRUNC;
    if (g & 0x400) h |= O_APPEND;
#endif
    return h;
}

bool is_obb_path(const std::string &host_path) {
    auto norm = [](std::string s) {
        for (char &c : s) {
            if (c == '\\') c = '/';
            c = (char)std::tolower((unsigned char)c);
        }
        return s;
    };
    const std::string h = norm(host_path);
    if (h == norm(obb_host_path())) return true;
    return h.size() > 4 && h.compare(h.size() - 4, 4, ".obb") == 0;
}

void note_obb_read(GuestRuntime *rt, long long pos) {
    if (pos < 0 || !pvz2_presence_enabled()) return;
    std::string group, file;
    {
        std::lock_guard<std::mutex> lk(rt->rsb_index_lock);
        if (!rt->rsb_index_tried) {
            rt->rsb_index_tried = true;
            rt->rsb_index.load(obb_host_path());
        }
        if (const std::string *g = rt->rsb_index.group_at((std::uint64_t)pos)) group = *g;
        if (const std::string *n = rt->rsb_index.file_starting_at((std::uint64_t)pos)) file = *n;
    }
    /* Per thread, so a loader thread re-reading inside one group reports it once,
     * unless the read offset rewinds back to the start of the group on a later open. */
    static thread_local std::string last_group, last_file;
    static thread_local long long last_pos = -1;
    const bool rewind = pos <= last_pos;
    if (!group.empty()) {
        if (group != last_group || rewind) {
            last_group = group;
            pvz2_presence_note("rsb", group.c_str());
        }
        last_pos = pos;
    }
    if (!file.empty() && (file != last_file || rewind)) {
        last_file = file;
        pvz2_presence_note("rsbfile", file.c_str());
    }
}

}  // namespace vfs
}  // namespace pvz2native

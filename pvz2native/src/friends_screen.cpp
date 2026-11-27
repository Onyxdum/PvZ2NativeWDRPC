/* friends_screen.cpp -- the friends list opened by the world map's envelope button (see friends_screen.h).
 *
 * Looks like the achievements list (same panel and row art, same font): title, an "Invite" button, then one
 * row per player of the port known to the cloud server, playing ones first. */

#include <pvz2native/friends_screen.h>

#include <pvz2native/achievement_catalog.h>
#include <pvz2native/cloud_save.h>
#include <pvz2native/config.h>
#include <pvz2native/config_screen.h>
#include <pvz2native/game/symbols.h>
#include <pvz2native/host_assets.h>
#include <pvz2native/host_files.h>
#include <pvz2native/log.h>
#include <pvz2native/rton_tree.h>
#include <pvz2native/social_invite.h>
#include <pvz2native/presence.h>
#include <pvz2native/input/input_queue.h>

#include "stb_truetype.h"

/* A private copy, as in gfx/joust_avatars.cpp: Discord serves PNG (JPEG for the rest). */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <SDL.h>
#include <glad/gl.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" void apply_magenta_colorkey(SDL_Surface *surface);
extern "C" {
#include <pvz2native/elf32/elf32_loader.h>
}

namespace {

using namespace pvz2native;

/* =========================================================================
 * Data (worker thread)
 * ========================================================================= */
struct Friend {
    std::string id, name, avatar, version, level;
    bool online = false;
    long ago = 0; /* seconds since last seen */
    int plants = 0, worlds = 0, levels = 0;
    bool can_gift = false; /* not sent a gift today */
    bool is_request = false; /* incoming friend request (Accept / Reject) */
    std::string invite_code; /* server-signed HMAC code from invite link */
    std::string where, world; /* "level" / "map" / "menu", and the world map's world */
};

enum class ActionKind { Accept, Reject, Remove };

struct FriendAction {
    std::string id;
    std::string name;
    ActionKind kind = ActionKind::Accept;
    std::string invite_code;
};

constexpr int kInviteLocalPort = 47321;

enum class ListState { Loading, NotLinked, Ready, Error };

struct Picture {
    std::vector<std::uint8_t> rgba; /* kPicSide x kPicSide, round */
    bool ready = false;
    bool failed = false;
    GLuint tex = 0; /* main thread */
};

constexpr int kPicSide = 128;
constexpr int kStatusEverySeconds = 60;

/* Never destroyed: at exit the worker is detached and may still be inside curl, so what it touches has to
 * outlive the static destructors. */
std::mutex &g_lock = *new std::mutex;
std::vector<Friend> &g_friends = *new std::vector<Friend>;
std::vector<Friend> &g_local_invites = *new std::vector<Friend>;
std::vector<FriendAction> &g_action_queue = *new std::vector<FriendAction>;
std::vector<std::string> &g_pending_toasts = *new std::vector<std::string>;
std::set<std::string> &g_seen_requests = *new std::set<std::string>;
std::string &g_my_invite_code = *new std::string;
ListState g_state = ListState::Loading;
std::map<std::string, Picture> &g_pics = *new std::map<std::string, Picture>; /* by URL */

std::atomic<bool> g_refresh{true};
std::atomic<bool> g_stop{false};
std::thread &g_worker = *new std::thread;
bool g_worker_started = false;
std::thread &g_listener = *new std::thread;
bool g_listener_started = false;

int lang_idx() {
    const pvz2_config_t *cfg = pvz2_config();
    const char *loc = cfg ? cfg->user_locale : "";
    if (std::strncmp(loc, "es", 2) == 0) return 1;
    if (std::strncmp(loc, "fr", 2) == 0) return 2;
    if (std::strncmp(loc, "de", 2) == 0) return 3;
    if (std::strncmp(loc, "pt", 2) == 0) return 4;
    if (std::strncmp(loc, "it", 2) == 0) return 5;
    return 0;
}

const char *L6(const char *en, const char *es, const char *fr, const char *de, const char *pt, const char *it) {
    const char *t[6] = {en, es, fr, de, pt, it};
    return t[lang_idx()];
}

#ifdef _WIN32
int run_hidden(const std::string &cmd) {
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::string cmdline = "cmd.exe /C \"" + cmd + "\"";
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
#else
int run_hidden(const std::string &cmd) { return std::system((cmd + " >/dev/null 2>&1").c_str()); }
#endif

std::string percent_encode(const char *s) {
    std::string out;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (std::isalnum(*p) || *p == '-' || *p == '_' || *p == '.') {
            out.push_back((char)*p);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", *p);
            out += hex;
        }
    }
    return out;
}

bool safe_token(const char *s) {
    for (; *s; ++s)
        if (!(std::isalnum((unsigned char)*s) || *s == '_' || *s == '-' || *s == '.')) return false;
    return true;
}

/* Progress from the save: last level, plants owned, worlds and map levels/events completed. */
/* Also `map`: the completed world map events, "2:1,2,3;5:1,11" (world id: event ids). Which of them are
 * levels is decided by the server from the game's own map definitions (cloud-server LEVEL_EVENTS): the
 * save lists plant rewards, gates and upgrades too. */
void read_progress(std::string &level, int &plants, int &worlds, int &levels, std::string &map) {
    const std::vector<std::uint8_t> data = host_files::read("userdata/pp.dat");
    RtonNode root;
    if (data.empty() || !rton_parse(data, root)) return;
    const RtonNode *objects = root.get("objects");
    if (objects == nullptr || objects->kind != RtonNode::Arr || objects->a.empty()) return;
    const RtonNode *info = objects->a[0].get("objdata");
    if (info == nullptr) return;
    if (const RtonNode *l = info->get("l"); l && l->kind == RtonNode::Str) level = l->s;
    if (const RtonNode *p = info->get("p"); p && p->kind == RtonNode::Arr) plants = (int)p->a.size();
    if (const RtonNode *w = info->get("wmed"); w && w->kind == RtonNode::Arr) {
        worlds = (int)w->a.size();
        for (const RtonNode &world : w->a) {
            const RtonNode *id = world.get("w");
            const RtonNode *e = world.get("e");
            if (id == nullptr || id->kind != RtonNode::Int || e == nullptr || e->kind != RtonNode::Arr) continue;
            levels += (int)e->a.size();
            if (!map.empty()) map += ';';
            map += std::to_string(id->i) + ':';
            bool first = true;
            for (const RtonNode &ev : e->a)
                if (const RtonNode *i = ev.get("i"); i && i->kind == RtonNode::Int) {
                    if (!first) map += ',';
                    map += std::to_string(i->i);
                    first = false;
                }
        }
    }
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

/* Only Discord's own CDN, and nothing a shell could read as syntax. */
bool url_allowed(const std::string &url) {
    if (url.rfind("https://cdn.discordapp.com/", 0) != 0) return false;
    for (char ch : url)
        if (!(std::isalnum((unsigned char)ch) || std::strchr(":/._-?=&", ch))) return false;
    return true;
}

void load_picture(const std::string &url) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : url) h = (h ^ c) * 1099511628211ULL;
    char name[64];
    std::snprintf(name, sizeof(name), "userdata/friend_avatars/%016llx.img", h);
    std::vector<std::uint8_t> bytes = host_files::read(name);
    if (bytes.empty()) {
        std::error_code ec;
        std::filesystem::create_directories("userdata/friend_avatars", ec);
        run_hidden("curl -sS -f -m 15 -o \"" + std::string(name) + "\" \"" + url + "\"");
        bytes = host_files::read(name);
    }
    int w = 0, hgt = 0, n = 0;
    unsigned char *px =
        bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &hgt, &n, 4);
    std::vector<std::uint8_t> out;
    if (px != nullptr && w > 0 && hgt > 0) {
        /* Centre square, box-sampled down to kPicSide, cut to a circle with a soft edge. */
        const int side = std::min(w, hgt), ox = (w - side) / 2, oy = (hgt - side) / 2;
        out.resize((std::size_t)kPicSide * kPicSide * 4);
        for (int y = 0; y < kPicSide; ++y) {
            for (int x = 0; x < kPicSide; ++x) {
                const int x0 = ox + x * side / kPicSide, x1 = std::max(x0 + 1, ox + (x + 1) * side / kPicSide);
                const int y0 = oy + y * side / kPicSide, y1 = std::max(y0 + 1, oy + (y + 1) * side / kPicSide);
                unsigned sum[4] = {0, 0, 0, 0}, cnt = 0;
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx, ++cnt)
                        for (int c = 0; c < 4; ++c) sum[c] += px[((std::size_t)sy * w + sx) * 4 + c];
                std::uint8_t *o = &out[((std::size_t)y * kPicSide + x) * 4];
                for (int c = 0; c < 4; ++c) o[c] = (std::uint8_t)(sum[c] / (cnt ? cnt : 1));
                const float dx = x + 0.5f - kPicSide * 0.5f, dy = y + 0.5f - kPicSide * 0.5f;
                const float edge = kPicSide * 0.5f - std::sqrt(dx * dx + dy * dy);
                const float a = edge >= 1.0f ? 1.0f : (edge <= 0.0f ? 0.0f : edge);
                o[3] = (std::uint8_t)(o[3] * a);
            }
        }
    }
    if (px) stbi_image_free(px);
    std::lock_guard<std::mutex> lk(g_lock);
    Picture &p = g_pics[url];
    if (out.empty()) {
        pvz2_log("[friends] picture could not be loaded (%zu bytes): %s", bytes.size(), url.c_str());
        p.failed = true;
    } else {
        p.rgba = std::move(out);
        p.ready = true;
    }
}

bool linked(const Pvz2CloudConfig *cloud) {
    return cloud != nullptr && cloud->auth_discord_id[0] && cloud->auth_discord_token[0] &&
           safe_token(cloud->auth_discord_id) && safe_token(cloud->auth_discord_token);
}

/* One request to <cloud server>/friends/v1/<route> as the linked Discord account. Returns the HTTP status
 * (0 when the server could not be reached); `out` gets the body. */
int friends_http(const char *method, const std::string &route, const std::string &body, std::string &out,
                 int timeout_s = 12) {
    out.clear();
    const Pvz2CloudConfig *cloud = pvz2_cloud_config();
    if (!linked(cloud)) return 0;
    std::string base = cloud->nas_server_url[0] ? cloud->nas_server_url : PVZ2_DEFAULT_NAS_SERVER_URL;
    while (!base.empty() && base.back() == '/') base.pop_back();
    static std::atomic<unsigned> s_seq{0};
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string tag = "pvz2_friends_" + std::to_string(s_seq.fetch_add(1)) + "_";
    const std::string in_file = (dir / (tag + "in.json")).string();
    const std::string out_file = (dir / (tag + "out.txt")).string();
    const std::string code_file = (dir / (tag + "code.txt")).string();
    std::string cmd = "curl -sS -m " + std::to_string(timeout_s) + " -X " + method +
                      " -H \"Content-Type: application/json\""
                      " -H \"X-Discord-Id: " + std::string(cloud->auth_discord_id) + "\""
                      " -H \"Authorization: Bearer " + std::string(cloud->auth_discord_token) + "\"";
    const std::string enc_name = percent_encode(cloud->auth_discord_name);
    if (!enc_name.empty()) cmd += " -H \"X-Discord-Name: " + enc_name + "\"";
    if (std::strcmp(method, "POST") == 0) {
        host_files::write(in_file, std::vector<std::uint8_t>(body.begin(), body.end()));
        cmd += " --data-binary @\"" + in_file + "\"";
    }
    cmd += " -o \"" + out_file + "\" -w \"%{http_code}\" \"" + base + "/friends/v1/" + route + "\" > \"" +
           code_file + "\"";
    const int rc = run_hidden(cmd);
    const std::vector<std::uint8_t> bytes = host_files::read(out_file);
    const std::vector<std::uint8_t> code = host_files::read(code_file);
    std::error_code ec;
    std::filesystem::remove(in_file, ec);
    std::filesystem::remove(out_file, ec);
    std::filesystem::remove(code_file, ec);
    const int status = (rc != 0 || code.empty()) ? 0 : std::atoi(std::string(code.begin(), code.end()).c_str());
    out.assign(bytes.begin(), bytes.end());
    if (status != 200)
        pvz2_log("[friends] %s %s/friends/v1/%s: curl rc=%d, HTTP %d", method, base.c_str(), route.c_str(), rc,
                 status);
    return status;
}

/* Gifts this player sent from the list, waiting for the worker, and how each went (friend id -> HTTP). */
std::vector<std::string> &g_gift_queue = *new std::vector<std::string>;
std::map<std::string, int> &g_gift_result = *new std::map<std::string, int>;

/* One round trip: report this player, read the list back. */
void sync_once() {
    if (!linked(pvz2_cloud_config())) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_state = ListState::NotLinked;
        return;
    }
    std::string level, map;
    int plants = 0, worlds = 0, levels = 0;
    read_progress(level, plants, worlds, levels, map);
    /* Where the player is right now: the level being played or the world map's world, as the Rich Presence
     * sees them (the save's `l` is only the last level played). */
    char live_world[64] = {}, live_level[96] = {};
    pvz2_presence_location(live_world, sizeof(live_world), live_level, sizeof(live_level));
    const char *where = pvz2_game_is_in_level() ? "level" : pvz2_game_on_world_map() ? "map" : "menu";
    auto safe = [](const char *s) {
        std::string o;
        for (; *s; ++s)
            if (std::isalnum((unsigned char)*s) || *s == '_') o.push_back(*s);
        return o;
    };
    const std::string safe_level = safe(std::strcmp(where, "level") == 0 ? live_level : "");
    const std::string safe_world = safe(live_world);
    char head[512];
    std::snprintf(head, sizeof(head),
                  "{\"version\":\"%s\",\"where\":\"%s\",\"world\":\"%s\",\"level\":\"%s\",\"plants\":%d,"
                  "\"worlds\":%d,\"levels\":%d,\"playing\":true",
                  sym().version ? sym().version : "", where, safe_world.c_str(), safe_level.c_str(), plants, worlds,
                  levels);
    /* `map` holds only digits, ':', ',' and ';' -- nothing to escape in JSON. */
    const std::string body = std::string(head) + ",\"map\":\"" + map + "\"}";
    (void)level;
    std::string out;
    if (friends_http("POST", "status", body, out) != 200) {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_friends.empty()) g_state = ListState::Error;
        return;
    }
    std::vector<Friend> list;
    for (const std::string &line : split(out, '\n')) {
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 10) continue;
        Friend fr;
        fr.id = f[0];
        fr.name = f[1];
        fr.avatar = f[2];
        fr.online = f[3] == "1";
        fr.ago = std::atol(f[4].c_str());
        fr.version = f[5];
        fr.level = f[6];
        fr.plants = std::atoi(f[7].c_str());
        fr.worlds = std::atoi(f[8].c_str());
        fr.levels = std::atoi(f[9].c_str());
        fr.can_gift = f.size() > 10 && f[10] == "1";
        if (f.size() > 12) {
            fr.where = f[11];
            fr.world = f[12];
        }
        fr.is_request = f.size() > 13 && f[13] == "request";
        list.push_back(std::move(fr));
    }
    std::vector<std::string> to_load;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        /* Merge any local pvz2native:// invites not yet in the server list. */
        std::set<std::string> known_ids;
        for (const Friend &fr : list) known_ids.insert(fr.id);
        std::vector<Friend> merged;
        for (const Friend &loc : g_local_invites) {
            if (known_ids.find(loc.id) == known_ids.end()) merged.push_back(loc);
        }
        for (const Friend &fr : list) merged.push_back(fr);
        /* Announce brand-new incoming requests once via toast. */
        for (const Friend &fr : merged) {
            if (fr.is_request && g_seen_requests.insert(fr.id).second) {
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              L6("Friend request from %s", "Solicitud de amistad de %s",
                                 "Demande d'ami de %s", "Freundschaftsanfrage von %s",
                                 "Pedido de amizade de %s", "Richiesta di amicizia da %s"),
                              fr.name.empty() ? L6("a player", "un jugador", "un joueur", "einem Spieler", "um jogador", "un giocatore")
                                              : fr.name.c_str());
                g_pending_toasts.push_back(msg);
            }
        }
        g_friends = std::move(merged);
        g_state = ListState::Ready;
        for (const Friend &fr : g_friends)
            if (url_allowed(fr.avatar) && g_pics.find(fr.avatar) == g_pics.end()) {
                g_pics[fr.avatar];
                to_load.push_back(fr.avatar);
            }
    }
    pvz2_log("[friends] status sent (%s, %d plants, %d worlds, %d levels); %zu other player(s)",
             safe_level.c_str(), plants, worlds, levels, list.size());
    for (const std::string &url : to_load) {
        if (g_stop.load()) return;
        load_picture(url);
    }
    bool need_code = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        need_code = g_my_invite_code.empty();
    }
    if (need_code && !g_stop.load()) {
        std::string code_out;
        if (friends_http("GET", "invite-code", "", code_out) == 200) {
            while (!code_out.empty() && (code_out.back() == '\n' || code_out.back() == '\r' || code_out.back() == ' '))
                code_out.pop_back();
            if (code_out.size() == 16) {
                std::lock_guard<std::mutex> lk(g_lock);
                g_my_invite_code = code_out;
            }
        }
    }
}

void send_queued_gifts() {
    std::vector<std::string> queue;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        queue.swap(g_gift_queue);
    }
    for (const std::string &to : queue) {
        std::string out;
        const int status = friends_http("POST", "gift", "{\"to\":\"" + to + "\"}", out);
        pvz2_log("[friends] gift to %s: HTTP %d %s", to.c_str(), status, out.c_str());
        std::lock_guard<std::mutex> lk(g_lock);
        g_gift_result[to] = status;
    }
    if (!queue.empty()) g_refresh.store(true);
}

/* --- Gifts while the game runs ---
 * At boot the gifts go into pp.dat before the game reads it (friends_claim_gifts_at_boot). Afterwards the
 * worker asks the server once a minute; new gifts are added on the main thread, between frames, straight to
 * the coins and gems of the live PlayerInfo (friends_tick_live_gifts), and the game saves them itself. */
constexpr int kLiveGiftEverySeconds = 60;
const char *const kGiftsAppliedPath = "userdata/friend_gifts_applied.txt";

struct LiveGifts {
    std::vector<std::string> ids; /* gifts not yet added */
    long long coins = 0, gems = 0;
    std::vector<std::string> names;
};
LiveGifts &g_live_gifts = *new LiveGifts;               /* under g_lock: waiting for the main thread */
std::vector<std::string> &g_gift_acks = *new std::vector<std::string>; /* under g_lock: added, to acknowledge */
std::mutex &g_applied_lock = *new std::mutex;           /* userdata/friend_gifts_applied.txt */

std::string read_applied() {
    const std::vector<std::uint8_t> b = host_files::read(kGiftsAppliedPath);
    return std::string(b.begin(), b.end());
}

void write_applied(const std::string &s) {
    host_files::write(kGiftsAppliedPath, std::vector<std::uint8_t>(s.begin(), s.end()));
}

void poll_live_gifts() {
    if (!linked(pvz2_cloud_config())) return;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_live_gifts.ids.empty()) return; /* the last batch is not in yet */
    }
    std::string out;
    if (friends_http("GET", "gifts", "", out) != 200 || out.empty()) return;
    std::string applied;
    {
        std::lock_guard<std::mutex> lk(g_applied_lock);
        applied = read_applied();
    }
    LiveGifts fresh;
    std::vector<std::string> already; /* in the save, but the server was never told */
    for (const std::string &line : split(out, '\n')) {
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 4 || f[0].empty() || !safe_token(f[0].c_str())) continue;
        if (applied.find(f[0] + "\n") != std::string::npos) {
            already.push_back(f[0]);
            continue;
        }
        fresh.ids.push_back(f[0]);
        fresh.coins += std::max(0L, std::atol(f[2].c_str()));
        fresh.gems += std::max(0L, std::atol(f[3].c_str()));
        if (std::find(fresh.names.begin(), fresh.names.end(), f[1]) == fresh.names.end()) fresh.names.push_back(f[1]);
    }
    std::lock_guard<std::mutex> lk(g_lock);
    g_gift_acks.insert(g_gift_acks.end(), already.begin(), already.end());
    if (!fresh.ids.empty()) {
        pvz2_log("[friends] %zu new gift(s) while playing: %lld coins, %lld gems", fresh.ids.size(), fresh.coins,
                 fresh.gems);
        g_live_gifts = std::move(fresh);
    }
}

void send_gift_acks() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        ids.swap(g_gift_acks);
    }
    if (ids.empty()) return;
    std::string body = "{\"ids\":[";
    for (std::size_t i = 0; i < ids.size(); ++i) body += (i ? ",\"" : "\"") + ids[i] + "\"";
    body += "]}";
    std::string out;
    if (friends_http("POST", "ack", body, out) != 200) {
        std::lock_guard<std::mutex> lk(g_lock); /* try again next round; they stay in the applied list */
        g_gift_acks.insert(g_gift_acks.end(), ids.begin(), ids.end());
        return;
    }
    std::lock_guard<std::mutex> lk(g_applied_lock); /* the server forgot them: so can we */
    std::string applied = read_applied();
    for (const std::string &id : ids) {
        const auto at = applied.find(id + "\n");
        if (at != std::string::npos) applied.erase(at, id.size() + 1);
    }
    write_applied(applied);
}

void process_queued_actions() {
    std::vector<FriendAction> acts;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        acts.swap(g_action_queue);
    }
    if (acts.empty()) return;
    const bool is_spanish = pvz2_config() && std::strncmp(pvz2_config()->user_locale, "es", 2) == 0;
    for (const FriendAction &a : acts) {
        std::string out;
        const char *route = a.kind == ActionKind::Accept ? "accept"
                          : a.kind == ActionKind::Reject ? "reject"
                                                         : "remove";
        const std::string body = (a.kind == ActionKind::Remove)
            ? ("{\"id\":\"" + a.id + "\"}")
            : (a.kind == ActionKind::Accept && !a.invite_code.empty()
                   ? ("{\"from\":\"" + a.id + "\",\"code\":\"" + a.invite_code + "\"}")
                   : ("{\"from\":\"" + a.id + "\"}"));
        const int status = friends_http("POST", route, body, out);
        pvz2_log("[friends] %s %s: HTTP %d", route, a.id.c_str(), status);
        std::lock_guard<std::mutex> lk(g_lock);
        g_local_invites.erase(std::remove_if(g_local_invites.begin(), g_local_invites.end(),
                                             [&](const Friend &f) { return f.id == a.id; }),
                              g_local_invites.end());
        g_friends.erase(std::remove_if(g_friends.begin(), g_friends.end(),
                                       [&](const Friend &f) {
                                           if (a.kind == ActionKind::Remove) return f.id == a.id && !f.is_request;
                                           return f.id == a.id && f.is_request;
                                       }),
                        g_friends.end());
        if (status == 200) {
            char msg[192];
            const char *who = a.name.empty()
                ? L6("your friend", "tu amigo", "ton ami", "deinen Freund", "seu amigo", "il tuo amico")
                : a.name.c_str();
            if (a.kind == ActionKind::Accept) {
                std::snprintf(msg, sizeof(msg),
                              L6("Now friends with %s!", "¡Ahora eres amigo de %s!",
                                 "Tu es maintenant ami avec %s !", "Jetzt mit %s befreundet!",
                                 "Agora você é amigo de %s!", "Ora sei amico di %s!"),
                              who);
            } else if (a.kind == ActionKind::Reject) {
                std::snprintf(msg, sizeof(msg), "%s",
                              L6("Invite rejected", "Invitación rechazada",
                                 "Invitation refusée", "Einladung abgelehnt",
                                 "Convite recusado", "Invito rifiutato"));
            } else {
                std::snprintf(msg, sizeof(msg),
                              L6("Removed %s from friends", "%s eliminado de amigos",
                                 "%s retiré de tes amis", "%s aus Freunden entfernt",
                                 "%s removido dos amigos", "%s rimosso dagli amici"),
                              who);
            }
            g_pending_toasts.push_back(msg);
        }
    }
    g_refresh.store(true);
}

void worker_main() {
    auto last = std::chrono::steady_clock::now() - std::chrono::seconds(kStatusEverySeconds);
    /* The first look comes a minute in: anything older was taken at boot. */
    auto last_gifts = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        process_queued_actions();
        send_queued_gifts();
        send_gift_acks();
        const auto now = std::chrono::steady_clock::now();
        if (g_refresh.exchange(false) || now - last >= std::chrono::seconds(kStatusEverySeconds)) {
            sync_once();
            last = std::chrono::steady_clock::now();
        }
        if (now - last_gifts >= std::chrono::seconds(kLiveGiftEverySeconds)) {
            poll_live_gifts();
            last_gifts = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

/* =========================================================================
 * Local invite micro-listener (127.0.0.1:47321) + pvz2native:// URI helpers
 * ========================================================================= */
std::string url_decode(const std::string &s) {
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
            out.push_back(s[i]);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool valid_discord_id(const std::string &s) {
    if (s.size() < 15 || s.size() > 22) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

bool valid_invite_code(const std::string &s) {
    if (s.size() != 16) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool parse_invite_uri(const char *uri, std::string &out_from, std::string &out_name, std::string &out_code) {
    if (!uri) return false;
    std::string s(uri);
    const std::size_t q = s.find('?');
    if (q == std::string::npos) return false;
    out_from.clear();
    out_name.clear();
    out_code.clear();
    for (const std::string &kv : split(s.substr(q + 1), '&')) {
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq);
        const std::string v = url_decode(kv.substr(eq + 1));
        if (k == "from") out_from = v;
        else if (k == "n") out_name = v.substr(0, 40);
        else if (k == "c") out_code = v;
    }
    return valid_discord_id(out_from) && valid_invite_code(out_code);
}

std::string json_str_field(const std::string &body, const char *key) {
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = body.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    std::string out;
    for (std::size_t i = pos + 1; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            out.push_back(body[++i]);
        } else if (body[i] == '"') {
            break;
        } else {
            out.push_back(body[i]);
        }
    }
    return out;
}

std::string allowed_invite_origin() {
    const Pvz2CloudConfig *cloud = pvz2_cloud_config();
    std::string base = (cloud && cloud->nas_server_url[0]) ? cloud->nas_server_url : PVZ2_DEFAULT_NAS_SERVER_URL;
    const std::size_t scheme = base.find("://");
    if (scheme == std::string::npos) return {};
    const std::size_t slash = base.find('/', scheme + 3);
    return (slash == std::string::npos) ? base : base.substr(0, slash);
}

std::string extract_header_value(const std::string &req, const char *name) {
    const std::size_t hdr_end = req.find("\r\n\r\n");
    const std::string hdrs = (hdr_end == std::string::npos) ? req : req.substr(0, hdr_end);
    const std::size_t nlen = std::strlen(name);
    std::size_t pos = 0;
    while (pos < hdrs.size()) {
        std::size_t nl = hdrs.find("\r\n", pos);
        if (nl == std::string::npos) nl = hdrs.size();
        const std::string line = hdrs.substr(pos, nl - pos);
        if (line.size() > nlen + 1 && line[nlen] == ':') {
            bool match = true;
            for (std::size_t i = 0; i < nlen; ++i) {
                if (std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)name[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::size_t v = nlen + 1;
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
                return line.substr(v);
            }
        }
        pos = nl + 2;
    }
    return {};
}

void queue_local_invite(const std::string &from_id, const std::string &name, const std::string &code, bool open_panel) {
    if (!valid_discord_id(from_id) || !valid_invite_code(code)) return;
    const Pvz2CloudConfig *cloud = pvz2_cloud_config();
    if (cloud && cloud->auth_discord_id[0] && from_id == cloud->auth_discord_id) return;
    Friend fr;
    fr.id = from_id;
    fr.name = name.empty() ? "Jugador" : name;
    fr.is_request = true;
    fr.invite_code = code;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        bool exists = false;
        for (Friend &f : g_local_invites)
            if (f.id == from_id) { f.invite_code = code; exists = true; break; }
        if (!exists) g_local_invites.push_back(fr);
        bool in_list = false;
        for (Friend &f : g_friends)
            if (f.id == from_id) { f.invite_code = code; in_list = true; break; }
        if (!in_list) g_friends.insert(g_friends.begin(), fr);
        if (g_state == ListState::Loading || g_state == ListState::Error) g_state = ListState::Ready;
        g_seen_requests.insert(from_id);
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      L6("Friend invite from %s", "Invitación de amistad de %s",
                         "Invitation d'ami de %s", "Freundeseinladung von %s",
                         "Convite de amizade de %s", "Invito di amicizia da %s"),
                      fr.name.c_str());
        g_pending_toasts.push_back(msg);
    }
    g_refresh.store(true);
    if (open_panel) friends_screen_request_open();
}

void invite_listener_main() {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) return;
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#else
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)kInviteLocalPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 4) != 0) {
#ifdef _WIN32
        closesocket(srv);
#else
        close(srv);
#endif
        return;
    }
    pvz2_log("[friends] local invite listener active on 127.0.0.1:%d", kInviteLocalPort);
    while (!g_stop.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        timeval tv{0, 250000};
        if (select((int)srv + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        auto cli = accept(srv, nullptr, nullptr);
#ifdef _WIN32
        if (cli == INVALID_SOCKET) continue;
        DWORD rcv_to = 1000;
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv_to, sizeof(rcv_to));
#else
        if (cli < 0) continue;
        timeval rcv_to{1, 0};
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
#endif
        char buf[4096];
        int n = (int)recv(cli, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            std::string req(buf, (std::size_t)n);
            const std::string origin = extract_header_value(req, "Origin");
            const std::string allowed = allowed_invite_origin();
            /* Reject any browser request from an untrusted website (prevents cross-site CSRF on localhost). */
            if (!origin.empty() && (allowed.empty() || origin != allowed)) {
                const std::string deny = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                send(cli, deny.c_str(), (int)deny.size(), 0);
#ifdef _WIN32
                closesocket(cli);
#else
                close(cli);
#endif
                continue;
            }
            const std::string cors =
                (!origin.empty() ? ("Access-Control-Allow-Origin: " + allowed + "\r\nVary: Origin\r\n") : "") +
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Access-Control-Allow-Private-Network: true\r\n"
                "Connection: close\r\n";
            std::string resp;
            if (req.rfind("OPTIONS ", 0) == 0) {
                resp = "HTTP/1.1 204 No Content\r\n" + cors + "\r\n";
            } else if (req.rfind("GET /ping", 0) == 0) {
                const bool lnk = linked(pvz2_cloud_config());
                const std::string body = std::string("{\"ok\":true,\"linked\":") + (lnk ? "true}" : "false}");
                resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" + cors +
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            } else if (req.rfind("POST /invite", 0) == 0) {
                const std::size_t sep = req.find("\r\n\r\n");
                const std::string body = (sep != std::string::npos) ? req.substr(sep + 4) : "";
                const std::string from_id = json_str_field(body, "from");
                const std::string name = json_str_field(body, "name");
                const std::string code = json_str_field(body, "code");
                const std::string action = json_str_field(body, "action");
                if (!valid_discord_id(from_id) || !valid_invite_code(code)) {
                    const std::string out = "{\"ok\":false,\"error\":\"bad_invite\"}";
                    resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n" + cors +
                           "Content-Length: " + std::to_string(out.size()) + "\r\n\r\n" + out;
                } else if (action == "open") {
                    queue_local_invite(from_id, name, code, true);
                    const std::string out = "{\"ok\":true}";
                    resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" + cors +
                           "Content-Length: " + std::to_string(out.size()) + "\r\n\r\n" + out;
                } else if (!linked(pvz2_cloud_config())) {
                    const std::string out = "{\"ok\":false,\"error\":\"not_linked\"}";
                    resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n" + cors +
                           "Content-Length: " + std::to_string(out.size()) + "\r\n\r\n" + out;
                } else {
                    {
                        std::lock_guard<std::mutex> lk(g_lock);
                        g_action_queue.push_back(
                            {from_id, name, action == "accept" ? ActionKind::Accept : ActionKind::Reject, code});
                    }
                    const std::string out = "{\"ok\":true}";
                    resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" + cors +
                           "Content-Length: " + std::to_string(out.size()) + "\r\n\r\n" + out;
                }
            } else {
                resp = "HTTP/1.1 404 Not Found\r\n" + cors + "Content-Length: 0\r\n\r\n";
            }
            send(cli, resp.c_str(), (int)resp.size(), 0);
        }
#ifdef _WIN32
        closesocket(cli);
#else
        close(cli);
#endif
    }
#ifdef _WIN32
    closesocket(srv);
#else
    close(srv);
#endif
}

/* What the gifts taken at boot said, shown as a notification once the game is on screen. */
std::string &g_gift_notice = *new std::string;

/* Adds coins and gems to the active profile's save (userdata/pp.dat, PlayerInfo `c` and `g`). The snapshot
 * files are other profiles and are not touched. Same safeguards as save_dates.cpp: a file not fully
 * understood is left alone, and the result must parse back with the new values before it replaces it. */
bool add_to_save(long long coins, long long gems) {
    const char *path = "userdata/pp.dat";
    const std::vector<std::uint8_t> data = host_files::read(path);
    RtonNode root;
    if (data.empty() || !rton_parse(data, root)) return false;
    auto info_of = [](const RtonNode &r) -> const RtonNode * {
        const RtonNode *objects = r.get("objects");
        if (objects == nullptr || objects->kind != RtonNode::Arr || objects->a.empty()) return nullptr;
        return objects->a[0].get("objdata");
    };
    const RtonNode *info = info_of(root);
    const RtonNode *c = info ? info->get("c") : nullptr;
    const RtonNode *g = info ? info->get("g") : nullptr;
    if (c == nullptr || g == nullptr || c->kind != RtonNode::Int || g->kind != RtonNode::Int) return false;
    const long long want_c = c->i + coins, want_g = g->i + gems;
    std::vector<RtonEdit> edits;
    edits.push_back({c->begin, c->end, rton_encode_int(c->tag, (std::uint64_t)want_c)});
    edits.push_back({g->begin, g->end, rton_encode_int(g->tag, (std::uint64_t)want_g)});
    const std::vector<std::uint8_t> out = rton_apply_edits(data, std::move(edits));
    RtonNode check;
    const RtonNode *ci = nullptr;
    if (!rton_parse(out, check) || (ci = info_of(check)) == nullptr || !ci->get("c") || !ci->get("g") ||
        ci->get("c")->i != want_c || ci->get("g")->i != want_g)
        return false;
    return host_files::write_atomic(path, out);
}

/* =========================================================================
 * Text
 * ========================================================================= */
/* "beach3" -> "Gran Ola Playa 3" */
std::string level_label(const std::string &level) {
    static const struct { const char *key, *n[6]; } kWorlds[] = {
        {"tutorial", {"Tutorial", "Tutorial", "Didacticiel", "Tutorial", "Tutorial", "Tutorial"}},
        {"modern", {"Modern Day", "Día moderno", "Temps modernes", "Neuzeit", "Tempos Modernos", "Tempi moderni"}},
        {"egypt", {"Ancient Egypt", "Antiguo Egipto", "Égypte antique", "Altes Ägypten", "Egito Antigo", "Antico Egitto"}},
        {"pirate", {"Pirate Seas", "Mares piratas", "Mers pirates", "Piratenseen", "Mares Piratas", "Mari dei pirati"}},
        {"cowboy", {"Wild West", "Salvaje Oeste", "Far West", "Wilder Westen", "Velho Oeste", "Selvaggio West"}},
        {"future", {"Far Future", "Futuro lejano", "Futur lointain", "Ferne Zukunft", "Futuro Distante", "Futuro lontano"}},
        {"dark", {"Dark Ages", "Edad Oscura", "Âge des ténèbres", "Finsteres Mittelalter", "Idade das Trevas", "Medioevo"}},
        {"beach", {"Big Wave Beach", "Gran Ola Playa", "Plage des déferlantes", "Strand der großen Welle", "Praia da Boa Onda", "Spiaggia delle grandi onde"}},
        {"iceage", {"Frostbite Caves", "Cuevas congeladas", "Grottes glaciaires", "Frostbeulenhöhlen", "Cavernas Congeladas", "Grotte congelate"}},
        {"lostcity", {"Lost City", "Ciudad perdida", "Cité perdue", "Verlorene Stadt", "Cidade Perdida", "Città perduta"}},
        {"eighties", {"Neon Mixtape Tour", "Neon Mixtape Tour", "Tournée Fluo Mixtape", "Neon-Mixtape-Tour", "Turnê Neon Mixtape", "Tour Neon Mixtape"}},
        {"dino", {"Jurassic Marsh", "Jurásico pantanoso", "Marais jurassique", "Juramoor", "Pântano Jurássico", "Palude giurassica"}},
    };
    std::string lv = level;
    for (char &ch : lv) ch = (char)std::tolower((unsigned char)ch);
    std::size_t digits = lv.size();
    while (digits > 0 && std::isdigit((unsigned char)lv[digits - 1])) --digits;
    const std::string world = lv.substr(0, digits), number = lv.substr(digits);
    const int li = lang_idx();
    for (const auto &w : kWorlds)
        if (world == w.key) return std::string(w.n[li]) + (number.empty() ? "" : " " + number);
    return level;
}

std::string ago_label(long s) {
    char buf[64];
    if (s < 3600)
        std::snprintf(buf, sizeof(buf),
                      L6("Seen %ld min ago", "Visto hace %ld min", "Vu il y a %ld min",
                         "Vor %ld Min. gesehen", "Visto há %ld min", "Visto %ld min fa"),
                      std::max(1L, s / 60));
    else if (s < 86400)
        std::snprintf(buf, sizeof(buf),
                      L6("Seen %ld h ago", "Visto hace %ld h", "Vu il y a %ld h",
                         "Vor %ld Std. gesehen", "Visto há %ld h", "Visto %ld h fa"),
                      s / 3600);
    else
        std::snprintf(buf, sizeof(buf),
                      L6("Seen %ld days ago", "Visto hace %ld días", "Vu il y a %ld j",
                         "Vor %ld Tagen gesehen", "Visto há %ld dias", "Visto %ld giorni fa"),
                      s / 86400);
    return buf;
}

/* =========================================================================
 * GL resources (main thread)
 * ========================================================================= */
struct FTex {
    GLuint id = 0;
    int w = 0, h = 0;
};

void *load_raw_from_rc(int rc_id, int *out_size) {
#ifdef _WIN32
    HMODULE self = GetModuleHandle(NULL);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(rc_id), RT_RCDATA);
    if (!res) return nullptr;
    HGLOBAL hg = LoadResource(self, res);
    if (!hg) return nullptr;
    DWORD bytes = SizeofResource(self, res);
    const void *ptr = LockResource(hg);
    if (!ptr || !bytes) return nullptr;
    void *buf = SDL_malloc(bytes);
    SDL_memcpy(buf, ptr, bytes);
    if (out_size) *out_size = (int)bytes;
    return buf;
#else
    return pvz2_load_asset_bytes(rc_id, out_size);
#endif
}

enum class BtnTone { None, Green, Grey, Red, GoldRing };

void recolor_button_surface(SDL_Surface *surf, BtnTone tone) {
    if (!surf || tone == BtnTone::None) return;
    if (tone == BtnTone::GoldRing) {
        /* Punch out the inner blue swirl of achievement_locked.bmp to leave only the golden medallion ring. */
        const float cx = (surf->w - 1) * 0.5f;
        const float cy = (surf->h - 1) * 0.5f;
        const float r_in = surf->w * (135.5f / 326.0f);
        const float r_out = surf->w * (138.0f / 326.0f);
        auto *rows = static_cast<std::uint8_t *>(surf->pixels);
        for (int y = 0; y < surf->h; ++y) {
            std::uint8_t *px = rows + y * surf->pitch;
            for (int x = 0; x < surf->w; ++x, px += 4) {
                if (px[3] == 0) continue;
                const float d = std::hypot((float)x - cx, (float)y - cy);
                if (d <= r_in) {
                    px[3] = 0;
                } else if (d < r_out) {
                    const float t = (d - r_in) / (r_out - r_in);
                    px[3] = (std::uint8_t)(px[3] * t);
                }
            }
        }
        return;
    }
    const auto hsv_to_rgb = [](float h, float s, float v, float &r, float &g, float &b) {
        const float c = v * s;
        const float hp = h * 6.0f;
        const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
        const float m = v - c;
        float r1 = 0, g1 = 0, b1 = 0;
        if      (hp < 1.0f) { r1 = c; g1 = x; }
        else if (hp < 2.0f) { r1 = x; g1 = c; }
        else if (hp < 3.0f) { g1 = c; b1 = x; }
        else if (hp < 4.0f) { g1 = x; b1 = c; }
        else if (hp < 5.0f) { r1 = x; b1 = c; }
        else                { r1 = c; b1 = x; }
        r = r1 + m; g = g1 + m; b = b1 + m;
    };
    auto *rows = static_cast<std::uint8_t *>(surf->pixels);
    for (int y = 0; y < surf->h; ++y) {
        std::uint8_t *px = rows + y * surf->pitch;
        for (int x = 0; x < surf->w; ++x, px += 4) {
            if (px[3] == 0) continue;
            const float rf = px[0] / 255.0f, gf = px[1] / 255.0f, bf = px[2] / 255.0f;
            const float mx = std::max(rf, std::max(gf, bf));
            const float mn = std::min(rf, std::min(gf, bf));
            const float sat = (mx > 0.0f) ? (mx - mn) / mx : 0.0f;
            /* Leave neutral outline, white rim, and white specular highlights untouched. */
            if (sat < 0.12f) continue;
            float nr = rf, ng = gf, nb = bf;
            if (tone == BtnTone::Green) {
                hsv_to_rgb(0.285f, std::min(1.0f, sat * 1.05f), std::min(1.0f, mx * 0.96f), nr, ng, nb);
            } else if (tone == BtnTone::Grey) {
                hsv_to_rgb(0.60f, 0.08f, mx * 0.78f, nr, ng, nb);
            } else if (tone == BtnTone::Red) {
                hsv_to_rgb(0.01f, std::min(1.0f, sat * 1.05f), std::min(1.0f, mx * 0.95f), nr, ng, nb);
            }
            px[0] = (std::uint8_t)std::min(255.0f, std::max(0.0f, nr * 255.0f));
            px[1] = (std::uint8_t)std::min(255.0f, std::max(0.0f, ng * 255.0f));
            px[2] = (std::uint8_t)std::min(255.0f, std::max(0.0f, nb * 255.0f));
        }
    }
}

FTex load_bmp_from_rc(int rc_id, BtnTone tone = BtnTone::None) {
    FTex t;
    int bytes = 0;
    void *buf = load_raw_from_rc(rc_id, &bytes);
    if (!buf || bytes <= 0) {
        if (buf) SDL_free(buf);
        return t;
    }
    SDL_RWops *rw = SDL_RWFromConstMem(buf, bytes);
    SDL_Surface *bmp = rw ? SDL_LoadBMP_RW(rw, 1) : nullptr;
    SDL_free(buf);
    if (!bmp) return t;
    SDL_Surface *surf = SDL_ConvertSurfaceFormat(bmp, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(bmp);
    if (!surf) return t;
    apply_magenta_colorkey(surf);
    recolor_button_surface(surf, tone);
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, surf->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    t.w = surf->w;
    t.h = surf->h;
    SDL_FreeSurface(surf);
    return t;
}

struct Font {
    stbtt_fontinfo info{};
    void *data = nullptr;
    GLuint tex = 0;
    static const int kW = 2048, kH = 2048;
    std::vector<unsigned char> atlas;
    stbtt_bakedchar big[224]{};   /* 92 px */
    stbtt_bakedchar small[224]{}; /* 48 px */
    bool ok = false;

    void load(int rc_id) {
        int sz = 0;
        data = load_raw_from_rc(rc_id, &sz);
        if (!data || !stbtt_InitFont(&info, (unsigned char *)data, 0)) return;
        atlas.assign((std::size_t)kW * kH, 0);
        stbtt_BakeFontBitmap((unsigned char *)data, 0, 92.0f, atlas.data(), kW, kH / 2, 32, 224, big);
        stbtt_BakeFontBitmap((unsigned char *)data, 0, 48.0f, atlas.data() + (std::size_t)kW * (kH / 2), kW, kH / 2,
                             32, 224, small);
        for (auto &g : small) {
            g.y0 += (short)(kH / 2);
            g.y1 += (short)(kH / 2);
        }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, kW, kH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas.data());
        atlas.clear();
        atlas.shrink_to_fit();
        ok = true;
    }
    void unload() {
        if (tex) glDeleteTextures(1, &tex);
        tex = 0;
        if (data) SDL_free(data);
        data = nullptr;
        ok = false;
    }
};

bool g_loaded = false;
Font g_font;
FTex g_tex_menu, g_tex_item;
FTex g_tex_pfp_frame, g_tex_pfp_placeholder;
FTex g_tex_btn_green_up, g_tex_btn_green_down;
FTex g_tex_btn_orange_up, g_tex_btn_orange_down;
FTex g_tex_btn_grey, g_tex_btn_red;
FTex g_tex_hud_btn;
std::atomic<Uint64> g_hud_seen_ms{0};
bool g_hud_btn_down = false;
float g_hud_brightness = 1.0f; /* how lit the game's HUD bar is (fades / loading dim) */

unsigned utf8_next(const char **p) {
    const unsigned char c = (unsigned char)**p;
    if (!c) return 0;
    ++*p;
    if (c < 0x80) return c;
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    unsigned cp = c & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
        const unsigned char cc = (unsigned char)**p;
        if ((cc & 0xC0) != 0x80) return '?';
        ++*p;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

float text_width(const char *s, bool big, float scale) {
    if (!g_font.ok) return 0;
    const stbtt_bakedchar *bc = big ? g_font.big : g_font.small;
    float w = 0;
    unsigned cp;
    while ((cp = utf8_next(&s)) != 0) w += (cp < 32 || cp > 255) ? 8.0f * scale : bc[cp - 32].xadvance * scale;
    return w;
}

/* Text with a drop shadow; glyphs outside [clip_top, clip_bot] are skipped. */
void draw_text(const char *s, bool big, float x, float y, float scale, float r, float g, float b,
               float clip_top = -1e9f, float clip_bot = 1e9f) {
    if (!g_font.ok || !s) return;
    const stbtt_bakedchar *bc = big ? g_font.big : g_font.small;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_font.tex);
    for (int pass = 0; pass < 2; ++pass) {
        const float off = pass == 0 ? 2.0f * scale * (big ? 2.0f : 1.0f) : 0.0f;
        const float a = pass == 0 ? 0.35f : 1.0f;
        float px = x + off;
        const char *p = s;
        unsigned cp;
        glBegin(GL_QUADS);
        while ((cp = utf8_next(&p)) != 0) {
            if (cp < 32 || cp > 255) {
                px += 8.0f * scale;
                continue;
            }
            const stbtt_bakedchar *cd = &bc[cp - 32];
            const float x0 = px + cd->xoff * scale, y0 = y + off + cd->yoff * scale;
            const float x1 = x0 + (cd->x1 - cd->x0) * scale, y1 = y0 + (cd->y1 - cd->y0) * scale;
            if (y0 >= clip_top && y1 <= clip_bot) {
                if (pass == 0) glColor4f(0, 0, 0, a);
                else glColor4f(r, g, b, a);
                const float fw = (float)Font::kW, fh = (float)Font::kH;
                glTexCoord2f(cd->x0 / fw, cd->y0 / fh); glVertex2f(x0, y0);
                glTexCoord2f(cd->x1 / fw, cd->y0 / fh); glVertex2f(x1, y0);
                glTexCoord2f(cd->x1 / fw, cd->y1 / fh); glVertex2f(x1, y1);
                glTexCoord2f(cd->x0 / fw, cd->y1 / fh); glVertex2f(x0, y1);
            }
            px += cd->xadvance * scale;
        }
        glEnd();
    }
}

/* Cut to fit `max_w`, with "..." when it does not. */
std::string fit(const std::string &s, bool big, float scale, float max_w) {
    if (text_width(s.c_str(), big, scale) <= max_w) return s;
    std::string cut = s;
    while (!cut.empty()) {
        do cut.pop_back();
        while (!cut.empty() && ((unsigned char)cut.back() & 0xC0) == 0x80);
        if (text_width((cut + "...").c_str(), big, scale) <= max_w) return cut + "...";
    }
    return "...";
}

void draw_tex(const FTex &t, float x, float y, float w, float h, float a = 1.0f) {
    if (!t.id) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glColor4f(1, 1, 1, a);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x, y);
    glTexCoord2f(1, 0); glVertex2f(x + w, y);
    glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
    glTexCoord2f(0, 1); glVertex2f(x, y + h);
    glEnd();
}

/* Horizontal 3-slice for PvZ2 glossy buttons: preserves rounded caps at any width. */
void draw_button_tex(const FTex &t, float x, float y, float w, float h, float src_cap = 36.0f) {
    if (!t.id || t.w <= 0 || t.h <= 0 || w <= 0.0f || h <= 0.0f) return;
    const float bu = std::min(src_cap / (float)t.w, 0.5f);
    const float cap = std::min(src_cap * (h / (float)t.h), w * 0.5f);
    const float xs[4] = {x, x + cap, x + w - cap, x + w};
    const float us[4] = {0.0f, bu, 1.0f - bu, 1.0f};
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    for (int col = 0; col < 3; ++col) {
        if (xs[col + 1] <= xs[col]) continue;
        glTexCoord2f(us[col],     0.0f); glVertex2f(xs[col],     y);
        glTexCoord2f(us[col + 1], 0.0f); glVertex2f(xs[col + 1], y);
        glTexCoord2f(us[col + 1], 1.0f); glVertex2f(xs[col + 1], y + h);
        glTexCoord2f(us[col],     1.0f); glVertex2f(xs[col],     y + h);
    }
    glEnd();
}

void fill_round_rect(float x, float y, float w, float h, float r, float cr, float cg, float cb, float ca) {
    r = std::min(r, std::min(w, h) * 0.5f);
    glDisable(GL_TEXTURE_2D);
    glColor4f(cr, cg, cb, ca);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x + w * 0.5f, y + h * 0.5f);
    const float cx[4] = {x + w - r, x + w - r, x + r, x + r};
    const float cy[4] = {y + r, y + h - r, y + h - r, y + r};
    const float a0[4] = {-0.5f, 0.0f, 0.5f, 1.0f};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i <= 8; ++i) {
            const float a = (a0[c] + 0.5f * (float)i / 8) * 3.14159265f;
            glVertex2f(cx[c] + r * std::cos(a), cy[c] + r * std::sin(a));
        }
    glVertex2f(cx[0], y);
    glEnd();
    glEnable(GL_TEXTURE_2D);
}

void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;
    g_tex_menu = load_bmp_from_rc(pvz2native::achievements::kRcAchievementMenu);
    g_tex_item = load_bmp_from_rc(pvz2native::achievements::kRcAchievementItem);
    g_tex_pfp_placeholder = load_bmp_from_rc(pvz2native::achievements::kRcAchievementLocked);
    g_tex_pfp_frame       = load_bmp_from_rc(pvz2native::achievements::kRcAchievementLocked, BtnTone::GoldRing);
    g_tex_btn_green_up    = load_bmp_from_rc(220, BtnTone::Green); /* RC_CFG_BTN_SAVE_UP */
    g_tex_btn_green_down  = load_bmp_from_rc(221, BtnTone::Green); /* RC_CFG_BTN_SAVE_DOWN */
    g_tex_btn_orange_up   = load_bmp_from_rc(229, BtnTone::None);  /* RC_CFG_BTN_CANCEL_UP */
    g_tex_btn_orange_down = load_bmp_from_rc(230, BtnTone::None);  /* RC_CFG_BTN_CANCEL_DOWN */
    g_tex_btn_grey        = load_bmp_from_rc(229, BtnTone::Grey);
    g_tex_btn_red         = load_bmp_from_rc(229, BtnTone::Red);
    g_font.load(213); /* RC_CFG_FONT */
}

/* =========================================================================
 * Screen
 * ========================================================================= */
std::atomic<bool> g_open_requested{false};
bool g_open = false;
float g_scroll = 0;
bool g_dragging = false;
int g_drag_y = 0;
float g_drag_scroll_start = 0;
Uint32 g_copied_ms = 0; /* "Copied!" on the invite button until +2 s */
bool g_invite_down = false;
std::string g_gift_down_id;
std::string g_req_down_id;
bool g_req_down_accept = false;
std::string g_remove_down_id;
std::string g_remove_confirm_id;
Uint32 g_remove_confirm_ms = 0;

/* Layout, in the panel's own 1080p units (the achievements panel art). */
constexpr float kRefH = 1080.0f;
constexpr float kListX = 187.5f, kListY = 343.5f, kListW = 1040.0f, kListH = 654.0f;
constexpr float kPad = 20.0f, kItemH = 161.0f, kItemStep = 181.0f;
constexpr float kButtonY = 236.0f, kButtonW = 380.0f, kButtonH = 84.0f;

struct Layout {
    float s, mx, my, mw, mh, lx, ly, lw, lh, bx, by, bw, bh;
};

Layout layout(float win_w, float win_h) {
    Layout L{};
    L.s = win_h / kRefH;
    const float pw = g_tex_menu.id ? (float)g_tex_menu.w : 1415.0f;
    const float ph = g_tex_menu.id ? (float)g_tex_menu.h : 1047.0f;
    if (pw * L.s > win_w) L.s = win_w / pw;
    L.mw = pw * L.s;
    L.mh = ph * L.s;
    L.mx = (win_w - L.mw) * 0.5f;
    L.my = (win_h - L.mh) * 0.5f;
    L.lx = L.mx + kListX * L.s;
    L.ly = L.my + kListY * L.s;
    L.lw = kListW * L.s;
    L.lh = kListH * L.s;
    L.bw = kButtonW * L.s;
    L.bh = kButtonH * L.s;
    L.bx = L.mx + (L.mw - L.bw) * 0.5f;
    L.by = L.my + kButtonY * L.s;
    return L;
}

float max_scroll(const Layout &L, int rows) {
    const float content = rows > 0 ? (kPad * 2 + (rows - 1) * kItemStep + kItemH) * L.s : 0.0f;
    return std::max(0.0f, content - L.lh);
}

/* Row i's box, gift + remove buttons, and (for incoming requests) Accept / Reject buttons. */
struct RowRect {
    float ix, iy, iw, ih, gx, gy, gw, gh;
    float dx, dy, dw, dh;
    float ax, ay, aw, ah, rx, ry, rw, rh;
};

RowRect row_rect(const Layout &L, std::size_t i) {
    RowRect r{};
    const float s = L.s;
    r.ih = kItemH * s;
    r.iw = g_tex_item.id ? g_tex_item.w * s : L.lw - 2 * kPad * s;
    r.ix = L.lx + (L.lw - r.iw) * 0.5f;
    r.iy = L.ly + (kPad + i * kItemStep) * s - g_scroll;
    r.dw = 58 * s;
    r.dh = 72 * s;
    r.dx = r.ix + r.iw - r.dw - 24 * s;
    r.dy = r.iy + (r.ih - r.dh) * 0.5f;
    r.gw = 210 * s;
    r.gh = 72 * s;
    r.gx = r.dx - r.gw - 10 * s;
    r.gy = r.dy;
    r.aw = 165 * s;
    r.ah = 70 * s;
    r.rw = 165 * s;
    r.rh = 70 * s;
    r.rx = r.ix + r.iw - r.rw - 28 * s;
    r.ry = r.iy + (r.ih - r.rh) * 0.5f;
    r.ax = r.rx - r.aw - 12 * s;
    r.ay = r.ry;
    return r;
}

/* What the gift button of a friend shows: 0 = can send, 1 = sending, 2 = sent today, 3 = failed. */
int gift_button_state(const Friend &fr) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (std::find(g_gift_queue.begin(), g_gift_queue.end(), fr.id) != g_gift_queue.end()) return 1;
    auto it = g_gift_result.find(fr.id);
    if (it != g_gift_result.end()) {
        if (it->second == -1) return 1;
        if (it->second == 200 || it->second == 429) return 2;
        return 3;
    }
    return fr.can_gift ? 0 : 2;
}

void upload_pictures() {
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto &kv : g_pics) {
        Picture &p = kv.second;
        if (!p.ready || p.tex != 0) continue;
        glGenTextures(1, &p.tex);
        glBindTexture(GL_TEXTURE_2D, p.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kPicSide, kPicSide, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.rgba.data());
        p.rgba.clear();
        p.rgba.shrink_to_fit();
    }
}

void draw_screen(float win_w, float win_h) {
    ensure_loaded();
    upload_pictures();
    const Layout L = layout(win_w, win_h);
    const float s = L.s;

    glDisable(GL_TEXTURE_2D);
    glColor4f(0, 0, 0, 0.5f);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(win_w, 0); glVertex2f(win_w, win_h); glVertex2f(0, win_h);
    glEnd();
    glEnable(GL_TEXTURE_2D);

    draw_tex(g_tex_menu, L.mx, L.my + 2 * s, L.mw, L.mh, 0.35f);
    draw_tex(g_tex_menu, L.mx, L.my, L.mw, L.mh);

    const char *title = L6("Friends", "Amigos", "Amis", "Freunde", "Amigos", "Amici");
    draw_text(title, true, L.mx + (L.mw - text_width(title, true, s)) * 0.5f, L.my + 126 * s, s, 0xfd / 255.f,
              0xdc / 255.f, 0x67 / 255.f);

    /* Invite button */
    const bool copied = g_copied_ms != 0 && SDL_GetTicks() - g_copied_ms < 2000;
    const FTex &inv_tex = (g_invite_down || copied) && g_tex_btn_green_down.id ? g_tex_btn_green_down : g_tex_btn_green_up;
    if (inv_tex.id) {
        draw_button_tex(inv_tex, L.bx, L.by, L.bw, L.bh);
    } else {
        fill_round_rect(L.bx, L.by + 4 * s, L.bw, L.bh, 22 * s, 0, 0, 0, 0.35f);
        fill_round_rect(L.bx, L.by, L.bw, L.bh, 22 * s, copied ? 0.30f : 0.23f, copied ? 0.62f : 0.56f, 0.16f, 1.0f);
    }
    {
        const char *label = copied
            ? L6("Link copied!", "¡Enlace copiado!", "Lien copié !", "Link kopiert!", "Link copiado!", "Link copiato!")
            : L6("Invite a friend", "Invitar a un amigo", "Inviter un ami", "Freund einladen", "Convidar um amigo", "Invita un amico");
        const float fs = s * 0.78f;
        const float press_y = g_invite_down ? 2.0f * s : 0.0f;
        draw_text(label, false, L.bx + (L.bw - text_width(label, false, fs)) * 0.5f,
                  L.by + L.bh * 0.5f + 13 * fs + press_y, fs, 1, 1, 1);
    }

    std::vector<Friend> rows;
    ListState state;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        rows = g_friends;
        state = g_state;
    }

    const float list_top = L.ly, list_bot = L.ly + L.lh;
    auto centre_message = [&](const char *msg) {
        const float fs = s * 0.75f;
        const std::string line = fit(msg, false, fs, L.lw - 40 * s);
        draw_text(line.c_str(), false, L.lx + (L.lw - text_width(line.c_str(), false, fs)) * 0.5f,
                  list_top + L.lh * 0.4f, fs, 1, 1, 1);
    };
    if (state == ListState::NotLinked) {
        centre_message(L6("Link Discord in Cloud Save to see your friends",
                          "Vincula Discord en Guardado en la nube para ver a tus amigos",
                          "Lie Discord dans Sauvegarde Cloud pour voir tes amis",
                          "Verknüpfe Discord im Cloud-Speicher, um Freunde zu sehen",
                          "Vincule o Discord no Salvamento na Nuvem para ver seus amigos",
                          "Collega Discord nel Salvataggio Cloud per vedere i tuoi amici"));
        return;
    }
    if (rows.empty()) {
        centre_message(state == ListState::Loading
                           ? L6("Loading...", "Cargando...", "Chargement...", "Lädt...", "Carregando...", "Caricamento...")
                       : state == ListState::Error
                           ? L6("Could not reach the server", "No se pudo conectar con el servidor",
                                "Impossible de joindre le serveur", "Server nicht erreichbar",
                                "Não foi possível conectar ao servidor", "Impossibile raggiungere il server")
                           : L6("Nobody else yet. Invite a friend!", "Aún no hay nadie más. ¡Invita a un amigo!",
                                "Personne d'autre pour l'instant. Invite un ami !",
                                "Noch niemand sonst hier. Lade einen Freund ein!",
                                "Ninguém ainda. Convide um amigo!", "Ancora nessuno. Invita un amico!"));
        return;
    }

    const float ms = max_scroll(L, (int)rows.size());
    g_scroll = std::max(0.0f, std::min(g_scroll, ms));

    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)L.lx, (GLint)(win_h - list_bot), (GLsizei)L.lw, (GLsizei)L.lh);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Friend &fr = rows[i];
        const float iy = list_top + (kPad + i * kItemStep) * s - g_scroll;
        const float ih = kItemH * s;
        if (iy + ih < list_top) continue;
        if (iy > list_bot) break;
        const RowRect rr = row_rect(L, i);
        const float iw = rr.iw, ix = rr.ix;
        if (g_tex_item.id) draw_tex(g_tex_item, ix, iy, iw, g_tex_item.h * s);

        /* Picture inside the PvZ2 golden medallion frame, with an online badge when playing. */
        const float frame = 152 * s;
        const float fx = ix + 6 * s, fy = iy + (ih - frame) * 0.5f;
        const float pic = 132 * s;
        const float px = fx + (frame - pic) * 0.5f, py = fy + (frame - pic) * 0.5f;
        GLuint tex = 0;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            auto it = g_pics.find(fr.avatar);
            if (it != g_pics.end()) tex = it->second.tex;
        }
        if (tex) {
            FTex t{tex, kPicSide, kPicSide};
            draw_tex(t, px, py, pic, pic);
            if (g_tex_pfp_frame.id) draw_tex(g_tex_pfp_frame, fx, fy, frame, frame);
        } else if (g_tex_pfp_placeholder.id) {
            draw_tex(g_tex_pfp_placeholder, fx, fy, frame, frame);
        } else {
            fill_round_rect(px, py, pic, pic, pic, 0.35f, 0.35f, 0.40f, 1);
        }
        if (fr.online) {
            const float bx = fx + frame * 0.84f, by = fy + frame * 0.82f;
            const float br = 15 * s, ir = 11 * s;
            fill_round_rect(bx - br, by - br, br * 2, br * 2, br, 0.08f, 0.12f, 0.10f, 1);
            fill_round_rect(bx - ir, by - ir, ir * 2, ir * 2, ir, 0.33f, 0.86f, 0.25f, 1);
            const float hr = 3.5f * s;
            fill_round_rect(bx - ir * 0.55f, by - ir * 0.65f, hr * 2, hr * 2, hr, 0.78f, 1.0f, 0.72f, 0.85f);
        }

        const float tx = fx + frame + 22 * s;
        const float max_w = (fr.is_request ? rr.ax : rr.gx) - tx - 20 * s;

        if (fr.is_request) {
            /* Incoming friend request: Accept (green) + Reject (red) PvZ2 buttons. */
            if (rr.ay >= list_top && rr.ay + rr.ah <= list_bot) {
                const bool acc_down = (g_req_down_id == fr.id && g_req_down_accept);
                const bool rej_down = (g_req_down_id == fr.id && !g_req_down_accept);
                const FTex &at = (acc_down && g_tex_btn_green_down.id) ? g_tex_btn_green_down : g_tex_btn_green_up;
                if (at.id) draw_button_tex(at, rr.ax, rr.ay, rr.aw, rr.ah);
                else fill_round_rect(rr.ax, rr.ay, rr.aw, rr.ah, 16 * s, 0.23f, 0.56f, 0.16f, 1);
                const char *albl = L6("Accept", "Aceptar", "Accepter", "Annehmen", "Aceitar", "Accetta");
                const float bs = s * 0.54f;
                draw_text(albl, false, rr.ax + (rr.aw - text_width(albl, false, bs)) * 0.5f,
                          rr.ay + rr.ah * 0.5f + 12 * bs + (acc_down ? 2.0f * s : 0.0f), bs, 1, 1, 1);

                if (g_tex_btn_red.id) draw_button_tex(g_tex_btn_red, rr.rx, rr.ry, rr.rw, rr.rh);
                else fill_round_rect(rr.rx, rr.ry, rr.rw, rr.rh, 16 * s, 0.70f, 0.20f, 0.18f, 1);
                const char *rlbl = L6("Reject", "Rechazar", "Refuser", "Ablehnen", "Recusar", "Rifiuta");
                draw_text(rlbl, false, rr.rx + (rr.rw - text_width(rlbl, false, bs)) * 0.5f,
                          rr.ry + rr.rh * 0.5f + 12 * bs + (rej_down ? 2.0f * s : 0.0f), bs, 1, 1, 1);
            }
        } else {
            /* Gift button: 1000 coins + 5 gems, once a day per friend, delivered within a minute if their game is open, else when it next starts. */
            const int st = gift_button_state(fr);
            const char *label = st == 0 ? L6("Send gift", "Regalar", "Offrir", "Schenken", "Presentear", "Regala")
                              : st == 1 ? "..."
                              : st == 2 ? L6("Sent today", "Enviado hoy", "Envoyé", "Gesendet", "Enviado", "Inviato")
                                        : L6("Error", "Error", "Erreur", "Fehler", "Erro", "Errore");
            if (rr.gy >= list_top && rr.gy + rr.gh <= list_bot) {
                const bool pressed = (st == 0 && g_gift_down_id == fr.id);
                const FTex &gt = (st == 0) ? (pressed && g_tex_btn_orange_down.id ? g_tex_btn_orange_down : g_tex_btn_orange_up)
                               : (st == 3) ? g_tex_btn_red
                                           : g_tex_btn_grey;
                if (gt.id) {
                    draw_button_tex(gt, rr.gx, rr.gy, rr.gw, rr.gh);
                } else {
                    fill_round_rect(rr.gx, rr.gy + 3 * s, rr.gw, rr.gh, 18 * s, 0, 0, 0, 0.3f);
                    if (st == 0)
                        fill_round_rect(rr.gx, rr.gy, rr.gw, rr.gh, 18 * s, 0.85f, 0.45f, 0.10f, 1);
                    else if (st == 3)
                        fill_round_rect(rr.gx, rr.gy, rr.gw, rr.gh, 18 * s, 0.70f, 0.20f, 0.18f, 1);
                    else
                        fill_round_rect(rr.gx, rr.gy, rr.gw, rr.gh, 18 * s, 0.40f, 0.40f, 0.44f, 1);
                }
                const float bs = s * 0.56f;
                const float press_y = pressed ? 2.0f * s : 0.0f;
                const float tc = (st == 0 || st == 3) ? 1.0f : 0.88f;
                draw_text(label, false, rr.gx + (rr.gw - text_width(label, false, bs)) * 0.5f,
                          rr.gy + rr.gh * 0.5f + 12 * bs + press_y, bs, tc, tc, tc);

                /* Compact red Remove ("X") button with 2-tap confirm ("?"). */
                const bool rem_armed = (g_remove_confirm_id == fr.id && SDL_GetTicks() - g_remove_confirm_ms < 4000);
                const bool rem_down = (g_remove_down_id == fr.id);
                if (g_tex_btn_red.id) draw_button_tex(g_tex_btn_red, rr.dx, rr.dy, rr.dw, rr.dh, 24.0f);
                else fill_round_rect(rr.dx, rr.dy, rr.dw, rr.dh, 14 * s, 0.72f, 0.18f, 0.16f, 1);
                const char *xlbl = rem_armed ? "?" : "X";
                const float xs = s * 0.62f;
                draw_text(xlbl, false, rr.dx + (rr.dw - text_width(xlbl, false, xs)) * 0.5f,
                          rr.dy + rr.dh * 0.5f + 13 * xs + (rem_down ? 2.0f * s : 0.0f), xs, 1, 1, 1);
            }
        }
        const std::string name = fit(fr.name, false, s, max_w);
        draw_text(name.c_str(), false, tx, iy + 58 * s, s, 0xfd / 255.f, 0xdc / 255.f, 0x67 / 255.f, list_top, list_bot);

        const float fs = s * 0.62f;
        if (fr.is_request) {
            const std::string req_lbl = fit(
                L6("Wants to be your friend", "Quiere ser tu amigo", "Veut être ton ami",
                   "Möchte dein Freund sein", "Quer ser seu amigo", "Vuole essere tuo amico"),
                false, fs, max_w);
            draw_text(req_lbl.c_str(), false, tx, iy + 104 * s, fs, 0.95f, 0.92f, 0.70f, list_top, list_bot);
        } else {
            std::string where;
            if (fr.online && fr.where == "level")
                where = std::string(L6("Playing: ", "Jugando: ", "Joue à : ", "Spielt: ", "Jogando: ", "Gioca a: ")) +
                        (fr.level.empty()
                             ? L6("a level", "un nivel", "un niveau", "ein Level", "uma fase", "un livello")
                             : level_label(fr.level));
            else if (fr.online && fr.where == "map")
                where = std::string(L6("On the world map", "En el mapa", "Sur la carte", "Auf der Weltkarte",
                                       "No mapa", "Sulla mappa")) +
                        (fr.world.empty() ? "" : ": " + level_label(fr.world));
            else if (fr.online && fr.where == "menu")
                where = L6("In the menus", "En el menú", "Dans les menus", "In den Menüs", "Nos menus", "Nei menu");
            else if (fr.online)
                where = L6("Playing now", "Jugando ahora", "En ligne", "Spielt gerade", "Jogando agora", "Sta giocando");
            else
                where = ago_label(fr.ago);
            if (!fr.version.empty()) where += "  (v" + fr.version + ")";
            where = fit(where, false, fs, max_w);
            if (fr.online)
                draw_text(where.c_str(), false, tx, iy + 100 * s, fs, 0.55f, 1.0f, 0.45f, list_top, list_bot);
            else
                draw_text(where.c_str(), false, tx, iy + 100 * s, fs, 0.85f, 0.85f, 0.85f, list_top, list_bot);

            char progress[160];
            std::snprintf(progress, sizeof(progress),
                          L6("%d plants  -  %d worlds  -  %d levels",
                             "%d plantas  -  %d mundos  -  %d niveles",
                             "%d plantes  -  %d mondes  -  %d niveaux",
                             "%d Pflanzen  -  %d Welten  -  %d Level",
                             "%d plantas  -  %d mundos  -  %d fases",
                             "%d piante  -  %d mondi  -  %d livelli"),
                          fr.plants, fr.worlds, fr.levels);
            const std::string prog = fit(progress, false, fs, max_w);
            draw_text(prog.c_str(), false, tx, iy + 136 * s, fs, 1, 1, 1, list_top, list_bot);
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

void draw_all(SDL_Window *window) {
    int ww, wh;
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    GLint prev_program = 0, prev_fbo = 0, prev_viewport[4]{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, (double)ww, (double)wh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();

    draw_screen((float)ww, (float)wh);

    glDisable(GL_SCISSOR_TEST);
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
    glUseProgram(prev_program);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

}  // namespace

extern "C" {

void friends_screen_request_open(void) { g_open_requested.store(true); }

void friends_screen_close(void) {
    g_open = false;
    g_dragging = false;
    g_invite_down = false;
    g_gift_down_id.clear();
    g_req_down_id.clear();
    g_remove_down_id.clear();
    g_remove_confirm_id.clear();
}

int friends_screen_is_open(void) { return (g_open || g_open_requested.load()) ? 1 : 0; }

void friends_screen_handle_event(const SDL_Event *e) {
    if (!g_open || !e) return;
    SDL_Window *win = SDL_GetMouseFocus();
    int ww = 1920, wh = 1080;
    if (win) SDL_GL_GetDrawableSize(win, &ww, &wh);
    const Layout L = layout((float)ww, (float)wh);
    int rows = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        rows = (int)g_friends.size();
    }
    const float ms = max_scroll(L, rows);
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        const float x = (float)e->button.x, y = (float)e->button.y;
        if (x < L.mx || x > L.mx + L.mw || y < L.my || y > L.my + L.mh) {
            friends_screen_close();
            config_screen_play_exit_sound();
            return;
        }
        if (x >= L.bx && x <= L.bx + L.bw && y >= L.by && y <= L.by + L.bh) {
            g_invite_down = true;
            g_remove_confirm_id.clear();
            return;
        }
        if (x >= L.lx && x <= L.lx + L.lw && y >= L.ly && y <= L.ly + L.lh) {
            std::vector<Friend> list;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                list = g_friends;
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                const RowRect rr = row_rect(L, i);
                if (list[i].is_request) {
                    if (x >= rr.ax && x <= rr.ax + rr.aw && y >= rr.ay && y <= rr.ay + rr.ah) {
                        g_req_down_id = list[i].id;
                        g_req_down_accept = true;
                        g_remove_confirm_id.clear();
                        return;
                    }
                    if (x >= rr.rx && x <= rr.rx + rr.rw && y >= rr.ry && y <= rr.ry + rr.rh) {
                        g_req_down_id = list[i].id;
                        g_req_down_accept = false;
                        g_remove_confirm_id.clear();
                        return;
                    }
                } else {
                    if (x >= rr.dx && x <= rr.dx + rr.dw && y >= rr.dy && y <= rr.dy + rr.dh) {
                        g_remove_down_id = list[i].id;
                        return;
                    }
                    if (x >= rr.gx && x <= rr.gx + rr.gw && y >= rr.gy && y <= rr.gy + rr.gh) {
                        g_remove_confirm_id.clear();
                        if (gift_button_state(list[i]) == 0) {
                            g_gift_down_id = list[i].id;
                        }
                        return;
                    }
                }
            }
            g_remove_confirm_id.clear();
            g_dragging = true;
            g_drag_y = (int)y;
            g_drag_scroll_start = g_scroll;
        }
    } else if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT) {
        const float x = (float)e->button.x, y = (float)e->button.y;
        const bool was_invite = g_invite_down;
        const std::string was_gift = g_gift_down_id;
        const std::string was_req = g_req_down_id;
        const bool was_req_acc = g_req_down_accept;
        const std::string was_rem = g_remove_down_id;
        g_invite_down = false;
        g_gift_down_id.clear();
        g_req_down_id.clear();
        g_remove_down_id.clear();
        g_dragging = false;
        if (was_invite && x >= L.bx && x <= L.bx + L.bw && y >= L.by && y <= L.by + L.bh) {
            pvz2native::invite_button_clicked();
            g_copied_ms = SDL_GetTicks();
            return;
        }
        if (!was_req.empty() && x >= L.lx && x <= L.lx + L.lw && y >= L.ly && y <= L.ly + L.lh) {
            std::vector<Friend> list;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                list = g_friends;
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (!list[i].is_request || list[i].id != was_req) continue;
                const RowRect rr = row_rect(L, i);
                const bool hit = was_req_acc
                    ? (x >= rr.ax && x <= rr.ax + rr.aw && y >= rr.ay && y <= rr.ay + rr.ah)
                    : (x >= rr.rx && x <= rr.rx + rr.rw && y >= rr.ry && y <= rr.ry + rr.rh);
                if (hit) {
                    std::lock_guard<std::mutex> lk(g_lock);
                    g_action_queue.push_back(
                        {list[i].id, list[i].name, was_req_acc ? ActionKind::Accept : ActionKind::Reject,
                         list[i].invite_code});
                    pvz2_log("[friends] %s request from %s queued", was_req_acc ? "accept" : "reject",
                             list[i].id.c_str());
                }
                break;
            }
            return;
        }
        if (!was_rem.empty() && x >= L.lx && x <= L.lx + L.lw && y >= L.ly && y <= L.ly + L.lh) {
            std::vector<Friend> list;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                list = g_friends;
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].is_request || list[i].id != was_rem) continue;
                const RowRect rr = row_rect(L, i);
                if (x >= rr.dx && x <= rr.dx + rr.dw && y >= rr.dy && y <= rr.dy + rr.dh) {
                    const Uint32 now = SDL_GetTicks();
                    if (g_remove_confirm_id == was_rem && now - g_remove_confirm_ms < 4000) {
                        g_remove_confirm_id.clear();
                        std::lock_guard<std::mutex> lk(g_lock);
                        g_action_queue.push_back({list[i].id, list[i].name, ActionKind::Remove});
                        pvz2_log("[friends] remove %s queued", list[i].id.c_str());
                    } else {
                        g_remove_confirm_id = was_rem;
                        g_remove_confirm_ms = now;
                        char msg[192];
                        std::snprintf(msg, sizeof(msg),
                                      L6("Click again to remove %s", "Pulsa de nuevo para eliminar a %s",
                                         "Clique encore pour retirer %s", "Erneut klicken, um %s zu entfernen",
                                         "Clique de novo para remover %s", "Clicca di nuovo per rimuovere %s"),
                                      list[i].name.c_str());
                        config_screen_toast(msg);
                    }
                }
                break;
            }
            return;
        }
        if (!was_gift.empty() && x >= L.lx && x <= L.lx + L.lw && y >= L.ly && y <= L.ly + L.lh) {
            std::vector<Friend> list;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                list = g_friends;
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].id != was_gift) continue;
                const RowRect rr = row_rect(L, i);
                if (x >= rr.gx && x <= rr.gx + rr.gw && y >= rr.gy && y <= rr.gy + rr.gh &&
                    gift_button_state(list[i]) == 0) {
                    std::lock_guard<std::mutex> lk(g_lock);
                    g_gift_queue.push_back(list[i].id);
                    pvz2_log("[friends] gift to %s queued", list[i].id.c_str());
                }
                break;
            }
        }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        g_dragging = false;
        g_invite_down = false;
        g_gift_down_id.clear();
        g_req_down_id.clear();
        g_remove_down_id.clear();
    } else if (e->type == SDL_MOUSEMOTION && g_dragging) {
        g_scroll = std::max(0.0f, std::min(ms, g_drag_scroll_start + (float)(g_drag_y - e->motion.y)));
    } else if (e->type == SDL_MOUSEWHEEL) {
        g_scroll = std::max(0.0f, std::min(ms, g_scroll - e->wheel.y * kItemStep * L.s * 0.5f));
    } else if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE) {
        friends_screen_close();
    }
}

void friends_screen_draw(SDL_Window *window) {
    if (!g_worker_started) {
        g_worker_started = true;
        pvz2_cloud_init();
        g_worker = std::thread(worker_main);
    }
    if (!g_listener_started) {
        g_listener_started = true;
        g_listener = std::thread(invite_listener_main);
    }
    if (g_open_requested.exchange(false) && !g_open) {
        g_open = true;
        g_scroll = 0;
        g_dragging = false;
        g_refresh.store(true);
        config_screen_play_exit_sound();
        pvz2_log("[friends] list opened");
    }
    /* Gifts delivered at boot + friend request toasts once the game is on screen. */
    static Uint32 s_first_ms = 0;
    if (s_first_ms == 0) s_first_ms = SDL_GetTicks() | 1;
    if (SDL_GetTicks() - s_first_ms > 4000) {
        if (!g_gift_notice.empty()) {
            config_screen_toast(g_gift_notice.c_str());
            g_gift_notice.clear();
        }
        std::vector<std::string> toasts;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            toasts.swap(g_pending_toasts);
        }
        for (const std::string &t : toasts) config_screen_toast(t.c_str());
    }
    if (!g_open || !window) return;
    draw_all(window);
}

void friends_claim_gifts_at_boot(void) {
    pvz2_cloud_init();
    if (!linked(pvz2_cloud_config())) {
        pvz2_log("[friends] boot: no Discord account linked, gifts not checked");
        return;
    }
    std::string out;
    const int status = friends_http("GET", "gifts", "", out, 6);
    pvz2_log("[friends] boot: gifts checked (HTTP %d, %zu bytes)", status, out.size());
    if (status != 200 || out.empty()) return;
    /* Gifts already written into the save but not acknowledged (the ack failed): never added twice. */
    const char *applied_path = "userdata/friend_gifts_applied.txt";
    const std::vector<std::uint8_t> applied_bytes = host_files::read(applied_path);
    std::string applied(applied_bytes.begin(), applied_bytes.end());
    long long coins = 0, gems = 0;
    std::vector<std::string> ids, fresh, names;
    for (const std::string &line : split(out, '\n')) {
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 4 || f[0].empty() || !safe_token(f[0].c_str())) continue;
        ids.push_back(f[0]);
        if (applied.find(f[0] + "\n") != std::string::npos) continue;
        fresh.push_back(f[0]);
        coins += std::max(0L, std::atol(f[2].c_str()));
        gems += std::max(0L, std::atol(f[3].c_str()));
        if (std::find(names.begin(), names.end(), f[1]) == names.end()) names.push_back(f[1]);
    }
    if (ids.empty()) return;
    if (!fresh.empty()) {
        if (!add_to_save(coins, gems)) {
            pvz2_log("[friends] %zu gift(s) could not be written into userdata/pp.dat -- kept for next time",
                     fresh.size());
            return;
        }
        for (const std::string &id : fresh) applied += id + "\n";
        host_files::write(applied_path, std::vector<std::uint8_t>(applied.begin(), applied.end()));
        std::string who;
        for (std::size_t i = 0; i < names.size() && i < 3; ++i) who += (i ? ", " : "") + names[i];
        if (names.size() > 3) who += "...";
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      L6("Gift from %s: %lld coins and %lld gems",
                         "Regalo de %s: %lld monedas y %lld gemas",
                         "Cadeau de %s : %lld pièces et %lld gemmes",
                         "Geschenk von %s: %lld Münzen und %lld Juwelen",
                         "Presente de %s: %lld moedas e %lld joias",
                         "Regalo da %s: %lld monete e %lld gemme"),
                      who.c_str(), coins, gems);
        g_gift_notice = msg;
        pvz2_log("[friends] %zu gift(s) added to the save: +%lld coins, +%lld gems", fresh.size(), coins, gems);
    }
    std::string body = "{\"ids\":[";
    for (std::size_t i = 0; i < ids.size(); ++i) body += (i ? ",\"" : "\"") + ids[i] + "\"";
    body += "]}";
    if (friends_http("POST", "ack", body, out, 6) == 200)
        host_files::write(applied_path, std::vector<std::uint8_t>()); /* the server forgot them too */
}

void friends_tick_live_gifts(void *guest_image) {
    auto *img = static_cast<pvz2_elf_image_t *>(guest_image);
    const std::uint32_t vt = sym().patch.player_info_vtable, vt2 = sym().patch.player_info_vtable2;
    if (img == nullptr || vt == 0 || vt2 == 0) return;
    LiveGifts gift;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_live_gifts.ids.empty()) return;
        gift = g_live_gifts;
    }
    /* Looking for the profile walks the whole heap: at most every few seconds while a gift waits (before the
     * profile is loaded it is not there yet). */
    static auto last_try = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_try < std::chrono::seconds(3)) return;
    last_try = now;

    constexpr std::uint32_t kVtable2At = 0x58, kCoinsAt = 0xa4, kGemsAt = 0xac, kEnd = 0xb0;
    auto rd = [img](std::uint32_t a) {
        std::uint32_t v;
        std::memcpy(&v, img->mem + a, 4);
        return v;
    };
    const std::uint32_t want = img->so_base + vt, want2 = img->so_base + vt2;
    auto is_profile = [&](std::uint32_t a) {
        return a >= img->images_end && a + kEnd <= img->mem_size && rd(a) == want && rd(a + kVtable2At) == want2;
    };
    static std::uint32_t profile = 0; /* where it was last time */
    if (profile == 0 || !is_profile(profile)) {
        profile = 0;
        int found = 0;
        for (std::uint32_t a = (img->images_end + 3u) & ~3u; a + kEnd <= img->mem_size; a += 4) {
            if (rd(a) != want || !is_profile(a)) continue;
            if (++found == 1) profile = a;
        }
        if (found != 1) {
            /* None: not loaded yet. Several: no way to tell which one the game saves -- leave the gift for
             * the next boot, which writes it into pp.dat instead. */
            if (found > 1) pvz2_log("[friends] %d player profiles in memory, gift left for the next start", found);
            profile = 0;
            return;
        }
    }
    const std::uint32_t coins = rd(profile + kCoinsAt), gems = rd(profile + kGemsAt);
    const long long new_coins = std::min<long long>((long long)coins + gift.coins, 0x7fffffff);
    const long long new_gems = std::min<long long>((long long)gems + gift.gems, 0x7fffffff);
    const std::uint32_t nc = (std::uint32_t)new_coins, ng = (std::uint32_t)new_gems;
    std::memcpy(img->mem + profile + kCoinsAt, &nc, 4);
    std::memcpy(img->mem + profile + kGemsAt, &ng, 4);
    pvz2_log("[friends] %zu gift(s) added while playing: coins %u -> %u, gems %u -> %u", gift.ids.size(), coins, nc,
             gems, ng);
    {
        std::lock_guard<std::mutex> lk(g_applied_lock);
        std::string applied = read_applied();
        for (const std::string &id : gift.ids) applied += id + "\n";
        write_applied(applied);
    }
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_gift_acks.insert(g_gift_acks.end(), gift.ids.begin(), gift.ids.end());
        g_live_gifts = LiveGifts{};
    }
    std::string who;
    for (std::size_t i = 0; i < gift.names.size() && i < 3; ++i) who += (i ? ", " : "") + gift.names[i];
    if (gift.names.size() > 3) who += "...";
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  L6("Gift from %s: %lld coins and %lld gems",
                     "Regalo de %s: %lld monedas y %lld gemas",
                     "Cadeau de %s : %lld pièces et %lld gemmes",
                     "Geschenk von %s: %lld Münzen und %lld Juwelen",
                     "Presente de %s: %lld moedas e %lld joias",
                     "Regalo da %s: %lld monete e %lld gemme"),
                  who.c_str(), gift.coins, gift.gems);
    config_screen_toast(msg);
}

void friends_screen_shutdown(void) {
    g_stop.store(true);
    if (g_worker.joinable()) g_worker.detach(); /* may be inside curl; the process is exiting */
    if (g_listener.joinable()) g_listener.detach();
    if (g_loaded) {
        auto del_tex = [](FTex &t) {
            if (t.id) glDeleteTextures(1, &t.id);
            t = FTex{};
        };
        del_tex(g_tex_menu);
        del_tex(g_tex_item);
        del_tex(g_tex_pfp_frame);
        del_tex(g_tex_pfp_placeholder);
        del_tex(g_tex_btn_green_up);
        del_tex(g_tex_btn_green_down);
        del_tex(g_tex_btn_orange_up);
        del_tex(g_tex_btn_orange_down);
        del_tex(g_tex_btn_grey);
        del_tex(g_tex_btn_red);
        g_font.unload();
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto &kv : g_pics)
            if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
        g_pics.clear();
        g_loaded = false;
    }
    if (g_tex_hud_btn.id) {
        glDeleteTextures(1, &g_tex_hud_btn.id);
        g_tex_hud_btn = FTex{};
    }
}

void friends_register_url_protocol(void) {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) return;
    HKEY hk = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\pvz2native", 0, nullptr, 0, KEY_WRITE, nullptr, &hk,
                        nullptr) == ERROR_SUCCESS) {
        const char *desc = "URL:PvZ2Native Protocol";
        RegSetValueExA(hk, nullptr, 0, REG_SZ, (const BYTE *)desc, (DWORD)std::strlen(desc) + 1);
        RegSetValueExA(hk, "URL Protocol", 0, REG_SZ, (const BYTE *)"", 1);
        RegCloseKey(hk);
    }
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\pvz2native\\shell\\open\\command", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        std::string cmd = std::string("\"") + exe + "\" \"%1\"";
        RegSetValueExA(hk, nullptr, 0, REG_SZ, (const BYTE *)cmd.c_str(), (DWORD)cmd.size() + 1);
        RegCloseKey(hk);
    }
#endif
}

int friends_try_forward_invite_uri(const char *uri) {
    std::string from_id, name, code;
    if (!parse_invite_uri(uri, from_id, name, code)) return 0;
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    DWORD to = 400;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    timeval to{0, 400000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)kInviteLocalPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int forwarded = 0;
    if (connect(s, (sockaddr *)&addr, sizeof(addr)) == 0) {
        std::string esc_name;
        for (char c : name) {
            if (c == '"' || c == '\\') esc_name.push_back('\\');
            esc_name.push_back(c);
        }
        const std::string body = "{\"from\":\"" + from_id + "\",\"name\":\"" + esc_name +
                                 "\",\"code\":\"" + code + "\",\"action\":\"open\"}";
        const std::string req =
            "POST /invite HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        if (send(s, req.c_str(), (int)req.size(), 0) > 0) {
            char buf[256];
            if (recv(s, buf, sizeof(buf) - 1, 0) > 0) forwarded = 1;
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return forwarded;
}

void friends_handle_invite_uri(const char *uri) {
    std::string from_id, name, code;
    if (!parse_invite_uri(uri, from_id, name, code)) return;
    queue_local_invite(from_id, name, code, false);
}

void friends_get_invite_code(char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_my_invite_code.empty()) {
        SDL_strlcpy(out, g_my_invite_code.c_str(), (std::size_t)cap);
    }
}

void friends_screen_set_hud_visible(int visible) {
    g_hud_seen_ms.store(visible ? SDL_GetTicks64() : 0, std::memory_order_relaxed);
}

void friends_screen_note_hud_visible(void) {
    friends_screen_set_hud_visible(1);
}

static bool hud_button_active(void) {
    const Uint64 seen = g_hud_seen_ms.load(std::memory_order_relaxed);
    if (seen == 0) return false;
    return (SDL_GetTicks64() - seen) <= 150;
}

static void hud_button_rect(float win_h, float *x, float *y, float *w, float *h) {
    /* 6th button on the 6.6.1 top-left WorldMap / PlayModes HUD bar (in 1536p units:
     * x0=32, step=188 -> x5=972, y=32, w=133, h=123), right beside the News button. */
    *x = (972.0f / 1536.0f) * win_h;
    *y = (32.0f / 1536.0f) * win_h;
    *w = (133.0f / 1536.0f) * win_h;
    *h = (123.0f / 1536.0f) * win_h;
}

int friends_screen_hud_button_event(const SDL_Event *e, SDL_Window *window) {
    if (!e || !window || g_open || !hud_button_active() || g_hud_brightness < 0.7f) {
        g_hud_btn_down = false;
        return 0;
    }
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    if (wh <= 0) return 0;
    float bx, by, bw, bh;
    hud_button_rect((float)wh, &bx, &by, &bw, &bh);
    auto in_btn = [&](float mx, float my) {
        return mx >= bx && mx <= bx + bw && my >= by && my <= by + bh;
    };
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        if (in_btn((float)e->button.x, (float)e->button.y)) {
            g_hud_btn_down = true;
            return 1;
        }
    } else if (e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT) {
        if (g_hud_btn_down) {
            g_hud_btn_down = false;
            if (in_btn((float)e->button.x, (float)e->button.y)) {
                friends_screen_request_open();
            }
            return 1;
        }
    } else if (e->type == SDL_MOUSEMOTION && g_hud_btn_down) {
        return 1;
    }
    return 0;
}

void friends_screen_draw_hud_button(SDL_Window *window) {
    if (!window || g_open || !hud_button_active()) return;
    if (!g_tex_hud_btn.id) {
        g_tex_hud_btn = load_bmp_from_rc(320); /* btn_friends_hud.bmp */
    }
    if (!g_tex_hud_btn.id) return;

    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    float bx, by, bw, bh;
    hud_button_rect((float)wh, &bx, &by, &bw, &bh);
    if (g_hud_btn_down) {
        const float shrink = bw * 0.04f;
        bx += shrink;
        by += shrink;
        bw -= shrink * 2.0f;
        bh -= shrink * 2.0f;
    }

    bool has_pending = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (const Friend &f : g_friends) {
            if (f.is_request) {
                has_pending = true;
                break;
            }
        }
    }

    GLint prev_program = 0, prev_fbo = 0, prev_viewport[4]{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, (double)ww, (double)wh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();

    /* Follow the game's own fades / loading dim: sample the News button's white border (the button just left
     * of ours) in the frame the game composited, and darken ours by the same amount. */
    float bright = 1.0f;
    {
        const float k = (float)wh / 576.0f;
        int rx = (int)(296.0f * k), ry = (int)(9.0f * k);
        int rw = std::max(4, (int)(40.0f * k)), rh = std::max(4, (int)(9.0f * k));
        if (rx + rw > ww) rw = ww - rx;
        if (rw > 0 && ry + rh <= wh) {
            std::vector<unsigned char> px((std::size_t)rw * rh * 4);
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(rx, wh - ry - rh, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            int mx = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                const int lum = (px[i] + px[i + 1] + px[i + 2]) / 3;
                if (lum > mx) mx = lum;
            }
            bright = std::min(1.0f, (float)mx / 235.0f);
        }
    }
    g_hud_brightness = bright;
    if (bright < 0.12f) {
        glMatrixMode(GL_TEXTURE);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glPopAttrib();
        glUseProgram(prev_program);
        glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
        return;
    }

    glBindTexture(GL_TEXTURE_2D, g_tex_hud_btn.id);
    const float shade = bright * (g_hud_btn_down ? 0.84f : 1.0f);
    glColor4f(shade, shade, shade, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(bx, by);
    glTexCoord2f(1, 0); glVertex2f(bx + bw, by);
    glTexCoord2f(1, 1); glVertex2f(bx + bw, by + bh);
    glTexCoord2f(0, 1); glVertex2f(bx, by + bh);
    glEnd();

    if (has_pending) {
        const float r = bh * 0.21f;
        const float cx = bx + bw - r * 0.45f;
        const float cy = by + r * 0.45f;
        fill_round_rect(cx - r * 1.18f, cy - r * 1.18f, r * 2.36f, r * 2.36f, r * 1.18f, bright, bright, bright, 1.0f);
        fill_round_rect(cx - r * 0.90f, cy - r * 0.90f, r * 1.80f, r * 1.80f, r * 0.90f, 0.88f * bright,
                        0.14f * bright, 0.12f * bright, 1.0f);
    }

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
    glUseProgram(prev_program);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

}  // extern "C"

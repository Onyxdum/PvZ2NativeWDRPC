/* Discord Rich Presence -- see include/pvz2native/presence.h.
 *
 * Why it looks the way it does:
 *
 *  - No library. Discord's local IPC is a tiny framed protocol (4-byte opcode,
 *    4-byte length, JSON), so it is spoken directly over the named pipe / unix
 *    socket. That keeps the static MinGW build free of a new dependency.
 *
 *  - Own thread. The taps run on guest worker threads and on the frame thread,
 *    which must never block on a socket. They only push a string onto a bounded
 *    queue; this thread does the matching, the file trace and the I/O.
 *
 *  - Rules, not code. The engine never tells the host "level 3 of Ancient Egypt
 *    started". The only things the host sees are the resource files it opens
 *    (Resources_GetAssetFileInfo) and what it logs. Which of those mean what is
 *    data, so it lives in a rules file the player (or a later patch) can edit
 *    without rebuilding.
 *
 * Rules file (presence_rules.txt, next to the .exe), one rule per line:
 *
 *     <pattern> | <details> | <state>
 *
 *  - Fields are separated by " | " (space, pipe, space), so a regex may use a
 *    bare '|' for alternation. <state> is optional.
 *  - <pattern> is matched against the event string "<source>:<text>", e.g.
 *    "asset:PROPERTIES/LEVELS/EGYPT_12.RTON". Plain patterns are a
 *    case-insensitive substring test. A "regex:" prefix makes it an ECMAScript
 *    regex (case-insensitive); "$1".."$9" in details/state are replaced by its
 *    capture groups.
 *  - The first matching rule in file order wins for that event, and the most
 *    recent event that matches anything decides what is shown -- so list the
 *    more specific rules first.
 *  - "default | <details> | <state>" is what is shown before anything matches.
 *  - Lines starting with ';' or '#' are comments.
 */

#include <pvz2native/config.h>
#include <pvz2native/host_paths.h>
#include <pvz2native/presence.h>

#include <SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace pvz2native {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kQueueMax = 4096;
constexpr std::size_t kTraceMax = 50000;
constexpr std::size_t kFieldMax = 128;   /* Discord's limit for details/state */
constexpr int kMinSendMs = 1500;         /* Discord allows ~5 updates / 20 s */
constexpr int kReconnectMs = 5000;

/* --- activity + rules ------------------------------------------------------ */

struct Activity {
    std::string details;
    std::string state;
    bool operator==(const Activity &o) const { return details == o.details && state == o.state; }
    bool operator!=(const Activity &o) const { return !(*this == o); }
};

struct Rule {
    bool is_regex = false;
    bool is_temp = false;
    std::string needle; /* lowercased, for substring rules */
    std::regex re;
    std::string details;
    std::string state;
};

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

/* Cuts to `max` bytes without splitting a UTF-8 sequence. */
std::string clip(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

std::string json_escape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

/* "$1".."$9" -> the matching capture group; anything else is copied. */
std::string expand(const std::string &tmpl, const std::smatch &m) {
    std::string o;
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] >= '1' && tmpl[i + 1] <= '9') {
            std::size_t g = (std::size_t)(tmpl[i + 1] - '0');
            if (g < m.size()) o += m[g].str();
            ++i;
        } else {
            o += tmpl[i];
        }
    }
    return o;
}

std::string data_dir() {
    char *p = pvz2_host_data_path();
    std::string s = p ? p : "";
    if (p) SDL_free(p);
    return s;
}

std::string install_dir() {
    char *p = pvz2_host_install_path();
    std::string s = p ? p : "";
    if (p) SDL_free(p);
    return s;
}

bool is_absolute(const std::string &p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() > 1 && p[1] == ':';
}

std::string resolve(const std::string &p) {
    if (is_absolute(p)) return p;
    std::string d = data_dir() + p;
    if (FILE *f = std::fopen(d.c_str(), "rb")) {
        std::fclose(f);
        return d;
    }
    std::string inst = install_dir() + p;
    if (FILE *f = std::fopen(inst.c_str(), "rb")) {
        std::fclose(f);
        return inst;
    }
    return d;
}

const char *kRulesTemplate =
    "; Discord Rich Presence rules -- see src/presence.cpp for format.\n"
    ";\n"
    ";   <pattern> | <details> | <state>\n"
    ";\n"
    "; Turn on `trace = 1` under [discord] in config.ini to inspect game events.\n"
    "\n"
    "default | Plants vs. Zombies 2 | In Main Menu\n";

bool g_is_spanish = false;

std::vector<Rule> load_rules(const std::string &path, Activity &def) {
    std::vector<Rule> rules;
    def = Activity{"Plants vs. Zombies 2", g_is_spanish ? "En el menú principal" : "In Main Menu"};

    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (FILE *w = std::fopen(path.c_str(), "wb")) {
            std::fputs(kRulesTemplate, w);
            std::fclose(w);
        }
        return rules;
    }
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    std::size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        ++lineno;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        /* "pattern | details |" -- an empty state with its separator left on. */
        if (line.size() > 2 && line.back() == '|' && line[line.size() - 2] == ' ')
            line = trim(line.substr(0, line.size() - 1));

        std::vector<std::string> parts;
        std::size_t a = 0;
        for (;;) {
            std::size_t b = line.find(" | ", a);
            if (b == std::string::npos) {
                parts.push_back(trim(line.substr(a)));
                break;
            }
            parts.push_back(trim(line.substr(a, b - a)));
            a = b + 3;
        }
        if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) {
            std::printf("[discord] %s:%d ignored (need \"pattern | details | state\")\n",
                        path.c_str(), lineno);
            continue;
        }
        if (parts[0] == "default") {
            def.details = parts[1];
            def.state = parts.size() > 2 ? parts[2] : "";
            continue;
        }
        Rule r;
        r.details = parts[1];
        r.state = parts.size() > 2 ? parts[2] : "";
        std::string pat = parts[0];
        if (pat.rfind("temp:", 0) == 0) {
            r.is_temp = true;
            pat = trim(pat.substr(5));
        }
        if (pat.rfind("regex:", 0) == 0) {
            try {
                r.re = std::regex(pat.substr(6), std::regex::ECMAScript | std::regex::icase);
                r.is_regex = true;
            } catch (const std::regex_error &) {
                std::printf("[discord] %s:%d ignored (bad regex)\n", path.c_str(), lineno);
                continue;
            }
        } else {
            r.needle = lower(pat);
        }
        rules.push_back(std::move(r));
    }
    return rules;
}

/* --- IPC transport --------------------------------------------------------- */

class Ipc {
public:
    ~Ipc() { close(); }

    bool connect() {
        close();
#ifdef _WIN32
        for (int i = 0; i < 10; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
            HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                                   nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                h_ = h;
                return true;
            }
        }
        return false;
#else
        std::vector<std::string> dirs;
        if (const char *xdg = std::getenv("XDG_RUNTIME_DIR")) {
            dirs.push_back(xdg);
            dirs.push_back(std::string(xdg) + "/app/com.discordapp.Discord");
            dirs.push_back(std::string(xdg) + "/app/dev.vencord.Vesktop");
            dirs.push_back(std::string(xdg) + "/snap.discord");
        }
        for (const char *v : {"TMPDIR", "TMP", "TEMP"})
            if (const char *d = std::getenv(v)) dirs.push_back(d);
        dirs.push_back("/tmp");
        for (const std::string &d : dirs) {
            for (int i = 0; i < 10; ++i) {
                int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) return false;
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/discord-ipc-%d", d.c_str(), i);
                if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0) {
                    fd_ = fd;
                    return true;
                }
                ::close(fd);
            }
        }
        return false;
#endif
    }

    void close() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }

    bool is_open() const {
#ifdef _WIN32
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

    bool send(std::uint32_t op, const std::string &json) {
        std::string frame(8, '\0');
        const std::uint32_t len = (std::uint32_t)json.size();
        std::memcpy(&frame[0], &op, 4); /* both ends are little-endian hosts */
        std::memcpy(&frame[4], &len, 4);
        frame += json;
        return write_all(frame.data(), frame.size());
    }

    /* Reads one frame if one is waiting (waits up to `wait_ms` for it to start).
     * Returns false when nothing arrived or the link is dead (check is_open). */
    bool recv(std::uint32_t &op, std::string &payload, int wait_ms) {
        if (!wait_readable(wait_ms)) return false;
        std::uint32_t hdr[2];
        if (!read_all(hdr, 8)) return false;
        op = hdr[0];
        if (hdr[1] > (1u << 20)) { /* nonsense length: drop the connection */
            close();
            return false;
        }
        payload.assign(hdr[1], '\0');
        return hdr[1] == 0 || read_all(&payload[0], hdr[1]);
    }

private:
    bool write_all(const void *p, std::size_t n) {
#ifdef _WIN32
        DWORD w = 0;
        if (!WriteFile(h_, p, (DWORD)n, &w, nullptr) || w != n) {
            close();
            return false;
        }
        return true;
#else
        const char *c = (const char *)p;
        while (n > 0) {
#ifdef MSG_NOSIGNAL
            const int flags = MSG_NOSIGNAL;
#else
            const int flags = 0;
#endif
            ssize_t w = ::send(fd_, c, n, flags);
            if (w <= 0) {
                close();
                return false;
            }
            c += w;
            n -= (std::size_t)w;
        }
        return true;
#endif
    }

    bool read_all(void *p, std::size_t n) {
#ifdef _WIN32
        DWORD r = 0;
        if (!ReadFile(h_, p, (DWORD)n, &r, nullptr) || r != n) {
            close();
            return false;
        }
        return true;
#else
        char *c = (char *)p;
        while (n > 0) {
            ssize_t r = ::recv(fd_, c, n, 0);
            if (r <= 0) {
                close();
                return false;
            }
            c += r;
            n -= (std::size_t)r;
        }
        return true;
#endif
    }

    bool wait_readable(int wait_ms) {
#ifdef _WIN32
        const auto until = Clock::now() + std::chrono::milliseconds(wait_ms);
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) {
                close();
                return false;
            }
            if (avail >= 8) return true;
            if (Clock::now() >= until) return false;
            Sleep(10);
        }
#else
        pollfd pfd{fd_, POLLIN, 0};
        int r = ::poll(&pfd, 1, wait_ms);
        if (r > 0 && (pfd.revents & (POLLERR | POLLHUP)) && !(pfd.revents & POLLIN)) {
            close();
            return false;
        }
        return r > 0;
#endif
    }

#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

/* --- the worker ------------------------------------------------------------ */

std::atomic<bool> g_on{false};
std::atomic<bool> g_stop{false};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::string> g_queue;
std::thread g_thread;

std::string g_client_id;
std::string g_image;
std::string g_trace_path;
std::string g_rules_path;
bool g_trace = false;

std::mutex g_user_mu;
std::string g_user_id;
std::string g_username;

/* Extracts a string field `"key":"value"` inside `json` starting at `from`. */
std::string extract_json_str(const std::string &json, const char *key, std::size_t from = 0) {
    std::string pat = std::string("\"") + key + "\":\"";
    std::size_t p = json.find(pat, from);
    if (p == std::string::npos) return {};
    p += pat.size();
    std::string out;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            ++p;
        }
        out += json[p++];
    }
    return out;
}

void parse_ready_user(const std::string &payload) {
    std::size_t u = payload.find("\"user\":");
    if (u == std::string::npos) return;
    std::string uid = extract_json_str(payload, "id", u);
    std::string uname = extract_json_str(payload, "username", u);
    std::string gname = extract_json_str(payload, "global_name", u);
    if (!uid.empty()) {
        std::lock_guard<std::mutex> lk(g_user_mu);
        g_user_id = uid;
        g_username = !uname.empty() ? uname : gname;
    }
}

std::string pid_string() {
#ifdef _WIN32
    return std::to_string((unsigned long)GetCurrentProcessId());
#else
    return std::to_string((long)getpid());
#endif
}

std::string activity_json(const Activity *a, std::int64_t start, unsigned nonce) {
    std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + pid_string();
    if (a) {
        j += ",\"activity\":{\"details\":\"" + json_escape(clip(a->details, kFieldMax)) + "\"";
        if (!a->state.empty()) j += ",\"state\":\"" + json_escape(clip(a->state, kFieldMax)) + "\"";
        j += ",\"timestamps\":{\"start\":" + std::to_string((long long)start) + "}";
        if (!g_image.empty()) {
            j += ",\"assets\":{\"large_image\":\"" + json_escape(g_image) +
                 "\",\"large_text\":\"PvZ2Native\"}";
        }
        j += "}";
    }
    j += "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
    return j;
}

/* Handshake; true once Discord answered READY. */
bool handshake(Ipc &ipc) {
    const std::string cid = !g_client_id.empty() ? g_client_id : "1560077802550853694";
    if (!ipc.send(0, "{\"v\":1,\"client_id\":\"" + json_escape(cid) + "\"}")) return false;
    std::uint32_t op = 0;
    std::string payload;
    if (!ipc.recv(op, payload, 3000)) return false;
    if (payload.find("\"READY\"") == std::string::npos) return false;
    parse_ready_user(payload);
    return true;
}

/* Throws away whatever Discord has replied (acks, errors, PINGs) so the pipe
 * never backs up. Answers PING with PONG, as the protocol asks. */
void drain(Ipc &ipc) {
    std::uint32_t op = 0;
    std::string payload;
    while (ipc.is_open() && ipc.recv(op, payload, 0)) {
        if (op == 3) ipc.send(4, payload);
        if (op == 2) ipc.close(); /* CLOSE */
    }
}

bool is_boss_level_id(const std::string &lvl) {
    if (lvl == "egypt25" || lvl == "pirate25" || lvl == "cowboy25" || lvl == "future25" ||
        lvl == "dark20" || lvl == "beach32" || lvl == "iceage30" || lvl == "iceage32" ||
        lvl == "lostcity32" || lvl == "eighties32" || lvl == "dino32" ||
        lvl == "modern32" || lvl == "modern33" || lvl == "modern34" ||
        lvl.rfind("modern_zomboss_", 0) == 0) {
        return true;
    }
    return false;
}

void worker() {
    Activity def;
    std::vector<Rule> rules = load_rules(g_rules_path, def);
    std::printf("[discord] %zu rule(s) from %s\n", rules.size(), g_rules_path.c_str());

    FILE *trace = nullptr;
    std::set<std::string> seen;
    std::size_t trace_lines = 0;
    if (g_trace) {
        trace = std::fopen(g_trace_path.c_str(), "wb");
        if (trace) std::printf("[discord] tracing events to %s\n", g_trace_path.c_str());
    }

    const std::int64_t start = (std::int64_t)std::time(nullptr);
    const auto t0 = Clock::now();

    Activity desired = def;
    Activity persistent = def;
    bool in_temp = false;
    Clock::time_point temp_until{};
    bool is_paused = false;
    bool is_chooser = false;
    int goals_count = 0;
    int boss_phase = 0;
    bool level_won = false;
    std::string current_level;
    Activity sent;
    bool have_sent = false;
    unsigned nonce = 1;
    Ipc ipc;
    auto last_connect = Clock::now() - std::chrono::milliseconds(kReconnectMs);
    auto last_send = Clock::now() - std::chrono::milliseconds(kMinSendMs);

    while (!g_stop.load()) {
        std::deque<std::string> batch;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(1000),
                          [] { return g_stop.load() || !g_queue.empty(); });
            batch.swap(g_queue);
        }

        for (const std::string &ev : batch) {
            const std::string ev_lower = lower(ev);
            const bool always_trace = ev.rfind("rsb:", 0) == 0 || ev.rfind("state:", 0) == 0;
            if (trace && trace_lines < kTraceMax && (always_trace || seen.insert(ev).second)) {
                ++trace_lines;
                const long ms =
                    (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
                std::fprintf(trace, "[%02ld:%02ld.%03ld] %s\n", ms / 60000, (ms / 1000) % 60, ms % 1000,
                             ev.c_str());
                std::fflush(trace);
            }

            if (ev.rfind("state:Board Level=", 0) == 0) {
                std::string lvl_raw = ev.substr(18);
                const auto colon = lvl_raw.find(':');
                current_level = (colon != std::string::npos) ? lvl_raw.substr(0, colon) : lvl_raw;
                if (is_boss_level_id(current_level)) {
                    if (boss_phase == 0) boss_phase = 1;
                } else {
                    boss_phase = 0;
                }
            } else if (ev.rfind("state:Board BossPhase=", 0) == 0) {
                try {
                    boss_phase = std::stoi(ev.substr(22));
                } catch (...) {
                    boss_phase = 1;
                }
                continue;
            } else if (ev.rfind("state:Board Goals=", 0) == 0) {
                try {
                    goals_count = std::stoi(ev.substr(18));
                } catch (...) {
                    goals_count = 0;
                }
                continue;
            } else if (ev == "state:Board Paused=true") {
                is_paused = true;
                continue;
            } else if (ev == "state:Board Paused=false") {
                is_paused = false;
                continue;
            } else if (ev == "state:Board Chooser=true") {
                is_chooser = true;
                continue;
            } else if (ev == "state:Board Chooser=false") {
                is_chooser = false;
                continue;
            } else if (ev == "state:Board State=LoadingLevel") {
                level_won = false;
                if (is_boss_level_id(current_level)) {
                    boss_phase = 1;
                } else {
                    boss_phase = 0;
                }
            } else if (ev == "state:Board State=PlayerWonOutro") {
                level_won = true;
                is_paused = false;
                is_chooser = false;
            } else if (ev == "state:Board State=PlayerLostOutro" ||
                       ev == "state:Board State=Initial" ||
                       ev == "state:Board State=LoadingSave" ||
                       ev == "state:Board State=LevelIntro" ||
                       ev == "state:Board State=PlayingLevel") {
                level_won = false;
            }

            if (ev.rfind("state:GameStateMgr State=GAME_WorldMap", 0) == 0 ||
                ev.rfind("state:GameStateMgr State=GAME_MainMenu", 0) == 0 ||
                ev.rfind("state:UniverseMap", 0) == 0 ||
                ev.rfind("state:WorldMap Current World=", 0) == 0) {
                is_paused = false;
                is_chooser = false;
                goals_count = 0;
                boss_phase = 0;
                level_won = false;
                current_level.clear();
            }

            for (const Rule &r : rules) {
                bool hit = false;
                std::smatch m;
                if (r.is_regex) {
                    hit = std::regex_search(ev, m, r.re);
                } else {
                    hit = ev_lower.find(r.needle) != std::string::npos;
                }
                if (!hit) continue;
                Activity matched{
                    r.is_regex ? expand(r.details, m) : r.details,
                    r.is_regex ? expand(r.state, m) : r.state,
                };
                if (r.is_temp) {
                    desired = matched;
                    in_temp = true;
                    temp_until = Clock::now() + std::chrono::seconds(15);
                } else {
                    persistent = matched;
                    in_temp = false;
                }
                break;
            }
        }

        if (in_temp && Clock::now() >= temp_until) {
            in_temp = false;
        }

        if (level_won) {
            desired = Activity{g_is_spanish ? "Nivel completado" : "Level Completed",
                               g_is_spanish ? "Recogiendo recompensa" : "Collecting reward"};
        } else if (!in_temp) {
            desired = persistent;
            const bool is_level_activity =
                (persistent.details != def.details &&
                 persistent.details != "En el mapa" &&
                 persistent.details != "On World Map" &&
                 persistent.details != "En el Almanaque" &&
                 persistent.details != "In Almanac" &&
                 persistent.details != "En la Tienda" &&
                 persistent.details != "In Shop" &&
                 persistent.details != "Jardín Zen" &&
                 persistent.details != "Zen Garden" &&
                 persistent.details != "Tronco de Viaje" &&
                 persistent.details != "Travel Log" &&
                 persistent.details != "Plants vs. Zombies 2");

            if (is_level_activity) {
                const bool is_boss = (desired.state.rfind("Zombot", 0) == 0 ||
                                      desired.state.find("Dr. Zomboss") != std::string::npos ||
                                      is_boss_level_id(current_level));

                if (is_boss) {
                    const int max_phases = (current_level.find("eighties") != std::string::npos ||
                                            desired.state.find("Multiescenario") != std::string::npos ||
                                            desired.state.find("Multi-stage") != std::string::npos)
                                               ? 5
                                               : 3;
                    const int cur_phase = (boss_phase > 0) ? (boss_phase > max_phases ? max_phases : boss_phase) : 1;
                    std::string extra;
                    if (is_paused && is_chooser) {
                        extra = g_is_spanish ? " - Eligiendo plantas - En pausa" : " - Choosing seeds - Paused";
                    } else if (is_paused) {
                        extra = g_is_spanish ? " - En pausa" : " - Paused";
                    } else if (is_chooser) {
                        extra = g_is_spanish ? " - Eligiendo plantas" : " - Choosing seeds";
                    }
                    const std::string phase_str = g_is_spanish
                                                      ? (" (Fase " + std::to_string(cur_phase) + "/" + std::to_string(max_phases) + extra + ")")
                                                      : (" (Phase " + std::to_string(cur_phase) + "/" + std::to_string(max_phases) + extra + ")");
                    desired.state += phase_str;
                } else {
                    if (goals_count > 0 &&
                        (desired.state.rfind("Día ", 0) == 0 || desired.state.rfind("Dia ", 0) == 0 ||
                         desired.state.rfind("Day ", 0) == 0 || desired.state.rfind("Noche ", 0) == 0 ||
                         desired.state.rfind("Night ", 0) == 0)) {
                        std::string stars;
                        const int n = goals_count > 3 ? 3 : goals_count;
                        for (int i = 0; i < n; ++i) stars += "⭐";
                        desired.state += " " + stars;
                    }

                    if (is_paused && is_chooser) {
                        desired.state = desired.state.empty()
                                            ? (g_is_spanish ? "Eligiendo plantas (En pausa)" : "Choosing seeds (Paused)")
                                            : desired.state + (g_is_spanish ? " (Eligiendo plantas - En pausa)" : " (Choosing seeds - Paused)");
                    } else if (is_paused) {
                        desired.state = desired.state.empty() ? (g_is_spanish ? "En pausa" : "Paused")
                                                              : desired.state + (g_is_spanish ? " (En pausa)" : " (Paused)");
                    } else if (is_chooser) {
                        desired.state =
                            desired.state.empty() ? (g_is_spanish ? "Eligiendo plantas" : "Choosing seeds")
                                                  : desired.state + (g_is_spanish ? " (Eligiendo plantas)" : " (Choosing seeds)");
                    }
                }
            }
        }

        if (ipc.is_open()) drain(ipc);

        if (!ipc.is_open()) {
            have_sent = false;
            if (Clock::now() - last_connect >= std::chrono::milliseconds(kReconnectMs)) {
                last_connect = Clock::now();
                if (ipc.connect() && !handshake(ipc)) ipc.close();
                if (ipc.is_open()) std::printf("[discord] connected\n");
            }
        }

        if (ipc.is_open() && (!have_sent || desired != sent) &&
            Clock::now() - last_send >= std::chrono::milliseconds(kMinSendMs)) {
            if (ipc.send(1, activity_json(&desired, start, nonce++))) {
                sent = desired;
                have_sent = true;
                last_send = Clock::now();
            }
        }
    }

    if (ipc.is_open()) {
        ipc.send(1, activity_json(nullptr, 0, nonce++));
        drain(ipc);
    }
    if (trace) std::fclose(trace);
}

}  // namespace
}  // namespace pvz2native

extern "C" {

void pvz2_presence_start(void) {
    using namespace pvz2native;
    const pvz2_config_t *cfg = pvz2_config();
    if (!cfg->discord_enabled) return;
    if (cfg->discord_client_id[0] == '\0') {
        std::printf("[discord] enabled but [discord] client_id is empty -- presence stays off\n");
        return;
    }
    if (g_thread.joinable()) return;

    g_is_spanish = (cfg->user_locale[0] != '\0' && std::strncmp(cfg->user_locale, "es", 2) == 0);
    g_client_id = cfg->discord_client_id;
    g_image = cfg->discord_image;
    g_trace = cfg->discord_trace != 0;

    if (cfg->discord_rules[0] != '\0' &&
        std::strcmp(cfg->discord_rules, "presence_rules.txt") != 0) {
        g_rules_path = resolve(cfg->discord_rules);
    } else {
        const std::string pref = g_is_spanish ? "presence_rules_es.txt" : "presence_rules_en.txt";
        const std::string rpref = resolve(pref);
        FILE *f = std::fopen(rpref.c_str(), "rb");
        if (f) {
            std::fclose(f);
            g_rules_path = rpref;
        } else {
            g_rules_path = resolve("presence_rules.txt");
        }
    }

    g_trace_path = resolve(cfg->discord_trace_file[0] ? cfg->discord_trace_file : "presence_trace.txt");

    g_stop = false;
    g_on = true;
    g_thread = std::thread(worker);
}

int pvz2_presence_enabled(void) { return pvz2native::g_on.load(std::memory_order_relaxed) ? 1 : 0; }

void pvz2_presence_note(const char *source, const char *text) {
    using namespace pvz2native;
    if (!g_on.load(std::memory_order_relaxed) || !source || !text) return;
    std::string ev = std::string(source) + ":" + text;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_queue.size() >= kQueueMax) g_queue.pop_front();
    g_queue.push_back(std::move(ev));
    g_cv.notify_one();
}

void pvz2_presence_stop(void) {
    using namespace pvz2native;
    if (!g_thread.joinable()) return;
    g_on = false;
    g_stop = true;
    g_cv.notify_all();
    g_thread.join();
}

int pvz2_presence_get_user(char *id_out, unsigned id_cap, char *name_out, unsigned name_cap) {
    using namespace pvz2native;
    std::lock_guard<std::mutex> lk(g_user_mu);
    if (g_user_id.empty()) {
        if (id_out && id_cap > 0) id_out[0] = '\0';
        if (name_out && name_cap > 0) name_out[0] = '\0';
        return 0;
    }
    if (id_out && id_cap > 0) std::snprintf(id_out, id_cap, "%s", g_user_id.c_str());
    if (name_out && name_cap > 0) std::snprintf(name_out, name_cap, "%s", g_username.c_str());
    return 1;
}

int pvz2_presence_auth_now(char *id_out, unsigned id_cap, char *name_out, unsigned name_cap) {
    using namespace pvz2native;
    if (pvz2_presence_get_user(id_out, id_cap, name_out, name_cap)) return 1;
    Ipc ipc;
    if (ipc.connect()) {
        handshake(ipc);
        ipc.close();
    }
    return pvz2_presence_get_user(id_out, id_cap, name_out, name_cap);
}

}  // extern "C"

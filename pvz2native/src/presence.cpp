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
    std::string large_image;
    std::string large_text;
    std::string small_image;
    std::string small_text;
    bool operator==(const Activity &o) const {
        return details == o.details && state == o.state &&
               large_image == o.large_image && large_text == o.large_text &&
               small_image == o.small_image && small_text == o.small_text;
    }
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

enum class PresLang { EN, ES, FR, DE, PT, IT };
PresLang g_lang = PresLang::EN;

struct PresStrings {
    const char *rules_file;
    const char *main_menu;
    const char *level_completed;
    const char *collecting_reward;
    const char *phase_word;
    const char *choosing_seeds;
    const char *paused;
    const char *world_map;
    const char *almanac;
    const char *shop;
    const char *zen_garden;
    const char *travel_log;
    const char *viewing_quests;
    const char *watching_credits;
};

static const PresStrings kPresEN = {
    "presence_rules_en.txt", "In Main Menu", "Level Completed", "Collecting reward",
    "Phase", "Choosing seeds", "Paused",
    "On World Map", "In Almanac", "In Shop", "Zen Garden", "Travel Log",
    "Viewing quests", "Watching credits"
};
static const PresStrings kPresES = {
    "presence_rules_es.txt", "En el menú principal", "Nivel completado", "Recogiendo recompensa",
    "Fase", "Eligiendo plantas", "En pausa",
    "En el mapa", "En el Almanaque", "En la Tienda", "Jardín Zen", "Tronco de Viaje",
    "Viendo misiones", "Viendo los créditos"
};
static const PresStrings kPresFR = {
    "presence_rules_fr.txt", "Dans le menu principal", "Niveau terminé", "Récupération de la récompense",
    "Phase", "Choix des plantes", "En pause",
    "Sur la carte", "Dans l'Almanach", "Dans la Boutique", "Jardin zen", "Carnet de voyage",
    "Consulte les quêtes", "Regarde les crédits"
};
static const PresStrings kPresDE = {
    "presence_rules_de.txt", "Im Hauptmenü", "Level abgeschlossen", "Belohnung einsammeln",
    "Phase", "Pflanzenauswahl", "Pausiert",
    "Auf der Weltkarte", "Im Lexikon", "Im Laden", "Zen-Garten", "Reiseklotz",
    "Sieht Quests an", "Sieht den Abspann"
};
static const PresStrings kPresPT = {
    "presence_rules_pt.txt", "No menu principal", "Nível concluído", "Coletando recompensa",
    "Fase", "Escolhendo plantas", "Em pausa",
    "No mapa", "No Almanaque", "Na Loja", "Jardim Zen", "Diário de Viagem",
    "Vendo missões", "Vendo os créditos"
};
static const PresStrings kPresIT = {
    "presence_rules_it.txt", "Nel menu principale", "Livello completato", "Riscossione ricompensa",
    "Fase", "Scelta delle piante", "In pausa",
    "Sulla mappa", "Nell'Almanacco", "Nel Negozio", "Giardino zen", "Diario di viaggio",
    "Guardando le missioni", "Guardando i crediti"
};

const PresStrings *pres_str() {
    switch (g_lang) {
        case PresLang::ES: return &kPresES;
        case PresLang::FR: return &kPresFR;
        case PresLang::DE: return &kPresDE;
        case PresLang::PT: return &kPresPT;
        case PresLang::IT: return &kPresIT;
        default:           return &kPresEN;
    }
}

std::vector<Rule> load_rules(const std::string &path, Activity &def) {
    std::vector<Rule> rules;
    def = Activity{"Plants vs. Zombies 2", pres_str()->main_menu};

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
bool g_world_icons = true;
std::string g_world_icon_url = "https://raw.githubusercontent.com/Onyxdum/PvZ2NativeWDRPC/main/assets/discord/v2/";
std::string g_button_url = "https://github.com/Onyxdum/PvZ2NativeWDRPC";
std::string g_button_label = "GitHub";

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
        if (!a->large_image.empty()) {
            j += ",\"assets\":{\"large_image\":\"" + json_escape(a->large_image) + "\"";
            if (!a->large_text.empty()) {
                j += ",\"large_text\":\"" + json_escape(clip(a->large_text, kFieldMax)) + "\"";
            }
            if (!a->small_image.empty()) {
                j += ",\"small_image\":\"" + json_escape(a->small_image) + "\"";
                if (!a->small_text.empty()) {
                    j += ",\"small_text\":\"" + json_escape(clip(a->small_text, kFieldMax)) + "\"";
                }
            }
            j += "}";
        } else if (!g_image.empty()) {
            j += ",\"assets\":{\"large_image\":\"" + json_escape(g_image) +
                 "\",\"large_text\":\"PvZ2Native\"}";
        }
        if (!g_button_url.empty() && g_button_url != "0" && g_button_url != "none") {
            const std::string lbl = !g_button_label.empty() ? g_button_label : "GitHub";
            j += ",\"buttons\":[{\"label\":\"" + json_escape(clip(lbl, 32)) + "\",\"url\":\"" + json_escape(g_button_url) + "\"}]";
        }
        j += "}";
    }
    j += "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
    return j;
}

std::string get_icon_uri(const std::string &key) {
    if (g_world_icon_url == "asset" || g_world_icon_url == "assets") {
        return key;
    }
    std::string base = g_world_icon_url;
    if (!base.empty() && base.back() != '/') base += '/';
    return base + key + ".webp";
}

void resolve_activity_assets(Activity &act, const std::string &cur_level, const std::string &cur_world, bool level_won) {
    const std::string default_pvz = get_icon_uri(!g_image.empty() ? g_image : "pvz2");
    if (!g_world_icons) {
        act.large_image = default_pvz;
        act.large_text = "Plants vs. Zombies 2";
        act.small_image.clear();
        act.small_text.clear();
        return;
    }

    std::string icon_key;
    std::string world_name;

    const std::string lvl_low = lower(cur_level);
    const std::string wrd_low = lower(cur_world);
    const std::string det_low = lower(act.details);
    const std::string st_low = lower(act.state);

    const bool is_world_map = (det_low.find("map") != std::string::npos ||
                               det_low.find("carte") != std::string::npos);

    auto matches = [&](const char *s) -> bool {
        if (lvl_low.find(s) != std::string::npos ||
            wrd_low.find(s) != std::string::npos ||
            det_low.find(s) != std::string::npos) {
            return true;
        }
        // When on the world map, act.state holds the localized world name
        if (is_world_map && st_low.find(s) != std::string::npos) {
            return true;
        }
        return false;
    };

    if (matches("modern") || matches("tiempos modernos") || matches("modern day") ||
        matches("temps modernes") || matches("moderne") || matches("vida moderna") ||
        matches("tempi moderni")) {
        icon_key = "tiempos_modernos";
        world_name = (g_lang == PresLang::ES) ? "Tiempos Modernos" : "Modern Day";
    } else if (matches("lostcity") || matches("ciudad perdida") || matches("lost city") ||
               matches("perdue") || matches("perduta") || matches("verlorene stadt") ||
               matches("cidade perdida")) {
        icon_key = "ciudad_perdida";
        world_name = (g_lang == PresLang::ES) ? "Ciudad Perdida" : "Lost City";
    } else if (matches("iceage") || matches("cuevas congeladas") || matches("frostbite caves") ||
               matches("grottes glaciaires") || matches("frostbeulen") || matches("geladura") ||
               matches("grotte congelate")) {
        icon_key = "cuevas_congeladas";
        world_name = (g_lang == PresLang::ES) ? "Cuevas Congeladas" : "Frostbite Caves";
    } else if (matches("dark") || matches("edad oscura") || matches("dark ages") ||
               matches("tenebres") || matches("mittelalter") ||
               matches("idade das trevas") || matches("secoli bui")) {
        icon_key = "edad_oscura";
        world_name = (g_lang == PresLang::ES) ? "Edad Oscura" : "Dark Ages";
    } else if (matches("future") || matches("futuro lejano") || matches("far future") ||
               matches("futur") || matches("ferne zukunft") ||
               matches("futuro distante") || matches("futuro lontano")) {
        icon_key = "futuro_lejano";
        world_name = (g_lang == PresLang::ES) ? "Futuro Lejano" : "Far Future";
    } else if (matches("dino") || matches("pantano") || matches("jurassic") ||
               matches("jurassisches") || matches("juras") || matches("giurassico")) {
        icon_key = "pantano_jurasico";
        world_name = (g_lang == PresLang::ES) ? "Pantano del Jurásico" : "Jurassic Marsh";
    } else if (matches("beach") || matches("playa") || matches("big wave") ||
               matches("deferlantes") || matches("riesenwellen") ||
               matches("boa onda") || matches("spiaggia")) {
        icon_key = "playa_gran_ola";
        world_name = (g_lang == PresLang::ES) ? "Playa de la Gran Ola" : "Big Wave Beach";
    } else if (matches("eighties") || matches("vueltacasete") || matches("neon mixtape") ||
               matches("fluotonik") || matches("neon-mixtape") || matches("idade da juba") ||
               matches("discomix")) {
        icon_key = "vueltacasete_neon";
        world_name = (g_lang == PresLang::ES) ? "Vueltacasete de Neón" : "Neon Mixtape Tour";
    } else if (lvl_low.rfind("lod_", 0) == 0 || matches("pinata") || matches("piñata")) {
        icon_key = "fiesta_pinatas";
        world_name = (g_lang == PresLang::ES) ? "Fiesta de Piñatas" : "Piñata Party";
    } else if (matches("egypt") || matches("egipto") ||
               matches("pirate") || matches("pirata") ||
               matches("cowboy") || matches("oeste") || matches("west") ||
               matches("tutorial") || matches("intro") || matches("casa del jugador") ||
               matches("player's house")) {
        icon_key = "mundos_no_mencionados";
        if (matches("egypt") || matches("egipto")) {
            world_name = (g_lang == PresLang::ES) ? "Antiguo Egipto" : "Ancient Egypt";
        } else if (matches("pirate") || matches("pirata")) {
            world_name = (g_lang == PresLang::ES) ? "Mares Piratas" : "Pirate Seas";
        } else if (matches("cowboy") || matches("oeste") || matches("west")) {
            world_name = (g_lang == PresLang::ES) ? "Salvaje Oeste" : "Wild West";
        } else {
            world_name = (g_lang == PresLang::ES) ? "Mundo Clásico" : "Classic World";
        }
    }

    if (!icon_key.empty()) {
        act.large_image = get_icon_uri(icon_key);
        act.large_text = world_name;
    } else {
        act.large_image = default_pvz;
        act.large_text = "Plants vs. Zombies 2";
    }

    /* Small badge icons for specific activities */
    if (level_won || det_low.find("nivel completado") != std::string::npos ||
        det_low.find("level completed") != std::string::npos ||
        st_low.find("recompensa") != std::string::npos ||
        st_low.find("reward") != std::string::npos) {
        act.small_image = get_icon_uri("recompensa");
        act.small_text = (g_lang == PresLang::ES) ? "Recompensa" : "Reward";
    } else if (det_low.find("tronco") != std::string::npos ||
               det_low.find("travel log") != std::string::npos ||
               det_low.find("carnet de voyage") != std::string::npos ||
               det_low.find("reiseklotz") != std::string::npos ||
               det_low.find("diario di viaggio") != std::string::npos ||
               det_low.find("diÃ¡rio de viagem") != std::string::npos ||
               lvl_low.find("challenge_levels") != std::string::npos) {
        act.small_image = get_icon_uri("tronco_viaje");
        act.small_text = (g_lang == PresLang::ES) ? "Tronco de Viaje" : "Travel Log";
    } else if (det_low.find("jardÃ­n zen") != std::string::npos ||
               det_low.find("jardin zen") != std::string::npos ||
               det_low.find("zen garden") != std::string::npos ||
               det_low.find("zen-garten") != std::string::npos ||
               st_low.find("cuidando las plantas") != std::string::npos) {
        act.small_image = get_icon_uri("jardin_zen");
        act.small_text = (g_lang == PresLang::ES) ? "JardÃ­n Zen" : "Zen Garden";
    } else if (lvl_low.find("vasebreaker") != std::string::npos ||
               det_low.find("rompejarrones") != std::string::npos ||
               det_low.find("vasebreaker") != std::string::npos ||
               det_low.find("vasenbrecher") != std::string::npos ||
               det_low.find("casse-vase") != std::string::npos ||
               det_low.find("spaccavasi") != std::string::npos ||
               det_low.find("quebravaso") != std::string::npos) {
        act.small_image = get_icon_uri("rompejarrones");
        act.small_text = (g_lang == PresLang::ES) ? "Rompejarrones" : "Vasebreaker";
    } else if (det_low.find("almanaque") != std::string::npos ||
               det_low.find("almanac") != std::string::npos ||
               det_low.find("almanach") != std::string::npos ||
               det_low.find("almanacco") != std::string::npos ||
               det_low.find("lexikon") != std::string::npos ||
               st_low.find("plantas y zombis") != std::string::npos ||
               st_low.find("suburban almanac") != std::string::npos ||
               st_low.find("almanaque suburbano") != std::string::npos) {
        act.small_image = get_icon_uri("almanaque");
        act.small_text = (g_lang == PresLang::ES || g_lang == PresLang::PT) ? "Almanaque" :
                         (g_lang == PresLang::DE) ? "Lexikon" :
                         (g_lang == PresLang::FR) ? "Almanach" :
                         (g_lang == PresLang::IT) ? "Almanacco" : "Almanac";
    } else {
        act.small_image.clear();
        act.small_text.clear();
    }
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

    std::string custom_path = resolve("presence_rules_custom.txt");
    FILE *fc = std::fopen(custom_path.c_str(), "rb");
    if (fc != nullptr) {
        std::fclose(fc);
        Activity custom_def;
        std::vector<Rule> custom_rules = load_rules(custom_path, custom_def);
        if (!custom_rules.empty()) {
            std::printf("[discord] %zu custom rule(s) from %s loaded\n", custom_rules.size(), custom_path.c_str());
            rules.insert(rules.begin(), custom_rules.begin(), custom_rules.end());
            if (!custom_def.details.empty() || !custom_def.state.empty()) {
                def = custom_def;
            }
        }
    }

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
    bool is_eighties_boss = false;
    bool travel_log_open = false;
    bool vasebreaker_menu_open = false;
    bool credits_open = false;
    std::string boss_name_detected;
    std::string current_level;
    std::string current_world;
    std::string current_game_state;
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

            /* Only detect boss from events if not already locked in or if phase > 3 */
            if (boss_name_detected.empty() || boss_phase > 3) {
                if (ev_lower.find("eighties") != std::string::npos ||
                    ev_lower.find("zombie_eighties") != std::string::npos ||
                    ev_lower.find("modern_zomboss_09") != std::string::npos ||
                    ev_lower.find("multiescenario") != std::string::npos ||
                    ev_lower.find("multi-stage") != std::string::npos) {
                    is_eighties_boss = true;
                    boss_name_detected = "eighties";
                } else if (ev_lower.find("zombie_pirate") != std::string::npos ||
                           ev_lower.find("modern_zomboss_02") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "pirate";
                } else if (ev_lower.find("zombie_egypt") != std::string::npos ||
                           ev_lower.find("modern_zomboss_01") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "egypt";
                } else if (ev_lower.find("zombie_cowboy") != std::string::npos ||
                           ev_lower.find("modern_zomboss_03") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "cowboy";
                } else if (ev_lower.find("zombie_future") != std::string::npos ||
                           ev_lower.find("modern_zomboss_04") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "future";
                } else if (ev_lower.find("zombie_dark") != std::string::npos ||
                           ev_lower.find("modern_zomboss_05") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "dark";
                } else if (ev_lower.find("zombie_beach") != std::string::npos ||
                           ev_lower.find("modern_zomboss_06") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "beach";
                } else if (ev_lower.find("zombie_iceage") != std::string::npos ||
                           ev_lower.find("modern_zomboss_07") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "iceage";
                } else if (ev_lower.find("zombie_lostcity") != std::string::npos ||
                           ev_lower.find("modern_zomboss_08") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "lostcity";
                } else if (ev_lower.find("zombie_dino") != std::string::npos ||
                           ev_lower.find("modern_zomboss_10") != std::string::npos) {
                    is_eighties_boss = false;
                    boss_name_detected = "dino";
                }
            }

            const auto wave_pos = ev.find("Set_Switch_Zomboss_Wave_");
            if (wave_pos != std::string::npos && wave_pos + 24 < ev.size()) {
                const char digit = ev[wave_pos + 24];
                if (digit >= '1' && digit <= '5') {
                    const int w = digit - '0';
                    if (w > boss_phase) boss_phase = w;
                    if (w >= 4) {
                        is_eighties_boss = true;
                        boss_name_detected = "eighties";
                    }
                }
            }

            static std::string last_known_world;
            const PresStrings *ps_ev = pres_str();

            auto restore_world_activity = [&]() {
                const std::string tgt_world = lower(!current_world.empty() ? current_world : last_known_world);
                if (!tgt_world.empty()) {
                    const std::string syn_ev = "state:worldmap current world=" + tgt_world;
                    for (const Rule &r : rules) {
                        if (!r.is_temp && !r.is_regex && !r.needle.empty() && syn_ev.find(r.needle) != std::string::npos) {
                            persistent = Activity{r.details, r.state};
                            desired = persistent;
                            return;
                        }
                    }
                }
                for (const Rule &r : rules) {
                    if (!r.is_temp && !r.is_regex && !r.needle.empty() && r.needle.find("universemap shown=true") != std::string::npos) {
                        persistent = Activity{r.details, r.state};
                        desired = persistent;
                        return;
                    }
                }
                persistent = def;
                desired = def;
            };

            if (ev.rfind("state:Board Level=", 0) == 0) {
                travel_log_open = false;
                vasebreaker_menu_open = false;
                credits_open = false;
                std::string lvl_raw = ev.substr(18);
                const auto colon = lvl_raw.find(':');
                current_level = (colon != std::string::npos) ? lvl_raw.substr(0, colon) : lvl_raw;
                if (is_boss_level_id(current_level)) {
                    if (boss_phase == 0) boss_phase = 1;
                    if (current_level.find("eighties") != std::string::npos ||
                        current_level == "modern_zomboss_09_eighties") {
                        is_eighties_boss = true;
                        boss_name_detected = "eighties";
                    }
                } else {
                    boss_phase = 0;
                    is_eighties_boss = false;
                    boss_name_detected.clear();
                }
            } else if (ev.rfind("state:Board BossPhase=", 0) == 0) {
                try {
                    boss_phase = std::stoi(ev.substr(22));
                    if (boss_phase > 3) {
                        is_eighties_boss = true;
                        boss_name_detected = "eighties";
                    }
                } catch (...) {
                    boss_phase = 1;
                }
                continue;
            } else if (ev == "state:Board EightiesBoss=true") {
                if (boss_name_detected.empty() || boss_phase > 3) {
                    is_eighties_boss = true;
                    boss_name_detected = "eighties";
                }
                continue;
            } else if (ev.rfind("state:Board BossName=", 0) == 0) {
                std::string bname = ev.substr(21);
                if (boss_name_detected.empty() || boss_phase > 3) {
                    boss_name_detected = bname;
                    if (boss_name_detected == "eighties") is_eighties_boss = true;
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
            } else if (ev.find("UI_SeedChooser") != std::string::npos ||
                       ev.find("SeedChooser_Arrive") != std::string::npos) {
                if (!current_level.empty()) {
                    is_chooser = true;
                }
            } else if (ev.find("SeedChooser_Away") != std::string::npos) {
                is_chooser = false;
            } else if (ev == "state:DialogClose=PinataReward") {
                in_temp = false;
                restore_world_activity();
                continue;
            } else if (ev == "state:Board State=LoadingLevel") {
                level_won = false;
                travel_log_open = false;
                vasebreaker_menu_open = false;
                credits_open = false;
                if (is_boss_level_id(current_level)) {
                    if (boss_phase == 0) boss_phase = 1;
                    if (current_level.find("eighties") != std::string::npos ||
                        current_level == "modern_zomboss_09_eighties") {
                        is_eighties_boss = true;
                        boss_name_detected = "eighties";
                    }
                } else {
                    boss_phase = 0;
                    is_eighties_boss = false;
                    boss_name_detected.clear();
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

            if (ev == "state:TravelLog Shown=true") {
                if (current_level.empty() && !level_won) {
                    travel_log_open = true;
                    in_temp = false;
                    persistent = Activity{ps_ev->travel_log, ps_ev->viewing_quests};
                    desired = persistent;
                }
                continue;
            } else if (ev == "state:TravelLog Shown=false") {
                travel_log_open = false;
                in_temp = false;
                if (current_level.empty() && !level_won && !credits_open) {
                    restore_world_activity();
                }
                continue;
            } else if (ev == "state:Credits Shown=true") {
                credits_open = true;
                level_won = false;
                travel_log_open = false;
                vasebreaker_menu_open = false;
                in_temp = false;
                persistent = Activity{"Plants vs. Zombies 2", ps_ev->watching_credits};
                desired = persistent;
                continue;
            } else if (ev == "state:Credits Shown=false") {
                credits_open = false;
                in_temp = false;
                continue;
            }

            auto delete_groups_has_exact_dialog = [&](const std::string &s) -> bool {
                constexpr const char *kPrefix = "state:Last DeleteGroups=";
                constexpr std::size_t kPrefixLen = 24;
                if (s.rfind(kPrefix, 0) != 0) return false;
                std::size_t pos = kPrefixLen;
                while (pos < s.size()) {
                    std::size_t comma = s.find(',', pos);
                    if (comma == std::string::npos) comma = s.size();
                    const std::string tok = trim(s.substr(pos, comma - pos));
                    if (tok == "UI_Almanac") {
                        if (current_game_state == "GAME_Almanac") return false;
                        return true;
                    }
                    if (tok == "UI_Store") {
                        if (current_game_state == "GAME_Store") return false;
                        return true;
                    }
                    if (tok == "UI_VaseBreakerMenu" || tok == "UI_Quests") {
                        return true;
                    }
                    pos = comma + 1;
                }
                return false;
            };

            const bool is_dialog_close = (ev.find("Play_UI_Button_X_Close_") != std::string::npos ||
                                          ev.find("Play_UI_Menu_Cancel_") != std::string::npos ||
                                          ev.find("Play_UI_Menu_Leave_") != std::string::npos ||
                                          ev.find("Play_UI_Menu_Store_Close") != std::string::npos ||
                                          ev.find("Play_UI_Menu_Almanac_Close_") != std::string::npos ||
                                          delete_groups_has_exact_dialog(ev));
            if (is_dialog_close) {
                if (travel_log_open) {
                    travel_log_open = false;
                    in_temp = false;
                    if (current_level.empty() && !level_won && !credits_open) {
                        restore_world_activity();
                    }
                }
                if (ev.find("Play_UI_Menu_Store_Close") != std::string::npos) {
                    if (current_game_state == "GAME_Store") current_game_state.clear();
                    restore_world_activity();
                } else if (current_game_state != "GAME_Store" && current_game_state != "GAME_Almanac") {
                    const std::string cur_det = lower(persistent.details);
                    const std::string des_det = lower(desired.details);
                    if (cur_det.find("rompejarrones") != std::string::npos ||
                        cur_det.find("vasebreaker") != std::string::npos ||
                        cur_det.find("almanaque") != std::string::npos ||
                        cur_det.find("almanac") != std::string::npos ||
                        cur_det.find("tienda") != std::string::npos ||
                        cur_det.find("store") != std::string::npos ||
                        des_det.find("rompejarrones") != std::string::npos ||
                        des_det.find("vasebreaker") != std::string::npos ||
                        des_det.find("almanaque") != std::string::npos ||
                        des_det.find("almanac") != std::string::npos ||
                        des_det.find("tienda") != std::string::npos ||
                        des_det.find("store") != std::string::npos ||
                        vasebreaker_menu_open ||
                        in_temp) {
                        vasebreaker_menu_open = false;
                        in_temp = false;
                        restore_world_activity();
                    }
                }
            }

            if (ev.rfind("state:GameStateMgr State=", 0) == 0) {
                current_game_state = ev.substr(25);
            }

            if (ev.rfind("state:WorldMap Current World=", 0) == 0) {
                current_world = ev.substr(29);
                last_known_world = current_world;
                is_paused = false;
                is_chooser = false;
                goals_count = 0;
                boss_phase = 0;
                level_won = false;
                is_eighties_boss = false;
                boss_name_detected.clear();
                current_level.clear();
                if (!travel_log_open && !vasebreaker_menu_open && !credits_open) {
                    in_temp = false;
                }
            } else if (ev.rfind("state:GameStateMgr State=GAME_WorldMap", 0) == 0) {
                if (current_world.empty() && !last_known_world.empty()) {
                    current_world = last_known_world;
                }
                is_paused = false;
                is_chooser = false;
                goals_count = 0;
                boss_phase = 0;
                level_won = false;
                is_eighties_boss = false;
                travel_log_open = false;
                vasebreaker_menu_open = false;
                boss_name_detected.clear();
                current_level.clear();
                in_temp = false;
                if (!credits_open) {
                    restore_world_activity();
                }
            } else if (ev.rfind("state:UniverseMap Shown=false", 0) == 0) {
                if (current_world.empty() && !last_known_world.empty()) {
                    current_world = last_known_world;
                }
                travel_log_open = false;
                in_temp = false;
                if (!vasebreaker_menu_open && !credits_open) {
                    restore_world_activity();
                }
                if (!travel_log_open && !vasebreaker_menu_open && !credits_open) {
                    restore_world_activity();
                }
            } else if (ev.rfind("state:GameStateMgr State=GAME_ArcadeMenu", 0) == 0 ||
                       ev.rfind("state:ArcadeMenu Shown=true", 0) == 0) {
                vasebreaker_menu_open = true;
                travel_log_open = false;
                credits_open = false;
                current_level.clear();
                in_temp = false;
            } else if (ev.rfind("state:ArcadeMenu Shown=false", 0) == 0) {
                vasebreaker_menu_open = false;
                in_temp = false;
                restore_world_activity();
            } else if (ev.rfind("state:GameStateMgr State=GAME_Almanac", 0) == 0 ||
                       ev.rfind("state:GameStateMgr State=GAME_Store", 0) == 0 ||
                       ev.rfind("state:GameStateMgr State=GAME_ZenGarden", 0) == 0) {
                travel_log_open = false;
                vasebreaker_menu_open = false;
                credits_open = false;
                in_temp = false;
            } else if (ev.rfind("state:GameStateMgr State=GAME_MainMenu", 0) == 0 ||
                       ev.rfind("state:GameStateMgr State=GAME_JoustOrAdventureSelector", 0) == 0) {
                is_paused = false;
                is_chooser = false;
                goals_count = 0;
                boss_phase = 0;
                level_won = false;
                is_eighties_boss = false;
                travel_log_open = false;
                vasebreaker_menu_open = false;
                credits_open = false;
                boss_name_detected.clear();
                current_level.clear();
                current_world.clear();
                in_temp = false;
            } else if (ev.rfind("state:GameStateMgr State=GAME_JoustLoadDashboard", 0) == 0) {
                is_paused = false;
                is_chooser = false;
                goals_count = 0;
                boss_phase = 0;
                level_won = false;
                is_eighties_boss = false;
                travel_log_open = false;
                vasebreaker_menu_open = false;
                credits_open = false;
                boss_name_detected.clear();
                current_level.clear();
                in_temp = false;
            } else if (ev.rfind("state:UniverseMap Shown=true", 0) == 0) {
                is_paused = false;
                is_chooser = false;
                goals_count = 0;
                boss_phase = 0;
                level_won = false;
                is_eighties_boss = false;
                travel_log_open = false;
                vasebreaker_menu_open = false;
                credits_open = false;
                boss_name_detected.clear();
                current_level.clear();
                current_world.clear();
                in_temp = false;
                for (const Rule &r : rules) {
                    if (!r.is_temp && !r.is_regex && !r.needle.empty() && r.needle.find("universemap shown=true") != std::string::npos) {
                        persistent = Activity{r.details, r.state};
                        desired = persistent;
                        break;
                    }
                }
            }

            /* Prevent background map streaming or victory quest loads from overwriting active modal dialogs */
            if ((level_won || !current_level.empty()) && ev_lower.rfind("rsb:ui_quests_", 0) == 0) {
                continue;
            }
            if ((travel_log_open || vasebreaker_menu_open || credits_open) &&
                (ev_lower.rfind("rsb:worldmap_", 0) == 0 ||
                 ev_lower.rfind("state:worldmap current world=", 0) == 0 ||
                 ev_lower.rfind("rsb:ui_mainmenu_", 0) == 0 ||
                 (vasebreaker_menu_open && ev_lower.find("almanac") != std::string::npos))) {
                continue;
            }
            if (credits_open && ev_lower.rfind("state:gamestatemgr state=game_mainmenu", 0) == 0) {
                continue;
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
                if (ev_lower.rfind("rsb:ui_quests_", 0) == 0 && current_level.empty() && !level_won) {
                    travel_log_open = true;
                } else if (ev_lower.rfind("rsb:ui_vasebreakermenu_", 0) == 0 && current_level.empty()) {
                    vasebreaker_menu_open = true;
                }
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

        const PresStrings *ps = pres_str();
        if (level_won) {
            desired = Activity{ps->level_completed, ps->collecting_reward};
        } else if (!in_temp) {
            desired = persistent;
            const bool is_level_activity =
                (persistent.details != def.details &&
                 persistent.details != ps->world_map &&
                 persistent.details != ps->almanac &&
                 persistent.details != ps->shop &&
                 persistent.details != ps->zen_garden &&
                 persistent.details != ps->travel_log &&
                 persistent.details != "Plants vs. Zombies 2");

            if (is_level_activity) {
                const std::string st_low = lower(desired.state);
                const bool is_boss = (st_low.find("zombot") != std::string::npos ||
                                      st_low.find("zomboss") != std::string::npos ||
                                      desired.state.find("Zumbô") != std::string::npos ||
                                      desired.state.find("Zumbão") != std::string::npos ||
                                      is_boss_level_id(current_level));

                if (is_boss) {
                    const bool is_eighties = is_eighties_boss ||
                                             (boss_name_detected == "eighties") ||
                                             (current_level.find("eighties") != std::string::npos) ||
                                             (desired.state.find("Multiescenario") != std::string::npos) ||
                                             (desired.state.find("Multi-stage") != std::string::npos) ||
                                             (desired.state.find("multipiste") != std::string::npos) ||
                                             (desired.state.find("Rampensau") != std::string::npos) ||
                                             (desired.state.find("Multimistureba") != std::string::npos) ||
                                             (desired.state.find("Multifasico") != std::string::npos);
                    const int max_phases = (is_eighties || boss_phase > 3) ? 5 : 3;
                    const int cur_phase = (boss_phase > 0) ? (boss_phase > max_phases ? max_phases : boss_phase) : 1;

                    if (is_eighties && current_level.rfind("modern", 0) == 0) {
                        if (g_rules_path.find("_pt") != std::string::npos) desired.state = "Zumbô Multimistureba";
                        else if (g_rules_path.find("_de") != std::string::npos) desired.state = "Zombot-Rampensau";
                        else if (g_rules_path.find("_fr") != std::string::npos) desired.state = "Zombot multipiste";
                        else if (g_rules_path.find("_it") != std::string::npos) desired.state = "Schiacciaverdure Multifasico Zombot";
                        else if (g_rules_path.find("_en") != std::string::npos) desired.state = "Multi-stage Masher";
                        else desired.state = "Zombot Machacante Multiescenario";
                    } else if (!boss_name_detected.empty() && current_level.rfind("modern", 0) == 0) {
                        if (boss_name_detected == "egypt") desired.state = "Zombot Esfinge Letal";
                        else if (boss_name_detected == "pirate") desired.state = "Zombot Corsario";
                        else if (boss_name_detected == "cowboy") desired.state = "Zombot Carro Blindado";
                        else if (boss_name_detected == "future") desired.state = "Zombot Mañanatrón";
                        else if (boss_name_detected == "dark") desired.state = "Zombot Dragón Oscuro";
                        else if (boss_name_detected == "beach") desired.state = "Zombot Subtiburotrónico";
                        else if (boss_name_detected == "iceage") desired.state = "Zombot Maestro del Colmillo 10.000 a.C.";
                        else if (boss_name_detected == "lostcity") desired.state = "Zombot Góndola Aerostática";
                        else if (boss_name_detected == "dino") desired.state = "Zombot Dinotrónico Mechasaurio";
                    }
                    std::string extra;
                    if (is_paused && is_chooser) {
                        extra = std::string(" - ") + ps->choosing_seeds + " - " + ps->paused;
                    } else if (is_paused) {
                        extra = std::string(" - ") + ps->paused;
                    } else if (is_chooser) {
                        extra = std::string(" - ") + ps->choosing_seeds;
                    }
                    const std::string phase_str = std::string(" (") + ps->phase_word + " " +
                                                  std::to_string(cur_phase) + "/" +
                                                  std::to_string(max_phases) + extra + ")";
                    desired.state += phase_str;
                } else {
                    if (goals_count > 0 &&
                        (desired.state.rfind("Día ", 0) == 0 || desired.state.rfind("Dia ", 0) == 0 ||
                         desired.state.rfind("Day ", 0) == 0 || desired.state.rfind("Jour ", 0) == 0 ||
                         desired.state.rfind("Tag ", 0) == 0 || desired.state.rfind("Giorno ", 0) == 0 ||
                         desired.state.rfind("Noche ", 0) == 0 || desired.state.rfind("Night ", 0) == 0 ||
                         desired.state.rfind("Nuit ", 0) == 0 || desired.state.rfind("Nacht ", 0) == 0 ||
                         desired.state.rfind("Noite ", 0) == 0 || desired.state.rfind("Notte ", 0) == 0)) {
                        std::string stars;
                        const int n = goals_count > 3 ? 3 : goals_count;
                        for (int i = 0; i < n; ++i) stars += "⭐";
                        desired.state += " " + stars;
                    }

                    if (is_paused && is_chooser) {
                        desired.state = desired.state.empty()
                                            ? (std::string(ps->choosing_seeds) + " (" + ps->paused + ")")
                                            : desired.state + " (" + ps->choosing_seeds + " - " + ps->paused + ")";
                    } else if (is_paused) {
                        desired.state = desired.state.empty()
                                            ? std::string(ps->paused)
                                            : desired.state + " (" + ps->paused + ")";
                    } else if (is_chooser) {
                        desired.state = desired.state.empty()
                                            ? std::string(ps->choosing_seeds)
                                            : desired.state + " (" + ps->choosing_seeds + ")";
                    }
                }
            }
        }

        resolve_activity_assets(desired, current_level, current_world, level_won);

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

    if (cfg->user_locale[0] != '\0') {
        if (std::strncmp(cfg->user_locale, "es", 2) == 0) g_lang = PresLang::ES;
        else if (std::strncmp(cfg->user_locale, "fr", 2) == 0) g_lang = PresLang::FR;
        else if (std::strncmp(cfg->user_locale, "de", 2) == 0) g_lang = PresLang::DE;
        else if (std::strncmp(cfg->user_locale, "pt", 2) == 0) g_lang = PresLang::PT;
        else if (std::strncmp(cfg->user_locale, "it", 2) == 0) g_lang = PresLang::IT;
        else g_lang = PresLang::EN;
    } else {
        g_lang = PresLang::EN;
    }
    g_client_id = cfg->discord_client_id;
    g_image = cfg->discord_image;
    g_trace = cfg->discord_trace != 0;
    g_world_icons = cfg->discord_world_icons != 0;
    if (cfg->discord_world_icon_url[0] != '\0') {
        std::string u = cfg->discord_world_icon_url;
        if (u == "https://raw.githubusercontent.com/Onyxdum/PvZ2NativeWDRPC/main/assets/discord/") {
            u = "https://raw.githubusercontent.com/Onyxdum/PvZ2NativeWDRPC/main/assets/discord/v2/";
        }
        g_world_icon_url = u;
    }

    if (cfg->discord_button_url[0] != '\0') {
        g_button_url = cfg->discord_button_url;
    } else {
        g_button_url = "https://github.com/Onyxdum/PvZ2NativeWDRPC";
    }
    if (cfg->discord_button_label[0] != '\0') {
        g_button_label = cfg->discord_button_label;
    } else {
        g_button_label = "GitHub";
    }

    if (cfg->discord_rules[0] != '\0' &&
        std::strcmp(cfg->discord_rules, "presence_rules.txt") != 0) {
        g_rules_path = resolve(cfg->discord_rules);
    } else {
        const std::string pref = pres_str()->rules_file;
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

void pvz2_presence_set_enabled(int enable) {
    pvz2_config_update_discord_enabled(enable);
    if (enable) {
        pvz2_presence_start();
    } else {
        pvz2_presence_stop();
    }
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

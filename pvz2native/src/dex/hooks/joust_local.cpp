/* Battlez ("Joust" inside the game) answered from inside the port.
 *
 * Battlez talks to two places, and neither exists any more:
 *
 *   - The CDN: joust_config / joust_schedule / joust_levels / joust_leaderboards / joust_crownrewards /
 *     joust_progressiverewards / joust_tournamentrewards (.json.<maj.min>). Every one of them is also in the
 *     OBB's Packages group as PACKAGES\JOUST_*.RTON, so android_http.cpp serves those like the other CDN
 *     files -- except the schedule, whose last tournament started in May 2018. That one is generated here:
 *     one tournament per week from 2026-01-05 00:00 UTC, cycling the level sets the original rotation used.
 *
 *   - The game server, https://pvz2-gs.awspopcap.com/joust/v1/<route>. The client builds every request
 *     from {path, params, postData, method}; the routes (libPVZ2.so 6.6.1, request builders at
 *     0x6a1a00..0x6a2b80) are:
 *         GET  dashboard?playerId=
 *         POST avatar                 {playerId, name, icon}
 *         POST match                  {playerId, matchId}
 *         POST completeMatch          {matchId, opponentId, winRatio, r{playerId, levelSeed, league, score, replay}}
 *         POST finalizeMatch          {matchId, playerId, streaks{reward,crown,difficulty: "KEEP"}, winRatio}
 *         POST registerForTournament  {playerId}
 *     and the responses are read with typed JSONPath getters (0xd5d7d4 string, 0xd5d918 int64 -- a JSON
 *     number written with a fraction is NOT an int and reads as the default --, 0xd5d9e8 object,
 *     0xd5da14 array). Field list and types: see joust_dashboard_json / joust_match_json below.
 *
 * This file is phase 1 of bringing Battlez back: a local stand-in that answers every route with
 * plausible data and logs each request and answer as `[joust]`, so the protocol can be confirmed screen
 * by screen before it moves to the owner's server (cloud-server/server.py). The opponent is a
 * placeholder with an empty replay, which the game turns into its own AI replay ("Creating AI
 * GeneratedReplay"). The local player's Battlez state (avatar, crowns, wins) is kept in
 * userdata/joust_local.txt. */

#include <pvz2native/cloud_save.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/host_files.h>
#include <pvz2native/log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace pvz2native {
namespace dex {

namespace {

constexpr std::int64_t kScheduleBase = 1767571200; /* Monday 2026-01-05 00:00 UTC */
constexpr std::int64_t kWeek = 7 * 24 * 3600;
constexpr int kScheduleWeeks = 520;                 /* ten years */
/* The level sets the 2017-18 schedule rotated through (joust_levels has 1..27, only these were used). */
constexpr int kLevelSets[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 22, 27};
constexpr int kLevelSetCount = (int)(sizeof(kLevelSets) / sizeof(kLevelSets[0]));

const char *const kProgressiveRewardKeys[] = {
    "ftue_win_streak_1", "soil_win_streak_1",   "wood_win_streak_1", "brick_win_streak_1",
    "iron_win_streak_1", "silver_win_streak_1", "gold_win_streak_1", "jade_win_streak_1",
    "crystal_win_streak_1"};
const char *const kTournamentRewardKeys[] = {
    "ftue_rewards_1", "soil_rewards_1",   "wood_rewards_1", "brick_rewards_1", "iron_rewards_1",
    "silver_rewards_1", "gold_rewards_1", "jade_rewards_1", "crystal_rewards_1"};

/* --- RTON writer (only what the schedule needs) --------------------------- */

struct Rton {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void varint(std::uint32_t v) {
        while (v >= 0x80) {
            u8((std::uint8_t)(v | 0x80));
            v >>= 7;
        }
        u8((std::uint8_t)v);
    }
    void str(const std::string &s) { /* 0x81: uncached ASCII, so no string-cache bookkeeping */
        u8(0x81);
        varint((std::uint32_t)s.size());
        b.insert(b.end(), s.begin(), s.end());
    }
    void i32(std::int32_t v) {
        u8(0x20);
        std::uint8_t raw[4];
        std::memcpy(raw, &v, 4);
        b.insert(b.end(), raw, raw + 4);
    }
    void array_begin(std::uint32_t n) {
        u8(0x86);
        u8(0xfd);
        varint(n);
    }
    void array_end() { u8(0xfe); }
    void object_begin() { u8(0x85); }
    void object_end() { u8(0xff); }
};

int level_set_for_week(int week) {
    return kLevelSets[((week % kLevelSetCount) + kLevelSetCount) % kLevelSetCount];
}

int current_week() {
    const std::int64_t now = (std::int64_t)std::time(nullptr);
    if (now < kScheduleBase) return 0;
    return (int)((now - kScheduleBase) / kWeek);
}

/* --- the local player's state ---------------------------------------------- */

struct State {
    bool loaded = false;
    long long name = 0; /* index into [JOUST_AVATAR_NAME_n]; 0 = never chosen */
    long long icon = 0; /* IMAGE_UI_JOUST_AVATARS_AVATAR_n */
    long long league = 1;
    long long crowns = 0;
    long long crown_streak = 0;
    long long reward_streak = 0;
    long long difficulty_band = 0;
    long long total_wins = 0;
    long long tournament = -1; /* week the crowns belong to */
    long long match_counter = 0;
    /* The match in flight, for completeMatch/finalizeMatch. */
    std::string match_id;
    std::string opponent_id;
    long long opponent_score = 0;
};

std::mutex g_state_lock;
State g_state;
std::mt19937 g_rng{(unsigned)std::time(nullptr)};

const char *kStatePath = "userdata/joust_local.txt";

void save_state_locked() {
    char buf[512];
    const int n = std::snprintf(buf, sizeof(buf),
                                "name=%lld\nicon=%lld\nleague=%lld\ncrowns=%lld\ncrownStreak=%lld\n"
                                "rewardStreak=%lld\ndifficultyBand=%lld\ntotalWins=%lld\ntournament=%lld\n"
                                "matchCounter=%lld\n",
                                g_state.name, g_state.icon, g_state.league, g_state.crowns,
                                g_state.crown_streak, g_state.reward_streak, g_state.difficulty_band,
                                g_state.total_wins, g_state.tournament, g_state.match_counter);
    if (n > 0) host_files::write(kStatePath, std::vector<std::uint8_t>(buf, buf + n));
}

void load_state_locked() {
    if (g_state.loaded) return;
    g_state.loaded = true;
    const std::vector<std::uint8_t> raw = host_files::read(kStatePath);
    const std::string text(raw.begin(), raw.end());
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const long long v = std::atoll(line.c_str() + eq + 1);
        if (k == "name") g_state.name = v;
        else if (k == "icon") g_state.icon = v;
        else if (k == "league") g_state.league = v;
        else if (k == "crowns") g_state.crowns = v;
        else if (k == "crownStreak") g_state.crown_streak = v;
        else if (k == "rewardStreak") g_state.reward_streak = v;
        else if (k == "difficultyBand") g_state.difficulty_band = v;
        else if (k == "totalWins") g_state.total_wins = v;
        else if (k == "tournament") g_state.tournament = v;
        else if (k == "matchCounter") g_state.match_counter = v;
    }
    /* A new week is a new tournament: crowns start over. */
    if (g_state.tournament != current_week()) {
        g_state.tournament = current_week();
        g_state.crowns = 0;
    }
}

/* --- request parsing --------------------------------------------------------- */

/* The raw text after `"key":` in a flat JSON body (number or string, quotes stripped), or the value of
 * `key=` in a query/form string. Good enough for the client's small request bodies. */
std::string field(const std::string &text, const std::string &key) {
    const std::string quoted = "\"" + key + "\"";
    std::size_t p = text.find(quoted);
    if (p != std::string::npos) {
        p = text.find(':', p + quoted.size());
        if (p == std::string::npos) return {};
        ++p;
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (p < text.size() && text[p] == '"') {
            const std::size_t e = text.find('"', p + 1);
            return text.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
        }
        std::size_t e = p;
        while (e < text.size() && text[e] != ',' && text[e] != '}' && text[e] != ']') ++e;
        return text.substr(p, e - p);
    }
    const std::string eq = key + "=";
    p = text.find(eq);
    while (p != std::string::npos && p != 0 && text[p - 1] != '?' && text[p - 1] != '&')
        p = text.find(eq, p + 1);
    if (p == std::string::npos) return {};
    p += eq.size();
    const std::size_t e = text.find('&', p);
    return text.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

long long field_int(const std::string &text, const std::string &key, long long fallback) {
    const std::string v = field(text, key);
    if (v.empty()) return fallback;
    return std::atoll(v.c_str());
}

/* "https://host/joust/v1/dashboard?x=1" -> "dashboard" */
std::string route_of(const std::string &url) {
    std::string u = url;
    const auto q = u.find('?');
    if (q != std::string::npos) u.resize(q);
    const auto v1 = u.find("/joust/v1/");
    if (v1 == std::string::npos) {
        const auto slash = u.find_last_of('/');
        return slash == std::string::npos ? u : u.substr(slash + 1);
    }
    return u.substr(v1 + 10);
}

/* --- response builders -------------------------------------------------------- */

std::string json_escape(const std::string &s) {
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

/* Player names: the game only carries a name NUMBER and turns it into one of its own names (patches.cpp
 * hooks that step). Numbers from kDiscordNameBase up are ours: each stands for a Discord name. Locally
 * there is one, the linked account's; the server will hand out the others with its responses. */
constexpr long long kDiscordNameBase = 1000000;
constexpr long long kLocalNameId = kDiscordNameBase;

std::string state_json_locked() {
    g_state.name = kLocalNameId;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"name\":%lld,\"icon\":%lld,\"league\":%lld,\"crowns\":%lld,\"crownStreak\":%lld,"
                  "\"rewardStreak\":%lld,\"difficultyBand\":%lld,\"totalWins\":%lld}",
                  g_state.name, g_state.icon, g_state.league, g_state.crowns, g_state.crown_streak,
                  g_state.reward_streak, g_state.difficulty_band, g_state.total_wins);
    return buf;
}

/* Dashboard: tournament (id = week index; endTime = SECONDS LEFT, not a date -- an epoch there showed
 * "20773 days remaining", exactly epoch/86400), the player's state, the leaderboard ({name, icon,
 * crowns} per entry, all ints) and the player's place in it. Locally the board holds only the player:
 * real rivals come from the server. */
std::string dashboard_json_locked() {
    const int week = current_week();
    const long long now = (long long)std::time(nullptr);
    long long end_time = kScheduleBase + (long long)(week + 1) * kWeek - now;
    if (end_time < 0) end_time = 0;

    struct Entry {
        long long name, icon, crowns;
        bool me;
    };
    std::vector<Entry> board;
    board.push_back({kLocalNameId, g_state.icon, g_state.crowns, true});
    for (std::size_t i = 1; i < board.size(); ++i) /* insertion sort, crowns descending */
        for (std::size_t j = i; j > 0 && board[j].crowns > board[j - 1].crowns; --j) std::swap(board[j], board[j - 1]);
    long long position = 1;
    std::string lb = "[";
    for (std::size_t i = 0; i < board.size(); ++i) {
        if (board[i].me) position = (long long)i + 1;
        char e[128];
        std::snprintf(e, sizeof(e), "%s{\"name\":%lld,\"icon\":%lld,\"crowns\":%lld}", i ? "," : "",
                      board[i].name, board[i].icon, board[i].crowns);
        lb += e;
    }
    lb += "]";

    char head[256];
    std::snprintf(head, sizeof(head), "{\"tournament\":{\"tournamentId\":%d,\"endTime\":%lld},\"position\":%lld,",
                  week + 1, end_time, position);
    return std::string(head) + "\"state\":" + state_json_locked() + ",\"leaderboard\":" + lb + "}";
}

std::string match_json_locked(const std::string &body) {
    (void)body;
    ++g_state.match_counter;
    char id[64];
    std::snprintf(id, sizeof(id), "local-%lld-%lld", (long long)std::time(nullptr), g_state.match_counter);
    g_state.match_id = id;
    g_state.opponent_id = "local-ai-" + std::to_string(g_rng() % 100000);
    g_state.opponent_score = 0; /* the game's AI replay produces the opponent's score */
    const long long opp_name = (long long)(g_rng() % 9981) + 1;
    const long long opp_icon = (long long)(g_rng() % 25) + 1;
    const std::string seed = "Joust" + std::to_string(level_set_for_week(current_week()));
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"matchId\":\"%s\",\"opponents\":[{\"opponentId\":\"%s\",\"name\":%lld,\"icon\":%lld,"
                  "\"league\":%lld,\"levelSeed\":\"%s\",\"score\":%lld,\"replay\":\"\"}]}",
                  json_escape(g_state.match_id).c_str(), json_escape(g_state.opponent_id).c_str(), opp_name,
                  opp_icon, g_state.league, seed.c_str(), g_state.opponent_score);
    return buf;
}

/* completeMatch and finalizeMatch read different fields ($.grantedRewards.crowns vs $.multiplier /
 * $.streaks.* / $.rewards); both get all of them, plus the updated state. */
std::string match_result_json_locked(const std::string &body, bool finalize) {
    if (!finalize) {
        /* winRatio is what the client says about the match; > 0 counts as a win. */
        const std::string wr = field(body, "winRatio");
        const bool win = !wr.empty() && std::atof(wr.c_str()) > 0.0;
        const long long granted = win ? 5 : 1; /* CrownSet_Normal: CrownsWinBase 5, CrownsLoss 1 */
        g_state.crowns += granted;
        if (win) {
            ++g_state.total_wins;
            ++g_state.crown_streak;
            ++g_state.reward_streak;
        } else {
            g_state.crown_streak = 0;
            g_state.reward_streak = 0;
        }
        save_state_locked();
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"grantedRewards\":{\"crowns\":%lld},\"multiplier\":1,\"streaks\":{\"reward\":%lld,"
                      "\"crown\":%lld,\"difficulty\":%lld},\"rewards\":[],\"state\":%s}",
                      granted, g_state.reward_streak, g_state.crown_streak, g_state.difficulty_band,
                      state_json_locked().c_str());
        return buf;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"grantedRewards\":{\"crowns\":0},\"multiplier\":1,\"streaks\":{\"reward\":%lld,"
                  "\"crown\":%lld,\"difficulty\":%lld},\"rewards\":[],\"state\":%s}",
                  g_state.reward_streak, g_state.crown_streak, g_state.difficulty_band,
                  state_json_locked().c_str());
    return buf;
}

}  // namespace

bool joust_player_name(long long name_id, std::string &out) {
    /* The local player's own row takes its number from the profile, not from state.name, and an
     * avatar never picked there is 0 -- so 0 is the local player too. Nobody else is ever sent 0. */
    if (name_id != kLocalNameId && name_id != 0) return false;
    const Pvz2CloudConfig *cloud = pvz2_cloud_config();
    if (cloud == nullptr || cloud->auth_discord_name[0] == '\0') return false;
    out = cloud->auth_discord_name;
    return true;
}

bool joust_is_game_server_url(const std::string &url) {
    return url.find("awspopcap.com/joust/") != std::string::npos || url.find("/joust/v1/") != std::string::npos;
}

std::string joust_local_respond(const std::string &method, const std::string &url, const std::string &body) {
    std::lock_guard<std::mutex> lk(g_state_lock);
    load_state_locked();
    const std::string route = route_of(url);
    std::string out;
    if (route == "dashboard" || route == "registerForTournament") {
        out = dashboard_json_locked();
    } else if (route == "avatar") {
        g_state.name = field_int(body, "name", g_state.name);
        g_state.icon = field_int(body, "icon", g_state.icon);
        save_state_locked();
        out = dashboard_json_locked();
    } else if (route == "match") {
        out = match_json_locked(body);
        save_state_locked();
    } else if (route == "completeMatch") {
        out = match_result_json_locked(body, false);
    } else if (route == "finalizeMatch") {
        out = match_result_json_locked(body, true);
    } else {
        out = "{}";
    }
    pvz2_log("[joust] %s %s route=%s", method.c_str(), url.c_str(), route.c_str());
    if (!body.empty()) pvz2_log("[joust]   request body (%zu bytes): %s", body.size(), body.c_str());
    pvz2_log("[joust]   response: %s", out.c_str());
    return out;
}

std::vector<std::uint8_t> joust_schedule_rton() {
    Rton r;
    const char magic[] = {'R', 'T', 'O', 'N', 1, 0, 0, 0};
    r.b.insert(r.b.end(), magic, magic + 8);
    r.str("objects");
    r.array_begin(1);
    r.object_begin();
    r.str("aliases");
    r.array_begin(1);
    r.str("JoustSchedule");
    r.array_end();
    r.str("objclass");
    r.str("JoustSchedule");
    r.str("objdata");
    r.object_begin();
    r.str("TournamentDefinitions");
    r.array_begin(kScheduleWeeks);
    for (int w = 0; w < kScheduleWeeks; ++w) {
        r.object_begin();
        r.str("CrownRewardKey");
        r.str("CrownSet_Normal");
        r.str("LeaderboardDefinitionKey");
        r.str("Leaderboard_Normal");
        r.str("LevelSetKey");
        r.str("LevelSet" + std::to_string(level_set_for_week(w)));
        r.str("ProgressiveRewardKeys");
        r.array_begin(9);
        for (const char *k : kProgressiveRewardKeys) r.str(k);
        r.array_end();
        r.str("StartDate");
        r.i32((std::int32_t)(kScheduleBase + (std::int64_t)w * kWeek));
        r.str("TournamentRewardKeys");
        r.array_begin(9);
        for (const char *k : kTournamentRewardKeys) r.str(k);
        r.array_end();
        r.object_end();
    }
    r.array_end();
    r.object_end(); /* objdata */
    r.object_end(); /* the JoustSchedule object */
    r.array_end();  /* objects */
    r.str("version");
    r.i32(1);
    r.object_end(); /* the root object */
    const char done[] = {'D', 'O', 'N', 'E'};
    r.b.insert(r.b.end(), done, done + 4);
    return r.b;
}

}  // namespace dex
}  // namespace pvz2native

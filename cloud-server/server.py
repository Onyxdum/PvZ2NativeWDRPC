#!/usr/bin/env python3
import base64
import hashlib
import hmac
import html
import json
import os
import random
import re
import secrets
import struct
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DATA_DIR = os.environ.get("PVZ2_DATA_DIR", "/data/saves")
PORT = int(os.environ.get("PVZ2_PORT", "8765"))
MAX_BODY_BYTES = 16 * 1024 * 1024  # 16 MB max per save bundle
RATE_LIMIT_WINDOW = 60
RATE_LIMIT_MAX_REQ = 30

os.makedirs(DATA_DIR, exist_ok=True)
VALID_ID_RE = re.compile(r"^[0-9]{15,22}$")

# Cache of sha256(discord_access_token) -> (discord_id, verified_at)
TOKEN_CACHE_FILE = os.path.join(DATA_DIR, "_verified_tokens.json")
TOKEN_CACHE = {}
RATE_BUCKETS = {}

try:
    if os.path.isfile(TOKEN_CACHE_FILE):
        with open(TOKEN_CACHE_FILE, "r", encoding="utf-8") as f:
            TOKEN_CACHE = json.load(f)
except Exception:
    TOKEN_CACHE = {}


def save_token_cache():
    try:
        tmp = TOKEN_CACHE_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(TOKEN_CACHE, f)
        os.replace(tmp, TOKEN_CACHE_FILE)
    except Exception:
        pass


def verify_discord_token(token: str, expected_id: str) -> bool:
    return discord_user_name(token, expected_id) is not None


def discord_avatar_url(discord_id: str, avatar_hash) -> str:
    """The account's profile picture on Discord's CDN, or Discord's default one."""
    if avatar_hash:
        return "https://cdn.discordapp.com/avatars/%s/%s.png?size=256" % (discord_id, avatar_hash)
    try:
        return "https://cdn.discordapp.com/embed/avatars/%d.png" % ((int(discord_id) >> 22) % 6)
    except ValueError:
        return "https://cdn.discordapp.com/embed/avatars/0.png"


def discord_cached_avatar(discord_id: str) -> str:
    for entry in TOKEN_CACHE.values():
        if entry.get("id") == discord_id and entry.get("avatar"):
            return entry["avatar"]
    return discord_avatar_url(discord_id, None)


def discord_user_name(token: str, expected_id: str, fallback_name: str = ""):
    """The Discord display name of the token's account when it is `expected_id`, else None.
    `fallback_name` (what the client says) is used only when the account is verified from the cache and
    Discord cannot give the name itself."""
    if not token or len(token) < 10:
        return None
    tok_hash = hashlib.sha256(token.encode("utf-8")).hexdigest()
    cached = TOKEN_CACHE.get(tok_hash)
    if cached and cached.get("id") == expected_id and cached.get("name"):
        return cached["name"]

    req = urllib.request.Request(
        "https://discord.com/api/v10/users/@me",
        headers={
            "Authorization": f"Bearer {token}",
            "User-Agent": "PvZ2NativeCloud/1.0",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=8) as resp:
            if resp.status != 200:
                return None
            user = json.loads(resp.read().decode("utf-8"))
            real_id = str(user.get("id", ""))
            if real_id and real_id == expected_id:
                name = user.get("global_name") or user.get("username") or "Player"
                avatar = discord_avatar_url(real_id, user.get("avatar"))
                TOKEN_CACHE[tok_hash] = {"id": real_id, "name": name, "avatar": avatar, "ts": int(time.time())}
                save_token_cache()
                return name
    except Exception:
        # Discord unreachable (or the token no longer works there): a token seen before for this account
        # still counts, as it always did for saves.
        if cached and cached.get("id") == expected_id:
            return cached.get("name") or fallback_name[:64] or "Player"
        return None
    return None


# --- Battlez ("Joust" inside the game) ---------------------------------------------------------------
#
# The game's own Battlez client talks to /joust/v1/<route> (the port forwards it here with the player's
# Discord token). Players are Discord accounts; the game's playerId is the same on every copy of the port
# and is ignored. Protocol: CLAUDE.md section 3.E. One tournament per week from Monday 2026-01-05 00:00
# UTC, the same calendar the port generates for joust_schedule.
#
# Names and pictures: the game shows a player's name and avatar from NUMBERS, and zeroes any number out of
# its own range while reading a response (name ids up to 9980, icons 0..24, icon k = AVATAR_k+1). So:
#   - every Discord account gets a name id from 9000 up (`nameId`; the AI uses 1..8999), and each response
#     carries {"names": {"<nameId>": "<Discord name>"}}, which the port shows instead of the game's names;
#   - icons 15..24 (AVATAR_16..25) are handed out per response to the Discord players in it, the player
#     first, and {"avatars": {"<icon>": "<picture URL>"}} says whose picture the port paints into each of
#     them; the AI uses icons 0..14.

JOUST_FILE = os.path.join(DATA_DIR, "_joust.json")
JOUST_LOCK = threading.Lock()
JOUST_SCHEDULE_BASE = 1767571200  # Monday 2026-01-05 00:00 UTC
JOUST_WEEK = 7 * 24 * 3600
JOUST_LEVEL_SETS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 22, 27]
JOUST_NAME_BASE = 9000      # Discord players: 9000..9980 (the game's limit is 9981 names)
JOUST_NAME_LAST = 9980
JOUST_ICON_FIRST = 15       # Discord pictures: icons 15..24
JOUST_ICON_LAST = 24
JOUST_MAX_REPLAYS = 500
JOUST_MAX_REPLAY_CHARS = 64 * 1024
JOUST_CROWNS_WIN = 5   # CrownSet_Normal
JOUST_CROWNS_LOSS = 1
JOUST_RATE_MAX_REQ = 600
JOUST_LIVE_WAIT_SECS = 15.0
JOUST_LIVE_MAX_QUEUE = 100
JOUST_LIVE_MAX_ROOMS = 200
JOUST_MAX_SCORE = 25_000_000
JOUST_MAX_SCORE_PER_SEC = 1_500_000
JOUST_MIN_MATCH_SECS = 20.0
JOUST_LIVE_LOCK = threading.Lock()
JOUST_LIVE_QUEUE = []   # list of dicts: {did, name, league, seed, ts, event, room_id, side, opp_did}
JOUST_LIVE_ROOMS = {}   # room_id -> {levelSeed, created, players: {did: {score, score_ts, ready, finished, last_seen, name}}}
# League rules of Leaderboard_Normal (joust_leaderboards in the OBB); leagues 1..8 = Soil..Jade.
JOUST_PROMOTION_COUNT = 5
JOUST_DEMOTION_COUNT = 3
JOUST_MIN_LEAGUE_FOR_DEMOTION = 4
JOUST_MIN_PLAYERS_FOR_DEMOTION = 10
JOUST_MAX_LEAGUE = 8
JOUST_LEAGUE_REWARD_KEYS = ["ftue_rewards_1", "soil_rewards_1", "wood_rewards_1", "brick_rewards_1",
                            "iron_rewards_1", "silver_rewards_1", "gold_rewards_1", "jade_rewards_1",
                            "crystal_rewards_1"]
# PlacementRewards of joust_tournamentrewards (OBB), "none" entries dropped: [Award, AwardParam, Quantity]
# per place, first place first.
JOUST_PLACEMENT_REWARDS = {"ftue_rewards_1":[[["tickets","",5]]],"soil_rewards_1":[[["gems","",100],["coins","",4000]],[["gems","",80],["coins","",2500]],[["gems","",60],["coins","",1250]],[["gems","",50],["coins","",500]],[["gems","",40],["coins","",500]],[["gems","",25],["coins","",250]],[["gems","",25],["coins","",250]],[["gems","",20]],[["gems","",20]],[["gems","",10]]],"wood_rewards_1":[[["gems","",125],["coins","",5000]],[["gems","",100],["coins","",2500]],[["gems","",75],["coins","",1250]],[["gems","",62],["coins","",500]],[["gems","",50],["coins","",500]],[["gems","",32],["coins","",250]],[["gems","",32],["coins","",250]],[["gems","",25]],[["gems","",25]],[["gems","",12]]],"brick_rewards_1":[[["gems","",150],["coins","",6000]],[["gems","",120],["coins","",2500]],[["gems","",90],["coins","",1250]],[["gems","",75],["coins","",500]],[["gems","",60],["coins","",500]],[["gems","",38],["coins","",250]],[["gems","",38],["coins","",250]],[["gems","",30]],[["gems","",30]],[["gems","",15]]],"iron_rewards_1":[[["gems","",180],["coins","",7000]],[["gems","",144],["coins","",6000]],[["gems","",108],["coins","",4000]],[["gems","",90],["coins","",3200]],[["gems","",72],["coins","",2800]],[["gems","",45],["coins","",2400]],[["gems","",45],["coins","",2000]],[["gems","",36],["coins","",1600]],[["gems","",36],["coins","",1200]],[["gems","",18]]],"silver_rewards_1":[[["gems","",220],["coins","",8000]],[["gems","",176],["coins","",6000]],[["gems","",132],["coins","",4000]],[["gems","",110],["coins","",3200]],[["gems","",88],["coins","",2800]],[["gems","",55],["coins","",2400]],[["gems","",55],["coins","",2000]],[["gems","",44],["coins","",1600]],[["gems","",44],["coins","",1200]],[["gems","",22]]],"gold_rewards_1":[[["gems","",275],["coins","",9000]],[["gems","",220],["coins","",8000]],[["gems","",165],["coins","",7500]],[["gems","",138],["coins","",6000]],[["gems","",110],["coins","",5000]],[["gems","",68],["coins","",4000]],[["gems","",68],["coins","",3000]],[["gems","",55],["coins","",2500]],[["gems","",55],["coins","",2000]],[["gems","",28]]],"jade_rewards_1":[[["gems","",350],["coins","",10000]],[["gems","",280],["coins","",8000]],[["gems","",210],["coins","",7500]],[["gems","",175],["coins","",6000]],[["gems","",140],["coins","",5000]],[["gems","",88],["coins","",4000]],[["gems","",88],["coins","",3000]],[["gems","",70],["coins","",2500]],[["gems","",70],["coins","",2000]],[["gems","",35]]],"crystal_rewards_1":[[["gems","",450],["coins","",15000]],[["gems","",360],["coins","",12000]],[["gems","",270],["coins","",11250]],[["gems","",225],["coins","",9000]],[["gems","",180],["coins","",7500]],[["gems","",112],["coins","",6000]],[["gems","",112],["coins","",4500]],[["gems","",90],["coins","",3750]],[["gems","",90],["coins","",3000]],[["gems","",45]]]}



def joust_load():
    try:
        with open(JOUST_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        data = {}
    data.setdefault("players", {})
    data.setdefault("replays", [])
    data.setdefault("matches", {})
    # Name ids handed out before the game's range was known (1000001...) move into 9000..9980.
    if data.get("nextNameId", 0) > JOUST_NAME_LAST:
        nxt = JOUST_NAME_BASE
        for q in data["players"].values():
            q["nameId"] = nxt
            nxt += 1
        data["nextNameId"] = nxt
    data.setdefault("nextNameId", JOUST_NAME_BASE)
    return data


JOUST = joust_load()


def joust_save():
    tmp = JOUST_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(JOUST, f)
    os.replace(tmp, JOUST_FILE)


def joust_week(now=None):
    now = int(now if now is not None else time.time())
    return max(0, (now - JOUST_SCHEDULE_BASE) // JOUST_WEEK)


def joust_level_seed(week):
    return "Joust%d" % JOUST_LEVEL_SETS[week % len(JOUST_LEVEL_SETS)]


def joust_player(discord_id, name):
    """The stored player, created on first sight; crowns start over with each week's tournament."""
    players = JOUST["players"]
    p = players.get(discord_id)
    if p is None:
        if JOUST["nextNameId"] > JOUST_NAME_LAST:
            JOUST["nextNameId"] = JOUST_NAME_BASE  # more players than names: numbers are shared
        p = {"nameId": JOUST["nextNameId"], "league": 1, "crowns": 0, "crownStreak": 0,
             "rewardStreak": 0, "difficultyBand": 0, "totalWins": 0, "tournament": joust_week()}
        JOUST["nextNameId"] += 1
        players[discord_id] = p
    p.pop("icon", None)
    p["name"] = name
    p["avatar"] = discord_cached_avatar(discord_id)
    week = joust_week()
    if p.get("tournament") != week:
        p["tournament"] = week
        p["crowns"] = 0
    return p


class JoustPeople:
    """The Discord players one response shows: their names, and an icon 15..24 each with its picture."""

    def __init__(self):
        self.names, self.avatars, self.icons = {}, {}, {}

    def icon(self, q):
        if "nameId" not in q:
            return 0
        self.names[str(q["nameId"])] = q.get("name", "Player")
        key = q["nameId"]
        if key not in self.icons:
            icon = JOUST_ICON_FIRST + len(self.icons)
            if icon > JOUST_ICON_LAST:
                return 0  # more players than squares: the game's first avatar
            self.icons[key] = icon
            if q.get("avatar"):
                self.avatars[str(icon)] = q["avatar"]
        return self.icons[key]

    def fields(self):
        return {"names": self.names, "avatars": self.avatars}


def joust_state(p, people):
    return {k: p[k] for k in ("league", "crowns", "crownStreak", "rewardStreak", "difficultyBand",
                              "totalWins")} | {"name": p["nameId"], "icon": people.icon(p)}


def joust_table(league=None):
    """Players of this week's tournament (one league, or all of them), most crowns first."""
    week = joust_week()
    rows = [(did, p) for did, p in JOUST["players"].items()
            if p.get("tournament") == week and (league is None or p.get("league") == league)]
    rows.sort(key=lambda r: (-r[1].get("crowns", 0), r[1].get("nameId", 0)))
    return rows


def joust_dashboard(discord_id, p, people):
    week = joust_week()
    state = joust_state(p, people)  # the player takes the first square
    board, position = [], 0
    for i, (did, q) in enumerate(joust_table(p["league"])):
        if did == discord_id:
            position = i  # the game reads it as a 0-based row index
        board.append({"name": q["nameId"], "icon": people.icon(q), "crowns": q.get("crowns", 0)})
    end_left = JOUST_SCHEDULE_BASE + (week + 1) * JOUST_WEEK - int(time.time())
    return {"tournament": {"tournamentId": week + 1, "endTime": max(0, end_left)},
            "position": position, "state": state, "leaderboard": board}


def joust_live_matchmake(discord_id, name, league, seed):
    """Try to pair `discord_id` with another live player searching on `seed` (wait up to JOUST_LIVE_WAIT_SECS).
    Returns (room_id, side, opponent_did) on match, or (None, None, None) on timeout."""
    now = time.time()
    entry = None
    with JOUST_LIVE_LOCK:
        # Cancel any previous waiter for this same account (prevents thread-exhaustion DoS) and prune stale ones.
        kept = []
        for q in JOUST_LIVE_QUEUE:
            if q["did"] == discord_id or (now - q["ts"] >= JOUST_LIVE_WAIT_SECS + 10.0):
                q["event"].set()
            else:
                kept.append(q)
        JOUST_LIVE_QUEUE[:] = kept
        for rid in [k for k, v in JOUST_LIVE_ROOMS.items() if now - v["created"] > 600.0]:
            del JOUST_LIVE_ROOMS[rid]
        if len(JOUST_LIVE_ROOMS) >= JOUST_LIVE_MAX_ROOMS:
            return None, None, None
        # Prefer same league, otherwise any waiting player on this week's level.
        partner_idx = None
        for i, q in enumerate(JOUST_LIVE_QUEUE):
            if q["seed"] == seed and q["did"] != discord_id:
                if q["league"] == league:
                    partner_idx = i
                    break
                if partner_idx is None:
                    partner_idx = i
        if partner_idx is not None:
            partner = JOUST_LIVE_QUEUE.pop(partner_idx)
            room_id = "live-%d-%s" % (int(now * 1000), secrets.token_hex(4))
            JOUST_LIVE_ROOMS[room_id] = {
                "levelSeed": seed,
                "created": now,
                "players": {
                    partner["did"]: {"score": 0, "score_ts": now, "ready": False, "finished": False, "last_seen": now, "name": partner["name"]},
                    discord_id: {"score": 0, "score_ts": now, "ready": False, "finished": False, "last_seen": now, "name": name},
                },
            }
            partner["room_id"] = room_id
            partner["side"] = "a"
            partner["opp_did"] = discord_id
            partner["event"].set()
            return room_id, "b", partner["did"]
        if len(JOUST_LIVE_QUEUE) >= JOUST_LIVE_MAX_QUEUE:
            return None, None, None
        entry = {
            "did": discord_id,
            "name": name,
            "league": league,
            "seed": seed,
            "ts": now,
            "event": threading.Event(),
            "room_id": None,
            "side": None,
            "opp_did": None,
        }
        JOUST_LIVE_QUEUE.append(entry)
    # Wait outside any locks so other players can join and pair immediately.
    matched = entry["event"].wait(timeout=JOUST_LIVE_WAIT_SECS)
    with JOUST_LIVE_LOCK:
        if entry in JOUST_LIVE_QUEUE:
            JOUST_LIVE_QUEUE.remove(entry)
        if matched and entry["room_id"]:
            return entry["room_id"], entry["side"], entry["opp_did"]
    return None, None, None


def joust_live_sync(discord_id, body):
    """Real-time score & ready-state sync for an active live Battlez room."""
    raw_mid = str(body.get("matchId") or "")
    room_id = raw_mid.split(":", 1)[0]
    now = time.time()
    with JOUST_LIVE_LOCK:
        room = JOUST_LIVE_ROOMS.get(room_id)
        if not room or discord_id not in room["players"]:
            return 404, {"ok": False, "error": "room_not_found"}
        me = room["players"][discord_id]
        if body.get("ready") and not me["ready"]:
            me["ready"] = True
            me["score_ts"] = now
        try:
            sc = int(body.get("score") or 0)
        except Exception:
            sc = 0
        sc = max(0, min(sc, JOUST_MAX_SCORE))
        # Only accept score progress while the match is running (after ready, before finished)
        # and clamp impossible single-tick jumps.
        if me["ready"] and not me["finished"] and sc > me["score"]:
            dt = max(0.25, now - me.get("score_ts", now))
            max_step = max(250_000, int(dt * JOUST_MAX_SCORE_PER_SEC))
            me["score"] = min(sc, me["score"] + max_step)
            me["score_ts"] = now
        if body.get("finished"):
            me["finished"] = True
        me["last_seen"] = now
        rival_did = next((d for d in room["players"] if d != discord_id), None)
        rival = room["players"].get(rival_did) if rival_did else None
        if rival is None:
            return 200, {"ok": True, "rivalScore": 0, "rivalReady": True, "rivalFinished": True,
                         "bothReady": True, "rivalOnline": False, "rivalName": ""}
        rival_online = (now - rival["last_seen"]) <= 12.0
        both_ready = bool(me["ready"] and (rival["ready"] or not rival_online))
        return 200, {
            "ok": True,
            "rivalScore": rival["score"],
            "rivalReady": rival["ready"],
            "rivalFinished": rival["finished"],
            "bothReady": both_ready,
            "rivalOnline": rival_online,
            "rivalName": rival["name"],
        }


def joust_match(discord_id, p, people, live_pair=None):
    """An opponent: a live matched player if `live_pair` is set, else a replay on this week's level, else AI."""
    people.icon(p)
    seed = joust_level_seed(joust_week())
    if live_pair and live_pair[0]:
        room_id, side, opp_did = live_pair
        match_id = "%s:%s" % (room_id, side)
        q = JOUST["players"].get(opp_did) or {}
        opp = {"opponentId": opp_did, "name": q.get("nameId", 0), "icon": people.icon(q),
               "league": q.get("league", p["league"]), "levelSeed": seed, "score": 0, "replay": ""}
        JOUST["matches"][match_id] = {"player": discord_id, "opponentId": opp_did, "levelSeed": seed,
                                      "liveRoom": room_id, "ts": int(time.time())}
        return {"matchId": match_id, "live": True, "rivalName": q.get("name", ""), "opponents": [opp]}
    # A match left at once scores 0 and replays nothing: never an opponent.
    pool = [r for r in JOUST["replays"]
            if r["levelSeed"] == seed and r["player"] != discord_id and r.get("score", 0) > 0]
    same_league = [r for r in pool if r.get("league") == p["league"]]
    pool = (same_league or pool)[-50:]
    match_id = "m%d-%s" % (int(time.time() * 1000), secrets.token_hex(4))
    if pool:
        r = random.choice(pool)
        q = JOUST["players"].get(r["player"]) or {}
        opp = {"opponentId": r["player"], "name": q.get("nameId", 0), "icon": people.icon(q),
               "league": r.get("league", 1), "levelSeed": seed, "score": r["score"], "replay": r["replay"]}
    else:
        # The game's own names and avatars, outside the numbers kept for Discord players.
        opp = {"opponentId": "ai-%d" % random.randint(1, 10 ** 6), "name": random.randint(1, JOUST_NAME_BASE - 1),
               "icon": random.randint(0, JOUST_ICON_FIRST - 1), "league": p["league"], "levelSeed": seed,
               "score": 0, "replay": ""}
    JOUST["matches"][match_id] = {"player": discord_id, "opponentId": opp["opponentId"], "levelSeed": seed,
                                  "ts": int(time.time())}
    # Forget matches nobody finished after a day.
    cutoff = time.time() - 86400
    for mid in [m for m, v in JOUST["matches"].items() if v.get("ts", 0) < cutoff]:
        del JOUST["matches"][mid]
    return {"matchId": match_id, "live": False, "opponents": [opp]}


def joust_complete(discord_id, p, body, people):
    r = body.get("r") or {}
    match = JOUST["matches"].get(str(body.get("matchId", "")))
    if match is None or match.get("player") != discord_id or match.get("done"):
        return 400, {"ErrorCode": "INVALID_MATCH_ID"}
    now = time.time()
    elapsed = now - float(match.get("ts") or 0)
    try:
        my_score = max(0, min(int(r.get("score") or 0), JOUST_MAX_SCORE))
    except Exception:
        my_score = 0
    win = float(body.get("winRatio") or 0) > 1.0
    if match.get("liveRoom"):
        with JOUST_LIVE_LOCK:
            room = JOUST_LIVE_ROOMS.get(match["liveRoom"])
            if room and discord_id in room["players"]:
                me = room["players"][discord_id]
                # Prevent claiming a sudden huge jump at completeMatch that wasn't earned during live sync.
                if not me["finished"] and my_score > me["score"]:
                    me["score"] = min(my_score, me["score"] + JOUST_MAX_SCORE_PER_SEC)
                me["finished"] = True
                me["last_seen"] = now
                rival_did = next((d for d in room["players"] if d != discord_id), None)
                if rival_did:
                    r_score = room["players"][rival_did]["score"]
                    if me["score"] != r_score:
                        win = me["score"] > r_score
    # Reject instant scripted crown farming (< JOUST_MIN_MATCH_SECS since match creation).
    if elapsed < JOUST_MIN_MATCH_SECS:
        match["done"] = True
        return 200, {"grantedRewards": {"crowns": 0}, "state": joust_state(p, people)}
    granted = JOUST_CROWNS_WIN if win else JOUST_CROWNS_LOSS
    p["crowns"] += granted
    if win:
        p["totalWins"] += 1
        p["crownStreak"] += 1
        p["rewardStreak"] += 1
    else:
        p["crownStreak"] = 0
        p["rewardStreak"] = 0
    replay = str(r.get("replay") or "")
    if replay and len(replay) <= JOUST_MAX_REPLAY_CHARS and int(r.get("score") or 0) > 0:
        JOUST["replays"].append({"player": discord_id, "levelSeed": str(r.get("levelSeed") or match["levelSeed"]),
                                 "league": p["league"], "score": int(r.get("score") or 0), "replay": replay,
                                 "ts": int(time.time())})
        del JOUST["replays"][:-JOUST_MAX_REPLAYS]
    match["done"] = True
    return 200, {"grantedRewards": {"crowns": granted}, "state": joust_state(p, people)}


def joust_finalize(discord_id, p, body):
    JOUST["matches"].pop(str(body.get("matchId", "")), None)
    return 200, {"multiplier": 1, "rewards": [],
                 "streaks": {"reward": p["rewardStreak"], "crown": p["crownStreak"],
                             "difficulty": p["difficultyBand"]}}


def joust_close_week(week, apply=True):
    """Freezes the standings of tournament `week`: everyone who played (crowns > 0) gets the reward of their
    place in their league -- the last rewarded place's reward below it, so nobody goes empty-handed -- and,
    with `apply`, moves league (Leaderboard_Normal). The result waits in `pendingResults` until the player's
    next dashboard. `apply=False` is the admin preview: results of the running week, leagues untouched."""
    leagues = {}
    for did, q in JOUST["players"].items():
        if q.get("tournament") == week and q.get("crowns", 0) > 0:
            leagues.setdefault(q.get("league", 1), []).append(q)
    for league, rows in leagues.items():
        rows.sort(key=lambda q: (-q.get("crowns", 0), q.get("nameId", 0)))
        n = len(rows)
        table = JOUST_PLACEMENT_REWARDS.get(JOUST_LEAGUE_REWARD_KEYS[min(league, len(JOUST_LEAGUE_REWARD_KEYS) - 1)], [])
        for i, q in enumerate(rows):
            new = league
            if i < JOUST_PROMOTION_COUNT and league < JOUST_MAX_LEAGUE:
                new = league + 1
            elif (n >= JOUST_MIN_PLAYERS_FOR_DEMOTION and league >= JOUST_MIN_LEAGUE_FOR_DEMOTION
                  and i >= n - JOUST_DEMOTION_COUNT):
                new = league - 1
            award = table[min(i, len(table) - 1)] if table else []
            q["pendingResults"] = {"newLeague": new, "previousLeague": league, "place": i + 1,
                                   "tournamentId": week + 1, "preview": not apply,
                                   "reward": [{"Award": a, "AwardParam": b, "AwardQuantity": c} for a, b, c in award]}
            if apply:
                q["league"] = new


def joust_close_finished_weeks():
    """Closes every past tournament that still has players in it (once: `closedWeek`)."""
    current = joust_week()
    weeks = {q.get("tournament") for q in JOUST["players"].values()
             if q.get("tournament", current) < current and q.get("closedWeek") != q.get("tournament")}
    for week in sorted(weeks):
        joust_close_week(week)
        for q in JOUST["players"].values():
            if q.get("tournament") == week:
                q["closedWeek"] = week


def joust_handle(route, discord_id, name, body, battlez_mode="live"):
    """(status, payload) for one Battlez request from an authenticated Discord account."""
    if route == "live/sync":
        return joust_live_sync(discord_id, body)
    live_pair = None
    if route == "match" and battlez_mode != "replay":
        with JOUST_LOCK:
            joust_close_finished_weeks()
            p_pre = joust_player(discord_id, name)
            league_pre = p_pre["league"]
            seed_pre = joust_level_seed(joust_week())
            joust_save()
        live_pair = joust_live_matchmake(discord_id, name, league_pre, seed_pre)
    with JOUST_LOCK:
        joust_close_finished_weeks()
        p = joust_player(discord_id, name)
        people = JoustPeople()
        if route in ("dashboard", "registerForTournament", "avatar"):
            # avatar: the game's avatar picker has nothing to choose, the Discord picture is the avatar.
            code, out = 200, joust_dashboard(discord_id, p, people)
            results = p.pop("pendingResults", None)
            if results:
                # Shown once: the game's tournament-end screen, then it grants the reward itself.
                out["results"] = {k: results[k] for k in ("newLeague", "previousLeague", "reward")}
                if results.get("preview"):
                    # The game only shows the results when the tournament id changed since its last
                    # dashboard, as it does at a real week's end: a preview reports a one-off id.
                    out["tournament"]["tournamentId"] = 100000 + joust_week()
        elif route == "match":
            code, out = 200, joust_match(discord_id, p, people, live_pair=live_pair)
        elif route == "completeMatch":
            code, out = joust_complete(discord_id, p, body, people)
        elif route == "finalizeMatch":
            code, out = joust_finalize(discord_id, p, body)
        else:
            code, out = 404, {"error": "unknown_route"}
        if code == 200:
            people.icon(p)
            out |= people.fields()
        joust_save()
    return code, out


def joust_global_table():
    with JOUST_LOCK:
        return [{"position": i + 1, "name": p.get("name", "Player"), "league": p.get("league", 1),
                 "crowns": p.get("crowns", 0), "totalWins": p.get("totalWins", 0)}
                for i, (_, p) in enumerate(joust_table())]


def joust_global_html():
    rows = "".join("<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td></tr>" % (
        r["position"], html.escape(r["name"]), r["league"], r["crowns"], r["totalWins"])
        for r in joust_global_table())
    week = joust_week()
    return ("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
            "<title>Battlez</title><style>body{font-family:sans-serif;max-width:720px;margin:2em auto;"
            "padding:0 16px}table{border-collapse:collapse;width:100%%}td,th{padding:6px 8px;"
            "border-bottom:1px solid #ccc;text-align:left}</style>"
            "<h1>Battlez &ndash; tabla global</h1><p>Torneo %d (nivel %s)</p>"
            "<table><tr><th>#</th><th>Jugador</th><th>Liga</th><th>Coronas</th><th>Victorias</th></tr>%s"
            "</table>" % (week + 1, joust_level_seed(week), rows or "<tr><td colspan=5>Nadie ha jugado todav&iacute;a</td></tr>"))


INVITE_DOWNLOAD_URL = "https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest"
DISCORD_CLIENT_ID = os.environ.get("DISCORD_CLIENT_ID", "").strip()
DISCORD_CLIENT_SECRET = os.environ.get("DISCORD_CLIENT_SECRET", "").strip()
PUBLIC_BASE_URL = os.environ.get("PVZ2_PUBLIC_URL", "https://pvz2.onyxdum.eu").rstrip("/")
INVITE_LOCAL_PORT = 47321


def _load_invite_secret():
    path = os.path.join(DATA_DIR, "_invite_secret")
    try:
        with open(path, "r", encoding="utf-8") as f:
            s = f.read().strip()
            if len(s) >= 32:
                return s.encode("utf-8")
    except OSError:
        pass
    s = secrets.token_hex(32)
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(s)
    except OSError:
        pass
    return s.encode("utf-8")


INVITE_SECRET = _load_invite_secret()


def _sign_invite_token(payload_dict):
    raw = json.dumps(payload_dict, separators=(",", ":")).encode("utf-8")
    b64 = base64.urlsafe_b64encode(raw).decode("ascii")
    sig = hmac.new(INVITE_SECRET, b64.encode("ascii"), hashlib.sha256).hexdigest()
    return f"{b64}.{sig}"


def _verify_invite_token(token, max_age=900):
    try:
        b64, sig = token.split(".", 1)
        expected = hmac.new(INVITE_SECRET, b64.encode("ascii"), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(sig, expected):
            return None
        data = json.loads(base64.urlsafe_b64decode(b64.encode("ascii")).decode("utf-8"))
        if int(time.time()) - int(data.get("ts", 0)) > max_age:
            return None
        return data
    except Exception:
        return None


def _inviter_profile(from_id, fallback_name=""):
    name, avatar = "", ""
    if VALID_ID_RE.match(from_id):
        with FRIENDS_LOCK:
            fp = FRIENDS.get(from_id) or {}
            name = fp.get("name") or ""
            avatar = fp.get("avatar") or ""
        if not name or not avatar:
            with JOUST_LOCK:
                jp = JOUST["players"].get(from_id) or {}
                if not name:
                    name = jp.get("name") or ""
                if not avatar:
                    avatar = jp.get("avatar") or ""
    if not name:
        name = fallback_name.strip()[:40]
    if not avatar.startswith("https://cdn.discordapp.com/"):
        avatar = ""
    return name, avatar


def _invite_page_shell(pic, title, subtitle, body_html, script_js=""):
    return (
        "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>PvZ2Native</title><style>"
        "body{font-family:system-ui,-apple-system,sans-serif;background:#0d2a2f;color:#f4f0dc;"
        "min-height:100vh;margin:0;display:flex;align-items:center;justify-content:center;padding:16px;box-sizing:border-box}"
        ".card{background:#1d3e42;border:4px solid #c9922e;border-radius:20px;max-width:460px;width:100%;"
        "padding:28px 24px;text-align:center;box-shadow:0 12px 32px rgba(0,0,0,.45)}"
        ".av{width:96px;height:96px;border-radius:50%;border:4px solid #e6b43c;object-fit:cover;margin-bottom:12px}"
        "h1{margin:6px 0 8px;font-size:1.45em;color:#fddc67}"
        "p{margin:6px 0 14px;color:#d6e4e5;line-height:1.45}"
        ".row{display:flex;gap:12px;justify-content:center;flex-wrap:wrap;margin-top:16px}"
        "a.b,button.b{display:inline-block;padding:12px 22px;border-radius:12px;border:2px solid #fff;"
        "font-size:1em;font-weight:700;color:#fff;text-decoration:none;cursor:pointer;"
        "box-shadow:0 3px 0 rgba(0,0,0,.35)}"
        ".green{background:linear-gradient(#4db830,#2a7818)}"
        ".red{background:linear-gradient(#cf3a32,#8a1d17)}"
        ".orange{background:linear-gradient(#d47a22,#8f470c)}"
        ".ghost{background:rgba(255,255,255,.08);border-color:rgba(255,255,255,.35);font-size:.9em;padding:9px 16px}"
        ".msg{margin-top:14px;font-weight:600;color:#fddc67}"
        "</style><div class=card>"
        f"{pic}<h1>{title}</h1><p>{subtitle}</p>{body_html}</div>{script_js}"
    )


def invite_code_for(did):
    """Per-user HMAC invite code so nobody can force-add a player just by guessing their Discord ID."""
    if not VALID_ID_RE.match(did):
        return ""
    return hmac.new(INVITE_SECRET, f"inv:{did}".encode("ascii"), hashlib.sha256).hexdigest()[:16]


def invite_code_valid(did, code):
    expected = invite_code_for(did)
    return bool(expected and isinstance(code, str) and len(code) == 16 and hmac.compare_digest(code, expected))


def invite_html(query):
    """/invite?from=<Discord id>&n=<name>&c=<hmac>: shows who invited you. Detects a running local PvZ2Native
    instance to offer Accept/Reject immediately; otherwise offers Download plus Discord web check /
    pvz2native:// protocol link."""
    q = urllib.parse.parse_qs(query)
    from_id = q.get("from", [""])[0].strip()
    raw_name = q.get("n", [""])[0].strip()[:40]
    code = q.get("c", [""])[0].strip()
    name, avatar = _inviter_profile(from_id, raw_name)
    who = html.escape(name) if name else "Un amigo"
    pic = f"<img class=av src='{html.escape(avatar)}' alt=''>" if avatar else ""
    valid_inviter = bool(VALID_ID_RE.match(from_id) and invite_code_valid(from_id, code))
    oauth_ready = bool(DISCORD_CLIENT_ID and DISCORD_CLIENT_SECRET and valid_inviter)

    proto_url = f"pvz2native://invite?from={urllib.parse.quote(from_id)}&n={urllib.parse.quote(name)}&c={urllib.parse.quote(code)}"
    oauth_url = f"/invite/oauth?from={urllib.parse.quote(from_id)}&n={urllib.parse.quote(name)}&c={urllib.parse.quote(code)}"

    body = (
        "<div id=actions>"
        f"<div id=dl class=row><a class='b green' href='{INVITE_DOWNLOAD_URL}'>Descargar / Download</a></div>"
        + (f"<div id=extra class=row>"
           + (f"<a class='b orange' href='{html.escape(oauth_url)}'>Ya tengo PvZ2Native (Discord)</a>" if oauth_ready else "")
           + f"<a class='b ghost' href='{html.escape(proto_url)}'>Abrir en PvZ2Native</a>"
           + "</div>" if valid_inviter else "")
        + "<div id=live class=row style='display:none'>"
        "<button class='b green' onclick=\"respond('accept')\">Aceptar / Accept</button>"
        "<button class='b red' onclick=\"respond('reject')\">Rechazar / Reject</button>"
        "</div>"
        "<div id=status class=msg></div>"
        "</div>"
    )
    script = ""
    if valid_inviter:
        js_from = json.dumps(from_id)
        js_name = json.dumps(name or "Un amigo")
        js_code = json.dumps(code)
        script = (
            "<script>"
            f"const FROM={js_from},NAME={js_name},CODE={js_code},BASE='http://127.0.0.1:{INVITE_LOCAL_PORT}';"
            "async function checkLocal(){"
            "try{const c=new AbortController();setTimeout(()=>c.abort(),650);"
            "const r=await fetch(BASE+'/ping',{signal:c.signal});"
            "if(r.ok){const d=await r.json();if(d&&d.ok){"
            "document.getElementById('dl').style.display='none';"
            "var ex=document.getElementById('extra');if(ex)ex.style.display='none';"
            "document.getElementById('live').style.display='flex';"
            "}}}catch(e){}}"
            "async function respond(act){"
            "const st=document.getElementById('status');"
            "st.textContent='Enviando...';"
            "try{const r=await fetch(BASE+'/invite',{method:'POST',"
            "headers:{'Content-Type':'application/json'},"
            "body:JSON.stringify({from:FROM,name:NAME,code:CODE,action:act})});"
            "if(r.ok){document.getElementById('live').style.display='none';"
            "st.textContent=act==='accept'?'¡Invitación aceptada! Ya aparece en tu lista de Amigos.':'Invitación rechazada.';}"
            "else{st.textContent='Vincula Discord dentro de PvZ2Native para aceptar amigos.';}}"
            "catch(e){st.textContent='No se pudo conectar con PvZ2Native.';}}"
            "checkLocal();"
            "</script>"
        )
    return _invite_page_shell(
        pic,
        f"{who} te invita a PvZ2Native",
        "Plants vs. Zombies 2 en PC, con amigos, guardado en la nube y Discord.",
        body,
        script,
    )


def invite_oauth_url(query):
    q = urllib.parse.parse_qs(query)
    from_id = q.get("from", [""])[0].strip()
    raw_name = q.get("n", [""])[0].strip()[:40]
    code = q.get("c", [""])[0].strip()
    if not (DISCORD_CLIENT_ID and DISCORD_CLIENT_SECRET and VALID_ID_RE.match(from_id) and invite_code_valid(from_id, code)):
        return None
    state = _sign_invite_token({"from": from_id, "n": raw_name, "c": code, "ts": int(time.time())})
    redir = f"{PUBLIC_BASE_URL}/invite/callback"
    return (
        "https://discord.com/oauth2/authorize"
        f"?client_id={urllib.parse.quote(DISCORD_CLIENT_ID)}"
        "&response_type=code&scope=identify"
        f"&redirect_uri={urllib.parse.quote(redir, safe='')}"
        f"&state={urllib.parse.quote(state, safe='')}"
    )


def invite_oauth_callback_html(query):
    q = urllib.parse.parse_qs(query)
    code = q.get("code", [""])[0].strip()
    state = q.get("state", [""])[0].strip()
    st = _verify_invite_token(state)
    if not code or not st or not (DISCORD_CLIENT_ID and DISCORD_CLIENT_SECRET):
        return _invite_page_shell("", "Enlace caducado", "Vuelve a abrir el enlace de invitación.",
                                  f"<div class=row><a class='b green' href='{INVITE_DOWNLOAD_URL}'>Descargar / Download</a></div>")
    from_id = st.get("from", "")
    raw_name = st.get("n", "")
    name, avatar = _inviter_profile(from_id, raw_name)
    who = html.escape(name) if name else "Un amigo"
    pic = f"<img class=av src='{html.escape(avatar)}' alt=''>" if avatar else ""
    try:
        redir = f"{PUBLIC_BASE_URL}/invite/callback"
        form = urllib.parse.urlencode({
            "client_id": DISCORD_CLIENT_ID,
            "client_secret": DISCORD_CLIENT_SECRET,
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": redir,
        }).encode("utf-8")
        req = urllib.request.Request(
            "https://discord.com/api/v10/oauth2/token",
            data=form,
            headers={"Content-Type": "application/x-www-form-urlencoded", "User-Agent": "PvZ2NativeCloud/1.0"},
        )
        with urllib.request.urlopen(req, timeout=8) as resp:
            tok = json.loads(resp.read().decode("utf-8"))
        access = tok.get("access_token", "")
        ureq = urllib.request.Request(
            "https://discord.com/api/v10/users/@me",
            headers={"Authorization": f"Bearer {access}", "User-Agent": "PvZ2NativeCloud/1.0"},
        )
        with urllib.request.urlopen(ureq, timeout=8) as resp:
            user = json.loads(resp.read().decode("utf-8"))
        me_id = str(user.get("id", ""))
    except Exception:
        return _invite_page_shell(pic, "No se pudo verificar Discord", "Inténtalo de nuevo más tarde.",
                                  f"<div class=row><a class='b green' href='{INVITE_DOWNLOAD_URL}'>Descargar / Download</a></div>")

    if not VALID_ID_RE.match(me_id) or not player_has_pvz2(me_id):
        return _invite_page_shell(
            pic,
            f"{who} te invita a PvZ2Native",
            "Todavía no hemos visto tu cuenta en PvZ2Native. ¡Descárgalo e inicia sesión con Discord para jugar juntos!",
            f"<div class=row><a class='b green' href='{INVITE_DOWNLOAD_URL}'>Descargar / Download</a></div>",
        )
    if me_id == from_id:
        return _invite_page_shell(pic, "¡Es tu propio enlace!", "Compártelo en Discord con un amigo.", "")
    if friend_are_friends(me_id, from_id):
        return _invite_page_shell(pic, f"Ya eres amigo de {who}", "Puedes verlo en el menú de Amigos dentro del juego.", "")

    act_tok = _sign_invite_token({"me": me_id, "from": from_id, "n": name, "c": st.get("c", ""), "ts": int(time.time())})
    body = (
        "<form method=POST action='/invite/respond' class=row>"
        f"<input type=hidden name=token value='{html.escape(act_tok)}'>"
        "<button class='b green' name=action value=accept>Aceptar / Accept</button>"
        "<button class='b red' name=action value=reject>Rechazar / Reject</button>"
        "</form>"
    )
    return _invite_page_shell(pic, f"{who} te invita a ser amigos", "¿Quieres aceptar la solicitud en PvZ2Native?", body)


def invite_web_respond_html(raw_body):
    form = urllib.parse.parse_qs(raw_body)
    tok = form.get("token", [""])[0]
    action = form.get("action", [""])[0]
    data = _verify_invite_token(tok)
    if not data:
        return _invite_page_shell("", "Solicitud caducada", "Vuelve a abrir el enlace de invitación.", "")
    me_id = data.get("me", "")
    from_id = data.get("from", "")
    name, avatar = _inviter_profile(from_id, data.get("n", ""))
    who = html.escape(name) if name else "tu amigo"
    pic = f"<img class=av src='{html.escape(avatar)}' alt=''>" if avatar else ""
    accept = (action == "accept")
    code, _ = friend_respond(me_id, from_id, accept, data.get("c", ""))
    if code != 200:
        return _invite_page_shell(pic, "Enlace de invitación no válido", "Pide un enlace nuevo a tu amigo.", "")
    if accept:
        return _invite_page_shell(pic, f"¡Ahora eres amigo de {who}!",
                                  "Abre el sobre de Amigos en PvZ2Native para ver su progreso y enviarle regalos.", "")
    return _invite_page_shell(pic, "Invitación rechazada", "No se ha añadido a tu lista de amigos.", "")


FRIENDS_FILE = os.path.join(DATA_DIR, "_friends.json")
FRIEND_LINKS_FILE = os.path.join(DATA_DIR, "_friend_links.json")
FRIENDS_LOCK = threading.Lock()
FRIENDS_ONLINE_SECS = 180  # a status this recent (sent every minute while playing) = playing now


def friend_links_load():
    try:
        with open(FRIEND_LINKS_FILE, "r", encoding="utf-8") as f:
            d = json.load(f)
        if isinstance(d, dict):
            return {"friends": d.get("friends") if isinstance(d.get("friends"), dict) else {},
                    "pending": d.get("pending") if isinstance(d.get("pending"), list) else []}
    except (OSError, ValueError):
        pass
    return {"friends": {}, "pending": []}


FRIEND_LINKS = friend_links_load()


def friend_links_save():
    tmp = FRIEND_LINKS_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(FRIEND_LINKS, f)
    os.replace(tmp, FRIEND_LINKS_FILE)


def player_has_pvz2(did):
    if not VALID_ID_RE.match(did):
        return False
    with FRIENDS_LOCK:
        if did in FRIENDS:
            return True
    with JOUST_LOCK:
        if did in JOUST["players"]:
            return True
    return os.path.isfile(os.path.join(DATA_DIR, f"{did}.json"))


def friend_are_friends(a, b):
    with FRIENDS_LOCK:
        return b in FRIEND_LINKS["friends"].get(a, [])


def _friend_add_pair_locked(a, b):
    fa = FRIEND_LINKS["friends"].setdefault(a, [])
    if b not in fa:
        fa.append(b)
    fb = FRIEND_LINKS["friends"].setdefault(b, [])
    if a not in fb:
        fb.append(a)
    FRIEND_LINKS["pending"] = [
        r for r in FRIEND_LINKS["pending"]
        if not ((r.get("from") == a and r.get("to") == b) or (r.get("from") == b and r.get("to") == a))
    ]
    friend_links_save()


def friend_request(from_id, to_id):
    if not VALID_ID_RE.match(from_id) or not VALID_ID_RE.match(to_id) or from_id == to_id:
        return 400, "invalid_id"
    with FRIENDS_LOCK:
        if to_id in FRIEND_LINKS["friends"].get(from_id, []):
            return 200, "already_friends"
        for r in FRIEND_LINKS["pending"]:
            if r.get("from") == to_id and r.get("to") == from_id:
                _friend_add_pair_locked(from_id, to_id)
                return 200, "accepted"
            if r.get("from") == from_id and r.get("to") == to_id:
                return 200, "pending"
        FRIEND_LINKS["pending"].append({"from": from_id, "to": to_id, "at": int(time.time())})
        friend_links_save()
    return 200, "pending"


def friend_respond(me, from_id, accept, invite_code=""):
    if not VALID_ID_RE.match(me) or not VALID_ID_RE.match(from_id) or me == from_id:
        return 400, "invalid_id"
    with FRIENDS_LOCK:
        has_pending = any(r.get("from") == from_id and r.get("to") == me for r in FRIEND_LINKS["pending"])
        if accept:
            if not has_pending and not invite_code_valid(from_id, invite_code):
                return 403, "invalid_invite_code"
            _friend_add_pair_locked(me, from_id)
            return 200, "accepted"
        FRIEND_LINKS["pending"] = [
            r for r in FRIEND_LINKS["pending"]
            if not (r.get("from") == from_id and r.get("to") == me)
        ]
        friend_links_save()
    return 200, "rejected"


def friend_remove(me, other_id):
    """Remove mutual friendship; intentionally leaves GIFTS['sent'] untouched so daily gift limits hold."""
    if not VALID_ID_RE.match(me) or not VALID_ID_RE.match(other_id) or me == other_id:
        return 400, "invalid_id"
    with FRIENDS_LOCK:
        fa = FRIEND_LINKS["friends"].get(me)
        if fa and other_id in fa:
            fa.remove(other_id)
        fb = FRIEND_LINKS["friends"].get(other_id)
        if fb and me in fb:
            fb.remove(me)
        FRIEND_LINKS["pending"] = [
            r for r in FRIEND_LINKS["pending"]
            if not ((r.get("from") == me and r.get("to") == other_id) or
                    (r.get("from") == other_id and r.get("to") == me))
        ]
        friend_links_save()
    return 200, "removed"


def friends_load():
    try:
        with open(FRIENDS_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


FRIENDS = friends_load()


def friends_save():
    tmp = FRIENDS_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(FRIENDS, f)
    os.replace(tmp, FRIENDS_FILE)


def friends_status(discord_id, name, body):
    """POST /friends/v1/status: what the player's game reports about itself, about once a minute."""
    def num(k):
        try:
            return max(0, min(int(body.get(k, 0)), 10 ** 9))
        except (TypeError, ValueError):
            return 0

    def text(k, n=32):
        return re.sub(r"[^0-9A-Za-z._ -]", "", str(body.get(k, "")))[:n]

    # Newer games send the completed map events ("map"); the counts are worked out here, as for cloud saves.
    worlds, levels = num("worlds"), num("levels")
    if body.get("map"):
        worlds, levels = progress_from_map(parse_map_string(body.get("map")))
    with FRIENDS_LOCK:
        FRIENDS[discord_id] = {
            "name": name, "avatar": discord_cached_avatar(discord_id), "lastSeen": int(time.time()),
            "playing": bool(body.get("playing", True)), "version": text("version", 12),
            "level": text("level"), "plants": num("plants"), "worlds": worlds, "levels": levels,
            "where": text("where", 8), "world": text("world"),
        }
        friends_save()


GIFTS_FILE = os.path.join(DATA_DIR, "_gifts.json")
GIFT_COINS = 1000  # what one gift carries
GIFT_GEMS = 5


def gifts_load():
    try:
        with open(GIFTS_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            data.setdefault("pending", {})
            data.setdefault("sent", {})
            return data
    except (OSError, ValueError):
        pass
    return {"pending": {}, "sent": {}}


GIFTS = gifts_load()


def gifts_save():
    tmp = GIFTS_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(GIFTS, f)
    os.replace(tmp, GIFTS_FILE)


def gift_day(now=None):
    return int((now if now is not None else time.time()) // 86400)


def gift_can_send(sender, recipient):
    """Caller holds FRIENDS_LOCK."""
    return GIFTS["sent"].get(sender + ":" + recipient) != gift_day()


def gift_send(sender, sender_name, recipient):
    """POST /friends/v1/gift {"to": id}: one gift per friend per (UTC) day, kept until the recipient's game
    takes it (it is written into their save the next time the game starts)."""
    with FRIENDS_LOCK:
        if recipient == sender or recipient not in FRIENDS:
            return 404, "unknown_player"
        if recipient not in FRIEND_LINKS["friends"].get(sender, []):
            return 403, "not_friends"
        if not gift_can_send(sender, recipient):
            return 429, "already_sent_today"
        GIFTS["sent"][sender + ":" + recipient] = gift_day()
        GIFTS["pending"].setdefault(recipient, []).append({
            "id": secrets.token_hex(8), "from": sender, "fromName": sender_name,
            "coins": GIFT_COINS, "gems": GIFT_GEMS, "ts": int(time.time())})
        # Old send marks are only needed for today.
        today = gift_day()
        GIFTS["sent"] = {k: d for k, d in GIFTS["sent"].items() if d == today}
        gifts_save()
    return 200, "ok"


def gifts_tsv(me):
    """GET /friends/v1/gifts: gifts waiting for this player, one per line: id, sender name, coins, gems."""
    with FRIENDS_LOCK:
        items = list(GIFTS["pending"].get(me, []))
    return "".join("%s\t%s\t%d\t%d\n" % (g["id"], re.sub(r"[\t\r\n]", " ", g.get("fromName", ""))[:80],
                                           g.get("coins", 0), g.get("gems", 0)) for g in items)


def gifts_ack(me, ids):
    """POST /friends/v1/ack {"ids": [...]}: the game has written these gifts into its save."""
    ids = {str(i) for i in ids} if isinstance(ids, list) else set()
    with FRIENDS_LOCK:
        left = [g for g in GIFTS["pending"].get(me, []) if g["id"] not in ids]
        if left:
            GIFTS["pending"][me] = left
        else:
            GIFTS["pending"].pop(me, None)
        gifts_save()


class _Rton:
    """Just enough of an RTON v1 reader to walk a PvZ2 save (pp.dat): values it does not need (floats,
    RTIDs) are read past, not kept."""
    _FIXED = {0x08: "<b", 0x0a: "<B", 0x10: "<h", 0x12: "<H", 0x20: "<i", 0x26: "<I", 0x22: "<f",
              0x42: "<d", 0x40: "<q", 0x46: "<Q"}

    def __init__(self, data):
        if data[:4] != b"RTON":
            raise ValueError("not RTON")
        self.d, self.p, self.c1, self.c2 = data, 8, [], []

    def byte(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def varint(self):
        r = sh = 0
        while True:
            x = self.byte()
            r |= (x & 0x7f) << sh
            sh += 7
            if not x & 0x80:
                return r

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def s8(self):
        return self.raw(self.varint()).decode("latin1")

    def s16(self):
        self.varint()
        return self.raw(self.varint()).decode("utf-8", "replace")

    def value(self, t):
        if t in (0, 1):
            return bool(t)
        if t in (0x09, 0x0b, 0x11, 0x13, 0x21, 0x27, 0x41, 0x47):
            return 0
        if t in (0x23, 0x43):
            return 0.0
        if t in self._FIXED:
            fmt = self._FIXED[t]
            return struct.unpack(fmt, self.raw(struct.calcsize(fmt)))[0]
        if t in (0x24, 0x28, 0x44, 0x48):
            return self.varint()
        if t in (0x25, 0x29, 0x45, 0x49):
            v = self.varint()
            return (v >> 1) ^ -(v & 1)
        if t == 0x81:
            return self.s8()
        if t == 0x82:
            return self.s16()
        if t == 0x90:
            v = self.s8()
            self.c1.append(v)
            return v
        if t == 0x91:
            return self.c1[self.varint()]
        if t == 0x92:
            v = self.s16()
            self.c2.append(v)
            return v
        if t == 0x93:
            return self.c2[self.varint()]
        if t == 0x84:
            return None
        if t == 0x83:
            sub = self.byte()
            if sub == 3:
                self.s16()
                self.s16()
            elif sub == 2:
                self.s16()
                self.varint()
                self.varint()
            elif sub != 0:
                raise ValueError("rtid %x" % sub)
            return None
        if t == 0x85:
            return self.obj()
        if t == 0x86:
            if self.byte() != 0xfd:
                raise ValueError("array")
            a = [self.value(self.byte()) for _ in range(self.varint())]
            if self.byte() != 0xfe:
                raise ValueError("array end")
            return a
        raise ValueError("tag %x" % t)

    def obj(self):
        o = {}
        while True:
            t = self.byte()
            if t == 0xff:
                return o
            k = self.value(t)
            o[k] = self.value(self.byte())


LEVEL_EVENTS = {  # world id -> map event ids of type "level" (WORLDMAP.RTON of the 6.6.1 OBB; 4.5.2 is a subset)
    1: "1-4,6",  # tutorial
    2: "1-10,19-20,22,24-26,29-31,34-37,40,42,84-86,88-90,92-95,97",  # egypt
    3: "1-10,48-65,67-69,71-74,76",  # pirate
    4: "1-10,27,84-97,110,112,114-117,119-122,124",  # cowboy
    5: "1,17,41-44,46-56,60-70,72-74,76-79,81",  # future
    6: "1,17,41-44,46-56,60-62,68",  # dark
    7: "1,17,41-44,46-56,60-62,68-80",  # beach
    8: "1,17,41-44,46-53,55-56,60-61,68,70-80,85",  # iceage
    9: "1,13-15,17-19,21-22,24-25,27,29-30,32-34,36-37,39-43,45-47,49-50,52-53,55-56",  # lostcity
    10: "1,13-15,17-19,21-22,24-25,27,29-30,32-49,53",  # eighties
    11: "1,13-15,17-19,21-22,24-25,27,29-30,32-34,36-41,43-52,55,57-60,62-65,67",  # dino
    12: "1,13-15,17-19,21-22,24-25,27,29-30,32-33,35-52,57-59,61-63,65-68,70",  # modern
}
TUTORIAL_WORLD = 1


def _ranges(spec):
    ids = set()
    for part in spec.split(","):
        a, _, b = part.partition("-")
        ids.update(range(int(a), int(b or a) + 1))
    return ids


LEVEL_EVENT_SETS = {w: _ranges(s) for w, s in LEVEL_EVENTS.items()}


def progress_from_map(completed):
    """{world id: [completed map event ids]} (the save's PlayerInfo `wmed`) -> (open worlds, completed levels).
    The save lists every completed map event -- plant rewards, gates and upgrades too -- so only the events
    that are levels on that world's map count. A world is open once it has an entry; the tutorial is not
    counted as a world."""
    worlds = levels = 0
    for wid, ids in completed.items():
        if wid not in LEVEL_EVENT_SETS:
            continue
        if wid != TUTORIAL_WORLD:
            worlds += 1
        levels += len(LEVEL_EVENT_SETS[wid] & set(ids))
    return worlds, levels


def parse_map_string(text):
    """What the game reports instead of a cloud save: "2:1,2,3;5:1,11" -> {2: [1, 2, 3], 5: [1, 11]}."""
    out = {}
    for chunk in str(text or "")[:20000].split(";"):
        wid, _, ids = chunk.partition(":")
        try:
            out[int(wid)] = [int(i) for i in ids.split(",") if i.strip()]
        except ValueError:
            continue
    return out


SAVE_PROGRESS_CACHE = {}  # Discord id -> (save file mtime, progress dict or None)


def save_progress(discord_id):
    """Plants, worlds and map levels of the player's CLOUD save (userdata/pp.dat in /data/saves/<id>.json),
    so the friends list shows each account's own game even when two accounts share one PC. None when the
    account has no cloud save or it cannot be read."""
    path = os.path.join(DATA_DIR, f"{discord_id}.json")
    try:
        mtime = os.path.getmtime(path)
    except OSError:
        return None
    cached = SAVE_PROGRESS_CACHE.get(discord_id)
    if cached and cached[0] == mtime:
        return cached[1]
    progress = None
    try:
        with open(path, "r", encoding="utf-8") as f:
            bundle = json.load(f)
        blob = (bundle.get("files") or {}).get("userdata/pp.dat")
        if blob:
            info = _Rton(base64.b64decode(blob)).obj()["objects"][0]["objdata"]
            completed = {}
            for w in info.get("wmed") or []:
                if isinstance(w, dict) and isinstance(w.get("w"), int):
                    completed[w["w"]] = [e.get("i") for e in (w.get("e") or []) if isinstance(e, dict)]
            worlds, levels = progress_from_map(completed)
            progress = {"plants": len(info.get("p") or []), "worlds": worlds, "levels": levels}
    except Exception:
        progress = None
    SAVE_PROGRESS_CACHE[discord_id] = (mtime, progress)
    return progress


def friends_tsv(me):
    """GET /friends/v1/list: incoming friend requests first (column 13 = 'request'), then accepted friends
    (column 13 = 'friend'), tab-separated: id, name, avatar URL, playing now (1/0), seconds since last seen,
    version, last level, plants, worlds, levels, can be sent a gift today (1/0), where, world, kind."""
    now = int(time.time())
    req_rows = []
    friend_rows = []
    with FRIENDS_LOCK:
        accepted = set(FRIEND_LINKS["friends"].get(me, []))
        pending_from = [r.get("from", "") for r in FRIEND_LINKS["pending"] if r.get("to") == me]
        for did in pending_from:
            if not did or did == me or did in accepted:
                continue
            p = FRIENDS.get(did) or {}
            ago = max(0, now - int(p.get("lastSeen", 0))) if p.get("lastSeen") else 0
            online = ago < FRIENDS_ONLINE_SECS and p.get("playing", False)
            req_rows.append((did, p, online, ago))
        for did in accepted:
            if not did or did == me:
                continue
            p = FRIENDS.get(did) or {}
            ago = max(0, now - int(p.get("lastSeen", 0))) if p.get("lastSeen") else 999999
            online = ago < FRIENDS_ONLINE_SECS and p.get("playing", False)
            friend_rows.append((not online, ago, did, p, online, gift_can_send(me, did)))
    friend_rows.sort(key=lambda r: (r[0], r[1]))

    def clean(v):
        return re.sub(r"[\t\r\n]", " ", str(v))[:80]

    def url(v):  # a Discord avatar URL is ~100 characters: cut at 80 it broke every picture
        return re.sub(r"[\t\r\n]", " ", str(v))[:300]

    lines = []
    for did, p, online, ago in req_rows:
        name, avatar = _inviter_profile(did, p.get("name", ""))
        prog = save_progress(did) or p
        lines.append("\t".join(clean(v) for v in (did, name or "Jugador")) + "\t" + url(avatar) +
                     "\t" + "\t".join(clean(v) for v in (
                         1 if online else 0, ago, p.get("version", ""), p.get("level", ""), prog.get("plants", 0),
                         prog.get("worlds", 0), prog.get("levels", 0), 0, p.get("where", ""),
                         p.get("world", ""), "request")) + "\n")
    for _, ago, did, p, online, can_gift in friend_rows:
        name, avatar = _inviter_profile(did, p.get("name", ""))
        prog = save_progress(did) or p
        lines.append("\t".join(clean(v) for v in (did, name or "Jugador")) + "\t" + url(avatar) +
                     "\t" + "\t".join(clean(v) for v in (
                         1 if online else 0, ago, p.get("version", ""), p.get("level", ""), prog.get("plants", 0),
                         prog.get("worlds", 0), prog.get("levels", 0), 1 if can_gift else 0, p.get("where", ""),
                         p.get("world", ""), "friend")) + "\n")
    return "".join(lines)


class SaveHandler(BaseHTTPRequestHandler):
    def _client_ip(self):
        cf_ip = self.headers.get("CF-Connecting-IP")
        if cf_ip:
            return cf_ip.strip()
        xff = self.headers.get("X-Forwarded-For")
        if xff:
            return xff.split(",")[0].strip()
        return self.client_address[0]

    def _check_rate_limit(self, kind="saves", max_req=RATE_LIMIT_MAX_REQ) -> bool:
        key = (kind, self._client_ip())
        now = time.time()
        with JOUST_LOCK:
            bucket = [t for t in RATE_BUCKETS.get(key, []) if now - t < RATE_LIMIT_WINDOW]
            if len(bucket) >= max_req:
                RATE_BUCKETS[key] = bucket
                return False
            bucket.append(now)
            RATE_BUCKETS[key] = bucket
        return True

    def _joust(self, method):
        """/joust/v1/<route>: Battlez for the Discord account in X-Discord-Id + Authorization."""
        if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
            self._send_json(429, {"error": "rate_limit_exceeded"})
            return
        route = self.path.split("?", 1)[0][len("/joust/v1/"):].strip("/")
        discord_id = self.headers.get("X-Discord-Id", "").strip()
        auth = self.headers.get("Authorization", "").strip()
        name = None
        if VALID_ID_RE.match(discord_id) and auth.lower().startswith("bearer "):
            claimed = urllib.parse.unquote(self.headers.get("X-Discord-Name", "")).strip()
            name = discord_user_name(auth[7:].strip(), discord_id, claimed)
            claimed_avatar = self.headers.get("X-Discord-Avatar", "").strip()
            if name is not None and re.fullmatch(r"(a_)?[0-9a-f]{32}", claimed_avatar):
                # Same rule as the name: only for an account verified by its token, and only when the
                # avatar could not be read from Discord itself.
                with JOUST_LOCK:
                    for entry in TOKEN_CACHE.values():
                        if entry.get("id") == discord_id and not entry.get("avatar"):
                            entry["avatar"] = discord_avatar_url(discord_id, claimed_avatar)
        if name is None:
            self._send_json(401, {"ErrorCode": "INVALID_PLAYER_ID"})
            return
        body = {}
        if method == "POST":
            length = int(self.headers.get("Content-Length", "0") or 0)
            if length < 0 or length > 1024 * 1024:
                self._send_json(413, {"error": "invalid_payload_size"})
                return
            try:
                # The game's request body can carry a stray byte or a NUL after the JSON value
                # (older ports sent them): read the first value and ignore what follows it.
                raw = self.rfile.read(length).decode("utf-8", "replace").strip("\x00 \r\n\t")
                body = json.JSONDecoder().raw_decode(raw)[0] if raw else {}
                if not isinstance(body, dict):
                    body = {}
            except Exception:
                self._send_json(400, {"error": "invalid_json"})
                return
        battlez_mode = self.headers.get("X-Battlez-Mode", "live").strip().lower()
        code, out = joust_handle(route, discord_id, name, body, battlez_mode=battlez_mode)
        self._send_json(code, out)

    def _friends(self, method):
        """/friends/v1/status (POST) and /friends/v1/list (GET), for the Discord account in X-Discord-Id +
        Authorization (same check as Battlez). Both answer with the list."""
        if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
            self._send_json(429, {"error": "rate_limit_exceeded"})
            return
        route = self.path.split("?", 1)[0][len("/friends/v1/"):].strip("/")
        discord_id = self.headers.get("X-Discord-Id", "").strip()
        auth = self.headers.get("Authorization", "").strip()
        name = None
        if VALID_ID_RE.match(discord_id) and auth.lower().startswith("bearer "):
            claimed = urllib.parse.unquote(self.headers.get("X-Discord-Name", "")).strip()
            name = discord_user_name(auth[7:].strip(), discord_id, claimed)
        if name is None:
            self._send_json(401, {"error": "unauthorized_discord_token"})
            return
        body = {}
        if method == "POST":
            length = int(self.headers.get("Content-Length", "0") or 0)
            if length < 0 or length > 64 * 1024:
                self._send_json(413, {"error": "invalid_payload_size"})
                return
            try:
                raw = self.rfile.read(length).decode("utf-8", "replace").strip("\x00 \r\n\t")
                body = json.JSONDecoder().raw_decode(raw)[0] if raw else {}
            except Exception:
                body = {}
            if not isinstance(body, dict):
                body = {}
        code, text = 200, None
        if method == "POST" and route == "status":
            friends_status(discord_id, name, body)
        elif method == "POST" and route == "gift":
            code, text = gift_send(discord_id, name, str(body.get("to", "")))
        elif method == "POST" and route == "accept":
            code, _ = friend_respond(discord_id, str(body.get("from", "")), True, str(body.get("code", "")))
        elif method == "POST" and route == "reject":
            code, _ = friend_respond(discord_id, str(body.get("from", "")), False)
        elif method == "POST" and route == "remove":
            code, _ = friend_remove(discord_id, str(body.get("id", "")))
        elif method == "POST" and route == "request":
            code, text = friend_request(discord_id, str(body.get("to", "")))
        elif method == "GET" and route == "invite-code":
            text = invite_code_for(discord_id)
        elif method == "GET" and route == "gifts":
            text = gifts_tsv(discord_id)
        elif method == "POST" and route == "ack":
            gifts_ack(discord_id, body.get("ids"))
            text = "ok"
        elif not (method == "GET" and route == "list"):
            self._send_json(404, {"error": "not_found"})
            return
        out = (friends_tsv(discord_id) if text is None else text).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def _send_html(self, text):
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.startswith("/joust/v1/"):
            self._joust("POST")
            return
        if self.path.startswith("/friends/v1/"):
            self._friends("POST")
            return
        if self.path.split("?", 1)[0].rstrip("/") == "/invite/respond":
            if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
                self._send_json(429, {"error": "rate_limit_exceeded"})
                return
            length = int(self.headers.get("Content-Length", "0") or 0)
            raw = self.rfile.read(min(max(length, 0), 16384)).decode("utf-8", "replace") if length > 0 else ""
            self._send_html(invite_web_respond_html(raw))
            return
        self._send_json(404, {"error": "not_found"})

    def _send_json(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _extract_discord_id(self):
        path = self.path.split("?", 1)[0].strip("/")
        parts = path.split("/")
        if len(parts) == 2 and parts[0] == "saves" and VALID_ID_RE.match(parts[1]):
            return parts[1]
        return None

    def _authenticate(self, discord_id: str) -> bool:
        auth = self.headers.get("Authorization", "").strip()
        if not auth.lower().startswith("bearer "):
            return False
        token = auth[7:].strip()
        return verify_discord_token(token, discord_id)

    def do_HEAD(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            return
        self.send_response(404)
        self.end_headers()

    def do_GET(self):
        if self.path == "/health":
            self._send_json(200, {"ok": True, "service": "pvz2-cloud"})
            return

        if self.path.startswith("/joust/v1/"):
            self._joust("GET")
            return
        if self.path.startswith("/friends/v1/"):
            self._friends("GET")
            return
        path = self.path.split("?", 1)[0].rstrip("/")
        if path == "/joust/admin/preview-results":
            # Admin: everyone in this week's tournament gets the results screen of the current standings on
            # their next dashboard, leagues untouched -- to see the tournament end without waiting a week.
            # Needs the key in <data dir>/_admin_key (no file = disabled).
            if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
                self._send_json(429, {"error": "rate_limit_exceeded"})
                return
            key = urllib.parse.parse_qs(self.path.split("?", 1)[1] if "?" in self.path else "").get("key", [""])[0]
            try:
                with open(os.path.join(DATA_DIR, "_admin_key"), "r", encoding="utf-8") as f:
                    expected = f.read().strip()
            except OSError:
                expected = ""
            if len(expected) < 16 or not secrets.compare_digest(key, expected):
                self._send_json(403, {"error": "forbidden"})
                return
            with JOUST_LOCK:
                joust_close_week(joust_week(), apply=False)
                pending = {q.get("name", "Player"): q["pendingResults"] for q in JOUST["players"].values()
                           if q.get("pendingResults")}
                joust_save()
            self._send_json(200, {"ok": True, "pending": pending})
            return
        if path in ("/battlez", "/battlez.json"):
            if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
                self._send_json(429, {"error": "rate_limit_exceeded"})
            elif path.endswith(".json"):
                self._send_json(200, {"tournament": joust_week() + 1, "table": joust_global_table()})
            else:
                self._send_html(joust_global_html())
            return
        if path == "/invite":
            if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
                self._send_json(429, {"error": "rate_limit_exceeded"})
            else:
                self._send_html(invite_html(self.path.split("?", 1)[1] if "?" in self.path else ""))
            return
        if path == "/invite/oauth":
            if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
                self._send_json(429, {"error": "rate_limit_exceeded"})
                return
            url = invite_oauth_url(self.path.split("?", 1)[1] if "?" in self.path else "")
            if not url:
                self._send_html(invite_html(self.path.split("?", 1)[1] if "?" in self.path else ""))
                return
            self.send_response(302)
            self.send_header("Location", url)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/invite/callback":
            if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
                self._send_json(429, {"error": "rate_limit_exceeded"})
            else:
                self._send_html(invite_oauth_callback_html(self.path.split("?", 1)[1] if "?" in self.path else ""))
            return

        if not self._check_rate_limit():
            self._send_json(429, {"error": "rate_limit_exceeded"})
            return

        discord_id = self._extract_discord_id()
        if not discord_id:
            self._send_json(400, {"error": "invalid_discord_id"})
            return

        if not self._authenticate(discord_id):
            self._send_json(401, {"error": "unauthorized_discord_token"})
            return

        save_path = os.path.join(DATA_DIR, f"{discord_id}.json")
        if not os.path.isfile(save_path):
            self._send_json(404, {"error": "not_found"})
            return

        with open(save_path, "rb") as f:
            data = f.read()

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self):
        if not self._check_rate_limit():
            self._send_json(429, {"error": "rate_limit_exceeded"})
            return

        discord_id = self._extract_discord_id()
        if not discord_id:
            self._send_json(400, {"error": "invalid_discord_id"})
            return

        header_id = self.headers.get("X-Discord-Id", "").strip()
        if header_id and header_id != discord_id:
            self._send_json(403, {"error": "discord_id_mismatch"})
            return

        if not self._authenticate(discord_id):
            self._send_json(401, {"error": "unauthorized_discord_token"})
            return

        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_BODY_BYTES:
            self._send_json(413, {"error": "invalid_payload_size"})
            return

        raw = self.rfile.read(length)
        try:
            parsed = json.loads(raw.decode("utf-8"))
            if parsed.get("format") not in ("pvz2_cloud_v1", "pvz2native-cloud-v1") or "files" not in parsed:
                self._send_json(400, {"error": "invalid_save_format"})
                return
        except Exception:
            self._send_json(400, {"error": "invalid_json"})
            return

        save_path = os.path.join(DATA_DIR, f"{discord_id}.json")
        tmp_path = save_path + ".tmp"
        with open(tmp_path, "wb") as f:
            f.write(raw)
        os.replace(tmp_path, save_path)

        self._send_json(200, {"ok": True, "discord_id": discord_id, "bytes": len(raw)})


if __name__ == "__main__":
    print(f"[PvZ2 Cloud Server] Listening on 0.0.0.0:{PORT} (saves -> {DATA_DIR})")
    ThreadingHTTPServer(("0.0.0.0", PORT), SaveHandler).serve_forever()

#!/usr/bin/env python3
import hashlib
import html
import json
import os
import random
import re
import secrets
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
                TOKEN_CACHE[tok_hash] = {"id": real_id, "name": name, "ts": int(time.time())}
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
# Names: the game shows a player's name from a NUMBER. Every Discord account gets one from 1000000 up
# (`nameId`), and each response carries {"names": {"<nameId>": "<Discord name>"}} for the numbers in it,
# which the port shows instead of the game's own names.

JOUST_FILE = os.path.join(DATA_DIR, "_joust.json")
JOUST_LOCK = threading.Lock()
JOUST_SCHEDULE_BASE = 1767571200  # Monday 2026-01-05 00:00 UTC
JOUST_WEEK = 7 * 24 * 3600
JOUST_LEVEL_SETS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 22, 27]
JOUST_NAME_BASE = 1000000
JOUST_MAX_REPLAYS = 500
JOUST_MAX_REPLAY_CHARS = 64 * 1024
JOUST_CROWNS_WIN = 5   # CrownSet_Normal
JOUST_CROWNS_LOSS = 1
JOUST_RATE_MAX_REQ = 120


def joust_load():
    try:
        with open(JOUST_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        data = {}
    data.setdefault("players", {})
    data.setdefault("replays", [])
    data.setdefault("matches", {})
    data.setdefault("nextNameId", JOUST_NAME_BASE + 1)
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
        p = {"nameId": JOUST["nextNameId"], "icon": 0, "league": 1, "crowns": 0, "crownStreak": 0,
             "rewardStreak": 0, "difficultyBand": 0, "totalWins": 0, "tournament": joust_week()}
        JOUST["nextNameId"] += 1
        players[discord_id] = p
    p["name"] = name
    week = joust_week()
    if p.get("tournament") != week:
        p["tournament"] = week
        p["crowns"] = 0
    return p


def joust_state(p):
    return {k: p[k] for k in ("league", "crowns", "crownStreak", "rewardStreak", "difficultyBand",
                              "totalWins", "icon")} | {"name": p["nameId"]}


def joust_table(league=None):
    """Players of this week's tournament (one league, or all of them), most crowns first."""
    week = joust_week()
    rows = [(did, p) for did, p in JOUST["players"].items()
            if p.get("tournament") == week and (league is None or p.get("league") == league)]
    rows.sort(key=lambda r: (-r[1].get("crowns", 0), r[1].get("nameId", 0)))
    return rows


def joust_dashboard(discord_id, p):
    week = joust_week()
    rows = joust_table(p["league"])
    board, names, position = [], {}, 1
    for i, (did, q) in enumerate(rows):
        if did == discord_id:
            position = i + 1
        board.append({"name": q["nameId"], "icon": q.get("icon", 0), "crowns": q.get("crowns", 0)})
        names[str(q["nameId"])] = q.get("name", "Player")
    end_left = JOUST_SCHEDULE_BASE + (week + 1) * JOUST_WEEK - int(time.time())
    return {"tournament": {"tournamentId": week + 1, "endTime": max(0, end_left)},
            "position": position, "state": joust_state(p), "leaderboard": board, "names": names}


def joust_match(discord_id, p):
    """An opponent: the replay of another player on this week's level, else the game's own AI."""
    seed = joust_level_seed(joust_week())
    pool = [r for r in JOUST["replays"] if r["levelSeed"] == seed and r["player"] != discord_id]
    same_league = [r for r in pool if r.get("league") == p["league"]]
    pool = (same_league or pool)[-50:]
    match_id = "m%d-%s" % (int(time.time() * 1000), secrets.token_hex(4))
    names = {}
    if pool:
        r = random.choice(pool)
        q = JOUST["players"].get(r["player"], {})
        opp = {"opponentId": r["player"], "name": q.get("nameId", 0), "icon": q.get("icon", 0),
               "league": r.get("league", 1), "levelSeed": seed, "score": r["score"], "replay": r["replay"]}
        names[str(q.get("nameId", 0))] = q.get("name", "Player")
    else:
        opp = {"opponentId": "ai-%d" % random.randint(1, 10 ** 6), "name": random.randint(1, 9980),
               "icon": random.randint(1, 25), "league": p["league"], "levelSeed": seed, "score": 0,
               "replay": ""}
    JOUST["matches"][match_id] = {"player": discord_id, "opponentId": opp["opponentId"], "levelSeed": seed,
                                  "ts": int(time.time())}
    # Forget matches nobody finished after a day.
    cutoff = time.time() - 86400
    for mid in [m for m, v in JOUST["matches"].items() if v.get("ts", 0) < cutoff]:
        del JOUST["matches"][mid]
    return {"matchId": match_id, "opponents": [opp], "names": names}


def joust_complete(discord_id, p, body):
    r = body.get("r") or {}
    match = JOUST["matches"].get(str(body.get("matchId", "")))
    if match is None or match.get("player") != discord_id:
        return 400, {"ErrorCode": "INVALID_MATCH_ID"}
    win = float(body.get("winRatio") or 0) > 1.0
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
    if replay and len(replay) <= JOUST_MAX_REPLAY_CHARS:
        JOUST["replays"].append({"player": discord_id, "levelSeed": str(r.get("levelSeed") or match["levelSeed"]),
                                 "league": p["league"], "score": int(r.get("score") or 0), "replay": replay,
                                 "ts": int(time.time())})
        del JOUST["replays"][:-JOUST_MAX_REPLAYS]
    match["done"] = True
    return 200, {"grantedRewards": {"crowns": granted}, "state": joust_state(p)}


def joust_finalize(discord_id, p, body):
    JOUST["matches"].pop(str(body.get("matchId", "")), None)
    return 200, {"multiplier": 1, "rewards": [],
                 "streaks": {"reward": p["rewardStreak"], "crown": p["crownStreak"],
                             "difficulty": p["difficultyBand"]}}


def joust_handle(route, discord_id, name, body):
    """(status, payload) for one Battlez request from an authenticated Discord account."""
    with JOUST_LOCK:
        p = joust_player(discord_id, name)
        if route in ("dashboard", "registerForTournament"):
            code, out = 200, joust_dashboard(discord_id, p)
        elif route == "avatar":
            try:
                p["icon"] = int(body.get("icon", p["icon"]))
            except (TypeError, ValueError):
                pass
            code, out = 200, joust_dashboard(discord_id, p)
        elif route == "match":
            code, out = 200, joust_match(discord_id, p)
        elif route == "completeMatch":
            code, out = joust_complete(discord_id, p, body)
        elif route == "finalizeMatch":
            code, out = joust_finalize(discord_id, p, body)
        else:
            code, out = 404, {"error": "unknown_route"}
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
                # The game's request body can end with its C string terminator.
                raw = self.rfile.read(length).rstrip(b"\x00 \r\n\t")
                body = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(body, dict):
                    body = {}
            except Exception:
                self._send_json(400, {"error": "invalid_json"})
                return
        code, out = joust_handle(route, discord_id, name, body)
        self._send_json(code, out)

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
        path = self.path.split("?", 1)[0].rstrip("/")
        if path in ("/battlez", "/battlez.json"):
            if not self._check_rate_limit("joust", JOUST_RATE_MAX_REQ):
                self._send_json(429, {"error": "rate_limit_exceeded"})
            elif path.endswith(".json"):
                self._send_json(200, {"tournament": joust_week() + 1, "table": joust_global_table()})
            else:
                self._send_html(joust_global_html())
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

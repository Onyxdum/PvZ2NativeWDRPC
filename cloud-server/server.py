#!/usr/bin/env python3
import hashlib
import json
import os
import re
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

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
    if not token or len(token) < 10:
        return False
    tok_hash = hashlib.sha256(token.encode("utf-8")).hexdigest()
    cached = TOKEN_CACHE.get(tok_hash)
    if cached and cached.get("id") == expected_id:
        return True

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
                return False
            user = json.loads(resp.read().decode("utf-8"))
            real_id = str(user.get("id", ""))
            if real_id and real_id == expected_id:
                TOKEN_CACHE[tok_hash] = {"id": real_id, "ts": int(time.time())}
                save_token_cache()
                return True
    except Exception:
        return False
    return False


class SaveHandler(BaseHTTPRequestHandler):
    def _client_ip(self):
        cf_ip = self.headers.get("CF-Connecting-IP")
        if cf_ip:
            return cf_ip.strip()
        xff = self.headers.get("X-Forwarded-For")
        if xff:
            return xff.split(",")[0].strip()
        return self.client_address[0]

    def _check_rate_limit(self) -> bool:
        ip = self._client_ip()
        now = time.time()
        bucket = RATE_BUCKETS.get(ip, [])
        bucket = [t for t in bucket if now - t < RATE_LIMIT_WINDOW]
        if len(bucket) >= RATE_LIMIT_MAX_REQ:
            RATE_BUCKETS[ip] = bucket
            return False
        bucket.append(now)
        RATE_BUCKETS[ip] = bucket
        return True

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
    HTTPServer(("0.0.0.0", PORT), SaveHandler).serve_forever()

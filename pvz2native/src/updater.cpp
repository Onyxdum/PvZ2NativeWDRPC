#include <pvz2native/updater.h>
#include <pvz2native/config.h>
#include <pvz2native/config_screen.h>
#include <pvz2native/host_assets.h>
#include <pvz2native/host_paths.h>
#include <pvz2native/log.h>

#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#else
#include <atomic>
#include <filesystem>
#include <sys/types.h>
#include <unistd.h>
#include "updater_install.h"
#endif

/* In-game updater.
 *
 * At startup a worker thread reads the latest GitHub release (or the feed named by the
 * PVZ2_UPDATE_FEED environment variable: an http(s) URL, a file:// URL or a plain path to a
 * release JSON -- used to test an update without publishing one). When the release is newer
 * than this build and the player has not skipped it:
 *   - Windows: a dialog shows the version and the release notes, with Update / Skip / Not now.
 *     Update writes a PowerShell installer to %TEMP%, starts it and closes the game the same
 *     way the window's X does (saves and the cloud upload run as usual). The installer waits
 *     for the game to exit, downloads the Windows zip with progress, checks its size and
 *     SHA-256 against the release, backs up every file it replaces, merges config.ini and the
 *     presence rules, and puts everything back if any file cannot be written. Whatever
 *     happens, the game is opened again at the end.
 *   - Linux: the same dialog (an SDL message box). Update downloads the right zip (loose binary or
 *     AppImage) in the background while the player keeps playing, checks its SHA-256, and installs
 *     it when the game is closed (updater_install.cpp: backup, merge, rollback), then reopens it. */

namespace {

const char *kApiUrl = "https://api.github.com/repos/Onyxdum/PvZ2NativeWDRPC/releases/latest";
const char *kReleasesPage = "https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest";

/* --- Minimal JSON reader (the release JSON only) ---------------------------------- */

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json *get(const char *key) const {
        if (type != Object) return nullptr;
        for (const auto &kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string get_str(const char *key) const {
        const Json *v = get(key);
        return (v && v->type == String) ? v->str : std::string();
    }
    bool get_bool(const char *key) const {
        const Json *v = get(key);
        return v && v->type == Bool && v->b;
    }
    long long get_int(const char *key) const {
        const Json *v = get(key);
        return (v && v->type == Number) ? (long long)v->num : 0;
    }
};

struct JsonParser {
    const char *p;
    const char *end;
    int depth = 0;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    static void put_utf8(std::string &out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned &out) {
        if (end - p < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string &out) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = 0;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(out, cp);
                break;
            }
            default: out.push_back(e); break;
            }
        }
        return false;
    }

    bool value(Json &v) {
        if (++depth > 64) return false;
        ws();
        if (p >= end) return false;
        bool ok = true;
        if (*p == '{') {
            v.type = Json::Object;
            ++p;
            ws();
            if (p < end && *p == '}') {
                ++p;
            } else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!string(key)) { ok = false; break; }
                    ws();
                    if (p >= end || *p != ':') { ok = false; break; }
                    ++p;
                    v.obj.emplace_back(std::move(key), Json());
                    if (!value(v.obj.back().second)) { ok = false; break; }
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; break; }
                    ok = false;
                    break;
                }
            }
        } else if (*p == '[') {
            v.type = Json::Array;
            ++p;
            ws();
            if (p < end && *p == ']') {
                ++p;
            } else {
                for (;;) {
                    v.arr.emplace_back();
                    if (!value(v.arr.back())) { ok = false; break; }
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; break; }
                    ok = false;
                    break;
                }
            }
        } else if (*p == '"') {
            v.type = Json::String;
            ok = string(v.str);
        } else if (end - p >= 4 && !strncmp(p, "true", 4)) {
            v.type = Json::Bool; v.b = true; p += 4;
        } else if (end - p >= 5 && !strncmp(p, "false", 5)) {
            v.type = Json::Bool; v.b = false; p += 5;
        } else if (end - p >= 4 && !strncmp(p, "null", 4)) {
            v.type = Json::Null; p += 4;
        } else {
            std::string num;
            while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) num.push_back(*p++);
            if (num.empty()) {
                ok = false;
            } else {
                v.type = Json::Number;
                v.num = strtod(num.c_str(), nullptr);
            }
        }
        --depth;
        return ok;
    }
};

bool parse_json(const std::string &text, Json &out) {
    JsonParser jp{text.data(), text.data() + text.size()};
    return jp.value(out);
}

/* --- Versions --------------------------------------------------------------------- */

struct Version {
    int part[4] = {0, 0, 0, 0};
    bool valid = false;
    bool prerelease = false; /* "v1.7.0-beta1" sorts before "v1.7.0" */
};

Version parse_version(const std::string &s) {
    Version v;
    size_t i = s.find_first_of("0123456789");
    if (i == std::string::npos) return v;
    v.valid = true;
    for (int k = 0; k < 4 && i < s.size(); ++k) {
        long n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') n = n * 10 + (s[i++] - '0');
        v.part[k] = (int)n;
        if (i < s.size() && s[i] == '.' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
            ++i;
            continue;
        }
        break;
    }
    v.prerelease = i < s.size() && (s[i] == '-' || s[i] == '+' || isalpha((unsigned char)s[i]));
    return v;
}

bool is_newer_version(const std::string &remote, const std::string &current) {
    Version r = parse_version(remote), c = parse_version(current);
    if (!r.valid) return false;
    for (int k = 0; k < 4; ++k)
        if (r.part[k] != c.part[k]) return r.part[k] > c.part[k];
    return c.prerelease && !r.prerelease;
}

/* --- Small helpers ---------------------------------------------------------------- */

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool ends_with(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool spanish_ui() {
    const pvz2_config_t *cfg = pvz2_config();
    return cfg && (cfg->user_locale[0] == 'e' || cfg->user_locale[0] == 'E') &&
           (cfg->user_locale[1] == 's' || cfg->user_locale[1] == 'S');
}

std::string data_dir() {
    std::string dir;
    if (char *base = pvz2_host_data_path()) {
        dir = base;
        SDL_free(base);
    }
    return dir;
}

std::string skip_file_path() {
    return data_dir() + "userdata/updater_skipped_version.txt";
}

std::string read_skipped_version() {
    std::string out;
    if (FILE *f = fopen(skip_file_path().c_str(), "rb")) {
        char buf[128] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        out.assign(buf, n);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    }
    return out;
}

void write_skipped_version(const std::string &tag) {
    std::string dir = data_dir() + "userdata";
#ifdef _WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    std::string mk = "mkdir -p '" + dir + "'";
    (void)!system(mk.c_str());
#endif
    if (FILE *f = fopen(skip_file_path().c_str(), "wb")) {
        fwrite(tag.data(), 1, tag.size(), f);
        fclose(f);
    }
}

bool read_local_file(std::string path, std::string &out) {
    if (path.rfind("file://", 0) == 0) {
        path = path.substr(7);
#ifdef _WIN32
        if (path.size() > 2 && path[0] == '/' && path[2] == ':') path = path.substr(1);
#endif
    }
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

/* "### What's New in v1.6.0" / "### Novedades en v1.6.0" -> its top-level bullets, without
 * the markdown. Falls back to the first lines of the notes. */
std::string release_highlights(const std::string &body, bool spanish) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = nl + 1;
    }

    auto clean = [](std::string s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '*' || s[i] == '`' || (s[i] == '_' && (i == 0 || s[i - 1] == ' '))) continue;
            out.push_back(s[i]);
        }
        while (!out.empty() && (out.back() == ':' || out.back() == ' ')) out.pop_back();
        if (out.size() > 90) out = out.substr(0, 87) + "...";
        return out;
    };

    const char *heading = spanish ? "### novedades" : "### what's new";
    size_t start = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lower(lines[i]).rfind(heading, 0) == 0) {
            start = i + 1;
            break;
        }
    }
    if (start == lines.size() && spanish) return release_highlights(body, false);

    std::string out;
    int count = 0;
    if (start < lines.size()) {
        for (size_t i = start; i < lines.size() && count < 8; ++i) {
            const std::string &l = lines[i];
            if (l.rfind("#", 0) == 0 || l.rfind("---", 0) == 0) break;
            if (l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0) {
                out += "\xE2\x80\xA2 " + clean(l.substr(2)) + "\n";
                ++count;
            }
        }
    } else {
        for (size_t i = 0; i < lines.size() && count < 6; ++i) {
            if (lines[i].empty() || lines[i].rfind("---", 0) == 0) continue;
            out += clean(lines[i]) + "\n";
            ++count;
        }
    }
    return out;
}

/* --- Release feed ------------------------------------------------------------------ */

struct Asset {
    std::string name, url, sha256;
    long long size = 0;
};

struct Release {
    std::string tag, page, body;
    bool draft = false, prerelease = false;
    std::vector<Asset> assets;
};

#ifdef _WIN32
bool http_get(const std::string &url, std::string &out, DWORD &status) {
    status = 0;
    HINTERNET net = InternetOpenA("PvZ2Native-Updater", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) {
        pvz2_log("[updater] InternetOpenA failed: %lu", GetLastError());
        return false;
    }
    DWORD timeout_ms = 15000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE | INTERNET_FLAG_NO_COOKIES;
    if (url.rfind("https://", 0) == 0) flags |= INTERNET_FLAG_SECURE;
    HINTERNET req = InternetOpenUrlA(net, url.c_str(),
                                     "Accept: application/vnd.github+json\r\n", (DWORD)-1L, flags, 0);
    if (!req) {
        pvz2_log("[updater] no connection to %s (error %lu)", url.c_str(), GetLastError());
        InternetCloseHandle(net);
        return false;
    }
    DWORD len = sizeof(status);
    HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL);

    char buffer[8192];
    DWORD got = 0;
    out.clear();
    while (InternetReadFile(req, buffer, sizeof(buffer), &got) && got > 0) out.append(buffer, got);

    InternetCloseHandle(req);
    InternetCloseHandle(net);
    return true;
}
#else
bool http_get(const std::string &url, std::string &out, unsigned long &status) {
    status = 0;
    for (char c : url)
        if (c == '\'' || c == '\\') return false;
    std::string cmd = "curl -sS -L -m 15 -A PvZ2Native-Updater -H 'Accept: application/vnd.github+json' "
                      "-w '\\n%{http_code}' '" + url + "' 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    size_t nl = out.rfind('\n');
    if (nl == std::string::npos) return false;
    status = strtoul(out.c_str() + nl + 1, nullptr, 10);
    out.resize(nl);
    return status != 0;
}
#endif

bool fetch_release(Release &rel) {
    const char *env = getenv("PVZ2_UPDATE_FEED");
    std::string feed = (env && *env) ? env : kApiUrl;
    std::string text;

    if (feed.rfind("http://", 0) == 0 || feed.rfind("https://", 0) == 0) {
#ifdef _WIN32
        DWORD status = 0;
#else
        unsigned long status = 0;
#endif
        if (!http_get(feed, text, status)) {
            pvz2_log("[updater] could not reach the release feed (offline?)");
            return false;
        }
        if (status == 403 || status == 429) {
            pvz2_log("[updater] GitHub refused the check (HTTP %lu, rate limit?) -- trying again next start",
                     (unsigned long)status);
            return false;
        }
        if (status != 200) {
            pvz2_log("[updater] release feed answered HTTP %lu", (unsigned long)status);
            return false;
        }
    } else if (!read_local_file(feed, text)) {
        pvz2_log("[updater] cannot read the local release feed %s", feed.c_str());
        return false;
    }
    if (env && *env) pvz2_log("[updater] using test feed %s", feed.c_str());

    Json root;
    if (!parse_json(text, root) || root.type != Json::Object) {
        pvz2_log("[updater] release feed is not valid JSON");
        return false;
    }
    rel.tag = root.get_str("tag_name");
    rel.page = root.get_str("html_url");
    rel.body = root.get_str("body");
    rel.draft = root.get_bool("draft");
    rel.prerelease = root.get_bool("prerelease");
    if (const Json *assets = root.get("assets")) {
        for (const Json &a : assets->arr) {
            Asset as;
            as.name = a.get_str("name");
            as.url = a.get_str("browser_download_url");
            as.size = a.get_int("size");
            std::string digest = a.get_str("digest");
            if (lower(digest).rfind("sha256:", 0) == 0) as.sha256 = lower(digest.substr(7));
            if (!as.url.empty()) rel.assets.push_back(as);
        }
    }
    if (rel.page.empty()) rel.page = kReleasesPage;
    return !rel.tag.empty();
}

/* --- Notifications from worker threads (shown on the main thread) --------------------- */

std::mutex g_toast_mutex;
std::string g_pending_toast;

void queue_toast(const std::string &text) {
    std::lock_guard<std::mutex> lock(g_toast_mutex);
    g_pending_toast = text;
}

#ifdef _WIN32

/* --- Windows installer ------------------------------------------------------------- */

const char kUpdaterPs1Script[] = R"ps1(
param(
    [string]$DownloadUrl,
    [string]$GameDir,
    [string]$TargetExe = "pvz2.exe",
    [long]$ExpectedSize = 0,
    [string]$ExpectedSha256 = "",
    [int]$GamePid = 0,
    [string]$NewVersion = ""
)

$ErrorActionPreference = "Stop"
$Host.UI.RawUI.WindowTitle = "Actualizador de PvZ2Native"
$self = $MyInvocation.MyCommand.Path
$work = Join-Path $env:TEMP ("pvz2_update_" + [guid]::NewGuid().ToString("N"))
$backup = Join-Path $GameDir "pvz2_update_backup"
$backedUp = New-Object System.Collections.Generic.List[string]
$created = New-Object System.Collections.Generic.List[string]

function Say([string]$text, [string]$color = "Gray") { Write-Host $text -ForegroundColor $color }

function Remove-Self {
    if ($self -and (Test-Path $self)) {
        Start-Process cmd.exe -ArgumentList "/c ping 127.0.0.1 -n 3 >nul & del `"$self`"" -WindowStyle Hidden
    }
}

function Remove-Work {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}

function Start-Game {
    $exe = Join-Path $GameDir $TargetExe
    if (!(Test-Path $exe)) { $exe = Join-Path $GameDir "pvz2.exe" }
    if (Test-Path $exe) { Start-Process -FilePath $exe -WorkingDirectory $GameDir }
}

function Stop-WithError([string]$message, [string]$state) {
    Say ""
    Say "[ERROR] $message" Red
    Say $state Yellow
    Say "Puedes descargar la version nueva a mano desde la pagina de releases de GitHub." Yellow
    try { Stop-Transcript | Out-Null } catch {}
    Say ""
    Read-Host "Pulsa Enter para volver a abrir el juego" | Out-Null
    Remove-Work
    Start-Game
    Remove-Self
    exit 1
}

function Copy-WithRetry([string]$from, [string]$to) {
    $parent = Split-Path -Parent $to
    if ($parent -and !(Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    for ($attempt = 1; $attempt -le 12; $attempt++) {
        try {
            Copy-Item -LiteralPath $from -Destination $to -Force
            return $true
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    return $false
}

function Restore-Backup {
    $ok = $true
    foreach ($rel in $backedUp) {
        $saved = Join-Path $backup $rel
        $live = Join-Path $GameDir $rel
        # The file that failed (or one never reached) can still be locked but is unchanged.
        $same = $false
        try { $same = (Get-FileHash -LiteralPath $saved).Hash -eq (Get-FileHash -LiteralPath $live).Hash } catch {}
        if ($same) { continue }
        if (!(Copy-WithRetry $saved $live)) {
            Say "  No se pudo restaurar $rel" Red
            $ok = $false
        }
    }
    foreach ($rel in $created) {
        Remove-Item -LiteralPath (Join-Path $GameDir $rel) -Force -ErrorAction SilentlyContinue
    }
    return $ok
}

Say "========================================================" Green
Say "   PvZ2Native - Actualizador automatico $NewVersion" Green
Say "========================================================" Green
Say ""

try { Start-Transcript -Path (Join-Path $GameDir "pvz2_updater.log") -Force | Out-Null } catch {}

# 1. Wait for the game to close by itself (it saves and uploads to the cloud on exit).
if ($GamePid -gt 0) {
    $proc = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
    if ($proc) {
        Say "Esperando a que el juego se cierre (guardando tu partida)..." Yellow
        if (!$proc.WaitForExit(180000)) {
            Say "[ERROR] El juego sigue abierto despues de 3 minutos. Cierralo y vuelve a aceptar la actualizacion al abrirlo." Red
            try { Stop-Transcript | Out-Null } catch {}
            Read-Host "Pulsa Enter para salir" | Out-Null
            Remove-Self
            exit 1
        }
    }
}
Start-Sleep -Milliseconds 700

$unchanged = "No se ha cambiado nada: tu instalacion sigue como estaba."

try {
    $probe = Join-Path $GameDir "pvz2_update_probe.tmp"
    [System.IO.File]::WriteAllText($probe, "x")
    Remove-Item -LiteralPath $probe -Force
} catch {
    Stop-WithError "No se puede escribir en la carpeta del juego ($GameDir). Prueba a mover el juego fuera de 'Archivos de programa' o a ejecutarlo como administrador." $unchanged
}

# 2. Download with progress.
New-Item -ItemType Directory -Path $work -Force | Out-Null
$zipFile = Join-Path $work "update.zip"
Say "Descargando $NewVersion..." Cyan
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $req = [System.Net.WebRequest]::Create($DownloadUrl)
    if ($req -is [System.Net.HttpWebRequest]) {
        $req.UserAgent = "PvZ2Native-Updater"
        $req.Timeout = 30000
        $req.ReadWriteTimeout = 60000
    }
    $resp = $req.GetResponse()
    $total = $resp.ContentLength
    if ($total -le 0) { $total = $ExpectedSize }
    $in = $resp.GetResponseStream()
    $out = [System.IO.File]::Create($zipFile)
    try {
        $buf = New-Object byte[] 262144
        $done = 0L
        $shown = -1
        while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
            $out.Write($buf, 0, $n)
            $done += $n
            if ($total -gt 0) {
                $pct = [int][math]::Floor(($done * 100.0) / $total)
                if ($pct -ne $shown) {
                    $shown = $pct
                    Write-Host -NoNewline ("`r  {0,3}%   {1:N1} / {2:N1} MB   " -f $pct, ($done / 1MB), ($total / 1MB))
                }
            }
        }
        Write-Host ""
    } finally {
        $out.Close()
        $in.Close()
        $resp.Close()
    }
} catch {
    Stop-WithError "No se pudo descargar la actualizacion: $($_.Exception.Message)" $unchanged
}

# 3. Integrity: size and SHA-256 published with the release.
$size = (Get-Item -LiteralPath $zipFile).Length
if ($ExpectedSize -gt 0 -and $size -ne $ExpectedSize) {
    Stop-WithError "La descarga esta incompleta ($size de $ExpectedSize bytes)." $unchanged
}
if ($ExpectedSha256) {
    $hash = (Get-FileHash -LiteralPath $zipFile -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $ExpectedSha256.ToLowerInvariant()) {
        Stop-WithError "La descarga esta danada (el SHA-256 no coincide con el de GitHub)." $unchanged
    }
    Say "  Descarga comprobada (tamano y SHA-256 correctos)." DarkGray
} else {
    Say "  El release no publica SHA-256; solo se comprueba el tamano." DarkGray
}

# 4. Extract to the temp folder.
Say "Descomprimiendo..." Cyan
$tmpDir = Join-Path $work "files"
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zipFile, $tmpDir)
} catch {
    Stop-WithError "El archivo descargado no se puede descomprimir: $($_.Exception.Message)" $unchanged
}

$src = $tmpDir
if (!(Test-Path (Join-Path $src "pvz2.exe"))) {
    $dirs = @(Get-ChildItem -LiteralPath $src -Directory)
    if ($dirs.Count -eq 1 -and (Test-Path (Join-Path $dirs[0].FullName "pvz2.exe"))) {
        $src = $dirs[0].FullName
    } else {
        Stop-WithError "El paquete descargado no contiene pvz2.exe." $unchanged
    }
}

# --- Merges for the player's own files ---

function Merge-PresenceRules([string]$destFile, [string]$srcFile) {
    $userLines = @(Get-Content -LiteralPath $destFile -Encoding UTF8)
    $newLines = @(Get-Content -LiteralPath $srcFile -Encoding UTF8)

    $userPatterns = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($line in $userLines) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) { continue }
        $pattern = $trimmed.Split('|', 2)[0].Trim().ToLowerInvariant()
        if ($pattern.Length -gt 0) { [void]$userPatterns.Add($pattern) }
    }

    $rulesToAdd = New-Object System.Collections.Generic.List[string]
    $pendingComments = New-Object System.Collections.Generic.List[string]
    foreach ($line in $newLines) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0) {
            if ($pendingComments.Count -gt 0) { $pendingComments.Add($line) }
            continue
        }
        if ($trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) {
            $pendingComments.Add($line)
            continue
        }
        $pattern = $trimmed.Split('|', 2)[0].Trim().ToLowerInvariant()
        if ($pattern.Length -gt 0 -and !$userPatterns.Contains($pattern)) {
            foreach ($c in $pendingComments) { $rulesToAdd.Add($c) }
            $pendingComments.Clear()
            $rulesToAdd.Add($line)
            [void]$userPatterns.Add($pattern)
        } else {
            $pendingComments.Clear()
        }
    }

    if ($rulesToAdd.Count -gt 0) {
        $merged = New-Object System.Collections.Generic.List[string]
        foreach ($l in $userLines) { $merged.Add($l) }
        $merged.Add("")
        $merged.Add("; --- [ACTUALIZACION: Reglas nuevas anadidas automaticamente] ---")
        foreach ($r in $rulesToAdd) { $merged.Add($r) }
        [System.IO.File]::WriteAllLines($destFile, $merged, (New-Object System.Text.UTF8Encoding($false)))
        Say "  [Fusion] $($rulesToAdd.Count) regla(s) nueva(s) en $(Split-Path -Leaf $destFile) (tus reglas se conservan)." Green
    } else {
        Say "  [OK] $(Split-Path -Leaf $destFile) al dia (tus reglas se conservan)." DarkGray
    }
}

function Merge-IniConfig([string]$destFile, [string]$srcFile) {
    $userLines = @(Get-Content -LiteralPath $destFile -Encoding UTF8)
    $newLines = @(Get-Content -LiteralPath $srcFile -Encoding UTF8)

    $userSections = @{}
    $currentSec = ""
    foreach ($line in $userLines) {
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith('[') -and $trimmed.EndsWith(']')) {
            $currentSec = $trimmed.Substring(1, $trimmed.Length - 2).Trim().ToLowerInvariant()
            if (!$userSections.ContainsKey($currentSec)) {
                $userSections[$currentSec] = New-Object 'System.Collections.Generic.HashSet[string]'
            }
        } elseif ($currentSec -ne "" -and $trimmed -match '^([^=;#]+)=') {
            [void]$userSections[$currentSec].Add($matches[1].Trim().ToLowerInvariant())
        }
    }

    $additions = New-Object System.Collections.Generic.List[string]
    $curReleaseSec = ""
    $pendingComments = New-Object System.Collections.Generic.List[string]
    $added = 0
    foreach ($line in $newLines) {
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith('[') -and $trimmed.EndsWith(']')) {
            $curReleaseSec = $trimmed.Substring(1, $trimmed.Length - 2).Trim().ToLowerInvariant()
            $pendingComments.Clear()
            continue
        }
        if ($trimmed.Length -eq 0) { continue }
        if ($trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) {
            $pendingComments.Add($line)
            continue
        }
        if ($curReleaseSec -ne "" -and $trimmed -match '^([^=]+)=') {
            $key = $matches[1].Trim().ToLowerInvariant()
            $alreadyHas = $userSections.ContainsKey($curReleaseSec) -and $userSections[$curReleaseSec].Contains($key)
            if (!$alreadyHas) {
                if ($additions.Count -eq 0) {
                    $additions.Add("")
                    $additions.Add("; --- [NUEVOS AJUSTES ANADIDOS TRAS LA ACTUALIZACION] ---")
                }
                $additions.Add("")
                $additions.Add("[$curReleaseSec]")
                foreach ($c in $pendingComments) { $additions.Add($c) }
                $additions.Add($line)
                $added++
                if (!$userSections.ContainsKey($curReleaseSec)) {
                    $userSections[$curReleaseSec] = New-Object 'System.Collections.Generic.HashSet[string]'
                }
                [void]$userSections[$curReleaseSec].Add($key)
            }
            $pendingComments.Clear()
        }
    }

    if ($added -gt 0) {
        $merged = New-Object System.Collections.Generic.List[string]
        foreach ($l in $userLines) { $merged.Add($l) }
        foreach ($a in $additions) { $merged.Add($a) }
        [System.IO.File]::WriteAllLines($destFile, $merged, (New-Object System.Text.UTF8Encoding($false)))
        Say "  [Fusion] $added opcion(es) nueva(s) en config.ini (tus ajustes se conservan)." Green
    } else {
        Say "  [OK] config.ini al dia (tus ajustes se conservan)." DarkGray
    }
}

# 5. What gets installed. Saves, the game data and custom files are never touched.
$plan = New-Object System.Collections.Generic.List[object]
Get-ChildItem -LiteralPath $src -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length).TrimStart('\')
    if ($rel -like 'userdata\*' -or $rel -ieq 'userdata' -or $rel -like '*.obb' -or
        $rel -ieq 'libPVZ2.so' -or $rel -like '*custom*' -or $rel -like 'pvz2_update_backup\*') {
        return
    }
    $plan.Add([pscustomobject]@{ Src = $_.FullName; Rel = $rel })
}
# A copy named pvz2native.exe (older installs / shortcuts) is kept in step with pvz2.exe.
if ((Test-Path (Join-Path $GameDir "pvz2native.exe")) -and !(Test-Path (Join-Path $src "pvz2native.exe"))) {
    $plan.Add([pscustomobject]@{ Src = (Join-Path $src "pvz2.exe"); Rel = "pvz2native.exe" })
}

# 6. Back up every file that will change.
Say "Guardando copia de seguridad de los archivos actuales..." Cyan
if (Test-Path $backup) { Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue }
foreach ($item in $plan) {
    $dest = Join-Path $GameDir $item.Rel
    if (Test-Path -LiteralPath $dest) {
        if (!(Copy-WithRetry $dest (Join-Path $backup $item.Rel))) {
            Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue
            Stop-WithError "No se pudo hacer la copia de seguridad de $($item.Rel)." $unchanged
        }
        $backedUp.Add($item.Rel)
    } else {
        $created.Add($item.Rel)
    }
}

# 7. Install; on any failure put the backup back.
Say "Instalando..." Cyan
$failed = $null
foreach ($item in $plan) {
    $dest = Join-Path $GameDir $item.Rel
    $leaf = Split-Path -Leaf $item.Rel
    try {
        if ($item.Rel -ieq 'config.ini' -and $backedUp.Contains($item.Rel)) {
            Copy-Item -LiteralPath $dest -Destination "$dest.bak" -Force
            Merge-IniConfig $dest $item.Src
        } elseif ($leaf -like 'presence_rules*.txt' -and $backedUp.Contains($item.Rel)) {
            Copy-Item -LiteralPath $dest -Destination "$dest.bak" -Force
            Merge-PresenceRules $dest $item.Src
        } elseif (!(Copy-WithRetry $item.Src $dest)) {
            $failed = "$($item.Rel) (archivo bloqueado o en uso)"
        }
    } catch {
        $failed = "$($item.Rel): $($_.Exception.Message)"
    }
    if ($failed) { break }
}

if ($failed) {
    Say "  Fallo al instalar $failed. Restaurando la version anterior..." Red
    if (Restore-Backup) {
        Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue
        Stop-WithError "No se pudo instalar $failed." "Se ha restaurado la version anterior: el juego sigue como estaba."
    }
    Stop-WithError "No se pudo instalar $failed." "Algunos archivos no se pudieron restaurar; hay una copia completa en $backup (copiala encima de la carpeta del juego)."
}

Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue
Remove-Work

Say ""
Say "========================================================" Green
Say "   Actualizacion a $NewVersion completada. Abriendo el juego..." Green
Say "========================================================" Green
try { Stop-Transcript | Out-Null } catch {}
Start-Sleep -Seconds 2
Start-Game
Remove-Self
)ps1";

std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/* Renames the Yes / No / Cancel buttons of the next message box on this thread. */
HHOOK g_box_hook = nullptr;
const wchar_t *g_box_labels[3];

LRESULT CALLBACK box_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HCBT_ACTIVATE) {
        HWND box = (HWND)wparam;
        SetDlgItemTextW(box, IDYES, g_box_labels[0]);
        SetDlgItemTextW(box, IDNO, g_box_labels[1]);
        SetDlgItemTextW(box, IDCANCEL, g_box_labels[2]);
        UnhookWindowsHookEx(g_box_hook);
        g_box_hook = nullptr;
        return 0;
    }
    return CallNextHookEx(g_box_hook, code, wparam, lparam);
}

enum class Choice { Update, Skip, Later };

Choice ask_player(const Release &rel, bool spanish) {
    std::string notes = release_highlights(rel.body, spanish);
    std::string text;
    if (spanish) {
        text = "Hay una nueva versi\xC3\xB3n de PvZ2Native: " + rel.tag + " (tienes " PVZ2NATIVE_APP_VERSION ").\n\n";
        if (!notes.empty()) text += "Novedades:\n" + notes + "\n";
        text += "Actualizar cierra el juego (guardando tu partida), instala la nueva versi\xC3\xB3n y lo vuelve "
                "a abrir. Tus partidas, config.ini y reglas de presencia se conservan.\n\n"
                "Omitir: no volver a avisar de esta versi\xC3\xB3n.";
        g_box_labels[0] = L"Actualizar";
        g_box_labels[1] = L"Omitir";
        g_box_labels[2] = L"Ahora no";
    } else {
        text = "A new version of PvZ2Native is available: " + rel.tag + " (you have " PVZ2NATIVE_APP_VERSION ").\n\n";
        if (!notes.empty()) text += "What's new:\n" + notes + "\n";
        text += "Update closes the game (your progress is saved), installs the new version and opens it again. "
                "Your saves, config.ini and presence rules are kept.\n\n"
                "Skip: don't remind me about this version.";
        g_box_labels[0] = L"Update";
        g_box_labels[1] = L"Skip";
        g_box_labels[2] = L"Not now";
    }

    g_box_hook = SetWindowsHookExW(WH_CBT, box_hook_proc, nullptr, GetCurrentThreadId());
    int btn = MessageBoxW(NULL, widen(text).c_str(), L"Plants vs. Zombies 2",
                          MB_YESNOCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON1 | MB_TOPMOST | MB_SETFOREGROUND);
    if (g_box_hook) {
        UnhookWindowsHookEx(g_box_hook);
        g_box_hook = nullptr;
    }
    if (btn == IDYES) return Choice::Update;
    if (btn == IDNO) return Choice::Skip;
    return Choice::Later;
}

void open_page(const std::string &url) {
    ShellExecuteW(NULL, L"open", widen(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
}

const Asset *pick_asset(const Release &rel) {
    for (const Asset &a : rel.assets) {
        std::string n = lower(a.name.empty() ? a.url : a.name);
        if (n.find("windows") != std::string::npos && ends_with(n, ".zip")) return &a;
    }
    return nullptr;
}

bool safe_arg(const std::string &s) {
    for (char c : s)
        if (c == '"' || c == '\r' || c == '\n') return false;
    return true;
}

bool launch_installer(const Release &rel, const Asset &asset) {
    wchar_t exe_buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(NULL, exe_buf, (DWORD)(sizeof(exe_buf) / sizeof(exe_buf[0])));
    std::wstring exe(exe_buf);
    size_t sep = exe.find_last_of(L"\\/");
    std::wstring game_dir = sep != std::wstring::npos ? exe.substr(0, sep) : L".";
    std::wstring exe_name = sep != std::wstring::npos ? exe.substr(sep + 1) : L"pvz2.exe";

    wchar_t tmp[MAX_PATH] = {0};
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    std::wstring ps1 = std::wstring(tmp) + L"pvz2_updater_" + std::to_wstring(GetCurrentProcessId()) + L".ps1";
    FILE *f = _wfopen(ps1.c_str(), L"wb");
    if (!f) {
        pvz2_log("[updater] cannot write the installer script to %%TEMP%%");
        return false;
    }
    fwrite(kUpdaterPs1Script, 1, sizeof(kUpdaterPs1Script) - 1, f);
    fclose(f);

    if (!safe_arg(asset.url) || !safe_arg(asset.sha256) || !safe_arg(rel.tag)) return false;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + ps1 +
                       L"\" -DownloadUrl \"" + widen(asset.url) +
                       L"\" -GameDir \"" + game_dir +
                       L"\" -TargetExe \"" + exe_name +
                       L"\" -ExpectedSize " + std::to_wstring(asset.size) +
                       L" -ExpectedSha256 \"" + widen(asset.sha256) +
                       L"\" -GamePid " + std::to_wstring(GetCurrentProcessId()) +
                       L" -NewVersion \"" + widen(rel.tag) + L"\"";

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, game_dir.c_str(), &si, &pi)) {
        pvz2_log("[updater] could not start PowerShell (error %lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    pvz2_log("[updater] installer started (pid=%lu) for %s, %lld bytes, sha256 %s", pi.dwProcessId,
             asset.name.c_str(), asset.size, asset.sha256.empty() ? "(none)" : asset.sha256.c_str());
    return true;
}

/* Closes the game the way the window's X does: during the boot it exits at once (nothing to
 * save yet), afterwards the main loop ends and the usual shutdown (saves, cloud) runs. */
void quit_game_cleanly() {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
    pvz2_request_app_quit();
}

#endif /* _WIN32 */

#ifndef _WIN32

/* --- Linux self-update -------------------------------------------------------------- */

std::mutex g_linux_mutex;
bool g_prompt_pending = false;     /* the worker found a release; the main thread asks */
Release g_prompt_release;
std::atomic<int> g_stage{0};       /* 0 nothing, 1 downloading, 2 ready to install at exit */
std::string g_staged_root, g_staged_tag, g_staged_game, g_staged_work;
bool g_staged_spanish = false;

std::string sh_quote(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    return out + "'";
}

std::string game_dir_no_slash() {
    std::string d = data_dir();
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d.empty() ? std::string(".") : d;
}

const Asset *pick_linux_asset(const Release &rel) {
    const char *ai = getenv("APPIMAGE");
    const bool appimage = ai && *ai;
    for (const Asset &a : rel.assets) {
        std::string n = lower(a.name.empty() ? a.url : a.name);
        if (n.find("linux") == std::string::npos || !ends_with(n, ".zip")) continue;
        if ((n.find("appimage") != std::string::npos) == appimage) return &a;
    }
    return nullptr;
}

std::string sha256_of(const std::string &path) {
    std::string cmd = "sha256sum " + sh_quote(path) + " 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return std::string();
    char buf[160] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    std::string out(buf, n);
    return lower(out.substr(0, out.find_first_of(" \t\n")));
}

/* Result of the last install, shown as a notification on the next start. */
std::string result_file() {
    return data_dir() + "userdata/updater_result.txt";
}

void report_last_install() {
    std::string text;
    if (!read_local_file(result_file(), text)) return;
    remove(result_file().c_str());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (!text.empty()) queue_toast(text);
}

void linux_download(Release rel, Asset asset, bool spanish) {
    namespace fs = std::filesystem;
    const std::string game = game_dir_no_slash();
    const std::string work = game + "/.pvz2_update";
    const std::string zip = work + "/update.zip";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    std::string reason;
    for (char c : asset.url)
        if (c == '\'' || c == '\n') reason = "bad url";
    if (reason.empty()) {
        pvz2_log("[updater] downloading %s", asset.url.c_str());
        std::string cmd = "curl -fsSL --retry 2 --connect-timeout 20 -m 1800 -A PvZ2Native-Updater -o " +
                          sh_quote(zip) + " " + sh_quote(asset.url) + " 2>/dev/null";
        int rc = system(cmd.c_str());
        if (rc != 0) reason = spanish ? "sin conexi\xC3\xB3n o descarga cortada" : "no connection or download cut off";
    }
    if (reason.empty() && asset.size > 0 && (long long)fs::file_size(zip, ec) != asset.size) {
        reason = spanish ? "descarga incompleta" : "incomplete download";
    }
    if (reason.empty() && !asset.sha256.empty()) {
        std::string got = sha256_of(zip);
        if (got.empty()) {
            pvz2_log("[updater] sha256sum not available, only the size was checked");
        } else if (got != asset.sha256) {
            reason = spanish ? "el archivo est\xC3\xA1 da\xC3\xB1" "ado (SHA-256)" : "the file is damaged (SHA-256)";
        }
    }
    std::string root;
    if (reason.empty()) {
        std::string err;
        if (!pvz2_updater::extract_zip(zip, work + "/files", err)) {
            reason = (spanish ? "no se pudo descomprimir: " : "could not unzip: ") + err;
        } else {
            root = pvz2_updater::package_root(work + "/files");
            if (root.empty()) reason = spanish ? "el paquete no contiene el juego" : "the package has no game binary";
        }
    }
    fs::remove(zip, ec);

    if (!reason.empty()) {
        fs::remove_all(work, ec);
        g_stage = 0;
        pvz2_log("[updater] %s download failed: %s", rel.tag.c_str(), reason.c_str());
        queue_toast((spanish ? "No se pudo descargar la actualizaci\xC3\xB3n: " : "Could not download the update: ") + reason);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_linux_mutex);
        g_staged_root = root;
        g_staged_tag = rel.tag;
        g_staged_game = game;
        g_staged_work = work;
        g_staged_spanish = spanish;
    }
    g_stage = 2;
    pvz2_log("[updater] %s ready in %s, installs when the game closes", rel.tag.c_str(), root.c_str());
    queue_toast(spanish ? "PvZ2Native " + rel.tag + " descargado: se instalar\xC3\xA1 al cerrar el juego"
                        : "PvZ2Native " + rel.tag + " is ready: it installs when you close the game");
}

void linux_prompt(const Release &rel) {
    const bool spanish = spanish_ui();
    std::string notes = release_highlights(rel.body, spanish);
    std::string text;
    const char *b_update, *b_skip, *b_later;
    if (spanish) {
        text = "Hay una nueva versi\xC3\xB3n de PvZ2Native: " + rel.tag + " (tienes " PVZ2NATIVE_APP_VERSION ").\n\n";
        if (!notes.empty()) text += "Novedades:\n" + notes + "\n";
        text += "Actualizar la descarga mientras sigues jugando y la instala al cerrar el juego, que se vuelve a "
                "abrir solo. Tus partidas y reglas de presencia se conservan.\n\n"
                "Omitir: no volver a avisar de esta versi\xC3\xB3n.";
        b_update = "Actualizar";
        b_skip = "Omitir";
        b_later = "Ahora no";
    } else {
        text = "A new version of PvZ2Native is available: " + rel.tag + " (you have " PVZ2NATIVE_APP_VERSION ").\n\n";
        if (!notes.empty()) text += "What's new:\n" + notes + "\n";
        text += "Update downloads it while you keep playing and installs it when you close the game, which then "
                "opens again. Your saves and presence rules are kept.\n\n"
                "Skip: don't remind me about this version.";
        b_update = "Update";
        b_skip = "Skip";
        b_later = "Not now";
    }
    const SDL_MessageBoxButtonData buttons[3] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, b_update},
        {0, 2, b_skip},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 3, b_later},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, SDL_GL_GetCurrentWindow(), "Plants vs. Zombies 2",
                                    text.c_str(), 3, buttons, nullptr};
    int id = 3;
    if (SDL_ShowMessageBox(&box, &id) < 0) {
        pvz2_log("[updater] could not show the dialog: %s", SDL_GetError());
        queue_toast(spanish ? "Nueva versi\xC3\xB3n de PvZ2Native: " + rel.tag + ". Desc\xC3\xA1rgala en GitHub."
                            : "PvZ2Native " + rel.tag + " is out. Get it on GitHub.");
        return;
    }
    if (id == 2) {
        write_skipped_version(rel.tag);
        pvz2_log("[updater] %s: skipped, will not ask again for this version", rel.tag.c_str());
        return;
    }
    if (id != 1) {
        pvz2_log("[updater] %s: not now", rel.tag.c_str());
        return;
    }
    const Asset *asset = pick_linux_asset(rel);
    if (!asset) {
        pvz2_log("[updater] %s has no matching Linux zip, opening the release page", rel.tag.c_str());
        SDL_OpenURL(rel.page.c_str());
        return;
    }
    int idle = 0;
    if (!g_stage.compare_exchange_strong(idle, 1)) return;
    queue_toast(spanish ? "Descargando PvZ2Native " + rel.tag + " en segundo plano..."
                        : "Downloading PvZ2Native " + rel.tag + " in the background...");
    std::thread(linux_download, rel, *asset, spanish).detach();
}

void linux_tick() {
    Release rel;
    {
        std::lock_guard<std::mutex> lock(g_linux_mutex);
        if (!g_prompt_pending) return;
        g_prompt_pending = false;
        rel = g_prompt_release;
    }
    linux_prompt(rel);
}

/* After SDL_Quit, at the very end of main: install the staged package and start the game again. */
void linux_install_on_exit() {
    if (g_stage.load() != 2) return;
    namespace fs = std::filesystem;
    std::string root, tag, game, work;
    bool spanish;
    {
        std::lock_guard<std::mutex> lock(g_linux_mutex);
        root = g_staged_root;
        tag = g_staged_tag;
        game = g_staged_game;
        work = g_staged_work;
        spanish = g_staged_spanish;
    }
    const char *ai = getenv("APPIMAGE");
    const std::string appimage = (ai && *ai) ? ai : "";

    std::string log, err;
    const bool ok = pvz2_updater::install_package(root, game, appimage, log, err);
    std::error_code ec;
    fs::remove_all(work, ec);

    if (FILE *f = fopen((game + "/pvz2_updater.log").c_str(), "wb")) {
        fprintf(f, "PvZ2Native updater: %s -> %s\n%s%s\n", PVZ2NATIVE_APP_VERSION, tag.c_str(), log.c_str(),
                ok ? "done" : ("FAILED: " + err).c_str());
        fclose(f);
    }
    fs::create_directories(game + "/userdata", ec);
    if (FILE *f = fopen((game + "/userdata/updater_result.txt").c_str(), "wb")) {
        std::string msg = ok ? (spanish ? "PvZ2Native actualizado a " + tag : "PvZ2Native updated to " + tag)
                             : (spanish ? "No se pudo instalar " + tag + " (" + err + "). Sigues con la versi\xC3\xB3n anterior"
                                        : "Could not install " + tag + " (" + err + "). You still have the previous version");
        fwrite(msg.data(), 1, msg.size(), f);
        fclose(f);
    }

    const std::string exe = !appimage.empty() ? appimage : game + "/pvz2";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (chdir(game.c_str()) != 0) _exit(127);
        execl(exe.c_str(), exe.c_str(), (char *)nullptr);
        _exit(127);
    }
}

#endif /* !_WIN32 */

void check_updates_worker() {
#ifndef _WIN32
    report_last_install();
#endif
    Release rel;
    if (!fetch_release(rel)) return;

    pvz2_log("[updater] installed %s, latest release %s", PVZ2NATIVE_APP_VERSION, rel.tag.c_str());
    if (rel.draft || rel.prerelease) {
        pvz2_log("[updater] %s is a draft or pre-release, ignored", rel.tag.c_str());
        return;
    }
    if (!is_newer_version(rel.tag, PVZ2NATIVE_APP_VERSION)) {
        pvz2_log("[updater] up to date");
        return;
    }
    if (read_skipped_version() == rel.tag) {
        pvz2_log("[updater] %s was skipped by the player", rel.tag.c_str());
        return;
    }

    const bool spanish = spanish_ui();
#ifdef _WIN32
    Choice choice = ask_player(rel, spanish);
    if (choice == Choice::Later) {
        pvz2_log("[updater] %s: not now", rel.tag.c_str());
        return;
    }
    if (choice == Choice::Skip) {
        write_skipped_version(rel.tag);
        pvz2_log("[updater] %s: skipped, will not ask again for this version", rel.tag.c_str());
        return;
    }

    const Asset *asset = pick_asset(rel);
    if (!asset) {
        pvz2_log("[updater] %s has no Windows zip, opening the release page", rel.tag.c_str());
        open_page(rel.page);
        return;
    }
    if (!launch_installer(rel, *asset)) {
        open_page(rel.page);
        return;
    }
    quit_game_cleanly();
#else
    (void)spanish;
    std::lock_guard<std::mutex> lock(g_linux_mutex);
    g_prompt_release = rel;
    g_prompt_pending = true;
#endif
}

}  // namespace

extern "C" void pvz2_check_updates_async(void) {
    const pvz2_config_t *cfg = pvz2_config();
    if (cfg && cfg->check_updates == 0) {
        pvz2_log("[updater] update checks disabled via config");
        return;
    }
    std::thread(check_updates_worker).detach();
}

extern "C" void pvz2_updater_tick(void) {
#ifndef _WIN32
    linux_tick();
#endif
    std::string toast;
    {
        std::lock_guard<std::mutex> lock(g_toast_mutex);
        if (g_pending_toast.empty()) return;
        toast.swap(g_pending_toast);
    }
    config_screen_toast(toast.c_str());
}

extern "C" void pvz2_updater_on_exit(void) {
#ifndef _WIN32
    linux_install_on_exit();
#endif
}

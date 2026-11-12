#include <pvz2native/updater.h>
#include <pvz2native/config.h>
#include <pvz2native/host_paths.h>
#include <pvz2native/log.h>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace {

const char *kApiUrl = "https://api.github.com/repos/Onyxdum/PvZ2NativeWDRPC/releases/latest";

const char kUpdaterPs1Script[] = R"ps1(
param(
    [string]$DownloadUrl,
    [string]$TargetExe,
    [int]$GamePid = 0
)

$Host.UI.RawUI.WindowTitle = "Actualizador de Plants vs. Zombies 2"
Write-Host "========================================================" -ForegroundColor Green
Write-Host "   Plants vs. Zombies 2 - Actualizador Automatico" -ForegroundColor Green
Write-Host "========================================================" -ForegroundColor Green
Write-Host ""
Write-Host "Cerrando el juego y preparando la actualizacion..." -ForegroundColor Yellow

if ($GamePid -gt 0) {
    try {
        $proc = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
        if ($proc) {
            $null = $proc.WaitForExit(10000)
        }
    } catch {}
}

$deadline = (Get-Date).AddSeconds(8)
while ((Get-Date) -lt $deadline) {
    $running = @(Get-Process -Name "pvz2", "pvz2native" -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { break }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 1

$gameDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (!$gameDir) { $gameDir = (Get-Location).Path }
Set-Location $gameDir

Write-Host "Descargando la nueva version desde GitHub..." -ForegroundColor Cyan
Write-Host "URL: $DownloadUrl" -ForegroundColor DarkGray

$zipFile = Join-Path $gameDir "pvz2_update.zip"
$tmpDir = Join-Path $gameDir "pvz2_update_tmp"

if (Test-Path $zipFile) { Remove-Item -Force $zipFile -ErrorAction SilentlyContinue }
if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue }

try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $wc = New-Object System.Net.WebClient
    $wc.Headers.Add("User-Agent", "PvZ2Native-Updater")
    $wc.DownloadFile($DownloadUrl, $zipFile)
} catch {
    Write-Host "[ERROR] No se pudo descargar la actualizacion: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Por favor descarga la actualizacion manualmente desde GitHub." -ForegroundColor Yellow
    Pause
    exit 1
}

Write-Host "Descomprimiendo archivos..." -ForegroundColor Cyan
try {
    Expand-Archive -Path $zipFile -DestinationPath $tmpDir -Force
} catch {
    Write-Host "[ERROR] Error al descomprimir el archivo: $($_.Exception.Message)" -ForegroundColor Red
    Pause
    exit 1
}

# --- Funciones de Fusion Inteligente ---

function Merge-PresenceRules([string]$destFile, [string]$srcFile) {
    if (!(Test-Path $destFile)) {
        Copy-Item $srcFile $destFile -Force
        return
    }

    $bakFile = "$destFile.bak"
    Copy-Item $destFile $bakFile -Force

    $userLines = Get-Content -Path $destFile -Encoding UTF8
    $newLines = Get-Content -Path $srcFile -Encoding UTF8

    $userPatterns = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($line in $userLines) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) {
            continue
        }
        $parts = $trimmed.Split('|', 2)
        $pattern = $parts[0].Trim().ToLowerInvariant()
        if ($pattern.Length -gt 0) {
            [void]$userPatterns.Add($pattern)
        }
    }

    $rulesToAdd = New-Object System.Collections.Generic.List[string]
    $pendingComments = New-Object System.Collections.Generic.List[string]

    foreach ($line in $newLines) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0) {
            if ($pendingComments.Count -gt 0) {
                $pendingComments.Add($line)
            }
            continue
        }
        if ($trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) {
            $pendingComments.Add($line)
            continue
        }

        $parts = $trimmed.Split('|', 2)
        $pattern = $parts[0].Trim().ToLowerInvariant()

        if ($pattern.Length -gt 0 -and !$userPatterns.Contains($pattern)) {
            if ($pendingComments.Count -gt 0) {
                foreach ($c in $pendingComments) {
                    $rulesToAdd.Add($c)
                }
                $pendingComments.Clear()
            }
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

        [System.IO.File]::WriteAllLines($destFile, $merged, [System.Text.Encoding]::UTF8)
        Write-Host "  [Fusion] $($rulesToAdd.Count) regla(s) nueva(s) incorporada(s) en $(Split-Path -Leaf $destFile) (respetando tus ajustes)." -ForegroundColor Green
    } else {
        Write-Host "  [OK] $(Split-Path -Leaf $destFile) al dia (tus reglas personalizadas se conservan)." -ForegroundColor DarkGray
    }
}

function Merge-IniConfig([string]$destFile, [string]$srcFile) {
    if (!(Test-Path $destFile)) {
        Copy-Item $srcFile $destFile -Force
        return
    }

    $bakFile = "$destFile.bak"
    Copy-Item $destFile $bakFile -Force

    $userLines = Get-Content -Path $destFile -Encoding UTF8
    $newLines = Get-Content -Path $srcFile -Encoding UTF8

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
            $key = $matches[1].Trim().ToLowerInvariant()
            [void]$userSections[$currentSec].Add($key)
        }
    }

    $additions = New-Object System.Collections.Generic.List[string]
    $curReleaseSec = ""
    $pendingComments = New-Object System.Collections.Generic.List[string]

    foreach ($line in $newLines) {
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith('[') -and $trimmed.EndsWith(']')) {
            $curReleaseSec = $trimmed.Substring(1, $trimmed.Length - 2).Trim().ToLowerInvariant()
            $pendingComments.Clear()
            continue
        }
        if ($trimmed.Length -eq 0) {
            continue
        }
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

                if (!$userSections.ContainsKey($curReleaseSec)) {
                    $userSections[$curReleaseSec] = New-Object 'System.Collections.Generic.HashSet[string]'
                }
                [void]$userSections[$curReleaseSec].Add($key)
            }
            $pendingComments.Clear()
        }
    }

    if ($additions.Count -gt 0) {
        $merged = New-Object System.Collections.Generic.List[string]
        foreach ($l in $userLines) { $merged.Add($l) }
        foreach ($a in $additions) { $merged.Add($a) }
        [System.IO.File]::WriteAllLines($destFile, $merged, [System.Text.Encoding]::UTF8)
        Write-Host "  [Fusion] $($additions.Count) nueva(s) opcion(es) anadida(s) a config.ini (tus ajustes se conservan)." -ForegroundColor Green
    } else {
        Write-Host "  [OK] config.ini al dia (tus ajustes se conservan)." -ForegroundColor DarkGray
    }
}

function Safe-CopyFile([string]$srcPath, [string]$dstPath) {
    $copied = $false
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        try {
            Copy-Item -Path $srcPath -Destination $dstPath -Force -ErrorAction Stop
            $copied = $true
            break
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    if (!$copied) {
        Write-Host "  [ERROR] No se pudo sobrescribir $(Split-Path -Leaf $dstPath): archivo bloqueado o en uso." -ForegroundColor Red
    }
}

Write-Host "Instalando y fusionando archivos actualizados..." -ForegroundColor Cyan
$src = $tmpDir
$dirs = Get-ChildItem -Path $src -Directory
if ($dirs.Count -eq 1 -and (Test-Path (Join-Path $dirs[0].FullName "pvz2.exe"))) {
    $src = $dirs[0].FullName
}

Get-ChildItem -Path $src -Recurse | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length).TrimStart('\')
    # Proteger partidas y datos principales del juego
    if ($rel -like 'userdata*' -or $rel -like '*.obb' -or $rel -ieq 'libPVZ2.so' -or $rel -like '*custom*') {
        return
    }

    $dest = Join-Path $gameDir $rel
    if ($_.PSIsContainer) {
        if (!(Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }
        return
    }

    # Fusion inteligente para config.ini
    if ($rel -ieq 'config.ini') {
        Merge-IniConfig $dest $_.FullName
        return
    }

    # Fusion inteligente para archivos de reglas de texto
    if ($rel -like 'presence_rules*.txt' -or $rel -like '*.txt') {
        Merge-PresenceRules $dest $_.FullName
        return
    }

    # Resto de archivos (ejecutables, dlls, recursos): copiar de forma segura con reintentos
    Safe-CopyFile $_.FullName $dest
}

if (Test-Path (Join-Path $gameDir "pvz2.exe")) {
    Safe-CopyFile (Join-Path $gameDir "pvz2.exe") (Join-Path $gameDir "pvz2native.exe")
}

Write-Host "Limpiando archivos temporales..." -ForegroundColor DarkGray
if (Test-Path $zipFile) { Remove-Item -Force $zipFile -ErrorAction SilentlyContinue }
if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "========================================================" -ForegroundColor Green
Write-Host "   Actualizacion completada con exito!" -ForegroundColor Green
Write-Host "   Reiniciando el juego..." -ForegroundColor Green
Write-Host "========================================================" -ForegroundColor Green
Start-Sleep -Seconds 1

if ($TargetExe -and (Test-Path (Join-Path $gameDir $TargetExe))) {
    Start-Process (Join-Path $gameDir $TargetExe)
} elseif (Test-Path (Join-Path $gameDir "pvz2.exe")) {
    Start-Process (Join-Path $gameDir "pvz2.exe")
} elseif (Test-Path (Join-Path $gameDir "pvz2native.exe")) {
    Start-Process (Join-Path $gameDir "pvz2native.exe")
}

$self = $MyInvocation.MyCommand.Path
if ($self -and (Test-Path $self)) {
    Start-Process cmd.exe -ArgumentList "/c ping 127.0.0.1 -n 2 >nul & del `"$self`"" -WindowStyle Hidden
}
)ps1";

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

SemVer parse_semver(const std::string &ver) {
    SemVer v;
    size_t start = ver.find_first_of("0123456789");
    if (start != std::string::npos) {
        sscanf(ver.c_str() + start, "%d.%d.%d", &v.major, &v.minor, &v.patch);
    }
    return v;
}

bool is_newer_version(const std::string &remote, const std::string &current) {
    SemVer r = parse_semver(remote);
    SemVer c = parse_semver(current);
    if (r.major != c.major) return r.major > c.major;
    if (r.minor != c.minor) return r.minor > c.minor;
    return r.patch > c.patch;
}

std::string extract_json_field(const std::string &json, const std::string &key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return "";
    size_t start_quote = json.find('"', colon + 1);
    if (start_quote == std::string::npos) return "";
    size_t end_quote = json.find('"', start_quote + 1);
    while (end_quote != std::string::npos && json[end_quote - 1] == '\\') {
        end_quote = json.find('"', end_quote + 1);
    }
    if (end_quote == std::string::npos) return "";
    return json.substr(start_quote + 1, end_quote - start_quote - 1);
}

std::string extract_windows_download_url(const std::string &json) {
    size_t pos = 0;
    std::string fallback_zip_url;
    while ((pos = json.find("\"browser_download_url\"", pos)) != std::string::npos) {
        size_t colon = json.find(':', pos + 22);
        if (colon == std::string::npos) break;
        size_t q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string url = json.substr(q1 + 1, q2 - q1 - 1);
        std::string lower = url;
        for (char &c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find("windows") != std::string::npos && lower.find(".zip") != std::string::npos) {
            return url;
        }
        if (lower.find(".zip") != std::string::npos && fallback_zip_url.empty()) {
            fallback_zip_url = url;
        }
        pos = q2 + 1;
    }
    return fallback_zip_url;
}

bool fetch_latest_release(std::string &out_json) {
    HINTERNET hInternet = InternetOpenA("PvZ2Native-Updater", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        pvz2_log("[updater] InternetOpenA failed: %lu", GetLastError());
        return false;
    }

    DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE;
    HINTERNET hUrl = InternetOpenUrlA(hInternet, kApiUrl,
                                      "User-Agent: PvZ2Native-Updater\r\nAccept: application/vnd.github.v3+json\r\n",
                                      -1L, flags, 0);
    if (!hUrl) {
        pvz2_log("[updater] InternetOpenUrlA failed: %lu", GetLastError());
        InternetCloseHandle(hInternet);
        return false;
    }

    char buffer[4096];
    DWORD bytes_read = 0;
    out_json.clear();
    while (InternetReadFile(hUrl, buffer, sizeof(buffer) - 1, &bytes_read) && bytes_read > 0) {
        buffer[bytes_read] = '\0';
        out_json.append(buffer, bytes_read);
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return !out_json.empty();
}

void check_updates_worker() {
    pvz2_log("[updater] checking for updates from GitHub...");
    std::string json;
    if (!fetch_latest_release(json)) {
        pvz2_log("[updater] unable to reach GitHub release API");
        return;
    }

    std::string tag_name = extract_json_field(json, "tag_name");
    std::string html_url = extract_json_field(json, "html_url");
    if (tag_name.empty()) {
        pvz2_log("[updater] unable to parse tag_name from release JSON");
        return;
    }

    pvz2_log("[updater] current version: %s, latest release: %s", PVZ2NATIVE_APP_VERSION, tag_name.c_str());

    if (!is_newer_version(tag_name, PVZ2NATIVE_APP_VERSION)) {
        pvz2_log("[updater] game is already up to date (%s)", PVZ2NATIVE_APP_VERSION);
        return;
    }

    std::string download_url = extract_windows_download_url(json);
    pvz2_log("[updater] new version found: %s, download: %s", tag_name.c_str(), download_url.c_str());

    /* Show modal prompt to player without encoding glitches */
    int btn = MessageBoxW(
        NULL,
        L"Nueva versi\u00f3n disponible.\n\n\u00bfDeseas actualizar el juego ahora autom\u00e1ticamente?\n(Solo tomar\u00e1 un minuto)",
        L"Plants vs. Zombies 2",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND
    );

    if (btn != IDYES) {
        pvz2_log("[updater] user skipped update for %s", tag_name.c_str());
        return;
    }

    pvz2_log("[updater] user accepted update. Preparing updater...");

    /* 1. Resolve current executable path and installation directory */
    char exe_path_buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path_buf, MAX_PATH);
    std::string current_exe(exe_path_buf);
    size_t sep_pos = current_exe.find_last_of("\\/");
    std::string game_dir = (sep_pos != std::string::npos) ? current_exe.substr(0, sep_pos) : ".";
    std::string exe_filename = (sep_pos != std::string::npos) ? current_exe.substr(sep_pos + 1) : "pvz2.exe";

    /* If no direct zip URL was found, open release page for manual download */
    if (download_url.empty()) {
        pvz2_log("[updater] direct zip download url not found, opening release page in browser");
        if (!html_url.empty()) {
            ShellExecuteA(NULL, "open", html_url.c_str(), NULL, NULL, SW_SHOWNORMAL);
        }
        return;
    }

    /* 2. Write updater.ps1 */
    std::string ps1_path = game_dir + "\\updater.ps1";
    {
        std::ofstream ps1(ps1_path, std::ios::binary);
        if (ps1) {
            ps1.write(kUpdaterPs1Script, sizeof(kUpdaterPs1Script) - 1);
        }
    }

    /* 3. Launch powershell.exe directly with its own console window */
    DWORD game_pid = GetCurrentProcessId();
    std::string cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + ps1_path +
                      "\" -DownloadUrl \"" + download_url +
                      "\" -TargetExe \"" + exe_filename +
                      "\" -GamePid " + std::to_string(game_pid);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};

    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    if (CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, game_dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        pvz2_log("[updater] updater process launched successfully (pid=%lu), exiting game", pi.dwProcessId);
    } else {
        pvz2_log("[updater] CreateProcessA failed: %lu, falling back to release page in browser", GetLastError());
        if (!html_url.empty()) {
            ShellExecuteA(NULL, "open", html_url.c_str(), NULL, NULL, SW_SHOWNORMAL);
        }
        return;
    }

    /* 4. Exit game process so the binary and files can be cleanly replaced */
    exit(0);
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

#else

extern "C" void pvz2_check_updates_async(void) {
    /* No-op on non-Windows */
}

#endif  // _WIN32

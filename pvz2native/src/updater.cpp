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

const char kUpdaterPs1Script[] =
    "param(\r\n"
    "    [string]$DownloadUrl,\r\n"
    "    [string]$TargetExe\r\n"
    ")\r\n"
    "\r\n"
    "$Host.UI.RawUI.WindowTitle = \"Actualizador de Plants vs. Zombies 2\"\r\n"
    "Write-Host \"========================================================\" -ForegroundColor Green\r\n"
    "Write-Host \"   Plants vs. Zombies 2 - Actualizador Automatico\" -ForegroundColor Green\r\n"
    "Write-Host \"========================================================\" -ForegroundColor Green\r\n"
    "Write-Host \"\"\r\n"
    "Write-Host \"Cerrando el juego y preparando la actualizacion...\" -ForegroundColor Yellow\r\n"
    "\r\n"
    "while ((Get-Process -Name \"pvz2\", \"pvz2native\" -ErrorAction SilentlyContinue).Count -gt 0) {\r\n"
    "    Start-Sleep -Seconds 1\r\n"
    "}\r\n"
    "Start-Sleep -Seconds 1\r\n"
    "\r\n"
    "$gameDir = Split-Path -Parent $MyInvocation.MyCommand.Path\r\n"
    "Set-Location $gameDir\r\n"
    "\r\n"
    "Write-Host \"Descargando la nueva version desde GitHub...\" -ForegroundColor Cyan\r\n"
    "Write-Host \"URL: $DownloadUrl\" -ForegroundColor DarkGray\r\n"
    "\r\n"
    "$zipFile = Join-Path $gameDir \"pvz2_update.zip\"\r\n"
    "$tmpDir = Join-Path $gameDir \"pvz2_update_tmp\"\r\n"
    "\r\n"
    "if (Test-Path $zipFile) { Remove-Item -Force $zipFile -ErrorAction SilentlyContinue }\r\n"
    "if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue }\r\n"
    "\r\n"
    "try {\r\n"
    "    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12\r\n"
    "    $wc = New-Object System.Net.WebClient\r\n"
    "    $wc.Headers.Add(\"User-Agent\", \"PvZ2Native-Updater\")\r\n"
    "    $wc.DownloadFile($DownloadUrl, $zipFile)\r\n"
    "} catch {\r\n"
    "    Write-Host \"[ERROR] No se pudo descargar la actualizacion: $($_.Exception.Message)\" -ForegroundColor Red\r\n"
    "    Write-Host \"Por favor descarga la actualizacion manualmente desde GitHub.\" -ForegroundColor Yellow\r\n"
    "    Pause\r\n"
    "    exit 1\r\n"
    "}\r\n"
    "\r\n"
    "Write-Host \"Descomprimiendo archivos...\" -ForegroundColor Cyan\r\n"
    "try {\r\n"
    "    Expand-Archive -Path $zipFile -DestinationPath $tmpDir -Force\r\n"
    "} catch {\r\n"
    "    Write-Host \"[ERROR] Error al descomprimir el archivo: $($_.Exception.Message)\" -ForegroundColor Red\r\n"
    "    Pause\r\n"
    "    exit 1\r\n"
    "}\r\n"
    "\r\n"
    "Write-Host \"Instalando archivos actualizados...\" -ForegroundColor Cyan\r\n"
    "$src = $tmpDir\r\n"
    "$dirs = Get-ChildItem -Path $src -Directory\r\n"
    "if ($dirs.Count -eq 1 -and (Test-Path (Join-Path $dirs[0].FullName \"pvz2.exe\"))) {\r\n"
    "    $src = $dirs[0].FullName\r\n"
    "}\r\n"
    "\r\n"
    "Get-ChildItem -Path $src -Recurse | ForEach-Object {\r\n"
    "    $rel = $_.FullName.Substring($src.Length).TrimStart('\\')\r\n"
    "    if ($rel -like 'userdata*' -or $rel -ieq 'config.ini' -or $rel -like '*.obb' -or $rel -ieq 'libPVZ2.so') {\r\n"
    "        return\r\n"
    "    }\r\n"
    "    $dest = Join-Path $gameDir $rel\r\n"
    "    if ($_.PSIsContainer) {\r\n"
    "        if (!(Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }\r\n"
    "    } else {\r\n"
    "        Copy-Item $_.FullName -Destination $dest -Force\r\n"
    "    }\r\n"
    "}\r\n"
    "\r\n"
    "if (Test-Path (Join-Path $gameDir \"pvz2.exe\")) {\r\n"
    "    Copy-Item (Join-Path $gameDir \"pvz2.exe\") -Destination (Join-Path $gameDir \"pvz2native.exe\") -Force\r\n"
    "}\r\n"
    "\r\n"
    "Write-Host \"Limpiando archivos temporales...\" -ForegroundColor DarkGray\r\n"
    "if (Test-Path $zipFile) { Remove-Item -Force $zipFile -ErrorAction SilentlyContinue }\r\n"
    "if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue }\r\n"
    "\r\n"
    "Write-Host \"\"\r\n"
    "Write-Host \"========================================================\" -ForegroundColor Green\r\n"
    "Write-Host \"   Actualizacion completada con exito!\" -ForegroundColor Green\r\n"
    "Write-Host \"   Reiniciando el juego...\" -ForegroundColor Green\r\n"
    "Write-Host \"========================================================\" -ForegroundColor Green\r\n"
    "Start-Sleep -Seconds 2\r\n"
    "\r\n"
    "if ($TargetExe -and (Test-Path (Join-Path $gameDir $TargetExe))) {\r\n"
    "    Start-Process (Join-Path $gameDir $TargetExe)\r\n"
    "} elseif (Test-Path (Join-Path $gameDir \"pvz2.exe\")) {\r\n"
    "    Start-Process (Join-Path $gameDir \"pvz2.exe\")\r\n"
    "} elseif (Test-Path (Join-Path $gameDir \"pvz2native.exe\")) {\r\n"
    "    Start-Process (Join-Path $gameDir \"pvz2native.exe\")\r\n"
    "}\r\n"
    "\r\n"
    "$selfPath = $MyInvocation.MyCommand.Path\r\n"
    "Start-Process powershell -ArgumentList \"-NoProfile -Command Start-Sleep -Seconds 1; Remove-Item -Force '$selfPath'\" -WindowStyle Hidden\r\n";

const char kUpdaterBatScript[] =
    "@echo off\r\n"
    "setlocal\r\n"
    "cd /d \"%~dp0\"\r\n"
    "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%~dp0updater.ps1\" -DownloadUrl \"%~1\" -TargetExe \"%~2\"\r\n"
    "(goto) 2>nul & del \"%~f0\"\r\n";

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

    /* Show modal prompt to player:
     * "Nueva version disponible ¿Deseas actualizar el Juego? Solo tomara un minuto" */
    int btn = MessageBoxW(
        NULL,
        L"Nueva version disponible \x00bfDeseas actualizar el Juego? Solo tomara un minuto",
        L"Plants vs. Zombies 2",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND
    );

    if (btn != IDYES) {
        pvz2_log("[updater] user skipped update for %s", tag_name.c_str());
        return;
    }

    pvz2_log("[updater] user accepted update. Opening release page and starting updater...");

    /* 1. Open the GitHub release page in the default web browser */
    if (!html_url.empty()) {
        ShellExecuteA(NULL, "open", html_url.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }

    /* 2. Resolve current executable path and installation directory */
    char exe_path_buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path_buf, MAX_PATH);
    std::string current_exe(exe_path_buf);
    size_t sep_pos = current_exe.find_last_of("\\/");
    std::string game_dir = (sep_pos != std::string::npos) ? current_exe.substr(0, sep_pos) : ".";
    std::string exe_filename = (sep_pos != std::string::npos) ? current_exe.substr(sep_pos + 1) : "pvz2.exe";

    /* If no direct zip URL was found, user has the release page opened in browser */
    if (download_url.empty()) {
        pvz2_log("[updater] direct zip download url not found, release page opened");
        return;
    }

    /* 3. Write updater.ps1 and updater.bat */
    std::string ps1_path = game_dir + "\\updater.ps1";
    std::string bat_path = game_dir + "\\updater.bat";

    {
        std::ofstream ps1(ps1_path, std::ios::binary);
        if (ps1) {
            ps1.write(kUpdaterPs1Script, sizeof(kUpdaterPs1Script) - 1);
        }
    }
    {
        std::ofstream bat(bat_path, std::ios::binary);
        if (bat) {
            bat.write(kUpdaterBatScript, sizeof(kUpdaterBatScript) - 1);
        }
    }

    /* 4. Launch updater.bat detached */
    std::string args = "\"" + download_url + "\" \"" + exe_filename + "\"";
    ShellExecuteA(NULL, "open", bat_path.c_str(), args.c_str(), game_dir.c_str(), SW_SHOWNORMAL);

    /* 5. Exit game process so the binary can be updated */
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

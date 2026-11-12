#include "pvz2native/steam_shortcut.h"
#include "pvz2native/config.h"
#include "pvz2native/host_paths.h"
#include "pvz2native/host_assets.h"
#include "pvz2native/log.h"
#include <SDL.h>

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

struct SteamStrings {
    const wchar_t *title;
    const wchar_t *prompt;
    const wchar_t *notice_running;
    const wchar_t *notice_success;
    const wchar_t *notice_already;
    const wchar_t *notice_no_steam;

    const char *status_not_found;
    const char *status_no_profiles;
    const char *status_restart_steam;
    const char *status_success;
    const char *status_already;
};

const SteamStrings kStringsES = {
    L"Plants vs. Zombies 2 - Steam",
    L"¿Deseas añadir Plants vs. Zombies 2 a tu biblioteca de Steam como juego externo?\n\n"
    L"Esto te permitirá iniciarlo desde Steam, usar mandos con Steam Input, "
    L"el Steam Overlay y capturas de pantalla.",
    L"¡Acceso directo añadido con éxito!\n\n"
    L"Nota: Como Steam está abierto, debes reiniciar Steam (cerrarlo y volver a abrirlo) "
    L"para que aparezca en tu biblioteca.",
    L"¡Plants vs. Zombies 2 ha sido añadido a tu biblioteca de Steam con éxito!",
    L"Plants vs. Zombies 2 ya está añadido en tu biblioteca de Steam.",
    L"No se encontró una instalación de Steam en este equipo.",

    "Steam no detectado en este sistema",
    "No se encontraron perfiles de Steam",
    "Añadido a Steam. Reinicia Steam para verlo.",
    "Añadido a tu biblioteca de Steam con éxito.",
    "Plants vs. Zombies 2 ya está en tu biblioteca de Steam."
};

const SteamStrings kStringsEN = {
    L"Plants vs. Zombies 2 - Steam",
    L"Would you like to add Plants vs. Zombies 2 to your Steam library as a non-Steam game?\n\n"
    L"This enables Steam Input controller support, the Steam Overlay, and screenshots.",
    L"Shortcut successfully added!\n\n"
    L"Note: Because Steam is currently running, please restart Steam (close and re-open it) "
    L"for the game to appear in your library.",
    L"Plants vs. Zombies 2 has been successfully added to your Steam library!",
    L"Plants vs. Zombies 2 is already in your Steam library.",
    L"Steam installation was not found on this computer.",

    "Steam was not found on this system",
    "No Steam user profiles found",
    "Added to Steam. Restart Steam to see it.",
    "Successfully added to your Steam library.",
    "Plants vs. Zombies 2 is already in your Steam library."
};

const SteamStrings kStringsFR = {
    L"Plants vs. Zombies 2 - Steam",
    L"Souhaitez-vous ajouter Plants vs. Zombies 2 à votre bibliothèque Steam en tant que jeu non-Steam ?\n\n"
    L"Cela permet la prise en charge des manettes Steam Input, l'overlay Steam et les captures d'écran.",
    L"Raccourci ajouté avec succès !\n\n"
    L"Remarque : Comme Steam est ouvert, veuillez redémarrer Steam pour qu'il apparaisse dans votre bibliothèque.",
    L"Plants vs. Zombies 2 a été ajouté avec succès à votre bibliothèque Steam !",
    L"Plants vs. Zombies 2 est déjà présent dans votre bibliothèque Steam.",
    L"Aucune installation de Steam n'a été trouvée sur cet ordinateur.",

    "Steam non détecté sur ce système",
    "Aucun profil Steam trouvé",
    "Ajouté à Steam. Redémarrez Steam pour le voir.",
    "Ajouté avec succès à votre bibliothèque Steam.",
    "Plants vs. Zombies 2 est déjà dans votre bibliothèque Steam."
};

const SteamStrings kStringsDE = {
    L"Plants vs. Zombies 2 - Steam",
    L"Möchtest du Plants vs. Zombies 2 als factionsfremdes Spiel zu deiner Steam-Bibliothek hinzufügen?\n\n"
    L"Dies aktiviert Steam Input Controller-Unterstützung, das Steam-Overlay und Screenshots.",
    L"Verknüpfung erfolgreich hinzugefügt!\n\n"
    L"Hinweis: Da Steam gerade läuft, starte Steam bitte neu, damit das Spiel in deiner Bibliothek erscheint.",
    L"Plants vs. Zombies 2 wurde erfolgreich zu deiner Steam-Bibliothek hinzugefügt!",
    L"Plants vs. Zombies 2 ist bereits in deiner Steam-Bibliothek vorhanden.",
    L"Steam-Installation wurde auf diesem Computer nicht gefunden.",

    "Steam wurde auf diesem System nicht gefunden",
    "Keine Steam-Profile gefunden",
    "Zu Steam hinzugefügt. Starte Steam neu, um es zu sehen.",
    "Erfolgreich zu deiner Steam-Bibliothek hinzugefügt.",
    "Plants vs. Zombies 2 ist bereits in deiner Steam-Bibliothek."
};

const SteamStrings kStringsPT = {
    L"Plants vs. Zombies 2 - Steam",
    L"Deseja adicionar Plants vs. Zombies 2 à sua biblioteca Steam como jogo externo?\n\n"
    L"Isso permitirá suporte a controles com Steam Input, Steam Overlay e capturas de tela.",
    L"Atalho adicionado com sucesso!\n\n"
    L"Nota: Como a Steam está aberta, reinicie a Steam para que ele apareça na sua biblioteca.",
    L"Plants vs. Zombies 2 foi adicionado à sua biblioteca Steam com sucesso!",
    L"Plants vs. Zombies 2 já está na sua biblioteca Steam.",
    L"A instalação da Steam não foi encontrada neste computador.",

    "Steam não detectada neste sistema",
    "Nenhum perfil da Steam encontrado",
    "Adicionado à Steam. Reinicie a Steam para ver.",
    "Adicionado à sua biblioteca Steam com sucesso.",
    "Plants vs. Zombies 2 já está na sua biblioteca Steam."
};

const SteamStrings kStringsIT = {
    L"Plants vs. Zombies 2 - Steam",
    L"Vuoi aggiungere Plants vs. Zombies 2 alla tua libreria di Steam come gioco non-Steam?\n\n"
    L"Questo attiverà il supporto per controller Steam Input, l'Overlay di Steam e gli screenshot.",
    L"Scorciatoia aggiunta con successo!\n\n"
    L"Nota: Poiché Steam è aperto, riavvia Steam per visualizzarlo nella tua libreria.",
    L"Plants vs. Zombies 2 è stato aggiunto con successo alla tua libreria di Steam!",
    L"Plants vs. Zombies 2 è già presente nella tua libreria di Steam.",
    L"Installazione di Steam non trovata su questo computer.",

    "Steam non rilevato su questo sistema",
    "Nessun profilo Steam trovato",
    "Aggiunto a Steam. Riavvia Steam per vederlo.",
    "Aggiunto con successo alla tua libreria di Steam.",
    "Plants vs. Zombies 2 è già presente nella tua libreria di Steam."
};

const SteamStrings &get_strings_for_locale(const char *loc) {
    if (!loc || !loc[0]) return kStringsES;
    if (std::strncmp(loc, "es", 2) == 0) return kStringsES;
    if (std::strncmp(loc, "fr", 2) == 0) return kStringsFR;
    if (std::strncmp(loc, "de", 2) == 0) return kStringsDE;
    if (std::strncmp(loc, "pt", 2) == 0) return kStringsPT;
    if (std::strncmp(loc, "it", 2) == 0) return kStringsIT;
    if (std::strncmp(loc, "en", 2) == 0) return kStringsEN;
    return kStringsEN;
}

std::string trim_str(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string get_config_path() {
    if (char *base = pvz2_host_data_path()) {
        std::string p = std::string(base) + "config.ini";
        SDL_free(base);
        return p;
    }
    return "config.ini";
}

bool is_steam_asked_in_config() {
    std::ifstream f(get_config_path());
    if (!f) return false;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim_str(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            for (char &c : section) c = (char)tolower(c);
            continue;
        }
        if (section == "steam") {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = trim_str(line.substr(0, eq));
                std::string v = trim_str(line.substr(eq + 1));
                for (char &c : k) c = (char)tolower(c);
                if (k == "asked_shortcut") {
                    return (v == "1" || v == "true" || v == "yes");
                }
            }
        }
    }
    return false;
}

void set_steam_asked_in_config() {
    std::string path = get_config_path();
    std::vector<std::string> lines;
    bool in_steam = false;
    bool found_key = false;
    bool found_section = false;

    {
        std::ifstream f(path);
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                std::string trimmed = trim_str(line);
                if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
                    std::string sec = trimmed.substr(1, trimmed.size() - 2);
                    for (char &c : sec) c = (char)tolower(c);
                    if (in_steam && !found_key) {
                        lines.push_back("asked_shortcut = 1");
                        found_key = true;
                    }
                    in_steam = (sec == "steam");
                    if (in_steam) found_section = true;
                } else if (in_steam) {
                    auto eq = trimmed.find('=');
                    if (eq != std::string::npos) {
                        std::string k = trim_str(trimmed.substr(0, eq));
                        for (char &c : k) c = (char)tolower(c);
                        if (k == "asked_shortcut") {
                            line = "asked_shortcut = 1";
                            found_key = true;
                        }
                    }
                }
                lines.push_back(line);
            }
            if (in_steam && !found_key) {
                lines.push_back("asked_shortcut = 1");
                found_key = true;
            }
        }
    }

    if (!found_section) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back("[steam]");
        lines.push_back("asked_shortcut = 1");
    }

    std::ofstream out(path, std::ios::trunc);
    for (const auto &l : lines) {
        out << l << "\n";
    }
}

// ---------------------------------------------------------------------------
// Steam VDF Parsing & Writing
// ---------------------------------------------------------------------------

struct VdfField {
    uint8_t type; // 1 = string, 2 = int32, 0 = submap (tags)
    std::string key;
    std::string str_val;
    int32_t int_val = 0;
    std::vector<std::string> submap_strings;
};

struct VdfShortcut {
    std::vector<VdfField> fields;

    std::string get_string(const std::string &k) const {
        for (const auto &f : fields) {
            if (f.type == 1 && f.key == k) return f.str_val;
        }
        return "";
    }

    int32_t get_int(const std::string &k, int32_t def = 0) const {
        for (const auto &f : fields) {
            if (f.type == 2 && f.key == k) return f.int_val;
        }
        return def;
    }

    void set_string(const std::string &k, const std::string &v) {
        for (auto &f : fields) {
            if (f.type == 1 && f.key == k) { f.str_val = v; return; }
        }
        VdfField nf;
        nf.type = 1; nf.key = k; nf.str_val = v;
        fields.push_back(nf);
    }

    void set_int(const std::string &k, int32_t v) {
        for (auto &f : fields) {
            if (f.type == 2 && f.key == k) { f.int_val = v; return; }
        }
        VdfField nf;
        nf.type = 2; nf.key = k; nf.int_val = v;
        fields.push_back(nf);
    }
};

bool read_null_str(const std::vector<uint8_t> &data, size_t &pos, std::string &out) {
    size_t start = pos;
    while (pos < data.size() && data[pos] != 0) {
        pos++;
    }
    if (pos >= data.size()) return false;
    out.assign(reinterpret_cast<const char*>(&data[start]), pos - start);
    pos++; // skip null
    return true;
}

void write_null_str(std::vector<uint8_t> &out, const std::string &s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

bool parse_vdf_data(const std::vector<uint8_t> &data, std::vector<VdfShortcut> &out) {
    out.clear();
    if (data.empty()) return true;
    size_t pos = 0;
    if (data[pos++] != 0) return false;
    std::string root_name;
    if (!read_null_str(data, pos, root_name)) return false;
    if (root_name != "shortcuts") return false;

    while (pos < data.size() && data[pos] == 0) {
        pos++; // skip 0
        std::string idx_str;
        if (!read_null_str(data, pos, idx_str)) break;
        VdfShortcut sc;
        while (pos < data.size()) {
            uint8_t t = data[pos++];
            if (t == 8) break; // end of entry
            std::string k;
            if (!read_null_str(data, pos, k)) break;
            VdfField fld;
            fld.type = t;
            fld.key = k;
            if (t == 1) { // string
                read_null_str(data, pos, fld.str_val);
            } else if (t == 2) { // int32
                if (pos + 4 <= data.size()) {
                    std::memcpy(&fld.int_val, &data[pos], 4);
                    pos += 4;
                }
            } else if (t == 0) { // submap (tags)
                while (pos < data.size()) {
                    uint8_t sub_t = data[pos++];
                    if (sub_t == 8) break;
                    std::string sub_k;
                    read_null_str(data, pos, sub_k);
                    if (sub_t == 1) {
                        std::string sub_v;
                        read_null_str(data, pos, sub_v);
                        fld.submap_strings.push_back(sub_v);
                    } else if (sub_t == 2) {
                        pos += 4;
                    }
                }
            }
            sc.fields.push_back(fld);
        }
        out.push_back(sc);
    }
    return true;
}

std::vector<uint8_t> serialize_vdf_data(const std::vector<VdfShortcut> &scs) {
    std::vector<uint8_t> out;
    out.push_back(0);
    write_null_str(out, "shortcuts");
    for (size_t i = 0; i < scs.size(); ++i) {
        out.push_back(0);
        write_null_str(out, std::to_string(i));
        for (const auto &f : scs[i].fields) {
            out.push_back(f.type);
            write_null_str(out, f.key);
            if (f.type == 1) {
                write_null_str(out, f.str_val);
            } else if (f.type == 2) {
                uint8_t bytes[4];
                std::memcpy(bytes, &f.int_val, 4);
                out.insert(out.end(), bytes, bytes + 4);
            } else if (f.type == 0) {
                for (size_t ti = 0; ti < f.submap_strings.size(); ++ti) {
                    out.push_back(1);
                    write_null_str(out, std::to_string(ti));
                    write_null_str(out, f.submap_strings[ti]);
                }
                out.push_back(8);
            }
        }
        out.push_back(8); // end of shortcut
    }
    out.push_back(8); // end of shortcuts
    out.push_back(8); // end of root
    return out;
}

int32_t calculate_shortcut_appid(const std::string &exe, const std::string &appname) {
    std::string key = "\"" + exe + "\"" + appname;
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(key.data()), key.size());
    uint32_t appid = static_cast<uint32_t>(crc) | 0x80000000u;
    return static_cast<int32_t>(appid);
}

// ---------------------------------------------------------------------------
// Steam Environment & Paths
// ---------------------------------------------------------------------------

#ifdef _WIN32
std::string get_steam_install_dir() {
    HKEY hKey;
    char path[MAX_PATH] = {0};
    DWORD path_len = sizeof(path);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "SteamPath", NULL, NULL, (LPBYTE)path, &path_len) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(path);
        }
        RegCloseKey(hKey);
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        path_len = sizeof(path);
        if (RegQueryValueExA(hKey, "InstallPath", NULL, NULL, (LPBYTE)path, &path_len) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(path);
        }
        RegCloseKey(hKey);
    }
    return "";
}

std::vector<std::string> get_steam_userdata_paths(const std::string &steam_path) {
    std::vector<std::string> results;
    std::string udata = steam_path + "\\userdata";
    for (char &c : udata) if (c == '/') c = '\\';

    HKEY hKey;
    DWORD active_user = 0;
    DWORD size = sizeof(active_user);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "ActiveUser", NULL, NULL, (LPBYTE)&active_user, &size);
        RegCloseKey(hKey);
    }

    if (active_user != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%u", (unsigned int)active_user);
        std::string p = udata + "\\" + buf;
        DWORD a = GetFileAttributesA(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            results.push_back(p);
            return results;
        }
    }

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((udata + "\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                std::strcmp(fd.cFileName, ".") != 0 && std::strcmp(fd.cFileName, "..") != 0) {
                results.push_back(udata + "\\" + fd.cFileName);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    return results;
}

bool is_steam_process_running() {
    bool running = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, "steam.exe") == 0) {
                    running = true;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }
    return running;
}

std::string get_current_exe_full_path() {
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    return std::string(exe_path);
}

std::string get_current_exe_dir(const std::string &exe_path) {
    auto pos = exe_path.find_last_of("\\/");
    if (pos != std::string::npos) {
        return exe_path.substr(0, pos);
    }
    return "";
}

#else

std::string get_steam_install_dir() {
    const char *home = getenv("HOME");
    if (!home) return "";
    std::string s1 = std::string(home) + "/.steam/steam";
    std::string s2 = std::string(home) + "/.local/share/Steam";
    std::ifstream f1(s1 + "/steam.sh");
    if (f1) return s1;
    std::ifstream f2(s2 + "/steam.sh");
    if (f2) return s2;
    return s1;
}

std::vector<std::string> get_steam_userdata_paths(const std::string &steam_path) {
    std::vector<std::string> res;
    res.push_back(steam_path + "/userdata");
    return res;
}

bool is_steam_process_running() {
    return false;
}

std::string get_current_exe_full_path() {
    return "pvz2";
}

std::string get_current_exe_dir(const std::string &) {
    return ".";
}

#endif

bool check_if_already_in_vdf(const std::vector<VdfShortcut> &scs, const std::string &exe_path) {
    std::string norm_exe = exe_path;
    for (char &c : norm_exe) {
        if (c == '/') c = '\\';
        c = (char)tolower(c);
    }

    for (const auto &sc : scs) {
        std::string e = sc.get_string("Exe");
        if (e.size() >= 2 && e.front() == '"' && e.back() == '"') {
            e = e.substr(1, e.size() - 2);
        }
        for (char &c : e) {
            if (c == '/') c = '\\';
            c = (char)tolower(c);
        }
        if (e == norm_exe) return true;
    }
    return false;
}

std::string find_artwork_file(const std::string &filename, const std::string &current_dir) {
#ifdef _WIN32
    // 1. Priority: Bundled assets/steam/ next to executable
    std::string p1 = current_dir + "\\assets\\steam\\" + filename;
    if (GetFileAttributesA(p1.c_str()) != INVALID_FILE_ATTRIBUTES) return p1;

    // 2. User's desktop Iconos folder
    std::string p2 = "C:\\Users\\onyxd\\Desktop\\Iconos\\" + filename;
    if (GetFileAttributesA(p2.c_str()) != INVALID_FILE_ATTRIBUTES) return p2;

    // 3. Iconos/ next to executable
    std::string p3 = current_dir + "\\Iconos\\" + filename;
    if (GetFileAttributesA(p3.c_str()) != INVALID_FILE_ATTRIBUTES) return p3;

    // 4. assets/ next to executable
    std::string p4 = current_dir + "\\assets\\" + filename;
    if (GetFileAttributesA(p4.c_str()) != INVALID_FILE_ATTRIBUTES) return p4;

    // 5. Exe root directory
    std::string p5 = current_dir + "\\" + filename;
    if (GetFileAttributesA(p5.c_str()) != INVALID_FILE_ATTRIBUTES) return p5;
#endif
    return "";
}

std::vector<uint8_t> load_artwork_bytes(const std::string &filename, int rc_id, const std::string &current_dir) {
    // 1. Try embedded PE resource (baked directly inside pvz2.exe!)
#ifdef _WIN32
    HMODULE hMod = GetModuleHandleA(NULL);
    HRSRC hRes = FindResourceA(hMod, MAKEINTRESOURCEA(rc_id), RT_RCDATA);
    if (hRes) {
        HGLOBAL hGlob = LoadResource(hMod, hRes);
        DWORD sz = SizeofResource(hMod, hRes);
        const void *ptr = LockResource(hGlob);
        if (ptr && sz > 0) {
            const uint8_t *b = reinterpret_cast<const uint8_t*>(ptr);
            pvz2_log("[steam] Loaded %s from embedded resource %d (%u bytes)", filename.c_str(), rc_id, (unsigned int)sz);
            return std::vector<uint8_t>(b, b + sz);
        }
    }
#endif

    // 2. Try host asset loader (Linux fallback)
    int asset_sz = 0;
    void *asset_buf = pvz2_load_asset_bytes(rc_id, &asset_sz);
    if (asset_buf && asset_sz > 0) {
        const uint8_t *b = reinterpret_cast<const uint8_t*>(asset_buf);
        std::vector<uint8_t> res(b, b + asset_sz);
        SDL_free(asset_buf);
        pvz2_log("[steam] Loaded %s from host asset loader (%d bytes)", filename.c_str(), asset_sz);
        return res;
    }

    // 3. Try filesystem locations (assets/steam, Desktop/Iconos, etc.)
    std::string path = find_artwork_file(filename, current_dir);
    if (!path.empty()) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            pvz2_log("[steam] Loaded %s from file %s", filename.c_str(), path.c_str());
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        }
    }

    return {};
}

void install_steam_artwork(const std::string &user_dir, uint32_t unsigned_appid, const std::string &current_dir) {
#ifdef _WIN32
    std::string grid_dir = user_dir + "\\config\\grid";
    CreateDirectoryA(grid_dir.c_str(), NULL);

    std::string id_str = std::to_string(unsigned_appid);
    uint64_t appid_64 = (static_cast<uint64_t>(unsigned_appid) << 32) | 0x02000000ULL;
    std::string id64_str = std::to_string(appid_64);

    auto grid_bytes = load_artwork_bytes("Grid.png", 350, current_dir);
    auto hero_bytes = load_artwork_bytes("Hero.png", 351, current_dir);
    auto logo_bytes = load_artwork_bytes("Logo.png", 352, current_dir);

    auto write_file = [](const std::string &p, const std::vector<uint8_t> &data) {
        if (data.empty()) return;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (out) out.write(reinterpret_cast<const char*>(data.data()), data.size());
    };

    // 1. Grid capsule (vertical and horizontal fallback)
    if (!grid_bytes.empty()) {
        write_file(grid_dir + "\\" + id_str + "p.png", grid_bytes);
        write_file(grid_dir + "\\" + id_str + ".png", grid_bytes);
        write_file(grid_dir + "\\" + id64_str + "p.png", grid_bytes);
        write_file(grid_dir + "\\" + id64_str + ".png", grid_bytes);
        pvz2_log("[steam] Installed Grid artwork (%zu bytes) to %s", grid_bytes.size(), grid_dir.c_str());
    }

    // 2. Hero banner
    if (!hero_bytes.empty()) {
        write_file(grid_dir + "\\" + id_str + "_hero.png", hero_bytes);
        write_file(grid_dir + "\\" + id64_str + "_hero.png", hero_bytes);
        pvz2_log("[steam] Installed Hero artwork (%zu bytes) to %s", hero_bytes.size(), grid_dir.c_str());
    }

    // 3. Logo
    if (!logo_bytes.empty()) {
        write_file(grid_dir + "\\" + id_str + "_logo.png", logo_bytes);
        write_file(grid_dir + "\\" + id64_str + "_logo.png", logo_bytes);
        pvz2_log("[steam] Installed Logo artwork (%zu bytes) to %s", logo_bytes.size(), grid_dir.c_str());
    }

    // 4. Logo position config JSON
    const char *logo_json = "{\"nVersion\":1,\"logoPosition\":{\"pinnedPosition\":\"CenterCenter\",\"nWidthPct\":49.87452948557086,\"nHeightPct\":74.30555555555554}}\n";
    {
        std::ofstream jf(grid_dir + "\\" + id_str + ".json", std::ios::trunc);
        if (jf) jf << logo_json;
    }
    {
        std::ofstream jf(grid_dir + "\\" + id64_str + ".json", std::ios::trunc);
        if (jf) jf << logo_json;
    }
#endif
}

} // namespace

extern "C" int pvz2_add_to_steam(char *msg_out, int msg_cap, const char *locale) {
    if (!locale || !locale[0]) {
        const pvz2_config_t *cfg = pvz2_config();
        locale = (cfg && cfg->user_locale[0]) ? cfg->user_locale : "es_ES";
    }
    const SteamStrings &str = get_strings_for_locale(locale);

    std::string steam_dir = get_steam_install_dir();
    if (steam_dir.empty()) {
        if (msg_out && msg_cap > 0) {
            std::snprintf(msg_out, msg_cap, "%s", str.status_not_found);
        }
        return 0;
    }

    std::vector<std::string> user_dirs = get_steam_userdata_paths(steam_dir);
    if (user_dirs.empty()) {
        if (msg_out && msg_cap > 0) {
            std::snprintf(msg_out, msg_cap, "%s", str.status_no_profiles);
        }
        return 0;
    }

    std::string current_exe = get_current_exe_full_path();
    std::string current_dir = get_current_exe_dir(current_exe);
    int added_count = 0;
    int already_count = 0;

    for (const auto &udir : user_dirs) {
        std::string config_dir = udir + "\\config";
#ifdef _WIN32
        CreateDirectoryA(config_dir.c_str(), NULL);
#endif
        std::string vdf_path = config_dir + "\\shortcuts.vdf";
        std::vector<uint8_t> vdf_bytes;
        {
            std::ifstream in(vdf_path, std::ios::binary);
            if (in) {
                vdf_bytes.assign((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            }
        }

        std::vector<VdfShortcut> shortcuts;
        if (!vdf_bytes.empty()) {
            parse_vdf_data(vdf_bytes, shortcuts);
        }

        std::string app_name = "Plants vs. Zombies 2";
        int32_t appid = calculate_shortcut_appid(current_exe, app_name);
        uint32_t unsigned_appid = static_cast<uint32_t>(appid);

        // Install or update custom Steam grid artwork (Grid, Hero, Logo)
        install_steam_artwork(udir, unsigned_appid, current_dir);

        if (check_if_already_in_vdf(shortcuts, current_exe)) {
            already_count++;
            continue; // Already present in this profile
        }

        // Backup existing shortcuts.vdf once
        std::string bak_path = vdf_path + ".bak";
        if (!vdf_bytes.empty()) {
            std::ofstream bak(bak_path, std::ios::binary | std::ios::trunc);
            if (bak) bak.write(reinterpret_cast<const char*>(vdf_bytes.data()), vdf_bytes.size());
        }

        // Build new shortcut
        VdfShortcut ns;
        ns.set_int("appid", appid);
        ns.set_string("AppName", app_name);
        ns.set_string("Exe", "\"" + current_exe + "\"");
        ns.set_string("StartDir", "\"" + current_dir + "\\\"");
        ns.set_string("icon", current_exe);
        ns.set_string("ShortcutPath", "");
        ns.set_string("LaunchOptions", "");
        ns.set_int("IsHidden", 0);
        ns.set_int("AllowDesktopConfig", 1);
        ns.set_int("AllowOverlay", 1);
        ns.set_int("openvr", 0);
        ns.set_int("Devkit", 0);
        ns.set_string("DevkitGameID", "");
        ns.set_int("DevkitOverrideAppID", 0);
        ns.set_int("LastPlayTime", 0);
        ns.set_string("FlatpakAppID", "");
        ns.set_string("sortas", "");

        VdfField tf;
        tf.type = 0;
        tf.key = "tags";
        ns.fields.push_back(tf);

        shortcuts.push_back(ns);

        std::vector<uint8_t> new_bytes = serialize_vdf_data(shortcuts);
        std::ofstream out(vdf_path, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(new_bytes.data()), new_bytes.size());
            out.close();
            added_count++;
            pvz2_log("[steam] Added shortcut to %s (appid: %d)", vdf_path.c_str(), appid);
        }
    }

    set_steam_asked_in_config();

    if (msg_out && msg_cap > 0) {
        if (added_count == 0 && already_count > 0) {
            std::snprintf(msg_out, msg_cap, "%s", str.status_already);
        } else if (is_steam_process_running()) {
            std::snprintf(msg_out, msg_cap, "%s", str.status_restart_steam);
        } else {
            std::snprintf(msg_out, msg_cap, "%s", str.status_success);
        }
    }
    return (added_count > 0 || already_count > 0) ? 1 : 0;
}

extern "C" void pvz2_check_steam_shortcut_prompt(void) {
#ifdef _WIN32
    // If the game was launched FROM Steam, Steam environment variables are present
    if (GetEnvironmentVariableA("SteamAppId", NULL, 0) > 0 ||
        GetEnvironmentVariableA("SteamGameId", NULL, 0) > 0) {
        return;
    }

    // Check if Steam is installed
    std::string steam_dir = get_steam_install_dir();
    if (steam_dir.empty()) return;

    // Check if the user was already asked in config.ini
    if (is_steam_asked_in_config()) return;

    // Check if already in Steam's shortcuts.vdf
    std::string current_exe = get_current_exe_full_path();
    std::vector<std::string> user_dirs = get_steam_userdata_paths(steam_dir);
    bool already_present = false;
    for (const auto &udir : user_dirs) {
        std::string vdf = udir + "\\config\\shortcuts.vdf";
        std::ifstream in(vdf, std::ios::binary);
        if (in) {
            std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            std::vector<VdfShortcut> scs;
            if (parse_vdf_data(b, scs)) {
                if (check_if_already_in_vdf(scs, current_exe)) {
                    already_present = true;
                    break;
                }
            }
        }
    }

    if (already_present) {
        set_steam_asked_in_config();
        return;
    }

    // Get localized strings
    const pvz2_config_t *cfg = pvz2_config();
    const char *locale = (cfg && cfg->user_locale[0]) ? cfg->user_locale : "es_ES";
    const SteamStrings &str = get_strings_for_locale(locale);

    // Show native Yes/No dialog
    int btn = MessageBoxW(NULL, str.prompt, str.title,
                          MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);

    // Record that we have asked so it won't prompt again
    set_steam_asked_in_config();

    if (btn == IDYES) {
        char status[256] = {0};
        pvz2_add_to_steam(status, sizeof(status), locale);

        if (is_steam_process_running()) {
            MessageBoxW(NULL, str.notice_running, str.title,
                        MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        } else {
            MessageBoxW(NULL, str.notice_success, str.title,
                        MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        }
    }
#endif
}

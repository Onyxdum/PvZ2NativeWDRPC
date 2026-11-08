#include <pvz2native/first_boot_wizard.h>
#include <pvz2native/config.h>
#include <pvz2native/host_paths.h>
#include "minizip/unzip.h"

#include <SDL.h>

#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

namespace fs = std::filesystem;

namespace {

static bool is_spanish_locale() {
    const pvz2_config_t *cfg = pvz2_config();
    if (cfg && strncmp(cfg->user_locale, "es", 2) == 0) return true;
    SDL_Locale *locs = SDL_GetPreferredLocales();
    if (locs) {
        if (locs[0].language && strncmp(locs[0].language, "es", 2) == 0) {
            SDL_free(locs);
            return true;
        }
        SDL_free(locs);
    }
    return false;
}

static bool file_exists_and_not_empty(const std::string &path) {
    if (path.empty()) return false;
    SDL_RWops *f = SDL_RWFromFile(path.c_str(), "rb");
    if (!f) return false;
    Sint64 size = SDL_RWsize(f);
    SDL_RWclose(f);
    return size > 0;
}

static std::string to_lower_str(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

#ifdef _WIN32
static std::wstring utf8_to_wide(const std::string &str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    std::wstring w(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &w[0], size);
    return w;
}

static std::string wide_to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string s(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &s[0], size, NULL, NULL);
    return s;
}

static std::string open_file_dialog_win(const wchar_t *title, const wchar_t *filter, const wchar_t *def_ext) {
    wchar_t buf[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf) / sizeof(buf[0]);
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = def_ext;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        return wide_to_utf8(buf);
    }
    return "";
}

static bool copy_file_platform(const std::string &src, const std::string &dst) {
    std::wstring wsrc = utf8_to_wide(src);
    std::wstring wdst = utf8_to_wide(dst);
    return CopyFileW(wsrc.c_str(), wdst.c_str(), FALSE) != FALSE;
}
#else
static std::string open_file_dialog_linux(const char *title, const char *filter) {
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "zenity --file-selection --title=\"%s\" --file-filter=\"%s\" 2>/dev/null", title, filter);
    FILE *f = popen(cmd, "r");
    if (!f) return "";
    char buf[1024] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[len - 1] = '\0';
            len--;
        }
        pclose(f);
        return std::string(buf);
    }
    pclose(f);
    return "";
}

static bool copy_file_platform(const std::string &src, const std::string &dst) {
    std::error_code ec;
    return fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
}
#endif

static bool extract_current_unz_file(unzFile uf, const std::string &dest_path) {
    if (unzOpenCurrentFile(uf) != UNZ_OK) return false;
    FILE *fout = fopen(dest_path.c_str(), "wb");
    if (!fout) {
        unzCloseCurrentFile(uf);
        return false;
    }
    char buf[65536];
    int r = 0;
    bool ok = true;
    while ((r = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, r, fout) != (size_t)r) {
            ok = false;
            break;
        }
    }
    fclose(fout);
    unzCloseCurrentFile(uf);
    return ok;
}

static bool scan_and_extract_archive(const std::string &archive_path, const std::string &dest_dir,
                                     bool &out_so, bool &out_obb, std::string &out_pkg) {
    unzFile uf = unzOpen64(archive_path.c_str());
    if (!uf) return false;

    if (unzGoToFirstFile(uf) != UNZ_OK) {
        unzClose(uf);
        return false;
    }

    std::string inner_apk_name = "";

    do {
        unz_file_info64 fi;
        char name_buf[512] = {0};
        if (unzGetCurrentFileInfo64(uf, &fi, name_buf, sizeof(name_buf) - 1, NULL, 0, NULL, 0) == UNZ_OK) {
            std::string entry_name = name_buf;
            std::string lower_entry = to_lower_str(entry_name);

            /* Check for libPVZ2.so */
            if (lower_entry.find("libpvz2.so") != std::string::npos) {
                const std::string dest_so = dest_dir + "libPVZ2.so";
                if (extract_current_unz_file(uf, dest_so)) {
                    out_so = true;
                }
            }
            /* Check for .obb file */
            else if (lower_entry.size() > 4 && lower_entry.compare(lower_entry.size() - 4, 4, ".obb") == 0) {
                const std::string dest_obb = dest_dir + "data.obb";
                if (extract_current_unz_file(uf, dest_obb)) {
                    out_obb = true;
                    if (lower_entry.find("row") != std::string::npos) out_pkg = "com.ea.game.pvz2_row";
                    else if (lower_entry.find("na") != std::string::npos) out_pkg = "com.ea.game.pvz2_na";
                }
            }
            /* Check for inner APK (e.g. in XAPK packages) */
            else if (lower_entry.size() > 4 && lower_entry.compare(lower_entry.size() - 4, 4, ".apk") == 0) {
                inner_apk_name = entry_name;
                if (lower_entry.find("row") != std::string::npos && out_pkg.empty()) out_pkg = "com.ea.game.pvz2_row";
                else if (lower_entry.find("na") != std::string::npos && out_pkg.empty()) out_pkg = "com.ea.game.pvz2_na";
            }
        }
    } while (unzGoToNextFile(uf) == UNZ_OK);

    /* If libPVZ2.so wasn't in the root, but an inner APK was found, extract from inner APK */
    if (!out_so && !inner_apk_name.empty()) {
        if (unzLocateFile(uf, inner_apk_name.c_str(), 1) == UNZ_OK) {
            const std::string temp_apk = dest_dir + "__temp_setup.apk";
            if (extract_current_unz_file(uf, temp_apk)) {
                unzFile inner_uf = unzOpen64(temp_apk.c_str());
                if (inner_uf) {
                    if (unzGoToFirstFile(inner_uf) == UNZ_OK) {
                        do {
                            unz_file_info64 fi;
                            char name_buf[512] = {0};
                            if (unzGetCurrentFileInfo64(inner_uf, &fi, name_buf, sizeof(name_buf) - 1, NULL, 0, NULL, 0) == UNZ_OK) {
                                std::string lower_entry = to_lower_str(name_buf);
                                if (lower_entry.find("libpvz2.so") != std::string::npos) {
                                    const std::string dest_so = dest_dir + "libPVZ2.so";
                                    if (extract_current_unz_file(inner_uf, dest_so)) {
                                        out_so = true;
                                        break;
                                    }
                                }
                            }
                        } while (unzGoToNextFile(inner_uf) == UNZ_OK);
                    }
                    unzClose(inner_uf);
                }
                std::remove(temp_apk.c_str());
            }
        }
    }

    unzClose(uf);
    return out_so || out_obb;
}

static void show_message_box(const char *title, const char *msg, Uint32 flags = SDL_MESSAGEBOX_INFORMATION) {
    SDL_ShowSimpleMessageBox(flags, title, msg, NULL);
}

} // namespace

extern "C" int pvz2_ensure_required_files(const pvz2_config_t *cfg) {
    if (!cfg) return 0;

    /* 1. If configured files exist and are not empty, boot immediately */
    if (file_exists_and_not_empty(cfg->so_path) && file_exists_and_not_empty(cfg->obb_path)) {
        return 1;
    }

    char *base = pvz2_host_data_path();
    const std::string base_dir = base ? base : "";
    if (base) SDL_free(base);

    /* 2. Check if files are already sitting in base_dir or base_dir/lib */
    std::string found_so = "";
    std::string found_obb = "";
    std::string detected_pkg = "";

    if (file_exists_and_not_empty(base_dir + "libPVZ2.so")) {
        found_so = base_dir + "libPVZ2.so";
    } else if (file_exists_and_not_empty(base_dir + "lib/libPVZ2.so")) {
        found_so = base_dir + "lib/libPVZ2.so";
    }

    if (file_exists_and_not_empty(base_dir + "data.obb")) {
        found_obb = base_dir + "data.obb";
    } else if (file_exists_and_not_empty(base_dir + "lib/main.147.com.ea.game.pvz2_row.obb")) {
        found_obb = base_dir + "lib/main.147.com.ea.game.pvz2_row.obb";
        detected_pkg = "com.ea.game.pvz2_row";
    } else {
        /* Scan base_dir and base_dir/lib for any *.obb file */
        std::error_code ec;
        for (const auto &dir_path : {base_dir, base_dir + "lib"}) {
            if (!fs::exists(dir_path, ec)) continue;
            for (const auto &entry : fs::directory_iterator(dir_path, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                std::string filename = entry.path().filename().string();
                std::string lower = to_lower_str(filename);
                if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".obb") == 0) {
                    found_obb = entry.path().string();
                    if (lower.find("row") != std::string::npos) detected_pkg = "com.ea.game.pvz2_row";
                    else if (lower.find("na") != std::string::npos) detected_pkg = "com.ea.game.pvz2_na";
                    break;
                }
            }
            if (!found_obb.empty()) break;
        }
    }

    /* If both were found already in folder, auto-configure and proceed without any prompt */
    if (!found_so.empty() && !found_obb.empty()) {
        pvz2_config_set_paths_and_package(found_so.c_str(), found_obb.c_str(),
                                          detected_pkg.empty() ? nullptr : detected_pkg.c_str());
        return 1;
    }

    /* 3. Missing files -> First-boot setup wizard */
    const bool es = is_spanish_locale();

    const char *title = es ? "PvZ2Native - Instalación Inicial" : "PvZ2Native - Initial Setup";
    const char *welcome_msg = es
        ? "No se han encontrado los archivos originales del juego (libPVZ2.so y/o data.obb).\n\n"
          "Para jugar a Plants vs. Zombies 2 Native en PC necesitas suministrar el archivo APK y/o OBB de la versión 4.5.2 de Android.\n\n"
          "¿Deseas seleccionarlos ahora mediante el asistente?"
        : "The original game files (libPVZ2.so and/or data.obb) were not found.\n\n"
          "To play Plants vs. Zombies 2 Native on PC, you need to provide the APK and/or OBB file from Android version 4.5.2.\n\n"
          "Would you like to select them now with the setup wizard?";

    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, es ? "Seleccionar archivos" : "Select Files" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, es ? "Salir" : "Exit" },
    };

    const SDL_MessageBoxData mb_data = {
        SDL_MESSAGEBOX_INFORMATION,
        NULL,
        title,
        welcome_msg,
        (int)(sizeof(buttons) / sizeof(buttons[0])),
        buttons,
        NULL
    };

    int button_id = 0;
    if (SDL_ShowMessageBox(&mb_data, &button_id) < 0 || button_id != 1) {
        return 0; /* User cancelled or closed dialog */
    }

    bool has_so = !found_so.empty();
    bool has_obb = !found_obb.empty();
    std::string last_selected_dir = "";

    /* Step A: Ask for APK / XAPK if libPVZ2.so is missing */
    if (!has_so) {
#ifdef _WIN32
        const wchar_t *filter_win = L"Android Package / XAPK (*.apk;*.xapk;*.zip)\0*.apk;*.xapk;*.zip\0Todos los archivos (*.*)\0*.*\0\0";
        const wchar_t *title_win = es ? L"Selecciona el archivo APK o XAPK de PvZ 2 (v4.5.2)" : L"Select PvZ 2 APK or XAPK (v4.5.2)";
        std::string chosen_apk = open_file_dialog_win(title_win, filter_win, L"apk");
#else
        std::string chosen_apk = open_file_dialog_linux(es ? "Selecciona el archivo APK o XAPK de PvZ 2 (v4.5.2)" : "Select PvZ 2 APK or XAPK (v4.5.2)", "*.apk *.xapk *.zip");
#endif
        if (chosen_apk.empty()) {
            return 0; /* User cancelled file picker */
        }

        std::error_code ec;
        last_selected_dir = fs::path(chosen_apk).parent_path().string();

        bool extracted_so = false;
        bool extracted_obb = false;
        std::string pkg_name = "";

        if (scan_and_extract_archive(chosen_apk, base_dir, extracted_so, extracted_obb, pkg_name)) {
            if (extracted_so) {
                has_so = true;
                found_so = base_dir + "libPVZ2.so";
            }
            if (extracted_obb) {
                has_obb = true;
                found_obb = base_dir + "data.obb";
                if (!pkg_name.empty()) detected_pkg = pkg_name;
            }
        }

        if (!has_so) {
            show_message_box(title, es
                ? "No se pudo encontrar libPVZ2.so dentro del archivo seleccionado.\n"
                  "Asegúrate de seleccionar un APK o XAPK válido de la versión 4.5.2 (armeabi-v7a)."
                : "Could not find libPVZ2.so inside the selected archive.\n"
                  "Please ensure you select a valid version 4.5.2 (armeabi-v7a) APK or XAPK.",
                SDL_MESSAGEBOX_ERROR);
            return 0;
        }
    }

    /* Step B: Check for OBB if still missing */
    if (!has_obb) {
        /* Check if an OBB exists in the same folder as the APK */
        if (!last_selected_dir.empty()) {
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator(last_selected_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                std::string fname = entry.path().filename().string();
                std::string lower = to_lower_str(fname);
                if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".obb") == 0) {
                    const std::string dest = base_dir + "data.obb";
                    if (copy_file_platform(entry.path().string(), dest)) {
                        has_obb = true;
                        found_obb = dest;
                        if (lower.find("row") != std::string::npos) detected_pkg = "com.ea.game.pvz2_row";
                        else if (lower.find("na") != std::string::npos) detected_pkg = "com.ea.game.pvz2_na";
                        break;
                    }
                }
            }
        }

        /* If still not found, prompt the user for the OBB */
        if (!has_obb) {
            show_message_box(title, es
                ? "Librería libPVZ2.so preparada con éxito.\n\n"
                  "Ahora selecciona el archivo de datos OBB (.obb) de PvZ 2 (v4.5.2)."
                : "libPVZ2.so extracted successfully.\n\n"
                  "Now please select the PvZ 2 OBB data file (.obb) for version 4.5.2.");

#ifdef _WIN32
            const wchar_t *filter_obb = L"PvZ 2 OBB Data (*.obb)\0*.obb\0Todos los archivos (*.*)\0*.*\0\0";
            const wchar_t *title_obb = es ? L"Selecciona el archivo OBB de PvZ 2 (v4.5.2)" : L"Select PvZ 2 OBB file (v4.5.2)";
            std::string chosen_obb = open_file_dialog_win(title_obb, filter_obb, L"obb");
#else
            std::string chosen_obb = open_file_dialog_linux(es ? "Selecciona el archivo OBB de PvZ 2 (v4.5.2)" : "Select PvZ 2 OBB file (v4.5.2)", "*.obb");
#endif
            if (chosen_obb.empty()) {
                return 0;
            }

            std::string lower_obb = to_lower_str(chosen_obb);
            if (lower_obb.find("row") != std::string::npos) detected_pkg = "com.ea.game.pvz2_row";
            else if (lower_obb.find("na") != std::string::npos) detected_pkg = "com.ea.game.pvz2_na";

            const std::string dest_obb = base_dir + "data.obb";
            if (copy_file_platform(chosen_obb, dest_obb)) {
                has_obb = true;
                found_obb = dest_obb;
            } else {
                show_message_box(title, es
                    ? "Error al copiar el archivo OBB al directorio del juego."
                    : "Error copying the OBB file to the game directory.",
                    SDL_MESSAGEBOX_ERROR);
                return 0;
            }
        }
    }

    /* Final verification */
    if (file_exists_and_not_empty(found_so) && file_exists_and_not_empty(found_obb)) {
        pvz2_config_set_paths_and_package("libPVZ2.so", "data.obb",
                                          detected_pkg.empty() ? "com.ea.game.pvz2_row" : detected_pkg.c_str());

        show_message_box(title, es
            ? "¡Plants vs. Zombies 2 configurado con éxito!\n\nIniciando el juego..."
            : "Plants vs. Zombies 2 configured successfully!\n\nLaunching the game...");
        return 1;
    }

    return 0;
}

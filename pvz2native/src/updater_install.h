#pragma once
/* Installs a downloaded release package over the game folder (Linux self-update; also built on
 * Windows only for the PVZ2_UPDATER_SELFTEST test program). Every file it replaces is backed up
 * first and put back if anything fails. Saves (userdata/), the game data (.obb, libPVZ2.so) and
 * custom files are never touched; presence_rules*.txt are merged, not overwritten. */

#include <string>

namespace pvz2_updater {

/* Extracts a .zip into dest_dir (created). False + err on a damaged archive. */
bool extract_zip(const std::string &zip_path, const std::string &dest_dir, std::string &err);

/* The folder inside an extracted package that holds pvz2 / pvz2.AppImage (the zip root or its
 * single top folder). Empty when the package has neither. */
std::string package_root(const std::string &extracted_dir);

/* Copies the package over game_dir. appimage_path, when set, is the running AppImage: the
 * package's pvz2.AppImage replaces that file. log gets one line per step. */
bool install_package(const std::string &pkg_root, const std::string &game_dir, const std::string &appimage_path,
                     std::string &log, std::string &err);

}  // namespace pvz2_updater

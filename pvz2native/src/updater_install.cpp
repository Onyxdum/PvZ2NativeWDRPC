#if !defined(_WIN32) || defined(PVZ2_UPDATER_SELFTEST)

#include "updater_install.h"

#include "minizip/unzip.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace pvz2_updater {
namespace {

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool read_lines(const fs::path &p, std::vector<std::string> &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return true;
}

std::string rule_pattern(const std::string &line) {
    std::string t = trim(line);
    size_t bar = t.find('|');
    return lower(trim(bar == std::string::npos ? t : t.substr(0, bar)));
}

bool is_comment(const std::string &t) {
    return !t.empty() && (t[0] == ';' || t[0] == '#');
}

/* The player's rules stay as they are; rules of the new file whose pattern the player does not
 * have yet are appended (with the comments just above them). */
bool merge_presence_rules(const fs::path &dest, const fs::path &src, std::string &log) {
    std::vector<std::string> user, fresh;
    if (!read_lines(dest, user) || !read_lines(src, fresh)) return false;

    std::set<std::string> have;
    for (const auto &l : user) {
        std::string t = trim(l);
        if (t.empty() || is_comment(t)) continue;
        std::string p = rule_pattern(l);
        if (!p.empty()) have.insert(p);
    }

    std::vector<std::string> add, pending;
    for (const auto &l : fresh) {
        std::string t = trim(l);
        if (t.empty()) {
            if (!pending.empty()) pending.push_back(l);
            continue;
        }
        if (is_comment(t)) {
            pending.push_back(l);
            continue;
        }
        std::string p = rule_pattern(l);
        if (!p.empty() && !have.count(p)) {
            add.insert(add.end(), pending.begin(), pending.end());
            add.push_back(l);
            have.insert(p);
        }
        pending.clear();
    }

    if (add.empty()) {
        log += "  " + dest.filename().string() + " up to date (your rules are kept)\n";
        return true;
    }
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto &l : user) f << l << "\n";
    f << "\n; --- [ACTUALIZACION: Reglas nuevas anadidas automaticamente] ---\n";
    for (const auto &l : add) f << l << "\n";
    f.close();
    log += "  merged " + std::to_string(add.size()) + " new line(s) into " + dest.filename().string() + "\n";
    return (bool)f;
}

/* Writes next to the target and renames over it: a running binary can be replaced that way. */
bool replace_file(const fs::path &src, const fs::path &dst, bool executable) {
    std::error_code ec;
    if (!dst.parent_path().empty()) fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = dst;
    tmp += ".pvz2new";
    fs::remove(tmp, ec);
    if (!fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec)) return false;
    if (executable) {
        fs::permissions(tmp,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, ec);
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool same_contents(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[65536], bb[65536];
    while (fa && fb) {
        fa.read(ba, sizeof(ba));
        fb.read(bb, sizeof(bb));
        if (fa.gcount() != fb.gcount() || std::memcmp(ba, bb, (size_t)fa.gcount()) != 0) return false;
    }
    return true;
}

bool is_protected(const std::string &rel) {
    std::string l = lower(rel);
    return l.rfind("userdata/", 0) == 0 || l == "userdata" || (l.size() > 4 && l.compare(l.size() - 4, 4, ".obb") == 0) ||
           l == "libpvz2.so" || l.find("custom") != std::string::npos || l.rfind("pvz2_update_backup/", 0) == 0;
}

bool is_executable_name(const std::string &rel) {
    std::string l = lower(rel);
    return l == "pvz2" || (l.size() > 9 && l.compare(l.size() - 9, 9, ".appimage") == 0);
}

}  // namespace

bool extract_zip(const std::string &zip_path, const std::string &dest_dir, std::string &err) {
    unzFile uf = unzOpen64(zip_path.c_str());
    if (!uf) {
        err = "not a zip file";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    bool ok = unzGoToFirstFile(uf) == UNZ_OK;
    if (!ok) err = "empty zip";
    while (ok) {
        unz_file_info64 fi{};
        char name[2048] = {0};
        if (unzGetCurrentFileInfo64(uf, &fi, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK) {
            err = "bad zip entry";
            ok = false;
            break;
        }
        std::string n(name);
        std::replace(n.begin(), n.end(), '\\', '/');
        const bool unsafe = n.empty() || n[0] == '/' || n.find("..") != std::string::npos || n.find(':') != std::string::npos;
        if (!unsafe) {
            fs::path out = fs::path(dest_dir) / fs::path(n);
            if (n.back() == '/') {
                fs::create_directories(out, ec);
            } else {
                fs::create_directories(out.parent_path(), ec);
                if (unzOpenCurrentFile(uf) != UNZ_OK) {
                    err = "cannot open " + n;
                    ok = false;
                    break;
                }
                std::ofstream f(out, std::ios::binary | std::ios::trunc);
                char buf[65536];
                int r;
                while ((r = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) f.write(buf, r);
                const bool wrote = (bool)f;
                f.close();
                if (unzCloseCurrentFile(uf) != UNZ_OK || r < 0 || !wrote) {
                    err = "damaged entry " + n;
                    ok = false;
                    break;
                }
                const unsigned mode = (unsigned)((fi.external_fa >> 16) & 0777);
                if (mode) fs::permissions(out, (fs::perms)mode, fs::perm_options::replace, ec);
            }
        }
        int next = unzGoToNextFile(uf);
        if (next == UNZ_END_OF_LIST_OF_FILE) break;
        if (next != UNZ_OK) {
            err = "damaged zip";
            ok = false;
        }
    }
    unzClose(uf);
    return ok;
}

std::string package_root(const std::string &extracted_dir) {
    auto has_game = [](const fs::path &d) {
        std::error_code ec;
        return fs::exists(d / "pvz2", ec) || fs::exists(d / "pvz2.AppImage", ec);
    };
    const fs::path root(extracted_dir);
    if (has_game(root)) return root.string();
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    if (dirs.size() == 1 && has_game(dirs[0])) return dirs[0].string();
    return std::string();
}

bool install_package(const std::string &pkg_root, const std::string &game_dir, const std::string &appimage_path,
                     std::string &log, std::string &err) {
    struct Item {
        fs::path src, dst;
        std::string rel;
    };
    std::error_code ec;
    const fs::path root(pkg_root), game(game_dir);
    std::vector<Item> plan;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (ec || is_protected(rel)) continue;
        fs::path dst = (rel == "pvz2.AppImage" && !appimage_path.empty()) ? fs::path(appimage_path) : game / rel;
        plan.push_back({it->path(), dst, rel});
    }
    if (plan.empty()) {
        err = "the package is empty";
        return false;
    }

    /* Back up every file that will change. */
    const fs::path backup = game / "pvz2_update_backup";
    fs::remove_all(backup, ec);
    std::vector<std::pair<fs::path, fs::path>> saved;  /* backup copy -> original place */
    std::vector<fs::path> created;
    for (const Item &item : plan) {
        if (fs::exists(item.dst, ec)) {
            fs::path b = backup / item.rel;
            fs::create_directories(b.parent_path(), ec);
            if (!fs::copy_file(item.dst, b, fs::copy_options::overwrite_existing, ec)) {
                fs::remove_all(backup, ec);
                err = "could not back up " + item.rel;
                return false;
            }
            saved.emplace_back(b, item.dst);
        } else {
            created.push_back(item.dst);
        }
    }
    log += "  backed up " + std::to_string(saved.size()) + " file(s)\n";

    std::string failed;
    for (const Item &item : plan) {
        const std::string leaf = lower(item.dst.filename().string());
        const bool rules = leaf.rfind("presence_rules", 0) == 0 && leaf.size() > 4 &&
                           leaf.compare(leaf.size() - 4, 4, ".txt") == 0 && fs::exists(item.dst, ec);
        bool ok;
        if (rules) {
            fs::path bak = item.dst;
            bak += ".bak";
            fs::copy_file(item.dst, bak, fs::copy_options::overwrite_existing, ec);
            ok = merge_presence_rules(item.dst, item.src, log);
        } else {
            ok = replace_file(item.src, item.dst, is_executable_name(item.rel));
        }
        if (!ok) {
            failed = item.rel;
            break;
        }
    }

    if (!failed.empty()) {
        bool restored = true;
        for (const auto &s : saved) {
            /* The file that failed (or one never reached) can still be locked but is unchanged. */
            if (same_contents(s.first, s.second)) continue;
            if (!replace_file(s.first, s.second, is_executable_name(s.second.filename().string()))) restored = false;
        }
        for (const auto &c : created) fs::remove(c, ec);
        if (restored) fs::remove_all(backup, ec);
        err = "could not write " + failed + (restored ? "; the previous version was restored"
                                                      : "; some files could not be restored, a copy is in " +
                                                            backup.string());
        return false;
    }
    fs::remove_all(backup, ec);
    log += "  installed " + std::to_string(plan.size()) + " file(s)\n";
    return true;
}

}  // namespace pvz2_updater

#endif

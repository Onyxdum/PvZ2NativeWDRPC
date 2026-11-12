/* com.popcap.SexyAppFramework.AndroidGameApp
 *
 * The engine's main bridge into Android: device info, config storage, input
 * events, and -- most importantly -- asset access. Behaviour here was
 * ground-truthed against the decompiled classes.dex rather than guessed, and
 * each hook notes what the real Java does.
 */

#include <pvz2native/config.h>
#include <pvz2native/host_files.h>
#include <pvz2native/dependencies/vfs.h>
#include <pvz2native/dex/config_store.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/diagnostics/verbose.h>
#include <pvz2native/input/input_queue.h>
#include <pvz2native/presence.h>
#include <pvz2native/cloud_save.h>
#include <pvz2native/config_screen.h>

#include <SDL.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace pvz2native {
namespace dex {
namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/AndroidGameApp";

/* sub_9F0774 wraps 1024 bytes of its own stack as the direct ByteBuffer, and
 * starts reading records 16 bytes in (the count sits at +0). */
constexpr std::uint32_t kEventBufferSize = 1024;
constexpr std::uint32_t kEventRecordsOffset = 16;

/* Loads the .obb index on first use -- parsing 588 RSG headers costs a few ms
 * and nothing before the RSB load needs it. */
const RsbIndex::Entry *find_asset(DexCall &d, const std::string &name) {
    std::lock_guard<std::mutex> lk(d.c.rt->rsb_index_lock);
    if (!d.c.rt->rsb_index_tried) {
        d.c.rt->rsb_index_tried = true;
        d.c.rt->rsb_index.load(vfs::obb_host_path());
    }
    return d.c.rt->rsb_index.find(name);
}

/* --- [log] input diagnostics --------------------------------------------- */

std::uint32_t rd32(const std::vector<std::uint8_t> &v, std::size_t off) {
    if (off + 4 > v.size()) return 0;
    return (std::uint32_t)v[off] | ((std::uint32_t)v[off + 1] << 8) |
           ((std::uint32_t)v[off + 2] << 16) | ((std::uint32_t)v[off + 3] << 24);
}

/* Phase names as the ENGINE reads them (sub_9F0298's switch), not as the
 * numbering suggests: 3 is the end of a touch, 1 is a move. */
const char *touch_phase_name(std::uint32_t p) {
    switch (p) {
        case 0: return "DOWN";
        case 1: return "MOVE";
        case 3: return "UP";
        case 4: return "CANCEL";
        default: return "??";
    }
}

/* Walks the exact bytes just handed to the engine, stepping each record by the
 * size the engine will step it -- so a desync shows up here as an "unknown
 * record type" rather than as silently misread input. */
void log_records(DexCall &d, const std::vector<std::uint8_t> &r) {
    std::size_t off = 0;
    while (off + 4 <= r.size()) {
        const std::uint32_t type = rd32(r, off);
        if (type == 0) {
            const std::uint32_t phase = rd32(r, off + 36);
            d.c.log("[input]   touch %-6s (phase=%u) id=%u x=%d y=%d", touch_phase_name(phase),
                    phase, rd32(r, off + 4), (int)rd32(r, off + 8), (int)rd32(r, off + 12));
            off += 48;
        } else if (type == 1) {
            d.c.log("[input]   key code=%u down=%u", rd32(r, off + 4), rd32(r, off + 8));
            off += 32;
        } else if (type == 6) {
            const std::uint32_t n = rd32(r, off + 8);
            d.c.log("[input]   text len=%u", n);
            off += 12 + n + ((4 - n % 4) % 4);
        } else {
            //d.c.log("[input]   UNKNOWN record type %u at +%u -- encoder/engine disagree",
                    //type, (unsigned)off);
            break;
        }
    }
}

/* Reads whether the Board is currently paused by inspecting m_pauseSources (+1084). */
bool is_board_paused(DexCall &d) {
    if (!pvz2_presence_enabled()) return false;
    if (!d.c.img || d.c.img->so_base == 0) return false;
    const std::uint32_t got_slot = d.c.img->so_base + 0x110c734u;
    if (!d.c.in_bounds(got_slot, 4)) return false;
    const std::uint32_t app_ptr_addr = d.c.read32(got_slot);
    if (app_ptr_addr == 0 || !d.c.in_bounds(app_ptr_addr, 4)) return false;
    const std::uint32_t lawn_app = d.c.read32(app_ptr_addr);
    if (lawn_app == 0 || !d.c.in_bounds(lawn_app + 1668u, 4)) return false;
    const std::uint32_t board = d.c.read32(lawn_app + 1668u);
    if (board == 0 || !d.c.in_bounds(board + 1084u, 4)) return false;
    if (!d.c.in_bounds(board + 156u, 4)) return false;
    const std::uint32_t bstate = d.c.read32(board + 156u);
    if (bstate == 0 || bstate == 9) return false; // Initial or EndLevel
    const std::uint32_t pause_set = d.c.read32(board + 1084u);
    if (pause_set == 0 || !d.c.in_bounds(pause_set + 12u, 4)) return false;
    const std::uint32_t root = d.c.read32(pause_set + 8u);
    const std::uint32_t count = d.c.read32(pause_set + 12u);
    return root != 0 && count > 0;
}

/* Same walk as log_records, but only to learn whether a touch started or ended
 * in this batch -- that feeds the [state] input counters in diagnostics/verbose. */
void scan_touches(const std::vector<std::uint8_t> &r, bool &had_down, bool &had_up) {
    std::size_t off = 0;
    while (off + 4 <= r.size()) {
        const std::uint32_t type = rd32(r, off);
        if (type == 0) {
            const std::uint32_t phase = rd32(r, off + 36);
            if (phase == 0) had_down = true;
            if (phase == 3) had_up = true;
            off += 48;
        } else if (type == 1) {
            off += 32;
        } else if (type == 6) {
            const std::uint32_t n = rd32(r, off + 8);
            off += 12 + n + ((4 - n % 4) % 4);
        } else {
            break;
        }
    }
}

/* boolean UI_ProcessEvents(ByteBuffer OutData)
 *
 * The real mUIEventManager.ProcessEvents writes the number of pending input
 * events into the front of the direct ByteBuffer, then returns whether any were
 * handled.
 *
 * That buffer is a slice of the ENGINE'S OWN STACK -- sub_9F0774's local event
 * scratch, wrapped by NewDirectByteBuffer. Leaving it untouched meant the engine
 * read a stack-garbage event count, and whenever that was >= 1 it dispatched
 * off_CCFA10[garbage index] with garbage payloads, ran off into the input
 * handlers (sub_9F0298 -> sub_9E8528) and returned straight to the halt
 * sentinel -- so onDrawFrame never got back to sub_9E8AE4 to run the update
 * loop, and LawnApp::Update never executed a single frame.
 *
 * This is also the ONLY way input gets in: there are no Native_onKey* entry
 * points, and the keyboard JNI surface (Device_ShowKeyboard and friends) never
 * returns text. So every touch, key and typed character the host collects is
 * serialised here -- see input_queue.h for the record layouts, which were read
 * out of the engine's own dispatch handlers.
 *
 * Layout: the count goes at +0, and the records start at +16. The engine wraps
 * 1024 bytes, so 1008 remain for records; input_queue::drain leaves anything
 * that does not fit queued for the next frame. */
void ui_process_events(DexCall &d) {
    if (pvz2_presence_enabled()) {
        static bool s_last_paused = false;
        const bool paused = is_board_paused(d);
        if (paused != s_last_paused) {
            s_last_paused = paused;
            pvz2_presence_note("state", paused ? "Board Paused=true" : "Board Paused=false");
        }
    }

    std::uint32_t buf = d.arg(0);
    if (buf == 0 || !d.c.in_bounds(buf, kEventBufferSize)) {
        if (buf != 0 && d.c.in_bounds(buf, 4)) d.c.write32(buf, 0);
        d.ret_bool(false);
        return;
    }

    static std::vector<std::uint8_t> records;
    const std::size_t count = input::drain(records, kEventBufferSize - kEventRecordsOffset);

    d.c.write32(buf, (std::uint32_t)count);
    for (std::size_t i = 0; i < records.size(); ++i) {
        d.c.write8(buf + kEventRecordsOffset + (std::uint32_t)i, records[i]);
    }
    /* All of this is [log] input=1 (or verbose=1) only. It used to print the "delivered" line
     * unconditionally, which spammed a normal (input=0) run with a line per
     * frame that had any event. The per-record breakdown was already gated;
     * now the summary line is too. */
    if (count != 0 && diagnostics::input_trace()) {
        d.c.log("[input] f=%d delivered %u event(s), %u bytes", diagnostics::current_frame() + 1,
                (unsigned)count, (unsigned)records.size());
        /* Spell out every record. Needed whenever input "does nothing" -- it
         * separates "the host never sent it" from "the engine was handed it and
         * ignored it", which no amount of staring at the engine's handlers can
         * distinguish. */
        log_records(d, records);
    }
    if (count != 0 && diagnostics::verbose()) {
        bool had_down = false, had_up = false;
        scan_touches(records, had_down, had_up);
        diagnostics::note_input_delivered((unsigned)count, had_down, had_up);
    }
    d.ret_bool(count != 0);
}

/* void Device_ShowKeyboard(...) / void Device_ShowNumericKeyboard()
 * void Device_HideKeyboard() / boolean Device_IsKeyboardShowing()
 *
 * On a device these drive the soft keyboard through InputMethodManager. Here
 * they drive SDL's text input: main.c polls pvz2_input_keyboard_wanted() and
 * calls SDL_StartTextInput/SDL_StopTextInput accordingly, which is what makes
 * SDL_TEXTINPUT events start arriving.
 *
 * ShowNumericKeyboard is new in 4.5.2 and was the whole of why the age-gate
 * screen did nothing: the "AGE" field asks for it on tap, it was unhooked, so
 * SDL text input never turned on and no typing reached the box. On a device the
 * only difference from the plain keyboard is setUseNumericKeyboard(true), i.e.
 * which soft IME appears; on PC the digits come from the same physical keyboard
 * either way and the engine's numeric field filters non-digits itself, so it
 * maps to the identical "text input on".
 *
 * IsKeyboardShowing used to be left unhooked because "neither is true here" --
 * correct while there was no keyboard at all, wrong now: the engine gates the
 * name-entry flow on it, so it has to reflect the real state. */
void show_keyboard(DexCall &d) {
    pvz2_input_set_keyboard_wanted(1);
    d.c.log("[input] engine asked for the keyboard -- SDL text input on");
    d.ret(0);
}

void show_numeric_keyboard(DexCall &d) {
    pvz2_input_set_keyboard_wanted(1);
    d.c.log("[input] engine asked for the NUMERIC keyboard -- SDL text input on");
    d.ret(0);
}

void hide_keyboard(DexCall &d) {
    pvz2_input_set_keyboard_wanted(0);
    d.c.log("[input] engine dismissed the keyboard");
    d.ret(0);
}

void is_keyboard_showing(DexCall &d) { d.ret_bool(pvz2_input_keyboard_wanted() != 0); }

/* Static challenge objective counts per level */
static const std::unordered_map<std::string_view, std::int32_t> kLevelChallengeGoals = {
    {"beach12", 1},
    {"beach13", 2},
    {"beach15", 1},
    {"beach16", 1},
    {"beach23", 1},
    {"beach25", 2},
    {"beach28", 1},
    {"beach29", 1},
    {"beach30", 1},
    {"beach9", 2},
    {"challenge_levels/egypt/cl_egypt13_test_nochanges", 2},
    {"challenge_levels/premiumquest_firepeashooter_03", 1},
    {"challenge_levels/premiumquest_homingthistle_02", 1},
    {"challenge_levels/premiumquest_homingthistle_03", 1},
    {"challenge_levels/premiumquest_jalapeno_01", 1},
    {"challenge_levels/premiumquest_jalapeno_01_eq", 1},
    {"challenge_levels/premiumquest_peanut_02", 3},
    {"challenge_levels/premiumquest_snowpea_03", 1},
    {"cowboy11", 1},
    {"cowboy12", 1},
    {"cowboy15", 1},
    {"cowboy16", 1},
    {"cowboy17", 1},
    {"cowboy18", 1},
    {"cowboy19", 2},
    {"cowboy20", 1},
    {"cowboy21", 2},
    {"cowboy23", 2},
    {"cowboy24", 1},
    {"cowboy4", 1},
    {"cowboy6", 1},
    {"cowboy8", 1},
    {"dark10", 1},
    {"dark12", 1},
    {"dark14", 1},
    {"dark15", 1},
    {"dark17", 1},
    {"dark19", 1},
    {"dark6", 1},
    {"dark9", 1},
    {"dino12", 1},
    {"dino15", 1},
    {"dino16", 1},
    {"dino19", 1},
    {"dino20", 1},
    {"dino22", 1},
    {"dino26", 2},
    {"dino27", 1},
    {"dino7", 1},
    {"egypt13", 2},
    {"egypt15", 1},
    {"egypt16", 1},
    {"egypt17", 1},
    {"egypt18", 1},
    {"egypt19", 1},
    {"egypt20", 1},
    {"egypt22", 1},
    {"egypt23", 1},
    {"egypt24", 1},
    {"egypt7", 1},
    {"eighties13", 1},
    {"eighties15", 1},
    {"eighties16", 1},
    {"eighties21", 1},
    {"eighties24", 1},
    {"eighties26", 1},
    {"eighties30", 1},
    {"eighties5", 1},
    {"eighties9", 1},
    {"future11", 1},
    {"future14", 1},
    {"future15", 1},
    {"future18", 1},
    {"future19", 1},
    {"future20", 1},
    {"future21", 2},
    {"future22", 1},
    {"future23", 2},
    {"future24", 1},
    {"future4", 1},
    {"future8", 1},
    {"future9", 1},
    {"goldbloom3", 1},
    {"goldbloom4", 1},
    {"goldbloom6", 1},
    {"iceage12", 1},
    {"iceage13", 1},
    {"iceage15", 1},
    {"iceage16", 1},
    {"iceage23", 1},
    {"iceage24", 1},
    {"iceage27", 1},
    {"iceage28", 1},
    {"lod_001", 1},
    {"lod_003", 3},
    {"lod_006", 1},
    {"lod_008", 1},
    {"lod_011", 3},
    {"lod_022", 3},
    {"lod_033", 2},
    {"lod_034", 1},
    {"lod_035", 1},
    {"lod_038", 1},
    {"lod_043", 1},
    {"lod_044", 3},
    {"lod_049", 3},
    {"lod_055", 1},
    {"lod_056", 1},
    {"lod_074", 3},
    {"lod_076", 3},
    {"lod_077", 1},
    {"lod_082", 2},
    {"lod_084", 1},
    {"lod_085", 1},
    {"lod_094", 2},
    {"lod_095", 1},
    {"lod_097", 2},
    {"lod_099", 1},
    {"lod_101", 2},
    {"lod_102", 2},
    {"lod_103", 3},
    {"lod_107", 3},
    {"lod_108", 1},
    {"lod_109", 1},
    {"lod_116", 1},
    {"lod_117", 3},
    {"lod_122", 1},
    {"lod_125", 1},
    {"lod_131", 1},
    {"lod_135", 1},
    {"lod_141", 1},
    {"lod_143", 3},
    {"lod_144", 1},
    {"lod_156", 1},
    {"lod_160", 1},
    {"lod_165", 1},
    {"lod_167", 1},
    {"lod_169", 2},
    {"lod_171", 1},
    {"lod_173", 1},
    {"lod_175", 1},
    {"lod_184", 1},
    {"lod_185", 3},
    {"lod_188", 1},
    {"lod_189", 1},
    {"lod_192", 1},
    {"lod_203", 1},
    {"lod_206", 1},
    {"lod_207", 1},
    {"lod_210", 3},
    {"lod_211", 3},
    {"lod_212", 3},
    {"lod_213", 3},
    {"lod_stpats_084", 1},
    {"lod_stpats_171", 1},
    {"lod_template", 3},
    {"lostcity12", 1},
    {"lostcity15", 1},
    {"lostcity16", 1},
    {"lostcity24", 1},
    {"lostcity30", 2},
    {"lostcity8", 1},
    {"modern12", 1},
    {"modern15", 1},
    {"modern16", 1},
    {"modern28", 1},
    {"modern30", 1},
    {"modern6", 1},
    {"pirate11", 1},
    {"pirate13", 1},
    {"pirate14", 1},
    {"pirate15", 1},
    {"pirate16", 1},
    {"pirate17", 1},
    {"pirate19", 2},
    {"pirate20", 1},
    {"pirate21", 2},
    {"pirate22", 1},
    {"pirate23", 2},
    {"pirate24", 1},
    {"pirate3", 1},
    {"pirate8", 1},
    {"preview_plant_hurrikale", 3},
};

/* Counts active challenge objectives for a level ID. */
std::int32_t count_level_challenges(const std::string &level_id) {
    if (level_id.empty()) return 0;

    std::string clean = level_id;
    const auto colon = clean.find(':');
    if (colon != std::string::npos) {
        clean = clean.substr(0, colon);
    }
    for (char &c : clean) {
        if (c == '\\') c = '/';
        else c = (char)std::tolower((unsigned char)c);
    }

    auto it = kLevelChallengeGoals.find(clean);
    if (it != kLevelChallengeGoals.end()) {
        return it->second;
    }

    namespace fs = std::filesystem;
    const std::string suffix = "_" + clean;
    const fs::path save_dir = "userdata/save";
    std::error_code ec;
    if (fs::exists(save_dir, ec)) {
        for (const auto &entry : fs::directory_iterator(save_dir, ec)) {
            if (!entry.is_directory()) continue;
            const std::string name = entry.path().filename().string();
            if (name.size() >= suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                const fs::path rton_path = entry.path() / "save_LevelModuleInstances.rton";
                FILE *f = std::fopen(rton_path.string().c_str(), "rb");
                if (!f) continue;
                std::fseek(f, 0, SEEK_END);
                const long sz = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 100000) {
                    std::string data;
                    data.resize((std::size_t)sz);
                    std::fread(&data[0], 1, (std::size_t)sz, f);
                    std::fclose(f);

                    static const char *kChallengeModules[] = {
                        "ProtectThePlantChallengeModule",
                        "StarChallengeKillZombiesInTime",
                        "StarChallengePlantBeforeLine",
                        "StarChallengePlantsLost",
                        "StarChallengeSaveMowers",
                        "StarChallengeSimultaneousPlants",
                        "StarChallengeSpendSunHoldout",
                        "StarChallengeSunProduced",
                        "StarChallengeSunUsed",
                        "StarChallengeZombieDistance",
                    };
                    int count = 0;
                    for (const char *mod : kChallengeModules) {
                        if (data.find(mod) != std::string::npos) {
                            ++count;
                        }
                    }
                    if (count > 0) return count;
                } else {
                    std::fclose(f);
                }
            }
        }
    }

    return 0;
}

std::mutex g_crashlytics_mu;
std::string g_last_board_level;
std::string g_last_world;
std::int32_t g_last_level_num = 0;
std::int32_t g_last_goals_count = 0;
bool g_has_seed_chooser = false;
bool g_in_seed_chooser = false;
bool g_has_protect_plant = false;


/* String Resources_GetAssetFileInfo(String InFilename, long[] outFileInfo)
 *
 * On a device this opens the name as an APK asset and answers with
 * (getPackageResourcePath(), afd.getStartOffset(), afd.getLength()) so native
 * code can read the asset straight out of the APK -- assets are stored
 * uncompressed there, so an offset+length into the zip is enough.
 *
 * This APK ships no assets at all, but the .obb does contain the files, stored
 * raw for everything except textures. So the identical contract is answered
 * pointing at the .obb instead: same shape, same read path on the guest side,
 * no extraction. Returns null -- exactly what Java returns for a missing asset
 * -- when the name is not in the RSB, or is a compressed texture that cannot be
 * served as a flat range. */
void get_asset_file_info(DexCall &d) {
    std::string name = d.string_arg(0);
    std::uint32_t out_array = d.arg(1);
    pvz2_presence_note("asset", name.c_str()); /* Discord Rich Presence tap */

    const RsbIndex::Entry *found = find_asset(d, name);
    if (found == nullptr || found->compressed || out_array == 0) {
        d.c.log("[asset] \"%s\" -> %s", name.c_str(),
                found == nullptr      ? "NOT IN RSB"
                : found->compressed   ? "compressed texture, cannot serve raw"
                                      : "no out array");
        d.ret(0); /* null */
        return;
    }

    /* long[] is a plain 8-bytes-per-element guest buffer (see NewLongArray),
     * little-endian like the rest of the guest. */
    d.c.write32(out_array + 0, (std::uint32_t)(found->offset & 0xFFFFFFFFu));
    d.c.write32(out_array + 4, (std::uint32_t)(found->offset >> 32));
    d.c.write32(out_array + 8, found->size);
    d.c.write32(out_array + 12, 0);
    d.c.log("[asset] \"%s\" -> %s @0x%llx size=0x%x", name.c_str(), vfs::obb_host_path(),
            (unsigned long long)found->offset, found->size);
    d.ret_string(vfs::obb_host_path());
}

/* long Resources_GetAssetFileSize(String InFilename)
 *
 * Java returns the asset's length, or -1 when absent. The engine sizes its read
 * buffer from THIS call, not from the long[] that GetAssetFileInfo fills --
 * returning 0 made it allocate nothing and read 0 bytes, which surfaced as
 * "Invalid document object in resource file" once the info call started
 * succeeding. */
void get_asset_file_size(DexCall &d) {
    std::string name = d.string_arg(0);
    const RsbIndex::Entry *found = find_asset(d, name);
    std::uint64_t size = (found != nullptr && !found->compressed) ? found->size : (std::uint64_t)-1;
    d.c.log("[asset] size(\"%s\") -> %lld", name.c_str(), (long long)(std::int64_t)size);
    d.ret64(size);
}

/* long Resources_GetFileSystemBlockCount(String path)
 * long Resources_GetFileSystemBlocksFree(String path)
 * long Resources_GetFileSystemBlockSize(String path)
 *
 * StatFs on a device, and the engine's only way to ask "is there room?".
 *
 * Unhooked these answered 0, which does not read as "unknown" -- it reads as a
 * full disk with zero-byte blocks, a state no real device is ever in. The same
 * reasoning as the statfs() shim in libc_unistd.cpp, which this deliberately
 * agrees with down to the block size: report the truth about the host disk, so
 * a space check neither blocks on a false negative nor greenlights a write that
 * then fails halfway. Java answers -1 when StatFs throws, so that is what an
 * unreadable path gets here too. */
constexpr std::uint64_t kBlockSize = 4096;

std::uint64_t disk_space(DexCall &d, int which) {
    const std::string guest_path = d.string_arg(0);
    std::error_code ec;
    const std::filesystem::space_info s =
        std::filesystem::space(vfs::translate(d.c.rt, guest_path), ec);
    if (ec) return (std::uint64_t)-1;
    switch (which) {
        case 0: return (std::uint64_t)s.capacity / kBlockSize;
        case 1: return (std::uint64_t)s.available / kBlockSize;
        default: return kBlockSize;
    }
}

void fs_block_count(DexCall &d) { d.ret64(disk_space(d, 0)); }
void fs_blocks_free(DexCall &d) { d.ret64(disk_space(d, 1)); }
void fs_block_size(DexCall &d) { d.ret64(disk_space(d, 2)); }

/* --- config storage ---
 *
 * SharedPreferences on a device: a real, persistent, typed key/value store the
 * engine both writes AND reads back. The previous port accepted every write and
 * discarded it, and answered every read and every ConfigKeyExists with "not
 * present". That is not "a valid first-launch state" -- it is an amnesiac store,
 * and it violates the one invariant the engine relies on: what you write, you
 * can read.
 *
 * The [jni] census caught the consequence: at the end of the loading screen the
 * engine polls Config_ConfigKeyExists once every frame, forever, and our store
 * says false every time. The boot writes a key and then waits for it to exist;
 * with a store that forgets, that wait never ends. So the fix is to be a real
 * store -- honour writes, answer reads and existence from what was written.
 *
 * It also survives across launches, and now so does this store: it is loaded
 * once from <save_dir>/prefs.dat (or directly next to the .exe when save_dir is
 * left at its default root path) and rewritten on every change, gated by
 * [game] persist_saves (on by default). That is what fixes the age gate --
 * without it the engine wrote the entered age, could read it back within the
 * session, but found the store empty on the next launch and asked again every
 * time. persist_saves=0 restores the old "every run is a clean first launch"
 * behaviour, which is still what you want while bringing a version up.
 *
 * Booleans and integers share one numeric map because a key is only ever read
 * back as the type it was written; strings are separate. ConfigKeyExists checks
 * both. */
struct ConfigStore {
    std::mutex lock;
    std::map<std::string, std::string> strings;
    std::map<std::string, std::int64_t> numbers;
};

std::string prefs_path() {
    return std::string(pvz2_config()->save_dir) + "/prefs.dat";
}

/* A tiny length-prefixed binary format, so a key or a string value may contain
 * any byte (spaces, '=', newlines) without an escaping scheme:
 *   'S' u32 keylen key u32 vallen val   -- a string entry
 *   'N' u32 keylen key i64 val          -- a numeric entry
 * A short read stops parsing and keeps whatever was read, so a truncated file
 * degrades to a partial store rather than a crash. */
bool read_u32(std::FILE *f, std::uint32_t &v) {
    return std::fread(&v, sizeof(v), 1, f) == 1;
}
bool read_key(std::FILE *f, std::string &key) {
    std::uint32_t n = 0;
    if (!read_u32(f, n) || n > (1u << 20)) return false;
    key.resize(n);
    return n == 0 || std::fread(&key[0], 1, n, f) == n;
}

/* Both callers hold s.lock. */
void store_load_locked(ConfigStore &s) {
    if (!pvz2_config()->persist_saves) return;
    std::FILE *f = std::fopen(prefs_path().c_str(), "rb");
    if (f == nullptr) return;
    int type;
    while ((type = std::fgetc(f)) != EOF) {
        std::string key;
        if (!read_key(f, key)) break;
        if (type == 'S') {
            std::uint32_t vlen = 0;
            if (!read_u32(f, vlen) || vlen > (1u << 24)) break;
            std::string val(vlen, '\0');
            if (vlen != 0 && std::fread(&val[0], 1, vlen, f) != vlen) break;
            s.strings[key] = std::move(val);
        } else if (type == 'N') {
            std::int64_t val = 0;
            if (std::fread(&val, sizeof(val), 1, f) != 1) break;
            s.numbers[key] = val;
        } else {
            break; /* unknown record -- stop rather than resync blindly */
        }
    }
    std::fclose(f);
}

void store_save_locked(ConfigStore &s) {
    if (!pvz2_config()->persist_saves || pvz2_cloud_writes_locked()) return;
    std::error_code ec;
    std::filesystem::create_directories(pvz2_config()->save_dir, ec);
    /* Serialise first, then replace the file in one step, so an interrupted write cannot leave a
     * half-written prefs file that fails to load next launch. */
    std::vector<std::uint8_t> out;
    const auto put = [&out](const void *p, std::size_t n) {
        const auto *b = static_cast<const std::uint8_t *>(p);
        out.insert(out.end(), b, b + n);
    };
    for (const auto &kv : s.strings) {
        const std::uint32_t klen = (std::uint32_t)kv.first.size();
        const std::uint32_t vlen = (std::uint32_t)kv.second.size();
        out.push_back('S');
        put(&klen, sizeof(klen));
        put(kv.first.data(), klen);
        put(&vlen, sizeof(vlen));
        put(kv.second.data(), vlen);
    }
    for (const auto &kv : s.numbers) {
        const std::uint32_t klen = (std::uint32_t)kv.first.size();
        const std::int64_t val = kv.second;
        out.push_back('N');
        put(&klen, sizeof(klen));
        put(kv.first.data(), klen);
        put(&val, sizeof(val));
    }
    host_files::write_atomic(prefs_path(), out);
}

ConfigStore &config_store() {
    static ConfigStore store;
    static std::once_flag once;
    /* Loaded exactly once, on first use -- by which point pvz2_config_load has
     * long run (it happens in main before the session starts). */
    std::call_once(once, [] {
        std::lock_guard<std::mutex> lk(store.lock);
        store_load_locked(store);
    });
    return store;
}

/* Names each key the engine touches ONCE, so the log shows the working set
 * without a per-frame poll flooding it. This is what turns "polling some key
 * every frame" into a named key. */
void note_key(DexCall &d, const char *op, const std::string &key) {
    static std::mutex seen_lock;
    static std::map<std::string, bool> seen;
    std::lock_guard<std::mutex> lk(seen_lock);
    if (seen.emplace(std::string(op) + "\0" + key, true).second) {
        d.c.log("[config] %s(\"%s\")", op, key.c_str());
    }
}

void config_key_exists(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "exists", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    d.ret_bool(s.strings.count(key) != 0 || s.numbers.count(key) != 0);
}

void config_erase_key(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "erase", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings.erase(key);
    s.numbers.erase(key);
    store_save_locked(s);
    d.ret(0);
}

void config_read_string(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "read.str", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.strings.find(key);
    /* Absent stays null -- the Java getString(key, null) default. A key present
     * but empty returns the empty string, which is a different, real value. */
    if (it == s.strings.end()) d.ret(0);
    else d.ret_string(it->second);
}

void config_read_integer(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "read.int", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.numbers.find(key);
    d.ret(it == s.numbers.end() ? 0u : (std::uint32_t)it->second);
}

void config_read_boolean(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "read.bool", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.numbers.find(key);
    d.ret_bool(it != s.numbers.end() && it->second != 0);
}

void config_write_string(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "write.str", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings[key] = d.string_arg(1);
    s.numbers.erase(key); /* a key is one type at a time, like SharedPreferences */
    store_save_locked(s);
    d.ret_bool(true);
}

void config_write_integer(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "write.int", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.numbers[key] = (std::int32_t)d.arg(1);
    s.strings.erase(key);
    store_save_locked(s);
    d.ret_bool(true);
}

void config_write_boolean(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "write.bool", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.numbers[key] = d.arg(1) != 0 ? 1 : 0;
    s.strings.erase(key);
    store_save_locked(s);
    d.ret_bool(true);
}

/* --- device / orientation --- */

/* int Device_GetCurrentUIOrientation() -- 4 is landscape-right, which matches
 * the host window's aspect. */
void get_ui_orientation(DexCall &d) { d.ret(4); }
void is_supported_orientation(DexCall &d) { d.ret_bool(true); }

/* boolean Web_SysOpenURL(String) -- on Android this launches an external
 * VIEW intent; here we open the host's default browser. */
void open_url(DexCall &d) {
    const std::string url = d.string_arg(0);
    d.c.log("[dex] Web_SysOpenURL intercepted for Cloud Save: %s", url.c_str());
    config_screen_request_open_cloud();
    d.ret_bool(true);
}

/* --- identity strings ---
 *
 * These end up in save files, analytics payloads and the version display. */
void product_version(DexCall &d) { d.ret_string(pvz2_config()->product_version_string); }

/* Everything below is a [game] key in config.ini rather than a literal here --
 * see config.h's package_name/activity_name/device_name/hardware_model/
 * os_version/currency_symbol/currency_code/product_version_string. package_name
 * in particular is read by dex/hooks/purchase_driver.cpp too, so the two can
 * never disagree the way this hook and that file's own copy used to. */
void product_version_string(DexCall &d) { d.ret_string(pvz2_config()->product_version_string); }
void currency_symbol(DexCall &d) { d.ret_string(pvz2_config()->currency_symbol); }
/* Info_SysGetUserCurrencyCode -- the ISO 4217 code (e.g. "USD"). Unhooked it
 * returned null, and the in-app store treats a null currency as "store not
 * configured", which surfaces as the "Service Unavailable" purchase dialog. */
void currency_code(DexCall &d) { d.ret_string(pvz2_config()->currency_code); }
void package_name(DexCall &d) { d.ret_string(pvz2_config()->package_name); } /* matches the real .obb name */
void activity_name(DexCall &d) { d.ret_string(pvz2_config()->activity_name); }
/* Both come from [game] user_locale in config.ini (default "en_US"). The country
 * is the part after '_', upper-cased, so the two can never disagree -- a locale
 * of es_AR yields country AR without a second setting to keep in sync. */
void user_locale(DexCall &d) { d.ret_string(pvz2_config()->user_locale); }
void country_code(DexCall &d) {
    const std::string locale = pvz2_config()->user_locale;
    const std::size_t us = locale.find('_');
    std::string country = (us == std::string::npos) ? "US" : locale.substr(us + 1);
    for (char &c : country) c = (char)std::toupper((unsigned char)c);
    d.ret_string(country.empty() ? "US" : country);
}
void device_name(DexCall &d) { d.ret_string(pvz2_config()->device_name); }
void device_id(DexCall &d) { d.ret_string("UNKNOWN/EMULATOR"); } /* AndroidGameApp.java's own fallback */

/* String Util_GetUUIDString()
 *
 * Java answers with UUID.randomUUID().toString(). Unhooked it returned null,
 * and the engine builds std::strings out of these without checking. We persist
 * one generated value in config.ini so it stays stable for this installation
 * but differs across fresh configs / machines. */
void uuid_string(DexCall &d) { d.ret_string(pvz2_config()->player_uuid); }
void os_version(DexCall &d) { d.ret_string(pvz2_config()->os_version); }
void hardware_model(DexCall &d) { d.ret_string(pvz2_config()->hardware_model); }

/* --- storage locations ---
 *
 * Relative-path semantics matter more than the exact string: the engine
 * concatenates these with resource names and hands the result to fopen, which
 * vfs::translate() then resolves. */
void data_folder(DexCall &d) { d.ret_string("."); }

/* --- Crashlytics state taps (Discord Rich Presence) ------------------------
 *
 * The engine reports every level start ("Board Level"), world map change
 * ("WorldMap Current World"), and screen transition ("GameStateMgr State")
 * through com.crashlytics.android.Crashlytics via EZJNI. Tapping those calls
 * gives Discord Rich Presence the exact level number even for brand-new levels
 * loaded from the in-memory Packages RSG, and restores the previous screen
 * immediately when closing full-screen submenus like the Almanac or Store. */
constexpr const char *kCrashlyticsClass = "com/crashlytics/android/Crashlytics";

/* Reads Board::m_levelNumber (+184) from the guest LawnApp instance (+1668).
 * For Endless Zones (DangerRoom) and VaseBreaker Endless, m_levelNumber holds the
 * current stage/streak/level number (1, 2, 3...). */
std::int32_t read_board_level_number(DexCall &d) {
    if (!d.c.img || d.c.img->so_base == 0) return 0;
    const std::uint32_t got_slot = d.c.img->so_base + 0x110c734u;
    if (!d.c.in_bounds(got_slot, 4)) return 0;
    const std::uint32_t app_ptr_addr = d.c.read32(got_slot);
    if (app_ptr_addr == 0 || !d.c.in_bounds(app_ptr_addr, 4)) return 0;
    const std::uint32_t lawn_app = d.c.read32(app_ptr_addr);
    if (lawn_app == 0 || !d.c.in_bounds(lawn_app + 1668u, 4)) return 0;
    const std::uint32_t board = d.c.read32(lawn_app + 1668u);
    if (board == 0 || !d.c.in_bounds(board + 184u, 4)) return 0;
    const std::int32_t lvl = (std::int32_t)d.c.read32(board + 184u);
    return (lvl > 0 && lvl < 100000) ? lvl : 0;
}

void crashlytics_set_string(DexCall &d) {
    const std::string key = d.string_arg(0);
    const std::string val = d.string_arg(1);
    if (pvz2_presence_enabled() && !key.empty()) {
        /* Filter out high-frequency resource tracking strings from trace/queue */
        if (key != "Last Serialized Object Class" &&
            key != "Resgroup Preload Refcounts" &&
            key != "Resgroup Refcounts" &&
            key != "Last Force LoadGroups" &&
            key != "Last DeleteGroups") {
            const std::string kv = key + "=" + val;
            pvz2_presence_note("state", kv.c_str());
        }

        std::lock_guard<std::mutex> lk(g_crashlytics_mu);
        if (key == "Last PrepareLoadGroups") {
            if (val.find("UI_SeedChooser") != std::string::npos) {
                g_has_seed_chooser = true;
            }
            if (val.find("ProtectThePlantChallengeModule") != std::string::npos) {
                g_has_protect_plant = true;
                if (g_last_goals_count < 1) {
                    g_last_goals_count = 1;
                    pvz2_presence_note("state", "Board Goals=1");
                }
            }
        } else if (key == "Board Level" && !val.empty()) {
            g_last_board_level = val;
            const std::int32_t num = read_board_level_number(d);
            if (num > 0) {
                g_last_level_num = num;
                const std::string lvl_with_num = "Board Level=" + val + ":" + std::to_string(num);
                pvz2_presence_note("state", lvl_with_num.c_str());
            } else {
                g_last_level_num = 0;
            }
            int goals = count_level_challenges(val);
            if (goals == 0 && g_has_protect_plant) goals = 1;
            g_last_goals_count = goals;
            pvz2_presence_note("state", ("Board Goals=" + std::to_string(goals)).c_str());
        } else if (key == "WorldMap Current World" && !val.empty()) {
            g_last_world = val;
            g_has_seed_chooser = false;
            g_in_seed_chooser = false;
            g_has_protect_plant = false;
            g_last_goals_count = 0;
            pvz2_presence_note("state", "Board Chooser=false");
            pvz2_presence_note("state", "Board Paused=false");
            pvz2_presence_note("state", "Board Goals=0");
        } else if (key == "Board State") {
            if (val == "LoadingLevel") {
                g_has_seed_chooser = false;
                g_in_seed_chooser = false;
                g_has_protect_plant = false;
                pvz2_presence_note("state", "Board Chooser=false");
                pvz2_presence_note("state", "Board Paused=false");
                const int goals = count_level_challenges(g_last_board_level);
                g_last_goals_count = goals;
                pvz2_presence_note("state", ("Board Goals=" + std::to_string(goals)).c_str());
            } else if (val == "LevelIntro") {
                if (g_has_seed_chooser) {
                    g_in_seed_chooser = true;
                    pvz2_presence_note("state", "Board Chooser=true");
                }
            } else if (val == "PlayingLevel") {
                if (g_in_seed_chooser) {
                    g_in_seed_chooser = false;
                    pvz2_presence_note("state", "Board Chooser=false");
                }
            } else if (val == "PlayerWonOutro" || val == "PlayerLostOutro" || val == "EndLevel" || val == "Initial") {
                g_has_seed_chooser = false;
                g_in_seed_chooser = false;
                g_has_protect_plant = false;
                g_last_goals_count = 0;
                pvz2_presence_note("state", "Board Chooser=false");
                pvz2_presence_note("state", "Board Paused=false");
                pvz2_presence_note("state", "Board Goals=0");
            }

            if (!g_last_board_level.empty()) {
                int goals = count_level_challenges(g_last_board_level);
                if (goals == 0 && g_has_protect_plant) goals = 1;
                g_last_goals_count = goals;
                pvz2_presence_note("state", ("Board Goals=" + std::to_string(goals)).c_str());

                const std::int32_t num = read_board_level_number(d);
                if (num > 0) {
                    g_last_level_num = num;
                    const std::string lvl_with_num = "Board Level=" + g_last_board_level + ":" + std::to_string(num);
                    pvz2_presence_note("state", lvl_with_num.c_str());
                } else if (g_last_level_num > 0) {
                    const std::string lvl_with_num = "Board Level=" + g_last_board_level + ":" + std::to_string(g_last_level_num);
                    pvz2_presence_note("state", lvl_with_num.c_str());
                } else {
                    const std::string lvl = "Board Level=" + g_last_board_level;
                    pvz2_presence_note("state", lvl.c_str());
                }
            }
        } else if (key == "GameStateMgr State") {
            if (val == "GAME_Game" && !g_last_board_level.empty()) {
                int goals = count_level_challenges(g_last_board_level);
                if (goals == 0 && g_has_protect_plant) goals = 1;
                g_last_goals_count = goals;
                pvz2_presence_note("state", ("Board Goals=" + std::to_string(goals)).c_str());

                const std::int32_t num = read_board_level_number(d);
                if (num > 0) {
                    g_last_level_num = num;
                    const std::string lvl_with_num = "Board Level=" + g_last_board_level + ":" + std::to_string(num);
                    pvz2_presence_note("state", lvl_with_num.c_str());
                } else if (g_last_level_num > 0) {
                    const std::string lvl_with_num = "Board Level=" + g_last_board_level + ":" + std::to_string(g_last_level_num);
                    pvz2_presence_note("state", lvl_with_num.c_str());
                } else {
                    const std::string lvl = "Board Level=" + g_last_board_level;
                    pvz2_presence_note("state", lvl.c_str());
                }
            } else if (val == "GAME_WorldMap") {
                g_has_seed_chooser = false;
                g_in_seed_chooser = false;
                g_has_protect_plant = false;
                g_last_goals_count = 0;
                pvz2_presence_note("state", "Board Chooser=false");
                pvz2_presence_note("state", "Board Paused=false");
                pvz2_presence_note("state", "Board Goals=0");
                if (!g_last_world.empty()) {
                    const std::string w = "WorldMap Current World=" + g_last_world;
                    pvz2_presence_note("state", w.c_str());
                }
            } else {
                g_has_seed_chooser = false;
                g_in_seed_chooser = false;
                g_has_protect_plant = false;
                g_last_goals_count = 0;
                pvz2_presence_note("state", "Board Chooser=false");
                pvz2_presence_note("state", "Board Paused=false");
                pvz2_presence_note("state", "Board Goals=0");
            }
        }
    }
    d.ret(0);
}

void crashlytics_set_bool(DexCall &d) {
    const std::string key = d.string_arg(0);
    const std::uint32_t val = d.arg(1);
    if (pvz2_presence_enabled() && !key.empty()) {
        const std::string kv = key + "=" + (val ? "true" : "false");
        pvz2_presence_note("state", kv.c_str());
    }
    d.ret(0);
}

void crashlytics_log(DexCall &d) {
    const std::string msg = d.string_arg(0);
    if (pvz2_presence_enabled() && !msg.empty()) {
        pvz2_presence_note("log", msg.c_str());
    }
    d.ret(0);
}

}  // namespace

void config_store_set_string(const std::string &key, const std::string &value) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings[key] = value;
    s.numbers.erase(key);
    store_save_locked(s);
}

bool config_store_get_string(const std::string &key, std::string &out) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.strings.find(key);
    if (it == s.strings.end()) return false;
    out = it->second;
    return true;
}

bool config_store_get_int64(const std::string &key, std::int64_t &out) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.numbers.find(key);
    if (it == s.numbers.end()) return false;
    out = it->second;
    return true;
}

void config_store_reload(void) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings.clear();
    s.numbers.clear();
    store_load_locked(s);
}

void register_android_game_app(HookTable &t) {
    t.add(kClass, "UI_ProcessEvents", ui_process_events);

    t.add(kClass, "Device_ShowKeyboard", show_keyboard);
    t.add(kClass, "Device_ShowNumericKeyboard", show_numeric_keyboard);
    t.add(kClass, "Device_HideKeyboard", hide_keyboard);
    t.add(kClass, "Device_IsKeyboardShowing", is_keyboard_showing);

    t.add(kClass, "Resources_GetAssetFileInfo", get_asset_file_info);
    t.add(kClass, "Resources_GetAssetFileSize", get_asset_file_size);

    t.add(kClass, "Resources_GetFileSystemBlockCount", fs_block_count);
    t.add(kClass, "Resources_GetFileSystemBlocksFree", fs_blocks_free);
    t.add(kClass, "Resources_GetFileSystemBlockSize", fs_block_size);

    /* A real, session-persistent key/value store -- see the ConfigStore note.
     * These have to agree with each other (a write must be visible to the next
     * read and to KeyExists), which is the whole point and why they moved from
     * an accept-and-forget stub to actual storage. */
    t.add(kClass, "Config_ConfigKeyExists", config_key_exists);
    t.add(kClass, "Config_ConfigEraseKey", config_erase_key);
    t.add(kClass, "Config_ConfigReadString", config_read_string);
    t.add(kClass, "Config_ConfigReadInteger", config_read_integer);
    t.add(kClass, "Config_ConfigReadBoolean", config_read_boolean);
    t.add(kClass, "Config_ConfigWriteString", config_write_string);
    t.add(kClass, "Config_ConfigWriteInteger", config_write_integer);
    t.add(kClass, "Config_ConfigWriteBoolean", config_write_boolean);

    t.add(kClass, "Web_SysOpenURL", open_url);
    t.add(kClass, "Device_IsSupportedUIOrientation", is_supported_orientation);
    t.add(kClass, "Device_GetCurrentUIOrientation", get_ui_orientation);

    t.add(kClass, "Info_SysGetProductVersion", product_version);
    t.add(kClass, "Info_SysGetProductVersionString", product_version_string);
    t.add(kClass, "Info_SysGetUserCurrencySymbol", currency_symbol);
    t.add(kClass, "Info_SysGetUserCurrencyCode", currency_code);
    t.add(kClass, "Info_SysGetPackageName", package_name);
    t.add(kClass, "Info_SysGetActivityName", activity_name);
    t.add(kClass, "Info_SysGetUserLocale", user_locale);
    t.add(kClass, "Info_SysGetCountryCodeString", country_code);

    t.add(kClass, "Device_GetDeviceName", device_name);
    t.add(kClass, "Diag_GetDeviceID", device_id);
    t.add(kClass, "Util_GetUUIDString", uuid_string);
    t.add(kClass, "Diag_GetOSVersion", os_version);
    t.add(kClass, "Diag_GetHardwareModel", hardware_model);

    t.add(kClass, "Device_GetCachesDir", data_folder);
    t.add(kClass, "Resources_GetResourceFolder", data_folder);
    t.add(kClass, "Resources_GetUserDataFolder", data_folder);
    t.add(kClass, "Resources_GetCacheDataFolder", data_folder);
    t.add(kClass, "Resources_GetAppSupportDataFolder", data_folder);
    t.add(kClass, "Resources_GetExternalStorageDirectory", data_folder);

    t.add(kCrashlyticsClass, "setString", crashlytics_set_string);
    t.add(kCrashlyticsClass, "setBool", crashlytics_set_bool);
    t.add(kCrashlyticsClass, "log", crashlytics_log);

    /* Deliberately NOT hooked, because the unhooked default (0/false/null) is
     * already the correct answer for this port:
     *   Device_IsTablet -- not true here.
     *   Info_SysGetTimeBombDate -- this build has no expiry.
     *   Info_SysGetIntentExtraDataString -- the app was not launched from an
     *     intent with extras. */
}

}  // namespace dex
}  // namespace pvz2native

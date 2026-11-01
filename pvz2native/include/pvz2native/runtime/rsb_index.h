#ifndef PVZ2NATIVE_RUNTIME_RSB_INDEX_H
#define PVZ2NATIVE_RUNTIME_RSB_INDEX_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>

namespace pvz2native {

/* The scheme prefix SexyAppFramework stamps on every resource lookup. Two layers
 * strip it -- RsbIndex::normalize on the way to an index lookup, and
 * vfs::translate on the way to a host open -- and they each used to spell it out
 * with their own local copy and their own sizeof arithmetic. */
constexpr char kAssetScheme[] = "ASSET:";
constexpr std::size_t kAssetSchemeLen = sizeof(kAssetScheme) - 1;

/* Index of every file packed inside the game's main RSB (the .obb).
 *
 * PvZ2 reaches its resources through an "ASSET:<path>" scheme that, on a real
 * device, AndroidGameApp.Resources_GetAssetFileInfo answers by calling
 * AssetManager.openFd() and handing native code back
 * (APK path, startOffset, length) -- assets are stored uncompressed in the
 * zip, so native just opens the APK and reads at that offset. This APK ships
 * no assets/ folder at all, so that lookup can never succeed here; everything
 * lives in the .obb instead. Because non-image entries are stored raw inside
 * the RSB, we can answer the exact same contract with (obb path, offset,
 * length) and no extraction step.
 *
 * RSB v4 layout: 0x28 rsgNumber, 0x2C rsgInfoBegin, 0x30 rsgInfoEachLength.
 * Each RSG info record holds the package name at +0, its absolute offset at
 * +0x80 and its size at +0x84. Each RSG is a self-contained "pgsr" package
 * whose payload starts at header +0x14, with a name list at +0x4C of length
 * +0x48. Names in that list use the PopCap 4-byte-per-node trie (byte0 =
 * character, 0 terminates a name; bytes 1..3 = uint24 sibling-branch offset
 * in 4-byte units), and each terminator is followed by
 * [flag][offset][size], offset being relative to the payload start. flag == 1
 * marks a texture, which carries 20 extra bytes of image info and whose
 * payload is zlib-compressed -- those are indexed but must not be served
 * raw. */
class RsbIndex {
public:
    struct Entry {
        uint64_t offset = 0; /* absolute byte offset inside the .obb */
        uint32_t size = 0;
        bool compressed = false; /* texture payloads (flag == 1) are zlib streams */
    };

    /* Parses the whole index once. Returns false (and leaves the index empty)
     * if the file is missing or is not an RSB. */
    bool load(const std::string &obb_path);

    bool loaded() const { return loaded_; }
    size_t size() const { return entries_.size(); }
    const std::string &path() const { return obb_path_; }

    /* Looks up a guest resource path (any case, '/' or '\\' separators, with
     * or without the "ASSET:" prefix). Returns nullptr if absent. */
    const Entry *find(const std::string &guest_path) const;

    /* Which RSG ("resource group") the absolute .obb offset `pos` falls inside,
     * and its name as stored in the RSB (e.g. a world's background group), or
     * nullptr. Used by Discord Rich Presence: the engine loads whole groups by
     * reading the .obb directly, so the group being read says what the game is
     * about to show. */
    const std::string *group_at(uint64_t pos) const;

    /* The file whose payload STARTS exactly at `pos`, as stored (upper-case,
     * '\\' separators), or nullptr. */
    const std::string *file_starting_at(uint64_t pos) const;

    /* The exact normalisation `find` applies, exposed for logging: strips
     * "ASSET:", uppercases, and converts '/' to '\\' to match how names are
     * stored in the RSB. Mirrors AndroidAssetUtils.GetAssetNameFromFileName
     * except for its ".smf" suffixing, which only applies to APK assets. */
    static std::string normalize(const std::string &guest_path);

private:
    bool loaded_ = false;
    std::string obb_path_;
    std::unordered_map<std::string, Entry> entries_;

    struct Group {
        uint64_t offset;
        uint32_t size;
        std::string name;
    };
    std::vector<Group> groups_;                    /* sorted by offset */
    std::map<uint64_t, std::string> by_offset_;    /* payload start -> file name */
};

}  // namespace pvz2native

#endif

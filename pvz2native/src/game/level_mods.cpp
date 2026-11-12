/* See include/pvz2native/game/level_mods.h. */

#include <pvz2native/game/level_mods.h>

#include <pvz2native/rton_tree.h>

#include <initializer_list>
#include <string>

namespace pvz2native {
namespace {

/* The same numbers Dark Ages day 9 uses: four potions at the start, a new one every 4 to 8 seconds, at most
 * fifteen on the lawn. */
constexpr std::uint64_t kInitialPotions = 4;
constexpr std::uint64_t kMaxPotions = 15;
constexpr std::uint64_t kSpawnMinSeconds = 4;
constexpr std::uint64_t kSpawnMaxSeconds = 8;

/* The alias the potion module is defined under, the one every potion level uses. */
const char kPotionAlias[] = "ZombiePotions";

/* Everything written here uses plain (uncached) strings: a cached one (0x90) would shift the index of every
 * cached string after it, and the file refers back to those by index. For the same reason nothing is copied
 * byte for byte unless it cannot hold a cache definition. */
void put_varint(std::vector<std::uint8_t> &out, std::uint64_t v) {
    const std::vector<std::uint8_t> n = rton_encode_varint(v);
    out.insert(out.end(), n.begin(), n.end());
}

void put_str(std::vector<std::uint8_t> &out, const std::string &s) {
    out.push_back(0x81);
    put_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

void put_uint(std::vector<std::uint8_t> &out, std::uint64_t v) {
    const std::vector<std::uint8_t> b = rton_encode_int(0x24, v);
    out.insert(out.end(), b.begin(), b.end());
}

void put_str_array(std::vector<std::uint8_t> &out, std::initializer_list<const char *> items) {
    out.push_back(0x86);
    out.push_back(0xFD);
    put_varint(out, items.size());
    for (const char *s : items) put_str(out, s);
    out.push_back(0xFE);
}

/* RTID(<alias>@<sheet>): 0x83 0x03, then the sheet and the alias, each as <chars> <bytes> <utf-8>. */
void put_rtid(std::vector<std::uint8_t> &out, const std::string &alias, const std::string &sheet) {
    out.push_back(0x83);
    out.push_back(0x03);
    for (const std::string *s : {&sheet, &alias}) {
        put_varint(out, s->size());
        put_varint(out, s->size());
        out.insert(out.end(), s->begin(), s->end());
    }
}

std::vector<std::uint8_t> potion_module_object() {
    std::vector<std::uint8_t> o;
    o.push_back(0x85);
    put_str(o, "aliases");
    put_str_array(o, {kPotionAlias});
    put_str(o, "objclass");
    put_str(o, "ZombiePotionModuleProperties");
    put_str(o, "objdata");
    o.push_back(0x85);
    put_str(o, "InitialPotionCount");
    put_uint(o, kInitialPotions);
    put_str(o, "MaxPotionCount");
    put_uint(o, kMaxPotions);
    put_str(o, "PotionSpawnTimer");
    o.push_back(0x85);
    put_str(o, "Max");
    put_uint(o, kSpawnMaxSeconds);
    put_str(o, "Min");
    put_uint(o, kSpawnMinSeconds);
    o.push_back(0xFF);
    put_str(o, "PotionTypes");
    put_str_array(o, {"zombiepotion_speed", "zombiepotion_toughness"});
    o.push_back(0xFF);
    o.push_back(0xFF);
    return o;
}

/* A wave entry `{"Type": RTID(...), "Row": ...}` without its Row. Empty when the entry is not that shape.
 * The Type is an RTID, whose strings are never cached, so its bytes can be copied as they are. */
std::vector<std::uint8_t> unpinned_copy(const std::vector<std::uint8_t> &file, const RtonNode &entry) {
    const RtonNode *type = entry.get("Type");
    if (entry.kind != RtonNode::Obj || type == nullptr || type->tag != 0x83) return {};
    std::vector<std::uint8_t> o;
    o.push_back(0x85);
    put_str(o, "Type");
    o.insert(o.end(), file.begin() + (std::ptrdiff_t)type->begin, file.begin() + (std::ptrdiff_t)type->end);
    o.push_back(0xFF);
    return o;
}

bool untouchable_class(const std::string &objclass) {
    return objclass.find("VaseBreaker") != std::string::npos || objclass.find("Beghouled") != std::string::npos ||
           objclass.find("Tutorial") != std::string::npos;
}

/* Appends elements before an array's closing 0xFE and rewrites its element count. */
void append_to_array(std::vector<RtonEdit> &edits, const RtonNode &arr, std::size_t added,
                     std::vector<std::uint8_t> elements) {
    edits.push_back({arr.count_begin, arr.count_end, rton_encode_varint(arr.a.size() + added)});
    edits.push_back({arr.end - 1, arr.end - 1, std::move(elements)});
}

}  // namespace

std::vector<std::uint8_t> level_with_mods(const std::vector<std::uint8_t> &level, const LevelMods &mods) {
    if (!mods.potions && mods.horde_multiplier <= 1) return {};
    RtonNode root;
    if (!rton_parse(level, root)) return {};
    const RtonNode *objects = root.get("objects");
    if (objects == nullptr || objects->kind != RtonNode::Arr) return {};

    const RtonNode *modules = nullptr;
    bool has_potions = false;
    std::vector<const RtonNode *> wave_lists;
    for (const RtonNode &obj : objects->a) {
        const RtonNode *cls = obj.get("objclass");
        if (cls == nullptr || cls->kind != RtonNode::Str) continue;
        if (untouchable_class(cls->s)) return {};
        const RtonNode *data = obj.get("objdata");
        if (data == nullptr) continue;
        if (cls->s == "LevelDefinition") modules = data->get("Modules");
        if (cls->s == "ZombiePotionModuleProperties") has_potions = true;
        if (const RtonNode *alias = obj.get("aliases"))
            for (const RtonNode &a : alias->a)
                if (a.kind == RtonNode::Str && a.s == kPotionAlias) has_potions = true;
        if (cls->s == "SpawnZombiesJitteredWaveActionProps") {
            const RtonNode *zombies = data->get("Zombies");
            if (zombies != nullptr && zombies->kind == RtonNode::Arr && !zombies->a.empty())
                wave_lists.push_back(zombies);
        }
    }
    if (modules == nullptr || modules->kind != RtonNode::Arr) return {};

    std::vector<RtonEdit> edits;
    if (mods.potions && !has_potions) {
        std::vector<std::uint8_t> ref;
        put_rtid(ref, kPotionAlias, "CurrentLevel");
        append_to_array(edits, *modules, 1, std::move(ref));
        append_to_array(edits, *objects, 1, potion_module_object());
    }
    const int extra = mods.horde_multiplier - 1;
    for (const RtonNode *zombies : extra > 0 ? wave_lists : std::vector<const RtonNode *>{}) {
        std::vector<std::uint8_t> copies;
        std::size_t count = 0;
        for (const RtonNode &entry : zombies->a) {
            const std::vector<std::uint8_t> one = unpinned_copy(level, entry);
            if (one.empty()) continue;
            for (int k = 0; k < extra; ++k) copies.insert(copies.end(), one.begin(), one.end());
            count += (std::size_t)extra;
        }
        if (count != 0) append_to_array(edits, *zombies, count, std::move(copies));
    }
    if (edits.empty()) return {};
    return rton_apply_edits(level, std::move(edits));
}

}  // namespace pvz2native

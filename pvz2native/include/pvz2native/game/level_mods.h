#ifndef PVZ2NATIVE_GAME_LEVEL_MODS_H
#define PVZ2NATIVE_GAME_LEVEL_MODS_H

#include <cstdint>
#include <vector>

/* What the harder difficulties change in a level, done to the level file itself as the engine reads it, so
 * the game runs its own systems on the result instead of us imitating them at runtime.
 *
 * - Potions (Point Zero): the ZombiePotionModule that Dark Ages day 9 and some Levels of the Day use, where
 *   potions sit on lawn tiles and a zombie that walks over one drinks it. Added with the same object and the
 *   same Modules entry those levels carry.
 * - Hordes (Hard++ / Point Zero): every zombie in a scripted wave gets extra copies in that wave. Most wave
 *   entries pin their zombie to a row; the copies leave the row out, so the game picks one for each of them
 *   and the extra zombies spread over the lawn instead of piling up behind the original. */
namespace pvz2native {

struct LevelMods {
    bool potions = false;
    int horde_multiplier = 1; /* each wave zombie appears this many times; 1 leaves waves alone */
};

/* `level` with `mods` applied, or empty when the file is not a level or nothing applies to it. Vasebreaker,
 * Beghouled and tutorial levels are left alone, as are potions in a level that already has them. */
std::vector<std::uint8_t> level_with_mods(const std::vector<std::uint8_t> &level, const LevelMods &mods);

}  // namespace pvz2native

#endif

/* Battlez avatar atlases of the 6.6.1 OBB (groups UI_Avatars_384/768/1536, atlases UI_Avatars_<res>_00),
 * taken from the OBB's resource manifest and atlas data: the size of each atlas, an FNV-1a hash of the
 * ETC1 blocks and of the alpha under IMAGE_UI_JOUST_AVATARS_AVATAR_1 (to recognise the upload, the way
 * gfx/cloud_icon.cpp recognises the main menu atlas), and the rectangle of AVATAR_1..25. Generated
 * offline; do not edit by hand. */
#ifndef PVZ2NATIVE_GFX_JOUST_AVATAR_ATLAS_H
#define PVZ2NATIVE_GFX_JOUST_AVATAR_ATLAS_H

#include <cstdint>

namespace pvz2native {
namespace joust_avatar_atlas {

struct Rect {
    int x, y, w, h;
};

struct Atlas {
    int dim;
    std::uint64_t etc_hash;
    std::uint64_t alpha_hash;
    Rect avatar[25]; /* AVATAR_1 .. AVATAR_25 */
};

constexpr Atlas kAtlases[] = {
    /* 384 */
    {512, 0x405c7659c462cc41ull, 0x01276c514a425a22ull,
     {{71, 1, 64, 64}, {401, 67, 64, 64}, {397, 133, 64, 64}, {1, 199, 64, 64}, {67, 199, 64, 64}, {133, 199, 64, 64}, {199, 199, 64, 64}, {265, 199, 64, 64}, {331, 199, 64, 64}, {137, 1, 64, 64}, {203, 1, 64, 64}, {269, 1, 64, 64}, {335, 1, 64, 64}, {401, 1, 64, 64}, {71, 67, 64, 64}, {137, 67, 64, 64}, {203, 67, 64, 64}, {269, 67, 64, 64}, {335, 67, 64, 64}, {1, 71, 64, 64}, {67, 133, 64, 64}, {133, 133, 64, 64}, {199, 133, 64, 64}, {265, 133, 64, 64}, {331, 133, 64, 64}}},
    /* 768 */
    {1024, 0x424ba68b49fb4415ull, 0x1f27c1ec15572c69ull,
     {{139, 1, 128, 128}, {789, 131, 128, 128}, {781, 261, 128, 128}, {1, 391, 128, 128}, {131, 391, 128, 128}, {261, 391, 128, 128}, {391, 391, 128, 128}, {521, 391, 128, 128}, {651, 391, 128, 128}, {269, 1, 128, 128}, {399, 1, 128, 128}, {529, 1, 128, 128}, {659, 1, 128, 128}, {789, 1, 128, 128}, {139, 131, 128, 128}, {269, 131, 128, 128}, {399, 131, 128, 128}, {529, 131, 128, 128}, {659, 131, 128, 128}, {1, 139, 128, 128}, {131, 261, 128, 128}, {261, 261, 128, 128}, {391, 261, 128, 128}, {521, 261, 128, 128}, {651, 261, 128, 128}}},
    /* 1536 */
    {2048, 0xcbec88f68a59ca5cull, 0xcf7148aa33caf8e9ull,
     {{275, 1, 256, 256}, {1565, 259, 256, 256}, {1549, 517, 256, 256}, {1, 775, 256, 256}, {259, 775, 256, 256}, {517, 775, 256, 256}, {775, 775, 256, 256}, {1033, 775, 256, 256}, {1291, 775, 256, 256}, {533, 1, 256, 256}, {791, 1, 256, 256}, {1049, 1, 256, 256}, {1307, 1, 256, 256}, {1565, 1, 256, 256}, {275, 259, 256, 256}, {533, 259, 256, 256}, {791, 259, 256, 256}, {1049, 259, 256, 256}, {1307, 259, 256, 256}, {1, 275, 256, 256}, {259, 517, 256, 256}, {517, 517, 256, 256}, {775, 517, 256, 256}, {1033, 517, 256, 256}, {1291, 517, 256, 256}}},
};

}  // namespace joust_avatar_atlas
}  // namespace pvz2native

#endif

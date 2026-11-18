#ifndef PVZ2NATIVE_GFX_JOUST_AVATARS_H
#define PVZ2NATIVE_GFX_JOUST_AVATARS_H

#include <cstdint>
#include <string>

/* Discord profile pictures as Battlez avatars.
 *
 * The game shows an avatar from a NUMBER: icon k (0..24; anything else is zeroed while a response is read)
 * is IMAGE_UI_JOUST_AVATARS_AVATAR_<k+1>, a 64/128/256 px square of the atlas UI_Avatars_<384|768|1536>_00.
 * The atlas reaches GL as ETC1 colour plus a GL_ALPHA image; both uploads are recognised by a hash of their
 * content (gfx/joust_avatar_atlas.h), the colour one is decoded to RGB8 instead of staying compressed, and
 * the two texture names are remembered.
 *
 * Icons 15..24 belong to Discord players: the server hands them out per response and says whose picture
 * each one shows ({"avatars": {"<icon>": "<URL>"}}). The picture -- from Discord's CDN, cached in
 * userdata/joust_avatars/ -- is painted into that square with glTexSubImage2D between frames. Icons 0..14
 * stay the game's own, for the AI opponents. */

namespace pvz2native {
namespace joust_avatars {

constexpr int kFirstDiscordIcon = 15;

/* GL side (libgles.cpp). `match_*` say whether an upload is an avatar atlas (index into the atlas table,
 * or -1); `uploaded` records the texture name that upload went to. */
int match_color(const void *etc1, int image_size, int width, int height);
int match_alpha(const void *alpha, int width, int height);
void uploaded(int atlas, bool alpha, std::uint32_t texture);
void deleted(std::uint32_t texture);

/* Paints pictures that finished downloading into the atlases. GL thread, between frames. */
void tick_gl();

/* From the server: icon `icon` (15..24) shows the picture at `url` (Discord's CDN only). */
void set_picture(int icon, const std::string &url);

}  // namespace joust_avatars
}  // namespace pvz2native

#endif

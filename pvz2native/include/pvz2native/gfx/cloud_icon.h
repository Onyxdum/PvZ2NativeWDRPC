#ifndef PVZ2NATIVE_GFX_CLOUD_ICON_H
#define PVZ2NATIVE_GFX_CLOUD_ICON_H

#include <cstdint>

/* Swaps the PopCap "P" on the main menu for the cloud icon newer PvZ2 builds use.
 *
 * The main menu atlas reaches GL as two uploads: the colour as ETC1 and the alpha
 * as a plain GL_ALPHA image of the same size. Both hooks get the complete image
 * the engine is about to upload, check a hash of the area under the "P" against
 * the stock atlas, and only on an exact match hand back a patched copy. The
 * engine's own buffer is never written to, and any other texture -- or a modded
 * atlas -- goes through untouched. */

namespace pvz2native {

/* ETC1 colour data (imageSize bytes). Returns either `data` or a patched copy. */
const void *cloud_icon_filter_etc1(const void *data, int image_size, int width, int height);

/* GL_ALPHA / GL_UNSIGNED_BYTE pixels. Returns either `data` or a patched copy. */
const void *cloud_icon_filter_alpha(const void *data, int width, int height);

}  // namespace pvz2native

#endif

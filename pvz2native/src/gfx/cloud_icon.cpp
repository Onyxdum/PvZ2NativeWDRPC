#include <pvz2native/gfx/cloud_icon.h>
#include <pvz2native/gfx/cloud_icon_data.h>
#include <pvz2native/config.h>
#include <pvz2native/log.h>

#include <cstring>
#include <vector>

namespace pvz2native {
namespace {

std::uint64_t fnv1a(std::uint64_t h, const std::uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

constexpr std::uint64_t kFnvSeed = 0xcbf29ce484222325ull;

/* Diff-mode block, base colour 31/31/31 with no delta and table 0: every pixel
 * decodes to 255 (or 253 at worst), so the alpha mask alone shapes the icon. */
const std::uint8_t kWhiteBlock[8] = {0xF8, 0xF8, 0xF8, 0x02, 0x00, 0x00, 0x00, 0x00};

/* Where the "P" sprite (IMAGE_UI_MAINMENU_MM_POPCAP) sits in one UI_MainMenu
 * atlas. The sprite itself is the same in 4.5.2 and 6.6.1 (same alpha hash),
 * only packed at another place; the ETC hash covers the 4x4 blocks touching it. */
struct Placement {
    const char *name;
    int dim, x, y, w, h;
    std::uint64_t etc_hash, alpha_hash;
    const std::uint8_t *cloud; /* w*h alpha */
};

const Placement kPlacements[] = {
    {"4.5.2 1536", CloudIcon1536::kDim, CloudIcon1536::kX, CloudIcon1536::kY, CloudIcon1536::kW,
     CloudIcon1536::kH, CloudIcon1536::kEtcHash, CloudIcon1536::kAlphaHash, kCloudAlpha1536},
    {"4.5.2 768", CloudIcon768::kDim, CloudIcon768::kX, CloudIcon768::kY, CloudIcon768::kW,
     CloudIcon768::kH, CloudIcon768::kEtcHash, CloudIcon768::kAlphaHash, kCloudAlpha768},
    /* 6.6.1: ax/ay from the resource manifest (1536) and found by the alpha hash (768). */
    {"6.6.1 1536", CloudIcon1536::kDim, 1593, 968, CloudIcon1536::kW, CloudIcon1536::kH,
     0x66fc37a9373ea3c0ull, CloudIcon1536::kAlphaHash, kCloudAlpha1536},
    {"6.6.1 768", CloudIcon768::kDim, 799, 490, CloudIcon768::kW, CloudIcon768::kH,
     0xb4018e9d66649d99ull, CloudIcon768::kAlphaHash, kCloudAlpha768},
};

bool etc_matches(const Placement &p, const std::uint8_t *src) {
    const int bw = p.dim / 4;
    const int bx0 = p.x / 4, bx1 = (p.x + p.w + 3) / 4;
    const int by0 = p.y / 4, by1 = (p.y + p.h + 3) / 4;
    std::uint64_t h = kFnvSeed;
    for (int by = by0; by < by1; ++by)
        h = fnv1a(h, src + (std::size_t)(by * bw + bx0) * 8, (std::size_t)(bx1 - bx0) * 8);
    return h == p.etc_hash;
}

/* Only the blocks wholly inside the sprite: an edge block can hold a
 * neighbouring sprite's pixels (6.6.1 768), and the cloud's edges are clear. */
void etc_patch(const Placement &p, std::uint8_t *dst) {
    const int bw = p.dim / 4;
    const int bx0 = (p.x + 3) / 4, bx1 = (p.x + p.w) / 4;
    const int by0 = (p.y + 3) / 4, by1 = (p.y + p.h) / 4;
    for (int by = by0; by < by1; ++by)
        for (int bx = bx0; bx < bx1; ++bx)
            std::memcpy(dst + (std::size_t)(by * bw + bx) * 8, kWhiteBlock, 8);
}

bool alpha_matches(const Placement &p, const std::uint8_t *src) {
    std::uint64_t h = kFnvSeed;
    for (int y = p.y; y < p.y + p.h; ++y)
        h = fnv1a(h, src + (std::size_t)y * p.dim + p.x, p.w);
    return h == p.alpha_hash;
}

void alpha_patch(const Placement &p, std::uint8_t *dst) {
    for (int y = 0; y < p.h; ++y)
        std::memcpy(dst + (std::size_t)(p.y + y) * p.dim + p.x, p.cloud + (std::size_t)y * p.w, p.w);
}

bool enabled() {
    const pvz2_config_t *cfg = pvz2_config();
    return cfg == nullptr || cfg->cloud_icon != 0;
}

}  // namespace

const void *cloud_icon_filter_etc1(const void *data, int image_size, int width, int height) {
    if (data == nullptr || width != height || !enabled()) return data;
    const auto *src = static_cast<const std::uint8_t *>(data);
    static thread_local std::vector<std::uint8_t> copy;

    for (const Placement &p : kPlacements) {
        if (width != p.dim || image_size < width * height / 2 || !etc_matches(p, src)) continue;
        copy.assign(src, src + image_size);
        etc_patch(p, copy.data());
        pvz2_log("[cloud-icon] main menu colour atlas (%s) patched", p.name);
        return copy.data();
    }
    return data;
}

const void *cloud_icon_filter_alpha(const void *data, int width, int height) {
    if (data == nullptr || width != height || !enabled()) return data;
    const auto *src = static_cast<const std::uint8_t *>(data);
    const std::size_t size = (std::size_t)width * height;
    static thread_local std::vector<std::uint8_t> copy;

    for (const Placement &p : kPlacements) {
        if (width != p.dim || !alpha_matches(p, src)) continue;
        copy.assign(src, src + size);
        alpha_patch(p, copy.data());
        pvz2_log("[cloud-icon] main menu alpha atlas (%s) patched", p.name);
        return copy.data();
    }
    return data;
}

}  // namespace pvz2native

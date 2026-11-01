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

template <typename Icon>
bool etc_matches(const std::uint8_t *src) {
    constexpr int bw = Icon::kDim / 4;
    constexpr int bx0 = Icon::kX / 4, bx1 = (Icon::kX + Icon::kW + 3) / 4;
    constexpr int by0 = Icon::kY / 4, by1 = (Icon::kY + Icon::kH + 3) / 4;
    std::uint64_t h = kFnvSeed;
    for (int by = by0; by < by1; ++by)
        h = fnv1a(h, src + (std::size_t)(by * bw + bx0) * 8, (std::size_t)(bx1 - bx0) * 8);
    return h == Icon::kEtcHash;
}

template <typename Icon>
void etc_patch(std::uint8_t *dst) {
    constexpr int bw = Icon::kDim / 4;
    constexpr int bx0 = Icon::kX / 4, bx1 = (Icon::kX + Icon::kW + 3) / 4;
    constexpr int by0 = Icon::kY / 4, by1 = (Icon::kY + Icon::kH + 3) / 4;
    for (int by = by0; by < by1; ++by)
        for (int bx = bx0; bx < bx1; ++bx)
            std::memcpy(dst + (std::size_t)(by * bw + bx) * 8, kWhiteBlock, 8);
}

template <typename Icon>
bool alpha_matches(const std::uint8_t *src) {
    std::uint64_t h = kFnvSeed;
    for (int y = Icon::kY; y < Icon::kY + Icon::kH; ++y)
        h = fnv1a(h, src + (std::size_t)y * Icon::kDim + Icon::kX, Icon::kW);
    return h == Icon::kAlphaHash;
}

template <typename Icon>
void alpha_patch(std::uint8_t *dst, const std::uint8_t *cloud) {
    for (int y = 0; y < Icon::kH; ++y)
        std::memcpy(dst + (std::size_t)(Icon::kY + y) * Icon::kDim + Icon::kX,
                    cloud + (std::size_t)y * Icon::kW, Icon::kW);
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

    if (width == CloudIcon1536::kDim && image_size >= width * height / 2 &&
        etc_matches<CloudIcon1536>(src)) {
        copy.assign(src, src + image_size);
        etc_patch<CloudIcon1536>(copy.data());
        pvz2_log("[cloud-icon] main menu colour atlas (1536) patched");
        return copy.data();
    }
    if (width == CloudIcon768::kDim && image_size >= width * height / 2 &&
        etc_matches<CloudIcon768>(src)) {
        copy.assign(src, src + image_size);
        etc_patch<CloudIcon768>(copy.data());
        pvz2_log("[cloud-icon] main menu colour atlas (768) patched");
        return copy.data();
    }
    return data;
}

const void *cloud_icon_filter_alpha(const void *data, int width, int height) {
    if (data == nullptr || width != height || !enabled()) return data;
    const auto *src = static_cast<const std::uint8_t *>(data);
    const std::size_t size = (std::size_t)width * height;
    static thread_local std::vector<std::uint8_t> copy;

    if (width == CloudIcon1536::kDim && alpha_matches<CloudIcon1536>(src)) {
        copy.assign(src, src + size);
        alpha_patch<CloudIcon1536>(copy.data(), kCloudAlpha1536);
        pvz2_log("[cloud-icon] main menu alpha atlas (1536) patched");
        return copy.data();
    }
    if (width == CloudIcon768::kDim && alpha_matches<CloudIcon768>(src)) {
        copy.assign(src, src + size);
        alpha_patch<CloudIcon768>(copy.data(), kCloudAlpha768);
        pvz2_log("[cloud-icon] main menu alpha atlas (768) patched");
        return copy.data();
    }
    return data;
}

}  // namespace pvz2native

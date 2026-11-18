#include <pvz2native/gfx/joust_avatars.h>

#include "joust_avatar_atlas.h"

#include <pvz2native/host_files.h>
#include <pvz2native/log.h>

#include <glad/gl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* A private copy (main.c builds stb_image only on Linux, for the cursor): Discord serves PNG, JPEG
 * covers the rest. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace pvz2native {
namespace joust_avatars {
namespace {

using joust_avatar_atlas::kAtlases;
constexpr int kAtlasCount = (int)(sizeof(kAtlases) / sizeof(kAtlases[0]));
constexpr int kIcons = 25;

std::uint64_t fnv1a(std::uint64_t h, const std::uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
constexpr std::uint64_t kFnvSeed = 0xcbf29ce484222325ull;

struct Picture {
    bool loading = false;
    bool ready = false;
    int side = 0;
    std::vector<std::uint8_t> rgba; /* decoded, square-cropped */
};

std::mutex g_lock;
std::uint32_t g_color_tex[kAtlasCount] = {};
std::uint32_t g_alpha_tex[kAtlasCount] = {};
std::map<std::string, Picture> g_pictures; /* URL -> picture */
std::string g_icon_url[kIcons];            /* icon -> picture painted there ("" = the game's own) */
bool g_dirty[kAtlasCount][kIcons] = {};

void mark_icon_dirty(int icon) {
    for (int a = 0; a < kAtlasCount; ++a) g_dirty[a][icon] = true;
}

#if defined(_WIN32)
int run_hidden(const std::string &cmd) {
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::string cmdline = "cmd.exe /C \"" + cmd + "\"";
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
#else
int run_hidden(const std::string &cmd) { return std::system((cmd + " >/dev/null 2>&1").c_str()); }
#endif

/* Only Discord's own CDN, and nothing a shell could read as syntax. */
bool url_allowed(const std::string &url) {
    if (url.rfind("https://cdn.discordapp.com/", 0) != 0) return false;
    for (char ch : url)
        if (!(std::isalnum((unsigned char)ch) || std::strchr(":/._-?=&", ch))) return false;
    return true;
}

std::string cache_name(const std::string &url) {
    const std::uint64_t h = fnv1a(kFnvSeed, (const std::uint8_t *)url.data(), url.size());
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.img", (unsigned long long)h);
    return std::string("userdata/joust_avatars/") + buf;
}

/* Worker: the picture from the cache or Discord's CDN, decoded and cropped to a square. */
void load_picture(std::string url) {
    const std::string file = cache_name(url);
    std::vector<std::uint8_t> bytes = host_files::read(file);
    if (bytes.empty()) {
        std::error_code ec;
        std::filesystem::create_directories("userdata/joust_avatars", ec);
        run_hidden("curl -sS -f -m 15 -o \"" + file + "\" \"" + url + "\"");
        bytes = host_files::read(file);
    }
    int w = 0, h = 0, n = 0;
    unsigned char *px = bytes.empty() ? nullptr
                                      : stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    std::lock_guard<std::mutex> lk(g_lock);
    Picture &p = g_pictures[url];
    p.loading = false;
    if (px == nullptr || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        pvz2_log("[joust] avatar picture could not be loaded: %s", url.c_str());
        return;
    }
    const int side = std::min(w, h), ox = (w - side) / 2, oy = (h - side) / 2;
    p.rgba.resize((std::size_t)side * side * 4);
    for (int y = 0; y < side; ++y)
        std::memcpy(&p.rgba[(std::size_t)y * side * 4], px + ((std::size_t)(y + oy) * w + ox) * 4,
                    (std::size_t)side * 4);
    stbi_image_free(px);
    p.side = side;
    p.ready = true;
    for (int i = 0; i < kIcons; ++i)
        if (g_icon_url[i] == url) mark_icon_dirty(i);
    pvz2_log("[joust] avatar picture ready (%dx%d)", side, side);
}

/* The picture scaled to w x h (bilinear), with rounded corners in the alpha. */
void scale_picture(const Picture &p, int w, int h, std::vector<std::uint8_t> &rgb, std::vector<std::uint8_t> &alpha) {
    rgb.resize((std::size_t)w * h * 3);
    alpha.resize((std::size_t)w * h);
    const float radius = 0.16f * (float)w;
    const int s = p.side;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float sx = ((float)x + 0.5f) * (float)s / (float)w - 0.5f;
            const float sy = ((float)y + 0.5f) * (float)s / (float)h - 0.5f;
            const int x0 = std::clamp((int)std::floor(sx), 0, s - 1), y0 = std::clamp((int)std::floor(sy), 0, s - 1);
            const int x1 = std::min(x0 + 1, s - 1), y1 = std::min(y0 + 1, s - 1);
            const float fx = std::clamp(sx - (float)x0, 0.f, 1.f), fy = std::clamp(sy - (float)y0, 0.f, 1.f);
            float a = 0;
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](int xx, int yy) { return (float)p.rgba[((std::size_t)yy * s + xx) * 4 + c]; };
                const float v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) +
                                (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                if (c < 3) rgb[((std::size_t)y * w + x) * 3 + c] = (std::uint8_t)std::lround(v);
                else a = v;
            }
            /* Rounded corners: distance outside the inner rectangle, one pixel of smoothing. */
            const float dx = std::max({radius - (float)x - 0.5f, (float)x + 0.5f - ((float)w - radius), 0.f});
            const float dy = std::max({radius - (float)y - 0.5f, (float)y + 0.5f - ((float)h - radius), 0.f});
            const float edge = std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.f, 1.f);
            alpha[(std::size_t)y * w + x] = (std::uint8_t)std::lround(a * edge);
        }
    }
}

}  // namespace

int match_color(const void *etc1, int image_size, int width, int height) {
    if (etc1 == nullptr || width != height) return -1;
    const auto *src = static_cast<const std::uint8_t *>(etc1);
    for (int a = 0; a < kAtlasCount; ++a) {
        const auto &at = kAtlases[a];
        if (width != at.dim || image_size < at.dim * at.dim / 2) continue;
        const auto &r = at.avatar[0];
        const int bw = at.dim / 4, bx0 = r.x / 4, bx1 = (r.x + r.w + 3) / 4, by0 = r.y / 4, by1 = (r.y + r.h + 3) / 4;
        std::uint64_t h = kFnvSeed;
        for (int by = by0; by < by1; ++by) h = fnv1a(h, src + (std::size_t)(by * bw + bx0) * 8, (std::size_t)(bx1 - bx0) * 8);
        if (h == at.etc_hash) return a;
    }
    return -1;
}

int match_alpha(const void *alpha, int width, int height) {
    if (alpha == nullptr || width != height) return -1;
    const auto *src = static_cast<const std::uint8_t *>(alpha);
    for (int a = 0; a < kAtlasCount; ++a) {
        const auto &at = kAtlases[a];
        if (width != at.dim) continue;
        const auto &r = at.avatar[0];
        std::uint64_t h = kFnvSeed;
        for (int y = r.y; y < r.y + r.h; ++y) h = fnv1a(h, src + (std::size_t)y * at.dim + r.x, (std::size_t)r.w);
        if (h == at.alpha_hash) return a;
    }
    return -1;
}

void uploaded(int atlas, bool alpha, std::uint32_t texture) {
    if (atlas < 0 || atlas >= kAtlasCount) return;
    std::lock_guard<std::mutex> lk(g_lock);
    (alpha ? g_alpha_tex : g_color_tex)[atlas] = texture;
    for (int i = 0; i < kIcons; ++i) g_dirty[atlas][i] = !g_icon_url[i].empty();
    pvz2_log("[joust] avatar atlas %d (%dpx) %s -> texture %u", atlas, kAtlases[atlas].dim, alpha ? "alpha" : "colour",
             texture);
}

void deleted(std::uint32_t texture) {
    if (texture == 0) return;
    std::lock_guard<std::mutex> lk(g_lock);
    for (int a = 0; a < kAtlasCount; ++a) {
        if (g_color_tex[a] == texture) g_color_tex[a] = 0;
        if (g_alpha_tex[a] == texture) g_alpha_tex[a] = 0;
    }
}

void tick_gl() {
    struct Job {
        int atlas, icon;
        Picture pic;
    };
    std::vector<Job> jobs;
    std::uint32_t color[kAtlasCount], alpha[kAtlasCount];
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (int a = 0; a < kAtlasCount; ++a) {
            color[a] = g_color_tex[a];
            alpha[a] = g_alpha_tex[a];
            if (color[a] == 0 || alpha[a] == 0) continue;
            for (int i = kFirstDiscordIcon; i < kIcons; ++i) {
                if (!g_dirty[a][i] || g_icon_url[i].empty()) continue;
                auto it = g_pictures.find(g_icon_url[i]);
                if (it == g_pictures.end() || !it->second.ready) continue;
                g_dirty[a][i] = false;
                jobs.push_back({a, i, it->second});
            }
        }
    }
    if (jobs.empty()) return;

    GLint prev_tex = 0, prev_align = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    std::vector<std::uint8_t> rgb, a8;
    for (const Job &j : jobs) {
        const auto &r = kAtlases[j.atlas].avatar[j.icon]; /* icon k shows AVATAR_k+1 */
        scale_picture(j.pic, r.w, r.h, rgb, a8);
        glBindTexture(GL_TEXTURE_2D, color[j.atlas]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        glBindTexture(GL_TEXTURE_2D, alpha[j.atlas]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_ALPHA, GL_UNSIGNED_BYTE, a8.data());
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) pvz2_log("[joust] painting avatars: GL error 0x%04x", (unsigned)err);
}

void set_picture(int icon, const std::string &url) {
    if (icon < kFirstDiscordIcon || icon >= kIcons || !url_allowed(url)) return;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_icon_url[icon] == url) return;
    g_icon_url[icon] = url;
    mark_icon_dirty(icon);
    Picture &p = g_pictures[url];
    if (!p.ready && !p.loading) {
        p.loading = true;
        std::thread(load_picture, url).detach();
    }
}

}  // namespace joust_avatars
}  // namespace pvz2native

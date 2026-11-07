/* credits_screen.cpp - Interactive PopCap-style scrolling credits roll overlay
 * using official extracted PROPERTIES/CREDITS.TXT and localized titles.
 */

#include <pvz2native/credits_screen.h>
#include "credits_data.h"

#include <pvz2native/config.h>
#include <pvz2native/dex/config_store.h>
#include <pvz2native/host_assets.h>
#include <pvz2native/log.h>

#include "stb_truetype.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <SDL.h>
#include <glad/gl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pvz2native {
namespace {

/* =========================================================================
 * Localization
 * ========================================================================= */
static CreditLang current_credit_lang() {
    const pvz2_config_t *cfg = pvz2_config();
    const char *loc = cfg ? cfg->user_locale : nullptr;
    if (!loc || !loc[0]) return CreditLang::ES; /* Default to ES */
    if (std::strncmp(loc, "es", 2) == 0) return CreditLang::ES;
    if (std::strncmp(loc, "en", 2) == 0) return CreditLang::EN;
    if (std::strncmp(loc, "fr", 2) == 0) return CreditLang::FR;
    if (std::strncmp(loc, "de", 2) == 0) return CreditLang::DE;
    if (std::strncmp(loc, "it", 2) == 0) return CreditLang::IT;
    if (std::strncmp(loc, "pt", 2) == 0) return CreditLang::PT;
    return CreditLang::ES;
}

static const char *kSkipButtonTexts[6] = {
    "Saltar",                 /* ES */
    "Skip",                   /* EN */
    "Passer",                 /* FR */
    "\xc3\x9c" "berspringen", /* DE: Überspringen */
    "Salta",                  /* IT */
    "Pular"                   /* PT */
};

static const char *kViewCreditsButtonTexts[6] = {
    "Ver cr\xc3\xa9" "ditos",       /* ES: Ver créditos */
    "View Credits",               /* EN */
    "Voir cr\xc3\xa9" "dits",       /* FR: Voir crédits */
    "Credits ansehen",            /* DE */
    "Vedi crediti",               /* IT */
    "Ver cr\xc3\xa9" "ditos"        /* PT: Ver créditos */
};

static const char *kPauseBadgeTexts[6] = {
    "PAUSA",
    "PAUSED",
    "PAUSE",
    "PAUSE",
    "PAUSA",
    "PAUSA"
};

/* =========================================================================
 * Font system (stb_truetype using RC_CFG_FONT 213)
 * ========================================================================= */
static const int RC_CFG_FONT = 213;
static const int RC_CREDITS_MUSIC = 318;

static void *load_raw_from_rc(int rc_id, int *out_size) {
#ifdef _WIN32
    HMODULE self = GetModuleHandleA(NULL);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(rc_id), RT_RCDATA);
    if (res) {
        HGLOBAL h = LoadResource(self, res);
        if (h) {
            DWORD bytes = SizeofResource(self, res);
            const void *ptr = LockResource(h);
            if (ptr && bytes) {
                void *buf = SDL_malloc(bytes);
                if (buf) {
                    SDL_memcpy(buf, ptr, bytes);
                    if (out_size) *out_size = (int)bytes;
                    return buf;
                }
            }
        }
    }
    return pvz2_load_asset_bytes(rc_id, out_size);
#else
    return pvz2_load_asset_bytes(rc_id, out_size);
#endif
}

struct CreditsFont {
    stbtt_fontinfo info{};
    void *data = nullptr;
    GLuint atlas_tex = 0;
    static const int kAtlasW = 2048;
    static const int kAtlasH = 2048;
    unsigned char atlas_buf[kAtlasW * kAtlasH]{};
    stbtt_bakedchar glyphs_header[224]{}; /* 80px bake */
    stbtt_bakedchar glyphs_item[224]{};   /* 44px bake */
    float scale_header = 0.0f;
    float scale_item   = 0.0f;
    int ascent_unscaled = 0;
    bool ok = false;

    bool load(int rc_id, float px_header, float px_item) {
        int sz = 0;
        data = load_raw_from_rc(rc_id, &sz);
        if (!data) return false;
        if (!stbtt_InitFont(&info, (unsigned char *)data, 0)) return false;
        scale_header = stbtt_ScaleForPixelHeight(&info, px_header);
        scale_item   = stbtt_ScaleForPixelHeight(&info, px_item);
        int asc = 0, desc = 0, lg = 0;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &lg);
        ascent_unscaled = asc;

        stbtt_BakeFontBitmap((unsigned char *)data, 0, px_header,
                             atlas_buf, kAtlasW, kAtlasH / 2, 32, 224, glyphs_header);
        stbtt_BakeFontBitmap((unsigned char *)data, 0, px_item,
                             atlas_buf + kAtlasW * (kAtlasH / 2), kAtlasW, kAtlasH / 2,
                             32, 224, glyphs_item);
        for (int i = 0; i < 224; ++i) {
            glyphs_item[i].y0 += (short)(kAtlasH / 2);
            glyphs_item[i].y1 += (short)(kAtlasH / 2);
        }

        glGenTextures(1, &atlas_tex);
        glBindTexture(GL_TEXTURE_2D, atlas_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, kAtlasW, kAtlasH, 0,
                     GL_ALPHA, GL_UNSIGNED_BYTE, atlas_buf);
        ok = true;
        return true;
    }

    void unload() {
        if (atlas_tex) { glDeleteTextures(1, &atlas_tex); atlas_tex = 0; }
        if (data) { SDL_free(data); data = nullptr; }
        ok = false;
    }
};

static CreditsFont g_font;
static bool g_font_loaded = false;

static unsigned int utf8_next(const char **p) {
    unsigned char c = (unsigned char)**p;
    if (!c) return 0;
    ++(*p);
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) {
        unsigned char c2 = (unsigned char)**p;
        if ((c2 & 0xC0) != 0x80) return '?';
        ++(*p);
        return ((unsigned int)(c & 0x1F) << 6) | (c2 & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {
        unsigned char c2 = (unsigned char)**p, c3;
        if ((c2 & 0xC0) != 0x80) return '?';
        ++(*p); c3 = (unsigned char)**p;
        if ((c3 & 0xC0) != 0x80) return '?';
        ++(*p);
        return ((unsigned int)(c & 0x0F) << 12) | ((unsigned int)(c2 & 0x3F) << 6) | (c3 & 0x3F);
    }
    return '?';
}

static float measure_string_cr(CreditsFont &f, const char *text, bool is_header, float scale) {
    if (!f.ok || !text) return 0.0f;
    stbtt_bakedchar *bc = is_header ? f.glyphs_header : f.glyphs_item;
    float w = 0.0f;
    const char *p = text;
    unsigned int cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp < 32 || cp > 255) { w += 8.0f * scale; continue; }
        w += bc[cp - 32].xadvance * scale;
    }
    return w + 1.0f * scale;
}

static float draw_string_cr(CreditsFont &font, const char *text, bool is_header,
                            float x, float y,
                            float cr, float cg, float cb, float alpha,
                            float scale, bool shadow = true) {
    if (!font.ok || !text || alpha <= 0.0f) return x;

    glBindTexture(GL_TEXTURE_2D, font.atlas_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    stbtt_bakedchar *bc = is_header ? font.glyphs_header : font.glyphs_item;
    float fw = (float)CreditsFont::kAtlasW;
    float fh = (float)CreditsFont::kAtlasH;

    auto emit_glyph = [&](float px, float py, stbtt_bakedchar *cd, float r, float g, float b, float a) {
        float x0 = px + cd->xoff * scale;
        float y0 = py + cd->yoff * scale;
        float x1 = x0 + (cd->x1 - cd->x0) * scale;
        float y1 = y0 + (cd->y1 - cd->y0) * scale;

        glBegin(GL_QUADS);
        glColor4f(r, g, b, a);
        glTexCoord2f(cd->x0 / fw, cd->y0 / fh); glVertex2f(x0, y0);
        glTexCoord2f(cd->x1 / fw, cd->y0 / fh); glVertex2f(x1, y0);
        glTexCoord2f(cd->x1 / fw, cd->y1 / fh); glVertex2f(x1, y1);
        glTexCoord2f(cd->x0 / fw, cd->y1 / fh); glVertex2f(x0, y1);
        glEnd();
    };

    auto emit_str = [&](float dx, float dy, float r, float g, float b, float a) {
        const char *p = text;
        float px = x + dx;
        float py = y + dy;
        unsigned int cp;
        while ((cp = utf8_next(&p)) != 0) {
            if (cp < 32 || cp > 255) { px += 8.0f * scale; continue; }
            stbtt_bakedchar *cd = &bc[cp - 32];
            if (cd->x0 == cd->x1) { px += cd->xadvance * scale; continue; }
            emit_glyph(px, py, cd, r, g, b, a);
            px += cd->xadvance * scale;
        }
    };

    if (shadow) {
        float s_off = 2.0f * scale;
        emit_str(s_off, s_off, 0.0f, 0.0f, 0.0f, 0.45f * alpha);
    }
    emit_str(0.0f, 0.0f, cr, cg, cb, alpha);

    float px = x;
    const char *p = text;
    unsigned int cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp < 32 || cp > 255) { px += 8.0f * scale; continue; }
        px += bc[cp - 32].xadvance * scale;
    }
    return px;
}

static std::vector<std::string> wrap_text_cr(CreditsFont &font, const char *text,
                                             float max_w, float scale) {
    std::vector<std::string> lines;
    if (!text || !text[0]) return lines;
    auto width_of = [&](const std::string &s) {
        return measure_string_cr(font, s.c_str(), false, scale);
    };
    std::string cur;
    std::string word;
    const char *p = text;
    while (true) {
        unsigned int cp = utf8_next(&p);
        if (cp == ' ' || cp == 0) {
            if (!word.empty()) {
                std::string test = cur.empty() ? word : (cur + " " + word);
                if (width_of(test) <= max_w) {
                    cur = test;
                } else {
                    if (!cur.empty()) lines.push_back(cur);
                    cur = word;
                }
                word.clear();
            }
            if (cp == 0) break;
        } else {
            if (cp < 128) word.push_back((char)cp);
            else {
                char buf[8];
                int len = 0;
                if (cp < 0x800) {
                    buf[len++] = (char)(0xC0 | (cp >> 6));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    buf[len++] = (char)(0xE0 | (cp >> 12));
                    buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[len++] = (char)(0x80 | (cp & 0x3F));
                }
                buf[len] = 0;
                word += buf;
            }
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

/* =========================================================================
 * Starfield Particle Simulation
 * ========================================================================= */
struct Star {
    float x, y, z;
    float size;
    float phase;
    float speed;
};

static const int kNumStars = 120;
static Star g_stars[kNumStars];
static bool g_stars_inited = false;

static void init_starfield() {
    for (int i = 0; i < kNumStars; ++i) {
        g_stars[i].x = (float)(rand() % 1920);
        g_stars[i].y = (float)(rand() % 1080);
        g_stars[i].z = 0.4f + ((float)(rand() % 100) / 100.0f) * 1.2f;
        g_stars[i].size = 1.0f + ((float)(rand() % 100) / 100.0f) * 2.5f;
        g_stars[i].phase = ((float)(rand() % 628) / 100.0f);
        g_stars[i].speed = 15.0f + ((float)(rand() % 100) / 100.0f) * 25.0f;
    }
    g_stars_inited = true;
}

static void update_starfield(float dt) {
    if (!g_stars_inited) init_starfield();
    for (int i = 0; i < kNumStars; ++i) {
        g_stars[i].y -= g_stars[i].speed * g_stars[i].z * dt;
        g_stars[i].phase += dt * 2.0f;
        if (g_stars[i].y < -20.0f) {
            g_stars[i].y = 1100.0f;
            g_stars[i].x = (float)(rand() % 1920);
        }
    }
}

/* =========================================================================
 * Screen State
 * ========================================================================= */
static bool g_credits_open = false;
static bool g_open_requested = false;
static bool g_modern34_reward_active = false;
static bool g_modern34_level_active = false;
static bool g_modern34_won = false;
static Uint32 g_modern34_won_time_ms = 0;

static float g_scroll_y = 0.0f;
static float g_scroll_speed = 62.0f;  /* pixels per second */
static bool  g_paused = false;
static float g_fade_alpha = 0.0f;     /* [0, 1] screen fade in/out */
static bool  g_fading_out = false;
static Uint32 g_last_tick_ms = 0;
static float g_finish_wait_timer = 0.0f;

/* User fast forward / rewind state */
static bool g_key_fast_forward = false;
static bool g_key_rewind = false;

/* Mouse hover on Skip button */
static bool g_skip_hover = false;
static bool g_skip_pressed = false;

static void ensure_font_loaded() {
    if (!g_font_loaded) {
        g_font.load(RC_CFG_FONT, 72.0f, 38.0f);
        g_font_loaded = true;
    }
}

static bool hit(float bx, float by, float bw, float bh, float px, float py) {
    return px >= bx && px <= bx + bw && py >= by && py <= by + bh;
}

/* =========================================================================
 * Native Credits Music ("Zombie Time" Ogg Vorbis streaming player)
 * ========================================================================= */
static SDL_AudioDeviceID g_music_dev = 0;
static SDL_AudioSpec     g_music_spec{};
static void             *g_music_ogg_buf = nullptr;
static int               g_music_ogg_size = 0;
static stb_vorbis       *g_music_vorbis = nullptr;
static bool              g_music_playing = false;
static bool              g_music_fading = false;
static float             g_music_fade_gain = 1.0f;
/* Master volume attenuation factor so Zombie Time matches native in-game Wwise loudness levels */
static constexpr float   kCreditsMusicMasterGain = 0.45f;
static std::atomic<int>  g_music_vol_pct{100};
static std::atomic<bool> g_music_active_flag{false};

static int sample_music_volume_pct() {
    std::int64_t muted = 0;
    if (pvz2native::dex::config_store_get_int64("Muted", muted) && muted != 0) {
        return 0;
    }
    std::int64_t vol = 100;
    pvz2native::dex::config_store_get_int64("MusicVolume", vol);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return (int)vol;
}

static void SDLCALL credits_music_audio_cb(void *, Uint8 *stream, int len) {
    SDL_memset(stream, 0, (size_t)len);
    if (!g_music_playing || !g_music_vorbis || len <= 0) return;

    short *out = reinterpret_cast<short *>(stream);
    const int total_samples = len / (int)sizeof(short);
    const int frames_needed = total_samples / 2;
    int frames_written = 0;

    while (frames_written < frames_needed) {
        const int got = stb_vorbis_get_samples_short_interleaved(
            g_music_vorbis, 2, out + frames_written * 2, (frames_needed - frames_written) * 2);
        if (got <= 0) {
            if (g_music_fading) break;
            stb_vorbis_seek_start(g_music_vorbis);
            const int retry = stb_vorbis_get_samples_short_interleaved(
                g_music_vorbis, 2, out + frames_written * 2, (frames_needed - frames_written) * 2);
            if (retry <= 0) {
                g_music_playing = false;
                g_music_active_flag.store(false, std::memory_order_relaxed);
                break;
            }
            frames_written += retry;
        } else {
            frames_written += got;
        }
    }

    const float base_gain = ((float)g_music_vol_pct.load(std::memory_order_relaxed) / 100.0f) * kCreditsMusicMasterGain;
    const float fade_step = (g_music_spec.freq > 0)
                            ? (1.0f / ((float)g_music_spec.freq * 0.85f))
                            : (1.0f / (44100.0f * 0.85f));

    for (int i = 0; i < frames_written; ++i) {
        if (g_music_fading) {
            g_music_fade_gain -= fade_step;
            if (g_music_fade_gain <= 0.0f) {
                g_music_fade_gain = 0.0f;
                g_music_playing = false;
                g_music_fading = false;
                g_music_active_flag.store(false, std::memory_order_relaxed);
                const int rem = (frames_needed - i) * 2;
                if (rem > 0) SDL_memset(out + i * 2, 0, (size_t)rem * sizeof(short));
                return;
            }
        }
        const float g = base_gain * g_music_fade_gain;
        if (g < 0.999f) {
            int l = (int)std::lrintf((float)out[i * 2] * g);
            int r = (int)std::lrintf((float)out[i * 2 + 1] * g);
            if (l < -32768) l = -32768; else if (l > 32767) l = 32767;
            if (r < -32768) r = -32768; else if (r > 32767) r = 32767;
            out[i * 2]     = (short)l;
            out[i * 2 + 1] = (short)r;
        }
    }
}

} // namespace
} // namespace pvz2native

using namespace pvz2native;

extern "C" {

/* =========================================================================
 * Public API
 * ========================================================================= */
void credits_screen_open(SDL_Window *window) {
    (void)window;
    g_credits_open = false;
    g_open_requested = false;
}

void credits_screen_close(void) {
    g_credits_open = false;
    g_fading_out = false;
    g_open_requested = false;
}

int credits_screen_is_open(void) {
    return 0;
}

void credits_screen_request_open(void) {
    g_open_requested = false;
}

int credits_screen_consume_open_request(void) {
    g_open_requested = false;
    return 0;
}

static std::string g_draper_world;
static int g_draper_level = 0;
static int g_draper_state = 0;

void credits_set_draper_info(const char *world, int level, int state) {
    if (world && world[0] != '\0') g_draper_world = world;
    if (level > 0) g_draper_level = level;
    g_draper_state = state;
}

const char *credits_get_draper_world(void) {
    return g_draper_world.c_str();
}

int credits_get_draper_level(void) {
    return g_draper_level;
}

int credits_get_draper_state(void) {
    return g_draper_state;
}

void credits_set_modern34_active(int active) {
    g_modern34_level_active = (active != 0);
}

int credits_get_modern34_active(void) {
    return g_modern34_level_active ? 1 : 0;
}

void credits_notify_modern34_reward_create(void) {
    g_modern34_reward_active = true;
}

void credits_notify_modern34_reward_destroy(void) {
    g_modern34_reward_active = false;
    g_modern34_won = false;
}

int credits_is_modern34_reward_active(void) {
    return g_modern34_reward_active ? 1 : 0;
}

void credits_mark_modern34_won(void) {
    g_modern34_won = true;
}

int credits_is_modern34_won(void) {
    return g_modern34_won ? 1 : 0;
}

void credits_update_frame(void) {
    if (g_music_active_flag.load(std::memory_order_relaxed)) {
        g_music_vol_pct.store(sample_music_volume_pct(), std::memory_order_relaxed);
    }
}

void credits_music_start(void) {
    g_music_vol_pct.store(sample_music_volume_pct(), std::memory_order_relaxed);

    if (!g_music_ogg_buf || g_music_ogg_size <= 0) {
        g_music_ogg_buf = load_raw_from_rc(RC_CREDITS_MUSIC, &g_music_ogg_size);
        if (!g_music_ogg_buf || g_music_ogg_size <= 0) {
            pvz2_log("[credits] Failed to load zombie_time.ogg asset (rc=%d)", RC_CREDITS_MUSIC);
            return;
        }
    }

    if (!g_music_dev) {
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            pvz2_log("[credits] SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s", SDL_GetError());
            return;
        }
        SDL_AudioSpec want{};
        want.freq     = 44100;
        want.format   = AUDIO_S16SYS;
        want.channels = 2;
        want.samples  = 2048;
        want.callback = credits_music_audio_cb;
        want.userdata = nullptr;
        g_music_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &g_music_spec, 0);
        if (!g_music_dev) {
            pvz2_log("[credits] SDL_OpenAudioDevice failed: %s", SDL_GetError());
            return;
        }
    }

    SDL_LockAudioDevice(g_music_dev);
    if (g_music_vorbis) {
        stb_vorbis_seek_start(g_music_vorbis);
    } else {
        int vorbis_err = 0;
        g_music_vorbis = stb_vorbis_open_memory(
            static_cast<const unsigned char *>(g_music_ogg_buf), g_music_ogg_size, &vorbis_err, nullptr);
        if (!g_music_vorbis) {
            SDL_UnlockAudioDevice(g_music_dev);
            pvz2_log("[credits] stb_vorbis_open_memory failed (err=%d)", vorbis_err);
            return;
        }
    }
    g_music_fade_gain = 1.0f;
    g_music_fading    = false;
    g_music_playing   = true;
    g_music_active_flag.store(true, std::memory_order_relaxed);
    SDL_UnlockAudioDevice(g_music_dev);

    SDL_PauseAudioDevice(g_music_dev, 0);
    pvz2_log("[credits] Playing 'Zombie Time' credits theme (vol=%d%%, %d bytes)",
             g_music_vol_pct.load(std::memory_order_relaxed), g_music_ogg_size);
}

void credits_music_fade_out(void) {
    if (!g_music_dev) return;
    SDL_LockAudioDevice(g_music_dev);
    if (g_music_playing && !g_music_fading) {
        g_music_fading = true;
    }
    SDL_UnlockAudioDevice(g_music_dev);
}

void credits_music_stop(void) {
    if (!g_music_dev) return;
    SDL_LockAudioDevice(g_music_dev);
    g_music_playing   = false;
    g_music_fading    = false;
    g_music_fade_gain = 0.0f;
    g_music_active_flag.store(false, std::memory_order_relaxed);
    SDL_UnlockAudioDevice(g_music_dev);
    SDL_PauseAudioDevice(g_music_dev, 1);
}

void credits_music_shutdown(void) {
    if (g_music_dev) {
        credits_music_stop();
        SDL_CloseAudioDevice(g_music_dev);
        g_music_dev = 0;
    }
    if (g_music_vorbis) {
        stb_vorbis_close(g_music_vorbis);
        g_music_vorbis = nullptr;
    }
    if (g_music_ogg_buf) {
        SDL_free(g_music_ogg_buf);
        g_music_ogg_buf  = nullptr;
        g_music_ogg_size = 0;
    }
}

int credits_screen_handle_reward_button_event(const SDL_Event *e) {
    (void)e;
    return 0;
}

void credits_screen_shutdown(void) {
    credits_music_shutdown();
    if (g_font_loaded) {
        g_font.unload();
        g_font_loaded = false;
    }
    g_credits_open = false;
    g_open_requested = false;
}

void credits_screen_handle_event(const SDL_Event *e) {
    if (!g_credits_open) return;

    if (e->type == SDL_KEYDOWN) {
        SDL_Keycode kc = e->key.keysym.sym;
        if (kc == SDLK_ESCAPE || kc == SDLK_AC_BACK) {
            g_fading_out = true;
        } else if (kc == SDLK_SPACE) {
            g_paused = !g_paused;
        } else if (kc == SDLK_DOWN || kc == SDLK_s || kc == SDLK_PAGEDOWN) {
            g_key_fast_forward = true;
        } else if (kc == SDLK_UP || kc == SDLK_w || kc == SDLK_PAGEUP) {
            g_key_rewind = true;
        } else if (kc == SDLK_RETURN || kc == SDLK_KP_ENTER) {
            g_fading_out = true;
        }
    } else if (e->type == SDL_KEYUP) {
        SDL_Keycode kc = e->key.keysym.sym;
        if (kc == SDLK_DOWN || kc == SDLK_s || kc == SDLK_PAGEDOWN) {
            g_key_fast_forward = false;
        } else if (kc == SDLK_UP || kc == SDLK_w || kc == SDLK_PAGEUP) {
            g_key_rewind = false;
        }
    } else if (e->type == SDL_MOUSEWHEEL) {
        /* Wheel scroll */
        g_scroll_y += (float)e->wheel.y * -45.0f;
    } else if (e->type == SDL_MOUSEMOTION) {
        int ww = 1920, wh = 1080;
        SDL_Window *win = SDL_GL_GetCurrentWindow();
        if (win) SDL_GL_GetDrawableSize(win, &ww, &wh);
        float s = (float)wh / 1080.0f;
        float mx = (float)e->motion.x;
        float my = (float)e->motion.y;

        float btn_w = 170.0f * s;
        float btn_h = 56.0f * s;
        float btn_x = (float)ww - btn_w - 35.0f * s;
        float btn_y = (float)wh - btn_h - 35.0f * s;

        g_skip_hover = hit(btn_x, btn_y, btn_w, btn_h, mx, my);
    } else if (e->type == SDL_MOUSEBUTTONDOWN) {
        if (e->button.button == SDL_BUTTON_LEFT) {
            if (g_skip_hover) {
                g_skip_pressed = true;
            } else {
                g_paused = !g_paused;
            }
        }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        if (e->button.button == SDL_BUTTON_LEFT) {
            if (g_skip_pressed && g_skip_hover) {
                g_fading_out = true;
            }
            g_skip_pressed = false;
        }
    } else if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        Uint8 b = e->cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START || b == SDL_CONTROLLER_BUTTON_BACK) {
            g_fading_out = true;
        } else if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_X) {
            g_paused = !g_paused;
        } else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            g_key_fast_forward = true;
        } else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) {
            g_key_rewind = true;
        }
    } else if (e->type == SDL_CONTROLLERBUTTONUP) {
        Uint8 b = e->cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) g_key_fast_forward = false;
        if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) g_key_rewind = false;
    } else if (e->type == SDL_CONTROLLERAXISMOTION) {
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            Sint16 val = e->caxis.value;
            if (val > 10000) { g_key_fast_forward = true; g_key_rewind = false; }
            else if (val < -10000) { g_key_rewind = true; g_key_fast_forward = false; }
            else { g_key_fast_forward = false; g_key_rewind = false; }
        }
    }
}

void credits_screen_draw(SDL_Window *window) {
    if (!g_credits_open) return;

    Uint32 now_ms = SDL_GetTicks();
    float dt = (float)(now_ms - g_last_tick_ms) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_last_tick_ms = now_ms;

    /* Handle fade in / out */
    if (g_fading_out) {
        g_fade_alpha -= dt * 2.5f;
        if (g_fade_alpha <= 0.0f) {
            g_fade_alpha = 0.0f;
            credits_screen_close();
            return;
        }
    } else if (g_fade_alpha < 1.0f) {
        g_fade_alpha += dt * 2.0f;
        if (g_fade_alpha > 1.0f) g_fade_alpha = 1.0f;
    }

    /* Update scroll speed */
    if (!g_paused) {
        float speed = g_scroll_speed;
        if (g_key_fast_forward) speed = 250.0f;
        else if (g_key_rewind) speed = -150.0f;
        g_scroll_y += speed * dt;
    }

    update_starfield(dt);

    int ww, wh;
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    float win_w = (float)ww, win_h = (float)wh;
    float s = win_h / 1080.0f;

    ensure_font_loaded();

    /* Save GL state */
    GLint prev_program = 0, prev_fbo = 0;
    GLint prev_viewport[4]{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, ww, wh);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, (double)win_w, (double)win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* 1. Deep Space Cosmic Background */
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    /* Gradient: Deepest midnight at top & bottom, cosmic purple/blue in center */
    glColor4f(0.02f, 0.03f, 0.07f, g_fade_alpha);
    glVertex2f(0, 0); glVertex2f(win_w, 0);
    glColor4f(0.06f, 0.08f, 0.16f, g_fade_alpha);
    glVertex2f(win_w, win_h * 0.5f); glVertex2f(0, win_h * 0.5f);

    glColor4f(0.06f, 0.08f, 0.16f, g_fade_alpha);
    glVertex2f(0, win_h * 0.5f); glVertex2f(win_w, win_h * 0.5f);
    glColor4f(0.02f, 0.03f, 0.07f, g_fade_alpha);
    glVertex2f(win_w, win_h); glVertex2f(0, win_h);
    glEnd();

    /* 2. Parallax Stars */
    for (int i = 0; i < kNumStars; ++i) {
        float sx = g_stars[i].x * (win_w / 1920.0f);
        float sy = g_stars[i].y * s;
        float sz = g_stars[i].size * s;
        float twinkle = 0.5f + 0.5f * std::sin(g_stars[i].phase);
        float star_a = (0.25f + 0.65f * twinkle) * g_fade_alpha * (g_stars[i].z / 1.6f);

        glColor4f(0.85f, 0.90f, 1.0f, star_a);
        glBegin(GL_QUADS);
        glVertex2f(sx - sz, sy - sz);
        glVertex2f(sx + sz, sy - sz);
        glVertex2f(sx + sz, sy + sz);
        glVertex2f(sx - sz, sy + sz);
        glEnd();
    }

    /* 3. Render Credit Lines */
    glEnable(GL_TEXTURE_2D);
    CreditLang lang = current_credit_lang();

    float cx = win_w * 0.5f;
    float cur_y = win_h - g_scroll_y;
    float max_content_y = 0.0f;

    for (size_t i = 0; i < kCreditEntriesCount; ++i) {
        const CreditEntry &entry = kCreditEntries[i];
        const char *txt = credit_get_text(entry, lang);

        switch (entry.kind) {
            case CreditKind::Space: {
                cur_y += 36.0f * s;
                break;
            }
            case CreditKind::TopHeader: {
                cur_y += 50.0f * s;
                if (cur_y > -100.0f && cur_y < win_h + 100.0f && txt && txt[0]) {
                    float str_w = measure_string_cr(g_font, txt, true, s * 0.95f);
                    float tx = cx - str_w * 0.5f;
                    /* Gold glowing color */
                    draw_string_cr(g_font, txt, true, tx, cur_y,
                                   1.0f, 0.85f, 0.20f, g_fade_alpha, s * 0.95f, true);
                }
                cur_y += 70.0f * s;
                break;
            }
            case CreditKind::Header: {
                cur_y += 45.0f * s;
                if (cur_y > -100.0f && cur_y < win_h + 100.0f && txt && txt[0]) {
                    float str_w = measure_string_cr(g_font, txt, true, s * 0.70f);
                    float tx = cx - str_w * 0.5f;

                    /* Decorative gold horizontal divider lines */
                    glDisable(GL_TEXTURE_2D);
                    float line_len = 160.0f * s;
                    float line_gap = 25.0f * s;
                    float ly = cur_y - 12.0f * s;

                    /* Left line */
                    glBegin(GL_QUADS);
                    glColor4f(1.0f, 0.85f, 0.25f, 0.0f); glVertex2f(tx - line_gap - line_len, ly - 1.5f * s);
                    glColor4f(1.0f, 0.85f, 0.25f, 0.8f * g_fade_alpha); glVertex2f(tx - line_gap, ly - 1.5f * s);
                    glColor4f(1.0f, 0.85f, 0.25f, 0.8f * g_fade_alpha); glVertex2f(tx - line_gap, ly + 1.5f * s);
                    glColor4f(1.0f, 0.85f, 0.25f, 0.0f); glVertex2f(tx - line_gap - line_len, ly + 1.5f * s);

                    /* Right line */
                    glColor4f(1.0f, 0.85f, 0.25f, 0.8f * g_fade_alpha); glVertex2f(tx + str_w + line_gap, ly - 1.5f * s);
                    glColor4f(1.0f, 0.85f, 0.25f, 0.0f); glVertex2f(tx + str_w + line_gap + line_len, ly - 1.5f * s);
                    glColor4f(1.0f, 0.85f, 0.25f, 0.0f); glVertex2f(tx + str_w + line_gap + line_len, ly + 1.5f * s);
                    glColor4f(1.0f, 0.85f, 0.25f, 0.8f * g_fade_alpha); glVertex2f(tx + str_w + line_gap, ly + 1.5f * s);
                    glEnd();
                    glEnable(GL_TEXTURE_2D);

                    /* Warm Yellow Header */
                    draw_string_cr(g_font, txt, true, tx, cur_y,
                                   1.0f, 0.92f, 0.35f, g_fade_alpha, s * 0.70f, true);
                }
                cur_y += 50.0f * s;
                break;
            }
            case CreditKind::Title: {
                cur_y += 28.0f * s;
                if (cur_y > -100.0f && cur_y < win_h + 100.0f && txt && txt[0]) {
                    float str_w = measure_string_cr(g_font, txt, false, s * 0.95f);
                    float tx = cx - str_w * 0.5f;
                    /* Amber / PopCap Gold */
                    draw_string_cr(g_font, txt, false, tx, cur_y,
                                   1.0f, 0.82f, 0.28f, g_fade_alpha, s * 0.95f, true);
                }
                cur_y += 34.0f * s;
                break;
            }
            case CreditKind::Name: {
                if (cur_y > -100.0f && cur_y < win_h + 100.0f && txt && txt[0]) {
                    float str_w = measure_string_cr(g_font, txt, false, s * 0.88f);
                    float tx = cx - str_w * 0.5f;
                    /* Crisp off-white */
                    draw_string_cr(g_font, txt, false, tx, cur_y,
                                   0.96f, 0.97f, 1.0f, g_fade_alpha, s * 0.88f, true);
                }
                cur_y += 30.0f * s;
                break;
            }
            case CreditKind::NoteName: {
                cur_y += 24.0f * s;
                if (cur_y > -100.0f && cur_y < win_h + 100.0f && txt && txt[0]) {
                    float str_w = measure_string_cr(g_font, txt, false, s * 0.90f);
                    float tx = cx - str_w * 0.5f;
                    /* Bright Sunflower Yellow */
                    draw_string_cr(g_font, txt, false, tx, cur_y,
                                   1.0f, 0.90f, 0.40f, g_fade_alpha, s * 0.90f, true);
                }
                cur_y += 30.0f * s;
                break;
            }
            case CreditKind::NoteText: {
                if (txt && txt[0]) {
                    float max_w = std::min(win_w * 0.75f, 960.0f * s);
                    std::vector<std::string> wrapped = wrap_text_cr(g_font, txt, max_w, s * 0.72f);
                    for (const auto &wline : wrapped) {
                        if (cur_y > -100.0f && cur_y < win_h + 100.0f) {
                            float str_w = measure_string_cr(g_font, wline.c_str(), false, s * 0.72f);
                            float tx = cx - str_w * 0.5f;
                            /* Soft silver lavender */
                            draw_string_cr(g_font, wline.c_str(), false, tx, cur_y,
                                           0.82f, 0.88f, 0.96f, 0.90f * g_fade_alpha, s * 0.72f, true);
                        }
                        cur_y += 25.0f * s;
                    }
                }
                cur_y += 14.0f * s;
                break;
            }
        }
        max_content_y = cur_y;
    }

    /* 4. Check for credits completion */
    if (cur_y < win_h * 0.35f) {
        /* All content has scrolled past center: hold and then fade out */
        g_finish_wait_timer += dt;
        if (g_finish_wait_timer > 4.5f) {
            g_fading_out = true;
        }
    }

    /* 5. Draw Skip Button (Bottom-Right) */
    {
        float btn_w = 170.0f * s;
        float btn_h = 56.0f * s;
        float btn_x = win_w - btn_w - 35.0f * s;
        float btn_y = win_h - btn_h - 35.0f * s;

        glDisable(GL_TEXTURE_2D);

        /* Button shadow */
        glColor4f(0.0f, 0.0f, 0.0f, 0.40f * g_fade_alpha);
        glBegin(GL_QUADS);
        glVertex2f(btn_x + 3.0f * s, btn_y + 4.0f * s);
        glVertex2f(btn_x + btn_w + 3.0f * s, btn_y + 4.0f * s);
        glVertex2f(btn_x + btn_w + 3.0f * s, btn_y + btn_h + 4.0f * s);
        glVertex2f(btn_x + 3.0f * s, btn_y + btn_h + 4.0f * s);
        glEnd();

        /* Button fill (PopCap wood/stone/gold button) */
        float top_r = g_skip_hover ? 0.38f : 0.28f;
        float top_g = g_skip_hover ? 0.48f : 0.36f;
        float top_b = g_skip_hover ? 0.25f : 0.18f;
        float bot_r = g_skip_hover ? 0.20f : 0.14f;
        float bot_g = g_skip_hover ? 0.28f : 0.20f;
        float bot_b = g_skip_hover ? 0.12f : 0.08f;

        glBegin(GL_QUADS);
        glColor4f(top_r, top_g, top_b, 0.95f * g_fade_alpha);
        glVertex2f(btn_x, btn_y); glVertex2f(btn_x + btn_w, btn_y);
        glColor4f(bot_r, bot_g, bot_b, 0.95f * g_fade_alpha);
        glVertex2f(btn_x + btn_w, btn_y + btn_h); glVertex2f(btn_x, btn_y + btn_h);
        glEnd();

        /* Golden border */
        float b_border = g_skip_hover ? 1.0f : 0.75f;
        glColor4f(0.95f * b_border, 0.85f * b_border, 0.35f * b_border, 0.90f * g_fade_alpha);
        float bw = 2.5f * s;
        glBegin(GL_QUADS);
        /* Top */
        glVertex2f(btn_x, btn_y); glVertex2f(btn_x + btn_w, btn_y);
        glVertex2f(btn_x + btn_w, btn_y + bw); glVertex2f(btn_x, btn_y + bw);
        /* Bottom */
        glVertex2f(btn_x, btn_y + btn_h - bw); glVertex2f(btn_x + btn_w, btn_y + btn_h - bw);
        glVertex2f(btn_x + btn_w, btn_y + btn_h); glVertex2f(btn_x, btn_y + btn_h);
        /* Left */
        glVertex2f(btn_x, btn_y); glVertex2f(btn_x + bw, btn_y);
        glVertex2f(btn_x + bw, btn_y + btn_h); glVertex2f(btn_x, btn_y + btn_h);
        /* Right */
        glVertex2f(btn_x + btn_w - bw, btn_y); glVertex2f(btn_x + btn_w, btn_y);
        glVertex2f(btn_x + btn_w, btn_y + btn_h); glVertex2f(btn_x + btn_w - bw, btn_y + btn_h);
        glEnd();

        glEnable(GL_TEXTURE_2D);

        /* Button Text */
        const char *skip_txt = kSkipButtonTexts[(size_t)lang];
        float stw = measure_string_cr(g_font, skip_txt, false, s * 0.80f);
        float stx = btn_x + (btn_w - stw) * 0.5f;
        float sty = btn_y + (btn_h - 18.0f * s) * 0.5f + 16.0f * s;
        draw_string_cr(g_font, skip_txt, false, stx, sty,
                       1.0f, 0.95f, 0.70f, g_fade_alpha, s * 0.80f, true);
    }

    /* 6. Pause Indicator Badge (Top-Right) */
    if (g_paused) {
        const char *pause_txt = kPauseBadgeTexts[(size_t)lang];
        float ptw = measure_string_cr(g_font, pause_txt, true, s * 0.60f);
        float ptx = win_w - ptw - 40.0f * s;
        float pty = 45.0f * s;

        float alpha_pulse = 0.75f + 0.25f * std::sin((float)now_ms * 0.006f);
        draw_string_cr(g_font, pause_txt, true, ptx, pty,
                       1.0f, 0.85f, 0.20f, alpha_pulse * g_fade_alpha, s * 0.60f, true);
    }

    /* Restore GL state */
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glUseProgram(prev_program);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

/* =========================================================================
 * Reward Screen Button Overlay ("Ver los créditos")
 * ========================================================================= */
void credits_screen_draw_reward_button_overlay(SDL_Window *window) {
    (void)window;
}

} // extern "C"

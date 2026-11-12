#include "pvz2native/input/gamepad.h"
#include <pvz2native/log.h>

#include "pvz2native/config.h"
#include "pvz2native/config_screen.h"
#include "pvz2native/achievements.h"
#include "pvz2native/input/input_queue.h"
#include "pvz2native/surface.h"
#include "gamepad_icons.h"

#include <glad/gl.h>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace {

enum class MenuId : int {
    Top = 0,
    Left = 1,
    Bottom = 2,
    Board = 3,
    Count = 4,
};

enum class DpadDir : int { None = 0, Up, Down, Left, Right };

struct FocusPoint {
    std::string name;
    float nx = 0.0f;
    float ny = 0.0f;
    MenuId menu = MenuId::Top;
    int row = 0;
    int col = 0;
};

struct DragGestureState {
    bool active = false;
    int step = 0;
    int start_x = 0;
    int start_y = 0;
    int end_x = 0;
    int end_y = 0;
    bool horizontal = false;
};

static const int kDragRepeats = 6;
static const int kDragBorderPad = 10;
static const float kDragCooldownSec = 0.10f;
static const float kDpadRepeatInitialSec = 0.18f;
static const float kDpadRepeatMinSec = 0.14f;
static const float kDpadRepeatAccelSec = 0.010f;

class GamepadModule {
public:
    void init(SDL_Window *window, int use_43);
    void shutdown();
    void reload();
    int handle_event(const SDL_Event *e);
    void tick(float dt_sec);

private:
    void try_open_controller();
    void rebuild_focus_points();
    void rebuild_bind_map();
    void refresh_config();

    MenuId classify_shortcut_menu(const char *name) const;
    const FocusPoint *find_board_cell(int row, int col) const;
    void get_default_menu_pos(MenuId menu, int *wx, int *wy) const;
    void goto_menu(MenuId target);
    void dpad_jump(DpadDir dir);
    void set_focus_index(int idx);
    int find_nearest_focus(float nx, float ny, MenuId only_menu, bool filter_menu) const;
    int find_focus_by_name(const char *name) const;
    void on_button_a_press();
    void on_button_a_release();
    void tap_bind(const char *bind_name);
    void inject_touch_down_at_cursor();
    void inject_touch_up_at_cursor();
    void inject_ui_mouse_down_at_cursor();
    void inject_ui_mouse_up_at_cursor();
    void warp_cursor(int wx, int wy);
    void norm_to_window(float nx, float ny, int *wx, int *wy) const;
    void cursor_to_norm(float *nx, float *ny) const;
    int map_window_to_touch(int wx, int wy, int *tx, int *ty) const;
    int map_norm_to_touch(float nx, float ny, int *tx, int *ty) const;
    void clamp_to_window(int *wx, int *wy) const;

    int handle_controller_button(const SDL_Event *e);
    DpadDir dpad_from_button(Uint8 button) const;
    void on_dpad_press(DpadDir dir);
    void on_dpad_release(DpadDir dir);
    void tick_left_stick(float dt_sec);
    void tick_right_stick(float dt_sec);
    void tick_right_stick_ui(float dt_sec);
    void tick_triggers();
    void tick_dpad_repeat(float dt_sec);

    /* Lawn mode (in a level): see the block comment above GamepadModule::grid_active. */
public:
    void draw(SDL_Window *window);
    void request_rumble(float low, float high, unsigned ms);

private:
    bool grid_active() const;
    int handle_grid_button(SDL_GameControllerButton button, bool down);
    void nav_step(DpadDir dir);
    void grid_move(DpadDir dir);
    void tick_grid(float dt_sec);
    void tick_aim_trigger(float dt_sec);
    void tick_pending_taps(float dt_sec);
    void tick_rumble();
    void grid_cell_norm(float *nx, float *ny) const;
    void grid_tile_size_norm(float *tw, float *th) const;
    bool level_active() const;
    bool chooser_active() const;
    float aspect() const;
    int bank_slots() const;
    void bank_slot_center(int slot, bool conveyor, float *nx, float *ny) const;
    void bank_packet_size_px(float H, float *pw, float *ph) const;
    void chooser_cell_center(float *nx, float *ny) const;
    void chooser_move(DpadDir dir);
    int handle_chooser_button(SDL_GameControllerButton button, bool down);
    void tick_chooser(float dt_sec);
    void tick_stick_steps(float dt_sec, void (GamepadModule::*step)(DpadDir));
    void queue_touch(float delay_sec, int type, float nx, float ny);
    void queue_scroll_drag(float from_ny, float to_ny, float nx);
    int current_mode() const;   /* 0 none, 1 level, 2 seed chooser */
    bool grid_ready() const;    /* active and past the intro wait */
    void tick_bank_right_stick(float dt_sec);
    void bank_step(int step);
    float right_edge_nx(float from_right_h) const;
    void queue_tap(float delay_sec, float nx, float ny);
    void queue_tap_bind(float delay_sec, const char *name);
    void sweep_tile_for_pickups();
    void note_controller_used();
    bool ensure_icon_texture();
    void draw_icon(int icon, float cx, float cy, float h) const;
    void rumble_now(float low, float high, unsigned ms);
    void start_drag_gesture(int dir_x, int dir_y);
    void process_drag_gesture();
    void apply_border_pad(int touch_w, int touch_h, int *x, int *y) const;

    bool is_our_controller_instance(SDL_JoystickID instance_id) const;
    bool controller_ready() const;
    bool overlay_open() const;

    SDL_Window *window_ = nullptr;
    SDL_GameController *controller_ = nullptr;
    int use_43_ = 0;
    int enabled_ = 0;
    int port_ = 0;
    float stick_speed_ = 1.6f;
    float stick_deadzone_ = 0.25f;
    float trigger_threshold_ = 0.50f;

    int cursor_x_ = 0;
    int cursor_y_ = 0;
    bool a_held_ = false;
    bool a_held_was_snap_ = false;
    MenuId active_menu_ = MenuId::Board;
    int current_focus_ = -1; /* index into focus_points_; source of truth for D-pad */
    bool snap_nav_ = false;  /* true after D-pad / shoulder menu jump; cleared by left stick */
    int menu_last_focus_[static_cast<int>(MenuId::Count)];
    int menu_last_x_[static_cast<int>(MenuId::Count)] = {};
    int menu_last_y_[static_cast<int>(MenuId::Count)] = {};
    bool menu_visited_[static_cast<int>(MenuId::Count)] = {};

    std::vector<FocusPoint> focus_points_;
    std::unordered_map<SDL_GameControllerButton, std::string> bind_by_button_;

    DpadDir dpad_held_ = DpadDir::None;
    float dpad_repeat_timer_ = 0.0f;
    float dpad_repeat_interval_ = kDpadRepeatInitialSec;

    bool l2_prev_ = false;
    bool r2_prev_ = false;
    float drag_cooldown_ = 0.0f;
    DragGestureState drag_;

    /* Lawn mode state */
    bool controller_mode_ = false;  /* last input came from the controller (mouse/keyboard clears it) */
    Uint32 ignore_mouse_until_ = 0; /* our own cursor warps produce motion events; not the player */
    bool aiming_ = false;           /* RT picked a plant that waits for a target (Banana Launcher...) */
    int aim_row_ = 0, aim_col_ = 0;
    float aim_timer_ = 0.0f;
    bool rt_prev_ = false;
    unsigned icon_tex_ = 0;
    bool grid_enabled_ = true;   /* R3 switches between lawn mode and the free cursor */
    bool grid_was_active_ = false;
    int grid_rows_ = 0, grid_cols_ = 0;
    int grid_row_ = 3, grid_col_ = 1;
    /* The lawn as a straight grid fitted to the [board] cells: x = ax + bx*col, y = ay + by*row. The
     * configured points are rounded to two decimals, so their spacing wobbles by a few pixels; the fit
     * gives evenly spaced tile centres and the true tile size. */
    float grid_ax_ = 0.0f, grid_bx_ = 0.07f, grid_ay_ = 0.0f, grid_by_ = 0.15f;
    int seed_slot_ = 1;          /* 1..8, 0 = none */
    float sweep_timer_ = 0.0f;
    DpadDir stick_dir_ = DpadDir::None;
    float stick_repeat_timer_ = 0.0f;
    float stick_repeat_interval_ = kDpadRepeatInitialSec;
    struct PendingTap {
        float delay;
        int type; /* PVZ2_TOUCH_DOWN / _MOVE / _UP, or -1 for a whole tap */
        float nx, ny;
    };
    std::vector<PendingTap> pending_taps_;

    /* Seed chooser: column 0 is the seed bank on the left, columns 1..5 the cards; rows are screen rows
     * (the card list scrolls under the box when it goes past the top or bottom). */
    bool chooser_was_active_ = false;
    int chooser_col_ = 1, chooser_row_ = 1;

    /* The box waits a moment after a level or the chooser comes up: the camera is still panning over the
     * lawn, and "Ready, set, plant" plays. */
    int mode_ = 0;
    float intro_wait_ = 0.0f;

    /* Conveyor belt: the chosen packet is a position along the belt (fraction of the screen height), not a
     * slot, so it can follow a packet that is still moving; the right stick slides it. */
    float conveyor_y_ = 0.0f;
    bool lt_prev_ = false;
    DpadDir rstick_dir_ = DpadDir::None;
    float rstick_timer_ = 0.0f;
    float rstick_interval_ = kDpadRepeatInitialSec;

    /* Rumble may be requested from a guest thread (sound hook); it is applied on the next tick. */
    std::mutex rumble_mu_;
    float rumble_low_ = 0.0f, rumble_high_ = 0.0f;
    unsigned rumble_ms_ = 0;
    Uint32 rumble_until_ = 0;
    float rumble_playing_ = 0.0f;
};

GamepadModule g_gamepad;

static float axis_to_float(Sint16 v) {
    const float denom = 32767.0f;
    return std::max(-1.0f, std::min(1.0f, static_cast<float>(v) / denom));
}

static float trigger_to_float(Sint16 v) {
    return std::max(0.0f, std::min(1.0f, static_cast<float>(v) / 32767.0f));
}

static float clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

static SDL_GameControllerButton button_name_to_sdl(const char *name) {
    if (!name || name[0] == '\0') return static_cast<SDL_GameControllerButton>(-1);
    if (SDL_strcasecmp(name, "start") == 0) return SDL_CONTROLLER_BUTTON_START;
    if (SDL_strcasecmp(name, "back") == 0) return SDL_CONTROLLER_BUTTON_BACK;
    if (SDL_strcasecmp(name, "b") == 0) return SDL_CONTROLLER_BUTTON_B;
    if (SDL_strcasecmp(name, "x") == 0) return SDL_CONTROLLER_BUTTON_X;
    if (SDL_strcasecmp(name, "y") == 0) return SDL_CONTROLLER_BUTTON_Y;
    if (SDL_strcasecmp(name, "l3") == 0) return SDL_CONTROLLER_BUTTON_LEFTSTICK;
    if (SDL_strcasecmp(name, "r3") == 0) return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    if (SDL_strcasecmp(name, "a") == 0) return SDL_CONTROLLER_BUTTON_A;
    if (SDL_strcasecmp(name, "guide") == 0) return SDL_CONTROLLER_BUTTON_GUIDE;
    return static_cast<SDL_GameControllerButton>(-1);
}

bool GamepadModule::controller_ready() const {
    return enabled_ != 0 && controller_ != nullptr;
}

bool GamepadModule::overlay_open() const {
    /* Settings and quit-confirm both report open via this flag. */
    return config_screen_is_open() != 0;
}

bool GamepadModule::is_our_controller_instance(SDL_JoystickID instance_id) const {
    if (!controller_) return false;
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller_);
    if (!joy) return false;
    return SDL_JoystickInstanceID(joy) == instance_id;
}

void GamepadModule::refresh_config() {
    const pvz2_config_t *cfg = pvz2_config();
    enabled_ = cfg->gamepad_enabled;
    port_ = cfg->gamepad_port;
    stick_speed_ = cfg->gamepad_stick_speed;
    if (stick_speed_ <= 0.0f) stick_speed_ = 1.6f;
    stick_deadzone_ = cfg->gamepad_stick_deadzone;
    if (stick_deadzone_ < 0.0f || stick_deadzone_ > 0.9f) stick_deadzone_ = 0.25f;
    trigger_threshold_ = cfg->gamepad_trigger_threshold;
    if (trigger_threshold_ <= 0.0f) trigger_threshold_ = 0.50f;
}

void GamepadModule::init(SDL_Window *window, int use_43) {
    window_ = window;
    use_43_ = use_43 ? 1 : 0;
    refresh_config();
    rebuild_focus_points();
    rebuild_bind_map();

    pvz2_log("[gamepad] init enabled=%d port=%d stick_speed=%.2f deadzone=%.2f "
                "board_cells=%u binds=%u focus=%zu",
                enabled_, port_, stick_speed_, stick_deadzone_,
                pvz2_config()->board_count, pvz2_config()->gamepad_bind_count,
                focus_points_.size());

    if (window_) {
        SDL_GetMouseState(&cursor_x_, &cursor_y_);
    }

    active_menu_ = MenuId::Board;
    current_focus_ = -1;
    snap_nav_ = false;
    for (int i = 0; i < static_cast<int>(MenuId::Count); ++i) menu_last_focus_[i] = -1;
    std::memset(menu_last_x_, 0, sizeof(menu_last_x_));
    std::memset(menu_last_y_, 0, sizeof(menu_last_y_));
    std::memset(menu_visited_, 0, sizeof(menu_visited_));

    dpad_held_ = DpadDir::None;
    dpad_repeat_timer_ = 0.0f;
    dpad_repeat_interval_ = kDpadRepeatInitialSec;
    l2_prev_ = false;
    r2_prev_ = false;
    drag_cooldown_ = 0.0f;
    drag_ = {};
    a_held_ = false;
    a_held_was_snap_ = false;

    if (!enabled_) {
        pvz2_log("[gamepad] disabled in config ([gamepad] enabled = 0)");
        return;
    }

    try_open_controller();
}

void GamepadModule::shutdown() {
    if (a_held_) on_button_a_release();
    if (controller_) {
        SDL_GameControllerClose(controller_);
        controller_ = nullptr;
    }
    window_ = nullptr;
}

void GamepadModule::reload() {
    const int prev_enabled = enabled_;
    const int prev_port = port_;
    refresh_config();
    rebuild_focus_points();
    rebuild_bind_map();

    /* Only reopen when the selected device / enabled flag changes. Stick
     * speed & deadzone tweaks just need refresh_config above. */
    if (enabled_ == prev_enabled && port_ == prev_port &&
        (controller_ != nullptr || !enabled_)) {
        return;
    }

    if (a_held_) on_button_a_release();
    if (controller_) {
        SDL_GameControllerClose(controller_);
        controller_ = nullptr;
    }
    if (enabled_) try_open_controller();
}

void GamepadModule::try_open_controller() {
    if (!enabled_ || controller_) return;

    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);

    const int n = SDL_NumJoysticks();
    pvz2_log("[gamepad] devices=%d preferred_port=%d", n, port_);
    for (int i = 0; i < n; ++i) {
        const char *name = SDL_JoystickNameForIndex(i);
        const int is_gc = SDL_IsGameController(i) ? 1 : 0;
        pvz2_log("[gamepad]   [%d] \"%s\"%s", i, name ? name : "?",
                    is_gc ? " [GameController]" : " [joystick only]");
    }

    auto try_index = [&](int index) -> bool {
        if (index < 0 || index >= n) return false;
        if (!SDL_IsGameController(index)) return false;
        SDL_GameController *c = SDL_GameControllerOpen(index);
        if (!c) return false;
        if (!SDL_GameControllerGetAttached(c)) {
            SDL_GameControllerClose(c);
            return false;
        }
        controller_ = c;
        const char *name = SDL_GameControllerName(c);
        pvz2_log("[gamepad] using \"%s\" (device index %d)", name ? name : "?", index);
        return true;
    };

    /* 1) Exact device index from config (Xbox wireless receivers often need 1..3). */
    if (try_index(port_)) return;

    /* 2) Treat port as Nth GameController among mapped pads (0 = first GC). */
    int gc_ordinal = 0;
    for (int i = 0; i < n; ++i) {
        if (!SDL_IsGameController(i)) continue;
        if (gc_ordinal == port_ && try_index(i)) return;
        ++gc_ordinal;
    }

    /* 3) First usable GameController on any index. */
    for (int i = 0; i < n; ++i) {
        if (try_index(i)) return;
    }

    pvz2_log("[gamepad] no usable GameController found "
                "(wireless 360: try port=1..3, or check xpad/xboxdrv)");
}

MenuId GamepadModule::classify_shortcut_menu(const char *name) const {
    if (!name) return MenuId::Top;
    if (std::strncmp(name, "plant_", 6) == 0 || std::strcmp(name, "replay") == 0) return MenuId::Left;
    if (std::strcmp(name, "back") == 0 || std::strcmp(name, "almanaq") == 0 ||
        std::strcmp(name, "connect") == 0 || std::strcmp(name, "vasebreaker") == 0 ||
        std::strcmp(name, "zen_garden") == 0 || std::strcmp(name, "speed") == 0 ||
        std::strcmp(name, "pause") == 0) {
        return MenuId::Top;
    }
    if (std::strcmp(name, "plant_food") == 0 || std::strncmp(name, "power_", 6) == 0 ||
        std::strcmp(name, "shovel") == 0) {
        return MenuId::Bottom;
    }
    return MenuId::Top;
}

void GamepadModule::rebuild_focus_points() {
    focus_points_.clear();
    const pvz2_config_t *cfg = pvz2_config();

    for (unsigned i = 0; i < cfg->shortcut_count; ++i) {
        const pvz2_shortcut_binding_t *in = &cfg->shortcuts[i];
        FocusPoint fp;
        fp.name = in->name;
        if (use_43_ && in->has_43) {
            fp.nx = in->x43;
            fp.ny = in->y43;
        } else {
            fp.nx = in->x;
            fp.ny = in->y;
        }
        fp.menu = classify_shortcut_menu(in->name);
        focus_points_.push_back(fp);
    }

    for (unsigned i = 0; i < cfg->board_count; ++i) {
        const pvz2_board_cell_t *cell = &cfg->board[i];
        FocusPoint fp;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "position_%d_%d", cell->row, cell->col);
        fp.name = buf;
        if (use_43_ && cell->has_43) {
            fp.nx = cell->x43;
            fp.ny = cell->y43;
        } else {
            fp.nx = cell->x;
            fp.ny = cell->y;
        }
        fp.menu = MenuId::Board;
        fp.row = cell->row;
        fp.col = cell->col;
        focus_points_.push_back(fp);
    }

    grid_rows_ = grid_cols_ = 0;
    for (const FocusPoint &fp : focus_points_) {
        if (fp.menu != MenuId::Board) continue;
        grid_rows_ = std::max(grid_rows_, fp.row);
        grid_cols_ = std::max(grid_cols_, fp.col);
    }
    {
        /* Least-squares line through the cells, per axis. */
        double n = 0, sc = 0, sx = 0, scc = 0, scx = 0, sr = 0, sy = 0, srr = 0, sry = 0;
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu != MenuId::Board) continue;
            n += 1;
            sc += fp.col; sx += fp.nx; scc += (double)fp.col * fp.col; scx += fp.col * (double)fp.nx;
            sr += fp.row; sy += fp.ny; srr += (double)fp.row * fp.row; sry += fp.row * (double)fp.ny;
        }
        const double dc = n * scc - sc * sc, dr = n * srr - sr * sr;
        if (n >= 2 && dc > 0) {
            grid_bx_ = (float)((n * scx - sc * sx) / dc);
            grid_ax_ = (float)((sx - grid_bx_ * sc) / n);
        }
        if (n >= 2 && dr > 0) {
            grid_by_ = (float)((n * sry - sr * sy) / dr);
            grid_ay_ = (float)((sy - grid_by_ * sr) / n);
        }
    }

    current_focus_ = -1;
    snap_nav_ = false;
    for (int i = 0; i < static_cast<int>(MenuId::Count); ++i) menu_last_focus_[i] = -1;
}

void GamepadModule::rebuild_bind_map() {
    bind_by_button_.clear();
    const pvz2_config_t *cfg = pvz2_config();

    for (unsigned i = 0; i < cfg->gamepad_bind_count; ++i) {
        const pvz2_gamepad_bind_t *entry = &cfg->gamepad_binds[i];
        if (!entry->bind_name[0]) continue;
        if (SDL_strcasecmp(entry->bind_name, "NULL") == 0) continue;

        const SDL_GameControllerButton btn = button_name_to_sdl(entry->button);
        if (btn == static_cast<SDL_GameControllerButton>(-1)) continue;
        /* Reserved controls stay in code paths, not remappable here. */
        if (btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_BACK ||
            btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ||
            btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ||
            btn == SDL_CONTROLLER_BUTTON_DPAD_UP || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
            btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            continue;
        }

        bind_by_button_[btn] = entry->bind_name;
    }
}

const FocusPoint *GamepadModule::find_board_cell(int row, int col) const {
    for (const FocusPoint &fp : focus_points_) {
        if (fp.menu == MenuId::Board && fp.row == row && fp.col == col) return &fp;
    }
    return nullptr;
}

void GamepadModule::norm_to_window(float nx, float ny, int *wx, int *wy) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float denom_w = (ww > 1) ? static_cast<float>(ww - 1) : 1.0f;
    const float denom_h = (wh > 1) ? static_cast<float>(wh - 1) : 1.0f;
    *wx = static_cast<int>(clamp01(nx) * denom_w + 0.5f);
    *wy = static_cast<int>(clamp01(ny) * denom_h + 0.5f);
}

void GamepadModule::cursor_to_norm(float *nx, float *ny) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float denom_w = (ww > 1) ? static_cast<float>(ww - 1) : 1.0f;
    const float denom_h = (wh > 1) ? static_cast<float>(wh - 1) : 1.0f;
    *nx = static_cast<float>(cursor_x_) / denom_w;
    *ny = static_cast<float>(cursor_y_) / denom_h;
}

void GamepadModule::clamp_to_window(int *wx, int *wy) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    if (ww > 0) *wx = std::max(0, std::min(*wx, ww - 1));
    if (wh > 0) *wy = std::max(0, std::min(*wy, wh - 1));
}

void GamepadModule::warp_cursor(int wx, int wy) {
    clamp_to_window(&wx, &wy);
    cursor_x_ = wx;
    cursor_y_ = wy;
    if (window_) {
        ignore_mouse_until_ = SDL_GetTicks() + 120;
        SDL_WarpMouseInWindow(window_, wx, wy);
    }

    /* Keep the guest touch in sync while A is held — D-pad snaps and stick
     * moves both go through here, so drag/hold gestures track the cursor. */
    if (a_held_ && !overlay_open()) {
        int tx = 0;
        int ty = 0;
        if (map_window_to_touch(cursor_x_, cursor_y_, &tx, &ty))
            pvz2_input_push_touch(PVZ2_TOUCH_MOVE, tx, ty);
    }
}

int GamepadModule::map_window_to_touch(int wx, int wy, int *tx, int *ty) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float tw = static_cast<float>(pvz2_touch_space_width());
    const float th = static_cast<float>(pvz2_touch_space_height());
    if (ww <= 0 || wh <= 0 || tw <= 0.0f || th <= 0.0f) return 0;
    if (wx < 0 || wy < 0 || wx >= ww || wy >= wh) return 0;

    const float denom_w = (ww > 1) ? static_cast<float>(ww - 1) : 1.0f;
    const float denom_h = (wh > 1) ? static_cast<float>(wh - 1) : 1.0f;
    *tx = static_cast<int>((static_cast<float>(wx) / denom_w) * (tw - 1.0f) + 0.5f);
    *ty = static_cast<int>((static_cast<float>(wy) / denom_h) * (th - 1.0f) + 0.5f);
    return 1;
}

int GamepadModule::map_norm_to_touch(float nx, float ny, int *tx, int *ty) const {
    const float tw = static_cast<float>(pvz2_touch_space_width());
    const float th = static_cast<float>(pvz2_touch_space_height());
    if (tw <= 0.0f || th <= 0.0f) return 0;

    nx = clamp01(nx);
    ny = clamp01(ny);
    *tx = static_cast<int>(nx * (tw - 1.0f) + 0.5f);
    *ty = static_cast<int>(ny * (th - 1.0f) + 0.5f);
    return 1;
}

void GamepadModule::get_default_menu_pos(MenuId menu, int *wx, int *wy) const {
    if (menu == MenuId::Board) {
        if (const FocusPoint *fp = find_board_cell(1, 1)) {
            norm_to_window(fp->nx, fp->ny, wx, wy);
            return;
        }
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == MenuId::Board) {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
    } else if (menu == MenuId::Left) {
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == MenuId::Left && fp.name == "plant_1") {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == MenuId::Left) {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
    } else {
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == menu) {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
    }

    norm_to_window(0.5f, 0.5f, wx, wy);
}

void GamepadModule::set_focus_index(int idx) {
    if (idx < 0 || idx >= static_cast<int>(focus_points_.size())) return;

    const int from = static_cast<int>(active_menu_);
    menu_last_x_[from] = cursor_x_;
    menu_last_y_[from] = cursor_y_;
    menu_last_focus_[from] = current_focus_;
    menu_visited_[from] = true;

    current_focus_ = idx;
    snap_nav_ = true;
    const FocusPoint &fp = focus_points_[static_cast<size_t>(idx)];
    active_menu_ = fp.menu;

    int wx = 0;
    int wy = 0;
    norm_to_window(fp.nx, fp.ny, &wx, &wy);
    warp_cursor(wx, wy);

    const int to = static_cast<int>(active_menu_);
    menu_visited_[to] = true;
    menu_last_x_[to] = wx;
    menu_last_y_[to] = wy;
    menu_last_focus_[to] = idx;
}

int GamepadModule::find_focus_by_name(const char *name) const {
    if (!name) return -1;
    for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
        if (focus_points_[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

int GamepadModule::find_nearest_focus(float nx, float ny, MenuId only_menu, bool filter_menu) const {
    int best = -1;
    float best_d = 1.0e9f;
    for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
        const FocusPoint &fp = focus_points_[static_cast<size_t>(i)];
        if (filter_menu && fp.menu != only_menu) continue;
        const float dx = fp.nx - nx;
        const float dy = fp.ny - ny;
        const float d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void GamepadModule::goto_menu(MenuId target) {
    const int from = static_cast<int>(active_menu_);
    menu_last_x_[from] = cursor_x_;
    menu_last_y_[from] = cursor_y_;
    menu_last_focus_[from] = current_focus_;
    menu_visited_[from] = true;

    int idx = -1;
    const int to = static_cast<int>(target);
    if (menu_visited_[to] && menu_last_focus_[to] >= 0 &&
        menu_last_focus_[to] < static_cast<int>(focus_points_.size()) &&
        focus_points_[static_cast<size_t>(menu_last_focus_[to])].menu == target) {
        idx = menu_last_focus_[to];
    } else if (target == MenuId::Board) {
        idx = find_focus_by_name("position_1_1");
        if (idx < 0) idx = find_nearest_focus(0.5f, 0.5f, MenuId::Board, true);
    } else if (target == MenuId::Left) {
        idx = find_focus_by_name("plant_1");
        if (idx < 0) idx = find_nearest_focus(0.05f, 0.16f, MenuId::Left, true);
    } else {
        idx = find_nearest_focus(0.5f, 0.5f, target, true);
    }

    if (idx < 0) {
        /* Fallback: any point tagged with that menu. */
        for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
            if (focus_points_[static_cast<size_t>(i)].menu == target) {
                idx = i;
                break;
            }
        }
    }

    if (idx >= 0) {
        set_focus_index(idx);
    } else {
        active_menu_ = target;
        int wx = 0, wy = 0;
        get_default_menu_pos(target, &wx, &wy);
        warp_cursor(wx, wy);
        menu_visited_[to] = true;
        menu_last_x_[to] = wx;
        menu_last_y_[to] = wy;
    }
}

void GamepadModule::dpad_jump(DpadDir dir) {
    if (dir == DpadDir::None || focus_points_.empty()) return;

    /* Navigate from the focused node (exact coords), not the pixel cursor —
     * warping rounds to integers and euclidean-from-cursor caused stuck loops
     * and jumps into the board from the top bar. */
    float cnx = 0.0f;
    float cny = 0.0f;
    int from_idx = current_focus_;
    if (from_idx < 0 || from_idx >= static_cast<int>(focus_points_.size())) {
        cursor_to_norm(&cnx, &cny);
        from_idx = find_nearest_focus(cnx, cny, MenuId::Top, false);
        if (from_idx >= 0) {
            cnx = focus_points_[static_cast<size_t>(from_idx)].nx;
            cny = focus_points_[static_cast<size_t>(from_idx)].ny;
            current_focus_ = from_idx;
            active_menu_ = focus_points_[static_cast<size_t>(from_idx)].menu;
        } else {
            return;
        }
    } else {
        cnx = focus_points_[static_cast<size_t>(from_idx)].nx;
        cny = focus_points_[static_cast<size_t>(from_idx)].ny;
    }

    /* Must move clearly along the pressed axis; heavy penalty on the cross axis
     * so a vertical plant column / horizontal top row wins over diagonal board hits. */
    static const float kEps = 0.015f;
    static const float kCrossWeight = 5.0f;

    int best = -1;
    float best_score = 1.0e9f;

    for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
        if (i == from_idx) continue;
        const FocusPoint &fp = focus_points_[static_cast<size_t>(i)];
        const float dx = fp.nx - cnx;
        const float dy = fp.ny - cny;

        float primary = 0.0f;
        float cross = 0.0f;
        switch (dir) {
            case DpadDir::Left:
                if (dx >= -kEps) continue;
                primary = -dx;
                cross = std::fabs(dy);
                break;
            case DpadDir::Right:
                if (dx <= kEps) continue;
                primary = dx;
                cross = std::fabs(dy);
                break;
            case DpadDir::Up:
                if (dy >= -kEps) continue;
                primary = -dy;
                cross = std::fabs(dx);
                break;
            case DpadDir::Down:
                if (dy <= kEps) continue;
                primary = dy;
                cross = std::fabs(dx);
                break;
            default:
                continue;
        }

        const float score = primary + kCrossWeight * cross;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }

    if (best >= 0) set_focus_index(best);
}

void GamepadModule::inject_touch_down_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);
    int tx = 0;
    int ty = 0;
    if (!map_window_to_touch(cursor_x_, cursor_y_, &tx, &ty)) return;
    pvz2_input_push_touch(PVZ2_TOUCH_DOWN, tx, ty);
}

void GamepadModule::inject_touch_up_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);
    int tx = 0;
    int ty = 0;
    if (!map_window_to_touch(cursor_x_, cursor_y_, &tx, &ty)) return;
    pvz2_input_push_touch(PVZ2_TOUCH_UP, tx, ty);
}

void GamepadModule::inject_ui_mouse_down_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);

    SDL_Event down;
    std::memset(&down, 0, sizeof(down));
    down.type = SDL_MOUSEBUTTONDOWN;
    down.button.timestamp = SDL_GetTicks();
    down.button.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    down.button.which = 0;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.state = SDL_PRESSED;
    down.button.clicks = 1;
    down.button.x = cursor_x_;
    down.button.y = cursor_y_;
    if (achievements_screen_is_open())
        achievements_screen_handle_event(&down);
    else
        config_screen_handle_event(&down);
}

void GamepadModule::inject_ui_mouse_up_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);

    SDL_Event up;
    std::memset(&up, 0, sizeof(up));
    up.type = SDL_MOUSEBUTTONUP;
    up.button.timestamp = SDL_GetTicks();
    up.button.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    up.button.which = 0;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.state = SDL_RELEASED;
    up.button.clicks = 1;
    up.button.x = cursor_x_;
    up.button.y = cursor_y_;
    if (achievements_screen_is_open())
        achievements_screen_handle_event(&up);
    else
        config_screen_handle_event(&up);
}

void GamepadModule::on_button_a_press() {
    if (a_held_) return;
    a_held_ = true;
    a_held_was_snap_ = snap_nav_ && active_menu_ != MenuId::Board;

    if (overlay_open()) {
        inject_ui_mouse_down_at_cursor();
        return;
    }
    inject_touch_down_at_cursor();
}

void GamepadModule::on_button_a_release() {
    if (!a_held_) return;
    a_held_ = false;

    if (overlay_open()) {
        inject_ui_mouse_up_at_cursor();
        a_held_was_snap_ = false;
        return;
    }

    inject_touch_up_at_cursor();

    /* Auto-return to the board only after releasing A, and only when the
     * press started from a fixed D-pad / L-R focus (not free analog aim). */
    if (a_held_was_snap_) {
        a_held_was_snap_ = false;
        const int from = static_cast<int>(active_menu_);
        menu_last_focus_[from] = current_focus_;
        menu_last_x_[from] = cursor_x_;
        menu_last_y_[from] = cursor_y_;
        menu_visited_[from] = true;
        goto_menu(MenuId::Board);
    }
}

void GamepadModule::tap_bind(const char *bind_name) {
    if (!bind_name || !bind_name[0]) return;
    if (SDL_strcasecmp(bind_name, "NULL") == 0) return;

    const char *lookup = bind_name;
    if (std::strncmp(bind_name, "bind_", 5) == 0) lookup = bind_name + 5;

    const pvz2_config_t *cfg = pvz2_config();
    const pvz2_shortcut_binding_t *shortcut = nullptr;
    for (unsigned i = 0; i < cfg->shortcut_count; ++i) {
        if (std::strcmp(cfg->shortcuts[i].name, lookup) == 0) {
            shortcut = &cfg->shortcuts[i];
            break;
        }
    }
    if (!shortcut) return;

    float nx = 0.0f;
    float ny = 0.0f;
    if (use_43_ && shortcut->has_43) {
        nx = shortcut->x43;
        ny = shortcut->y43;
    } else {
        nx = shortcut->x;
        ny = shortcut->y;
    }

    int tx = 0;
    int ty = 0;
    if (!map_norm_to_touch(nx, ny, &tx, &ty)) return;
    pvz2_input_push_touch(PVZ2_TOUCH_DOWN, tx, ty);
    pvz2_input_push_touch(PVZ2_TOUCH_UP, tx, ty);
}

DpadDir GamepadModule::dpad_from_button(Uint8 button) const {
    switch (static_cast<SDL_GameControllerButton>(button)) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return DpadDir::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return DpadDir::Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return DpadDir::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return DpadDir::Right;
        default:
            return DpadDir::None;
    }
}

void GamepadModule::on_dpad_press(DpadDir dir) {
    if (dir == DpadDir::None) return;
    dpad_held_ = dir;
    dpad_repeat_timer_ = 0.0f;
    dpad_repeat_interval_ = kDpadRepeatInitialSec;
    nav_step(dir);
}

void GamepadModule::on_dpad_release(DpadDir dir) {
    if (dpad_held_ == dir) dpad_held_ = DpadDir::None;
}

int GamepadModule::handle_controller_button(const SDL_Event *e) {
    if (!controller_ready()) return 0;
    if (!is_our_controller_instance(e->cbutton.which)) return 0;

    const bool down = e->type == SDL_CONTROLLERBUTTONDOWN;
    const SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(e->cbutton.button);

    /* Overlay UI: cursor + A hold-click + Back (same as Escape) only. */
    if (overlay_open()) {
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (down) on_button_a_press();
            else on_button_a_release();
            return 1;
        }
        if (!down) return 1;
        if (button == SDL_CONTROLLER_BUTTON_BACK) {
            config_screen_request_escape();
            return 1;
        }
        return 1;
    }

    /* R3 switches lawn mode on and off while in a level. */
    if (button == SDL_CONTROLLER_BUTTON_RIGHTSTICK && pvz2_game_is_in_level()) {
        if (down) {
            grid_enabled_ = !grid_enabled_;
            rumble_now(0.15f, 0.25f, 60);
            if (pvz2_config()->verbose) pvz2_log("[gamepad] lawn mode %s", grid_enabled_ ? "on" : "off");
        }
        return 1;
    }
    if (grid_active() && handle_grid_button(button, down)) return 1;

    const DpadDir dpad = dpad_from_button(e->cbutton.button);
    if (dpad != DpadDir::None) {
        if (down) {
            on_dpad_press(dpad);
        } else {
            on_dpad_release(dpad);
        }
        return 1;
    }

    if (button == SDL_CONTROLLER_BUTTON_A) {
        if (down) on_button_a_press();
        else on_button_a_release();
        return 1;
    }

    if (!down) return 0;

    switch (button) {
        case SDL_CONTROLLER_BUTTON_BACK:
            config_screen_request_escape();
            return 1;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            goto_menu(MenuId::Top);
            return 1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            goto_menu(MenuId::Bottom);
            return 1;
        default:
            break;
    }

    const auto it = bind_by_button_.find(button);
    if (it != bind_by_button_.end()) {
        tap_bind(it->second.c_str());
        return 1;
    }

    return 0;
}

int GamepadModule::handle_event(const SDL_Event *e) {
    if (!enabled_) return 0;

    switch (e->type) {
        case SDL_CONTROLLERDEVICEADDED:
            /* Rescan: wireless receivers add/remove slots dynamically. */
            if (!controller_) try_open_controller();
            return 0;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (controller_ && is_our_controller_instance(e->cdevice.which)) {
                if (a_held_) on_button_a_release();
                if (pvz2_config()->verbose) pvz2_log("[gamepad] disconnected (port %d)", port_);
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
            }
            return 0;
        case SDL_CONTROLLERBUTTONDOWN:
            if (controller_ready() && is_our_controller_instance(e->cbutton.which)) note_controller_used();
            return handle_controller_button(e);
        case SDL_CONTROLLERBUTTONUP:
            return handle_controller_button(e);
        /* Mouse or keyboard in use: the controller's lawn mode steps aside (box, prompts and pickup taps
         * all stop) until the controller is touched again. */
        case SDL_MOUSEMOTION:
            if (SDL_GetTicks() < ignore_mouse_until_) return 0;
            if (std::abs(e->motion.xrel) + std::abs(e->motion.yrel) < 3) return 0;
            controller_mode_ = false;
            return 0;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEWHEEL:
        case SDL_KEYDOWN:
            controller_mode_ = false;
            return 0;
        default:
            return 0;
    }
}

void GamepadModule::note_controller_used() {
    controller_mode_ = true;
}

void GamepadModule::tick_left_stick(float dt_sec) {
    Sint16 ax = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 ay = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);

    float sx = axis_to_float(ax);
    float sy = axis_to_float(ay);

    if (std::fabs(sx) < stick_deadzone_) sx = 0.0f;
    if (std::fabs(sy) < stick_deadzone_) sy = 0.0f;
    if (sx == 0.0f && sy == 0.0f) return;

    /* Free aim: leave snap/D-pad focus mode so A no longer teleports to board. */
    snap_nav_ = false;
    current_focus_ = -1;

    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float speed = stick_speed_ * dt_sec;
    cursor_x_ += static_cast<int>(sx * speed * static_cast<float>(ww));
    cursor_y_ += static_cast<int>(sy * speed * static_cast<float>(wh));
    warp_cursor(cursor_x_, cursor_y_);
}

void GamepadModule::apply_border_pad(int touch_w, int touch_h, int *x, int *y) const {
    int min_x = kDragBorderPad;
    int max_x = touch_w - 1 - kDragBorderPad;
    int min_y = kDragBorderPad;
    int max_y = touch_h - 1 - kDragBorderPad;

    if (min_x > max_x) {
        min_x = 0;
        max_x = touch_w - 1;
    }
    if (min_y > max_y) {
        min_y = 0;
        max_y = touch_h - 1;
    }

    if (*x < min_x) *x = min_x;
    if (*x > max_x) *x = max_x;
    if (*y < min_y) *y = min_y;
    if (*y > max_y) *y = max_y;
}

void GamepadModule::start_drag_gesture(int dir_x, int dir_y) {
    if (drag_.active) return;

    const int touch_w = static_cast<int>(pvz2_touch_space_width());
    const int touch_h = static_cast<int>(pvz2_touch_space_height());
    if (touch_w <= 1 || touch_h <= 1) return;

    const int center_x = (touch_w * 50) / 100;
    const int center_y = (touch_h * 50) / 100;
    const int top_y = (touch_h * 20) / 100;
    const int bottom_y = (touch_h * 80) / 100;
    const int left_x = (touch_w * 20) / 100;
    const int right_x = (touch_w * 80) / 100;

    drag_.horizontal = std::abs(dir_x) > std::abs(dir_y);
    if (drag_.horizontal) {
        drag_.start_y = center_y;
        drag_.end_y = center_y;
        drag_.start_x = center_x;
        drag_.end_x = (dir_x > 0) ? left_x : right_x;
        apply_border_pad(touch_w, touch_h, &drag_.start_x, &drag_.start_y);
        apply_border_pad(touch_w, touch_h, &drag_.end_x, &drag_.end_y);
    } else {
        drag_.start_x = center_x;
        drag_.end_x = center_x;
        drag_.start_y = center_y;
        drag_.end_y = (dir_y < 0) ? bottom_y : top_y;
        apply_border_pad(touch_w, touch_h, &drag_.start_x, &drag_.start_y);
        apply_border_pad(touch_w, touch_h, &drag_.end_x, &drag_.end_y);
    }

    if (drag_.start_x == drag_.end_x && drag_.start_y == drag_.end_y) return;

    drag_.active = true;
    drag_.step = 0;
}

void GamepadModule::process_drag_gesture() {
    if (!drag_.active) return;

    if (drag_.step == 0) {
        pvz2_input_push_touch(PVZ2_TOUCH_DOWN, drag_.start_x, drag_.start_y);
    } else if (drag_.step <= kDragRepeats) {
        int cx = drag_.start_x;
        int cy = drag_.start_y;
        if (drag_.horizontal) {
            cx = drag_.start_x +
                 ((drag_.end_x - drag_.start_x) * drag_.step) / kDragRepeats;
        } else {
            cy = drag_.start_y +
                 ((drag_.end_y - drag_.start_y) * drag_.step) / kDragRepeats;
        }
        pvz2_input_push_touch(PVZ2_TOUCH_MOVE, cx, cy);
    } else {
        pvz2_input_push_touch(PVZ2_TOUCH_UP, drag_.end_x, drag_.end_y);
        drag_.active = false;
        drag_.step = 0;
        drag_cooldown_ = kDragCooldownSec;
        return;
    }

    drag_.step++;
}

void GamepadModule::tick_right_stick(float dt_sec) {
    if (drag_cooldown_ > 0.0f) drag_cooldown_ = std::max(0.0f, drag_cooldown_ - dt_sec);

    process_drag_gesture();
    if (drag_.active) return;

    Sint16 ax = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTX);
    Sint16 ay = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
    float sx = axis_to_float(ax);
    float sy = axis_to_float(ay);

    if (std::fabs(sx) < stick_deadzone_ && std::fabs(sy) < stick_deadzone_) return;
    if (drag_cooldown_ > 0.0f) return;

    /* Hold to repeat: each finished gesture + cooldown starts another while
     * the stick stays deflected (no need to recenter). */
    int dir_x = 0;
    int dir_y = 0;
    if (std::fabs(sy) >= std::fabs(sx)) {
        dir_y = (sy < 0.0f) ? -1 : 1;
    } else {
        dir_x = (sx < 0.0f) ? -1 : 1;
    }

    start_drag_gesture(dir_x, dir_y);
}

void GamepadModule::tick_right_stick_ui(float dt_sec) {
    /* Config list scroll: same effect as mouse wheel (one item per pulse). */
    if (drag_cooldown_ > 0.0f) drag_cooldown_ = std::max(0.0f, drag_cooldown_ - dt_sec);

    Sint16 ay = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
    float sy = axis_to_float(ay);
    if (std::fabs(sy) < stick_deadzone_) return;
    if (drag_cooldown_ > 0.0f) return;

    SDL_Event wheel;
    std::memset(&wheel, 0, sizeof(wheel));
    wheel.type = SDL_MOUSEWHEEL;
    wheel.wheel.timestamp = SDL_GetTicks();
    wheel.wheel.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    wheel.wheel.which = 0;
    /* Stick up (negative Y) → wheel +1 → list scrolls toward top (same as mouse). */
    wheel.wheel.y = (sy < 0.0f) ? 1 : -1;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    if (achievements_screen_is_open())
        achievements_screen_handle_event(&wheel);
    else
        config_screen_handle_event(&wheel);

    drag_cooldown_ = kDragCooldownSec;
}

void GamepadModule::tick_triggers() {
    const Sint16 l2 = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    const Sint16 r2 = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    const bool l2_down = trigger_to_float(l2) >= trigger_threshold_;
    const bool r2_down = trigger_to_float(r2) >= trigger_threshold_;

    if (l2_down && !l2_prev_) goto_menu(MenuId::Left);
    if (r2_down && !r2_prev_) goto_menu(MenuId::Board);

    l2_prev_ = l2_down;
    r2_prev_ = r2_down;
}

void GamepadModule::tick_dpad_repeat(float dt_sec) {
    if (dpad_held_ == DpadDir::None) return;

    dpad_repeat_timer_ += dt_sec;
    if (dpad_repeat_timer_ < dpad_repeat_interval_) return;

    dpad_repeat_timer_ -= dpad_repeat_interval_;
    nav_step(dpad_held_);
    dpad_repeat_interval_ = std::max(kDpadRepeatMinSec, dpad_repeat_interval_ - kDpadRepeatAccelSec);
}

void GamepadModule::tick(float dt_sec) {
    if (!controller_ready()) return;

    /* Keep logical cursor synced with the OS cursor (mouse or prior warp). */
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);

    {
        const float lx = axis_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX));
        const float ly = axis_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY));
        const float rx = axis_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTX));
        const float ry = axis_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY));
        const float lt = trigger_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        const float rt = trigger_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        if (std::max(std::max(std::fabs(lx), std::fabs(ly)), std::max(std::fabs(rx), std::fabs(ry))) > 0.5f ||
            std::max(lt, rt) > trigger_threshold_)
            note_controller_used();
    }

    /* Overlays still need free cursor aim + right-stick list scroll. */
    if (overlay_open()) {
        tick_rumble();
        tick_left_stick(dt_sec);
        tick_right_stick_ui(dt_sec);
        return;
    }

    tick_pending_taps(dt_sec);
    tick_rumble();
    if (grid_active()) {
        tick_grid(dt_sec);
        tick_dpad_repeat(dt_sec);
        return;
    }
    mode_ = 0;
    chooser_was_active_ = false;
    if (grid_was_active_) {
        grid_was_active_ = false;
        pending_taps_.clear();
        aiming_ = false;
    }

    tick_left_stick(dt_sec);
    tick_right_stick(dt_sec);
    tick_triggers();
    tick_dpad_repeat(dt_sec);
}


/* --------------------------------------------------------------------------
 * Lawn mode -- in a level, the controller works like the Xbox 360 game: a box moves tile by tile over
 * the lawn (D-pad or left stick), LB/RB pick a seed packet, A plants it on the box, X digs up with the
 * shovel, Y gives plant food, B clears the seed choice. Sun and coins are picked up by moving the box
 * over them. R3 switches back to the free cursor (power-ups need it), and again to return. In the seed
 * chooser the same box walks over the cards and the seed bank: A picks or returns one, Y starts.
 *
 * Everything is done with the touches a finger would make (tap the packet, then the tile), so the game
 * itself decides what happens.
 *
 * Geometry. The game lays its UI out in units of the screen HEIGHT, with the seed bank and the chooser
 * anchored to the left edge and the "let's rock" button to the right one; the numbers below were measured
 * on 1920x1080 screenshots and are fractions of the height. The [board] points in config.ini are tap
 * targets rounded to two decimals, which drifts by ~10 px towards the bottom row; on 16:9 the measured
 * lawn grid is used instead, on 4:3 a straight-line fit of the configured cells.
 * ------------------------------------------------------------------------ */

static const float kTapGapSec = 0.07f;    /* between "pick the packet" and "tap the tile" */
static const float kSweepEverySec = 0.25f;
static const float kStickTileThreshold = 0.55f;

/* Seed bank (fractions of the height; x from the left edge). */
static const float kBankX = 0.0843f;
static const float kPacketW = 0.150f, kPacketH = 0.0926f;
static const float kBankY0 = 0.162f, kBankStep = 0.0983f;        /* regular bank: 8 slots */
static const float kConveyorY0 = 0.048f, kConveyorStep = 0.1046f; /* conveyor belt: packets stack from the top */
static const int kBankSlots = 8, kConveyorSlots = 9;

/* Lawn on a 16:9 screen (fractions of width / height). */
static const float kLawnX0 = 0.3630f, kLawnStepX = 0.07193f;
static const float kLawnY0 = 0.2083f, kLawnStepY = 0.1528f;

/* Seed chooser cards (fractions of the height; x from the left edge), 5 columns, 4 rows on screen. */
static const float kCardX0 = 0.2704f, kCardStepX = 0.1648f;
static const float kCardY0 = 0.5037f, kCardStepY = 0.1074f;
static const float kCardW = 0.148f, kCardH = 0.0944f;
static const int kCardCols = 5, kCardRows = 4;
/* "Let's rock": right edge minus 0.1528 of the height, at 0.94 of the height. */
static const float kRockFromRight = 0.1528f, kRockY = 0.94f;
/* Top-right buttons (level and chooser): pause, and the level's speed-up. */
static const float kPauseFromRight = 0.0602f, kSpeedFromRight = 0.162f, kTopButtonsY = 0.0556f;
/* Seed chooser "pick the same plants again" button, bottom left. */
static const float kReselectX = 0.0833f, kReselectY = 0.944f;
static const float kIntroWaitSec = 3.0f;
static const float kConveyorSlideSpeed = 0.9f; /* screen heights per second at full stick */

extern "C" int pvz2_game_in_seed_chooser(void);
extern "C" int pvz2_game_level_has_conveyor(void);

bool GamepadModule::level_active() const {
    return grid_enabled_ && controller_mode_ && controller_ready() && !overlay_open() && pvz2_game_is_in_level() &&
           !pvz2_game_is_paused() && grid_rows_ > 0 && grid_cols_ > 0;
}

bool GamepadModule::chooser_active() const {
    return grid_enabled_ && controller_mode_ && controller_ready() && !overlay_open() &&
           pvz2_game_in_seed_chooser() && !pvz2_game_is_paused();
}

bool GamepadModule::grid_active() const { return level_active() || chooser_active(); }

int GamepadModule::current_mode() const {
    if (chooser_active()) return 2;
    if (level_active()) return 1;
    return 0;
}

bool GamepadModule::grid_ready() const {
    const int m = current_mode();
    return m != 0 && m == mode_ && intro_wait_ <= 0.0f;
}

float GamepadModule::right_edge_nx(float from_right_h) const { return 1.0f - from_right_h / aspect(); }

float GamepadModule::aspect() const {
    int ww = 0, wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0 || wh <= 0) return 16.0f / 9.0f;
    return static_cast<float>(ww) / static_cast<float>(wh);
}

int GamepadModule::bank_slots() const {
    return pvz2_game_level_has_conveyor() && !chooser_active() ? kConveyorSlots : kBankSlots;
}

void GamepadModule::bank_slot_center(int slot, bool conveyor, float *nx, float *ny) const {
    *nx = kBankX / aspect();
    *ny = conveyor ? kConveyorY0 + kConveyorStep * static_cast<float>(slot - 1)
                   : kBankY0 + kBankStep * static_cast<float>(slot - 1);
}

void GamepadModule::bank_packet_size_px(float H, float *pw, float *ph) const {
    *pw = kPacketW * H;
    *ph = kPacketH * H;
}

void GamepadModule::grid_cell_norm(float *nx, float *ny) const {
    if (!use_43_) {
        *nx = kLawnX0 + kLawnStepX * static_cast<float>(grid_col_ - 1);
        *ny = kLawnY0 + kLawnStepY * static_cast<float>(grid_row_ - 1);
        return;
    }
    *nx = grid_ax_ + grid_bx_ * static_cast<float>(grid_col_);
    *ny = grid_ay_ + grid_by_ * static_cast<float>(grid_row_);
}

void GamepadModule::grid_tile_size_norm(float *tw, float *th) const {
    if (!use_43_) {
        *tw = kLawnStepX;
        *th = kLawnStepY;
        return;
    }
    *tw = std::fabs(grid_bx_);
    *th = std::fabs(grid_by_);
}

void GamepadModule::chooser_cell_center(float *nx, float *ny) const {
    if (chooser_col_ == 0) {
        bank_slot_center(chooser_row_, false, nx, ny);
        return;
    }
    *nx = (kCardX0 + kCardStepX * static_cast<float>(chooser_col_ - 1)) / aspect();
    *ny = kCardY0 + kCardStepY * static_cast<float>(chooser_row_ - 1);
}

void GamepadModule::queue_tap(float delay_sec, float nx, float ny) {
    pending_taps_.push_back({delay_sec, -1, nx, ny});
}

void GamepadModule::queue_touch(float delay_sec, int type, float nx, float ny) {
    pending_taps_.push_back({delay_sec, type, nx, ny});
}

void GamepadModule::queue_tap_bind(float delay_sec, const char *name) {
    const int idx = find_focus_by_name(name);
    if (idx < 0) return;
    const FocusPoint &fp = focus_points_[static_cast<size_t>(idx)];
    queue_tap(delay_sec, fp.nx, fp.ny);
}

/* A slow drag with a pause before letting go, so the list moves by the distance dragged and does not
 * fling on. */
void GamepadModule::queue_scroll_drag(float from_ny, float to_ny, float nx) {
    const int kSteps = 8;
    const float kStepSec = 0.02f;
    queue_touch(0.0f, PVZ2_TOUCH_DOWN, nx, from_ny);
    for (int i = 1; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / kSteps;
        queue_touch(kStepSec * i, PVZ2_TOUCH_MOVE, nx, from_ny + (to_ny - from_ny) * t);
    }
    for (int i = 1; i <= 6; ++i) queue_touch(kStepSec * (kSteps + i), PVZ2_TOUCH_MOVE, nx, to_ny);
    queue_touch(kStepSec * (kSteps + 7), PVZ2_TOUCH_UP, nx, to_ny);
}

void GamepadModule::tick_pending_taps(float dt_sec) {
    /* Each entry's delay counts from when it was queued; all of them tick together. */
    for (PendingTap &t : pending_taps_) t.delay -= dt_sec;
    while (!pending_taps_.empty() && pending_taps_.front().delay <= 0.0f) {
        const PendingTap t = pending_taps_.front();
        pending_taps_.erase(pending_taps_.begin());
        int tx = 0, ty = 0;
        if (!map_norm_to_touch(t.nx, t.ny, &tx, &ty)) continue;
        if (t.type < 0) {
            pvz2_input_push_touch(PVZ2_TOUCH_DOWN, tx, ty);
            pvz2_input_push_touch(PVZ2_TOUCH_UP, tx, ty);
        } else {
            pvz2_input_push_touch(t.type, tx, ty);
        }
    }
}

/* Taps the tile under the box at its centre and four points around it: a sun or coin lying anywhere
 * on the tile is picked up, and a tap on bare lawn or on a plant does nothing when no packet, shovel
 * or plant food is armed (and none is: those are only armed for the instant of an A / X / Y press). */
void GamepadModule::sweep_tile_for_pickups() {
    float cx = 0.0f, cy = 0.0f, tw = 0.0f, th = 0.0f;
    grid_cell_norm(&cx, &cy);
    grid_tile_size_norm(&tw, &th);
    static const float kOff[5][2] = {{0, 0}, {-0.3f, -0.3f}, {0.3f, -0.3f}, {-0.3f, 0.3f}, {0.3f, 0.3f}};
    for (const auto &o : kOff) queue_tap(0.0f, cx + o[0] * tw, cy + o[1] * th);
}

void GamepadModule::grid_move(DpadDir dir) {
    int r = grid_row_, c = grid_col_;
    switch (dir) {
        case DpadDir::Up: --r; break;
        case DpadDir::Down: ++r; break;
        case DpadDir::Left: --c; break;
        case DpadDir::Right: ++c; break;
        default: return;
    }
    if (r < 1 || r > grid_rows_ || c < 1 || c > grid_cols_) return;
    grid_row_ = r;
    grid_col_ = c;
    sweep_timer_ = 0.0f;
    if (!aiming_) sweep_tile_for_pickups();
}

void GamepadModule::chooser_move(DpadDir dir) {
    if (chooser_col_ == 0) {
        /* In the seed bank: up/down along the 8 slots, right back into the cards at the nearest row. */
        if (dir == DpadDir::Up && chooser_row_ > 1) --chooser_row_;
        else if (dir == DpadDir::Down && chooser_row_ < kBankSlots) ++chooser_row_;
        else if (dir == DpadDir::Right) {
            const float y = kBankY0 + kBankStep * static_cast<float>(chooser_row_ - 1);
            int best = 1;
            for (int r = 1; r <= kCardRows; ++r)
                if (std::fabs(kCardY0 + kCardStepY * (r - 1) - y) < std::fabs(kCardY0 + kCardStepY * (best - 1) - y))
                    best = r;
            chooser_col_ = 1;
            chooser_row_ = best;
        }
        return;
    }
    const float list_x = (kCardX0 + kCardStepX * 2.0f) / aspect(); /* drag in the middle column */
    switch (dir) {
        case DpadDir::Left:
            if (chooser_col_ > 1) {
                --chooser_col_;
            } else {
                /* Into the seed bank, at the slot nearest this row. */
                const float y = kCardY0 + kCardStepY * static_cast<float>(chooser_row_ - 1);
                int best = 1;
                for (int k = 1; k <= kBankSlots; ++k)
                    if (std::fabs(kBankY0 + kBankStep * (k - 1) - y) < std::fabs(kBankY0 + kBankStep * (best - 1) - y))
                        best = k;
                chooser_col_ = 0;
                chooser_row_ = best;
            }
            break;
        case DpadDir::Right:
            if (chooser_col_ < kCardCols) ++chooser_col_;
            break;
        case DpadDir::Up:
            if (chooser_row_ > 1) {
                --chooser_row_;
            } else {
                /* Scroll the list down one row; the box stays on the top row. */
                const float y = kCardY0 + kCardStepY * 0.5f;
                queue_scroll_drag(y, y + kCardStepY, list_x);
            }
            break;
        case DpadDir::Down:
            if (chooser_row_ < kCardRows) {
                ++chooser_row_;
            } else {
                const float y = kCardY0 + kCardStepY * 2.5f;
                queue_scroll_drag(y, y - kCardStepY, list_x);
            }
            break;
        default:
            break;
    }
}

void GamepadModule::nav_step(DpadDir dir) {
    if (chooser_active()) chooser_move(dir);
    else if (level_active()) grid_move(dir);
    else dpad_jump(dir);
}

int GamepadModule::handle_grid_button(SDL_GameControllerButton button, bool down) {
    /* Start pauses in a level and in the chooser, even during the intro wait. */
    if (button == SDL_CONTROLLER_BUTTON_START) {
        if (down) {
            queue_tap(0.0f, right_edge_nx(kPauseFromRight), kTopButtonsY);
            rumble_now(0.10f, 0.15f, 50);
        }
        return 1;
    }
    if (!grid_ready()) return 1; /* swallowed while the camera pans in */
    if (chooser_active()) return handle_chooser_button(button, down);
    float cx = 0.0f, cy = 0.0f;
    grid_cell_norm(&cx, &cy);
    const bool packets = pvz2_game_has_seed_packets() != 0;
    const bool conveyor = pvz2_game_level_has_conveyor() != 0;
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
            const DpadDir d = dpad_from_button(static_cast<Uint8>(button));
            if (down) on_dpad_press(d);
            else on_dpad_release(d);
            return 1;
        }
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (down && packets) bank_step(button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? 1 : -1);
            return 1;
        case SDL_CONTROLLER_BUTTON_A:
            if (down) {
                if (aiming_) {
                    /* A also fires an aimed plant at the box. */
                    queue_tap(0.0f, cx, cy);
                    aiming_ = false;
                    rumble_now(0.40f, 0.30f, 120);
                } else if (seed_slot_ > 0 && packets) {
                    float px = 0.0f, py = 0.0f;
                    bank_slot_center(seed_slot_, conveyor, &px, &py);
                    if (conveyor) py = conveyor_y_;
                    queue_tap(0.0f, px, py);
                    queue_tap(kTapGapSec, cx, cy);
                    rumble_now(0.25f, 0.20f, 70);
                } else {
                    queue_tap(0.0f, cx, cy); /* no packet chosen: just tap the tile */
                    rumble_now(0.15f, 0.10f, 50);
                }
            }
            return 1;
        case SDL_CONTROLLER_BUTTON_X:
            if (down && !aiming_) {
                queue_tap_bind(0.0f, "shovel");
                queue_tap(kTapGapSec, cx, cy);
                rumble_now(0.30f, 0.15f, 90);
            }
            return 1;
        case SDL_CONTROLLER_BUTTON_Y:
            if (down && !aiming_) {
                queue_tap_bind(0.0f, "plant_food");
                queue_tap(kTapGapSec, cx, cy);
                rumble_now(0.45f, 0.45f, 180);
            }
            return 1;
        case SDL_CONTROLLER_BUTTON_B:
            if (down) {
                if (aiming_) {
                    /* Tapping the picked plant again puts it down without firing. */
                    const int r = grid_row_, c = grid_col_;
                    grid_row_ = aim_row_;
                    grid_col_ = aim_col_;
                    float ax = 0.0f, ay = 0.0f;
                    grid_cell_norm(&ax, &ay);
                    grid_row_ = r;
                    grid_col_ = c;
                    queue_tap(0.0f, ax, ay);
                    aiming_ = false;
                } else {
                    seed_slot_ = 0;
                }
            }
            return 1;
        default:
            return 0;
    }
}

int GamepadModule::handle_chooser_button(SDL_GameControllerButton button, bool down) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
            const DpadDir d = dpad_from_button(static_cast<Uint8>(button));
            if (down) on_dpad_press(d);
            else on_dpad_release(d);
            return 1;
        }
        case SDL_CONTROLLER_BUTTON_A:
            if (down) {
                /* A card goes into the seed bank; a packet in the bank goes back. */
                float nx = 0.0f, ny = 0.0f;
                chooser_cell_center(&nx, &ny);
                queue_tap(0.0f, nx, ny);
                rumble_now(0.20f, 0.15f, 60);
            }
            return 1;
        case SDL_CONTROLLER_BUTTON_Y:
            if (down) {
                queue_tap(0.0f, right_edge_nx(kRockFromRight), kRockY); /* "Let's rock!" */
                rumble_now(0.35f, 0.35f, 150);
            }
            return 1;
        case SDL_CONTROLLER_BUTTON_X:
            if (down) {
                queue_tap(0.0f, kReselectX / aspect(), kReselectY); /* pick the same plants again */
                rumble_now(0.20f, 0.15f, 60);
            }
            return 1;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (down) chooser_col_ = 0; /* jump to the seed bank */
            return 1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (down && chooser_col_ == 0) {
                chooser_col_ = 1;
                chooser_row_ = 1;
            }
            return 1;
        case SDL_CONTROLLER_BUTTON_B:
            return 1; /* nothing to cancel here; swallowed so it does not tap a stray bind */
        default:
            return 0;
    }
}

/* LB / RB (and the right stick): the next or previous seed packet. On a conveyor belt the frame snaps to
 * the next packet position along the belt from wherever it slid to. */
void GamepadModule::bank_step(int step) {
    if (pvz2_game_level_has_conveyor()) {
        const float top = kConveyorY0, bottom = kConveyorY0 + kConveyorStep * (kConveyorSlots - 1);
        if (seed_slot_ == 0) {
            seed_slot_ = 1;
            conveyor_y_ = step > 0 ? top : bottom;
        } else {
            const float k = (conveyor_y_ - kConveyorY0) / kConveyorStep;
            float next = step > 0 ? std::floor(k + 0.01f) + 1.0f : std::ceil(k - 0.01f) - 1.0f;
            if (next < 0.0f) next = kConveyorSlots - 1;
            if (next > kConveyorSlots - 1) next = 0.0f;
            conveyor_y_ = kConveyorY0 + kConveyorStep * next;
        }
        conveyor_y_ = std::max(top, std::min(bottom, conveyor_y_));
    } else {
        const int n = bank_slots();
        int slot = seed_slot_ == 0 ? (step > 0 ? 1 : n) : seed_slot_ + step;
        if (slot < 1) slot = n;
        if (slot > n) slot = 1;
        seed_slot_ = slot;
    }
    rumble_now(0.0f, 0.12f, 30);
}

/* Right stick up/down: on a conveyor it slides the frame along the belt (to catch a packet still on its
 * way up); with a regular seed bank it steps packet by packet like LB / RB. */
void GamepadModule::tick_bank_right_stick(float dt_sec) {
    if (!pvz2_game_has_seed_packets()) return;
    const float ry = axis_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY));
    if (pvz2_game_level_has_conveyor()) {
        if (std::fabs(ry) < stick_deadzone_) return;
        if (seed_slot_ == 0) seed_slot_ = 1;
        const float top = kConveyorY0, bottom = kConveyorY0 + kConveyorStep * (kConveyorSlots - 1);
        conveyor_y_ = std::max(top, std::min(bottom, conveyor_y_ + ry * kConveyorSlideSpeed * dt_sec));
        return;
    }
    DpadDir d = DpadDir::None;
    if (ry <= -kStickTileThreshold) d = DpadDir::Up;
    else if (ry >= kStickTileThreshold) d = DpadDir::Down;
    if (d != rstick_dir_) {
        rstick_dir_ = d;
        rstick_timer_ = 0.0f;
        rstick_interval_ = kDpadRepeatInitialSec;
        if (d != DpadDir::None) bank_step(d == DpadDir::Down ? 1 : -1);
    } else if (d != DpadDir::None) {
        rstick_timer_ += dt_sec;
        if (rstick_timer_ >= rstick_interval_) {
            rstick_timer_ -= rstick_interval_;
            rstick_interval_ = std::max(kDpadRepeatMinSec, rstick_interval_ - kDpadRepeatAccelSec);
            bank_step(d == DpadDir::Down ? 1 : -1);
        }
    }
}

/* RT: plants you tap and then point somewhere (Banana Launcher, Strawburst-style plants...). The first
 * press taps the plant under the box and starts aiming; the box turns red and stops collecting (a pickup
 * tap would fire it); the next RT or A fires at the box, B puts it down. */
void GamepadModule::tick_aim_trigger(float dt_sec) {
    const bool rt = trigger_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) >=
                    trigger_threshold_;
    if (rt && !rt_prev_) {
        float cx = 0.0f, cy = 0.0f;
        grid_cell_norm(&cx, &cy);
        queue_tap(0.0f, cx, cy);
        if (!aiming_) {
            aiming_ = true;
            aim_row_ = grid_row_;
            aim_col_ = grid_col_;
            aim_timer_ = 0.0f;
            rumble_now(0.20f, 0.30f, 80);
        } else {
            aiming_ = false;
            rumble_now(0.40f, 0.30f, 120);
        }
    }
    rt_prev_ = rt;
    /* A plant that fired by itself on the first tap (nothing to aim) must not keep the box red forever. */
    if (aiming_) {
        aim_timer_ += dt_sec;
        if (aim_timer_ > 8.0f) aiming_ = false;
    }
}

/* Left stick steps one cell at a time, like a held D-pad. */
void GamepadModule::tick_stick_steps(float dt_sec, void (GamepadModule::*step)(DpadDir)) {
    const float sx = axis_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX));
    const float sy = axis_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY));
    DpadDir d = DpadDir::None;
    if (std::fabs(sx) >= kStickTileThreshold || std::fabs(sy) >= kStickTileThreshold) {
        if (std::fabs(sx) > std::fabs(sy)) d = sx > 0 ? DpadDir::Right : DpadDir::Left;
        else d = sy > 0 ? DpadDir::Down : DpadDir::Up;
    }
    if (d != stick_dir_) {
        stick_dir_ = d;
        stick_repeat_timer_ = 0.0f;
        stick_repeat_interval_ = kDpadRepeatInitialSec;
        if (d != DpadDir::None) (this->*step)(d);
    } else if (d != DpadDir::None) {
        stick_repeat_timer_ += dt_sec;
        if (stick_repeat_timer_ >= stick_repeat_interval_) {
            stick_repeat_timer_ -= stick_repeat_interval_;
            stick_repeat_interval_ = std::max(kDpadRepeatMinSec, stick_repeat_interval_ - kDpadRepeatAccelSec);
            (this->*step)(d);
        }
    }
}

void GamepadModule::tick_chooser(float dt_sec) {
    if (!chooser_was_active_) {
        chooser_was_active_ = true;
        chooser_col_ = 1;
        chooser_row_ = 1;
        pending_taps_.clear();
    }
    tick_stick_steps(dt_sec, &GamepadModule::chooser_move);
}

void GamepadModule::tick_grid(float dt_sec) {
    const int m = current_mode();
    if (m != mode_) {
        mode_ = m;
        intro_wait_ = kIntroWaitSec;
        pending_taps_.clear();
    }
    if (intro_wait_ > 0.0f) {
        intro_wait_ -= dt_sec;
        return;
    }
    if (chooser_active()) {
        grid_was_active_ = false;
        tick_chooser(dt_sec);
        return;
    }
    chooser_was_active_ = false;
    if (!grid_was_active_) {
        /* Entering a level: start on the middle row, first column, first packet. */
        grid_was_active_ = true;
        grid_row_ = (grid_rows_ + 1) / 2;
        grid_col_ = 1;
        seed_slot_ = 1;
        conveyor_y_ = kConveyorY0;
        pending_taps_.clear();
        aiming_ = false;
    }
    tick_aim_trigger(dt_sec);
    tick_stick_steps(dt_sec, &GamepadModule::grid_move);
    tick_bank_right_stick(dt_sec);

    /* LT: the game's speed-up button (top right). */
    const bool lt = trigger_to_float(SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) >=
                    trigger_threshold_;
    if (lt && !lt_prev_) {
        queue_tap(0.0f, right_edge_nx(kSpeedFromRight), kTopButtonsY);
        rumble_now(0.10f, 0.20f, 60);
    }
    lt_prev_ = lt;

    /* Keep collecting what lands on the tile while the box rests there. */
    sweep_timer_ += dt_sec;
    if (sweep_timer_ >= kSweepEverySec && !aiming_) {
        sweep_timer_ = 0.0f;
        sweep_tile_for_pickups();
    }
}

/* ---- rumble ---- */

void GamepadModule::rumble_now(float low, float high, unsigned ms) {
    request_rumble(low, high, ms);
}

void GamepadModule::request_rumble(float low, float high, unsigned ms) {
    std::lock_guard<std::mutex> lk(rumble_mu_);
    /* Keep the strongest of what was asked for since the last tick. */
    if (std::max(low, high) >= std::max(rumble_low_, rumble_high_)) {
        rumble_low_ = low;
        rumble_high_ = high;
        rumble_ms_ = ms;
    }
}

void GamepadModule::tick_rumble() {
    float low, high;
    unsigned ms;
    {
        std::lock_guard<std::mutex> lk(rumble_mu_);
        low = rumble_low_;
        high = rumble_high_;
        ms = rumble_ms_;
        rumble_low_ = rumble_high_ = 0.0f;
        rumble_ms_ = 0;
    }
    if (ms == 0 || !controller_ready()) return;
    const Uint32 now = SDL_GetTicks();
    const float strength = std::max(low, high);
    /* A weaker buzz does not cut a stronger one short. */
    if (now < rumble_until_ && strength < rumble_playing_) return;
    rumble_until_ = now + ms;
    rumble_playing_ = strength;
    SDL_GameControllerRumble(controller_, static_cast<Uint16>(clamp01(low) * 65535.0f),
                             static_cast<Uint16>(clamp01(high) * 65535.0f), ms);
}

/* ---- the box, the chosen packet and the button prompts, drawn over the game frame ---- */

static void gl_quad(float x, float y, float w, float h) {
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
}

/* The Xbox 360 game's tile cursor: four L-shaped corners, with a dark edge so it reads on any lawn. */
static void draw_corner_brackets(float x, float y, float w, float h, float t, float r, float g, float b, float a) {
    const float L = std::min(w, h) * 0.30f;
    for (int pass = 0; pass < 2; ++pass) {
        const float o = pass == 0 ? 1.5f : 0.0f; /* outline first, then the colour */
        const float tt = t + 2.0f * o;
        if (pass == 0) glColor4f(0.0f, 0.0f, 0.0f, 0.55f * a);
        else glColor4f(r * a + (1.0f - a) * r * 0.6f, g * a + (1.0f - a) * g * 0.6f, b * a + (1.0f - a) * b * 0.6f,
                       1.0f); /* pulse by brightness, not transparency: no see-through seams */
        const float x0 = x - o, y0 = y - o, x1 = x + w + o, y1 = y + h + o, LL = L + o;
        /* Each corner is a horizontal bar plus a vertical bar that starts where the horizontal one ends:
         * overlapping them would draw the corner square twice, darker than the rest when translucent. */
        glBegin(GL_QUADS);
        gl_quad(x0, y0, LL, tt);            gl_quad(x0, y0 + tt, tt, LL - tt);          /* top left */
        gl_quad(x1 - LL, y0, LL, tt);       gl_quad(x1 - tt, y0 + tt, tt, LL - tt);     /* top right */
        gl_quad(x0, y1 - tt, LL, tt);       gl_quad(x0, y1 - LL, tt, LL - tt);          /* bottom left */
        gl_quad(x1 - LL, y1 - tt, LL, tt);  gl_quad(x1 - tt, y1 - LL, tt, LL - tt);     /* bottom right */
        glEnd();
    }
}

static void draw_frame_rect(float x, float y, float w, float h, float t, float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    gl_quad(x, y, w, t);
    gl_quad(x, y + h - t, w, t);
    gl_quad(x, y + t, t, h - 2 * t);
    gl_quad(x + w - t, y + t, t, h - 2 * t);
    glEnd();
}

bool GamepadModule::ensure_icon_texture() {
    if (icon_tex_ != 0) return true;
    GLuint id = 0;
    glGenTextures(1, &id);
    if (id == 0) return false;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pvz2native::gamepad_icons::kAtlasW, pvz2native::gamepad_icons::kAtlasH,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pvz2native::gamepad_icons::kAtlasRGBA);
    icon_tex_ = id;
    return true;
}

/* One button prompt, `h` pixels tall, centred on (cx, cy). Needs GL_TEXTURE_2D on and the atlas bound. */
void GamepadModule::draw_icon(int icon, float cx, float cy, float h) const {
    using namespace pvz2native::gamepad_icons;
    const Rect &r = kRects[icon];
    const float w = h * static_cast<float>(r.w) / static_cast<float>(r.h);
    const float u0 = static_cast<float>(r.x) / kAtlasW, u1 = static_cast<float>(r.x + r.w) / kAtlasW;
    const float v0 = static_cast<float>(r.y) / kAtlasH, v1 = static_cast<float>(r.y + r.h) / kAtlasH;
    const float x = cx - w * 0.5f, y = cy - h * 0.5f;
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    glEnd();
}

void GamepadModule::draw(SDL_Window *window) {
    if (!window || !grid_ready()) return;
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) return;
    const float W = static_cast<float>(dw), H = static_cast<float>(dh);

    GLint prev_program = 0, prev_fbo = 0, prev_viewport[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, dw, dh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();

    const float pulse = 0.80f + 0.20f * std::sin(static_cast<float>(SDL_GetTicks()) * 0.008f);
    const float t = std::max(3.0f, H * 0.0055f);
    const float icon_h = H * 0.045f;
    const float asp = W / H;

    if (chooser_active()) {
        /* Seed chooser: corners on the card or bank packet under the box, Y on "Let's rock". */
        float nx = 0.0f, ny = 0.0f;
        chooser_cell_center(&nx, &ny);
        const float cw = (chooser_col_ == 0 ? kPacketW : kCardW) * H;
        const float ch = (chooser_col_ == 0 ? kPacketH : kCardH) * H;
        draw_corner_brackets(nx * W - cw * 0.5f - t, ny * H - ch * 0.5f - t, cw + 2 * t, ch + 2 * t, t,
                             1.0f, 0.86f, 0.10f, pulse);
        if (ensure_icon_texture()) {
            using namespace pvz2native::gamepad_icons;
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, icon_tex_);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            const float rx = (1.0f - kRockFromRight / asp) * W;
            draw_icon(Y, rx - 0.135f * H, kRockY * H - 0.045f * H, icon_h);
            draw_icon(A, nx * W + cw * 0.5f, ny * H - ch * 0.5f, icon_h * 0.8f);
            draw_icon(X, kReselectX * H + 0.065f * H, kReselectY * H - 0.040f * H, icon_h);
            draw_icon(START, (1.0f - kPauseFromRight / asp) * W - 0.035f * H, kTopButtonsY * H + 0.045f * H,
                      icon_h * 0.75f);
            glDisable(GL_TEXTURE_2D);
        }
    } else {
        /* The box on the lawn: yellow corners, red while a plant waits for its target. */
        float cx = 0.0f, cy = 0.0f, tw = 0.0f, th = 0.0f;
        grid_cell_norm(&cx, &cy);
        grid_tile_size_norm(&tw, &th);
        const float inset = 0.025f;
        const float bx = (cx - tw * (0.5f - inset)) * W, by = (cy - th * (0.5f - inset)) * H;
        const float bw = tw * (1.0f - 2 * inset) * W, bh = th * (1.0f - 2 * inset) * H;
        if (aiming_) draw_corner_brackets(bx, by, bw, bh, t, 1.0f, 0.25f, 0.15f, pulse);
        else draw_corner_brackets(bx, by, bw, bh, t, 1.0f, 0.86f, 0.10f, pulse);

        /* The chosen seed packet: a yellow frame and a marker pointing at it, as on the 360. */
        const bool packets = pvz2_game_has_seed_packets() != 0;
        const bool conveyor = pvz2_game_level_has_conveyor() != 0;
        float pw = 0.0f, ph = 0.0f;
        bank_packet_size_px(H, &pw, &ph);
        const int n = bank_slots();
        if (packets && seed_slot_ > 0 && seed_slot_ <= n && !aiming_) {
            float snx = 0.0f, sny = 0.0f;
            bank_slot_center(seed_slot_, conveyor, &snx, &sny);
            if (conveyor) sny = conveyor_y_;
            const float px = snx * W - pw * 0.5f, py = sny * H - ph * 0.5f;
            draw_frame_rect(px - 1.5f, py - 1.5f, pw + 3.0f, ph + 3.0f, t + 3.0f, 0.0f, 0.0f, 0.0f, 0.5f);
            draw_frame_rect(px, py, pw, ph, t, 1.0f, 0.86f, 0.10f, 1.0f);
            /* Marker beside the packet, pointing at it. */
            const float mx = px + pw + t * 1.5f, my = sny * H, ms = ph * 0.22f;
            glColor4f(0.0f, 0.0f, 0.0f, 0.55f);
            glBegin(GL_TRIANGLES);
            glVertex2f(mx - 2.0f, my);
            glVertex2f(mx + ms + 2.0f, my - ms - 2.0f);
            glVertex2f(mx + ms + 2.0f, my + ms + 2.0f);
            glEnd();
            glColor4f(1.0f, 0.86f, 0.10f, 1.0f);
            glBegin(GL_TRIANGLES);
            glVertex2f(mx, my);
            glVertex2f(mx + ms, my - ms);
            glVertex2f(mx + ms, my + ms);
            glEnd();
        }

        /* Button prompts on the things they work. */
        if (ensure_icon_texture()) {
            using namespace pvz2native::gamepad_icons;
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, icon_tex_);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            if (packets) {
                /* LB beside the first packet's top corner, RB beside the last one's bottom corner. */
                float fx = 0.0f, fy = 0.0f, lx = 0.0f, ly = 0.0f;
                bank_slot_center(1, conveyor, &fx, &fy);
                bank_slot_center(n, conveyor, &lx, &ly);
                const float ix = fx * W + pw * 0.5f + icon_h * 0.85f;
                draw_icon(LB, ix, fy * H - ph * 0.5f + (conveyor ? icon_h * 0.5f : 0.0f), icon_h);
                draw_icon(RB, ix, std::min(ly * H + ph * 0.5f, H - icon_h * 0.6f), icon_h);
            }
            const int shovel = find_focus_by_name("shovel");
            if (shovel >= 0) {
                const FocusPoint &f = focus_points_[static_cast<size_t>(shovel)];
                draw_icon(X, f.nx * W - icon_h * 0.9f, f.ny * H - icon_h * 0.7f, icon_h);
            }
            const int food = find_focus_by_name("plant_food");
            if (food >= 0) {
                const FocusPoint &f = focus_points_[static_cast<size_t>(food)];
                draw_icon(Y, f.nx * W - icon_h * 1.1f, f.ny * H - icon_h * 0.7f, icon_h);
            }
            /* While aiming: RT on the box's corner, as the "fire" prompt. */
            if (aiming_) draw_icon(RT, bx + bw, by, icon_h);
            /* Start on the pause button, LT on the speed-up button (both top right). */
            draw_icon(START, (1.0f - kPauseFromRight / asp) * W - 0.035f * H, kTopButtonsY * H + 0.045f * H,
                      icon_h * 0.75f);
            draw_icon(LT, (1.0f - kSpeedFromRight / asp) * W - 0.045f * H, kTopButtonsY * H + 0.040f * H,
                      icon_h * 0.9f);
            glDisable(GL_TEXTURE_2D);
        }
    }

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glUseProgram(prev_program);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

}  // namespace

extern "C" {

void pvz2_gamepad_init(SDL_Window *window, int use_43) { g_gamepad.init(window, use_43); }

void pvz2_gamepad_shutdown(void) { g_gamepad.shutdown(); }

void pvz2_gamepad_reload(void) { g_gamepad.reload(); }

int pvz2_gamepad_handle_event(const SDL_Event *e) { return g_gamepad.handle_event(e); }

void pvz2_gamepad_tick(float dt_sec) { g_gamepad.tick(dt_sec); }

void pvz2_gamepad_draw(SDL_Window *window) { g_gamepad.draw(window); }

void pvz2_gamepad_rumble(float low, float high, unsigned ms) { g_gamepad.request_rumble(low, high, ms); }

}  // extern "C"

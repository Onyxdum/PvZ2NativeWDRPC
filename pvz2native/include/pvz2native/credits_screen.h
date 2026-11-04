#ifndef PVZ2NATIVE_CREDITS_SCREEN_H
#define PVZ2NATIVE_CREDITS_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Window SDL_Window;
typedef union SDL_Event SDL_Event;

void credits_screen_open(SDL_Window *window);
void credits_screen_close(void);
int  credits_screen_is_open(void);
void credits_screen_request_open(void);
int  credits_screen_consume_open_request(void);
void credits_screen_handle_event(const SDL_Event *e);
void credits_screen_draw(SDL_Window *window);
void credits_screen_draw_reward_button_overlay(SDL_Window *window);
int  credits_screen_handle_reward_button_event(const SDL_Event *e);
void credits_screen_shutdown(void);

/* Notifications from game engine when Modern Day Day 34 reward dialog is active */
void credits_set_modern34_active(int active);
int  credits_get_modern34_active(void);
void credits_notify_modern34_reward_create(void);
void credits_notify_modern34_reward_destroy(void);
int  credits_is_modern34_reward_active(void);
void credits_mark_modern34_won(void);
int  credits_is_modern34_won(void);
void credits_update_frame(void);

/* Telemetry context tracked from Draper HTTP calls */
void credits_set_draper_info(const char *world, int level, int state);
const char *credits_get_draper_world(void);
int  credits_get_draper_level(void);
int  credits_get_draper_state(void);

#ifdef __cplusplus
}
#endif

#endif /* PVZ2NATIVE_CREDITS_SCREEN_H */

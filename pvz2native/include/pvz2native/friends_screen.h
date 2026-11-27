#ifndef PVZ2NATIVE_FRIENDS_SCREEN_H
#define PVZ2NATIVE_FRIENDS_SCREEN_H
/* Friends: the world map's envelope button opens this list of the port's other players (Discord name and
 * picture, playing now or last seen, progress), with an "Invite" button that copies the invite link.
 *
 * Data: while a Discord account is linked for cloud saves, a worker thread reports this player's status to
 * the cloud server once a minute (POST /friends/v1/status: version, last level, plants, worlds, levels read
 * from userdata/pp.dat) and gets the list back (tab-separated, see cloud-server/server.py friends_tsv).
 * The worker starts with the first friends_screen_draw() call.
 *
 * All GL work runs on the main thread in friends_screen_draw(). */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Window SDL_Window;
typedef union SDL_Event SDL_Event;

/* From any thread: open the list on the next frame (and refresh it). */
void friends_screen_request_open(void);
void friends_screen_close(void);
int friends_screen_is_open(void);

/* Forward SDL events while the list is open. */
void friends_screen_handle_event(const SDL_Event *e);

/* Every frame, after the other overlays and before SDL_GL_SwapWindow. Draws only while open. */
void friends_screen_draw(SDL_Window *window);

/* At boot, before the game reads its save: gifts friends sent (GET /friends/v1/gifts) are added to the
 * coins and gems of userdata/pp.dat, then acknowledged; a notification tells who sent them once the game
 * is on screen. Blocks for at most a few seconds, and does nothing without a linked Discord account. */
void friends_claim_gifts_at_boot(void);

/* Main thread, between frames (session.cpp), with the guest image (pvz2_elf_image_t *): gifts that arrive
 * while the game runs (the worker asks once a minute) are added to the coins and gems of the live player
 * profile, with the same notification; the game saves them itself. */
void friends_tick_live_gifts(void *guest_image);

/* Stop the worker and release GL resources. Idempotent. */
void friends_screen_shutdown(void);

/* Register pvz2native:// URL scheme for the current user (Windows HKCU, no admin). */
void friends_register_url_protocol(void);

/* If another pvz2.exe is already running, forward a pvz2native://invite?... URI to it and return 1. */
int friends_try_forward_invite_uri(const char *uri);

/* Queue an incoming pvz2native://invite?... URI for this running instance. */
void friends_handle_invite_uri(const char *uri);

/* Copy this player's server-signed HMAC invite code (empty if not yet fetched / not linked). */
void friends_get_invite_code(char *out, int cap);

/* Native top-left HUD Friends button (used on 6.6.1 where the game removed its own social button). */
void friends_screen_note_hud_visible(void);
void friends_screen_set_hud_visible(int visible);
int friends_screen_hud_button_event(const SDL_Event *e, SDL_Window *window);
void friends_screen_draw_hud_button(SDL_Window *window);

#ifdef __cplusplus
}
#endif

#endif /* PVZ2NATIVE_FRIENDS_SCREEN_H */

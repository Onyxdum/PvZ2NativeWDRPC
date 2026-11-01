#ifndef PVZ2NATIVE_PRESENCE_H
#define PVZ2NATIVE_PRESENCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Discord Rich Presence.
 *
 * Talks to the local Discord client over its IPC socket directly (named pipe on
 * Windows, unix socket elsewhere), so there is no extra library to build or ship.
 * Everything runs on its own worker thread: the taps below only append to a
 * queue, so the JIT/frame threads never wait on Discord.
 *
 * What the presence says is NOT hardcoded. The engine reports what it is doing
 * only indirectly -- which resource files it opens and what it logs -- so this
 * module is fed those as "events" (pvz2_presence_note) and a rules file maps them
 * to text. See presence.cpp for the rules format.
 *
 * All three functions are no-ops unless [discord] enabled = 1 and a client_id is
 * set, and pvz2_presence_note is a single atomic load in that case, so the taps
 * are free to sit on hot paths. */

/* Reads [discord] from the loaded config and starts the worker. Call after
 * pvz2_config_load, before the guest boots. Safe to call when disabled. */
void pvz2_presence_start(void);

/* Reports one engine event. `source` is "asset" (a resource path the engine
 * asked for) or "log" (a line the guest wrote through liblog). Thread-safe. */
void pvz2_presence_note(const char *source, const char *text);

/* 1 when presence is running. Lets a caller skip building the text for a tap. */
int pvz2_presence_enabled(void);

/* Clears the activity, closes the connection and joins the worker. */
void pvz2_presence_stop(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif

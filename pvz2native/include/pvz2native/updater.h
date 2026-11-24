#ifndef PVZ2NATIVE_UPDATER_H
#define PVZ2NATIVE_UPDATER_H

#ifdef __cplusplus
extern "C" {
#endif

#define PVZ2NATIVE_APP_VERSION "v1.6.0"

/* Checks the latest GitHub release on a worker thread (never blocks the start).
 * Windows: when it is newer and not skipped, a dialog shows the version and its notes
 * (Update / Skip / Not now); Update starts the installer and closes the game cleanly.
 * Linux: a notification only. PVZ2_UPDATE_FEED (URL or path of a release JSON)
 * replaces the GitHub feed, for testing. */
void pvz2_check_updates_async(void);

/* Main thread, once per frame: shows a pending update notification. */
void pvz2_updater_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* PVZ2NATIVE_UPDATER_H */

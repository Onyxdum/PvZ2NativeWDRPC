#ifndef PVZ2NATIVE_STEAM_SHORTCUT_H
#define PVZ2NATIVE_STEAM_SHORTCUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Checks if the user should be prompted to add PvZ2 to their Steam library.
 * MUST be called AFTER check_required_files() so missing OBB/SO errors
 * are displayed first! */
void pvz2_check_steam_shortcut_prompt(void);

/* Manually add the current game executable to Steam library shortcuts.
 * Returns 1 on success, 0 on failure. If msg_out is non-null, writes status message. */
int pvz2_add_to_steam(char *msg_out, int msg_cap, const char *locale);

#ifdef __cplusplus
}
#endif

#endif // PVZ2NATIVE_STEAM_SHORTCUT_H

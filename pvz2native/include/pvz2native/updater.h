#ifndef PVZ2NATIVE_UPDATER_H
#define PVZ2NATIVE_UPDATER_H

#ifdef __cplusplus
extern "C" {
#endif

#define PVZ2NATIVE_APP_VERSION "v1.4.2"

/* Asynchronously checks GitHub releases for updates in the background.
 * If a newer version is available, prompts the player with a dialog:
 * "Nueva version disponible ¿Deseas actualizar el Juego? Solo tomara un minuto"
 * If accepted, opens the GitHub release page and launches the automated updater. */
void pvz2_check_updates_async(void);

#ifdef __cplusplus
}
#endif

#endif /* PVZ2NATIVE_UPDATER_H */

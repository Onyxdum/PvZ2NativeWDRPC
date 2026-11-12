#ifndef PVZ2NATIVE_FIRST_BOOT_WIZARD_H
#define PVZ2NATIVE_FIRST_BOOT_WIZARD_H

#include <pvz2native/config.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Checks if required game files (libPVZ2.so and data.obb) exist.
 * If both exist, returns 1 immediately with zero delay/dialogs.
 * If either is missing, checks for nearby game files or runs the interactive
 * setup wizard allowing the player to select their 4.5.2 APK / XAPK and OBB.
 * Returns 1 if files are ready to boot, or 0 if cancelled / failed. */
int pvz2_ensure_required_files(const pvz2_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* PVZ2NATIVE_FIRST_BOOT_WIZARD_H */

#ifndef PVZ2NATIVE_CLOUD_SAVE_H
#define PVZ2NATIVE_CLOUD_SAVE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum Pvz2CloudProvider {
    PVZ2_CLOUD_NAS = 0,
    PVZ2_CLOUD_GDRIVE = 1,
    PVZ2_CLOUD_SERVER = 2,
    PVZ2_CLOUD_GIST = 3,
    PVZ2_CLOUD_PROVIDER_COUNT = 4
};

/* Default hosted NAS endpoint ("yo invito" ZimaOS server via Cloudflare Tunnel). */
#define PVZ2_DEFAULT_NAS_SERVER_URL "https://pvz2.onyxdum.eu"

typedef struct Pvz2CloudConfig {
    int provider;
    int auto_sync;

    /* Built-in ZimaOS NAS server URL (used when provider == PVZ2_CLOUD_NAS) */
    char nas_server_url[512];
    /* Custom private server URL (used when provider == PVZ2_CLOUD_SERVER) */
    char server_url[512];

    /* Authenticated Discord account for NAS / Private Server saves (top button) */
    char auth_discord_id[64];
    char auth_discord_name[128];
    char auth_discord_token[256];

    /* Authenticated Google Drive account (top button when provider == Google Drive) */
    char gdrive_user[128];
    char gdrive_token[512];
    char gdrive_refresh_token[512];
    char gdrive_file_id[128];

    /* Authenticated GitHub Gist account (top button when provider == GitHub Gist) */
    char github_user[128];
    char gist_token[256];
    char gist_id[128];

    /* Local Discord desktop app account detected via IPC (bottom button) */
    char local_discord_id[64];
    char local_discord_name[128];
} Pvz2CloudConfig;

void pvz2_cloud_init(void);
Pvz2CloudConfig *pvz2_cloud_config(void);
void pvz2_cloud_save_config(void);

/* Top button: authenticate in the browser with the account for the current provider
 * (Discord for Cloud/Server, Google for Google Drive, GitHub for GitHub Gist). */
bool pvz2_cloud_start_provider_auth(char *status_out, size_t status_cap, const char *locale);
bool pvz2_cloud_is_auth_in_progress(void);
bool pvz2_cloud_is_provider_authenticated(void);

/* Bottom button: detect/refresh local Discord desktop app account via IPC. */
bool pvz2_cloud_sync_local_discord(bool force_connect, char *status_out, size_t status_cap, const char *locale);

bool pvz2_cloud_upload(char *status_out, size_t status_cap, const char *locale);
bool pvz2_cloud_download(char *status_out, size_t status_cap, const char *locale);

bool pvz2_cloud_export_local(char *status_out, size_t status_cap, const char *locale);
bool pvz2_cloud_import_local(char *status_out, size_t status_cap, const char *locale);

bool pvz2_cloud_writes_locked(void);
void pvz2_cloud_restart_game(void);
void pvz2_cloud_on_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* PVZ2NATIVE_CLOUD_SAVE_H */

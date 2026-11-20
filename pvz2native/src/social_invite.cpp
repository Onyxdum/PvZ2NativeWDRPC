/* social_invite.cpp -- the world map's invite button, reinvented for Discord (see social_invite.h). */

#include <pvz2native/social_invite.h>

#include <pvz2native/cloud_save.h>
#include <pvz2native/config.h>
#include <pvz2native/config_screen.h>
#include <pvz2native/friends_screen.h>
#include <pvz2native/log.h>

#include <SDL.h>

#include <atomic>
#include <cstring>

namespace pvz2native {
namespace {

std::atomic<bool> g_invite_requested{false};

std::string url_encode(const char *s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        const unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

bool locale_is(const char *code) {
    const char *loc = pvz2_config() ? pvz2_config()->user_locale : "";
    return loc && std::strncmp(loc, code, 2) == 0;
}

}  // namespace

std::string invite_url() {
    pvz2_cloud_init();
    const Pvz2CloudConfig *cc = pvz2_cloud_config();
    std::string base = cc->nas_server_url[0] ? cc->nas_server_url : PVZ2_DEFAULT_NAS_SERVER_URL;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base + "/invite";
    /* The account linked for cloud saves, else the one the local Discord app is signed in to. */
    const char *id = cc->auth_discord_id[0] ? cc->auth_discord_id : cc->local_discord_id;
    const char *name = cc->auth_discord_id[0] ? cc->auth_discord_name : cc->local_discord_name;
    if (id[0]) {
        url += "?from=" + url_encode(id);
        if (name[0]) url += "&n=" + url_encode(name);
        char code[32] = {};
        friends_get_invite_code(code, (int)sizeof(code));
        if (code[0]) url += "&c=" + url_encode(code);
    }
    return url;
}

void invite_button_clicked() { g_invite_requested.store(true); }

}  // namespace pvz2native

/* Main thread, once per frame (config_screen_draw). */
extern "C" void pvz2_invite_tick(void) {
    using namespace pvz2native;
    if (!g_invite_requested.exchange(false)) return;
    const std::string url = invite_url();
    const bool ok = SDL_SetClipboardText(url.c_str()) == 0;
    pvz2_log("[invite] %s %s", ok ? "copied" : "could not copy", url.c_str());
    const char *text;
    if (locale_is("es"))
        text = ok ? "Enlace de invitación copiado: pégalo en Discord" : "No se pudo copiar el enlace de invitación";
    else if (locale_is("fr"))
        text = ok ? "Lien d'invitation copié : colle-le sur Discord" : "Impossible de copier le lien d'invitation";
    else if (locale_is("de"))
        text = ok ? "Einladungslink kopiert: in Discord einfügen" : "Einladungslink konnte nicht kopiert werden";
    else if (locale_is("it"))
        text = ok ? "Link di invito copiato: incollalo su Discord" : "Impossibile copiare il link di invito";
    else if (locale_is("pt"))
        text = ok ? "Link de convite copiado: cole no Discord" : "Não foi possível copiar o link de convite";
    else
        text = ok ? "Invite link copied: paste it in Discord" : "Could not copy the invite link";
    config_screen_toast(text);
}

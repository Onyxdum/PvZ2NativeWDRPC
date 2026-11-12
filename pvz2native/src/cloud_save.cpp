#include <pvz2native/cloud_save.h>

#include <pvz2native/config.h>
#include <pvz2native/host_paths.h>
#include <pvz2native/presence.h>

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

Pvz2CloudConfig g_cfg{};
std::mutex g_cfg_mutex;
bool g_inited = false;
bool g_writes_locked = false;
std::atomic<bool> g_auth_running{false};
std::atomic<bool> g_auth_listening{false};
std::atomic<std::time_t> g_auth_deadline{0};
char g_last_locale[16] = "en_US";

struct CloudStrings {
    const char *html_lang;
    const char *html_title;
    const char *returning_to_game;
    const char *connected_as;
    const char *github_linked;
    const char *github_link_err;
    const char *github_link_err_sub;
    const char *discord_linked;
    const char *discord_link_err;
    const char *discord_link_err_sub;
    const char *gdrive_linked;
    const char *gdrive_link_err;
    const char *gdrive_link_err_sub;
    const char *signin_discord_first;
    const char *signin_google_first;
    const char *signin_github_first;
    const char *err_empty_url;
    const char *no_local_files;
    const char *server_word;
    const char *conn_failed_fmt;
    const char *uploaded_fmt;
    const char *no_save_found_fmt;
    const char *invalid_save_data;
    const char *downloaded_restart_fmt;
    const char *gdrive_auth_failed;
    const char *gdrive_upload_err;
    const char *gdrive_uploaded_fmt;
    const char *gdrive_no_save;
    const char *gdrive_download_err;
    const char *gdrive_invalid_file;
    const char *gdrive_downloaded_fmt;
    const char *gist_upload_err;
    const char *gist_uploaded_fmt;
    const char *gist_no_save;
    const char *gist_download_err;
    const char *gist_not_pvz2;
    const char *gist_unpack_err;
    const char *gist_downloaded_fmt;
    const char *discord_connected_fmt;
    const char *discord_not_detected;
    const char *export_success_fmt;
    const char *export_err;
    const char *import_success_fmt;
    const char *import_err_read;
    const char *import_err_invalid;
};

static const CloudStrings kCloudEN = {
    "en", "PvZ2Native - Cloud Save",
    "Returning to Plants vs. Zombies 2...",
    "Connected as",
    "GitHub Gist linked!", "Failed to link GitHub",
    "Could not complete GitHub authorization. Please try again.",
    "Discord linked!", "Failed to link Discord",
    "Could not complete Discord authorization. Please try again.",
    "Google Drive linked!", "Failed to link Google Drive",
    "Could not complete Google Drive authorization. Please try again.",
    "Sign in with Discord at the top button first",
    "Sign in with Google at the top button first",
    "Sign in with GitHub at the top button first",
    "Error: server URL is empty",
    "No local files to upload",
    "server",
    "Could not connect to %s",
    "Save uploaded to %s (%zu files)",
    "No save found on %s for this account",
    "Invalid save data",
    "Downloaded from %s (%zu files). Press Restart",
    "Google Drive auth failed (please sign in again)",
    "Failed to upload to Google Drive",
    "Uploaded to Google Drive (%zu files)",
    "No save found in your Google Drive",
    "Failed to download from Google Drive",
    "Invalid Google Drive save file",
    "Downloaded from Google Drive (%zu files). Press Restart",
    "Failed to upload to GitHub Gist",
    "Uploaded to GitHub Gist (%zu files)",
    "No save found in your GitHub Gists",
    "Failed to download from GitHub Gist",
    "Gist does not contain a PvZ2 save",
    "Failed to unpack save from Gist",
    "Downloaded from GitHub Gist (%zu files). Press Restart",
    "Connected to Discord as %s (%s)",
    "Local Discord not detected (open Discord)",
    "Save exported to JSON (%zu files)",
    "Failed to write JSON file",
    "Save imported from JSON (%zu files). Press Restart",
    "Failed to read JSON file",
    "The JSON file does not contain a valid PvZ 2 save"
};

static const CloudStrings kCloudES = {
    "es", "PvZ2Native - Guardado en la Nube",
    "Volviendo a Plants vs. Zombies 2...",
    "Conectado como",
    "¡GitHub Gist vinculado!", "Error al vincular GitHub",
    "No se pudo completar la autorización con GitHub. Inténtalo de nuevo.",
    "¡Discord vinculado!", "Error al vincular Discord",
    "No se pudo completar la autorización con Discord. Inténtalo de nuevo.",
    "¡Google Drive vinculado!", "Error al vincular Google Drive",
    "No se pudo completar la autorización con Google Drive. Inténtalo de nuevo.",
    "Primero inicia sesión con Discord en el botón de arriba",
    "Primero inicia sesión con Google en el botón de arriba",
    "Primero inicia sesión con GitHub en el botón de arriba",
    "Error: URL del servidor vacía",
    "No hay archivos locales para subir",
    "el servidor",
    "No se pudo conectar a %s",
    "Partida subida a %s (%zu archivos)",
    "No se encontró partida en %s para esta cuenta",
    "Datos de partida inválidos",
    "Descargado de %s (%zu archivos). Pulsa Reiniciar",
    "Error de autenticación con Google Drive (vuelve a iniciar sesión)",
    "Error al subir a Google Drive",
    "Subido a Google Drive (%zu archivos)",
    "No se encontró partida en tu Google Drive",
    "Error al descargar de Google Drive",
    "Archivo de Google Drive inválido",
    "Descargado de Google Drive (%zu archivos). Pulsa Reiniciar",
    "Error al subir a GitHub Gist",
    "Subido a GitHub Gist (%zu archivos)",
    "No se encontró ninguna partida en tus Gists de GitHub",
    "Error al descargar de GitHub Gist",
    "El Gist no contiene una partida de PvZ2",
    "No se pudo extraer la partida del Gist",
    "Descargado de GitHub Gist (%zu archivos). Pulsa Reiniciar",
    "Conectado a Discord como %s (%s)",
    "Discord local no detectado (abre Discord)",
    "Partida exportada a JSON (%zu archivos)",
    "Error al guardar el archivo JSON",
    "Partida importada de JSON (%zu archivos). Pulsa Reiniciar",
    "Error al leer el archivo JSON",
    "El archivo JSON no contiene una partida válida de PvZ 2"
};

static const CloudStrings kCloudFR = {
    "fr", "PvZ2Native - Sauvegarde Cloud",
    "Retour vers Plants vs. Zombies 2...",
    "Connecté en tant que",
    "GitHub Gist lié !", "Erreur de liaison GitHub",
    "Impossible de terminer l'autorisation avec GitHub. Veuillez réessayer.",
    "Discord lié !", "Erreur de liaison Discord",
    "Impossible de terminer l'autorisation avec Discord. Veuillez réessayer.",
    "Google Drive lié !", "Erreur de liaison Google Drive",
    "Impossible de terminer l'autorisation avec Google Drive. Veuillez réessayer.",
    "Connectez-vous d'abord avec Discord via le bouton du haut",
    "Connectez-vous d'abord avec Google via le bouton du haut",
    "Connectez-vous d'abord avec GitHub via le bouton du haut",
    "Erreur : l'URL du serveur est vide",
    "Aucun fichier local à envoyer",
    "le serveur",
    "Impossible de se connecter à %s",
    "Sauvegarde envoyée sur %s (%zu fichiers)",
    "Aucune sauvegarde trouvée sur %s pour ce compte",
    "Données de sauvegarde invalides",
    "Téléchargé depuis %s (%zu fichiers). Appuyez sur Redémarrer",
    "Échec d'authentification Google Drive (reconnectez-vous)",
    "Échec de l'envoi vers Google Drive",
    "Envoyé sur Google Drive (%zu fichiers)",
    "Aucune sauvegarde trouvée sur votre Google Drive",
    "Échec du téléchargement depuis Google Drive",
    "Fichier de sauvegarde Google Drive invalide",
    "Téléchargé depuis Google Drive (%zu fichiers). Appuyez sur Redémarrer",
    "Échec de l'envoi vers GitHub Gist",
    "Envoyé sur GitHub Gist (%zu fichiers)",
    "Aucune sauvegarde trouvée dans vos Gists GitHub",
    "Échec du téléchargement depuis GitHub Gist",
    "Le Gist ne contient pas de sauvegarde PvZ2",
    "Impossible d'extraire la sauvegarde du Gist",
    "Téléchargé depuis GitHub Gist (%zu fichiers). Appuyez sur Redémarrer",
    "Connecté à Discord en tant que %s (%s)",
    "Discord local non détecté (ouvrez Discord)",
    "Sauvegarde exportée en JSON (%zu fichiers)",
    "Échec de l'écriture du fichier JSON",
    "Sauvegarde importée depuis JSON (%zu fichiers). Appuyez sur Redémarrer",
    "Échec de la lecture du fichier JSON",
    "Le fichier JSON ne contient pas de sauvegarde PvZ 2 valide"
};

static const CloudStrings kCloudDE = {
    "de", "PvZ2Native - Cloud-Speicher",
    "Zurück zu Plants vs. Zombies 2...",
    "Verbunden als",
    "GitHub Gist verknüpft!", "Fehler bei GitHub-Verknüpfung",
    "Die Autorisierung mit GitHub konnte nicht abgeschlossen werden.",
    "Discord verknüpft!", "Fehler bei Discord-Verknüpfung",
    "Die Autorisierung mit Discord konnte nicht abgeschlossen werden.",
    "Google Drive verknüpft!", "Fehler bei Google Drive-Verknüpfung",
    "Die Autorisierung mit Google Drive konnte nicht abgeschlossen werden.",
    "Melde dich zuerst oben mit Discord an",
    "Melde dich zuerst oben mit Google an",
    "Melde dich zuerst oben mit GitHub an",
    "Fehler: Server-URL ist leer",
    "Keine lokalen Dateien zum Hochladen vorhanden",
    "Server",
    "Verbindung zu %s fehlgeschlagen",
    "Spielstand auf %s hochgeladen (%zu Dateien)",
    "Kein Spielstand auf %s für dieses Konto gefunden",
    "Ungültige Spielstanddaten",
    "Von %s heruntergeladen (%zu Dateien). Neustart drücken",
    "Google Drive-Authentifizierung fehlgeschlagen (bitte neu anmelden)",
    "Hochladen auf Google Drive fehlgeschlagen",
    "Auf Google Drive hochgeladen (%zu Dateien)",
    "Kein Spielstand in deinem Google Drive gefunden",
    "Herunterladen von Google Drive fehlgeschlagen",
    "Ungültige Google Drive-Spielstanddatei",
    "Von Google Drive heruntergeladen (%zu Dateien). Neustart drücken",
    "Hochladen auf GitHub Gist fehlgeschlagen",
    "Auf GitHub Gist hochgeladen (%zu Dateien)",
    "Kein Spielstand in deinen GitHub Gists gefunden",
    "Herunterladen von GitHub Gist fehlgeschlagen",
    "Das Gist enthält keinen PvZ2-Spielstand",
    "Spielstand konnte nicht aus dem Gist entpackt werden",
    "Von GitHub Gist heruntergeladen (%zu Dateien). Neustart drücken",
    "Mit Discord verbunden als %s (%s)",
    "Lokales Discord nicht erkannt (Discord öffnen)",
    "Spielstand als JSON exportiert (%zu Dateien)",
    "Fehler beim Schreiben der JSON-Datei",
    "Spielstand aus JSON importiert (%zu Dateien). Neustart drücken",
    "Fehler beim Lesen der JSON-Datei",
    "Die JSON-Datei enthält keinen gültigen PvZ 2-Spielstand"
};

static const CloudStrings kCloudPT = {
    "pt", "PvZ2Native - Salvamento na Nuvem",
    "Voltando para Plants vs. Zombies 2...",
    "Conectado como",
    "GitHub Gist vinculado!", "Erro ao vincular o GitHub",
    "Não foi possível concluir a autorização com o GitHub. Tente novamente.",
    "Discord vinculado!", "Erro ao vincular o Discord",
    "Não foi possível concluir a autorização com o Discord. Tente novamente.",
    "Google Drive vinculado!", "Erro ao vincular o Google Drive",
    "Não foi possível concluir a autorização com o Google Drive. Tente novamente.",
    "Primeiro entre com o Discord no botão acima",
    "Primeiro entre com o Google no botão acima",
    "Primeiro entre com o GitHub no botão acima",
    "Erro: URL do servidor vazia",
    "Nenhum arquivo local para enviar",
    "o servidor",
    "Não foi possível conectar a %s",
    "Jogo salvo enviado para %s (%zu arquivos)",
    "Nenhum jogo salvo encontrado em %s para esta conta",
    "Dados de salvamento inválidos",
    "Baixado de %s (%zu arquivos). Pressione Reiniciar",
    "Falha na autenticação do Google Drive (entre novamente)",
    "Erro ao enviar para o Google Drive",
    "Enviado para o Google Drive (%zu arquivos)",
    "Nenhum jogo salvo encontrado no seu Google Drive",
    "Erro ao baixar do Google Drive",
    "Arquivo de salvamento do Google Drive inválido",
    "Baixado do Google Drive (%zu arquivos). Pressione Reiniciar",
    "Erro ao enviar para o GitHub Gist",
    "Enviado para o GitHub Gist (%zu arquivos)",
    "Nenhum jogo salvo encontrado nos seus Gists do GitHub",
    "Erro ao baixar do GitHub Gist",
    "O Gist não contém um jogo salvo de PvZ2",
    "Não foi possível extrair o jogo salvo do Gist",
    "Baixado do GitHub Gist (%zu arquivos). Pressione Reiniciar",
    "Conectado ao Discord como %s (%s)",
    "Discord local não detectado (abra o Discord)",
    "Jogo salvo exportado para JSON (%zu arquivos)",
    "Falha ao salvar o arquivo JSON",
    "Jogo salvo importado de JSON (%zu arquivos). Pressione Reiniciar",
    "Falha ao ler o arquivo JSON",
    "O arquivo JSON não contém um jogo salvo válido do PvZ 2"
};

static const CloudStrings kCloudIT = {
    "it", "PvZ2Native - Salvataggio nel Cloud",
    "Ritorno a Plants vs. Zombies 2...",
    "Connesso come",
    "GitHub Gist collegato!", "Errore di collegamento GitHub",
    "Impossibile completare l'autorizzazione con GitHub. Riprova.",
    "Discord collegato!", "Errore di collegamento Discord",
    "Impossibile completare l'autorizzazione con Discord. Riprova.",
    "Google Drive collegato!", "Errore di collegamento Google Drive",
    "Impossibile completare l'autorizzazione con Google Drive. Riprova.",
    "Accedi prima con Discord dal pulsante in alto",
    "Accedi prima con Google dal pulsante in alto",
    "Accedi prima con GitHub dal pulsante in alto",
    "Errore: URL del server vuoto",
    "Nessun file locale da caricare",
    "il server",
    "Impossibile connettersi a %s",
    "Salvataggio caricato su %s (%zu file)",
    "Nessun salvataggio trovato su %s per questo account",
    "Dati di salvataggio non validi",
    "Scaricato da %s (%zu file). Premi Riavvia",
    "Autenticazione Google Drive fallita (accedi di nuovo)",
    "Errore durante il caricamento su Google Drive",
    "Caricato su Google Drive (%zu file)",
    "Nessun salvataggio trovato sul tuo Google Drive",
    "Errore durante il download da Google Drive",
    "File di salvataggio Google Drive non valido",
    "Scaricato da Google Drive (%zu file). Premi Riavvia",
    "Errore durante il caricamento su GitHub Gist",
    "Caricato su GitHub Gist (%zu file)",
    "Nessun salvataggio trovato nei tuoi Gist di GitHub",
    "Errore durante il download da GitHub Gist",
    "Il Gist non contiene un salvataggio di PvZ2",
    "Impossibile estrarre il salvataggio dal Gist",
    "Scaricato da GitHub Gist (%zu file). Premi Riavvia",
    "Connesso a Discord come %s (%s)",
    "Discord locale non rilevato (apri Discord)",
    "Salvataggio esportato in JSON (%zu file)",
    "Errore durante il salvataggio del file JSON",
    "Salvataggio importato da JSON (%zu file). Premi Riavvia",
    "Errore durante la lettura del file JSON",
    "Il file JSON non contiene un salvataggio PvZ 2 valido"
};

const CloudStrings *get_cloud_strings(const char *locale = nullptr) {
    const char *loc = (locale && locale[0]) ? locale : g_last_locale;
    if (!loc || !loc[0]) {
        const pvz2_config_t *cfg = pvz2_config();
        loc = (cfg && cfg->user_locale[0]) ? cfg->user_locale : "en_US";
    } else {
        std::snprintf(g_last_locale, sizeof(g_last_locale), "%s", loc);
    }
    if (std::strncmp(loc, "es", 2) == 0) return &kCloudES;
    if (std::strncmp(loc, "fr", 2) == 0) return &kCloudFR;
    if (std::strncmp(loc, "de", 2) == 0) return &kCloudDE;
    if (std::strncmp(loc, "pt", 2) == 0) return &kCloudPT;
    if (std::strncmp(loc, "it", 2) == 0) return &kCloudIT;
    return &kCloudEN;
}

void copy_str(char *dst, std::size_t cap, const std::string &src) {
    if (cap == 0) return;
    const std::size_t n = std::min(cap - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && ws((unsigned char)s.back())) s.pop_back();
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

std::string to_lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string config_ini_path() {
    if (char *base = pvz2_host_data_path()) {
        std::string r = std::string(base) + "config.ini";
        SDL_free(base);
        return r;
    }
    return "config.ini";
}

void load_cloud_section() {
    std::ifstream f(config_ini_path());
    if (!f) return;

    std::string line, section;
    while (std::getline(f, line)) {
        const std::size_t hash = line.find_first_of("#;");
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = to_lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        if (section != "cloud") continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = to_lower(trim(line.substr(0, eq)));
        const std::string v = trim(line.substr(eq + 1));
        if (k == "provider") {
            int p = std::atoi(v.c_str());
            if (p >= 0 && p < PVZ2_CLOUD_PROVIDER_COUNT) g_cfg.provider = p;
        } else if (k == "auto_sync") {
            g_cfg.auto_sync = (v == "1" || to_lower(v) == "true" || to_lower(v) == "yes") ? 1 : 0;
        } else if (k == "nas_server_url" && !v.empty()) {
            if (v == "http://zimaos.local:8787" || v == "http://10.246.0.1:8765") {
                copy_str(g_cfg.nas_server_url, sizeof(g_cfg.nas_server_url), PVZ2_DEFAULT_NAS_SERVER_URL);
            } else {
                copy_str(g_cfg.nas_server_url, sizeof(g_cfg.nas_server_url), v);
            }
        } else if (k == "server_url" && !v.empty()) {
            copy_str(g_cfg.server_url, sizeof(g_cfg.server_url), v);
        } else if (k == "auth_discord_id") {
            copy_str(g_cfg.auth_discord_id, sizeof(g_cfg.auth_discord_id), v);
        } else if (k == "auth_discord_name") {
            copy_str(g_cfg.auth_discord_name, sizeof(g_cfg.auth_discord_name), v);
        } else if (k == "auth_discord_token") {
            copy_str(g_cfg.auth_discord_token, sizeof(g_cfg.auth_discord_token), v);
        } else if (k == "gdrive_user") {
            copy_str(g_cfg.gdrive_user, sizeof(g_cfg.gdrive_user), v);
        } else if (k == "gdrive_token") {
            copy_str(g_cfg.gdrive_token, sizeof(g_cfg.gdrive_token), v);
        } else if (k == "gdrive_refresh_token") {
            copy_str(g_cfg.gdrive_refresh_token, sizeof(g_cfg.gdrive_refresh_token), v);
        } else if (k == "gdrive_file_id") {
            copy_str(g_cfg.gdrive_file_id, sizeof(g_cfg.gdrive_file_id), v);
        } else if (k == "github_user") {
            copy_str(g_cfg.github_user, sizeof(g_cfg.github_user), v);
        } else if (k == "gist_token") {
            copy_str(g_cfg.gist_token, sizeof(g_cfg.gist_token), v);
        } else if (k == "gist_id") {
            copy_str(g_cfg.gist_id, sizeof(g_cfg.gist_id), v);
        }
    }
}

struct SaveFileEntry {
    std::string rel_path;
    fs::path host_path;
};

fs::path local_save_dir() {
    const pvz2_config_t *cfg = pvz2_config();
    if (cfg && cfg->save_dir[0] != '\0') return fs::path(cfg->save_dir);
    return fs::path("save");
}

fs::path local_userdata_dir() {
    return fs::path("userdata");
}

std::vector<SaveFileEntry> collect_local_save_files() {
    std::vector<SaveFileEntry> out;
    std::error_code ec;

    const fs::path sdir = local_save_dir();
    if (fs::exists(sdir, ec) && fs::is_directory(sdir, ec)) {
        for (auto it = fs::recursive_directory_iterator(sdir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            fs::path rel = fs::relative(it->path(), sdir, ec);
            if (ec) continue;
            out.push_back({"save/" + rel.generic_string(), it->path()});
        }
    }

    const fs::path udir = local_userdata_dir();
    if (fs::exists(udir, ec) && fs::is_directory(udir, ec)) {
        for (auto it = fs::recursive_directory_iterator(udir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            fs::path rel = fs::relative(it->path(), udir, ec);
            if (ec) continue;
            std::string rel_s = rel.generic_string();
            if (rel_s.rfind("CDN.", 0) == 0 || rel_s.rfind("resumableDownload", 0) == 0) {
                continue;
            }
            out.push_back({"userdata/" + rel_s, it->path()});
        }
    }
    return out;
}

fs::path resolve_local_rel_path(const std::string &rel_path) {
    if (rel_path.rfind("save/", 0) == 0) {
        return local_save_dir() / fs::path(rel_path.substr(5));
    }
    if (rel_path.rfind("userdata/", 0) == 0) {
        return local_userdata_dir() / fs::path(rel_path.substr(9));
    }
    return fs::path(rel_path);
}

std::string safe_id_slug(const std::string &raw) {
    std::string safe;
    for (char c : raw) {
        if (std::isalnum((unsigned char)c) || c == '_' || c == '-') safe.push_back(c);
    }
    return safe.empty() ? "default" : safe;
}

/* --- Base64 & JSON bundle helpers ---------------------------------------- */

static const char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<std::uint8_t> &in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        std::uint32_t v = ((std::uint32_t)in[i] << 16) |
                          ((std::uint32_t)in[i + 1] << 8) |
                          (std::uint32_t)in[i + 2];
        out.push_back(kB64Table[(v >> 18) & 0x3F]);
        out.push_back(kB64Table[(v >> 12) & 0x3F]);
        out.push_back(kB64Table[(v >> 6) & 0x3F]);
        out.push_back(kB64Table[v & 0x3F]);
        i += 3;
    }
    if (i < in.size()) {
        std::uint32_t v = (std::uint32_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (std::uint32_t)in[i + 1] << 8;
        out.push_back(kB64Table[(v >> 18) & 0x3F]);
        out.push_back(kB64Table[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < in.size()) ? kB64Table[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<std::uint8_t> base64_decode(const std::string &in) {
    static int T[256];
    static bool init = false;
    if (!init) {
        std::fill(std::begin(T), std::end(T), -1);
        for (int i = 0; i < 64; ++i) T[(unsigned char)kB64Table[i]] = i;
        init = true;
    }
    std::vector<std::uint8_t> out;
    out.reserve((in.size() * 3) / 4);
    std::uint32_t val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) {
            if (c == '=') break;
            continue;
        }
        val = (val << 6) + (std::uint32_t)T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((std::uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

bool read_file_bytes(const fs::path &p, std::vector<std::uint8_t> &out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz < 0) return false;
    out.resize((std::size_t)sz);
    f.seekg(0, std::ios::beg);
    if (sz > 0 && !f.read(reinterpret_cast<char *>(out.data()), sz)) return false;
    return true;
}

bool write_file_bytes(const fs::path &p, const std::vector<std::uint8_t> &data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!data.empty()) {
        f.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
    }
    return (bool)f;
}

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

std::string build_bundle_json(std::size_t *file_count_out) {
    const auto files = collect_local_save_files();
    if (file_count_out) *file_count_out = 0;
    std::ostringstream ss;
    ss << "{\"format\":\"pvz2_cloud_v1\","
       << "\"discord_id\":\"" << json_escape(g_cfg.auth_discord_id) << "\","
       << "\"discord_name\":\"" << json_escape(g_cfg.auth_discord_name) << "\","
       << "\"timestamp\":" << (long long)std::time(nullptr) << ","
       << "\"files\":{";
    bool first = true;
    std::size_t count = 0;
    for (const auto &entry : files) {
        std::vector<std::uint8_t> bytes;
        if (!read_file_bytes(entry.host_path, bytes)) continue;
        if (!first) ss << ",";
        first = false;
        ss << "\"" << json_escape(entry.rel_path) << "\":\"" << base64_encode(bytes) << "\"";
        ++count;
    }
    ss << "}}";
    if (file_count_out) *file_count_out = count;
    return ss.str();
}

bool unpack_bundle_json(const std::string &json, std::size_t *restored_out) {
    if (restored_out) *restored_out = 0;
    const std::size_t files_pos = json.find("\"files\"");
    if (files_pos == std::string::npos) return false;
    const std::size_t brace = json.find('{', files_pos);
    if (brace == std::string::npos) return false;

    std::size_t i = brace + 1;
    std::size_t restored = 0;
    auto parse_str = [&](std::string &out) -> bool {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' ||
                                   json[i] == '\n' || json[i] == ',')) {
            ++i;
        }
        if (i >= json.size() || json[i] != '"') return false;
        ++i;
        out.clear();
        while (i < json.size()) {
            char c = json[i++];
            if (c == '"') return true;
            if (c == '\\' && i < json.size()) {
                char e = json[i++];
                switch (e) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    default:   out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    };

    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' ||
                                   json[i] == '\n' || json[i] == ',')) {
            ++i;
        }
        if (i >= json.size() || json[i] == '}') break;
        std::string rel_path;
        if (!parse_str(rel_path)) break;
        while (i < json.size() && json[i] != ':') ++i;
        if (i >= json.size()) break;
        ++i;
        std::string b64;
        if (!parse_str(b64)) break;
        if (rel_path.find("..") != std::string::npos) continue;
        const std::vector<std::uint8_t> data = base64_decode(b64);
        const fs::path dst = resolve_local_rel_path(rel_path);
        if (write_file_bytes(dst, data)) ++restored;
    }

    if (restored_out) *restored_out = restored;
    return restored > 0;
}

/* --- Subprocess helper for curl ------------------------------------------- */

int run_cmd_quiet(const std::string &cmd) {
#if defined(_WIN32)
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::string cmdline = "cmd.exe /C \"" + cmd + "\"";
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 25000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
#else
    return std::system(cmd.c_str());
#endif
}

std::string json_extract_string(const std::string &json, const std::string &key, std::size_t from = 0) {
    const std::string pat = "\"" + key + "\"";
    const std::size_t pos = json.find(pat, from);
    if (pos == std::string::npos) return "";
    const std::size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return "";
    const std::size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (std::size_t i = q1 + 1; i < json.size(); ++i) {
        if (json[i] == '"') return out;
        if (json[i] == '\\' && i + 1 < json.size()) {
            out.push_back(json[++i]);
        } else {
            out.push_back(json[i]);
        }
    }
    return "";
}

/* --- URL query decoder for local loopback OAuth callback ------------------ */

std::string url_decode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out.push_back(' ');
        } else if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out.push_back((char)std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string query_param(const std::string &query, const std::string &key) {
    const std::string pat = key + "=";
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        if (pair.rfind(pat, 0) == 0) {
            return url_decode(pair.substr(pat.size()));
        }
        pos = amp + 1;
    }
    return "";
}

/* --- Browser OAuth Loopback Portal (Discord / Google Drive / GitHub Gist) - */

constexpr int kAuthPort = 53682;

constexpr const char *kGoogleClientId = "202264815644.apps.googleusercontent.com";
constexpr const char *kGoogleClientSecret = "X4Z3ca8xfWDb1Voo-F9a7ZxJ";

constexpr const char *kGithubClientId = "178c6fc778ccc68e1d6a";
constexpr const char *kGithubClientSecret = "34ddeff2b558a23d38fba8a6de74f086ede1cc0b";

constexpr const char *kDiscordPkceVerifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
constexpr const char *kDiscordPkceChallenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";

std::string build_google_oauth_url() {
    std::ostringstream u;
    u << "https://accounts.google.com/o/oauth2/auth"
      << "?client_id=" << kGoogleClientId
      << "&redirect_uri=http%3A%2F%2F127.0.0.1%3A" << kAuthPort << "%2F"
      << "&response_type=code"
      << "&scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive.file+email+profile"
      << "&access_type=offline"
      << "&prompt=select_account+consent"
      << "&state=gdrive";
    return u.str();
}

std::string build_github_oauth_url() {
    std::ostringstream u;
    u << "https://github.com/login/oauth/authorize"
      << "?client_id=" << kGithubClientId
      << "&redirect_uri=http%3A%2F%2F127.0.0.1%3A" << kAuthPort << "%2Fcallback"
      << "&scope=gist"
      << "&state=github";
    return u.str();
}

std::string build_discord_oauth_url() {
    const char *client_id = pvz2_config()->discord_client_id[0]
        ? pvz2_config()->discord_client_id
        : "1560077802550853694";
    std::ostringstream u;
    u << "https://discord.com/oauth2/authorize"
      << "?client_id=" << client_id
      << "&response_type=code"
      << "&redirect_uri=http%3A%2F%2F127.0.0.1%3A" << kAuthPort << "%2Fauth"
      << "&scope=identify"
      << "&state=discord"
      << "&code_challenge=" << kDiscordPkceChallenge
      << "&code_challenge_method=S256"
      << "&prompt=consent";
    return u.str();
}

#if defined(_WIN32)
BOOL CALLBACK enum_focus_hwnd_cb(HWND hwnd, LPARAM lparam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == (DWORD)lparam && IsWindowVisible(hwnd)) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | 0, 0);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
        return FALSE;
    }
    return TRUE;
}
#endif

void focus_game_window() {
#if defined(_WIN32)
    EnumWindows(enum_focus_hwnd_cb, (LPARAM)GetCurrentProcessId());
#else
    for (Uint32 id = 1; id < 16; ++id) {
        if (SDL_Window *w = SDL_GetWindowFromID(id)) {
            SDL_RaiseWindow(w);
        }
    }
#endif
}

std::string build_result_html(bool ok, const std::string &title, const std::string &subtitle) {
    const CloudStrings *cs = get_cloud_strings();
    std::ostringstream h;
    h << "<!DOCTYPE html><html lang='" << cs->html_lang << "'><head><meta charset='utf-8'>"
      << "<title>" << cs->html_title << "</title>"
      << "<style>"
      << "body{margin:0;font-family:'Segoe UI',system-ui,sans-serif;background:#13151a;color:#f3f4f6;"
      << "display:flex;align-items:center;justify-content:center;min-height:100vh;}"
      << ".card{background:#1e222b;border:2px solid " << (ok ? "#22c55e" : "#ef4444")
      << ";border-radius:16px;padding:32px;max-width:460px;width:90%;"
      << "box-shadow:0 20px 50px rgba(0,0,0,0.6);text-align:center;}"
      << "h1{margin:0 0 10px;color:" << (ok ? "#4ade80" : "#f87171") << ";font-size:24px;}"
      << "p{color:#d1d5db;font-size:15px;line-height:1.5;margin:0;}"
      << "</style></head><body><div class='card'>"
      << "<h1>" << title << "</h1>"
      << "<p>" << subtitle << "</p>"
      << (ok ? "<script>setTimeout(function(){window.close();},1200);</script>" : "")
      << "</div></body></html>";
    return h.str();
}

std::string build_auth_html(int provider) {
    std::string local_id, local_name;
    {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        local_id = g_cfg.local_discord_id;
        local_name = g_cfg.local_discord_name;
    }

    std::ostringstream h;
    h << "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>"
      << "<title>PvZ2Native - Guardado en la Nube</title>"
      << "<style>"
      << "body{margin:0;font-family:'Segoe UI',system-ui,sans-serif;background:#13151a;color:#f3f4f6;"
      << "display:flex;align-items:center;justify-content:center;min-height:100vh;}"
      << ".card{background:#1e222b;border:2px solid #3a4152;border-radius:16px;padding:32px;max-width:460px;width:90%;"
      << "box-shadow:0 20px 50px rgba(0,0,0,0.6);text-align:center;}"
      << "h1{margin:0 0 8px;color:#fddc67;font-size:24px;}"
      << "p{color:#9ca3af;font-size:14px;line-height:1.5;margin:0 0 20px;}"
      << ".btn{display:block;width:100%;box-sizing:border-box;padding:13px 18px;margin:10px 0;border-radius:10px;"
      << "font-weight:600;font-size:15px;cursor:pointer;border:none;text-decoration:none;transition:0.15s;}"
      << ".btn-discord{background:#5865F2;color:#fff;}.btn-discord:hover{background:#4752C4;}"
      << ".btn-google{background:#2563eb;color:#fff;}.btn-google:hover{background:#1d4ed8;}"
      << ".btn-github{background:#24292f;color:#fff;border:1px solid #4b5563;}.btn-github:hover{background:#32383f;}"
      << ".btn-green{background:#16a34a;color:#fff;}.btn-green:hover{background:#15803d;}"
      << ".msg{margin-top:14px;font-size:14px;color:#4ade80;min-height:20px;}"
      << ".err{color:#f87171;}"
      << ".sep{margin:18px 0;border-top:1px solid #2e3442;position:relative;}"
      << "</style></head><body><div class='card'>";

    if (provider == PVZ2_CLOUD_GDRIVE) {
        h << "<h1>Conectar Google Drive</h1>"
          << "<p>Autoriza el acceso a Google Drive para guardar y sincronizar tu partida automáticamente.</p>"
          << "<a class='btn btn-google' href='" << build_google_oauth_url() << "'>"
          << "Continuar con Google Drive</a>";
    } else if (provider == PVZ2_CLOUD_GIST) {
        h << "<h1>Conectar GitHub Gist</h1>"
          << "<p>Autoriza tu cuenta de GitHub para guardar tu partida en un Gist privado automáticamente.</p>"
          << "<a class='btn btn-github' href='" << build_github_oauth_url() << "'>"
          << "Continuar con GitHub</a>";
    } else {
        h << "<h1>Conectar cuenta de Discord</h1>"
          << "<p>Elige la cuenta de Discord donde se registrará tu partida en la nube.</p>";
        if (!local_id.empty()) {
            h << "<button class='btn btn-green' onclick=\"finish('discord','"
              << json_escape(local_id) << "','" << json_escape(local_name) << "','ipc_verified')\">"
              << "Continuar como " << json_escape(local_name) << " (" << json_escape(local_id) << ")"
              << "</button><div class='sep'></div>";
        }
        h << "<a class='btn btn-discord' href='" << build_discord_oauth_url() << "'>"
          << "Iniciar sesión con otra cuenta en Discord.com</a>";
    }

    h << "<div id='status' class='msg'></div></div>"
      << "<script>"
      << "function setMsg(t,err){var el=document.getElementById('status');el.className=err?'msg err':'msg';el.textContent=t;}"
      << "function finish(prov,id,name,tok){"
      << "  setMsg('Vinculando con PvZ2Native...',false);"
      << "  fetch('/complete?provider='+encodeURIComponent(prov)+'&id='+encodeURIComponent(id)+'&name='+encodeURIComponent(name)+'&token='+encodeURIComponent(tok))"
      << "  .then(()=>{document.querySelector('.card').innerHTML='<h1 style=\"color:#4ade80\">¡Cuenta vinculada!</h1><p>Ya puedes cerrar esta pestaña y volver a Plants vs. Zombies 2.</p>';});"
      << "}"
      << "if(window.location.hash.indexOf('access_token=')!==-1){"
      << "  var params=new URLSearchParams(window.location.hash.substring(1));"
      << "  var tok=params.get('access_token');"
      << "  setMsg('Verificando cuenta con Discord...',false);"
      << "  fetch('https://discord.com/api/v10/users/@me',{headers:{Authorization:'Bearer '+tok}})"
      << "  .then(r=>r.json()).then(u=>{if(u&&u.id)finish('discord',u.id,u.username||u.global_name||'User',tok);else setMsg('Error al verificar Discord',true);})"
      << "  .catch(()=>setMsg('Error de red al verificar Discord',true));"
      << "}"
      << "</script></body></html>";
    return h.str();
}

bool exchange_google_oauth_code(const std::string &code, std::string &user_out) {
    const fs::path tmp_out = fs::temp_directory_path() / "pvz2_google_tok.json";
    std::error_code ec;
    fs::remove(tmp_out, ec);

    std::ostringstream cmd;
    cmd << "curl -sS -f -m 15 -X POST "
        << "--data-urlencode \"code=" << code << "\" "
        << "-d \"client_id=" << kGoogleClientId << "\" "
        << "-d \"client_secret=" << kGoogleClientSecret << "\" "
        << "-d \"redirect_uri=http://127.0.0.1:" << kAuthPort << "/\" "
        << "-d \"grant_type=authorization_code\" "
        << "-o \"" << tmp_out.string() << "\" "
        << "\"https://oauth2.googleapis.com/token\"";

    if (run_cmd_quiet(cmd.str()) != 0) {
        fs::remove(tmp_out, ec);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp_out, bytes)) return false;
    fs::remove(tmp_out, ec);

    const std::string tjson(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::string access_tok = json_extract_string(tjson, "access_token");
    const std::string refresh_tok = json_extract_string(tjson, "refresh_token");
    if (access_tok.empty()) return false;

    std::string email = "Google Drive";
    const fs::path tmp_u = fs::temp_directory_path() / "pvz2_google_user.json";
    fs::remove(tmp_u, ec);
    const std::string ucmd =
        "curl -sS -f -m 10 -H \"Authorization: Bearer " + access_tok + "\" "
        "-o \"" + tmp_u.string() + "\" "
        "\"https://www.googleapis.com/oauth2/v2/userinfo\"";
    if (run_cmd_quiet(ucmd) == 0) {
        std::vector<std::uint8_t> ubytes;
        if (read_file_bytes(tmp_u, ubytes)) {
            const std::string ujson(reinterpret_cast<const char *>(ubytes.data()), ubytes.size());
            std::string em = json_extract_string(ujson, "email");
            if (em.empty()) em = json_extract_string(ujson, "name");
            if (!em.empty()) email = em;
        }
    }
    fs::remove(tmp_u, ec);

    {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        copy_str(g_cfg.gdrive_user, sizeof(g_cfg.gdrive_user), email);
        copy_str(g_cfg.gdrive_token, sizeof(g_cfg.gdrive_token), access_tok);
        if (!refresh_tok.empty()) {
            copy_str(g_cfg.gdrive_refresh_token, sizeof(g_cfg.gdrive_refresh_token), refresh_tok);
        }
        g_cfg.gdrive_file_id[0] = '\0';
    }
    pvz2_cloud_save_config();
    user_out = email;
    return true;
}

bool refresh_google_access_token() {
    std::string refresh_tok;
    {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        refresh_tok = g_cfg.gdrive_refresh_token;
    }
    if (refresh_tok.empty()) return false;

    const fs::path tmp_out = fs::temp_directory_path() / "pvz2_google_ref.json";
    std::error_code ec;
    fs::remove(tmp_out, ec);

    std::ostringstream cmd;
    cmd << "curl -sS -f -m 15 -X POST "
        << "--data-urlencode \"refresh_token=" << refresh_tok << "\" "
        << "-d \"client_id=" << kGoogleClientId << "\" "
        << "-d \"client_secret=" << kGoogleClientSecret << "\" "
        << "-d \"grant_type=refresh_token\" "
        << "-o \"" << tmp_out.string() << "\" "
        << "\"https://oauth2.googleapis.com/token\"";

    if (run_cmd_quiet(cmd.str()) != 0) {
        fs::remove(tmp_out, ec);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp_out, bytes)) return false;
    fs::remove(tmp_out, ec);

    const std::string tjson(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::string access_tok = json_extract_string(tjson, "access_token");
    if (access_tok.empty()) return false;

    {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        copy_str(g_cfg.gdrive_token, sizeof(g_cfg.gdrive_token), access_tok);
    }
    pvz2_cloud_save_config();
    return true;
}

bool exchange_github_oauth_code(const std::string &code, std::string &user_out) {
    const fs::path tmp_out = fs::temp_directory_path() / "pvz2_github_tok.json";
    std::error_code ec;
    fs::remove(tmp_out, ec);

    std::ostringstream cmd;
    cmd << "curl -sS -f -m 15 -X POST "
        << "-H \"Accept: application/json\" "
        << "--data-urlencode \"code=" << code << "\" "
        << "-d \"client_id=" << kGithubClientId << "\" "
        << "-d \"client_secret=" << kGithubClientSecret << "\" "
        << "-d \"redirect_uri=http://127.0.0.1:" << kAuthPort << "/callback\" "
        << "-o \"" << tmp_out.string() << "\" "
        << "\"https://github.com/login/oauth/access_token\"";

    if (run_cmd_quiet(cmd.str()) != 0) {
        fs::remove(tmp_out, ec);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp_out, bytes)) return false;
    fs::remove(tmp_out, ec);

    const std::string tjson(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::string access_tok = json_extract_string(tjson, "access_token");
    if (access_tok.empty()) return false;

    std::string login = "GitHub";
    const fs::path tmp_u = fs::temp_directory_path() / "pvz2_github_user.json";
    fs::remove(tmp_u, ec);
    const std::string ucmd =
        "curl -sS -f -m 10 -H \"Accept: application/vnd.github+json\" "
        "-H \"Authorization: Bearer " + access_tok + "\" "
        "-o \"" + tmp_u.string() + "\" "
        "\"https://api.github.com/user\"";
    if (run_cmd_quiet(ucmd) == 0) {
        std::vector<std::uint8_t> ubytes;
        if (read_file_bytes(tmp_u, ubytes)) {
            const std::string ujson(reinterpret_cast<const char *>(ubytes.data()), ubytes.size());
            std::string lg = json_extract_string(ujson, "login");
            if (!lg.empty()) login = lg;
        }
    }
    fs::remove(tmp_u, ec);

    {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        copy_str(g_cfg.github_user, sizeof(g_cfg.github_user), login);
        copy_str(g_cfg.gist_token, sizeof(g_cfg.gist_token), access_tok);
        g_cfg.gist_id[0] = '\0';
    }
    pvz2_cloud_save_config();
    user_out = login;
    return true;
}

bool exchange_discord_oauth_code(const std::string &code, std::string &user_out) {
    const char *client_id = pvz2_config()->discord_client_id[0]
        ? pvz2_config()->discord_client_id
        : "1560077802550853694";
    const fs::path tmp_out = fs::temp_directory_path() / "pvz2_discord_tok.json";
    std::error_code ec;
    fs::remove(tmp_out, ec);

    std::ostringstream cmd;
    cmd << "curl -sS -f -m 15 -X POST "
        << "-H \"Content-Type: application/x-www-form-urlencoded\" "
        << "--data-urlencode \"code=" << code << "\" "
        << "-d \"client_id=" << client_id << "\" "
        << "-d \"grant_type=authorization_code\" "
        << "-d \"redirect_uri=http://127.0.0.1:" << kAuthPort << "/auth\" "
        << "-d \"code_verifier=" << kDiscordPkceVerifier << "\" "
        << "-o \"" << tmp_out.string() << "\" "
        << "\"https://discord.com/api/v10/oauth2/token\"";

    if (run_cmd_quiet(cmd.str()) != 0) {
        fs::remove(tmp_out, ec);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp_out, bytes)) return false;
    fs::remove(tmp_out, ec);

    const std::string tjson(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::string access_tok = json_extract_string(tjson, "access_token");
    if (access_tok.empty()) return false;

    const fs::path tmp_u = fs::temp_directory_path() / "pvz2_discord_user.json";
    fs::remove(tmp_u, ec);
    const std::string ucmd =
        "curl -sS -f -m 10 -H \"Authorization: Bearer " + access_tok + "\" "
        "-o \"" + tmp_u.string() + "\" "
        "\"https://discord.com/api/v10/users/@me\"";
    if (run_cmd_quiet(ucmd) != 0) {
        fs::remove(tmp_u, ec);
        return false;
    }

    std::vector<std::uint8_t> ubytes;
    if (!read_file_bytes(tmp_u, ubytes)) return false;
    fs::remove(tmp_u, ec);

    const std::string ujson(reinterpret_cast<const char *>(ubytes.data()), ubytes.size());
    const std::string id = json_extract_string(ujson, "id");
    std::string username = json_extract_string(ujson, "username");
    if (username.empty()) username = json_extract_string(ujson, "global_name");
    if (id.empty()) return false;

    {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        copy_str(g_cfg.auth_discord_id, sizeof(g_cfg.auth_discord_id), id);
        copy_str(g_cfg.auth_discord_name, sizeof(g_cfg.auth_discord_name), username);
        copy_str(g_cfg.auth_discord_token, sizeof(g_cfg.auth_discord_token), access_tok);
    }
    pvz2_cloud_save_config();
    user_out = username + " (" + id + ")";
    return true;
}

void close_sock(int s) {
#if defined(_WIN32)
    closesocket((SOCKET)s);
#else
    ::close(s);
#endif
}

void send_html_response(int cli, const std::string &html) {
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/html; charset=utf-8\r\n"
         << "Cache-Control: no-store, no-cache, must-revalidate\r\n"
         << "Content-Length: " << html.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << html;
    const std::string rstr = resp.str();
    ::send(cli, rstr.data(), (int)rstr.size(), 0);
}

void run_auth_server_thread() {
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int srv = (int)::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        g_auth_listening = false;
        g_auth_running = false;
        return;
    }
    int opt = 1;
#if defined(_WIN32)
    setsockopt((SOCKET)srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)kAuthPort);

    if (::bind(srv, (sockaddr *)&addr, sizeof(addr)) != 0 || ::listen(srv, 4) != 0) {
        close_sock(srv);
        g_auth_listening = false;
        g_auth_running = false;
        return;
    }

    g_auth_listening = true;

    while (std::time(nullptr) < g_auth_deadline.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
#if defined(_WIN32)
        FD_SET((SOCKET)srv, &rfds);
#else
        FD_SET(srv, &rfds);
#endif
        timeval tv{1, 0};
        int sel = ::select(srv + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        int cli = (int)::accept(srv, nullptr, nullptr);
        if (cli < 0) continue;

        char buf[8192] = {};
        int n = ::recv(cli, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::string req(buf, (std::size_t)n);
            const std::size_t sp1 = req.find(' ');
            const std::size_t sp2 = (sp1 != std::string::npos) ? req.find(' ', sp1 + 1) : std::string::npos;
            std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                ? req.substr(sp1 + 1, sp2 - sp1 - 1)
                : "/auth";

            const std::size_t qpos = path.find('?');
            const std::string route = (qpos != std::string::npos) ? path.substr(0, qpos) : path;
            const std::string query = (qpos != std::string::npos) ? path.substr(qpos + 1) : "";

            if (route == "/complete") {
                std::string prov = query_param(query, "provider");
                std::string id = query_param(query, "id");
                std::string name = query_param(query, "name");
                std::string tok = query_param(query, "token");

                {
                    std::lock_guard<std::mutex> lk(g_cfg_mutex);
                    if (prov == "discord") {
                        copy_str(g_cfg.auth_discord_id, sizeof(g_cfg.auth_discord_id), id);
                        copy_str(g_cfg.auth_discord_name, sizeof(g_cfg.auth_discord_name), name);
                        copy_str(g_cfg.auth_discord_token, sizeof(g_cfg.auth_discord_token), tok);
                    } else if (prov == "google") {
                        copy_str(g_cfg.gdrive_user, sizeof(g_cfg.gdrive_user), name);
                        copy_str(g_cfg.gdrive_token, sizeof(g_cfg.gdrive_token), tok);
                    } else if (prov == "github") {
                        copy_str(g_cfg.github_user, sizeof(g_cfg.github_user), name);
                        copy_str(g_cfg.gist_token, sizeof(g_cfg.gist_token), tok);
                    }
                }
                pvz2_cloud_save_config();

                const char *ok_resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Connection: close\r\n\r\n{\"ok\":true}";
                ::send(cli, ok_resp, (int)std::strlen(ok_resp), 0);
            } else if (!query_param(query, "code").empty()) {
                const CloudStrings *cs = get_cloud_strings();
                const std::string code = query_param(query, "code");
                const std::string state = query_param(query, "state");
                std::string user_label;

                if (route == "/callback" || state == "github") {
                    if (exchange_github_oauth_code(code, user_label)) {
                        send_html_response(
                            cli,
                            build_result_html(true, cs->github_linked,
                                              std::string(cs->connected_as) + " <b>@" + user_label +
                                                  "</b>.<br>" + cs->returning_to_game));
                        g_auth_deadline = 0;
                        g_auth_running = false;
                        focus_game_window();
                    } else {
                        send_html_response(
                            cli,
                            build_result_html(false, cs->github_link_err, cs->github_link_err_sub));
                    }
                } else if (state == "discord" || route == "/auth") {
                    if (exchange_discord_oauth_code(code, user_label)) {
                        send_html_response(
                            cli,
                            build_result_html(true, cs->discord_linked,
                                              std::string(cs->connected_as) + " <b>" + user_label +
                                                  "</b>.<br>" + cs->returning_to_game));
                        g_auth_deadline = 0;
                        g_auth_running = false;
                        focus_game_window();
                    } else {
                        send_html_response(
                            cli,
                            build_result_html(false, cs->discord_link_err, cs->discord_link_err_sub));
                    }
                } else {
                    if (exchange_google_oauth_code(code, user_label)) {
                        send_html_response(
                            cli,
                            build_result_html(true, cs->gdrive_linked,
                                              std::string(cs->connected_as) + " <b>" + user_label +
                                                  "</b>.<br>" + cs->returning_to_game));
                        g_auth_deadline = 0;
                        g_auth_running = false;
                        focus_game_window();
                    } else {
                        send_html_response(
                            cli,
                            build_result_html(false, cs->gdrive_link_err, cs->gdrive_link_err_sub));
                    }
                }
            } else {
                int active_prov = g_cfg.provider;
                const std::string p_param = query_param(query, "p");
                if (p_param == "gdrive") active_prov = PVZ2_CLOUD_GDRIVE;
                else if (p_param == "gist") active_prov = PVZ2_CLOUD_GIST;
                else if (p_param == "discord") active_prov = PVZ2_CLOUD_NAS;

                send_html_response(cli, build_auth_html(active_prov));
            }
        }
        close_sock(cli);
    }

    close_sock(srv);
    g_auth_listening = false;
    g_auth_running = false;
}

/* --- NAS (ZimaOS Default Server) & Private HTTP Server -------------------- */

std::string build_http_save_endpoint(const std::string &base_url) {
    std::string base = trim(base_url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/saves/" + safe_id_slug(g_cfg.auth_discord_id);
}

fs::path resolve_zimaos_smb_save_file() {
    const std::string fname = safe_id_slug(g_cfg.auth_discord_id) + ".json";
#if defined(_WIN32)
    std::error_code ec;
    const fs::path unc_dir = "\\\\10.246.0.1\\ZimaOS-HD\\AppData\\pvz2-cloud-server\\data\\saves";
    if (fs::exists("\\\\10.246.0.1\\ZimaOS-HD", ec)) {
        fs::create_directories(unc_dir, ec);
        return unc_dir / fname;
    }
    const fs::path z_dir = "Z:\\AppData\\pvz2-cloud-server\\data\\saves";
    if (fs::exists("Z:\\", ec)) {
        fs::create_directories(z_dir, ec);
        return z_dir / fname;
    }
#endif
    return {};
}

bool http_server_upload(const std::string &base_url, bool is_nas, char *status_out, std::size_t cap,
                        const CloudStrings *cs) {
    if (g_cfg.auth_discord_id[0] == '\0' || g_cfg.auth_discord_token[0] == '\0') {
        std::snprintf(status_out, cap, "%s", cs->signin_discord_first);
        return false;
    }
    if (base_url.empty()) {
        std::snprintf(status_out, cap, "%s", cs->err_empty_url);
        return false;
    }

    std::size_t count = 0;
    const std::string bundle = build_bundle_json(&count);
    if (count == 0) {
        std::snprintf(status_out, cap, "%s", cs->no_local_files);
        return false;
    }

    const fs::path tmp = fs::temp_directory_path() / "pvz2_cloud_upload.json";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bundle.data(), (std::streamsize)bundle.size());
    }

    const std::string url = build_http_save_endpoint(base_url);
    const std::string cmd =
        "curl -sS -f -m 15 -X PUT -H \"Content-Type: application/json\" "
        "-H \"X-Discord-Id: " + safe_id_slug(g_cfg.auth_discord_id) + "\" "
        "-H \"Authorization: Bearer " + std::string(g_cfg.auth_discord_token) + "\" "
        "--data-binary @\"" + tmp.string() + "\" \"" + url + "\"";
    int rc = run_cmd_quiet(cmd);
    std::error_code ec;
    fs::remove(tmp, ec);

    if (rc != 0 && is_nas) {
        const fs::path smb_file = resolve_zimaos_smb_save_file();
        if (!smb_file.empty()) {
            std::ofstream sf(smb_file, std::ios::binary | std::ios::trunc);
            if (sf) {
                sf.write(bundle.data(), (std::streamsize)bundle.size());
                if (sf.good()) rc = 0;
            }
        }
    }

    if (rc != 0) {
        std::snprintf(status_out, cap, cs->conn_failed_fmt,
                      is_nas ? "Cloud" : cs->server_word);
        return false;
    }
    std::snprintf(status_out, cap, cs->uploaded_fmt,
                  is_nas ? "Cloud" : cs->server_word, count);
    return true;
}

bool http_server_download(const std::string &base_url, bool is_nas, char *status_out, std::size_t cap,
                          const CloudStrings *cs) {
    if (g_cfg.auth_discord_id[0] == '\0' || g_cfg.auth_discord_token[0] == '\0') {
        std::snprintf(status_out, cap, "%s", cs->signin_discord_first);
        return false;
    }
    if (base_url.empty()) {
        std::snprintf(status_out, cap, "%s", cs->err_empty_url);
        return false;
    }

    const fs::path tmp = fs::temp_directory_path() / "pvz2_cloud_download.json";
    std::error_code ec;
    fs::remove(tmp, ec);

    const std::string url = build_http_save_endpoint(base_url);
    const std::string cmd =
        "curl -sS -f -m 15 -H \"X-Discord-Id: " + safe_id_slug(g_cfg.auth_discord_id) + "\" "
        "-H \"Authorization: Bearer " + std::string(g_cfg.auth_discord_token) + "\" "
        "-o \"" + tmp.string() + "\" \"" + url + "\"";
    int rc = run_cmd_quiet(cmd);

    std::vector<std::uint8_t> bytes;
    bool got_bytes = false;
    if (rc == 0 && fs::exists(tmp, ec)) {
        got_bytes = read_file_bytes(tmp, bytes);
        fs::remove(tmp, ec);
    } else if (is_nas) {
        fs::remove(tmp, ec);
        const fs::path smb_file = resolve_zimaos_smb_save_file();
        if (!smb_file.empty() && fs::exists(smb_file, ec)) {
            got_bytes = read_file_bytes(smb_file, bytes);
        }
    }

    if (!got_bytes) {
        std::snprintf(status_out, cap, cs->no_save_found_fmt,
                      is_nas ? "Cloud" : cs->server_word);
        return false;
    }

    std::size_t restored = 0;
    const std::string json(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (!unpack_bundle_json(json, &restored)) {
        std::snprintf(status_out, cap, "%s", cs->invalid_save_data);
        return false;
    }

    g_writes_locked = true;
    std::snprintf(status_out, cap, cs->downloaded_restart_fmt,
                  is_nas ? "Cloud" : cs->server_word, restored);
    return true;
}

/* --- Google Drive REST API v3 --------------------------------------------- */

std::string gdrive_find_file_id() {
    if (g_cfg.gdrive_file_id[0] != '\0') return g_cfg.gdrive_file_id;
    const fs::path tmp = fs::temp_directory_path() / "pvz2_gd_list.json";
    std::error_code ec;
    fs::remove(tmp, ec);

    const std::string url =
        "https://www.googleapis.com/drive/v3/files?q=name%3D'pvz2_cloud_save.json'+and+trashed%3Dfalse&fields=files(id,name)";
    std::string cmd =
        "curl -sS -f -m 15 -H \"Authorization: Bearer " + std::string(g_cfg.gdrive_token) + "\" "
        "-o \"" + tmp.string() + "\" \"" + url + "\"";
    if (run_cmd_quiet(cmd) != 0) {
        if (refresh_google_access_token()) {
            cmd = "curl -sS -f -m 15 -H \"Authorization: Bearer " + std::string(g_cfg.gdrive_token) + "\" "
                  "-o \"" + tmp.string() + "\" \"" + url + "\"";
            if (run_cmd_quiet(cmd) != 0) return "";
        } else {
            return "";
        }
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp, bytes)) return "";
    fs::remove(tmp, ec);

    const std::string resp(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    std::string id = json_extract_string(resp, "id");
    if (!id.empty()) {
        copy_str(g_cfg.gdrive_file_id, sizeof(g_cfg.gdrive_file_id), id);
        pvz2_cloud_save_config();
    }
    return id;
}

bool gdrive_api_upload(char *status_out, std::size_t cap, const CloudStrings *cs) {
    if (g_cfg.gdrive_token[0] == '\0') {
        std::snprintf(status_out, cap, "%s", cs->signin_google_first);
        return false;
    }
    std::size_t count = 0;
    const std::string bundle = build_bundle_json(&count);
    if (count == 0) {
        std::snprintf(status_out, cap, "%s", cs->no_local_files);
        return false;
    }

    std::string file_id = gdrive_find_file_id();
    std::error_code ec;
    if (file_id.empty()) {
        /* Create file metadata */
        const fs::path meta_in = fs::temp_directory_path() / "pvz2_gd_meta.json";
        const fs::path meta_out = fs::temp_directory_path() / "pvz2_gd_create.json";
        {
            std::ofstream f(meta_in, std::ios::binary | std::ios::trunc);
            f << "{\"name\":\"pvz2_cloud_save.json\",\"mimeType\":\"application/json\"}";
        }
        const std::string create_cmd =
            "curl -sS -f -m 15 -X POST "
            "-H \"Authorization: Bearer " + std::string(g_cfg.gdrive_token) + "\" "
            "-H \"Content-Type: application/json\" "
            "--data-binary @\"" + meta_in.string() + "\" "
            "-o \"" + meta_out.string() + "\" "
            "\"https://www.googleapis.com/drive/v3/files\"";
        const int crc = run_cmd_quiet(create_cmd);
        fs::remove(meta_in, ec);
        if (crc == 0) {
            std::vector<std::uint8_t> rbytes;
            if (read_file_bytes(meta_out, rbytes)) {
                const std::string rstr(reinterpret_cast<const char *>(rbytes.data()), rbytes.size());
                file_id = json_extract_string(rstr, "id");
                if (!file_id.empty()) {
                    copy_str(g_cfg.gdrive_file_id, sizeof(g_cfg.gdrive_file_id), file_id);
                    pvz2_cloud_save_config();
                }
            }
        }
        fs::remove(meta_out, ec);
    }

    if (file_id.empty()) {
        std::snprintf(status_out, cap, "%s", cs->gdrive_auth_failed);
        return false;
    }

    const fs::path tmp_in = fs::temp_directory_path() / "pvz2_gd_upload.json";
    {
        std::ofstream f(tmp_in, std::ios::binary | std::ios::trunc);
        f.write(bundle.data(), (std::streamsize)bundle.size());
    }
    const std::string up_cmd =
        "curl -sS -f -m 20 -X PATCH "
        "-H \"Authorization: Bearer " + std::string(g_cfg.gdrive_token) + "\" "
        "-H \"Content-Type: application/json\" "
        "--data-binary @\"" + tmp_in.string() + "\" "
        "\"https://www.googleapis.com/upload/drive/v3/files/" + file_id + "?uploadType=media\"";
    const int rc = run_cmd_quiet(up_cmd);
    fs::remove(tmp_in, ec);

    if (rc != 0) {
        std::snprintf(status_out, cap, "%s", cs->gdrive_upload_err);
        return false;
    }
    std::snprintf(status_out, cap, cs->gdrive_uploaded_fmt, count);
    return true;
}

bool gdrive_api_download(char *status_out, std::size_t cap, const CloudStrings *cs) {
    if (g_cfg.gdrive_token[0] == '\0') {
        std::snprintf(status_out, cap, "%s", cs->signin_google_first);
        return false;
    }
    std::string file_id = gdrive_find_file_id();
    if (file_id.empty()) {
        std::snprintf(status_out, cap, "%s", cs->gdrive_no_save);
        return false;
    }

    const fs::path tmp_out = fs::temp_directory_path() / "pvz2_gd_dl.json";
    std::error_code ec;
    fs::remove(tmp_out, ec);

    const std::string dl_cmd =
        "curl -sS -f -m 20 -H \"Authorization: Bearer " + std::string(g_cfg.gdrive_token) + "\" "
        "-o \"" + tmp_out.string() + "\" "
        "\"https://www.googleapis.com/drive/v3/files/" + file_id + "?alt=media\"";
    if (run_cmd_quiet(dl_cmd) != 0 || !fs::exists(tmp_out, ec)) {
        std::snprintf(status_out, cap, "%s", cs->gdrive_download_err);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp_out, bytes)) {
        fs::remove(tmp_out, ec);
        return false;
    }
    fs::remove(tmp_out, ec);

    std::size_t restored = 0;
    const std::string json(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (!unpack_bundle_json(json, &restored)) {
        std::snprintf(status_out, cap, "%s", cs->gdrive_invalid_file);
        return false;
    }

    g_writes_locked = true;
    std::snprintf(status_out, cap, cs->gdrive_downloaded_fmt, restored);
    return true;
}

/* --- GitHub Gist API (with auto-discovery of existing Gist) --------------- */

std::string gist_find_existing_id() {
    if (g_cfg.gist_id[0] != '\0') return g_cfg.gist_id;
    const fs::path tmp = fs::temp_directory_path() / "pvz2_gist_list.json";
    std::error_code ec;
    fs::remove(tmp, ec);

    const std::string cmd =
        "curl -sS -f -m 15 -H \"Accept: application/vnd.github+json\" "
        "-H \"Authorization: Bearer " + std::string(g_cfg.gist_token) + "\" "
        "-o \"" + tmp.string() + "\" \"https://api.github.com/gists?per_page=50\"";
    if (run_cmd_quiet(cmd) != 0) return "";

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp, bytes)) return "";
    fs::remove(tmp, ec);

    const std::string rstr(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::size_t marker = rstr.find("pvz2_cloud_save.json");
    if (marker == std::string::npos) return "";
    const std::size_t id_pos = rstr.rfind("\"id\"", marker);
    if (id_pos == std::string::npos) return "";
    std::string id = json_extract_string(rstr, "id", id_pos);
    if (!id.empty()) {
        copy_str(g_cfg.gist_id, sizeof(g_cfg.gist_id), id);
        pvz2_cloud_save_config();
    }
    return id;
}

bool gist_upload(char *status_out, std::size_t cap, const CloudStrings *cs) {
    if (g_cfg.gist_token[0] == '\0') {
        std::snprintf(status_out, cap, "%s", cs->signin_github_first);
        return false;
    }
    std::size_t count = 0;
    const std::string bundle = build_bundle_json(&count);
    if (count == 0) {
        std::snprintf(status_out, cap, "%s", cs->no_local_files);
        return false;
    }

    const std::string existing_id = gist_find_existing_id();
    std::ostringstream payload;
    payload << "{\"description\":\"PvZ2Native Cloud Save\",\"public\":false,"
            << "\"files\":{\"pvz2_cloud_save.json\":{\"content\":\""
            << json_escape(bundle) << "\"}}}";

    const fs::path tmp_in = fs::temp_directory_path() / "pvz2_gist_req.json";
    const fs::path tmp_out = fs::temp_directory_path() / "pvz2_gist_resp.json";
    {
        const std::string pstr = payload.str();
        std::ofstream f(tmp_in, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(pstr.data(), (std::streamsize)pstr.size());
    }

    const bool has_id = !existing_id.empty();
    const std::string method = has_id ? "PATCH" : "POST";
    const std::string url = has_id
        ? ("https://api.github.com/gists/" + existing_id)
        : std::string("https://api.github.com/gists");

    const std::string cmd =
        "curl -sS -f -m 20 -X " + method +
        " -H \"Accept: application/vnd.github+json\" "
        "-H \"Authorization: Bearer " + std::string(g_cfg.gist_token) + "\" "
        "--data-binary @\"" + tmp_in.string() + "\" "
        "-o \"" + tmp_out.string() + "\" \"" + url + "\"";

    const int rc = run_cmd_quiet(cmd);
    std::error_code ec;
    fs::remove(tmp_in, ec);

    if (rc != 0) {
        fs::remove(tmp_out, ec);
        std::snprintf(status_out, cap, "%s", cs->gist_upload_err);
        return false;
    }

    if (!has_id) {
        std::vector<std::uint8_t> resp;
        if (read_file_bytes(tmp_out, resp)) {
            const std::string rstr(reinterpret_cast<const char *>(resp.data()), resp.size());
            std::string new_id = json_extract_string(rstr, "id");
            if (!new_id.empty()) {
                copy_str(g_cfg.gist_id, sizeof(g_cfg.gist_id), new_id);
                pvz2_cloud_save_config();
            }
        }
    }
    fs::remove(tmp_out, ec);

    std::snprintf(status_out, cap, cs->gist_uploaded_fmt, count);
    return true;
}

bool gist_download(char *status_out, std::size_t cap, const CloudStrings *cs) {
    if (g_cfg.gist_token[0] == '\0') {
        std::snprintf(status_out, cap, "%s", cs->signin_github_first);
        return false;
    }
    const std::string gid = gist_find_existing_id();
    if (gid.empty()) {
        std::snprintf(status_out, cap, "%s", cs->gist_no_save);
        return false;
    }

    const fs::path tmp_out = fs::temp_directory_path() / "pvz2_gist_dl.json";
    std::error_code ec;
    fs::remove(tmp_out, ec);

    const std::string url = "https://api.github.com/gists/" + gid;
    const std::string cmd =
        "curl -sS -f -m 20 -H \"Accept: application/vnd.github+json\" "
        "-H \"Authorization: Bearer " + std::string(g_cfg.gist_token) + "\" "
        "-o \"" + tmp_out.string() + "\" \"" + url + "\"";

    if (run_cmd_quiet(cmd) != 0 || !fs::exists(tmp_out, ec)) {
        std::snprintf(status_out, cap, "%s", cs->gist_download_err);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(tmp_out, bytes)) {
        fs::remove(tmp_out, ec);
        return false;
    }
    fs::remove(tmp_out, ec);

    const std::string rstr(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::size_t fmt_pos = rstr.find("pvz2_cloud_v1");
    if (fmt_pos == std::string::npos) {
        std::snprintf(status_out, cap, "%s", cs->gist_not_pvz2);
        return false;
    }
    std::size_t start = rstr.rfind("\"content\"", fmt_pos);
    if (start == std::string::npos) return false;
    start = rstr.find('"', start + 9);
    if (start == std::string::npos) return false;
    ++start;

    std::string inner_json;
    inner_json.reserve(rstr.size() / 2);
    for (std::size_t i = start; i < rstr.size(); ++i) {
        char c = rstr[i];
        if (c == '"') break;
        if (c == '\\' && i + 1 < rstr.size()) {
            char e = rstr[++i];
            switch (e) {
                case '"':  inner_json.push_back('"'); break;
                case '\\': inner_json.push_back('\\'); break;
                case '/':  inner_json.push_back('/'); break;
                case 'n':  inner_json.push_back('\n'); break;
                case 'r':  inner_json.push_back('\r'); break;
                case 't':  inner_json.push_back('\t'); break;
                default:   inner_json.push_back(e); break;
            }
        } else {
            inner_json.push_back(c);
        }
    }

    std::size_t restored = 0;
    if (!unpack_bundle_json(inner_json, &restored)) {
        std::snprintf(status_out, cap, "%s", cs->gist_unpack_err);
        return false;
    }

    g_writes_locked = true;
    std::snprintf(status_out, cap, cs->gist_downloaded_fmt, restored);
    return true;
}

}  // namespace

extern "C" void pvz2_cloud_init(void) {
    if (g_inited) return;
    g_inited = true;

    std::memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.provider = PVZ2_CLOUD_NAS;
    g_cfg.auto_sync = 0;
    copy_str(g_cfg.nas_server_url, sizeof(g_cfg.nas_server_url), PVZ2_DEFAULT_NAS_SERVER_URL);
    copy_str(g_cfg.server_url, sizeof(g_cfg.server_url), "http://127.0.0.1:8787");

    load_cloud_section();
    pvz2_cloud_sync_local_discord(false, nullptr, 0, nullptr);
}

extern "C" Pvz2CloudConfig *pvz2_cloud_config(void) {
    if (!g_inited) pvz2_cloud_init();
    return &g_cfg;
}

extern "C" void pvz2_cloud_save_config(void) {
    if (!g_inited) pvz2_cloud_init();
    const std::string path = config_ini_path();

    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    std::vector<std::string> out;
    bool in_cloud = false;
    bool wrote_cloud = false;

    auto emit_cloud_block = [&]() {
        out.push_back("[cloud]");
        out.push_back("provider = " + std::to_string(g_cfg.provider));
        out.push_back("auto_sync = " + std::to_string(g_cfg.auto_sync ? 1 : 0));
        out.push_back(std::string("nas_server_url = ") + g_cfg.nas_server_url);
        out.push_back(std::string("server_url = ") + g_cfg.server_url);
        out.push_back(std::string("auth_discord_id = ") + g_cfg.auth_discord_id);
        out.push_back(std::string("auth_discord_name = ") + g_cfg.auth_discord_name);
        out.push_back(std::string("auth_discord_token = ") + g_cfg.auth_discord_token);
        out.push_back(std::string("gdrive_user = ") + g_cfg.gdrive_user);
        out.push_back(std::string("gdrive_token = ") + g_cfg.gdrive_token);
        out.push_back(std::string("gdrive_refresh_token = ") + g_cfg.gdrive_refresh_token);
        out.push_back(std::string("gdrive_file_id = ") + g_cfg.gdrive_file_id);
        out.push_back(std::string("github_user = ") + g_cfg.github_user);
        out.push_back(std::string("gist_token = ") + g_cfg.gist_token);
        out.push_back(std::string("gist_id = ") + g_cfg.gist_id);
        out.push_back("");
        wrote_cloud = true;
    };

    for (const std::string &raw : lines) {
        std::string t = trim(raw);
        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            const std::string sec = to_lower(trim(t.substr(1, t.size() - 2)));
            if (sec == "cloud") {
                if (!wrote_cloud) emit_cloud_block();
                in_cloud = true;
                continue;
            } else {
                in_cloud = false;
            }
        }
        if (!in_cloud) out.push_back(raw);
    }

    if (!wrote_cloud) {
        if (!out.empty() && !out.back().empty()) out.push_back("");
        emit_cloud_block();
    }

    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) return;
    for (const std::string &l : out) {
        wf << l << "\n";
    }
}

extern "C" bool pvz2_cloud_start_provider_auth(char *status_out, size_t status_cap, const char *locale) {
    if (!g_inited) pvz2_cloud_init();
    get_cloud_strings(locale);
    pvz2_cloud_sync_local_discord(false, nullptr, 0, locale);

    g_auth_deadline = std::time(nullptr) + 240;
    if (!g_auth_running.exchange(true)) {
        g_auth_listening = false;
        std::thread(run_auth_server_thread).detach();
        for (int i = 0; i < 40 && !g_auth_listening.load(); ++i) {
            SDL_Delay(10);
        }
    }

    const int prov = g_cfg.provider;
    std::string url;
    if (prov == PVZ2_CLOUD_GDRIVE) {
        url = build_google_oauth_url();
    } else if (prov == PVZ2_CLOUD_GIST) {
        url = build_github_oauth_url();
    } else {
        url = build_discord_oauth_url();
    }
    SDL_OpenURL(url.c_str());

    if (status_out && status_cap > 0) {
        status_out[0] = '\0';
    }
    return true;
}

extern "C" bool pvz2_cloud_is_auth_in_progress(void) {
    return g_auth_running.load();
}

extern "C" bool pvz2_cloud_is_provider_authenticated(void) {
    if (!g_inited) pvz2_cloud_init();
    std::lock_guard<std::mutex> lk(g_cfg_mutex);
    switch (g_cfg.provider) {
        case PVZ2_CLOUD_NAS:
        case PVZ2_CLOUD_SERVER:
            return g_cfg.auth_discord_id[0] != '\0';
        case PVZ2_CLOUD_GDRIVE:
            return g_cfg.gdrive_token[0] != '\0';
        case PVZ2_CLOUD_GIST:
            return g_cfg.gist_token[0] != '\0';
        default:
            return false;
    }
}

extern "C" bool pvz2_cloud_sync_local_discord(bool force_connect, char *status_out,
                                              size_t status_cap, const char *locale) {
    if (!g_inited) pvz2_cloud_init();
    const CloudStrings *cs = get_cloud_strings(locale);
    char id[64] = {};
    char name[128] = {};
    bool ok = pvz2_presence_get_user(id, sizeof(id), name, sizeof(name)) != 0;
    if (!ok && force_connect) {
        ok = pvz2_presence_auth_now(id, sizeof(id), name, sizeof(name)) != 0;
    }
    if (ok && id[0] != '\0') {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        copy_str(g_cfg.local_discord_id, sizeof(g_cfg.local_discord_id), id);
        copy_str(g_cfg.local_discord_name, sizeof(g_cfg.local_discord_name), name);
        if (status_out && status_cap > 0) {
            std::snprintf(status_out, status_cap, cs->discord_connected_fmt,
                          g_cfg.local_discord_name, g_cfg.local_discord_id);
        }
        return true;
    }
    if (status_out && status_cap > 0) {
        std::lock_guard<std::mutex> lk(g_cfg_mutex);
        if (g_cfg.local_discord_id[0] != '\0') {
            std::snprintf(status_out, status_cap, cs->discord_connected_fmt,
                          g_cfg.local_discord_name, g_cfg.local_discord_id);
            return true;
        }
        std::snprintf(status_out, status_cap, "%s", cs->discord_not_detected);
    }
    return false;
}

extern "C" bool pvz2_cloud_upload(char *status_out, size_t status_cap, const char *locale) {
    if (!g_inited) pvz2_cloud_init();
    const CloudStrings *cs = get_cloud_strings(locale);
    char dummy[256];
    if (!status_out || status_cap == 0) {
        status_out = dummy;
        status_cap = sizeof(dummy);
    }
    switch (g_cfg.provider) {
        case PVZ2_CLOUD_NAS:
            return http_server_upload(g_cfg.nas_server_url, true, status_out, status_cap, cs);
        case PVZ2_CLOUD_GDRIVE:
            return gdrive_api_upload(status_out, status_cap, cs);
        case PVZ2_CLOUD_SERVER:
            return http_server_upload(g_cfg.server_url, false, status_out, status_cap, cs);
        case PVZ2_CLOUD_GIST:
            return gist_upload(status_out, status_cap, cs);
        default:
            return false;
    }
}

extern "C" bool pvz2_cloud_download(char *status_out, size_t status_cap, const char *locale) {
    if (!g_inited) pvz2_cloud_init();
    const CloudStrings *cs = get_cloud_strings(locale);
    char dummy[256];
    if (!status_out || status_cap == 0) {
        status_out = dummy;
        status_cap = sizeof(dummy);
    }
    switch (g_cfg.provider) {
        case PVZ2_CLOUD_NAS:
            return http_server_download(g_cfg.nas_server_url, true, status_out, status_cap, cs);
        case PVZ2_CLOUD_GDRIVE:
            return gdrive_api_download(status_out, status_cap, cs);
        case PVZ2_CLOUD_SERVER:
            return http_server_download(g_cfg.server_url, false, status_out, status_cap, cs);
        case PVZ2_CLOUD_GIST:
            return gist_download(status_out, status_cap, cs);
        default:
            return false;
    }
}

extern "C" bool pvz2_cloud_writes_locked(void) {
    return g_writes_locked;
}

extern "C" void pvz2_cloud_restart_game(void) {
#if defined(_WIN32)
    char exe[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0) {
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        if (CreateProcessA(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    TerminateProcess(GetCurrentProcess(), 0);
#else
    char exe[4096] = {};
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        pid_t pid = ::fork();
        if (pid == 0) {
            ::execl(exe, exe, (char *)nullptr);
            ::_exit(0);
        }
    }
    ::_exit(0);
#endif
}

extern "C" void pvz2_cloud_on_quit(void) {
    if (!g_inited) return;
    if (g_cfg.auto_sync && !g_writes_locked && pvz2_cloud_is_provider_authenticated()) {
        char msg[256] = {};
        pvz2_cloud_upload(msg, sizeof(msg), nullptr);
    }
}

extern "C" bool pvz2_cloud_export_local(char *status_out, size_t status_cap, const char *locale) {
    if (!g_inited) pvz2_cloud_init();
    const CloudStrings *cs = get_cloud_strings(locale);
    char dummy[256];
    if (!status_out || status_cap == 0) {
        status_out = dummy;
        status_cap = sizeof(dummy);
    }

    std::size_t count = 0;
    const std::string bundle = build_bundle_json(&count);
    if (count == 0) {
        std::snprintf(status_out, status_cap, "%s", cs->no_local_files);
        return false;
    }

    std::string out_path;
#if defined(_WIN32)
    char file_path[MAX_PATH] = "";
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    std::snprintf(file_path, sizeof(file_path), "pvz2_save_%04d%02d%02d_%02d%02d%02d.json",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    std::error_code cwd_ec;
    const fs::path cwd = fs::current_path(cwd_ec);

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "PvZ 2 Save JSON (*.json) *.json All Files (*.*) *.* ";
    ofn.lpstrFile = file_path;
    ofn.nMaxFile = sizeof(file_path);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "json";
    ofn.lpstrTitle = (std::strncmp(cs->html_lang, "es", 2) == 0) ? "Exportar partida de PvZ 2 (.json)" : "Export PvZ 2 Save (.json)";

    BOOL ok = GetSaveFileNameA(&ofn);
    if (!cwd.empty()) {
        fs::current_path(cwd, cwd_ec);
    }

    if (!ok) {
        return false;
    }
    out_path = file_path;
#else
    out_path = (fs::path(pvz2_config()->save_dir) / "pvz2_save_backup.json").string();
#endif

    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs) {
        std::snprintf(status_out, status_cap, "%s", cs->export_err);
        return false;
    }
    ofs.write(bundle.data(), bundle.size());
    ofs.close();

    std::snprintf(status_out, status_cap, cs->export_success_fmt, count);
    return true;
}

extern "C" bool pvz2_cloud_import_local(char *status_out, size_t status_cap, const char *locale) {
    if (!g_inited) pvz2_cloud_init();
    const CloudStrings *cs = get_cloud_strings(locale);
    char dummy[256];
    if (!status_out || status_cap == 0) {
        status_out = dummy;
        status_cap = sizeof(dummy);
    }

    std::string in_path;
#if defined(_WIN32)
    char file_path[MAX_PATH] = "";
    std::error_code cwd_ec;
    const fs::path cwd = fs::current_path(cwd_ec);

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "PvZ 2 Save JSON (*.json) *.json All Files (*.*) *.* ";
    ofn.lpstrFile = file_path;
    ofn.nMaxFile = sizeof(file_path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "json";
    ofn.lpstrTitle = (std::strncmp(cs->html_lang, "es", 2) == 0) ? "Importar partida de PvZ 2 (.json)" : "Import PvZ 2 Save (.json)";

    BOOL ok = GetOpenFileNameA(&ofn);
    if (!cwd.empty()) {
        fs::current_path(cwd, cwd_ec);
    }

    if (!ok) {
        return false;
    }
    in_path = file_path;
#else
    in_path = (fs::path(pvz2_config()->save_dir) / "pvz2_save_backup.json").string();
#endif

    std::ifstream ifs(in_path, std::ios::binary);
    if (!ifs) {
        std::snprintf(status_out, status_cap, "%s", cs->import_err_read);
        return false;
    }
    std::string json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    std::size_t restored = 0;
    if (!unpack_bundle_json(json, &restored) || restored == 0) {
        std::snprintf(status_out, status_cap, "%s", cs->import_err_invalid);
        return false;
    }

    g_writes_locked = true;
    std::snprintf(status_out, status_cap, cs->import_success_fmt, restored);
    return true;
}

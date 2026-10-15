#include <cerrno>
#include <atomic>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "api.hpp"
#include "auth.hpp"
#include "catalog.hpp"
#include "channel_sync.hpp"
#include "db_manager.hpp"
#include "download_manager.hpp"
#include "httplib.h"
#include "metadata.hpp"
#include "release_prober.hpp"
#include "settings.hpp"
#include "signal_watcher.hpp"
#include "telegram_client.hpp"
#include "tmdb_client.hpp"
#include "tracker.hpp"

namespace {

constexpr const char* kDbPath = "db/telegarrm.db";
constexpr const char* kTdlibDir = "db/tdlib";
constexpr const char* kTmdbImageDir = "db/tmdb/images";
constexpr const char* kSetupCodePath = "db/codigo-acceso.txt";
constexpr int kDefaultPort = 8080;
// Código de salida para pedir a systemd que reinicie el servicio (RestartForceExitStatus=75)
constexpr int kRestartExitCode = 75;

// Credenciales de la API de Telegram (https://my.telegram.org): de las variables de entorno o, en
// Docker, de los archivos que digan TELEGARRM_API_ID_FILE y TELEGARRM_API_HASH_FILE (D-054)
std::optional<TelegramClient::Config> telegramConfigFromEnv() {
    const Secret apiIdSecret = readSecret("TELEGARRM_API_ID");
    const Secret apiHashSecret = readSecret("TELEGARRM_API_HASH");
    for (const Secret* secret : {&apiIdSecret, &apiHashSecret}) {
        if (!secret->error.empty()) {
            std::cerr << "Error: " << secret->error << "." << std::endl;
            return std::nullopt;
        }
    }
    if (apiIdSecret.value.empty() || apiHashSecret.value.empty()) {
        std::cerr << "Error: faltan TELEGARRM_API_ID y TELEGARRM_API_HASH (o, en Docker, TELEGARRM_API_ID_FILE y "
                     "TELEGARRM_API_HASH_FILE); se obtienen en https://my.telegram.org." << std::endl;
        return std::nullopt;
    }
    const char* apiId = apiIdSecret.value.c_str();

    char* end = nullptr;
    errno = 0;
    const long id = std::strtol(apiId, &end, 10);
    if (errno != 0 || *end != '\0' || id <= 0 || id > INT_MAX) {
        std::cerr << "Error: TELEGARRM_API_ID debe ser un número entero positivo." << std::endl;
        return std::nullopt;
    }

    TelegramClient::Config config;
    config.apiId = static_cast<int>(id);
    config.apiHash = apiHashSecret.value;
    config.databaseDir = kTdlibDir;
    return config;
}

// TELEGARRM_PORT (opcional, para pruebas con otra instancia): puerto de la web; si no, 8080
int portFromEnv() {
    const char* value = std::getenv("TELEGARRM_PORT");
    if (!value || !*value) {
        return kDefaultPort;
    }
    char* end = nullptr;
    const long port = std::strtol(value, &end, 10);
    return (*end == '\0' && port > 0 && port < 65536) ? static_cast<int>(port) : kDefaultPort;
}

// telegarrm --reset-password: olvida la contraseña de la web y cierra todas las sesiones (D-053). Al
// volver a entrar, la web pide crear otra con el código que se deja en db/codigo-acceso.txt.
int resetPassword() {
    DbManager db(kDbPath);
    if (!db.open() || !Auth::reset(db)) {
        std::cerr << "Error: no se pudo borrar la contraseña en '" << kDbPath << "'." << std::endl;
        return 1;
    }
    std::cout << "Contraseña borrada y sesiones cerradas. Abre la web: pedirá crear otra con el código de "
              << kSetupCodePath << "." << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--reset-password") {
        return resetPassword();
    }
    if (argc > 1) {
        std::cerr << "Uso: " << argv[0] << " [--reset-password]" << std::endl;
        return 2;
    }

    // Debe ir antes de crear cualquier hilo (ver signal_watcher.hpp)
    SignalWatcher signals;

    std::cout << "Iniciando Telegarrm " TELEGARRM_VERSION "..." << std::endl;
    const int port = portFromEnv();

    auto telegramConfig = telegramConfigFromEnv();
    if (!telegramConfig) {
        return 1;
    }

    // Base de datos: crea db/telegarrm.db y sus tablas si no existen
    DbManager db(kDbPath);
    if (!db.open()) {
        std::cerr << "Error: no se pudo inicializar la base de datos en '" << kDbPath << "'." << std::endl;
        return 1;
    }
    // Registrar la versión en ejecución; /api/status la lee después desde SQLite
    db.setSetting("version", TELEGARRM_VERSION);

    // Acceso a la web (D-053). Sin contraseña, deja el código para crearla en db/codigo-acceso.txt.
    Auth auth(db, kSetupCodePath);
    if (!auth.configured()) {
        auth.prepareSetup();
    }

    // Búfer de descargas de los ajustes. Si no se puede usar (ej. el disco no está montado o el
    // servicio no tiene permiso), se arranca con el predeterminado y la web lo avisa.
    const AppSettings settings = loadSettings(db);
    std::string downloadDir = kTdlibDir;
    std::string downloadDirWarning;
    if (!settings.downloadDir.empty()) {
        const PathCheck check = checkPath(settings.downloadDir);
        if (check.ok) {
            downloadDir = settings.downloadDir;
            telegramConfig->filesDir = settings.downloadDir;
        } else {
            downloadDirWarning = "No se pudo usar " + settings.downloadDir + " (" + check.error + "); se usa " + kTdlibDir;
            std::cerr << "Advertencia: " << downloadDirWarning << std::endl;
        }
    }
    std::cout << "Búfer de descargas: " << downloadDir << std::endl;

    // Catálogo en memoria, derivado de los mensajes guardados. Lo calcula el hilo de
    // sincronización al arrancar, para no retrasar la web (con miles de mensajes tarda segundos).
    Catalog catalog(db);

    // Metadatos de TMDB (opcionales: sin token, el catálogo usa solo los datos de las fichas)
    const Secret tmdbToken = readSecret("TELEGARRM_TMDB_TOKEN");
    if (!tmdbToken.error.empty()) {
        std::cerr << "Error: " << tmdbToken.error << "." << std::endl;
        return 1;
    }
    TmdbClient tmdb(db, tmdbToken.value, kTmdbImageDir);
    MetadataService metadata(db, catalog, tmdb);

    TelegramClient telegram(std::move(*telegramConfig));
    // Cola de descargas; TDLib guarda los archivos en el búfer y después se importan a la biblioteca.
    // Para los nombres: título, año e identificador de TMDB si los hay; si no, los del catálogo.
    DownloadManager downloads(db, telegram, downloadDir, [&catalog, &metadata](const DbManager::Download& download) {
        const auto ref = catalog.findRelease(download.chatId, download.messageId);
        return ref ? tracking::workInfo(*ref->item, metadata) : library::WorkInfo{download.title, std::nullopt, 0};
    });
    // Calidad real de los archivos sin descargarlos enteros (D-042)
    ReleaseProber prober(db, telegram);
    // Seguimiento: pone en cola los episodios nuevos y las versiones mejores de lo que se sigue
    Tracker tracker(db, catalog, metadata, downloads, prober);
    // Cada cambio en los mensajes de un canal (también los que llegan en tiempo real) recalcula su parte
    // del catálogo, busca lo nuevo en TMDB y revisa las obras seguidas
    ChannelSync sync(db, telegram, [&catalog, &metadata, &tracker](std::int64_t chatId) {
        catalog.rebuildChannel(chatId);
        metadata.requestRun();
        tracker.requestRun();
    });

    // Inicializar el servidor HTTP
    httplib::Server svr;
    std::atomic<bool> restartRequested{false};
    registerApiRoutes(svr, {db, telegram, sync, catalog, metadata, tmdb, downloads, tracker, prober, auth, downloadDir,
                            downloadDirWarning,
                            [&svr, &restartRequested] {
                                restartRequested = true;
                                svr.stop();  // Termina listen_after_bind() tras responder a la petición
                            }});

    // Configurar la carpeta web estática
    if (!svr.set_mount_point("/", "./web")) {
        std::cerr << "Advertencia: El directorio './web' no existe o no se puede montar." << std::endl;
    }

    // cpp-httplib activa SO_REUSEPORT por defecto, lo que deja a dos instancias escuchar
    // a la vez en el mismo puerto. Con SO_REUSEADDR se puede reiniciar enseguida,
    // pero una segunda instancia falla al hacer bind.
    svr.set_socket_options([](socket_t sock) {
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
    });

    // El puerto se reserva antes de arrancar TDLib: una segunda instancia termina aquí
    // sin llegar a tocar la sesión de Telegram de la primera
    if (!svr.bind_to_port("0.0.0.0", port)) {
        std::cerr << "Error: no se pudo escuchar en el puerto " << port << "." << std::endl;
        return 1;
    }

    // Hilo de TDLib. Debe lanzarse antes de listen_after_bind(), que es bloqueante.
    if (!telegram.start()) {
        std::cerr << "Error: no se pudo iniciar el cliente de Telegram." << std::endl;
        return 1;
    }
    // Sincronización del historial de los canales vigilados (espera a que haya sesión),
    // metadatos, descargas y seguimiento, cada uno en su hilo
    sync.start();
    metadata.start();
    downloads.start();
    tracker.start();

    std::cout << "Servidor web escuchando en http://localhost:" << port << std::endl;

    // SIGINT/SIGTERM detienen el servidor, lo que hace volver a listen_after_bind()
    signals.start([&svr] { svr.stop(); });

    // listen_after_bind() bloquea el hilo principal, actuando como bucle del daemon
    svr.listen_after_bind();

    // El vigilante usa svr: hay que pararlo antes de que se destruya. El seguimiento usa la cola de
    // descargas, y esta y la sincronización usan TelegramClient: se paran antes. La BD se cierra en
    // su destructor.
    signals.stop();
    tracker.stop();
    downloads.stop();
    metadata.stop();
    sync.stop();
    telegram.stop();
    if (restartRequested) {
        std::cout << "Telegarrm detenido para reiniciarse." << std::endl;
        return kRestartExitCode;
    }
    std::cout << "Telegarrm detenido." << std::endl;
    return 0;
}

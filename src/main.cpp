#include <cerrno>
#include <atomic>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>

#include "api.hpp"
#include "catalog.hpp"
#include "channel_sync.hpp"
#include "db_manager.hpp"
#include "download_manager.hpp"
#include "httplib.h"
#include "metadata.hpp"
#include "settings.hpp"
#include "signal_watcher.hpp"
#include "telegram_client.hpp"
#include "tmdb_client.hpp"

namespace {

constexpr const char* kDbPath = "db/telegarrm.db";
constexpr const char* kTdlibDir = "db/tdlib";
constexpr const char* kTmdbImageDir = "db/tmdb/images";
constexpr int kPort = 8080;
// Código de salida para pedir a systemd que reinicie el servicio (RestartForceExitStatus=75)
constexpr int kRestartExitCode = 75;

// Credenciales de la API de Telegram (https://my.telegram.org), leídas del entorno
std::optional<TelegramClient::Config> telegramConfigFromEnv() {
    const char* apiId = std::getenv("TELEGARRM_API_ID");
    const char* apiHash = std::getenv("TELEGARRM_API_HASH");
    if (!apiId || !*apiId || !apiHash || !*apiHash) {
        std::cerr << "Error: faltan las variables de entorno TELEGARRM_API_ID y TELEGARRM_API_HASH "
                     "(se obtienen en https://my.telegram.org)." << std::endl;
        return std::nullopt;
    }

    char* end = nullptr;
    errno = 0;
    const long id = std::strtol(apiId, &end, 10);
    if (errno != 0 || *end != '\0' || id <= 0 || id > INT_MAX) {
        std::cerr << "Error: TELEGARRM_API_ID debe ser un número entero positivo." << std::endl;
        return std::nullopt;
    }

    TelegramClient::Config config;
    config.apiId = static_cast<int>(id);
    config.apiHash = apiHash;
    config.databaseDir = kTdlibDir;
    return config;
}

}  // namespace

int main() {
    // Debe ir antes de crear cualquier hilo (ver signal_watcher.hpp)
    SignalWatcher signals;

    std::cout << "Iniciando Telegarrm " TELEGARRM_VERSION "..." << std::endl;

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
    const char* tmdbToken = std::getenv("TELEGARRM_TMDB_TOKEN");
    TmdbClient tmdb(db, tmdbToken ? tmdbToken : "", kTmdbImageDir);
    MetadataService metadata(db, catalog, tmdb);

    TelegramClient telegram(std::move(*telegramConfig));
    // Cada cambio en los mensajes de un canal recalcula su parte del catálogo y busca lo nuevo en TMDB
    ChannelSync sync(db, telegram, [&catalog, &metadata](std::int64_t chatId) {
        catalog.rebuildChannel(chatId);
        metadata.requestRun();
    });
    // Cola de descargas; TDLib guarda los archivos en el búfer
    DownloadManager downloads(db, telegram, downloadDir);

    // Inicializar el servidor HTTP
    httplib::Server svr;
    std::atomic<bool> restartRequested{false};
    registerApiRoutes(svr, {db, telegram, sync, catalog, metadata, tmdb, downloads, downloadDir, downloadDirWarning,
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
    if (!svr.bind_to_port("0.0.0.0", kPort)) {
        std::cerr << "Error: no se pudo escuchar en el puerto " << kPort << "." << std::endl;
        return 1;
    }

    // Hilo de TDLib. Debe lanzarse antes de listen_after_bind(), que es bloqueante.
    if (!telegram.start()) {
        std::cerr << "Error: no se pudo iniciar el cliente de Telegram." << std::endl;
        return 1;
    }
    // Sincronización del historial de los canales vigilados (espera a que haya sesión),
    // metadatos y descargas, cada uno en su hilo
    sync.start();
    metadata.start();
    downloads.start();

    std::cout << "Servidor web escuchando en http://localhost:" << kPort << std::endl;

    // SIGINT/SIGTERM detienen el servidor, lo que hace volver a listen_after_bind()
    signals.start([&svr] { svr.stop(); });

    // listen_after_bind() bloquea el hilo principal, actuando como bucle del daemon
    svr.listen_after_bind();

    // El vigilante usa svr: hay que pararlo antes de que se destruya. La sincronización usa
    // TelegramClient, así que se para antes que él. La BD se cierra en su destructor.
    signals.stop();
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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "catalog.hpp"
#include "db_manager.hpp"
#include "library.hpp"

class TelegramClient;

// Descarga de un archivo lógico del catálogo: los datos salen del catálogo, no de lo que envíe el navegador
DbManager::Download makeDownload(const Catalog::Item& item, const Catalog::Release& release);
// "«Ted Lasso» 4x10 (1080p)", para el historial de actividad
std::string describeDownload(const DbManager::Download& download);

// Procesa la cola de descargas (tabla downloads) en su propio hilo, una descarga cada vez
// (docs/DECISIONS.md, D-031 y D-034). Cada descarga es un archivo lógico del catálogo: se piden a
// TDLib todas sus partes al búfer y, al terminar, se importa a la biblioteca (descomprimir,
// renombrar para Jellyfin y mover). Si sustituye a una versión peor, después se borra esa (D-037).
class DownloadManager {
public:
    struct Progress {
        std::int64_t downloaded = 0;  // Bytes
        std::int64_t total = 0;
        double bytesPerSecond = 0;
        int importPercent = -1;       // Progreso de la descompresión (-1 = aún no)
    };

    // Datos de la obra para los nombres en la biblioteca (título, año y TMDB)
    using WorkInfoResolver = std::function<library::WorkInfo(const DbManager::Download&)>;

    // filesDir: carpeta donde TDLib guarda los archivos (para comprobar el espacio libre)
    DownloadManager(DbManager& db, TelegramClient& telegram, std::string filesDir, WorkInfoResolver workInfo);
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    void start();
    // Detener antes que TelegramClient. Lo que se estaba descargando vuelve a la cola al arrancar.
    void stop();
    // Hay algo nuevo en la cola
    void wake();
    // Pide cancelar la descarga en curso; false si no es la que se está descargando
    bool cancelActive(std::int64_t id);
    // Progreso en vivo de la descarga en curso (std::nullopt si no es la activa)
    std::optional<Progress> liveProgress(std::int64_t id) const;

private:
    enum class Outcome { Completed, Failed, Cancelled, Stopped };

    void run();
    // Descarga las partes; al terminar, download.parts tiene sus rutas locales
    Outcome process(DbManager::Download& download, std::string& error);
    // Importa a la biblioteca una descarga terminada
    Outcome importToLibrary(DbManager::Download& download, std::string& error);
    // Tras importar una versión mejor: borra (o conserva) las que sustituye. Devuelve el texto para
    // el historial; vacío si no había nada que sustituir.
    std::string replaceOlder(const DbManager::Download& download);
    // Corrige con ffprobe la calidad de las descargas importadas antes de D-039 (una sola vez)
    void probeCompletedDownloads();
    // Historial de las descargas automáticas y de las sustituciones
    void logOutcome(const DbManager::Download& download, Outcome outcome, const std::string& error);
    bool telegramReady() const;
    bool sleepFor(std::chrono::milliseconds duration);
    bool stopping();

    DbManager& db_;
    TelegramClient& telegram_;
    const std::string filesDir_;
    const WorkInfoResolver workInfo_;
    std::thread worker_;

    std::mutex controlMutex_;  // Protege stopRequested_ y wakeRequested_
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool wakeRequested_ = false;

    std::atomic<std::int64_t> activeId_{0};
    std::atomic<std::int64_t> cancelId_{0};
    mutable std::mutex progressMutex_;  // Protege progress_
    Progress progress_;
};

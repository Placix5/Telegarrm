#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "db_manager.hpp"

class TelegramClient;

// Procesa la cola de descargas (tabla downloads) en su propio hilo, una descarga cada vez
// (docs/DECISIONS.md, D-031). Cada descarga es un archivo lógico del catálogo: se piden a TDLib
// todas sus partes y se sigue su progreso. Los archivos quedan en la caché de TDLib (db/tdlib);
// moverlos y descomprimirlos en la biblioteca es el siguiente paso de la Fase 3.
class DownloadManager {
public:
    struct Progress {
        std::int64_t downloaded = 0;  // Bytes
        std::int64_t total = 0;
        double bytesPerSecond = 0;
    };

    // filesDir: carpeta donde TDLib guarda los archivos (para comprobar el espacio libre)
    DownloadManager(DbManager& db, TelegramClient& telegram, std::string filesDir);
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
    Outcome process(DbManager::Download& download, std::string& error);
    bool telegramReady() const;
    bool sleepFor(std::chrono::milliseconds duration);
    bool stopping();

    DbManager& db_;
    TelegramClient& telegram_;
    const std::string filesDir_;
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

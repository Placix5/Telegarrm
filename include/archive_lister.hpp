#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catalog.hpp"
#include "db_manager.hpp"

class TelegramClient;

namespace archive {

// ¿Hay que leer el índice de este archivo? (D-057, D-060) Un ZIP, RAR o 7z de una serie que no dice su
// episodio en el nombre (un pack o un comprimido sin marcador), con todas sus partes publicadas
bool shouldList(const Catalog::Item& item, const Catalog::Release& release, std::int64_t now);

// Trozo de una parte que hay que pedir para tener los últimos `bytes` del archivo entero
struct PartRange {
    std::size_t part = 0;  // Índice en release.parts
    std::int64_t offset = 0;
    std::int64_t length = 0;
};
// Los trozos de cada parte que forman `length` bytes desde `offset` del archivo entero (sus partes, en
// orden, una detrás de otra)
std::vector<PartRange> spanRanges(const std::vector<Catalog::Part>& parts, std::int64_t offset, std::int64_t length);

}  // namespace archive

// Lee el índice de los comprimidos de las series (D-057, D-060): qué vídeos lleva cada ZIP, RAR o 7z,
// pidiendo a Telegram solo unos kB (archive_index.hpp dice cuáles). En su propio hilo, de uno en uno, con la prioridad más baja; cuando aprende
// algo, avisa para recalcular el catálogo de ese canal.
class ArchiveLister {
public:
    using ChangedCallback = std::function<void(std::int64_t chatId)>;

    ArchiveLister(DbManager& db, TelegramClient& telegram, Catalog& catalog, ChangedCallback changed);
    ~ArchiveLister();

    ArchiveLister(const ArchiveLister&) = delete;
    ArchiveLister& operator=(const ArchiveLister&) = delete;

    void start();
    void stop();
    // Hay comprimidos nuevos que mirar (el catálogo ha cambiado)
    void requestRun();

private:
    void run();
    // Una pasada por todo el catálogo. Devuelve si quedó algo pendiente (fallos de red)
    bool pass();
    // Lee y guarda el índice de un archivo. false si no se pudo por la red o porque se está descargando
    bool list(const Catalog::Release& release);
    std::string readRange(std::int64_t chatId, std::int64_t messageId, std::int64_t offset, std::int64_t length,
                          std::string& error);
    bool stopping();

    DbManager& db_;
    TelegramClient& telegram_;
    Catalog& catalog_;
    ChangedCallback changed_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool runRequested_ = false;
    // Fallos de red: no se reintenta hasta pasado un rato. (chat, primera parte) -> cuándo
    std::map<std::pair<std::int64_t, std::int64_t>, std::chrono::steady_clock::time_point> retryAfter_;
};

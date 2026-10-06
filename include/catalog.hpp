#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "db_manager.hpp"

// Catálogo de series y películas, derivado de los mensajes guardados de cada canal.
// Vive en memoria: se recalcula al arrancar y cuando cambian los mensajes de un canal,
// así que una mejora del parser se aplica sin migraciones (ver docs/DECISIONS.md, D-020).
class Catalog {
public:
    struct File {
        std::int64_t messageId = 0;
        std::string fileName;
        std::int64_t size = 0;      // Bytes
        int season = 0;             // 0 = sin marcador de episodio
        int episode = 0;
        int episodeEnd = 0;         // Último episodio si el fichero trae varios
        std::string episodeTitle;
        std::string quality;
        bool archive = false;       // .rar, .zip, .7z, .001...
    };

    struct Item {
        std::int64_t chatId = 0;
        std::int64_t anchorMessageId = 0;   // Ficha que abre el bloque; 0 = archivos sin ficha
        std::string kind;                   // "series" o "movie"
        std::string title;
        std::optional<int> year;
        std::string quality;
        std::vector<std::string> languages;
        std::vector<std::string> genres;
        std::string description;            // Texto completo de la ficha
        std::int64_t posterMessageId = 0;   // Foto de la ficha (0 = sin portada)
        std::string channelTitle;
        std::vector<File> files;            // Series: por temporada y episodio
        std::int64_t totalSize = 0;
        int seasonCount = 0;
        int episodeCount = 0;               // Episodios distintos (las versiones repetidas cuentan una vez)
    };

    using ItemPtr = std::shared_ptr<const Item>;

    explicit Catalog(DbManager& db);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    void rebuildAll();
    void rebuildChannel(std::int64_t chatId);
    void removeChannel(std::int64_t chatId);

    std::vector<ItemPtr> items() const;
    ItemPtr find(std::int64_t chatId, std::int64_t anchorMessageId) const;

    // Agrupa los mensajes de un canal (en orden cronológico) en series y películas.
    // Cada foto con pie (la "ficha") abre un elemento y los vídeos siguientes le pertenecen.
    static std::vector<Item> buildItems(std::int64_t chatId, const std::string& channelTitle,
                                        const std::vector<DbManager::Message>& messages);

private:
    DbManager& db_;
    // Serializa reconstrucciones y borrados: sin él, quitar un canal mientras se recalcula
    // podría dejar sus elementos de vuelta en el catálogo
    std::mutex rebuildMutex_;
    mutable std::mutex mutex_;  // Protege byChannel_
    std::map<std::int64_t, std::vector<ItemPtr>> byChannel_;
};

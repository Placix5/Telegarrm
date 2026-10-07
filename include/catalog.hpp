#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_manager.hpp"

// Catálogo de series y películas, derivado de los mensajes guardados de los canales.
// Vive en memoria: se recalcula al arrancar y cuando cambian los mensajes de un canal,
// así que una mejora del parser se aplica sin migraciones (docs/DECISIONS.md, D-020 y D-025).
//
// Cómo se construye:
//  1. En cada tema de cada canal, cada foto con pie (la "ficha") abre un bloque y los
//     archivos siguientes de ese tema le pertenecen.
//  2. Las partes de un archivo troceado (.zip.001, .part2.rar...) forman un solo Release.
//  3. Los bloques con el mismo título (o título alternativo) se unen en un Item: las
//     temporadas, los episodios en emisión y las versiones 1080p/4K de una misma obra.
class Catalog {
public:
    struct Part {
        std::int64_t messageId = 0;
        std::string fileName;
        std::int64_t size = 0;  // Bytes
        int number = 0;         // 0 = archivo sin trocear
    };

    // Un archivo lógico: un vídeo, o un comprimido troceado en varias partes
    struct Release {
        std::int64_t chatId = 0;
        std::int64_t anchorMessageId = 0;  // Bloque (ficha) del que viene
        std::int64_t topicId = 0;
        std::string name;                  // Nombre común a las partes
        std::vector<Part> parts;
        std::int64_t size = 0;
        bool archive = false;              // Hay que descomprimirlo
        std::string quality;               // "2160p", "1080p"...; vacío si no consta
        bool hdr = false;
        std::vector<std::string> tags;     // Distinguen versiones de una misma calidad: "REMUX", "Open Matte"...
        long tmdbId = 0;                   // Si el nombre del fichero lo incluye (0 = no)
        int season = 0;                    // 0 = sin episodio (películas)
        int episode = 0;
        int episodeEnd = 0;                // Último episodio si trae varios
        std::string episodeTitle;
        std::int64_t date = 0;             // Unix, segundos (publicación de la última parte)
    };

    struct Item {
        std::int64_t chatId = 0;           // Identificador: la ficha más antigua de la obra
        std::int64_t anchorMessageId = 0;
        std::string kind;                  // "series" o "movie"
        std::string title;
        std::vector<std::string> alternateTitles;
        std::optional<int> year;
        long tmdbId = 0;                     // El más repetido en los nombres de fichero (0 = ninguno)
        std::vector<std::string> qualities;  // Versiones disponibles, de mejor a peor
        std::vector<std::string> languages;
        std::vector<std::string> genres;
        std::vector<std::string> topics;     // Temas del foro donde aparece
        bool airing = false;                 // Aparece en un tema "en emisión"
        std::string synopsis;
        std::string description;             // Ficha original (la más antigua)
        std::int64_t posterChatId = 0;
        std::int64_t posterMessageId = 0;    // 0 = sin portada
        std::string channelTitle;
        std::vector<Release> releases;       // Series: por temporada, episodio y calidad
        std::int64_t totalSize = 0;
        int seasonCount = 0;
        int episodeCount = 0;                // Episodios distintos (las versiones cuentan una vez)
        std::int64_t updatedAt = 0;          // Publicación más reciente
        // (chat, ficha) de todos los bloques que forman la obra: find() acepta cualquiera
        std::vector<std::pair<std::int64_t, std::int64_t>> blockIds;
    };

    using ItemPtr = std::shared_ptr<const Item>;

    // Novedad (D-043): archivos lógicos nuevos de una obra recién publicados en Telegram. Sirven para
    // avisar en la web a quien está viendo esa obra.
    struct Event {
        std::int64_t id = 0;
        std::int64_t at = 0;               // Unix, segundos (cuando el catálogo lo supo)
        std::int64_t chatId = 0;           // Obra (su identificador en el catálogo)
        std::int64_t anchorMessageId = 0;
        std::string title;
        std::string kind;
        std::vector<Release> releases;     // Los archivos nuevos
    };

    // Mensajes de un canal para construir el catálogo de forma aislada (tests)
    struct ChannelInput {
        std::int64_t chatId = 0;
        std::string title;
        std::vector<DbManager::Message> messages;  // En orden cronológico
        std::vector<DbManager::Topic> topics;
    };

    explicit Catalog(DbManager& db);
    ~Catalog();

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    void rebuildAll();
    void rebuildChannel(std::int64_t chatId);
    void removeChannel(std::int64_t chatId);

    std::vector<ItemPtr> items() const;
    ItemPtr find(std::int64_t chatId, std::int64_t anchorMessageId) const;

    // Archivo lógico que contiene ese mensaje (cualquiera de sus partes) y su obra
    struct ReleaseRef {
        ItemPtr item;
        const Release* release = nullptr;  // Apunta dentro de *item, que lo mantiene vivo
    };
    std::optional<ReleaseRef> findRelease(std::int64_t chatId, std::int64_t messageId) const;

    // Novedades posteriores a afterId (0 = todas las que se conservan), de la más antigua a la más nueva
    std::vector<Event> eventsAfter(std::int64_t afterId) const;
    // Identificador de la última novedad (0 = ninguna desde que arrancó el servicio)
    std::int64_t lastEventId() const;

    // Construye el catálogo de unos canales sin caché ni BD (para los tests)
    static std::vector<Item> buildItems(const std::vector<ChannelInput>& channels);

    struct ParseCache;  // Análisis de cada mensaje, reutilizado entre reconstrucciones
    struct Block;       // Ficha + archivos que le pertenecen

private:
    void publish();  // Une los bloques de todos los canales y sustituye la instantánea

    DbManager& db_;
    // Serializa reconstrucciones y borrados (y protege blocks_ y caches_): sin él, quitar un
    // canal mientras se recalcula podría dejar sus elementos de vuelta en el catálogo
    std::mutex rebuildMutex_;
    std::map<std::int64_t, std::vector<Block>> blocks_;
    std::unordered_map<std::int64_t, std::unique_ptr<ParseCache>> caches_;

    mutable std::mutex mutex_;  // Protege items_, releaseIndex_, events_ y nextEventId_
    std::vector<ItemPtr> items_;
    // (chat, mensaje de cualquier parte) -> (obra, índice del Release)
    std::map<std::pair<std::int64_t, std::int64_t>, std::pair<ItemPtr, std::size_t>> releaseIndex_;
    std::deque<Event> events_;  // Las últimas novedades, solo en memoria
    std::int64_t nextEventId_ = 1;
};

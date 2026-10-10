#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "catalog.hpp"
#include "db_manager.hpp"

class TmdbClient;

// Enriquece el catálogo con TMDB (docs/DECISIONS.md, D-029 y D-030). En su propio hilo, busca
// cada obra en TMDB, guarda la coincidencia (con sus identificadores externos y episodios) en
// SQLite y la ofrece a la API. Sin token de TMDB no hace nada y la API usa solo las fichas.
class MetadataService {
public:
    using Info = DbManager::Metadata;
    using InfoPtr = std::shared_ptr<const Info>;

    struct Stats {
        bool enabled = false;      // Hay token de TMDB
        bool working = false;      // Consultando ahora
        std::size_t total = 0;     // Obras en el catálogo
        std::size_t matched = 0;   // Con coincidencia en TMDB
        std::size_t unmatched = 0; // Buscadas sin éxito
    };

    MetadataService(DbManager& db, Catalog& catalog, TmdbClient& tmdb);
    ~MetadataService();

    MetadataService(const MetadataService&) = delete;
    MetadataService& operator=(const MetadataService&) = delete;

    void start();
    void stop();
    // Hay obras nuevas en el catálogo: consultarlas sin esperar a la siguiente ronda
    void requestRun();

    // Datos de TMDB de una obra; nullptr si no se ha encontrado (o aún no se ha buscado)
    InfoPtr lookup(const Catalog::Item& item) const;
    Stats stats() const;

    // Clave estable de una obra: tipo|título normalizado|año. Si el parser cambia el título o el
    // año, la obra se vuelve a buscar (con la caché de TMDB, casi sin coste).
    static std::string workKey(const Catalog::Item& item);

    // Busca una obra en TMDB ahora (lo usa el hilo; también tools/catalog_dump.cpp para comparar un
    // cambio del criterio). std::nullopt si TMDB no ha respondido (se reintentará); matchedBy "none"
    // si no hay coincidencia.
    std::optional<Info> resolve(const Catalog::Item& item);

private:
    void run();
    bool due(const Catalog::Item& item, const Info* known, std::int64_t now) const;
    bool sleepFor(std::chrono::seconds duration);
    bool stopping();

    DbManager& db_;
    Catalog& catalog_;
    TmdbClient& tmdb_;
    std::thread worker_;

    std::mutex controlMutex_;  // Protege stopRequested_ y runRequested_
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool runRequested_ = false;

    mutable std::mutex mutex_;  // Protege byKey_ y working_
    std::unordered_map<std::string, InfoPtr> byKey_;
    bool working_ = false;
    // Lo buscado antes de esta fecha (Unix) se buscó con un criterio anterior. Solo lo usa el hilo.
    std::int64_t matcherChangedAt_ = 0;
};

// Obras del catálogo que son la misma según TMDB (D-048): la misma serie o película publicada en
// varios canales o con otro nombre ("Boku no Hero Academia" en CrunchyShur y "My Hero Academia" en
// Las Cositas). El catálogo de la web muestra cada grupo como una sola obra; cada una conserva sus
// archivos, su numeración y su seguimiento.
namespace works {

using Lookup = std::function<MetadataService::InfoPtr(const Catalog::Item&)>;

// Grupos en el orden de su primera obra en items; las obras sin datos de TMDB van solas. Dentro de
// cada grupo, por preferencia: la del canal añadido antes (channelRank menor; los que no están, al
// final), la que se llama como en TMDB, la de más episodios y la de más archivos.
std::vector<std::vector<Catalog::ItemPtr>> group(const std::vector<Catalog::ItemPtr>& items, const Lookup& lookup,
                                                 const std::map<std::int64_t, int>& channelRank);

}  // namespace works

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

class DownloadManager;
class MetadataService;

// Seguimiento (Fase 4, docs/DECISIONS.md D-035): descarga sola los episodios nuevos de las series
// seguidas y las películas seguidas, y pide las versiones mejores de lo que ya se tiene.
namespace tracking {

// Rango de una versión para decidir si es mejor: resolución, después HDR y después REMUX.
// Las ediciones (Extendida, IMAX...) no cuentan: son otras versiones, no mejores.
int versionRank(const std::string& quality, bool hdr, const std::vector<std::string>& tags);
// ¿La calidad no pasa del máximo? (máximo vacío = sin límite; una calidad desconocida siempre cabe)
bool withinQuality(const std::string& quality, const std::string& maxQuality);
// ¿Están publicadas todas las partes? Un archivo sin trocear siempre lo está. Uno troceado, si los
// números van seguidos desde 1 y la última parte es más pequeña que la primera; si no se puede
// saber, cuando lleva 2 h sin partes nuevas.
bool releaseComplete(const Catalog::Release& release, std::int64_t now);

// Versión de una obra seguida que ya se tiene o se está bajando
struct Owned {
    std::int64_t downloadId = 0;
    std::string status;  // queued / downloading / importing / completed
    int rank = 0;
    std::string label;   // "1080p", "4K HDR"... (para el historial)
    int season = 0;
    int episode = 0;
    int episodeEnd = 0;
};

struct Rule {
    std::int64_t since = 0;  // Unix: solo cuenta lo publicado después (cuando se empezó a seguir)
    std::string maxQuality;  // "" = la mejor
};

struct Action {
    enum class Type { NewEpisode, NewMovie, Upgrade };
    Type type = Type::NewEpisode;
    std::size_t release = 0;              // Índice en item.releases
    std::vector<std::int64_t> replaces;   // Descargas que sustituye cuando termine (en la biblioteca o bajándose)
    std::vector<std::int64_t> cancel;     // Descargas en cola, sin empezar, que ya no hacen falta
    std::string previousLabel;            // Mejor versión que se tenía (mejoras)
};

struct Plan {
    std::vector<Action> actions;
    bool waiting = false;  // Hay versiones aún incompletas: volver a mirar más tarde
};

// Qué poner en cola para una obra seguida. handled(release) indica si ese archivo ya tiene una
// descarga (aunque fallara o se cancelara) o si el seguimiento ya lo puso en cola: nunca se repite.
Plan plan(const Catalog::Item& item, const Rule& rule, const std::vector<Owned>& owned,
          const std::function<bool(const Catalog::Release&)>& handled, std::int64_t now);

// Episodios que faltan de una serie (D-040): la mejor versión de cada episodio que no se tiene ni
// se está bajando, dentro de la calidad máxima y sin 3D. Índices en item.releases, por episodio.
std::vector<std::size_t> missingEpisodes(const Catalog::Item& item, const std::vector<Owned>& owned,
                                         const std::string& maxQuality);
// Episodios distintos de una serie y cuántos se tienen (o se están bajando)
struct EpisodeCount {
    int known = 0;
    int owned = 0;
};
EpisodeCount countEpisodes(const Catalog::Item& item, const std::vector<Owned>& owned);

// Versiones que se tienen de cada obra del catálogo (por chat y ficha de la obra), a partir de las
// descargas en cola, en curso o en la biblioteca
using OwnedByItem = std::map<std::pair<std::int64_t, std::int64_t>, std::vector<Owned>>;
OwnedByItem ownedByItem(const Catalog& catalog, const std::vector<DbManager::Download>& downloads);

// Obra del catálogo de cada seguimiento: por cualquiera de sus fichas; si no, por TMDB o por la
// clave de obra (si el catálogo cambia la ficha principal). item es nullptr si ya no está.
struct FollowMatch {
    DbManager::Follow follow;
    Catalog::ItemPtr item;
};
std::vector<FollowMatch> matchFollows(const std::vector<Catalog::ItemPtr>& items,
                                      const std::vector<DbManager::Follow>& follows, const MetadataService& metadata);

// Datos de un seguimiento nuevo para una obra
DbManager::Follow followFor(const Catalog::Item& item, const MetadataService& metadata);

}  // namespace tracking

// Aplica el seguimiento en su propio hilo: cuando cambia el catálogo (mensajes nuevos), cuando se
// sigue algo y cada pocos minutos mientras haya archivos a medio publicar.
class Tracker {
public:
    Tracker(DbManager& db, Catalog& catalog, MetadataService& metadata, DownloadManager& downloads);
    ~Tracker();

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    void start();
    // Detener antes que DownloadManager
    void stop();
    void requestRun();

private:
    void run();
    // Una pasada por todas las obras seguidas; true si alguna espera a que se complete un archivo
    bool evaluate();
    void enqueue(const DbManager::Follow& follow, const Catalog::Item& item, const tracking::Action& action);

    DbManager& db_;
    Catalog& catalog_;
    MetadataService& metadata_;
    DownloadManager& downloads_;
    std::thread worker_;

    std::mutex mutex_;  // Protege stopRequested_ y runRequested_
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool runRequested_ = false;
};

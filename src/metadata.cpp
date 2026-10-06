#include "metadata.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iostream>
#include <set>
#include <utility>

#include "media_parser.hpp"
#include "tmdb_client.hpp"

namespace {

using Json = TmdbClient::Json;
using namespace std::chrono_literals;

// Validez de las respuestas guardadas de TMDB (D-030)
constexpr auto kSearchMaxAge = 24h * 30;
constexpr auto kMovieMaxAge = 24h * 30;
constexpr auto kSeriesMaxAge = 24h * 7;
constexpr auto kAiringMaxAge = 24h;
// Cada cuánto se revisa el catálogo aunque no haya cambios (para refrescar las series en emisión)
constexpr auto kRoundInterval = std::chrono::hours(6);
// Espera mientras el catálogo aún no se ha calculado
constexpr auto kCatalogWait = std::chrono::seconds(10);
// Puntuación mínima para aceptar un resultado de búsqueda (ver score())
constexpr int kMinScore = 80;
constexpr std::size_t kProgressEvery = 100;

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::optional<int> yearOf(const std::string& date) {
    const auto isDigit = [](unsigned char c) { return std::isdigit(c) != 0; };
    if (date.size() >= 4 && std::all_of(date.begin(), date.begin() + 4, isDigit)) {
        return std::stoi(date.substr(0, 4));
    }
    return std::nullopt;
}

std::string text(const Json& object, const char* field) {
    const auto it = object.find(field);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : "";
}

std::int64_t number(const Json& object, const char* field) {
    const auto it = object.find(field);
    return (it != object.end() && it->is_number_integer()) ? it->get<std::int64_t>() : 0;
}

bool keysMatch(const std::string& a, const std::string& b) {
    return !a.empty() && a == b;
}

bool keysContain(const std::string& a, const std::string& b) {
    if (a.size() < 4 || b.size() < 4) {
        return false;
    }
    return a.find(b) != std::string::npos || b.find(a) != std::string::npos;
}

// Puntuación de un resultado de búsqueda: título exacto (en castellano u original) 100,
// contenido 50; año igual +30, a un año +15, distinto -40. Se acepta desde kMinScore: título
// exacto, o título contenido con el mismo año. Mejor sin datos que con los de otra obra.
int score(const Json& result, const Catalog::Item& item, bool series) {
    const std::string localKey = media::titleKey(text(result, series ? "name" : "title"));
    const std::string originalKey = media::titleKey(text(result, series ? "original_name" : "original_title"));

    std::vector<std::string> itemKeys = {media::titleKey(item.title)};
    for (const std::string& alternate : item.alternateTitles) {
        itemKeys.push_back(media::titleKey(alternate));
    }

    int points = 0;
    for (const std::string& key : itemKeys) {
        if (keysMatch(key, localKey) || keysMatch(key, originalKey)) {
            points = std::max(points, 100);
        } else if (keysContain(key, localKey) || keysContain(key, originalKey)) {
            points = std::max(points, 50);
        }
    }

    const auto resultYear = yearOf(text(result, series ? "first_air_date" : "release_date"));
    if (item.year && resultYear) {
        const int difference = std::abs(*item.year - *resultYear);
        points += difference == 0 ? 30 : difference == 1 ? 15 : -40;
    }
    return points;
}

}  // namespace

MetadataService::MetadataService(DbManager& db, Catalog& catalog, TmdbClient& tmdb)
    : db_(db), catalog_(catalog), tmdb_(tmdb) {
    // Lo ya consultado está disponible desde el arranque, aunque no haya token ni red
    for (Info& info : db_.loadMetadata()) {
        const std::string key = info.workKey;
        byKey_[key] = std::make_shared<const Info>(std::move(info));
    }
}

MetadataService::~MetadataService() {
    stop();
}

std::string MetadataService::workKey(const Catalog::Item& item) {
    return item.kind + "|" + media::titleKey(item.title) + "|" + (item.year ? std::to_string(*item.year) : "");
}

void MetadataService::start() {
    if (worker_.joinable() || !tmdb_.enabled()) {
        if (!tmdb_.enabled()) {
            std::cout << "[TMDB] Sin TELEGARRM_TMDB_TOKEN: el catálogo usa solo los datos de las fichas" << std::endl;
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        stopRequested_ = false;
    }
    worker_ = std::thread(&MetadataService::run, this);
}

void MetadataService::stop() {
    if (!worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        stopRequested_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void MetadataService::requestRun() {
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        runRequested_ = true;
    }
    cv_.notify_all();
}

bool MetadataService::sleepFor(std::chrono::seconds duration) {
    std::unique_lock<std::mutex> lock(controlMutex_);
    cv_.wait_for(lock, duration, [this] { return stopRequested_ || runRequested_; });
    runRequested_ = false;
    return !stopRequested_;
}

bool MetadataService::stopping() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return stopRequested_;
}

MetadataService::InfoPtr MetadataService::lookup(const Catalog::Item& item) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byKey_.find(workKey(item));
    if (it == byKey_.end() || it->second->mediaType.empty()) {
        return nullptr;
    }
    return it->second;
}

MetadataService::Stats MetadataService::stats() const {
    Stats stats;
    stats.enabled = tmdb_.enabled();
    const auto items = catalog_.items();
    stats.total = items.size();
    std::lock_guard<std::mutex> lock(mutex_);
    stats.working = working_;
    for (const Catalog::ItemPtr& item : items) {
        const auto it = byKey_.find(workKey(*item));
        if (it != byKey_.end()) {
            ++(it->second->mediaType.empty() ? stats.unmatched : stats.matched);
        }
    }
    return stats;
}

bool MetadataService::due(const Catalog::Item& item, const Info* known, std::int64_t now) const {
    if (!known) {
        return true;
    }
    const auto age = std::chrono::seconds(now - known->updatedAt);
    if (known->mediaType.empty()) {
        return age > kSearchMaxAge;  // Sin coincidencia: se reintenta al mes
    }
    if (item.kind == "series") {
        return age > (item.airing ? kAiringMaxAge : kSeriesMaxAge);
    }
    return age > kMovieMaxAge;
}

void MetadataService::run() {
    std::cout << "[TMDB] Metadatos activados" << std::endl;
    while (!stopping()) {
        std::vector<Catalog::ItemPtr> items = catalog_.items();
        if (items.empty()) {
            if (!sleepFor(kCatalogWait)) {
                return;
            }
            continue;
        }

        // Pendientes: primero las series en emisión y lo publicado más recientemente
        const std::int64_t now = nowSeconds();
        std::vector<Catalog::ItemPtr> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::set<std::string> seen;
            for (const Catalog::ItemPtr& item : items) {
                const std::string key = workKey(*item);
                const auto it = byKey_.find(key);
                if (seen.insert(key).second && due(*item, it != byKey_.end() ? it->second.get() : nullptr, now)) {
                    pending.push_back(item);
                }
            }
            working_ = !pending.empty();
        }
        std::stable_sort(pending.begin(), pending.end(), [](const Catalog::ItemPtr& a, const Catalog::ItemPtr& b) {
            return std::make_pair(!a->airing, -a->updatedAt) < std::make_pair(!b->airing, -b->updatedAt);
        });
        if (!pending.empty()) {
            std::cout << "[TMDB] " << pending.size() << " obras por consultar" << std::endl;
        }

        std::size_t done = 0;
        std::size_t matched = 0;
        for (const Catalog::ItemPtr& item : pending) {
            if (stopping()) {
                return;
            }
            std::optional<Info> info = resolve(*item);
            if (!info) {
                continue;  // TMDB no respondió: se reintentará en la siguiente ronda
            }
            matched += info->mediaType.empty() ? 0 : 1;
            db_.saveMetadata(*info);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const std::string key = info->workKey;
                byKey_[key] = std::make_shared<const Info>(std::move(*info));
            }
            if (++done % kProgressEvery == 0) {
                std::cout << "[TMDB] " << done << "/" << pending.size() << " obras consultadas (" << matched
                          << " encontradas)" << std::endl;
            }
        }
        if (!pending.empty()) {
            std::cout << "[TMDB] Ronda terminada: " << done << " obras consultadas, " << matched << " encontradas"
                      << std::endl;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            working_ = false;
        }
        if (!sleepFor(std::chrono::duration_cast<std::chrono::seconds>(kRoundInterval))) {
            return;
        }
    }
}

std::optional<MetadataService::Info> MetadataService::resolve(const Catalog::Item& item) {
    const bool series = item.kind == "series";
    const std::string type = series ? "tv" : "movie";
    const auto detailsMaxAge = series ? (item.airing ? kAiringMaxAge : kSeriesMaxAge) : kMovieMaxAge;

    Info info;
    info.workKey = workKey(item);
    info.updatedAt = nowSeconds();

    // 1) Identificador de TMDB en los nombres de fichero (D-026)
    Json details;
    if (item.tmdbId > 0) {
        const auto response = tmdb_.get("/" + type + "/" + std::to_string(item.tmdbId),
                                        {{"append_to_response", "external_ids"}}, detailsMaxAge);
        if (!response) {
            return std::nullopt;
        }
        if (response->status == 200) {
            details = response->body;
            info.matchedBy = "tmdbid";
        }
    }

    // 2) Búsqueda por título y títulos alternativos, con el año si se conoce
    if (details.is_null()) {
        std::vector<std::string> titles = {item.title};
        titles.insert(titles.end(), item.alternateTitles.begin(), item.alternateTitles.end());
        std::int64_t bestId = 0;
        int bestScore = 0;
        const int perfectScore = 100 + (item.year ? 30 : 0);  // Título y año exactos
        for (const std::string& title : titles) {
            if (bestScore >= perfectScore) {
                break;
            }
            for (const bool withYear : {true, false}) {
                if (withYear && !item.year) {
                    continue;
                }
                TmdbClient::Params params = {{"query", title}, {"include_adult", "false"}};
                if (withYear) {
                    params.emplace_back(series ? "first_air_date_year" : "year", std::to_string(*item.year));
                }
                const auto response = tmdb_.get("/search/" + type, params, kSearchMaxAge);
                if (!response) {
                    return std::nullopt;
                }
                for (const Json& result : response->body.value("results", Json::array())) {
                    const int points = score(result, item, series);
                    if (points > bestScore) {
                        bestScore = points;
                        bestId = number(result, "id");
                    }
                }
                if (bestScore >= perfectScore) {
                    break;  // No hace falta seguir buscando
                }
            }
        }
        if (bestScore < kMinScore || bestId == 0) {
            info.matchedBy = "none";
            return info;
        }
        const auto response = tmdb_.get("/" + type + "/" + std::to_string(bestId),
                                        {{"append_to_response", "external_ids"}}, detailsMaxAge);
        if (!response) {
            return std::nullopt;
        }
        if (response->status != 200) {
            info.matchedBy = "none";
            return info;
        }
        details = response->body;
        info.matchedBy = "search";
    }

    // 3) Datos e identificadores externos (para poder cambiar de fuente algún día, D-029)
    info.mediaType = type;
    info.providerId = number(details, "id");
    info.title = text(details, series ? "name" : "title");
    info.originalTitle = text(details, series ? "original_name" : "original_title");
    info.overview = text(details, "overview");
    info.year = yearOf(text(details, series ? "first_air_date" : "release_date"));
    info.posterPath = text(details, "poster_path");
    for (const Json& genre : details.value("genres", Json::array())) {
        const std::string name = text(genre, "name");
        if (!name.empty()) {
            info.genres.push_back(name);
        }
    }
    const Json external = details.value("external_ids", Json::object());
    info.imdbId = text(external, "imdb_id");
    info.tvdbId = number(external, "tvdb_id");
    info.wikidataId = text(external, "wikidata_id");

    // 4) Episodios de las temporadas que hay en el catálogo
    if (series) {
        std::set<int> seasons;
        for (const Catalog::Release& release : item.releases) {
            if (release.episode > 0) {
                seasons.insert(release.season);
            }
        }
        for (const int season : seasons) {
            if (stopping()) {
                return std::nullopt;
            }
            const auto response = tmdb_.get("/tv/" + std::to_string(info.providerId) + "/season/" + std::to_string(season),
                                            {}, item.airing ? kAiringMaxAge : kSeriesMaxAge);
            if (!response) {
                return std::nullopt;
            }
            if (response->status != 200) {
                continue;  // La temporada no existe en TMDB (numeración distinta)
            }
            for (const Json& episode : response->body.value("episodes", Json::array())) {
                info.episodes.push_back({season, static_cast<int>(number(episode, "episode_number")),
                                         text(episode, "name"), text(episode, "overview"), text(episode, "air_date")});
            }
        }
    }
    return info;
}

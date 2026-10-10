#include "metadata.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iostream>
#include <limits>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
// Resultados de búsqueda cuyos títulos alternativos se miran si ninguno encaja por su nombre
constexpr std::size_t kAlternativeCandidates = 3;
constexpr std::size_t kProgressEvery = 100;
// Versión del criterio de coincidencia: al cambiarla, todas las obras se vuelven a buscar (con la
// caché de TMDB casi no cuesta) y, mientras, se siguen mostrando los datos anteriores. 2: penalización
// leve del año en series. 3: mismas palabras en otro orden y títulos alternativos de TMDB (D-048).
constexpr int kMatcherVersion = 3;
constexpr const char* kMatcherVersionSetting = "tmdb_matcher_version";
constexpr const char* kMatcherChangedSetting = "tmdb_matcher_changed_at";

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

// Palabras de un título, normalizadas y ordenadas: "Mugenjou-hen" -> "hen", "mugenjou"
std::vector<std::string> titleWords(const std::string& title) {
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&words, &word] {
        std::string key = media::titleKey(word);
        if (!key.empty()) {
            words.push_back(std::move(key));
        }
        word.clear();
    };
    for (const char c : title) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80 && std::isalnum(byte) == 0) {
            flush();
        } else {
            word.push_back(c);
        }
    }
    flush();
    std::sort(words.begin(), words.end());
    return words;
}

// Títulos de una obra (el principal y los alternativos) con su clave y sus palabras
struct ItemTitle {
    std::string key;
    std::vector<std::string> words;
    // Junta dos cosas ("Chicken Run: Amanecer de los Nuggets + Así se hizo"): sus palabras en otro orden
    // son otra obra ("Así se hizo 'Chicken Run: Amanecer de los nuggets'")
    bool combined = false;
};

ItemTitle makeTitle(const std::string& title) {
    return {media::titleKey(title), titleWords(title), title.find(" + ") != std::string::npos};
}

std::vector<ItemTitle> itemTitles(const Catalog::Item& item) {
    std::vector<ItemTitle> titles = {makeTitle(item.title)};
    for (const std::string& alternate : item.alternateTitles) {
        titles.push_back(makeTitle(alternate));
    }
    return titles;
}

// Cómo encaja un título de TMDB con los de la obra: 100 si es el mismo o tiene las mismas palabras
// en otro orden (al menos tres: "Guardianes de la noche: Kimetsu no Yaiba La fortaleza infinita");
// 50 si uno contiene al otro (solo con withContains)
int titlePoints(const std::vector<ItemTitle>& titles, const std::string& candidate, bool withContains) {
    const std::string key = media::titleKey(candidate);
    const std::vector<std::string> words = titleWords(candidate);
    int points = 0;
    for (const ItemTitle& title : titles) {
        if (keysMatch(title.key, key) || (title.words.size() >= 3 && !title.combined && title.words == words)) {
            return 100;
        }
        if (withContains && keysContain(title.key, key)) {
            points = 50;
        }
    }
    return points;
}

// Año igual +30, a un año +15, distinto -40 en películas (remakes) y solo -10 en series, cuyo año en
// la ficha suele ser el de la temporada o la subida y no el del estreno (Ultimate Spider-Man: 2015 en
// la ficha, 2012 en TMDB)
int yearPoints(const Json& result, const Catalog::Item& item, bool series) {
    const auto resultYear = yearOf(text(result, series ? "first_air_date" : "release_date"));
    if (!item.year || !resultYear) {
        return 0;
    }
    const int difference = std::abs(*item.year - *resultYear);
    return difference == 0 ? 30 : difference == 1 ? 15 : (series ? -10 : -40);
}

// Puntuación de un resultado de búsqueda: su título en castellano u original (titlePoints) más el año.
// Se acepta desde kMinScore: título exacto, o título contenido con el mismo año. Mejor sin datos que
// con los de otra obra.
int score(const Json& result, const std::vector<ItemTitle>& titles, const Catalog::Item& item, bool series) {
    const int points = std::max(titlePoints(titles, text(result, series ? "name" : "title"), true),
                                titlePoints(titles, text(result, series ? "original_name" : "original_title"), true));
    return points + yearPoints(result, item, series);
}

// ¿Un título alternativo de TMDB es otro nombre de la obra entera? Los de una temporada, un arco o un
// especial ("Kimetsu no Yaiba: Yuukaku-hen", tipo "Season 3 Romaji") no: un arco publicado como serie
// aparte acabaría en la carpeta de la serie, con sus episodios en la temporada 1 de la serie y
// sustituyendo a los de verdad (D-041). Las abreviaturas ("BNHA") tampoco.
//
// Los canales de anime publican cada arco o secuela como una serie aparte, numerada desde la
// temporada 1, y muchos arcos no llevan tipo en TMDB ("Gintama.: Porori-hen"). En sus series:
// - Si la obra se llama como la serie de TMDB y algo más ("Full Metal Panic! The Second Raid",
//   "Bakemonogatari"), es una secuela: no vale por un título alternativo.
// - Solo valen los nombres de la obra entera con tipo: romaji, en inglés, título completo...
//   ("Boku no Hero Academia", "Romaji").
// Los canales normales numeran como TMDB ("Ataque a los Titanes 4x29") y no lo necesitan.
bool wholeWorkTitle(const Json& alternative, const Json& result, const Catalog::Item& item, bool series) {
    static const std::regex kShort(R"(abbrev|nickname)", std::regex::ECMAScript | std::regex::icase);
    static const std::regex kPartial(
        R"(season|temporada|saison|staffel|stagione|\bs\d|series \d|series title|\b(?:\d+(?:st|nd|rd|th)|second|third|fourth|fifth)\b|special|especial|\b(?:ova|ona|oad)\b|movie|film|pel(?:í|i)cula|\barc\b|arco|\bpart|parte|cour|episod)",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex kWholeWork(
        R"(roma|translit|english|full title|official|former title|spelling|macron|uncensored|retronym)",
        std::regex::ECMAScript | std::regex::icase);
    const std::string type = text(alternative, "type");
    if (std::regex_search(type, kShort)) {
        return false;
    }
    if (!series) {
        return true;
    }
    if (std::regex_search(type, kPartial)) {
        return false;
    }
    if (!item.anime) {
        return true;
    }
    const std::string key = media::titleKey(item.title);
    for (const char* field : {"name", "original_name"}) {
        const std::string show = media::titleKey(text(result, field));
        if (show.size() >= 4 && key.size() > show.size() && key.find(show) != std::string::npos) {
            return false;
        }
    }
    return std::regex_search(type, kWholeWork);
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
    if (!known || known->updatedAt < matcherChangedAt_) {
        return true;  // Sin buscar, o buscada con un criterio anterior
    }
    // Sus archivos dicen su identificador de TMDB y lo guardado no salió de él: lo buscó otra obra con
    // la misma clave (de otro canal, quizá antes de que este canal estuviera en el catálogo)
    if (item.tmdbId > 0 && known->matchedBy != "tmdbid") {
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
    // Criterio de coincidencia nuevo: todo lo buscado antes se vuelve a buscar (due()). La fecha se
    // guarda para que un reinicio a mitad no deje obras con el criterio anterior.
    if (db_.getSetting(kMatcherVersionSetting).value_or("") != std::to_string(kMatcherVersion)) {
        db_.setSetting(kMatcherChangedSetting, std::to_string(nowSeconds()));
        db_.setSetting(kMatcherVersionSetting, std::to_string(kMatcherVersion));
        std::cout << "[TMDB] Criterio de coincidencia nuevo: se vuelven a buscar todas las obras" << std::endl;
    }
    try {
        matcherChangedAt_ = std::stoll(db_.getSetting(kMatcherChangedSetting).value_or("0"));
    } catch (const std::exception&) {
        matcherChangedAt_ = 0;
    }
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
            // Obras de varios canales con la misma clave ("Look Back" de 2024 en Las Cositas y en
            // CrunchyShur) se buscan una vez: con la que lleve el identificador de TMDB en sus
            // archivos, si alguna lo lleva (D-048)
            std::map<std::string, Catalog::ItemPtr> byWorkKey;
            for (const Catalog::ItemPtr& item : items) {
                const auto [it, inserted] = byWorkKey.emplace(workKey(*item), item);
                if (!inserted && it->second->tmdbId == 0 && item->tmdbId > 0) {
                    it->second = item;
                }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [key, item] : byWorkKey) {
                const auto it = byKey_.find(key);
                if (due(*item, it != byKey_.end() ? it->second.get() : nullptr, now)) {
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
        const std::vector<ItemTitle> keys = itemTitles(item);
        std::int64_t bestId = 0;
        int bestScore = 0;
        const int perfectScore = 100 + (item.year ? 30 : 0);  // Título y año exactos
        std::vector<Json> candidates;  // Resultados distintos, en el orden de TMDB
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
                    const int points = score(result, keys, item, series);
                    if (points > bestScore) {
                        bestScore = points;
                        bestId = number(result, "id");
                    }
                    const std::int64_t id = number(result, "id");
                    if (std::none_of(candidates.begin(), candidates.end(),
                                     [id](const Json& candidate) { return number(candidate, "id") == id; })) {
                        candidates.push_back(result);
                    }
                }
                if (bestScore >= perfectScore) {
                    break;  // No hace falta seguir buscando
                }
            }
        }
        // Ningún nombre encaja: los otros títulos de los primeros resultados ("Boku no Hero Academia"
        // es el romaji de "My Hero Academia"; TMDB lo encuentra, pero no lo dice en el resultado).
        // Solo cuentan los títulos iguales, no los contenidos.
        for (std::size_t i = 0; bestScore < kMinScore && i < candidates.size() && i < kAlternativeCandidates; ++i) {
            const std::int64_t id = number(candidates[i], "id");
            const auto response = tmdb_.get("/" + type + "/" + std::to_string(id) + "/alternative_titles", {},
                                            kSearchMaxAge);
            if (!response) {
                return std::nullopt;
            }
            // Películas: "titles"; series: "results"
            const Json list = response->body.value(series ? "results" : "titles", Json::array());
            for (const Json& alternative : list) {
                if (wholeWorkTitle(alternative, candidates[i], item, series) &&
                    titlePoints(keys, text(alternative, "title"), false) == 100) {
                    const int points = 100 + yearPoints(candidates[i], item, series);
                    if (points > bestScore) {
                        bestScore = points;
                        bestId = id;
                    }
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

namespace works {

std::vector<std::vector<Catalog::ItemPtr>> group(const std::vector<Catalog::ItemPtr>& items, const Lookup& lookup,
                                                 const std::map<std::int64_t, int>& channelRank) {
    std::vector<std::vector<Catalog::ItemPtr>> groups;
    std::vector<MetadataService::InfoPtr> infos;  // Datos de TMDB de cada grupo (de su primera obra)
    std::map<std::pair<std::string, std::int64_t>, std::size_t> byWork;  // (tipo, id de TMDB) -> grupo
    for (const Catalog::ItemPtr& item : items) {
        MetadataService::InfoPtr info = lookup(*item);
        if (info && info->providerId > 0) {
            const auto [it, inserted] = byWork.emplace(std::make_pair(info->mediaType, info->providerId), groups.size());
            if (!inserted) {
                groups[it->second].push_back(item);
                continue;
            }
        }
        groups.push_back({item});
        infos.push_back(std::move(info));
    }

    for (std::size_t i = 0; i < groups.size(); ++i) {
        std::vector<Catalog::ItemPtr>& members = groups[i];
        if (members.size() < 2) {
            continue;
        }
        const MetadataService::InfoPtr& info = infos[i];
        const std::string localKey = media::titleKey(info->title);
        const std::string originalKey = media::titleKey(info->originalTitle);
        const auto rankOf = [&channelRank](std::int64_t chatId) {
            const auto it = channelRank.find(chatId);
            return it != channelRank.end() ? it->second : std::numeric_limits<int>::max();
        };
        const auto preference = [&](const Catalog::ItemPtr& item) {
            const std::string key = media::titleKey(item->title);
            const bool sameName = keysMatch(key, localKey) || keysMatch(key, originalKey);
            return std::make_tuple(rankOf(item->chatId), !sameName, -item->episodeCount,
                                   -static_cast<long long>(item->releases.size()), item->anchorMessageId);
        };
        std::stable_sort(members.begin(), members.end(), [&preference](const Catalog::ItemPtr& a,
                                                                      const Catalog::ItemPtr& b) {
            return preference(a) < preference(b);
        });
    }
    return groups;
}

}  // namespace works

#include "tracker.hpp"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "download_manager.hpp"
#include "library.hpp"
#include "media_parser.hpp"
#include "metadata.hpp"

namespace tracking {
namespace {

using Release = Catalog::Release;

// Un archivo troceado del que no se puede saber si está completo se da por terminado tras esto
constexpr std::int64_t kSettleSeconds = 2 * 3600;
// Un archivo incompleto deja de esperarse (y de revisarse cada pocos minutos) tras esto
constexpr std::int64_t kGiveUpSeconds = 24 * 3600;

bool hasTag(const std::vector<std::string>& tags, const char* tag) {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

bool coversEpisode(const Owned& owned, int season, int episode) {
    return owned.season == season && episode >= owned.episode && episode <= std::max(owned.episode, owned.episodeEnd);
}

}  // namespace

int versionRank(const std::string& quality, bool hdr, const std::vector<std::string>& tags) {
    return media::qualityRank(quality) * 4 + (hdr ? 2 : 0) + (hasTag(tags, "REMUX") ? 1 : 0);
}

bool withinQuality(const std::string& quality, const std::string& maxQuality) {
    return maxQuality.empty() || media::qualityRank(quality) <= media::qualityRank(maxQuality);
}

bool releaseComplete(const Release& release, std::int64_t now) {
    if (release.parts.empty()) {
        return false;
    }
    if (release.parts.size() == 1 && release.parts.front().number == 0) {
        return true;  // Un solo archivo: el mensaje solo aparece cuando la subida ha terminado
    }
    for (std::size_t i = 0; i < release.parts.size(); ++i) {
        if (release.parts[i].number != static_cast<int>(i) + 1) {
            return false;  // Falta alguna parte intermedia (o la primera)
        }
    }
    // Las partes llenas miden lo mismo; la última, lo que sobra
    if (release.parts.size() > 1 && release.parts.back().size < release.parts.front().size) {
        return true;
    }
    return now - release.date >= kSettleSeconds;
}

Plan plan(const Catalog::Item& item, const Rule& rule, const std::vector<Owned>& owned,
          const std::function<bool(const Release&)>& handled, std::int64_t now) {
    Plan result;
    const bool series = item.kind == "series";

    // Huecos de la obra: cada episodio en una serie; uno solo en una película (sus versiones)
    struct Slot {
        std::int64_t firstSeen = std::numeric_limits<std::int64_t>::max();
        std::vector<std::size_t> releases;
    };
    std::map<std::pair<int, int>, Slot> slots;
    for (std::size_t i = 0; i < item.releases.size(); ++i) {
        const Release& release = item.releases[i];
        if (series && release.episode == 0) {
            continue;  // Archivos sueltos de una serie (extras): no se siguen
        }
        Slot& slot = slots[series ? std::make_pair(release.season, release.episode) : std::make_pair(0, 0)];
        slot.firstSeen = std::min(slot.firstSeen, release.date);
        slot.releases.push_back(i);
    }

    for (const auto& [key, slot] : slots) {
        std::vector<const Owned*> have;
        int ownedRank = -1;
        for (const Owned& version : owned) {
            if (!series || coversEpisode(version, key.first, key.second)) {
                have.push_back(&version);
                ownedRank = std::max(ownedRank, version.rank);
            }
        }

        // La mejor versión publicada después de seguir la obra, dentro de la calidad máxima. A igual
        // rango, la primera del catálogo (que ordena de mejor a peor y después por publicación).
        const Release* best = nullptr;
        std::size_t bestIndex = 0;
        int bestRank = -1;
        for (const std::size_t index : slot.releases) {
            const Release& release = item.releases[index];
            if (release.date < rule.since || !withinQuality(release.quality, rule.maxQuality) ||
                hasTag(release.tags, "3D") || handled(release)) {
                continue;
            }
            const int rank = versionRank(release.quality, release.hdr, release.tags);
            if (rank > bestRank) {
                best = &release;
                bestIndex = index;
                bestRank = rank;
            }
        }
        if (!best) {
            continue;
        }
        if (have.empty()) {
            // Un episodio que ya estaba publicado antes de seguir la serie no es nuevo (aunque se vuelva a
            // subir, ej. con la temporada completa)
            if (series && slot.firstSeen < rule.since) {
                continue;
            }
        } else if (bestRank <= ownedRank) {
            continue;  // Lo que se tiene es igual o mejor
        }
        // Si la mejor versión aún se está publicando, se espera por ella en vez de bajar una peor
        if (!releaseComplete(*best, now)) {
            if (now - best->date < kGiveUpSeconds) {
                result.waiting = true;
            }
            continue;
        }

        Action action;
        action.type = !have.empty() ? Action::Type::Upgrade : series ? Action::Type::NewEpisode : Action::Type::NewMovie;
        action.release = bestIndex;
        int previousRank = -1;
        for (const Owned* version : have) {
            (version->status == "queued" ? action.cancel : action.replaces).push_back(version->downloadId);
            if (version->rank > previousRank) {
                previousRank = version->rank;
                action.previousLabel = version->label;
            }
        }
        result.actions.push_back(std::move(action));
    }
    return result;
}

namespace {

// Episodios (temporada, episodio) que cubre lo que se tiene
std::set<std::pair<int, int>> ownedEpisodes(const std::vector<Owned>& owned) {
    std::set<std::pair<int, int>> episodes;
    for (const Owned& version : owned) {
        for (int episode = version.episode; version.episode > 0 && episode <= std::max(version.episode, version.episodeEnd);
             ++episode) {
            episodes.emplace(version.season, episode);
        }
    }
    return episodes;
}

}  // namespace

std::vector<std::size_t> missingEpisodes(const Catalog::Item& item, const std::vector<Owned>& owned,
                                         const std::string& maxQuality) {
    std::vector<std::size_t> chosen;
    if (item.kind != "series") {
        return chosen;
    }
    // La mejor versión que cabe de cada episodio (por su primer episodio si trae varios)
    std::map<std::pair<int, int>, std::size_t> best;
    for (std::size_t i = 0; i < item.releases.size(); ++i) {
        const Release& release = item.releases[i];
        if (release.episode == 0 || !withinQuality(release.quality, maxQuality) || hasTag(release.tags, "3D")) {
            continue;
        }
        const auto key = std::make_pair(release.season, release.episode);
        const auto it = best.find(key);
        if (it == best.end() || versionRank(release.quality, release.hdr, release.tags) >
                                    versionRank(item.releases[it->second].quality, item.releases[it->second].hdr,
                                                item.releases[it->second].tags)) {
            best[key] = i;
        }
    }
    // En orden de temporada y episodio; un archivo con varios episodios (1x01-02) cubre todos los suyos
    std::set<std::pair<int, int>> covered = ownedEpisodes(owned);
    for (const auto& [key, index] : best) {
        if (covered.count(key)) {
            continue;
        }
        const Release& release = item.releases[index];
        for (int episode = release.episode; episode <= std::max(release.episode, release.episodeEnd); ++episode) {
            covered.emplace(release.season, episode);
        }
        chosen.push_back(index);
    }
    return chosen;
}

EpisodeCount countEpisodes(const Catalog::Item& item, const std::vector<Owned>& owned) {
    std::set<std::pair<int, int>> known;
    for (const Release& release : item.releases) {
        for (int episode = release.episode; release.episode > 0 && episode <= std::max(release.episode, release.episodeEnd);
             ++episode) {
            known.emplace(release.season, episode);
        }
    }
    const std::set<std::pair<int, int>> have = ownedEpisodes(owned);
    EpisodeCount count;
    count.known = static_cast<int>(known.size());
    for (const auto& episode : known) {
        count.owned += have.count(episode) ? 1 : 0;
    }
    return count;
}

OwnedByItem ownedByItem(const Catalog& catalog, const std::vector<DbManager::Download>& downloads) {
    static const std::set<std::string> kOwnedStatuses = {"queued", "downloading", "importing", "completed"};
    OwnedByItem result;
    for (const DbManager::Download& download : downloads) {
        if (!kOwnedStatuses.count(download.status)) {
            continue;
        }
        const auto ref = catalog.findRelease(download.chatId, download.messageId);
        if (!ref) {
            continue;
        }
        result[{ref->item->chatId, ref->item->anchorMessageId}].push_back(
            {download.id, download.status, versionRank(download.quality, download.hdr, download.tags),
             library::versionLabel(download.quality, download.hdr, download.tags), download.season, download.episode,
             download.episodeEnd});
    }
    return result;
}

std::vector<FollowMatch> matchFollows(const std::vector<Catalog::ItemPtr>& items,
                                      const std::vector<DbManager::Follow>& follows, const MetadataService& metadata) {
    std::map<std::pair<std::int64_t, std::int64_t>, Catalog::ItemPtr> byBlock;
    for (const Catalog::ItemPtr& item : items) {
        for (const auto& block : item->blockIds) {
            byBlock.emplace(block, item);
        }
    }

    std::vector<FollowMatch> matches;
    for (const DbManager::Follow& follow : follows) {
        FollowMatch match{follow, nullptr};
        const auto it = byBlock.find({follow.chatId, follow.anchorId});
        if (it != byBlock.end()) {
            match.item = it->second;
        } else {
            // La ficha ya no identifica a la obra (ej. se borró el mensaje): por TMDB o por la clave
            Catalog::ItemPtr byKey;
            for (const Catalog::ItemPtr& item : items) {
                if (item->kind != follow.kind) {
                    continue;
                }
                const auto info = metadata.lookup(*item);
                const std::int64_t tmdbId = info ? info->providerId : item->tmdbId;
                if (follow.tmdbId != 0 && tmdbId == follow.tmdbId) {
                    match.item = item;
                    break;
                }
                if (!byKey && !follow.workKey.empty() && MetadataService::workKey(*item) == follow.workKey) {
                    byKey = item;
                }
            }
            if (!match.item) {
                match.item = byKey;
            }
        }
        matches.push_back(std::move(match));
    }
    return matches;
}

DbManager::Follow followFor(const Catalog::Item& item, const MetadataService& metadata) {
    const auto info = metadata.lookup(item);
    DbManager::Follow follow;
    follow.kind = item.kind;
    follow.title = item.title;
    follow.year = item.year ? item.year : (info ? info->year : std::nullopt);
    follow.tmdbId = info ? info->providerId : item.tmdbId;
    follow.workKey = MetadataService::workKey(item);
    follow.chatId = item.chatId;
    follow.anchorId = item.anchorMessageId;
    return follow;
}

}  // namespace tracking

namespace {

// Revisión periódica: más frecuente mientras haya archivos a medio publicar
constexpr auto kWaitingInterval = std::chrono::minutes(5);
constexpr auto kIdleInterval = std::chrono::minutes(30);

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

}  // namespace

Tracker::Tracker(DbManager& db, Catalog& catalog, MetadataService& metadata, DownloadManager& downloads)
    : db_(db), catalog_(catalog), metadata_(metadata), downloads_(downloads) {}

Tracker::~Tracker() {
    stop();
}

void Tracker::start() {
    if (worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
    }
    worker_ = std::thread(&Tracker::run, this);
}

void Tracker::stop() {
    if (!worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void Tracker::requestRun() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runRequested_ = true;
    }
    cv_.notify_all();
}

void Tracker::run() {
    bool waiting = false;
    for (;;) {
        {
            // La primera pasada espera a que se pida (el catálogo se calcula al arrancar y avisa)
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, waiting ? kWaitingInterval : kIdleInterval,
                         [this] { return stopRequested_ || runRequested_; });
            if (stopRequested_) {
                return;
            }
            runRequested_ = false;
        }
        try {
            waiting = evaluate();
        } catch (const std::exception& e) {
            std::cerr << "[Seguimiento] Error al revisar las obras seguidas: " << e.what() << std::endl;
        }
    }
}

bool Tracker::evaluate() {
    const std::vector<DbManager::Follow> follows = db_.listFollows();
    if (follows.empty()) {
        return false;
    }
    const std::vector<Catalog::ItemPtr> items = catalog_.items();
    if (items.empty()) {
        return false;  // El catálogo aún no se ha calculado
    }

    // Archivos que ya tienen descarga (cualquier estado) o que el seguimiento ya puso en cola
    const std::vector<DbManager::Download> downloads = db_.listDownloads();
    std::set<std::pair<std::int64_t, std::int64_t>> handledParts;
    for (const DbManager::Download& download : downloads) {
        handledParts.emplace(download.chatId, download.messageId);
    }
    for (const auto& release : db_.listAutoReleases()) {
        handledParts.insert(release);
    }
    const auto handled = [&handledParts](const Catalog::Release& release) {
        return std::any_of(release.parts.begin(), release.parts.end(), [&](const Catalog::Part& part) {
            return handledParts.count({release.chatId, part.messageId}) > 0;
        });
    };

    // Versiones que se tienen de cada obra (por su identificador en el catálogo)
    const tracking::OwnedByItem ownedByItem = tracking::ownedByItem(catalog_, downloads);

    bool waiting = false;
    const std::int64_t now = nowSeconds();
    for (const tracking::FollowMatch& match : tracking::matchFollows(items, follows, metadata_)) {
        if (!match.item) {
            continue;  // Ya no está en el catálogo (ej. se quitó el canal)
        }
        const Catalog::Item& item = *match.item;

        // Si el catálogo ha cambiado la ficha principal o TMDB ha encontrado la obra, se actualiza
        DbManager::Follow current = tracking::followFor(item, metadata_);
        current.id = match.follow.id;
        // Lo ya conocido no se pierde si TMDB deja de encontrar la obra: sirve para reencontrarla
        if (current.tmdbId == 0) {
            current.tmdbId = match.follow.tmdbId;
        }
        if (!current.year) {
            current.year = match.follow.year;
        }
        if (current.chatId != match.follow.chatId || current.anchorId != match.follow.anchorId ||
            current.title != match.follow.title || current.year != match.follow.year ||
            current.tmdbId != match.follow.tmdbId || current.workKey != match.follow.workKey) {
            db_.updateFollowWork(current);
        }

        const auto owned = ownedByItem.find({item.chatId, item.anchorMessageId});
        const tracking::Plan plan =
            tracking::plan(item, {match.follow.createdAt, match.follow.maxQuality},
                           owned != ownedByItem.end() ? owned->second : std::vector<tracking::Owned>{}, handled, now);
        waiting = waiting || plan.waiting;
        for (const tracking::Action& action : plan.actions) {
            enqueue(match.follow, item, action);
        }
    }
    return waiting;
}

void Tracker::enqueue(const DbManager::Follow& follow, const Catalog::Item& item, const tracking::Action& action) {
    const Catalog::Release& release = item.releases[action.release];
    DbManager::Download download = makeDownload(item, release);
    download.origin = "auto";
    download.followId = follow.id;
    download.replaces = action.replaces;

    const DbManager::AddDownloadResult added = db_.addDownload(download);
    // Aunque falle o ya exista, se anota para no intentarlo en cada pasada
    db_.addAutoRelease(download.chatId, download.messageId, follow.id);
    if (!added.ok) {
        if (!added.duplicate) {
            std::cerr << "[Seguimiento] No se pudo poner en cola " << describeDownload(download) << std::endl;
        }
        return;
    }

    std::string message = describeDownload(download);
    switch (action.type) {
        case tracking::Action::Type::NewEpisode:
            message += ": episodio nuevo, en cola.";
            break;
        case tracking::Action::Type::NewMovie:
            message += ": publicada, en cola.";
            break;
        case tracking::Action::Type::Upgrade:
            message += ": mejor que la versión que tienes" +
                       (action.previousLabel.empty() ? std::string() : " (" + action.previousLabel + ")") + ", en cola.";
            break;
    }
    // La versión peor que esperaba en cola ya no hace falta
    for (const std::int64_t id : action.cancel) {
        const auto queued = db_.getDownload(id);
        if (queued && queued->status == "queued" && db_.setDownloadStatus(id, "cancelled")) {
            message += " Se cancela la descarga en cola de " +
                       library::versionLabel(queued->quality, queued->hdr, queued->tags) + ".";
        }
    }

    const char* type = action.type == tracking::Action::Type::Upgrade      ? "queued_upgrade"
                       : action.type == tracking::Action::Type::NewEpisode ? "queued_episode"
                                                                            : "queued_movie";
    db_.addActivity({0, 0, type, message, download.chatId, download.messageId, follow.id, added.id});
    std::cout << "[Seguimiento] " << message << std::endl;
    downloads_.wake();
}

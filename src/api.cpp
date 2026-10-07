#include "api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "catalog.hpp"
#include "channel_sync.hpp"
#include "db_manager.hpp"
#include "download_manager.hpp"
#include "httplib.h"
#include "metadata.hpp"
#include "release_prober.hpp"
#include "settings.hpp"
#include "telegram_client.hpp"
#include "tmdb_client.hpp"
#include "tracker.hpp"

namespace {

using Json = nlohmann::json;

// Espera máxima a que TDLib responda a una petición hecha desde la API
constexpr auto kTelegramTimeout = std::chrono::seconds(30);
// Límite de mensajes por página en /api/channels/{id}/messages
constexpr int kMaxMessagesPerPage = 500;
// Las portadas se sirven con la menor resolución que tenga al menos este ancho (px)
constexpr int kPosterMinWidth = 600;
// Búfer de descargas si no se configura otro: TDLib guarda los archivos junto a su base de datos
constexpr const char* kDefaultDownloadDir = "db/tdlib";
// Las portadas no cambian: el navegador puede guardarlas una semana
constexpr const char* kPosterCacheControl = "max-age=604800";
// Calidades máximas que admite un seguimiento ("" = la mejor)
const std::set<std::string> kMaxQualities = {"", "1080p", "720p"};
// Entradas del historial de actividad por página
constexpr int kActivityPageSize = 100;

// Paso del inicio de sesión: estado que debe tener TDLib, petición que se le envía
// y campo del cuerpo JSON (se llama igual en la API y en TDLib)
struct AuthStep {
    const char* path;
    const char* expectedState;
    const char* tdlibRequest;
    const char* field;
};

constexpr AuthStep kAuthSteps[] = {
    {"/api/telegram/auth/phone", "authorizationStateWaitPhoneNumber", "setAuthenticationPhoneNumber", "phone_number"},
    {"/api/telegram/auth/code", "authorizationStateWaitCode", "checkAuthenticationCode", "code"},
    {"/api/telegram/auth/password", "authorizationStateWaitPassword", "checkAuthenticationPassword", "password"},
};

std::string typeOf(const Json& object) {
    return object.is_object() ? object.value("@type", "") : "";
}

bool isError(const std::optional<Json>& response) {
    return !response || typeOf(*response) == "error";
}

std::string errorMessage(const std::optional<Json>& response) {
    return response ? response->value("message", "Error desconocido de Telegram")
                    : std::string("Telegram no ha respondido a tiempo");
}

void sendJson(httplib::Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, status, {{"error", message}});
}

bool requireTelegramReady(TelegramClient& telegram, httplib::Response& res) {
    if (typeOf(telegram.authorizationState()) != "authorizationStateReady") {
        sendError(res, 409, "Primero hay que iniciar sesión en Telegram");
        return false;
    }
    return true;
}

std::optional<std::int64_t> parseId(const std::string& text) {
    try {
        std::size_t pos = 0;
        const std::int64_t value = std::stoll(text, &pos);
        return pos == text.size() ? std::optional<std::int64_t>(value) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Tipo de chat para la web: "channel", "supergroup" o "group"; vacío si no sirve (chats privados)
// y rol de la cuenta en él: "owner", "admin" o "member"
struct ChatKind {
    std::string type;
    std::string role;
};

std::string roleFromStatus(const Json& status) {
    const std::string type = typeOf(status);
    if (type == "chatMemberStatusCreator") {
        return "owner";
    }
    if (type == "chatMemberStatusAdministrator") {
        return "admin";
    }
    return "member";
}

ChatKind classifyChat(TelegramClient& telegram, const Json& chat) {
    const Json& type = chat.at("type");
    const std::string typeName = typeOf(type);
    if (typeName == "chatTypeSupergroup") {
        const auto group = telegram.request(
            {{"@type", "getSupergroup"}, {"supergroup_id", type.value("supergroup_id", std::int64_t{0})}},
            kTelegramTimeout);
        const std::string role = isError(group) ? "member" : roleFromStatus(group->value("status", Json()));
        return {type.value("is_channel", false) ? "channel" : "supergroup", role};
    }
    if (typeName == "chatTypeBasicGroup") {
        const auto group = telegram.request(
            {{"@type", "getBasicGroup"}, {"basic_group_id", type.value("basic_group_id", std::int64_t{0})}},
            kTelegramTimeout);
        const std::string role = isError(group) ? "member" : roleFromStatus(group->value("status", Json()));
        return {"group", role};
    }
    return {};
}

Json channelJson(const DbManager::Channel& channel, std::int64_t syncingChatId) {
    return {{"id", channel.id},
            {"title", channel.title},
            {"added_at", channel.addedAt},
            {"message_count", channel.messageCount},
            {"file_count", channel.fileCount},
            {"history_complete", channel.historyComplete},
            {"last_sync_at", channel.lastSyncAt ? Json(*channel.lastSyncAt) : Json(nullptr)},
            {"syncing", channel.id == syncingChatId}};
}

Json messageJson(const DbManager::Message& message) {
    return {{"id", message.messageId},
            {"date", message.date},
            {"album_id", message.mediaAlbumId},
            {"type", message.contentType},
            {"text", message.text},
            {"file_name", message.fileName ? Json(*message.fileName) : Json(nullptr)},
            {"file_size", message.fileSize ? Json(*message.fileSize) : Json(nullptr)},
            {"mime_type", message.mimeType ? Json(*message.mimeType) : Json(nullptr)},
            {"topic_id", message.topicId}};
}

void registerStatusRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram, MetadataService& metadata) {
    // Estado del servicio, de la BD (lee 'version' de settings), de la sesión de Telegram y de TMDB
    server.Get("/api/status", [&db, &telegram, &metadata](const httplib::Request&, httplib::Response& res) {
        Json body = {{"status", "Telegarrm is running"}, {"version", TELEGARRM_VERSION}};
        int status = 200;

        if (const auto dbVersion = db.getSetting("version")) {
            body["database"] = {{"status", "ok"}, {"version", *dbVersion}};
        } else {
            body["database"] = {{"status", "error"}, {"version", nullptr}};
            status = 503;
        }

        const Json auth = telegram.authorizationState();
        const std::string authType = typeOf(auth);
        const std::string connType = typeOf(telegram.connectionState());
        Json tg = {{"authorization_state", authType.empty() ? Json(nullptr) : Json(authType)},
                   {"connection_state", connType.empty() ? Json(nullptr) : Json(connType)}};
        if (authType == "authorizationStateWaitPassword") {
            tg["password_hint"] = auth.value("password_hint", "");
        }
        body["telegram"] = tg;

        const MetadataService::Stats stats = metadata.stats();
        body["metadata"] = {{"enabled", stats.enabled},
                            {"working", stats.working},
                            {"total", stats.total},
                            {"matched", stats.matched},
                            {"unmatched", stats.unmatched}};

        sendJson(res, status, body);
    });
}

void registerAuthRoutes(httplib::Server& server, TelegramClient& telegram) {
    // Inicio de sesión en Telegram, paso a paso: teléfono -> código -> contraseña 2FA (si la hay)
    for (const AuthStep& step : kAuthSteps) {
        server.Post(step.path, [&telegram, step](const httplib::Request& req, httplib::Response& res) {
            const Json body = Json::parse(req.body, nullptr, false);
            const Json* value = (body.is_object() && body.contains(step.field)) ? &body.at(step.field) : nullptr;
            if (!value || !value->is_string() || value->get_ref<const std::string&>().empty()) {
                sendError(res, 400, std::string("El cuerpo debe ser JSON con el campo '") + step.field + "'");
                return;
            }

            const std::string current = typeOf(telegram.authorizationState());
            if (current != step.expectedState) {
                sendError(res, 409, "Telegram no espera este dato ahora (estado actual: " +
                                        (current.empty() ? std::string("desconocido") : current) + ")");
                return;
            }

            const auto response = telegram.request({{"@type", step.tdlibRequest}, {step.field, *value}}, kTelegramTimeout);
            if (!response) {
                sendError(res, 504, "Telegram no ha respondido a tiempo");
                return;
            }
            if (typeOf(*response) == "error") {
                // Ej. PHONE_NUMBER_INVALID, PHONE_CODE_INVALID, PASSWORD_HASH_INVALID
                sendError(res, 400, response->value("message", "Error desconocido de Telegram"));
                return;
            }
            sendJson(res, 200, {{"ok", true}});
        });
    }
}

Json nullable(const std::string& value) {
    return value.empty() ? Json(nullptr) : Json(value);
}

using InfoPtr = MetadataService::InfoPtr;

Json tmdbJson(const InfoPtr& info) {
    if (!info) {
        return nullptr;
    }
    return {{"id", info->providerId},
            {"type", info->mediaType},
            {"title", info->title},
            {"original_title", info->originalTitle},
            {"year", info->year ? Json(*info->year) : Json(nullptr)},
            {"genres", info->genres}};
}

Json followJson(const DbManager::Follow& follow) {
    return {{"id", follow.id}, {"max_quality", follow.maxQuality}, {"created_at", follow.createdAt}};
}

// Seguimiento de cada obra del catálogo, por su identificador
using FollowIndex = std::map<std::pair<std::int64_t, std::int64_t>, DbManager::Follow>;

FollowIndex followIndex(DbManager& db, const std::vector<Catalog::ItemPtr>& items, const MetadataService& metadata) {
    FollowIndex index;
    for (const tracking::FollowMatch& match : tracking::matchFollows(items, db.listFollows(), metadata)) {
        if (match.item) {
            index.emplace(std::make_pair(match.item->chatId, match.item->anchorMessageId), match.follow);
        }
    }
    return index;
}

const DbManager::Follow* findFollow(const FollowIndex& index, const Catalog::Item& item) {
    const auto it = index.find({item.chatId, item.anchorMessageId});
    return it != index.end() ? &it->second : nullptr;
}

Json itemSummaryJson(const Catalog::Item& item, const InfoPtr& info, const DbManager::Follow* follow) {
    const bool hdr = std::any_of(item.releases.begin(), item.releases.end(),
                                 [](const Catalog::Release& release) { return release.hdr; });
    const std::optional<int> year = item.year ? item.year : (info ? info->year : std::nullopt);
    return {{"chat_id", item.chatId},
            {"anchor_id", item.anchorMessageId},
            {"kind", item.kind},
            {"title", item.title},
            {"alternate_titles", item.alternateTitles},
            {"year", year ? Json(*year) : Json(nullptr)},
            {"tmdb", tmdbJson(info)},
            {"qualities", item.qualities},
            {"hdr", hdr},
            {"languages", item.languages},
            // Los géneros de TMDB están normalizados en castellano; si no hay, los de la ficha
            {"genres", info && !info->genres.empty() ? info->genres : item.genres},
            {"topics", item.topics},
            {"airing", item.airing},
            {"channel_title", item.channelTitle},
            {"has_poster", item.posterMessageId != 0 || (info && !info->posterPath.empty())},
            {"seasons", item.seasonCount},
            {"episodes", item.episodeCount},
            {"release_count", item.releases.size()},
            {"total_size", item.totalSize},
            {"updated_at", item.updatedAt},
            {"followed", follow != nullptr}};
}

// Series: episodios conocidos, cuántos se tienen y qué falta por cada calidad máxima (D-040)
Json seriesLibraryJson(const Catalog::Item& item, const std::vector<tracking::Owned>& owned) {
    const tracking::EpisodeCount count = tracking::countEpisodes(item, owned);
    Json missing = Json::object();
    for (const std::string& maxQuality : kMaxQualities) {
        int episodes = 0;
        std::int64_t size = 0;
        const std::vector<std::size_t> chosen = tracking::missingEpisodes(item, owned, maxQuality);
        for (const std::size_t index : chosen) {
            const Catalog::Release& release = item.releases[index];
            episodes += std::max(release.episode, release.episodeEnd) - release.episode + 1;
            size += release.size;
        }
        missing[maxQuality] = {{"episodes", episodes}, {"files", chosen.size()}, {"size", size}};
    }
    return {{"episodes", count.known}, {"owned", count.owned}, {"missing", missing}};
}

using Probes = tracking::Probes;

// Calidad comprobada de un archivo (D-042): {"quality", "hdr"}, {"error"} o null si no se ha mirado
Json probeJson(const Probes& probes, const Catalog::Release& release) {
    const auto it = probes.find({release.chatId, release.parts.front().messageId});
    if (it == probes.end()) {
        return nullptr;
    }
    if (!it->second.error.empty()) {
        return {{"error", it->second.error}};
    }
    return {{"quality", it->second.quality}, {"hdr", it->second.hdr}};
}

Json itemDetailJson(const Catalog::Item& item, const InfoPtr& info, const DbManager::Follow* follow,
                    const std::vector<tracking::Owned>& owned, const Probes& probes) {
    Json detail = itemSummaryJson(item, info, follow);
    detail["follow"] = follow ? followJson(*follow) : Json(nullptr);
    detail["library"] = item.kind == "series" ? seriesLibraryJson(item, owned) : Json(nullptr);
    detail["synopsis"] = item.synopsis;           // De la ficha de Telegram
    detail["overview"] = info ? info->overview : "";  // De TMDB
    detail["description"] = item.description;
    detail["external_ids"] = info ? Json{{"tmdb", info->providerId},
                                         {"imdb", nullable(info->imdbId)},
                                         {"tvdb", info->tvdbId ? Json(info->tvdbId) : Json(nullptr)},
                                         {"wikidata", nullable(info->wikidataId)}}
                                  : Json(nullptr);
    std::map<std::pair<int, int>, const DbManager::MetadataEpisode*> episodes;
    if (info) {
        for (const DbManager::MetadataEpisode& episode : info->episodes) {
            episodes[{episode.season, episode.episode}] = &episode;
        }
    }
    Json releases = Json::array();
    for (const Catalog::Release& release : item.releases) {
        Json parts = Json::array();
        for (const Catalog::Part& part : release.parts) {
            parts.push_back({{"message_id", part.messageId},
                             {"file_name", part.fileName},
                             {"size", part.size},
                             {"number", part.number}});
        }
        const bool isEpisode = release.episode > 0;
        // Título del episodio: el de TMDB en castellano; si no hay, el del nombre del fichero
        const auto tmdbEpisode = episodes.find({release.season, release.episode});
        const DbManager::MetadataEpisode* episode = tmdbEpisode != episodes.end() ? tmdbEpisode->second : nullptr;
        const std::string episodeTitle = episode && !episode->name.empty() ? episode->name : release.episodeTitle;
        releases.push_back({{"chat_id", release.chatId},
                            {"message_id", release.parts.front().messageId},
                            {"name", release.name},
                            {"quality", nullable(release.quality)},
                            {"hdr", release.hdr},
                            {"tags", release.tags},
                            {"size", release.size},
                            {"archive", release.archive},
                            {"season", isEpisode ? Json(release.season) : Json(nullptr)},
                            {"episode", isEpisode ? Json(release.episode) : Json(nullptr)},
                            {"episode_end", release.episodeEnd > 0 ? Json(release.episodeEnd) : Json(nullptr)},
                            {"episode_title", nullable(episodeTitle)},
                            {"episode_overview", episode ? nullable(episode->overview) : Json(nullptr)},
                            {"air_date", episode ? nullable(episode->airDate) : Json(nullptr)},
                            {"date", release.date},
                            {"topic_id", release.topicId},
                            {"probe", probeJson(probes, release)},
                            {"parts", parts}});
    }
    detail["releases"] = releases;
    return detail;
}

// Elige el tamaño de foto más pequeño con al menos kPosterMinWidth de ancho (o el mayor si no hay)
const Json* choosePhotoSize(const Json& photo) {
    const auto sizes = photo.find("sizes");
    if (sizes == photo.end() || !sizes->is_array() || sizes->empty()) {
        return nullptr;
    }
    const Json* best = nullptr;
    for (const Json& size : *sizes) {
        const int width = size.value("width", 0);
        if (!best) {
            best = &size;
            continue;
        }
        const int bestWidth = best->value("width", 0);
        const bool fits = width >= kPosterMinWidth;
        const bool bestFits = bestWidth >= kPosterMinWidth;
        if ((fits && (!bestFits || width < bestWidth)) || (!fits && !bestFits && width > bestWidth)) {
            best = &size;
        }
    }
    return best;
}

void sendFile(httplib::Response& res, const std::string& path, const char* contentType) {
    std::ifstream in(path, std::ios::binary);
    if (path.empty() || !in) {
        sendError(res, 502, "La imagen no está disponible");
        return;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.set_header("Cache-Control", kPosterCacheControl);
    res.set_content(data, contentType);
}

void registerCatalogRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram, Catalog& catalog,
                           MetadataService& metadata, TmdbClient& tmdb) {
    server.Get("/api/catalog", [&db, &catalog, &metadata](const httplib::Request&, httplib::Response& res) {
        const std::vector<Catalog::ItemPtr> items = catalog.items();
        const FollowIndex follows = followIndex(db, items, metadata);
        Json result = Json::array();
        for (const Catalog::ItemPtr& item : items) {
            result.push_back(itemSummaryJson(*item, metadata.lookup(*item), findFollow(follows, *item)));
        }
        sendJson(res, 200, result);
    });

    server.Get(R"(/api/catalog/(-?\d+)/(\d+))", [&db, &catalog, &metadata](const httplib::Request& req,
                                                                        httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        const auto anchorId = parseId(req.matches[2].str());
        const auto item = (chatId && anchorId) ? catalog.find(*chatId, *anchorId) : nullptr;
        if (!item) {
            sendError(res, 404, "Elemento no encontrado en el catálogo");
            return;
        }
        const FollowIndex follows = followIndex(db, catalog.items(), metadata);
        // Lo que se tiene: descargas y vídeos que ya están en la biblioteca (D-041)
        const AppSettings settings = loadSettings(db);
        const std::vector<library::DiskVideo> disk = library::videosOnDisk(
            item->kind == "series" ? settings.seriesDir : settings.moviesDir, tracking::workInfo(*item, metadata));
        const std::vector<tracking::Owned> owned =
            tracking::ownedVersions(*item, tracking::ownedByItem(catalog, db.listDownloads()), disk);
        const tracking::Probes probes = db.listProbes();
        const Catalog::Item effective = tracking::applyProbes(*item, probes);
        Json detail = itemDetailJson(*item, metadata.lookup(*item), findFollow(follows, *item), owned, probes);
        if (item->kind == "series") {
            detail["library"] = seriesLibraryJson(effective, owned);  // Lo que falta, con la calidad comprobada
        }
        Json onDisk = Json::array();
        for (const library::DiskVideo& video : disk) {
            onDisk.push_back({{"file", std::filesystem::path(video.path).filename().string()},
                              {"season", video.episode > 0 ? Json(video.season) : Json(nullptr)},
                              {"episode", video.episode > 0 ? Json(video.episode) : Json(nullptr)},
                              {"episode_end", video.episodeEnd > video.episode ? Json(video.episodeEnd) : Json(nullptr)},
                              {"quality", nullable(video.quality)},
                              {"hdr", video.hdr}});
        }
        detail["on_disk"] = onDisk;
        sendJson(res, 200, detail);
    });

    // Portada: la foto de la ficha (TDLib la descarga la primera vez y la guarda en su caché). Si la
    // obra no tiene ficha con foto, la carátula de TMDB, guardada en disco (D-030).
    server.Get(R"(/api/catalog/(-?\d+)/(\d+)/poster)", [&catalog, &telegram, &metadata, &tmdb](
                                                             const httplib::Request& req, httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        const auto anchorId = parseId(req.matches[2].str());
        const auto item = (chatId && anchorId) ? catalog.find(*chatId, *anchorId) : nullptr;
        if (!item) {
            sendError(res, 404, "Elemento no encontrado en el catálogo");
            return;
        }
        if (item->posterMessageId == 0) {
            const auto info = metadata.lookup(*item);
            const auto path = info && !info->posterPath.empty() ? tmdb.image("w500", info->posterPath) : std::nullopt;
            if (!path) {
                sendError(res, 404, "Este elemento no tiene portada");
                return;
            }
            sendFile(res, *path, path->size() > 4 && path->compare(path->size() - 4, 4, ".png") == 0 ? "image/png"
                                                                                                      : "image/jpeg");
            return;
        }
        if (!requireTelegramReady(telegram, res)) {
            return;
        }

        const auto message = telegram.request(
            {{"@type", "getMessage"}, {"chat_id", item->posterChatId}, {"message_id", item->posterMessageId}},
            kTelegramTimeout);
        if (isError(message)) {
            sendError(res, 502, "No se pudo leer la ficha: " + errorMessage(message));
            return;
        }
        // photo debe vivir mientras se use size, que apunta dentro de él
        const Json content = message->value("content", Json::object());
        const Json photo = content.value("photo", Json::object());
        const Json* size = typeOf(content) == "messagePhoto" ? choosePhotoSize(photo) : nullptr;
        if (!size || !size->contains("photo")) {
            sendError(res, 404, "La ficha no tiene foto");
            return;
        }

        Json file = size->at("photo");
        const Json local = file.value("local", Json::object());
        if (!local.value("is_downloading_completed", false)) {
            const auto downloaded = telegram.request({{"@type", "downloadFile"},
                                                      {"file_id", file.value("id", 0)},
                                                      {"priority", 16},
                                                      {"offset", 0},
                                                      {"limit", 0},
                                                      {"synchronous", true}},
                                                     kTelegramTimeout);
            if (isError(downloaded)) {
                sendError(res, 502, "No se pudo descargar la portada: " + errorMessage(downloaded));
                return;
            }
            file = *downloaded;
        }

        // Telegram guarda las fotos en JPEG
        sendFile(res, file.value("local", Json::object()).value("path", ""), "image/jpeg");
    });
}

Json downloadJson(const DbManager::Download& d, const std::optional<DownloadManager::Progress>& live) {
    return {{"id", d.id},
            {"chat_id", d.chatId},
            {"message_id", d.messageId},
            {"title", d.title},
            {"kind", d.kind},
            {"season", d.season > 0 ? Json(d.season) : Json(nullptr)},
            {"episode", d.episode > 0 ? Json(d.episode) : Json(nullptr)},
            {"episode_end", d.episodeEnd > 0 ? Json(d.episodeEnd) : Json(nullptr)},
            {"name", d.name},
            {"quality", nullable(d.quality)},
            {"hdr", d.hdr},
            {"tags", d.tags},
            {"archive", d.archive},
            {"total_size", d.totalSize},
            {"downloaded_size", live ? live->downloaded : d.downloadedSize},
            {"bytes_per_second", live ? live->bytesPerSecond : 0.0},
            {"import_percent", live && live->importPercent >= 0 ? Json(live->importPercent) : Json(nullptr)},
            {"status", d.status},
            {"error", nullable(d.error)},
            {"library_path", nullable(d.libraryPath)},
            {"origin", d.origin},
            {"follow_id", d.followId ? Json(*d.followId) : Json(nullptr)},
            {"replaces", d.replaces},
            {"created_at", d.createdAt},
            {"updated_at", d.updatedAt}};
}

void registerProbeRoutes(httplib::Server& server, Catalog& catalog, ReleaseProber& prober) {
    // Calidad real de un archivo del catálogo sin descargarlo entero (D-042): {"quality", "hdr"}
    server.Post(R"(/api/releases/(-?\d+)/(\d+)/probe)", [&catalog, &prober](const httplib::Request& req,
                                                                         httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        const auto messageId = parseId(req.matches[2].str());
        const auto ref = (chatId && messageId) ? catalog.findRelease(*chatId, *messageId) : std::nullopt;
        if (!ref) {
            sendError(res, 404, "Ese archivo no está en el catálogo");
            return;
        }
        const ReleaseProber::Result result = prober.probe(*ref->release);
        if (!result.ok) {
            sendError(res, 409, result.error);
            return;
        }
        sendJson(res, 200, {{"quality", result.info.quality}, {"hdr", result.info.hdr.value_or(false)}});
    });
}

void registerDownloadRoutes(httplib::Server& server, DbManager& db, Catalog& catalog, DownloadManager& downloads) {
    server.Get("/api/downloads", [&db, &downloads](const httplib::Request&, httplib::Response& res) {
        Json result = Json::array();
        for (const DbManager::Download& download : db.listDownloads()) {
            result.push_back(downloadJson(download, downloads.liveProgress(download.id)));
        }
        sendJson(res, 200, result);
    });

    // Encolar un archivo lógico del catálogo: {"chat_id": -100..., "message_id": ...} (cualquiera de
    // sus partes). Los datos de la descarga salen del catálogo, no de lo que envíe el navegador.
    server.Post("/api/downloads", [&db, &catalog, &downloads](const httplib::Request& req, httplib::Response& res) {
        const Json body = Json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("chat_id") || !body["chat_id"].is_number_integer() ||
            !body.contains("message_id") || !body["message_id"].is_number_integer()) {
            sendError(res, 400, "El cuerpo debe ser JSON con los campos numéricos 'chat_id' y 'message_id'");
            return;
        }
        const auto ref = catalog.findRelease(body["chat_id"].get<std::int64_t>(), body["message_id"].get<std::int64_t>());
        if (!ref) {
            sendError(res, 404, "Ese archivo no está en el catálogo");
            return;
        }
        // Con la calidad comprobada del archivo, si se ha mirado (D-042)
        const Catalog::Item effective = tracking::applyProbes(*ref->item, db.listProbes());
        const auto index = static_cast<std::size_t>(ref->release - ref->item->releases.data());
        const DbManager::AddDownloadResult added = db.addDownload(makeDownload(effective, effective.releases[index]));
        if (added.duplicate) {
            sendJson(res, 409, {{"error", "Ya está en la cola, descargándose o descargado"}, {"id", added.id}});
            return;
        }
        if (!added.ok) {
            sendError(res, 500, "No se pudo añadir a la cola");
            return;
        }
        downloads.wake();
        sendJson(res, 201, {{"id", added.id}});
    });

    server.Post(R"(/api/downloads/(\d+)/cancel)", [&db, &downloads](const httplib::Request& req, httplib::Response& res) {
        const auto id = parseId(req.matches[1].str());
        const auto download = id ? db.getDownload(*id) : std::nullopt;
        if (!download) {
            sendError(res, 404, "Descarga no encontrada");
            return;
        }
        if (download->status == "queued") {
            db.setDownloadStatus(*id, "cancelled");
        } else if ((download->status != "downloading" && download->status != "importing") || !downloads.cancelActive(*id)) {
            sendError(res, 409, "Solo se pueden cancelar descargas en cola o en curso");
            return;
        }
        sendJson(res, 202, {{"ok", true}});
    });

    server.Post(R"(/api/downloads/(\d+)/retry)", [&db, &downloads](const httplib::Request& req, httplib::Response& res) {
        const auto id = parseId(req.matches[1].str());
        const auto download = id ? db.getDownload(*id) : std::nullopt;
        if (!download) {
            sendError(res, 404, "Descarga no encontrada");
            return;
        }
        if (download->status != "failed" && download->status != "cancelled") {
            sendError(res, 409, "Solo se pueden reintentar descargas fallidas o canceladas");
            return;
        }
        db.setDownloadStatus(*id, "queued");
        downloads.wake();
        sendJson(res, 202, {{"ok", true}});
    });

    // Quitar del historial. Los archivos de una descarga terminada se quedan en la caché de TDLib.
    server.Delete(R"(/api/downloads/(\d+))", [&db](const httplib::Request& req, httplib::Response& res) {
        const auto id = parseId(req.matches[1].str());
        const auto download = id ? db.getDownload(*id) : std::nullopt;
        if (!download) {
            sendError(res, 404, "Descarga no encontrada");
            return;
        }
        if (download->status == "queued" || download->status == "downloading" || download->status == "importing") {
            sendError(res, 409, "Cancélala antes de quitarla de la lista");
            return;
        }
        db.deleteDownload(*id);
        sendJson(res, 200, {{"ok", true}});
    });
}

void registerChannelRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram, ChannelSync& sync,
                           Catalog& catalog) {
    // Canales y grupos de la cuenta (lista principal y archivados), para elegir cuáles vigilar
    server.Get("/api/telegram/chats", [&db, &telegram](const httplib::Request&, httplib::Response& res) {
        if (!requireTelegramReady(telegram, res)) {
            return;
        }

        std::set<std::int64_t> watched;
        for (const DbManager::Channel& channel : db.listChannels()) {
            watched.insert(channel.id);
        }

        std::vector<std::int64_t> chatIds;
        std::set<std::int64_t> seen;
        for (const char* list : {"chatListMain", "chatListArchive"}) {
            const auto chats = telegram.request(
                {{"@type", "getChats"}, {"chat_list", {{"@type", list}}}, {"limit", 1000}}, kTelegramTimeout);
            if (isError(chats)) {
                sendError(res, 502, "No se pudo obtener la lista de chats: " + errorMessage(chats));
                return;
            }
            for (const Json& id : chats->value("chat_ids", Json::array())) {
                if (id.is_number_integer() && seen.insert(id.get<std::int64_t>()).second) {
                    chatIds.push_back(id.get<std::int64_t>());
                }
            }
        }

        Json result = Json::array();
        for (const std::int64_t chatId : chatIds) {
            const auto chat = telegram.request({{"@type", "getChat"}, {"chat_id", chatId}}, kTelegramTimeout);
            if (isError(chat)) {
                continue;
            }
            const ChatKind kind = classifyChat(telegram, *chat);
            if (kind.type.empty()) {
                continue;  // Chats privados, bots, etc.
            }
            result.push_back({{"id", chatId},
                              {"title", chat->value("title", "")},
                              {"type", kind.type},
                              {"role", kind.role},
                              {"watched", watched.count(chatId) > 0}});
        }
        sendJson(res, 200, result);
    });

    server.Get("/api/channels", [&db, &sync](const httplib::Request&, httplib::Response& res) {
        Json result = Json::array();
        const std::int64_t syncing = sync.syncingChatId();
        for (const DbManager::Channel& channel : db.listChannels()) {
            result.push_back(channelJson(channel, syncing));
        }
        sendJson(res, 200, result);
    });

    // Añadir un canal o grupo a la lista de vigilados: {"chat_id": -100...}
    server.Post("/api/channels", [&db, &telegram, &sync](const httplib::Request& req, httplib::Response& res) {
        if (!requireTelegramReady(telegram, res)) {
            return;
        }
        const Json body = Json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("chat_id") || !body["chat_id"].is_number_integer()) {
            sendError(res, 400, "El cuerpo debe ser JSON con el campo numérico 'chat_id'");
            return;
        }
        const std::int64_t chatId = body["chat_id"].get<std::int64_t>();

        const auto chat = telegram.request({{"@type", "getChat"}, {"chat_id", chatId}}, kTelegramTimeout);
        if (isError(chat)) {
            sendError(res, 404, "Chat no encontrado: " + errorMessage(chat));
            return;
        }
        if (classifyChat(telegram, *chat).type.empty()) {
            sendError(res, 400, "Solo se pueden vigilar canales y grupos");
            return;
        }
        if (!db.addChannel(chatId, chat->value("title", ""))) {
            sendError(res, 409, "El canal ya está en la lista");
            return;
        }
        sync.requestSync();

        const auto channel = db.getChannel(chatId);
        sendJson(res, 201, channel ? channelJson(*channel, sync.syncingChatId()) : Json{{"id", chatId}});
    });

    server.Delete(R"(/api/channels/(-?\d+))", [&db, &catalog, &sync](const httplib::Request& req, httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        if (!chatId || !db.removeChannel(*chatId)) {
            sendError(res, 404, "Canal no encontrado");
            return;
        }
        catalog.removeChannel(*chatId);
        sync.requestSync();  // Deja de recibir sus mensajes al momento (closeChat)
        sendJson(res, 200, {{"ok", true}});
    });

    // Buscar mensajes nuevos ya, sin esperar a la siguiente ronda periódica
    server.Post(R"(/api/channels/(-?\d+)/sync)", [&db, &sync](const httplib::Request& req, httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        if (!chatId || !db.getChannel(*chatId)) {
            sendError(res, 404, "Canal no encontrado");
            return;
        }
        sync.requestSync();
        sendJson(res, 202, {{"ok", true}});
    });

    // Temas de un grupo con temas (foro), con cuántos mensajes guardados tiene cada uno
    server.Get(R"(/api/channels/(-?\d+)/topics)", [&db](const httplib::Request& req, httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        if (!chatId || !db.getChannel(*chatId)) {
            sendError(res, 404, "Canal no encontrado");
            return;
        }
        Json result = Json::array();
        for (const DbManager::Topic& topic : db.listTopics(*chatId)) {
            result.push_back({{"id", topic.id}, {"name", topic.name}, {"message_count", topic.messageCount}});
        }
        sendJson(res, 200, result);
    });

    // Mensajes guardados de un canal, del más reciente al más antiguo (?limit=50&offset=0)
    server.Get(R"(/api/channels/(-?\d+)/messages)", [&db](const httplib::Request& req, httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        if (!chatId || !db.getChannel(*chatId)) {
            sendError(res, 404, "Canal no encontrado");
            return;
        }
        const auto limit = parseId(req.has_param("limit") ? req.get_param_value("limit") : "50");
        const auto offset = parseId(req.has_param("offset") ? req.get_param_value("offset") : "0");
        if (!limit || !offset || *limit < 1 || *offset < 0) {
            sendError(res, 400, "Parámetros 'limit' u 'offset' no válidos");
            return;
        }

        Json result = Json::array();
        const int pageSize = static_cast<int>(std::min<std::int64_t>(*limit, kMaxMessagesPerPage));
        const int pageOffset = static_cast<int>(std::min<std::int64_t>(*offset, INT32_MAX));
        for (const DbManager::Message& message : db.listMessages(*chatId, pageSize, pageOffset)) {
            result.push_back(messageJson(message));
        }
        sendJson(res, 200, result);
    });
}

const char* qualityText(const std::string& maxQuality) {
    return maxQuality.empty() ? "la mejor" : maxQuality == "1080p" ? "hasta 1080p" : "hasta 720p";
}

// Calidad máxima de un cuerpo JSON: std::nullopt (y error 400) si no es válida
std::optional<std::string> maxQualityFrom(const Json& body, httplib::Response& res) {
    const Json value = body.value("max_quality", Json(""));
    if (!value.is_string() || !kMaxQualities.count(value.get<std::string>())) {
        sendError(res, 400, "'max_quality' debe ser \"\", \"1080p\" o \"720p\"");
        return std::nullopt;
    }
    return value.get<std::string>();
}

std::string formatGigabytes(std::int64_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / 1e9);
    std::string text = buffer;
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

void registerFollowRoutes(httplib::Server& server, DbManager& db, Catalog& catalog, MetadataService& metadata,
                          Tracker& tracker, DownloadManager& downloads) {
    // Descargar los episodios que faltan de una serie (o la serie completa si no se tiene ninguno):
    // {"max_quality": ""}. La mejor versión de cada episodio que no está ya en cola ni descargado (D-040).
    server.Post(R"(/api/catalog/(-?\d+)/(\d+)/download)", [&db, &catalog, &metadata, &downloads](
                                                                  const httplib::Request& req, httplib::Response& res) {
        const Json body = req.body.empty() ? Json::object() : Json::parse(req.body, nullptr, false);
        if (!body.is_object()) {
            sendError(res, 400, "El cuerpo debe ser un objeto JSON");
            return;
        }
        const auto maxQuality = maxQualityFrom(body, res);
        if (!maxQuality) {
            return;
        }
        const auto chatId = parseId(req.matches[1].str());
        const auto anchorId = parseId(req.matches[2].str());
        const auto item = (chatId && anchorId) ? catalog.find(*chatId, *anchorId) : nullptr;
        if (!item || item->kind != "series") {
            sendError(res, 404, "Serie no encontrada en el catálogo");
            return;
        }
        const AppSettings settings = loadSettings(db);
        const std::vector<tracking::Owned> owned = tracking::ownedVersions(
            *item, tracking::ownedByItem(catalog, db.listDownloads()),
            library::videosOnDisk(settings.seriesDir, tracking::workInfo(*item, metadata)));
        // Con la calidad comprobada de cada archivo, si se ha mirado (D-042)
        const Catalog::Item effective = tracking::applyProbes(*item, db.listProbes());
        const std::vector<std::size_t> chosen = tracking::missingEpisodes(effective, owned, *maxQuality);
        std::int64_t total = 0;
        for (const std::size_t index : chosen) {
            total += effective.releases[index].size;
        }
        if (chosen.empty()) {
            sendJson(res, 200, {{"queued", 0}, {"size", 0}});
            return;
        }

        // Que quepa en la biblioteca de series con el margen de los ajustes (se importan de una en una)
        if (settings.seriesDir.empty()) {
            sendError(res, 409, "Define la biblioteca de series en Ajustes");
            return;
        }
        const PathCheck library = checkPath(settings.seriesDir);
        if (library.ok && library.freeBytes < total + settings.minFreeBytes) {
            sendError(res, 409, "No cabe en la biblioteca de series: hacen falta " + formatGigabytes(total) + " (más " +
                                    formatGigabytes(settings.minFreeBytes) + " de margen) y quedan " +
                                    formatGigabytes(library.freeBytes));
            return;
        }

        int queued = 0;
        std::int64_t size = 0;
        for (const std::size_t index : chosen) {
            const Catalog::Release& release = effective.releases[index];
            if (db.addDownload(makeDownload(effective, release)).ok) {
                ++queued;
                size += release.size;
            }
        }
        downloads.wake();
        sendJson(res, 201, {{"queued", queued}, {"size", size}});
    });

    // Obras seguidas, con la obra del catálogo que les corresponde ahora
    server.Get("/api/follows", [&db, &catalog, &metadata](const httplib::Request&, httplib::Response& res) {
        Json result = Json::array();
        for (const tracking::FollowMatch& match : tracking::matchFollows(catalog.items(), db.listFollows(), metadata)) {
            const DbManager::Follow& follow = match.follow;
            Json entry = followJson(follow);
            entry["kind"] = follow.kind;
            entry["title"] = match.item ? match.item->title : follow.title;
            entry["year"] = follow.year ? Json(*follow.year) : Json(nullptr);
            entry["found"] = match.item != nullptr;
            entry["chat_id"] = match.item ? match.item->chatId : follow.chatId;
            entry["anchor_id"] = match.item ? match.item->anchorMessageId : follow.anchorId;
            entry["airing"] = match.item && match.item->airing;
            result.push_back(entry);
        }
        sendJson(res, 200, result);
    });

    // Seguir una obra: {"chat_id": -100..., "anchor_id": ..., "max_quality": "1080p"}
    server.Post("/api/follows", [&db, &catalog, &metadata, &tracker](const httplib::Request& req, httplib::Response& res) {
        const Json body = Json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("chat_id") || !body["chat_id"].is_number_integer() ||
            !body.contains("anchor_id") || !body["anchor_id"].is_number_integer()) {
            sendError(res, 400, "El cuerpo debe ser JSON con los campos numéricos 'chat_id' y 'anchor_id'");
            return;
        }
        const auto maxQuality = maxQualityFrom(body, res);
        if (!maxQuality) {
            return;
        }
        const auto item = catalog.find(body["chat_id"].get<std::int64_t>(), body["anchor_id"].get<std::int64_t>());
        if (!item) {
            sendError(res, 404, "Elemento no encontrado en el catálogo");
            return;
        }
        const FollowIndex follows = followIndex(db, catalog.items(), metadata);
        if (const DbManager::Follow* existing = findFollow(follows, *item)) {
            sendJson(res, 409, {{"error", "Ya sigues esta obra"}, {"id", existing->id}});
            return;
        }

        DbManager::Follow follow = tracking::followFor(*item, metadata);
        follow.maxQuality = *maxQuality;
        const auto id = db.addFollow(follow);
        if (!id) {
            sendError(res, 500, "No se pudo guardar el seguimiento");
            return;
        }
        const auto saved = db.getFollow(*id);
        const std::string what = item->kind == "series" ? "se descargarán solos los episodios nuevos"
                                                        : "se descargará sola si se publica una versión nueva o mejor";
        db.addActivity({0, 0, "follow",
                        "Sigues «" + item->title + "» (calidad: " + qualityText(*maxQuality) + "): " + what + ".",
                        item->chatId, item->anchorMessageId, *id, std::nullopt});
        tracker.requestRun();
        sendJson(res, 201, saved ? followJson(*saved) : Json{{"id", *id}});
    });

    // Cambiar la calidad máxima: {"max_quality": "1080p"}
    server.Put(R"(/api/follows/(\d+))", [&db, &tracker](const httplib::Request& req, httplib::Response& res) {
        const auto id = parseId(req.matches[1].str());
        const Json body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) {
            sendError(res, 400, "El cuerpo debe ser un objeto JSON");
            return;
        }
        const auto maxQuality = maxQualityFrom(body, res);
        if (!maxQuality) {
            return;
        }
        if (!id || !db.updateFollowQuality(*id, *maxQuality)) {
            sendError(res, 404, "Seguimiento no encontrado");
            return;
        }
        tracker.requestRun();
        const auto follow = db.getFollow(*id);
        sendJson(res, 200, follow ? followJson(*follow) : Json{{"id", *id}});
    });

    // Dejar de seguir. Lo descargado y lo que esté en la cola se queda.
    server.Delete(R"(/api/follows/(\d+))", [&db](const httplib::Request& req, httplib::Response& res) {
        const auto id = parseId(req.matches[1].str());
        const auto follow = id ? db.getFollow(*id) : std::nullopt;
        if (!follow || !db.deleteFollow(*id)) {
            sendError(res, 404, "Seguimiento no encontrado");
            return;
        }
        db.addActivity({0, 0, "unfollow", "Dejas de seguir «" + follow->title + "».", follow->chatId, follow->anchorId,
                        follow->id, std::nullopt});
        sendJson(res, 200, {{"ok", true}});
    });

    // Historial de actividad, del más reciente al más antiguo: ?before=<id> para la página siguiente
    server.Get("/api/activity", [&db, &catalog](const httplib::Request& req, httplib::Response& res) {
        const auto before = parseId(req.has_param("before") ? req.get_param_value("before") : "0");
        if (!before || *before < 0) {
            sendError(res, 400, "Parámetro 'before' no válido");
            return;
        }
        Json result = Json::array();
        for (const DbManager::Activity& entry : db.listActivity(kActivityPageSize, *before)) {
            // Enlace a la obra: el mensaje es un archivo (descargas) o una ficha (seguimiento)
            Catalog::ItemPtr item;
            if (entry.chatId != 0) {
                if (const auto ref = catalog.findRelease(entry.chatId, entry.messageId)) {
                    item = ref->item;
                } else {
                    item = catalog.find(entry.chatId, entry.messageId);
                }
            }
            result.push_back({{"id", entry.id},
                              {"at", entry.at},
                              {"type", entry.type},
                              {"message", entry.message},
                              {"item", item ? Json{{"chat_id", item->chatId}, {"anchor_id", item->anchorMessageId}}
                                            : Json(nullptr)},
                              {"follow_id", entry.followId ? Json(*entry.followId) : Json(nullptr)},
                              {"download_id", entry.downloadId ? Json(*entry.downloadId) : Json(nullptr)}});
        }
        sendJson(res, 200, result);
    });
}

}  // namespace

// Ruta sin barras finales ni "." o ".." intermedios ("/srv/media/" -> "/srv/media")
std::string normalizePath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    std::string normal = std::filesystem::path(path).lexically_normal().string();
    while (normal.size() > 1 && normal.back() == '/') {
        normal.pop_back();
    }
    return normal;
}

Json pathCheckJson(const PathCheck& check) {
    return {{"ok", check.ok}, {"error", nullable(check.error)}, {"free_bytes", check.freeBytes}};
}

Json settingsJson(DbManager& db, const ApiServices& services) {
    const AppSettings settings = loadSettings(db);
    const std::string configuredDownloadDir = settings.downloadDir.empty() ? kDefaultDownloadDir : settings.downloadDir;
    const std::string downloadPath = std::filesystem::absolute(configuredDownloadDir).string();
    const PathCheck download = checkPath(downloadPath);

    Json checks = {{"download_dir", pathCheckJson(download)}};
    Json sameFilesystem = nullptr;  // ¿Mover del búfer a la biblioteca será un simple renombrado?
    for (const auto& [field, path] : {std::pair<const char*, std::string>{"movies_dir", settings.moviesDir},
                                      std::pair<const char*, std::string>{"series_dir", settings.seriesDir}}) {
        if (path.empty()) {
            checks[field] = nullptr;
            continue;
        }
        const PathCheck library = checkPath(path);
        checks[field] = pathCheckJson(library);
        // Se prueba a renombrar de verdad (con el aislamiento, mismo disco no basta, D-034)
        const auto same = download.ok && library.ok ? canRename(downloadPath, path) : std::nullopt;
        if (same) {
            sameFilesystem = sameFilesystem.is_null() ? Json(*same) : Json(sameFilesystem.get<bool>() && *same);
        }
    }

    return {{"download_dir", settings.downloadDir},
            {"movies_dir", settings.moviesDir},
            {"series_dir", settings.seriesDir},
            {"min_free_bytes", settings.minFreeBytes},
            {"keep_replaced", settings.keepReplaced},
            {"default_download_dir", kDefaultDownloadDir},
            {"active_download_dir", services.activeDownloadDir},
            {"restart_required", configuredDownloadDir != services.activeDownloadDir && download.ok},
            {"warning", nullable(services.downloadDirWarning)},
            {"checks", checks},
            {"same_filesystem", sameFilesystem}};
}

void registerSettingsRoutes(httplib::Server& server, DbManager& db, const ApiServices& services) {
    // services se copia en las lambdas: sus referencias apuntan a objetos que viven en main()
    server.Get("/api/settings", [&db, services](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, settingsJson(db, services));
    });

    // {"download_dir": "/srv/media/.telegarrm/descargas", "movies_dir": "...", "series_dir": "...",
    //  "min_free_gb": 2, "keep_replaced": false}. Una ruta vacía vuelve al valor predeterminado (o deja la biblioteca sin definir).
    server.Put("/api/settings", [&db, services](const httplib::Request& req, httplib::Response& res) {
        const Json body = Json::parse(req.body, nullptr, false);
        if (!body.is_object()) {
            sendError(res, 400, "El cuerpo debe ser un objeto JSON");
            return;
        }
        AppSettings settings = loadSettings(db);
        Json fieldErrors = Json::object();
        for (const auto& [field, target] : {std::pair<const char*, std::string*>{"download_dir", &settings.downloadDir},
                                            std::pair<const char*, std::string*>{"movies_dir", &settings.moviesDir},
                                            std::pair<const char*, std::string*>{"series_dir", &settings.seriesDir}}) {
            if (!body.contains(field)) {
                continue;
            }
            if (!body[field].is_string()) {
                fieldErrors[field] = "Debe ser texto";
                continue;
            }
            const std::string path = normalizePath(body[field].get<std::string>());
            if (!path.empty()) {
                const PathCheck check = checkPath(path);
                if (!check.ok) {
                    fieldErrors[field] = check.error;
                    continue;
                }
            }
            *target = path;
        }
        if (body.contains("min_free_gb")) {
            const Json& value = body["min_free_gb"];
            if (!value.is_number() || value.get<double>() < 0 || value.get<double>() > 100'000) {
                fieldErrors["min_free_gb"] = "Debe ser un número de GB entre 0 y 100 000";
            } else {
                settings.minFreeBytes = static_cast<std::int64_t>(value.get<double>() * 1e9);
            }
        }
        if (body.contains("keep_replaced")) {
            if (!body["keep_replaced"].is_boolean()) {
                fieldErrors["keep_replaced"] = "Debe ser true o false";
            } else {
                settings.keepReplaced = body["keep_replaced"].get<bool>();
            }
        }
        if (!fieldErrors.empty()) {
            sendJson(res, 400, {{"error", "Hay ajustes que no se pueden usar"}, {"fields", fieldErrors}});
            return;
        }
        if (!saveSettings(db, settings)) {
            sendError(res, 500, "No se pudieron guardar los ajustes");
            return;
        }
        sendJson(res, 200, settingsJson(db, services));
    });

    // Reinicio ordenado (para aplicar el búfer de descargas). systemd vuelve a arrancar el servicio.
    server.Post("/api/restart", [services](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 202, {{"ok", true}});
        services.requestRestart();
    });
}

void registerApiRoutes(httplib::Server& server, const ApiServices& services) {
    registerStatusRoutes(server, services.db, services.telegram, services.metadata);
    registerAuthRoutes(server, services.telegram);
    registerChannelRoutes(server, services.db, services.telegram, services.sync, services.catalog);
    registerCatalogRoutes(server, services.db, services.telegram, services.catalog, services.metadata, services.tmdb);
    registerDownloadRoutes(server, services.db, services.catalog, services.downloads);
    registerProbeRoutes(server, services.catalog, services.prober);
    registerFollowRoutes(server, services.db, services.catalog, services.metadata, services.tracker, services.downloads);
    registerSettingsRoutes(server, services.db, services);
}

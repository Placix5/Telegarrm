#include "api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "catalog.hpp"
#include "channel_sync.hpp"
#include "db_manager.hpp"
#include "httplib.h"
#include "telegram_client.hpp"

namespace {

using Json = nlohmann::json;

// Espera máxima a que TDLib responda a una petición hecha desde la API
constexpr auto kTelegramTimeout = std::chrono::seconds(30);
// Límite de mensajes por página en /api/channels/{id}/messages
constexpr int kMaxMessagesPerPage = 500;
// Las portadas se sirven con la menor resolución que tenga al menos este ancho (px)
constexpr int kPosterMinWidth = 600;
// Las portadas no cambian: el navegador puede guardarlas una semana
constexpr const char* kPosterCacheControl = "max-age=604800";

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
            {"mime_type", message.mimeType ? Json(*message.mimeType) : Json(nullptr)}};
}

void registerStatusRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram) {
    // Estado del servicio, de la BD (lee 'version' de settings) y de la sesión de Telegram
    server.Get("/api/status", [&db, &telegram](const httplib::Request&, httplib::Response& res) {
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

Json itemSummaryJson(const Catalog::Item& item) {
    return {{"chat_id", item.chatId},
            {"anchor_id", item.anchorMessageId},
            {"kind", item.kind},
            {"title", item.title},
            {"year", item.year ? Json(*item.year) : Json(nullptr)},
            {"quality", nullable(item.quality)},
            {"languages", item.languages},
            {"genres", item.genres},
            {"channel_title", item.channelTitle},
            {"has_poster", item.posterMessageId != 0},
            {"seasons", item.seasonCount},
            {"episodes", item.episodeCount},
            {"file_count", item.files.size()},
            {"total_size", item.totalSize}};
}

Json itemDetailJson(const Catalog::Item& item) {
    Json detail = itemSummaryJson(item);
    detail["description"] = item.description;
    Json files = Json::array();
    for (const Catalog::File& file : item.files) {
        files.push_back({{"message_id", file.messageId},
                         {"file_name", file.fileName},
                         {"size", file.size},
                         {"season", file.episode > 0 ? Json(file.season) : Json(nullptr)},
                         {"episode", file.episode > 0 ? Json(file.episode) : Json(nullptr)},
                         {"episode_end", file.episodeEnd > 0 ? Json(file.episodeEnd) : Json(nullptr)},
                         {"episode_title", nullable(file.episodeTitle)},
                         {"quality", nullable(file.quality)},
                         {"archive", file.archive}});
    }
    detail["files"] = files;
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

void registerCatalogRoutes(httplib::Server& server, TelegramClient& telegram, Catalog& catalog) {
    server.Get("/api/catalog", [&catalog](const httplib::Request&, httplib::Response& res) {
        Json result = Json::array();
        for (const Catalog::ItemPtr& item : catalog.items()) {
            result.push_back(itemSummaryJson(*item));
        }
        sendJson(res, 200, result);
    });

    server.Get(R"(/api/catalog/(-?\d+)/(\d+))", [&catalog](const httplib::Request& req, httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        const auto anchorId = parseId(req.matches[2].str());
        const auto item = (chatId && anchorId) ? catalog.find(*chatId, *anchorId) : nullptr;
        if (!item) {
            sendError(res, 404, "Elemento no encontrado en el catálogo");
            return;
        }
        sendJson(res, 200, itemDetailJson(*item));
    });

    // Portada: la foto de la ficha. TDLib la descarga la primera vez y la guarda en su caché.
    server.Get(R"(/api/catalog/(-?\d+)/(\d+)/poster)", [&catalog, &telegram](const httplib::Request& req,
                                                                              httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        const auto anchorId = parseId(req.matches[2].str());
        const auto item = (chatId && anchorId) ? catalog.find(*chatId, *anchorId) : nullptr;
        if (!item || item->posterMessageId == 0) {
            sendError(res, 404, "Este elemento no tiene portada");
            return;
        }
        if (!requireTelegramReady(telegram, res)) {
            return;
        }

        const auto message = telegram.request(
            {{"@type", "getMessage"}, {"chat_id", item->chatId}, {"message_id", item->posterMessageId}}, kTelegramTimeout);
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

        const std::string path = file.value("local", Json::object()).value("path", "");
        std::ifstream in(path, std::ios::binary);
        if (path.empty() || !in) {
            sendError(res, 502, "La portada descargada no está disponible");
            return;
        }
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        res.set_header("Cache-Control", kPosterCacheControl);
        res.set_content(data, "image/jpeg");  // Telegram guarda las fotos en JPEG
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

    server.Delete(R"(/api/channels/(-?\d+))", [&db, &catalog](const httplib::Request& req, httplib::Response& res) {
        const auto chatId = parseId(req.matches[1].str());
        if (!chatId || !db.removeChannel(*chatId)) {
            sendError(res, 404, "Canal no encontrado");
            return;
        }
        catalog.removeChannel(*chatId);
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

}  // namespace

void registerApiRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram, ChannelSync& sync,
                       Catalog& catalog) {
    registerStatusRoutes(server, db, telegram);
    registerAuthRoutes(server, telegram);
    registerChannelRoutes(server, db, telegram, sync, catalog);
    registerCatalogRoutes(server, telegram, catalog);
}

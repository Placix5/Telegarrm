#include "channel_sync.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <utility>

#include "telegram_client.hpp"

namespace {

using Json = TelegramClient::Json;
using Message = DbManager::Message;

// Cada cuánto se buscan mensajes nuevos en todos los canales (recupera lo que no llegue en tiempo real)
constexpr auto kSyncInterval = std::chrono::minutes(15);
// Tiempo real: se sincroniza este tiempo después del último mensaje nuevo...
constexpr auto kRealtimeQuiet = std::chrono::seconds(20);
// ...y como mucho este después del primero, aunque sigan llegando
constexpr auto kRealtimeMaxDelay = std::chrono::minutes(2);
// Reintento mientras la sesión de Telegram no está lista
constexpr auto kNotReadyRetry = std::chrono::seconds(5);
// Pausa entre lotes del historial, para no saturar a Telegram
constexpr auto kBatchDelay = std::chrono::milliseconds(500);
// Mensajes por petición (máximo de TDLib: 100)
constexpr int kBatchSize = 100;
// Acota la espera a TDLib (y por tanto lo que tarda stop()) por debajo del TimeoutStopSec de systemd
constexpr auto kRequestTimeout = std::chrono::seconds(20);
// Frecuencia de los mensajes de progreso en el log
constexpr std::size_t kProgressEvery = 1000;
// Repaso de borrados (D-059): lotes de kBatchSize mensajes por canal en cada ronda (unos 3000 cada 15 min)
constexpr int kAuditBatches = 30;
// Salvaguarda: si en un repaso desaparecen más de tantos y más de esta parte de lo repasado, es más
// probable un fallo que un borrado de verdad, y no se quita nada
constexpr std::size_t kAuditMaxGone = 100;
constexpr double kAuditMaxGoneShare = 0.3;

std::string typeOf(const Json& object) {
    return object.is_object() ? object.value("@type", "") : "";
}

// TDLib codifica los int64 como cadenas en JSON (ej. media_album_id)
std::int64_t int64Field(const Json& object, const char* field) {
    const auto it = object.find(field);
    if (it == object.end()) {
        return 0;
    }
    if (it->is_number_integer()) {
        return it->get<std::int64_t>();
    }
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (const std::exception&) {
            return 0;
        }
    }
    return 0;
}

std::string captionOf(const Json& content) {
    const auto it = content.find("caption");
    return (it != content.end() && it->is_object()) ? it->value("text", "") : "";
}

// Convierte un mensaje de TDLib; std::nullopt si no tiene texto ni fichero que interese
std::optional<Message> toMessage(std::int64_t chatId, const Json& tdMessage) {
    const Json& content = tdMessage.at("content");
    const std::string type = typeOf(content);

    Message message;
    message.chatId = chatId;
    message.messageId = tdMessage.at("id").get<std::int64_t>();
    message.date = tdMessage.value("date", 0);
    message.mediaAlbumId = int64Field(tdMessage, "media_album_id");
    message.contentType = type;
    // En los grupos con temas: {"@type": "messageTopicForum", "forum_topic_id": 5}
    const auto topic = tdMessage.find("topic_id");
    if (topic != tdMessage.end() && typeOf(*topic) == "messageTopicForum") {
        message.topicId = topic->value("forum_topic_id", std::int64_t{0});
    }

    // Tipo de contenido con fichero -> campo que lo contiene (ej. messageVideo -> "video")
    static const std::pair<const char*, const char*> kFileContents[] = {
        {"messageDocument", "document"},
        {"messageVideo", "video"},
        {"messageAudio", "audio"},
        {"messageAnimation", "animation"},
    };

    if (type == "messageText") {
        message.text = content.at("text").value("text", "");
        return message;
    }
    if (type == "messagePhoto") {
        // Carátulas: solo interesa el texto; la imagen se pedirá cuando haga falta
        message.text = captionOf(content);
        return message;
    }
    for (const auto& [contentType, field] : kFileContents) {
        if (type != contentType) {
            continue;
        }
        const Json& media = content.at(field);
        const Json& file = media.at(field);  // ej. content.video.video
        message.text = captionOf(content);
        const std::string fileName = media.value("file_name", "");
        const std::string mimeType = media.value("mime_type", "");
        if (!fileName.empty()) {
            message.fileName = fileName;
        }
        if (!mimeType.empty()) {
            message.mimeType = mimeType;
        }
        const std::int64_t size = file.value("size", std::int64_t{0});
        message.fileSize = size > 0 ? size : file.value("expected_size", std::int64_t{0});
        return message;
    }
    return std::nullopt;
}

// Segundos de espera de un error FLOOD_WAIT ("Too Many Requests: retry after 15")
std::optional<int> floodWaitSeconds(const Json& error) {
    if (error.value("code", 0) != 429) {
        return std::nullopt;
    }
    static const std::regex kRetryAfter(R"(retry after (\d+))");
    std::smatch match;
    const std::string text = error.value("message", "");
    if (std::regex_search(text, match, kRetryAfter)) {
        return std::stoi(match[1].str());
    }
    return 5;
}

}  // namespace

ChannelSync::ChannelSync(DbManager& db, TelegramClient& telegram, ChangeListener onChannelChanged)
    : db_(db), telegram_(telegram), onChannelChanged_(std::move(onChannelChanged)) {
    telegram_.addUpdateListener([this](const Json& update) { onUpdate(update); });
}

ChannelSync::~ChannelSync() {
    stop();
}

void ChannelSync::start() {
    if (worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
    }
    worker_ = std::thread(&ChannelSync::run, this);
}

void ChannelSync::stop() {
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

void ChannelSync::requestSync() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        syncRequested_ = true;
    }
    cv_.notify_all();
}

void ChannelSync::onUpdate(const Json& update) {
    // Borrados en el canal (D-059). from_cache: solo se ha olvidado en TDLib; !is_permanent: el mensaje
    // ha cambiado de identificador (ej. al terminar de enviarse)
    if (typeOf(update) == "updateDeleteMessages") {
        if (!update.value("is_permanent", false) || update.value("from_cache", false)) {
            return;
        }
        const std::int64_t chatId = update.value("chat_id", std::int64_t{0});
        const Json ids = update.value("message_ids", Json::array());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!watched_.count(chatId)) {
                return;
            }
            for (const Json& id : ids) {
                if (id.is_number_integer()) {
                    deleted_[chatId].push_back(id.get<std::int64_t>());
                }
            }
        }
        cv_.notify_all();
        return;
    }
    if (typeOf(update) != "updateNewMessage") {
        return;
    }
    const auto message = update.find("message");
    if (message == update.end() || !message->is_object()) {
        return;
    }
    const std::int64_t chatId = message->value("chat_id", std::int64_t{0});
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!watched_.count(chatId)) {
            return;
        }
        const auto now = Clock::now();
        if (pending_.empty()) {
            firstEventAt_ = now;
        }
        if (pending_.insert(chatId).second) {
            // Una línea por canal y grupo de avisos (una película en cinco partes no deja cinco)
            std::cout << "[Sync] Aviso de mensaje nuevo en el chat " << chatId << ": se leerá en unos segundos"
                      << std::endl;
        }
        lastEventAt_ = now;
    }
    cv_.notify_all();
}

ChannelSync::Clock::time_point ChannelSync::realtimeDeadline() const {
    return std::min(lastEventAt_ + kRealtimeQuiet, firstEventAt_ + kRealtimeMaxDelay);
}

void ChannelSync::updateOpenChats(const std::vector<DbManager::Channel>& channels) {
    std::set<std::int64_t> wanted;
    for (const DbManager::Channel& channel : channels) {
        wanted.insert(channel.id);
    }
    for (auto it = openChats_.begin(); it != openChats_.end();) {
        if (wanted.count(*it)) {
            ++it;
            continue;
        }
        telegram_.send({{"@type", "closeChat"}, {"chat_id", *it}});
        it = openChats_.erase(it);
    }
    for (const std::int64_t chatId : wanted) {
        if (openChats_.insert(chatId).second) {
            // Con el chat abierto, TDLib recibe todas sus actualizaciones (en canales y supergrupos
            // solo las de los chats abiertos). No marca nada como leído.
            telegram_.send({{"@type", "openChat"}, {"chat_id", chatId}}, [chatId](const Json& response) {
                if (typeOf(response) == "error") {
                    std::cerr << "[Sync] No se pudo abrir el chat " << chatId << " para recibir sus mensajes al momento: "
                              << response.value("message", "desconocido") << std::endl;
                }
            });
        }
    }
}

bool ChannelSync::sleepFor(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    return !cv_.wait_for(lock, duration, [this] { return stopRequested_; });
}

bool ChannelSync::stopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopRequested_;
}

bool ChannelSync::telegramReady() const {
    return typeOf(telegram_.authorizationState()) == "authorizationStateReady";
}

void ChannelSync::run() {
    // Primer cálculo del catálogo con los mensajes ya guardados
    for (const DbManager::Channel& channel : db_.listChannels()) {
        if (stopping()) {
            return;
        }
        onChannelChanged_(channel.id);
    }

    auto nextRound = Clock::now();  // Ronda completa: al arrancar y cada kSyncInterval
    for (;;) {
        // Esperar a la parada, a una petición de sincronizar, a la siguiente ronda o a que se
        // asienten los mensajes nuevos recibidos en tiempo real
        bool full = false;
        std::set<std::int64_t> realtime;
        std::map<std::int64_t, std::vector<std::int64_t>> deletions;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopRequested_) {
                    return;
                }
                const auto now = Clock::now();
                if (syncRequested_ || now >= nextRound) {
                    full = true;
                    break;
                }
                if (!pending_.empty() && now >= realtimeDeadline()) {
                    realtime.swap(pending_);
                    break;
                }
                if (!deleted_.empty()) {
                    deletions.swap(deleted_);
                    break;
                }
                cv_.wait_until(lock, pending_.empty() ? nextRound : std::min(nextRound, realtimeDeadline()));
            }
            syncRequested_ = false;
            if (full) {
                pending_.clear();  // La ronda completa los incluye
            }
        }

        applyDeletions(deletions);
        if (!full && realtime.empty()) {
            continue;  // Solo había borrados
        }

        const std::vector<DbManager::Channel> channels = db_.listChannels();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            watched_.clear();
            for (const DbManager::Channel& channel : channels) {
                watched_.insert(channel.id);
            }
        }
        if (!telegramReady()) {
            openChats_.clear();  // Al volver la sesión (quizá un cliente nuevo) hay que abrirlos otra vez
            nextRound = Clock::now() + kNotReadyRetry;
            continue;
        }
        if (!realtime.empty()) {
            std::cout << "[Sync] Mensajes nuevos al momento en " << realtime.size() << " canal(es)" << std::endl;
        }
        for (const DbManager::Channel& channel : channels) {
            if (!full && !realtime.count(channel.id)) {
                continue;
            }
            syncingChatId_ = channel.id;
            Result result = syncChannel(channel, full);
            // En la ronda completa, también un tramo del repaso de borrados (con el historial ya leído)
            if (full && result == Result::Ok && channel.historyComplete) {
                result = auditChannel(channel);
            }
            syncingChatId_ = 0;
            if (result == Result::Stopped) {
                return;
            }
        }
        // Después de getChat (syncChannel): TDLib ya conoce los chats que se abren
        updateOpenChats(channels);
        if (full) {
            nextRound = Clock::now() + kSyncInterval;
        }
    }
}

ChannelSync::Result ChannelSync::fetchHistory(std::int64_t chatId, std::int64_t fromMessageId, HistoryPage& page) {
    for (;;) {
        const auto response = telegram_.request({{"@type", "getChatHistory"},
                                                 {"chat_id", chatId},
                                                 {"from_message_id", fromMessageId},
                                                 {"offset", 0},
                                                 {"limit", kBatchSize},
                                                 {"only_local", false}},
                                                kRequestTimeout);
        if (stopping()) {
            return Result::Stopped;
        }
        if (!response) {
            std::cerr << "[Sync] Telegram no respondió a getChatHistory (canal " << chatId << ")" << std::endl;
            return Result::Failed;
        }
        if (typeOf(*response) == "error") {
            if (const auto wait = floodWaitSeconds(*response)) {
                std::cout << "[Sync] Telegram pide esperar " << *wait << " s (FLOOD_WAIT)" << std::endl;
                if (!sleepFor(std::chrono::seconds(*wait + 1))) {
                    return Result::Stopped;
                }
                continue;
            }
            std::cerr << "[Sync] Error al leer el historial del canal " << chatId << ": "
                      << response->value("message", "desconocido") << std::endl;
            return Result::Failed;
        }

        const auto messages = response->find("messages");
        if (messages == response->end() || !messages->is_array()) {
            std::cerr << "[Sync] Respuesta inesperada de getChatHistory (canal " << chatId << ")" << std::endl;
            return Result::Failed;
        }
        for (const Json& tdMessage : *messages) {
            const auto idField = tdMessage.is_object() ? tdMessage.find("id") : tdMessage.end();
            if (!tdMessage.is_object() || idField == tdMessage.end() || !idField->is_number_integer()) {
                continue;
            }
            const std::int64_t id = idField->get<std::int64_t>();
            // Con offset 0, from_message_id puede venir incluido en la respuesta
            if (fromMessageId != 0 && id >= fromMessageId) {
                continue;
            }
            page.minId = page.total == 0 ? id : std::min(page.minId, id);
            page.maxId = page.total == 0 ? id : std::max(page.maxId, id);
            ++page.total;
            // Un mensaje con una forma inesperada se salta, pero no detiene la sincronización
            try {
                if (auto message = toMessage(chatId, tdMessage)) {
                    page.messages.push_back(std::move(*message));
                }
            } catch (const std::exception& e) {
                std::cerr << "[Sync] Mensaje " << id << " del canal " << chatId << " ignorado: " << e.what()
                          << std::endl;
            }
        }
        return Result::Ok;
    }
}

ChannelSync::Result ChannelSync::syncChannel(const DbManager::Channel& channel, bool full) {
    const std::int64_t chatId = channel.id;

    // getChat asegura que TDLib conoce el chat y permite detectar cambios de título
    const auto chat = telegram_.request({{"@type", "getChat"}, {"chat_id", chatId}}, kRequestTimeout);
    if (stopping()) {
        return Result::Stopped;
    }
    if (!chat || typeOf(*chat) == "error") {
        std::cerr << "[Sync] Canal " << chatId << " no accesible: "
                  << (chat ? chat->value("message", "desconocido") : std::string("sin respuesta")) << std::endl;
        return Result::Failed;
    }
    const std::string title = chat->value("title", "");
    if (!title.empty() && title != channel.title && db_.updateChannelTitle(chatId, title)) {
        onChannelChanged_(chatId);  // El título del canal se usa en el catálogo
    }

    // Grupos con temas (foros): se guarda la lista de temas con sus nombres (en la ronda completa;
    // la sincronización en tiempo real solo trae los mensajes nuevos)
    const Json type = chat->value("type", Json::object());
    if (full && typeOf(type) == "chatTypeSupergroup") {
        const auto group = telegram_.request(
            {{"@type", "getSupergroup"}, {"supergroup_id", type.value("supergroup_id", std::int64_t{0})}}, kRequestTimeout);
        if (group && typeOf(*group) == "supergroup" && group->value("is_forum", false) &&
            syncTopics(chatId) == Result::Stopped) {
            return Result::Stopped;
        }
    }

    DbManager::SyncCursor cursor{channel.newestMessageId, channel.oldestMessageId, channel.historyComplete};

    // 1) Mensajes nuevos: desde el más reciente hacia atrás hasta llegar a lo ya guardado
    if (cursor.newestMessageId != 0) {
        std::vector<Message> fresh;
        std::int64_t from = 0;
        std::int64_t newest = cursor.newestMessageId;
        for (;;) {
            HistoryPage page;
            const Result result = fetchHistory(chatId, from, page);
            if (result != Result::Ok) {
                return result;
            }
            if (page.total == 0) {
                break;
            }
            for (Message& message : page.messages) {
                if (message.messageId > cursor.newestMessageId) {
                    fresh.push_back(std::move(message));
                }
            }
            newest = std::max(newest, page.maxId);
            if (page.minId <= cursor.newestMessageId) {
                break;
            }
            from = page.minId;
            if (!sleepFor(kBatchDelay)) {
                return Result::Stopped;
            }
        }
        cursor.newestMessageId = newest;
        if (!db_.saveSyncBatch(chatId, fresh, cursor)) {
            return Result::Failed;
        }
        if (!fresh.empty()) {
            std::cout << "[Sync] " << title << ": " << fresh.size() << " mensajes nuevos" << std::endl;
            onChannelChanged_(chatId);
        }
    }

    // 2) Historial pendiente: desde el mensaje más antiguo guardado hacia el principio del canal
    std::size_t read = 0;
    if (!cursor.historyComplete) {
        std::cout << "[Sync] " << title << ": leyendo el historial..." << std::endl;
    }
    while (!cursor.historyComplete) {
        HistoryPage page;
        const Result result = fetchHistory(chatId, cursor.oldestMessageId, page);
        if (result != Result::Ok) {
            return result;
        }
        if (page.total == 0) {
            cursor.historyComplete = true;
        } else {
            cursor.oldestMessageId = page.minId;
            cursor.newestMessageId = std::max(cursor.newestMessageId, page.maxId);
        }
        if (!db_.saveSyncBatch(chatId, page.messages, cursor)) {
            return Result::Failed;
        }

        const std::size_t before = read;
        read += page.total;
        if (cursor.historyComplete) {
            std::cout << "[Sync] " << title << ": historial completo (" << read << " mensajes leídos en esta pasada)"
                      << std::endl;
            onChannelChanged_(chatId);
        } else {
            // En historiales largos el catálogo se actualiza por tramos, sin esperar al final
            if (read / kProgressEvery != before / kProgressEvery) {
                std::cout << "[Sync] " << title << ": " << read << " mensajes leídos..." << std::endl;
                onChannelChanged_(chatId);
            }
            if (!sleepFor(kBatchDelay)) {
                return Result::Stopped;
            }
        }
    }
    return Result::Ok;
}

ChannelSync::Result ChannelSync::syncTopics(std::int64_t chatId) {
    std::vector<DbManager::Topic> topics;
    Json request = {{"@type", "getForumTopics"}, {"chat_id", chatId},       {"query", ""},
                    {"offset_date", 0},          {"offset_message_id", 0}, {"offset_forum_topic_id", 0},
                    {"limit", 100}};
    // Paginación por fecha, mensaje y tema; el límite de páginas evita un bucle si Telegram repite la página
    for (int page = 0; page < 50; ++page) {
        const auto response = telegram_.request(request, kRequestTimeout);
        if (stopping()) {
            return Result::Stopped;
        }
        if (!response || typeOf(*response) == "error") {
            // No es grave: los mensajes se sincronizan igual, solo faltarán los nombres de los temas
            std::cerr << "[Sync] No se pudieron leer los temas del chat " << chatId << ": "
                      << (response ? response->value("message", "desconocido") : std::string("sin respuesta")) << std::endl;
            return Result::Failed;
        }
        const Json pageTopics = response->value("topics", Json::array());
        for (const Json& topic : pageTopics) {
            const Json info = topic.value("info", Json::object());
            const std::int64_t id = info.value("forum_topic_id", std::int64_t{0});
            // Las páginas pueden solaparse: cada tema se guarda una sola vez
            const bool seen = std::any_of(topics.begin(), topics.end(),
                                          [id](const DbManager::Topic& known) { return known.id == id; });
            if (!seen) {
                topics.push_back({id, info.value("name", ""), 0});
            }
        }
        const auto nextDate = response->value("next_offset_date", 0);
        const auto nextMessage = response->value("next_offset_message_id", std::int64_t{0});
        const auto nextTopic = response->value("next_offset_forum_topic_id", 0);
        if (pageTopics.empty() || (nextDate == 0 && nextMessage == 0 && nextTopic == 0)) {
            break;
        }
        request["offset_date"] = nextDate;
        request["offset_message_id"] = nextMessage;
        request["offset_forum_topic_id"] = nextTopic;
    }

    // Solo se escribe (y se avisa al catálogo) si la lista ha cambiado
    std::vector<DbManager::Topic> stored = db_.listTopics(chatId);
    const auto sameTopics = [&] {
        if (stored.size() != topics.size()) {
            return false;
        }
        for (const DbManager::Topic& topic : topics) {
            const auto it = std::find_if(stored.begin(), stored.end(),
                                         [&](const DbManager::Topic& s) { return s.id == topic.id; });
            if (it == stored.end() || it->name != topic.name) {
                return false;
            }
        }
        return true;
    };
    if (!sameTopics() && db_.replaceTopics(chatId, topics)) {
        std::cout << "[Sync] Chat " << chatId << ": " << topics.size() << " temas" << std::endl;
        onChannelChanged_(chatId);
    }
    return Result::Ok;
}

void ChannelSync::applyDeletions(const std::map<std::int64_t, std::vector<std::int64_t>>& deletions) {
    for (const auto& [chatId, ids] : deletions) {
        const int deleted = db_.deleteMessages(chatId, ids);
        if (deleted > 0) {
            std::cout << "[Sync] Chat " << chatId << ": " << deleted << " mensajes borrados en Telegram" << std::endl;
            onChannelChanged_(chatId);
        }
    }
}

ChannelSync::Result ChannelSync::auditChannel(const DbManager::Channel& channel) {
    const std::int64_t chatId = channel.id;
    auto cursor = auditCursor_.find(chatId);
    if (cursor == auditCursor_.end()) {
        cursor = auditCursor_.emplace(chatId, std::numeric_limits<std::int64_t>::max()).first;
    }
    std::vector<std::int64_t> gone;
    std::size_t checked = 0;
    for (int batch = 0; batch < kAuditBatches;) {
        const std::vector<std::int64_t> ids = db_.messageIdsBefore(chatId, cursor->second, kBatchSize);
        if (ids.empty()) {
            cursor->second = std::numeric_limits<std::int64_t>::max();  // Vuelta completa: se empieza otra
            break;
        }
        const auto response = telegram_.request(
            {{"@type", "getMessages"}, {"chat_id", chatId}, {"message_ids", ids}}, kRequestTimeout);
        if (stopping()) {
            return Result::Stopped;
        }
        if (response && typeOf(*response) == "error") {
            if (const auto wait = floodWaitSeconds(*response)) {
                if (!sleepFor(std::chrono::seconds(*wait + 1))) {
                    return Result::Stopped;
                }
                continue;  // El mismo lote
            }
        }
        const Json messages =
            response && typeOf(*response) == "messages" ? response->value("messages", Json()) : Json();
        if (!messages.is_array() || messages.size() != ids.size()) {
            // Sin respuesta clara no se quita nada; se sigue en la próxima ronda desde aquí
            std::cerr << "[Sync] No se pudo repasar el canal " << chatId << ": "
                      << (response ? response->value("message", "respuesta inesperada") : std::string("sin respuesta"))
                      << std::endl;
            break;
        }
        std::vector<std::int64_t> missing;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (messages[i].is_null()) {
                missing.push_back(ids[i]);
            }
        }
        // Un mensaje quitado por error ya no se volvería a leer: uno de los que faltan se comprueba por
        // separado, y Telegram tiene que confirmar que no existe (404)
        if (!missing.empty()) {
            const auto single = telegram_.request(
                {{"@type", "getMessage"}, {"chat_id", chatId}, {"message_id", missing.front()}}, kRequestTimeout);
            if (stopping()) {
                return Result::Stopped;
            }
            if (!single || typeOf(*single) != "error" || single->value("code", 0) != 404) {
                std::cerr << "[Sync] " << channel.title << ": Telegram no confirma que el mensaje " << missing.front()
                          << " esté borrado; no se quita nada en este repaso" << std::endl;
                return Result::Ok;
            }
            gone.insert(gone.end(), missing.begin(), missing.end());
        }
        checked += ids.size();
        cursor->second = ids.back();
        ++batch;
        if (!sleepFor(kBatchDelay)) {
            return Result::Stopped;
        }
    }
    if (gone.empty()) {
        return Result::Ok;
    }
    const bool tooMany = gone.size() > kAuditMaxGone &&
                         static_cast<double>(gone.size()) > kAuditMaxGoneShare * static_cast<double>(checked);
    if (tooMany) {
        std::cerr << "[Sync] " << channel.title << ": Telegram no encuentra " << gone.size() << " de " << checked
                  << " mensajes repasados. Parece un fallo, no un borrado: no se quita nada" << std::endl;
        return Result::Ok;
    }
    applyDeletions({{chatId, gone}});
    return Result::Ok;
}

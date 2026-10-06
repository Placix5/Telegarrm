#include "channel_sync.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <regex>
#include <string>

#include "telegram_client.hpp"

namespace {

using Json = TelegramClient::Json;
using Message = DbManager::Message;

// Cada cuánto se buscan mensajes nuevos en todos los canales
constexpr auto kSyncInterval = std::chrono::minutes(15);
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

ChannelSync::ChannelSync(DbManager& db, TelegramClient& telegram) : db_(db), telegram_(telegram) {}

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
    while (!stopping()) {
        const bool ready = telegramReady();
        if (ready) {
            for (const DbManager::Channel& channel : db_.listChannels()) {
                syncingChatId_ = channel.id;
                const Result result = syncChannel(channel);
                syncingChatId_ = 0;
                if (result == Result::Stopped) {
                    return;
                }
            }
        }

        // Esperar a la siguiente ronda, a una petición de sincronizar o a la parada
        const std::chrono::seconds wait = ready ? std::chrono::seconds(kSyncInterval) : kNotReadyRetry;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, wait, [this] { return stopRequested_ || syncRequested_; });
        syncRequested_ = false;
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

ChannelSync::Result ChannelSync::syncChannel(const DbManager::Channel& channel) {
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
    if (!title.empty() && title != channel.title) {
        db_.updateChannelTitle(chatId, title);
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
        } else {
            if (read / kProgressEvery != before / kProgressEvery) {
                std::cout << "[Sync] " << title << ": " << read << " mensajes leídos..." << std::endl;
            }
            if (!sleepFor(kBatchDelay)) {
                return Result::Stopped;
            }
        }
    }
    return Result::Ok;
}

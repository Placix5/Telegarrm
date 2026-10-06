#include "download_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#include "telegram_client.hpp"

namespace {

using Json = TelegramClient::Json;
using Clock = std::chrono::steady_clock;

constexpr auto kPollInterval = std::chrono::seconds(1);
constexpr auto kPersistEvery = std::chrono::seconds(5);
constexpr auto kIdleWait = std::chrono::seconds(30);     // Sin nada en cola (wake() despierta antes)
constexpr auto kNotReadyWait = std::chrono::seconds(5);  // Sin sesión de Telegram
constexpr auto kRequestTimeout = std::chrono::seconds(20);
constexpr auto kStallTimeout = std::chrono::minutes(15);
constexpr auto kSpeedWindow = std::chrono::seconds(5);
// Espacio que debe quedar libre después de la descarga (la tarjeta SD no debe llenarse)
constexpr std::int64_t kSafetyMargin = 2'000'000'000;
constexpr int kPriority = 16;      // TDLib: 1 (baja) a 32 (alta)
constexpr int kMaxRestarts = 5;    // Reintentos de una parte que se detiene sin terminar
constexpr int kInactivePolls = 3;  // Sondeos seguidos sin actividad antes de reintentar

std::string typeOf(const Json& object) {
    return object.is_object() ? object.value("@type", "") : "";
}

bool isError(const std::optional<Json>& response) {
    return !response || typeOf(*response) == "error";
}

std::string errorText(const std::optional<Json>& response) {
    return response ? response->value("message", "error desconocido") : std::string("Telegram no responde");
}

// Objeto "file" de TDLib de un mensaje con documento, vídeo, audio o animación
std::optional<Json> fileOfMessage(const Json& message) {
    static const std::pair<const char*, const char*> kFields[] = {
        {"messageDocument", "document"}, {"messageVideo", "video"}, {"messageAudio", "audio"}, {"messageAnimation", "animation"}};
    const Json content = message.value("content", Json::object());
    const std::string type = typeOf(content);
    for (const auto& [contentType, field] : kFields) {
        if (type == contentType) {
            const Json media = content.value(field, Json::object());
            const Json file = media.value(field, Json::object());
            if (file.is_object() && file.contains("id")) {
                return file;
            }
        }
    }
    return std::nullopt;
}

// Tamaño en unidades del SI con coma decimal: "12,3 GB", "251 MB"
std::string formatSize(std::int64_t bytes) {
    char buffer[32];
    if (bytes >= 1'000'000'000) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / 1e9);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f MB", static_cast<double>(bytes) / 1e6);
    }
    std::string text = buffer;
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

}  // namespace

DownloadManager::DownloadManager(DbManager& db, TelegramClient& telegram, std::string filesDir)
    : db_(db), telegram_(telegram), filesDir_(std::move(filesDir)) {}

DownloadManager::~DownloadManager() {
    stop();
}

void DownloadManager::start() {
    if (worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        stopRequested_ = false;
    }
    worker_ = std::thread(&DownloadManager::run, this);
}

void DownloadManager::stop() {
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

void DownloadManager::wake() {
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        wakeRequested_ = true;
    }
    cv_.notify_all();
}

bool DownloadManager::cancelActive(std::int64_t id) {
    if (id == 0 || activeId_ != id) {
        return false;
    }
    cancelId_ = id;
    wake();
    return true;
}

std::optional<DownloadManager::Progress> DownloadManager::liveProgress(std::int64_t id) const {
    if (id == 0 || activeId_ != id) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(progressMutex_);
    return progress_;
}

bool DownloadManager::sleepFor(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(controlMutex_);
    cv_.wait_for(lock, duration, [this] { return stopRequested_ || wakeRequested_; });
    wakeRequested_ = false;
    return !stopRequested_;
}

bool DownloadManager::stopping() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return stopRequested_;
}

bool DownloadManager::telegramReady() const {
    return typeOf(telegram_.authorizationState()) == "authorizationStateReady";
}

void DownloadManager::run() {
    if (const int requeued = db_.requeueInterruptedDownloads()) {
        std::cout << "[Descargas] " << requeued << " descargas interrumpidas vuelven a la cola" << std::endl;
    }

    while (!stopping()) {
        if (!telegramReady()) {
            sleepFor(kNotReadyWait);
            continue;
        }
        std::optional<DbManager::Download> next = db_.nextQueuedDownload();
        if (!next) {
            sleepFor(kIdleWait);
            continue;
        }

        DbManager::Download& download = *next;
        db_.setDownloadStatus(download.id, "downloading");
        {
            std::lock_guard<std::mutex> lock(progressMutex_);
            progress_ = {download.downloadedSize, download.totalSize, 0};
        }
        activeId_ = download.id;
        std::cout << "[Descargas] Empieza #" << download.id << ": " << download.title << " (" << download.name << ", "
                  << formatSize(download.totalSize) << ")" << std::endl;

        std::string error;
        const Outcome outcome = process(download, error);
        activeId_ = 0;
        cancelId_ = 0;

        switch (outcome) {
            case Outcome::Completed:
                db_.setDownloadStatus(download.id, "completed");
                std::cout << "[Descargas] Terminada #" << download.id << ": " << download.name << std::endl;
                break;
            case Outcome::Failed:
                db_.setDownloadStatus(download.id, "failed", error);
                std::cerr << "[Descargas] Falló #" << download.id << ": " << error << std::endl;
                break;
            case Outcome::Cancelled:
                db_.setDownloadStatus(download.id, "cancelled");
                std::cout << "[Descargas] Cancelada #" << download.id << std::endl;
                break;
            case Outcome::Stopped:
                return;  // Sigue "downloading": al arrancar vuelve a la cola y TDLib continúa
        }
    }
}

DownloadManager::Outcome DownloadManager::process(DbManager::Download& download, std::string& error) {
    // 1) Espacio libre: lo que falta por bajar más el margen de seguridad
    std::error_code ec;
    const auto space = std::filesystem::space(filesDir_, ec);
    const std::int64_t remaining = std::max<std::int64_t>(0, download.totalSize - download.downloadedSize);
    if (!ec && static_cast<std::int64_t>(space.available) < remaining + kSafetyMargin) {
        error = "Espacio insuficiente en el disco: hacen falta " + formatSize(remaining + kSafetyMargin) +
                " (con 2 GB de margen) y quedan " + formatSize(static_cast<std::int64_t>(space.available));
        return Outcome::Failed;
    }

    // 2) Pedir a TDLib cada parte. getMessage da un identificador de archivo válido en esta sesión.
    struct ActivePart {
        DbManager::DownloadPart data;
        std::int64_t fileId = 0;
        bool done = false;
        int restarts = 0;
        int inactivePolls = 0;
    };
    std::vector<ActivePart> parts;
    const auto startDownload = [this](std::int64_t fileId) {
        return telegram_.request({{"@type", "downloadFile"},
                                  {"file_id", fileId},
                                  {"priority", kPriority},
                                  {"offset", 0},
                                  {"limit", 0},
                                  {"synchronous", false}},
                                 kRequestTimeout);
    };

    for (const DbManager::DownloadPart& stored : download.parts) {
        ActivePart part{stored};
        const auto message = telegram_.request(
            {{"@type", "getMessage"}, {"chat_id", download.chatId}, {"message_id", stored.messageId}}, kRequestTimeout);
        if (stopping()) {
            return Outcome::Stopped;
        }
        if (isError(message)) {
            error = "No se pudo leer el mensaje de la parte " + stored.fileName + ": " + errorText(message);
            return Outcome::Failed;
        }
        const auto file = fileOfMessage(*message);
        if (!file) {
            error = "El mensaje de la parte " + stored.fileName + " ya no tiene archivo";
            return Outcome::Failed;
        }
        part.fileId = file->value("id", std::int64_t{0});
        const Json local = file->value("local", Json::object());
        const std::string path = local.value("path", "");
        if (local.value("is_downloading_completed", false) && !path.empty() && std::filesystem::exists(path, ec)) {
            part.done = true;
            part.data.localPath = path;
            part.data.downloadedSize = part.data.size;
        } else {
            const auto started = startDownload(part.fileId);
            if (isError(started)) {
                error = "Telegram no permite descargar " + stored.fileName + ": " + errorText(started);
                return Outcome::Failed;
            }
        }
        parts.push_back(std::move(part));
    }

    // 3) Seguimiento hasta que terminen todas las partes
    auto lastAdvance = Clock::now();
    auto lastPersist = Clock::now();
    auto speedSince = Clock::now();
    std::int64_t lastTotal = -1;
    std::int64_t speedBase = download.downloadedSize;

    for (;;) {
        if (stopping()) {
            return Outcome::Stopped;
        }
        if (cancelId_ == download.id) {
            // Se para la descarga y se borra lo bajado a medias, para no dejar basura en el disco
            std::vector<DbManager::DownloadPart> data;
            for (const ActivePart& part : parts) {
                telegram_.request({{"@type", "cancelDownloadFile"}, {"file_id", part.fileId}, {"only_if_pending", false}},
                                  kRequestTimeout);
                telegram_.request({{"@type", "deleteFile"}, {"file_id", part.fileId}}, kRequestTimeout);
                DbManager::DownloadPart cleared = part.data;
                cleared.downloadedSize = 0;
                cleared.localPath.clear();
                data.push_back(cleared);
            }
            // Lo descargado se ha borrado: el progreso guardado vuelve a cero
            db_.updateDownloadProgress(download.id, 0, data);
            return Outcome::Cancelled;
        }

        bool allDone = true;
        std::int64_t total = 0;
        for (ActivePart& part : parts) {
            if (!part.done) {
                const auto file = telegram_.request({{"@type", "getFile"}, {"file_id", part.fileId}}, kRequestTimeout);
                if (!isError(file)) {
                    const Json local = file->value("local", Json::object());
                    part.data.downloadedSize = local.value("downloaded_size", std::int64_t{0});
                    if (local.value("is_downloading_completed", false)) {
                        part.done = true;
                        part.data.localPath = local.value("path", "");
                        part.data.downloadedSize = part.data.size;
                    } else if (!local.value("is_downloading_active", false)) {
                        // Detenida sin terminar (ej. corte de red): se vuelve a pedir, con un límite
                        if (++part.inactivePolls >= kInactivePolls) {
                            part.inactivePolls = 0;
                            if (++part.restarts > kMaxRestarts) {
                                error = "La parte " + part.data.fileName + " se detiene una y otra vez";
                                return Outcome::Failed;
                            }
                            startDownload(part.fileId);
                        }
                    } else {
                        part.inactivePolls = 0;
                    }
                }
            }
            allDone = allDone && part.done;
            total += part.data.downloadedSize;
        }

        const auto now = Clock::now();
        if (total > lastTotal) {
            lastTotal = total;
            lastAdvance = now;
        } else if (now - lastAdvance > kStallTimeout) {
            error = "La descarga no avanza desde hace 15 minutos";
            return Outcome::Failed;
        }
        {
            std::lock_guard<std::mutex> lock(progressMutex_);
            progress_.downloaded = total;
            progress_.total = download.totalSize;
            const auto window = std::chrono::duration<double>(now - speedSince).count();
            if (window >= std::chrono::duration<double>(kSpeedWindow).count()) {
                progress_.bytesPerSecond = static_cast<double>(total - speedBase) / window;
                speedBase = total;
                speedSince = now;
            }
        }
        if (allDone || now - lastPersist >= kPersistEvery) {
            std::vector<DbManager::DownloadPart> data;
            for (const ActivePart& part : parts) {
                data.push_back(part.data);
            }
            db_.updateDownloadProgress(download.id, total, data);
            lastPersist = now;
        }
        if (allDone) {
            return Outcome::Completed;
        }
        sleepFor(kPollInterval);
    }
}

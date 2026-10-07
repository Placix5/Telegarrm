#include "download_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#include "settings.hpp"
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

std::string pad2(int number) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d", number);
    return buffer;
}

}  // namespace

DbManager::Download makeDownload(const Catalog::Item& item, const Catalog::Release& release) {
    DbManager::Download download;
    download.chatId = release.chatId;
    download.messageId = release.parts.front().messageId;
    download.title = item.title;
    download.kind = item.kind;
    download.season = release.season;
    download.episode = release.episode;
    download.episodeEnd = release.episodeEnd;
    download.name = release.name;
    download.quality = release.quality;
    download.hdr = release.hdr;
    download.tags = release.tags;
    download.archive = release.archive;
    download.totalSize = release.size;
    for (const Catalog::Part& part : release.parts) {
        download.parts.push_back({part.messageId, part.number, part.fileName, part.size, 0, ""});
    }
    return download;
}

std::string describeDownload(const DbManager::Download& download) {
    std::string text = "«" + download.title + "»";
    if (download.episode > 0) {
        text += " " + std::to_string(download.season) + "x" + pad2(download.episode) +
                (download.episodeEnd > download.episode ? "-" + pad2(download.episodeEnd) : "");
    }
    const std::string label = library::versionLabel(download.quality, download.hdr, download.tags);
    return text + " (" + (label.empty() ? "calidad desconocida" : label) + ")";
}

DownloadManager::DownloadManager(DbManager& db, TelegramClient& telegram, std::string filesDir,
                                 WorkInfoResolver workInfo)
    : db_(db), telegram_(telegram), filesDir_(std::move(filesDir)), workInfo_(std::move(workInfo)) {}

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
    probeCompletedDownloads();

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
        Outcome outcome = process(download, error);
        if (outcome == Outcome::Completed) {
            std::cout << "[Descargas] Descargada #" << download.id << ": " << download.name << std::endl;
            outcome = importToLibrary(download, error);
        }
        activeId_ = 0;
        cancelId_ = 0;

        switch (outcome) {
            case Outcome::Completed:
                db_.setDownloadStatus(download.id, "completed");
                std::cout << "[Descargas] En la biblioteca #" << download.id << ": " << download.libraryPath << std::endl;
                logOutcome(download, outcome, error);
                break;
            case Outcome::Failed:
                db_.setDownloadStatus(download.id, "failed", error);
                std::cerr << "[Descargas] Falló #" << download.id << ": " << error << std::endl;
                logOutcome(download, outcome, error);
                break;
            case Outcome::Cancelled:
                db_.setDownloadStatus(download.id, "cancelled");
                std::cout << "[Descargas] Cancelada #" << download.id << std::endl;
                break;
            case Outcome::Stopped:
                return;  // Sigue "downloading"/"importing": al arrancar vuelve a la cola y se retoma
        }
    }
}

DownloadManager::Outcome DownloadManager::process(DbManager::Download& download, std::string& error) {
    // 1) Espacio libre: lo que falta por bajar más el margen de los ajustes (se lee en cada descarga,
    //    así que cambiarlo no exige reiniciar)
    std::error_code ec;
    const std::int64_t margin = loadSettings(db_).minFreeBytes;
    const auto space = std::filesystem::space(filesDir_, ec);
    const std::int64_t remaining = std::max<std::int64_t>(0, download.totalSize - download.downloadedSize);
    if (!ec && static_cast<std::int64_t>(space.available) < remaining + margin) {
        error = "Espacio insuficiente en " + filesDir_ + ": hacen falta " + formatSize(remaining + margin) + " (con " +
                formatSize(margin) + " de margen) y quedan " + formatSize(static_cast<std::int64_t>(space.available));
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
            for (std::size_t i = 0; i < parts.size(); ++i) {
                download.parts[i] = parts[i].data;
            }
            return Outcome::Completed;
        }
        sleepFor(kPollInterval);
    }
}

DownloadManager::Outcome DownloadManager::importToLibrary(DbManager::Download& download, std::string& error) {
    db_.setDownloadStatus(download.id, "importing");
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        progress_.downloaded = download.totalSize;
        progress_.bytesPerSecond = 0;
        progress_.importPercent = download.archive ? 0 : 100;
    }

    const AppSettings settings = loadSettings(db_);
    library::ImportRequest request;
    request.id = download.id;
    request.kind = download.kind;
    request.work = workInfo_(download);
    request.season = download.season;
    request.episode = download.episode;
    request.episodeEnd = download.episodeEnd;
    request.quality = download.quality;
    request.hdr = download.hdr;
    request.tags = download.tags;
    request.archive = download.archive;
    for (const DbManager::DownloadPart& part : download.parts) {
        request.parts.push_back(part.localPath);
    }
    request.libraryRoot = download.kind == "series" ? settings.seriesDir : settings.moviesDir;
    request.minFreeBytes = settings.minFreeBytes;

    const library::ImportResult result = library::importRelease(
        request,
        [this](int percent) {
            std::lock_guard<std::mutex> lock(progressMutex_);
            progress_.importPercent = percent;
        },
        [this, &download] { return stopping() || cancelId_ == download.id; });

    if (result.stopped) {
        if (cancelId_ != download.id) {
            return Outcome::Stopped;
        }
        // Cancelada al importar: se libera el búfer
        std::error_code ec;
        for (const std::string& part : request.parts) {
            std::filesystem::remove(part, ec);
        }
        return Outcome::Cancelled;
    }
    if (!result.ok) {
        error = "Descargado, pero no se pudo llevar a la biblioteca: " + result.error;
        return Outcome::Failed;
    }
    download.libraryPath = result.libraryPath;
    download.libraryFiles = result.files;
    db_.setDownloadLibrary(download.id, result.libraryPath, result.files);
    // La calidad real del vídeo manda sobre la del nombre (D-039): es la que compara el seguimiento
    if (result.probed && (result.probed->quality != download.quality || result.probed->hdr != download.hdr)) {
        std::cout << "[Descargas] #" << download.id << ": el vídeo es "
                  << library::versionLabel(result.probed->quality, result.probed->hdr.value_or(false), {})
                  << " (el nombre decía " << (download.quality.empty() ? "nada" : download.quality) << ")" << std::endl;
        download.quality = result.probed->quality;
        download.hdr = result.probed->hdr.value_or(download.hdr);
        db_.setDownloadQuality(download.id, download.quality, download.hdr);
    }
    return Outcome::Completed;
}

void DownloadManager::probeCompletedDownloads() {
    // Una vez (D-039): las descargas importadas antes de leer la calidad del propio vídeo
    constexpr const char* kSetting = "quality_probe_version";
    if (db_.getSetting(kSetting).value_or("") == "1" || library::findFfprobe().empty()) {
        return;
    }
    int corrected = 0;
    for (const DbManager::Download& download : db_.listDownloads()) {
        if (stopping()) {
            return;  // Se termina en el siguiente arranque
        }
        if ((download.status != "completed" && download.status != "replaced") || download.libraryFiles.empty()) {
            continue;
        }
        // El vídeo principal: el primero que exista de los colocados (los subtítulos no se reconocen)
        for (const std::string& file : download.libraryFiles) {
            const auto probed = library::probeVideo(file, [this] { return stopping(); });
            if (!probed) {
                continue;
            }
            const bool hdr = probed->hdr.value_or(download.hdr);
            if (probed->quality != download.quality || hdr != download.hdr) {
                db_.setDownloadQuality(download.id, probed->quality, hdr);
                std::cout << "[Descargas] #" << download.id << " (" << download.name << "): el vídeo es "
                          << library::versionLabel(probed->quality, hdr, {}) << ", no "
                          << (download.quality.empty() ? "calidad desconocida" : download.quality) << std::endl;
                ++corrected;
            }
            break;
        }
    }
    db_.setSetting(kSetting, "1");
    if (corrected > 0) {
        std::cout << "[Descargas] Calidad corregida en " << corrected << " descargas según el propio vídeo" << std::endl;
    }
}

std::string DownloadManager::replaceOlder(const DbManager::Download& download) {
    const AppSettings settings = loadSettings(db_);
    std::vector<std::string> labels;
    std::size_t removed = 0;
    bool unknownFiles = false;
    for (const std::int64_t id : download.replaces) {
        // Solo las que llegaron a la biblioteca (una que falló o se canceló no tiene nada que borrar)
        const auto old = db_.getDownload(id);
        if (!old || old->status != "completed") {
            continue;
        }
        if (!settings.keepReplaced) {
            if (old->libraryFiles.empty()) {
                unknownFiles = true;  // Descargas anteriores a la versión 7 de la BD
            } else {
                removed += library::removeFiles(old->libraryFiles, {settings.moviesDir, settings.seriesDir},
                                                download.libraryFiles);
            }
        }
        db_.setDownloadStatus(id, "replaced");
        labels.push_back(library::versionLabel(old->quality, old->hdr, old->tags));
        std::cout << "[Descargas] #" << id << " sustituida por #" << download.id << std::endl;
    }
    if (labels.empty()) {
        return "";
    }

    std::string previous;
    for (const std::string& label : labels) {
        previous += (previous.empty() ? "" : ", ") + (label.empty() ? std::string("calidad desconocida") : label);
    }
    std::string message = describeDownload(download) + " ya está en la biblioteca y sustituye a la versión " + previous;
    if (settings.keepReplaced) {
        return message + ", que se conserva (Ajustes).";
    }
    if (unknownFiles) {
        return message + ". No consta qué archivos tenía la versión anterior: bórralos a mano si quieres.";
    }
    return message + " (" + std::to_string(removed) + (removed == 1 ? " archivo borrado)." : " archivos borrados).");
}

void DownloadManager::logOutcome(const DbManager::Download& download, Outcome outcome, const std::string& error) {
    DbManager::Activity activity;
    activity.chatId = download.chatId;
    activity.messageId = download.messageId;
    activity.followId = download.followId;
    activity.downloadId = download.id;
    if (outcome == Outcome::Completed) {
        const std::string replaced = download.replaces.empty() ? "" : replaceOlder(download);
        if (!replaced.empty()) {
            activity.type = "upgraded";
            activity.message = replaced;
        } else if (download.origin == "auto") {
            activity.type = "completed";
            activity.message = describeDownload(download) + " ya está en la biblioteca.";
        }
    } else if (outcome == Outcome::Failed && download.origin == "auto") {
        activity.type = "failed";
        activity.message = "Falló la descarga automática de " + describeDownload(download) + ": " + error;
    }
    if (!activity.type.empty()) {
        db_.addActivity(activity);
    }
}

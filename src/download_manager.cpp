#include "download_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
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
        // Importación y sustituciones, sin que las cruce una revisión de la biblioteca (D-051)
        std::unique_lock<std::mutex> libraryLock(libraryMutex_, std::defer_lock);
        if (outcome == Outcome::Completed) {
            std::cout << "[Descargas] Descargada #" << download.id << ": " << download.name << std::endl;
            libraryLock.lock();
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
        const auto file = TelegramClient::fileOfMessage(*message);
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
    // Un episodio sustituye siempre a las otras versiones del mismo episodio; una película, solo si es
    // una mejora del seguimiento (a mano se pueden tener varias versiones, D-041)
    request.replaceOthers = download.kind == "series" || download.origin == "auto";

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
    superseded_ = result.superseded;
    db_.setDownloadLibrary(download.id, result.libraryPath, result.files);
    // La calidad real del vídeo manda sobre la del nombre (D-039): es la que compara el seguimiento.
    // También queda como calidad comprobada del archivo en el catálogo (D-042).
    if (result.probed) {
        db_.saveProbe({download.chatId, download.messageId, result.probed->quality,
                       result.probed->hdr.value_or(download.hdr), "", 0});
    }
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

namespace {

// Carpeta de la obra de un archivo de la biblioteca: la de encima de "Season 04" en las series
std::string workFolderOf(const std::string& file) {
    const std::filesystem::path parent = std::filesystem::path(file).parent_path();
    return parent.filename().string().rfind("Season ", 0) == 0 ? parent.parent_path().string() : parent.string();
}

}  // namespace

DownloadManager::LibraryScan DownloadManager::scanLibrary() {
    std::lock_guard<std::mutex> lock(libraryMutex_);
    return scanLocked(true);
}

DownloadManager::LibraryDeletion DownloadManager::deleteFromLibrary(const std::string& kind, const std::string& folder,
                                                                    const std::vector<std::string>& paths) {
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> lock(libraryMutex_);
    LibraryDeletion result;
    const AppSettings settings = loadSettings(db_);
    const std::string root = kind == "series" ? settings.seriesDir : kind == "movie" ? settings.moviesDir : "";
    if (root.empty()) {
        result.error = "Biblioteca desconocida";
        return result;
    }
    std::vector<std::string> files;
    for (const std::string& path : paths) {
        const std::vector<std::string> found = library::libraryFilesToDelete(root, folder, path);
        if (found.empty()) {
            result.error = "No está en la biblioteca: " + (path.empty() ? folder : folder + "/" + path);
            return result;
        }
        files.insert(files.end(), found.begin(), found.end());
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    std::error_code ec;
    std::map<std::string, std::int64_t> sizes;
    for (const std::string& file : files) {
        sizes[file] = static_cast<std::int64_t>(fs::file_size(file, ec));
    }
    result.files = library::removeFiles(files, {settings.seriesDir, settings.moviesDir}, {});
    for (const auto& [file, size] : sizes) {
        if (!fs::exists(file, ec)) {
            result.bytes += size;
        }
    }
    result.ok = result.files > 0;
    if (!result.ok) {
        result.error = "No se pudo borrar nada";
        return result;
    }

    DbManager::Activity activity;
    activity.type = "deleted";
    const std::string what = paths.size() == 1 && paths.front().empty() ? "«" + folder + "»"
                                                                         : "«" + folder + "» (" + std::to_string(paths.size()) +
                                                                               (paths.size() == 1 ? " elemento)" : " elementos)");
    activity.message = "Borrado desde la biblioteca: " + what + ", " + std::to_string(result.files) +
                       (result.files == 1 ? " archivo" : " archivos") + " y " + formatSize(result.bytes) + ".";
    db_.addActivity(activity);
    std::cout << "[Biblioteca] Borrado " << root << "/" << folder << ": " << result.files << " archivos ("
              << formatSize(result.bytes) << ")" << std::endl;
    result.downloads = static_cast<int>(scanLocked(false).missing.size());
    return result;
}

DownloadManager::LibraryScan DownloadManager::scanLocked(bool logEach) {
    namespace fs = std::filesystem;
    LibraryScan scan;
    const AppSettings settings = loadSettings(db_);
    const std::vector<DbManager::Download> downloads = db_.listDownloads();
    std::error_code ec;
    const auto exists = [&ec](const std::string& path) { return !path.empty() && fs::exists(path, ec); };

    // Archivos que siguen donde los dejó alguna descarga: no se le asignan a otra
    std::set<std::string> claimed;
    for (const DbManager::Download& download : downloads) {
        if (download.status == "completed" || download.status == "replaced") {
            for (const std::string& file : download.libraryFiles) {
                if (exists(file)) {
                    claimed.insert(file);
                }
            }
        }
    }

    // Archivos de la biblioteca por nombre, para encontrar lo movido de carpeta (se recorre una vez,
    // y solo si hace falta)
    std::map<std::string, std::vector<std::string>> byName;
    bool indexed = false;
    const auto index = [&]() {
        indexed = true;
        for (const std::string& root : {settings.seriesDir, settings.moviesDir}) {
            if (root.empty() || !fs::is_directory(root, ec)) {
                continue;
            }
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    byName[it->path().filename().string()].push_back(it->path().string());
                }
            }
            ec.clear();
        }
    };

    for (const DbManager::Download& download : downloads) {
        if (download.status != "completed") {
            continue;
        }
        ++scan.checked;
        std::vector<std::string> present;
        for (const std::string& file : download.libraryFiles) {
            if (exists(file)) {
                present.push_back(file);
            }
        }
        if (!present.empty()) {
            if (present.size() < download.libraryFiles.size()) {
                db_.setDownloadLibrary(download.id, download.libraryPath, present);  // Quedan algunos (el vídeo)
            }
            continue;
        }
        if (download.libraryFiles.empty() && fs::is_directory(download.libraryPath, ec)) {
            continue;  // Importada antes de D-037: solo se sabe su carpeta, y sigue ahí
        }

        // ¿Movida? 1) El mismo nombre en otra carpeta de la biblioteca
        std::vector<std::string> found;
        if (!indexed) {
            index();
        }
        for (const std::string& file : download.libraryFiles) {
            const auto it = byName.find(fs::path(file).filename().string());
            if (it != byName.end() && it->second.size() == 1 && !claimed.count(it->second.front())) {
                found.push_back(it->second.front());
            }
        }
        // 2) Renombrada: en la carpeta de su obra, un vídeo del mismo episodio (en películas, con su
        //    calidad si hay varios) que no sea de otra descarga
        if (found.empty()) {
            const std::string root = download.kind == "series" ? settings.seriesDir : settings.moviesDir;
            const library::WorkInfo work = workInfo_(download);
            std::string best;
            for (const library::DiskVideo& video : library::videosOnDisk(root, work)) {
                const bool sameEpisode = download.kind != "series" ||
                                         (video.season == download.season && video.episode == download.episode);
                if (!sameEpisode || claimed.count(video.path)) {
                    continue;
                }
                if (best.empty() || video.quality == download.quality) {
                    best = video.path;
                }
            }
            if (!best.empty()) {
                found.push_back(best);
            }
        }
        if (!found.empty()) {
            db_.setDownloadLibrary(download.id, workFolderOf(found.front()), found);
            claimed.insert(found.begin(), found.end());
            ++scan.moved;
            std::cout << "[Biblioteca] #" << download.id << " (" << download.name << ") está ahora en "
                      << found.front() << std::endl;
            continue;
        }

        db_.setDownloadStatus(download.id, "deleted");
        if (logEach) {
            DbManager::Activity activity;
            activity.type = "deleted";
            activity.message = describeDownload(download) + " ya no está en la biblioteca.";
            activity.chatId = download.chatId;
            activity.messageId = download.messageId;
            activity.followId = download.followId;
            activity.downloadId = download.id;
            db_.addActivity(activity);
        }
        scan.missing.push_back(download);
        std::cout << "[Biblioteca] #" << download.id << " (" << download.name << ") ya no está" << std::endl;
    }
    std::cout << "[Biblioteca] Revisadas " << scan.checked << " descargas: " << scan.moved << " movidas, "
              << scan.missing.size() << " ya no están" << std::endl;
    return scan;
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

std::string DownloadManager::replaceOlder(const DbManager::Download& download, const std::vector<std::string>& superseded) {
    const AppSettings settings = loadSettings(db_);
    const std::set<std::string> supersededFiles(superseded.begin(), superseded.end());

    // Descargas anteriores: las que el seguimiento pidió sustituir y las que colocaron los archivos
    // que la importación ha encontrado como versiones anteriores (D-041). Solo las que llegaron a la
    // biblioteca: una que falló o se canceló no tiene nada que borrar.
    std::vector<DbManager::Download> older;
    for (const DbManager::Download& other : db_.listDownloads()) {
        if (other.id == download.id || other.status != "completed") {
            continue;
        }
        const bool listed = std::find(download.replaces.begin(), download.replaces.end(), other.id) != download.replaces.end();
        const bool placedThem = std::any_of(other.libraryFiles.begin(), other.libraryFiles.end(),
                                            [&](const std::string& file) { return supersededFiles.count(file) > 0; });
        if (listed || placedThem) {
            older.push_back(other);
        }
    }
    if (older.empty() && superseded.empty()) {
        return "";
    }

    std::vector<std::string> files(superseded.begin(), superseded.end());
    std::vector<std::string> previous;  // Para el historial: versión de cada descarga o nombre del archivo suelto
    std::set<std::string> described;
    for (const DbManager::Download& old : older) {
        files.insert(files.end(), old.libraryFiles.begin(), old.libraryFiles.end());
        const std::string label = library::versionLabel(old.quality, old.hdr, old.tags);
        previous.push_back("la versión " + (label.empty() ? std::string("de calidad desconocida") : label));
        described.insert(old.libraryFiles.begin(), old.libraryFiles.end());
    }
    for (const std::string& file : superseded) {
        if (!described.count(file) && !library::isSubtitle(file)) {
            previous.push_back(std::filesystem::path(file).filename().string());
        }
    }
    bool unknownFiles = false;
    std::size_t removed = 0;
    if (!settings.keepReplaced) {
        unknownFiles = std::any_of(older.begin(), older.end(),
                                   [](const DbManager::Download& old) { return old.libraryFiles.empty(); });
        removed = library::removeFiles(files, {settings.moviesDir, settings.seriesDir}, download.libraryFiles);
    }
    for (const DbManager::Download& old : older) {
        db_.setDownloadStatus(old.id, "replaced");
        std::cout << "[Descargas] #" << old.id << " sustituida por #" << download.id << std::endl;
    }

    std::string list;
    for (std::size_t i = 0; i < previous.size(); ++i) {
        list += (i == 0 ? "" : i + 1 == previous.size() ? " y " : ", ") + previous[i];
    }
    std::string message = describeDownload(download) + " ya está en la biblioteca y sustituye a " + list;
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
        const std::string replaced = replaceOlder(download, superseded_);
        superseded_.clear();
        if (!replaced.empty()) {
            // Mejora pedida por el seguimiento, o un duplicado que sustituye a la versión anterior
            activity.type = download.origin == "auto" ? "upgraded" : "replaced";
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

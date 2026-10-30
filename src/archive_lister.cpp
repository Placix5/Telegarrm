#include "archive_lister.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <system_error>

#include "archive_index.hpp"
#include "telegram_client.hpp"
#include "tracker.hpp"

namespace archive {

bool shouldList(const Catalog::Item& item, const Catalog::Release& release, std::int64_t now) {
    return item.kind == "series" && release.archive && release.episode == 0 && release.size > 0 &&
           !release.parts.empty() && isZipName(release.parts.front().fileName) &&
           tracking::releaseComplete(release, now);
}

std::vector<PartRange> tailRanges(const std::vector<Catalog::Part>& parts, std::int64_t bytes) {
    std::vector<PartRange> ranges;
    std::int64_t remaining = bytes;
    for (std::size_t i = parts.size(); i-- > 0 && remaining > 0;) {
        const std::int64_t take = std::min(remaining, parts[i].size);
        if (take > 0) {
            ranges.insert(ranges.begin(), PartRange{i, parts[i].size - take, take});
            remaining -= take;
        }
    }
    return ranges;
}

}  // namespace archive

namespace {

using Json = TelegramClient::Json;

constexpr int kPriority = 1;  // La más baja: por detrás de las descargas (16) y de la calidad (8)
constexpr auto kRequestTimeout = std::chrono::seconds(20);
constexpr auto kDownloadTimeout = std::chrono::seconds(90);
constexpr auto kPause = std::chrono::seconds(3);              // Entre un comprimido y el siguiente
constexpr auto kIdleInterval = std::chrono::hours(6);         // Sin avisos, una pasada cada tanto
constexpr auto kRetryInterval = std::chrono::minutes(10);     // Si quedó algo por la red
constexpr auto kRetryAfterFailure = std::chrono::hours(1);    // El mismo archivo, tras un fallo de red
constexpr int kListingsPerRebuild = 10;                       // Se recalcula el canal cada tantos

std::string typeOf(const Json& object) {
    return object.is_object() ? object.value("@type", "") : "";
}

std::string errorText(const std::optional<Json>& response) {
    return response ? response->value("message", "error desconocido") : std::string("Telegram no responde");
}

std::string readAt(const std::string& path, std::int64_t offset, std::int64_t length) {
    std::ifstream in(path, std::ios::binary);
    if (!in.seekg(offset)) {
        return "";
    }
    std::string data(static_cast<std::size_t>(length), '\0');
    in.read(&data[0], length);
    data.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return data;
}

}  // namespace

ArchiveLister::ArchiveLister(DbManager& db, TelegramClient& telegram, Catalog& catalog, ChangedCallback changed)
    : db_(db), telegram_(telegram), catalog_(catalog), changed_(std::move(changed)) {}

ArchiveLister::~ArchiveLister() {
    stop();
}

void ArchiveLister::start() {
    if (worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
    }
    worker_ = std::thread(&ArchiveLister::run, this);
}

void ArchiveLister::stop() {
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

void ArchiveLister::requestRun() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runRequested_ = true;
    }
    cv_.notify_all();
}

bool ArchiveLister::stopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopRequested_;
}

void ArchiveLister::run() {
    bool pending = false;
    for (;;) {
        {
            // La primera pasada espera al aviso del catálogo, que se calcula al arrancar
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, pending ? std::chrono::duration_cast<std::chrono::seconds>(kRetryInterval)
                                       : std::chrono::duration_cast<std::chrono::seconds>(kIdleInterval),
                         [this] { return stopRequested_ || runRequested_; });
            if (stopRequested_) {
                return;
            }
            runRequested_ = false;
        }
        try {
            pending = pass();
        } catch (const std::exception& e) {
            std::cerr << "[Comprimidos] Error al leer los índices: " << e.what() << std::endl;
            pending = true;
        }
    }
}

bool ArchiveLister::pass() {
    if (typeOf(telegram_.authorizationState()) != "authorizationStateReady") {
        return true;
    }
    const std::vector<Catalog::ItemPtr> items = catalog_.items();
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const auto clock = std::chrono::steady_clock::now();
    std::map<std::int64_t, std::map<std::int64_t, DbManager::ArchiveListing>> stored;
    std::vector<const Catalog::Release*> todo;  // Apuntan dentro de items, que los mantiene vivos
    for (const Catalog::ItemPtr& item : items) {
        for (const Catalog::Release& release : item->releases) {
            if (!archive::shouldList(*item, release, now)) {
                continue;
            }
            auto channel = stored.find(release.chatId);
            if (channel == stored.end()) {
                channel = stored.emplace(release.chatId, db_.archiveListings(release.chatId)).first;
            }
            const auto listing = channel->second.find(release.parts.front().messageId);
            if (listing != channel->second.end() && listing->second.archiveSize == release.size) {
                continue;  // Ya leído (o ya se sabe que no se puede)
            }
            const auto retry = retryAfter_.find({release.chatId, release.parts.front().messageId});
            if (retry != retryAfter_.end() && retry->second > clock) {
                continue;
            }
            todo.push_back(&release);
        }
    }
    if (todo.empty()) {
        return false;
    }
    std::cout << "[Comprimidos] " << todo.size() << " por leer" << std::endl;

    std::set<std::int64_t> changedChats;
    int sinceRebuild = 0;
    const auto flush = [&] {
        for (const std::int64_t chatId : changedChats) {
            changed_(chatId);
        }
        changedChats.clear();
        sinceRebuild = 0;
    };
    bool pending = false;
    for (const Catalog::Release* release : todo) {
        if (stopping()) {
            break;
        }
        if (list(*release)) {
            changedChats.insert(release->chatId);
            if (++sinceRebuild >= kListingsPerRebuild) {
                flush();
            }
        } else {
            retryAfter_[{release->chatId, release->parts.front().messageId}] =
                std::chrono::steady_clock::now() + kRetryAfterFailure;
            pending = true;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, kPause, [this] { return stopRequested_; })) {
            break;
        }
    }
    flush();
    return pending;
}

bool ArchiveLister::list(const Catalog::Release& release) {
    for (const Catalog::Part& part : release.parts) {
        // Si se está descargando, no se toca: otra petición del mismo archivo cambiaría la suya
        if (db_.isDownloadingMessage(release.chatId, part.messageId)) {
            return false;
        }
    }
    std::string error;
    std::string tail = readTail(release, std::min(release.size, archive::kZipTailBytes), error);
    if (tail.empty()) {
        std::cerr << "[Comprimidos] " << release.name << ": " << error << std::endl;
        return false;
    }
    archive::ZipIndex index = archive::readZipIndex(tail);
    if (index.needTail > 0 && index.needTail <= release.size) {
        tail = readTail(release, index.needTail, error);
        if (tail.empty()) {
            std::cerr << "[Comprimidos] " << release.name << ": " << error << std::endl;
            return false;
        }
        index = archive::readZipIndex(tail);
    }

    DbManager::ArchiveListing listing;
    listing.chatId = release.chatId;
    listing.messageId = release.parts.front().messageId;
    listing.archiveSize = release.size;
    if (index.ok) {
        for (const archive::Entry& entry : index.entries) {
            listing.entries.emplace_back(entry.name, entry.size);
        }
        std::cout << "[Comprimidos] " << release.name << ": " << archive::videoEntries(index.entries).size()
                  << " vídeos" << std::endl;
    } else {
        // Un fallo del propio archivo se recuerda: no se vuelve a pedir mientras no cambie
        listing.error = index.error.empty() ? "El índice del ZIP no cabe en lo leído" : index.error;
        std::cout << "[Comprimidos] " << release.name << ": " << listing.error << std::endl;
    }
    db_.saveArchiveListing(listing);
    return true;
}

std::string ArchiveLister::readTail(const Catalog::Release& release, std::int64_t bytes, std::string& error) {
    std::string tail;
    for (const archive::PartRange& range : archive::tailRanges(release.parts, bytes)) {
        const std::string data =
            readRange(release.chatId, release.parts[range.part].messageId, range.offset, range.length, error);
        if (data.empty()) {
            return "";
        }
        tail += data;
    }
    if (static_cast<std::int64_t>(tail.size()) != bytes) {
        error = "No se pudo leer el final del archivo";
        return "";
    }
    return tail;
}

std::string ArchiveLister::readRange(std::int64_t chatId, std::int64_t messageId, std::int64_t offset,
                                     std::int64_t length, std::string& error) {
    const auto message = telegram_.request(
        {{"@type", "getMessage"}, {"chat_id", chatId}, {"message_id", messageId}}, kRequestTimeout);
    if (!message || typeOf(*message) == "error") {
        error = "No se pudo leer el mensaje: " + errorText(message);
        return "";
    }
    const auto file = TelegramClient::fileOfMessage(*message);
    if (!file) {
        error = "El mensaje ya no tiene archivo";
        return "";
    }
    const std::int64_t fileId = file->value("id", std::int64_t{0});
    Json local = file->value("local", Json::object());
    std::error_code ec;
    const bool alreadyHere = local.value("is_downloading_completed", false) &&
                             std::filesystem::exists(local.value("path", ""), ec);
    if (!alreadyHere) {
        // Solo ese trozo: TDLib lo guarda en su sitio dentro del archivo a medias
        const auto downloaded = telegram_.request({{"@type", "downloadFile"},
                                                   {"file_id", fileId},
                                                   {"priority", kPriority},
                                                   {"offset", offset},
                                                   {"limit", length},
                                                   {"synchronous", true}},
                                                  kDownloadTimeout);
        if (!downloaded || typeOf(*downloaded) == "error") {
            error = "Telegram no permite leer el final del archivo: " + errorText(downloaded);
            return "";
        }
        local = downloaded->value("local", Json::object());
    }
    const std::string path = local.value("path", "");
    std::string data = path.empty() ? std::string() : readAt(path, offset, length);
    // El trozo no se queda en el búfer (salvo que entretanto haya empezado su descarga completa)
    if (!alreadyHere && !db_.isDownloadingMessage(chatId, messageId)) {
        telegram_.request({{"@type", "cancelDownloadFile"}, {"file_id", fileId}, {"only_if_pending", false}},
                          kRequestTimeout);
        telegram_.request({{"@type", "deleteFile"}, {"file_id", fileId}}, kRequestTimeout);
    }
    if (static_cast<std::int64_t>(data.size()) != length) {
        error = "No se pudo leer el final del archivo";
        return "";
    }
    return data;
}

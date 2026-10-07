#include "release_prober.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include "telegram_client.hpp"

namespace {

using Json = TelegramClient::Json;

// Lo que se descarga de cada archivo: la cabecera de un MKV ocupa unos pocos kB, pero puede ir
// detrás de las cabeceras del comprimido o de adjuntos (tipos de letra de los subtítulos)
constexpr std::int64_t kProbeBytes = 4'000'000;
// Si no basta (vídeo suelto con elementos al final o adjuntos delante): el final y un principio más largo
constexpr std::int64_t kLongProbeBytes = 16'000'000;
constexpr int kPriority = 8;  // Menos que las descargas (16): no les quita ancho de banda
constexpr auto kRequestTimeout = std::chrono::seconds(20);
constexpr auto kDownloadTimeout = std::chrono::seconds(90);

std::string typeOf(const Json& object) {
    return object.is_object() ? object.value("@type", "") : "";
}

std::string errorText(const std::optional<Json>& response) {
    return response ? response->value("message", "error desconocido") : std::string("Telegram no responde");
}

std::string readPrefix(const std::string& path, std::int64_t bytes) {
    std::ifstream in(path, std::ios::binary);
    std::string data(static_cast<std::size_t>(bytes), '\0');
    in.read(&data[0], bytes);
    data.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return data;
}

}  // namespace

ReleaseProber::ReleaseProber(DbManager& db, TelegramClient& telegram) : db_(db), telegram_(telegram) {}

ReleaseProber::Result ReleaseProber::probe(const Catalog::Release& release) {
    std::lock_guard<std::mutex> lock(mutex_);
    Result result;
    if (release.parts.empty()) {
        result.error = "El archivo no tiene partes";
        return result;
    }
    const std::int64_t chatId = release.chatId;
    const std::int64_t messageId = release.parts.front().messageId;
    DbManager::Probe saved{chatId, messageId, "", false, "", 0};
    const auto fail = [&](const std::string& error, bool remember) {
        result.error = error;
        if (remember) {  // Los fallos del propio archivo se recuerdan; los de red, no
            saved.error = error;
            db_.saveProbe(saved);
        }
        return result;
    };
    if (library::findFfprobe().empty()) {
        return fail("Falta ffprobe: «sudo apt install ffmpeg»", false);
    }
    // Si se está descargando, no se toca: otra petición de descarga del mismo archivo cambiaría la suya
    if (db_.isDownloadingMessage(chatId, messageId)) {
        return fail("Se está descargando: su calidad se sabrá al importarlo", false);
    }

    const auto message = telegram_.request(
        {{"@type", "getMessage"}, {"chat_id", chatId}, {"message_id", messageId}}, kRequestTimeout);
    if (!message || typeOf(*message) == "error") {
        return fail("No se pudo leer el mensaje: " + errorText(message), false);
    }
    const auto file = TelegramClient::fileOfMessage(*message);
    if (!file) {
        return fail("El mensaje ya no tiene archivo", true);
    }
    const std::int64_t fileId = file->value("id", std::int64_t{0});
    const std::int64_t size = std::max(file->value("size", std::int64_t{0}), file->value("expected_size", std::int64_t{0}));
    const std::int64_t wanted = size > 0 ? std::min(size, kProbeBytes) : kProbeBytes;
    Json local = file->value("local", Json::object());
    std::error_code ec;
    const bool alreadyHere = local.value("is_downloading_completed", false) &&
                             std::filesystem::exists(local.value("path", ""), ec);

    // Solo el principio del archivo
    if (!alreadyHere) {
        const auto downloaded = telegram_.request({{"@type", "downloadFile"},
                                                   {"file_id", fileId},
                                                   {"priority", kPriority},
                                                   {"offset", 0},
                                                   {"limit", wanted},
                                                   {"synchronous", true}},
                                                  kDownloadTimeout);
        if (!downloaded || typeOf(*downloaded) == "error") {
            return fail("Telegram no permite leer el principio del archivo: " + errorText(downloaded), false);
        }
        local = downloaded->value("local", Json::object());
    }
    const std::string path = local.value("path", "");
    const std::string data = path.empty() ? std::string() : readPrefix(path, wanted);

    // Limpieza: el trozo descargado no se queda en el búfer (salvo que entretanto haya empezado su
    // descarga completa, que lo aprovecha)
    const auto cleanUp = [&] {
        if (!alreadyHere && !db_.isDownloadingMessage(chatId, messageId)) {
            telegram_.request({{"@type", "cancelDownloadFile"}, {"file_id", fileId}, {"only_if_pending", false}},
                              kRequestTimeout);
            telegram_.request({{"@type", "deleteFile"}, {"file_id", fileId}}, kRequestTimeout);
        }
    };
    if (data.empty()) {
        cleanUp();
        return fail("No se pudo leer el principio del archivo", false);
    }

    std::string reason;
    auto video = library::videoFromPrefix(data, reason);
    if (!video && data.compare(0, 4, "Rar!") == 0) {
        // RAR que comprime el vídeo: 7-Zip descomprime el principio aunque el archivo esté cortado
        video = library::videoFromPrefixWith7z(data, ".rar", reason);
    }
    if (!video) {
        cleanUp();
        return fail(reason, true);
    }
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() / ("telegarrm-calidad-" + std::to_string(::getpid()) + ".bin");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(video->data(), static_cast<std::streamsize>(video->size()));
    }
    std::string diagnostics;
    auto info = library::probeVideo(temporary.string(), nullptr, true, &diagnostics);
    std::filesystem::remove(temporary, ec);
    // Un vídeo suelto (no comprimido) que necesita más que su principio: un MP4 con el índice (moov) al
    // final, o un MKV que apunta a elementos del final o con adjuntos grandes delante. Se piden su final
    // y después un principio más largo, y se analiza el archivo a medias de TDLib, que guarda cada trozo
    // en su sitio.
    const bool plain = video->size() == data.size();
    if (!info && plain && !alreadyHere && size > wanted) {
        // El MP4 suele tener el índice al final; el MKV, adjuntos delante (antes el principio largo)
        const bool mp4 = video->size() >= 8 && video->compare(4, 4, "ftyp") == 0;
        std::vector<std::pair<std::int64_t, std::int64_t>> ranges = {{size - wanted, wanted},
                                                                     {0, std::min(size, kLongProbeBytes)}};
        if (!mp4) {
            std::swap(ranges[0], ranges[1]);
        }
        for (const auto& [offset, limit] : ranges) {
            const auto more = telegram_.request({{"@type", "downloadFile"},
                                                 {"file_id", fileId},
                                                 {"priority", kPriority},
                                                 {"offset", offset},
                                                 {"limit", limit},
                                                 {"synchronous", true}},
                                                kDownloadTimeout);
            if (!more || typeOf(*more) == "error") {
                break;
            }
            info = library::probeVideo(more->value("local", Json::object()).value("path", path), nullptr, true,
                                       &diagnostics);
            if (info) {
                break;
            }
        }
    }
    // Dentro de un comprimido no se puede pedir el final del vídeo, pero sí un principio más largo
    if (!info && !plain && !alreadyHere && size > wanted) {
        const std::int64_t longer = std::min(size, kLongProbeBytes);
        const auto more = telegram_.request({{"@type", "downloadFile"},
                                             {"file_id", fileId},
                                             {"priority", kPriority},
                                             {"offset", 0},
                                             {"limit", longer},
                                             {"synchronous", true}},
                                            kDownloadTimeout);
        if (more && typeOf(*more) != "error") {
            const std::string longData = readPrefix(more->value("local", Json::object()).value("path", path), longer);
            std::string longReason;
            auto longVideo = library::videoFromPrefix(longData, longReason);
            if (!longVideo && longData.compare(0, 4, "Rar!") == 0) {
                longVideo = library::videoFromPrefixWith7z(longData, ".rar", longReason);
            }
            if (longVideo) {
                {
                    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                    out.write(longVideo->data(), static_cast<std::streamsize>(longVideo->size()));
                }
                info = library::probeVideo(temporary.string(), nullptr, true, &diagnostics);
                std::filesystem::remove(temporary, ec);
                video = std::move(longVideo);
            }
        }
    }
    cleanUp();
    if (!info) {
        const bool mp4Inside = !plain && video->size() >= 8 && video->compare(4, 4, "ftyp") == 0;
        return fail(mp4Inside ? "Es un MP4 dentro de un comprimido, con su índice al final: sin descargarlo "
                                "entero no se puede leer"
                              : "ffprobe no reconoce el vídeo" +
                                    (diagnostics.empty() ? std::string() : " (" + diagnostics + ")"),
                    true);
    }

    result.ok = true;
    result.info = *info;
    saved.quality = info->quality;
    saved.hdr = info->hdr.value_or(false);
    db_.saveProbe(saved);
    std::cout << "[Calidad] " << release.name << ": " << library::versionLabel(saved.quality, saved.hdr, {})
              << " (el nombre dice " << (release.quality.empty() ? "nada" : release.quality) << ")" << std::endl;
    return result;
}

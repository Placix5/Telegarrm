#include "library.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <regex>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "media_parser.hpp"
#include "process.hpp"

namespace library {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxNameBytes = 180;
// Un vídeo con "sample" o "muestra" en el nombre y menos del 30 % del mayor es una muestra
constexpr double kSampleRatio = 0.3;
// Los nombres repetidos se numeran como mucho hasta aquí: "Peli (2).mkv"... "Peli (99).mkv"
constexpr int kMaxDuplicates = 99;
// Carpeta temporal de descompresión, oculta dentro de la biblioteca (mismo disco que el destino)
constexpr const char* kTemporaryDir = ".telegarrm";

const std::set<std::string> kVideoExtensions = {".mkv", ".mp4", ".avi", ".m4v", ".ts",   ".m2ts",
                                                ".wmv", ".mov", ".mpg", ".mpeg", ".webm"};
const std::set<std::string> kSubtitleExtensions = {".srt", ".ass", ".ssa", ".sub", ".idx", ".vtt", ".sup"};

std::string lowerExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string pad2(int number) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d", number);
    return buffer;
}

std::string titleWithYear(const WorkInfo& work) {
    return sanitizeName(work.title) + (work.year ? " (" + std::to_string(*work.year) + ")" : "");
}

std::string labelSuffix(const std::string& versionLabel) {
    const std::string label = versionLabel.empty() ? "" : sanitizeName(versionLabel);
    return label.empty() ? "" : " - " + label;
}

// Mueve un fichero: un renombrado si está en el mismo disco; si no, copia y borra el original
bool moveFile(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    if (ec != std::errc::cross_device_link) {
        error = "No se pudo mover " + from.filename().string() + ": " + ec.message();
        return false;
    }
    // Discos distintos (D-033 recomienda evitarlo): se copia a un temporal y se renombra al final
    const fs::path temporary = to.string() + ".parcial";
    fs::copy_file(from, temporary, fs::copy_options::overwrite_existing, ec);
    if (!ec) {
        fs::rename(temporary, to, ec);
    }
    if (ec) {
        error = "No se pudo copiar " + from.filename().string() + " a la biblioteca: " + ec.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    fs::remove(from, ec);
    return true;
}

// Coloca un fichero en "to". Si ya hay uno idéntico (mismo tamaño), se considera importado (así
// reintentar una importación a medias no duplica nada); si hay otro distinto, "Nombre (2).ext".
std::optional<fs::path> place(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec) {
        error = "No se pudo crear la carpeta " + to.parent_path().string() + ": " + ec.message();
        return std::nullopt;
    }
    fs::path target = to;
    for (int copy = 2; fs::exists(target, ec); ++copy) {
        if (fs::file_size(target, ec) == fs::file_size(from, ec)) {
            fs::remove(from, ec);
            return target;
        }
        if (copy > kMaxDuplicates) {
            error = "Demasiados archivos con el nombre " + to.filename().string();
            return std::nullopt;
        }
        target = to.parent_path() / (to.stem().string() + " (" + std::to_string(copy) + ")" + to.extension().string());
    }
    if (!moveFile(from, target, error)) {
        return std::nullopt;
    }
    return target;
}

// Mensaje claro para los fallos habituales de 7-Zip
std::string extractionError(const process::Result& result) {
    const std::string output = result.output;
    if (output.find("Unsupported Method") != std::string::npos) {
        return "7-Zip no puede descomprimir este formato. Para los RAR hace falta su códec: "
               "«sudo apt install 7zip-rar»";
    }
    if (output.find("Wrong password") != std::string::npos || output.find("encrypted") != std::string::npos) {
        return "El comprimido tiene contraseña";
    }
    if (output.find("Missing volume") != std::string::npos || output.find("Unexpected end") != std::string::npos ||
        output.find("Data Error") != std::string::npos || output.find("CRC Failed") != std::string::npos) {
        return "Faltan partes del comprimido o están dañadas";
    }
    // Última línea con texto de la salida
    std::string last;
    std::size_t end = output.find_last_not_of(" \r\n\b");
    if (end != std::string::npos) {
        const std::size_t start = output.find_last_of("\r\n\b", end);
        last = output.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
    }
    return "7-Zip terminó con el código " + std::to_string(result.exitCode) + (last.empty() ? "" : ": " + last);
}

}  // namespace

std::string sanitizeName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == ':') {
            out += " -";  // "Batman: El regreso" -> "Batman - El regreso"
        } else if (byte < 0x20 || byte == 0x7F || std::strchr("/\\*?\"<>|", c)) {
            out += ' ';
        } else {
            out += c;
        }
    }
    // Espacios repetidos fuera; ni espacios ni puntos al final (Windows y Samba no los admiten)
    static const std::regex kSpaces(R"( {2,})");
    out = std::regex_replace(out, kSpaces, " ");
    const auto first = out.find_first_not_of(' ');
    out = first == std::string::npos ? "" : out.substr(first);
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;  // No partir un carácter UTF-8
        }
        out.resize(cut);
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
    }
    return out.empty() ? "Sin título" : out;
}

std::string workFolderName(const WorkInfo& work) {
    return titleWithYear(work) + (work.tmdbId > 0 ? " [tmdbid-" + std::to_string(work.tmdbId) + "]" : "");
}

std::string seasonFolderName(int season) {
    return "Season " + pad2(season);
}

std::string movieFileName(const WorkInfo& work, const std::string& versionLabel, const std::string& extension) {
    return titleWithYear(work) + labelSuffix(versionLabel) + extension;
}

std::string episodeFileName(const WorkInfo& work, int season, int episode, int episodeEnd,
                            const std::string& versionLabel, const std::string& extension) {
    const std::string range = episodeEnd > episode ? "-E" + pad2(episodeEnd) : "";
    return sanitizeName(work.title) + " S" + pad2(season) + "E" + pad2(episode) + range + labelSuffix(versionLabel) +
           extension;
}

std::string versionLabel(const std::string& quality, bool hdr, const std::vector<std::string>& tags) {
    std::string label = quality == "2160p" ? "4K" : quality;
    if (hdr) {
        label += label.empty() ? "HDR" : " HDR";
    }
    for (const std::string& tag : tags) {
        label += (label.empty() ? "" : " ") + tag;
    }
    return label;
}

std::size_t removeFiles(const std::vector<std::string>& files, const std::vector<std::string>& roots,
                        const std::vector<std::string>& keep) {
    std::error_code ec;
    // Rutas reales (sin enlaces ni "..") de las raíces y de lo que hay que conservar
    std::vector<fs::path> realRoots;
    for (const std::string& root : roots) {
        if (!root.empty()) {
            const fs::path real = fs::canonical(root, ec);
            if (!ec && real != real.root_path()) {
                realRoots.push_back(real);
            }
        }
    }
    std::set<fs::path> protectedFiles;
    for (const std::string& file : keep) {
        protectedFiles.insert(fs::weakly_canonical(file, ec));
    }
    const auto rootOf = [&realRoots](const fs::path& path) -> const fs::path* {
        for (const fs::path& root : realRoots) {
            const auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
            if (mismatch.first == root.end() && mismatch.second != path.end()) {
                return &root;  // path está dentro de root (y no es root)
            }
        }
        return nullptr;
    };

    std::size_t removed = 0;
    for (const std::string& file : files) {
        if (file.empty() || fs::symlink_status(file, ec).type() != fs::file_type::regular) {
            continue;  // No existe, o es un enlace o una carpeta: no se toca
        }
        const fs::path real = fs::canonical(file, ec);
        const fs::path* root = ec ? nullptr : rootOf(real);
        if (!root || protectedFiles.count(real)) {
            continue;
        }
        if (!fs::remove(real, ec) || ec) {
            continue;
        }
        ++removed;
        // Carpetas vacías ("Season 01", la de la película...) hasta la raíz de la biblioteca, sin incluirla
        for (fs::path dir = real.parent_path(); dir != *root && rootOf(dir); dir = dir.parent_path()) {
            if (!fs::is_empty(dir, ec) || ec || !fs::remove(dir, ec)) {
                break;
            }
        }
    }
    return removed;
}

std::string findFfprobe() {
    for (const char* candidate : {"/usr/bin/ffprobe", "/usr/local/bin/ffprobe"}) {
        if (::access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    return "";
}

std::string qualityFromSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return "";
    }
    if (width >= 3200 || height >= 1800) {
        return "2160p";
    }
    if (width >= 1600 || height >= 900) {
        return "1080p";
    }
    if (width >= 1100 || height >= 650) {
        return "720p";
    }
    if (height >= 540) {
        return "576p";
    }
    return height >= 400 ? "480p" : "360p";
}

std::optional<VideoInfo> parseProbe(const std::string& output) {
    // La salida puede llevar avisos antes del JSON: se toma de la primera llave a la última
    const auto begin = output.find('{');
    const auto end = output.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        return std::nullopt;
    }
    const nlohmann::json doc = nlohmann::json::parse(output.substr(begin, end - begin + 1), nullptr, false);
    if (!doc.is_object() || !doc.contains("streams") || !doc["streams"].is_array() || doc["streams"].empty() ||
        !doc["streams"][0].is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& stream = doc["streams"][0];
    const auto number = [&stream](const char* field) {
        const auto it = stream.find(field);
        return it != stream.end() && it->is_number_integer() ? it->get<int>() : 0;
    };
    VideoInfo info;
    info.quality = qualityFromSize(number("width"), number("height"));
    if (info.quality.empty()) {
        return std::nullopt;
    }
    const auto transfer = stream.find("color_transfer");
    const std::string colorTransfer = transfer != stream.end() && transfer->is_string() ? transfer->get<std::string>() : "";
    bool dolbyVision = false;
    const auto sideData = stream.find("side_data_list");
    if (sideData != stream.end() && sideData->is_array()) {
        for (const nlohmann::json& entry : *sideData) {
            const std::string type = entry.is_object() ? entry.value("side_data_type", "") : "";
            dolbyVision = dolbyVision || type.find("DOVI") != std::string::npos ||
                          type.find("Dolby Vision") != std::string::npos;
        }
    }
    if (dolbyVision || colorTransfer == "smpte2084" || colorTransfer == "arib-std-b67") {
        info.hdr = true;  // HDR10/HDR10+ (PQ), HLG o Dolby Vision
    } else if (!colorTransfer.empty() && colorTransfer != "unknown") {
        info.hdr = false;
    }
    return info;
}

std::optional<VideoInfo> probeVideo(const std::string& path, const std::function<bool()>& shouldStop) {
    static const std::string ffprobe = findFfprobe();
    if (ffprobe.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    // "file:" y la ruta absoluta: ningún nombre se toma por una opción ni por otro protocolo
    const std::string input = "file:" + fs::absolute(path, ec).string();
    const process::Result run = process::run({ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                                              "stream=width,height,color_transfer:stream_side_data=side_data_type",
                                              "-of", "json", input},
                                             nullptr, shouldStop);
    if (!run.started || run.stopped || run.exitCode != 0) {
        return std::nullopt;
    }
    return parseProbe(run.output);
}

std::string findSevenZip() {
    for (const char* candidate : {"/usr/bin/7z", "/usr/bin/7zz", "/usr/local/bin/7z", "/usr/local/bin/7zz", "/usr/bin/7za"}) {
        if (::access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    return "";
}

ImportResult importRelease(const ImportRequest& request, const std::function<void(int)>& progress,
                           const std::function<bool()>& shouldStop) {
    ImportResult result;
    std::error_code ec;
    if (request.libraryRoot.empty()) {
        result.error = std::string("Define la biblioteca de ") + (request.kind == "series" ? "series" : "películas") +
                       " en Ajustes";
        return result;
    }
    if (request.parts.empty()) {
        result.error = "La descarga no tiene archivos";
        return result;
    }
    for (const std::string& part : request.parts) {
        if (!fs::exists(part, ec)) {
            result.error = "Falta el archivo descargado " + fs::path(part).filename().string();
            return result;
        }
    }

    const fs::path root(request.libraryRoot);
    const fs::path temporary = root / kTemporaryDir / std::to_string(request.id);
    std::vector<fs::path> files;

    // 1) Descomprimir en una carpeta temporal del mismo disco que la biblioteca
    if (request.archive) {
        const std::string sevenZip = findSevenZip();
        if (sevenZip.empty()) {
            result.error = "Falta 7-Zip para descomprimir: «sudo apt install 7zip»";
            return result;
        }
        std::uintmax_t archiveSize = 0;
        for (const std::string& part : request.parts) {
            archiveSize += fs::file_size(part, ec);
        }
        const auto space = fs::space(root, ec);
        if (!ec && static_cast<std::int64_t>(space.available) < static_cast<std::int64_t>(archiveSize) + request.minFreeBytes) {
            result.error = "No hay espacio en la biblioteca para descomprimir (hacen falta unos " +
                           std::to_string(archiveSize / 1'000'000'000 + 1) + " GB más el margen)";
            return result;
        }
        fs::remove_all(temporary, ec);
        fs::create_directories(temporary, ec);
        if (ec) {
            result.error = "No se pudo crear la carpeta temporal " + temporary.string() + ": " + ec.message();
            return result;
        }

        // "--": nada de lo que sigue es una opción, aunque el nombre empiece por "-"
        static const std::regex kPercent(R"((\d{1,3})%)");
        const process::Result run = process::run(
            {sevenZip, "x", "-y", "-bso0", "-bse1", "-bsp1", "-o" + temporary.string(), "--", request.parts.front()},
            [&progress](const std::string& chunk) {
                int percent = -1;
                for (auto it = std::sregex_iterator(chunk.begin(), chunk.end(), kPercent); it != std::sregex_iterator(); ++it) {
                    percent = std::stoi((*it)[1].str());
                }
                if (percent >= 0 && progress) {
                    progress(std::min(percent, 100));
                }
            },
            shouldStop);
        if (run.stopped) {
            fs::remove_all(temporary, ec);
            result.stopped = true;
            return result;
        }
        // 0 = bien; 1 = avisos que no impiden extraer
        if (!run.started || (run.exitCode != 0 && run.exitCode != 1)) {
            result.error = run.started ? extractionError(run) : run.output;
            fs::remove_all(temporary, ec);
            return result;
        }
        for (auto it = fs::recursive_directory_iterator(temporary, ec); it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) {
                files.push_back(it->path());
            }
        }
    } else {
        for (const std::string& part : request.parts) {
            files.emplace_back(part);
        }
    }

    // 2) Vídeos (sin muestras) y subtítulos
    std::vector<fs::path> videos;
    std::vector<fs::path> subtitles;
    std::uintmax_t largest = 0;
    for (const fs::path& file : files) {
        const std::string extension = lowerExtension(file);
        if (kVideoExtensions.count(extension)) {
            videos.push_back(file);
            largest = std::max(largest, fs::file_size(file, ec));
        } else if (kSubtitleExtensions.count(extension)) {
            subtitles.push_back(file);
        }
    }
    videos.erase(std::remove_if(videos.begin(), videos.end(),
                                [&](const fs::path& video) {
                                    const std::string name = lower(video.filename().string());
                                    const bool looksLikeSample = name.find("sample") != std::string::npos ||
                                                                 name.find("muestra") != std::string::npos;
                                    return looksLikeSample && fs::file_size(video, ec) < largest * kSampleRatio;
                                }),
                 videos.end());
    std::sort(videos.begin(), videos.end());
    if (videos.empty()) {
        result.error = "No hay ningún vídeo en lo descargado";
        fs::remove_all(temporary, ec);
        return result;
    }

    // 3) Destinos con nombres para Jellyfin. La resolución y el HDR, los del propio vídeo si ffprobe
    //    los puede leer (D-039); si no, los del nombre o la ficha.
    const fs::path workDir = root / sanitizeName(workFolderName(request.work));
    std::vector<std::pair<fs::path, fs::path>> moves;
    for (const fs::path& video : videos) {
        const std::string extension = lowerExtension(video);
        const std::optional<VideoInfo> probed = probeVideo(video.string(), shouldStop);
        const std::string quality = probed ? probed->quality : request.quality;
        const bool hdr = probed && probed->hdr ? *probed->hdr : request.hdr;
        const std::string versionLabel = library::versionLabel(quality, hdr, request.tags);
        if (probed && fs::file_size(video, ec) == largest) {
            result.probed = VideoInfo{quality, hdr};
        }
        fs::path target;
        if (request.kind == "series") {
            auto episode = media::parseEpisode(video.filename().string());
            if (!episode && videos.size() == 1 && request.episode > 0) {
                episode = media::EpisodeInfo{request.season, request.episode, request.episodeEnd, "", ""};
            }
            target = episode ? workDir / seasonFolderName(episode->season) /
                                   episodeFileName(request.work, episode->season, episode->episode, episode->episodeEnd,
                                                   versionLabel, extension)
                             : workDir / "extras" / sanitizeName(video.filename().string());
        } else {
            target = videos.size() == 1 ? workDir / movieFileName(request.work, versionLabel, extension)
                                        : workDir / sanitizeName(video.filename().string());
        }
        moves.emplace_back(video, target);
    }
    // Subtítulos sueltos, junto al vídeo si solo hay uno: "Peli (2020) - 1080p.Spanish.srt"
    if (videos.size() == 1) {
        const fs::path& videoTarget = moves.front().second;
        for (const fs::path& subtitle : subtitles) {
            moves.emplace_back(subtitle, videoTarget.parent_path() /
                                             (videoTarget.stem().string() + "." + sanitizeName(subtitle.stem().string()) +
                                              lowerExtension(subtitle)));
        }
    }

    // 4) Mover (un renombrado si el búfer y la biblioteca comparten disco)
    for (const auto& [from, to] : moves) {
        const auto placed = place(from, to, result.error);
        if (!placed) {
            return result;  // La carpeta temporal se queda: al reintentar se vuelve a extraer
        }
        result.files.push_back(placed->string());
    }

    // 5) Limpieza: partes del comprimido y carpeta temporal (los vídeos sueltos ya se han movido)
    if (request.archive) {
        for (const std::string& part : request.parts) {
            fs::remove(part, ec);
        }
        fs::remove_all(temporary, ec);
        fs::remove(root / kTemporaryDir, ec);  // Solo si ha quedado vacía
    }
    result.ok = true;
    result.libraryPath = workDir.string();
    return result;
}

}  // namespace library

#include "library.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <zlib.h>

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

const std::set<std::string> kVideoExtensions = {".mkv", ".mp4", ".avi",  ".m4v",  ".ts",   ".m2ts",
                                                ".wmv", ".mov", ".mpg", ".mpeg", ".webm", ".flv"};
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

std::optional<std::size_t> findVideoStart(const std::string& data) {
    std::optional<std::size_t> start;
    const auto earliest = [&start](std::size_t position) {
        if (!start || position < *start) {
            start = position;
        }
    };
    // Matroska/WebM: cabecera EBML (1A 45 DF A3) con su tipo de documento poco después
    static const std::string kEbml("\x1A\x45\xDF\xA3", 4);
    for (std::size_t at = data.find(kEbml); at != std::string::npos; at = data.find(kEbml, at + 1)) {
        const std::string head = data.substr(at, 64);
        if (head.find("matroska") != std::string::npos || head.find("webm") != std::string::npos) {
            earliest(at);
            break;
        }
    }
    // MP4/MOV: caja "ftyp", precedida de su tamaño (cuatro bytes, el primero a 0)
    for (std::size_t at = data.find("ftyp"); at != std::string::npos; at = data.find("ftyp", at + 1)) {
        if (at >= 4 && data[at - 4] == '\0') {
            earliest(at - 4);
            break;
        }
    }
    // FLV (hay vídeos Flash con extensión .mp4)
    for (std::size_t at = data.find("FLV\x01"); at != std::string::npos; at = data.find("FLV\x01", at + 1)) {
        earliest(at);
        break;
    }
    // AVI: "RIFF" + tamaño + "AVI "
    for (std::size_t at = data.find("RIFF"); at != std::string::npos; at = data.find("RIFF", at + 1)) {
        if (at + 12 <= data.size() && data.compare(at + 8, 4, "AVI ") == 0) {
            earliest(at);
            break;
        }
    }
    return start;
}

namespace {

// Lo que se descomprime como mucho del principio de un vídeo: basta para su cabecera
constexpr std::size_t kMaxInflated = 16'000'000;

std::uint32_t littleEndian(const std::string& data, std::size_t at, int bytes) {
    std::uint32_t value = 0;
    for (int i = bytes - 1; i >= 0; --i) {
        value = (value << 8) | static_cast<unsigned char>(data[at + static_cast<std::size_t>(i)]);
    }
    return value;
}

// Descomprime un flujo deflate (sin cabecera, como en ZIP) hasta donde lleguen los datos
std::string inflatePrefix(const std::string& compressed) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return "";
    }
    std::string out;
    char buffer[65536];
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    int status = Z_OK;
    while (status == Z_OK && out.size() < kMaxInflated) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        status = inflate(&stream, Z_NO_FLUSH);
        out.append(buffer, sizeof(buffer) - stream.avail_out);
        if (status == Z_BUF_ERROR || (stream.avail_in == 0 && stream.avail_out != 0)) {
            break;  // Se acabaron los datos descargados: vale con lo que haya salido
        }
    }
    inflateEnd(&stream);
    return out;
}

}  // namespace

std::optional<std::string> videoFromPrefix(const std::string& data, std::string& reason) {
    // ZIP: se recorren las cabeceras locales hasta la del vídeo (puede haber un .nfo antes)
    static const std::string kZipHeader("PK\x03\x04", 4);
    for (std::size_t at = 0; data.compare(0, 4, kZipHeader) == 0 && at + 30 <= data.size() &&
                             data.compare(at, 4, kZipHeader) == 0;) {
        const std::uint32_t flags = littleEndian(data, at + 6, 2);
        const std::uint32_t method = littleEndian(data, at + 8, 2);
        const std::uint32_t compressedSize = littleEndian(data, at + 18, 4);
        const std::size_t nameLength = littleEndian(data, at + 26, 2);
        const std::size_t extraLength = littleEndian(data, at + 28, 2);
        const std::size_t start = at + 30 + nameLength + extraLength;
        if (start > data.size()) {
            break;
        }
        const std::string name = data.substr(at + 30, nameLength);
        if (kVideoExtensions.count(lowerExtension(fs::path(name)))) {
            if (flags & 1) {
                reason = "El ZIP tiene contraseña";
                return std::nullopt;
            }
            std::string video;
            if (method == 0) {
                video = data.substr(start);
            } else if (method == 8) {
                video = inflatePrefix(data.substr(start));
            } else {
                reason = "El ZIP usa un método de compresión que no se puede leer por partes (" + std::to_string(method) + ")";
                return std::nullopt;
            }
            const auto videoStart = findVideoStart(video);
            if (!videoStart) {
                reason = "No se reconoce el vídeo dentro del ZIP";
                return std::nullopt;
            }
            return video.substr(*videoStart);
        }
        // Otro archivo antes del vídeo: se salta si su tamaño consta en la cabecera
        if ((flags & 8) || compressedSize == 0xFFFFFFFF) {
            break;
        }
        at = start + compressedSize;
    }

    // MKV, MP4 o AVI sueltos, o dentro de un comprimido que los guarda sin comprimir
    if (const auto start = findVideoStart(data)) {
        return data.substr(*start);
    }
    if (data.compare(0, 4, "Rar!") == 0) {
        reason = "El RAR comprime el vídeo";
        return std::nullopt;
    }
    // Para entender formatos nuevos: cómo empieza el archivo
    char hex[4];
    std::string head;
    for (std::size_t i = 0; i < std::min<std::size_t>(12, data.size()); ++i) {
        std::snprintf(hex, sizeof(hex), "%02X", static_cast<unsigned char>(data[i]));
        head += (i ? " " : "") + std::string(hex);
    }
    reason = "No se encuentra el vídeo en los primeros MB (el archivo empieza por " + head + ")";
    return std::nullopt;
}

std::optional<std::string> videoFromPrefixWith7z(const std::string& data, const std::string& extension,
                                                 std::string& reason) {
    const std::string sevenZip = findSevenZip();
    if (sevenZip.empty()) {
        reason = "Falta 7-Zip para leer el comprimido";
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path temporary = fs::temp_directory_path() / ("telegarrm-prefijo-" + std::to_string(::getpid()) + extension);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    // -so: lo descomprimido sale por la salida estándar; sin mensajes (-bs*0) no se mezcla con nada
    std::string video;
    bool enough = false;
    process::run({sevenZip, "e", "-so", "-bso0", "-bse0", "-bsp0", "-y", "--", temporary.string()},
                 [&](const std::string& chunk) {
                     video += chunk;
                     enough = video.size() >= kMaxInflated;
                 },
                 [&enough] { return enough; });
    fs::remove(temporary, ec);
    const auto start = findVideoStart(video);
    if (!start) {
        reason = "7-Zip no puede descomprimir el principio del comprimido";
        return std::nullopt;
    }
    return video.substr(*start);
}

std::optional<VideoInfo> probeVideo(const std::string& path, const std::function<bool()>& shouldStop, bool partial,
                                    std::string* diagnostics) {
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
    std::optional<VideoInfo> info;
    if (run.started && !run.stopped && (run.exitCode == 0 || partial)) {
        info = parseProbe(run.output);
    }
    if (!info && diagnostics) {
        // La primera línea con texto (sin la ruta del archivo, que no aporta)
        std::string text = run.output.substr(0, run.output.find('{'));
        const auto colon = text.find(": ");
        if (text.compare(0, 5, "file:") == 0 && colon != std::string::npos) {
            text = text.substr(colon + 2);
        }
        *diagnostics = text.substr(0, text.find('\n'));
    }
    return info;
}

bool isSubtitle(const std::string& path) {
    return kSubtitleExtensions.count(lowerExtension(fs::path(path))) > 0;
}

std::optional<VideoInfo> probeVideoCached(const std::string& path) {
    struct Entry {
        std::uintmax_t size = 0;
        fs::file_time_type modified;
        std::optional<VideoInfo> info;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    const fs::file_time_type modified = fs::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(path);
        if (it != cache.end() && it->second.size == size && it->second.modified == modified) {
            return it->second.info;
        }
    }
    const std::optional<VideoInfo> info = probeVideo(path);
    std::lock_guard<std::mutex> lock(mutex);
    cache[path] = {size, modified, info};
    return info;
}

std::string findWorkFolder(const std::string& root, const WorkInfo& work) {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) {
        return "";
    }
    const fs::path exact = fs::path(root) / sanitizeName(workFolderName(work));
    if (fs::is_directory(exact, ec)) {
        return exact.string();
    }
    if (work.tmdbId > 0) {
        // Jellyfin entiende "[tmdbid-N]" y "[tmdbid=N]"
        const std::string id = std::to_string(work.tmdbId);
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string name = lower(it->path().filename().string());
            if ((name.find("[tmdbid-" + id + "]") != std::string::npos ||
                 name.find("[tmdbid=" + id + "]") != std::string::npos) &&
                it->is_directory(ec)) {
                return it->path().string();
            }
        }
    }
    // Sin TMDB, carpetas como "Serie (2012)" o "Serie"
    for (const std::string& name : {titleWithYear(work), sanitizeName(work.title)}) {
        if (fs::is_directory(fs::path(root) / name, ec)) {
            return (fs::path(root) / name).string();
        }
    }
    return "";
}

std::vector<DiskVideo> videosOnDisk(const std::string& root, const WorkInfo& work) {
    std::vector<DiskVideo> videos;
    const std::string folder = findWorkFolder(root, work);
    if (folder.empty()) {
        return videos;
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            // "Season 01" sí; carpetas ocultas (temporales) y más profundas, no
            if (!name.empty() && name.front() == '.') {
                it.disable_recursion_pending();
            } else if (it.depth() >= 1) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec) || !kVideoExtensions.count(lowerExtension(it->path()))) {
            continue;
        }
        DiskVideo video;
        video.path = it->path().string();
        if (const auto episode = media::parseEpisode(name)) {
            video.season = episode->season;
            video.episode = episode->episode;
            video.episodeEnd = episode->episodeEnd;
        }
        video.quality = media::detectQuality(name);
        video.hdr = media::detectHdr(name);
        if (video.quality.empty()) {
            if (const auto probed = probeVideoCached(video.path)) {
                video.quality = probed->quality;
                video.hdr = probed->hdr.value_or(false);
            }
        }
        videos.push_back(std::move(video));
    }
    return videos;
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

    // 5) Otras versiones de lo importado en la misma carpeta (D-041): se devuelven para que el gestor
    //    de descargas las sustituya
    if (request.replaceOthers) {
        std::set<fs::path> placed;
        for (const std::string& file : result.files) {
            placed.insert(fs::path(file));
        }
        std::set<fs::path> superseded;
        for (std::size_t i = 0; i < videos.size() && i < result.files.size(); ++i) {
            const fs::path target(result.files[i]);  // Los vídeos van primero en moves
            const auto placedEpisode = media::parseEpisode(target.filename().string());
            if (request.kind == "series" && !placedEpisode) {
                continue;  // Extras
            }
            for (auto it = fs::directory_iterator(target.parent_path(), ec); !ec && it != fs::directory_iterator();
                 it.increment(ec)) {
                const fs::path other = it->path();
                if (placed.count(other) || !it->is_regular_file(ec) || !kVideoExtensions.count(lowerExtension(other))) {
                    continue;
                }
                if (request.kind == "series") {
                    const auto episode = media::parseEpisode(other.filename().string());
                    if (!episode || episode->season != placedEpisode->season ||
                        std::max(episode->episode, episode->episodeEnd) < placedEpisode->episode ||
                        episode->episode > std::max(placedEpisode->episode, placedEpisode->episodeEnd)) {
                        continue;  // Otro episodio
                    }
                }
                superseded.insert(other);
                // Sus subtítulos: "Serie S01E01 - 1080p.Spanish.srt"
                const std::string prefix = other.stem().string() + ".";
                for (auto sub = fs::directory_iterator(other.parent_path(), ec); !ec && sub != fs::directory_iterator();
                     sub.increment(ec)) {
                    const std::string name = sub->path().filename().string();
                    if (!placed.count(sub->path()) && name.compare(0, prefix.size(), prefix) == 0 &&
                        kSubtitleExtensions.count(lowerExtension(sub->path()))) {
                        superseded.insert(sub->path());
                    }
                }
            }
        }
        for (const fs::path& file : superseded) {
            result.superseded.push_back(file.string());
        }
    }

    // 6) Limpieza: partes del comprimido y carpeta temporal (los vídeos sueltos ya se han movido)
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

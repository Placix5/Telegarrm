#include "archive_index.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace archive {

namespace {

// Mismo criterio que la importación (library.cpp): una muestra pesa menos del 30 % del vídeo mayor
constexpr double kSampleRatio = 0.3;

std::uint64_t readLe(const std::string& data, std::size_t at, int bytes) {
    std::uint64_t value = 0;
    for (int i = bytes - 1; i >= 0; --i) {
        value = (value << 8) | static_cast<unsigned char>(data[at + static_cast<std::size_t>(i)]);
    }
    return value;
}

bool hasSignature(const std::string& data, std::int64_t at, const char* signature) {
    return at >= 0 && static_cast<std::size_t>(at) + 4 <= data.size() &&
           data.compare(static_cast<std::size_t>(at), 4, signature, 4) == 0;
}

bool validUtf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const int follow = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (follow < 0 || i + static_cast<std::size_t>(follow) >= text.size()) {
            return false;
        }
        for (int k = 1; k <= follow; ++k) {
            if ((static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]) >> 6) != 0x2) {
                return false;
            }
        }
        i += static_cast<std::size_t>(follow) + 1;
    }
    return true;
}

void appendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

std::string lowerExtension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    const auto slash = name.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return "";
    }
    std::string extension = name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

}  // namespace

std::string cp437ToUtf8(const std::string& text) {
    static const unsigned short kHigh[128] = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
        0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
        0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
        0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
        0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
        0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
        0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
        0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
        0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
        0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};
    std::string out;
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        appendUtf8(out, byte < 0x80 ? byte : kHigh[byte - 0x80]);
    }
    return out;
}

ZipIndex readZipIndex(const std::string& tail) {
    ZipIndex index;
    const auto size = static_cast<std::int64_t>(tail.size());
    // Fin del directorio central: "PK\5\6" y 22 bytes, más un comentario de hasta 64 kB
    std::int64_t end = -1;
    for (std::int64_t at = size - 22; at >= 0 && at >= size - 22 - 65535; --at) {
        if (hasSignature(tail, at, "PK\x05\x06") &&
            at + 22 + static_cast<std::int64_t>(readLe(tail, static_cast<std::size_t>(at) + 20, 2)) <= size) {
            end = at;
            break;
        }
    }
    if (end < 0) {
        index.error = "No tiene el índice de un ZIP al final";
        return index;
    }
    const auto at = static_cast<std::size_t>(end);
    std::uint64_t count = readLe(tail, at + 10, 2);
    std::uint64_t directorySize = readLe(tail, at + 12, 4);
    std::int64_t directoryEnd = end;
    // ZIP64 (más de 4 GB o de 65 535 archivos): el registro de verdad va justo antes, con su localizador
    if (count == 0xFFFF || directorySize == 0xFFFFFFFF || readLe(tail, at + 16, 4) == 0xFFFFFFFF) {
        const std::int64_t locator = end - 20;
        const std::int64_t record = locator - 56;
        if (!hasSignature(tail, locator, "PK\x06\x07") || !hasSignature(tail, record, "PK\x06\x06")) {
            index.error = "Índice ZIP64 incompleto";
            return index;
        }
        count = readLe(tail, static_cast<std::size_t>(record) + 32, 8);
        directorySize = readLe(tail, static_cast<std::size_t>(record) + 40, 8);
        directoryEnd = record;
    }
    // El directorio central va justo antes de su final
    const std::int64_t start = directoryEnd - static_cast<std::int64_t>(directorySize);
    if (start < 0) {
        const std::int64_t need = size - start;
        if (need > kZipMaxTailBytes) {
            index.error = "El índice del ZIP es demasiado grande";
        } else {
            index.needTail = need;
        }
        return index;
    }

    std::size_t pos = static_cast<std::size_t>(start);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!hasSignature(tail, static_cast<std::int64_t>(pos), "PK\x01\x02") ||
            pos + 46 > static_cast<std::size_t>(directoryEnd)) {
            index.error = "Índice del ZIP dañado";
            return index;
        }
        const auto flags = readLe(tail, pos + 8, 2);
        std::uint64_t unpacked = readLe(tail, pos + 24, 4);
        const auto nameLength = static_cast<std::size_t>(readLe(tail, pos + 28, 2));
        const auto extraLength = static_cast<std::size_t>(readLe(tail, pos + 30, 2));
        const auto commentLength = static_cast<std::size_t>(readLe(tail, pos + 32, 2));
        const std::size_t next = pos + 46 + nameLength + extraLength + commentLength;
        if (next > static_cast<std::size_t>(directoryEnd)) {
            index.error = "Índice del ZIP dañado";
            return index;
        }
        std::string name = tail.substr(pos + 46, nameLength);
        bool unicode = (flags & 0x800) != 0;  // Nombre en UTF-8
        // Campos extra: tamaño ZIP64 (0x0001) y nombre en UTF-8 de Info-ZIP (0x7075)
        std::size_t extra = pos + 46 + nameLength;
        const std::size_t extraEnd = extra + extraLength;
        while (extra + 4 <= extraEnd) {
            const auto id = readLe(tail, extra, 2);
            const auto length = static_cast<std::size_t>(readLe(tail, extra + 2, 2));
            if (extra + 4 + length > extraEnd) {
                break;
            }
            if (id == 0x0001 && unpacked == 0xFFFFFFFF && length >= 8) {
                unpacked = readLe(tail, extra + 4, 8);
            } else if (id == 0x7075 && length > 5) {
                name = tail.substr(extra + 9, length - 5);
                unicode = true;
            }
            extra += 4 + length;
        }
        if (!unicode && !validUtf8(name)) {
            name = cp437ToUtf8(name);
        }
        std::replace(name.begin(), name.end(), '\\', '/');
        if (!name.empty() && name.back() != '/') {
            index.entries.push_back({name, static_cast<std::int64_t>(unpacked)});
        }
        pos = next;
    }
    index.ok = true;
    return index;
}

bool isZipName(const std::string& fileName) {
    static const std::regex kZip(R"(\.zip(?:\.\d{3})?$)", std::regex::icase);
    return std::regex_search(fileName, kZip);
}

bool isVideoName(const std::string& fileName) {
    static const std::set<std::string> kVideoExtensions = {".mkv", ".mp4", ".avi",  ".m4v",  ".ts",   ".m2ts",
                                                           ".wmv", ".mov", ".mpg", ".mpeg", ".webm", ".flv"};
    return kVideoExtensions.count(lowerExtension(fileName)) > 0;
}

std::vector<Entry> videoEntries(const std::vector<Entry>& entries) {
    std::vector<Entry> videos;
    std::int64_t largest = 0;
    for (const Entry& entry : entries) {
        if (isVideoName(entry.name)) {
            videos.push_back(entry);
            largest = std::max(largest, entry.size);
        }
    }
    videos.erase(std::remove_if(videos.begin(), videos.end(),
                                [largest](const Entry& video) {
                                    std::string name = video.name;
                                    std::transform(name.begin(), name.end(), name.begin(),
                                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    const bool sample = name.find("sample") != std::string::npos ||
                                                        name.find("muestra") != std::string::npos;
                                    return sample && static_cast<double>(video.size) < largest * kSampleRatio;
                                }),
                 videos.end());
    return videos;
}

}  // namespace archive

#include "archive_index.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "process.hpp"

namespace archive {

namespace {

// Mismo criterio que la importación (library.cpp): una muestra pesa menos del 30 % del vídeo mayor
constexpr double kSampleRatio = 0.3;
// RAR: lo que se pide de cada vez (una cabecera ocupa unos cientos de bytes; TDLib baja trozos mayores)
constexpr std::int64_t kRarChunk = 16384;
// Una cabecera más grande o tantos bloques solo pueden ser un archivo dañado (o no un RAR)
constexpr std::int64_t kRarMaxHeader = 1'000'000;
constexpr int kRarMaxBlocks = 20000;

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

// Número de tamaño variable de RAR 5: 7 bits por byte, el bit alto dice si sigue
bool readVint(const std::string& data, std::size_t& at, std::uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64 && at < data.size(); shift += 7) {
        const auto byte = static_cast<unsigned char>(data[at++]);
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

// Número de 7z: los bits altos del primer byte dicen cuántos bytes más lleva
bool readSevenZipNumber(const std::string& data, std::size_t& at, std::uint64_t& value) {
    if (at >= data.size()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(data[at++]);
    value = 0;
    unsigned mask = 0x80;
    for (int i = 0; i < 8; ++i) {
        if ((first & mask) == 0) {
            value |= static_cast<std::uint64_t>(first & (mask - 1)) << (8 * i);
            return true;
        }
        if (at >= data.size()) {
            return false;
        }
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[at++])) << (8 * i);
        mask >>= 1;
    }
    return true;
}

// Nombre de RAR 4 con la marca de Unicode: el nombre en ASCII, un 0 y el nombre real codificado
// respecto a él (el algoritmo de unrar, EncodeFileName::Decode)
std::string decodeRar4Unicode(const std::string& ascii, const std::string& encoded) {
    std::vector<unsigned> name;
    std::size_t pos = 0;
    const auto next = [&]() -> unsigned { return pos < encoded.size() ? static_cast<unsigned char>(encoded[pos++]) : 0; };
    const unsigned high = next();
    unsigned flags = 0;
    int flagBits = 0;
    while (pos < encoded.size() && name.size() < 1024) {
        if (flagBits == 0) {
            flags = next();
            flagBits = 8;
        }
        switch (flags >> 6) {
            case 0:
                name.push_back(next());
                break;
            case 1:
                name.push_back(next() + (high << 8));
                break;
            case 2: {
                const unsigned low = next();
                name.push_back(low + (next() << 8));
                break;
            }
            default: {
                unsigned length = next();
                if (length & 0x80) {
                    const unsigned correction = next();
                    for (length = (length & 0x7F) + 2; length > 0; --length) {
                        const std::size_t i = name.size();
                        const unsigned c = i < ascii.size() ? static_cast<unsigned char>(ascii[i]) : 0;
                        name.push_back(((c + correction) & 0xFF) + (high << 8));
                    }
                } else {
                    for (length += 2; length > 0; --length) {
                        const std::size_t i = name.size();
                        name.push_back(i < ascii.size() ? static_cast<unsigned char>(ascii[i]) : 0);
                    }
                }
                break;
            }
        }
        flags = (flags << 2) & 0xFF;
        flagBits -= 2;
    }
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        unsigned code = name[i];
        // Pares sustitutos de UTF-16 (caracteres fuera del plano básico)
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < name.size() && name[i + 1] >= 0xDC00 && name[i + 1] < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (name[++i] - 0xDC00);
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code != 0) {
            appendUtf8(out, code);
        }
    }
    return out;
}

// Lecturas de un RAR con un pequeño búfer: varias cabeceras seguidas (subtítulos, .nfo) salen de una
class ChunkReader {
public:
    ChunkReader(const Reader& read, std::int64_t size) : read_(read), size_(size) {}

    // Al menos `length` bytes desde `offset` (menos si se acaba el archivo); nullopt si falla la lectura
    std::optional<std::string> at(std::int64_t offset, std::int64_t length) {
        length = std::min(length, size_ - offset);
        if (offset < offset_ || offset + length > offset_ + static_cast<std::int64_t>(data_.size())) {
            const std::int64_t want = std::min(std::max(length, kRarChunk), size_ - offset);
            std::optional<std::string> data = read_(offset, want);
            if (!data || static_cast<std::int64_t>(data->size()) != want) {
                return std::nullopt;
            }
            offset_ = offset;
            data_ = std::move(*data);
        }
        return data_.substr(static_cast<std::size_t>(offset - offset_));
    }

private:
    const Reader& read_;
    std::int64_t size_;
    std::int64_t offset_ = 0;
    std::string data_;
};

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

Format formatOf(const std::string& fileName) {
    static const std::regex kRar(R"(\.rar(?:\.\d{3})?$)", std::regex::icase);
    static const std::regex kSevenZip(R"(\.7z(?:\.\d{3})?$)", std::regex::icase);
    if (isZipName(fileName)) {
        return Format::Zip;
    }
    if (std::regex_search(fileName, kRar)) {
        return Format::Rar;
    }
    if (std::regex_search(fileName, kSevenZip)) {
        return Format::SevenZip;
    }
    return Format::None;
}

Listing listZip(std::int64_t size, const Reader& read) {
    Listing listing;
    std::optional<std::string> tail = read(size - std::min(size, kZipTailBytes), std::min(size, kZipTailBytes));
    if (!tail) {
        listing.readFailed = true;
        return listing;
    }
    ZipIndex index = readZipIndex(*tail);
    if (index.needTail > 0 && index.needTail <= size) {
        tail = read(size - index.needTail, index.needTail);
        if (!tail) {
            listing.readFailed = true;
            return listing;
        }
        index = readZipIndex(*tail);
    }
    listing.ok = index.ok;
    listing.entries = std::move(index.entries);
    listing.error = index.ok ? "" : index.error.empty() ? "El índice del ZIP no cabe en lo leído" : index.error;
    return listing;
}

Listing listRar(const std::vector<std::int64_t>& partSizes, const Reader& read) {
    Listing listing;
    std::vector<std::int64_t> partEnds;  // Dónde acaba cada parte en el archivo entero
    std::int64_t size = 0;
    for (const std::int64_t partSize : partSizes) {
        size += partSize;
        partEnds.push_back(size);
    }
    const auto fail = [&listing](const std::string& error) {
        listing.error = error;
        listing.entries.clear();
        return listing;
    };
    ChunkReader reader(read, size);
    int version = 0;
    std::int64_t pos = 0;
    for (int blocks = 0; pos < size; ++blocks) {
        if (blocks >= kRarMaxBlocks) {
            return fail("Demasiados bloques: el RAR parece dañado");
        }
        std::optional<std::string> data = reader.at(pos, 64);
        if (!data) {
            listing.readFailed = true;
            return listing;
        }
        // Firma: al principio y al de cada volumen (.part2.rar...)
        if (data->compare(0, 6, "Rar!\x1a\x07", 6) == 0 && data->size() >= 8) {
            version = (*data)[6] == '\x01' && (*data)[7] == '\x00' ? 5 : (*data)[6] == '\x00' ? 4 : 0;
            if (version == 0) {
                return fail("Versión de RAR desconocida");
            }
            pos += version == 5 ? 8 : 7;
            continue;
        }
        if (version == 0) {
            return fail("No es un RAR");
        }
        std::int64_t next = 0;
        bool endOfArchive = false;
        if (version == 5) {
            // CRC32 (4), tamaño de la cabecera, y la cabecera: tipo, banderas, [extra], [datos]...
            std::size_t at = 4;
            std::uint64_t headerSize = 0;
            if (!readVint(*data, at, headerSize) || headerSize == 0 || headerSize > kRarMaxHeader) {
                return fail("Cabecera de RAR dañada");
            }
            const std::int64_t total = static_cast<std::int64_t>(at + headerSize);
            data = reader.at(pos, total);
            if (!data) {
                listing.readFailed = true;
                return listing;
            }
            if (static_cast<std::int64_t>(data->size()) < total) {
                return fail("El RAR está cortado");
            }
            data->resize(static_cast<std::size_t>(total));
            std::uint64_t type = 0, flags = 0, extraSize = 0, dataSize = 0;
            if (!readVint(*data, at, type) || !readVint(*data, at, flags) ||
                ((flags & 0x1) && !readVint(*data, at, extraSize)) || ((flags & 0x2) && !readVint(*data, at, dataSize))) {
                return fail("Cabecera de RAR dañada");
            }
            if (type == 4) {
                return fail("RAR con los nombres cifrados: no se puede ver qué lleva");
            }
            if (type == 2 && (flags & 0x8) == 0) {  // Archivo, salvo la continuación de uno del volumen anterior
                std::uint64_t fileFlags = 0, unpacked = 0, attributes = 0, compression = 0, hostOs = 0, nameLength = 0;
                if (!readVint(*data, at, fileFlags) || !readVint(*data, at, unpacked) ||
                    !readVint(*data, at, attributes)) {
                    return fail("Cabecera de RAR dañada");
                }
                at += ((fileFlags & 0x2) ? 4 : 0) + ((fileFlags & 0x4) ? 4 : 0);  // Fecha y CRC32
                if (!readVint(*data, at, compression) || !readVint(*data, at, hostOs) ||
                    !readVint(*data, at, nameLength) || at + nameLength > data->size()) {
                    return fail("Cabecera de RAR dañada");
                }
                if ((fileFlags & 0x1) == 0) {  // Las carpetas no
                    const std::int64_t entrySize = (fileFlags & 0x8) ? 0 : static_cast<std::int64_t>(unpacked);
                    listing.entries.push_back({data->substr(at, static_cast<std::size_t>(nameLength)), entrySize});
                }
            }
            endOfArchive = type == 5;
            next = pos + total + static_cast<std::int64_t>(dataSize);
        } else {
            // RAR 1.5-4: CRC (2), tipo (1), banderas (2), tamaño (2) y, con la bandera 0x8000, datos (4)
            if (data->size() < 7) {
                return fail("El RAR está cortado");
            }
            const auto type = static_cast<unsigned char>((*data)[2]);
            const auto flags = readLe(*data, 3, 2);
            const auto headerSize = static_cast<std::int64_t>(readLe(*data, 5, 2));
            if (headerSize < 7) {
                return fail("Cabecera de RAR dañada");
            }
            data = reader.at(pos, headerSize);
            if (!data) {
                listing.readFailed = true;
                return listing;
            }
            if (static_cast<std::int64_t>(data->size()) < headerSize) {
                return fail("El RAR está cortado");
            }
            std::uint64_t dataSize = (flags & 0x8000) && headerSize >= 11 ? readLe(*data, 7, 4) : 0;
            if (type == 0x73 && (flags & 0x0080)) {
                return fail("RAR con los nombres cifrados: no se puede ver qué lleva");
            }
            if (type == 0x74 || type == 0x7A) {  // Archivo o subbloque: tamaños de 64 bits con la bandera 0x100
                const bool large = (flags & 0x100) != 0;
                const std::size_t nameAt = large ? 40 : 32;
                if (headerSize < static_cast<std::int64_t>(nameAt)) {
                    return fail("Cabecera de RAR dañada");
                }
                std::uint64_t unpacked = readLe(*data, 11, 4);
                if (large) {
                    dataSize |= readLe(*data, 32, 4) << 32;
                    unpacked |= readLe(*data, 36, 4) << 32;
                }
                const auto nameLength = static_cast<std::size_t>(readLe(*data, 26, 2));
                const bool directory = (flags & 0xE0) == 0xE0;
                if (type == 0x74 && (flags & 0x01) == 0 && !directory) {  // Sin las continuaciones ni carpetas
                    if (nameAt + nameLength > data->size()) {
                        return fail("Cabecera de RAR dañada");
                    }
                    std::string name = data->substr(nameAt, nameLength);
                    const auto zero = name.find('\0');
                    if ((flags & 0x200) && zero != std::string::npos) {
                        name = decodeRar4Unicode(name.substr(0, zero), name.substr(zero + 1));
                    } else if (!(flags & 0x200) && !validUtf8(name)) {
                        name = cp437ToUtf8(name);
                    }
                    std::replace(name.begin(), name.end(), '\\', '/');
                    listing.entries.push_back({name, static_cast<std::int64_t>(unpacked)});
                }
            }
            endOfArchive = type == 0x7B;
            next = pos + headerSize + static_cast<std::int64_t>(dataSize);
        }
        if (endOfArchive) {
            // Fin de un volumen: el siguiente empieza con la siguiente parte (y si no hay más, se acabó)
            const auto end = std::upper_bound(partEnds.begin(), partEnds.end(), pos);
            next = end == partEnds.end() ? size : *end;
        }
        if (next <= pos) {
            return fail("Cabecera de RAR dañada");
        }
        pos = next;
    }
    if (version == 0) {
        return fail("No es un RAR");
    }
    listing.ok = true;
    return listing;
}

std::optional<SevenZipStart> readSevenZipStart(const std::string& first32) {
    if (first32.size() < 32 || first32.compare(0, 6, "7z\xbc\xaf\x27\x1c", 6) != 0) {
        return std::nullopt;
    }
    SevenZipStart start;
    start.headerOffset = 32 + static_cast<std::int64_t>(readLe(first32, 12, 8));
    start.headerSize = static_cast<std::int64_t>(readLe(first32, 20, 8));
    if (start.headerOffset < 32 || start.headerSize < 0) {
        return std::nullopt;
    }
    return start;
}

std::optional<std::pair<std::int64_t, std::int64_t>> sevenZipPackedHeader(const std::string& header) {
    // kEncodedHeader (0x17), kPackInfo (0x06): posición y número de flujos, y kSize (0x09) con sus tamaños
    if (header.size() < 2 || header[0] != '\x17' || header[1] != '\x06') {
        return std::nullopt;
    }
    std::size_t at = 2;
    std::uint64_t packPos = 0, streams = 0;
    if (!readSevenZipNumber(header, at, packPos) || !readSevenZipNumber(header, at, streams) || streams == 0 ||
        streams > 64 || at >= header.size() || header[at++] != '\x09') {
        return std::nullopt;
    }
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < streams; ++i) {
        std::uint64_t size = 0;
        if (!readSevenZipNumber(header, at, size)) {
            return std::nullopt;
        }
        total += size;
    }
    return std::make_pair(32 + static_cast<std::int64_t>(packPos), static_cast<std::int64_t>(total));
}

std::vector<Entry> parseSevenZipList(const std::string& output) {
    std::vector<Entry> entries;
    std::istringstream in(output);
    std::string line;
    std::map<std::string, std::string> fields;
    const auto flush = [&] {
        const auto path = fields.find("Path");
        const std::string attributes = fields.count("Attributes") ? fields["Attributes"] : "";
        const bool folder = fields["Folder"] == "+" || (!attributes.empty() && attributes[0] == 'D');
        if (path != fields.end() && !path->second.empty() && !folder) {
            std::int64_t size = 0;
            try {
                size = fields["Size"].empty() ? 0 : std::stoll(fields["Size"]);
            } catch (const std::exception&) {
            }
            std::string name = path->second;
            std::replace(name.begin(), name.end(), '\\', '/');
            entries.push_back({name, size});
        }
        fields.clear();
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            flush();
            continue;
        }
        const auto equals = line.find(" = ");
        if (equals != std::string::npos) {
            fields[line.substr(0, equals)] = line.substr(equals + 3);
        } else if (line.size() > 2 && line.compare(line.size() - 2, 2, " =") == 0) {
            fields[line.substr(0, line.size() - 2)] = "";
        }
    }
    flush();
    return entries;
}

Listing listSevenZip(std::int64_t size, const Reader& read, const std::string& sevenZip, const std::string& tempDir) {
    namespace fs = std::filesystem;
    Listing listing;
    if (sevenZip.empty()) {
        listing.readFailed = true;
        listing.error = "Falta 7-Zip para leer el índice de los 7z";
        return listing;
    }
    // Trozos que hay que tener: el principio, el índice y, si está comprimido, sus datos
    std::vector<std::pair<std::int64_t, std::string>> pieces;
    const auto piece = [&](std::int64_t offset, std::int64_t length) -> const std::string* {
        std::optional<std::string> data = read(offset, length);
        if (!data) {
            listing.readFailed = true;
            return nullptr;
        }
        pieces.emplace_back(offset, std::move(*data));
        return &pieces.back().second;
    };
    const std::string* first = piece(0, std::min<std::int64_t>(size, 32));
    if (!first) {
        return listing;
    }
    const std::optional<SevenZipStart> start = readSevenZipStart(*first);
    if (!start) {
        listing.error = "No es un 7z";
        return listing;
    }
    if (start->headerSize == 0) {
        listing.ok = true;  // Vacío
        return listing;
    }
    if (start->headerSize > kZipMaxTailBytes || start->headerOffset + start->headerSize > size) {
        listing.error = start->headerOffset + start->headerSize > size ? "El 7z está cortado"
                                                                        : "El índice del 7z es demasiado grande";
        return listing;
    }
    const std::string* header = piece(start->headerOffset, start->headerSize);
    if (!header) {
        return listing;
    }
    if (const auto packed = sevenZipPackedHeader(*header)) {
        if (packed->second > kZipMaxTailBytes || packed->first + packed->second > start->headerOffset) {
            listing.error = "El índice del 7z es demasiado grande";
            return listing;
        }
        if (!piece(packed->first, packed->second)) {
            return listing;
        }
    }

    // Copia dispersa: del tamaño del archivo, pero solo con esos trozos (no ocupa más que ellos)
    static std::atomic<int> counter{0};
    const fs::path copy = fs::path(tempDir) / ("telegarrm-indice-" + std::to_string(::getpid()) + "-" +
                                               std::to_string(counter++) + ".7z");
    {
        std::ofstream out(copy, std::ios::binary | std::ios::trunc);
        for (const auto& [offset, data] : pieces) {
            out.seekp(offset);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        if (!out) {
            listing.readFailed = true;
            listing.error = "No se pudo escribir en " + tempDir;
        }
    }
    std::error_code ec;
    if (!listing.readFailed) {
        fs::resize_file(copy, static_cast<std::uintmax_t>(size), ec);
    }
    if (!listing.readFailed && !ec) {
        // Una contraseña cualquiera: con los nombres cifrados falla en vez de preguntarla. La salida se
        // guarda entera (process::run solo se queda con el final)
        std::string output;
        const process::Result result = process::run(
            {sevenZip, "l", "-slt", "-ba", "-pX", "--", copy.string()},
            [&output](const std::string& chunk) {
                if (output.size() < static_cast<std::size_t>(kZipMaxTailBytes) * 4) {
                    output += chunk;
                }
            },
            nullptr);
        if (result.exitCode == 0) {
            listing.ok = true;
            listing.entries = parseSevenZipList(output);
        } else if (result.output.find("Wrong password") != std::string::npos) {
            listing.error = "7z con los nombres cifrados: no se puede ver qué lleva";
        } else {
            listing.error = "7-Zip no entiende el índice del 7z";
        }
    } else if (ec) {
        listing.readFailed = true;
        listing.error = "No se pudo escribir en " + tempDir + ": " + ec.message();
    }
    fs::remove(copy, ec);
    return listing;
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

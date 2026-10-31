#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Qué hay dentro de un comprimido sin descargarlo (docs/DECISIONS.md, D-057 y D-060), leyendo solo
// unos kB de sitios concretos:
// - ZIP: guarda su índice (el directorio central) al final.
// - 7z: los 32 primeros bytes dicen dónde está su índice, también al final, casi siempre comprimido:
//   lo lee 7-Zip de una copia dispersa del archivo con solo esos trozos.
// - RAR: no tiene índice; cada archivo lleva su cabecera delante de sus datos, y se salta de una a otra.
// Las lecturas las hace quien llama (ArchiveLister, con Telegram) a través de un Reader.
namespace archive {

struct Entry {
    std::string name;       // Ruta dentro del comprimido, en UTF-8 ("Temporada 1/Serie 1x01.mkv")
    std::int64_t size = 0;  // Bytes sin comprimir
};

// Lo que se pide primero del final de un ZIP: basta para el índice de unos cientos de archivos
constexpr std::int64_t kZipTailBytes = 65536;
// Más no se pide nunca: un índice así de grande no es de una temporada
constexpr std::int64_t kZipMaxTailBytes = 4'000'000;

struct ZipIndex {
    bool ok = false;
    std::string error;           // En castellano, si !ok y needTail == 0
    std::int64_t needTail = 0;   // El índice no cabe en lo leído: hacen falta estos bytes del final
    std::vector<Entry> entries;  // Solo archivos, sin carpetas
};

// tail: los últimos bytes del ZIP (todo él, si es pequeño)
ZipIndex readZipIndex(const std::string& tail);

// ¿Es un ZIP por su nombre? "Serie.zip", "Serie.zip.001"
bool isZipName(const std::string& fileName);

enum class Format { None, Zip, Rar, SevenZip };
// Por el nombre de su primera parte: .zip(.001), .rar, .part1.rar, .rar.001, .7z(.001)
Format formatOf(const std::string& fileName);

// Lee `length` bytes desde `offset` del comprimido entero (sus partes una detrás de otra). nullopt si
// no se pudo (la red): entonces no se sabe nada del archivo y se volverá a intentar.
using Reader = std::function<std::optional<std::string>(std::int64_t offset, std::int64_t length)>;

struct Listing {
    bool ok = false;
    bool readFailed = false;     // Falló una lectura (o falta 7-Zip): no se guarda nada
    std::string error;           // Si !ok, en castellano: un fallo del propio archivo, que se recuerda
    std::vector<Entry> entries;  // Solo archivos, sin carpetas
};

// size: el del comprimido entero
Listing listZip(std::int64_t size, const Reader& read);
// partSizes: los de sus partes, en orden. Valen los volúmenes (.part1.rar, .part2.rar...), cada uno con
// su propia firma, y un RAR cortado en trozos (.rar.001...)
Listing listRar(const std::vector<std::int64_t>& partSizes, const Reader& read);
// sevenZip: ruta del programa (library::findSevenZip()); tempDir: dónde dejar un momento la copia dispersa
Listing listSevenZip(std::int64_t size, const Reader& read, const std::string& sevenZip, const std::string& tempDir);

// Lo que se lee del final de un 7z: dónde está su índice (relativo al principio del archivo). Del primer
// bloque de 32 bytes; nullopt si no es un 7z.
struct SevenZipStart {
    std::int64_t headerOffset = 0;
    std::int64_t headerSize = 0;
};
std::optional<SevenZipStart> readSevenZipStart(const std::string& first32);
// Si el índice está comprimido, dónde están sus datos comprimidos: (posición, bytes). nullopt si no lo está
// o no se entiende.
std::optional<std::pair<std::int64_t, std::int64_t>> sevenZipPackedHeader(const std::string& header);
// Los archivos de la salida de «7z l -slt»
std::vector<Entry> parseSevenZipList(const std::string& output);
// ¿Es un vídeo por su extensión?
bool isVideoName(const std::string& fileName);
// Los vídeos de un comprimido, sin las muestras ("sample.mkv" diminuto junto a los episodios)
std::vector<Entry> videoEntries(const std::vector<Entry>& entries);

// Nombres de ZIP sin la marca de UTF-8: los de Windows van en la página de códigos 437
std::string cp437ToUtf8(const std::string& text);

}  // namespace archive

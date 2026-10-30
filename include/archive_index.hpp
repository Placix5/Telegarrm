#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Qué hay dentro de un comprimido sin descargarlo (docs/DECISIONS.md, D-057). Un ZIP guarda su índice
// (el directorio central) al final: con los últimos kB del archivo, o de su última parte si está
// troceado (.zip.001, .zip.002...), se saben los nombres y tamaños de todo lo que lleva. Funciones
// puras: la descarga de ese final la hace ArchiveLister.
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
// ¿Es un vídeo por su extensión?
bool isVideoName(const std::string& fileName);
// Los vídeos de un comprimido, sin las muestras ("sample.mkv" diminuto junto a los episodios)
std::vector<Entry> videoEntries(const std::vector<Entry>& entries);

// Nombres de ZIP sin la marca de UTF-8: los de Windows van en la página de códigos 437
std::string cp437ToUtf8(const std::string& text);

}  // namespace archive

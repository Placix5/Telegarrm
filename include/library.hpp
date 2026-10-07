#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Importación a la biblioteca (Fase 3, docs/DECISIONS.md D-034): descomprime lo descargado si
// hace falta, elige los vídeos (y sus subtítulos) y los mueve con nombres que Jellyfin reconoce:
//   peliculas/Título (Año) [tmdbid-N]/Título (Año) - 1080p.mkv
//   series/Serie (Año) [tmdbid-N]/Season 01/Serie S01E01 - 1080p.mkv
namespace library {

// Datos de la obra para los nombres (de TMDB si los hay; si no, del catálogo)
struct WorkInfo {
    std::string title;
    std::optional<int> year;
    long tmdbId = 0;
};

struct ImportRequest {
    std::int64_t id = 0;                // Descarga: nombre de la carpeta temporal
    std::string kind;                   // "series" o "movie"
    WorkInfo work;
    int season = 0;                     // Episodio del archivo lógico (0 = sin episodio)
    int episode = 0;
    int episodeEnd = 0;
    // Versión según el nombre o la ficha. La resolución y el HDR los corrige lo que diga el propio
    // vídeo (ffprobe, D-039); las etiquetas (REMUX...) salen solo del nombre.
    std::string quality;
    bool hdr = false;
    std::vector<std::string> tags;
    bool archive = false;               // Hay que descomprimir
    std::vector<std::string> parts;     // Rutas locales, en orden (la primera abre el comprimido)
    std::string libraryRoot;            // Biblioteca de películas o de series
    std::int64_t minFreeBytes = 0;      // Espacio que debe quedar libre tras descomprimir
    // Buscar en la carpeta de destino otras versiones de lo importado (D-041): en series, los otros
    // vídeos de los mismos episodios; en películas, el resto de vídeos de la película
    bool replaceOthers = false;
};

// Calidad real de un vídeo, leída con ffprobe (D-039)
struct VideoInfo {
    std::string quality;      // "2160p", "1080p"...
    std::optional<bool> hdr;  // std::nullopt = ffprobe no da información de color
};

struct ImportResult {
    bool ok = false;
    bool stopped = false;               // Interrumpida (parada del servicio o cancelación)
    std::string error;                  // En castellano
    std::string libraryPath;            // Carpeta de la obra en la biblioteca
    std::vector<std::string> files;     // Archivos colocados
    std::optional<VideoInfo> probed;    // Calidad real del vídeo principal (el mayor), si se pudo leer
    // Versiones anteriores encontradas (con replaceOthers), con sus subtítulos. No se borran aquí.
    std::vector<std::string> superseded;
};

// Vídeo que ya está en la carpeta de una obra en la biblioteca (D-041)
struct DiskVideo {
    std::string path;
    int season = 0;      // 0 = sin episodio (películas, extras)
    int episode = 0;
    int episodeEnd = 0;
    std::string quality; // De la etiqueta del nombre ("- 720p") o, si no la lleva, del propio vídeo
    bool hdr = false;
};

// Carpeta de una obra en la biblioteca: la que lleve su "[tmdbid-N]" o, si no, la de su nombre.
// Vacía si no existe.
std::string findWorkFolder(const std::string& root, const WorkInfo& work);
// Vídeos de la carpeta de una obra (y de sus subcarpetas "Season NN")
std::vector<DiskVideo> videosOnDisk(const std::string& root, const WorkInfo& work);
// probeVideo con caché en memoria por ruta, tamaño y fecha de modificación
std::optional<VideoInfo> probeVideoCached(const std::string& path);

// Descomprime (si hace falta) y coloca los vídeos en la biblioteca. Si todo va bien, borra las
// partes del búfer. progress recibe el porcentaje de la descompresión; shouldStop la interrumpe.
ImportResult importRelease(const ImportRequest& request, const std::function<void(int)>& progress,
                           const std::function<bool()>& shouldStop);

// Borra los archivos de una versión sustituida (D-037). Solo ficheros normales (no enlaces) dentro
// de alguna de las carpetas de biblioteca (roots) y que no estén en keep (la versión nueva). Después
// quita las carpetas que hayan quedado vacías, sin llegar a la raíz. Devuelve cuántos ha borrado.
std::size_t removeFiles(const std::vector<std::string>& files, const std::vector<std::string>& roots,
                        const std::vector<std::string>& keep);

// --- Nombres (funciones puras, con tests) ---

// Quita lo que no vale en un nombre de fichero (/ \ : * ? " < > | y controles) y los puntos o
// espacios finales; limita la longitud sin partir caracteres UTF-8
std::string sanitizeName(const std::string& name);
// "Título (Año) [tmdbid-N]"
std::string workFolderName(const WorkInfo& work);
// "Season 01" (Jellyfin lo reconoce sin ambigüedad)
std::string seasonFolderName(int season);
// "Título (Año) - 1080p.mkv" (sin etiqueta: "Título (Año).mkv")
std::string movieFileName(const WorkInfo& work, const std::string& versionLabel, const std::string& extension);
// "Serie S01E01 - 1080p.mkv"; con varios episodios, "Serie S01E02-E03 - 1080p.mkv"
std::string episodeFileName(const WorkInfo& work, int season, int episode, int episodeEnd,
                            const std::string& versionLabel, const std::string& extension);

// Etiqueta de versión para el nombre: "4K HDR REMUX", "1080p"; vacía si no hay calidad ni etiquetas
std::string versionLabel(const std::string& quality, bool hdr, const std::vector<std::string>& tags);

// ¿Es un subtítulo (.srt, .ass...)?
bool isSubtitle(const std::string& path);

// Ruta de 7-Zip (vacío si no está instalado)
std::string findSevenZip();
// Ruta de ffprobe, del paquete ffmpeg (vacío si no está instalado)
std::string findFfprobe();

// Analiza un vídeo con ffprobe (sin shell). std::nullopt si no está instalado o no lo reconoce.
// partial: es solo el principio del archivo (se acepta lo que ffprobe lea aunque acabe con error).
// diagnostics (opcional) recibe lo que ffprobe dijo si no se pudo leer el vídeo.
std::optional<VideoInfo> probeVideo(const std::string& path, const std::function<bool()>& shouldStop = nullptr,
                                    bool partial = false, std::string* diagnostics = nullptr);
// Dónde empieza el vídeo en los primeros bytes de un archivo (D-042): un MKV/WebM, un MP4/MOV o un
// AVI, sueltos o guardados sin comprimir dentro de un ZIP o RAR. std::nullopt si no se encuentra.
std::optional<std::size_t> findVideoStart(const std::string& data);
// El principio del vídeo a partir de los primeros bytes de un archivo (D-042): tal cual, sacado de
// un ZIP (sin comprimir o con deflate, que se descomprime aunque falte el resto) o de un RAR sin
// compresión. Si no se puede, std::nullopt y el motivo en reason.
std::optional<std::string> videoFromPrefix(const std::string& data, std::string& reason);
// Lo mismo con 7-Zip, para comprimidos que comprimen el vídeo (RAR): descomprime el principio de un
// archivo cortado y entrega lo que pueda (sin shell)
std::optional<std::string> videoFromPrefixWith7z(const std::string& data, const std::string& extension,
                                                 std::string& reason);
// Interpreta la salida JSON de ffprobe (función pura, con tests)
std::optional<VideoInfo> parseProbe(const std::string& output);
// "1080p" a partir del tamaño de la imagen; mira también el ancho (1920x800 es 1080p)
std::string qualityFromSize(int width, int height);

}  // namespace library

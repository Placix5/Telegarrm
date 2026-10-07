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
};

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

// Ruta de 7-Zip (vacío si no está instalado)
std::string findSevenZip();
// Ruta de ffprobe, del paquete ffmpeg (vacío si no está instalado)
std::string findFfprobe();

// Analiza un vídeo con ffprobe (sin shell). std::nullopt si no está instalado o no lo reconoce.
std::optional<VideoInfo> probeVideo(const std::string& path, const std::function<bool()>& shouldStop = nullptr);
// Interpreta la salida JSON de ffprobe (función pura, con tests)
std::optional<VideoInfo> parseProbe(const std::string& output);
// "1080p" a partir del tamaño de la imagen; mira también el ancho (1920x800 es 1080p)
std::string qualityFromSize(int width, int height);

}  // namespace library

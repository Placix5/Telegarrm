#pragma once

#include <optional>
#include <string>
#include <vector>

// Análisis de nombres de fichero y de las "fichas" (foto + pie con los datos de una
// serie o película) que se publican en los canales. Funciones puras, sin estado:
// se prueban en tests/parser_tests.cpp con ejemplos reales de los canales.
namespace media {

struct EpisodeInfo {
    int season = 0;
    int episode = 0;
    int episodeEnd = 0;        // Último episodio si el fichero trae varios (1x01-02); 0 = uno solo
    std::string seriesName;    // Texto antes del marcador ("Generator Rex #01x01..."); puede estar vacío
    std::string episodeTitle;  // Texto después del marcador; puede estar vacío
};

struct Ficha {
    std::string title;
    std::vector<std::string> alternateTitles;  // "Hijack (Secuestro en el aire)" -> {"Secuestro en el aire"}
    std::optional<int> year;
    std::string quality;                 // Ej. "1080p"; vacío si no consta
    bool hdr = false;
    int season = 0;                      // "Serie - Temporada 4"; 0 = no consta o son varias
    int episode = 0;                     // Línea "Episodio 8" (fichas de series en emisión)
    int episodeEnd = 0;                  // "Episodios 2 y 3"
    std::vector<std::string> genres;     // De los hashtags: #Acción -> "Acción"
    std::vector<std::string> languages;  // Ej. "Castellano", "Latino", "Inglés", "VOSE"
    std::string synopsis;                // Párrafo tras "SINOPSIS:"
};

// Fichero troceado: "Peli.zip.003" -> {"Peli.zip", 3}; "Serie.part2.rar" -> {"Serie.rar", 2}
struct PartInfo {
    std::string base;  // Nombre común a todas las partes (el propio nombre si no es una parte)
    int number = 0;    // 0 = no es una parte
};

// Busca un marcador de episodio: S01E01, 1x01, #01x01, 1x01-02, T1E3, "Temporada 1 Capítulo 3"
std::optional<EpisodeInfo> parseEpisode(const std::string& text);

// Interpreta el pie de una ficha. Admite el formato por líneas "emoji | valor" y el texto libre.
Ficha parseFicha(const std::string& caption);

// Deja solo el título: quita emojis, extensión, [etiquetas], (paréntesis), calidad, códecs,
// idiomas, "temporadas 1-3", créditos y separadores sobrantes
std::string cleanTitle(const std::string& raw);

// "2160p", "1080p", "720p"...; 4K y UHD se normalizan a "2160p". Vacío si no hay.
std::string detectQuality(const std::string& text);
// Orden de calidades para elegir la mejor versión: 2160p > 1080p > 720p > ... > desconocida
int qualityRank(const std::string& quality);
bool detectHdr(const std::string& text);
// Etiquetas que distinguen versiones de una misma calidad: "REMUX", "Open Matte", "IMAX", "SDR"...
std::vector<std::string> detectTags(const std::string& text);
// Identificador de TheMovieDB que algunos nombres de fichero incluyen ("tmdbid_8078", "[tmdbid-10664]")
std::optional<long> detectTmdbId(const std::string& text);
PartInfo splitParts(const std::string& fileName);
std::optional<int> detectYear(const std::string& text);
// Por banderas (🇪🇸, 🇪🇦, 🇲🇽...) y palabras (castellano, latino, VOSE...), sin repetir
std::vector<std::string> detectLanguages(const std::string& text);

// Vídeo o archivo comprimido (por tipo MIME o extensión)
bool isMediaFile(const std::string& fileName, const std::string& mimeType);
bool isArchive(const std::string& fileName);

// Clave para comparar títulos: minúsculas, sin acentos, sin espacios ni signos
// ("Ultimate Spider-Man" y "Ultimate Spiderman" -> "ultimatespiderman")
std::string titleKey(const std::string& title);

// Quita emojis y símbolos gráficos (banderas incluidas); conserva letras con acento
std::string stripSymbols(const std::string& text);

}  // namespace media

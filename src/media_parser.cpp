#include "media_parser.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <utility>

namespace media {
namespace {

constexpr auto kIcase = std::regex::ECMAScript | std::regex::icase;

// --- UTF-8 -----------------------------------------------------------------

// Decodifica el code point que empieza en text[i] y avanza i. Bytes inválidos -> U+FFFD.
char32_t nextCodePoint(const std::string& text, std::size_t& i) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t length = 1;
    char32_t cp = lead;
    if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        cp = lead & 0x07;
    } else if (lead >= 0xE0) {
        length = 3;
        cp = lead & 0x0F;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        cp = lead & 0x1F;
    } else if (lead >= 0x80) {
        ++i;
        return 0xFFFD;
    }
    if (i + length > text.size()) {
        ++i;
        return 0xFFFD;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto cont = static_cast<unsigned char>(text[i + k]);
        if ((cont & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    i += length;
    return cp;
}

bool isSymbol(char32_t cp) {
    return (cp >= 0x2190 && cp <= 0x2BFF)       // Flechas, símbolos técnicos, dingbats (✅)
           || (cp >= 0x1F000 && cp <= 0x1FAFF)  // Emojis y banderas (indicadores regionales)
           || (cp >= 0xFE00 && cp <= 0xFE0F)    // Selectores de variante
           || cp == 0x200D || cp == 0x20E3      // Unión de emojis, tecla
           || (cp >= 0xE0020 && cp <= 0xE007F)  // Etiquetas de banderas
           || cp == 0xFFFD;
}

// Letras latinas con acento -> letra base en minúscula (solo las habituales en títulos)
char foldLatin(char32_t cp) {
    static const std::pair<const char32_t*, char> kFolds[] = {
        {U"áàäâãåÁÀÄÂÃÅ", 'a'}, {U"éèëêÉÈËÊ", 'e'}, {U"íìïîÍÌÏÎ", 'i'}, {U"óòöôõÓÒÖÔÕ", 'o'},
        {U"úùüûÚÙÜÛ", 'u'},     {U"ñÑ", 'n'},       {U"çÇ", 'c'},
    };
    for (const auto& [letters, base] : kFolds) {
        for (const char32_t* p = letters; *p; ++p) {
            if (*p == cp) {
                return base;
            }
        }
    }
    return 0;
}

// --- Cadenas ---------------------------------------------------------------

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Quita espacios y separadores sobrantes de los extremos
std::string trimSeparators(const std::string& text) {
    static const std::string kSeparators = " \t\r\n-|:#._,;/\\";
    const auto first = text.find_first_not_of(kSeparators);
    if (first == std::string::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(kSeparators);
    return text.substr(first, last - first + 1);
}

std::string trimSpaces(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string collapseSpaces(const std::string& text) {
    static const std::regex kSpaces(R"(\s+)");
    return std::regex_replace(text, kSpaces, " ");
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string removeExtension(const std::string& text) {
    static const std::regex kExtension(
        R"((\.part\d+)?\.(mkv|mp4|avi|m4v|ts|wmv|mov|mpe?g|webm|rar|zip|7z|srt|\d{3})$)", kIcase);
    return std::regex_replace(text, kExtension, "");
}

void addUnique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

// Idioma por código de país de una bandera (🇪🇸 = indicadores regionales E y S)
const char* languageForCountry(const std::string& code) {
    static const std::pair<const char*, const char*> kCountries[] = {
        {"ES", "Castellano"}, {"EA", "Castellano"},  // EA (Ceuta y Melilla) se usa como bandera de España
        {"MX", "Latino"},     {"AR", "Latino"},     {"CO", "Latino"},     {"CL", "Latino"},
        {"PE", "Latino"},     {"VE", "Latino"},     {"UY", "Latino"},     {"US", "Inglés"},
        {"GB", "Inglés"},     {"JP", "Japonés"},    {"FR", "Francés"},    {"IT", "Italiano"},
        {"DE", "Alemán"},     {"KR", "Coreano"},    {"PT", "Portugués"},  {"BR", "Portugués"},
    };
    for (const auto& [country, language] : kCountries) {
        if (code == country) {
            return language;
        }
    }
    return nullptr;
}

}  // namespace

std::string stripSymbols(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const char32_t cp = nextCodePoint(text, i);
        if (!isSymbol(cp)) {
            out.append(text, start, i - start);
        }
    }
    return out;
}

std::string titleKey(const std::string& title) {
    std::string key;
    std::size_t i = 0;
    while (i < title.size()) {
        const char32_t cp = nextCodePoint(title, i);
        if (cp < 0x80) {
            if (std::isalnum(static_cast<int>(cp))) {
                key.push_back(static_cast<char>(std::tolower(static_cast<int>(cp))));
            }
        } else if (const char base = foldLatin(cp)) {
            key.push_back(base);
        }
    }
    return key;
}

std::string detectQuality(const std::string& text) {
    static const std::regex kQuality(R"((?:^|[^0-9a-z])(2160p|4k|uhd|1080p|1080i|720p|576p|480p|360p)(?![0-9a-z]))",
                                     kIcase);
    std::smatch match;
    if (!std::regex_search(text, match, kQuality)) {
        return "";
    }
    const std::string quality = toLowerAscii(match[1].str());
    if (quality == "4k" || quality == "uhd") {
        return "2160p";
    }
    if (quality == "1080i") {
        return "1080p";
    }
    return quality;
}

std::optional<int> detectYear(const std::string& text) {
    // (?![0-9xXpP]): descarta resoluciones como 1920x1080
    static const std::regex kYear(R"((?:^|[^0-9])((?:19|20)\d{2})(?![0-9xXpP]))");
    std::smatch match;
    if (std::regex_search(text, match, kYear)) {
        return std::stoi(match[1].str());
    }
    return std::nullopt;
}

std::vector<std::string> detectLanguages(const std::string& text) {
    // Cada idioma con la posición en la que aparece, para devolverlos en orden de aparición
    std::vector<std::pair<std::size_t, std::string>> found;

    // Banderas: dos indicadores regionales seguidos
    std::size_t i = 0;
    char32_t previous = 0;
    std::size_t previousPos = 0;
    while (i < text.size()) {
        const std::size_t pos = i;
        const char32_t cp = nextCodePoint(text, i);
        const bool regional = cp >= 0x1F1E6 && cp <= 0x1F1FF;
        if (regional && previous) {
            const std::string code{static_cast<char>('A' + (previous - 0x1F1E6)), static_cast<char>('A' + (cp - 0x1F1E6))};
            if (const char* language = languageForCountry(code)) {
                found.emplace_back(previousPos, language);
            }
            previous = 0;
        } else {
            previous = regional ? cp : 0;
            previousPos = pos;
        }
    }

    static const std::pair<std::regex, const char*> kWords[] = {
        {std::regex(R"(castellano|espa(?:ñ|Ñ|n)ol|spanish)", kIcase), "Castellano"},
        {std::regex(R"(latino)", kIcase), "Latino"},
        {std::regex(R"(ingl(?:é|É|e)s|english)", kIcase), "Inglés"},
        {std::regex(R"(japon(?:é|É|e)s|japanese)", kIcase), "Japonés"},
        {std::regex(R"(\bvose\b|\bvos\b|subtitulad)", kIcase), "VOSE"},
    };
    for (const auto& [pattern, language] : kWords) {
        std::smatch match;
        if (std::regex_search(text, match, pattern)) {
            found.emplace_back(static_cast<std::size_t>(match.position(0)), language);
        }
    }

    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> languages;
    for (const auto& entry : found) {
        addUnique(languages, entry.second);
    }
    return languages;
}

std::string cleanTitle(const std::string& raw) {
    std::string text = removeExtension(stripSymbols(raw));

    // Nombres de "escena" sin espacios: The.Office.US.S01E01 -> The Office US S01E01
    if (text.find(' ') == std::string::npos) {
        std::replace(text.begin(), text.end(), '.', ' ');
        std::replace(text.begin(), text.end(), '_', ' ');
    }

    // Unos corchetes al principio son parte del nombre ("[REC] 2"); en otro sitio, etiquetas
    static const std::regex kLeadingBrackets(R"(^\s*\[([^\]]*)\])");
    text = std::regex_replace(text, kLeadingBrackets, "$1");

    static const std::regex kNoise[] = {
        std::regex(R"(\[[^\]]*\]|\([^)]*\)|\{[^}]*\})"),                                  // [Etiquetas] (2010) {x}
        std::regex(R"(\btmdb[-_ ]?id[-_= ]?\d+)", kIcase),                                     // tmdbid_8078
        std::regex(R"((cr(?:é|É|e)ditos?|credits?)\b.*$)", kIcase),                        // Créditos y lo que siga
        std::regex(R"(@\S+|https?://\S+|t\.me/\S+)", kIcase),                            // Usuarios y enlaces
        std::regex(R"(\b(?:temporadas?|seasons?)\s*\d+(?:\s*(?:-|a|al|y|&)\s*\d+)*)", kIcase),
        std::regex(R"(\b(?:serie\s+)?complet[ao]\b|\bfin de serie\b)", kIcase),
        std::regex(R"(\b(?:2160p|4k|uhd|1080[pi]|720p|576p|480p|360p)\b)", kIcase),
        std::regex(R"(\b(?:hdr(?:10)?\+?|dolby ?vision|x26[45]|h\.?26[45]|hevc|avc|10 ?bits?|web-?dl|web-?rip|blu-?ray|bdrip|brrip|hdrip|dvdrip|hdtv|remux|aac|ac3|e?ac-?3|dts|ddp?\s?5\.1|5\.1|2\.0)\b)", kIcase),
        std::regex(R"(\b(?:castellano|espa(?:ñ|Ñ|n)ol|latino|ingl(?:é|É|e)s|english|dual|multi|vose|vos|subtitulad[oa]s?|audio)\b)", kIcase),
        std::regex(R"(\brotulado\b|\bv\. ?ext\b|\b(?:open ?matte|imax|sdr|3d|extended|extendida|versi(?:ó|Ó|o)n extendida|director'?s cut|montaje del director|remaster(?:ed|izad[ao])?|unrated|sin censura|special edition|edici(?:ó|Ó|o)n (?:especial|coleccionista))\b)", kIcase),
        std::regex(R"(\bson \d+ partes(?: en (?:zip|rar|7z))?)", kIcase),
    };
    for (const std::regex& noise : kNoise) {
        text = std::regex_replace(text, noise, " ");
    }
    return trimSeparators(collapseSpaces(text));
}

int qualityRank(const std::string& quality) {
    static const char* const kOrder[] = {"360p", "480p", "576p", "720p", "1080p", "2160p"};
    for (int i = 0; i < 6; ++i) {
        if (quality == kOrder[i]) {
            return i + 1;
        }
    }
    return 0;
}

bool detectHdr(const std::string& text) {
    static const std::regex kHdr(R"((?:^|[^0-9a-z])(?:hdr(?:10)?\+?|dolby ?vision)(?![0-9a-z]))", kIcase);
    return std::regex_search(text, kHdr);
}

std::vector<std::string> detectTags(const std::string& text) {
    // Filtro previo: casi ningún nombre lleva etiquetas y las 10 expresiones son caras
    static const char* const kHints[] = {"remux",    "matte",    "imax",    "sdr",     "3d",    "extend", "v. ext",
                                         "director", "remaster", "unrated", "censura", "edition", "edici", "rotulado"};
    const std::string lower = toLowerAscii(text);
    if (std::none_of(std::begin(kHints), std::end(kHints),
                     [&lower](const char* hint) { return lower.find(hint) != std::string::npos; })) {
        return {};
    }
    static const std::pair<std::regex, const char*> kTags[] = {
        {std::regex(R"(\bremux\b)", kIcase), "REMUX"},
        {std::regex(R"(\bopen ?matte\b)", kIcase), "Open Matte"},
        {std::regex(R"(\bimax\b)", kIcase), "IMAX"},
        {std::regex(R"(\bsdr\b)", kIcase), "SDR"},
        {std::regex(R"(\b3d\b)", kIcase), "3D"},
        {std::regex(R"(\b(?:extended|extendida)\b|\bv\. ?ext\b)", kIcase), "Extendida"},
        {std::regex(R"(rotulado (?:en )?castellano)", kIcase), "Rotulado en castellano"},
        {std::regex(R"(rotulado (?:en )?ingl(?:é|É|e)s)", kIcase), "Rotulado en inglés"},
        {std::regex(R"(director'?s cut|montaje del director)", kIcase), "Montaje del director"},
        {std::regex(R"(\bremaster(?:ed|izad[ao])?\b)", kIcase), "Remasterizada"},
        {std::regex(R"(\bunrated\b|sin censura)", kIcase), "Sin censura"},
        {std::regex(R"(special edition|edici(?:ó|Ó|o)n (?:especial|coleccionista))", kIcase), "Edición especial"},
    };
    std::vector<std::string> tags;
    for (const auto& [pattern, tag] : kTags) {
        if (std::regex_search(text, pattern)) {
            tags.push_back(tag);
        }
    }
    return tags;
}

std::optional<long> detectTmdbId(const std::string& text) {
    static const std::regex kTmdb(R"(tmdb[-_ ]?id[-_= ]?(\d{1,9}))", kIcase);
    std::smatch match;
    if (std::regex_search(text, match, kTmdb)) {
        return std::stol(match[1].str());
    }
    return std::nullopt;
}

PartInfo splitParts(const std::string& fileName) {
    // ".part2.rar" y también "_part06.rar"
    static const std::regex kRarPart(R"(^(.*)[._ -]part0*(\d+)\.rar$)", kIcase);
    static const std::regex kNumbered(R"(^(.*)\.(\d{3})$)");
    std::smatch match;
    if (std::regex_match(fileName, match, kRarPart)) {
        return {match[1].str() + ".rar", std::stoi(match[2].str())};
    }
    if (std::regex_match(fileName, match, kNumbered)) {
        return {match[1].str(), std::stoi(match[2].str())};
    }
    return {fileName, 0};
}

std::optional<EpisodeInfo> parseEpisode(const std::string& text) {
    const std::string clean = removeExtension(text);

    // Cada patrón captura temporada, episodio y, opcionalmente, el último episodio de un rango
    static const std::regex kPatterns[] = {
        std::regex(R"((?:^|[^0-9A-Za-z])S(\d{1,2})[ ._-]?E(\d{1,3})(?:[ ._-]?-?[ ._-]?E(\d{1,3}))?(?![0-9]))", kIcase),
        std::regex(R"((?:^|[^0-9A-Za-z])#?(0?[1-9]|[1-9]\d)x(?!26[45](?![0-9]))(\d{1,3})(?:-(\d{1,3}))?(?![0-9]))", kIcase),
        std::regex(R"((?:^|[^0-9A-Za-z])T(\d{1,2})[ ._-]?E(\d{1,3})()(?![0-9]))", kIcase),
        std::regex(R"(temporada\s*(\d{1,2})\D{0,20}?cap(?:(?:i|í|Í)tulo|\.)?\s*(\d{1,3})()(?![0-9]))", kIcase),
    };

    for (const std::regex& pattern : kPatterns) {
        std::smatch match;
        if (!std::regex_search(clean, match, pattern)) {
            continue;
        }
        EpisodeInfo info;
        info.season = std::stoi(match[1].str());
        info.episode = std::stoi(match[2].str());
        if (match[3].matched && match[3].length() > 0) {
            const int end = std::stoi(match[3].str());
            info.episodeEnd = end > info.episode ? end : 0;
        }
        info.seriesName = cleanTitle(clean.substr(0, static_cast<std::size_t>(match.position(0))));
        info.episodeTitle = cleanTitle(clean.substr(static_cast<std::size_t>(match.position(0) + match.length(0))));
        return info;
    }
    return std::nullopt;
}

Ficha parseFicha(const std::string& caption) {
    Ficha ficha;
    static const std::regex kMetadataLine(R"(^(?:#|@|https?:|t\.me|cr(?:é|É|e)ditos?)|^(?:19|20)\d{2}$|^\d{3,4}p$)", kIcase);
    static const std::string kCalendar = "\xF0\x9F\x93\x85";  // 📅

    std::string titleLine;
    for (const std::string& line : splitLines(caption)) {
        // "✅ | Ultimate Spider-Man" -> "Ultimate Spider-Man"
        const std::string value = trimSeparators(stripSymbols(line));
        if (value.empty()) {
            continue;
        }
        if (titleLine.empty() && !std::regex_search(value, kMetadataLine)) {
            titleLine = value;
        }
        if (!ficha.year && line.find(kCalendar) != std::string::npos) {
            ficha.year = detectYear(value);
        }
    }

    ficha.title = cleanTitle(titleLine);

    // "Ted Lasso - Temporada 4 (1080p)": una sola temporada en el título
    static const std::regex kSeason(R"(\btemporada\s*(\d{1,2})(?!\s*(?:-|a|al|y)\s*\d))", kIcase);
    std::smatch seasonMatch;
    if (std::regex_search(titleLine, seasonMatch, kSeason)) {
        ficha.season = std::stoi(seasonMatch[1].str());
    }

    // Paréntesis del título que no son año, calidad ni idioma: "Hijack (Secuestro en el aire)".
    // Lo que queda de "(1080p y 1080p REMUX)" es "y": un alternativo así uniría obras distintas.
    static const std::regex kParens(R"(\(([^)]*)\))");
    static const std::regex kOnlyYear(R"(^(?:19|20)\d{2}$)");
    static const std::string kStopwords[] = {"and", "the", "del", "las", "los", "con", "por", "para"};
    for (auto it = std::sregex_iterator(titleLine.begin(), titleLine.end(), kParens); it != std::sregex_iterator(); ++it) {
        const std::string alternate = cleanTitle((*it)[1].str());
        const std::string key = titleKey(alternate);
        const bool meaningful = key.size() >= 3 && std::find(std::begin(kStopwords), std::end(kStopwords), key) ==
                                                       std::end(kStopwords);
        if (meaningful && !std::regex_match(alternate, kOnlyYear) && key != titleKey(ficha.title)) {
            addUnique(ficha.alternateTitles, alternate);
        }
    }

    // Línea "Episodio 8" o "Episodios 2 y 3" (no la lista "Episodios: [Episodio 1] [...]")
    static const std::regex kEpisodeLine(R"(^episodios?\s*(\d{1,3})(?:\s*(?:y|-|al|a)\s*(\d{1,3}))?$)", kIcase);
    // "SINOPSIS:" seguido (tras líneas vacías) del párrafo de la sinopsis
    static const std::regex kSynopsisHeader(R"(^sinopsis\s*:?$)", kIcase);
    bool inSynopsis = false;
    for (const std::string& line : splitLines(caption)) {
        const std::string value = trimSeparators(stripSymbols(line));
        std::smatch match;
        if (inSynopsis) {
            if (value.empty()) {
                if (!ficha.synopsis.empty()) {
                    inSynopsis = false;
                }
                continue;
            }
            // La sinopsis conserva su puntuación: solo se recortan espacios
            ficha.synopsis += (ficha.synopsis.empty() ? "" : " ") + trimSpaces(stripSymbols(line));
            continue;
        }
        if (std::regex_match(value, kSynopsisHeader)) {
            inSynopsis = ficha.synopsis.empty();
        } else if (!ficha.episode && std::regex_match(value, match, kEpisodeLine)) {
            ficha.episode = std::stoi(match[1].str());
            if (match[2].matched) {
                const int end = std::stoi(match[2].str());
                ficha.episodeEnd = end > ficha.episode ? end : 0;
            }
        }
    }

    // Año: línea con 📅 > paréntesis del título > línea que solo tiene el año ("2015", "Año: 2015").
    // Nunca de la sinopsis, que suele mencionar otros años.
    if (!ficha.year) {
        static const std::regex kYearInParens(R"(\(((?:19|20)\d{2})\))");
        std::smatch match;
        if (std::regex_search(titleLine, match, kYearInParens)) {
            ficha.year = std::stoi(match[1].str());
        }
    }
    if (!ficha.year) {
        static const std::regex kYearLine(R"(^(?:a(?:ñ|Ñ|n)o|year|estreno)?\s*:?\s*((?:19|20)\d{2})$)", kIcase);
        // Cada segmento de una línea de metadatos: "2020 | 720p" -> "2020", "720p"
        static const std::regex kSegmentSeparator(R"(\s*(?:\||·|•)\s*)");
        for (const std::string& line : splitLines(caption)) {
            const std::string value = stripSymbols(line);
            for (auto it = std::sregex_token_iterator(value.begin(), value.end(), kSegmentSeparator, -1);
                 it != std::sregex_token_iterator() && !ficha.year; ++it) {
                std::smatch match;
                const std::string segment = trimSeparators(it->str());
                if (std::regex_match(segment, match, kYearLine)) {
                    ficha.year = std::stoi(match[1].str());
                }
            }
            if (ficha.year) {
                break;
            }
        }
    }
    // La versión (calidad, HDR) se declara en la primera línea; más abajo se mencionan otras
    ficha.quality = detectQuality(titleLine);
    if (ficha.quality.empty()) {
        ficha.quality = detectQuality(caption);
    }
    ficha.hdr = detectHdr(titleLine);
    ficha.languages = detectLanguages(caption);

    static const std::regex kHashtag(R"(#([^\s#,.;]+))");
    for (auto it = std::sregex_iterator(caption.begin(), caption.end(), kHashtag); it != std::sregex_iterator(); ++it) {
        addUnique(ficha.genres, (*it)[1].str());
    }
    return ficha;
}

bool isArchive(const std::string& fileName) {
    static const std::regex kArchive(R"(\.(rar|zip|7z|\d{3})$)", kIcase);
    return std::regex_search(fileName, kArchive);
}

bool isMediaFile(const std::string& fileName, const std::string& mimeType) {
    static const std::regex kVideo(R"(\.(mkv|mp4|avi|m4v|ts|wmv|mov|mpe?g|webm)$)", kIcase);
    return mimeType.rfind("video/", 0) == 0 || std::regex_search(fileName, kVideo) || isArchive(fileName);
}

}  // namespace media

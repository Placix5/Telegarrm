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

namespace {

// Lo que hay antes del número no es una serie sino un extra o una película: "Serie - OVA 2" no llega aquí,
// pero "Serie 5 ONA - 01", "Película 07" u "Opening 5" sí
bool precededBySpecial(const std::string& before) {
    static const std::regex kSpecial(
        R"((?:^|[^0-9a-z])(?:ova|onas?|oad|especial|special|sp|pel(?:í|Í|i)cula|movie|film|opening|ending|op|ed|ncop|nced|pv|cm|vol(?:umen)?|parte?|trailer|tr(?:á|Á|a)iler)\s*$)",
        kIcase);
    return std::regex_search(before, kSpecial);
}

bool isYear(int number) {
    return number >= 1900 && number <= 2099;
}

// "Boku no Hero - Final Season" -> "Boku no Hero" y la marca de última temporada
bool takeFinalSeason(std::string& name) {
    static const std::regex kFinal(R"(\s*[-:]?\s*final season\s*$)", kIcase);
    std::smatch match;
    if (!std::regex_search(name, match, kFinal)) {
        return false;
    }
    name = trimSeparators(name.substr(0, static_cast<std::size_t>(match.position(0))));
    return true;
}

// Formatos del anime (canal CrunchyShur, 08/10/2026). Primero los que dicen la temporada ("T2 - 01",
// "S2 - 08", "S3 EP11"); después la numeración absoluta, que no la dice
std::optional<EpisodeInfo> parseAnimeEpisode(const std::string& clean) {
    // Guiones bajos como espacios: "One_Piece_Capitulo_8_Título"
    std::string text = clean;
    std::replace(text.begin(), text.end(), '_', ' ');

    struct Pattern {
        std::regex regex;
        bool withSeason;  // El grupo 1 es la temporada y el 2 el episodio; si no, el 1 es el episodio
        bool withArc = false;  // Con temporada: el grupo 2 es el nombre del arco y el 3 el episodio
    };
    static const Pattern kPatterns[] = {
        {std::regex(R"((?:^|\s)[TS](\d{1,2})\s*-\s*(\d{1,4})(?:v\d)?(?![0-9]|p\b))", kIcase), true},
        {std::regex(R"((?:^|\s)S(\d{1,2})\s*EP?\s*(\d{1,4})(?![0-9]))", kIcase), true},
        // "Kimetsu no Yaiba S3 - Katanakaji no Sato Hen - 01 [1080p]": temporada, arco y episodio (D-049)
        {std::regex(R"((?:^|\s)[TS](\d{1,2})\s+-\s+([^\d\s\[\(][^\[\(]*?)\s+-\s*(\d{1,4})(?:v\d)?(?=\s|\.|\[|\(|$))",
                    kIcase),
         true, true},
        // "Serie - 01", "Serie - 001 [h264]", "Serie - 01 - Título", "Serie - 25 END"
        {std::regex(R"(\s-\s*(\d{1,4})(?:v\d)?(?=\s|\.|\[|\(|$))", kIcase), false},
        // "Serie 003 [7E936FD9]", "Kochikame 165 - Título" (sin guion, solo con 3 o 4 cifras)
        {std::regex(R"(\s(\d{3,4})(?:v\d)?(?=\s*(?:\[|\(|-\s|$)))", kIcase), false},
        // "42 - Despertar"
        {std::regex(R"(^(\d{1,3})\s*-\s+(?=\S))", kIcase), false},
        // "FWnF Bleach Kai 53", "BB Code Geass R2 14 [hash]": dos cifras al final (sin contar corchetes)
        {std::regex(R"(\s(\d{2,4})(?:v\d)?(?:\s*(?:\[[^\]]*\]|\([^)]*\)))*\s*$)", kIcase), false},
    };
    for (const Pattern& pattern : kPatterns) {
        std::smatch match;
        if (!std::regex_search(text, match, pattern.regex)) {
            continue;
        }
        const std::string before = text.substr(0, static_cast<std::size_t>(match.position(0)));
        const int episode = std::stoi(match[pattern.withArc ? 3 : pattern.withSeason ? 2 : 1].str());
        if (precededBySpecial(before) || (!pattern.withSeason && isYear(episode)) ||
            (pattern.withArc && precededBySpecial(match[2].str()))) {
            continue;  // "Serie 5 ONA - 01", "Batman - 1989", "Serie S2 - OVA - 01"
        }
        EpisodeInfo info;
        info.season = pattern.withSeason ? std::stoi(match[1].str()) : 1;
        info.episode = episode;
        info.absolute = !pattern.withSeason;
        info.seriesName = cleanTitle(before);
        info.finalSeason = takeFinalSeason(info.seriesName);
        info.episodeTitle = cleanTitle(text.substr(static_cast<std::size_t>(match.position(0) + match.length(0))));
        return info;
    }
    return std::nullopt;
}

}  // namespace

std::optional<EpisodeInfo> parseEpisode(const std::string& text, bool animeFormats) {
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
    return animeFormats ? parseAnimeEpisode(clean) : std::nullopt;
}

bool isAnimeNumbered(const std::string& fileName) {
    const std::string clean = removeExtension(fileName);
    // Sin marcador "normal" (S01E01, 1x01...) y con uno del anime
    return !parseEpisode(fileName, false) && parseAnimeEpisode(clean).has_value();
}

namespace {

const std::regex kTitleParens(R"(\(([^()]*)\))");
const std::regex kOnlyYear(R"(^\s*(?:19|20)\d{2}\s*$)");

// Paréntesis con datos técnicos: "(1080p AV1)", "(1080p - Versión del Blu-ray)", "(DVDRip y 1080p los
// episodios del 6 al 12)", "(TVRip)". Lo que quede al quitarles la calidad no es un título (D-048).
bool technicalParens(const std::string& inner) {
    static const std::regex kSource(
        R"((?:^|[^0-9a-z])(?:dvd(?:-?rip)?|vhs(?:-?rip)?|tv-?rip|hdtv|bd-?rip|br-?rip|web-?dl|web-?rip|blu-?ray|remux|hdr(?:10)?\+?|x26[45]|h\.?26[45]|hevc|av1)(?![0-9a-z]))",
        kIcase);
    return !detectQuality(inner).empty() || std::regex_search(inner, kSource);
}

// ¿El paréntesis va al final del título? Después solo hay calidad, etiquetas u otros paréntesis
// ("Here (Aquí) (1080p)", "Hijack (Secuestro en el aire)") o un guion que separa el resto
// ("Apocalipsis en el instituto (High School of the Dead) - Temporada 1 + OVA"). Solo esos pueden ser
// títulos alternativos.
bool trailingParens(const std::string& line, std::size_t end) {
    static const std::regex kDashFollows(R"(^\s+-\s)");
    const std::string rest = line.substr(end);
    return cleanTitle(rest).empty() || std::regex_search(rest, kDashFollows);
}

// Paréntesis que son parte del título: al principio ("(500) días juntos"), pegados a una palabra
// ("(Des)encanto") o seguidos de más título ("Evangelion: 1.0 You Are (Not) Alone"). Se quitan
// solo los paréntesis. El año y los datos técnicos siguen fuera del título.
std::string unwrapTitleParens(const std::string& line) {
    const auto wordByte = [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x80 || std::isalnum(byte) != 0;
    };
    static const std::regex kWordFollows(R"(^\s+[^\s\-:|·•(\[])");
    std::string result;
    std::size_t last = 0;
    for (auto it = std::sregex_iterator(line.begin(), line.end(), kTitleParens); it != std::sregex_iterator(); ++it) {
        const auto start = static_cast<std::size_t>(it->position(0));
        const std::size_t end = start + static_cast<std::size_t>(it->length(0));
        const std::string inner = (*it)[1].str();
        const bool atStart = trimSpaces(line.substr(0, start)).empty();
        const bool glued = (start > 0 && wordByte(line[start - 1])) || (end < line.size() && wordByte(line[end]));
        const bool wordFollows = std::regex_search(line.substr(end), kWordFollows);
        const bool partOfTitle = !trimSpaces(inner).empty() && !trailingParens(line, end) &&
                                 (atStart || glued || wordFollows) && !std::regex_match(inner, kOnlyYear) &&
                                 !technicalParens(inner);
        result += line.substr(last, start - last);
        result += partOfTitle ? inner : it->str();
        last = end;
    }
    return result + line.substr(last);
}

}  // namespace

Ficha parseFicha(const std::string& caption, bool animeFormats) {
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

    ficha.title = cleanTitle(unwrapTitleParens(titleLine));

    // "Ted Lasso - Temporada 4 (1080p)": una sola temporada en el título
    static const std::regex kSeason(R"(\btemporada\s*(\d{1,2})(?!\s*(?:-|a|al|y)\s*\d))", kIcase);
    std::smatch seasonMatch;
    if (std::regex_search(titleLine, seasonMatch, kSeason)) {
        ficha.season = std::stoi(seasonMatch[1].str());
    }

    // Canales de anime (D-047): "Serie Season 2", "Serie 2nd Season", "Serie S2", "Serie T3" y
    // "Serie: Final Season", que además se quitan del título para que se una con las otras temporadas
    if (animeFormats) {
        static const std::regex kAnimeSeasons[] = {
            std::regex(R"(\s*[-:]?\s*\bseason\s*(\d{1,2})\b)", kIcase),
            std::regex(R"(\s*[-:]?\s*\b(\d{1,2})(?:st|nd|rd|th) season\b)", kIcase),
            std::regex(R"(\s+[ST](\d{1,2})\s*$)", kIcase),
        };
        for (const std::regex& pattern : kAnimeSeasons) {
            std::smatch match;
            if (ficha.season == 0 && std::regex_search(ficha.title, match, pattern)) {
                ficha.season = std::stoi(match[1].str());
                ficha.title = trimSeparators(std::regex_replace(ficha.title, pattern, " "));
            }
        }
        if (ficha.season == 0) {
            ficha.finalSeason = takeFinalSeason(ficha.title);
        }
    }

    // Canales de anime: la línea pegada debajo del título, si es un título y no un dato ("Vigilante:
    // Boku no Hero Academia Illegals" / "My Hero Academia: Vigilantes"). Las etiquetas, las listas
    // y los datos ("⭐ MyAnimeList Score") no cuentan.
    if (animeFormats) {
        const std::vector<std::string> lines = splitLines(caption);
        std::size_t first = 0;
        while (first < lines.size() && trimSeparators(stripSymbols(lines[first])).empty()) {
            ++first;
        }
        if (first + 1 < lines.size()) {
            const std::string raw = trimSpaces(lines[first + 1]);
            const std::string second = trimSeparators(stripSymbols(raw));
            static const std::regex kNotATitle(
                R"(^(?:[#>@\-•*]|https?:|t\.me)|score|sinopsis|g(?:é|É|e)nero|episodios?\b|temporadas?\b|^\d+$|:\s*$)", kIcase);
            const std::string alternate = cleanTitle(second);
            const std::string key = titleKey(alternate);
            if (!second.empty() && !std::regex_search(raw, kNotATitle) && !std::regex_search(second, kNotATitle) &&
                second.size() <= 120 && key.size() >= 4 && key != titleKey(ficha.title)) {
                addUnique(ficha.alternateTitles, alternate);
            }
        }
    }

    // Paréntesis del final del título que no son año, calidad ni idioma: "Hijack (Secuestro en el
    // aire)". Lo que queda de "(1080p y 1080p REMUX)" es "y", de "(1080p AV1)" es "AV1" y de "You Are
    // (Not) Alone" es "Not": un alternativo así uniría obras distintas (D-048).
    static const std::string kStopwords[] = {"and", "the", "del", "las", "los", "con", "por", "para"};
    for (auto it = std::sregex_iterator(titleLine.begin(), titleLine.end(), kTitleParens); it != std::sregex_iterator();
         ++it) {
        const std::string inner = (*it)[1].str();
        if (!trailingParens(titleLine, static_cast<std::size_t>(it->position(0) + it->length(0))) ||
            technicalParens(inner)) {
            continue;
        }
        const std::string alternate = cleanTitle(inner);
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

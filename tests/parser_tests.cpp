// Tests del parser y de la construcción del catálogo. Sin framework: se ejecutan con
// `ctest --test-dir build` o directamente con ./build/telegarrm_tests.
// Los casos "reales" reproducen mensajes de canales sincronizados en la Pi (06/10/2026).

#include <cstdint>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "media_parser.hpp"

namespace {

int checks = 0;
int failures = 0;

std::string show(const std::string& value) { return "\"" + value + "\""; }
// Sin esta sobrecarga, un literal "..." se convertiría a bool antes que a std::string
std::string show(const char* value) { return show(std::string(value)); }
std::string show(int value) { return std::to_string(value); }
std::string show(std::int64_t value) { return std::to_string(value); }
std::string show(bool value) { return value ? "true" : "false"; }
std::string show(const std::optional<int>& value) { return value ? std::to_string(*value) : "(nada)"; }
std::string show(const std::optional<long>& value) { return value ? std::to_string(*value) : "(nada)"; }
std::string show(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "{";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << (i ? ", " : "") << show(values[i]);
    }
    out << "}";
    return out.str();
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* expression, int line) {
    ++checks;
    if (!(actual == expected)) {
        ++failures;
        std::cerr << "tests/parser_tests.cpp:" << line << ": " << expression << "\n    esperado: " << show(expected)
                  << "\n    obtenido: " << show(actual) << std::endl;
    }
}

#define CHECK_EQ(actual, expected) checkEq((actual), (expected), #actual, __LINE__)
#define CHECK(condition) checkEq(static_cast<bool>(condition), true, #condition, __LINE__)

using Message = DbManager::Message;
using Strings = std::vector<std::string>;

Message photo(std::int64_t id, const std::string& caption) {
    Message message;
    message.messageId = id;
    message.contentType = "messagePhoto";
    message.text = caption;
    return message;
}

Message video(std::int64_t id, const std::string& fileName, std::int64_t size, const std::string& caption = "",
              const std::string& mimeType = "video/x-matroska") {
    Message message;
    message.messageId = id;
    message.contentType = "messageDocument";
    message.fileName = fileName;
    message.fileSize = size;
    message.mimeType = mimeType;
    message.text = caption;
    return message;
}

Message text(std::int64_t id, const std::string& content) {
    Message message;
    message.messageId = id;
    message.contentType = "messageText";
    message.text = content;
    return message;
}

// Fichas reales
const std::string kSpiderManFicha =
    "\xE2\x9C\x85 | Ultimate Spider-Man\n\n\xF0\x9F\x93\x85 | 2015\n\xF0\x9F\x93\xBA | 1080p\n"
    "\xF0\x9F\x8E\xAD | #Acción #Comedia\n\xF0\x9F\x94\x8A | \xF0\x9F\x87\xAA\xF0\x9F\x87\xA6";  // ✅ 📅 📺 🎭 🔊 🇪🇦
const std::string kGeneratorRexFicha = "Generator Rex temporadas 1-3 castellano\ncréditos: @romag_01";

void testEpisodes() {
    {
        const auto e = media::parseEpisode("1x01 - Ultimate Spiderman.mkv");  // Real
        CHECK(e.has_value());
        CHECK_EQ(e->season, 1);
        CHECK_EQ(e->episode, 1);
        CHECK_EQ(e->seriesName, "");
        CHECK_EQ(e->episodeTitle, "Ultimate Spiderman");
    }
    {
        const auto e = media::parseEpisode("Generator Rex #01x01 - El Día Que Todo Cambió.mp4");  // Real
        CHECK(e.has_value());
        CHECK_EQ(e->season, 1);
        CHECK_EQ(e->episode, 1);
        CHECK_EQ(e->seriesName, "Generator Rex");
        CHECK_EQ(e->episodeTitle, "El Día Que Todo Cambió");
    }
    {
        const auto e = media::parseEpisode("The.Office.US.S02E05.720p.WEB-DL.x264.mkv");
        CHECK(e.has_value());
        CHECK_EQ(e->season, 2);
        CHECK_EQ(e->episode, 5);
        CHECK_EQ(e->seriesName, "The Office US");
        CHECK_EQ(e->episodeTitle, "");
    }
    {
        const auto e = media::parseEpisode("Serie S01E01-E02.mkv");
        CHECK(e.has_value());
        CHECK_EQ(e->episode, 1);
        CHECK_EQ(e->episodeEnd, 2);
    }
    {
        const auto e = media::parseEpisode("Serie 3x07-08 - Doble.mkv");
        CHECK(e.has_value());
        CHECK_EQ(e->season, 3);
        CHECK_EQ(e->episode, 7);
        CHECK_EQ(e->episodeEnd, 8);
        CHECK_EQ(e->episodeTitle, "Doble");
    }
    {
        const auto e = media::parseEpisode("Mi Serie T2E10.avi");
        CHECK(e.has_value());
        CHECK_EQ(e->season, 2);
        CHECK_EQ(e->episode, 10);
    }
    {
        const auto e = media::parseEpisode("Mi Serie Temporada 1 Capítulo 3.mp4");
        CHECK(e.has_value());
        CHECK_EQ(e->season, 1);
        CHECK_EQ(e->episode, 3);
    }
    // Sin marcador de episodio: películas, resoluciones y códecs no deben confundirse
    CHECK(!media::parseEpisode("Blade Runner 2049 (2017) 2160p.mkv"));
    CHECK(!media::parseEpisode("Video 1920x1080.mp4"));
    CHECK(!media::parseEpisode("Pelicula.1080p.x264.mkv"));
}

void testFichas() {
    {
        const media::Ficha f = media::parseFicha(kSpiderManFicha);
        CHECK_EQ(f.title, "Ultimate Spider-Man");
        CHECK_EQ(f.year, std::optional<int>(2015));
        CHECK_EQ(f.quality, "1080p");
        CHECK_EQ(f.genres, (Strings{"Acción", "Comedia"}));
        CHECK_EQ(f.languages, Strings{"Castellano"});
    }
    {
        const media::Ficha f = media::parseFicha(kGeneratorRexFicha);
        CHECK_EQ(f.title, "Generator Rex");
        CHECK_EQ(f.year, std::optional<int>());
        CHECK_EQ(f.quality, "");
        CHECK_EQ(f.languages, Strings{"Castellano"});
    }
    {
        // Ficha de película en texto libre
        const media::Ficha f = media::parseFicha(
            "Blade Runner 2049 (2017)\nCiencia ficción\n\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8 Castellano | "
            "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 Inglés\n4K HDR");
        CHECK_EQ(f.title, "Blade Runner 2049");
        CHECK_EQ(f.year, std::optional<int>(2017));
        CHECK_EQ(f.quality, "2160p");
        CHECK_EQ(f.languages, (Strings{"Castellano", "Inglés"}));
    }
}

void testHelpers() {
    CHECK_EQ(media::cleanTitle("Ultimate Spider-Man [Castellano] [Arreglado]"), "Ultimate Spider-Man");  // Real
    CHECK_EQ(media::cleanTitle("\xF0\x9F\x94\x90 Close Enough"), "Close Enough");                      // Real (🔐)
    CHECK_EQ(media::cleanTitle("Generator Rex (Castellano)"), "Generator Rex");                        // Real
    CHECK_EQ(media::cleanTitle("Pelicula.Larga.2019.1080p.BluRay.x264.mkv"), "Pelicula Larga 2019");

    CHECK_EQ(media::detectQuality("Serie 4K HDR"), "2160p");
    CHECK_EQ(media::detectQuality("emision 1080i"), "1080p");
    CHECK_EQ(media::detectQuality("sin calidad"), "");

    CHECK_EQ(media::detectYear("Video 1920x1080"), std::optional<int>());
    CHECK_EQ(media::detectYear("Matrix (1999)"), std::optional<int>(1999));

    CHECK_EQ(media::titleKey("Ultimate Spider-Man"), media::titleKey("Ultimate Spiderman"));
    CHECK_EQ(media::titleKey("Teoría De Cuerdas"), "teoriadecuerdas");

    CHECK_EQ(media::detectLanguages("\xF0\x9F\x87\xB2\xF0\x9F\x87\xBD Latino | \xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 Inglés"),
             (Strings{"Latino", "Inglés"}));
    CHECK_EQ(media::detectLanguages("castellano y VOSE"), (Strings{"Castellano", "VOSE"}));

    CHECK(media::isMediaFile("x.mkv", ""));
    CHECK(media::isMediaFile("x.part1.rar", ""));
    CHECK(media::isMediaFile("x.001", ""));
    CHECK(media::isMediaFile("video", "video/mp4"));
    CHECK(!media::isMediaFile("x.pdf", "application/pdf"));
    CHECK(media::isArchive("x.7z"));
    CHECK(!media::isArchive("x.mkv"));
}

std::vector<Catalog::Item> build(std::int64_t chatId, const std::string& title, const std::vector<Message>& messages,
                                 const std::vector<DbManager::Topic>& topics = {}) {
    return Catalog::buildItems({{chatId, title, messages, topics}});
}

Message inTopic(Message message, std::int64_t topicId) {
    message.topicId = topicId;
    return message;
}

const Catalog::Item* findTitle(const std::vector<Catalog::Item>& items, const std::string& title) {
    for (const Catalog::Item& item : items) {
        if (item.title == title) {
            return &item;
        }
    }
    return nullptr;
}

void testCatalogSpiderMan() {
    // Canal real de una serie: ficha, episodios "NxNN - Serie.mkv" y textos de cierre
    const std::vector<Message> messages = {
        photo(1, kSpiderManFicha),
        video(2, "1x01 - Ultimate Spiderman.mkv", 365'000'000),
        video(3, "1x02 - Ultimate Spiderman.mkv", 364'000'000),
        video(4, "2x01 - Ultimate Spiderman.mkv", 429'000'000),
        video(5, "2x02 - Ultimate Spiderman.mkv", 430'000'000),
        text(6, "\xE2\x9C\x85" "FIN DE SERIE" "\xE2\x9C\x85"),
        text(7, "CREDITOS A @LexB9"),
    };
    const auto items = build(-100, "Ultimate Spider-Man [Castellano] [Arreglado]", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    const Catalog::Item& item = items[0];
    CHECK_EQ(item.kind, "series");
    CHECK_EQ(item.title, "Ultimate Spider-Man");
    CHECK_EQ(item.year, std::optional<int>(2015));
    CHECK_EQ(item.qualities, Strings{"1080p"});
    CHECK_EQ(item.languages, Strings{"Castellano"});
    CHECK_EQ(item.genres, (Strings{"Acción", "Comedia"}));
    CHECK_EQ(item.anchorMessageId, std::int64_t{1});
    CHECK_EQ(item.posterMessageId, std::int64_t{1});
    CHECK_EQ(item.seasonCount, 2);
    CHECK_EQ(item.episodeCount, 4);
    CHECK_EQ(item.totalSize, std::int64_t{1'588'000'000});
    CHECK(!item.airing);
    // "Ultimate Spiderman" es el nombre de la serie, no el título de cada episodio
    CHECK_EQ(item.releases[0].episodeTitle, "");
}

void testCatalogGeneratorRex() {
    // Canal real: ficha de una línea, portadas de temporada sin pie y enlaces al final
    const std::vector<Message> messages = {
        photo(10, kGeneratorRexFicha),
        photo(11, ""),
        video(12, "Generator Rex #01x01 - El Día Que Todo Cambió.mp4", 268'000'000,
              "Generator Rex #01x01 - El Día Que Todo Cambió", "video/mp4"),
        video(13, "Generator Rex #01x02 - Teoría De Cuerdas.mp4", 263'000'000,
              "Generator Rex #01x02 - Teoría De Cuerdas", "video/mp4"),
        photo(14, ""),
        video(15, "Generator Rex #02x01 - Inicio.mp4", 263'000'000, "", "video/mp4"),
        text(16, "https://t.me/ABTVEsp"),
    };
    const auto items = build(-200, "Generator Rex [Castellano]", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    const Catalog::Item& item = items[0];
    CHECK_EQ(item.title, "Generator Rex");
    CHECK_EQ(item.posterMessageId, std::int64_t{10});
    CHECK_EQ(item.seasonCount, 2);
    CHECK_EQ(item.episodeCount, 3);
    CHECK_EQ(item.releases[0].episodeTitle, "El Día Que Todo Cambió");
    CHECK_EQ(item.releases[1].episodeTitle, "Teoría De Cuerdas");
}

void testCatalogLibraryChannel() {
    // Canal "biblioteca" con varias fichas seguidas: cada una abre su propio elemento
    const std::vector<Message> messages = {
        photo(1, "Serie A\n2020 | 720p"),
        video(2, "Serie A S01E01.mkv", 100),
        video(3, "Serie A S01E02.mkv", 100),
        photo(4, "Película B (2010)\n1080p"),
        video(5, "Pelicula.B.2010.1080p.mkv", 2000),
        photo(6, "Película C"),
        video(7, "Pelicula C.part1.rar", 1000, "", ""),
        video(8, "Pelicula C.part2.rar", 500, "", ""),
    };
    const auto items = build(-300, "Mi biblioteca", messages);
    CHECK_EQ(static_cast<int>(items.size()), 3);
    const Catalog::Item* serie = findTitle(items, "Serie A");
    const Catalog::Item* movieB = findTitle(items, "Película B");
    const Catalog::Item* movieC = findTitle(items, "Película C");
    CHECK(serie && movieB && movieC);
    if (!serie || !movieB || !movieC) {
        return;
    }
    CHECK_EQ(serie->kind, "series");
    CHECK_EQ(serie->year, std::optional<int>(2020));
    CHECK_EQ(movieB->kind, "movie");
    CHECK_EQ(movieB->year, std::optional<int>(2010));
    CHECK_EQ(movieB->qualities, Strings{"1080p"});
    // Las dos partes del .rar forman un único archivo lógico
    CHECK_EQ(movieC->kind, "movie");
    CHECK_EQ(static_cast<int>(movieC->releases.size()), 1);
    CHECK_EQ(static_cast<int>(movieC->releases[0].parts.size()), 2);
    CHECK_EQ(movieC->releases[0].size, std::int64_t{1500});
    CHECK(movieC->releases[0].archive);
}

void testCatalogWithoutFicha() {
    // Sin ficha: el título sale de los nombres de fichero; una resubida cuenta como el mismo episodio
    const std::vector<Message> messages = {
        video(1, "Mi Serie 1x01.mkv", 100),
        video(2, "Mi Serie 1x02.mkv", 100),
        video(3, "Mi Serie 1x01.mkv", 120),
    };
    const auto items = build(-400, "Canal", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    CHECK_EQ(items[0].anchorMessageId, std::int64_t{1});  // Sin ficha, lo identifica su primer archivo
    CHECK_EQ(items[0].title, "Mi Serie");
    CHECK_EQ(items[0].episodeCount, 2);
    CHECK_EQ(static_cast<int>(items[0].releases.size()), 3);
    // Las dos versiones de 1x01 quedan juntas, por orden de publicación
    CHECK_EQ(items[0].releases[0].parts[0].messageId, std::int64_t{1});
    CHECK_EQ(items[0].releases[1].parts[0].messageId, std::int64_t{3});
}

// --- Formatos reales del canal grande (grupo con temas) ---

const std::vector<DbManager::Topic> kTopics = {
    {2, "Películas", 0}, {4, "Series", 0}, {335, "Series en emisión", 0}, {16088, "Películas 4K", 0}};

void testCatalogMovieVersions() {
    // La misma película en "Películas" (1080p) y en "Películas 4K" (4K HDR), troceada en .zip.00N
    const std::string synopsis = "\n\nSINOPSIS:\n\nDos compañeros de trabajo huyen de un asesino.\n\nCastellano y VO";
    const std::vector<Message> messages = {
        inTopic(photo(100, "El asesino con ojos de corazón (1080p)" + synopsis), 2),
        inTopic(video(101, "El asesino con ojos de corazón (1080p).zip.001", 2'150'000'000, "", ""), 2),
        inTopic(video(102, "El asesino con ojos de corazón (1080p).zip.002", 1'000'000'000, "", ""), 2),
        inTopic(photo(200, "El asesino con ojos de corazón (4K HDR)" + synopsis), 16088),
        inTopic(video(201, "El asesino con ojos de corazón (4K HDR).zip.001", 2'150'000'000, "", ""), 16088),
        inTopic(video(202, "El asesino con ojos de corazón (4K HDR).zip.002", 2'150'000'000, "", ""), 16088),
        inTopic(video(203, "El asesino con ojos de corazón (4K HDR).zip.003", 900'000'000, "", ""), 16088),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    const Catalog::Item& item = items[0];
    CHECK_EQ(item.kind, "movie");
    CHECK_EQ(item.title, "El asesino con ojos de corazón");
    CHECK_EQ(item.qualities, (Strings{"2160p", "1080p"}));
    CHECK_EQ(item.topics, (Strings{"Películas", "Películas 4K"}));
    CHECK_EQ(item.synopsis, "Dos compañeros de trabajo huyen de un asesino.");
    CHECK_EQ(static_cast<int>(item.releases.size()), 2);
    // La mejor versión primero: 4K HDR, tres partes
    CHECK_EQ(item.releases[0].quality, "2160p");
    CHECK(item.releases[0].hdr);
    CHECK_EQ(static_cast<int>(item.releases[0].parts.size()), 3);
    CHECK_EQ(item.releases[0].size, std::int64_t{5'200'000'000});
    CHECK_EQ(item.releases[1].quality, "1080p");
    CHECK(!item.releases[1].hdr);
    CHECK_EQ(item.anchorMessageId, std::int64_t{100});
}

void testCatalogAiringSeries() {
    // Temporadas completas en "Series" y episodios sueltos en "Series en emisión", uno por ficha
    const std::vector<Message> messages = {
        inTopic(photo(10, "Ted Lasso - Temporada 3 (1080p)\n\nSINOPSIS:\n\nUn entrenador.\n\nCastellano, Latino y VOSE"), 4),
        inTopic(video(11, "Ted Lasso 3x01.mkv", 2'000'000'000), 4),
        inTopic(video(12, "Ted Lasso 3x02.mkv", 2'000'000'000), 4),
        inTopic(photo(20, "Ted Lasso - Temporada 4 (1080p)\n\nEpisodio 8\n\nCastellano, Latino y VOSE\n\n"
                          "(Son 3 partes en rar)\n\nEpisodios: [Episodio 1] [Episodios 2 y 3]\n\n"
                          "Temporadas: [Temporada 1] [Temporada 2]"), 335),
        inTopic(video(21, "Ted Lasso 4x08.part1.rar", 2'150'000'000, "", ""), 335),
        inTopic(video(22, "Ted Lasso 4x08.part2.rar", 2'150'000'000, "", ""), 335),
        inTopic(video(23, "Ted Lasso 4x08.part3.rar", 450'000'000, "", ""), 335),
        // Ficha de episodio cuyo archivo no lleva marcador: el episodio sale de la ficha
        inTopic(photo(30, "Ted Lasso - Temporada 4 (1080p)\n\nEpisodio 9"), 335),
        inTopic(video(31, "Ted Lasso nuevo.mkv", 1'500'000'000), 335),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    const Catalog::Item& item = items[0];
    CHECK_EQ(item.kind, "series");
    CHECK_EQ(item.title, "Ted Lasso");
    CHECK(item.airing);
    CHECK_EQ(item.seasonCount, 2);
    CHECK_EQ(item.episodeCount, 4);
    CHECK_EQ(item.languages, (Strings{"Castellano", "Latino", "VOSE"}));
    CHECK_EQ(item.synopsis, "Un entrenador.");
    CHECK_EQ(static_cast<int>(item.releases.size()), 4);
    CHECK_EQ(item.releases[2].season, 4);
    CHECK_EQ(item.releases[2].episode, 8);
    CHECK_EQ(static_cast<int>(item.releases[2].parts.size()), 3);
    CHECK_EQ(item.releases[3].episode, 9);
}

void testCatalogInterleavedTopics() {
    // Mensajes de dos temas intercalados: cada archivo va con la ficha de su tema
    const std::vector<Message> messages = {
        inTopic(photo(1, "Peli A (1080p)"), 2),
        inTopic(photo(2, "Serie B - Temporada 1"), 4),
        inTopic(video(3, "Peli A (1080p).mkv", 100), 2),
        inTopic(video(4, "Serie B 1x01.mkv", 100), 4),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 2);
    const Catalog::Item* movie = findTitle(items, "Peli A");
    const Catalog::Item* series = findTitle(items, "Serie B");
    CHECK(movie && series);
    if (!movie || !series) {
        return;
    }
    CHECK_EQ(movie->kind, "movie");
    CHECK_EQ(static_cast<int>(movie->releases.size()), 1);
    CHECK_EQ(movie->releases[0].parts[0].messageId, std::int64_t{3});
    CHECK_EQ(series->kind, "series");
    CHECK_EQ(series->releases[0].parts[0].messageId, std::int64_t{4});
}

void testCatalogAlternateTitles() {
    // "Hijack (Secuestro en el aire)" y "Secuestro en el aire" son la misma serie
    const std::vector<Message> messages = {
        inTopic(photo(1, "Hijack (Secuestro en el aire) - Temporada 1 (1080p)"), 4),
        inTopic(video(2, "Secuestro en el aire 1x01.mkv", 100), 4),
        inTopic(photo(3, "Secuestro en el aire - Temporada 2 (1080p)"), 4),
        inTopic(video(4, "Secuestro en el aire 2x01.mkv", 100), 4),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    CHECK_EQ(items[0].title, "Hijack");
    CHECK_EQ(items[0].alternateTitles, Strings{"Secuestro en el aire"});
    CHECK_EQ(items[0].seasonCount, 2);
}

void testCatalogRemakes() {
    // Mismo título y años distintos: dos películas
    const std::vector<Message> messages = {
        inTopic(photo(1, "La guerra de los mundos (2005) (1080p)"), 2),
        inTopic(video(2, "La guerra de los mundos (2005) (1080p).mkv", 100), 2),
        inTopic(photo(3, "La guerra de los mundos (2025) (4K HDR)"), 16088),
        inTopic(video(4, "La guerra de los mundos (2025) (4K HDR).mkv", 100), 16088),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 2);
    std::set<int> years;
    for (const Catalog::Item& item : items) {
        years.insert(item.year.value_or(0));
    }
    CHECK(years == (std::set<int>{2005, 2025}));
}

void testBigChannelFichas() {
    {
        const media::Ficha f = media::parseFicha(
            "Ted Lasso - Temporada 4 (1080p)\n\nEpisodio 8\n\nCastellano, Latino y VOSE\n\n(Son 3 partes en rar)\n\n"
            "Episodios: [Episodio 1] [Episodios 2 y 3] [Episodio 4]\n\nTemporadas: [Temporada 1] [Temporada 2]");
        CHECK_EQ(f.title, "Ted Lasso");
        CHECK_EQ(f.season, 4);
        CHECK_EQ(f.episode, 8);
        CHECK_EQ(f.quality, "1080p");
        CHECK_EQ(f.languages, (Strings{"Castellano", "Latino", "VOSE"}));
    }
    {
        const media::Ficha f = media::parseFicha("Serie - Temporada 2\n\nEpisodios 2 y 3");
        CHECK_EQ(f.episode, 2);
        CHECK_EQ(f.episodeEnd, 3);
    }
    {
        const media::Ficha f = media::parseFicha("Hijack (Secuestro en el aire) - Temporada 1 (1080p)\n\nSINOPSIS:\n\nUn vuelo.");
        CHECK_EQ(f.title, "Hijack");
        CHECK_EQ(f.alternateTitles, Strings{"Secuestro en el aire"});
        CHECK_EQ(f.season, 1);
        CHECK_EQ(f.synopsis, "Un vuelo.");
    }
    {
        const media::Ficha f = media::parseFicha("La guerra de los mundos (2025) (4K HDR)\n\nSINOPSIS:\n\nWill Radford.");
        CHECK_EQ(f.title, "La guerra de los mundos");
        CHECK_EQ(f.year, std::optional<int>(2025));
        CHECK_EQ(f.quality, "2160p");
        CHECK(f.hdr);
        CHECK(f.alternateTitles.empty());
    }
    {
        const media::Ficha f = media::parseFicha(
            "My Hero Academia: Vigilantes - Temporada 1\n\nAudio:\n- Japonés [VO] \n- Español (España) \n"
            "- Español (Latinoamérica) \n- Inglés ");
        CHECK_EQ(f.title, "My Hero Academia: Vigilantes");
        CHECK_EQ(f.languages, (Strings{"Japonés", "Castellano", "Latino", "Inglés"}));
    }
    {
        const media::PartInfo p = media::splitParts("Peli (4K HDR).zip.001");
        CHECK_EQ(p.base, "Peli (4K HDR).zip");
        CHECK_EQ(p.number, 1);
    }
    {
        const media::PartInfo p = media::splitParts("Ted Lasso 4x08.part2.rar");
        CHECK_EQ(p.base, "Ted Lasso 4x08.rar");
        CHECK_EQ(p.number, 2);
    }
    CHECK_EQ(media::splitParts("Serie.7z.010").number, 10);
    {
        // Variante real con guion bajo
        const media::PartInfo p = media::splitParts("Piratas_del_Caribe_1080p_REMUX_part06.rar");
        CHECK_EQ(p.base, "Piratas_del_Caribe_1080p_REMUX.rar");
        CHECK_EQ(p.number, 6);
    }
    CHECK_EQ(media::splitParts("Peli.mkv").number, 0);
    CHECK(media::qualityRank("2160p") > media::qualityRank("1080p"));
    CHECK(media::qualityRank("720p") > media::qualityRank(""));
    CHECK(media::detectHdr("Peli (4K HDR10+)"));
    CHECK(!media::detectHdr("Peli (1080p)"));
}


Message inAlbum(Message message, std::int64_t albumId) {
    message.mediaAlbumId = albumId;
    return message;
}

// Errores reales encontrados al sincronizar el canal grande (06/10/2026)
void testRealRegressions() {
    // "..._2_0x264_..." no es la temporada 0, episodio 264: es el códec
    CHECK(!media::parseEpisode("El_cielo_gira_2004_tmdbid_63619_480pLayer_3_2_0x264_spa_LasCositas.mkv"));
    CHECK(!media::parseEpisode("Pelicula 5.1x264.mkv"));
    // Corchetes al principio: parte del nombre
    CHECK_EQ(media::cleanTitle("[REC] 2 (1080p)"), "REC 2");
    // "Open Matte" es una etiqueta de versión, no un título alternativo
    {
        const media::Ficha f = media::parseFicha("No es país para viejos (Open Matte 1080p)\n\nCastellano, Gallego y VOSE");
        CHECK_EQ(f.title, "No es país para viejos");
        CHECK(f.alternateTitles.empty());
        CHECK_EQ(f.year, std::optional<int>());
    }
    // "(1080p y 1080p REMUX)" no deja "y" como título alternativo (unía cientos de películas)
    CHECK(media::parseFicha("Hokum (1080p y 1080p REMUX)\n\nSINOPSIS:\n\nUn escritor.").alternateTitles.empty());
    CHECK(media::parseFicha("Otra (4K y 1080p)").alternateTitles.empty());
    // El año de la sinopsis no cuenta
    {
        const media::Ficha f = media::parseFicha(
            "Super Mario Bros: La Película (2023) (1080p)\n\nSINOPSIS:\n\nEn 1993 los fontaneros viajan.");
        CHECK_EQ(f.year, std::optional<int>(2023));
    }
    CHECK_EQ(media::detectTmdbId("Alien_Resurrección_1997_tmdbid_8078_Special_Edition_zip"), std::optional<long>(8078));
    CHECK_EQ(media::detectTmdbId("[REC]² (2009) [tmdbid-10664]"), std::optional<long>(10664));
    CHECK_EQ(media::detectTmdbId("Peli (1080p)"), std::optional<long>());
    CHECK_EQ(media::detectTags("Insidious La puerta roja (1080p REMUX)"), Strings{"REMUX"});
    CHECK_EQ(media::detectTags("El último Late Night (4K SDR)"), Strings{"SDR"});
    CHECK_EQ(media::detectTags("Sonic La película (Open Matte 1080p)"), Strings{"Open Matte"});
    CHECK_EQ(media::detectTags("Vaiana (2026) (1080p) [ROTULADO INGLÉS]"), Strings{"Rotulado en inglés"});
    CHECK_EQ(media::detectTags("Nosferatu [V. Ext] (4K HDR)"), Strings{"Extendida"});
    CHECK_EQ(media::cleanTitle("Vaiana (2026) (4K HDR) ROTULADO CASTELLANO"), "Vaiana");
    // El HDR de otra versión mencionado más abajo no cuenta
    CHECK(!media::parseFicha("Vaiana (2026) (1080p)\n\nTambién disponible en 4K HDR").hdr);
}

void testCatalogFilesWithoutFicha() {
    // En "Películas 4K" se suben películas sin ficha detrás de otra ficha: no son suyas.
    // La película sin ficha se une después con su ficha de "Películas" por el título.
    const std::vector<Message> messages = {
        inTopic(photo(1, "El último Late Night (4K SDR)"), 16088),
        inTopic(inAlbum(video(2, "El último Late Night (4K SDR).zip.001", 2'000, "", ""), 7), 16088),
        inTopic(inAlbum(video(3, "El último Late Night (4K SDR).zip.002", 1'000, "", ""), 7), 16088),
        inTopic(inAlbum(video(4, "Las ovejas detectives (4K HDR).zip.001", 2'000, "", ""), 8), 16088),
        inTopic(inAlbum(video(5, "Las ovejas detectives (4K HDR).zip.002", 2'000, "", ""), 8), 16088),
        inTopic(photo(10, "Las ovejas detectives (1080p)"), 2),
        inTopic(inAlbum(video(11, "Las ovejas detectives (1080p).mkv", 1'500), 9), 2),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 2);
    const Catalog::Item* lateNight = findTitle(items, "El último Late Night");
    const Catalog::Item* sheep = findTitle(items, "Las ovejas detectives");
    CHECK(lateNight && sheep);
    if (!lateNight || !sheep) {
        return;
    }
    CHECK_EQ(static_cast<int>(lateNight->releases.size()), 1);
    CHECK_EQ(lateNight->releases[0].tags, Strings{"SDR"});
    CHECK_EQ(sheep->qualities, (Strings{"2160p", "1080p"}));
    CHECK_EQ(static_cast<int>(sheep->releases.size()), 2);
    CHECK_EQ(sheep->posterMessageId, std::int64_t{10});
}

void testCatalogSharedAlbum() {
    // Real: un álbum con las últimas partes de una película y las primeras de la siguiente
    // (sin ficha), y partes publicadas desordenadas
    const std::vector<Message> messages = {
        inTopic(photo(1, "Hokum (4K SDR)"), 16088),
        inTopic(inAlbum(video(2, "Hokum (4K SDR).zip.001", 1'000, "", ""), 5), 16088),
        inTopic(inAlbum(video(3, "Hokum (4K SDR).zip.002", 1'000, "", ""), 5), 16088),
        inTopic(inAlbum(video(4, "Proyecto Salvación (4K HDR)_part02.rar", 1'000, "", ""), 5), 16088),
        inTopic(inAlbum(video(5, "Proyecto Salvación (4K HDR)_part01.rar", 1'000, "", ""), 5), 16088),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 2);
    const Catalog::Item* project = findTitle(items, "Proyecto Salvación");
    CHECK(project != nullptr);
    if (project) {
        CHECK_EQ(static_cast<int>(project->releases.size()), 1);
        CHECK_EQ(project->releases[0].parts[0].number, 1);
        CHECK_EQ(project->releases[0].parts[0].messageId, std::int64_t{5});
    }
}

void testCatalogDifferentFileName() {
    // El primer archivo tras la ficha (y los de su álbum) son suyos aunque se llamen distinto
    const std::vector<Message> messages = {
        inTopic(photo(1, "El Cuervo (1994) (1080p)"), 2),
        inTopic(inAlbum(video(2, "The Crow 1994 1080p.zip.001", 1'000, "", ""), 3), 2),
        inTopic(inAlbum(video(3, "The Crow 1994 1080p.zip.002", 1'000, "", ""), 3), 2),
        inTopic(photo(4, "Sonic La película (Open Matte 1080p)"), 2),
        inTopic(inAlbum(video(5, "Sonic La película (Open Matte 1080p).mkv", 1'000), 6), 2),
        inTopic(photo(7, "No es país para viejos (Open Matte 1080p)"), 2),
        inTopic(inAlbum(video(8, "No es país para viejos (1080p Open Matte).mkv", 1'000), 9), 2),
    };
    const auto items = build(-500, "Las Cositas", messages, kTopics);
    // "Open Matte" no une a Sonic con No es país para viejos
    CHECK_EQ(static_cast<int>(items.size()), 3);
    const Catalog::Item* crow = findTitle(items, "El Cuervo");
    CHECK(crow != nullptr);
    if (crow) {
        CHECK_EQ(static_cast<int>(crow->releases.size()), 1);
        CHECK_EQ(crow->year, std::optional<int>(1994));
    }
}

}  // namespace

int main() {
    testEpisodes();
    testFichas();
    testHelpers();
    testCatalogSpiderMan();
    testCatalogGeneratorRex();
    testCatalogLibraryChannel();
    testCatalogWithoutFicha();
    testBigChannelFichas();
    testCatalogMovieVersions();
    testCatalogAiringSeries();
    testCatalogInterleavedTopics();
    testCatalogAlternateTitles();
    testCatalogRemakes();
    testRealRegressions();
    testCatalogFilesWithoutFicha();
    testCatalogDifferentFileName();
    testCatalogSharedAlbum();

    std::cout << (checks - failures) << "/" << checks << " comprobaciones correctas" << std::endl;
    return failures == 0 ? 0 : 1;
}

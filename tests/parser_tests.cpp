// Tests del parser y de la construcción del catálogo. Sin framework: se ejecutan con
// `ctest --test-dir build` o directamente con ./build/telegarrm_tests.
// Los casos "reales" reproducen mensajes de canales sincronizados en la Pi (06/10/2026).

#include <cstdint>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "catalog.hpp"
#include "library.hpp"
#include "metadata.hpp"
#include "process.hpp"
#include "media_parser.hpp"
#include "tmdb_client.hpp"
#include "tracker.hpp"

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

void testCatalogRepeatedSeriesName() {
    // Canal de prueba de Plácido (07/10/2026): sin ficha y con un nombre que no es el de la serie
    std::vector<Message> messages = {video(1, "1x01 - Ultimate Spiderman.mkv", 364'520'659)};
    auto items = build(-5530696118, "Prueba Claude", messages);
    // Con un solo episodio no se sabe si "Ultimate Spiderman" es la serie o el título del episodio
    CHECK(items.size() == 1 && items[0].title == "Prueba Claude");
    // Con dos que lo repiten, es la serie; sigue siendo la misma obra (la identifica su primer archivo)
    messages.push_back(video(2, "1x02 - Ultimate Spiderman.mkv", 364'000'000));
    items = build(-5530696118, "Prueba Claude", messages);
    CHECK(items.size() == 1 && items[0].title == "Ultimate Spiderman" && items[0].anchorMessageId == 1);
    CHECK(items.size() == 1 && items[0].releases[1].episodeTitle.empty());
    // Si el canal se llama como la serie, se queda su título (mejor escrito)
    items = build(-100, "Ultimate Spider-Man [Castellano]", messages);
    CHECK(items.size() == 1 && items[0].title == "Ultimate Spider-Man");
    // Títulos de episodio distintos: no hay nombre repetido
    items = build(-400, "Mi canal", {video(1, "1x01 - Piloto.mkv", 100), video(2, "1x02 - El regreso.mkv", 100)});
    CHECK(items.size() == 1 && items[0].title == "Mi canal");
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


// Cola de descargas sobre una BD temporal (fuera del proyecto)
void testDownloadQueue() {
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("telegarrm_tests_" + std::to_string(::getpid()) + ".db")).string();
    std::filesystem::remove(path);
    {
        DbManager db(path);
        CHECK(db.open());

        DbManager::Download download;
        download.chatId = -500;
        download.messageId = 101;
        download.title = "Hokum";
        download.kind = "movie";
        download.name = "Hokum (4K HDR)";
        download.quality = "2160p";
        download.hdr = true;
        download.tags = {"REMUX"};
        download.archive = true;
        download.totalSize = 3000;
        download.parts = {{101, 1, "Hokum.part01.rar", 2000, 0, ""}, {102, 2, "Hokum.part02.rar", 1000, 0, ""}};

        const auto first = db.addDownload(download);
        CHECK(first.ok);
        // El mismo archivo no se puede encolar dos veces mientras esté activo
        const auto again = db.addDownload(download);
        CHECK(again.duplicate);
        CHECK_EQ(again.id, first.id);

        auto next = db.nextQueuedDownload();
        CHECK(next.has_value());
        if (next) {
            CHECK_EQ(static_cast<int>(next->parts.size()), 2);
            CHECK_EQ(next->tags, Strings{"REMUX"});
            CHECK(next->hdr && next->archive);
        }

        // Interrumpida a medias: al arrancar vuelve a la cola con su progreso
        CHECK(db.setDownloadStatus(first.id, "downloading"));
        CHECK(db.updateDownloadProgress(first.id, 2000, {{101, 1, "Hokum.part01.rar", 2000, 2000, "/tmp/x"}}));
        CHECK_EQ(db.requeueInterruptedDownloads(), 1);
        next = db.getDownload(first.id);
        CHECK(next && next->status == "queued");
        CHECK(next && next->downloadedSize == 2000);
        CHECK(next && next->parts[0].localPath == "/tmp/x");

        // Cancelada, ya se puede volver a encolar (es otra descarga)
        CHECK(db.setDownloadStatus(first.id, "cancelled"));
        const auto retry = db.addDownload(download);
        CHECK(retry.ok);
        CHECK(retry.id != first.id);
        CHECK_EQ(static_cast<int>(db.listDownloads().size()), 2);

        // Borrar una descarga borra sus partes (en cascada)
        CHECK(db.deleteDownload(first.id));
        CHECK(!db.getDownload(first.id));
        CHECK_EQ(static_cast<int>(db.listDownloads().size()), 1);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}


void testLibraryNames() {
    using library::WorkInfo;
    CHECK_EQ(library::sanitizeName("Batman: El regreso del Caballero Oscuro"), "Batman - El regreso del Caballero Oscuro");
    CHECK_EQ(library::sanitizeName("AC/DC: ¿Qué? *Live*"), "AC DC - ¿Qué Live");  // "?" no vale en Windows ni Samba
    CHECK_EQ(library::sanitizeName("Etc..."), "Etc");
    CHECK_EQ(library::sanitizeName("   "), "Sin título");
    {
        // Recorte sin partir caracteres UTF-8 (cada "ñ" son dos bytes)
        const std::string longName = library::sanitizeName(std::string(300, 'a') + std::string(100, '\xC3') );
        CHECK(longName.size() <= 180);
        std::string accents;
        for (int i = 0; i < 120; ++i) {
            accents += "ñ";
        }
        const std::string cut = library::sanitizeName(accents);
        CHECK(cut.size() <= 180 && cut.size() % 2 == 0);
    }
    CHECK_EQ(library::workFolderName(WorkInfo{"Ted Lasso", 2020, 97546}), "Ted Lasso (2020) [tmdbid-97546]");
    CHECK_EQ(library::workFolderName(WorkInfo{"Ted Lasso", std::nullopt, 0}), "Ted Lasso");
    CHECK_EQ(library::seasonFolderName(4), "Season 04");
    CHECK_EQ(library::movieFileName(WorkInfo{"Hokum", 2025, 1}, "4K HDR", ".mkv"), "Hokum (2025) - 4K HDR.mkv");
    CHECK_EQ(library::movieFileName(WorkInfo{"Hokum", 2025, 1}, "", ".mkv"), "Hokum (2025).mkv");
    CHECK_EQ(library::episodeFileName(WorkInfo{"Ted Lasso", 2020, 1}, 4, 8, 0, "1080p", ".mkv"), "Ted Lasso S04E08 - 1080p.mkv");
    CHECK_EQ(library::episodeFileName(WorkInfo{"Serie", std::nullopt, 0}, 1, 2, 3, "", ".mp4"), "Serie S01E02-E03.mp4");
    CHECK_EQ(library::versionLabel("2160p", true, {"REMUX"}), "4K HDR REMUX");
    CHECK_EQ(library::versionLabel("1080p", false, {}), "1080p");
    CHECK_EQ(library::versionLabel("", false, {}), "");
}

void testProcessWithoutShell() {
    // Sin shell: el ";" llega tal cual como argumento, no ejecuta nada más
    const process::Result result = process::run({"/bin/echo", "hola; rm -rf /"}, nullptr, nullptr);
    CHECK(result.started);
    CHECK_EQ(result.exitCode, 0);
    CHECK_EQ(result.output, "hola; rm -rf /\n");
    const process::Result missing = process::run({"/no/existe"}, nullptr, nullptr);
    CHECK(!missing.started);
}

void writeFile(const std::filesystem::path& path, std::size_t size, char fill) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    std::string data(size, fill);
    // Contenido variado para que el zip no lo comprima casi a nada (y se trocee en varias partes)
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>((i * 2654435761u + static_cast<unsigned char>(fill)) >> 13);
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void testLibraryImport() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("telegarrm_import_" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path buffer = base / "descargas";
    const fs::path movies = base / "peliculas";
    const fs::path series = base / "series";
    fs::create_directories(movies);
    fs::create_directories(series);

    // 1) Película sin comprimir: se mueve con su nombre de Jellyfin
    writeFile(buffer / "documents" / "Peli.Original.1080p.mkv", 5000, 'p');
    library::ImportRequest movie;
    movie.id = 1;
    movie.kind = "movie";
    movie.work = {"Batman: El regreso", 2012, 123};
    movie.quality = "1080p";
    movie.parts = {(buffer / "documents" / "Peli.Original.1080p.mkv").string()};
    movie.libraryRoot = movies.string();
    const library::ImportResult moved = library::importRelease(movie, nullptr, nullptr);
    CHECK(moved.ok);
    CHECK(fs::exists(movies / "Batman - El regreso (2012) [tmdbid-123]" / "Batman - El regreso (2012) - 1080p.mkv"));
    CHECK(!fs::exists(movie.parts[0]));

    // 2) Temporada en un zip troceado: episodios a "Season 01", la muestra y el .nfo fuera
    const std::string sevenZip = library::findSevenZip();
    if (sevenZip.empty()) {
        std::cout << "(7-Zip no está instalado: se omite la prueba con comprimidos)" << std::endl;
    } else {
        const fs::path source = base / "origen";
        writeFile(source / "Serie 1x01.mkv", 6000, 'a');
        writeFile(source / "Serie 1x02.mkv", 6000, 'b');
        writeFile(source / "sample.mkv", 500, 'c');
        writeFile(source / "info.nfo", 100, 'd');
        fs::create_directories(buffer / "documents");
        const process::Result packed = process::run(
            {sevenZip, "a", "-tzip", "-mx0", "-v4k", (buffer / "documents" / "Pack.zip").string(),
             (source / "Serie 1x01.mkv").string(), (source / "Serie 1x02.mkv").string(),
             (source / "sample.mkv").string(), (source / "info.nfo").string()},
            nullptr, nullptr);
        CHECK_EQ(packed.exitCode, 0);

        std::vector<std::string> parts;
        for (const auto& entry : fs::directory_iterator(buffer / "documents")) {
            if (entry.path().filename().string().rfind("Pack.zip.", 0) == 0) {
                parts.push_back(entry.path().string());
            }
        }
        std::sort(parts.begin(), parts.end());
        CHECK(parts.size() > 1);  // De verdad está troceado

        library::ImportRequest season;
        season.id = 2;
        season.kind = "series";
        season.work = {"Serie", 2021, 0};
        season.archive = true;
        season.parts = parts;
        season.libraryRoot = series.string();
        int lastPercent = -1;
        const library::ImportResult imported =
            library::importRelease(season, [&lastPercent](int percent) { lastPercent = percent; }, nullptr);
        CHECK(imported.ok);
        if (!imported.ok) {
            std::cerr << "    error: " << imported.error << std::endl;
        }
        const fs::path seasonDir = series / "Serie (2021)" / "Season 01";
        CHECK(fs::exists(seasonDir / "Serie S01E01.mkv"));
        CHECK(fs::exists(seasonDir / "Serie S01E02.mkv"));
        CHECK(!fs::exists(series / "Serie (2021)" / "extras"));  // La muestra no se importa
        CHECK_EQ(static_cast<int>(imported.files.size()), 2);
        CHECK(!fs::exists(parts.front()));                      // El búfer queda libre
        CHECK(!fs::exists(series / ".telegarrm"));              // Sin temporales
    }
    fs::remove_all(base);
}

}  // namespace


// --- Seguimiento (Fase 4, D-035) ---

Message at(Message message, std::int64_t date) {
    message.date = date;
    return message;
}

using Action = tracking::Action;

// Archivos ya descargados o puestos en cola, por su primera parte
std::function<bool(const Catalog::Release&)> handledIds(std::set<std::int64_t> ids) {
    return [ids](const Catalog::Release& release) { return ids.count(release.parts.front().messageId) > 0; };
}

const Catalog::Release* releaseAt(const Catalog::Item& item, std::size_t index) {
    return index < item.releases.size() ? &item.releases[index] : nullptr;
}

void testTrackingRules() {
    using tracking::versionRank;
    // Resolución, después HDR, después REMUX; las ediciones no cuentan
    CHECK(versionRank("2160p", false, {}) > versionRank("1080p", true, {"REMUX"}));
    CHECK(versionRank("1080p", true, {}) > versionRank("1080p", false, {"REMUX"}));
    CHECK(versionRank("1080p", false, {"REMUX"}) > versionRank("1080p", false, {}));
    CHECK_EQ(versionRank("1080p", false, {"Extendida"}), versionRank("1080p", false, {}));
    CHECK(versionRank("720p", false, {}) > versionRank("", false, {}));

    CHECK(tracking::withinQuality("2160p", ""));
    CHECK(!tracking::withinQuality("2160p", "1080p"));
    CHECK(tracking::withinQuality("1080p", "1080p"));
    CHECK(!tracking::withinQuality("1080p", "720p"));
    CHECK(tracking::withinQuality("", "720p"));  // Calidad desconocida: cabe

    // ¿Están publicadas todas las partes?
    Catalog::Release single;
    single.parts = {{1, "Peli.mkv", 1000, 0}};
    single.date = 1000;
    CHECK(tracking::releaseComplete(single, 1000));
    Catalog::Release split;
    split.date = 1000;
    split.parts = {{1, "Serie 4x08.part1.rar", 2000, 1}, {2, "Serie 4x08.part2.rar", 2000, 2}};
    CHECK(!tracking::releaseComplete(split, 1060));        // Todas llenas: puede faltar la última
    CHECK(tracking::releaseComplete(split, 1000 + 7200));  // Dos horas sin partes nuevas
    split.parts.push_back({3, "Serie 4x08.part3.rar", 500, 3});
    CHECK(tracking::releaseComplete(split, 1060));         // La última es más pequeña
    Catalog::Release gap;
    gap.date = 1000;
    gap.parts = {{2, "Peli.zip.002", 2000, 2}, {3, "Peli.zip.003", 500, 3}};
    CHECK(!tracking::releaseComplete(gap, 1000 + 7200));   // Falta la primera
}

void testTrackingSeries() {
    // Serie en emisión (formato real del tema "Series en emisión"): se sigue en el instante 1500
    std::vector<Message> messages = {
        at(inTopic(photo(10, "Ted Lasso - Temporada 4 (1080p)\n\nEpisodio 8\n\n(Son 3 partes en rar)"), 335), 1000),
        at(inTopic(video(11, "Ted Lasso 4x08.part1.rar", 2'147'483'648, "", ""), 335), 1000),
        at(inTopic(video(12, "Ted Lasso 4x08.part2.rar", 2'147'483'648, "", ""), 335), 1000),
        at(inTopic(video(13, "Ted Lasso 4x08.part3.rar", 448'497'302, "", ""), 335), 1000),
        at(inTopic(photo(20, "Ted Lasso - Temporada 4 (1080p)\n\nEpisodio 9"), 335), 2000),
        at(inTopic(video(21, "Ted Lasso 4x09.mkv", 3'920'306'847), 335), 2000),
    };
    const tracking::Rule rule{1500, ""};
    const std::vector<tracking::Owned> none;
    auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }

    // 1) Solo el episodio publicado después de seguir
    tracking::Plan plan = tracking::plan(items[0], rule, none, handledIds({}), 3000);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 1);
    CHECK(!plan.waiting);
    if (plan.actions.size() == 1) {
        const Catalog::Release* chosen = releaseAt(items[0], plan.actions[0].release);
        CHECK(plan.actions[0].type == Action::Type::NewEpisode);
        CHECK(chosen && chosen->episode == 9);
    }
    // Ya en cola (o descargado): no se repite
    plan = tracking::plan(items[0], rule, none, handledIds({21}), 3000);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 0);

    // 2) Episodio 10 a medio publicar (dos partes llenas): se espera
    messages.push_back(at(inTopic(photo(30, "Ted Lasso - Temporada 4 (1080p)\n\nEpisodio 10"), 335), 2500));
    messages.push_back(at(inTopic(video(31, "Ted Lasso 4x10.part1.rar", 2'147'483'648, "", ""), 335), 2500));
    messages.push_back(at(inTopic(video(32, "Ted Lasso 4x10.part2.rar", 2'147'483'648, "", ""), 335), 2500));
    items = build(-500, "Las Cositas", messages, kTopics);
    plan = tracking::plan(items[0], rule, none, handledIds({21}), 2600);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 0);
    CHECK(plan.waiting);
    // Llega la última parte: ya se puede descargar
    messages.push_back(at(inTopic(video(33, "Ted Lasso 4x10.part3.rar", 300'000'000, "", ""), 335), 2700));
    items = build(-500, "Las Cositas", messages, kTopics);
    plan = tracking::plan(items[0], rule, none, handledIds({21}), 2800);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 1);
    CHECK(!plan.waiting);
    if (plan.actions.size() == 1) {
        const Catalog::Release* chosen = releaseAt(items[0], plan.actions[0].release);
        CHECK(chosen && chosen->episode == 10 && chosen->parts.size() == 3);
    }

    // 3) El canal vuelve a subir la temporada (después de seguirla): el 4x08 no es nuevo
    messages.push_back(at(inTopic(photo(40, "Ted Lasso - Temporada 4 (1080p)\n\nSINOPSIS:\n\nOtra vez."), 4), 3000));
    messages.push_back(at(inTopic(video(41, "Ted Lasso 4x08.mkv", 3'000'000'000), 4), 3000));
    items = build(-500, "Las Cositas", messages, kTopics);
    plan = tracking::plan(items[0], rule, none, handledIds({21, 31}), 3100);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 0);

    // 4) Versión mejor (4K) del 4x09, que ya está en la biblioteca en 1080p
    messages.push_back(at(inTopic(video(50, "Ted Lasso 4x09 4K HDR.mkv", 9'000'000'000), 4), 3500));
    items = build(-500, "Las Cositas", messages, kTopics);
    std::vector<tracking::Owned> owned = {{7, "completed", tracking::versionRank("1080p", false, {}), "1080p", 4, 9, 0}};
    plan = tracking::plan(items[0], rule, owned, handledIds({21, 31}), 3600);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 1);
    if (plan.actions.size() == 1) {
        const Action& action = plan.actions[0];
        const Catalog::Release* chosen = releaseAt(items[0], action.release);
        CHECK(action.type == Action::Type::Upgrade);
        CHECK(chosen && chosen->parts.front().messageId == 50);
        CHECK_EQ(static_cast<int>(action.replaces.size()), 1);
        CHECK(action.cancel.empty());
        CHECK_EQ(action.previousLabel, "1080p");
    }
    // Con calidad máxima 1080p, el 4K no se pide
    plan = tracking::plan(items[0], {1500, "1080p"}, owned, handledIds({21, 31}), 3600);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 0);
    // Si la de 1080p aún estaba en cola sin empezar, se cancela en vez de sustituirse después
    owned[0].status = "queued";
    plan = tracking::plan(items[0], rule, owned, handledIds({21, 31}), 3600);
    CHECK(plan.actions.size() == 1 && plan.actions[0].cancel.size() == 1 && plan.actions[0].replaces.empty());
    // Un vídeo de la biblioteca que no viene de una descarga también se mejora; lo sustituye la importación
    owned[0] = {0, "completed", tracking::versionRank("1080p", false, {}), "1080p", 4, 9, 0};
    plan = tracking::plan(items[0], rule, owned, handledIds({21, 31}), 3600);
    CHECK(plan.actions.size() == 1 && plan.actions[0].type == Action::Type::Upgrade && plan.actions[0].replaces.empty() &&
          plan.actions[0].previousLabel == "1080p");
    // Una versión igual o peor no es una mejora
    owned[0] = {7, "completed", tracking::versionRank("2160p", true, {}), "4K HDR", 4, 9, 0};
    plan = tracking::plan(items[0], rule, owned, handledIds({21, 31}), 3600);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 0);
}

void testTrackingMovies() {
    std::vector<Message> messages = {
        at(inTopic(photo(10, "Dune (2021) (1080p)\n\nSINOPSIS:\n\nArena."), 2), 1000),
        at(inTopic(video(11, "Dune (2021) 1080p.mkv", 4'000'000'000), 2), 1000),
    };
    const tracking::Rule rule{1500, ""};
    auto items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    // Seguir no descarga lo que ya estaba publicado
    tracking::Plan plan = tracking::plan(items[0], rule, {}, handledIds({}), 2000);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 0);

    // Se publica en 4K (y en 3D, que nunca se elige solo): sin tenerla, se descarga la 4K
    messages.push_back(at(inTopic(photo(20, "Dune (2021) (4K HDR)\n\nSINOPSIS:\n\nArena."), 16088), 3000));
    messages.push_back(at(inTopic(video(21, "Dune (2021) 4K HDR 3D.mkv", 30'000'000'000), 16088), 3000));
    messages.push_back(at(inTopic(video(22, "Dune (2021) 4K HDR.mkv", 20'000'000'000), 16088), 3000));
    items = build(-500, "Las Cositas", messages, kTopics);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    plan = tracking::plan(items[0], rule, {}, handledIds({}), 3100);
    CHECK_EQ(static_cast<int>(plan.actions.size()), 1);
    if (plan.actions.size() == 1) {
        const Catalog::Release* chosen = releaseAt(items[0], plan.actions[0].release);
        CHECK(plan.actions[0].type == Action::Type::NewMovie);
        CHECK(chosen && chosen->parts.front().messageId == 22);
    }
    // Con la de 1080p descargada (antes de seguirla), la 4K es una mejora
    const std::vector<tracking::Owned> owned = {{3, "completed", tracking::versionRank("1080p", false, {}), "1080p", 0, 0, 0}};
    plan = tracking::plan(items[0], rule, owned, handledIds({11}), 3100);
    CHECK(plan.actions.size() == 1 && plan.actions[0].type == Action::Type::Upgrade &&
          plan.actions[0].replaces == std::vector<std::int64_t>{3});
}

void testFollowsDatabase() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("telegarrm_follows_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    {
        DbManager db((base / "test.db").string());
        CHECK(db.open());

        DbManager::Follow follow;
        follow.kind = "series";
        follow.title = "Ted Lasso";
        follow.year = 2020;
        follow.workKey = "series|tedlasso|2020";
        follow.chatId = -500;
        follow.anchorId = 10;
        follow.maxQuality = "1080p";
        const auto id = db.addFollow(follow);
        CHECK(id.has_value());
        CHECK(db.updateFollowQuality(*id, ""));
        follow.id = *id;
        follow.anchorId = 20;
        follow.tmdbId = 97546;
        CHECK(db.updateFollowWork(follow));
        const auto stored = db.getFollow(*id);
        CHECK(stored && stored->maxQuality.empty() && stored->anchorId == 20 && stored->tmdbId == 97546);
        CHECK(stored && stored->year == std::optional<int>(2020) && stored->createdAt > 0);
        CHECK_EQ(static_cast<int>(db.listFollows().size()), 1);

        // Descarga automática: origen, seguimiento, sustituciones y archivos colocados
        DbManager::Download download;
        download.chatId = -500;
        download.messageId = 21;
        download.title = "Ted Lasso";
        download.kind = "series";
        download.name = "Ted Lasso 4x09";
        download.totalSize = 100;
        download.origin = "auto";
        download.followId = *id;
        download.replaces = {3, 4};
        download.parts = {{21, 0, "Ted Lasso 4x09.mkv", 100, 0, ""}};
        const auto added = db.addDownload(download);
        CHECK(added.ok);
        CHECK(db.setDownloadLibrary(added.id, "/srv/media/series/Ted Lasso", {"/a.mkv", "/a.srt"}));
        const auto saved = db.getDownload(added.id);
        CHECK(saved && saved->origin == "auto" && saved->followId == std::optional<std::int64_t>(*id));
        CHECK(saved && saved->replaces == (std::vector<std::int64_t>{3, 4}));
        CHECK(saved && saved->libraryFiles == (Strings{"/a.mkv", "/a.srt"}));
        // Las manuales siguen siendo manuales
        download.messageId = 22;
        download.origin.clear();
        download.followId.reset();
        download.replaces.clear();
        const auto manual = db.addDownload(download);
        const auto manualSaved = db.getDownload(manual.id);
        CHECK(manualSaved && manualSaved->origin == "manual" && !manualSaved->followId && manualSaved->replaces.empty());

        CHECK(db.addAutoRelease(-500, 21, *id));
        CHECK(db.addAutoRelease(-500, 21, *id));  // Repetido: se ignora
        CHECK_EQ(static_cast<int>(db.listAutoReleases().size()), 1);

        // Historial: del más reciente al más antiguo, por páginas
        for (int i = 1; i <= 3; ++i) {
            CHECK(db.addActivity({0, 1000 + i, "completed", "Entrada " + std::to_string(i), -500, 21, *id, added.id}));
        }
        const auto page = db.listActivity(2, 0);
        CHECK(page.size() == 2 && page[0].message == "Entrada 3" && page[1].message == "Entrada 2");
        const auto next = page.empty() ? std::vector<DbManager::Activity>{} : db.listActivity(2, page.back().id);
        CHECK(next.size() == 1 && next[0].message == "Entrada 1" && next[0].downloadId == added.id);

        CHECK(db.deleteFollow(*id));
        CHECK(!db.getFollow(*id));
        CHECK(db.listFollows().empty());
    }
    fs::remove_all(base);
}

void testFollowMatching() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("telegarrm_match_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    {
        DbManager db((base / "test.db").string());
        CHECK(db.open());
        Catalog catalog(db);
        TmdbClient tmdb(db, "", (base / "images").string());
        MetadataService metadata(db, catalog, tmdb);

        const std::vector<Message> messages = {
            at(inTopic(photo(10, "Ted Lasso - Temporada 3 (1080p)"), 4), 1000),
            at(inTopic(video(11, "Ted Lasso 3x01.mkv", 1000), 4), 1000),
            at(inTopic(photo(20, "Ted Lasso - Temporada 4 (1080p)\n\nEpisodio 9"), 335), 2000),
            at(inTopic(video(21, "Ted Lasso 4x09.mkv", 1000), 335), 2000),
        };
        std::vector<Catalog::ItemPtr> items;
        for (Catalog::Item& item : build(-500, "Las Cositas", messages, kTopics)) {
            items.push_back(std::make_shared<const Catalog::Item>(std::move(item)));
        }
        CHECK_EQ(static_cast<int>(items.size()), 1);
        if (items.size() != 1) {
            return;
        }

        DbManager::Follow byAnchor = tracking::followFor(*items[0], metadata);
        byAnchor.anchorId = 20;  // Cualquier ficha de la obra vale
        DbManager::Follow byKey = tracking::followFor(*items[0], metadata);
        byKey.anchorId = 999;    // La ficha ya no existe: por la clave de obra
        DbManager::Follow gone = byKey;
        gone.workKey = "series|otraserie|";
        const auto matches = tracking::matchFollows(items, {byAnchor, byKey, gone}, metadata);
        CHECK(matches.size() == 3 && matches[0].item == items[0] && matches[1].item == items[0] && !matches[2].item);
        CHECK_EQ(byAnchor.workKey, MetadataService::workKey(*items[0]));
    }
    fs::remove_all(base);
}

void testRemoveReplacedFiles() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("telegarrm_replace_" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path movies = base / "peliculas";
    const fs::path folder = movies / "Dune (2021) [tmdbid-438631]";
    const fs::path old = folder / "Dune (2021) - 1080p.mkv";
    const fs::path oldSubtitle = folder / "Dune (2021) - 1080p.Spanish.srt";
    const fs::path fresh = folder / "Dune (2021) - 4K HDR.mkv";
    const fs::path outside = base / "otra" / "importante.mkv";
    const fs::path episode = base / "series" / "Serie (2020)" / "Season 01" / "Serie S01E01 - 1080p.mkv";
    for (const fs::path& file : {old, oldSubtitle, fresh, outside, episode}) {
        writeFile(file, 10, 'x');
    }
    std::error_code ec;
    fs::create_symlink(outside, folder / "enlace.mkv", ec);

    // Nada fuera de la biblioteca, ni enlaces, ni la versión nueva
    const std::size_t removed = library::removeFiles(
        {old.string(), oldSubtitle.string(), fresh.string(), outside.string(), (folder / "enlace.mkv").string(),
         (movies / ".." / "otra" / "importante.mkv").string(), (folder / "no-existe.mkv").string()},
        {movies.string()}, {fresh.string()});
    CHECK_EQ(static_cast<int>(removed), 2);
    CHECK(!fs::exists(old) && !fs::exists(oldSubtitle));
    CHECK(fs::exists(fresh) && fs::exists(outside));
    CHECK(fs::is_symlink(folder / "enlace.mkv"));

    // El episodio sustituido se va con sus carpetas vacías, pero la biblioteca se queda
    CHECK_EQ(static_cast<int>(library::removeFiles({episode.string()}, {movies.string(), (base / "series").string()}, {})), 1);
    CHECK(!fs::exists(base / "series" / "Serie (2020)"));
    CHECK(fs::exists(base / "series"));
    // Sin bibliotecas definidas no se borra nada
    CHECK_EQ(static_cast<int>(library::removeFiles({fresh.string()}, {"", "/"}, {})), 0);
    CHECK(fs::exists(fresh));
    fs::remove_all(base);
}


void testVideoQuality() {
    CHECK_EQ(library::qualityFromSize(3840, 2160), "2160p");
    CHECK_EQ(library::qualityFromSize(3840, 1600), "2160p");  // Panorámica
    CHECK_EQ(library::qualityFromSize(1920, 1080), "1080p");
    CHECK_EQ(library::qualityFromSize(1920, 800), "1080p");
    CHECK_EQ(library::qualityFromSize(1280, 720), "720p");
    CHECK_EQ(library::qualityFromSize(1280, 534), "720p");
    CHECK_EQ(library::qualityFromSize(720, 576), "576p");
    CHECK_EQ(library::qualityFromSize(720, 480), "480p");
    CHECK_EQ(library::qualityFromSize(640, 360), "360p");
    CHECK_EQ(library::qualityFromSize(0, 0), "");

    // Salidas reales de ffprobe en la Pi (07/10/2026)
    const auto spiderMan = library::parseProbe(
        R"({"programs":[],"stream_groups":[],"streams":[{"codec_name":"h264","width":1280,"height":720}]})");
    CHECK(spiderMan && spiderMan->quality == "720p" && !spiderMan->hdr);
    const auto muppets = library::parseProbe(
        R"({"streams":[{"codec_name":"hevc","width":3840,"height":2160,"color_space":"bt2020nc",)"
        R"("color_transfer":"smpte2084","color_primaries":"bt2020"}]})");
    CHECK(muppets && muppets->quality == "2160p" && muppets->hdr == std::optional<bool>(true));
    const auto sdr = library::parseProbe(R"({"streams":[{"width":1920,"height":1080,"color_transfer":"bt709"}]})");
    CHECK(sdr && sdr->hdr == std::optional<bool>(false));
    const auto dolby = library::parseProbe(
        R"({"streams":[{"width":3840,"height":2160,"side_data_list":[{"side_data_type":"DOVI configuration record"}]}]})");
    CHECK(dolby && dolby->hdr == std::optional<bool>(true));
    // Avisos antes del JSON, archivo no reconocido y salida rota
    CHECK(library::parseProbe("[mkv] aviso\n{\"streams\":[{\"width\":1280,\"height\":720}]}").has_value());
    CHECK(!library::parseProbe("roto.mkv: Invalid data found when processing input\n{}"));
    CHECK(!library::parseProbe("{\"streams\":[{\"width\":"));
}

void testLibraryProbe() {
    namespace fs = std::filesystem;
    const char* ffmpeg = "/usr/bin/ffmpeg";
    if (library::findFfprobe().empty() || ::access(ffmpeg, X_OK) != 0) {
        std::cout << "(ffmpeg no está instalado: se omite la prueba con vídeos reales)" << std::endl;
        return;
    }
    const fs::path base = fs::temp_directory_path() / ("telegarrm_probe_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base / "descargas");
    fs::create_directories(base / "series");
    // Un fotograma de 1280x720, aunque el nombre y la ficha digan 1080p
    const fs::path video = base / "descargas" / "1x02 - Ultimate Spiderman 1080p.mkv";
    const process::Result made = process::run({ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
                                               "color=c=black:s=1280x720:d=0.04", "-frames:v", "1", "-c:v", "ffv1",
                                               video.string()},
                                              nullptr, nullptr);
    CHECK(made.started && made.exitCode == 0);
    const auto probed = library::probeVideo(video.string());
    CHECK(probed && probed->quality == "720p");

    library::ImportRequest request;
    request.id = 2;
    request.kind = "series";
    request.work = {"Ultimate Spider-Man", 2012, 34391};
    request.season = 1;
    request.episode = 2;
    request.quality = "1080p";
    request.parts = {video.string()};
    request.libraryRoot = (base / "series").string();
    // Antes de importarlo: el principio del vídeo dentro de un ZIP, con deflate (lo normal en ZIP), sin
    // comprimir y con otro archivo delante, como en la comprobación previa a la descarga (D-042)
    const std::string sevenZip = library::findSevenZip();
    if (!sevenZip.empty()) {
        writeFile(base / "zip" / "info.nfo", 300, 'n');
        const std::vector<std::vector<std::string>> archives = {
            {"a", "-tzip", (base / "deflate.zip").string(), video.string()},
            {"a", "-tzip", "-mx=0", (base / "store.zip").string(), video.string()},
            {"a", "-tzip", "-mx=0", (base / "nfo.zip").string(), (base / "zip" / "info.nfo").string(), video.string()},
        };
        for (std::vector<std::string> args : archives) {
            args.insert(args.begin(), sevenZip);
            args.insert(args.begin() + 2, "-bso0");
            const process::Result packed = process::run(args, nullptr, nullptr);
            CHECK(packed.started && packed.exitCode == 0);
        }
        for (const char* name : {"deflate.zip", "store.zip", "nfo.zip"}) {
            std::ifstream in(base / name, std::ios::binary);
            const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string reason;
            const auto prefix = library::videoFromPrefix(data.substr(0, data.size() * 3 / 4), reason);
            CHECK(prefix.has_value());
            if (prefix) {
                std::ofstream(base / "prefijo.mkv", std::ios::binary) << *prefix;
                const auto info = library::probeVideo((base / "prefijo.mkv").string(), nullptr, true);
                CHECK(info && info->quality == "720p");
            }
            if (!prefix) {
                std::cerr << "    " << name << ": " << reason << std::endl;
            }
        }
    }

    const library::ImportResult result = library::importRelease(request, nullptr, nullptr);
    CHECK(result.ok);
    CHECK(result.probed && result.probed->quality == "720p");
    CHECK(fs::exists(base / "series" / "Ultimate Spider-Man (2012) [tmdbid-34391]" / "Season 01" /
                     "Ultimate Spider-Man S01E02 - 720p.mkv"));
    fs::remove_all(base);
}

void testMissingEpisodes() {
    const std::vector<Message> messages = {
        video(1, "Mi Serie 1x01 1080p.mkv", 1000),
        video(2, "Mi Serie 1x01 4K.mkv", 4000),
        video(3, "Mi Serie 1x02 1080p.mkv", 1000),
        video(4, "Mi Serie 1x03-04 1080p.mkv", 2000),
        video(5, "Mi Serie 1x04 720p.mkv", 500),
        video(6, "Mi Serie 2x01 4K 3D.mkv", 5000),
        video(7, "Mi Serie 2x01 1080p.mkv", 1000),
    };
    const auto items = build(-400, "Canal", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    const Catalog::Item& item = items[0];
    const auto firstParts = [&item](const std::vector<std::size_t>& chosen) {
        std::vector<std::int64_t> ids;
        for (const std::size_t index : chosen) {
            ids.push_back(item.releases[index].parts.front().messageId);
        }
        return ids;
    };

    // Sin nada descargado: la serie completa, la mejor versión de cada episodio. El 1x03-04 cubre el
    // 1x04 y el 3D nunca se elige solo
    CHECK(firstParts(tracking::missingEpisodes(item, {}, "")) == (std::vector<std::int64_t>{2, 3, 4, 7}));
    // Hasta 1080p
    CHECK(firstParts(tracking::missingEpisodes(item, {}, "1080p")) == (std::vector<std::int64_t>{1, 3, 4, 7}));
    // Con el 1x02 en la biblioteca y el 1x01 en cola (en cualquier versión) solo faltan los demás
    const std::vector<tracking::Owned> owned = {{10, "completed", 20, "1080p", 1, 2, 0}, {11, "queued", 20, "1080p", 1, 1, 0}};
    CHECK(firstParts(tracking::missingEpisodes(item, owned, "")) == (std::vector<std::int64_t>{4, 7}));
    const tracking::EpisodeCount count = tracking::countEpisodes(item, owned);
    CHECK_EQ(count.known, 5);
    CHECK_EQ(count.owned, 2);
    // Las películas no tienen episodios
    const auto movies = build(-400, "Canal", {video(1, "Peli (2020) 1080p.mkv", 1000)});
    CHECK(movies.size() == 1 && tracking::missingEpisodes(movies[0], {}, "").empty());
}


void testLibraryOnDisk() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("telegarrm_disk_" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path series = base / "series";
    // Carpeta con el identificador de TMDB pero otro nombre (ej. creada por otra herramienta)
    const fs::path folder = series / "Ultimate Spiderman [tmdbid=34391]";
    writeFile(folder / "Season 01" / "Ultimate Spider-Man S01E01 - 720p.mkv", 10, 'a');
    writeFile(folder / "Season 01" / "Ultimate Spider-Man S01E02-E03 - 1080p.mkv", 10, 'b');
    writeFile(folder / "Season 01" / "Ultimate Spider-Man S01E01 - 720p.Spanish.srt", 10, 'c');
    writeFile(folder / ".telegarrm" / "5" / "Ultimate Spider-Man S01E04.mkv", 10, 'd');  // Temporal: no cuenta
    writeFile(series / "Otra serie (2020)" / "Season 01" / "Otra serie S01E01.mkv", 10, 'e');

    const library::WorkInfo work{"Ultimate Spider-Man", 2012, 34391};
    CHECK_EQ(library::findWorkFolder(series.string(), work), folder.string());
    CHECK_EQ(library::findWorkFolder(series.string(), {"Otra serie", 2020, 0}), (series / "Otra serie (2020)").string());
    CHECK_EQ(library::findWorkFolder(series.string(), {"No existe", 2020, 99}), "");
    CHECK_EQ(library::findWorkFolder("", work), "");

    auto videos = library::videosOnDisk(series.string(), work);
    std::sort(videos.begin(), videos.end(),
              [](const library::DiskVideo& a, const library::DiskVideo& b) { return a.episode < b.episode; });
    CHECK_EQ(static_cast<int>(videos.size()), 2);
    if (videos.size() == 2) {
        CHECK(videos[0].season == 1 && videos[0].episode == 1 && videos[0].quality == "720p");
        CHECK(videos[1].episode == 2 && videos[1].episodeEnd == 3 && videos[1].quality == "1080p");
    }

    // Lo que hay en la biblioteca cuenta como descargado (aunque no venga de ninguna descarga)
    const std::vector<Message> messages = {
        video(1, "1x01 - Ultimate Spiderman.mkv", 100), video(2, "1x02 - Ultimate Spiderman.mkv", 100),
        video(3, "1x03 - Ultimate Spiderman.mkv", 100), video(4, "1x04 - Ultimate Spiderman.mkv", 100)};
    const auto items = build(-5530696118, "Prueba Claude", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() == 1) {
        const std::vector<tracking::Owned> owned = tracking::ownedVersions(items[0], {}, videos);
        CHECK(owned.size() == 2 && owned[0].downloadId == 0);
        const auto missing = tracking::missingEpisodes(items[0], owned, "");
        CHECK(missing.size() == 1 && items[0].releases[missing[0]].episode == 4);
        CHECK_EQ(tracking::countEpisodes(items[0], owned).owned, 3);
    }
    fs::remove_all(base);
}

void testImportSupersedes() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("telegarrm_supersede_" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path season = base / "series" / "Ultimate Spider-Man (2012) [tmdbid-34391]" / "Season 01";
    // Ya en la biblioteca: el 1x01 con otra etiqueta y sus subtítulos, y el 1x02
    writeFile(season / "Ultimate Spider-Man S01E01 - 1080p.mkv", 50, 'a');
    writeFile(season / "Ultimate Spider-Man S01E01 - 1080p.Spanish.srt", 5, 'b');
    writeFile(season / "Ultimate Spider-Man S01E02.mkv", 50, 'c');
    writeFile(base / "descargas" / "1x01 - Ultimate Spiderman.mkv", 60, 'd');

    library::ImportRequest request;
    request.id = 12;
    request.kind = "series";
    request.work = {"Ultimate Spider-Man", 2012, 34391};
    request.season = 1;
    request.episode = 1;
    request.quality = "720p";
    request.parts = {(base / "descargas" / "1x01 - Ultimate Spiderman.mkv").string()};
    request.libraryRoot = (base / "series").string();
    request.replaceOthers = true;
    library::ImportResult result = library::importRelease(request, nullptr, nullptr);
    CHECK(result.ok);
    auto superseded = result.superseded;
    std::sort(superseded.begin(), superseded.end());
    CHECK(superseded == (Strings{(season / "Ultimate Spider-Man S01E01 - 1080p.Spanish.srt").string(),
                                 (season / "Ultimate Spider-Man S01E01 - 1080p.mkv").string()}));
    // No se borra aquí: lo hace el gestor de descargas (según Ajustes)
    CHECK(fs::exists(season / "Ultimate Spider-Man S01E01 - 1080p.mkv"));

    // Películas: a mano se pueden tener varias versiones
    const fs::path movies = base / "peliculas";
    writeFile(movies / "Dune (2021)" / "Dune (2021) - 1080p.mkv", 50, 'e');
    writeFile(base / "descargas" / "Dune 4K.mkv", 80, 'f');
    library::ImportRequest movie;
    movie.id = 13;
    movie.kind = "movie";
    movie.work = {"Dune", 2021, 0};
    movie.quality = "2160p";
    movie.parts = {(base / "descargas" / "Dune 4K.mkv").string()};
    movie.libraryRoot = movies.string();
    result = library::importRelease(movie, nullptr, nullptr);
    CHECK(result.ok && result.superseded.empty());
    // ...salvo que sea una mejora del seguimiento
    writeFile(base / "descargas" / "Dune 4K HDR.mkv", 90, 'g');
    movie.hdr = true;
    movie.parts = {(base / "descargas" / "Dune 4K HDR.mkv").string()};
    movie.replaceOthers = true;
    result = library::importRelease(movie, nullptr, nullptr);
    CHECK(result.ok && result.superseded.size() == 2);
    fs::remove_all(base);
}


void testFindVideoStart() {
    const std::string ebml("\x1A\x45\xDF\xA3\xA3\x42\x86\x81\x01\x42\x82\x88matroska", 20);
    CHECK(library::findVideoStart(ebml + "datos") == std::optional<std::size_t>(0));
    // Dentro de un ZIP sin comprimir: cabecera local (PK\3\4...), nombre y después el MKV
    const std::string zip = std::string("PK\x03\x04\x0A\x00\x00\x00", 8) + std::string(22, '\0') + "Peli (2020) 1080p.mkv";
    CHECK(library::findVideoStart(zip + ebml) == std::optional<std::size_t>(zip.size()));
    // Una firma EBML suelta sin "matroska" detrás no cuenta
    CHECK(!library::findVideoStart(std::string("\x1A\x45\xDF\xA3", 4) + std::string(80, 'x')));
    const std::string mp4 = std::string("\x00\x00\x00\x20", 4) + "ftypisom";
    CHECK(library::findVideoStart("Rar!\x1A\x07" + mp4) == std::optional<std::size_t>(6));
    CHECK(library::findVideoStart(std::string("RIFF\x10\x00\x00\x00" "AVI LIST", 16)) == std::optional<std::size_t>(0));
    CHECK(!library::findVideoStart("RIFF\x10"));  // Cortado: no se lee fuera de los datos
    CHECK(!library::findVideoStart("texto sin vídeo"));
    // Vídeo Flash con extensión .mp4 (real: "Gente Hablando - S01E05.mp4" empieza por 46 4C 56 01)
    CHECK(library::findVideoStart(std::string("FLV\x01\x05\x00\x00\x00\x09", 9)) == std::optional<std::size_t>(0));
}


void testCatalogEvents() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("telegarrm_events_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    {
        DbManager db((base / "test.db").string());
        CHECK(db.open());
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        const auto inChat = [](std::int64_t chatId, Message message, std::int64_t date) {
            message.chatId = chatId;
            message.date = date;
            return message;
        };
        CHECK(db.addChannel(-500, "Prueba Claude"));
        CHECK(db.saveSyncBatch(-500, {inChat(-500, video(1, "1x01 - Ultimate Spiderman.mkv", 100), now - 100)},
                               {1, 1, true}));
        Catalog catalog(db);
        catalog.rebuildChannel(-500);
        CHECK_EQ(catalog.lastEventId(), std::int64_t{0});  // Al arrancar no hay novedades

        // Llega el 1x02: una novedad de la obra, con su archivo
        CHECK(db.saveSyncBatch(-500, {inChat(-500, video(2, "1x02 - Ultimate Spiderman.mkv", 100), now)}, {2, 1, true}));
        catalog.rebuildChannel(-500);
        CHECK_EQ(catalog.lastEventId(), std::int64_t{1});
        auto events = catalog.eventsAfter(0);
        CHECK(events.size() == 1 && events[0].releases.size() == 1 && events[0].releases[0].episode == 2);
        CHECK(events.size() == 1 && events[0].anchorMessageId == 1 && events[0].title == "Ultimate Spiderman");

        // Un RAR en dos partes: la primera es novedad; la segunda, no (es el mismo archivo)
        CHECK(db.saveSyncBatch(-500, {inChat(-500, video(3, "1x03 - Ultimate Spiderman.part1.rar", 2000, "", ""), now)},
                               {3, 1, true}));
        catalog.rebuildChannel(-500);
        CHECK(db.saveSyncBatch(-500, {inChat(-500, video(4, "1x03 - Ultimate Spiderman.part2.rar", 500, "", ""), now)},
                               {4, 1, true}));
        catalog.rebuildChannel(-500);
        CHECK_EQ(catalog.lastEventId(), std::int64_t{2});
        events = catalog.eventsAfter(1);
        CHECK(events.size() == 1 && events[0].releases[0].episode == 3);

        // Algo antiguo que aparece ahora (ej. al leer el historial) no es novedad
        CHECK(db.saveSyncBatch(-500, {inChat(-500, video(5, "1x04 - Ultimate Spiderman.mkv", 100), now - 3 * 86400)},
                               {5, 1, true}));
        catalog.rebuildChannel(-500);
        // Un canal nuevo tampoco, aunque su contenido sea reciente
        CHECK(db.addChannel(-600, "Otro canal"));
        CHECK(db.saveSyncBatch(-600, {inChat(-600, video(1, "Otra serie 1x01.mkv", 100), now)}, {1, 1, true}));
        catalog.rebuildChannel(-600);
        CHECK_EQ(catalog.lastEventId(), std::int64_t{2});
        CHECK(catalog.eventsAfter(2).empty());
    }
    fs::remove_all(base);
}


// --- Canales de anime (D-047), con nombres reales de CrunchyShur (08/10/2026) ---

void testAnimeEpisodes() {
    const auto anime = [](const std::string& name) { return media::parseEpisode(name, true); };
    auto e = anime("[Ñ] Boku no Hero Academia - 01 [BD 720p] [142592D6].mkv");
    CHECK(e && e->absolute && e->season == 1 && e->episode == 1);
    CHECK(e && media::titleKey(e->seriesName).find("bokunoheroacademia") != std::string::npos);
    e = anime("[Ñ] Boku no Hero Academia T2 - 25.mkv");
    CHECK(e && !e->absolute && e->season == 2 && e->episode == 25);
    e = anime("Boku no Hero Academia T6 - 25 END [1080p].mkv");
    CHECK(e && e->season == 6 && e->episode == 25);
    e = anime("My Hero Academia Vigilantes S2 - 08.mkv");
    CHECK(e && !e->absolute && e->season == 2 && e->episode == 8 && e->seriesName == "My Hero Academia Vigilantes");
    e = anime("Boku no Hero - Final Season - 10.mkv");
    CHECK(e && e->absolute && e->finalSeason && e->episode == 10 && e->seriesName == "Boku no Hero");
    e = anime("Dragon Ball - 001 [h264 AAC ES-JP].mp4");
    CHECK(e && e->absolute && e->episode == 1 && e->seriesName == "Dragon Ball");
    e = anime("[NTF] Naruto Shippuden 003 [7E936FD9].avi");
    CHECK(e && e->absolute && e->episode == 3);
    e = anime("FWnF Bleach Kai 53.mkv");
    CHECK(e && e->absolute && e->episode == 53);
    e = anime("[AS] LHG HD - 01 - En la noche eterna.mp4");
    CHECK(e && e->episode == 1 && e->episodeTitle == "En la noche eterna");
    e = anime("42 - Despertar.mkv");
    CHECK(e && e->episode == 42 && e->episodeTitle == "Despertar");
    e = anime("JoJo's Bizarre Adventure Diamond is Unbreakable S3 EP11.mkv");
    CHECK(e && !e->absolute && e->season == 3 && e->episode == 11);
    // No son episodios: un ONA, un año, una película numerada
    CHECK(!anime("Boku no Hero Academia 5 ONA - 01 HLB.mkv"));
    CHECK(!anime("Batman - 1989.mkv"));
    CHECK(!anime("[Ñ] Boku no Hero Academia - Película 1.part1.rar"));
    // Fuera de los canales de anime, nada cambia (Las Cositas: "Ladybug - 027.mkv" no es un episodio)
    CHECK(!media::parseEpisode("Ladybug - 027.mkv"));
    CHECK(!media::parseEpisode("[Ñ] Boku no Hero Academia - 01 [BD 720p].mkv"));
    CHECK(media::isAnimeNumbered("Ladybug - 027.mkv"));
    CHECK(!media::isAnimeNumbered("Ladybug 1x27.mkv"));
    CHECK(!media::isAnimeNumbered("Rocky (1976) (1080p).zip.001"));

    // Fichas
    media::Ficha f = media::parseFicha(
        "Vigilante: Boku no Hero Academia Illegals S2\nMy Hero Academia: Vigilantes Season 2\n\n\xE2\xAD\x90\xEF\xB8\x8F MyAnimeList Score: 8.00",
        true);
    CHECK_EQ(f.title, "Vigilante: Boku no Hero Academia Illegals");
    CHECK_EQ(f.season, 2);
    CHECK_EQ(f.alternateTitles, Strings{"My Hero Academia: Vigilantes"});
    f = media::parseFicha("Boku no Hero Academia: Final Season\n\n\xE2\xAD\x90\xEF\xB8\x8F MyAnimeList Score:", true);
    CHECK(f.finalSeason && f.title == "Boku no Hero Academia");
    f = media::parseFicha("Temporada 3 - Boku no Hero Academia", true);
    CHECK(f.season == 3 && f.title == "Boku no Hero Academia");
    // Fuera de los canales de anime, igual que siempre
    f = media::parseFicha("Ataque a los Titanes: La temporada final\n\nSINOPSIS:");
    CHECK(!f.finalSeason && f.title == "Ataque a los Titanes: La temporada final");
    f = media::parseFicha("Rocas Cochambrosas\nCumbres Mocarrosas");
    CHECK(f.alternateTitles.empty());
}

void testCatalogAnimeChannel() {
    const std::vector<DbManager::Topic> topics = {{3840, "Boku no Hero Academia", 0}, {2187, "Naruto", 0},
                                                  {4067, "Listado Animes", 0}};
    std::vector<Message> messages;
    std::int64_t id = 1;
    const auto add = [&](Message message, std::int64_t topic) { messages.push_back(inTopic(message, topic)); };
    const auto two = [](int n) { return (n < 10 ? "0" : "") + std::to_string(n); };
    add(photo(id++, "Temporada 1 - Boku no Hero Academia"), 3840);
    for (int n = 1; n <= 13; ++n) {
        add(video(id++, "[Ñ] Boku no Hero Academia - " + two(n) + " [BD 720p].mkv", 500), 3840);
    }
    add(photo(id++, "OVA 1 - ¡Rescate! ¡Entrenamiento de salvamento!"), 3840);
    add(video(id++, "[Ñ] Boku no Hero Academia - OVA 1 [720p].mkv", 300), 3840);
    add(photo(id++, "Temporada 2 - Boku no Hero Academia"), 3840);
    for (int n = 1; n <= 12; ++n) {
        add(video(id++, "[Ñ] Boku no Hero Academia T2 - " + two(n) + ".mkv", 500), 3840);
    }
    add(photo(id++, "Boku no Hero Academia the Movie 4: You're Next\nMy Hero Academia: You're Next\n\n"
                    "\xE2\xAD\x90\xEF\xB8\x8F MyAnimeList Score: 7.51"), 3840);
    add(video(id++, "Boku no Hero Academia - You're Next [1080p].part1.rar", 2000, "", ""), 3840);
    add(video(id++, "Boku no Hero Academia - You're Next [1080p].part2.rar", 1000, "", ""), 3840);
    add(photo(id++, "Boku no Hero Academia: Final Season\n\n\xE2\xAD\x90\xEF\xB8\x8F MyAnimeList Score:"), 3840);
    for (int n = 1; n <= 5; ++n) {
        add(video(id++, "Boku no Hero - Final Season - " + two(n) + ".mkv", 1500), 3840);
    }
    // Una ficha por arco: son la serie del tema
    add(photo(id++, "Exámenes Chūnin"), 2187);
    for (int n = 20; n <= 25; ++n) {
        add(video(id++, "Naruto - 0" + std::to_string(n) + ".mkv", 300), 2187);
    }
    add(photo(id++, "Destrucción de la Hoja"), 2187);
    for (int n = 26; n <= 30; ++n) {
        add(video(id++, "Naruto - 0" + std::to_string(n) + ".mkv", 300), 2187);
    }

    const auto items = build(-700, "CrunchyShur", messages, topics);
    const Catalog::Item* hero = nullptr;
    const Catalog::Item* movie = nullptr;
    const Catalog::Item* naruto = nullptr;
    for (const Catalog::Item& item : items) {
        hero = item.title == "Boku no Hero Academia" && item.kind == "series" ? &item : hero;
        movie = item.kind == "movie" && item.title.find("You're Next") != std::string::npos ? &item : movie;
        naruto = item.title == "Naruto" ? &item : naruto;
    }
    CHECK_EQ(static_cast<int>(items.size()), 3);
    CHECK(hero != nullptr && movie != nullptr && naruto != nullptr);
    if (hero) {
        // T1 (de la ficha), T2 y la "Final Season", que pasa a ser la 3; la OVA, como otro archivo
        CHECK_EQ(hero->seasonCount, 3);
        CHECK_EQ(hero->episodeCount, 30);
        CHECK_EQ(static_cast<int>(hero->releases.size()), 31);
        const bool finalIsThird = std::any_of(hero->releases.begin(), hero->releases.end(),
                                              [](const Catalog::Release& r) { return r.season == 3 && r.episode == 5; });
        CHECK(finalIsThird);
    }
    if (movie) {
        CHECK_EQ(movie->alternateTitles, Strings{"My Hero Academia: You're Next"});
    }
    if (naruto) {
        CHECK(naruto->kind == "series" && naruto->seasonCount == 1 && naruto->episodeCount == 11);
    }
}

int main() {
    testEpisodes();
    testFichas();
    testHelpers();
    testCatalogSpiderMan();
    testCatalogGeneratorRex();
    testCatalogLibraryChannel();
    testCatalogWithoutFicha();
    testCatalogRepeatedSeriesName();
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
    testDownloadQueue();
    testLibraryNames();
    testProcessWithoutShell();
    testLibraryImport();
    testTrackingRules();
    testTrackingSeries();
    testTrackingMovies();
    testFollowsDatabase();
    testFollowMatching();
    testRemoveReplacedFiles();
    testVideoQuality();
    testLibraryProbe();
    testMissingEpisodes();
    testLibraryOnDisk();
    testImportSupersedes();
    testFindVideoStart();
    testCatalogEvents();
    testAnimeEpisodes();
    testCatalogAnimeChannel();

    std::cout << (checks - failures) << "/" << checks << " comprobaciones correctas" << std::endl;
    return failures == 0 ? 0 : 1;
}

// Tests del parser y de la construcción del catálogo. Sin framework: se ejecutan con
// `ctest --test-dir build` o directamente con ./build/telegarrm_tests.
// Los casos "reales" reproducen mensajes de canales sincronizados en la Pi (06/10/2026).

#include <cstdint>
#include <iostream>
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
    const auto items = Catalog::buildItems(-100, "Ultimate Spider-Man [Castellano] [Arreglado]", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    const Catalog::Item& item = items[0];
    CHECK_EQ(item.kind, "series");
    CHECK_EQ(item.title, "Ultimate Spider-Man");
    CHECK_EQ(item.year, std::optional<int>(2015));
    CHECK_EQ(item.quality, "1080p");
    CHECK_EQ(item.languages, Strings{"Castellano"});
    CHECK_EQ(item.genres, (Strings{"Acción", "Comedia"}));
    CHECK_EQ(item.anchorMessageId, std::int64_t{1});
    CHECK_EQ(item.posterMessageId, std::int64_t{1});
    CHECK_EQ(item.seasonCount, 2);
    CHECK_EQ(item.episodeCount, 4);
    CHECK_EQ(item.totalSize, std::int64_t{1'588'000'000});
    // "Ultimate Spiderman" es el nombre de la serie, no el título de cada episodio
    CHECK_EQ(item.files[0].episodeTitle, "");
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
    const auto items = Catalog::buildItems(-200, "Generator Rex [Castellano]", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    const Catalog::Item& item = items[0];
    CHECK_EQ(item.title, "Generator Rex");
    CHECK_EQ(item.posterMessageId, std::int64_t{10});
    CHECK_EQ(item.seasonCount, 2);
    CHECK_EQ(item.episodeCount, 3);
    CHECK_EQ(item.files[0].episodeTitle, "El Día Que Todo Cambió");
    CHECK_EQ(item.files[1].episodeTitle, "Teoría De Cuerdas");
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
    const auto items = Catalog::buildItems(-300, "Mi biblioteca", messages);
    CHECK_EQ(static_cast<int>(items.size()), 3);
    if (items.size() != 3) {
        return;
    }
    CHECK_EQ(items[0].kind, "series");
    CHECK_EQ(items[0].title, "Serie A");
    CHECK_EQ(items[0].year, std::optional<int>(2020));
    CHECK_EQ(items[1].kind, "movie");
    CHECK_EQ(items[1].title, "Película B");
    CHECK_EQ(items[1].year, std::optional<int>(2010));
    CHECK_EQ(items[1].quality, "1080p");
    CHECK_EQ(items[2].kind, "movie");
    CHECK_EQ(static_cast<int>(items[2].files.size()), 2);
    CHECK(items[2].files[0].archive);
}

void testCatalogWithoutFicha() {
    // Sin ficha: el título sale de los nombres de fichero; una resubida cuenta como el mismo episodio
    const std::vector<Message> messages = {
        video(1, "Mi Serie 1x01.mkv", 100),
        video(2, "Mi Serie 1x02.mkv", 100),
        video(3, "Mi Serie 1x01.mkv", 120),
    };
    const auto items = Catalog::buildItems(-400, "Canal", messages);
    CHECK_EQ(static_cast<int>(items.size()), 1);
    if (items.size() != 1) {
        return;
    }
    CHECK_EQ(items[0].anchorMessageId, std::int64_t{0});
    CHECK_EQ(items[0].title, "Mi Serie");
    CHECK_EQ(items[0].episodeCount, 2);
    CHECK_EQ(static_cast<int>(items[0].files.size()), 3);
    // Las dos versiones de 1x01 quedan juntas, por orden de publicación
    CHECK_EQ(items[0].files[0].messageId, std::int64_t{1});
    CHECK_EQ(items[0].files[1].messageId, std::int64_t{3});
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

    std::cout << (checks - failures) << "/" << checks << " comprobaciones correctas" << std::endl;
    return failures == 0 ? 0 : 1;
}

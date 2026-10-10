// Herramienta de desarrollo: calcula el catálogo de una copia de la BD y lo escribe en un archivo, una
// obra por línea (separada por tabuladores), para comparar el catálogo antes y después de un cambio en
// el parser sin tocar el servicio. Uso: telegarrm_catalog_dump <copia.db> <salida.tsv> [--tmdb]
// Con --tmdb (y TELEGARRM_TMDB_TOKEN) añade dos columnas: la coincidencia de TMDB guardada en la copia
// y la que da el criterio actual, para revisar un cambio del criterio obra por obra (D-048). Las
// respuestas de TMDB se guardan en la caché de la copia.
// Trabaja sobre una COPIA: abrir la BD aplica las migraciones pendientes.

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "db_manager.hpp"
#include "metadata.hpp"
#include "tmdb_client.hpp"

namespace {

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        out += (out.empty() ? "" : " / ") + value;
    }
    return out;
}

// "tv:65930 My Hero Academia", "-" sin coincidencia o "?" si TMDB no respondió
std::string describe(const DbManager::Metadata* info) {
    if (!info) {
        return "?";
    }
    if (info->mediaType.empty()) {
        return "-";
    }
    return info->mediaType + ":" + std::to_string(info->providerId) + " " + info->title;
}

}  // namespace

int main(int argc, char** argv) {
    const bool withTmdb = argc == 4 && std::strcmp(argv[3], "--tmdb") == 0;
    if (argc != 3 && !withTmdb) {
        std::cerr << "Uso: " << argv[0] << " <copia.db> <salida.tsv> [--tmdb]" << std::endl;
        return 2;
    }
    DbManager db(argv[1]);
    if (!db.open()) {
        return 1;
    }
    Catalog catalog(db);
    catalog.rebuildAll();

    std::unique_ptr<TmdbClient> tmdb;
    std::unique_ptr<MetadataService> metadata;
    if (withTmdb) {
        const char* token = std::getenv("TELEGARRM_TMDB_TOKEN");
        if (!token || !*token) {
            std::cerr << "--tmdb necesita TELEGARRM_TMDB_TOKEN" << std::endl;
            return 2;
        }
        tmdb = std::make_unique<TmdbClient>(db, token, std::string(argv[1]) + ".imagenes");
        metadata = std::make_unique<MetadataService>(db, catalog, *tmdb);  // Sin start(): no lanza su hilo
    }

    std::ofstream out(argv[2]);
    std::size_t done = 0;
    for (const Catalog::ItemPtr& item : catalog.items()) {
        out << item->chatId << '\t' << item->anchorMessageId << '\t' << item->kind << '\t' << item->title << '\t'
            << join(item->alternateTitles) << '\t' << (item->year ? std::to_string(*item->year) : "") << '\t'
            << item->seasonCount << '\t' << item->episodeCount << '\t' << item->releases.size() << '\t'
            << join(item->topics);
        if (metadata) {
            const MetadataService::InfoPtr stored = metadata->lookup(*item);
            const std::optional<DbManager::Metadata> fresh = metadata->resolve(*item);
            out << '\t' << (stored ? describe(stored.get()) : "-") << '\t' << describe(fresh ? &*fresh : nullptr);
            if (++done % 250 == 0) {
                std::cerr << done << " obras buscadas en TMDB" << std::endl;
            }
        }
        out << '\n';
    }
    std::cerr << catalog.items().size() << " obras" << std::endl;
    return 0;
}

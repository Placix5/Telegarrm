// Herramienta de desarrollo: calcula el catálogo de una copia de la BD y lo escribe en un archivo, una
// obra por línea (separada por tabuladores), para comparar el catálogo antes y después de un cambio en
// el parser sin tocar el servicio. Uso: telegarrm_catalog_dump <copia.db> <salida.tsv>
// Trabaja sobre una COPIA: abrir la BD aplica las migraciones pendientes.

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "db_manager.hpp"

namespace {

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        out += (out.empty() ? "" : " / ") + value;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0] << " <copia.db> <salida.tsv>" << std::endl;
        return 2;
    }
    DbManager db(argv[1]);
    if (!db.open()) {
        return 1;
    }
    Catalog catalog(db);
    catalog.rebuildAll();
    std::ofstream out(argv[2]);
    for (const Catalog::ItemPtr& item : catalog.items()) {
        out << item->chatId << '\t' << item->anchorMessageId << '\t' << item->kind << '\t' << item->title << '\t'
            << join(item->alternateTitles) << '\t' << (item->year ? std::to_string(*item->year) : "") << '\t'
            << item->seasonCount << '\t' << item->episodeCount << '\t' << item->releases.size() << '\t'
            << join(item->topics) << '\n';
    }
    std::cerr << catalog.items().size() << " obras" << std::endl;
    return 0;
}

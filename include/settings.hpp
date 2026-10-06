#pragma once

#include <cstdint>
#include <optional>
#include <string>

class DbManager;

// Ajustes editables desde la web, guardados en la tabla settings (docs/DECISIONS.md, D-032).
struct AppSettings {
    // Búfer de descargas: files_directory de TDLib. Vacío = dentro de su base de datos (db/tdlib).
    // Se aplica al reiniciar el servicio.
    std::string downloadDir;
    // Bibliotecas finales (para Jellyfin). Las usará el postproceso de la Fase 3.
    std::string moviesDir;
    std::string seriesDir;
    // Espacio que debe quedar libre tras cada descarga
    std::int64_t minFreeBytes = 2'000'000'000;
};

AppSettings loadSettings(DbManager& db);
bool saveSettings(DbManager& db, const AppSettings& settings);

// Comprobación de una ruta: que sea absoluta, exista, sea una carpeta y el servicio pueda escribir
// en ella (se crea y se borra un fichero de prueba: con el aislamiento de systemd, los permisos del
// sistema de archivos no bastan para saberlo)
struct PathCheck {
    bool ok = false;
    std::string error;            // Motivo en castellano si !ok
    std::int64_t freeBytes = 0;
    std::optional<std::uint64_t> device;  // Sistema de archivos (para saber si mover es renombrar)
};

PathCheck checkPath(const std::string& path);

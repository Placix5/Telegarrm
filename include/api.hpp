#pragma once

#include <functional>
#include <string>

namespace httplib {
class Server;
}
class Catalog;
class ChannelSync;
class DbManager;
class DownloadManager;
class MetadataService;
class ReleaseProber;
class TelegramClient;
class TmdbClient;
class Tracker;

// Servicios que usa la API. Deben vivir más que el servidor HTTP.
struct ApiServices {
    DbManager& db;
    TelegramClient& telegram;
    ChannelSync& sync;
    Catalog& catalog;
    MetadataService& metadata;
    TmdbClient& tmdb;
    DownloadManager& downloads;
    Tracker& tracker;
    ReleaseProber& prober;
    // Búfer de descargas con el que arrancó TDLib (para saber si un cambio exige reiniciar)
    std::string activeDownloadDir;
    // Aviso si la carpeta configurada no se pudo usar al arrancar (vacío = sin aviso)
    std::string downloadDirWarning;
    // Parada ordenada seguida de reinicio (código de salida 75, ver deploy/telegarrm.service)
    std::function<void()> requestRestart;
};

// Registra los endpoints REST (/api/...)
void registerApiRoutes(httplib::Server& server, const ApiServices& services);

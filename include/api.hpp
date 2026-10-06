#pragma once

namespace httplib {
class Server;
}
class Catalog;
class ChannelSync;
class DbManager;
class DownloadManager;
class MetadataService;
class TelegramClient;
class TmdbClient;

// Servicios que usa la API. Deben vivir más que el servidor HTTP.
struct ApiServices {
    DbManager& db;
    TelegramClient& telegram;
    ChannelSync& sync;
    Catalog& catalog;
    MetadataService& metadata;
    TmdbClient& tmdb;
    DownloadManager& downloads;
};

// Registra los endpoints REST (/api/...)
void registerApiRoutes(httplib::Server& server, const ApiServices& services);

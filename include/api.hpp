#pragma once

namespace httplib {
class Server;
}
class Catalog;
class ChannelSync;
class DbManager;
class TelegramClient;

// Registra los endpoints REST (/api/...). Los objetos referenciados deben vivir más que el servidor.
void registerApiRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram, ChannelSync& sync,
                       Catalog& catalog);

#pragma once

namespace httplib {
class Server;
}
class DbManager;
class TelegramClient;

// Registra los endpoints REST (/api/...). db y telegram deben vivir más que el servidor.
void registerApiRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram);

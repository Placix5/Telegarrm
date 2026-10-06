#include <cstdio>
#include <iostream>
#include <string>

#include "db_manager.hpp"
#include "httplib.h"
#include "telegram_client.hpp"

namespace {

constexpr const char* kDbPath = "db/telegarrm.db";
constexpr int kPort = 8080;

// Escapa una cadena para incrustarla dentro de un literal JSON
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

int main() {
    std::cout << "Iniciando Telegarrm " TELEGARRM_VERSION " (Fase 1)..." << std::endl;

    // Base de datos: crea db/telegarrm.db y sus tablas si no existen
    DbManager db(kDbPath);
    if (!db.open()) {
        std::cerr << "Error: no se pudo inicializar la base de datos en '" << kDbPath << "'." << std::endl;
        return 1;
    }
    // Registrar la versión en ejecución; /api/status la lee después desde SQLite
    db.setSetting("version", TELEGARRM_VERSION);

    // Hilo de Telegram (simulado). Debe lanzarse antes de listen(), que es bloqueante.
    TelegramClient telegram;
    telegram.start();

    // Inicializar el servidor HTTP
    httplib::Server svr;

    // Endpoint de estado: incluye la versión leída de la tabla settings
    svr.Get("/api/status", [&db](const httplib::Request&, httplib::Response& res) {
        const auto dbVersion = db.getSetting("version");

        std::string body = R"({"status": "Telegarrm is running", "version": ")" TELEGARRM_VERSION R"(", )";
        if (dbVersion) {
            body += R"("database": {"status": "ok", "version": ")" + jsonEscape(*dbVersion) + R"("}})";
        } else {
            body += R"("database": {"status": "error", "version": null}})";
            res.status = 503;
        }
        res.set_content(body, "application/json");
    });

    // Configurar la carpeta web estática
    if (!svr.set_mount_point("/", "./web")) {
        std::cerr << "Advertencia: El directorio './web' no existe o no se puede montar." << std::endl;
    }

    // cpp-httplib activa SO_REUSEPORT por defecto, lo que deja a dos instancias escuchar
    // a la vez en el mismo puerto. Con SO_REUSEADDR se puede reiniciar enseguida,
    // pero una segunda instancia falla al hacer bind.
    svr.set_socket_options([](socket_t sock) {
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
    });

    if (!svr.bind_to_port("0.0.0.0", kPort)) {
        std::cerr << "Error: no se pudo escuchar en el puerto " << kPort << "." << std::endl;
        return 1;
    }
    std::cout << "Servidor web escuchando en http://localhost:" << kPort << std::endl;

    // listen_after_bind() bloquea el hilo principal, actuando como bucle del daemon.
    // Al salir de main, los destructores detienen el hilo de Telegram y cierran la BD.
    svr.listen_after_bind();

    return 0;
}

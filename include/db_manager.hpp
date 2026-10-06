#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct sqlite3;

// Gestor de la base de datos SQLite de Telegarrm.
// Los métodos públicos son seguros entre hilos: cpp-httplib atiende las
// peticiones desde un pool de hilos y TDLib escribirá desde su propio hilo.
class DbManager {
public:
    explicit DbManager(std::string path);

    DbManager(const DbManager&) = delete;
    DbManager& operator=(const DbManager&) = delete;

    // Abre (o crea) el fichero, incluida su carpeta, y crea el esquema si no existe.
    bool open();

    // Devuelve std::nullopt si la clave no existe, es NULL o hubo un error.
    std::optional<std::string> getSetting(const std::string& key);
    bool setSetting(const std::string& key, const std::string& value);

private:
    struct Closer {
        void operator()(sqlite3* db) const;
    };

    std::string path_;
    std::unique_ptr<sqlite3, Closer> db_;
    std::mutex mutex_;
};

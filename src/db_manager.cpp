#include "db_manager.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <system_error>

namespace {

constexpr const char* kSchema = R"SQL(
    CREATE TABLE IF NOT EXISTS settings (
        key   TEXT PRIMARY KEY,
        value TEXT
    );
    CREATE TABLE IF NOT EXISTS channels (
        id   INTEGER PRIMARY KEY,
        name TEXT
    );
)SQL";

// Finaliza automáticamente las sentencias preparadas.
struct StmtFinalizer {
    void operator()(sqlite3_stmt* stmt) const { sqlite3_finalize(stmt); }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtFinalizer>;

void logError(sqlite3* db, const char* context) {
    std::cerr << "[DB] Error al " << context << ": " << sqlite3_errmsg(db) << std::endl;
}

StmtPtr prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        logError(db, "preparar la sentencia");
        return nullptr;
    }
    return StmtPtr(raw);
}

bool execSql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[DB] Error SQL: " << (err ? err : "desconocido") << std::endl;
        sqlite3_free(err);
        return false;
    }
    return true;
}

}  // namespace

void DbManager::Closer::operator()(sqlite3* db) const {
    sqlite3_close(db);
}

DbManager::DbManager(std::string path) : path_(std::move(path)) {}

bool DbManager::open() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Crear la carpeta contenedora (ej. db/) si todavía no existe
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "[DB] No se pudo crear la carpeta '" << parent.string()
                      << "': " << ec.message() << std::endl;
            return false;
        }
    }

    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(path_.c_str(), &raw,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    // sqlite3_open_v2 puede devolver un handle incluso si falla: hay que cerrarlo igualmente
    db_.reset(raw);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] No se pudo abrir '" << path_ << "': "
                  << (raw ? sqlite3_errmsg(raw) : sqlite3_errstr(rc)) << std::endl;
        db_.reset();
        return false;
    }

    if (!execSql(db_.get(), kSchema)) {
        db_.reset();
        return false;
    }
    return true;
}

std::optional<std::string> DbManager::getSetting(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return std::nullopt;
    }

    StmtPtr stmt = prepare(db_.get(), "SELECT value FROM settings WHERE key = ?1;");
    if (!stmt) {
        return std::nullopt;
    }
    // SQLITE_STATIC: 'key' sigue vivo mientras exista la sentencia
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), static_cast<int>(key.size()), SQLITE_STATIC);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (!text) {
            return std::nullopt;
        }
        return std::string(text, static_cast<size_t>(sqlite3_column_bytes(stmt.get(), 0)));
    }
    if (rc != SQLITE_DONE) {
        logError(db_.get(), "leer el ajuste");
    }
    return std::nullopt;
}

bool DbManager::setSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }

    StmtPtr stmt = prepare(db_.get(),
                           "INSERT OR REPLACE INTO settings (key, value) VALUES (?1, ?2);");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), static_cast<int>(key.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, value.c_str(), static_cast<int>(value.size()), SQLITE_STATIC);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "guardar el ajuste");
        return false;
    }
    return true;
}

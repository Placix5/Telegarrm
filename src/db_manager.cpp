#include "db_manager.hpp"

#include <sqlite3.h>

#include <ctime>
#include <filesystem>
#include <iostream>
#include <system_error>

namespace {

// Migraciones del esquema, en orden. PRAGMA user_version guarda la última aplicada.
// Nunca se modifica una migración ya publicada: los cambios van en una nueva.
struct Migration {
    int version;
    const char* sql;
};

constexpr Migration kMigrations[] = {
    {1, R"SQL(
        CREATE TABLE IF NOT EXISTS settings (
            key   TEXT PRIMARY KEY,
            value TEXT
        );
        CREATE TABLE IF NOT EXISTS channels (
            id   INTEGER PRIMARY KEY,
            name TEXT
        );
    )SQL"},
    {2, R"SQL(
        ALTER TABLE channels RENAME COLUMN name TO title;
        ALTER TABLE channels ADD COLUMN added_at          INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE channels ADD COLUMN newest_message_id INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE channels ADD COLUMN oldest_message_id INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE channels ADD COLUMN history_complete  INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE channels ADD COLUMN last_sync_at      INTEGER;
        CREATE TABLE messages (
            chat_id        INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
            message_id     INTEGER NOT NULL,
            date           INTEGER NOT NULL,
            media_album_id INTEGER NOT NULL DEFAULT 0,
            content_type   TEXT    NOT NULL,
            text           TEXT    NOT NULL DEFAULT '',
            file_name      TEXT,
            file_size      INTEGER,
            mime_type      TEXT,
            PRIMARY KEY (chat_id, message_id)
        ) WITHOUT ROWID;
    )SQL"},
};

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

// Transacción que se deshace sola si no se confirma (ej. por un return anticipado)
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db), active_(execSql(db, "BEGIN IMMEDIATE;")) {}
    ~Transaction() {
        if (active_) {
            execSql(db_, "ROLLBACK;");
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool ok() const { return active_; }
    // Si COMMIT falla (ej. SQLITE_BUSY) la transacción sigue abierta: el destructor la deshace
    bool commit() {
        if (!execSql(db_, "COMMIT;")) {
            return false;
        }
        active_ = false;
        return true;
    }

private:
    sqlite3* db_;
    bool active_;
};

// SQLITE_STATIC: las cadenas enlazadas deben vivir hasta el último sqlite3_step()
void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_STATIC);
}

void bindOptionalText(sqlite3_stmt* stmt, int index, const std::optional<std::string>& value) {
    if (value) {
        bindText(stmt, index, *value);
    } else {
        sqlite3_bind_null(stmt, index);
    }
}

void bindOptionalInt64(sqlite3_stmt* stmt, int index, const std::optional<std::int64_t>& value) {
    if (value) {
        sqlite3_bind_int64(stmt, index, *value);
    } else {
        sqlite3_bind_null(stmt, index);
    }
}

std::string columnText(sqlite3_stmt* stmt, int index) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
    return text ? std::string(text, static_cast<size_t>(sqlite3_column_bytes(stmt, index))) : std::string();
}

std::optional<std::string> columnOptionalText(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return columnText(stmt, index);
}

std::optional<std::int64_t> columnOptionalInt64(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(stmt, index);
}

constexpr const char* kSelectChannels = R"SQL(
    SELECT c.id, COALESCE(c.title, ''), c.added_at, c.newest_message_id, c.oldest_message_id,
           c.history_complete, c.last_sync_at,
           (SELECT COUNT(*) FROM messages m WHERE m.chat_id = c.id),
           (SELECT COUNT(*) FROM messages m WHERE m.chat_id = c.id AND m.file_size IS NOT NULL)
    FROM channels c
)SQL";

DbManager::Channel readChannel(sqlite3_stmt* stmt) {
    DbManager::Channel channel;
    channel.id = sqlite3_column_int64(stmt, 0);
    channel.title = columnText(stmt, 1);
    channel.addedAt = sqlite3_column_int64(stmt, 2);
    channel.newestMessageId = sqlite3_column_int64(stmt, 3);
    channel.oldestMessageId = sqlite3_column_int64(stmt, 4);
    channel.historyComplete = sqlite3_column_int(stmt, 5) != 0;
    channel.lastSyncAt = columnOptionalInt64(stmt, 6);
    channel.messageCount = sqlite3_column_int64(stmt, 7);
    channel.fileCount = sqlite3_column_int64(stmt, 8);
    return channel;
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

    // WAL + synchronous=NORMAL: menos escrituras en la tarjeta SD y sin riesgo de corrupción
    // (como mucho se pierde la última transacción en un corte de luz)
    if (!execSql(db_.get(), "PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;") ||
        !migrate()) {
        db_.reset();
        return false;
    }
    return true;
}

bool DbManager::migrate() {
    StmtPtr stmt = prepare(db_.get(), "PRAGMA user_version;");
    if (!stmt || sqlite3_step(stmt.get()) != SQLITE_ROW) {
        logError(db_.get(), "leer la versión del esquema");
        return false;
    }
    const int current = sqlite3_column_int(stmt.get(), 0);
    stmt.reset();

    for (const Migration& migration : kMigrations) {
        if (migration.version <= current) {
            continue;
        }
        Transaction tx(db_.get());
        const std::string setVersion = "PRAGMA user_version = " + std::to_string(migration.version) + ";";
        if (!tx.ok() || !execSql(db_.get(), migration.sql) || !execSql(db_.get(), setVersion.c_str()) ||
            !tx.commit()) {
            std::cerr << "[DB] Falló la migración a la versión " << migration.version << std::endl;
            return false;
        }
        std::cout << "[DB] Esquema actualizado a la versión " << migration.version << std::endl;
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
    bindText(stmt.get(), 1, key);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return columnOptionalText(stmt.get(), 0);
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

    StmtPtr stmt = prepare(db_.get(), "INSERT OR REPLACE INTO settings (key, value) VALUES (?1, ?2);");
    if (!stmt) {
        return false;
    }
    bindText(stmt.get(), 1, key);
    bindText(stmt.get(), 2, value);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "guardar el ajuste");
        return false;
    }
    return true;
}

std::vector<DbManager::Channel> DbManager::listChannels() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Channel> channels;
    if (!db_) {
        return channels;
    }

    const std::string sql = std::string(kSelectChannels) + " ORDER BY c.title COLLATE NOCASE;";
    StmtPtr stmt = prepare(db_.get(), sql.c_str());
    if (!stmt) {
        return channels;
    }
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        channels.push_back(readChannel(stmt.get()));
    }
    if (rc != SQLITE_DONE) {
        logError(db_.get(), "listar los canales");
    }
    return channels;
}

std::optional<DbManager::Channel> DbManager::getChannel(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return std::nullopt;
    }

    const std::string sql = std::string(kSelectChannels) + " WHERE c.id = ?1;";
    StmtPtr stmt = prepare(db_.get(), sql.c_str());
    if (!stmt) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return readChannel(stmt.get());
    }
    if (rc != SQLITE_DONE) {
        logError(db_.get(), "leer el canal");
    }
    return std::nullopt;
}

bool DbManager::addChannel(std::int64_t id, const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }

    StmtPtr stmt = prepare(db_.get(), "INSERT OR IGNORE INTO channels (id, title, added_at) VALUES (?1, ?2, ?3);");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    bindText(stmt.get(), 2, title);
    sqlite3_bind_int64(stmt.get(), 3, static_cast<std::int64_t>(std::time(nullptr)));

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "añadir el canal");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

bool DbManager::removeChannel(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }

    // Los mensajes se borran en cascada (foreign_keys = ON)
    StmtPtr stmt = prepare(db_.get(), "DELETE FROM channels WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "quitar el canal");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

bool DbManager::updateChannelTitle(std::int64_t id, const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }

    StmtPtr stmt = prepare(db_.get(), "UPDATE channels SET title = ?2 WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    bindText(stmt.get(), 2, title);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "renombrar el canal");
        return false;
    }
    return true;
}

bool DbManager::saveSyncBatch(std::int64_t chatId, const std::vector<Message>& messages, const SyncCursor& cursor) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }

    Transaction tx(db_.get());
    if (!tx.ok()) {
        return false;
    }

    StmtPtr update = prepare(db_.get(), R"SQL(
        UPDATE channels
        SET newest_message_id = ?2, oldest_message_id = ?3, history_complete = ?4, last_sync_at = ?5
        WHERE id = ?1;
    )SQL");
    if (!update) {
        return false;
    }
    sqlite3_bind_int64(update.get(), 1, chatId);
    sqlite3_bind_int64(update.get(), 2, cursor.newestMessageId);
    sqlite3_bind_int64(update.get(), 3, cursor.oldestMessageId);
    sqlite3_bind_int(update.get(), 4, cursor.historyComplete ? 1 : 0);
    sqlite3_bind_int64(update.get(), 5, static_cast<std::int64_t>(std::time(nullptr)));
    if (sqlite3_step(update.get()) != SQLITE_DONE) {
        logError(db_.get(), "actualizar el cursor del canal");
        return false;
    }
    if (sqlite3_changes(db_.get()) == 0) {
        return false;  // El canal se quitó mientras se sincronizaba
    }

    StmtPtr insert = prepare(db_.get(), R"SQL(
        INSERT OR REPLACE INTO messages
            (chat_id, message_id, date, media_album_id, content_type, text, file_name, file_size, mime_type)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);
    )SQL");
    if (!insert) {
        return false;
    }
    for (const Message& message : messages) {
        sqlite3_reset(insert.get());
        sqlite3_bind_int64(insert.get(), 1, chatId);
        sqlite3_bind_int64(insert.get(), 2, message.messageId);
        sqlite3_bind_int64(insert.get(), 3, message.date);
        sqlite3_bind_int64(insert.get(), 4, message.mediaAlbumId);
        bindText(insert.get(), 5, message.contentType);
        bindText(insert.get(), 6, message.text);
        bindOptionalText(insert.get(), 7, message.fileName);
        bindOptionalInt64(insert.get(), 8, message.fileSize);
        bindOptionalText(insert.get(), 9, message.mimeType);
        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
            logError(db_.get(), "guardar un mensaje");
            return false;
        }
    }

    if (!tx.commit()) {
        logError(db_.get(), "confirmar el lote de mensajes");
        return false;
    }
    return true;
}

std::vector<DbManager::Message> DbManager::listMessages(std::int64_t chatId, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Message> messages;
    if (!db_) {
        return messages;
    }

    StmtPtr stmt = prepare(db_.get(), R"SQL(
        SELECT message_id, date, media_album_id, content_type, text, file_name, file_size, mime_type
        FROM messages WHERE chat_id = ?1
        ORDER BY message_id DESC LIMIT ?2 OFFSET ?3;
    )SQL");
    if (!stmt) {
        return messages;
    }
    sqlite3_bind_int64(stmt.get(), 1, chatId);
    sqlite3_bind_int(stmt.get(), 2, limit);
    sqlite3_bind_int(stmt.get(), 3, offset);

    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        Message message;
        message.chatId = chatId;
        message.messageId = sqlite3_column_int64(stmt.get(), 0);
        message.date = sqlite3_column_int64(stmt.get(), 1);
        message.mediaAlbumId = sqlite3_column_int64(stmt.get(), 2);
        message.contentType = columnText(stmt.get(), 3);
        message.text = columnText(stmt.get(), 4);
        message.fileName = columnOptionalText(stmt.get(), 5);
        message.fileSize = columnOptionalInt64(stmt.get(), 6);
        message.mimeType = columnOptionalText(stmt.get(), 7);
        messages.push_back(std::move(message));
    }
    if (rc != SQLITE_DONE) {
        logError(db_.get(), "listar los mensajes");
    }
    return messages;
}

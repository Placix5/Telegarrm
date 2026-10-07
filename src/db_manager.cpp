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
    {3, R"SQL(
        ALTER TABLE messages ADD COLUMN topic_id INTEGER NOT NULL DEFAULT 0;
        CREATE TABLE topics (
            chat_id  INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
            topic_id INTEGER NOT NULL,
            name     TEXT    NOT NULL,
            PRIMARY KEY (chat_id, topic_id)
        ) WITHOUT ROWID;
        -- Los mensajes guardados hasta ahora no tienen tema: se vuelve a leer el historial una vez
        -- (INSERT OR REPLACE completa las filas existentes sin duplicarlas)
        UPDATE channels SET newest_message_id = 0, oldest_message_id = 0, history_complete = 0;
    )SQL"},
    {4, R"SQL(
        -- Respuestas de TMDB tal cual (D-029): se conservan aunque TMDB deje de estar disponible
        CREATE TABLE tmdb_cache (
            request    TEXT    PRIMARY KEY,  -- Ruta y parámetros, sin el token
            status     INTEGER NOT NULL,     -- Código HTTP (200, 404...)
            body       TEXT    NOT NULL,
            fetched_at INTEGER NOT NULL
        ) WITHOUT ROWID;
        -- Coincidencia de cada obra del catálogo con TMDB, con sus identificadores externos
        CREATE TABLE metadata (
            work_key       TEXT    PRIMARY KEY,  -- tipo|título normalizado|año (ver MetadataService)
            provider       TEXT    NOT NULL,
            media_type     TEXT    NOT NULL DEFAULT '',  -- movie / tv; vacío = sin coincidencia
            provider_id    INTEGER NOT NULL DEFAULT 0,
            imdb_id        TEXT    NOT NULL DEFAULT '',
            tvdb_id        INTEGER NOT NULL DEFAULT 0,
            wikidata_id    TEXT    NOT NULL DEFAULT '',
            title          TEXT    NOT NULL DEFAULT '',
            original_title TEXT    NOT NULL DEFAULT '',
            overview       TEXT    NOT NULL DEFAULT '',
            year           INTEGER,
            genres         TEXT    NOT NULL DEFAULT '',  -- Separados por saltos de línea
            poster_path    TEXT    NOT NULL DEFAULT '',
            matched_by     TEXT    NOT NULL,             -- tmdbid / search / none
            updated_at     INTEGER NOT NULL
        ) WITHOUT ROWID;
        CREATE TABLE metadata_episodes (
            work_key TEXT    NOT NULL REFERENCES metadata(work_key) ON DELETE CASCADE,
            season   INTEGER NOT NULL,
            episode  INTEGER NOT NULL,
            name     TEXT    NOT NULL DEFAULT '',
            overview TEXT    NOT NULL DEFAULT '',
            air_date TEXT    NOT NULL DEFAULT '',
            PRIMARY KEY (work_key, season, episode)
        ) WITHOUT ROWID;
    )SQL"},
    {5, R"SQL(
        -- Cola de descargas persistente (Fase 3). Cada descarga es un archivo lógico (Release)
        -- y sus partes; sobrevive a reinicios.
        CREATE TABLE downloads (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            chat_id         INTEGER NOT NULL,
            message_id      INTEGER NOT NULL,  -- Primera parte: identifica el archivo lógico
            title           TEXT    NOT NULL,  -- Obra
            kind            TEXT    NOT NULL,  -- series / movie
            season          INTEGER NOT NULL DEFAULT 0,
            episode         INTEGER NOT NULL DEFAULT 0,
            episode_end     INTEGER NOT NULL DEFAULT 0,
            name            TEXT    NOT NULL,
            quality         TEXT    NOT NULL DEFAULT '',
            hdr             INTEGER NOT NULL DEFAULT 0,
            tags            TEXT    NOT NULL DEFAULT '',  -- Separadas por saltos de línea
            archive         INTEGER NOT NULL DEFAULT 0,
            total_size      INTEGER NOT NULL,
            downloaded_size INTEGER NOT NULL DEFAULT 0,
            status          TEXT    NOT NULL,  -- queued / downloading / completed / failed / cancelled
            error           TEXT    NOT NULL DEFAULT '',
            created_at      INTEGER NOT NULL,
            updated_at      INTEGER NOT NULL
        );
        CREATE INDEX downloads_by_status ON downloads (status, id);
        CREATE TABLE download_parts (
            download_id     INTEGER NOT NULL REFERENCES downloads(id) ON DELETE CASCADE,
            message_id      INTEGER NOT NULL,
            number          INTEGER NOT NULL,
            file_name       TEXT    NOT NULL,
            size            INTEGER NOT NULL,
            downloaded_size INTEGER NOT NULL DEFAULT 0,
            local_path      TEXT    NOT NULL DEFAULT '',
            PRIMARY KEY (download_id, message_id)
        ) WITHOUT ROWID;
    )SQL"},
    {6, R"SQL(
        -- Importación a la biblioteca (D-034): dónde quedó cada descarga
        ALTER TABLE downloads ADD COLUMN library_path TEXT NOT NULL DEFAULT '';
        -- Lo descargado antes de existir la importación vuelve a la cola: sus archivos siguen en
        -- el búfer, así que solo se importa
        UPDATE downloads SET status = 'queued' WHERE status = 'completed';
    )SQL"},
    {7, R"SQL(
        -- Seguimiento (Fase 4, D-035): obras seguidas
        CREATE TABLE follows (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            kind        TEXT    NOT NULL,             -- series / movie
            title       TEXT    NOT NULL,
            year        INTEGER,
            tmdb_id     INTEGER NOT NULL DEFAULT 0,
            work_key    TEXT    NOT NULL DEFAULT '',  -- tipo|título normalizado|año (MetadataService)
            chat_id     INTEGER NOT NULL,             -- Una ficha de la obra (el catálogo acepta cualquiera)
            anchor_id   INTEGER NOT NULL,
            max_quality TEXT    NOT NULL DEFAULT '',  -- '' = la mejor; '1080p', '720p'...
            created_at  INTEGER NOT NULL              -- Solo cuenta lo publicado después
        );
        -- Archivos lógicos que el seguimiento ya puso en cola: nunca se repiten solos
        CREATE TABLE auto_releases (
            chat_id    INTEGER NOT NULL,
            message_id INTEGER NOT NULL,  -- Primera parte
            follow_id  INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            PRIMARY KEY (chat_id, message_id)
        ) WITHOUT ROWID;
        -- Historial de lo que hace el sistema solo (D-037)
        CREATE TABLE activity (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            at          INTEGER NOT NULL,
            type        TEXT    NOT NULL,
            message     TEXT    NOT NULL,
            chat_id     INTEGER NOT NULL DEFAULT 0,  -- Mensaje de la obra (ficha o archivo), para enlazarla
            message_id  INTEGER NOT NULL DEFAULT 0,
            follow_id   INTEGER,
            download_id INTEGER
        );
        ALTER TABLE downloads ADD COLUMN origin        TEXT NOT NULL DEFAULT 'manual';  -- manual / auto
        ALTER TABLE downloads ADD COLUMN follow_id     INTEGER;
        ALTER TABLE downloads ADD COLUMN replaces      TEXT NOT NULL DEFAULT '';  -- Descargas que sustituye
        ALTER TABLE downloads ADD COLUMN library_files TEXT NOT NULL DEFAULT '';  -- Archivos colocados
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

// Listas cortas (géneros, etiquetas) guardadas como texto separado por saltos de línea
std::string joinLines(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        out += (out.empty() ? "" : "\n") + value;
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        values.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

std::int64_t now() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

constexpr const char* kSelectDownload = R"SQL(
    SELECT id, chat_id, message_id, title, kind, season, episode, episode_end, name, quality, hdr, tags,
           archive, total_size, downloaded_size, status, error, created_at, updated_at, library_path,
           origin, follow_id, replaces, library_files
    FROM downloads
)SQL";

DbManager::Download readDownload(sqlite3_stmt* stmt) {
    DbManager::Download d;
    d.id = sqlite3_column_int64(stmt, 0);
    d.chatId = sqlite3_column_int64(stmt, 1);
    d.messageId = sqlite3_column_int64(stmt, 2);
    d.title = columnText(stmt, 3);
    d.kind = columnText(stmt, 4);
    d.season = sqlite3_column_int(stmt, 5);
    d.episode = sqlite3_column_int(stmt, 6);
    d.episodeEnd = sqlite3_column_int(stmt, 7);
    d.name = columnText(stmt, 8);
    d.quality = columnText(stmt, 9);
    d.hdr = sqlite3_column_int(stmt, 10) != 0;
    d.tags = splitLines(columnText(stmt, 11));
    d.archive = sqlite3_column_int(stmt, 12) != 0;
    d.totalSize = sqlite3_column_int64(stmt, 13);
    d.downloadedSize = sqlite3_column_int64(stmt, 14);
    d.status = columnText(stmt, 15);
    d.error = columnText(stmt, 16);
    d.createdAt = sqlite3_column_int64(stmt, 17);
    d.updatedAt = sqlite3_column_int64(stmt, 18);
    d.libraryPath = columnText(stmt, 19);
    d.origin = columnText(stmt, 20);
    d.followId = columnOptionalInt64(stmt, 21);
    for (const std::string& id : splitLines(columnText(stmt, 22))) {
        try {
            d.replaces.push_back(std::stoll(id));
        } catch (const std::exception&) {
            // Valor dañado: se ignora
        }
    }
    d.libraryFiles = splitLines(columnText(stmt, 23));
    return d;
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
            (chat_id, message_id, date, media_album_id, content_type, text, file_name, file_size, mime_type, topic_id)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);
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
        sqlite3_bind_int64(insert.get(), 10, message.topicId);
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
    return queryMessages(chatId, "ORDER BY message_id DESC LIMIT ?2 OFFSET ?3", limit, offset);
}

std::vector<DbManager::Message> DbManager::channelMessages(std::int64_t chatId) {
    return queryMessages(chatId, "ORDER BY message_id ASC", -1, 0);
}

std::vector<DbManager::Message> DbManager::queryMessages(std::int64_t chatId, const char* orderAndLimit, int limit,
                                                         int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Message> messages;
    if (!db_) {
        return messages;
    }

    const std::string sql = std::string(R"SQL(
        SELECT message_id, date, media_album_id, content_type, text, file_name, file_size, mime_type, topic_id
        FROM messages WHERE chat_id = ?1 )SQL") + orderAndLimit + ";";
    StmtPtr stmt = prepare(db_.get(), sql.c_str());
    if (!stmt) {
        return messages;
    }
    sqlite3_bind_int64(stmt.get(), 1, chatId);
    // Los parámetros que la consulta no usa se ignoran
    if (sqlite3_bind_parameter_count(stmt.get()) >= 3) {
        sqlite3_bind_int(stmt.get(), 2, limit);
        sqlite3_bind_int(stmt.get(), 3, offset);
    }

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
        message.topicId = sqlite3_column_int64(stmt.get(), 8);
        messages.push_back(std::move(message));
    }
    if (rc != SQLITE_DONE) {
        logError(db_.get(), "listar los mensajes");
    }
    return messages;
}

bool DbManager::replaceTopics(std::int64_t chatId, const std::vector<Topic>& topics) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }

    Transaction tx(db_.get());
    StmtPtr remove = prepare(db_.get(), "DELETE FROM topics WHERE chat_id = ?1;");
    StmtPtr insert = prepare(db_.get(), "INSERT INTO topics (chat_id, topic_id, name) VALUES (?1, ?2, ?3);");
    if (!tx.ok() || !remove || !insert) {
        return false;
    }
    sqlite3_bind_int64(remove.get(), 1, chatId);
    if (sqlite3_step(remove.get()) != SQLITE_DONE) {
        logError(db_.get(), "borrar los temas");
        return false;
    }
    for (const Topic& topic : topics) {
        sqlite3_reset(insert.get());
        sqlite3_bind_int64(insert.get(), 1, chatId);
        sqlite3_bind_int64(insert.get(), 2, topic.id);
        bindText(insert.get(), 3, topic.name);
        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
            logError(db_.get(), "guardar un tema");
            return false;
        }
    }
    return tx.commit();
}

std::vector<DbManager::Topic> DbManager::listTopics(std::int64_t chatId) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Topic> topics;
    if (!db_) {
        return topics;
    }

    StmtPtr stmt = prepare(db_.get(), R"SQL(
        SELECT t.topic_id, t.name,
               (SELECT COUNT(*) FROM messages m WHERE m.chat_id = t.chat_id AND m.topic_id = t.topic_id)
        FROM topics t WHERE t.chat_id = ?1 ORDER BY t.topic_id;
    )SQL");
    if (!stmt) {
        return topics;
    }
    sqlite3_bind_int64(stmt.get(), 1, chatId);
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        topics.push_back({sqlite3_column_int64(stmt.get(), 0), columnText(stmt.get(), 1),
                          sqlite3_column_int64(stmt.get(), 2)});
    }
    if (rc != SQLITE_DONE) {
        logError(db_.get(), "listar los temas");
    }
    return topics;
}

std::optional<DbManager::CachedResponse> DbManager::getCachedResponse(const std::string& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return std::nullopt;
    }
    StmtPtr stmt = prepare(db_.get(), "SELECT status, body, fetched_at FROM tmdb_cache WHERE request = ?1;");
    if (!stmt) {
        return std::nullopt;
    }
    bindText(stmt.get(), 1, request);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return CachedResponse{sqlite3_column_int(stmt.get(), 0), columnText(stmt.get(), 1), sqlite3_column_int64(stmt.get(), 2)};
}

bool DbManager::putCachedResponse(const std::string& request, int status, const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), R"SQL(
        INSERT OR REPLACE INTO tmdb_cache (request, status, body, fetched_at) VALUES (?1, ?2, ?3, ?4);
    )SQL");
    if (!stmt) {
        return false;
    }
    bindText(stmt.get(), 1, request);
    sqlite3_bind_int(stmt.get(), 2, status);
    bindText(stmt.get(), 3, body);
    sqlite3_bind_int64(stmt.get(), 4, now());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "guardar la respuesta de TMDB");
        return false;
    }
    return true;
}

std::vector<DbManager::Metadata> DbManager::loadMetadata() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Metadata> all;
    if (!db_) {
        return all;
    }
    StmtPtr stmt = prepare(db_.get(), R"SQL(
        SELECT work_key, provider, media_type, provider_id, imdb_id, tvdb_id, wikidata_id, title, original_title,
               overview, year, genres, poster_path, matched_by, updated_at
        FROM metadata;
    )SQL");
    StmtPtr episodes = prepare(db_.get(), R"SQL(
        SELECT season, episode, name, overview, air_date FROM metadata_episodes
        WHERE work_key = ?1 ORDER BY season, episode;
    )SQL");
    if (!stmt || !episodes) {
        return all;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Metadata m;
        m.workKey = columnText(stmt.get(), 0);
        m.provider = columnText(stmt.get(), 1);
        m.mediaType = columnText(stmt.get(), 2);
        m.providerId = sqlite3_column_int64(stmt.get(), 3);
        m.imdbId = columnText(stmt.get(), 4);
        m.tvdbId = sqlite3_column_int64(stmt.get(), 5);
        m.wikidataId = columnText(stmt.get(), 6);
        m.title = columnText(stmt.get(), 7);
        m.originalTitle = columnText(stmt.get(), 8);
        m.overview = columnText(stmt.get(), 9);
        if (const auto year = columnOptionalInt64(stmt.get(), 10)) {
            m.year = static_cast<int>(*year);
        }
        m.genres = splitLines(columnText(stmt.get(), 11));
        m.posterPath = columnText(stmt.get(), 12);
        m.matchedBy = columnText(stmt.get(), 13);
        m.updatedAt = sqlite3_column_int64(stmt.get(), 14);

        sqlite3_reset(episodes.get());
        bindText(episodes.get(), 1, m.workKey);
        while (sqlite3_step(episodes.get()) == SQLITE_ROW) {
            m.episodes.push_back({sqlite3_column_int(episodes.get(), 0), sqlite3_column_int(episodes.get(), 1),
                                  columnText(episodes.get(), 2), columnText(episodes.get(), 3),
                                  columnText(episodes.get(), 4)});
        }
        all.push_back(std::move(m));
    }
    return all;
}

bool DbManager::saveMetadata(const Metadata& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    Transaction tx(db_.get());
    StmtPtr upsert = prepare(db_.get(), R"SQL(
        INSERT OR REPLACE INTO metadata
            (work_key, provider, media_type, provider_id, imdb_id, tvdb_id, wikidata_id, title, original_title,
             overview, year, genres, poster_path, matched_by, updated_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15);
    )SQL");
    StmtPtr removeEpisodes = prepare(db_.get(), "DELETE FROM metadata_episodes WHERE work_key = ?1;");
    StmtPtr insertEpisode = prepare(db_.get(), R"SQL(
        INSERT INTO metadata_episodes (work_key, season, episode, name, overview, air_date)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6);
    )SQL");
    if (!tx.ok() || !upsert || !removeEpisodes || !insertEpisode) {
        return false;
    }
    const std::string genres = joinLines(m.genres);
    bindText(upsert.get(), 1, m.workKey);
    bindText(upsert.get(), 2, m.provider);
    bindText(upsert.get(), 3, m.mediaType);
    sqlite3_bind_int64(upsert.get(), 4, m.providerId);
    bindText(upsert.get(), 5, m.imdbId);
    sqlite3_bind_int64(upsert.get(), 6, m.tvdbId);
    bindText(upsert.get(), 7, m.wikidataId);
    bindText(upsert.get(), 8, m.title);
    bindText(upsert.get(), 9, m.originalTitle);
    bindText(upsert.get(), 10, m.overview);
    bindOptionalInt64(upsert.get(), 11, m.year ? std::optional<std::int64_t>(*m.year) : std::nullopt);
    bindText(upsert.get(), 12, genres);
    bindText(upsert.get(), 13, m.posterPath);
    bindText(upsert.get(), 14, m.matchedBy);
    sqlite3_bind_int64(upsert.get(), 15, m.updatedAt);
    if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
        logError(db_.get(), "guardar los metadatos");
        return false;
    }
    // INSERT OR REPLACE borra la fila anterior, y con ella (en cascada) sus episodios; por si acaso:
    bindText(removeEpisodes.get(), 1, m.workKey);
    if (sqlite3_step(removeEpisodes.get()) != SQLITE_DONE) {
        logError(db_.get(), "borrar los episodios");
        return false;
    }
    for (const MetadataEpisode& e : m.episodes) {
        sqlite3_reset(insertEpisode.get());
        bindText(insertEpisode.get(), 1, m.workKey);
        sqlite3_bind_int(insertEpisode.get(), 2, e.season);
        sqlite3_bind_int(insertEpisode.get(), 3, e.episode);
        bindText(insertEpisode.get(), 4, e.name);
        bindText(insertEpisode.get(), 5, e.overview);
        bindText(insertEpisode.get(), 6, e.airDate);
        if (sqlite3_step(insertEpisode.get()) != SQLITE_DONE) {
            logError(db_.get(), "guardar un episodio");
            return false;
        }
    }
    return tx.commit();
}

DbManager::AddDownloadResult DbManager::addDownload(const Download& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    AddDownloadResult result;
    if (!db_) {
        return result;
    }
    Transaction tx(db_.get());
    StmtPtr existing = prepare(db_.get(), R"SQL(
        SELECT id FROM downloads
        WHERE chat_id = ?1 AND message_id = ?2 AND status IN ('queued', 'downloading', 'importing', 'completed');
    )SQL");
    StmtPtr insert = prepare(db_.get(), R"SQL(
        INSERT INTO downloads (chat_id, message_id, title, kind, season, episode, episode_end, name, quality, hdr,
                               tags, archive, total_size, status, created_at, updated_at, origin, follow_id, replaces)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, 'queued', ?14, ?14, ?15, ?16, ?17);
    )SQL");
    StmtPtr insertPart = prepare(db_.get(), R"SQL(
        INSERT INTO download_parts (download_id, message_id, number, file_name, size) VALUES (?1, ?2, ?3, ?4, ?5);
    )SQL");
    if (!tx.ok() || !existing || !insert || !insertPart) {
        return result;
    }

    sqlite3_bind_int64(existing.get(), 1, d.chatId);
    sqlite3_bind_int64(existing.get(), 2, d.messageId);
    if (sqlite3_step(existing.get()) == SQLITE_ROW) {
        result.duplicate = true;
        result.id = sqlite3_column_int64(existing.get(), 0);
        return result;
    }

    const std::string tags = joinLines(d.tags);
    std::vector<std::string> replacedIds;
    for (const std::int64_t replaced : d.replaces) {
        replacedIds.push_back(std::to_string(replaced));
    }
    const std::string replaces = joinLines(replacedIds);
    const std::string origin = d.origin.empty() ? "manual" : d.origin;
    sqlite3_bind_int64(insert.get(), 1, d.chatId);
    sqlite3_bind_int64(insert.get(), 2, d.messageId);
    bindText(insert.get(), 3, d.title);
    bindText(insert.get(), 4, d.kind);
    sqlite3_bind_int(insert.get(), 5, d.season);
    sqlite3_bind_int(insert.get(), 6, d.episode);
    sqlite3_bind_int(insert.get(), 7, d.episodeEnd);
    bindText(insert.get(), 8, d.name);
    bindText(insert.get(), 9, d.quality);
    sqlite3_bind_int(insert.get(), 10, d.hdr ? 1 : 0);
    bindText(insert.get(), 11, tags);
    sqlite3_bind_int(insert.get(), 12, d.archive ? 1 : 0);
    sqlite3_bind_int64(insert.get(), 13, d.totalSize);
    sqlite3_bind_int64(insert.get(), 14, now());
    bindText(insert.get(), 15, origin);
    bindOptionalInt64(insert.get(), 16, d.followId);
    bindText(insert.get(), 17, replaces);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        logError(db_.get(), "añadir la descarga");
        return result;
    }
    const std::int64_t id = sqlite3_last_insert_rowid(db_.get());
    for (const DownloadPart& part : d.parts) {
        sqlite3_reset(insertPart.get());
        sqlite3_bind_int64(insertPart.get(), 1, id);
        sqlite3_bind_int64(insertPart.get(), 2, part.messageId);
        sqlite3_bind_int(insertPart.get(), 3, part.number);
        bindText(insertPart.get(), 4, part.fileName);
        sqlite3_bind_int64(insertPart.get(), 5, part.size);
        if (sqlite3_step(insertPart.get()) != SQLITE_DONE) {
            logError(db_.get(), "añadir una parte de la descarga");
            return result;
        }
    }
    if (!tx.commit()) {
        return result;
    }
    result.ok = true;
    result.id = id;
    return result;
}

std::vector<DbManager::Download> DbManager::listDownloads() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Download> downloads;
    if (!db_) {
        return downloads;
    }
    const std::string sql = std::string(kSelectDownload) + " ORDER BY id DESC;";
    StmtPtr stmt = prepare(db_.get(), sql.c_str());
    if (!stmt) {
        return downloads;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        downloads.push_back(readDownload(stmt.get()));
    }
    return downloads;
}

std::optional<DbManager::Download> DbManager::queryDownloadWithParts(const char* where, std::int64_t id) {
    // Requiere mutex_ tomado
    const std::string sql = std::string(kSelectDownload) + where;
    StmtPtr stmt = prepare(db_.get(), sql.c_str());
    if (!stmt) {
        return std::nullopt;
    }
    if (sqlite3_bind_parameter_count(stmt.get()) >= 1) {
        sqlite3_bind_int64(stmt.get(), 1, id);
    }
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    Download d = readDownload(stmt.get());

    StmtPtr parts = prepare(db_.get(), R"SQL(
        SELECT message_id, number, file_name, size, downloaded_size, local_path
        FROM download_parts WHERE download_id = ?1 ORDER BY number, message_id;
    )SQL");
    if (!parts) {
        return std::nullopt;
    }
    sqlite3_bind_int64(parts.get(), 1, d.id);
    while (sqlite3_step(parts.get()) == SQLITE_ROW) {
        d.parts.push_back({sqlite3_column_int64(parts.get(), 0), sqlite3_column_int(parts.get(), 1),
                           columnText(parts.get(), 2), sqlite3_column_int64(parts.get(), 3),
                           sqlite3_column_int64(parts.get(), 4), columnText(parts.get(), 5)});
    }
    return d;
}

std::optional<DbManager::Download> DbManager::getDownload(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return std::nullopt;
    }
    return queryDownloadWithParts(" WHERE id = ?1;", id);
}

std::optional<DbManager::Download> DbManager::nextQueuedDownload() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return std::nullopt;
    }
    return queryDownloadWithParts(" WHERE status = 'queued' ORDER BY id LIMIT 1;", 0);
}

bool DbManager::setDownloadStatus(std::int64_t id, const std::string& status, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), "UPDATE downloads SET status = ?2, error = ?3, updated_at = ?4 WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    bindText(stmt.get(), 2, status);
    bindText(stmt.get(), 3, error);
    sqlite3_bind_int64(stmt.get(), 4, now());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "cambiar el estado de la descarga");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

bool DbManager::updateDownloadProgress(std::int64_t id, std::int64_t downloadedSize, const std::vector<DownloadPart>& parts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    Transaction tx(db_.get());
    StmtPtr download = prepare(db_.get(), "UPDATE downloads SET downloaded_size = ?2, updated_at = ?3 WHERE id = ?1;");
    StmtPtr part = prepare(db_.get(), R"SQL(
        UPDATE download_parts SET downloaded_size = ?3, local_path = ?4 WHERE download_id = ?1 AND message_id = ?2;
    )SQL");
    if (!tx.ok() || !download || !part) {
        return false;
    }
    sqlite3_bind_int64(download.get(), 1, id);
    sqlite3_bind_int64(download.get(), 2, downloadedSize);
    sqlite3_bind_int64(download.get(), 3, now());
    if (sqlite3_step(download.get()) != SQLITE_DONE) {
        logError(db_.get(), "guardar el progreso");
        return false;
    }
    for (const DownloadPart& p : parts) {
        sqlite3_reset(part.get());
        sqlite3_bind_int64(part.get(), 1, id);
        sqlite3_bind_int64(part.get(), 2, p.messageId);
        sqlite3_bind_int64(part.get(), 3, p.downloadedSize);
        bindText(part.get(), 4, p.localPath);
        if (sqlite3_step(part.get()) != SQLITE_DONE) {
            logError(db_.get(), "guardar el progreso de una parte");
            return false;
        }
    }
    return tx.commit();
}

int DbManager::requeueInterruptedDownloads() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return 0;
    }
    StmtPtr stmt = prepare(db_.get(),
                           "UPDATE downloads SET status = 'queued', updated_at = ?1 WHERE status IN ('downloading', 'importing');");
    if (!stmt) {
        return 0;
    }
    sqlite3_bind_int64(stmt.get(), 1, now());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "devolver las descargas a la cola");
        return 0;
    }
    return sqlite3_changes(db_.get());
}

bool DbManager::deleteDownload(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), "DELETE FROM downloads WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "borrar la descarga");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

bool DbManager::setDownloadLibrary(std::int64_t id, const std::string& libraryPath,
                                   const std::vector<std::string>& files) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(),
                           "UPDATE downloads SET library_path = ?2, library_files = ?3, updated_at = ?4 WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    const std::string fileList = joinLines(files);
    sqlite3_bind_int64(stmt.get(), 1, id);
    bindText(stmt.get(), 2, libraryPath);
    bindText(stmt.get(), 3, fileList);
    sqlite3_bind_int64(stmt.get(), 4, now());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "guardar la ruta en la biblioteca");
        return false;
    }
    return true;
}

bool DbManager::setDownloadQuality(std::int64_t id, const std::string& quality, bool hdr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), "UPDATE downloads SET quality = ?2, hdr = ?3 WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    bindText(stmt.get(), 2, quality);
    sqlite3_bind_int(stmt.get(), 3, hdr ? 1 : 0);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "guardar la calidad de la descarga");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

// --- Seguimiento ---

namespace {

constexpr const char* kSelectFollow = R"SQL(
    SELECT id, kind, title, year, tmdb_id, work_key, chat_id, anchor_id, max_quality, created_at FROM follows
)SQL";

DbManager::Follow readFollow(sqlite3_stmt* stmt) {
    DbManager::Follow f;
    f.id = sqlite3_column_int64(stmt, 0);
    f.kind = columnText(stmt, 1);
    f.title = columnText(stmt, 2);
    if (const auto year = columnOptionalInt64(stmt, 3)) {
        f.year = static_cast<int>(*year);
    }
    f.tmdbId = sqlite3_column_int64(stmt, 4);
    f.workKey = columnText(stmt, 5);
    f.chatId = sqlite3_column_int64(stmt, 6);
    f.anchorId = sqlite3_column_int64(stmt, 7);
    f.maxQuality = columnText(stmt, 8);
    f.createdAt = sqlite3_column_int64(stmt, 9);
    return f;
}

// Entradas del historial que se conservan
constexpr int kMaxActivity = 5000;

}  // namespace

std::optional<std::int64_t> DbManager::addFollow(const Follow& f) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return std::nullopt;
    }
    StmtPtr stmt = prepare(db_.get(), R"SQL(
        INSERT INTO follows (kind, title, year, tmdb_id, work_key, chat_id, anchor_id, max_quality, created_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);
    )SQL");
    if (!stmt) {
        return std::nullopt;
    }
    bindText(stmt.get(), 1, f.kind);
    bindText(stmt.get(), 2, f.title);
    bindOptionalInt64(stmt.get(), 3, f.year ? std::optional<std::int64_t>(*f.year) : std::nullopt);
    sqlite3_bind_int64(stmt.get(), 4, f.tmdbId);
    bindText(stmt.get(), 5, f.workKey);
    sqlite3_bind_int64(stmt.get(), 6, f.chatId);
    sqlite3_bind_int64(stmt.get(), 7, f.anchorId);
    bindText(stmt.get(), 8, f.maxQuality);
    sqlite3_bind_int64(stmt.get(), 9, f.createdAt > 0 ? f.createdAt : now());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "seguir la obra");
        return std::nullopt;
    }
    return sqlite3_last_insert_rowid(db_.get());
}

std::vector<DbManager::Follow> DbManager::listFollows() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Follow> follows;
    if (!db_) {
        return follows;
    }
    const std::string sql = std::string(kSelectFollow) + " ORDER BY id;";
    StmtPtr stmt = prepare(db_.get(), sql.c_str());
    if (!stmt) {
        return follows;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        follows.push_back(readFollow(stmt.get()));
    }
    return follows;
}

std::optional<DbManager::Follow> DbManager::getFollow(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return std::nullopt;
    }
    const std::string sql = std::string(kSelectFollow) + " WHERE id = ?1;";
    StmtPtr stmt = prepare(db_.get(), sql.c_str());
    if (!stmt) {
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return readFollow(stmt.get());
}

bool DbManager::updateFollowQuality(std::int64_t id, const std::string& maxQuality) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), "UPDATE follows SET max_quality = ?2 WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    bindText(stmt.get(), 2, maxQuality);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "cambiar la calidad del seguimiento");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

bool DbManager::updateFollowWork(const Follow& f) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), R"SQL(
        UPDATE follows SET title = ?2, year = ?3, tmdb_id = ?4, work_key = ?5, chat_id = ?6, anchor_id = ?7
        WHERE id = ?1;
    )SQL");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, f.id);
    bindText(stmt.get(), 2, f.title);
    bindOptionalInt64(stmt.get(), 3, f.year ? std::optional<std::int64_t>(*f.year) : std::nullopt);
    sqlite3_bind_int64(stmt.get(), 4, f.tmdbId);
    bindText(stmt.get(), 5, f.workKey);
    sqlite3_bind_int64(stmt.get(), 6, f.chatId);
    sqlite3_bind_int64(stmt.get(), 7, f.anchorId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "actualizar la obra seguida");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

bool DbManager::deleteFollow(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), "DELETE FROM follows WHERE id = ?1;");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "dejar de seguir la obra");
        return false;
    }
    return sqlite3_changes(db_.get()) > 0;
}

bool DbManager::addAutoRelease(std::int64_t chatId, std::int64_t messageId, std::int64_t followId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), R"SQL(
        INSERT OR IGNORE INTO auto_releases (chat_id, message_id, follow_id, created_at) VALUES (?1, ?2, ?3, ?4);
    )SQL");
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, chatId);
    sqlite3_bind_int64(stmt.get(), 2, messageId);
    sqlite3_bind_int64(stmt.get(), 3, followId);
    sqlite3_bind_int64(stmt.get(), 4, now());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "anotar el archivo puesto en cola");
        return false;
    }
    return true;
}

std::vector<std::pair<std::int64_t, std::int64_t>> DbManager::listAutoReleases() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::int64_t, std::int64_t>> releases;
    if (!db_) {
        return releases;
    }
    StmtPtr stmt = prepare(db_.get(), "SELECT chat_id, message_id FROM auto_releases;");
    if (!stmt) {
        return releases;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        releases.emplace_back(sqlite3_column_int64(stmt.get(), 0), sqlite3_column_int64(stmt.get(), 1));
    }
    return releases;
}

// --- Historial de actividad ---

bool DbManager::addActivity(const Activity& a) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        return false;
    }
    StmtPtr stmt = prepare(db_.get(), R"SQL(
        INSERT INTO activity (at, type, message, chat_id, message_id, follow_id, download_id)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);
    )SQL");
    StmtPtr prune = prepare(db_.get(), "DELETE FROM activity WHERE id <= (SELECT MAX(id) FROM activity) - ?1;");
    if (!stmt || !prune) {
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, a.at > 0 ? a.at : now());
    bindText(stmt.get(), 2, a.type);
    bindText(stmt.get(), 3, a.message);
    sqlite3_bind_int64(stmt.get(), 4, a.chatId);
    sqlite3_bind_int64(stmt.get(), 5, a.messageId);
    bindOptionalInt64(stmt.get(), 6, a.followId);
    bindOptionalInt64(stmt.get(), 7, a.downloadId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        logError(db_.get(), "anotar la actividad");
        return false;
    }
    // Los identificadores son correlativos (AUTOINCREMENT): se conservan los kMaxActivity últimos
    sqlite3_bind_int(prune.get(), 1, kMaxActivity);
    if (sqlite3_step(prune.get()) != SQLITE_DONE) {
        logError(db_.get(), "recortar el historial");
    }
    return true;
}

std::vector<DbManager::Activity> DbManager::listActivity(int limit, std::int64_t beforeId) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Activity> entries;
    if (!db_) {
        return entries;
    }
    StmtPtr stmt = prepare(db_.get(), R"SQL(
        SELECT id, at, type, message, chat_id, message_id, follow_id, download_id FROM activity
        WHERE ?1 = 0 OR id < ?1 ORDER BY id DESC LIMIT ?2;
    )SQL");
    if (!stmt) {
        return entries;
    }
    sqlite3_bind_int64(stmt.get(), 1, beforeId);
    sqlite3_bind_int(stmt.get(), 2, limit);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Activity a;
        a.id = sqlite3_column_int64(stmt.get(), 0);
        a.at = sqlite3_column_int64(stmt.get(), 1);
        a.type = columnText(stmt.get(), 2);
        a.message = columnText(stmt.get(), 3);
        a.chatId = sqlite3_column_int64(stmt.get(), 4);
        a.messageId = sqlite3_column_int64(stmt.get(), 5);
        a.followId = columnOptionalInt64(stmt.get(), 6);
        a.downloadId = columnOptionalInt64(stmt.get(), 7);
        entries.push_back(std::move(a));
    }
    return entries;
}

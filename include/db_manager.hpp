#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

// Gestor de la base de datos SQLite de Telegarrm.
// Los métodos públicos son seguros entre hilos: cpp-httplib atiende las
// peticiones desde un pool de hilos y la sincronización corre en su propio hilo.
class DbManager {
public:
    // Canal de Telegram vigilado y estado de su sincronización
    struct Channel {
        std::int64_t id = 0;              // chat_id de TDLib
        std::string title;
        std::int64_t addedAt = 0;         // Unix, segundos
        std::int64_t newestMessageId = 0; // Mensaje más reciente guardado (0 = ninguno)
        std::int64_t oldestMessageId = 0; // Mensaje más antiguo guardado (cursor del historial)
        bool historyComplete = false;     // Se ha llegado al principio del canal
        std::optional<std::int64_t> lastSyncAt;
        std::int64_t messageCount = 0;
        std::int64_t fileCount = 0;
    };

    // Mensaje de un canal con su texto y, si lo tiene, su fichero
    struct Message {
        std::int64_t chatId = 0;
        std::int64_t messageId = 0;
        std::int64_t date = 0;            // Unix, segundos
        std::int64_t mediaAlbumId = 0;    // Mensajes enviados juntos como álbum (0 = ninguno)
        std::string contentType;          // Tipo de TDLib: messageDocument, messageVideo, messageText...
        std::string text;                 // Texto o pie del mensaje
        std::optional<std::string> fileName;
        std::optional<std::int64_t> fileSize; // Bytes
        std::optional<std::string> mimeType;
    };

    // Cursor de sincronización que se guarda junto con un lote de mensajes
    struct SyncCursor {
        std::int64_t newestMessageId = 0;
        std::int64_t oldestMessageId = 0;
        bool historyComplete = false;
    };

    explicit DbManager(std::string path);

    DbManager(const DbManager&) = delete;
    DbManager& operator=(const DbManager&) = delete;

    // Abre (o crea) el fichero, incluida su carpeta, y aplica las migraciones pendientes.
    bool open();

    // Devuelve std::nullopt si la clave no existe, es NULL o hubo un error.
    std::optional<std::string> getSetting(const std::string& key);
    bool setSetting(const std::string& key, const std::string& value);

    std::vector<Channel> listChannels();
    std::optional<Channel> getChannel(std::int64_t id);
    // false si ya existía o hubo un error
    bool addChannel(std::int64_t id, const std::string& title);
    // Borra también sus mensajes. false si no existía o hubo un error.
    bool removeChannel(std::int64_t id);
    bool updateChannelTitle(std::int64_t id, const std::string& title);

    // Guarda un lote de mensajes y el nuevo cursor en una sola transacción, para que
    // una interrupción nunca deje el cursor por delante de los mensajes guardados.
    // false si hubo un error o el canal ya no existe (ej. se quitó durante la sincronización).
    bool saveSyncBatch(std::int64_t chatId, const std::vector<Message>& messages, const SyncCursor& cursor);

    // Mensajes más recientes de un canal (para inspeccionar y diseñar el catálogo)
    std::vector<Message> listMessages(std::int64_t chatId, int limit, int offset);
    // Todos los mensajes de un canal en orden cronológico (para construir el catálogo)
    std::vector<Message> channelMessages(std::int64_t chatId);

private:
    struct Closer {
        void operator()(sqlite3* db) const;
    };

    bool migrate();
    // orderAndLimit: cláusulas ORDER BY / LIMIT ?2 OFFSET ?3 (fragmento fijo, nunca datos de usuario)
    std::vector<Message> queryMessages(std::int64_t chatId, const char* orderAndLimit, int limit, int offset);

    std::string path_;
    std::unique_ptr<sqlite3, Closer> db_;
    std::mutex mutex_;
};

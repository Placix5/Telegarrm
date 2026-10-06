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
        std::int64_t topicId = 0;         // Tema del foro (0 = el chat no tiene temas)
    };

    // Tema de un grupo con temas (foro)
    struct Topic {
        std::int64_t id = 0;
        std::string name;
        std::int64_t messageCount = 0;    // Solo al listar
    };

    // Cursor de sincronización que se guarda junto con un lote de mensajes
    struct SyncCursor {
        std::int64_t newestMessageId = 0;
        std::int64_t oldestMessageId = 0;
        bool historyComplete = false;
    };

    // --- Metadatos (TMDB) ---
    struct CachedResponse {
        int status = 0;
        std::string body;
        std::int64_t fetchedAt = 0;  // Unix, segundos
    };

    struct MetadataEpisode {
        int season = 0;
        int episode = 0;
        std::string name;
        std::string overview;
        std::string airDate;  // AAAA-MM-DD, como lo da TMDB
    };

    // Coincidencia de una obra del catálogo con TMDB (mediaType vacío = buscada sin éxito)
    struct Metadata {
        std::string workKey;
        std::string provider = "tmdb";
        std::string mediaType;
        std::int64_t providerId = 0;
        std::string imdbId;
        std::int64_t tvdbId = 0;
        std::string wikidataId;
        std::string title;
        std::string originalTitle;
        std::string overview;
        std::optional<int> year;
        std::vector<std::string> genres;
        std::string posterPath;
        std::string matchedBy;  // tmdbid / search / none
        std::int64_t updatedAt = 0;
        std::vector<MetadataEpisode> episodes;
    };

    // --- Descargas ---
    struct DownloadPart {
        std::int64_t messageId = 0;
        int number = 0;
        std::string fileName;
        std::int64_t size = 0;
        std::int64_t downloadedSize = 0;
        std::string localPath;  // Vacío hasta que termina
    };

    struct Download {
        std::int64_t id = 0;
        std::int64_t chatId = 0;
        std::int64_t messageId = 0;  // Primera parte
        std::string title;
        std::string kind;
        int season = 0;
        int episode = 0;
        int episodeEnd = 0;
        std::string name;
        std::string quality;
        bool hdr = false;
        std::vector<std::string> tags;
        bool archive = false;
        std::int64_t totalSize = 0;
        std::int64_t downloadedSize = 0;
        std::string status;  // queued / downloading / completed / failed / cancelled
        std::string error;
        std::int64_t createdAt = 0;
        std::int64_t updatedAt = 0;
        std::vector<DownloadPart> parts;
    };

    struct AddDownloadResult {
        bool ok = false;
        bool duplicate = false;  // Ya está en la cola, descargándose o descargado
        std::int64_t id = 0;
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

    // Sustituye la lista de temas de un chat (se lee entera de Telegram en cada sincronización)
    bool replaceTopics(std::int64_t chatId, const std::vector<Topic>& topics);
    std::vector<Topic> listTopics(std::int64_t chatId);

    // Caché de respuestas de TMDB
    std::optional<CachedResponse> getCachedResponse(const std::string& request);
    bool putCachedResponse(const std::string& request, int status, const std::string& body);

    // Coincidencias con TMDB (con sus episodios)
    std::vector<Metadata> loadMetadata();
    bool saveMetadata(const Metadata& metadata);

    // Cola de descargas. Solo puede haber una descarga activa (en cola, descargando o
    // descargada) por archivo lógico.
    AddDownloadResult addDownload(const Download& download);
    std::vector<Download> listDownloads();  // Sin partes, de la más reciente a la más antigua
    std::optional<Download> getDownload(std::int64_t id);  // Con partes
    std::optional<Download> nextQueuedDownload();          // La más antigua en cola, con partes
    bool setDownloadStatus(std::int64_t id, const std::string& status, const std::string& error = "");
    bool updateDownloadProgress(std::int64_t id, std::int64_t downloadedSize, const std::vector<DownloadPart>& parts);
    // Tras un reinicio, lo que estaba descargándose vuelve a la cola (TDLib continúa donde lo dejó)
    int requeueInterruptedDownloads();
    bool deleteDownload(std::int64_t id);

private:
    struct Closer {
        void operator()(sqlite3* db) const;
    };

    bool migrate();
    // orderAndLimit: cláusulas ORDER BY / LIMIT ?2 OFFSET ?3 (fragmento fijo, nunca datos de usuario)
    std::vector<Message> queryMessages(std::int64_t chatId, const char* orderAndLimit, int limit, int offset);
    // where: cláusula fija (WHERE / ORDER BY / LIMIT) con, como mucho, el parámetro ?1. Requiere mutex_.
    std::optional<Download> queryDownloadWithParts(const char* where, std::int64_t id);

    std::string path_;
    std::unique_ptr<sqlite3, Closer> db_;
    std::mutex mutex_;
};

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
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
        // queued / downloading / importing (descomprimiendo y moviendo) / completed (en la
        // biblioteca) / failed / cancelled / replaced (sustituida por una versión mejor)
        std::string status;
        std::string error;
        std::int64_t createdAt = 0;
        std::int64_t updatedAt = 0;
        std::string libraryPath;  // Carpeta de la obra en la biblioteca, al terminar
        std::vector<std::string> libraryFiles;  // Archivos que colocó en la biblioteca (D-037)
        std::string origin = "manual";          // manual / auto (la pidió el seguimiento)
        std::optional<std::int64_t> followId;   // Obra seguida que la pidió
        // Descargas a las que sustituye (versión peor): al terminar esta, se borran sus archivos y
        // pasan a "replaced"
        std::vector<std::int64_t> replaces;
        std::vector<DownloadPart> parts;
    };

    // --- Seguimiento (D-035) ---
    struct Follow {
        std::int64_t id = 0;
        std::string kind;            // series / movie
        std::string title;
        std::optional<int> year;
        std::int64_t tmdbId = 0;     // 0 = sin coincidencia en TMDB
        std::string workKey;         // MetadataService::workKey
        std::int64_t chatId = 0;     // Una ficha de la obra
        std::int64_t anchorId = 0;
        std::string maxQuality;      // "" = la mejor; "1080p", "720p"...
        std::int64_t createdAt = 0;  // Solo cuenta lo publicado después
    };

    // Entrada del historial de actividad (D-037)
    struct Activity {
        std::int64_t id = 0;
        std::int64_t at = 0;         // Unix, segundos (0 = ahora)
        std::string type;            // follow, unfollow, queued_episode, queued_movie, queued_upgrade,
                                     // completed, upgraded, replaced, failed
        std::string message;         // En castellano, listo para mostrar
        std::int64_t chatId = 0;     // Mensaje de la obra (ficha o archivo) para enlazarla
        std::int64_t messageId = 0;
        std::optional<std::int64_t> followId;
        std::optional<std::int64_t> downloadId;
    };

    // Calidad real de un archivo lógico (D-042)
    struct Probe {
        std::int64_t chatId = 0;
        std::int64_t messageId = 0;  // Primera parte
        std::string quality;         // Vacía si no se pudo leer
        bool hdr = false;
        std::string error;
        std::int64_t probedAt = 0;
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
    // Tras un reinicio, lo que estaba descargándose o importándose vuelve a la cola (TDLib continúa
    // donde lo dejó; la importación se repite)
    int requeueInterruptedDownloads();
    // Carpeta de la obra y archivos colocados en la biblioteca
    bool setDownloadLibrary(std::int64_t id, const std::string& libraryPath, const std::vector<std::string>& files);
    bool deleteDownload(std::int64_t id);
    // Calidad real del vídeo (ffprobe, D-039)
    bool setDownloadQuality(std::int64_t id, const std::string& quality, bool hdr);

    // ¿Hay una descarga en curso (descargando o importando) con ese mensaje entre sus partes?
    bool isDownloadingMessage(std::int64_t chatId, std::int64_t messageId);

    // Calidad real de los archivos (chat, primera parte)
    bool saveProbe(const Probe& probe);
    std::map<std::pair<std::int64_t, std::int64_t>, Probe> listProbes();

    // Seguimiento
    std::optional<std::int64_t> addFollow(const Follow& follow);
    std::vector<Follow> listFollows();
    std::optional<Follow> getFollow(std::int64_t id);
    bool updateFollowQuality(std::int64_t id, const std::string& maxQuality);
    // Datos de la obra (título, TMDB, ficha) cuando el catálogo los cambia
    bool updateFollowWork(const Follow& follow);
    bool deleteFollow(std::int64_t id);
    // Archivos lógicos (chat, primera parte) que el seguimiento ya puso en cola
    bool addAutoRelease(std::int64_t chatId, std::int64_t messageId, std::int64_t followId);
    std::vector<std::pair<std::int64_t, std::int64_t>> listAutoReleases();

    // Historial de actividad: conserva las entradas más recientes
    bool addActivity(const Activity& activity);
    // De la más reciente a la más antigua; beforeId (0 = desde la última) para paginar
    std::vector<Activity> listActivity(int limit, std::int64_t beforeId);

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

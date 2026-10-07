#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "db_manager.hpp"

class TelegramClient;

// Copia el historial de los canales vigilados en la tabla messages.
// Corre en su propio hilo y usa peticiones síncronas a TDLib (TelegramClient::request).
// - La primera vez recorre el historial completo hacia atrás, por lotes, guardando el
//   cursor junto con cada lote: si se interrumpe, continúa donde lo dejó.
// - Después solo trae los mensajes nuevos. Se repite periódicamente o al pedirlo.
// - Tiempo real (D-036): abre los canales vigilados en TDLib (openChat) y, cuando llega un
//   updateNewMessage de uno de ellos, trae sus mensajes nuevos. Los avisos se agrupan: una
//   película en cinco partes provoca una sola sincronización.
// Se construye antes de TelegramClient::start() (se suscribe a sus actualizaciones) y se
// detiene antes que él.
class ChannelSync {
public:
    // Se llama (desde el hilo de sincronización) cuando cambian los mensajes guardados de un canal
    using ChangeListener = std::function<void(std::int64_t chatId)>;

    ChannelSync(DbManager& db, TelegramClient& telegram, ChangeListener onChannelChanged);
    ~ChannelSync();

    ChannelSync(const ChannelSync&) = delete;
    ChannelSync& operator=(const ChannelSync&) = delete;

    void start();
    // Detener antes que TelegramClient: espera a que termine la petición en curso
    void stop();
    // Despierta al hilo para sincronizar ya todos los canales (ej. al añadir o quitar uno)
    void requestSync();
    // Canal que se está sincronizando ahora (0 = ninguno)
    std::int64_t syncingChatId() const { return syncingChatId_; }

private:
    enum class Result { Ok, Failed, Stopped };

    // Página de getChatHistory
    struct HistoryPage {
        std::vector<DbManager::Message> messages;  // Solo los que tienen texto o fichero
        std::int64_t minId = 0;                    // De todos los mensajes devueltos
        std::int64_t maxId = 0;
        std::size_t total = 0;                     // 0 = no hay más mensajes
    };

    using Clock = std::chrono::steady_clock;

    void run();
    bool telegramReady() const;
    // Actualización de TDLib (en su hilo receptor): solo apunta el canal, sin peticiones
    void onUpdate(const nlohmann::json& update);
    // Cuándo sincronizar los canales con mensajes nuevos. Requiere mutex_.
    Clock::time_point realtimeDeadline() const;
    // Abre en TDLib los canales vigilados y cierra los que ya no lo están
    void updateOpenChats(const std::vector<DbManager::Channel>& channels);
    // full: también la lista de temas; si no, solo los mensajes nuevos (sincronización en tiempo real)
    Result syncChannel(const DbManager::Channel& channel, bool full);
    // Lista de temas de un grupo con temas (getForumTopics, paginado)
    Result syncTopics(std::int64_t chatId);
    // getChatHistory con reintentos tras FLOOD_WAIT; devuelve mensajes con id < fromMessageId
    Result fetchHistory(std::int64_t chatId, std::int64_t fromMessageId, HistoryPage& page);
    // Espera interrumpible; false si se ha pedido parar
    bool sleepFor(std::chrono::milliseconds duration);
    bool stopping();

    DbManager& db_;
    TelegramClient& telegram_;
    const ChangeListener onChannelChanged_;
    std::thread worker_;
    std::mutex mutex_;  // Protege los miembros siguientes, hasta syncingChatId_
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool syncRequested_ = false;
    std::set<std::int64_t> watched_;        // Canales vigilados (para filtrar las actualizaciones)
    std::set<std::int64_t> pending_;        // Canales con mensajes nuevos sin sincronizar
    Clock::time_point firstEventAt_;        // Primer y último aviso del grupo pendiente
    Clock::time_point lastEventAt_;
    std::atomic<std::int64_t> syncingChatId_{0};

    std::set<std::int64_t> openChats_;      // Abiertos con openChat (solo los usa el hilo de sincronización)
};

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "db_manager.hpp"

class TelegramClient;

// Copia el historial de los canales vigilados en la tabla messages.
// Corre en su propio hilo y usa peticiones síncronas a TDLib (TelegramClient::request).
// - La primera vez recorre el historial completo hacia atrás, por lotes, guardando el
//   cursor junto con cada lote: si se interrumpe, continúa donde lo dejó.
// - Después solo trae los mensajes nuevos. Se repite periódicamente o al pedirlo.
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
    // Despierta al hilo para sincronizar ya (ej. al añadir un canal)
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

    void run();
    bool telegramReady() const;
    Result syncChannel(const DbManager::Channel& channel);
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
    std::mutex mutex_;  // Protege stopRequested_ y syncRequested_
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool syncRequested_ = false;
    std::atomic<std::int64_t> syncingChatId_{0};
};

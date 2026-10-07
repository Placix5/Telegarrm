#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

// Cliente de Telegram sobre la interfaz JSON de TDLib.
// Un único hilo propio recibe todas las respuestas y actualizaciones de TDLib
// (td_receive no admite llamadas concurrentes); send() y request() son seguros
// desde cualquier hilo, como los del servidor HTTP.
// - Solo puede haber una instancia por proceso: td_receive() es global.
// - start() y stop() deben llamarse desde el mismo hilo (el principal).
// - request() no debe llamarse desde un Handler: bloquearía el hilo receptor.
class TelegramClient {
public:
    using Json = nlohmann::json;
    // Recibe la respuesta de TDLib (puede ser un objeto "error"). Se ejecuta en el hilo de TDLib.
    using Handler = std::function<void(const Json&)>;

    struct Config {
        int apiId = 0;
        std::string apiHash;
        // Carpeta de la sesión de TDLib. Da acceso completo a la cuenta: se crea con permisos 0700.
        std::string databaseDir;
        // Búfer de descargas (files_directory). Vacío = dentro de databaseDir.
        std::string filesDir;
    };

    explicit TelegramClient(Config config);
    ~TelegramClient();

    TelegramClient(const TelegramClient&) = delete;
    TelegramClient& operator=(const TelegramClient&) = delete;

    // Crea el cliente de TDLib y lanza el hilo receptor. No bloquea.
    bool start();
    // Cierra la sesión de TDLib de forma ordenada y espera al hilo receptor
    void stop();

    // Envía una petición a TDLib; onResponse (opcional) recibe su respuesta
    void send(Json request, Handler onResponse = nullptr);
    // Envía una petición y espera su respuesta (puede ser un objeto "error").
    // std::nullopt si se agota el tiempo.
    std::optional<Json> request(Json request, std::chrono::milliseconds timeout);

    // Último objeto authorizationState* recibido (ej. authorizationStateWaitPhoneNumber)
    Json authorizationState() const;
    // Último objeto connectionState* recibido (ej. connectionStateReady)
    Json connectionState() const;

    // Recibe cada actualización de TDLib (updateNewMessage...) en el hilo receptor: debe volver
    // enseguida y no puede llamar a request(). Se registra antes de start().
    void addUpdateListener(Handler listener);

private:
    void run();
    void createClient();
    void handleUpdate(const Json& update);
    void onAuthorizationState(const Json& state);
    void failPendingRequests(const std::string& reason);

    const Config config_;
    std::atomic<int> clientId_{0};
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> closed_{false};
    bool loggedOut_ = false;  // Solo lo usa el hilo receptor

    mutable std::mutex mutex_;  // Protege los miembros siguientes
    Json authorizationState_;
    Json connectionState_;
    std::unordered_map<std::uint64_t, Handler> pending_;
    std::uint64_t nextRequestId_ = 1;

    // Solo se modifica antes de start(): el hilo receptor lo lee sin mutex
    std::vector<Handler> updateListeners_;
};

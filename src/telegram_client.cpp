#include "telegram_client.hpp"

#include <td/telegram/td_json_client.h>

#include <filesystem>
#include <future>
#include <iostream>
#include <system_error>
#include <utility>

namespace {

using Json = TelegramClient::Json;
using Clock = std::chrono::steady_clock;

// td_receive() espera como máximo esto antes de volver sin mensajes
constexpr double kReceiveTimeoutSeconds = 1.0;
// Tiempo máximo para que TDLib guarde su estado al cerrar
constexpr auto kCloseTimeout = std::chrono::seconds(10);
// Log interno de TDLib por stderr: 1 = solo errores
constexpr int kTdlibLogVerbosity = 1;

Json makeError(const std::string& message) {
    return {{"@type", "error"}, {"code", 500}, {"message", message}};
}

std::string typeOf(const Json& object) {
    return object.is_object() ? object.value("@type", "") : "";
}

void logError(const std::string& context, const Json& error) {
    std::cerr << "[Telegram] Error en " << context << ": " << error.value("message", "desconocido")
              << " (código " << error.value("code", 0) << ")" << std::endl;
}

// Handler que solo informa si la petición falla
TelegramClient::Handler logErrors(std::string context) {
    return [context = std::move(context)](const Json& response) {
        if (typeOf(response) == "error") {
            logError(context, response);
        }
    };
}

}  // namespace

TelegramClient::TelegramClient(Config config) : config_(std::move(config)) {}

TelegramClient::~TelegramClient() {
    stop();
}

bool TelegramClient::start() {
    if (worker_.joinable()) {
        return true;  // Ya está en marcha
    }

    // La sesión de TDLib da acceso completo a la cuenta: carpeta solo para el propietario
    std::error_code ec;
    std::filesystem::create_directories(config_.databaseDir, ec);
    if (!ec) {
        std::filesystem::permissions(config_.databaseDir, std::filesystem::perms::owner_all, ec);
    }
    if (ec) {
        std::cerr << "[Telegram] No se pudo preparar la carpeta '" << config_.databaseDir
                  << "': " << ec.message() << std::endl;
        return false;
    }

    const Json logLevel = {{"@type", "setLogVerbosityLevel"}, {"new_verbosity_level", kTdlibLogVerbosity}};
    td_execute(logLevel.dump().c_str());

    stopRequested_ = false;
    closed_ = false;
    running_ = true;
    createClient();
    worker_ = std::thread(&TelegramClient::run, this);
    return true;
}

void TelegramClient::stop() {
    if (!worker_.joinable()) {
        return;
    }
    stopRequested_ = true;
    // TDLib guarda su estado y termina emitiendo authorizationStateClosed.
    // Si ya se cerró por su cuenta, el hilo receptor ha terminado y basta con unirlo.
    if (running_) {
        send({{"@type", "close"}}, logErrors("close"));
    }
    worker_.join();
}

void TelegramClient::createClient() {
    loggedOut_ = false;
    clientId_ = td_create_client_id();
    // TDLib crea el cliente al recibir la primera petición
    send({{"@type", "getOption"}, {"name", "version"}}, [](const Json& response) {
        if (typeOf(response) == "optionValueString") {
            std::cout << "[Telegram] TDLib " << response.value("value", "") << " iniciado" << std::endl;
        }
    });
}

void TelegramClient::send(Json request, Handler onResponse) {
    std::uint64_t requestId = 0;  // 0 = no enviada (los identificadores empiezan en 1)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            requestId = nextRequestId_++;
            if (onResponse) {
                pending_.emplace(requestId, std::move(onResponse));
            }
        }
    }
    if (requestId == 0) {
        if (onResponse) {
            onResponse(makeError("El cliente de Telegram no está en marcha"));
        }
        return;
    }

    // TDLib devuelve "@extra" tal cual en la respuesta: así se asocia con su handler
    request["@extra"] = requestId;
    td_send(clientId_, request.dump().c_str());
}

std::optional<TelegramClient::Json> TelegramClient::request(Json request, std::chrono::milliseconds timeout) {
    // shared_ptr: el handler puede ejecutarse después de que esta función haya vuelto por timeout
    auto promise = std::make_shared<std::promise<Json>>();
    auto future = promise->get_future();
    send(std::move(request), [promise](const Json& response) { promise->set_value(response); });

    if (future.wait_for(timeout) != std::future_status::ready) {
        return std::nullopt;
    }
    return future.get();
}

TelegramClient::Json TelegramClient::authorizationState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return authorizationState_;
}

TelegramClient::Json TelegramClient::connectionState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connectionState_;
}

std::optional<TelegramClient::Json> TelegramClient::fileOfMessage(const Json& message) {
    static const std::pair<const char*, const char*> kFields[] = {
        {"messageDocument", "document"}, {"messageVideo", "video"}, {"messageAudio", "audio"}, {"messageAnimation", "animation"}};
    const Json content = message.value("content", Json::object());
    const std::string type = typeOf(content);
    for (const auto& [contentType, field] : kFields) {
        if (type == contentType) {
            const Json media = content.value(field, Json::object());
            const Json file = media.value(field, Json::object());
            if (file.is_object() && file.contains("id")) {
                return file;
            }
        }
    }
    return std::nullopt;
}

void TelegramClient::addUpdateListener(Handler listener) {
    if (worker_.joinable()) {
        std::cerr << "[Telegram] addUpdateListener debe llamarse antes de start()" << std::endl;
        return;
    }
    updateListeners_.push_back(std::move(listener));
}

void TelegramClient::run() {
    std::optional<Clock::time_point> closeDeadline;

    while (!closed_) {
        if (stopRequested_) {
            if (!closeDeadline) {
                closeDeadline = Clock::now() + kCloseTimeout;
            } else if (Clock::now() > *closeDeadline) {
                std::cerr << "[Telegram] TDLib no se cerró a tiempo; se abandona la espera." << std::endl;
                break;
            }
        }

        const char* raw = td_receive(kReceiveTimeoutSeconds);
        if (!raw) {
            continue;
        }
        const Json message = Json::parse(raw, nullptr, false);
        if (message.is_discarded()) {
            std::cerr << "[Telegram] Mensaje de TDLib con JSON no válido." << std::endl;
            continue;
        }

        // Un handler que lance una excepción no debe tumbar el hilo receptor
        try {
            const auto extra = message.find("@extra");
            if (extra == message.end()) {
                // Ignorar actualizaciones de clientes anteriores (ver onAuthorizationState)
                if (message.value("@client_id", clientId_.load()) == clientId_) {
                    handleUpdate(message);
                }
                continue;
            }

            Handler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = pending_.find(extra->get<std::uint64_t>());
                if (it != pending_.end()) {
                    handler = std::move(it->second);
                    pending_.erase(it);
                }
            }
            if (handler) {
                handler(message);
            } else if (typeOf(message) == "error") {
                logError("petición sin handler", message);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Telegram] Error procesando un mensaje de TDLib: " << e.what() << std::endl;
        }
    }

    failPendingRequests("El cliente de Telegram se ha detenido");
}

void TelegramClient::handleUpdate(const Json& update) {
    const std::string type = typeOf(update);
    if (type == "updateAuthorizationState") {
        onAuthorizationState(update.at("authorization_state"));
    } else if (type == "updateConnectionState") {
        const Json& state = update.at("state");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connectionState_ = state;
        }
        std::cout << "[Telegram] Conexión: " << typeOf(state) << std::endl;
    }
    for (const Handler& listener : updateListeners_) {
        listener(update);
    }
}

void TelegramClient::onAuthorizationState(const Json& state) {
    const std::string type = typeOf(state);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        authorizationState_ = state;
    }
    // Solo el tipo: el objeto completo puede incluir el número de teléfono
    std::cout << "[Telegram] Autorización: " << type << std::endl;

    if (type == "authorizationStateWaitTdlibParameters") {
        send({{"@type", "setTdlibParameters"},
              {"use_test_dc", false},
              {"database_directory", config_.databaseDir},
              {"files_directory", config_.filesDir},  // Vacío = dentro de database_directory
              {"database_encryption_key", ""},
              {"use_file_database", true},
              {"use_chat_info_database", true},
              {"use_message_database", true},
              {"use_secret_chats", false},
              {"api_id", config_.apiId},
              {"api_hash", config_.apiHash},
              {"system_language_code", "es"},
              {"device_model", "Telegarrm"},  // Nombre visible en "Dispositivos" de Telegram
              {"system_version", ""},
              {"application_version", TELEGARRM_VERSION}},
             logErrors("setTdlibParameters"));
    } else if (type == "authorizationStateReady") {
        send({{"@type", "getMe"}}, [](const Json& me) {
            if (typeOf(me) == "user") {
                std::cout << "[Telegram] Sesión iniciada como " << me.value("first_name", "") << std::endl;
            }
        });
    } else if (type == "authorizationStateLoggingOut") {
        loggedOut_ = true;
    } else if (type == "authorizationStateClosed") {
        if (!stopRequested_ && loggedOut_) {
            // Sesión cerrada desde fuera (ej. revocada en otro dispositivo):
            // un cliente nuevo permite volver a iniciar sesión desde la web
            std::cout << "[Telegram] Sesión cerrada; se crea un cliente nuevo." << std::endl;
            createClient();
            return;
        }
        if (!stopRequested_) {
            // Ej. la base de datos de TDLib está bloqueada por otro proceso.
            // No se recrea el cliente para no entrar en un bucle de reintentos.
            std::cerr << "[Telegram] TDLib se ha cerrado de forma inesperada." << std::endl;
        }
        closed_ = true;
    }
}

void TelegramClient::failPendingRequests(const std::string& reason) {
    std::unordered_map<std::uint64_t, Handler> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;  // Bajo el mutex: send() ya no puede añadir handlers nuevos
        pending.swap(pending_);
    }
    const Json error = makeError(reason);
    for (auto& entry : pending) {
        entry.second(error);
    }
}

#include "api.hpp"

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "db_manager.hpp"
#include "httplib.h"
#include "telegram_client.hpp"

namespace {

using Json = nlohmann::json;

// Espera máxima a que TDLib responda a una petición hecha desde la API
constexpr auto kTelegramTimeout = std::chrono::seconds(30);

// Paso del inicio de sesión: estado que debe tener TDLib, petición que se le envía
// y campo del cuerpo JSON (se llama igual en la API y en TDLib)
struct AuthStep {
    const char* path;
    const char* expectedState;
    const char* tdlibRequest;
    const char* field;
};

constexpr AuthStep kAuthSteps[] = {
    {"/api/telegram/auth/phone", "authorizationStateWaitPhoneNumber", "setAuthenticationPhoneNumber", "phone_number"},
    {"/api/telegram/auth/code", "authorizationStateWaitCode", "checkAuthenticationCode", "code"},
    {"/api/telegram/auth/password", "authorizationStateWaitPassword", "checkAuthenticationPassword", "password"},
};

std::string typeOf(const Json& object) {
    return object.is_object() ? object.value("@type", "") : "";
}

void sendJson(httplib::Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, status, {{"error", message}});
}

}  // namespace

void registerApiRoutes(httplib::Server& server, DbManager& db, TelegramClient& telegram) {
    // Estado del servicio, de la BD (lee 'version' de settings) y de la sesión de Telegram
    server.Get("/api/status", [&db, &telegram](const httplib::Request&, httplib::Response& res) {
        Json body = {{"status", "Telegarrm is running"}, {"version", TELEGARRM_VERSION}};
        int status = 200;

        if (const auto dbVersion = db.getSetting("version")) {
            body["database"] = {{"status", "ok"}, {"version", *dbVersion}};
        } else {
            body["database"] = {{"status", "error"}, {"version", nullptr}};
            status = 503;
        }

        const Json auth = telegram.authorizationState();
        const std::string authType = typeOf(auth);
        const std::string connType = typeOf(telegram.connectionState());
        Json tg = {{"authorization_state", authType.empty() ? Json(nullptr) : Json(authType)},
                   {"connection_state", connType.empty() ? Json(nullptr) : Json(connType)}};
        if (authType == "authorizationStateWaitPassword") {
            tg["password_hint"] = auth.value("password_hint", "");
        }
        body["telegram"] = tg;

        sendJson(res, status, body);
    });

    // Inicio de sesión en Telegram, paso a paso: teléfono -> código -> contraseña 2FA (si la hay)
    for (const AuthStep& step : kAuthSteps) {
        server.Post(step.path, [&telegram, step](const httplib::Request& req, httplib::Response& res) {
            const Json body = Json::parse(req.body, nullptr, false);
            const Json* value = (body.is_object() && body.contains(step.field)) ? &body.at(step.field) : nullptr;
            if (!value || !value->is_string() || value->get_ref<const std::string&>().empty()) {
                sendError(res, 400, std::string("El cuerpo debe ser JSON con el campo '") + step.field + "'");
                return;
            }

            const std::string current = typeOf(telegram.authorizationState());
            if (current != step.expectedState) {
                sendError(res, 409, "Telegram no espera este dato ahora (estado actual: " +
                                        (current.empty() ? std::string("desconocido") : current) + ")");
                return;
            }

            const auto response = telegram.request({{"@type", step.tdlibRequest}, {step.field, *value}}, kTelegramTimeout);
            if (!response) {
                sendError(res, 504, "Telegram no ha respondido a tiempo");
                return;
            }
            if (typeOf(*response) == "error") {
                // Ej. PHONE_NUMBER_INVALID, PHONE_CODE_INVALID, PASSWORD_HASH_INVALID
                sendError(res, 400, response->value("message", "Error desconocido de Telegram"));
                return;
            }
            sendJson(res, 200, {{"ok", true}});
        });
    }
}

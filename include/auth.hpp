#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "db_manager.hpp"

// Acceso a la web y la API (docs/DECISIONS.md, D-053): una contraseña (la de Plácido) y sesiones con
// cookie. La contraseña se guarda con scrypt en settings; las sesiones, por el SHA-256 de su token en
// la tabla sessions (quien lea la BD no puede usarlas). La primera contraseña exige un código de un
// solo uso que se escribe en db/codigo-acceso.txt: solo lo lee quien tenga acceso a la Pi.
namespace auth {

constexpr const char* kCookie = "telegarrm_session";
constexpr std::size_t kMinPasswordLength = 10;
constexpr std::size_t kMaxPasswordLength = 256;

// "scrypt$N$r$p$sal$hash" (sal y hash en hexadecimal)
std::string hashPassword(const std::string& password);
// Comparación en tiempo constante; false si stored no tiene el formato de hashPassword
bool verifyPassword(const std::string& password, const std::string& stored);
// bytes aleatorios (generador de OpenSSL) en hexadecimal
std::string randomHex(std::size_t bytes);
std::string sha256Hex(const std::string& text);
// Valor de una cookie en la cabecera Cookie ("a=1; telegarrm_session=abc" -> "abc")
std::string cookieValue(const std::string& header, const std::string& name);

}  // namespace auth

class Auth {
public:
    // setupCodePath: archivo donde se deja el código para crear la contraseña (db/codigo-acceso.txt)
    Auth(DbManager& db, std::string setupCodePath);

    Auth(const Auth&) = delete;
    Auth& operator=(const Auth&) = delete;

    struct Result {
        bool ok = false;
        int status = 200;      // Código HTTP si falla: 400, 401, 403, 409, 429
        std::string error;
        std::string token;     // Sesión nueva (setup, login y cambio de contraseña)
        int retryAfter = 0;    // Segundos que esperar antes de otro intento (429)
    };

    // ¿Hay contraseña? Si no, la web pide crearla
    bool configured();
    // Sin contraseña: deja el código para crearla en su archivo (si no lo había ya)
    void prepareSetup();
    // Crea la primera contraseña con el código de un solo uso
    Result setup(const std::string& code, const std::string& password, const std::string& client);
    Result login(const std::string& password, const std::string& client);
    // Cambia la contraseña: cierra las demás sesiones y da una nueva a quien la cambia
    Result changePassword(const std::string& current, const std::string& password, const std::string& token,
                          const std::string& client);
    // ¿Sesión válida? Alarga su caducidad con el uso
    bool valid(const std::string& token);
    void logout(const std::string& token);

    // Desde la línea de órdenes (telegarrm --reset-password): borra la contraseña y todas las sesiones
    static bool reset(DbManager& db);

private:
    using Clock = std::chrono::steady_clock;
    struct Failures {
        int count = 0;
        Clock::time_point last{};         // Último fallo: pasada una hora, se olvidan
        Clock::time_point lockedUntil{};
    };

    // Requieren mutex_ tomado
    int lockedFor(const std::string& client) const;  // Segundos que le quedan bloqueado (0 = ninguno)
    void fail(const std::string& client);
    std::string newSession();
    Result tooMany(int seconds) const;

    DbManager& db_;
    const std::string setupCodePath_;
    std::mutex mutex_;  // Protege failures_ y setupCode_, y serializa los intentos
    std::map<std::string, Failures> failures_;  // Por cliente (IP) y "*" (todos)
    std::string setupCode_;
};

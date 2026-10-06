#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace httplib {
class Client;
}
class DbManager;

// Cliente de la API de TheMovieDB (v3) con caché en SQLite (docs/DECISIONS.md, D-029 y D-030).
// - Cada respuesta (también los 404) se guarda en la tabla tmdb_cache y se reutiliza mientras no
//   caduque. Si TMDB no responde, se usa la guardada aunque haya caducado.
// - Las carátulas se guardan en disco y no se vuelven a pedir.
// - Sin token, enabled() es false y no se hace ninguna petición: Telegarrm funciona igual con
//   los datos de las fichas de Telegram.
// Seguro entre hilos: las peticiones se hacen de una en una y espaciadas.
class TmdbClient {
public:
    using Json = nlohmann::json;
    using Params = std::vector<std::pair<std::string, std::string>>;

    struct Response {
        int status = 0;  // 200, 404...
        Json body;
    };

    // imageDir: carpeta para las carátulas (se crea si no existe)
    TmdbClient(DbManager& db, std::string token, std::string imageDir);
    ~TmdbClient();

    TmdbClient(const TmdbClient&) = delete;
    TmdbClient& operator=(const TmdbClient&) = delete;

    bool enabled() const { return !token_.empty(); }

    // GET /3<path> en castellano (language=es-ES). std::nullopt si no hay token o si TMDB no
    // responde y no hay copia guardada.
    std::optional<Response> get(const std::string& path, Params params, std::chrono::hours maxAge);

    // Carátula de TMDB ("/abc.jpg") en el tamaño pedido ("w500"): ruta local o std::nullopt
    std::optional<std::string> image(const std::string& size, const std::string& filePath);

private:
    // Requiere mutex_ tomado: espera lo necesario para no pasar de unas 10 peticiones por segundo
    void throttle();

    DbManager& db_;
    const std::string token_;
    const std::string imageDir_;
    std::mutex mutex_;
    std::unique_ptr<httplib::Client> api_;     // Conexión persistente a api.themoviedb.org
    std::unique_ptr<httplib::Client> images_;  // y a image.tmdb.org
    std::chrono::steady_clock::time_point lastRequest_;
};

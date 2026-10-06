#include "tmdb_client.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <system_error>
#include <thread>

#include "db_manager.hpp"
#include "httplib.h"

namespace {

using Json = TmdbClient::Json;

// TMDB admite bastantes más; 10 por segundo como máximo es de sobra para un catálogo personal
constexpr auto kMinInterval = std::chrono::milliseconds(100);
constexpr time_t kConnectTimeoutSeconds = 5;
constexpr time_t kReadTimeoutSeconds = 15;
// Si TMDB pide esperar (429), como mucho se espera esto antes de seguir con lo guardado
constexpr int kMaxRetryAfterSeconds = 10;

std::optional<TmdbClient::Response> parse(int status, const std::string& body) {
    Json json = Json::parse(body, nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return TmdbClient::Response{status, std::move(json)};
}

std::unique_ptr<httplib::Client> makeClient(const std::string& host) {
    auto client = std::make_unique<httplib::Client>(host);
    client->set_connection_timeout(kConnectTimeoutSeconds, 0);
    client->set_read_timeout(kReadTimeoutSeconds, 0);
    client->set_keep_alive(true);
    return client;
}

}  // namespace

TmdbClient::TmdbClient(DbManager& db, std::string token, std::string imageDir)
    : db_(db), token_(std::move(token)), imageDir_(std::move(imageDir)) {}

TmdbClient::~TmdbClient() = default;

void TmdbClient::throttle() {
    const auto wait = lastRequest_ + kMinInterval - std::chrono::steady_clock::now();
    if (wait > std::chrono::steady_clock::duration::zero()) {
        std::this_thread::sleep_for(wait);
    }
    lastRequest_ = std::chrono::steady_clock::now();
}

std::optional<TmdbClient::Response> TmdbClient::get(const std::string& path, Params params, std::chrono::hours maxAge) {
    if (!enabled()) {
        return std::nullopt;
    }

    // Clave de la caché: ruta y parámetros ordenados (el token va en una cabecera, nunca aquí)
    params.emplace_back("language", "es-ES");
    std::sort(params.begin(), params.end());
    std::string request = "/3" + path;
    for (std::size_t i = 0; i < params.size(); ++i) {
        request += (i == 0 ? "?" : "&") + params[i].first + "=" + httplib::encode_query_component(params[i].second);
    }

    const auto cached = db_.getCachedResponse(request);
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (cached && now - cached->fetchedAt < std::chrono::duration_cast<std::chrono::seconds>(maxAge).count()) {
        return parse(cached->status, cached->body);
    }
    // Sin red, una respuesta caducada es mejor que ninguna
    const auto fallback = [&cached]() -> std::optional<Response> {
        return cached ? parse(cached->status, cached->body) : std::nullopt;
    };

    std::lock_guard<std::mutex> lock(mutex_);
    throttle();
    if (!api_) {
        api_ = makeClient("https://api.themoviedb.org");
        api_->set_default_headers({{"Authorization", "Bearer " + token_}, {"Accept", "application/json"}});
    }
    const auto result = api_->Get(request);
    if (!result) {
        std::cerr << "[TMDB] Sin respuesta (" << httplib::to_string(result.error()) << ")" << std::endl;
        return fallback();
    }
    const int status = result->status;
    if (status == 200 || status == 404) {
        db_.putCachedResponse(request, status, result->body);
        return parse(status, result->body);
    }
    if (status == 401) {
        std::cerr << "[TMDB] TELEGARRM_TMDB_TOKEN no es válido (HTTP 401)" << std::endl;
    } else if (status == 429) {
        const int retryAfter = std::clamp(std::atoi(result->get_header_value("Retry-After").c_str()), 1,
                                          kMaxRetryAfterSeconds);
        std::cerr << "[TMDB] Demasiadas peticiones: se espera " << retryAfter << " s" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(retryAfter));
    } else {
        std::cerr << "[TMDB] HTTP " << status << " en " << path << std::endl;
    }
    return fallback();
}

std::optional<std::string> TmdbClient::image(const std::string& size, const std::string& filePath) {
    // Solo rutas con la forma que usa TMDB ("/abc123.jpg"): nada que permita salir de la carpeta
    static const std::regex kFilePath(R"(^/[A-Za-z0-9_-]+\.(jpg|jpeg|png)$)");
    static const std::regex kSize(R"(^(w\d{2,4}|original)$)");
    if (!std::regex_match(filePath, kFilePath) || !std::regex_match(size, kSize)) {
        return std::nullopt;
    }

    const std::filesystem::path dir = std::filesystem::path(imageDir_) / size;
    const std::filesystem::path local = dir / filePath.substr(1);
    std::error_code ec;
    if (std::filesystem::exists(local, ec)) {
        return local.string();
    }
    if (!enabled()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (std::filesystem::exists(local, ec)) {  // Otro hilo la ha descargado mientras esperábamos
        return local.string();
    }
    throttle();
    if (!images_) {
        images_ = makeClient("https://image.tmdb.org");
    }
    const auto result = images_->Get("/t/p/" + size + filePath);
    if (!result || result->status != 200) {
        std::cerr << "[TMDB] No se pudo descargar la imagen " << filePath << std::endl;
        return std::nullopt;
    }

    // Se escribe en un temporal y se renombra: nunca queda una imagen a medias
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path temporary = local.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary);
        out.write(result->body.data(), static_cast<std::streamsize>(result->body.size()));
        if (!out) {
            std::cerr << "[TMDB] No se pudo guardar la imagen en " << dir.string() << std::endl;
            return std::nullopt;
        }
    }
    std::filesystem::rename(temporary, local, ec);
    if (ec) {
        return std::nullopt;
    }
    return local.string();
}

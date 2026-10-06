#include "settings.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "db_manager.hpp"

namespace {

constexpr const char* kDownloadDir = "download_dir";
constexpr const char* kMoviesDir = "library_movies_dir";
constexpr const char* kSeriesDir = "library_series_dir";
constexpr const char* kMinFreeBytes = "min_free_bytes";

}  // namespace

AppSettings loadSettings(DbManager& db) {
    AppSettings settings;
    settings.downloadDir = db.getSetting(kDownloadDir).value_or("");
    settings.moviesDir = db.getSetting(kMoviesDir).value_or("");
    settings.seriesDir = db.getSetting(kSeriesDir).value_or("");
    if (const auto minFree = db.getSetting(kMinFreeBytes)) {
        try {
            settings.minFreeBytes = std::stoll(*minFree);
        } catch (const std::exception&) {
            // Valor dañado: se queda el predeterminado
        }
    }
    return settings;
}

bool saveSettings(DbManager& db, const AppSettings& settings) {
    return db.setSetting(kDownloadDir, settings.downloadDir) && db.setSetting(kMoviesDir, settings.moviesDir) &&
           db.setSetting(kSeriesDir, settings.seriesDir) &&
           db.setSetting(kMinFreeBytes, std::to_string(settings.minFreeBytes));
}

PathCheck checkPath(const std::string& path) {
    PathCheck check;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.empty() || path.front() != '/') {
        check.error = "Debe ser una ruta absoluta (empieza por /)";
        return check;
    }
    if (!fs::exists(path, ec)) {
        check.error = "La carpeta no existe";
        return check;
    }
    if (!fs::is_directory(path, ec)) {
        check.error = "No es una carpeta";
        return check;
    }

    struct stat info {};
    if (::stat(path.c_str(), &info) == 0) {
        check.device = static_cast<std::uint64_t>(info.st_dev);
    }
    const auto space = fs::space(path, ec);
    if (!ec) {
        check.freeBytes = static_cast<std::int64_t>(space.available);
    }

    // Prueba de escritura real (el aislamiento de systemd puede dejarla en solo lectura)
    const fs::path probe = fs::path(path) / (".telegarrm-prueba-" + std::to_string(::getpid()));
    {
        std::ofstream out(probe);
        out << "ok";
        if (!out) {
            const int error = errno;
            check.error = error == EROFS
                              ? "El servicio no puede escribir aquí (solo lectura): añade la ruta con "
                                "«sudo ./deploy/install-service.sh " + path + "»"
                              : std::string("No se puede escribir: ") + std::strerror(error);
            fs::remove(probe, ec);
            return check;
        }
    }
    fs::remove(probe, ec);
    check.ok = true;
    return check;
}

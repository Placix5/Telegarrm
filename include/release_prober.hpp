#pragma once

#include <mutex>
#include <string>

#include "catalog.hpp"
#include "db_manager.hpp"
#include "library.hpp"

class TelegramClient;

// Calidad real de un archivo del catálogo sin descargarlo entero (docs/DECISIONS.md, D-042): se
// piden a Telegram solo sus primeros megas, donde está la cabecera del vídeo (también dentro de un
// ZIP o RAR, que los canales publican sin comprimir), se analizan con ffprobe y se borran.
class ReleaseProber {
public:
    struct Result {
        bool ok = false;
        std::string error;  // En castellano, si !ok
        library::VideoInfo info;
    };

    ReleaseProber(DbManager& db, TelegramClient& telegram);

    ReleaseProber(const ReleaseProber&) = delete;
    ReleaseProber& operator=(const ReleaseProber&) = delete;

    // Comprueba un archivo lógico (por su primera parte) y guarda el resultado en la BD. Tarda unos
    // segundos; las comprobaciones van de una en una. No llamar desde el hilo de TDLib.
    Result probe(const Catalog::Release& release);

private:
    DbManager& db_;
    TelegramClient& telegram_;
    std::mutex mutex_;  // Una comprobación cada vez
};

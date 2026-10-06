#pragma once

#include <functional>
#include <string>
#include <vector>

// Ejecución de programas externos sin shell (docs/DECISIONS.md, D-018): posix_spawn con los
// argumentos separados, así que un nombre de fichero con comillas o ";" no puede inyectar nada.
namespace process {

struct Result {
    bool started = false;  // false si no se pudo lanzar (el motivo está en output)
    bool stopped = false;  // Se paró porque shouldStop() devolvió true
    int exitCode = -1;     // Código de salida (-1 si terminó por una señal)
    std::string output;    // Salida estándar y de errores juntas (las últimas 64 kB)
};

// Ejecuta argv[0] (ruta absoluta) con stdin vacío. onOutput recibe la salida a trozos según llega;
// shouldStop se consulta cada medio segundo y, si devuelve true, el proceso recibe SIGTERM.
// El hijo no hereda descriptores abiertos ni la máscara de señales bloqueadas del servicio.
Result run(const std::vector<std::string>& argv, const std::function<void(const std::string&)>& onOutput,
           const std::function<bool()>& shouldStop);

}  // namespace process

#include <iostream>
#include "httplib.h"

int main() {
    // Inicializar el servidor HTTP
    httplib::Server svr;

    // Endpoint de prueba (Status)
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status": "Telegarrm is running", "version": "0.1.0"})", "application/json");
    });

    // Configurar la carpeta web estática (Fase 1)
    auto ret = svr.set_mount_point("/", "./web");
    if (!ret) {
        std::cerr << "Advertencia: El directorio './web' no existe o no se puede montar." << std::endl;
    }

    std::cout << "Iniciando Telegarrm (Fase 0)..." << std::endl;
    std::cout << "Servidor web escuchando en http://localhost:8080" << std::endl;

    // Arrancar el servidor en el puerto 8080
    // svr.listen bloqueará el hilo principal, actuando como nuestro bucle daemon temporal.
    svr.listen("0.0.0.0", 8080);

    return 0;
}

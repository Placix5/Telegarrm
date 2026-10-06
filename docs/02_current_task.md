# Tarea Actual para Claude Code

**Fase Actual:** Inicio de Fase 1 (Integración de Base de Datos y TDLib base).

## Estado Actual
Gemini (el arquitecto) ya ha creado la estructura de carpetas (`src`, `include`, `build`, `web`, `db`), el archivo `CMakeLists.txt` inicial, la librería `httplib.h` y un `main.cpp` con el servidor web básico.

## Tus Objetivos

1.  **Refinar el CMakeLists.txt**: 
    *   Prepara el sistema de compilación para encontrar y enlazar `SQLite3`.
    *   (Opcional por ahora pero necesario pronto) Prepara el enlace para `TDLib`.
2.  **Integrar Base de Datos**:
    *   Crea una clase o módulo para gestionar SQLite (ej. `src/db_manager.cpp` / `include/db_manager.hpp`).
    *   Debe inicializar la base de datos en la carpeta `db/telegarrm.db`.
    *   Crea una tabla básica `settings (key TEXT PRIMARY KEY, value TEXT)` y una tabla `channels (id INTEGER PRIMARY KEY, name TEXT)`.
3.  **Endpoint de prueba de BD**:
    *   En `main.cpp`, modifica el endpoint `/api/status` para que también intente leer una clave (ej. `version`) desde la tabla `settings` de SQLite, demostrando que la base de datos está conectada y funcionando.
4.  **Preparación de Hilos para TDLib**:
    *   Crea un archivo esqueleto `src/telegram_client.cpp` y su cabecera `include/telegram_client.hpp`.
    *   Implementa una clase `TelegramClient` que tenga un método `start()`. Por ahora, este método solo debe lanzar un hilo (`std::thread`) con un bucle infinito que imprima "Hilo TDLib simulado corriendo..." cada 5 segundos.
    *   En `main.cpp`, instancia `TelegramClient`, llama a `start()`, y luego inicia el servidor HTTP (`svr.listen()`).

## Criterios de Aceptación
*   El proyecto debe compilar sin errores usando CMake.
*   El servidor web no debe bloquearse (ambos hilos deben correr concurrentemente).
*   Se debe generar el archivo SQLite `db/telegarrm.db` automáticamente al ejecutar.
*   El endpoint `/api/status` devuelve JSON válido sin errores.

¡Comienza analizando los archivos actuales en `src/` e `include/` y ejecuta los comandos necesarios para lograr esto! Cuando termines, asegúrate de documentar brevemente los cambios.

# Telegarrm - Guía para Claude Code

Eres Claude Code, el desarrollador principal de este proyecto. Tu tarea es implementar el código en base a las especificaciones dadas por el usuario y el arquitecto (Gemini).

## Sobre el Proyecto
Telegarrm es un servicio *daemon* (estilo stack ARR como Sonarr/Radarr) escrito en C++ que utiliza Telegram (TDLib) como fuente para descargar series y películas de canales específicos, exponiendo una API REST y una interfaz web estática.

## Stack Tecnológico
*   **Lenguaje**: C++ (Estándar C++17 o superior).
*   **Sistema de Build**: CMake.
*   **Servidor Web**: `cpp-httplib` (solo cabecera, `include/httplib.h`).
*   **Librería Telegram**: TDLib (Telegram Database Library).
*   **Base de Datos**: SQLite3 (C API).

## Reglas de Desarrollo
1.  **NO bloquees el hilo principal**: El servidor HTTP de `cpp-httplib` usa un método `listen()` bloqueante. TDLib y sus callbacks deben correr en hilos secundarios (ej. `std::thread`).
2.  **Gestión de Memoria**: Al usar TDLib y C++, presta especial atención a la fuga de memoria y al uso de punteros inteligentes (`std::unique_ptr`, `std::shared_ptr`).
3.  **Código Modular**: Mantén separada la lógica de base de datos (`src/db_manager.cpp`), la de web (`src/api.cpp`, aún por crear: de momento los endpoints están en `src/main.cpp`) y la de Telegram (`src/telegram_client.cpp`), con sus cabeceras en `include/`. La carpeta `db/` no es código: solo contiene la BD generada al ejecutar (ignorada por git).
4.  **Responde en español** en los comentarios del código y en la terminal, salvo requerimiento técnico.

## Comandos de Proyecto
*   **Dependencias** (Debian / Raspberry Pi OS): `sudo apt install build-essential cmake libsqlite3-dev`
*   **Compilar**: `cmake -S . -B build && cmake --build build`
*   **Ejecutar**: `./build/telegarrm` (o `.exe` en Windows), desde la raíz del proyecto: las rutas `db/` y `web/` son relativas al directorio actual.

## Tarea Actual Activa
Por favor, lee el archivo `docs/02_current_task.md` para conocer tu tarea inmediata. Cuando termines, realiza los commits necesarios y avisa al usuario.

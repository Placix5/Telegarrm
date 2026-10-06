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
3.  **Código Modular**: Mantén separada la lógica de base de datos (`db/` o `src/db.cpp`), la de web (`src/api.cpp`) y la de Telegram (`src/telegram.cpp`).
4.  **Responde en español** en los comentarios del código y en la terminal, salvo requerimiento técnico.

## Comandos de Proyecto
*   **Compilar**: `mkdir build && cd build && cmake .. && cmake --build .`
*   **Ejecutar**: `./build/telegarrm` (o `.exe` en Windows).

## Tarea Actual Activa
Por favor, lee el archivo `docs/02_task_phase_0.md` para conocer tu tarea inmediata. Cuando termines, realiza los commits necesarios y avisa al usuario.

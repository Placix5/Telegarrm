# Telegarrm - Guía para Claude Code

Eres Claude Code, el desarrollador principal y responsable técnico de este proyecto: planificas e implementas el código. Gemini revisa el código y la documentación; el usuario (Plácido) decide prioridades. La documentación antigua de `docs/` escrita por Gemini puede ser incoherente: contrástala con el código antes de seguirla.

## Sobre el Proyecto
Telegarrm es un servicio *daemon* (estilo stack ARR como Sonarr/Radarr) escrito en C++ que utiliza Telegram (TDLib) como fuente para descargar series y películas de canales específicos, exponiendo una API REST y una interfaz web estática. Corre en una Raspberry Pi (Debian, arm64).

El proyecto anterior (CLI) está en `~/TelegramDownloader`: de ahí se migran el parser de episodios, la descompresión y el renombrado.

## Stack Tecnológico
*   **Lenguaje**: C++ (Estándar C++17 o superior).
*   **Sistema de Build**: CMake.
*   **Servidor Web**: `cpp-httplib` (solo cabecera, `include/httplib.h`).
*   **JSON**: `nlohmann/json` (solo cabecera, `include/nlohmann/json.hpp`).
*   **Librería Telegram**: TDLib, interfaz JSON (`td_send`/`td_receive`), compilada en `~/td/tdlib`.
*   **Base de Datos**: SQLite3 (C API).

## Reglas de Desarrollo
1.  **NO bloquees el hilo principal**: El servidor HTTP de `cpp-httplib` usa un método `listen()` bloqueante. TDLib y sus callbacks deben correr en hilos secundarios (ej. `std::thread`).
2.  **Gestión de Memoria**: Al usar TDLib y C++, presta especial atención a la fuga de memoria y al uso de punteros inteligentes (`std::unique_ptr`, `std::shared_ptr`).
3.  **Código Modular**: Mantén separada la lógica de base de datos (`src/db_manager.cpp`), la de web/API (`src/api.cpp`) y la de Telegram (`src/telegram_client.cpp`), con sus cabeceras en `include/`. La carpeta `db/` no es código: solo contiene los datos generados al ejecutar (ignorada por git).
4.  **Responde en español** en los comentarios del código y en la terminal, salvo requerimiento técnico.
5.  **Secretos solo en variables de entorno** (`TELEGARRM_API_ID`, `TELEGARRM_API_HASH`): nunca en git, en la BD ni en los logs.
6.  **Sin shell**: no ejecutes comandos externos con `system`/`popen` y cadenas concatenadas; usa `posix_spawn` con argumentos separados.
7.  **Localización**: unidades del SI (kB/MB/GB, MB/s, base 1000), fechas `dd/mm/aaaa`, hora `Europe/Madrid`.

## Comandos de Proyecto
*   **Dependencias** (Debian / Raspberry Pi OS): `sudo apt install build-essential cmake libsqlite3-dev gperf zlib1g-dev libssl-dev 7zip 7zip-rar ffmpeg`, más TDLib compilado en `~/td/tdlib` (ver README). 7-Zip lo usa la importación a la biblioteca (`7zip-rar` añade el códec de RAR); `ffprobe` (de `ffmpeg`) lee la calidad real de cada vídeo.
*   **Compilar**: `cmake -S . -B build && cmake --build build`
*   **Tests**: `ctest --test-dir build` (parser y catálogo, con casos reales en `tests/parser_tests.cpp`). Cada formato nuevo de canal que se descubra se añade como caso de test.
*   **Ejecutar**: `TELEGARRM_API_ID=... TELEGARRM_API_HASH=... [TELEGARRM_TMDB_TOKEN=...] ./build/telegarrm` desde la raíz del proyecto: las rutas `db/` y `web/` son relativas al directorio actual.
*   **En la Pi corre como servicio de sistema** (`telegarrm.service`). Desplegar: `cmake --build build && systemctl restart telegarrm` (sin sudo, por la regla de polkit). Logs: `journalctl -u telegarrm`. Las pruebas de arranque y de errores, en otro directorio de trabajo con credenciales falsas, para no tocar la sesión real.

## Flujo de Trabajo
*   La planificación vigente está en `docs/ROADMAP.md`: mantenla al día al cerrar cada hito.
*   **Registra cada decisión en `docs/DECISIONS.md` en el momento de tomarla** (contexto, decisión, alternativas descartadas, consecuencias). Si una cambia, márcala como sustituida en lugar de borrarla.
*   Commits pequeños y descriptivos, **solo en local**: Plácido decide cuándo hacer push a GitHub.
*   Antes de dar algo por terminado: compilar sin warnings, pasar los tests y probarlo en la Pi.

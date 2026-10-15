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
*   **Herramientas externas** (lanzadas sin shell): 7-Zip (`7zip` y `7zip-rar`) para descomprimir y `ffprobe` (paquete `ffmpeg`) para leer la calidad real de los vídeos. zlib para leer el principio de un ZIP comprimido.

La arquitectura (hilos, flujo de datos, tablas y módulos) está en `docs/01_architecture_and_phases.md`.

## Reglas de Desarrollo
1.  **NO bloquees el hilo principal**: El servidor HTTP de `cpp-httplib` usa un método `listen()` bloqueante. TDLib y sus callbacks deben correr en hilos secundarios (ej. `std::thread`).
2.  **Gestión de Memoria**: Al usar TDLib y C++, presta especial atención a la fuga de memoria y al uso de punteros inteligentes (`std::unique_ptr`, `std::shared_ptr`).
3.  **Código Modular**: Mantén separada la lógica de base de datos (`src/db_manager.cpp`), la de web/API (`src/api.cpp`) y la de Telegram (`src/telegram_client.cpp`), con sus cabeceras en `include/`. La carpeta `db/` no es código: solo contiene los datos generados al ejecutar (ignorada por git).
4.  **Responde en español** en los comentarios del código y en la terminal, salvo requerimiento técnico.
5.  **Secretos solo en variables de entorno** (`TELEGARRM_API_ID`, `TELEGARRM_API_HASH`, `TELEGARRM_TMDB_TOKEN`; en la Pi, en `~/.config/telegarrm/env`): nunca en git, en la BD ni en los logs.
6.  **Sin shell**: no ejecutes comandos externos con `system`/`popen` y cadenas concatenadas; usa `posix_spawn` con argumentos separados.
7.  **Localización**: unidades del SI (kB/MB/GB, MB/s, base 1000), fechas `dd/mm/aaaa`, hora `Europe/Madrid`.

## Comandos de Proyecto
*   **Dependencias** (Debian / Raspberry Pi OS): `sudo apt install build-essential cmake libsqlite3-dev gperf zlib1g-dev libssl-dev 7zip 7zip-rar ffmpeg`, más TDLib compilado en `~/td/tdlib` (ver README). 7-Zip lo usa la importación a la biblioteca (`7zip-rar` añade el códec de RAR); `ffprobe` (de `ffmpeg`) lee la calidad real de cada vídeo.
*   **Compilar**: `cmake -S . -B build && cmake --build build`
*   **Tests**: `ctest --test-dir build` o `./build/telegarrm_tests` (parser, catálogo, BD, biblioteca y seguimiento, con casos reales en `tests/parser_tests.cpp`; algunos generan vídeos y comprimidos con ffmpeg y 7-Zip). Cada formato nuevo de canal que se descubra se añade como caso de test. La compilación completa tarda más de 2 min en la Pi: lánzala en segundo plano.
*   **Ejecutar**: `TELEGARRM_API_ID=... TELEGARRM_API_HASH=... [TELEGARRM_TMDB_TOKEN=...] ./build/telegarrm` desde la raíz del proyecto: las rutas `db/` y `web/` son relativas al directorio actual.
*   **En la Pi corre en Docker** desde el 15/10/2026 (D-054), con el compose en `/opt/docker/telegarrm` (`compose.yaml`, `.env` y `secretos/`), los datos de siempre (`~/Telegarrm/db`, montada) y la imagen que publica GitHub Actions. Desplegar: Plácido hace el push, se espera a que GitHub publique la imagen (unos 3 min) y `cd /opt/docker/telegarrm && sg docker -c "docker compose pull && docker compose up -d"` (`plax` está en el grupo `docker`; `sg` le da el grupo a la orden). Antes, mira en la BD (`downloads`) que no haya nada descargándose o importándose: se reanuda, pero la importación se repite. Logs: `sg docker -c "docker compose logs"`. El servicio nativo (`telegarrm.service`) está desactivado: no lo arranques con el contenedor en marcha (misma sesión de Telegram); solo para volver atrás (parar el contenedor y `systemctl start telegarrm`). Lo compilado en `build/` sirve para tests y para instancias de prueba en otra carpeta, con credenciales falsas y otro `TELEGARRM_PORT`.
*   **La API pide sesión** (D-053): la contraseña y las sesiones son de Plácido. No leas `db/codigo-acceso.txt` ni crees, cambies o restablezcas la contraseña. Para probar la API, levanta otra instancia en una carpeta temporal con credenciales de Telegram falsas y `TELEGARRM_PORT` (ej. 8091), crea allí una contraseña de prueba y párala por su PID al terminar (no con `pkill -f`, que coincide con la propia orden).
*   **La web** (`web/`) se sirve tal cual: un cambio se ve al recargar la página, sin recompilar ni reiniciar. Valida `app.js` como ES2017 (sin `??` ni `{...obj}`). Para verla, la extensión Claude in Chrome en `http://plax.local:8080`: abre una pestaña propia y ciérrala al terminar.
*   **Probar el móvil**: la ventana de Chrome no se puede redimensionar. Carga la web en un `<iframe>` de 390×844 desde la consola de la pestaña: dentro, las reglas `@media` se aplican como en un teléfono. Mide los desbordes con `getBoundingClientRect` en vez de fiarte de la vista. Recarga con Ctrl+Mayús+R antes, para no ver CSS en caché.
*   **Cambios en el parser o el catálogo**: *Canal Principal* (el canal de Plácido que se añadió primero; su identificador real está en la memoria local de Claude, no en el repositorio) es lo primero y no puede empeorar. Antes de desplegar, compara el catálogo antes y después con `tools/catalog_dump.cpp`: `cmake --build build --target telegarrm_catalog_dump` y `./build/telegarrm_catalog_dump <copia.db> <salida.tsv>`, sobre una copia hecha con la API de copia de SQLite. Sus líneas de *Canal Principal* deben salir idénticas; si no, pregunta a Plácido (o, si el cambio es justo para arreglar algo suyo, revisa una a una las que cambian y cuéntaselas).
*   **Cambios en el criterio de TMDB** (`metadata.cpp`): con `--tmdb` (y `TELEGARRM_TMDB_TOKEN` cargado de `~/.config/telegarrm/env` con `set -a; . ~/.config/telegarrm/env; set +a`), `telegarrm_catalog_dump` añade la coincidencia guardada y la nueva de cada obra. Revisa a mano las de *Canal Principal* que cambian o aparecen. Las series de los canales de anime no pueden caer en una serie de TMDB que tenga sus arcos como temporadas: sus episodios irían a la temporada 1 y sustituirían a los de verdad. Al cambiar el criterio, sube `kMatcherVersion`.
*   **Docker** (D-054): la imagen la compila GitHub Actions (`.github/workflows/docker.yml`) con cada push a `main`; el compose del servidor está en `deploy/docker/`. Ninguna clave puede ir en el `Dockerfile`, el compose, `env.example` ni la imagen: llegan por `*_FILE` (Docker secrets). TDLib está fijado en el `Dockerfile` al mismo commit que `~/td`; si cambia uno, cambia el otro. La Pi tiene poco disco libre para compilar TDLib dentro de Docker: las imágenes las compila GitHub. No toques los demás contenedores de la Pi (Jellyfin, Nginx Proxy Manager, wg-easy, DuckDNS).
*   **Migraciones de la BD**: antes de desplegar una (con el contenedor), copia `db/telegarrm.db` con la API de copia de SQLite (no con `cp`, por el WAL) a un sitio temporal.

## Flujo de Trabajo
*   La planificación vigente está en `docs/ROADMAP.md`: mantenla al día al cerrar cada hito.
*   **Registra cada decisión en `docs/DECISIONS.md` en el momento de tomarla** (contexto, decisión, alternativas descartadas, consecuencias). Si una cambia, márcala como sustituida en lugar de borrarla.
*   Commits pequeños y descriptivos, **solo en local**: Plácido decide cuándo hacer push a GitHub.
*   **Añade al commit los archivos uno a uno** (nunca `git add -A`): Plácido y Gemini trabajan a la vez en la misma carpeta y puede haber archivos suyos sin revisar.
*   Antes de dar algo por terminado: compilar sin warnings, pasar los tests y probarlo en la Pi.
*   **No borres a mano archivos de la biblioteca (`/srv/media`) ni otros datos de Plácido**: dale el comando y que decida él. La aplicación sí borra, con sus reglas (D-037, D-041).
*   Las descargas de prueba van a la biblioteca real: avisa de qué queda para que Plácido lo borre si quiere.

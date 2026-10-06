# Telegarrm

Telegarrm es un servicio daemon (stack ARR) que utiliza Telegram (TDLib) como fuente e indexador para descargas automáticas de series y películas.

## Estructura del Proyecto
- `src/`: Código fuente C++
  - `main.cpp`: arranque del daemon y servidor HTTP
  - `api.cpp`: endpoints REST (`/api/...`)
  - `db_manager.cpp`: acceso a SQLite (clase `DbManager`)
  - `telegram_client.cpp`: cliente de TDLib en su propio hilo (clase `TelegramClient`)
  - `signal_watcher.cpp`: parada ordenada con SIGINT/SIGTERM (clase `SignalWatcher`)
- `include/`: Cabeceras y dependencias de un solo archivo (`httplib.h`, `nlohmann/json.hpp`)
- `web/`: Interfaz web (Frontend)
- `db/`: Datos generados al ejecutar: `telegarrm.db` (SQLite) y `tdlib/` (sesión de Telegram)
- `docs/`: Documentación; la planificación vigente está en [docs/ROADMAP.md](docs/ROADMAP.md)
- `build/`: Archivos de compilación

## Fases de Desarrollo
Detalle y tareas en [docs/ROADMAP.md](docs/ROADMAP.md).

- [x] **Fase 0**: Estructura base y servidor HTTP (`cpp-httplib`).
- [ ] **Fase 1**: Motor TDLib en un hilo propio y SQLite para configuración y estado. *En curso: falta el primer inicio de sesión real y el servicio systemd.*
- [ ] **Fase 2**: Canales y catálogo: lectura del historial de los canales elegidos y catálogo en la web.
- [ ] **Fase 3**: Descargas: cola, descompresión y renombrado (núcleo antiguo de C++).
- [ ] **Fase 4**: Tele-ARR: escucha de mensajes nuevos, reemplazo de calidades y auto-descarga de capítulos en seguimiento.

## Dependencias
- Compilador con C++17, CMake >= 3.14, SQLite3 y TDLib >= 1.8.
- Paquetes en Debian / Raspberry Pi OS:
  ```bash
  sudo apt install build-essential cmake git libsqlite3-dev gperf zlib1g-dev libssl-dev
  ```
- TDLib (solo la librería JSON compartida) en `~/td/tdlib`, donde CMake lo encuentra automáticamente. En una Raspberry Pi 5 tarda unos 30 minutos:
  ```bash
  git clone --depth 1 https://github.com/tdlib/td.git ~/td
  cmake -S ~/td -B ~/td/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/td/tdlib -DTD_INSTALL_STATIC_LIBRARIES=OFF
  cmake --build ~/td/build --target tdjson -j3
  cmake --install ~/td/build
  ```
  Si TDLib está en otra ruta, añade `-DTd_DIR=<prefijo>/lib/cmake/Td` al configurar Telegarrm.

## Configuración
Las credenciales de la API de Telegram se obtienen en <https://my.telegram.org> ("API development tools") y se pasan **solo por variables de entorno**:

| Variable | Descripción |
| --- | --- |
| `TELEGARRM_API_ID` | `api_id` de la aplicación (número) |
| `TELEGARRM_API_HASH` | `api_hash` de la aplicación |

Sin ellas, el servicio no arranca. La sesión de Telegram se guarda en `db/tdlib/` (permisos `0700`): da acceso completo a la cuenta, así que no la copies ni la subas a ningún sitio.

## Compilación y ejecución
```bash
cmake -S . -B build
cmake --build build
TELEGARRM_API_ID=123456 TELEGARRM_API_HASH=abcdef... ./build/telegarrm
```
Ejecútalo desde la raíz del proyecto: las rutas `db/` y `web/` son relativas al directorio actual. Ctrl+C (SIGINT) o SIGTERM lo detienen de forma ordenada.

La primera vez, abre `http://<ip-de-la-pi>:8080/` e inicia sesión en Telegram (teléfono, código y, si la tienes, contraseña de verificación en dos pasos). La sesión se conserva entre reinicios.

> La web aún no tiene autenticación y el inicio de sesión viaja por HTTP sin cifrar: úsala solo dentro de tu red local o a través de la VPN.

## API
| Método y ruta | Descripción |
| --- | --- |
| `GET /api/status` | Estado del servicio, de la BD (`version` leída de `settings`) y de Telegram. HTTP 503 si la BD falla. |
| `POST /api/telegram/auth/phone` | `{"phone_number": "+34..."}` |
| `POST /api/telegram/auth/code` | `{"code": "12345"}` |
| `POST /api/telegram/auth/password` | `{"password": "..."}` (verificación en dos pasos) |

Ejemplo de `/api/status`:
```json
{"status": "Telegarrm is running", "version": "0.1.0",
 "database": {"status": "ok", "version": "0.1.0"},
 "telegram": {"authorization_state": "authorizationStateReady", "connection_state": "connectionStateReady"}}
```
Los pasos del inicio de sesión responden `{"ok": true}`, o `{"error": "..."}` con HTTP 400 (dato incorrecto, ej. `PHONE_CODE_INVALID`), 409 (Telegram no espera ese dato ahora) o 504 (Telegram no responde).

## Historial de cambios
### Fase 1: TDLib real e inicio de sesión desde la web
- `TelegramClient` usa TDLib (interfaz JSON, `td_send`/`td_receive`) en su propio hilo. Cada respuesta se asocia a su petición mediante `@extra`, con variantes asíncrona (`send`) y síncrona con timeout (`request`). Al parar, cierra TDLib de forma ordenada (`close`) para que guarde su estado.
- Credenciales por variables de entorno; sesión en `db/tdlib/` con permisos `0700`.
- Endpoints de la API movidos a `src/api.cpp`; nuevos endpoints de inicio de sesión.
- Primera página web (`web/index.html`): estado del servicio y formulario de inicio de sesión con los errores de Telegram traducidos.
- El puerto se reserva antes de arrancar TDLib: una segunda instancia termina sin tocar la sesión de la primera.

### Inicio de Fase 1: SQLite e hilos
- `CMakeLists.txt` busca y enlaza SQLite3.
- `DbManager` crea `db/telegarrm.db` con las tablas `settings (key, value)` y `channels (id, name)`. Es seguro entre hilos (mutex interno) y gestiona los recursos de SQLite con `std::unique_ptr`.
- `/api/status` lee la clave `version` de `settings`, que se guarda en cada arranque.
- `SignalWatcher` atiende SIGINT/SIGTERM con `sigwait` en un hilo dedicado: detiene el servidor HTTP y el hilo de Telegram y cierra la BD antes de salir con código 0 (en Windows no hace nada).
- El servidor ya no usa `SO_REUSEPORT` (activado por defecto en cpp-httplib): una segunda instancia en el mismo puerto ahora falla en lugar de repartirse las peticiones con la primera.

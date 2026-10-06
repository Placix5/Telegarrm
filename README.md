# Telegarrm

Telegarrm es un servicio daemon (stack ARR) que utiliza Telegram (TDLib) como fuente e indexador para descargas automáticas de series y películas.

## Estructura del Proyecto
- `src/`: Código fuente C++
  - `main.cpp`: arranque del daemon y servidor HTTP
  - `db_manager.cpp`: acceso a SQLite (clase `DbManager`)
  - `telegram_client.cpp`: hilo del cliente de Telegram (clase `TelegramClient`, de momento simulado)
- `include/`: Cabeceras y dependencias de un solo archivo (ej. `httplib.h`)
- `web/`: Interfaz web (Frontend)
- `db/`: Bases de datos SQLite3 (`telegarrm.db` se crea al arrancar)
- `build/`: Archivos de compilación

## Fases de Desarrollo
Detalle en [docs/01_architecture_and_phases.md](docs/01_architecture_and_phases.md).

- [x] **Fase 0**: Estructura base y servidor HTTP (`cpp-httplib`).
- [ ] **Fase 1**: Motor TDLib en un hilo propio y SQLite para configuración y estado. *En curso: SQLite listo; el hilo de TDLib todavía es simulado.*
- [ ] **Fase 2**: Catálogo y frontend: lectura del historial del canal con expresiones regulares y endpoints para la web.
- [ ] **Fase 3**: Descarga y cola: descarga, descompresión y renombrado (core antiguo de C++).
- [ ] **Fase 4**: Tele-ARR: escucha de mensajes nuevos, reemplazo de calidades y auto-descarga de capítulos en seguimiento.

## Dependencias
- Compilador con C++17, CMake >= 3.14 y SQLite3 (cabeceras de desarrollo).
- En Debian / Raspberry Pi OS:
  ```bash
  sudo apt install build-essential cmake libsqlite3-dev
  ```
- TDLib (opcional por ahora): ver [Opciones de compilación](#opciones-de-compilación).

## Compilación
```bash
cmake -S . -B build
cmake --build build
./build/telegarrm
```
Ejecútalo desde la raíz del proyecto: las rutas `db/` y `web/` son relativas al directorio actual. Ctrl+C (SIGINT) o SIGTERM lo detienen de forma ordenada.

El servidor arrancará en `http://localhost:8080/api/status`, que devuelve el estado y la versión leída de la tabla `settings` de SQLite:
```json
{"status": "Telegarrm is running", "version": "0.1.0", "database": {"status": "ok", "version": "0.1.0"}}
```
Si la lectura de la base de datos falla, `database.status` vale `"error"` y la respuesta es HTTP 503.

### Opciones de compilación
| Opción | Por defecto | Descripción |
| --- | --- | --- |
| `TELEGARRM_WITH_TDLIB` | `OFF` | Busca TDLib (`find_package(Td 1.8.0)`), enlaza `Td::TdStatic` y define `TELEGARRM_HAS_TDLIB`. Si TDLib no está en una ruta estándar, añade `-DTd_DIR=<prefijo>/lib/cmake/Td`. |

## Historial de cambios
### Inicio de Fase 1: SQLite e hilo de Telegram
- `CMakeLists.txt` busca y enlaza SQLite3 y deja preparado el enlace opcional con TDLib.
- `DbManager` crea `db/telegarrm.db` con las tablas `settings (key, value)` y `channels (id, name)`. Es seguro entre hilos (mutex interno) y gestiona los recursos de SQLite con `std::unique_ptr`.
- `/api/status` lee la clave `version` de `settings`, que se guarda en cada arranque.
- `TelegramClient::start()` lanza un hilo que imprime "Hilo TDLib simulado corriendo..." cada 5 s mientras el servidor HTTP atiende en el hilo principal. `stop()` (y el destructor) lo detiene sin esperar al siguiente ciclo.
- `SignalWatcher` atiende SIGINT/SIGTERM con `sigwait` en un hilo dedicado: detiene el servidor HTTP y el hilo de Telegram y cierra la BD antes de salir con código 0 (en Windows no hace nada).
- El servidor ya no usa `SO_REUSEPORT` (activado por defecto en cpp-httplib): una segunda instancia en el mismo puerto ahora falla en lugar de repartirse las peticiones con la primera.

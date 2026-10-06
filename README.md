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
- [x] **Fase 0**: Estructura base y servidor HTTP (`cpp-httplib`).
- [ ] **Fase 1**: Frontend básico (Catálogo) y lectura de canal con expresiones regulares.
- [ ] **Fase 2**: Integración de descargas bajo demanda (Core antiguo de C++).
- [ ] **Fase 3**: Automatización, escuchador en segundo plano y reemplazo inteligente de calidades.

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
Ejecútalo desde la raíz del proyecto: las rutas `db/` y `web/` son relativas al directorio actual.

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
- El servidor ya no usa `SO_REUSEPORT` (activado por defecto en cpp-httplib): una segunda instancia en el mismo puerto ahora falla en lugar de repartirse las peticiones con la primera.

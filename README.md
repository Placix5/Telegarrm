# Telegarrm

Telegarrm es un servicio daemon (stack ARR) que utiliza Telegram (TDLib) como fuente e indexador para descargas automáticas de series y películas.

## Estructura del Proyecto
- `src/`: Código fuente C++
  - `main.cpp`: arranque del daemon y servidor HTTP
  - `api.cpp`: endpoints REST (`/api/...`)
  - `channel_sync.cpp`: copia el historial de los canales vigilados en SQLite (clase `ChannelSync`)
  - `media_parser.cpp`: interpreta nombres de fichero y fichas (episodios, título, año, calidad, idioma)
  - `catalog.cpp`: agrupa los mensajes de cada canal en series y películas (clase `Catalog`)
  - `metadata.cpp`, `tmdb_client.cpp`: metadatos de TMDB con caché local
  - `download_manager.cpp`: cola de descargas; `library.cpp`: descompresión, nombres para Jellyfin y sustitución de versiones; `process.cpp`: procesos externos sin shell
  - `tracker.cpp`: seguimiento de series y películas (episodios nuevos y versiones mejores)
  - `settings.cpp`: ajustes editables desde la web
  - `db_manager.cpp`: acceso a SQLite (clase `DbManager`)
  - `telegram_client.cpp`: cliente de TDLib en su propio hilo (clase `TelegramClient`)
  - `signal_watcher.cpp`: parada ordenada con SIGINT/SIGTERM (clase `SignalWatcher`)
- `include/`: Cabeceras y dependencias de un solo archivo (`httplib.h`, `nlohmann/json.hpp`)
- `tests/`: Tests del parser y del catálogo con ejemplos reales (`ctest`)
- `web/`: Interfaz web (`index.html`, `style.css`, `app.js`, sin dependencias)
- `deploy/`: Servicio de systemd, regla de polkit y script de instalación
- `db/`: Datos generados al ejecutar: `telegarrm.db` (SQLite) y `tdlib/` (sesión de Telegram)
- `docs/`: Documentación; la planificación vigente está en [docs/ROADMAP.md](docs/ROADMAP.md) y los motivos de cada decisión en [docs/DECISIONS.md](docs/DECISIONS.md)
- `build/`: Archivos de compilación

## Fases de Desarrollo
Detalle y tareas en [docs/ROADMAP.md](docs/ROADMAP.md).

- [x] **Fase 0**: Estructura base y servidor HTTP (`cpp-httplib`).
- [x] **Fase 1**: Motor TDLib en un hilo propio y SQLite para configuración y estado. *Completada.*
- [x] **Fase 2**: Canales y catálogo: lectura del historial de los canales elegidos, catálogo con versiones y metadatos de TMDB.
- [x] **Fase 3**: Descargas: cola, descompresión y renombrado para Jellyfin.
- [x] **Fase 4**: Tele-ARR: mensajes nuevos en tiempo real, seguimiento de series y películas, auto-descarga de episodios nuevos, sustitución por versiones mejores e historial de actividad.

## Dependencias
- Compilador con C++17, CMake >= 3.14, SQLite3 y TDLib >= 1.8.
- Paquetes en Debian / Raspberry Pi OS:
  ```bash
  sudo apt install build-essential cmake git libsqlite3-dev gperf zlib1g-dev libssl-dev 7zip 7zip-rar
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
| `TELEGARRM_TMDB_TOKEN` | Opcional. "API Read Access Token" de [TMDB](https://www.themoviedb.org/settings/api) para títulos de episodio, sinopsis y carátulas. Sin él, el catálogo usa solo las fichas de Telegram (y lo ya consultado). |

Sin ellas, el servicio no arranca.

El resto se configura desde la pestaña **Ajustes** de la web: búfer de descargas (dónde descarga TDLib; se aplica con "Reiniciar ahora"), bibliotecas de películas y series, espacio libre mínimo y si se conserva la versión anterior cuando el seguimiento la mejora. Estructura recomendada en [D-033](docs/DECISIONS.md#d-033-estructura-de-carpetas-srvmedia-y-jellyfin): todo en el mismo disco (`/srv/media/descargas`, `/srv/media/peliculas`, `/srv/media/series`) para que llevar lo descargado a la biblioteca sea instantáneo. La sesión de Telegram se guarda en `db/tdlib/` (permisos `0700`): da acceso completo a la cuenta, así que no la copies ni la subas a ningún sitio.

## Compilación y ejecución
```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
TELEGARRM_API_ID=123456 TELEGARRM_API_HASH=abcdef... ./build/telegarrm
```
Ejecútalo desde la raíz del proyecto: las rutas `db/` y `web/` son relativas al directorio actual. Ctrl+C (SIGINT) o SIGTERM lo detienen de forma ordenada.

La primera vez, abre `http://<ip-de-la-pi>:8080/` e inicia sesión en Telegram (teléfono, código y, si la tienes, contraseña de verificación en dos pasos). La sesión se conserva entre reinicios.

### Como servicio (systemd)
`deploy/telegarrm.service` es un servicio de sistema que corre como `plax`, aislado (solo puede escribir en `db/` y `/srv/media`), y lee las credenciales de `~/.config/telegarrm/env` (permisos `600`, con las dos variables de [Configuración](#configuración)). La regla de polkit `deploy/50-telegarrm.rules` permite a `plax` arrancarlo, pararlo y reiniciarlo sin `sudo`. Motivos en [docs/DECISIONS.md](docs/DECISIONS.md) (D-013).

El servicio solo puede escribir en `db/` y en `/srv/media`; para otra carpeta (ej. un RAID montado en otro sitio), pásala al script: `sudo ./deploy/install-service.sh /mnt/raid`.

Instalación con `sudo`, una vez y cada vez que cambie algo en `deploy/`. El script **copia** la unidad y la regla a `/etc` como ficheros de root; no las enlaza, porque las leen systemd y polkit con privilegios.
```bash
sudo ./deploy/install-service.sh
```
Desplegar una versión nueva del programa no necesita `sudo`: `cmake --build build && systemctl restart telegarrm` (con parada ordenada). Logs con `journalctl -u telegarrm -f`.

> La web aún no tiene autenticación y el inicio de sesión viaja por HTTP sin cifrar: úsala solo dentro de tu red local o a través de la VPN.

## API
| Método y ruta | Descripción |
| --- | --- |
| `GET /api/status` | Estado del servicio, de la BD (`version` leída de `settings`) y de Telegram. HTTP 503 si la BD falla. |
| `POST /api/telegram/auth/phone` | `{"phone_number": "+34..."}` |
| `POST /api/telegram/auth/code` | `{"code": "12345"}` |
| `POST /api/telegram/auth/password` | `{"password": "..."}` (verificación en dos pasos) |
| `GET /api/telegram/chats` | Canales y grupos de la cuenta (incluidos los archivados), con `type` y `role`, para elegir cuáles vigilar |
| `GET /api/channels` | Canales vigilados y estado de su sincronización |
| `POST /api/channels` | `{"chat_id": -100...}`: vigilar un canal o grupo; empieza a sincronizarse al momento |
| `DELETE /api/channels/{id}` | Dejar de vigilarlo (borra sus mensajes guardados, no los de Telegram) |
| `POST /api/channels/{id}/sync` | Buscar mensajes nuevos ya. Normalmente no hace falta: llegan al momento (D-036) y, además, hay una ronda cada 15 min |
| `GET /api/channels/{id}/messages?limit=50&offset=0` | Mensajes guardados, del más reciente al más antiguo (con su `topic_id`) |
| `GET /api/channels/{id}/topics` | Temas de un grupo con temas, con cuántos mensajes tiene cada uno |
| `GET /api/catalog` | Obras (series y películas) de todos los canales: título, títulos alternativos, año, versiones disponibles (`qualities`, `hdr`), idiomas, géneros, temas, `airing` (en emisión), `followed` (en seguimiento), temporadas, episodios, tamaño en bytes y datos de TMDB |
| `GET /api/catalog/{chat}/{ficha}` | Obra completa: sinopsis, ficha original, seguimiento (`follow`) y sus archivos lógicos (`releases`) con calidad, HDR, etiquetas de versión, temporada y episodio, y sus partes. Vale cualquier ficha de la obra |
| `GET /api/catalog/{chat}/{ficha}/poster` | Portada: la foto de la ficha (de Telegram) o, si no hay, la carátula de TMDB; se guardan tras la primera vez |
| `GET /api/downloads` | Cola de descargas con su progreso (`downloaded_size`, `bytes_per_second`, `import_percent`, `library_path`; `status`: queued, downloading, importing, completed —en la biblioteca—, failed, cancelled, replaced —sustituida por una versión mejor—), su origen (`origin`: manual o auto) y las descargas a las que sustituye (`replaces`) |
| `POST /api/downloads` | `{"chat_id": -100..., "message_id": ...}` (cualquier parte de un archivo del catálogo): 201, o 409 si ya está en la cola o descargado |
| `POST /api/downloads/{id}/cancel` | Cancelar (borra lo descargado a medias) |
| `POST /api/downloads/{id}/retry` | Reintentar una descarga fallida o cancelada |
| `DELETE /api/downloads/{id}` | Quitar de la lista una descarga terminada, fallida o cancelada |
| `GET /api/follows` | Obras seguidas: calidad máxima (`max_quality`), desde cuándo (`created_at`) y su ficha actual en el catálogo (`found` = sigue en él) |
| `POST /api/follows` | `{"chat_id": -100..., "anchor_id": ..., "max_quality": ""}` (`""` = la mejor, `"1080p"` o `"720p"`): seguir una obra; 409 si ya se sigue |
| `PUT /api/follows/{id}` | `{"max_quality": "1080p"}`: cambiar la calidad máxima |
| `DELETE /api/follows/{id}` | Dejar de seguir (lo descargado se queda) |
| `GET /api/activity?before={id}` | Historial de actividad, del más reciente al más antiguo, de 100 en 100, con enlace a la obra (`item`) |
| `GET /api/settings` | Ajustes, con la comprobación de cada ruta (escritura, espacio libre), si búfer y bibliotecas comparten disco y si hace falta reiniciar |
| `PUT /api/settings` | `{"download_dir", "movies_dir", "series_dir", "min_free_gb", "keep_replaced"}`: valida antes de guardar (400 con el motivo por campo) |
| `POST /api/restart` | Parada ordenada y reinicio (código 75; systemd lo vuelve a arrancar) |

Ejemplo de `/api/status`:
```json
{"status": "Telegarrm is running", "version": "0.1.0",
 "database": {"status": "ok", "version": "0.1.0"},
 "telegram": {"authorization_state": "authorizationStateReady", "connection_state": "connectionStateReady"}}
```
Los pasos del inicio de sesión responden `{"ok": true}`, o `{"error": "..."}` con HTTP 400 (dato incorrecto, ej. `PHONE_CODE_INVALID`), 409 (Telegram no espera ese dato ahora) o 504 (Telegram no responde).

## Historial de cambios
### Fase 4: seguimiento (Tele-ARR)
- Mensajes nuevos en tiempo real (D-036): los canales vigilados se abren en TDLib (`openChat`) y cada `updateNewMessage` dispara una sincronización rápida. Los avisos se agrupan para no saturar TDLib.
- Seguimiento (D-035): botón «Seguir» en cada ficha, con calidad máxima.
  - Los episodios nuevos de las series seguidas se descargan solos, también la primera versión que se publique de una película seguida. Solo cuenta lo publicado después de seguir.
  - Se espera a que estén todas las partes de un comprimido.
- Mejoras (D-037): una versión con más resolución, HDR o REMUX sustituye a la descargada. La anterior se borra cuando la nueva ya está en la biblioteca, o se conserva si así se elige en *Ajustes*.
- Pestaña *Actividad*: obras seguidas y el historial de lo que el sistema hace solo. Filtro «En seguimiento» en el catálogo; las descargas automáticas se distinguen en *Descargas*.
- Migración v7: tablas `follows`, `auto_releases` y `activity`; en `downloads`, origen, seguimiento, sustituciones y archivos colocados. Tests: 343 comprobaciones.
- Comprobado en la Pi: un episodio nuevo de una serie seguida (860 MB, 20 s) y una mejora de 1080p a 4K HDR que borró la versión anterior.

### Fase 3: descargas e importación a la biblioteca
- Importación (D-034): al terminar, cada descarga se descomprime si hace falta (7-Zip, sin shell), se eligen los vídeos y subtítulos y se mueven a la biblioteca con nombres para Jellyfin (`Título (Año) [tmdbid-N]/…`, `Season 01/Serie S01E01 - 1080p.mkv`), y se libera el búfer. Comprobado en la Pi con un ZIP (451 MB) y un RAR de 2 partes (1,86 GB, descomprimido en ~20 s); los RAR necesitan el paquete `7zip-rar`.
- Pestaña *Ajustes*: búfer de descargas de TDLib (aplicado con un reinicio ordenado desde la web), bibliotecas y espacio libre mínimo, con validación real de cada ruta (D-032). La unidad admite `/srv/media` y rutas adicionales vía `install-service.sh`.
- Arreglos de la web: flecha propia en los desplegables y botones de descarga de ancho fijo que muestran el progreso.
- Cola de descargas persistente: botón "Almacenar en disco" por versión o por temporada, pestaña *Descargas* con progreso, velocidad y tiempo restante, cancelación (borra lo parcial), reintento, reanudación tras reiniciar y comprobación de espacio libre. Los archivos quedan, de momento, en la caché de TDLib (D-031).

### Fase 2: canales, sincronización y catálogo
- Metadatos de TMDB (D-029, D-030): sinopsis, títulos y sinopsis de episodio, géneros, carátulas e identificadores externos, guardados en local; créditos de TMDB en la web. Funciona sin token con los datos de las fichas.
- Grupos con temas (foros): cada mensaje guarda su tema y las fichas se agrupan dentro de cada tema (migración v3, que relee el historial una vez).
- Obras con versiones: las temporadas, los episodios en emisión y las versiones 1080p/4K de una misma obra se unen por título; las partes de los archivos troceados (`.zip.001`, `.part01.rar`, `_part06.rar`) forman un único archivo. La web muestra las versiones de cada episodio o película y las series en emisión.
- Compilación optimizada por defecto (`RelWithDebInfo`) y caché del análisis: el catálogo de 33 000 mensajes se calcula en unos 2 s.
- Catálogo en memoria (`Catalog`): cada foto con pie (la "ficha") abre una serie o película y los vídeos siguientes le pertenecen. El parser (`media_parser`) reconoce `1x01`, `#01x01`, `S01E01`, `T1E3` y `Temporada 1 Capítulo 3`, y extrae de la ficha título, año, calidad, géneros e idioma (banderas incluidas). Tiene 103 comprobaciones con casos reales en `tests/`.
- Web con pestañas (Catálogo, Canales, Estado): cuadrícula con portadas, búsqueda sin acentos, filtro por tipo y ficha con los episodios por temporada.
- Esquema de BD versionado con migraciones (`PRAGMA user_version`); WAL para escribir menos en la tarjeta SD.
- `ChannelSync` copia el historial de los canales vigilados en la tabla `messages` (texto, nombre, tamaño y tipo de fichero), por lotes de 100. El cursor se guarda en la misma transacción que cada lote, así que tras un reinicio continúa donde lo dejó. Respeta los `FLOOD_WAIT` de Telegram y después busca mensajes nuevos cada 15 min.
- La web permite elegir los canales vigilados entre los chats de la cuenta, sincronizarlos a mano y quitarlos.
- Servicio de sistema de systemd aislado (`deploy/telegarrm.service`) y regla de polkit para reiniciarlo sin `sudo`. Sustituye al servicio de usuario inicial.
- Registro de decisiones en `docs/DECISIONS.md`.

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

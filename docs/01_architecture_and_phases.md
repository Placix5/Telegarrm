# Arquitectura de Telegarrm

*Actualizada el 10/10/2026: canales de anime (D-047), obras agrupadas por TMDB (D-048) y temporadas con ficha propia (D-049).*

El plan inicial (de Gemini) preveía una tabla `media` con las series y películas. No llegó a existir: el catálogo se calcula en memoria a partir de los mensajes guardados (D-020), así que una mejora del parser se aplica sin migraciones. Los motivos de cada decisión están en [DECISIONS.md](DECISIONS.md) y el plan, en [ROADMAP.md](ROADMAP.md).

## Visión general

Un único proceso en C++17 (`build/telegarrm`) que corre en la Raspberry Pi como servicio de systemd (D-013):

```
Telegram ⇄ TDLib ⇄ TelegramClient (hilo receptor: respuestas y actualizaciones)
                     │
                     ├─ ChannelSync ──────► tabla messages ──► Catalog (en memoria)
                     │  (historial, ronda cada 15 min                │
                     │   y mensajes nuevos al momento)               ├─► MetadataService ⇄ TMDB
                     │                                               │
                     ├─ ReleaseProber ◄──── Tracker (seguimiento) ◄──┘
                     │  (primeros MB → ffprobe)    │
                     │                             ▼
                     └─ DownloadManager ◄──── tabla downloads ◄──── API REST ◄── web (navegador)
                        (búfer → 7-Zip → ffprobe → biblioteca /srv/media)
```

## Canales

El catálogo distingue dos tipos de canal, que detecta solo por los nombres de sus archivos (D-047):
- **Normales**, como *Las Cositas*: fichas (foto con pie) seguidas de sus archivos, episodios `1x01` o `S01E01` y temas por categoría («Películas», «Series en emisión»…).
- **De anime**, como CrunchyShur: un tema por obra y numeración del anime (`Serie - 01`, `Serie S2 - 08`, `Serie T2 - 01`). Lo son si al menos una cuarta parte de sus archivos usa esa numeración.
  - Solo en ellos se aplican sus reglas: temporadas de la ficha, «Final Season», y extras y arcos unidos a la serie del tema.
  - Sus obras no se mezclan con las de otros canales, así que añadir un canal de anime no cambia el resto del catálogo.

**Temporadas con ficha propia** (D-049): una ficha de serie cuyo título amplía el de otra serie del canal y cuya numeración la continúa es esa serie (las temporadas 3 y 4 de *Kimetsu no Yaiba*, cada una con su ficha). Los spin-offs, que vuelven a la temporada 1, siguen aparte.

**La misma obra en varios canales** (D-048): el catálogo calcula las obras de cada canal por separado y TMDB dice cuáles son la misma (*Boku no Hero Academia* en CrunchyShur y *My Hero Academia* en *Las Cositas*). La web las muestra como una sola, y en su ficha se elige de qué canal ver los archivos. Cada una conserva su numeración, sus archivos y su seguimiento; lo que se descarga de cualquiera va a la misma carpeta de la biblioteca, porque la carpeta lleva el identificador de TMDB.
- Para reconocer una obra, `MetadataService` compara sus títulos con el nombre del resultado de TMDB (también con las mismas palabras en otro orden) y, si ninguno encaja, con los títulos alternativos de los tres primeros resultados (el romaji, por ejemplo).
- No valen los de una temporada, un arco o un especial. En los canales de anime, tampoco los de una secuela (*Full Metal Panic! The Second Raid*) ni los que no dicen que son el nombre de la obra entera: un arco publicado como serie aparte acabaría en la temporada 1 de la serie.

## Hilos

| Hilo | Qué hace | Cuándo trabaja |
| --- | --- | --- |
| Principal | Servidor HTTP (`cpp-httplib`, `listen` bloqueante); las peticiones las atiende su grupo de hilos | Siempre |
| `SignalWatcher` | Espera SIGINT/SIGTERM con `sigwait` y detiene el servidor (D-010) | Al parar |
| `TelegramClient` | Único `td_receive`: entrega cada respuesta a su petición (`@extra`) y reparte las actualizaciones (D-009) | Siempre |
| `ChannelSync` | Historial de los canales vigilados y mensajes nuevos: ronda completa cada 15 min y sincronización rápida 20 s después de un `updateNewMessage` (D-015, D-036) | Con mensajes nuevos |
| `MetadataService` | Busca en TMDB las obras nuevas del catálogo (D-029, D-030). Si cambia el criterio, vuelve a buscarlas todas, mostrando mientras los datos anteriores (D-048) | Cuando cambia el catálogo y cada 6 h |
| `DownloadManager` | Cola de descargas, de una en una; importa cada una a la biblioteca (D-031, D-034) | Con algo en cola |
| `Tracker` | Revisa las obras seguidas y pone en cola lo nuevo o mejor (D-035) | Cuando cambia el catálogo; cada 5 min si hay algo a medio publicar |

`ReleaseProber` (D-042) no tiene hilo propio: lo llaman la API y el `Tracker`, y sus comprobaciones van de una en una. Las herramientas externas (7-Zip y ffprobe) se lanzan con `posix_spawn`, sin shell (D-018).

**Orden de parada**: vigilante de señales, seguimiento, descargas, metadatos, sincronización y TDLib (que guarda su estado con `close`). Lo que se estaba descargando vuelve a la cola al arrancar.

## Recorrido de un episodio nuevo de una serie seguida

1. Telegram avisa con `updateNewMessage` (los canales vigilados están abiertos con `openChat`). `ChannelSync` espera 20 s sin avisos (2 min como mucho), trae los mensajes nuevos con `getChatHistory` y los guarda junto con su cursor.
2. `Catalog` recalcula ese canal: fichas, archivos, partes y obras.
3. `MetadataService` busca lo nuevo en TMDB. `Tracker` revisa las obras seguidas:
   - Comprueba la calidad real de lo que podría pedir (`ReleaseProber`).
   - Mira lo que ya se tiene: descargas y vídeos de la carpeta de la obra en la biblioteca (D-041).
   - Decide con `tracking::plan` y lo pone en cola, anotándolo en el historial (`activity`).
4. `DownloadManager` pide las partes a TDLib, que las guarda en el búfer. Después `library::importRelease`:
   1. Descomprime con 7-Zip si es un comprimido.
   2. Lee la calidad del vídeo con ffprobe.
   3. Lo renombra para Jellyfin y lo mueve a la biblioteca.
   4. Sustituye las versiones anteriores de ese episodio (D-037, D-041).

## Datos

**SQLite** (`db/telegarrm.db`): WAL, una conexión con mutex y migraciones con `PRAGMA user_version`. Va por la versión 8 (D-014).

| Tabla | Contenido |
| --- | --- |
| `settings` | Ajustes de la web, versión y marcas internas (`tmdb_matcher_version`, `quality_probe_version`) |
| `channels`, `messages`, `topics` | Canales vigilados con su cursor de sincronización, sus mensajes (texto o archivo) y los temas de los foros |
| `tmdb_cache`, `metadata`, `metadata_episodes` | Respuestas de TMDB y la coincidencia de cada obra, con sus episodios |
| `downloads`, `download_parts` | Cola de descargas: estado, progreso, origen (manual o del seguimiento), archivos colocados y descargas a las que sustituye |
| `follows`, `auto_releases` | Obras seguidas y archivos que el seguimiento ya puso en cola |
| `activity` | Historial de lo que hace el sistema solo (las 5000 entradas más recientes) |
| `release_probes` | Calidad real de cada archivo, comprobada antes de descargarlo o al importarlo |

**Fuera de SQLite**:
- `db/tdlib/`: sesión de Telegram (permisos `0700`).
- `db/tmdb/images/`: carátulas.
- El búfer de TDLib (`files_directory`, en *Ajustes*).
- La biblioteca: `/srv/media/peliculas` y `/srv/media/series`, con nombres para Jellyfin (D-033).

## Código

| Módulo | Responsabilidad |
| --- | --- |
| `main.cpp` | Arranque, servidor HTTP con la web estática de `web/`, orden de construcción y parada |
| `api.cpp` | Endpoints REST (`/api/...`) |
| `telegram_client.cpp` | TDLib: peticiones asíncronas y síncronas, autorización y reparto de actualizaciones |
| `channel_sync.cpp` | Historial y mensajes nuevos de los canales vigilados |
| `media_parser.cpp` | Nombres de fichero y fichas: episodios, título, año, calidad, etiquetas, partes e idiomas |
| `catalog.cpp` | Obras (`Item`) y archivos lógicos (`Release`) a partir de los mensajes; detecta los canales de anime (D-047) |
| `tmdb_client.cpp`, `metadata.cpp` | Cliente de TMDB con caché y límite de peticiones; coincidencia de cada obra y obras que son la misma (`works::group`, D-048) |
| `download_manager.cpp` | Cola de descargas, importación, sustitución de versiones e historial de descargas |
| `library.cpp` | Descompresión, ffprobe, nombres para Jellyfin, vídeos de la biblioteca y borrado seguro |
| `release_prober.cpp` | Calidad real de un archivo sin descargarlo entero |
| `tracker.cpp` | Seguimiento: reglas puras (`tracking::`) y su hilo (`Tracker`) |
| `settings.cpp` | Ajustes y comprobación de rutas |
| `process.cpp` | Procesos externos sin shell |
| `db_manager.cpp` | SQLite |
| `signal_watcher.cpp` | Parada ordenada |

Las reglas de decisión (parser, catálogo, seguimiento, episodios que faltan, nombres, borrado seguro, novedades) son funciones puras o casi. Se prueban en `tests/parser_tests.cpp` con ejemplos reales de los canales (505 comprobaciones; D-022). `tools/catalog_dump.cpp` calcula el catálogo de una copia de la BD para comparar un cambio del parser obra por obra (D-047) y, con `--tmdb`, la coincidencia de TMDB guardada y la del criterio actual (D-048).

## Web

HTML, CSS y JavaScript sin dependencias ni compilación (D-016), servidos desde `web/`: un cambio se ve al recargar la página, sin reiniciar el servicio. Rutas con `#/…` (catálogo, ficha, descargas, actividad, canales, ajustes y estado) y sondeo periódico de la API.

El catálogo muestra una tarjeta por obra, con las de varios canales juntas («En 2 canales»), y se puede filtrar por canal. La ficha de una de esas obras empieza con los canales donde está: cada uno lleva a su ficha (D-048).

Pensada también para el móvil (D-046):
- Hasta 640 px de ancho, las pestañas forman una rejilla de 3×2 y las tablas de episodios y versiones se convierten en bloques.
- En pantallas táctiles, botones y selectores miden al menos 44 px.
- Las confirmaciones son un `<dialog>` propio y los errores, avisos en la esquina (D-044).

Las novedades llegan a la web sin conexiones permanentes (D-043):
- Al recalcular un canal, `Catalog` anota los archivos lógicos nuevos como novedades (las 100 últimas, en memoria).
- `/api/status`, que la web pide cada 2 s, trae el número de la última novedad, y `/api/events` da las siguientes.
- Si una es de la obra abierta, la ficha se redibuja en su sitio y aparece un aviso en la esquina.
- La ficha abierta también se redibuja cuando cambia una de sus descargas o cada 30 s, solo si su contenido ha cambiado (D-045).

## Seguridad

- Secretos solo por variables de entorno (D-006).
- Sesión de TDLib con permisos `0700` (D-008).
- Servicio aislado: solo escribe en `db/` y `/srv/media` (D-013).
- Procesos externos sin shell (D-018).
- Solo se borran ficheros de la biblioteca que la propia aplicación colocó o que son versiones anteriores de lo importado, nunca fuera de sus carpetas (D-037, D-041).
- La web aún no tiene autenticación ni HTTPS: solo para la red local (ver ROADMAP).

# Desarrollo

Cómo está hecho Telegarrm y cómo trabajar en él. La explicación técnica de fondo está en [arquitectura.md](arquitectura.md); el porqué de cada decisión, en [DECISIONS.md](DECISIONS.md); y lo que falta por hacer, en [ROADMAP.md](ROADMAP.md).

## Tecnología
- **C++17** con CMake, en un único programa que hace de servidor.
- **[TDLib](https://github.com/tdlib/td)**, la librería oficial de Telegram, a través de su interfaz JSON.
- **SQLite** para todos los datos.
- **[cpp-httplib](https://github.com/yhirose/cpp-httplib)** para la web y la API, y **[nlohmann/json](https://github.com/nlohmann/json)** para el JSON. Las dos son de un solo archivo y están en `include/`.
- **OpenSSL** (consultas HTTPS a TMDB y la contraseña de la web) y **zlib**.
- **7-Zip** y **ffprobe** como programas externos, lanzados sin terminal (`posix_spawn`).
- La web es **HTML, CSS y JavaScript sin dependencias ni compilación**: se sirve tal cual desde `web/`.

## Estructura
- `src/`, `include/`: el programa.
  - `main.cpp`: arranque, servidor HTTP y parada ordenada.
  - `api.cpp`: la API REST y el filtro que exige sesión en ella.
  - `auth.cpp`: contraseña de la web (scrypt), sesiones y límite de intentos.
  - `telegram_client.cpp`: TDLib en su propio hilo.
  - `channel_sync.cpp`: copia los mensajes de los canales vigilados en SQLite, con los nuevos al momento.
  - `media_parser.cpp`: lee nombres de archivo y fichas (episodio, título, año, calidad, idioma).
  - `catalog.cpp`: agrupa los mensajes en series y películas con sus versiones.
  - `metadata.cpp`, `tmdb_client.cpp`: datos de TMDB, con caché local.
  - `download_manager.cpp`: cola de descargas, importación a la biblioteca y sustitución de versiones.
  - `library.cpp`: descompresión, calidad real con ffprobe, nombres para Jellyfin y borrado seguro.
  - `release_prober.cpp`: calidad real de un archivo leyendo solo su principio.
  - `tracker.cpp`: seguimiento de series y películas.
  - `settings.cpp`, `db_manager.cpp`, `process.cpp`, `signal_watcher.cpp`: ajustes y claves, SQLite, procesos externos y señales.
- `web/`: la interfaz (`index.html`, `style.css`, `app.js`).
- `tests/`: los tests, con nombres y fichas reales de canales.
- `tools/catalog_dump.cpp`: calcula el catálogo de una copia de la base de datos, para comparar un cambio del parser obra por obra.
- `deploy/`: compose de Docker (`deploy/docker/`) y servicio de systemd.
- `Dockerfile` y `.github/workflows/docker.yml`: la imagen de Docker y su publicación automática.
- `docs/`: la documentación.

## Compilar y probar
Con las dependencias de [la instalación sin Docker](instalacion-nativa.md):
```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build        # o ./build/telegarrm_tests
```
Los tests cubren el parser, el catálogo, la base de datos, la biblioteca, el seguimiento, el criterio de TMDB (con respuestas guardadas, sin red) y el acceso. Algunos generan vídeos y comprimidos con ffmpeg y 7-Zip. Cada formato nuevo de canal que se descubre se añade como caso de test.

Para probar sin tocar una instalación en uso, levanta otra instancia en una carpeta temporal, con claves de Telegram falsas y otro puerto:
```bash
mkdir -p /tmp/prueba && cd /tmp/prueba && ln -s ~/Telegarrm/web web
TELEGARRM_API_ID=1 TELEGARRM_API_HASH=falso TELEGARRM_PORT=8091 ~/Telegarrm/build/telegarrm
```

### Comparar el catálogo
Un cambio en el parser o el catálogo puede alterar cientos de obras. Antes de darlo por bueno, se compara el catálogo antes y después sobre una copia de la base de datos (hecha con la API de copia de SQLite, no con `cp`, por el registro WAL):
```bash
cmake --build build --target telegarrm_catalog_dump
./build/telegarrm_catalog_dump copia.db antes.tsv     # con el código anterior
./build/telegarrm_catalog_dump copia.db despues.tsv   # con el nuevo
```
Con `--tmdb` (y `TELEGARRM_TMDB_TOKEN`) añade, para cada obra, la coincidencia de TMDB guardada y la que da el criterio actual.

## La imagen de Docker
GitHub Actions (`.github/workflows/docker.yml`) la compila con cada push a `main`, en máquinas x86_64 y ARM64, y la publica en `ghcr.io/placix5/telegarrm`. El `Dockerfile` tiene tres etapas:
1. TDLib, fijado en una versión concreta;
2. Telegarrm, compilado y con los tests pasados (si fallan, no hay imagen);
3. la imagen final, solo con lo necesario.

La caché guarda TDLib compilado, así que cada versión nueva tarda unos minutos. Ninguna clave puede ir en el `Dockerfile`, el compose ni la imagen.

## Normas del proyecto
- **Idioma**: el código se comenta en castellano, y la web y los mensajes están en castellano.
- **Localización**: unidades del Sistema Internacional (kB, MB, GB en base 1000), fechas `dd/mm/aaaa` y hora de `Europe/Madrid`.
- **Claves**: solo por variables de entorno o archivos; nunca en git, en la base de datos ni en el registro.
- **Sin terminal**: los programas externos se lanzan con `posix_spawn` y argumentos separados, nunca con `system` o `popen`.
- **Hilos**: el servidor HTTP ocupa el hilo principal; TDLib, la sincronización, los metadatos, las descargas y el seguimiento tienen cada uno el suyo.
- **JavaScript**: compatible con ES2017 (sin `??` ni `{...obj}`).
- **Decisiones**: cada una se anota en [DECISIONS.md](DECISIONS.md) cuando se toma, con su contexto, las alternativas descartadas y sus consecuencias.

## Documentación técnica
- [arquitectura.md](arquitectura.md): hilos, flujo de datos, tablas y módulos.
- [DECISIONS.md](DECISIONS.md): el registro de todas las decisiones de diseño, de la D-001 en adelante.
- [ROADMAP.md](ROADMAP.md): la hoja de ruta: lo hecho por fases y lo pendiente.
- [historial.md](historial.md): los cambios de cada versión.
- [historico/](historico/): notas de trabajo del desarrollo inicial.

Telegarrm se desarrolla con la ayuda de asistentes de IA. [`CLAUDE.md`](../CLAUDE.md), en la raíz, recoge las instrucciones de trabajo para Claude Code. El registro de decisiones y la hoja de ruta mencionan a las personas y asistentes que participan: el autor del proyecto (Plácido), Claude Code en el desarrollo y Gemini en la revisión.

# Telegarrm

Telegarrm convierte canales y grupos de Telegram en una biblioteca de series y películas, al estilo de Sonarr y Radarr. Lee lo que se publica en los canales que elijas, lo ordena en un catálogo con portadas y sinopsis, y lo descarga a tu biblioteca con nombres pensados para Jellyfin (que Plex y Emby también reconocen). Si sigues una serie, descarga solo cada episodio nuevo en cuanto se publica.

Se maneja desde el navegador, también desde el móvil, y está pensado para funcionar siempre encendido en un servidor doméstico o una Raspberry Pi.

## Qué hace
- **Catálogo**: las series y películas de tus canales, con portada, sinopsis, géneros y títulos de episodio (de [TMDB](https://www.themoviedb.org/)). Muestra todas las versiones de cada episodio o película (1080p, 4K HDR, REMUX…) y junta la misma obra aunque esté en varios canales o con otro nombre.
- **Descargas**: un episodio, una temporada o la serie entera (solo lo que te falta). Descomprime lo que haga falta (ZIP, RAR, 7z), lo renombra y lo deja en tu biblioteca.
- **Seguimiento**: al seguir una serie, sus episodios nuevos se descargan solos; si se publica una versión mejor (más resolución, HDR…), sustituye a la que tenías.
- **Calidad real**: comprueba la resolución y el HDR de un archivo leyendo solo su principio, sin descargarlo entero.
- **Biblioteca**: lo que tienes en disco, con la opción de borrar series, temporadas o archivos.
- **Actividad**: un historial de lo que Telegarrm ha hecho solo.

## Cómo funciona
1. Inicias sesión con tu cuenta de Telegram desde la web de Telegarrm.
2. Eliges qué canales o grupos vigilar. Telegarrm lee sus mensajes y reconoce las fichas (una foto con la información de la obra) y los archivos de vídeo que les siguen.
3. Navegas por el catálogo y descargas o sigues lo que quieras. Los archivos llegan a tu biblioteca con nombres como `Serie (2020) [tmdbid-123]/Season 01/Serie S01E01 - 1080p.mkv`.

## Requisitos
- Un equipo Linux con **Docker**, x86_64 o ARM64 (una Raspberry Pi 4 o 5 sirve). También se puede [compilar e instalar sin Docker](docs/instalacion-nativa.md).
- Una cuenta de **Telegram** y las claves de su API (`api_id` y `api_hash`), gratuitas en <https://my.telegram.org>, en «API development tools».
- Opcional: un token de **TMDB** («API Read Access Token», gratuito en <https://www.themoviedb.org/settings/api>) para portadas, sinopsis y títulos de episodio.
- Una carpeta para la biblioteca, mejor con el búfer de descargas en el mismo disco.

## Puesta en marcha rápida (Docker)
La imagen se publica en `ghcr.io/placix5/telegarrm` y **no lleva ninguna clave**: las tuyas se quedan en archivos de tu equipo. La guía completa, con todas las opciones, está en [docs/instalacion-docker.md](docs/instalacion-docker.md).

1. Descarga [`deploy/docker/compose.yaml`](deploy/docker/compose.yaml) y [`deploy/docker/env.example`](deploy/docker/env.example) a una carpeta, por ejemplo `/opt/docker/telegarrm`, y renombra el segundo a `.env`. Ahí se cambian las rutas, el usuario y el puerto.
2. Crea las carpetas de datos, de claves y de la biblioteca, con tu usuario como dueño (aquí, el 1000; mira el tuyo con `id`):
   ```bash
   sudo mkdir -p /opt/docker/telegarrm/db /opt/docker/telegarrm/secretos /srv/media/descargas /srv/media/peliculas /srv/media/series
   sudo chown -R 1000:1000 /opt/docker/telegarrm /srv/media
   sudo chmod 700 /opt/docker/telegarrm/db /opt/docker/telegarrm/secretos
   ```
3. Guarda tus claves en esa carpeta, una por archivo y solo con el valor: `telegram_api_id`, `telegram_api_hash` y `tmdb_token` (este puede quedar vacío). Por ejemplo, con un editor:
   ```bash
   sudo -u '#1000' nano /opt/docker/telegarrm/secretos/telegram_api_hash
   ```
   Cuando estén los tres, deja que solo los pueda leer su dueño:
   ```bash
   sudo chmod 600 /opt/docker/telegarrm/secretos/*
   ```
4. Arranca Telegarrm desde la carpeta del compose:
   ```bash
   docker compose up -d
   ```
5. Abre `http://<ip-del-equipo>:8080` y sigue los [primeros pasos](#primeros-pasos).

Para actualizar: `docker compose pull && docker compose up -d`.

## Primeros pasos
1. **Crea la contraseña de la web.** La primera vez, Telegarrm pide un código de un solo uso que solo se puede leer en el propio equipo; así nadie de tu red puede adelantarse:
   ```bash
   sudo cat /opt/docker/telegarrm/db/codigo-acceso.txt
   ```
2. **Inicia sesión en Telegram** en la pestaña *Estado*: tu número, el código que te envía Telegram y, si la tienes, tu contraseña de verificación en dos pasos.
3. **Configura la biblioteca** en *Ajustes*: el búfer de descargas y las carpetas de películas y series (en el ejemplo, `/srv/media/descargas`, `/srv/media/peliculas` y `/srv/media/series`).
4. **Elige los canales** en *Canales*. El catálogo se rellena mientras se leen sus mensajes.

Cómo se usa cada pestaña: [docs/uso.md](docs/uso.md).

## Seguridad
- Tus claves de Telegram y TMDB nunca van en el repositorio ni en la imagen.
- La web pide contraseña y limita los intentos fallidos.
- La carpeta de datos contiene la sesión de tu cuenta de Telegram: quien la tenga puede usar tu cuenta. No la compartas ni la subas a ningún sitio.
- **Aún no hay HTTPS**: usa Telegarrm solo en tu red local o a través de una VPN, y no lo publiques en Internet.

Más detalles en [docs/configuracion.md](docs/configuracion.md).

## Uso responsable
Telegarrm descarga lo que se publica en los canales a los que tú tienes acceso. Asegúrate de que tienes derecho a descargar ese contenido según las leyes de tu país.

## Documentación
| | |
| --- | --- |
| [Uso](docs/uso.md) | Qué hace cada pestaña: catálogo, descargas, seguimiento, biblioteca |
| [Instalación con Docker](docs/instalacion-docker.md) | Paso a paso, claves, actualizar y mudar una instalación |
| [Instalación sin Docker](docs/instalacion-nativa.md) | Compilar en Debian o Raspberry Pi OS e instalarlo como servicio |
| [Configuración y seguridad](docs/configuracion.md) | Variables, ajustes, contraseña de la web y qué se guarda |
| [API](docs/api.md) | La API REST que usa la web |
| [Desarrollo](docs/desarrollo.md) | Estructura del código, compilación, tests y documentación técnica |
| [Historial de cambios](docs/historial.md) | Qué ha cambiado en cada versión |

## Licencia
[GNU GPL v3](LICENSE).

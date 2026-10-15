# Instalación con Docker

La forma recomendada de instalar Telegarrm. Solo hace falta un equipo Linux con Docker y Docker Compose, x86_64 o ARM64. El [README](../README.md) tiene un resumen rápido; aquí están todos los detalles.

## La imagen
`ghcr.io/placix5/telegarrm` se compila y publica automáticamente con cada cambio del repositorio, para x86_64 y ARM64, después de pasar los tests. Lleva el programa, TDLib (la librería oficial de Telegram), 7-Zip con soporte de RAR y ffprobe. **No lleva ninguna clave**: las tuyas se leen al arrancar de archivos de tu equipo (Docker secrets).

Etiquetas:
- `latest`: la última versión.
- `sha-<commit>`: una versión concreta, para no actualizar sin querer.

## Archivos
Todo está en [`deploy/docker/`](../deploy/docker/):
- `compose.yaml`: el servicio. Normalmente no hace falta tocarlo.
- `env.example`: rutas, usuario y puerto. Se copia como `.env` junto a `compose.yaml`.

| Variable de `.env` | Por defecto | Qué es |
| --- | --- | --- |
| `PUID`, `PGID` | `1000` | Usuario y grupo con el que corre Telegarrm. Debe ser el dueño de la biblioteca (y el mismo que use Jellyfin). Mira el tuyo con `id`. |
| `TELEGARRM_DATA` | `/opt/docker/telegarrm/db` | Datos de Telegarrm: base de datos, sesión de Telegram y contraseña de la web. |
| `TELEGARRM_SECRETS` | `/opt/docker/telegarrm/secretos` | Carpeta con los archivos de las claves. |
| `MEDIA_DIR` | `/srv/media` | Biblioteca y búfer de descargas. Se monta con la misma ruta dentro del contenedor. |
| `TELEGARRM_PORT` | `8080` | Puerto de la web en tu equipo. |
| `TELEGARRM_TAG` | `latest` | Versión de la imagen. |
| `TZ` | `Europe/Madrid` | Zona horaria. |

Jellyfin (o Plex, o Emby) no forma parte de este compose: va en el suyo y solo tiene que poder leer `MEDIA_DIR`.

## Paso a paso
Los ejemplos usan las rutas por defecto y el usuario 1000. Cámbialos si los tuyos son otros.

1. **Compose**: copia `compose.yaml` y `env.example` a una carpeta, por ejemplo `/opt/docker/telegarrm/`, renombra el segundo a `.env` y ajústalo.
2. **Carpetas**, del usuario que corre Telegarrm y solo accesibles por él las de datos y claves:
   ```bash
   sudo mkdir -p /opt/docker/telegarrm/db /opt/docker/telegarrm/secretos /srv/media/descargas /srv/media/peliculas /srv/media/series
   sudo chown -R 1000:1000 /opt/docker/telegarrm /srv/media
   sudo chmod 700 /opt/docker/telegarrm/db /opt/docker/telegarrm/secretos
   ```
   Créalas antes de arrancar: si falta una, Docker la crea como root y Telegarrm no podrá escribir en ella.
3. **Claves**: tres archivos en la carpeta de claves, cada uno solo con su valor, sin nada más:

   | Archivo | Valor |
   | --- | --- |
   | `telegram_api_id` | Tu `api_id` de <https://my.telegram.org> (un número) |
   | `telegram_api_hash` | Tu `api_hash` de <https://my.telegram.org> |
   | `tmdb_token` | Tu «API Read Access Token» de TMDB. Puede estar vacío, pero el archivo tiene que existir. |

   Lo más sencillo es crearlos con un editor, como el usuario que corre Telegarrm, y después dejar que solo él pueda leerlos:
   ```bash
   sudo -u '#1000' nano /opt/docker/telegarrm/secretos/telegram_api_id
   sudo -u '#1000' nano /opt/docker/telegarrm/secretos/telegram_api_hash
   sudo -u '#1000' touch /opt/docker/telegarrm/secretos/tmdb_token
   sudo chmod 600 /opt/docker/telegarrm/secretos/*
   ```
   Así las claves no quedan en el historial de la terminal. El contenedor las ve en `/run/secrets/`, de solo lectura, y no aparecen en sus variables de entorno ni con `docker inspect`.
4. **Arrancar**, desde la carpeta del compose:
   ```bash
   docker compose up -d
   docker compose logs -f
   ```
5. **Primer uso**: abre `http://<ip-del-equipo>:8080`. La web pide crear una contraseña con un código de un solo uso, que está en la carpeta de datos:
   ```bash
   sudo cat /opt/docker/telegarrm/db/codigo-acceso.txt
   ```
   Después inicia sesión en Telegram en *Estado*, indica las carpetas en *Ajustes* (las de tu equipo, como `/srv/media/series`: dentro del contenedor tienen la misma ruta) y elige los canales en *Canales*.

Si la imagen no se descarga porque el paquete no es público, inicia sesión una vez con un token de GitHub con permiso `read:packages`: `docker login ghcr.io`.

## Mantenimiento
- **Actualizar**: `docker compose pull && docker compose up -d`. Los datos se conservan en `TELEGARRM_DATA`.
- **Registro**: `docker compose logs -f`.
- **Restablecer la contraseña de la web** (si se olvida): deja un código nuevo en `codigo-acceso.txt` y la web pide crear otra.
  ```bash
  docker compose exec telegarrm /app/telegarrm --reset-password
  ```
- **Copia de seguridad**: basta con la carpeta `TELEGARRM_DATA`, mejor con el contenedor parado. Guárdala con cuidado: contiene la sesión de tu cuenta de Telegram.

## Cómo está protegido el contenedor
- Corre con el usuario de `PUID`, no como root.
- Su sistema de archivos es de solo lectura, salvo los datos, la biblioteca y `/tmp` (en memoria).
- No tiene privilegios del sistema ni puede ganarlos (`cap_drop: ALL`, `no-new-privileges`).
- Se reinicia solo si se cae, y también cuando se pide «Reiniciar ahora» en *Ajustes*.
- Al pararlo, cierra la conexión con Telegram de forma ordenada (tiene 30 s).

## Mudar una instalación
La carpeta de datos contiene la base de datos, la contraseña de la web y la sesión de Telegram. Copiándola, la instalación nueva sigue exactamente donde estaba la antigua, sin volver a iniciar sesión. Sirve también para pasar de una instalación [sin Docker](instalacion-nativa.md), cuya carpeta de datos es `db/`, dentro de la del programa.

1. **Para la instalación antigua y no la vuelvas a arrancar.** Dos instalaciones con la misma sesión de Telegram a la vez se estorban. Si era un servicio:
   ```bash
   sudo systemctl disable --now telegarrm
   ```
2. **Copia la carpeta de datos** directamente al equipo nuevo, por ejemplo con `scp`, y dásela al usuario de Telegarrm:
   ```bash
   scp -r ~/Telegarrm/db/* equipo-nuevo:/opt/docker/telegarrm/db/
   ```
   ```bash
   sudo chown -R 1000:1000 /opt/docker/telegarrm/db && sudo chmod 700 /opt/docker/telegarrm/db
   ```
   La sesión de Telegram da acceso completo a tu cuenta: no la copies a ningún otro sitio ni la dejes a la vista.
3. **Arranca** con `docker compose up -d`. Si las carpetas de la biblioteca cambian de ruta, ajústalas en *Ajustes*.

La imagen lleva la misma versión de TDLib que la instalación sin Docker que describe esta documentación (1.8.67), así que la sesión sigue valiendo. Si no fuera así, la web pediría iniciar sesión otra vez.

## Sin HTTPS
La web aún no tiene HTTPS: la contraseña y el inicio de sesión de Telegram viajan sin cifrar. Úsala solo en tu red local o a través de una VPN. Si la pones detrás de un proxy inverso con HTTPS (Nginx Proxy Manager, Caddy, Traefik…) que envíe la cabecera `X-Forwarded-Proto: https`, la cookie de sesión se marca como segura.

# Instalación con Docker

La forma recomendada de instalar Telegarrm. Solo hace falta un equipo Linux con Docker y Docker Compose, x86_64 o ARM64. El [README](../README.md) tiene un resumen rápido; aquí están todos los detalles.

## La imagen
`ghcr.io/placix5/telegarrm` se compila y publica automáticamente con cada cambio del repositorio, para x86_64 y ARM64, después de pasar los tests. Lleva el programa, TDLib (la librería oficial de Telegram), 7-Zip con soporte de RAR y ffprobe. **No lleva ninguna clave**: las tuyas le llegan al arrancar, desde tu compose.

Etiquetas:
- `latest`: la última versión.
- `sha-<commit>`: una versión concreta, para no actualizar sin querer.

## Dos formas de instalarlo
Las dos están en [`deploy/docker/`](../deploy/docker/) y funcionan igual; cambia dónde están las claves y los datos.

| | [Sencilla](#forma-sencilla) | [Con Docker secrets](#con-docker-secrets) |
| --- | --- | --- |
| Archivos | Un compose | Compose, `.env` y tres archivos de claves |
| Claves | En el compose | En archivos aparte, con permisos 600 |
| ¿Se ven con `docker inspect`? | Sí | No |
| Datos de Telegarrm | Volumen de Docker, que se crea solo | Carpeta del servidor, que creas tú |

En las dos, Jellyfin (o Plex, o Emby) va en su propio compose y solo tiene que poder leer la biblioteca.

**¿Cuál elegir?** La sencilla basta para la mayoría: `docker inspect` solo lo puede usar quien ya controla Docker en tu equipo, y eso ya le daría acceso a todo. Lo que hay que cuidar es no compartir el compose con las claves dentro, por ejemplo al pedir ayuda en un foro. La de Docker secrets es para quien prefiere que las claves no estén en ningún archivo que se copie o se enseñe.

## Forma sencilla
Los ejemplos usan `/srv/media` como biblioteca y el usuario 1000. Cámbialos si los tuyos son otros.

1. **Biblioteca**: tiene que existir y ser del usuario con el que corre Telegarrm. Si no existe, Docker la crearía como root y Telegarrm no podría escribir en ella.
   ```bash
   sudo mkdir -p /srv/media/descargas /srv/media/peliculas /srv/media/series
   sudo chown -R 1000:1000 /srv/media
   ```
2. **Compose**: copia [`deploy/docker/compose.yaml`](../deploy/docker/compose.yaml) a una carpeta de tu equipo, por ejemplo `/opt/docker/telegarrm/`, y edítalo:

   | Dónde | Qué poner |
   | --- | --- |
   | `TELEGARRM_API_ID`, `TELEGARRM_API_HASH` | Tus claves de <https://my.telegram.org>, en «API development tools» |
   | `TELEGARRM_TMDB_TOKEN` | Opcional: tu «API Read Access Token» de [TMDB](https://www.themoviedb.org/settings/api), para portadas, sinopsis y títulos de episodio |
   | `user` | Tu usuario y grupo, el dueño de la biblioteca. Mira los tuyos con `id`. |
   | `ports` | El primer número es el puerto de la web en tu equipo |
   | `volumes` | Cambia las dos rutas `/srv/media` si tu biblioteca está en otro sitio. Deben ser iguales: así las rutas de *Ajustes* son las mismas dentro y fuera del contenedor. |

   No lo copies dentro de una copia del repositorio de Telegarrm: tus claves podrían acabar en git.
3. **Arrancar**, desde la carpeta del compose:
   ```bash
   docker compose up -d
   docker compose logs -f
   ```
   Si falta una clave, el registro lo dice: ponla en el compose y repite `docker compose up -d`.
4. **Primer uso**: abre `http://<ip-del-equipo>:8080`. La web pide crear una contraseña con un código de un solo uso. Para leerlo, desde la carpeta del compose:
   ```bash
   docker compose exec telegarrm cat db/codigo-acceso.txt
   ```
   Después inicia sesión en Telegram en *Estado*, indica las carpetas en *Ajustes* (las de tu equipo, como `/srv/media/series`) y elige los canales en *Canales*.

Los datos de Telegarrm (base de datos, sesión de Telegram y contraseña de la web) quedan en el volumen `telegarrm_datos`, que Docker crea la primera vez con el usuario 1000 como dueño y solo accesible por él. Se conserva al actualizar o recrear el contenedor; solo se borra con `docker compose down -v` o `docker volume rm`.

Si usas otro usuario que no sea el 1000, el volumen nuevo no sería suyo: usa una carpeta, como en [la otra forma](#con-docker-secrets) (`/opt/docker/telegarrm/db:/app/db`, creada antes y de ese usuario).

## Con Docker secrets
Las claves van en archivos del servidor con permisos 600, que el contenedor ve en `/run/secrets/`, de solo lectura. No están en el compose, ni en las variables de entorno del contenedor, ni salen con `docker inspect`. Los archivos están en [`deploy/docker/con-secrets/`](../deploy/docker/con-secrets/):
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

Los ejemplos usan las rutas por defecto y el usuario 1000.

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
   Así las claves no quedan en el historial de la terminal.
4. **Arrancar y primer uso**: como en la forma sencilla, desde la carpeta del compose (`docker compose up -d`). El código de la contraseña también está en `/opt/docker/telegarrm/db/codigo-acceso.txt`.

## Mantenimiento
Todos los comandos, desde la carpeta del compose.
- **Actualizar**: `docker compose pull && docker compose up -d`. Los datos se conservan.
- **Registro**: `docker compose logs -f`.
- **Restablecer la contraseña de la web** (si se olvida): deja un código nuevo en `codigo-acceso.txt` y la web pide crear otra.
  ```bash
  docker compose exec telegarrm /app/telegarrm --reset-password
  ```
- **Copia de seguridad**, mejor con el contenedor parado. Guárdala con cuidado: contiene la sesión de tu cuenta de Telegram.
  - Con el volumen (forma sencilla): `docker compose stop`, después `docker compose cp telegarrm:/app/db ./copia-telegarrm` y `docker compose start`.
  - Con una carpeta: basta con copiar la carpeta de datos.

## Mudar una instalación
Los datos de Telegarrm contienen la base de datos, la contraseña de la web y la sesión de Telegram. Copiándolos, la instalación nueva sigue exactamente donde estaba la antigua, sin volver a iniciar sesión. Sirve también para pasar de una instalación [sin Docker](instalacion-nativa.md), cuya carpeta de datos es `db/`, dentro de la del programa.

1. **Para la instalación antigua y no la vuelvas a arrancar.** Dos instalaciones con la misma sesión de Telegram a la vez se estorban. Si era un servicio:
   ```bash
   sudo systemctl disable --now telegarrm
   ```
2. **Copia los datos** directamente al equipo nuevo, por ejemplo con `scp`, a una carpeta solo tuya (aquí, `~/copia-telegarrm`):
   ```bash
   scp -r ~/Telegarrm/db equipo-nuevo:copia-telegarrm
   ```
   La sesión de Telegram da acceso completo a tu cuenta: no la copies a ningún otro sitio ni la dejes a la vista.
3. **Ponlos en su sitio** en el equipo nuevo, desde la carpeta del compose:
   - Con el volumen (forma sencilla): crea el contenedor sin arrancarlo y copia los datos dentro. `-a` conserva el dueño de los archivos, que debe ser el usuario de Telegarrm.
     ```bash
     docker compose create
     docker compose cp -a ~/copia-telegarrm/. telegarrm:/app/db/
     ```
   - Con una carpeta: muévelos a ella y dásela al usuario de Telegarrm.
     ```bash
     sudo mv ~/copia-telegarrm/* /opt/docker/telegarrm/db/
     sudo chown -R 1000:1000 /opt/docker/telegarrm/db && sudo chmod 700 /opt/docker/telegarrm/db
     ```
4. **Arranca** con `docker compose up -d` y borra la copia (`~/copia-telegarrm`). Si las carpetas de la biblioteca cambian de ruta, ajústalas en *Ajustes*.

La imagen lleva la misma versión de TDLib que la instalación sin Docker que describe esta documentación (1.8.67), así que la sesión sigue valiendo. Si no fuera así, la web pediría iniciar sesión otra vez.

## Cómo está protegido el contenedor
- Corre con tu usuario, no como root.
- Su sistema de archivos es de solo lectura, salvo los datos, la biblioteca y `/tmp` (en memoria).
- No tiene privilegios del sistema ni puede ganarlos (`cap_drop: ALL`, `no-new-privileges`).
- Se reinicia solo si se cae, y también cuando se pide «Reiniciar ahora» en *Ajustes*.
- Al pararlo, cierra la conexión con Telegram de forma ordenada (tiene 30 s).

Si la imagen no se descarga porque el paquete no es público, inicia sesión una vez con un token de GitHub con permiso `read:packages`: `docker login ghcr.io`.

## Sin HTTPS
La web aún no tiene HTTPS: la contraseña y el inicio de sesión de Telegram viajan sin cifrar. Úsala solo en tu red local o a través de una VPN. Si la pones detrás de un proxy inverso con HTTPS (Nginx Proxy Manager, Caddy, Traefik…) que envíe la cabecera `X-Forwarded-Proto: https`, la cookie de sesión se marca como segura.

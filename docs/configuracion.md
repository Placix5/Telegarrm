# Configuración y seguridad

## Claves
Telegarrm necesita las claves de la API de Telegram y, opcionalmente, un token de TMDB. Se le dan **solo** por variables de entorno o por archivos; nunca se guardan en el repositorio, en la imagen de Docker ni en su base de datos.

| Variable | Obligatoria | Qué es |
| --- | --- | --- |
| `TELEGARRM_API_ID` | Sí | `api_id` de tu aplicación en <https://my.telegram.org> («API development tools»), un número |
| `TELEGARRM_API_HASH` | Sí | `api_hash` de esa aplicación |
| `TELEGARRM_TMDB_TOKEN` | No | «API Read Access Token» de [TMDB](https://www.themoviedb.org/settings/api), para portadas, sinopsis y títulos de episodio. Sin él, el catálogo usa solo lo que dicen las fichas de Telegram. |
| `TELEGARRM_PORT` | No | Puerto de la web (8080 si no se indica) |

Cada clave se puede dar también como archivo: `TELEGARRM_API_ID_FILE`, `TELEGARRM_API_HASH_FILE` y `TELEGARRM_TMDB_TOKEN_FILE`, con la ruta de un archivo que solo contenga el valor. Es lo que usa Docker (Docker secrets). No pueden estar a la vez una variable y su `_FILE`, y un archivo vacío cuenta como «sin configurar».

## Ajustes
El resto se configura en la pestaña *Ajustes* de la web (ver [Uso](uso.md#ajustes)): el búfer de descargas, las bibliotecas de películas y series, el espacio libre mínimo y si se conservan las versiones sustituidas.

Estructura recomendada, todo en el mismo disco para que pasar lo descargado a la biblioteca sea instantáneo:
```
/srv/media/
├── descargas/   ← búfer de descargas
├── peliculas/   ← biblioteca de películas
└── series/      ← biblioteca de series
```
Jellyfin, Plex o Emby solo necesitan leer `peliculas/` y `series/`.

## Contraseña de la web
La web y toda la API piden contraseña. Hay un solo usuario y una sola contraseña.

### Primera vez
Mientras no haya contraseña, la web solo permite crearla, y para eso pide un código de un solo uso. Así nadie de tu red puede adelantarse a crearla: hay que tener acceso al equipo donde corre Telegarrm.
- El código está en el archivo `codigo-acceso.txt` de la carpeta de datos (`db/codigo-acceso.txt` sin Docker; en Docker, dentro de `TELEGARRM_DATA`), con permisos 600.
- Se escribe con o sin guion, en mayúsculas o minúsculas.
- La contraseña debe tener al menos 10 caracteres.
- El código vale hasta que se crea la contraseña, aunque Telegarrm se reinicie entretanto. Al crearla, el archivo se borra.
- El código nunca aparece en el registro de Telegarrm, solo en el archivo.

### Sesiones
- Al entrar, el navegador guarda una cookie de sesión que dura 30 días desde la última vez que se usa y sigue valiendo aunque Telegarrm se reinicie.
- Cada navegador o dispositivo tiene su propia sesión. «Salir», en la cabecera, la cierra.
- Cambiar la contraseña (en *Ajustes*) cierra las sesiones de los demás navegadores y dispositivos.

### Intentos fallidos
Tras 5 intentos fallidos desde un mismo dispositivo hay que esperar 30 segundos, y la espera se dobla con cada nuevo fallo, hasta 15 minutos. Tras 20 fallos en total, vengan de donde vengan, la espera es para todos. Los fallos se olvidan pasada una hora.

### Si se olvida
Este comando borra la contraseña y todas las sesiones, y deja un código nuevo en `codigo-acceso.txt`. No toca nada más: ni la sesión de Telegram, ni los canales, ni las descargas, ni los ajustes. No hace falta parar Telegarrm.
- Con Docker, desde la carpeta del compose:
  ```bash
  docker compose exec telegarrm /app/telegarrm --reset-password
  ```
- Sin Docker, desde la carpeta del programa y con el usuario del servicio:
  ```bash
  ./build/telegarrm --reset-password
  ```

## Qué guarda Telegarrm
Todo está en su carpeta de datos (`db/` sin Docker; `TELEGARRM_DATA` en Docker):

| | Qué es |
| --- | --- |
| `telegarrm.db` | Base de datos SQLite: canales, mensajes, catálogo de TMDB, descargas, seguimiento, ajustes, la contraseña de la web (solo su resultado con scrypt, nunca en claro) y las sesiones (solo la huella SHA-256 de cada una) |
| `tdlib/` | La sesión de Telegram. **Da acceso completo a tu cuenta**: no la copies a ningún sitio salvo para mudar la instalación, y nunca la compartas |
| `tmdb/` | Carátulas de TMDB descargadas |
| `codigo-acceso.txt` | El código para crear la contraseña, solo mientras no exista |

Las claves de Telegram y TMDB no se guardan aquí: llegan cada vez que arranca, por variables o archivos.

## Seguridad
- **Claves**: nunca en el repositorio, la imagen ni la base de datos. En Docker, tampoco en las variables de entorno del contenedor.
- **Web**: contraseña con scrypt, sesiones con cookie que el JavaScript no puede leer y que otras webs no pueden usar (`HttpOnly`, `SameSite=Strict`), límite de intentos, y rechazo de las peticiones que cambian algo enviadas desde otra web.
- **Archivos**: Telegarrm solo borra dentro de las carpetas de la biblioteca, nunca a través de enlaces, y solo lo que él mismo colocó, las versiones sustituidas o lo que pidas borrar desde la pestaña *Biblioteca*.
- **Programas externos** (7-Zip, ffprobe): se lanzan sin pasar por una terminal, con cada argumento por separado, para que un nombre de archivo raro no pueda ejecutar nada.
- **Sin HTTPS**: la contraseña de la web y el inicio de sesión de Telegram viajan sin cifrar. Usa Telegarrm solo en tu red local o a través de una VPN, y no lo publiques en Internet. Detrás de un proxy inverso con HTTPS que envíe `X-Forwarded-Proto: https`, la cookie de sesión se marca como segura.

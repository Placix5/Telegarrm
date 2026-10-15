# API

La web de Telegarrm usa una API REST en JSON, en el mismo puerto (`http://<ip-del-equipo>:8080/api/...`). Sirve también para automatizar cosas por tu cuenta. Los números D-0XX enlazan a la [decisión](DECISIONS.md) que explica cada parte.

## Autenticación
Toda la API, salvo `/api/auth/`, necesita la cookie de sesión `telegarrm_session`, que se obtiene con `POST /api/auth/login`. Sin ella responde 401 ([D-053](DECISIONS.md)). Las peticiones que cambian algo y llegan con una cabecera `Origin` de otra web se rechazan con 403.

Ejemplo con curl:
```bash
curl -c cookies.txt -X POST http://localhost:8080/api/auth/login -d '{"password": "tu contraseña"}'
curl -b cookies.txt http://localhost:8080/api/status
```

Los errores se devuelven como `{"error": "..."}` con el código HTTP correspondiente.

## Rutas
| Método y ruta | Descripción |
| --- | --- |
| `GET /api/auth/status` | `{"configured", "authenticated"}`: si hay contraseña y si esta sesión es válida |
| `POST /api/auth/setup` | `{"code", "password"}`: crea la primera contraseña con el código de `db/codigo-acceso.txt` y abre sesión |
| `POST /api/auth/login` | `{"password"}`: abre sesión. 401 si no es la contraseña; 429 con `Retry-After` tras varios fallos |
| `POST /api/auth/logout` | Cierra la sesión |
| `POST /api/auth/password` | `{"current", "password"}`: cambia la contraseña y cierra las demás sesiones |
| `GET /api/status` | Estado del servicio, de la BD (`version` leída de `settings`), de Telegram, de TMDB (`metadata`: obras encontradas y sin encontrar) y la última novedad del catálogo (`events.last_id`). HTTP 503 si la BD falla. |
| `GET /api/events?after={id}` | Novedades del catálogo posteriores a `id`: archivos nuevos de cada obra recién publicados en Telegram ([D-043](DECISIONS.md)). La web las usa para avisar a quien está viendo esa obra |
| `POST /api/telegram/auth/phone` | `{"phone_number": "+34..."}` |
| `POST /api/telegram/auth/code` | `{"code": "12345"}` |
| `POST /api/telegram/auth/password` | `{"password": "..."}` (verificación en dos pasos) |
| `GET /api/telegram/chats` | Canales y grupos de la cuenta (incluidos los archivados), con `type` y `role`, para elegir cuáles vigilar |
| `GET /api/channels` | Canales vigilados y estado de su sincronización |
| `POST /api/channels` | `{"chat_id": -100...}`: vigilar un canal o grupo; empieza a sincronizarse al momento |
| `DELETE /api/channels/{id}` | Dejar de vigilarlo (borra sus mensajes guardados, no los de Telegram) |
| `POST /api/channels/{id}/sync` | Buscar mensajes nuevos ya. Normalmente no hace falta: llegan al momento ([D-036](DECISIONS.md)) y, además, hay una ronda cada 15 min |
| `GET /api/channels/{id}/messages?limit=50&offset=0` | Mensajes guardados, del más reciente al más antiguo (con su `topic_id`) |
| `GET /api/channels/{id}/topics` | Temas de un grupo con temas, con cuántos mensajes tiene cada uno |
| `GET /api/catalog` | Obras (series y películas) de todos los canales: título, títulos alternativos, año, versiones disponibles (`qualities`, `hdr`), idiomas, géneros, temas, `airing` (en emisión), `followed` (en seguimiento), temporadas, episodios, tamaño en bytes y datos de TMDB. Las que tienen la misma ficha de TMDB van juntas, con la de cada canal en `sources` |
| `GET /api/catalog/{chat}/{ficha}` | Obra completa: sinopsis, ficha original, seguimiento (`follow`), la misma obra en otros canales (`sources`) y sus archivos lógicos (`releases`) con calidad, HDR, etiquetas de versión, temporada y episodio, y sus partes. Vale cualquier ficha de la obra |
| `POST /api/catalog/{chat}/{ficha}/download` | `{"max_quality": ""}`: en una serie, pone en cola los episodios que faltan (la mejor versión de cada uno). Si no se tiene ninguno, la serie completa. 409 si no cabe en la biblioteca. La ficha de una serie trae el resumen en `library` |
| `POST /api/releases/{chat}/{mensaje}/probe` | Calidad real de un archivo del catálogo leyendo solo sus primeros MB ([D-042](DECISIONS.md)): `{"quality", "hdr"}`, o 409 con el motivo. La ficha trae el resultado en `releases[].probe` y lo que ya hay en la biblioteca en `on_disk` |
| `GET /api/catalog/{chat}/{ficha}/poster` | Portada: la foto de la ficha (de Telegram) o, si no hay, la carátula de TMDB; se guardan tras la primera vez |
| `GET /api/downloads` | Cola de descargas con su progreso (`downloaded_size`, `bytes_per_second`, `import_percent`, `library_path`; `status`: queued, downloading, importing, completed —en la biblioteca—, failed, cancelled, replaced —sustituida por una versión mejor—, deleted —borrada de la biblioteca—), su origen (`origin`: manual o auto) y las descargas a las que sustituye (`replaces`) |
| `POST /api/downloads` | `{"chat_id": -100..., "message_id": ...}` (cualquier parte de un archivo del catálogo): 201, o 409 si ya está en la cola o descargado |
| `POST /api/downloads/{id}/cancel` | Cancelar (borra lo descargado a medias) |
| `POST /api/downloads/{id}/retry` | Reintentar una descarga fallida o cancelada |
| `DELETE /api/downloads/{id}` | Quitar de la lista una descarga terminada, sustituida, borrada, fallida o cancelada (los archivos de la biblioteca no se tocan) |
| `GET /api/library` | Lo que hay en las bibliotecas ([D-052](DECISIONS.md)): por cada carpeta, título, año, `tmdb_id`, tamaño, archivos, temporadas con sus episodios y su ficha del catálogo (`item`) si la tiene |
| `GET /api/library/files?kind=series\|movie&folder=...` | Archivos de una carpeta de la biblioteca, con su ruta relativa, tamaño, episodio y calidad según el nombre |
| `POST /api/library/delete` | `{"kind": "series", "folder": "...", "paths": ["Season 01"]}` (`[""]` = la carpeta entera): borra de la biblioteca, solo archivos normales dentro de ella ([D-052](DECISIONS.md)). Devuelve `{"files", "bytes", "downloads"}` |
| `POST /api/library/scan` | Revisar la biblioteca ([D-051](DECISIONS.md)): `{"checked", "moved", "missing": [{"id", "description"}]}`. Lo movido se apunta; lo que ya no está pasa a `deleted`. 503 mientras el catálogo se calcula al arrancar |
| `GET /api/follows` | Obras seguidas: calidad máxima (`max_quality`), desde cuándo (`created_at`) y su ficha actual en el catálogo (`found` = sigue en él) |
| `POST /api/follows` | `{"chat_id": -100..., "anchor_id": ..., "max_quality": ""}` (`""` = la mejor, `"1080p"` o `"720p"`): seguir una obra; 409 si ya se sigue |
| `PUT /api/follows/{id}` | `{"max_quality": "1080p"}`: cambiar la calidad máxima |
| `DELETE /api/follows/{id}` | Dejar de seguir (lo descargado se queda) |
| `GET /api/activity?before={id}` | Historial de actividad, del más reciente al más antiguo, de 100 en 100, con enlace a la obra (`item`) |
| `GET /api/settings` | Ajustes, con la comprobación de cada ruta (escritura, espacio libre), si búfer y bibliotecas comparten disco y si hace falta reiniciar |
| `PUT /api/settings` | `{"download_dir", "movies_dir", "series_dir", "min_free_gb", "keep_replaced"}`: valida antes de guardar (400 con el motivo por campo) |
| `POST /api/restart` | Parada ordenada y reinicio (sale con el código 75; systemd o Docker lo vuelven a arrancar) |

Ejemplo de `/api/status`:
```json
{"status": "Telegarrm is running", "version": "0.1.0",
 "database": {"status": "ok", "version": "0.1.0"},
 "telegram": {"authorization_state": "authorizationStateReady", "connection_state": "connectionStateReady"},
 "metadata": {"enabled": true, "working": false, "total": 2227, "matched": 1973, "unmatched": 254},
 "events": {"last_id": 0}}
```
Los pasos del inicio de sesión en Telegram (`/api/telegram/auth/...`) responden `{"ok": true}`, o `{"error": "..."}` con HTTP 400 (dato incorrecto, ej. `PHONE_CODE_INVALID`), 409 (Telegram no espera ese dato ahora) o 504 (Telegram no responde).

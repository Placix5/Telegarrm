# Hoja de ruta de Telegarrm

Documento vivo con el plan de implementación; los motivos de cada decisión están en [DECISIONS.md](DECISIONS.md). Lo mantiene Claude Code (desarrollo y plan técnico); Gemini revisa el código y la documentación; Plácido decide prioridades y valida en uso real.

## Objetivo
Un servicio tipo *stack ARR* (Sonarr/Radarr) que corre siempre en la Raspberry Pi y usa uno o dos canales de Telegram como única fuente:
1. **Catálogo**: mostrar en una web las series y películas publicadas en los canales elegidos.
2. **Descarga**: botón "Almacenar en disco" que descarga, descomprime, renombra y coloca los ficheros en la biblioteca sin intervención.
3. **Seguimiento**: vigilar los mensajes nuevos, descargar los episodios nuevos de lo que se sigue y, si llega una versión mejor (resolución, HDR, REMUX), reemplazarla.

Todo se gestiona desde el navegador, sin SSH ni terminal.

## Principios
- **Un único binario C++17**: servidor HTTP (`cpp-httplib`), TDLib (interfaz JSON) en su propio hilo y SQLite. La web es estática y la sirve el propio binario.
- **Secretos solo en variables de entorno** (`TELEGARRM_API_ID`, `TELEGARRM_API_HASH`), nunca en git ni en la BD. La sesión de TDLib vive en `db/tdlib/` con permisos `0700`.
- **Nada de comandos de shell montados con cadenas**: el núcleo antiguo usa `popen`/`system` con rutas y tokens concatenados (riesgo de inyección con nombres de fichero raros). Al migrarlo se usará `posix_spawn` con argumentos separados y el cliente HTTP de `cpp-httplib`.
- **Localización**: unidades del Sistema Internacional (tamaños en kB/MB/GB y velocidades en MB/s, base 1000), fechas `dd/mm/aaaa` y hora `Europe/Madrid`.
- **Datos personales** (teléfono, sesión) solo en local; sin telemetría.
- Hitos pequeños: cada uno compila, se prueba en la Pi, se commitea y se sube a GitHub.

## Fase 1: Motor TDLib (completada)
- [x] SQLite (`DbManager`), hilos y parada ordenada con SIGINT/SIGTERM.
- [x] TDLib 1.8 (interfaz JSON) en un hilo propio, con peticiones asíncronas y síncronas.
- [x] Inicio de sesión desde la web: teléfono, código y contraseña 2FA.
- [x] Primer inicio de sesión real con la cuenta de Plácido.
- [x] Servicio de systemd: primero de usuario y después de sistema, aislado y con regla de polkit (`deploy/`, D-012 y D-013). Falta que Plácido lo instale con `sudo`.

## Fase 2: Canales y catálogo (completada)
- [x] **Canales**: listar los chats de la cuenta (`getChats`, incluidos archivados) para elegir desde la web cuáles vigilar (tabla `channels`).
- [x] **Sincronización**: recorrer el historial (`getChatHistory` paginado, respetando los `FLOOD_WAIT` de Telegram) y guardar los mensajes con texto o fichero: id, fecha, álbum, texto, nombre, tamaño y tipo MIME. Reanudable; mensajes nuevos cada 15 min.
- [x] **Parser**: adaptar `episode_parser` del proyecto antiguo (SxxEyy, 1x01, calidades, códecs) con tests unitarios (`ctest`) construidos con ejemplos reales del canal. Tabla `media`: tipo, título, año, temporada, episodio, calidad, códec, idioma.
- [x] **Catálogo**: en memoria, con las fichas como separador (D-020, D-021). El diseño cambió respecto al plan inicial: sin tabla `media`.
- [x] **API**: `GET /api/catalog`, `GET /api/catalog/{chat}/{ficha}` y la portada.
- [x] **Web**: catálogo en tarjetas con portada, búsqueda y filtro por tipo; ficha con temporadas y episodios.
- [x] Canal grande de Plácido (grupo con temas, ~33 000 mensajes): temas (D-025), obras con versiones y partes (D-026), asignación de archivos a fichas (D-027) y rendimiento (D-028).
- [x] **Metadatos de TMDB** (D-029, D-030): títulos de episodio, sinopsis, géneros, carátulas e identificadores externos, en caché local.
- [x] Créditos de TMDB en la web (requisito de su licencia).
- [ ] Usar TMDB para separar *remakes* que el catálogo une (ej. *Vaiana* de 2016 y de 2026).

Formato real visto: canales de una serie con una ficha (foto + pie con título, año, calidad, géneros e idioma), portadas de temporada sin pie, episodios `1x01 - Serie.mkv` o `Serie #01x01 - Título.mp4` y textos de cierre.

## Fase 3: Descargas (completada)
- [x] **Rutas** de la biblioteca y del búfer configurables desde la web (`settings`).
- [x] **Elegir versión**: la web ofrece las versiones de cada película o episodio (4K HDR, 1080p, REMUX…) y, en las series, la temporada completa en una versión.
- [x] **Cola persistente** (tablas `downloads` y `download_parts`) con un trabajador: progreso, reintentos, reanudación tras reiniciar, cancelación y comprobación de espacio libre (D-031). Los archivos quedan en la caché de TDLib.
- [x] **Ajustes** en la web: búfer de descargas de TDLib, bibliotecas y espacio libre mínimo (D-032).
- [x] **Biblioteca**: `/srv/media/{descargas,peliculas,series}` (D-033); en el servidor, el RAID montado en `/srv/media`.
- [x] **Importación** (D-034): descompresión con 7-Zip sin shell (zip troceado, multiparte RAR, 7z), vídeos sin muestras, subtítulos, nombres para Jellyfin (`Título (Año) [tmdbid-N]`, `Season 01`) y limpieza del búfer. Comprobado con descargas reales en ZIP y RAR multiparte (el RAR necesita el paquete `7zip-rar`).
- [x] **API**: `POST /api/downloads`, `GET /api/downloads` (progreso), cancelar, reintentar y quitar.
- [x] **Web**: botón "Almacenar en disco" y pestaña *Descargas* con el progreso.

## Fase 4: Seguimiento (Tele-ARR)
- [x] **Seguir** series y películas desde su ficha, con calidad máxima (la mejor, hasta 1080p, hasta 720p). Tabla `follows`; la obra se reencuentra aunque cambie su ficha principal (D-035).
- [x] **Tiempo real** (D-036): `openChat` en los canales vigilados y escucha de `updateNewMessage`. Se agrupan los avisos (20 s de calma, 2 min como mucho) y se traen solo los mensajes nuevos. Después, el catálogo los analiza y el seguimiento los evalúa. La ronda de cada 15 min recupera lo perdido.
- [x] **Auto-descarga**: episodios nuevos de las series seguidas y la primera versión que se publique de una película seguida. Solo lo publicado después de seguir. Espera a que estén todas las partes de un comprimido.
- [x] **Mejoras**: una versión con más resolución, HDR o REMUX sustituye a la que se tiene. La anterior se borra solo cuando la nueva ya está en la biblioteca, o se conserva si así se elige en *Ajustes* (D-037).
- [x] **Historial de actividad** (tabla `activity`) y pestaña *Actividad* con lo que se sigue. Filtro «En seguimiento» en el catálogo.
- [x] Comprobado en la Pi con un episodio nuevo (*Presidente Curtis* 1x09) y con una mejora de 1080p a 4K HDR (*El show de los Muppets*).
- [x] Tiempo real comprobado con el canal de prueba de Plácido. Se subió el 1x02 de *Ultimate Spider-Man* a una serie seguida: aviso al instante, lectura 20 s después, catálogo y seguimiento al momento, y el episodio en la biblioteca unos 10 s más tarde (D-036, D-038).
- [x] **Calidad real del vídeo** al importar, con `ffprobe` (D-039): resolución y HDR del propio archivo, que mandan sobre el nombre y la ficha. Las descargas anteriores se corrigieron al arrancar: los dos episodios de *Ultimate Spider-Man* eran 720p, no 1080p ni «calidad desconocida».
- [x] **Episodios que faltan / serie completa** (D-040): en la ficha de una serie, cuántos episodios conocidos se tienen y un botón para descargar los que faltan (o la serie entera), con la mejor versión de cada uno dentro de la calidad elegida. Comprueba antes que caben en la biblioteca.
- Ideas para después: notificaciones (Telegram o correo) de lo descargado; vigilar ediciones y borrados de mensajes (`updateMessageContent`, `updateDeleteMessages`); seguir una obra antes de que esté en el catálogo.

## Transversal (antes de exponer la web fuera de la red local)
- **Autenticación** en la web y la API: ahora cualquiera en la LAN puede usarla.
- **HTTPS** mediante proxy inverso: el login de Telegram y la contraseña 2FA viajan hoy en claro por HTTP.
- Tests (`ctest`) del parser y de la BD.
- Copia de seguridad de `db/`.

# Hoja de ruta de Telegarrm

Documento vivo con el plan de implementación; los motivos de cada decisión están en [DECISIONS.md](DECISIONS.md). Lo mantiene Claude Code (desarrollo y plan técnico); Gemini revisa el código y la documentación; Plácido decide prioridades y valida en uso real.

## Objetivo
Un servicio tipo *stack ARR* (Sonarr/Radarr) que corre siempre en la Raspberry Pi y usa uno o dos canales de Telegram como única fuente:
1. **Catálogo**: mostrar en una web las series y películas publicadas en los canales elegidos.
2. **Descarga**: botón "Almacenar en disco" que descarga, descomprime, renombra y coloca los ficheros en la biblioteca sin intervención.
3. **Seguimiento**: vigilar los mensajes nuevos y, si llega una versión mejor (audio, códecs) de algo en seguimiento, reemplazarla.

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

## Fase 2: Canales y catálogo (en curso)
- [x] **Canales**: listar los chats de la cuenta (`getChats`, incluidos archivados) para elegir desde la web cuáles vigilar (tabla `channels`).
- [x] **Sincronización**: recorrer el historial (`getChatHistory` paginado, respetando los `FLOOD_WAIT` de Telegram) y guardar los mensajes con texto o fichero: id, fecha, álbum, texto, nombre, tamaño y tipo MIME. Reanudable; mensajes nuevos cada 15 min.
- [x] **Parser**: adaptar `episode_parser` del proyecto antiguo (SxxEyy, 1x01, calidades, códecs) con tests unitarios (`ctest`) construidos con ejemplos reales del canal. Tabla `media`: tipo, título, año, temporada, episodio, calidad, códec, idioma.
- [x] **Catálogo**: en memoria, con las fichas como separador (D-020, D-021). El diseño cambió respecto al plan inicial: sin tabla `media`.
- [x] **API**: `GET /api/catalog`, `GET /api/catalog/{chat}/{ficha}` y la portada.
- [x] **Web**: catálogo en tarjetas con portada, búsqueda y filtro por tipo; ficha con temporadas y episodios.
- [x] Canal grande de Plácido (grupo con temas, ~33 000 mensajes): temas (D-025), obras con versiones y partes (D-026), asignación de archivos a fichas (D-027) y rendimiento (D-028).
- [ ] **Metadatos de TMDB** (D-029): títulos de episodio, sinopsis, carátulas y desambiguar *remakes*. Pendiente de la credencial `TELEGARRM_TMDB_TOKEN`.
- [ ] Créditos de TMDB en la web (requisito de su licencia).

Formato real visto: canales de una serie con una ficha (foto + pie con título, año, calidad, géneros e idioma), portadas de temporada sin pie, episodios `1x01 - Serie.mkv` o `Serie #01x01 - Título.mp4` y textos de cierre.

## Fase 3: Descargas
- **Rutas** de la biblioteca (series, películas, buffer temporal) configurables desde la web (`settings`).
- **Elegir versión**: la web ofrece las versiones de cada película o episodio (`Release`: 4K HDR, 1080p, REMUX…) y se descarga solo la elegida.
- **Cola persistente** (tabla `downloads`) con un trabajador: `downloadFile` + `updateFile` para el progreso, reintentos y reanudación tras reiniciar.
- **Postproceso** migrado del núcleo antiguo: descompresión (7z, zip troceado `.zip.001`, multiparte `.partN.rar`), limpieza de nombres y colocación en `Serie/Temporada 01/Serie - S01E01.mkv`.
- **API**: `POST /api/downloads`, `GET /api/downloads` (progreso), `DELETE /api/downloads/{id}`.
- **Web**: botón "Almacenar en disco" y progreso en tiempo real.

## Fase 4: Seguimiento (Tele-ARR)
- Marcar series y películas como "en seguimiento".
- Escuchar `updateNewMessage` en los canales vigilados (en especial el tema "Series en emisión", donde se publica un episodio por ficha) y parsear cada mensaje nuevo:
  - Episodio nuevo de algo en seguimiento: se descarga.
  - Versión nueva de algo ya descargado: se compara (resolución, códec, audio, tamaño) y, si es mejor, se reemplaza. El fichero antiguo se conserva hasta verificar el nuevo.
- Historial de reemplazos visible en la web.

## Transversal (antes de exponer la web fuera de la red local)
- **Autenticación** en la web y la API: ahora cualquiera en la LAN puede usarla.
- **HTTPS** mediante proxy inverso: el login de Telegram y la contraseña 2FA viajan hoy en claro por HTTP.
- Tests (`ctest`) del parser y de la BD.
- Copia de seguridad de `db/`.

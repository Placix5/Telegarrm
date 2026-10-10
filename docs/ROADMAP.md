# Hoja de ruta de Telegarrm

*Estado al 10/10/2026: Fases 1 a 4.1 completadas; canales de anime (D-047), obras agrupadas por TMDB (D-048) y temporadas con ficha propia (D-049).*

Documento vivo con el plan de implementación; los motivos de cada decisión están en [DECISIONS.md](DECISIONS.md) y cómo está construido, en [01_architecture_and_phases.md](01_architecture_and_phases.md). Lo mantiene Claude Code (desarrollo y plan técnico); Gemini revisa el código y la documentación; Plácido decide prioridades y valida en uso real.

## Objetivo
Un servicio tipo *stack ARR* (Sonarr/Radarr) que corre siempre en la Raspberry Pi y usa uno o dos canales de Telegram como única fuente:
1. **Catálogo**: mostrar en una web las series y películas publicadas en los canales elegidos.
2. **Descarga**: botón "Almacenar en disco" que descarga, descomprime, renombra y coloca los ficheros en la biblioteca sin intervención.
3. **Seguimiento**: vigilar los mensajes nuevos, descargar los episodios nuevos de lo que se sigue y, si llega una versión mejor (resolución, HDR, REMUX), reemplazarla.

Todo se gestiona desde el navegador, sin SSH ni terminal.

## Principios
- **Un único binario C++17**: servidor HTTP (`cpp-httplib`), TDLib (interfaz JSON) en su propio hilo y SQLite. La web es estática y la sirve el propio binario.
- **Secretos solo en variables de entorno** (`TELEGARRM_API_ID`, `TELEGARRM_API_HASH`), nunca en git ni en la BD. La sesión de TDLib vive en `db/tdlib/` con permisos `0700`.
- **Nada de comandos de shell montados con cadenas**: el proyecto antiguo usaba `popen`/`system` con rutas y tokens concatenados (riesgo de inyección con nombres de fichero raros). Aquí 7-Zip y ffprobe se lanzan con `posix_spawn` y argumentos separados, y TMDB se consulta con el cliente HTTP de `cpp-httplib`.
- **Localización**: unidades del Sistema Internacional (tamaños en kB/MB/GB y velocidades en MB/s, base 1000), fechas `dd/mm/aaaa` y hora `Europe/Madrid`.
- **Datos personales** (teléfono, sesión) solo en local; sin telemetría.
- Hitos pequeños: cada uno compila sin avisos, pasa los tests, se prueba en la Pi y se commitea en local; Plácido decide cuándo subirlo a GitHub.

## Fase 1: Motor TDLib (completada)
- [x] SQLite (`DbManager`), hilos y parada ordenada con SIGINT/SIGTERM.
- [x] TDLib 1.8 (interfaz JSON) en un hilo propio, con peticiones asíncronas y síncronas.
- [x] Inicio de sesión desde la web: teléfono, código y contraseña 2FA.
- [x] Primer inicio de sesión real con la cuenta de Plácido.
- [x] Servicio de systemd: primero de usuario y después de sistema, aislado y con regla de polkit (`deploy/`, D-012 y D-013). Instalado en la Pi.

## Fase 2: Canales y catálogo (completada)
- [x] **Canales**: listar los chats de la cuenta (`getChats`, incluidos archivados) para elegir desde la web cuáles vigilar (tabla `channels`).
- [x] **Sincronización**: recorrer el historial (`getChatHistory` paginado, respetando los `FLOOD_WAIT` de Telegram) y guardar los mensajes con texto o fichero: id, fecha, álbum, tema, texto, nombre, tamaño y tipo MIME. Reanudable; mensajes nuevos cada 15 min (y, desde la Fase 4, al momento).
- [x] **Parser**: adaptar `episode_parser` del proyecto antiguo (SxxEyy, 1x01, calidades, etiquetas de versión, partes) con tests unitarios (`ctest`) construidos con ejemplos reales del canal.
- [x] **Catálogo**: en memoria, con las fichas como separador (D-020, D-021). El diseño cambió respecto al plan inicial: no hay tabla `media`.
- [x] **API**: `GET /api/catalog`, `GET /api/catalog/{chat}/{ficha}` y la portada.
- [x] **Web**: catálogo en tarjetas con portada, búsqueda y filtro por tipo; ficha con temporadas y episodios.
- [x] Canal grande de Plácido (grupo con temas, ~33 000 mensajes): temas (D-025), obras con versiones y partes (D-026), asignación de archivos a fichas (D-027) y rendimiento (D-028).
- [x] **Metadatos de TMDB** (D-029, D-030): títulos de episodio, sinopsis, géneros, carátulas e identificadores externos, en caché local.
- [x] Créditos de TMDB en la web (requisito de su licencia).
- Pendiente: separar *remakes* con TMDB (en [Siguientes pasos](#siguientes-pasos)).

Formato real visto: canales de una serie con una ficha (foto + pie con título, año, calidad, géneros e idioma), portadas de temporada sin pie, episodios `1x01 - Serie.mkv` o `Serie #01x01 - Título.mp4` y textos de cierre.

## Fase 3: Descargas (completada)
- [x] **Rutas** de la biblioteca y del búfer configurables desde la web (`settings`).
- [x] **Elegir versión**: la web ofrece las versiones de cada película o episodio (4K HDR, 1080p, REMUX…) y, en las series, la temporada completa en una versión.
- [x] **Cola persistente** (tablas `downloads` y `download_parts`) con un trabajador: progreso, reintentos, reanudación tras reiniciar, cancelación y comprobación de espacio libre (D-031).
- [x] **Ajustes** en la web: búfer de descargas de TDLib, bibliotecas y espacio libre mínimo (D-032).
- [x] **Biblioteca**: `/srv/media/{descargas,peliculas,series}` (D-033); en el servidor, el RAID montado en `/srv/media`.
- [x] **Importación** (D-034): descompresión con 7-Zip sin shell (zip troceado, multiparte RAR, 7z), vídeos sin muestras, subtítulos, nombres para Jellyfin (`Título (Año) [tmdbid-N]`, `Season 01`) y limpieza del búfer. Comprobado con descargas reales en ZIP y RAR multiparte (el RAR necesita el paquete `7zip-rar`).
- [x] **API**: `POST /api/downloads`, `GET /api/downloads` (progreso), cancelar, reintentar y quitar.
- [x] **Web**: botón "Almacenar en disco" y pestaña *Descargas* con el progreso.

## Fase 4: Seguimiento (Tele-ARR) (completada)
- [x] **Seguir** series y películas desde su ficha, con calidad máxima (la mejor, hasta 1080p, hasta 720p). Tabla `follows`; la obra se reencuentra aunque cambie su ficha principal (D-035).
- [x] **Tiempo real** (D-036): `openChat` en los canales vigilados y escucha de `updateNewMessage`. Se agrupan los avisos (20 s de calma, 2 min como mucho) y se traen solo los mensajes nuevos. Después, el catálogo los analiza y el seguimiento los evalúa. La ronda de cada 15 min recupera lo perdido.
- [x] **Auto-descarga**: episodios nuevos de las series seguidas y la primera versión que se publique de una película seguida. Solo lo publicado después de seguir. Espera a que estén todas las partes de un comprimido.
- [x] **Mejoras**: una versión con más resolución, HDR o REMUX sustituye a la que se tiene. La anterior se borra solo cuando la nueva ya está en la biblioteca, o se conserva si así se elige en *Ajustes* (D-037).
- [x] **Historial de actividad** (tabla `activity`) y pestaña *Actividad* con lo que se sigue. Filtro «En seguimiento» en el catálogo.
- [x] Comprobado en la Pi con un episodio nuevo (*Presidente Curtis* 1x09) y con una mejora de 1080p a 4K HDR (*El show de los Muppets*).
- [x] Tiempo real comprobado con el canal de prueba de Plácido. Se subió el 1x02 de *Ultimate Spider-Man* a una serie seguida: aviso al instante, lectura 20 s después, catálogo y seguimiento al momento, y el episodio en la biblioteca unos 10 s más tarde (D-036, D-038).
- [x] **Nombre de la serie** sin ficha: el texto que repiten los episodios tras el marcador (`1x01 - Ultimate Spiderman.mkv`) es la serie (D-038).
- [x] **Calidad real del vídeo** al importar, con `ffprobe` (D-039): resolución y HDR del propio archivo, que mandan sobre el nombre y la ficha. Las descargas anteriores se corrigieron al arrancar: los dos episodios de *Ultimate Spider-Man* eran 720p, no 1080p ni «calidad desconocida».
- [x] **La biblioteca dice qué se tiene** (D-041): cuentan los vídeos que ya hay en la carpeta de la obra, aunque vengan de un canal quitado o se copiaran a mano. Al importar un episodio, sus otras versiones se sustituyen: queda la más reciente.
- [x] **Calidad real antes de descargar** (D-042): se leen solo los primeros MB del archivo (también dentro de ZIP y RAR) y se analizan con ffprobe. Botón «?» por versión, «Comprobar calidades» por temporada; el seguimiento comprueba antes de decidir.
- [x] **Episodios que faltan / serie completa** (D-040): en la ficha de una serie, cuántos episodios conocidos se tienen y un botón para descargar los que faltan (o la serie entera), con la mejor versión de cada uno dentro de la calidad elegida. Comprueba antes que caben en la biblioteca.
## Fase 4.1: mejoras de uso (completada)
- [x] El logo de la cabecera lleva al catálogo.
- [x] «Añadidas recientemente» encima del catálogo (las 12 obras con publicaciones más recientes).
- [x] Aviso emergente si llega algo nuevo de la obra que se está viendo (D-043): novedades del catálogo consultadas con el sondeo de `/api/status`, sin conexiones permanentes.
- [x] La ficha abierta se actualiza sola (D-045) cuando termina una descarga suya, llega una novedad o cada 30 s, sin perder la posición.
- [x] **Móvil** (D-046): pestañas en 3×2, tablas que no ensanchan la página (y que en el móvil se reorganizan en bloques), botones y selectores de 44 px al tacto, avisos al 90 % del ancho.
- [x] Confirmaciones con el estilo de la página, con un resumen de lo que se va a hacer, y errores como avisos en la esquina, en lugar de los cuadros del navegador (D-044).

## Canales de anime (08/10/2026)
- [x] CrunchyShur (un tema por obra, nombres como `Serie - 01`, `Serie S2 - 08`, `Serie T2 - 01`): episodios, temporadas de las fichas («S2», «Final Season»), extras y arcos unidos a la serie del tema (D-047). Solo en los canales que se detectan como de anime.
- [x] *Las Cositas* idéntica antes y después, comprobado con `tools/catalog_dump.cpp`. Sus 24 obras que se habían unido a CrunchyShur vuelven a ser suyas.
- [ ] *Hunter x Hunter (2011)* sale en dos obras, por el nombre del grupo de subtítulos (`[BB]`) delante; unos 40 especiales numerados («SP 06», «OVA 03») salen como películas sueltas.
- [ ] Decidir si los 105 archivos `Ladybug - 027` de *Las Cositas* deben leerse como episodios (cambiaría *Ladybug*: +100 episodios).
- [x] TMDB no encontraba las obras que solo tienen el nombre japonés (*Boku no Hero Academia*; en TMDB es *My Hero Academia*): resuelto con los títulos alternativos de TMDB (D-048).

## La misma obra en varios canales (10/10/2026)
- [x] Fichas que unían películas distintas por un paréntesis: «(1080p AV1)» juntaba *La fortaleza infinita*, *Obsession* y *Mortal Kombat II*; «You Are (Not) Alone», las tres de *Evangelion* (D-048).
- [x] TMDB: mismas palabras en otro orden y títulos alternativos (romaji, inglés…), sin arcos ni secuelas en los canales de anime.
- [x] Catálogo con una tarjeta por obra (las de la misma ficha de TMDB, juntas), filtro por canal y, en la ficha, los canales donde está.
- [x] Temporadas y arcos con ficha propia que continúan la numeración de su serie, unidos a ella (D-049): *Kimetsu no Yaiba* con sus 4 temporadas en los dos canales, *Miracle Workers*, *Shingeki no Kyojin*…
- [ ] Los arcos que vuelven a empezar en la temporada 1 sin decir la temporada siguen siendo obras aparte (*Full Metal Panic! The Second Raid*).
- [ ] Si cambia la ficha de TMDB de una obra ya descargada, su carpeta de la biblioteca no se mueve sola.
- [ ] Datos de TMDB mal puestos que juntan obras distintas: *La conquista del planeta de los simios* y *Batalla por el planeta de los simios* salen juntas, y *Ranma ½* de 2024 (CrunchyShur) comparte datos con la de 1989 (*Las Cositas*).

## Siguientes pasos
Por decidir con Plácido; ninguno está empezado.

### Antes de exponer la web fuera de la red local
- **Autenticación** en la web y la API: ahora cualquiera en la LAN puede usarla.
- **HTTPS** mediante proxy inverso: el login de Telegram y la contraseña 2FA viajan hoy en claro por HTTP.

### Paso al servidor definitivo
- Llevar el servicio al servidor con el SSD del sistema y el RAID: búfer y bibliotecas en el RAID (D-033), y Jellyfin en Docker (datos de los contenedores en `/opt/docker`) leyendo `/srv/media`.
- `deploy/jellyfin-compose.yml`: borrador añadido el 07/10/2026, fuera del trabajo de Claude.
  - Bien: monta las bibliotecas en solo lectura y usa el usuario 1000 (`plax`), que puede leer lo que importa Telegarrm.
  - Pendiente de decidir si se queda en el repositorio.
  - Si se queda, cambiar `./jellyfin_config` por `/opt/docker/jellyfin/config`: tal como está, la configuración y la BD de Jellyfin se crearían dentro del repositorio (`deploy/jellyfin_config`), sin ignorar en git.
- **Copia de seguridad de `db/`**: la sesión de Telegram, la BD y las carátulas. Con cuidado: la sesión da acceso a la cuenta.

### Mejoras pendientes
- Usar TMDB para separar *remakes* que el catálogo une (ej. *Vaiana* de 2016 y de 2026).
- Calidad real de los MP4 que van dentro de un comprimido con el índice al final: hoy no se pueden leer sin descargarlos enteros (D-042).
- Notificaciones (Telegram o correo) de lo descargado.
- Vigilar ediciones y borrados de mensajes (`updateMessageContent`, `updateDeleteMessages`).
- Seguir una obra antes de que esté en el catálogo.
- Renombrar los archivos de la biblioteca cuya etiqueta de calidad no coincide con el vídeo (importados antes de D-039).

### Hecho de lo transversal
- [x] Tests (`ctest`) del parser, el catálogo, la BD, la biblioteca, el seguimiento y TMDB: 505 comprobaciones, algunas con vídeos y comprimidos reales generados con ffmpeg y 7-Zip.

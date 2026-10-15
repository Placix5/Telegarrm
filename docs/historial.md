# Historial de cambios

Del más reciente al más antiguo. Los números D-0XX enlazan al [registro de decisiones](DECISIONS.md), que explica el porqué de cada cambio. Los canales reales de las pruebas aparecen con nombres genéricos (*Canal Principal*, Canal Anime).

## Docker
- La Raspberry Pi donde se desarrolla y prueba Telegarrm pasa al contenedor, con sus datos montados tal cual (misma contraseña, sesión de Telegram y catálogo) y el servicio nativo desactivado.
- `Dockerfile` en tres etapas (TDLib 1.8.67, Telegarrm con sus tests y una imagen final de Debian trixie solo con lo necesario), publicada por GitHub Actions en `ghcr.io/placix5/telegarrm` para x86_64 y ARM64 ([D-054](DECISIONS.md)).
- Compose propio en `deploy/docker/`, con las claves como Docker secrets (`*_FILE`), rutas configurables, usuario sin privilegios y sistema de archivos de solo lectura. Jellyfin queda en su compose aparte.

## Contraseña para la web
- La web y la API piden contraseña ([D-053](DECISIONS.md)). Se guarda con scrypt; las sesiones duran 30 días desde su último uso, con cookie `HttpOnly` y `SameSite=Strict`, y en la BD solo está su huella.
- La primera contraseña exige el código de `db/codigo-acceso.txt`. Hay un límite de intentos fallidos, «Salir» en la cabecera, cambio de contraseña en *Ajustes* y `--reset-password` para restablecerla.
- Tests: 606 comprobaciones.

## Pestaña «Biblioteca»
- Lo que hay en disco, con su ficha del catálogo, y borrado de series, temporadas, películas o archivos con confirmación ([D-052](DECISIONS.md)). Solo puede borrar archivos dentro de la biblioteca. Las descargas afectadas pasan a «Borrada».
- «Revisar la biblioteca» pasa aquí desde *Ajustes*. Con siete pestañas, en el móvil se reparten en filas (4 + 3).
- Tests: 569 comprobaciones.

## Revisar la biblioteca
- «Revisar la biblioteca» (D-051; hoy en la pestaña «Biblioteca»): si se borran o mueven archivos a mano, encuentra lo movido y apunta dónde está; lo borrado pasa a «Borrada» y deja de contar como que se tiene.
- El mismo archivo subido otra vez cuenta una sola vez. En los canales de anime, los cortos que acompañan a cada episodio (*Maomao no Hitorigoto*) pasan a «Otros archivos».
- Tests: 544 comprobaciones.

## Películas con otro nombre en TMDB
- TMDB reconoce una película aunque la ficha lleve la saga delante («Kimetsu no Yaiba: Guardianes de la Noche - Tren Infinito»), diga «Movie» o «Película», o tenga una errata que el nombre del archivo no tiene ([D-050](DECISIONS.md)). *Tren infinito* sale una sola vez, con las fichas de los dos canales.
- *Canal Principal*: 21 películas más reconocidas en TMDB, sin cambiar ni perder ninguna. Tests: 523 comprobaciones.

## Temporadas con ficha propia
- Una ficha de temporada o de arco con su propio título («Kimetsu no Yaiba: Guardianes de la Noche - Arco de la aldea de los herreros») se une a su serie si continúa su numeración ([D-049](DECISIONS.md)). *Kimetsu no Yaiba* sale con sus 4 temporadas en *Canal Principal* y en Canal Anime, y como una sola obra.
- En los canales de anime, los arcos que vuelven a empezar en el 01 dentro de una temporada se numeran a continuación, como en TMDB, y se lee `Serie S3 - Arco - 01`.
- Tests: 505 comprobaciones.

## La misma obra en varios canales
- El catálogo junta las obras que son la misma según TMDB, aunque estén en varios canales o con otro nombre ([D-048](DECISIONS.md)). *Boku no Hero Academia* (Canal Anime) y *My Hero Academia* (*Canal Principal*) salen como una sola obra; en su ficha se elige de qué canal ver los archivos. 91 obras están en más de un sitio.
- Filtro por canal en el catálogo.
- TMDB reconoce el título con las palabras en otro orden y los títulos alternativos (romaji, inglés…), sin confundir los arcos de un canal de anime con su serie: 2 570 de 3 481 obras encontradas (antes, 2 400).
- Las fichas con «(1080p AV1)» o «You Are (Not) Alone» ya no juntan películas distintas: *La fortaleza infinita* sale sola, con su ficha de TMDB correcta.
- *Canal Principal* se revisó obra por obra (`tools/catalog_dump.cpp`, ahora también con `--tmdb`): solo cambian las fichas con esos paréntesis. Tests: 494 comprobaciones.

## Canales de anime
- Canales como Canal Anime, con un tema por obra y nombres de anime (`Serie - 01`, `Serie S2 - 08`, `Serie T2 - 01`, `Serie 003`, `42 - Título`), se detectan solos por sus nombres de archivo y se leen con sus reglas ([D-047](DECISIONS.md)):
  - La temporada sale de la ficha («Temporada 2», «Serie S2») y «Final Season» es la siguiente a la última.
  - OVAs, especiales y películas numeradas se unen a la serie del tema, y también los arcos con ficha propia.
  - Sus obras no se mezclan con las de otros canales.
- *Canal Principal* no cambia: se comprobó obra por obra con `tools/catalog_dump.cpp` (`cmake --build build --target telegarrm_catalog_dump`), que calcula el catálogo de una copia de la BD.
- Canal Anime pasa de 9 595 obras a 1 242 (*My Hero Academia*: 8 temporadas y 170 episodios). Tests: 463 comprobaciones.

## Fase 4.1: mejoras de uso
- El logo de la cabecera lleva al catálogo.
- «Añadidas recientemente»: fila con las 12 obras con publicaciones más recientes, con «hace 2 h», «ayer»…
- Avisos en directo ([D-043](DECISIONS.md)): si se está viendo la ficha de una obra y llega algo nuevo de ella por Telegram, aparece un aviso en la esquina. El catálogo anota las novedades y la web las consulta con el sondeo de `/api/status` que ya hacía.
- La ficha abierta se actualiza sola ([D-045](DECISIONS.md)): al terminar una descarga suya (aparece «✓ En tu biblioteca» y se recuentan los episodios), al llegar una novedad (la fila nueva se ilumina) y cada 30 s. Sin perder la posición.
- Barras de desplazamiento y controles nativos con el tema del sistema (claro u oscuro).
- Web para el móvil ([D-046](DECISIONS.md)):
  - Pestañas en una rejilla de 3×2.
  - Tablas de episodios y versiones reorganizadas en bloques, sin desplazarse de lado.
  - Botones y selectores de 44 px al tacto, con letra de 16 px en los campos para que el iPhone no amplíe la página.
  - Avisos al 90 % del ancho.
  - Ninguna pantalla es ya más ancha que el teléfono.
- Confirmaciones propias en lugar de las del navegador ([D-044](DECISIONS.md)): explican qué va a pasar y resumen los datos (episodios, tamaño, calidad). Lo destructivo va en rojo y con el foco en «Cancelar». Los errores salen como avisos en la esquina.

## Fase 4: seguimiento (Tele-ARR)
- **Tiempo real** ([D-036](DECISIONS.md)): los canales vigilados se abren en TDLib (`openChat`) y cada `updateNewMessage` dispara una sincronización rápida. Los avisos se agrupan para no saturar TDLib.
- **Seguimiento** ([D-035](DECISIONS.md)): botón «Seguir» en cada ficha, con calidad máxima.
  - Los episodios nuevos de las series seguidas se descargan solos, y también la primera versión que se publique de una película seguida. Solo cuenta lo publicado después de seguir.
  - Se espera a que estén todas las partes de un comprimido.
- **Mejoras** ([D-037](DECISIONS.md)): una versión con más resolución, HDR o REMUX sustituye a la descargada. La anterior se borra cuando la nueva ya está en la biblioteca, o se conserva si así se elige en *Ajustes*.
- **Calidad real**:
  - Al importar, `ffprobe` lee cada vídeo; su resolución y su HDR mandan sobre el nombre y la ficha ([D-039](DECISIONS.md)).
  - Antes de descargar, se leen solo los primeros MB de cada archivo, también dentro de ZIP y RAR ([D-042](DECISIONS.md)).
- **La biblioteca dice qué se tiene** ([D-041](DECISIONS.md)): cuenta lo que ya hay en la carpeta de cada obra, y un episodio importado sustituye a sus otras versiones.
- **Serie completa** ([D-040](DECISIONS.md)): en la ficha de una serie, cuántos episodios se tienen y un botón para descargar los que faltan (o la serie entera), en la mejor versión de cada uno.
- Sin ficha, el nombre de la serie se toma del texto que repiten los episodios (`1x01 - Ultimate Spiderman.mkv`, D-038).
- **Web**:
  - Pestaña *Actividad* con las obras seguidas y el historial.
  - Filtro «En seguimiento» en el catálogo.
  - Descargas automáticas y sustituidas distinguidas en *Descargas*.
  - Botones «?» y «Comprobar calidades».
  - Episodios ya en la biblioteca marcados.
- **BD**:
  - Migración v7: tablas `follows`, `auto_releases` y `activity`; en `downloads`, origen, seguimiento, sustituciones y archivos colocados.
  - Migración v8: tabla `release_probes`.
  - Tests: 411 comprobaciones.
- **Comprobado en la Pi**:
  - Un episodio nuevo de una serie seguida (860 MB, 20 s).
  - Una mejora de 1080p a 4K HDR que borró la versión anterior.
  - Con un canal de prueba, un episodio subido en directo que se descargó solo unos 30 s después.
  - La calidad real de 23 archivos variados sin descargarlos.

## Fase 3: descargas e importación a la biblioteca
- Importación ([D-034](DECISIONS.md)): al terminar, cada descarga se descomprime si hace falta (7-Zip, sin shell), se eligen los vídeos y subtítulos y se mueven a la biblioteca con nombres para Jellyfin (`Título (Año) [tmdbid-N]/…`, `Season 01/Serie S01E01 - 1080p.mkv`), y se libera el búfer. Comprobado en la Pi con un ZIP (451 MB) y un RAR de 2 partes (1,86 GB, descomprimido en ~20 s); los RAR necesitan el paquete `7zip-rar`.
- Pestaña *Ajustes*: búfer de descargas de TDLib (aplicado con un reinicio ordenado desde la web), bibliotecas y espacio libre mínimo, con validación real de cada ruta ([D-032](DECISIONS.md)). La unidad admite `/srv/media` y rutas adicionales vía `install-service.sh`.
- Arreglos de la web: flecha propia en los desplegables y botones de descarga de ancho fijo que muestran el progreso.
- Cola de descargas persistente: botón "Almacenar en disco" por versión o por temporada, pestaña *Descargas* con progreso, velocidad y tiempo restante, cancelación (borra lo parcial), reintento, reanudación tras reiniciar y comprobación de espacio libre ([D-031](DECISIONS.md)). Al principio los archivos se quedaban en la caché de TDLib; la importación ([D-034](DECISIONS.md)) los lleva ya a la biblioteca.

## Fase 2: canales, sincronización y catálogo
- Metadatos de TMDB (D-029, D-030): sinopsis, títulos y sinopsis de episodio, géneros, carátulas e identificadores externos, guardados en local; créditos de TMDB en la web. Funciona sin token con los datos de las fichas.
- Grupos con temas (foros): cada mensaje guarda su tema y las fichas se agrupan dentro de cada tema (migración v3, que relee el historial una vez).
- Obras con versiones: las temporadas, los episodios en emisión y las versiones 1080p/4K de una misma obra se unen por título; las partes de los archivos troceados (`.zip.001`, `.part01.rar`, `_part06.rar`) forman un único archivo. La web muestra las versiones de cada episodio o película y las series en emisión.
- Compilación optimizada por defecto (`RelWithDebInfo`) y caché del análisis: el catálogo de 33 000 mensajes se calcula en unos 2 s.
- Catálogo en memoria (`Catalog`): cada foto con pie (la "ficha") abre una serie o película y los vídeos siguientes le pertenecen. El parser (`media_parser`) reconoce `1x01`, `#01x01`, `S01E01`, `T1E3` y `Temporada 1 Capítulo 3`, y extrae de la ficha título, año, calidad, géneros e idioma (banderas incluidas). Tiene 103 comprobaciones con casos reales en `tests/`.
- Web con pestañas (Catálogo, Canales, Estado): cuadrícula con portadas, búsqueda sin acentos, filtro por tipo y ficha con los episodios por temporada.
- Esquema de BD versionado con migraciones (`PRAGMA user_version`); WAL para escribir menos en la tarjeta SD.
- `ChannelSync` copia el historial de los canales vigilados en la tabla `messages` (texto, nombre, tamaño y tipo de fichero), por lotes de 100. El cursor se guarda en la misma transacción que cada lote, así que tras un reinicio continúa donde lo dejó. Respeta los `FLOOD_WAIT` de Telegram y después busca mensajes nuevos cada 15 min.
- La web permite elegir los canales vigilados entre los chats de la cuenta, sincronizarlos a mano y quitarlos.
- Servicio de sistema de systemd aislado (`deploy/telegarrm.service`) y regla de polkit para reiniciarlo sin `sudo`. Sustituye al servicio de usuario inicial.
- Registro de decisiones en [`docs/DECISIONS.md`](DECISIONS.md).

## Fase 1: TDLib real e inicio de sesión desde la web
- `TelegramClient` usa TDLib (interfaz JSON, `td_send`/`td_receive`) en su propio hilo. Cada respuesta se asocia a su petición mediante `@extra`, con variantes asíncrona (`send`) y síncrona con timeout (`request`). Al parar, cierra TDLib de forma ordenada (`close`) para que guarde su estado.
- Credenciales por variables de entorno; sesión en `db/tdlib/` con permisos `0700`.
- Endpoints de la API movidos a `src/api.cpp`; nuevos endpoints de inicio de sesión.
- Primera página web (`web/index.html`): estado del servicio y formulario de inicio de sesión con los errores de Telegram traducidos.
- El puerto se reserva antes de arrancar TDLib: una segunda instancia termina sin tocar la sesión de la primera.

## Inicio de Fase 1: SQLite e hilos
- `CMakeLists.txt` busca y enlaza SQLite3.
- `DbManager` crea `db/telegarrm.db` con las tablas `settings (key, value)` y `channels (id, name)`. Es seguro entre hilos (mutex interno) y gestiona los recursos de SQLite con `std::unique_ptr`.
- `/api/status` lee la clave `version` de `settings`, que se guarda en cada arranque.
- `SignalWatcher` atiende SIGINT/SIGTERM con `sigwait` en un hilo dedicado: detiene el servidor HTTP y el hilo de Telegram y cierra la BD antes de salir con código 0 (en Windows no hace nada).
- El servidor ya no usa `SO_REUSEPORT` (activado por defecto en cpp-httplib): una segunda instancia en el mismo puerto ahora falla en lugar de repartirse las peticiones con la primera.

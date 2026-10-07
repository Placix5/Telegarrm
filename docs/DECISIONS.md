# Registro de decisiones

Cada decisión técnica o de proyecto se anota aquí cuando se toma, con su motivo. Las decisiones no se borran: si cambian, la antigua se marca como **Sustituida** y se enlaza la nueva.

Formato: **Contexto** (qué problema había), **Decisión**, **Alternativas descartadas** y **Consecuencias** (qué implica a partir de ahora). Las decisiones de Plácido se indican como tales.

| ID | Decisión | Estado |
| --- | --- | --- |
| [D-001](#d-001-reparto-de-papeles) | Claude Code lleva el proyecto; Gemini revisa | Vigente |
| [D-002](#d-002-commits-solo-en-local) | Commits solo en local | Vigente |
| [D-003](#d-003-interfaz-json-de-tdlib-con-la-api-td_sendtd_receive) | Interfaz JSON de TDLib con `td_send`/`td_receive` | Vigente |
| [D-004](#d-004-tdlib-compilado-desde-el-código-fuente-en-tdtdlib) | TDLib compilado en `~/td/tdlib` | Vigente |
| [D-005](#d-005-dependencias-de-solo-cabecera-dentro-del-repositorio) | Dependencias de solo cabecera en el repositorio | Vigente |
| [D-006](#d-006-credenciales-solo-por-variables-de-entorno) | Credenciales solo por variables de entorno | Vigente |
| [D-007](#d-007-inicio-de-sesión-de-telegram-desde-la-web) | Inicio de sesión de Telegram desde la web | Vigente |
| [D-008](#d-008-sesión-de-tdlib-en-dbtdlib-con-permisos-0700) | Sesión de TDLib en `db/tdlib` (0700) | Vigente |
| [D-009](#d-009-modelo-de-hilos-de-telegramclient) | Modelo de hilos de `TelegramClient` | Vigente |
| [D-010](#d-010-parada-ordenada-con-sigwait-en-un-hilo-dedicado) | Parada ordenada con `sigwait` | Vigente |
| [D-011](#d-011-una-sola-instancia-so_reuseaddr-y-puerto-reservado-antes-de-tdlib) | Una sola instancia | Vigente |
| [D-012](#d-012-servicio-de-usuario-de-systemd) | Servicio de usuario de systemd | Sustituida por D-013 |
| [D-013](#d-013-servicio-de-sistema-aislado-y-regla-de-polkit) | Servicio de sistema aislado + polkit | Vigente |
| [D-014](#d-014-sqlite-una-conexión-wal-y-migraciones-versionadas) | SQLite: una conexión, WAL, migraciones | Vigente |
| [D-015](#d-015-sincronización-del-historial-de-los-canales) | Sincronización del historial | Vigente |
| [D-016](#d-016-web-sin-frameworks-ni-paso-de-compilación) | Web sin frameworks | Vigente |
| [D-017](#d-017-selección-de-canales-a-cargo-del-usuario) | Selección de canales a cargo del usuario | Vigente |
| [D-018](#d-018-sin-comandos-de-shell-montados-con-cadenas) | Sin comandos de shell montados con cadenas | Vigente |
| [D-019](#d-019-localización-y-datos-personales) | Localización y datos personales | Vigente |
| [D-020](#d-020-catálogo-en-memoria-derivado-de-los-mensajes) | Catálogo en memoria derivado de los mensajes | Vigente |
| [D-021](#d-021-las-fichas-separan-los-elementos-del-catálogo) | Las fichas separan los elementos del catálogo | Sustituida por D-026 |
| [D-022](#d-022-tests-con-ctest-sin-framework-externo) | Tests con CTest, sin framework externo | Vigente |
| [D-023](#d-023-portadas-descargadas-bajo-demanda-con-tdlib) | Portadas descargadas bajo demanda con TDLib | Vigente |
| [D-024](#d-024-búsqueda-y-navegación-en-el-navegador) | Búsqueda y navegación en el navegador | Vigente |
| [D-025](#d-025-temas-de-los-foros) | Temas de los foros | Vigente |
| [D-026](#d-026-obras-versiones-y-partes) | Obras, versiones y partes | Vigente |
| [D-027](#d-027-qué-archivos-pertenecen-a-una-ficha) | Qué archivos pertenecen a una ficha | Vigente |
| [D-028](#d-028-compilación-optimizada-por-defecto-y-caché-del-análisis) | Compilación optimizada y caché del análisis | Vigente |
| [D-029](#d-029-metadatos-de-themoviedb-tmdb) | Metadatos de TheMovieDB (TMDB) | Vigente |
| [D-030](#d-030-cómo-se-integra-tmdb) | Cómo se integra TMDB | Vigente |
| [D-031](#d-031-cola-de-descargas) | Cola de descargas | Vigente |
| [D-032](#d-032-ajustes-desde-la-web-y-reinicio-con-el-código-75) | Ajustes desde la web y reinicio con el código 75 | Vigente |
| [D-033](#d-033-estructura-de-carpetas-srvmedia-y-jellyfin) | Estructura de carpetas `/srv/media` y Jellyfin | Vigente |
| [D-034](#d-034-importación-a-la-biblioteca) | Importación a la biblioteca | Vigente |

---

## D-001: Reparto de papeles
*06/10/2026 · Decisión de Plácido*

- **Contexto**: al principio Gemini planificaba (documentos `docs/0X_current_task.md`) y Claude Code programaba. La documentación de Gemini resultó incoherente en varios puntos.
- **Decisión**: Claude Code lleva el proyecto completo (plan técnico y código). Gemini revisa el código y la documentación. Plácido decide prioridades y valida en uso real.
- **Consecuencias**: el plan vigente es [ROADMAP.md](ROADMAP.md), que mantiene Claude Code. Los documentos 01–03 de Gemini quedan como histórico.

## D-002: Commits solo en local
*06/10/2026 · Decisión de Plácido*

- **Decisión**: Claude Code hace commits pequeños en local; Plácido decide cuándo subirlos a GitHub.
- **Consecuencias**: la Pi no necesita credenciales de GitHub.

## D-003: Interfaz JSON de TDLib con la API `td_send`/`td_receive`
*06/10/2026 · Recomendación de Claude Code aceptada por Plácido*

- **Contexto**: TDLib ofrece una interfaz C++ nativa (`Td::TdStatic`, `td_api.h`) y otra JSON (`Td::TdJson`). El proyecto antiguo usaba JSON con la API `td_json_client_*`, ya obsoleta.
- **Decisión**: interfaz JSON con la API actual (`td_create_client_id`, `td_send`, `td_receive`).
- **Alternativas descartadas**: la interfaz nativa es tipada, pero `td_api.h` es enorme (compilaciones mucho más lentas en la Pi), obliga a enlazar TDLib de forma estática y el código antiguo habría que reescribirlo entero.
- **Consecuencias**: dependencia de `nlohmann/json`. TDLib envía los `int64` como cadenas JSON (ej. `media_album_id`). Los errores de forma se detectan en ejecución, así que se valida cada campo y se capturan las excepciones para que un mensaje raro no tumbe ningún hilo.

## D-004: TDLib compilado desde el código fuente en `~/td/tdlib`
*06/10/2026*

- **Contexto**: Debian no empaqueta TDLib.
- **Decisión**: clonar `tdlib/td` y compilar solo `libtdjson` compartida (`-DTD_INSTALL_STATIC_LIBRARIES=OFF`, target `tdjson`, `-j3` con `nice`; unos 30 minutos en la Pi 5). Se instala en `~/td/tdlib`, donde CMake la busca, y el binario la encuentra por `RUNPATH`.
- **`find_package(Td)` sin versión**: TDLib declara compatibilidad *exacta*, así que pedir `1.8.0` falla con la 1.8.67 y cualquier actualización rompería la configuración. El mínimo (1.8.0) se comprueba a mano.
- **Consecuencias**: actualizar TDLib supone recompilarlo. El binario necesita `~/td/tdlib/lib` en tiempo de ejecución.

## D-005: Dependencias de solo cabecera dentro del repositorio
*06/10/2026*

- **Decisión**: `cpp-httplib` 0.59.0 (heredada de la estructura base) y `nlohmann/json` 3.12.0 en `include/`. La copia de `json.hpp` se comprobó byte a byte contra el tag `v3.12.0` (SHA-256 `aaf127c0…5de63`).
- **Alternativas descartadas**: el paquete `nlohmann-json3-dev` necesita `sudo` y fija la versión de la distribución.
- **Consecuencias**: el proyecto compila igual en cualquier máquina. Actualizarlas es reemplazar el fichero y verificarlo.

## D-006: Credenciales solo por variables de entorno
*06/10/2026 · Decisión de Plácido*

- **Decisión**: `TELEGARRM_API_ID` y `TELEGARRM_API_HASH` solo por entorno. Como servicio, se leen de `~/.config/telegarrm/env` (permisos `600`).
- **Alternativas descartadas**: guardarlas en la tabla `settings` (lo proponía `03_current_task.md`), porque los secretos acabarían en la BD y en sus copias de seguridad. Tampoco un `config.json` como el proyecto antiguo.
- **Consecuencias**: sin ellas, el servicio no arranca y da un error claro. Nunca se escriben en logs ni en la BD.

## D-007: Inicio de sesión de Telegram desde la web
*06/10/2026*

- **Contexto**: el proyecto antiguo pedía teléfono y código por terminal; `03_current_task.md` proponía hacerlo por CLI la primera vez.
- **Decisión**: endpoints `/api/telegram/auth/{phone,code,password}` y un formulario web guiado por el `authorizationState` de TDLib.
- **Motivo**: el objetivo del proyecto es no depender de SSH, y un servicio de systemd no tiene terminal.
- **Consecuencias**: hasta que la web tenga autenticación y HTTPS, el inicio de sesión viaja en claro por la red local (pendiente en ROADMAP, "Transversal"). Si Telegram cierra la sesión (ej. revocada desde otro dispositivo), se crea un cliente nuevo para volver a iniciarla desde la web.

## D-008: Sesión de TDLib en `db/tdlib` con permisos `0700`
*06/10/2026*

- **Decisión**: la sesión da acceso completo a la cuenta de Telegram, así que su carpeta se crea con permisos `0700`. `db/` está en `.gitignore`.
- **Consecuencias**: no debe copiarse ni subirse a ningún sitio. Las copias de seguridad de `db/` deben tratarse como secretas.

## D-009: Modelo de hilos de `TelegramClient`
*06/10/2026*

- **Decisión**:
  - Un único hilo llama a `td_receive`, porque TDLib no admite llamadas concurrentes.
  - Cada petición lleva un `@extra` con un identificador que la asocia a su *handler*. `send()` es asíncrono y `request()` es síncrono con timeout, para los hilos HTTP y de sincronización.
  - Al parar se envía `close` y se espera a `authorizationStateClosed` (máximo 10 s), para que TDLib guarde su base de datos.
- **Alternativas descartadas**: el esquema del proyecto antiguo (variables globales y esperas en bucle sin asociar respuestas).
- **Consecuencias**:
  - Los *handlers* se ejecutan en el hilo receptor y no pueden llamar a `request()`, porque se bloquearía.
  - Solo puede haber un `TelegramClient` por proceso.
  - Si TDLib se cierra durante el arranque (ej. base de datos bloqueada), no se reintenta, para evitar bucles; solo se recrea el cliente tras un cierre de sesión (`LoggingOut`).

## D-010: Parada ordenada con `sigwait` en un hilo dedicado
*06/10/2026*

- **Decisión**: SIGINT y SIGTERM se bloquean al principio de `main()`, antes de crear ningún hilo. Un hilo dedicado las espera con `sigwait` y llama a `svr.stop()`. El orden de parada es: servidor HTTP → sincronización → TDLib → BD.
- **Alternativas descartadas**: un manejador de señales que llame a `svr.stop()`. Funciona con httplib 0.59, pero depende de que `stop()` siga siendo seguro en un manejador en futuras versiones.
- **Consecuencias**: en Windows no hace nada (no es plataforma objetivo).

## D-011: Una sola instancia: `SO_REUSEADDR` y puerto reservado antes de TDLib
*06/10/2026*

- **Contexto**: cpp-httplib activa `SO_REUSEPORT` por defecto. Dos instancias podían escuchar a la vez en el puerto 8080, repartirse las peticiones y escribir en la misma BD.
- **Decisión**: usar solo `SO_REUSEADDR` (permite reiniciar enseguida) y reservar el puerto antes de arrancar TDLib.
- **Consecuencias**: una segunda instancia termina con código 1 sin tocar la sesión de Telegram de la primera.

## D-012: Servicio de usuario de systemd
*06/10/2026 · **Sustituida por [D-013](#d-013-servicio-de-sistema-aislado-y-regla-de-polkit)***

- **Decisión original**: servicio de usuario (`systemctl --user`), para poder reiniciarlo sin `sudo` durante el desarrollo.
- **Por qué se sustituyó**: necesitaba `loginctl enable-linger` para arrancar con la Pi sin sesión iniciada, el aislamiento de systemd era limitado y lo habitual en un servidor es un servicio de sistema (propuesta de Plácido).

## D-013: Servicio de sistema aislado y regla de polkit
*06/10/2026 · Propuesta de Plácido*

- **Decisión**:
  - **Unidad**: `/etc/systemd/system/telegarrm.service`, con `User=plax` y aislamiento:
    - `ProtectSystem=strict` y `ProtectHome=read-only`; solo `db/` tiene escritura.
    - Sin capacidades, `NoNewPrivileges`, filtros seccomp y `MemoryDenyWriteExecute`.
    - Solo las familias de red necesarias.
  - **Instalación**: `sudo ./deploy/install-service.sh`, que copia la unidad y la regla a `/etc` como ficheros de root. Se repite cuando cambie algo en `deploy/`.
  - **polkit**: la regla `deploy/50-telegarrm.rules` permite a `plax` hacer `start`, `stop` y `restart` *solo* de esta unidad sin `sudo`, para desplegar versiones nuevas. No permite habilitarla, modificarla ni tocar otras unidades.
- **Unidad copiada, no enlazada**: systemd la lee como root. Si fuera un enlace a la `home`, `plax` podría cambiar `User=` o `ExecStart=` y escalar privilegios.
- **Verificación**: antes de instalarlo se probó el binario real con las 24 propiedades del servicio mediante `systemd-run --user`. TDLib conecta, el sincronizador escribe en `db/` y la parada es ordenada. Dentro del proceso: `Seccomp: 2`, `NoNewPrivs: 1` y la raíz en solo lectura.
- **Verificación tras instalarlo** (06/10/2026):
  - Corre como `plax` con `CapEff` vacío, `NoNewPrivs: 1` y `Seccomp: 2`.
  - Solo `db/` se monta con escritura y la raíz es de solo lectura.
  - `systemd-analyze security` da una exposición de 3.1 ("OK").
  - `plax` reinicia el servicio sin `sudo` en unos 50 ms. polkit le niega `disable` y cualquier acción sobre otras unidades.
- **Consecuencias**:
  - Cambiar algo en `deploy/` exige volver a ejecutar el script con `sudo`.
  - En la Fase 3 hay que añadir la ruta de la biblioteca de series y películas a `ReadWritePaths`.
  - Los logs se consultan con `journalctl -u telegarrm`.

## D-014: SQLite: una conexión, WAL y migraciones versionadas
*06/10/2026*

- **Decisión**:
  - Una única conexión protegida por un `std::mutex`: hay pocas escrituras y así es más simple.
  - `journal_mode=WAL` y `synchronous=NORMAL`, para escribir menos en la tarjeta SD. En un corte de luz como mucho se pierde la última transacción, sin corromper la BD.
  - Migraciones numeradas con `PRAGMA user_version`. Nunca se edita una migración ya aplicada: los cambios van en una nueva.
  - `foreign_keys=ON`: quitar un canal borra sus mensajes en cascada.
- **Consecuencias**: el esquema evoluciona sin perder datos (la BD real pasó de v0 a v2 tras una copia de seguridad). Si en algún momento hay mucha concurrencia de lectura, se puede pasar a una conexión por hilo.

## D-015: Sincronización del historial de los canales
*06/10/2026*

- **Decisión**:
  - Hilo propio (`ChannelSync`) con peticiones síncronas a TDLib.
  - Se guardan todos los mensajes con texto o fichero, no solo los ficheros. Los textos y los pies de las portadas dan contexto al parser (nombre de la serie, temporadas).
  - De las fotos solo se guarda el pie. La imagen se pedirá a TDLib cuando haga falta (portadas del catálogo).
  - Lotes de 100 mensajes (máximo de TDLib) con 500 ms de pausa entre ellos. Se respetan los `FLOOD_WAIT` y se buscan mensajes nuevos cada 15 minutos.
  - El cursor del canal (mensaje más nuevo, más antiguo e historial completo) se guarda en la misma transacción que cada lote. Tras un reinicio continúa sin huecos ni duplicados.
- **Consecuencias**: aún no se detectan ediciones ni borrados de mensajes, ni hay escucha en tiempo real (Fase 4, `updateNewMessage`). El timeout de las peticiones (20 s) queda por debajo del `TimeoutStopSec` de systemd (30 s).

## D-016: Web sin frameworks ni paso de compilación
*06/10/2026*

- **Decisión**:
  - HTML, CSS y JavaScript sin dependencias, servidos por el propio binario.
  - Sondeo cada 2 s en lugar de WebSockets: suficiente en la red local y sin dependencias nuevas. Se revisará con la cola de descargas.
  - El contenido que viene de Telegram se pinta siempre con `textContent`, nunca con `innerHTML`.
- **Consecuencias**: nada que compilar ni instalar. Si la interfaz crece mucho, se valorará una librería pequeña sin paso de compilación.

## D-017: Selección de canales a cargo del usuario
*06/10/2026*

- **Decisión**: `/api/telegram/chats` lista canales, supergrupos y grupos (también los archivados) con el rol de la cuenta, sin chats privados. El usuario elige en la web qué vigilar; la aplicación no elige por él.
- **Contexto**: la cuenta no es propietaria de ningún canal, y el contenido parece repartido en canales de una sola serie (ej. `Generator Rex [Castellano]`). Falta que Plácido confirme cómo está organizado, porque afecta al diseño del parser y del catálogo.
- **Consecuencias**: para las pruebas solo se añadió temporalmente un canal de serie, que se quitó después junto con sus mensajes.

## D-018: Sin comandos de shell montados con cadenas
*06/10/2026*

- **Contexto**: el código antiguo usa `popen`/`system` concatenando nombres de fichero y tokens (`ffprobe`, `curl` contra TheTVDB). Un nombre con comillas permite inyectar comandos.
- **Decisión**: al migrarlo, procesos externos con `posix_spawn` y argumentos separados, y peticiones HTTP con el cliente de `cpp-httplib`.

## D-019: Localización y datos personales
*06/10/2026 · Normativa de la empresa (Andalucía, España, UE)*

- **Decisión**: unidades del Sistema Internacional (tamaños en kB/MB/GB y velocidades en MB/s, en base 1000), fechas `dd/mm/aaaa`, hora `Europe/Madrid` y textos en español. Los datos personales (teléfono, sesión) se quedan solo en la Pi y no hay telemetría.
- **Consecuencias**: la web formatea fechas y números con `es-ES`; la API devuelve fechas como segundos Unix (UTC) y tamaños en bytes.

## D-020: Catálogo en memoria derivado de los mensajes
*06/10/2026*

- **Contexto**: el catálogo (series, películas y sus archivos) se obtiene interpretando los mensajes guardados, y el parser irá mejorando con cada canal nuevo.
- **Decisión**: la clase `Catalog` lo calcula en memoria a partir de la tabla `messages`. Lo hace al arrancar y cada vez que cambian los mensajes de un canal: mensajes nuevos, cada 1000 mensajes durante una carga larga y al completar el historial. Cada canal guarda su lista de elementos como punteros compartidos inmutables, así que la API los lee sin copias ni bloqueos largos.
- **Alternativas descartadas**: tablas `media`/`episodes` en SQLite. Cada mejora del parser obligaría a migrar o recalcular datos guardados, y aún no hay nada que necesite persistir.
- **Consecuencias**: mejorar el parser solo requiere reiniciar el servicio. Calcular un canal de 100 mensajes es instantáneo; con miles habrá que medirlo. Los elementos se identifican por `(chat_id, id del mensaje de la ficha)`, un identificador estable que podrán usar las descargas (Fase 3) y el seguimiento (Fase 4). Esos datos sí se guardarán en tablas.

## D-021: Las fichas separan los elementos del catálogo
*06/10/2026 · **Sustituida por [D-026](#d-026-obras-versiones-y-partes)***

- **Contexto**: en los canales reales (`Ultimate Spider-Man`, `Generator Rex`) cada serie empieza con una foto con pie (la "ficha": título, año, calidad, géneros e idioma), seguida de los vídeos. Hay fotos sin pie que son portadas de temporada, y textos sueltos de cierre, créditos o enlaces. Cada autor usa un formato distinto.
- **Decisión**:
  - Cada foto con pie abre un elemento nuevo y los vídeos o comprimidos siguientes le pertenecen. Las fotos sin pie y los textos no cortan.
  - **Título**: primero el de la ficha; si no hay, el nombre de la serie en los ficheros; después el nombre del fichero (películas) y, en último caso, el título del canal sin `[etiquetas]`.
  - Es una **serie** si algún fichero tiene marcador de episodio (`1x01`, `#01x01`, `S01E01`, `T1E3`, `Temporada 1 Capítulo 3`); si no, es una **película**.
  - Un "título de episodio" repetido en más de la mitad de los episodios es el nombre de la serie (`1x01 - Ultimate Spiderman.mkv`) y se descarta.
- **Alternativas descartadas**: un elemento por canal, que fallaría con un canal "biblioteca" con muchas series o películas. Agrupar solo por el nombre del fichero tampoco sirve: hay canales cuyos ficheros no lo llevan.
- **Consecuencias**: funciona tanto con canales de una serie como con canales biblioteca. Un mismo episodio publicado dos veces (una versión mejorada) aparece dos veces en la ficha y cuenta como un solo episodio: la base para el reemplazo de la Fase 4. Pendiente de validar con películas reales.

## D-022: Tests con CTest, sin framework externo
*06/10/2026*

- **Decisión**: `tests/parser_tests.cpp` usa dos macros propias (`CHECK`, `CHECK_EQ`) y se registra en CTest. El código, salvo `main()`, se compila como biblioteca estática (`telegarrm_core`) que comparten el ejecutable y los tests. Los casos reproducen mensajes reales de los canales sincronizados.
- **Alternativas descartadas**: GoogleTest o Catch2, por ser una dependencia más que descargar y compilar en la Pi para unos pocos tests.
- **Consecuencias**: `ctest --test-dir build` tras compilar. Se comprobó que los tests detectan fallos (una copia con expectativas erróneas falla con mensajes claros). Si los tests crecen mucho, se puede adoptar un framework.

## D-023: Portadas descargadas bajo demanda con TDLib
*06/10/2026*

- **Decisión**: `GET /api/catalog/{chat}/{ficha}/poster` pide a TDLib la foto de la ficha. Elige el tamaño más pequeño de al menos 600 px de ancho (en la práctica, 853×1280 y unos 150 kB), la descarga si hace falta y la sirve con caché de navegador de una semana. TDLib la guarda en `db/tdlib`.
- **Alternativas descartadas**: descargar todas las portadas al sincronizar, porque se gastaría tráfico y disco en portadas que quizá nunca se vean.
- **Consecuencias**: la primera vista de una portada tarda unos 0,25 s; después, unos 0,02 s. Mientras se descarga, la petición ocupa un hilo del servidor HTTP; las imágenes usan `loading="lazy"` para no pedirlas todas a la vez.

## D-024: Búsqueda y navegación en el navegador
*06/10/2026*

- **Decisión**: `/api/catalog` devuelve el resumen de todos los elementos, y la web filtra y ordena en el navegador (sin distinguir mayúsculas ni acentos, ordenando con las reglas del español). La navegación usa rutas con `#` (`#/catalogo`, `#/catalogo/{chat}/{ficha}`, `#/canales`, `#/estado`). Sin sesión de Telegram solo se muestra *Estado*.
- **Alternativas descartadas**: búsqueda en el servidor, innecesaria para un catálogo de decenas o cientos de títulos.
- **Consecuencias**: búsqueda instantánea y sin peticiones. Si el catálogo llega a miles de títulos, habrá que paginar en la API.

## D-025: Temas de los foros
*06/10/2026*

- **Contexto**: el canal principal de Plácido ("Las Cositas 3", unos 33 000 mensajes) es un supergrupo con temas: *Películas*, *Películas 4K*, *Series*, *Series en emisión*, *Índices y colecciones*, *Avisos importantes* y *General*. Los mensajes de los distintos temas se publican intercalados en el tiempo.
- **Decisión**:
  - Cada mensaje guarda su tema (`messages.topic_id`, migración v3) y cada sincronización guarda la lista de temas con sus nombres (`getForumTopics`, tabla `topics`).
  - Las fichas se agrupan **dentro de cada tema**.
  - El nombre del tema orienta el tipo: "Películas…" → película, "Series…" → serie (los marcadores de episodio mandan).
  - Una obra presente en un tema "…en emisión" se marca como *en emisión*.
- **Alternativas descartadas**: agrupar por canal, que mezclaría archivos de un tema con la ficha de otro.
- **Consecuencias**: la migración v3 reinició los cursores para releer el historial una vez y rellenar el tema de los mensajes ya guardados (antes se hizo copia de la BD). La Fase 4 vigilará el tema "Series en emisión" para las descargas automáticas.

## D-026: Obras, versiones y partes
*06/10/2026 · Sustituye a D-021*

- **Contexto**: en el canal grande cada ficha es una temporada, un episodio en emisión (una ficha por episodio) o una versión de una película (1080p en *Películas* y 4K en *Películas 4K*). Además, los archivos grandes van troceados (`.zip.001`…, `.part01.rar`, `_part06.rar`). Plácido quiere elegir qué versión descargar.
- **Decisión**:
  - **Release** (archivo lógico): un vídeo, o todas las partes de un comprimido troceado, ordenadas por número aunque se publiquen desordenadas. Lleva calidad, HDR, etiquetas de versión (REMUX, Open Matte, SDR, IMAX, Extendida, Rotulado en castellano/inglés…), temporada y episodio.
  - **Item** (obra): une los bloques del mismo tipo con la misma clave de título o título alternativo, aunque estén en temas o canales distintos. Las películas homónimas con años distintos se separan (*La guerra de los mundos* de 2005 y de 2025).
  - El identificador de la obra es su ficha más antigua; `find()` acepta cualquiera de sus fichas, así que los enlaces no se rompen.
- **Consecuencias**:
  - El catálogo del canal grande pasa de unas 3200 fichas a unas 2200 obras: 294 películas con varias calidades y 9 series en emisión unidas a sus temporadas completas (*Outlander*: 8 temporadas y 93 episodios en una sola obra).
  - La Fase 3 descargará *Releases* concretos.
  - Las películas sin año en ninguna ficha pueden unirse por error con un *remake* (*Vaiana* de 2016 y de 2026). Se resolverá con TMDB (D-029).

## D-027: Qué archivos pertenecen a una ficha
*06/10/2026*

- **Contexto**: en *Películas 4K* muchas películas se suben sin ficha justo detrás de la ficha de otra. Con "todo lo que sigue a una ficha es suyo", la ficha de *Hokum* acabó con 53 películas.
- **Decisión**: un archivo pertenece al bloque abierto si:
  - es otra parte de un archivo troceado ya visto (mismo nombre base), o
  - es el primer archivo tras la ficha (aunque se llame distinto: "The Crow" para "El Cuervo"; su nombre pasa a valer para los siguientes), o
  - su nombre encaja con el título: igual, o uno contiene al otro (`vigilantes` en `myheroacademiavigilantes`; con claves de menos de 4 letras, solo como prefijo).

  Si no, abre un bloque sin ficha, que después se une con su ficha por título.
- **Alternativas descartadas**: usar el álbum de Telegram. Se probó y encadenaba obras, porque un mismo álbum puede llevar las últimas partes de una película y las primeras de la siguiente.
- **Títulos alternativos**: solo con contenido real (al menos 3 caracteres, sin conjunciones ni artículos). De `Hokum (1080p y 1080p REMUX)` quedaba "y", que unía cientos de películas. Las etiquetas técnicas (`Open Matte`, `REMUX`, `SDR`…) tampoco cuentan: `(Open Matte 1080p)` unía *Sonic* con *No es país para viejos*.
- **Otros errores reales corregidos**, todos con test de regresión:
  - El año ya no se toma de la sinopsis.
  - `…_2_0x264` ya no se interpreta como el episodio 0x264.
  - Los corchetes al principio son parte del nombre (`[REC] 2`).
  - `icase` de `std::regex` no convierte las mayúsculas acentuadas (`INGLÉS`), así que los patrones las incluyen.

## D-028: Compilación optimizada por defecto y caché del análisis
*06/10/2026*

- **Contexto**: reconstruir el catálogo del canal grande tardaba 25,5 s en cada arranque y bloqueaba la sincronización. La causa principal: CMake sin tipo de compilación no optimiza (`-O0`), y con `std::regex` eso lo hace entre 5,5 y 7 veces más lento (medido con los 29 816 nombres de fichero reales).
- **Decisión**:
  - `CMAKE_BUILD_TYPE=RelWithDebInfo` por defecto (`-O2` y con símbolos, para diagnosticar fallos).
  - Caché del análisis por mensaje, compartida entre las partes de un mismo archivo troceado.
  - Filtro previo por palabras clave antes de las expresiones de etiquetas.
  - El primer cálculo del catálogo lo hace el hilo de sincronización al arrancar, para no retrasar la web.
- **Consecuencias**: arranque en frío en 2,1 s (12 veces menos) y reconstrucciones incrementales por debajo de 1 s. Si el catálogo crece mucho más, el siguiente paso sería sustituir las expresiones más usadas por código a mano o por una biblioteca de expresiones más rápida.

## D-029: Metadatos de TheMovieDB (TMDB)
*06/10/2026 · Aprobada por Gemini y Plácido; implementada en D-030*

- **Contexto**: Plácido quiere títulos de episodios, descripciones y datos fiables de cada serie y película, y propuso TheTVDB o TheMovieDB.
- **Decisión propuesta**: TMDB.
  - Gratuito para uso **no comercial**, con atribución: logo de TMDB y el aviso *"This product uses the TMDB API but is not endorsed or certified by TMDB"* en una sección de créditos de la web.
  - Cubre series y películas, tiene datos en español (`es-ES`): títulos, sinopsis, episodios y carátulas.
  - 220 obras ya traen su identificador de TMDB en el nombre de los archivos (`tmdbid_8078`), lo que da una coincidencia exacta.
  - La credencial irá en `TELEGARRM_TMDB_TOKEN`, en el mismo fichero de entorno (D-006).
  - Las respuestas se guardarán en una caché en SQLite, para no repetir consultas.
- **Alternativas evaluadas** (06/10/2026, comparando los datos reales de *Ted Lasso*):

  | Fuente | Licencia / coste | Cobertura | En castellano |
  | --- | --- | --- | --- |
  | **TMDB** | Gratis para uso no comercial con atribución; uso comercial de pago | Series y películas, carátulas | Sinopsis y títulos de episodio ("Piloto", "Pastitas"…) |
  | **TVmaze** | CC BY-SA 4.0: libre para cualquier uso con atribución | Solo series, sin películas | No: títulos y resúmenes en inglés ("Pilot", "Biscuits"…) |
  | **Wikidata** | CC0 (dominio público) | Series y películas; pocos episodios; sin carátulas (derechos de autor) | Título y descripción breve ("serie de televisión de comedia estadounidense"); solo 11 de 44 episodios con título en castellano |
  | **TheTVDB** | API v4: PIN de suscriptor de pago para proyectos personales | Solo series | — |

  No hay ninguna fuente abierta que iguale a TMDB en castellano (títulos de episodio, sinopsis y carátulas).
- **Cómo reducir la dependencia de TMDB** (por si deja de ser gratuito o cambia sus condiciones):
  1. Telegarrm funciona sin TMDB: el catálogo sale de las fichas de Telegram (título, año, calidad y, en el canal grande, sinopsis). TMDB solo lo enriquece.
  2. Todo lo obtenido se guarda en local (SQLite y carátulas en disco): lo ya consultado se conserva aunque TMDB cierre.
  3. De cada obra se guardan sus identificadores externos (IMDb, TVDB, Wikidata), que TMDB proporciona. Con ellos, cambiar de fuente sería un cruce exacto.
  4. Los metadatos se piden a través de un "proveedor" intercambiable: TMDB es el primero. Wikidata (CC0) y TVmaze (CC BY-SA) se podrían añadir como alternativa o complemento.
- **Consecuencias**: solo se envían a TMDB títulos y años, nunca datos personales. Si el proyecto llegara a tener uso comercial, habría que pedir licencia a TMDB.

## D-030: Cómo se integra TMDB
*06/10/2026 · Implementa D-029*

- **Decisión**:
  - **`TmdbClient`**: cliente HTTPS (cpp-httplib con OpenSSL, verificando el certificado con los del sistema), una petición cada vez y como mucho unas 10 por segundo.
    - Cada respuesta, también los 404, se guarda en `tmdb_cache` (migración v4), y si TMDB falla se usa la guardada aunque haya caducado.
    - El token solo va en la cabecera `Authorization`: nunca en la clave de la caché ni en los logs.
    - Las carátulas se guardan en `db/tmdb/images`. Solo se aceptan rutas con la forma `/abc.jpg`, para que no se pueda salir de la carpeta.
  - **`MetadataService`**: un hilo que recorre el catálogo, primero las series en emisión y lo más reciente.
    - Usa el `tmdbid` del nombre de los archivos si lo hay; si no, busca por título y títulos alternativos, con y sin año.
    - **Puntuación**: título exacto (castellano u original) 100 puntos y contenido 50; año igual +30, a un año +15 y distinto −40. Se acepta desde 80, es decir, título exacto o contenido con el mismo año: mejor sin datos que con los de otra obra.
    - **Qué guarda** (tablas `metadata` y `metadata_episodes`): identificadores (TMDB, IMDb, TVDB, Wikidata), título, título original, sinopsis, géneros, carátula y los episodios de las temporadas presentes en el catálogo.
  - **Cada obra se identifica** por `tipo|título normalizado|año`. Si el parser cambia el título o el año, se vuelve a buscar, casi sin coste gracias a la caché.
  - **Validez**: búsquedas y películas 30 días, series 7 días, series en emisión 1 día; las obras sin coincidencia se reintentan al mes.
  - **En la web**: la sinopsis de TMDB tiene preferencia sobre la de la ficha, y los títulos y sinopsis de episodio vienen de TMDB. La carátula es la foto de la ficha y, si no hay, la de TMDB. Los créditos de TMDB (logo y aviso) van en el pie de todas las páginas.
- **Consecuencias**:
  - La primera pasada sobre ~2200 obras dura unos 16 minutos (unas 2,4 obras/s). En las primeras 100, el 95 % encontró coincidencia.
  - **Sin token** (comprobado): no se hace ninguna petición, `/api/status` informa `metadata.enabled = false` y la web sigue mostrando lo ya guardado (601 obras en la prueba, *Ted Lasso* con sus títulos de episodio en castellano).
  - **Pendiente**: usar TMDB para separar *remakes* que el catálogo une (D-026).

## D-031: Cola de descargas
*06/10/2026 · Inicio de la Fase 3*

- **Decisión**:
  - **Qué se encola**: tablas `downloads` y `download_parts` (migración v5). Cada descarga es un archivo lógico del catálogo (D-026) con todas sus partes. `POST /api/downloads` recibe solo `chat_id` y `message_id`, y el resto se toma del catálogo, nunca del navegador. Solo puede haber una descarga activa (en cola, descargando o descargada) por archivo.
  - **`DownloadManager`**: un hilo que descarga de una en una, pidiendo a TDLib todas las partes y siguiendo el progreso cada segundo (se guarda cada 5 s).
    - Antes de empezar comprueba que cabe, con 2 GB de margen, para no llenar la tarjeta SD.
    - Reintenta las partes que se detienen (hasta 5 veces) y falla si no avanza en 15 minutos.
    - Tras un reinicio, lo que se estaba descargando vuelve a la cola y TDLib continúa donde lo dejó.
    - Cancelar para la descarga, **borra lo descargado a medias** y deja el progreso a cero.
  - **Dónde quedan los archivos**: en la caché de TDLib (`db/tdlib/documents`), porque la biblioteca final (disco y carpetas) aún no está definida.
  - **Web**:
    - Botón "Almacenar en disco" en cada versión, y "Temporada completa" en cada versión de una temporada.
    - Pestaña *Descargas* con progreso, velocidad, tiempo restante y acciones (cancelar, reintentar, quitar).
- **Alternativas descartadas**: descargar varios archivos a la vez, porque TDLib ya reparte cada archivo en varias conexiones (llegó a unos 19 MB/s) y una cola secuencial es más predecible en una Raspberry Pi.
- **Consecuencias**: probado con archivos reales de 20 MB (unos 2 s) y 251 MB (cancelado y reintentado), más tests de la cola sobre una BD temporal. Siguiente paso de la Fase 3: mover y descomprimir lo descargado en la biblioteca (D-018: sin shell) y liberar la caché de TDLib. Para eso hay que definir la ruta de la biblioteca y añadirla a `ReadWritePaths` del servicio (D-013).

## D-032: Ajustes desde la web y reinicio con el código 75
*07/10/2026 · A petición de Plácido*

- **Contexto**: el búfer de descargas debe poder estar en otro disco (en el servidor, el RAID), no en la tarjeta SD ni en el SSD del sistema. En TDLib es `files_directory`, que solo se fija al arrancar el cliente.
- **Decisión**:
  - **Pestaña *Ajustes***, con los valores en la tabla `settings`: búfer de descargas, bibliotecas de películas y de series, y espacio libre mínimo.
  - **Validación al guardar**: cada ruta debe ser absoluta, existir, ser una carpeta y admitir escritura. La escritura se comprueba creando y borrando un fichero de prueba, porque con el aislamiento de systemd los permisos no bastan. Si la bloquea el aislamiento (`EROFS`), el mensaje indica el comando que la permite.
  - **Avisos útiles**: la web muestra el espacio libre de cada ruta y avisa si el búfer y las bibliotecas están en discos distintos, porque entonces cada archivo se escribiría dos veces.
  - **Aplicar el búfer**: hace falta reiniciar. "Reiniciar ahora" (`POST /api/restart`) hace la parada ordenada y sale con el código 75. La unidad lo marca como salida correcta y fuerza el reinicio (`SuccessExitStatus=75`, `RestartForceExitStatus=75`, `RestartSec=3`).
  - **Si el búfer configurado no se puede usar** al arrancar (disco sin montar, sin permiso), se arranca con el predeterminado (`db/tdlib`) y la web lo avisa, en lugar de fallar.
  - El margen de espacio libre se lee en cada descarga, así que cambiarlo no exige reiniciar.
- **Alternativas descartadas**: recrear el cliente de TDLib en caliente. Es más complejo (descargas en curso, sincronización) y un reinicio ordenado de unos segundos lo resuelve todo.
- **Permisos de escritura**: la unidad permite escribir en `db/` y en `/srv/media` (opcional, D-033). Otras rutas se añaden con `sudo ./deploy/install-service.sh /ruta…`, que las guarda en `/etc/systemd/system/telegarrm.service.d/rutas.conf` (solo rutas absolutas con caracteres seguros).
- **Verificación**: las validaciones dan los mensajes esperados (ruta relativa, inexistente, de solo lectura por el aislamiento, margen negativo). Un búfer de prueba se aplicó con "Reiniciar ahora" y una descarga real cayó en él; después se restauró el predeterminado.
- **Efecto secundario descubierto al usarlo**: TDLib guarda las rutas de los archivos *relativas* a `files_directory`. Al cambiar el búfer, lo descargado en el anterior no se reutiliza: si hace falta, TDLib lo vuelve a bajar, y lo antiguo queda huérfano y se puede borrar. Lo avisa la ayuda de *Ajustes*.

## D-033: Estructura de carpetas `/srv/media` y Jellyfin
*07/10/2026 · Aceptada por Plácido (creó `/srv/media` en la Pi)*

- **Contexto**:
  - El destino final es un servidor con un SSD para el sistema y un RAID para los datos.
  - Plácido quiere que todas las escrituras grandes ocurran en el RAID.
  - Jellyfin irá en Docker; sus imágenes y datos están en `/opt/docker`.
- **Propuesta**: montar el RAID en `/srv/media`. Según el estándar de jerarquía de ficheros (FHS), `/srv` es para los datos que sirve el sistema. `/opt/docker` se queda para la configuración de los contenedores, en el SSD.
  ```
  /srv/media/               <- RAID
  ├── descargas/            <- búfer de TDLib (Ajustes → Búfer de descargas)
  ├── peliculas/            <- biblioteca de películas (Jellyfin)
  └── series/               <- biblioteca de series (Jellyfin)
  ```
  - **Mismo sistema de archivos para todo**: pasar lo descargado a la biblioteca es un renombrado instantáneo, sin escribir los datos otra vez. Solo los comprimidos se escriben una segunda vez, al extraerlos.
  - **Lo pequeño se queda en el SSD**: la base de datos de Telegarrm, la sesión de TDLib y la caché de TMDB (`db/`).
  - **Jellyfin** monta solo las bibliotecas, en solo lectura (`/srv/media/peliculas:/media/peliculas:ro`), y no ve el búfer. Se ejecuta con el usuario de Telegarrm (`user: "1000:1000"`) o con un grupo compartido. Telegarrm crea los ficheros con `UMask=0027`, así que el grupo puede leerlos.
- **Para el postproceso** (siguiente paso de la Fase 3), nombres que Jellyfin reconoce sin ambigüedad, con el identificador de TMDB que ya tenemos (D-030):
  - `peliculas/Título (Año) [tmdbid-N]/Título (Año).mkv`
  - `series/Serie (Año) [tmdbid-N]/Season 01/Serie S01E01.mkv`
  
  Se usa `Season`, no `Temporada`, porque es lo que Jellyfin detecta con seguridad.
- **En la Pi** (solo tiene la tarjeta SD), la misma estructura en la SD sirve para probar el flujo completo. En el servidor, al montar el RAID en `/srv/media`, no hay que cambiar ningún ajuste.

## D-034: Importación a la biblioteca
*07/10/2026 · Cierra la Fase 3*

- **Decisión**: al terminar una descarga, `DownloadManager` la pasa a *importing* y `library::importRelease`:
  1. **Descomprime** con 7-Zip si es un comprimido (`.zip.001`…, `.partN.rar`, `.7z`), en una carpeta temporal oculta dentro de la biblioteca (`.telegarrm/<id>`), en el mismo disco que el destino.
     - 7-Zip se lanza **sin shell** (D-018) con `posix_spawn`: argumentos separados, `--` antes del nombre, la entrada estándar vacía (un comprimido con contraseña falla en vez de quedarse esperando) y sin heredar los descriptores del servicio.
     - El hijo recibe la máscara de señales vacía: el servicio bloquea SIGTERM en todos sus hilos (D-010) y, si el hijo la heredara, no se le podría parar.
     - El progreso (`-bsp1`) se muestra en la web.
  2. **Elige los vídeos**: descarta las muestras (con "sample" o "muestra" en el nombre y menos del 30 % del mayor) y el resto (`.nfo`, imágenes…). Los subtítulos sueltos se colocan junto al vídeo cuando solo hay uno.
  3. **Renombra para Jellyfin** con título, año e identificador de TMDB si los hay (D-030); si no, con los del catálogo:
     - `peliculas/Título (Año) [tmdbid-N]/Título (Año) - 4K HDR.mkv`. La etiqueta de versión permite tener varias versiones de una misma película.
     - `series/Serie (Año) [tmdbid-N]/Season 01/Serie S01E01 - 1080p.mkv`, con `S01E02-E03` si son varios episodios. Los vídeos sin episodio de una serie van a `extras/`.
     - Se quita lo que no vale en un nombre de fichero: `:` se convierte en ` -`, y `/ \ * ? " < > |`, los caracteres de control y los puntos finales desaparecen. La longitud se limita sin partir caracteres UTF-8.
  4. **Mueve**: un renombrado si es posible. Si no (`EXDEV`), copia a un temporal, lo renombra y borra el original.
     - Si el destino ya existe con el mismo tamaño, se da por importado, así que reintentar no duplica nada. Si es distinto, se numera: `Nombre (2).mkv`.
     - Al terminar se borran las partes del búfer y la carpeta temporal.
- **Hallazgo**: dentro del aislamiento de systemd, cada `ReadWritePaths` es un punto de montaje distinto. `rename()` falla entre ellos (`EXDEV`) aunque estén en el mismo disco. Por eso la comprobación de *Ajustes* prueba a renombrar de verdad un fichero entre el búfer y cada biblioteca, y la estructura de D-033 pone todo bajo `/srv/media` (un único punto de montaje).
- **Estados de una descarga**: `queued` → `downloading` → `importing` → `completed` (en la biblioteca, con `library_path`). Si falla, `failed` con un motivo claro. Reintentar no vuelve a descargar lo que ya está en el búfer. La migración v6 devolvió a la cola las descargas terminadas antes de existir la importación, para importarlas.
- **Criterio de TMDB, versión 2**: en series, un año distinto penaliza poco (−10), porque el año de la ficha suele ser el de la temporada (*Ultimate Spider-Man*: 2015 en la ficha, 2012 en TMDB). Al cambiar el criterio, las obras sin coincidencia se vuelven a buscar una vez.
- **Verificación**:
  - Tests con un ZIP troceado de verdad (episodios a `Season 01`, la muestra y el `.nfo` fuera, búfer y temporales limpios) y con el lanzador de procesos (un argumento `"hola; rm -rf /"` llega tal cual).
  - En la Pi, *Toy Story Toons: Fiestasaurio Rex* (ZIP de 451 MB) se descargó en ~13 s y se importó en ~4 s.
- **RAR**: Debian distribuye 7-Zip sin el códec de RAR (licencia no libre). Hace falta el paquete `7zip-rar`, que Plácido instaló el 07/10/2026. Comprobado con *Westworld 4x04*, un RAR de 2 partes y 1,86 GB: se descargó en unos 46 s (40–70 MB/s), se descomprimió en unos 20 s y quedó en `Westworld (2016) [tmdbid-63247]/Season 04/Westworld S04E04 - 1080p.mkv`, con permisos `640` y sin restos. Sin el códec, la importación falla con un mensaje que indica el paquete y se puede reintentar sin volver a descargar.
- **Nota**: TDLib guarda todos sus archivos en el búfer, también las fotos de las fichas que la web pide como carátulas (`photos/`, unos 7 MB para 40 portadas). No interfiere con la importación.

## D-035: Seguimiento de series y películas
*07/10/2026 · Fase 4*

- **Contexto**: la Fase 4 (docs/05_current_task.md) pide que el servicio funcione solo: descargar los episodios nuevos de lo que se sigue y sustituir una película o episodio descargado cuando llega una versión mejor.
- **Decisión**:
  - **Tabla `follows`**: una fila por obra seguida, con tipo, título, año, identificador de TMDB, clave de obra (`tipo|título|año`, la de `MetadataService`) y una ficha de la obra (`chat_id`, `anchor_id`). La obra se reencuentra en el catálogo por cualquiera de sus fichas; si ya no está, por TMDB y después por la clave. Si la ficha principal cambia, se actualiza sola.
  - **Solo cuenta lo que se publica después de seguir** (`created_at`). Seguir no descarga el catálogo antiguo; para eso están los botones de temporada.
    - **Series**: un episodio es nuevo si su primera publicación en el catálogo es posterior a seguir la serie. Así, si un canal vuelve a subir la temporada completa, los episodios antiguos que no se descargaron no se bajan.
    - **Películas**: si no está descargada, se descarga la primera versión que se publique después de seguirla; si ya lo está, solo las mejoras.
  - **Mejoras**: una versión publicada después de seguir sustituye a la que se tiene si su rango es mayor. Rango = resolución, después HDR y después REMUX. Las ediciones (Extendida, IMAX, Open Matte…) no son mejoras: son otras versiones. Las versiones en 3D nunca se eligen solas.
  - **Calidad máxima** por obra seguida (`max_quality`): «la mejor», «hasta 1080p» o «hasta 720p». Responde a «no siempre voy a querer las dos» (1080p y 4K).
  - **Archivos incompletos**: un comprimido troceado se publica en varios mensajes y puede tardar minutos en estar entero. Se espera mientras falten números de parte o todas midan lo mismo (la última suele ser más pequeña). Si no hay forma de saberlo, se espera 2 h desde la última parte. Si la mejor versión de un episodio aún está incompleta, se espera por ella en vez de bajar una peor.
  - **Una sola vez**: un archivo que el seguimiento ya puso en cola (tabla `auto_releases`) o que tiene cualquier descarga, aunque fallara o se cancelara, no se vuelve a poner en cola solo. Cancelar una descarga automática es definitivo; reintentarla, manual.
  - Si llega una versión mejor mientras la anterior aún está en cola sin empezar, la anterior se cancela.
- **Descartado**:
  - Comparar códec o audio: los nombres del canal casi nunca los llevan y un x265 no es mejor que un x264 por sí mismo.
  - Leer el número de partes de la ficha («Son 3 partes en rar»): las fichas con varias versiones lo dan por versión («4K: Son 3 partes… Remux: Son 5…»).
  - Seguir obras que aún no están en el catálogo.
- **Consecuencias**: la evaluación (`tracking::plan`) es una función pura, probada con catálogos de ejemplo. La ejecuta `Tracker` en su hilo cuando cambia el catálogo, cuando se sigue algo y cada 5 min si hay algo esperando.
- **Verificación** (07/10/2026):
  - Tests con el formato real del tema «Series en emisión»: solo el episodio posterior a seguir; espera mientras el `.part2.rar` de un episodio en 3 partes es la última parte; la temporada vuelta a subir no cuenta; 4K como mejora de 1080p (cancela si la 1080p seguía en cola); límite de 1080p; películas y 3D.
  - En la Pi, con la fecha del seguimiento retrasada a mano en la BD para simular que ya se seguía:
    - *Presidente Curtis*, seguida desde el 21/09/2026 a las 16:00: se puso en cola solo el 1x09 (los 1x01–1x08 son anteriores). Se descargó en 20 s y quedó en `Season 01/Presidente Curtis S01E09 - 1080p.mkv`.
    - Volver a revisar no repite nada.

## D-036: Mensajes nuevos en tiempo real
*07/10/2026 · Fase 4*

- **Contexto**: hasta ahora los mensajes nuevos se leían cada 15 min. La Fase 4 pide reaccionar al momento sin saturar TDLib.
- **Decisión**:
  - Se llama a `openChat` en cada canal vigilado. TDLib solo recibe todas las actualizaciones de canales y supergrupos de los chats abiertos. No marca nada como leído (eso lo hace `viewMessages`). Al dejar de vigilar un canal, se cierra con `closeChat`.
  - `TelegramClient` reparte las actualizaciones a quien se suscribe. `ChannelSync` solo mira `updateNewMessage` de los canales vigilados y apunta el canal; no hace peticiones desde el hilo de TDLib.
  - **Agrupación**: se sincroniza 20 s después del último mensaje nuevo, y como mucho 2 min después del primero. Una película en 5 partes provoca una sincronización, no cinco. La sincronización rápida solo pide los mensajes nuevos de esos canales (`getChatHistory`), sin releer los temas.
  - El mensaje se guarda por el camino de siempre (`saveSyncBatch` con su cursor), en vez de convertir la actualización y guardarla aparte: un único escritor y ningún hueco si se pierde alguna actualización. Después, el catálogo lo analiza con el parser, TMDB lo busca y el seguimiento lo evalúa.
  - La ronda de cada 15 min se mantiene. Recupera lo que se pierda (servicio parado, cortes de red) y relee los temas.
- **Verificación** (07/10/2026): en el canal de prueba «Prueba Claude», con la serie seguida, Plácido subió `1x02 - Ultimate Spiderman.mkv` (364 MB):
  - 20:07:09: aviso `updateNewMessage`.
  - 20:07:29: sincronización rápida tras 20 s de calma. Catálogo recalculado en 121 ms, ya con el nombre «Ultimate Spiderman» (D-038). En la misma pasada, el seguimiento pone el 1x02 en cola y TMDB encuentra la serie.
  - Unos 10 s después: descargado e importado en `Ultimate Spider-Man (2012) [tmdbid-34391]/Season 01/`, junto al 1x01.
- **Descartado**: guardar directamente el mensaje de la actualización. Duplicaría la conversión y podría dejar el cursor por delante de mensajes que no llegaron como actualización.

## D-037: Sustitución de versiones e historial de actividad
*07/10/2026 · Fase 4*

- **Decisión**:
  - **Qué archivos colocó cada descarga**: columna `library_files` de `downloads`. Hace falta para borrar exactamente la versión anterior y no otra cosa.
  - **Al mejorar**, la descarga nueva lleva en `replaces` las que sustituye. Solo cuando la nueva está en la biblioteca, el gestor de descargas trata las anteriores que sigan *completed*:
    - Borra sus archivos, salvo que *Ajustes* diga conservarlos (`keep_replaced`; Jellyfin muestra entonces las dos versiones).
    - Las marca como `replaced`.
    - Solo se borran ficheros normales dentro de las carpetas de la biblioteca y que no sean de la versión nueva. Después se quitan las carpetas que queden vacías, sin subir más allá de la biblioteca.
  - **Tabla `activity`**: registra lo que hace el sistema solo. Episodio o película en cola, mejora en cola, descarga automática terminada o fallida, versión sustituida. También seguir y dejar de seguir, para entender después por qué se descargó algo. Se conservan las 5000 entradas más recientes.
  - **Web**: botón «Seguir» en cada ficha, con la calidad máxima. Filtro «En seguimiento» en el catálogo. Pestaña *Actividad* con lo que se sigue y el historial.
  - Las descargas guardan su origen (`manual` / `auto`) y la obra seguida que las pidió (`follow_id`).
- **Verificación** (07/10/2026): tests de `removeFiles` (no toca nada fuera de la biblioteca, ni enlaces, ni la versión nueva; quita las carpetas vacías sin borrar la raíz). En la Pi, *El show de los Muppets - Especial*:
  1. Se descargó a mano en 1080p.
  2. Al seguirla (con la fecha retrasada a antes de su publicación), el seguimiento pidió la 4K HDR como mejora.
  3. Se descargaron 3,7 GB en 73 s y se descomprimieron en 46 s.
  4. Quedó `El show de los Muppets (2026) - 4K HDR.mkv`, se borró la 1080p y la descarga anterior pasó a *Sustituida*.
  5. El historial lo cuenta: «… ya está en la biblioteca y sustituye a la versión 1080p (1 archivo borrado)».
- Las tres descargas anteriores a la migración v7 recibieron su `library_files` a mano, para que también se puedan sustituir.

## D-038: Nombre de la serie repetido en los archivos
*07/10/2026 · Fase 4*

- **Contexto**: Plácido creó el canal «Prueba Claude» para probar el seguimiento y subió `1x01 - Ultimate Spiderman.mkv`, sin ficha. En el formato `1x01 - Texto`, el texto tras el marcador puede ser la serie o el título del episodio. Sin ficha ni nombre de serie en el fichero, el catálogo usaba el nombre del canal: la serie se llamaba «Prueba Claude» y TMDB no la encontraba. En el canal original funcionaba porque había ficha y el canal se llamaba como la serie.
- **Decisión**: sin ficha ni nombre de serie en los ficheros, si el texto tras el marcador se repite en más de la mitad de los episodios, y como mínimo en dos, es el nombre de la serie. Si el canal se llama igual, se queda el título del canal, que suele estar mejor escrito («Spider-Man»). Con un solo episodio no se puede saber y sigue mandando el canal.
- **Consecuencias**: la obra no cambia de identidad al cambiar de nombre (la identifica su primer archivo), así que un seguimiento hecho antes de que llegue el segundo episodio sigue funcionando y su título se actualiza solo.

## D-039: Calidad real del vídeo con ffprobe
*07/10/2026 · Fase 4*

- **Contexto**: la calidad salía solo del nombre del fichero o de la ficha. Los dos episodios de *Ultimate Spider-Man* de la biblioteca resultaron ser 720p (1280×720). El 1x01 se había importado como «1080p» porque así lo decía la ficha del canal original, y el 1x02 como «calidad desconocida». Para el seguimiento (D-035) «desconocida» es la peor calidad, así que un 720p publicado después habría pasado por mejora.
- **Decisión**:
  - Al importar, cada vídeo se analiza con `ffprobe` (paquete `ffmpeg`), lanzado sin shell (D-018). Se usan el ancho y el alto del primer flujo de vídeo y su transferencia de color.
  - **Resolución**: se mira también el ancho, por las películas panorámicas (1920×800 es 1080p). 2160p desde 3200 de ancho o 1800 de alto; 1080p desde 1600 o 900; 720p desde 1100 o 650; después 576p, 480p y 360p.
  - **HDR**: transferencia PQ (`smpte2084`) o HLG (`arib-std-b67`), o metadatos de Dolby Vision. Si ffprobe no da información de color, se mantiene lo que diga el nombre.
  - Lo que diga el archivo manda sobre el nombre y la ficha. Se usa para el nombre en la biblioteca («- 720p») y se guarda en la descarga, que es lo que compara el seguimiento. Las etiquetas (REMUX, Extendida…) siguen saliendo del nombre.
  - Las descargas ya terminadas se analizan una vez al arrancar (ajuste `quality_probe_version`) para corregir su calidad en la BD. Sus archivos no se renombran.
  - Sin `ffprobe`, todo sigue como antes (calidad del nombre).
- **Descartado**: `mediainfo`, que añade otra dependencia. `ffprobe` ya viene con `ffmpeg`, que Plácido instaló el 07/10/2026.
- **Verificación**:
  - Tests con salidas reales de ffprobe (720p, 4K PQ, Dolby Vision, archivo roto) y una importación con un vídeo de 1280×720 generado con `ffmpeg` y llamado «1080p»: queda como `S01E02 - 720p.mkv`.
  - En la Pi, al arrancar se corrigieron los dos episodios de *Ultimate Spider-Man* (720p). El resto (1080p, 4K HDR) ya era correcto.
  - Cada análisis tarda unos 0,1 s y funciona dentro del aislamiento de systemd.

## D-040: Descargar los episodios que faltan (o la serie completa)
*07/10/2026 · Fase 4*

- **Contexto**: Plácido quiere saber si tiene todos los episodios de una serie y, si no, descargar los que faltan de una vez, sin ir temporada por temporada. También pidió un botón de serie completa.
- **Decisión**:
  - Son el mismo botón. En la ficha de una serie, el recuadro «Serie completa» dice cuántos episodios conocidos hay descargados o en cola y ofrece «Descargar los N que faltan». Si no se tiene ninguno, el botón dice «Descargar la serie completa».
  - Por cada episodio que falta se elige su mejor versión dentro de la calidad elegida (la del seguimiento si se sigue la serie) y sin 3D. Un archivo con varios episodios (1x01-02) cubre todos los que trae.
  - Un episodio cuenta como «tenido» si alguna de sus versiones está en cola, descargándose, importándose o en la biblioteca. Una descarga fallida o cancelada no cuenta.
  - Lo elige el servidor (`POST /api/catalog/{chat}/{ficha}/download`) con la misma lógica que el seguimiento (`tracking::missingEpisodes`), probada con tests. Antes comprueba que caben en la biblioteca de series con el margen de *Ajustes*.
- **Verificación**: en la Pi, la serie de prueba muestra «Tienes 1 de 2 episodios conocidos» y el botón para el que falta. Westworld (35 episodios, 143 GB) se rechaza porque no cabe en los 101 GB libres, sin poner nada en cola.

## D-041: La biblioteca dice qué episodios se tienen
*07/10/2026 · Fase 4*

- **Contexto**: «Descargar el episodio que falta» bajó otra vez el 1x01 de *Ultimate Spider-Man*, que ya estaba en la biblioteca. Era una descarga del canal original, que Plácido había quitado: su archivo ya no estaba en el catálogo y «lo que se tiene» solo salía de las descargas que el catálogo reconoce. Quedaron dos archivos idénticos con etiquetas distintas: «- 1080p», de la ficha falsa del canal original, y «- 720p», leída del vídeo (D-039).
- **Decisión**:
  - **Lo que se tiene** de una obra son sus descargas en curso o terminadas más los vídeos que ya hay en su carpeta de la biblioteca. La carpeta se busca por `[tmdbid-N]` (también `[tmdbid=N]`, la otra forma que entiende Jellyfin) o, si no, por su nombre. Así cuentan los archivos de canales quitados, los copiados a mano y los de otras herramientas.
    - El episodio sale del nombre del archivo (`S01E01`, `1x01`…).
    - La calidad sale de la etiqueta del nombre o, si no la lleva, del propio vídeo con ffprobe. Ese análisis se guarda en memoria por ruta, tamaño y fecha.
    - Lo usan el recuadro «Serie completa» (D-040) y el seguimiento (D-035).
  - **Un archivo por episodio**: al importar un episodio, los otros vídeos del mismo episodio en esa carpeta, con sus subtítulos, se sustituyen. Se borran salvo que *Ajustes* diga conservarlos (`keep_replaced`), y las descargas que los colocaron pasan a *Sustituida*. Así, si se cuela un duplicado, se queda el más reciente, como pidió Plácido.
  - En **películas** sí puede haber varias versiones a propósito (1080p y 4K). Solo se sustituyen cuando la descarga la pidió el seguimiento como mejora.
  - La ficha marca los episodios que ya están en la biblioteca, con su calidad.
- **Descartado**: guardar en cada descarga el identificador de TMDB para reconocerla sin el catálogo. No cubre los archivos que no vienen de una descarga, y la carpeta de la biblioteca sí.

## D-042: Calidad real antes de descargar
*07/10/2026 · Fase 4*

- **Contexto**: Plácido preguntó si se puede saber la calidad de un archivo antes de descargarlo, en vez de fiarse de lo que dice el canal. Ya había fichas que mentían (D-039).
- **Decisión**: TDLib permite descargar solo un trozo de un archivo (`downloadFile` con `offset` y `limit`). `ReleaseProber` pide los primeros 4 MB de la primera parte, saca de ahí el principio del vídeo, lo analiza con ffprobe (D-039) y borra el trozo del búfer (`cancelDownloadFile` y `deleteFile`).
  - **Dónde está el vídeo** en esos bytes:
    - Un MKV, MP4, AVI o FLV suelto: está al principio.
    - Un ZIP: se recorren sus cabeceras locales hasta el vídeo. Si lo guarda sin comprimir se copia; si usa deflate (casi todos los de los canales), se descomprime con zlib lo que haya.
    - Un RAR: si guarda el vídeo sin comprimir, está tal cual; si lo comprime, 7-Zip descomprime el principio aunque el archivo esté cortado (`7z e -so`, sin shell) y entrega lo que puede.
  - **Si no basta**:
    - Un vídeo suelto pide además un principio de 16 MB y su final. En un MP4 va primero el final (su índice suele estar ahí); en el resto, primero el principio largo (los MKV pueden llevar adjuntos delante).
    - Dentro de un comprimido solo se puede pedir el principio largo.
    - TDLib guarda cada trozo en su sitio, así que ffprobe analiza el archivo a medias.
  - **Resultado**: tabla `release_probes`, por chat y primera parte. Al importar se guarda también la calidad leída del vídeo entero. Los fallos del propio archivo se recuerdan y los de red no.
  - **Dónde se usa**:
    - La ficha muestra la calidad comprobada con «✓», con un botón «?» por versión y «Comprobar calidades» por temporada o película.
    - «Serie completa» (D-040) y la descarga manual eligen con ella.
    - El seguimiento (D-035) comprueba antes de decidir lo que podría pedir (como mucho 10 archivos por pasada), así que las mejoras y la calidad máxima se aplican sobre la calidad real.
  - Nunca se comprueba un archivo que se está descargando: otra petición `downloadFile` cambiaría su rango. Las comprobaciones van de una en una y con menos prioridad que las descargas.
- **Verificación** (07/10/2026), con 23 archivos variados de *Las Cositas* sin descargar:
  - Todos se leen. Tardan de 0,5 a 1,5 s; los que necesitan el principio largo, alrededor de 1,3 s.
  - Detecta el HDR de los 4K (*The Mandalorian and Grogu*, *Muppets*).
  - Descubre fichas falsas: *Padre no hay más que uno* dice 1080p y es 4K. Y los dos «.mp4» de *Gente Hablando* son en realidad FLV.
  - El búfer queda limpio después de cada comprobación.
- **Descartado**: descargar entero para mirar (llenaría el búfer) y fiarse del tamaño o de la duración.

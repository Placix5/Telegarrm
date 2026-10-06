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

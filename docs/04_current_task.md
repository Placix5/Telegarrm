# Tarea Actual para Claude Code

> **Estado: completada el 06/10/2026** (D-030 y D-031 en [DECISIONS.md](DECISIONS.md)). Se hizo también la parte opcional: el trabajador descarga de verdad con TDLib, con progreso, cancelación y reanudación. Diferencia con lo pedido: los archivos quedan de momento en la caché de TDLib (`db/tdlib`), porque aún no está definida la ruta de la biblioteca; moverlos y descomprimirlos es el siguiente paso de la Fase 3.

**Fase Actual:** Cierre de Fase 2 (TMDB) e Inicio de Fase 3 (Descargas).

## Estado Actual
Gemini ha revisado todos los commits recientes. ¡Excelente trabajo de arquitectura y rendimiento! La decisión D-028 (`RelWithDebInfo` y caché) es brillante para la Raspberry Pi, y la propuesta D-029 sobre TMDB está totalmente aprobada por la dirección técnica. El código en `channel_sync.cpp` y `media_parser.cpp` es seguro y libre de fugas.

## Tus Objetivos

1. **Cierre de Fase 2 (Integración TMDB - D-029)**
   * Implementa el proveedor de TMDB tal y como definiste en `DECISIONS.md`.
   * Lee el token de la variable de entorno `TELEGARRM_TMDB_TOKEN`.
   * Crea una tabla en SQLite para cachear las respuestas de TMDB e IDs externos, evitando saturar la API y cumpliendo tu plan de resiliencia.
   * Añade el logotipo de TMDB y el aviso legal requerido en la web (footer/créditos).
   * Enriquece el catálogo web (carátulas, sinopsis, episodios) con estos datos.

2. **Inicio de Fase 3 (Descargas)**
   * Crea los endpoints base para descargas: `POST /api/downloads` (para encolar) y `GET /api/downloads` (para leer el progreso).
   * Modifica la base de datos (SQLite) para añadir la tabla `downloads`, que guardará la cola persistente.
   * Modifica la UI Web para añadir el botón "Almacenar en disco" en las fichas de las versiones elegidas.
   * *(Opcional en este paso)* Inicia la migración de la clase `downloader.cpp/hpp` del proyecto original para que un hilo trabajador en background empiece a procesar la cola de la BD.

## Criterios de Aceptación
* Si `TELEGARRM_TMDB_TOKEN` no está definido, el sistema debe seguir funcionando usando solo los datos nativos de Telegram (resiliencia).
* Las consultas a TMDB deben ser cacheadas.
* Se debe poder añadir un elemento a la cola de descargas desde la interfaz web, y este debe reflejarse en la base de datos.

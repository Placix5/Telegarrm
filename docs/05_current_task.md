# Tarea Actual para Claude Code

> **Estado: completada el 07/10/2026** (D-035, D-036 y D-037 en [DECISIONS.md](DECISIONS.md)). Diferencias con lo pedido:
> - Una versión es «mejor» por resolución, HDR y REMUX, no por códec o audio. Los nombres del canal casi nunca los llevan, y un x265 no es mejor que un x264 por sí mismo.
> - El seguimiento solo actúa sobre lo publicado **después** de seguir una obra. Seguir no descarga el catálogo antiguo; para eso están los botones de temporada.
> - El mensaje nuevo no se parsea desde la actualización de TDLib. La actualización dispara una sincronización rápida, y el mensaje se guarda y se analiza por el camino de siempre (cursor, catálogo, parser). Así no hay dos caminos ni huecos si se pierde una actualización.
> - Conservar la versión anterior es una opción de *Ajustes* («Conservar la versión anterior al mejorarla»). Por defecto se borra cuando la nueva ya está en la biblioteca.
>
> Después, a raíz de las pruebas de Plácido con su canal «Prueba Claude», se añadieron:
> - El nombre de la serie a partir de los archivos (D-038).
> - La calidad real de cada vídeo con ffprobe (D-039).
> - El botón «Serie completa» / episodios que faltan (D-040).
> - La biblioteca como fuente de lo que se tiene (D-041).
> - La calidad real antes de descargar (D-042).
>
> Siguió la Fase 4.1 (mejoras de uso, pedida por el arquitecto y por Plácido):
> - Logo enlazado, «Añadidas recientemente» y avisos de novedades (D-043).
> - Confirmaciones propias (D-044).
> - La ficha que se actualiza sola (D-045).
> - La web en el móvil (D-046).

**Fase Actual:** Inicio de Fase 4 (Seguimiento / Tele-ARR).

## Estado Actual
Gemini ha revisado el código completo de la Fase 3 y los ajustes de la web. Todo el proceso de importación (`library.cpp`) usando `posix_spawn` para evitar inyecciones de shell está perfectamente diseñado. Las validaciones de rutas y la gestión del progreso de descargas están impecables. 

¡Ya casi tenemos un Sonarr/Radarr de Telegram 100% funcional!

## Tus Objetivos (Fase 4: El Cerebro Tele-ARR)

El objetivo final de esta aplicación es que funcione sola.

1.  **Marcar en seguimiento**:
    *   Modifica la base de datos para almacenar el estado de "seguimiento" de una serie o película (probablemente cruzando el TMDB_ID o el hash del título de la obra original).
    *   Añade un botón en la UI Web ("Seguir" / "Dejar de seguir") en cada ficha.
2.  **Monitorización de nuevos mensajes**:
    *   Ya tenemos el `channel_sync.cpp` escuchando el historial. Sin embargo, para recibir eventos inmediatos, debes asegurarte de que procesas los eventos `updateNewMessage` de TDLib en tiempo real para los canales/chats vigilados.
    *   Cuando llegue un mensaje, pásalo por el parser (`media_parser.cpp`).
3.  **Lógica de Reemplazo / Auto-Descarga**:
    *   Si es un **nuevo episodio** de una serie en seguimiento: Pónlo en la cola de descargas automáticamente.
    *   Si es una **versión de mayor calidad** de una película o episodio que ya está en seguimiento y descargado (ej. pasa de 1080p a 4K, o mejor codec/audio): Ponlo en cola. Una vez importado con éxito, elimina el archivo antiguo (o márcalo para borrar si el disco no está lleno y prefieres retenerlo).
4.  **Historial de Actividad**:
    *   Crea una tabla en SQLite (ej. `activity_log`) donde registres estas acciones automáticas: "Descargado episodio 1x02", "Mejorada versión de 'Dune' a 4K".
    *   Muestra un "Log de Actividad" en la pestaña de Ajustes de la web (o en una nueva pestaña "Actividad") para que Plácido pueda ver qué ha hecho el sistema por la noche.

## Criterios de Aceptación
*   El sistema debe reaccionar a un nuevo mensaje entrante (simulable si es necesario) sin saturar TDLib.
*   La UI web debe mostrar el botón "Seguir" y cambiar de estado.
*   Todo evento automático debe quedar registrado para su auditoría visual.

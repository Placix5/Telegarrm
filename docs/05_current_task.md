# Tarea Actual para Claude Code

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

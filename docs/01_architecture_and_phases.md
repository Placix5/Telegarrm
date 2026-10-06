# Arquitectura de Telegarrm

Telegarrm evoluciona el concepto de un script de descargas de Telegram a un servicio *Daemon* completo (estilo Sonarr/Radarr) optimizado para ejecutarse en segundo plano (ej. en una Raspberry Pi).

## Componentes del Sistema

1.  **Core / Daemon (C++)**
    *   Proceso principal en ejecución continua.
    *   Gestiona el ciclo de vida del servidor web y de la conexión con TDLib.
2.  **API REST (cpp-httplib)**
    *   Incrustada en el binario C++. Expondrá endpoints (`/api/...`) para el Frontend.
    *   Servirá los archivos estáticos de la interfaz web.
3.  **Módulo de Base de Datos (SQLite3)**
    *   Ligera, local, sin servidor.
    *   **Tablas previstas:**
        *   `channels`: Canales de Telegram vigilados.
        *   `media`: Series/Películas encontradas (metadatos, calidades).
        *   `downloads`: Cola y estado actual de descargas en curso.
4.  **Módulo Telegram (TDLib)**
    *   Conectado con la cuenta del usuario.
    *   Lee historial de canales para poblar el catálogo.
    *   Recibe eventos `updateNewMessage` para reemplazos automáticos de mejor calidad.
5.  **Frontend (Web UI)**
    *   SPA HTML/JS/CSS pura para gestionar las descargas visualmente desde cualquier navegador de la red local.

## Fases de Implementación

*   **Fase 0: Estructura Base y API**: Configuración de CMake, integración de `cpp-httplib` y servidor web que responde en el hilo principal.
*   **Fase 1: Motor TDLib e Hilos**: Migración del código TDLib del antiguo proyecto, ejecutándolo en un hilo separado. Implementación de SQLite para guardar configuración/estado.
*   **Fase 2: Catálogo y Frontend**: Lectura del historial del canal de Telegram usando Regex para extraer metadatos de las series. Endpoints para servir los datos a la UI web.
*   **Fase 3: Descarga y Cola**: Interfaz web funcional con botón de descarga. Integración del sistema de descarga, descompresión y renombrado del proyecto original.
*   **Fase 4: Tele-ARR (Automatización)**: Sistema pasivo de escucha de mensajes para actualizar calidades de archivos ya descargados y auto-descarga de nuevos capítulos en seguimiento.

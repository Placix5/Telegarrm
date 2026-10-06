# Tarea Actual para Claude Code

> **Estado: completada el 06/10/2026, con dos cambios respecto a esta especificación:**
> - Credenciales (`api_id`, `api_hash`) en **variables de entorno**, no en la tabla `settings`: decisión de Plácido, para que los secretos no estén en la BD.
> - Inicio de sesión (teléfono, código, 2FA) **desde la web** (`/api/telegram/auth/*` y `web/index.html`) en lugar de la terminal, para no depender de SSH.
>
> Se usa la interfaz JSON (`Td::TdJson`) como el proyecto original, pero con la API nueva (`td_send`/`td_receive`). La planificación vigente está en [ROADMAP.md](ROADMAP.md).

**Fase Actual:** Fase 1 Continuación (Integración de la librería real de TDLib).

## Estado Actual
El servidor HTTP, la base de datos (SQLite3) con sus envoltorios seguros en C++ y la gestión de hilos simulados y parada ordenada están completamente implementados y funcionando sin fugas de memoria.

## Tus Objetivos

El objetivo de esta tarea es **traer la lógica de autenticación y conexión de TDLib** desde el antiguo proyecto (`Z:\TelegramDownloader`) hacia nuestro nuevo módulo `TelegramClient`.

1.  **Añadir TDLib al CMakeLists.txt**:
    *   Configura `CMakeLists.txt` para encontrar y enlazar TDLib (`Td::TdStatic` o `Td::TdJson`, dependiendo de cómo lo hacíais en el proyecto original).
2.  **Migrar Lógica Base de TDLib**:
    *   Abre y lee el archivo `Z:\TelegramDownloader\src\main.cpp` (o donde el proyecto original inicialice TDLib). No lo modifiques, solo analízalo.
    *   Adapta ese código para inicializar la conexión con Telegram dentro del método `run()` de nuestro `TelegramClient` actual.
    *   Deberás configurar la recepción de eventos asíncronos de TDLib (usualmente a través de un `ClientManager` o el json interface).
3.  **Gestión de Configuración / Auth**:
    *   El antiguo proyecto seguramente leía las credenciales (API ID, API Hash, Teléfono) desde un archivo o la terminal.
    *   Adapta la lógica para que, si el cliente no está autorizado, busque estas credenciales en nuestra base de datos (tabla `settings`).
    *   *(Por ahora, puedes leer las credenciales codificadas en variables temporales o pedirlas por CLI solo la primera vez para guardarlas en SQLite).*

## Criterios de Aceptación
*   El proyecto debe enlazar con TDLib correctamente.
*   Al ejecutar `./build/telegarrm`, el hilo secundario de `TelegramClient` debe poder conectar con los servidores de Telegram y reportar el estado de autorización (ej. "Waiting for phone number" o "Ready").
*   La desconexión (`telegram.stop()`) debe cerrar el cliente de TDLib de forma ordenada para no corromper la base de datos interna de Telegram.

## Ayuda
Puedes revisar la implementación original en:
*   `Z:\TelegramDownloader\src\downloader.cpp`
*   `Z:\TelegramDownloader\include\downloader.hpp`

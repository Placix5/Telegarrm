# Telegarrm

Telegarrm es un servicio daemon (stack ARR) que utiliza Telegram (TDLib) como fuente e indexador para descargas automáticas de series y películas.

## Estructura del Proyecto
- `src/`: Código fuente C++
- `include/`: Cabeceras y dependencias de un solo archivo (ej. `httplib.h`)
- `web/`: Interfaz web (Frontend)
- `db/`: Bases de datos SQLite3
- `build/`: Archivos de compilación

## Fases de Desarrollo
- [x] **Fase 0**: Estructura base y servidor HTTP (`cpp-httplib`).
- [ ] **Fase 1**: Frontend básico (Catálogo) y lectura de canal con expresiones regulares.
- [ ] **Fase 2**: Integración de descargas bajo demanda (Core antiguo de C++).
- [ ] **Fase 3**: Automatización, escuchador en segundo plano y reemplazo inteligente de calidades.

## Compilación (Fase 0)
```bash
mkdir build && cd build
cmake ..
cmake --build .
./telegarrm
```
El servidor arrancará en `http://localhost:8080/api/status`.

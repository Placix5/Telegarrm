# Instalación sin Docker

Para compilar Telegarrm e instalarlo como servicio del sistema en Debian o Raspberry Pi OS. Si tienes Docker, es más sencillo seguir [la instalación con Docker](instalacion-docker.md).

Los ejemplos suponen que el repositorio está en `~/Telegarrm` y la biblioteca en `/srv/media`.

## 1. Dependencias
- Compilador con C++17, CMake 3.14 o posterior, SQLite3 y TDLib 1.8 o posterior.
- Paquetes:
  ```bash
  sudo apt install build-essential cmake git libsqlite3-dev gperf zlib1g-dev libssl-dev 7zip 7zip-rar ffmpeg
  ```
  - `7zip` descomprime lo descargado; `7zip-rar` añade el soporte de RAR, que Debian distribuye aparte (repositorio `non-free`) por su licencia.
  - `ffmpeg` trae `ffprobe`, que lee la resolución y el HDR reales de cada vídeo.
  - `zlib1g-dev` y `libssl-dev`: los usan TDLib, la lectura de los ZIP y las consultas a TMDB.

## 2. TDLib
La librería oficial de Telegram. Basta con su versión JSON compartida, instalada en `~/td/tdlib`, donde la busca Telegarrm. En una Raspberry Pi 5 tarda unos 30 minutos:
```bash
git clone https://github.com/tdlib/td.git ~/td
git -C ~/td checkout 42e6a5259551178d1dab54a22ad96d14bd906e20
cmake -S ~/td -B ~/td/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/td/tdlib -DTD_INSTALL_STATIC_LIBRARIES=OFF
cmake --build ~/td/build --target tdjson -j3
cmake --install ~/td/build
```
Ese commit es la versión 1.8.67, la misma de la imagen de Docker: así una instalación se puede pasar a la otra sin volver a iniciar sesión. Con más memoria se puede compilar con más procesos (`-j`); cada uno necesita unos 1,5 GB.

Si TDLib está en otra ruta, añade `-DTd_DIR=<prefijo>/lib/cmake/Td` al configurar Telegarrm.

## 3. Compilar
```bash
git clone https://github.com/Placix5/Telegarrm.git ~/Telegarrm
cd ~/Telegarrm
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## 4. Claves
Las claves se pasan **solo por variables de entorno** (o por archivos, ver [Configuración](configuracion.md)). Guárdalas en un archivo que solo pueda leer tu usuario:
```bash
mkdir -p ~/.config/telegarrm
install -m 600 /dev/null ~/.config/telegarrm/env
nano ~/.config/telegarrm/env
```
Con este contenido y tus valores:
```
TELEGARRM_API_ID=123456
TELEGARRM_API_HASH=0123456789abcdef0123456789abcdef
TELEGARRM_TMDB_TOKEN=eyJhbGciOi...
```
`TELEGARRM_TMDB_TOKEN` es opcional.

## 5. Probarlo a mano
Desde la carpeta del programa, porque las rutas `db/` y `web/` son relativas a ella:
```bash
cd ~/Telegarrm
set -a; . ~/.config/telegarrm/env; set +a
./build/telegarrm
```
Ctrl+C lo detiene de forma ordenada. Abre `http://<ip-del-equipo>:8080` y sigue los [primeros pasos](../README.md#primeros-pasos). El código para crear la contraseña está en `~/Telegarrm/db/codigo-acceso.txt`.

## 6. Instalarlo como servicio
`deploy/telegarrm.service` es un servicio de systemd que arranca Telegarrm al encender el equipo, aislado: solo puede escribir en `db/` y en `/srv/media`. La regla de polkit `deploy/50-telegarrm.rules` permite a tu usuario arrancarlo, pararlo y reiniciarlo sin `sudo`.

Los dos archivos llevan un usuario y unas rutas concretas (`plax` y `/home/plax/Telegarrm`). Antes de instalarlos, cámbialos por los tuyos:
- en `deploy/telegarrm.service`: `User`, `Group`, `WorkingDirectory`, `EnvironmentFile`, `ExecStart` y `ReadWritePaths`;
- en `deploy/50-telegarrm.rules`: el usuario.

Después, instálalo (y repítelo cada vez que cambie algo en `deploy/`):
```bash
sudo ./deploy/install-service.sh
```
Si la biblioteca está en otra carpeta, por ejemplo un disco montado en `/mnt/raid`, pásala al script para que el servicio pueda escribir en ella: `sudo ./deploy/install-service.sh /mnt/raid`.

- **Actualizar**: `git pull && cmake --build build && systemctl restart telegarrm`.
- **Registro**: `journalctl -u telegarrm -f`.
- **Restablecer la contraseña de la web**: `cd ~/Telegarrm && ./build/telegarrm --reset-password`.

No tengas a la vez el servicio y un contenedor de Docker con los mismos datos: compartirían la sesión de Telegram. Para pasar a Docker, mira [Mudar una instalación](instalacion-docker.md#mudar-una-instalación).

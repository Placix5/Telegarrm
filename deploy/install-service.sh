#!/bin/sh
# Instala o actualiza el servicio de sistema de Telegarrm y su regla de polkit (D-013).
#
# Uso: sudo ./deploy/install-service.sh [RUTA...]
#   Sin rutas: la unidad permite escribir en db/ y en /srv/media (D-033).
#   Con rutas: además, en esas carpetas (ej. un RAID montado en otro sitio). Se guardan en
#   /etc/systemd/system/telegarrm.service.d/rutas.conf y sustituyen a las de la vez anterior.
# Repetirlo cada vez que cambie algo en deploy/.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Ejecútalo con sudo: sudo $0 $*" >&2
    exit 1
fi

# Solo rutas absolutas y sin caracteres que systemd pudiera interpretar
for path in "$@"; do
    case "$path" in
        /*) ;;
        *) echo "La ruta debe ser absoluta: $path" >&2; exit 1 ;;
    esac
    case "$path" in
        *[!A-Za-z0-9/._-]*) echo "Solo se admiten letras, números y / . _ - en las rutas: $path" >&2; exit 1 ;;
    esac
done

cd "$(dirname "$0")"

# Copias propiedad de root: systemd y polkit no deben leer ficheros que pueda editar un usuario
install -m 644 -o root -g root telegarrm.service /etc/systemd/system/telegarrm.service
install -m 644 -o root -g root 50-telegarrm.rules /etc/polkit-1/rules.d/50-telegarrm.rules

if [ "$#" -gt 0 ]; then
    dropin=/etc/systemd/system/telegarrm.service.d
    install -d -m 755 -o root -g root "$dropin"
    {
        echo "# Generado por install-service.sh: carpetas donde Telegarrm puede escribir"
        echo "[Service]"
        for path in "$@"; do
            echo "ReadWritePaths=-$path"
        done
    } > "$dropin/rutas.conf"
    chmod 644 "$dropin/rutas.conf"
    echo "Rutas con escritura añadidas: $*"
fi

systemctl daemon-reload
systemctl enable telegarrm
systemctl restart telegarrm

echo "Telegarrm instalado: $(systemctl is-active telegarrm). Logs: journalctl -u telegarrm -f"

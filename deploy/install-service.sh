#!/bin/sh
# Instala o actualiza el servicio de sistema de Telegarrm y su regla de polkit (D-013).
# Uso: sudo ./deploy/install-service.sh
# Repetirlo cada vez que cambie deploy/telegarrm.service o deploy/50-telegarrm.rules.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Ejecútalo con sudo: sudo $0" >&2
    exit 1
fi

cd "$(dirname "$0")"

# Copias propiedad de root: systemd y polkit no deben leer ficheros que pueda editar un usuario
install -m 644 -o root -g root telegarrm.service /etc/systemd/system/telegarrm.service
install -m 644 -o root -g root 50-telegarrm.rules /etc/polkit-1/rules.d/50-telegarrm.rules

systemctl daemon-reload
systemctl enable telegarrm
systemctl restart telegarrm

echo "Telegarrm instalado: $(systemctl is-active telegarrm). Logs: journalctl -u telegarrm -f"

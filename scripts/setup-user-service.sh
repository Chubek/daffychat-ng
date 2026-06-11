#!/usr/bin/env bash
set -euo pipefail

SERVICE_SRC="/usr/share/daffychat/daffychat.user.service"
SERVICE_DST_DIR="${HOME}/.config/systemd/user"
SERVICE_DST="${SERVICE_DST_DIR}/daffychat.service"
CONFIG_DIR="${HOME}/.daffychat"

mkdir -p "${SERVICE_DST_DIR}" "${CONFIG_DIR}"

if [[ ! -f "${SERVICE_SRC}" ]]; then
  echo "missing ${SERVICE_SRC}" >&2
  exit 1
fi

install -m 0644 "${SERVICE_SRC}" "${SERVICE_DST}"

mkdir -p "${CONFIG_DIR}"
echo "Using shared config at /etc/daffychat/daffychat.conf"

systemctl --user daemon-reload
systemctl --user enable --now daffychat.service

echo "User service enabled: systemctl --user status daffychat.service"

#!/usr/bin/env bash
set -euo pipefail

SERVICE_SRC="/usr/share/daffychat/daffychat.user.service"
SERVICE_DST_DIR="${HOME}/.config/systemd/user"
SERVICE_DST="${SERVICE_DST_DIR}/daffychat.service"
CONFIG_DIR="${HOME}/.daffychat"
CONFIG_DST="${CONFIG_DIR}/Daffychat.config"
CONFIG_DEFAULT="/usr/share/daffychat/Daffychat.config.default"

mkdir -p "${SERVICE_DST_DIR}" "${CONFIG_DIR}"

if [[ ! -f "${SERVICE_SRC}" ]]; then
  echo "missing ${SERVICE_SRC}" >&2
  exit 1
fi

install -m 0644 "${SERVICE_SRC}" "${SERVICE_DST}"

if [[ ! -f "${CONFIG_DST}" ]]; then
  install -m 0644 "${CONFIG_DEFAULT}" "${CONFIG_DST}"
fi

systemctl --user daemon-reload
systemctl --user enable --now daffychat.service

echo "User service enabled: systemctl --user status daffychat.service"

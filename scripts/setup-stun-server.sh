#!/usr/bin/env bash
set -euo pipefail

COTURN_SRC_DIR="/usr/share/daffychat/third_party/coturn"
COTURN_BUILD_DIR="/var/lib/daffychat/coturn-build"
TURN_CONFIG_DIR="/etc/daffychat"
TURN_CONFIG_PATH="${TURN_CONFIG_DIR}/turnserver.conf"
TURN_SERVICE_PATH="/etc/systemd/system/daffychat-stun.service"
TURN_BIN_PATH=""

resolve_turnserver_bin() {
  if command -v turnserver >/dev/null 2>&1; then
    TURN_BIN_PATH="$(command -v turnserver)"
    return 0
  fi

  if [[ -x "/usr/local/bin/turnserver" ]]; then
    TURN_BIN_PATH="/usr/local/bin/turnserver"
    return 0
  fi

  return 1
}

build_turnserver_from_vendored_source() {
  if [[ ! -d "${COTURN_SRC_DIR}" ]]; then
    echo "[stun-setup] missing vendored coturn source: ${COTURN_SRC_DIR}" >&2
    return 1
  fi

  if ! command -v cmake >/dev/null 2>&1; then
    echo "[stun-setup] cmake is required to build vendored coturn." >&2
    return 1
  fi

  echo "[stun-setup] building coturn turnserver from ${COTURN_SRC_DIR}"
  mkdir -p "${COTURN_BUILD_DIR}"
  cmake -S "${COTURN_SRC_DIR}" -B "${COTURN_BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${COTURN_BUILD_DIR}" --target turnserver -j"$(nproc 2>/dev/null || echo 2)"

  if [[ -x "${COTURN_BUILD_DIR}/bin/turnserver" ]]; then
    install -m 0755 "${COTURN_BUILD_DIR}/bin/turnserver" /usr/local/bin/turnserver
  elif [[ -x "${COTURN_BUILD_DIR}/src/apps/relay/turnserver" ]]; then
    install -m 0755 "${COTURN_BUILD_DIR}/src/apps/relay/turnserver" /usr/local/bin/turnserver
  else
    echo "[stun-setup] coturn build succeeded but turnserver binary not found." >&2
    return 1
  fi

  TURN_BIN_PATH="/usr/local/bin/turnserver"
  return 0
}

write_turnserver_config() {
  mkdir -p "${TURN_CONFIG_DIR}"
  cat > "${TURN_CONFIG_PATH}" <<'EOF'
# DaffyChat STUN-only coturn config.
listening-port=3478
listening-ip=0.0.0.0
realm=daffychat.local
fingerprint
stun-only
no-auth
no-cli
simple-log
EOF
}

write_systemd_service() {
  local service_user="root"
  if id -u turnserver >/dev/null 2>&1; then
    service_user="turnserver"
  fi

  cat > "${TURN_SERVICE_PATH}" <<EOF
[Unit]
Description=DaffyChat STUN server (coturn)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${service_user}
ExecStart=${TURN_BIN_PATH} -c ${TURN_CONFIG_PATH}
Restart=on-failure
RestartSec=2
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF
}

main() {
  if ! resolve_turnserver_bin; then
    build_turnserver_from_vendored_source
  fi

  if [[ -z "${TURN_BIN_PATH}" || ! -x "${TURN_BIN_PATH}" ]]; then
    echo "[stun-setup] turnserver is not available after setup attempts." >&2
    exit 1
  fi

  write_turnserver_config
  write_systemd_service

  if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    systemctl enable --now daffychat-stun.service
    echo "[stun-setup] enabled daffychat-stun.service using ${TURN_BIN_PATH}"
  else
    echo "[stun-setup] systemctl not found; service file written to ${TURN_SERVICE_PATH}" >&2
  fi
}

main "$@"

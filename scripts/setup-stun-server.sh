#!/usr/bin/env bash
set -euo pipefail

COTURN_SRC_DIR="/usr/share/daffychat/third_party/coturn"
COTURN_BUILD_DIR="/var/lib/daffychat/coturn-build"
TURN_CONFIG_DIR="/etc/daffychat"
TURN_CONFIG_PATH="${TURN_CONFIG_DIR}/turnserver.conf"
TURN_SERVICE_PATH="/etc/systemd/system/daffychat-stun.service"
DAFFYCHAT_CONFIG_PATH="/etc/daffychat/daffychat.conf"
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
no-tls
no-dtls
no-auth
no-cli
simple-log
EOF
}

detect_stun_host() {
  local fqdn=""
  fqdn="$(hostname -f 2>/dev/null || true)"
  if [[ -n "${fqdn}" && "${fqdn}" != "localhost" ]]; then
    echo "${fqdn}"
    return 0
  fi

  local host_short=""
  host_short="$(hostname 2>/dev/null || true)"
  if [[ -n "${host_short}" && "${host_short}" != "localhost" ]]; then
    echo "${host_short}"
    return 0
  fi

  local first_ip=""
  first_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
  if [[ -n "${first_ip}" ]]; then
    echo "${first_ip}"
    return 0
  fi

  echo "127.0.0.1"
}

update_daffychat_stun_config() {
  if [[ ! -f "${DAFFYCHAT_CONFIG_PATH}" ]]; then
    return 0
  fi

  local configured_url=""
  configured_url="$(awk -F'\"' '/^[[:space:]]*stun_url[[:space:]]*=/{print $2; exit}' "${DAFFYCHAT_CONFIG_PATH}" || true)"
  local should_replace="0"
  if [[ -z "${configured_url}" ]]; then
    should_replace="1"
  elif [[ "${configured_url}" == "stun:127.0.0.1:3478" || "${configured_url}" == "stun:localhost:3478" ]]; then
    should_replace="1"
  fi

  if [[ "${should_replace}" != "1" ]]; then
    echo "[stun-setup] keeping existing stun_url in ${DAFFYCHAT_CONFIG_PATH}: ${configured_url}"
    return 0
  fi

  local stun_host=""
  stun_host="$(detect_stun_host)"
  local new_url="stun:${stun_host}:3478"
  sed -i -E "s|^[[:space:]]*stun_url[[:space:]]*=.*$|stun_url = \"${new_url}\"|" "${DAFFYCHAT_CONFIG_PATH}"
  echo "[stun-setup] updated daffychat stun_url to ${new_url}"
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
  update_daffychat_stun_config
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

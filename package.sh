#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<'EOF'
Usage: ./package.sh [version]

Builds a minimal .deb containing:
  - /usr/bin/daffychat-server
  - /usr/share/daffychat/client.html
  - /usr/share/daffychat/guide.html
  - /usr/share/daffychat/manifest.webmanifest
  - /usr/share/daffychat/sw.js
  - /usr/share/daffychat/icon.svg
  - /usr/share/daffychat/Daffychat.config.default
  - /usr/share/daffychat/daffychat.user.service
  - /usr/share/daffychat/setup-user-service.sh
  - /usr/share/daffychat/setup-stun-server.sh
  - /usr/share/daffychat/third_party/coturn (vendored source for postinst build fallback)
  - /lib/systemd/system/daffychat.service
  - /lib/systemd/system/daffychat@.service

Default version: 0.1.0
EOF
  exit 0
fi

VERSION="${1:-0.1.0}"
PKG_NAME="daffychat"
ARCH="amd64"
ROOT="build/pkg/${PKG_NAME}_${VERSION}_${ARCH}"

echo "[package] preparing ${ROOT}"
rm -rf "${ROOT}"
mkdir -p "${ROOT}/DEBIAN" \
         "${ROOT}/usr/bin" \
         "${ROOT}/usr/share/daffychat" \
         "${ROOT}/usr/share/daffychat/third_party" \
         "${ROOT}/lib/systemd/system"

echo "[package] configuring CMake build"
cmake -S . -B build
cmake --build build -j"$(nproc)"

echo "[package] staging files"
install -m 0755 "build/daffychat-server" "${ROOT}/usr/bin/daffychat-server"
install -m 0644 "client.html" "${ROOT}/usr/share/daffychat/client.html"
install -m 0644 "guide.html" "${ROOT}/usr/share/daffychat/guide.html"
install -m 0644 "manifest.webmanifest" "${ROOT}/usr/share/daffychat/manifest.webmanifest"
install -m 0644 "sw.js" "${ROOT}/usr/share/daffychat/sw.js"
install -m 0644 "icon.svg" "${ROOT}/usr/share/daffychat/icon.svg"
install -m 0644 "config/daffychat.conf" "${ROOT}/usr/share/daffychat/Daffychat.config.default"
install -m 0644 "daffychat.user.service" "${ROOT}/usr/share/daffychat/daffychat.user.service"
install -m 0755 "scripts/setup-user-service.sh" "${ROOT}/usr/share/daffychat/setup-user-service.sh"
install -m 0755 "scripts/setup-stun-server.sh" "${ROOT}/usr/share/daffychat/setup-stun-server.sh"
install -m 0644 "daffychat.service" "${ROOT}/lib/systemd/system/daffychat.service"
install -m 0644 "daffychat@.service" "${ROOT}/lib/systemd/system/daffychat@.service"
cp -a "third_party/coturn" "${ROOT}/usr/share/daffychat/third_party/"

cat > "${ROOT}/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: DaffyChat Maintainers <maintainers@example.com>
Depends: libc6 (>= 2.31), libstdc++6, systemd
Description: DaffyChat minimal one-to-one voice chat baseline server
 Stage 1 package containing HTTP static serving and health endpoint.
EOF

cat > "${ROOT}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ -n "$SUDO_USER" ]; then
  TARGET_USER="$SUDO_USER"
  TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
else
  TARGET_USER="$(id -un)"
  TARGET_HOME="$HOME"
fi

if [ -n "$TARGET_HOME" ] && [ -d "$TARGET_HOME" ]; then
  install -d -m 0755 "$TARGET_HOME/.daffychat"
  if [ ! -f "$TARGET_HOME/.daffychat/Daffychat.config" ]; then
    install -m 0644 /usr/share/daffychat/Daffychat.config.default "$TARGET_HOME/.daffychat/Daffychat.config"
  fi
  chown -R "$TARGET_USER":"$TARGET_USER" "$TARGET_HOME/.daffychat" || true
fi

if command -v systemctl >/dev/null 2>&1; then
  systemctl daemon-reload || true
fi

if [ -x /usr/share/daffychat/setup-stun-server.sh ]; then
  /usr/share/daffychat/setup-stun-server.sh || echo "warning: STUN setup failed; inspect logs and run setup-stun-server.sh manually" >&2
fi
exit 0
EOF
chmod 0755 "${ROOT}/DEBIAN/postinst"

OUT_DIR="build/dist"
mkdir -p "${OUT_DIR}"
OUT_DEB="${OUT_DIR}/${PKG_NAME}_${VERSION}_${ARCH}.deb"

echo "[package] building ${OUT_DEB}"
dpkg-deb --build "${ROOT}" "${OUT_DEB}"
echo "[package] done: ${OUT_DEB}"

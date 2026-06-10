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
  - /lib/systemd/system/daffychat.service
  - /etc/default/daffychat

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
         "${ROOT}/etc/default" \
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
install -m 0644 "daffychat.service" "${ROOT}/lib/systemd/system/daffychat.service"
cat > "${ROOT}/etc/default/daffychat" <<'EOF'
DAFFYCHAT_HOST=0.0.0.0
DAFFYCHAT_PORT=8080
DAFFYCHAT_PASSWORD=changeme
DAFFYCHAT_STUN_URL=stun:stun.l.google.com:19302
DAFFYCHAT_TURN_URL=
DAFFYCHAT_TURN_USERNAME=
DAFFYCHAT_TURN_PASSWORD=
EOF

cat > "${ROOT}/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: DaffyChat Maintainers <maintainers@example.com>
Depends: libc6 (>= 2.31), libstdc++6
Description: DaffyChat minimal one-to-one voice chat baseline server
 Stage 1 package containing HTTP static serving and health endpoint.
EOF

cat > "${ROOT}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
  systemctl daemon-reload || true
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

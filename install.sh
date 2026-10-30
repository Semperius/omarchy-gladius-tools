#!/bin/bash
# ==============================================================================
# WARHAMMER 40,000 GLADIUS SUPREME COMPANION TOOLS INSTALLER
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$HOME/.local/bin"
mkdir -p "$HOME/.local/share/gladius_screensaver/auth_sounds/fail"
mkdir -p "$HOME/.local/share/gladius_screensaver/auth_sounds/success"
mkdir -p "$HOME/.config/omarchy/branding/gladius_screensavers"
mkdir -p "$HOME/.config/systemd/user"
mkdir -p "$HOME/.config/omarchy/hooks/theme-set.d"

echo "[1/5] Installing screensaver scripts and tools into ~/.local/bin/..."
install -m 755 "$SCRIPT_DIR/screensaver/gladius-screensaver-lock" "$HOME/.local/bin/"
install -m 755 "$SCRIPT_DIR/screensaver/gladius-screensaver-runner" "$HOME/.local/bin/"
install -m 755 "$SCRIPT_DIR/screensaver/gladius-screensaver-dismiss" "$HOME/.local/bin/"
install -m 755 "$SCRIPT_DIR/screensaver/gladius-auth-sound" "$HOME/.local/bin/"
install -m 755 "$SCRIPT_DIR/screensaver/gladius-rotation-manager" "$HOME/.local/bin/"

if [[ -f "$SCRIPT_DIR/screensaver/mpv_screensaver_input.conf" ]]; then
  cp "$SCRIPT_DIR/screensaver/mpv_screensaver_input.conf" "$HOME/.local/share/gladius_screensaver/"
fi

if [[ -d "$SCRIPT_DIR/screensaver/art" ]]; then
  cp -r "$SCRIPT_DIR/screensaver/art/"* "$HOME/.config/omarchy/branding/gladius_screensavers/" || true
fi

if [[ -d "$SCRIPT_DIR/screensaver/auth_sounds" ]]; then
  cp -r "$SCRIPT_DIR/screensaver/auth_sounds/"* "$HOME/.local/share/gladius_screensaver/auth_sounds/" || true
fi

echo "[2/5] Building and installing gladius-smoke-trail daemon..."
if command -v gcc >/dev/null 2>&1 && pkg-config --exists gtk4 gtk4-layer-shell-0 cairo 2>/dev/null; then
  (cd "$SCRIPT_DIR/smoke-trail" && make install)
else
  echo "Notice: gtk4 or gtk4-layer-shell-0 dev libraries not detected. Skipping smoke-trail compilation."
fi

echo "[3/5] Installing systemd user services and timers..."
cp "$SCRIPT_DIR/systemd/"* "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable --now gladius-rotation.timer 2>/dev/null || true

echo "[4/5] Installing theme automation hook..."
install -m 755 "$SCRIPT_DIR/hooks/gladius-supreme.sh" "$HOME/.config/omarchy/hooks/theme-set.d/"

echo "[5/5] Installation complete! The Omnissiah approves."

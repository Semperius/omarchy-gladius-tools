#!/bin/bash
THEME_NAME="$1"

case "${THEME_NAME,,}" in
  *gladius* | *warhammer*)
    # 1. Apply large Ultramarine pointer with crimson outline
    hyprctl setcursor gladius-ultramarine 48 2>/dev/null || true
    gsettings set org.gnome.desktop.interface cursor-theme "gladius-ultramarine" 2>/dev/null || true
    gsettings set org.gnome.desktop.interface cursor-size 48 2>/dev/null || true

    # 2. Ensure smoke trail daemon is running
    if ! pgrep -f '[g]ladius-smoke-trail' >/dev/null; then
      "$HOME/.local/bin/gladius-smoke-trail" >/dev/null 2>&1 &
    fi

    # 3. Ensure 24h screensaver artwork & soundtrack rotation timer is active
    systemctl --user enable --now gladius-rotation.timer 2>/dev/null || true

    # 4. Refresh lock screen theme settings
    omarchy-shell lock reload 2>/dev/null || true
    ;;
esac

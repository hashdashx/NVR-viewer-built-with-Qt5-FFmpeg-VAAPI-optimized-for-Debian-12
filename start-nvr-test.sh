#!/usr/bin/env bash
set -e

export DISPLAY=:99
RESOLUTION="1920x1080x24"

echo "[INFO] Menjalankan Xvfb di $DISPLAY..."
Xvfb :99 -screen 0 $RESOLUTION -dpi 96 &
sleep 2

echo "[INFO] Menjalankan matchbox-window-manager (mouse aktif)..."
matchbox-window-manager -use_titlebar no &
sleep 1

echo "[INFO] Menjalankan x11vnc (port 5900, tanpa password)..."
x11vnc -display :99 -rfbport 5900 -forever -nopw -shared -bg -o /tmp/x11vnc.log
sleep 1

echo "[INFO] Menjalankan aplikasi NVR GUI..."
cd /root/nvr-gui-vaapi/build
./nvr_gui ../config.json &
sleep 2

# opsional: otomatis fullscreen pakai F11
xdotool search --onlyvisible --name "NVR" windowactivate key F11 || true
wait

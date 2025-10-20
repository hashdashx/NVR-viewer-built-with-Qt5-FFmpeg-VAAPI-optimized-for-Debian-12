# === Konfigurasi dasar ===
export DISPLAY=:0
export LIBVA_DRIVER_NAME=r600     # driver VAAPI untuk AMD Radeon HD 6480G
xhost +SI:localuser:root || true  # beri izin root akses ke Xorg
RESOLUTION="1920x1080x24"

echo "[INFO] Menjalankan aplikasi NVR GUI..."
cd /root/nvr-gui-vaapi-t
./nvr_gui_vaapi ../config.json &
# 🧠 NVR Systemd Automation Stack

Kumpulan konfigurasi **systemd** untuk sistem **NVR (Network Video Recorder)** berbasis Debian/Armbian dengan mode headless.  
Tujuan utama konfigurasi ini adalah:

- Menjalankan **GUI headless (Xorg)** tanpa desktop penuh  
- Menjalankan **aplikasi NVR otomatis** pada display fisik  
- Melakukan **mount NAS otomatis** dengan retry saat boot  
- Memantau koneksi NAS dan mengendalikan NVR berdasarkan status mount  
- Mencegah **eMMC 32 GB internal** penuh akibat penulisan data lokal saat NAS gagal

---

## 🗂️ 1. FSTAB Configuration

**File:** `/etc/fstab`
```bash
//192.168.60.8/AgentMedia  /mnt/nas/AgentMedia  cifs  credentials=/etc/cifs-creds-agentdvr,iocharset=utf8,file_mode=0777,dir_mode=0777,nofail  0  0
```

### 💡 Penjelasan
- **CIFS** digunakan untuk mount folder NAS (SMB share).  
- `nofail` memastikan sistem tetap bisa boot walau NAS tidak tersedia.  
- `credentials=/etc/cifs-creds-agentdvr` berisi username & password SMB.  
- Mount ini digunakan sebagai lokasi penyimpanan utama rekaman NVR.  
- Jika fstab gagal mount saat boot karena jaringan belum siap, proses akan diulang oleh `mount-agentmedia.service`.

---

## ⚙️ 2. mount-agentmedia.service

**File:** `/etc/systemd/system/mount-agentmedia.service`
```ini
[Unit]
Description=Auto mount NAS AgentMedia
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c '\
  echo "[NAS] Mounting from fstab..."; \
  for i in {1..10}; do \
    if /bin/mount -a -t cifs 2>/tmp/mount-nas.log; then \
      echo "[NAS] Mount OK."; \
      exit 0; \
    fi; \
    echo "[NAS] Attempt $i failed, retrying in 5s..."; \
    sleep 5; \
  done; \
  echo "[NAS] Mount failed after 10 retries"; \
  exit 1;'
SuccessExitStatus=0 32
TimeoutStartSec=90

[Install]
WantedBy=graphical.target
```

### 🧩 Fungsi
- Menjalankan `mount -a -t cifs` hingga 10x percobaan.
- Dipicu **setelah jaringan aktif**, memastikan NAS siap sebelum mount.
- Menghindari error *FAILED* pada saat boot awal.
- Jika NAS gagal termount, akan dipantau dan ditangani oleh `nas-monitor.service`.

---

## 🧩 3. nas-monitor.service

**File:** `/etc/systemd/system/nas-monitor.service`
```ini
[Unit]
Description=Monitor NAS AgentMedia mount and control NVR accordingly
After=mount-agentmedia.service
Requires=mount-agentmedia.service

[Service]
Type=simple
ExecStart=/bin/bash -c '\
  MNT=/mnt/nas/AgentMedia; \
  echo "[NAS-MON] Starting NAS monitor..."; \
  LAST_STATE="unknown"; \
  WAS_FAILED=0; \
  while true; do \
    if mountpoint -q "$MNT" && timeout 2 ls "$MNT" >/dev/null 2>&1; then \
      if [ "$LAST_STATE" != "mounted" ]; then \
        echo "[NAS-MON] NAS is mounted and OK."; \
        if [ "$WAS_FAILED" -eq 1 ]; then \
          echo "[NAS-MON] NAS reconnected. Running nvr-only.sh..."; \
          if /root/nvr-only.sh; then \
            echo "[NAS-MON] nvr-only.sh executed successfully."; \
          else \
            echo "[NAS-MON] nvr-only.sh FAILED to run!"; \
          fi; \
          WAS_FAILED=0; \
        fi; \
        LAST_STATE="mounted"; \
      fi; \
    else \
      if [ "$LAST_STATE" != "unmounted" ]; then \
        echo "[NAS-MON] NAS is not mounted! Running kill-nvr.sh..."; \
        if /root/kill-nvr.sh; then \
          echo "[NAS-MON] kill-nvr.sh executed successfully."; \
        else \
          echo "[NAS-MON] kill-nvr.sh FAILED to run!"; \
        fi; \
        echo "[NAS-MON] Retrying mount..."; \
        /bin/mount -a -t cifs >/dev/null 2>&1; \
        LAST_STATE="unmounted"; \
        WAS_FAILED=1; \
      fi; \
    fi; \
    sleep 15; \
  done'
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### 💡 Penjelasan
- Memantau status mount NAS secara terus-menerus.
- Jika NAS **hilang** → jalankan `/root/kill-nvr.sh` dan set status gagal.
- Jika NAS **tersambung kembali** setelah sempat gagal → jalankan `/root/nvr-only.sh`.
- Tidak spam log: hanya mencatat saat status berubah.
- Menggunakan `timeout 2` agar deteksi CIFS cepat (maksimal 2 detik).

---

## 🖥️ 4. xorg.service

**File:** `/etc/systemd/system/xorg.service`
```ini
[Unit]
Description=Start Xorg Display Server on :0
After=network.target systemd-user-sessions.service

[Service]
ExecStart=/usr/bin/Xorg :0 -nolisten tcp vt1
Restart=always
User=root
WorkingDirectory=/root
StandardOutput=null
StandardError=null

[Install]
WantedBy=graphical.target
```

### 💡 Penjelasan
- Menjalankan Xorg di display `:0` tanpa desktop manager penuh.
- `After=network.target` menjaga urutan boot tetap wajar.
- Dipindahkan ke `WantedBy=graphical.target` agar aktif di fase GUI, bukan teks mode.
- Menghindari loop dependensi yang terjadi jika menggunakan `multi-user.target`.

---

## 🎛️ 5. nvr-gui.service

**File:** `/etc/systemd/system/nvr-gui.service`
```ini
[Unit]
Description=NVR GUI
After=xorg.service network-online.target
Requires=xorg.service
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/root
ExecStartPre=/bin/bash -c 'for i in {1..20}; do [ -S /tmp/.X11-unix/X0 ] && exit 0 || echo "[WAIT] Xorg belum siap ($i)..." && sleep 1; done; exit 1'
ExecStart=/bin/bash /root/nvr.sh
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=graphical.target
```

### 💡 Penjelasan
- Menjalankan GUI NVR setelah Xorg siap (display `:0`).
- `ExecStartPre` memastikan socket Xorg sudah tersedia sebelum aplikasi dijalankan.
- `Restart=always` menjaga agar GUI tetap hidup jika crash.
- `WantedBy=graphical.target` membuat service ini otomatis aktif setelah tampilan grafis siap.

---

## 🧩 Urutan Booting

1. `network-online.target` → memastikan jaringan siap.  
2. `mount-agentmedia.service` → melakukan mount NAS.  
3. `nas-monitor.service` → memantau status NAS selama runtime.  
4. `xorg.service` → menyiapkan tampilan headless display.  
5. `nvr-gui.service` → menjalankan GUI aplikasi NVR.  

---

## 📊 Contoh Log (`journalctl -u nas-monitor.service -f`)
```
[NAS-MON] Starting NAS monitor...
[NAS-MON] NAS is mounted and OK.
[NAS-MON] NAS is not mounted! Running kill-nvr.sh...
[NAS-MON] kill-nvr.sh executed successfully.
[NAS-MON] Retrying mount...
[NAS-MON] NAS is mounted and OK.
[NAS-MON] NAS reconnected. Running nvr-only.sh...
[NAS-MON] nvr-only.sh executed successfully.
```

---

## ✅ Kesimpulan
Semua konfigurasi di atas membentuk sistem otomatis yang tahan gangguan jaringan:

| Komponen | Fungsi Utama |
|-----------|--------------|
| **fstab** | Definisi mount NAS |
| **mount-agentmedia.service** | Menangani mount awal dengan retry |
| **nas-monitor.service** | Deteksi putus/nyambung NAS, kontrol NVR otomatis |
| **xorg.service** | Menjalankan server X headless |
| **nvr-gui.service** | Menjalankan aplikasi GUI NVR setelah Xorg siap |

---

📍 **Dibuat untuk:** integrasi NVR headless berbasis Debian/Armbian  
📅 **Versi:** 1.0  
✍️ **Author:** hashdashx

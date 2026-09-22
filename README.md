# M5StickS3 OOB Console KVM (`blackbox_oob`)

An ultra-compact, out-of-band (OOB) management and crash blackbox appliance for headless servers and hypervisors (Proxmox VE, Debian, Ubuntu, RHEL) built on the **M5Stack StickS3** (ESP32-S3-PICO-1-N8R8).

Compatible with **[M5Launcher](https://github.com/bmorcelli/Launcher)** as a completely self-contained, monolithic firmware with zero external filesystem partition dependencies.

---

## Key Features

- **Interactive VT100/ANSI Web Terminal (USB CDC-ACM)**: Full 2D screen grid (`80x24`) with in-place curses cursor positioning, alternate screen buffer (flawless rendering for `top`, `htop`, `vi`, `nano`), DEC box-drawing graphics, and private mode escape sequences over WebSockets.
- **Kernel Panic Netconsole Receiver**: Dedicated UDP listener on port `6666` that ingests kernel panic messages even if the host USB stack freezes during a hard crash.
- **Power Loss Detection & Battery Backup**: Real-time **M5PM1** PMIC monitoring detects VBUS drops below `4.0V`, transitions seamlessly to the internal 250mAh LiPo battery, and records power outage events.
- **Persistent Alerting & Zabbix Integration**: Automatically pushes diagnostic summaries and the last 10KB of console logs to a Zabbix server via raw TCP (`port 10051`). If the network switch is down during a blackout, logs are queued into non-volatile storage (`blackbox_oob` NVS) and transmitted upon power recovery.
- **Web-Based Configuration & SoftAP Onboarding**: No hardcoded Wi-Fi credentials or server settings. Easily configure Wi-Fi, Wake-on-LAN MAC, Zabbix server, and Basic Auth directly via the Web UI (`/settings`) or initial SoftAP mode (`192.168.4.1`).
- **Wake-on-LAN (WoL) Power On**: Web UI button and REST API trigger standard Magic Packets to power up the host server remotely.
- **Local TFT LCD Telemetry & Energy Saver**: Double-buffered 1.14" IPS display (`M5Canvas`) eliminating all text artifacts and ghosting. Features automatic 60-second inactivity backlight sleep to save battery with instant wake-up on button click or alert events.
- **Physical Safety Override**: Front "M5" button provides a 3-second long-press hardware reboot mechanism (`esp_restart()`) to recover from hangs without battery drainage.
- **M5Launcher Compatible**: Monolithic single-app binary with zero filesystem partition dependencies (SPIFFS/LittleFS) and isolated NVS namespace (`blackbox_oob`) to prevent bootloader state corruption.

---

## System Architecture

```
Host Server (Proxmox/Linux)
   ├── USB CDC-ACM (serial-getty@ttyACM0) ──> M5StickS3 TinyUSB (Core 1)
   └── UDP :6666 (Kernel netconsole)      ──> M5StickS3 Netconsole Listener (Core 1)
                                                       │
                                            [ 10KB FreeRTOS StreamBuffer ]
                                                       │
Client Browser                            ──> M5StickS3 HTTP/WebSocket Server (Core 0)
Zabbix Server (:10051)                    <── M5StickS3 Alert & NVS Engine (Core 0)
```

---

## Hardware Specifications (M5StickS3 / SKU: K150)

Reference: [M5StickS3 Documentation](https://docs.m5stack.com/en/core/StickS3)

| Component | Specification |
|---|---|
| **MCU** | ESP32-S3-PICO-1-N8R8 (Dual-Core Xtensa LX7 @ 240MHz) |
| **Memory** | 512KB SRAM, 8MB Flash, 8MB Octal PSRAM |
| **PMIC** | M5PM1 (I2C `0x6e`) with VBUS sensing & battery telemetry |
| **Battery** | Integrated 250mAh Lithium Polymer battery |
| **Display** | 1.14" 135x240 ST7789P3 IPS LCD |
| **Buttons** | KEY1 (GPIO11 / Front M5 Button), KEY2 (GPIO12 / Side Button) |
| **USB** | Native USB OTG Type-C (CDC-ACM virtual serial port) |
| **Wireless** | 2.4 GHz Wi-Fi (802.11 b/g/n) |

---

## Initial Setup & Wi-Fi Provisioning

No recompilation is needed to configure your network or server credentials:

1. **First Boot (SoftAP Mode)**:
   - If no Wi-Fi credentials are saved, the M5StickS3 automatically broadcasts an AP in dual `APSTA` mode: `M5StickS3-KVM-Setup` (password: `password123`).
   - The onboard 1.14" LCD displays the SoftAP SSID, password, and configuration IP (`192.168.4.1`).
2. **Web Configuration Portal**:
   - Connect your phone or laptop to `M5StickS3-KVM-Setup`.
   - Open your browser to `http://192.168.4.1` and open the **Settings** panel.
   - Click **Scan Networks** to discover nearby Wi-Fi APs, choose your network, and enter the password:
     - Target Host Wake-on-LAN MAC address.
     - Zabbix Server IP / Port (default `10051`).
     - Web UI Basic Auth username and password.
   - Choose your preferred application method:
     - **Save & Restart**: Writes settings to NVS and reboots immediately to connect to your Wi-Fi network.
     - **Save & Connect (Station)**: Writes settings to NVS and transitions directly to Station mode without rebooting.
     - *(Note: Leaving the password field blank preserves your previously saved password without overwriting it).*
3. **Normal Operation**:
   - The device connects to your Wi-Fi network and displays its assigned IP address on the screen.
   - Access the terminal anytime via `http://<ASSIGNED_IP>`.

---

## Physical Controls & Display Management

### Energy Conservation & Display Auto-Sleep
- To preserve the 250mAh battery and prevent unnecessary energy dissipation during headless operation, the ST7789 display automatically enters **sleep mode** (backlight turns off completely) after **60 seconds of inactivity**.
- **Instant Wake-Up**: Pressing either the **Front M5 Button** or the **Side Button** instantly wakes the display back up at normal brightness.
- **Emergency Wake-Up**: If a power drop (`VBUS_DROP`), kernel panic, or OOM event is detected, the display immediately wakes up automatically at maximum brightness and displays the alert dashboard.

### Button Functions
- **Front M5 Button (KEY1 / GPIO11)**:
  - **Click (When Asleep)**: Wakes up the display without changing the page.
  - **Click (When Awake)**: Cycle display pages:
    - *Page 0*: Main Dashboard (Management IP, Wi-Fi RSSI in dBm, Battery Level %, Alert Status, Byte-formatted RX/TX counters, Host USB `/dev/ttyACM0` connection status).
    - *Page 1*: Wi-Fi & Network Setup details (Active Mode, SSID, Connection Status/Password, Web URL).
    - *Page 2*: System Telemetry (Uptime, Netconsole packet count, VBUS Voltage & Charging status, Free RAM/PSRAM, Zabbix Server target).
  - **Long Press (3s)**: Physical reset (`esp_restart()`).
- **Side Button (KEY2 / GPIO12)**:
  - **Click (When Asleep)**: Wakes up the display.
  - **Click (When Awake)**: **Toggle Wi-Fi Mode**:
    - Switches smoothly between **SoftAP mode** (`192.168.4.1`) and **Station mode** (connecting to your saved local Wi-Fi router).
    - Automatically flips display to Page 0 (Dashboard) when connecting to Station to show IP and link status.
  - **Double Click**: Power off device immediately.

---

## Host Configuration (Proxmox VE / Linux)

To connect your host server to the M5StickS3 dongle:

### 1. Enable Serial Console via USB CDC-ACM
Connect the M5StickS3 USB-C port to the host server USB port. Once detected as `/dev/ttyACM0`:
```bash
# Enable and start serial getty on ttyACM0 (115200 baud default)
sudo systemctl enable --now serial-getty@ttyACM0.service
```
> [!TIP]
> **Terminal Window Sizing & Curses Apps (`top`, `vi`, `htop`)**:
> - Click the **Fit Host (stty)** button in Quick Keys to automatically synchronize your browser's exact terminal dimensions (`stty rows <R> cols <C>`) to the host kernel.
> - When running `clear`, the terminal wipes both the active grid and scrollback buffer cleanly without leaving residual directory listings.
> - For rich 256-color support and alternate screen buffer switching in curses applications, set `export TERM=xterm-256color` in your host shell profile.

### 2. Configure UDP Netconsole (Kernel Crash Blackbox)
Ensure kernel panic logs route to the M5StickS3 even if the host locks up:
```bash
# Add netconsole module configuration (replace with your IPs and dongle MAC)
# Syntax: netconsole=<local-port>@<local-ip>/<interface>,<remote-port>@<dongle-ip>/<dongle-mac>
echo "netconsole 4444@192.168.1.50/eth0,6666@192.168.1.150/$(cat /sys/class/net/eth0/address)" | sudo tee -a /etc/modules
```

### 3. Configure Zabbix Trapper (Alert & Crash Ingestion)
The M5StickS3 dongle acts as an active Zabbix Sender when power drops, kernel panics occur, or the serial interface `/dev/ttyACM0` disconnects / stops:
- **Zabbix Item Type**: **Zabbix trapper**
- **Type of Information**: **Text** (or **Character** / **Log**). *(Do NOT set to Numeric/Float/Integer!)*
- **Key**: Configurable in Web UI Settings (default: `system.oob.panic`).
- **Payload Content**: A structured, multi-line UTF-8 text message containing:
  - Alert Event (e.g. `[ALERT] SERIAL_LOST: Host serial interface (/dev/ttyACM0) disconnected or serial-getty stopped`, `[ALERT] POWER_LOSS`, or `[ALERT] PANIC`).
  - System Uptime and VBUS Voltage telemetry.
  - Complete trailing 10KB rolling console log snapshot safely escaped into RFC 8259-compliant JSON.

---

## Web Management & Display Telemetry

- **Firmware Build Version**: The active firmware version (e.g. `v0.1.18`, dynamically injected from the Gitea CI git release tag/commit count) and exact build timestamp (`__DATE__ __TIME__`) are displayed in the WebUI header and Diagnostics tab.
- **Independent Status Indicators**:
  - **Host Serial Status** (`Host: ACM0 Connected` vs `Host: Disconnected`): Directly monitors the physical USB CDC-ACM interface, DTR/RTS line state, and host power state.
  - **WebSocket Status** (`Web: Connected` vs `Web: Disconnected`): Monitors your browser's real-time connection to the dongle.
  - **Terminal Alert Banner**: Displays immediate warning notices above Quick Keys if `/dev/ttyACM0` is closed or host power drops.
- **On-Device LCD Visual Alarm**:
  - When serial connection is lost or server powers off, the 1.14" ST7789P3 LCD automatically wakes up, flashes red/orange every 500ms, and displays `! SERIAL LOST (ACM0) !` or `! POWER LOSS (VBUS) !`.

## Building and CI/CD

### Building with PlatformIO
```bash
# Build monolithic firmware binary
pio run -e m5sticks3

# Flash to M5StickS3
pio run -e m5sticks3 --target upload
```

### Automated CI with Gitea Actions
The repository includes `.gitea/workflows/build.yml` which automatically:
- Installs PlatformIO in an isolated virtual environment.
- Computes version tags (`vX.Y.Z` / commit count).
- Compiles `m5sticks3-kvm-${VERSION}.bin`.
- Publishes releases on git tags and deploys main-branch builds to the generic package registry.

### Loading via M5Launcher
Flash the monolithic `m5sticks3-kvm-${VERSION}.bin` via SD card or OTA in [M5Launcher](https://github.com/bmorcelli/Launcher). Because all assets are compiled into `.rodata` and all settings live in namespace `blackbox_oob`, it runs seamlessly alongside existing apps without altering partition schemes. The partition table (`partitions.csv`) allocates dual 3.75MB OTA slots (`app0` / `app1`) aligned to 64KB MMU boundaries with dedicated NVS and OTA data sectors.

---

## License

MIT License. Designed for robust, open-source server administration.

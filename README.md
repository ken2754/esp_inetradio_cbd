# ES3C28P Internet Radio & Word Clock

[日本語 (Japanese)](README_ja.md)

ESP-IDF firmware for the **ES3C28P** (`ESP32-S3-N16R8`, 2.8" `320×240` IPS display + `FT6336G` capacitive touch + `ES8311` audio codec) combining a full-bleed Internet Radio player and a colloquial English Word Clock behind a unified, button-free touch gesture UI and Web configuration portal.

---

## Target Hardware (`ES3C28P`)

- **MCU**: ESP32-S3-N16R8 (240 MHz dual-core, 16 MB QIO Flash, 8 MB Octal PSRAM)
- **Display**: 2.8" `320×240` IPS TFT (`ILI9341V`, 4-wire SPI @ 40 MHz, PWM backlight)
- **Touch**: `FT6336G` capacitive touch controller (shared I2C @ 400 kHz)
- **Audio**: `ES8311` I2S codec + `FM8002E` speaker amplifier (GPIO1 active-low shutdown)

### Hardware Pinout

| Subsystem | Signals | GPIO |
| :--- | :--- | :--- |
| **LCD (`ILI9341V`)** | `CS / DC / MOSI / SCLK / MISO / BL` | `10 / 46 / 11 / 12 / 13 / 45` (`RST` tied to `CHIP_PU`) |
| **Touch (`FT6336G`)** | `SDA / SCL / INT / RST` | `16 / 15 / 17 / 18` |
| **Audio (`ES8311` + `FM8002E`)** | `PA_EN (Active-Low) / MCLK / BCLK / DOUT / LRCK / DIN` | `1 / 4 / 5 / 8 / 7 / 6` |
| **Misc** | `BOOT` button (hold 2.5 s for SoftAP) / `RGB_LED` | `0 / 42` |

---

## Features

### 1. Internet Radio & Album Art Player
- Streams **MP3**, **AAC**, and **AAC+** stations (including `.pls` / `.m3u` playlist resolution), with PSRAM ring buffering, ICY `StreamTitle` metadata parsing (`Artist - Track`), and automatic reconnect.
- Prefetches **240×240 JPEG album cover art** in the background and renders it onto a shared `320×240` RGB565 backdrop with 8-direction outlined track/artist overlays and a bottom gradient scrim.
- Preloaded with **8 SomaFM preset stations** (*Groove Salad*, *Drone Zone*, *Space Station*, *Secret Agent*, *Lush*, *Deep Space One*, *Beat Blender*, *Indie Pop Rocks!*), customizable up to 24 stations via the Web UI or REST API.

### 2. SNTP Clock & Colloquial Word Clock
- Automatic **SNTP** time synchronization (`pool.ntp.org`, `time.google.com`, `ntp.nict.jp`) with configurable POSIX timezone (default `JST-9`).
- **Word Clock** displays natural 5-minute colloquial English phrases (e.g., `It's a quarter past three..`) with `0–4` trailing periods indicating exact remainder minutes (`minute % 5`), rendered with 8-direction black outlines over the live album art backdrop when playing.
- **5 Word Clock Font Modes** (`wc_font_mode` `0–4`):
  - `0`: **Auto** — cycles through all 4 fonts automatically at each 5-minute block
  - `1`: **Modern Sans** (`Montserrat 28`)
  - `2`: **Bold Display** (`Montserrat 32`)
  - `3`: **Compact** (`Montserrat 24`)
  - `4`: **Retro Mono** (`Unscii 16`)
- A bottom status bar temporarily shows the active track/artist (or IP/AP address when stopped) for 60 seconds and can be recalled with a single tap.

---

## Button-Free Touch Gesture UI (`320×240`)

While **Player** or **Word Clock** is active, there are zero on-screen buttons. All control uses gestures:

| View | Gesture | Condition / Threshold | Action |
| :--- | :--- | :--- | :--- |
| **Player / Word Clock** | **Main Swipe Left / Right** | `y >= 44` (or any `y` when stopped), `\|dx\| >= 35 px` | Toggles views (`Player / Stopped ↔ Word Clock`) |
| **Player / Word Clock** | **Top-Strip Swipe Left / Right** | `y < 44` while playing, `\|dx\| >= 35 px` | Switches station (Left `dx <= -35`: Next, Right `dx >= 35`: Previous) + 2 s banner |
| **Player** | **Vertical Drag** | `\|dy\| >= 14 px`, `\|dy\| > 1.2 × \|dx\|` | Adjusts **Volume** (`0–100%`) live with OSD bar; saves on release |
| **Word Clock** | **Vertical Drag** | `\|dy\| >= 14 px`, `\|dy\| > 1.2 × \|dx\|` | Adjusts **Brightness** (`10–100%`) live with OSD bar; saves on release |
| **Player / Word Clock** | **Long-Press** | Hold `>= 550 ms` (`movement < 16 px`) | Stops playback (if playing) and opens **Stopped** menu |
| **Player** | **Double-Tap** | 2 taps within `<= 280 ms`, `< 28 px` apart | Next station + 2 s station banner |
| **Word Clock** | **Double-Tap** | 2 taps within `<= 280 ms`, `< 28 px` apart | Cycles Word Clock font mode (`Auto` → `Modern Sans` → `Bold Display` → `Compact` → `Retro Mono`) |
| **Player** | **Single-Tap** | Resolved after `280 ms` window | Shows station banner (2 s) + time & gesture hint toast |
| **Word Clock** | **Single-Tap** | Resolved after `280 ms` window | Re-shows bottom status bar (60 s) + time & font hint toast |
| **Stopped** | **Header & List Tap** | Top strip buttons + station list | `Play` (resume), `Stations` tab (tap to play), `Settings` tab (Wi-Fi/AP info + `Enable AP Config Mode`), `Sleep` |
| **Stopped** | **Horizontal Swipe** | `\|dx\| >= 45 px`, `2×\|dx\| > 3×\|dy\|` | Switches to `Word Clock` |
| **Stopped** | **Idle Auto-Clock** | Configurable (`Off / 30s / 60s / 3m / 5m`) | Automatically transitions to `Word Clock` after inactivity |
| **Sleep** | **Tap Anywhere** | After `350 ms` wake guard | Wakes LCD/backlight to **Stopped** menu (external playback also wakes directly to **Player**) |

---

## Build & Flash

Requires **ESP-IDF v5.3.5+** (minimum `>= 5.3.0`).

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

> **Note**: Partition table (`partitions.csv`) allocates a `4M` `factory` app partition and `64 KB` NVS partition on 16 MB Flash.

---

## Wi-Fi & Web UI Setup

1. On first boot (or when Wi-Fi credentials are empty / fail after retries), the device starts SoftAP **`ES3C28P-Radio`** (open network) with a Captive Portal at **`http://192.168.4.1/`**.
   - You can also enter SoftAP mode anytime by holding the **BOOT** button (`GPIO0`) for **2.5 seconds** or tapping **Enable AP Config Mode** under *Stopped → Settings*.
2. Connect from a phone or PC, open `http://192.168.4.1/`, scan networks, and save your Wi-Fi SSID & password.
3. Once connected to your LAN (STA mode), access the Web UI anytime at:
   - **`http://es3c28p-radio.local/`** (mDNS)
   - **`http://<device-ip>/`** (shown in *Stopped → Settings* and *Word Clock* status)

The Web UI lets you control playback/volume, manage preset stations (add/edit/delete/reorder/reset), configure Wi-Fi, adjust display settings (brightness, autoplay, IPS color inversion, 180° flip, RGB/BGR swap), switch active views, and configure SNTP timezone, Word Clock font, and Idle Auto-Clock.

---

## REST API Reference

| Method & Path | Purpose | Request / Query / Response Details |
| :--- | :--- | :--- |
| `GET /` | Web Configuration UI | Returns the single-page HTML/JS portal |
| `GET /api/status` | System & Playback Status | Returns JSON: `state_text`, `station_index`, `station_name`, `stream_title`, `stream_artist`, `stream_track`, `codec`, `status_msg`, `sample_rate`, `bitrate_kbps`, `volume`, `muted`, `buffer_pct`, `wifi_state`, `wifi_ssid`, `ip_addr`, `ap_ssid`, `ap_ip`, `rssi`, `brightness`, `autoplay`, `invert_color`, `flip_180`, `swap_rb`, `view`, `time_str`, `time_synced` |
| `POST /api/control` | Playback, View & Volume Control | JSON body (all fields optional): `action` (`"play"` + optional `"station"`, `"stop"`, `"toggle"`, `"next"`, `"prev"`, `"mute"` + optional `"muted"`), `view` (`"player"`, `"wordclock"`, `"stopped"`, `"sleep"`), `volume` (`0–100`) |
| `GET /api/stations` | List Preset Stations | Returns JSON array of `{name, url, genre}` |
| `POST /api/stations` | Add Station | JSON `{name, url, genre?}` (max 24 stations) |
| `PUT /api/stations` | Edit or Reorder Station | Edit: `{index, name, url, genre?}` / Reorder: `{move_from, move_to}` |
| `DELETE /api/stations?index=<n>` | Delete Station | Query `index` (0-based) |
| `POST /api/stations/reset` | Reset Stations | Restores the 8 default SomaFM stations |
| `GET /api/wifi/scan` | Scan Wi-Fi Networks | Returns up to 16 APs: `[{ssid, rssi, secure}, ...]` |
| `POST /api/wifi` | Save Wi-Fi Credentials | JSON `{ssid, password?}`; persists to NVS and initiates STA connection |
| `POST /api/display` | Display & Autoplay Settings | JSON `{brightness? (10–100), autoplay?, invert_color?, flip_180?, swap_rb?}` |
| `GET /api/clock` | Clock Settings | Returns `timezone`, `wc_font_mode`, `idle_clock_sec` |
| `POST /api/clock` | Update Clock Settings | JSON (all optional): `timezone`, `wc_font_mode` (`0–4`), `idle_clock_sec` (`0–65535`) |

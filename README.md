# ES3C28P Internet Radio, Word Clock & Antigravity Quota Dashboard

[日本語 (Japanese)](README_ja.md)

ESP-IDF firmware for the **ES3C28P** (`ESP32-S3-N16R8`, 2.8" `320×240` IPS display + `FT6336G` capacitive touch + `ES8311` audio codec) combining a full-bleed Internet Radio player, a colloquial English Word Clock, and an Antigravity (`agy`) 5-slot AI quota dashboard behind a unified, button-free touch gesture UI and Web configuration portal.

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

### 3. Antigravity (`agy`) 5-Slot Quota Dashboard
- **Left Card**: Wi-Fi / IP / AP status, 12-hour digital clock + AM/PM, date, and current radio track or `Stopped` status.
- **Right Card**: 5 real-time quota progress bars with reset timestamps:
  - `Gemini 5h`
  - `Gemini Wk`
  - `Claude 5h`
  - `Claude Wk`
  - `Argon Wk`
- **Color Coding**: Green (`>= 50%`), Yellow (`30–49%`), Orange (`10–29%`), Red (`< 10%`).
- **Translucent Backdrop**: When radio playback has cover art, the `320×240` album art stays softly visible behind the cards (50% black dim).
- **On-Demand Polling**: Quota polling is **paused** in all other views (`player`, `wordclock`, `stopped`, `sleep`) to conserve CPU, bandwidth, and TLS memory; it activates automatically when viewing `agy` (polling every 60 s) or on manual refresh.

---

## Button-Free Touch Gesture UI (`320×240`)

While **Player**, **Word Clock**, or **Agy Dashboard** is active, there are zero on-screen buttons. All control uses gestures:

| View | Gesture | Condition / Threshold | Action |
| :--- | :--- | :--- | :--- |
| **Player / Word Clock / Agy** | **Main Swipe Left / Right** | `y >= 44` (or any `y` when stopped), `\|dx\| >= 35 px` | Cycles views (`Player / Stopped ↔ Word Clock ↔ Agy Dashboard`) |
| **Player / Word Clock / Agy** | **Top-Strip Swipe Left / Right** | `y < 44` while playing, `\|dx\| >= 35 px` | Switches station (Left `dx <= -35`: Next, Right `dx >= 35`: Previous) + 2 s banner |
| **Player** | **Vertical Drag** | `\|dy\| >= 14 px`, `\|dy\| > 1.2 × \|dx\|` | Adjusts **Volume** (`0–100%`) live with OSD bar; saves on release |
| **Word Clock / Agy** | **Vertical Drag** | `\|dy\| >= 14 px`, `\|dy\| > 1.2 × \|dx\|` | Adjusts **Brightness** (`10–100%`) live with OSD bar; saves on release |
| **Player / Word Clock / Agy** | **Long-Press** | Hold `>= 550 ms` (`movement < 16 px`) | Stops playback (if playing) and opens **Stopped** menu |
| **Player** | **Double-Tap** | 2 taps within `<= 280 ms`, `< 28 px` apart | Next station + 2 s station banner |
| **Word Clock** | **Double-Tap** | 2 taps within `<= 280 ms`, `< 28 px` apart | Cycles Word Clock font mode (`Auto` → `Modern Sans` → `Bold Display` → `Compact` → `Retro Mono`) |
| **Agy Dashboard** | **Double-Tap** | 2 taps within `<= 280 ms`, `< 28 px` apart | Triggers immediate manual quota refresh |
| **Player** | **Single-Tap** | Resolved after `280 ms` window | Shows station banner (2 s) + time & gesture hint toast |
| **Word Clock** | **Single-Tap** | Resolved after `280 ms` window | Re-shows bottom status bar (60 s) + time & font hint toast |
| **Agy Dashboard** | **Single-Tap** | Resolved after `280 ms` window | Shows gesture hint toast (`2xTap: Refresh \| Hold: Menu`) |
| **Stopped** | **Header & List Tap** | Top strip buttons + station list | `Play` (resume), `Stations` tab (tap to play), `Settings` tab (Wi-Fi/AP info + `Enable AP Config Mode`), `Sleep` |
| **Stopped** | **Horizontal Swipe** | `\|dx\| >= 45 px`, `2×\|dx\| > 3×\|dy\|` | Cycles to `Word Clock` / `Agy Dashboard` |
| **Stopped** | **Idle Auto-Clock** | Configurable (`Off / 30s / 60s / 3m / 5m`) | Automatically transitions to `Word Clock` or `Agy Dashboard` after inactivity |
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
   - **`http://<device-ip>/`** (shown in *Stopped → Settings*, *Word Clock* status, and *Agy Dashboard*)

The Web UI lets you control playback/volume, manage preset stations (add/edit/delete/reorder/reset), configure Wi-Fi, adjust display settings (brightness, autoplay, IPS color inversion, 180° flip, RGB/BGR swap), switch active views, and configure SNTP timezone, Word Clock font, Idle Auto-Clock, and Agy OAuth credentials.

---

## Antigravity (`agy`) OAuth Setup

Never commit OAuth secrets (`tools/client_secret.json` is listed in `.gitignore`).

### Option A: Helper Script (`tools/get_token.py`)

1. Place your Google OAuth Desktop/Web client JSON file at `tools/client_secret.json` (supports standard Google Cloud `"installed"` / `"web"` JSON or a flat `{"client_id": ..., "client_secret": ...}` object).
2. Run the helper script on your PC:
   ```bash
   python3 tools/get_token.py
   ```
   - Optional flags: `--host es3c28p-radio.local` (or device IP / `192.168.4.1`) to upload without prompting, or `--no-upload` to only print and verify the token locally.
3. The script attempts to auto-read an existing Antigravity CLI refresh token from the Linux Secret Service keyring (`org.freedesktop.secrets`), or launches a browser Google OAuth sign-in flow (`http://localhost:8085/`), verifies live quota retrieval, and uploads `oauth_client_id`, `oauth_client_secret`, and `oauth_refresh_token` to the device via `POST /api/agy` (falling back to `POST /save`).

### Option B: Web UI

Open `http://es3c28p-radio.local/` (or `http://192.168.4.1/` in AP mode), scroll to **Agy Clock & Quota → Clock & OAuth Settings**, paste your **OAuth Client ID**, **OAuth Client Secret**, and **OAuth Refresh Token**, and click **Save / Apply**.

---

## REST API Reference

| Method & Path | Purpose | Request / Query / Response Details |
| :--- | :--- | :--- |
| `GET /` | Web Configuration UI | Returns the single-page HTML/JS portal |
| `GET /api/status` | System & Playback Status | Returns JSON: `state_text`, `station_index`, `station_name`, `stream_title`, `stream_artist`, `stream_track`, `codec`, `status_msg`, `sample_rate`, `bitrate_kbps`, `volume`, `muted`, `buffer_pct`, `wifi_state`, `wifi_ssid`, `ip_addr`, `ap_ssid`, `ap_ip`, `rssi`, `brightness`, `autoplay`, `invert_color`, `flip_180`, `swap_rb`, `view`, `time_str`, `time_synced` |
| `POST /api/control` | Playback, View & Volume Control | JSON body (all fields optional): `action` (`"play"` + optional `"station"`, `"stop"`, `"toggle"`, `"next"`, `"prev"`, `"mute"` + optional `"muted"`, `"refresh_quota"`), `view` (`"player"`, `"wordclock"`, `"agy"`, `"stopped"`, `"sleep"`), `volume` (`0–100`) |
| `GET /api/stations` | List Preset Stations | Returns JSON array of `{name, url, genre}` |
| `POST /api/stations` | Add Station | JSON `{name, url, genre?}` (max 24 stations) |
| `PUT /api/stations` | Edit or Reorder Station | Edit: `{index, name, url, genre?}` / Reorder: `{move_from, move_to}` |
| `DELETE /api/stations?index=<n>` | Delete Station | Query `index` (0-based) |
| `POST /api/stations/reset` | Reset Stations | Restores the 8 default SomaFM stations |
| `GET /api/wifi/scan` | Scan Wi-Fi Networks | Returns up to 16 APs: `[{ssid, rssi, secure}, ...]` |
| `POST /api/wifi` | Save Wi-Fi Credentials | JSON `{ssid, password?}`; persists to NVS and initiates STA connection |
| `POST /api/display` | Display & Autoplay Settings | JSON `{brightness? (10–100), autoplay?, invert_color?, flip_180?, swap_rb?}` |
| `GET /api/agy` | Clock, OAuth & Quota Status | Returns `timezone`, `wc_font_mode`, `idle_clock_sec`, `idle_clock_view`, `has_cid`, `has_sec`, `has_refr`, `oauth_client_id`, and `quota` (`is_loading`, `has_error`, `error_msg`, `last_update_sec`, `windows[5]`) |
| `POST /api/agy` | Update Clock, OAuth & Refresh | JSON (all optional): `timezone`, `wc_font_mode` (`0–4`), `idle_clock_sec` (`0–65535`), `idle_clock_view` (`0\|1`), `oauth_client_id` (or `cid`), `oauth_client_secret` (or `sec`), `oauth_refresh_token` (or `refr`), `refresh` (`true` to trigger immediate fetch) |
| `POST /save` | Legacy Form Config Endpoint | `application/x-www-form-urlencoded` accepting `cid`, `sec`, `refr`, `tz`, `font` (`0–4`), `bright` (`10–100`) |

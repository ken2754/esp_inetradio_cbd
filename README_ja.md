# ES3C28P インターネットラジオ・ワードクロック

[English](README.md)

**ES3C28P**（`ESP32-S3-N16R8`、2.8インチ `320×240` IPS 液晶 + `FT6336G` 静電容量タッチ + `ES8311` オーディオコーデック）向けの ESP-IDF ファームウェアです。フルスクリーン・アルバムアート表示のインターネットラジオと英語ワードクロックを、ボタンレスのタッチジェスチャー UI とブラウザ設定ポータルで統合しています。

---

## 対象ハードウェア (`ES3C28P`)

- **MCU**: ESP32-S3-N16R8（240 MHz デュアルコア、16 MB QIO Flash、8 MB Octal PSRAM）
- **ディスプレイ**: 2.8インチ `320×240` IPS TFT（`ILI9341V`、4線 SPI @ 40 MHz、PWM バックライト）
- **タッチパネル**: `FT6336G` 静電容量タッチコントローラ（共有 I2C @ 400 kHz）
- **オーディオ**: `ES8311` I2S コーデック + `FM8002E` スピーカーアンプ（GPIO1 Active-Low シャットダウン）

### ハードウェアピンアサイン

| サブシステム | 信号 | GPIO |
| :--- | :--- | :--- |
| **LCD (`ILI9341V`)** | `CS / DC / MOSI / SCLK / MISO / BL` | `10 / 46 / 11 / 12 / 13 / 45`（`RST` は `CHIP_PU` 直結） |
| **タッチ (`FT6336G`)** | `SDA / SCL / INT / RST` | `16 / 15 / 17 / 18` |
| **オーディオ (`ES8311` + `FM8002E`)** | `PA_EN (Low有効) / MCLK / BCLK / DOUT / LRCK / DIN` | `1 / 4 / 5 / 8 / 7 / 6` |
| **その他** | `BOOT` ボタン（2.5秒長押しで SoftAP 起動） / `RGB_LED` | `0 / 42` |

---

## 主な機能

### 1. インターネットラジオ & アルバムアート再生
- **MP3 / AAC / AAC+** ストリーム再生（`.pls` / `.m3u` プレイリスト解決、PSRAM 大容量リングバッファ、ICY `StreamTitle` による `Artist - Track` 解析、自動再接続対応）。
- バックグラウンドで **240×240 JPEG アルバムジャケット** を取得し、共有 `320×240` RGB565 背景へ描画。8方向黒縁取りテキストと下部グラデーションスクリムにより高い視認性を確保しています。
- 初期プリセットとして **SomaFM 8局**（*Groove Salad*, *Drone Zone*, *Space Station*, *Secret Agent*, *Lush*, *Deep Space One*, *Beat Blender*, *Indie Pop Rocks!*）を収録。Web UI / REST API から最大 24局まで登録・編集・並び替え可能です。

### 2. SNTP 時刻同期 & 英語ワードクロック
- **SNTP** 自動時刻同期（`pool.ntp.org` / `time.google.com` / `ntp.nict.jp`）および POSIX タイムゾーン設定（デフォルト `JST-9`）に対応。
- **Word Clock** は 5分刻みの自然な英語表現（例：`It's a quarter past three..`）を表示し、末尾のピリオド数（`0〜4` 個）で端数の分（`minute % 5`）を表現します。ラジオ再生中はアルバムアート背景の上に8方向黒縁取り文字で表示されます。
- **5種類のフォントモード**（`wc_font_mode` `0〜4`）:
  - `0`: **Auto** — 5分ブロックごとに 4種類のフォントを自動ローテーション
  - `1`: **Modern Sans** (`Montserrat 28`)
  - `2`: **Bold Display** (`Montserrat 32`)
  - `3`: **Compact** (`Montserrat 24`)
  - `4`: **Retro Mono** (`Unscii 16`)
- 画面下部のステータスバーに再生中の曲名／アーティスト（停止時は IP / AP 情報）を 60秒間表示し、シングルタップでいつでも再表示できます。

---

## ボタンレス・タッチジェスチャー UI (`320×240`)

**Player**・**Word Clock** の各画面には画面上の操作ボタンがなく、すべてタッチジェスチャーで操作します。

| 画面 | ジェスチャー | 条件 / しきい値 | 動作 |
| :--- | :--- | :--- | :--- |
| **Player / Word Clock** | **メイン領域 左右スワイプ** | `y >= 44`（停止時は全領域）、`\|dx\| >= 35 px` | 画面を水平切替（`Player / Stopped ↔ Word Clock`） |
| **Player / Word Clock** | **上部ストリップ 左右スワイプ** | 再生中に `y < 44`、`\|dx\| >= 35 px` | 選局（左 `dx <= -35`: 次の局、右 `dx >= 35`: 前の局）＋ 2秒バナー表示 |
| **Player** | **上下ドラッグ** | `\|dy\| >= 14 px`、`\|dy\| > 1.2 × \|dx\|` | **音量**（`0〜100%`）を OSD バー付きでリアルタイム調整、離すと保存 |
| **Word Clock** | **上下ドラッグ** | `\|dy\| >= 14 px`、`\|dy\| > 1.2 × \|dx\|` | **画面輝度**（`10〜100%`）を OSD バー付きでリアルタイム調整、離すと保存 |
| **Player / Word Clock** | **長押し（ホールド）** | `>= 550 ms`（移動量 `< 16 px`） | 再生を停止し、**Stopped** メニュー画面へ移行 |
| **Player** | **ダブルタップ** | `<= 280 ms` 以内に 2回（間隔 `< 28 px`） | 次の局へ切替 ＋ 2秒局名バナー表示 |
| **Word Clock** | **ダブルタップ** | `<= 280 ms` 以内に 2回（間隔 `< 28 px`） | フォントモード切替（`Auto` → `Modern Sans` → `Bold Display` → `Compact` → `Retro Mono`） |
| **Player** | **シングルタップ** | `280 ms` 判定ウィンドウ後に確定 | 局名バナー（2秒）＋ 現在時刻・操作ヒントトーストを表示 |
| **Word Clock** | **シングルタップ** | `280 ms` 判定ウィンドウ後に確定 | 下部ステータスバー（60秒）を再表示 ＋ 時刻・フォントヒントを表示 |
| **Stopped** | **ヘッダー & リスト操作** | 上部ボタン + 放送局リスト | `Play`（再生再開）、`Stations` タブ（タップで選局再生）、`Settings` タブ（Wi-Fi/AP 情報 + `Enable AP Config Mode`）、`Sleep` |
| **Stopped** | **左右スワイプ** | `\|dx\| >= 45 px`、`2×\|dx\| > 3×\|dy\|` | `Word Clock` へ切替 |
| **Stopped** | **アイドル自動時計移行** | 設定可能（`Off / 30s / 60s / 3m / 5m`） | 無操作時間が経過すると自動で `Word Clock` へ切替 |
| **Sleep** | **画面のどこかをタップ** | スリープ突入後 `350 ms` ガード以降 | LCD・バックライトを復帰し **Stopped** 画面へ戻る（Web UI からの再生開始時は **Player** へ直接復帰） |

---

## ビルドと書き込み

**ESP-IDF v5.3.5 以上**（最小要件 `>= 5.3.0`）が必要です。

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

> **補足**: パーティション構成（`partitions.csv`）は 16 MB Flash 上に `4M` の `factory` アプリ領域と `64 KB` の NVS 領域を確保します。

---

## Wi-Fi 設定 & Web UI

1. 初回起動時（または Wi-Fi 未設定・接続失敗時）、本体は暗号化なしの SoftAP **`ES3C28P-Radio`** を起動し、**`http://192.168.4.1/`** で Captive Portal が開きます。
   - 稼働中でも本体 **`BOOT` ボタン（GPIO0）を 2.5秒長押し**、または *Stopped → Settings* の **Enable AP Config Mode** からいつでも AP モードを起動できます。
2. PC やスマホから `ES3C28P-Radio` に接続し、ブラウザで `http://192.168.4.1/` を開いて周辺 Wi-Fi をスキャンし、SSID とパスワードを保存します。
3. Wi-Fi 接続（STA モード）完了後は、以下の URL からいつでも Web UI にアクセスできます:
   - **`http://es3c28p-radio.local/`** (mDNS)
   - **`http://<画面に表示されたIP>/`**（*Stopped → Settings*、*Word Clock* 下部で確認可能）

Web UI からは、再生・音量操作、プリセット局の追加・編集・削除・並び替え・リセット、Wi-Fi 設定、ディスプレイ設定（輝度、自動再生、IPS 色反転、180度回転、RGB/BGR 入替）、表示画面切替、SNTP タイムゾーン・Word Clock フォント・アイドル自動時計の設定が行えます。

---

## REST API リファレンス

| メソッド & パス | 用途 | リクエスト / クエリ / レスポンス仕様 |
| :--- | :--- | :--- |
| `GET /` | Web 設定ポータル | シングルページ HTML/JS UI を返却 |
| `GET /api/status` | 状態取得 | JSON を返却：`state_text`, `station_index`, `station_name`, `stream_title`, `stream_artist`, `stream_track`, `codec`, `status_msg`, `sample_rate`, `bitrate_kbps`, `volume`, `muted`, `buffer_pct`, `wifi_state`, `wifi_ssid`, `ip_addr`, `ap_ssid`, `ap_ip`, `rssi`, `brightness`, `autoplay`, `invert_color`, `flip_180`, `swap_rb`, `view`, `time_str`, `time_synced` |
| `POST /api/control` | 再生・画面・音量制御 | JSON（各フィールド任意）：`action`（`"play"` + 任意 `"station"`, `"stop"`, `"toggle"`, `"next"`, `"prev"`, `"mute"` + 任意 `"muted"`）、`view`（`"player"`, `"wordclock"`, `"stopped"`, `"sleep"`）、`volume`（`0〜100`） |
| `GET /api/stations` | 放送局一覧取得 | `[{name, url, genre}, ...]` の JSON 配列を返却 |
| `POST /api/stations` | 放送局追加 | JSON `{name, url, genre?}`（最大 24局） |
| `PUT /api/stations` | 放送局編集・並び替え | 編集：`{index, name, url, genre?}` / 並び替え：`{move_from, move_to}` |
| `DELETE /api/stations?index=<n>` | 放送局削除 | クエリ `index`（0始まり） |
| `POST /api/stations/reset` | 放送局リセット | デフォルトの SomaFM 8局へ復元 |
| `GET /api/wifi/scan` | Wi-Fi スキャン | 最大 16件の AP 一覧：`[{ssid, rssi, secure}, ...]` |
| `POST /api/wifi` | Wi-Fi 設定保存 | JSON `{ssid, password?}`。NVS へ保存して STA 接続を開始 |
| `POST /api/display` | 表示・自動再生設定 | JSON `{brightness? (10–100), autoplay?, invert_color?, flip_180?, swap_rb?}` |
| `GET /api/clock` | 時計設定取得 | `timezone`, `wc_font_mode`, `idle_clock_sec` を返却 |
| `POST /api/clock` | 時計設定更新 | JSON（各フィールド任意）：`timezone`, `wc_font_mode`（`0〜4`）, `idle_clock_sec`（`0〜65535`） |

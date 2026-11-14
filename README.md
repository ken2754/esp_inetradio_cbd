# ES3C28P Internet Radio (ESP-IDF)

ESP32-S3-N16R8 搭載の **ES3C28P (2.8" Cheap Black Display)** 向けインターネットラジオファームウェアです（ESP-IDF v5.3+）。

## 主な機能

- **内蔵スピーカー再生**: `ES8311` オーディオコーデック + `FM8002E` パワーアンプ (GPIO1 Active-Low) + I2S による MP3 / AAC / AAC+ ストリーミング再生（ICY メタデータ `StreamTitle` 対応、PSRAM 大容量リングバッファ、自動再接続）
- **静電容量タッチ UI (LVGL v9)**: `ILI9341V` (320x240 IPS) + `FT6336G` タッチパネルで選局・再生/停止・音量調整・ミュート・ネットワーク確認が可能
- **ブラウザ経由の設定 Web サーバー**:
  - 初回起動時や Wi-Fi 未接続時は SoftAP (`ES3C28P-Radio` / `http://192.168.4.1/`) ＋ Captive Portal で動作
  - 周辺 Wi-Fi スキャン・SSID/パスワード設定
  - プリセット局の再生・追加・編集・削除・並び替え・リセット
  - ディスプレイ設定（輝度、自動再生、IPS色反転、180度回転、RGB/BGR切替）をリアルタイム反映
- **SomaFM デフォルト8局プリセット**:
  1. SomaFM: Groove Salad
  2. SomaFM: Drone Zone
  3. SomaFM: Space Station
  4. SomaFM: Secret Agent
  5. SomaFM: Lush
  6. SomaFM: Deep Space One
  7. SomaFM: Beat Blender
  8. SomaFM: Indie Pop Rocks!

## ハードウェアピンアサイン (ES3C28P)

| 機能 | 信号 | GPIO |
| :--- | :--- | :--- |
| LCD (ILI9341V) | CS / DC / MOSI / SCLK / MISO / BL | 10 / 46 / 11 / 12 / 13 / 45 |
| Touch (FT6336G) | SDA / SCL / INT / RST | 16 / 15 / 17 / 18 |
| Audio (ES8311 + FM8002E) | PA_EN(Low有効) / MCLK / BCLK / DOUT / LRCK / DIN | 1 / 4 / 5 / 6 / 7 / 8 |
| BOOT ボタン | 長押し (2.5秒) で AP モード起動 | 0 |

## ビルドと書き込み

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## 使い方

1. 電源投入後、Wi-Fi 未設定の場合は画面に AP モード案内が表示されます。
2. PC やスマホから Wi-Fi `ES3C28P-Radio`（パスワードなし）に接続し、ブラウザで `http://192.168.4.1/` を開きます。
3. 「Wi-Fi 設定」からご自宅の Wi-Fi を設定すると、自動的に接続して SomaFM の再生が始まります。
4. 以降は `http://<画面に表示されたIP>/` または `http://es3c28p-radio.local/` からいつでもブラウザ設定できます。

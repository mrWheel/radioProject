# ESP32-S3 Internet Radio

Native ESP-IDF 5.2+ project for VS Code, the TFT-LCD-Display-EC11 piggyback board and a PCM5102A I2S DAC. It uses MichMich's captive-portal Wi-Fi provisioner, mrWheel's FTP server, LittleFS station storage and Espressif's MP3/AAC decoder.

## Included behavior

- Normal mode is **Volume**. Rotate EC11 to change volume.
- Press EN-push to enter **Station selection**.
- Rotate to browse; only rotation resets the 20-second inactivity timer.
- Press EN-push to start the highlighted station and return to Volume.
- After 20 seconds without rotation, selection is cancelled and Volume returns.
- GPIO 1 is a reserved, debounced auxiliary button; it currently only logs an event.
- `stations.json` is uploaded in the LittleFS image and can later be replaced over FTP.

## Prerequisites

- VS Code with Espressif's ESP-IDF extension
- ESP-IDF 5.2 through 5.5
- An ESP32-S3 board with 4 MB flash matching TheGrooveboxProject R3 pinout

Managed dependencies are downloaded on the first configure. The two explicitly requested components are vendored under `components/` with their upstream licences.

## Build and flash

```sh
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The build uses the custom 4 MB partition table and automatically creates and flashes the `storage` LittleFS partition from `littlefs/`.

On first boot, connect to the `Internet-Radio-Setup` access point. The captive portal stores Wi-Fi credentials in NVS. After connection, FTP is available on port 21, rooted at `/littlefs`. FTP is unencrypted and unauthenticated: use it only on a trusted LAN.

## Edit stations

`littlefs/stations.json` accepts up to 32 stations:

```json
{"version":1,"stations":[{"name":"Example","url":"https://example.net/live.mp3","codec":"mp3"}]}
```

Supported codec values are `mp3` and `aac`. Plain HTTP and HTTPS URLs work; TLS uses ESP-IDF's certificate bundle. After changing the file over FTP, reboot to reload it.

## Hardware defaults

The defaults are copied from `mrWheel/TheGrooveboxProject` `platformio.ini`: TFT BL/RST/CS/SCLK/MOSI/DC = 2/4/5/12/11/15; EC11 push/A/B = 6/16/17; auxiliary = 1; I2S BCLK/LRCLK/DATA/enable = 38/40/42/41. Display size is 320×240.

Change them under `idf.py menuconfig` → **Component config → Radio hardware**. Defaults remain fixed when menuconfig is not used.

## Architecture

- `radio_storage`: mounts LittleFS and creates its flash image
- `station_store`: validates and loads `stations.json`
- `radio_input`: non-blocking EC11 and button polling
- `radio_display`: ESP-IDF `esp_lcd` ST7789 driver and UI state indication
- `radio_audio`: HTTP(S) → Espressif MP3/AAC decoder → PCM volume → I2S
- `ftp_service`: exposes the mounted LittleFS through mrWheel's server
- `main`: application state machine and the exact 20-second rule

Station names and mode changes are also printed to the serial monitor. The compact native display renderer uses distinct Volume/Selection screens without pulling in a large GUI framework.

## Notes

- The sample stream URLs are third-party endpoints and can change; replace them when necessary.
- Some AAC stations use HLS (`.m3u8`), which this compact HTTP pipeline does not parse. Use a direct AAC/ADTS stream.
- This project is GPL-3.0-or-later as a combined work because both requested vendored components use that licence. See each component's `LICENSE`.


# ESP32-S3 Internet Radio

Native ESP-IDF 5.2+ project for VS Code, the TFT-LCD-Display-EC11 piggyback board and a PCM5102A I2S DAC. It uses MichMich's captive-portal Wi-Fi provisioner, LittleFS station storage and Espressif's MP3/AAC decoder.

## Included behavior

- Normal mode is **Volume**. Rotate EC11 to change volume.
- Press EN-push to enter **Station selection**.
- Rotate to browse; only rotation resets the 20-second inactivity timer.
- Press EN-push to start the highlighted station and return to Volume.
- After 20 seconds without rotation, selection is cancelled and Volume returns.
- GPIO 1 is a reserved, debounced auxiliary button; it currently only logs an event.
- `stations.json` is uploaded in the LittleFS image.

## Prerequisites

- VS Code with Espressif's ESP-IDF extension
- ESP-IDF 5.2 or newer (developed and tested against 6.0.2)
- An ESP32-S3 board with 4 MB flash and octal PSRAM, matching TheGrooveboxProject R3 pinout

Managed dependencies are downloaded on the first configure. The two explicitly requested components are vendored under `components/` with their upstream licences.

## Build and flash

```sh
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The build uses the custom 4 MB partition table and automatically creates and flashes the `storage` LittleFS partition from `littlefs/`.

On first boot, connect to the `Internet-Radio-Setup` access point. The captive portal stores Wi-Fi credentials in NVS.

## Edit stations

`littlefs/stations.json` accepts up to 32 stations:

```json
{"version":1,"stations":[{"name":"Example","url":"https://example.net/live.mp3","codec":"mp3"}]}
```

Supported codec values are `mp3` and `aac`. Plain HTTP and HTTPS URLs work; TLS uses ESP-IDF's certificate bundle. Rebuild and flash the LittleFS image after changing the file.

## Hardware defaults

The TFT/EC11/auxiliary pins are copied from `mrWheel/TheGrooveboxProject` `platformio.ini` and live in the `esp32_s3_piggyback` component's own Kconfig: TFT BL/RST/CS/SCLK/MOSI/DC = 2/4/5/12/11/15; EC11 push/A/B = 6/16/17; auxiliary = 1. Display size is 320×240.

The I2S pins live in `radio_board`'s Kconfig: BCLK/LRCLK/DATA = 38/40/42. The PCM5102A's enable pin is disabled by default (`-1`); set `RADIO_I2S_ENABLE_GPIO` if your board wires one.

Change them under `idf.py menuconfig` → **Component config → TFT LCD Display EC11** (display/encoder pins) or **→ Radio hardware** (I2S pins, encoder direction, selection timeout, default volume). Defaults remain fixed when menuconfig is not used.

## Architecture

- `radio_storage`: mounts LittleFS and creates its flash image
- `station_store`: validates and loads `stations.json`
- `radio_settings`: persists the last-played station index in NVS
- `radio_board`: Kconfig-driven I2S pin/timeout/volume defaults shared by other components
- `wifi_provisioner` (MichMich): saved-credential Wi-Fi connection with captive-portal fallback
- `radio_input`: EC11 rotation/button events from the piggyback component, dispatched to the UI task
- `radio_display`: renders the Volume/Selection/Status screens through mrWheel's `esp32_s3_piggyback` component (never drives the ST7789/EC11 hardware directly)
- `radio_audio`: HTTP(S) fetch and the Espressif MP3/AAC decoder → I2S output, split across two tasks (see below)
- `main`: application state machine and the exact 20-second rule

Station names and mode changes are also printed to the serial monitor. The compact native display renderer uses distinct Volume/Selection screens without pulling in a large GUI framework.

### Audio pipeline

`radio_audio` runs two pinned tasks connected by a 128 KB PSRAM ring buffer, so a network stall can never stall the I2S feed directly. Playback only starts once the buffer is at least 50% full (`BUF_PREFILL_THRESHOLD`), trading a slightly longer startup wait for more margin against early network hiccups:

- `radio_fetch` (core 0, alongside the Wi-Fi driver task): owns the HTTP(S)/ICY connection, strips interleaved ICY `StreamTitle` metadata, and pushes raw compressed audio bytes into the buffer.
- `radio_stream` (core 1): pulls bytes from the buffer, decodes MP3/AAC via `esp_audio_simple_dec`, applies bounded software volume, and writes PCM to I2S.

The I2S channel is also configured with extra DMA margin (~174 ms instead of the ESP-IDF default ~33 ms) and `auto_clear` enabled, so an underrun emits silence instead of repeating stale samples.

## Notes

- The sample stream URLs are third-party endpoints and can change; replace them when necessary.
- Some AAC stations use HLS (`.m3u8`), which this compact HTTP pipeline does not parse. Use a direct AAC/ADTS stream.
- This project is GPL-3.0-or-later as a combined work because both requested vendored components use that licence. See each component's `LICENSE`.


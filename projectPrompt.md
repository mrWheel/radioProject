# Project Prompt — ESP32-S3 Internet Radio

Build and maintain a production-oriented VS Code ESP-IDF internet-radio firmware for the ESP32-S3 TFT-LCD-Display-EC11 piggyback board.

## Non-negotiable requirements

1. Use native ESP-IDF (not Arduino) and keep the project directly usable with the Espressif VS Code extension.
2. Use MichMich's `esp-idf-wifi-provisioner` component for saved-credential connection and captive-portal fallback.
3. Use mrWheel's native `ftp-server` component. Expose the already-mounted LittleFS root so `stations.json` can be maintained on a trusted LAN. Clearly warn that FTP is unencrypted.
4. Use mrWheel's `esp32_s3_piggyback` component from `https://github.com/mrWheel/esp32-s3-Piggyback`. Do NOT try to drive this board yourself. Read de API-documentation in the `README.md` file.
5. Store preferred stations in `/littlefs/stations.json`. Validate bounded name, URL and codec fields and never crash on malformed input.
6. Default UI mode is Volume. Rotation changes volume. EN-push opens Station Selection. Rotation browses stations and is the only action that restarts its 20-second inactivity timeout. EN-push selects/starts the highlighted station and returns to Volume. Timeout cancels browsing and returns to Volume. Reserve GPIO 1's button for future functionality.
7. Keep networking, decoding, UI/input and file operations in separate components/tasks. No UI loop may block on network traffic and I2S functioning.
8. Support direct HTTP/HTTPS MP3 and AAC streams. Feed decoded 16-bit PCM to the PCM5102A over I2S and apply bounded software volume.
9. Embed the initial LittleFS content in the normal build/flash workflow using a custom 8 MB partition table.
10. Use messaging for. Make the `Internet->I2S` path the most important path that should NOT be interupted ().

## Hardware defaults

- TFT BL 2, RST 4, CS 5, SCLK 12, MOSI 11, DC 15; 320×240
- EC11 EN-push 6, A 16, B 17
- Reserved auxiliary button 1
- I2S BCLK 38, LRCLK 40, DATA 42, enable 41

## Quality bar

Keep public component APIs small, bound all arrays and JSON reads, check ESP-IDF errors, avoid secrets in source control, preserve upstream licences, document build/flash/provisioning/FTP/station editing, and verify both compilation and the generated LittleFS image before release. Treat stream reconnect, metadata parsing, visual typography, credential reset, and safe live station-file reload as incremental enhancements without changing the required control semantics.

## VSCode/ESP-IDF

The idf.py command is in 'source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"'
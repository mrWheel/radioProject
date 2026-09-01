# ESP32-S3 Internet Radio

ESP32-S3 Internet Radio is firmware for the TFT-LCD-Display-EC11 piggyback board.
It is built with native ESP-IDF and is intended for use from VS Code.
The radio connects to a Wi-Fi network using saved credentials.
When no credentials are available, a captive portal assists with provisioning.

Use the EC11 rotary encoder for everyday operation.
In the default Volume mode, turning the encoder adjusts the audio level.
Press the encoder to browse the configured internet radio stations.
Turning the encoder selects a station, and pressing it starts playback.
The selection screen returns to Volume mode after twenty seconds of inactivity.

Stations are stored in a LittleFS JSON file and can be managed in the web UI.
The browser interface provides station browsing, playback status and volume control.
Station changes and volume changes stay synchronised with the physical controls.
The audio pipeline supports direct HTTP and HTTPS MP3 and AAC streams.
ICY stream titles are shown on both the TFT display and the web interface.

Compressed audio is buffered between the network and decoding tasks.
This keeps temporary network stalls from interrupting the I2S audio output.
The PCM5102A DAC receives decoded 16-bit PCM over I2S.
The standard build creates a LittleFS image containing the web UI and stations.
The project targets an ESP32-S3 with the supplied 4 MB partition layout.
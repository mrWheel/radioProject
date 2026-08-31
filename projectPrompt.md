# Project Prompt — ESP32-S3 Internet Radio

Build and maintain a production-oriented VS Code ESP-IDF internet-radio firmware for the ESP32-S3 TFT-LCD-Display-EC11 piggyback board.

## Non-negotiable requirements

1. Use native ESP-IDF (not Arduino) and keep the project directly usable with the Espressif VS Code extension.
2. Use MichMich's `esp-idf-wifi-provisioner` component for saved-credential connection and captive-portal fallback.
3. Use mrWheel's `esp32_s3_piggyback` component from `https://github.com/mrWheel/esp32-s3-Piggyback`. Do NOT try to drive this board yourself. Read de API-documentation in the `README.md` file.
4. Store preferred stations in `/littlefs/stations.json`. Validate bounded name, URL and codec fields and never crash on malformed input.
5. Default UI mode is Volume. Rotation changes volume. EN-push opens Station Selection. Rotation browses stations and is the only action that restarts its 20-second inactivity timeout. EN-push selects/starts the highlighted station and returns to Volume. Timeout cancels browsing and returns to Volume. Reserve GPIO 1's button for future functionality.
6. Keep networking, decoding, UI/input and file operations in separate components/tasks. No UI loop may block on network traffic and I2S functioning.
7. Support direct HTTP/HTTPS MP3 and AAC streams. Feed decoded 16-bit PCM to the PCM5102A over I2S and apply bounded software volume.
8. Embed the initial LittleFS content in the normal build/flash workflow using a custom partition table. Active default is 4 MB (`partitions/radio_4mb.csv`); an 8 MB variant (`partitions/radio_8mb.csv`) is available for boards with more flash.
9. Decouple network fetch from decoding/I2S with a buffer between them (messaging), so the `Internet->I2S` path is the most important path and is never interrupted by network stalls.
10. Include a native embedded web UI for station browsing and basic controls. The implementation must fit the ESP-IDF firmware architecture and preserve the audio pipeline. The web UI's HTML, CSS and JS live as plain files in `/littlefs` (`littlefs/index.html`, `littlefs/style.css`, `littlefs/app.js`), served from the mounted LittleFS filesystem by `esp_http_server`, consistent with requirement 8's LittleFS image embedding — they must not be embedded as C string literals in the firmware.
11. The embedded web UI uses the native ESP-IDF `esp_http_server` WebSocket support (`CONFIG_HTTPD_WS_SUPPORT=y`) on the `/ws` endpoint for real-time state push (station/volume/track) and command handling, in addition to the existing `/api/state` and `/api/command` HTTP endpoints which remain available. The WebSocket data handler only receives, validates and queues commands; long-running work (station switching, filesystem writes) runs in a dedicated worker task, never directly in the WebSocket callback. Every command acknowledgement (including `volumeSet`) must return the complete current state (actual volume from `radio_audio`, `playing`, station, track) rather than a partial payload, so the browser never resets fields it wasn't told about. A station switched from the web UI must persist via `radio_settings_save()`, the same as a station switched from the physical EC11 control, so it remains the default across a reboot.
12. ICY `StreamTitle` metadata is split exactly once, in `main/app_main.c` (`build_icy_lines()` inside `title_cb`), into up to 3 display-ready lines, so neither the display nor the web GUI has to re-decide it — both consumers always end up showing the same 3 lines. The split first breaks on the earliest `"<space>TOKEN<space>"` separator it recognizes (`split_icy_segments()` / `find_icy_separator()`: `" - "`, `" | "`, `" / "`, `" ~ "`, `" :: "`, en/em dash, bullet, middot — extend the list as new stations turn up), taking at most 2 splits so a title is never cut into more than 3 segments; any separators beyond that stay embedded as literal text in the 3rd segment instead of being lost. Segments are then laid out strictly left-to-right: each one takes as many of the remaining lines as it needs — word-wrapped on single spaces (`emit_wrapped()`, never splitting a multi-byte UTF-8 character) if it's wider than the display's character budget (`radio_display_title_max_chars()`, the same width/scale formula `draw_now_playing_line()` draws with) — before the next segment gets a turn, so a single unseparated long title can use all 3 lines while the leftmost, most important segment is never sacrificed to make room for a later one. A later segment (or the tail of one) that no longer fits the 3-line budget is simply dropped. A line with nothing to show falls back to a single dash `-`, never an empty string. Both consumers receive `(line1, line2, line3)` (`radio_display_now_playing()`, `web_gui_notify_title()`), render them as three separate, vertically stacked fields, and only redraw/rebroadcast when at least one of the three actually changed since the last value. On the physical TFT display these three lines are horizontally centered and drawn at a larger font scale (`2`, up from the rest of the Volume screen's `1`) than surrounding text.
13. Only add/select stations with an advertised bitrate (`icy-br`) of **128 kbps or lower**. A 320 kbps HTTPS station (BluesMusicFan Radio, `orbit.citrus3.com`) was observed stalling the ring buffer for multiple seconds — a single `esp_http_client_read()` call blocked over a second — because the network path/server (and, on HTTPS, the extra TLS-decrypt cost sharing core 0 with the WiFi driver) could not sustain that bitrate reliably to this board. The stall-reconnect watchdog (requirement 15) bounds the damage but does not fix the root cause, so `stations.json` entries above 128 kbps must be treated as unsupported/best-effort, not added as primary stations.
14. The physical EC11/TFT UI and the web GUI must never go out of sync on station or volume:
    - A station or volume change made on the physical device (`main/app_main.c`'s `ui_task()`) is immediately mirrored to every connected web GUI client via `web_gui_notify_device_state()`, which updates `web_gui`'s own station-index tracking and broadcasts full state over `/ws`.
    - A station or volume change made from the web GUI (`web_gui_apply_command()`) is mirrored back onto the physical device via the `web_gui_set_state_applied_cb()` callback (`on_web_gui_state_applied()` in `app_main.c`), which updates the EC11 state (`s.playing`/`s.selected`/`s.volume`) and redraws the TFT.
    - On boot, once the saved station has started playing and `web_gui_init()` has run, `web_gui_notify_device_state()` is called once so the web GUI's initial `getState` reports the actually-restored station/volume instead of a stale default.
    - When switching to a different station, audio is fully muted (`radio_audio`'s existing `s_muted`/warm-up mechanism — no PCM is written to the I2S bus while muted) until the new stream has decoded real audio frames. For the whole mute window, both the TFT and the web GUI show the centered placeholder text `"Switch Station .."` on line 1 (`radio_audio_set_mute_callback()` → `on_audio_mute_changed()` in `app_main.c`, lines 2/3 = `"-"`), reverting to the normal 3-line ICY split (see requirement 12) only once the stream is confirmed playing.
15. Two independent, complementary reactions to network trouble live in `radio_audio.c`:
    - **Stall-reconnect watchdog**: `stream_task()` counts consecutive ring-buffer underruns (`s_buf_consecutive_underruns`, reset the moment data flows again). After `STALL_UNDERRUN_THRESHOLD` (100, roughly 5 s of continuous starvation) it sets `s_reconnect_requested`; `fetch_task()` checks that flag every loop iteration and, if set, closes and reopens the HTTP connection to the same URL (preserving the verified/unverified TLS retry logic) instead of waiting indefinitely on a stalled read.
    - **"Stream stalled" indicator**: `radio_audio_set_stall_callback()` fires `(stalled=true, ctx)` whenever `fetch_task()` ends *without* a deliberate stop/switch (`s_stop_requested` still false) — i.e. the connection never opened after all retries, an HTTP redirect could not be resolved, an ICY/stream read failed outright, or a post-stall reconnect itself failed. `app_main.c`'s `stall_cb()` reacts by showing the centered placeholder text `"Stream stalled"` on line 1 on both the TFT (`radio_display_now_playing()`) and the web GUI (`web_gui_notify_title()`), lines 2/3 = `"-"`, the same convention used for `"Switch Station .."`. The callback fires `(stalled=false, ctx)` at the top of every fresh `fetch_task()` run to clear a stale indicator before a new attempt.

## Hardware defaults

- TFT BL 2, RST 4, CS 5, SCLK 12, MOSI 11, DC 15; 320×240
- EC11 EN-push 6, A 16, B 17
- Reserved auxiliary button 1
- I2S BCLK 38, LRCLK 40, DATA 42; DAC enable disabled by default (-1), override via menuconfig if your board needs one

## Quality bar

Keep public component APIs small, bound all arrays and JSON reads, check ESP-IDF errors, avoid secrets in source control, preserve upstream licences, document build/flash/provisioning/station editing, and verify both compilation and the generated LittleFS image before release. Treat stream reconnect, metadata parsing, visual typography, credential reset, and safe live station-file reload as incremental enhancements without changing the required control semantics.

## VSCode/ESP-IDF

The idf.py command is in 'source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"'

## Radio station debug and browser-comparison requirements

The project must provide diagnostic output that explains why a station works in a browser but fails or behaves differently on the ESP32-S3. The goal is not to redesign the player. The goal is to make the monitor logs self-explanatory for every station-open attempt.

### Debugging objective

For every station URL attempt, the output must clearly answer:

- which station is being opened and which exact URL is used;
- whether the URL is HTTP or HTTPS;
- whether the hostname resolves correctly and which IP addresses were returned;
- whether TCP connection and TLS negotiation succeed;
- which HTTP status and response headers were returned;
- whether a redirect occurred and where it leads;
- whether the response is audio, HTML, JSON/XML, a playlist, HLS/DASH, or an error page;
- whether ICY metadata is announced and whether `icy-metaint` is present;
- whether the first payload bytes look like MP3, AAC, Ogg, FLAC, M3U, PLS, HTML, or another format;
- which decoder was selected and whether it started successfully;
- what the first decoder or read failure was;
- why the stream stopped, including EOF, timeout, reset, TLS failure, or decoder failure;
- what ESP-IDF error code and `errno` were involved;
- how much heap was available before and after the connection attempt.

### Logging structure and standards

Use structured logging grouped by subsystem and maintain clear TAG values such as:

- `RADIO`
- `RADIO_HTTP`
- `RADIO_TLS`
- `RADIO_STREAM`
- `RADIO_CODEC`
- `RADIO_BUFFER`

Use:

- `ESP_LOGE()` for actual failures;
- `ESP_LOGW()` for suspicious but recoverable conditions;
- `ESP_LOGI()` for important connection and stream facts;
- `ESP_LOGD()` for lower-level diagnostics;
- `ESP_LOGV()` only for very detailed per-packet analysis.

Do not flood the monitor during normal playback. Most diagnostic output should occur during station selection, URL opening, DNS/TCP/TLS resolution, HTTP response inspection, payload sniffing, decoder startup, and final teardown.

### Required output patterns

Every station attempt must begin with a clear connection banner showing:

- station name;
- input URL;
- free heap before open;
- minimum free heap so far.

At the end of a failed attempt, print a matching summary showing:

- station name;
- final URL;
- HTTP status if available;
- response Content-Type;
- failure reason;
- ESP-IDF error code or `errno` value.

For successful streams, print a final summary showing:

- final URL;
- codec selected;
- content type;
- final status.

### URL, DNS, TCP, and TLS diagnostics

The project must log the parsed URL fields before opening a network connection:

- scheme;
- host;
- port;
- path.

Log DNS resolution results for all practical families, especially IPv4 and IPv6. If DNS fails, log the exact `getaddrinfo` error rather than hiding it behind a generic connection failure.

Log TCP connect success or failure explicitly, including `errno` and `strerror(errno)` whenever meaningful.

For HTTPS streams, log:

- HTTPS detected;
- certificate bundle setup;
- SNI hostname where applicable;
- TLS handshake success/failure;
- certificate verification result;
- esp-tls/mbedTLS errors if available;
- unsupported protocol/cipher or hostname mismatch warnings.

Do not globally disable certificate verification as a workaround. The purpose is diagnosis, not hiding TLS errors.

### TLS verification fallback (approved, bounded exception)

Some broadcaster CDNs (e.g. streamtheworld.com, used by Radio 538) serve a certificate chain whose topmost cert is a legacy/deprecated root (e.g. "Go Daddy Class 2 Certification Authority") that Mozilla's trust store — and therefore `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` — no longer includes, even though a trusted intermediate ("G2") exists further down the chain. `esp_crt_bundle_attach`'s mbedTLS callback matches only the topmost chain cert against the bundle and does not backtrack like a browser, so verified TLS hard-fails for these hosts no matter how complete the bundle is.

The approved, generic fix (`create_http_client()` / `open_with_retries()` in [radio_audio.c](components/radio_audio/radio_audio.c)) is:

1. Attempt the connection with full certificate verification (`crt_bundle_attach = esp_crt_bundle_attach`) up to 3 times.
2. Only for `https://` URLs, if all verified attempts fail, retry once more with verification skipped (`crt_bundle_attach = NULL`).
3. Log every attempt via `TAG_TLS`/`TAG_HTTP` so the monitor always shows whether a stream is playing verified or unverified, and why.

This is a bounded, per-connection, logged last resort — not a global `CONFIG_ESP_TLS_INSECURE` policy applied blindly. The Kconfig options it depends on are documented in `sdkconfig.defaults`:

```
CONFIG_ESP_TLS_INSECURE=y
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
```

Without both of these, esp-tls hard-errors instead of connecting when no CA option is set at all, so the unverified fallback attempt would simply fail to open rather than degrade gracefully.

### HTTP redirect following (manual streaming API)

This project uses the manual `esp_http_client_open()` + `esp_http_client_fetch_headers()` + `esp_http_client_read()` streaming pattern (not `esp_http_client_perform()`), because streaming must hand bytes to the decoder as they arrive. ESP-IDF's built-in redirect following (`esp_http_check_response()`, `HTTP_EVENT_REDIRECT`) only fires inside `esp_http_client_perform()`, so it never triggers on this code path — a `3xx` response with a `Location` header (e.g. the `livestream-redirect` APIs some CDNs use) was previously never followed at all.

`fetch_task()` now wraps station opening in an explicit redirect loop (`current_url` buffer, `max_redirects = 5`): on a `3xx` status it reads the `Location` response header, and if present and redirects remain, re-opens a fresh client at that URL (re-running the same verified/unverified TLS retry logic per hop) instead of failing immediately. Exceeding `max_redirects` or a missing `Location` header ends the attempt with a clear "unresolved HTTP redirect" failure reason — it is never silently swallowed.

**Pitfall this depends on getting right:** `esp_http_client_get_header()` only reads headers *we* set on the outgoing request; it can never see what the server sent back. Reading any response header — `Location`, `Content-Type`, `Transfer-Encoding`, `Content-Length`, `icy-metaint`, etc. — requires `esp_http_client_get_response_header()`, and that function's backing storage only exists when `CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS=y` is set (default: disabled). Every response-header lookup in `radio_audio.c` must use `esp_http_client_get_response_header()`, and this Kconfig option (plus `CONFIG_ESP_HTTP_CLIENT_MAX_SAVED_RESPONSE_HEADERS` / `CONFIG_ESP_HTTP_CLIENT_MAX_RESPONSE_HEADER_SIZE`) must stay enabled in `sdkconfig.defaults` — otherwise redirects, Content-Type sniffing, and ICY metadata detection all silently stop working for every station, not just one.

### HTTP request and response diagnostics

Before sending the request, log the actual method, URL, and important headers such as:

- `User-Agent`;
- `Accept`;
- `Icy-Metadata`;
- `Connection`;
- optional `Range`, `Authorization`, etc.

For direct Internet radio streams, prefer:

```text
User-Agent: ESP32-Internet-Radio/1.0
Icy-Metadata: 1
```

`Icy-Metadata: 1` is important because some servers only expose ICY metadata when it is explicitly requested. If the server returns `icy-metaint: N`, the stream contains metadata blocks interleaved with audio data and those bytes must be removed before passing audio to the decoder.

The project must always log the HTTP status code and important response headers, including at least:

- `Content-Type`;
- `Content-Length`;
- `Transfer-Encoding`;
- `Connection`;
- `Location`;
- `Server`;
- `Cache-Control`;
- `Accept-Ranges`;
- `Content-Range`;
- `icy-name`;
- `icy-description`;
- `icy-genre`;
- `icy-url`;
- `icy-br`;
- `icy-metaint`;
- `ice-audio-info`.

Redirects must be logged clearly, including the redirect count and the target URL. A maximum redirect count must be enforced. Redirect loops must be detected when practical.

### Content-type and payload validation

Do not rely on a filename extension alone. Validate both HTTP headers and the initial payload bytes. Detect suspicious responses such as:

- HTML pages;
- XML or JSON pages;
- playlist URLs;
- HLS or DASH manifests;
- unsupported direct stream types.

Recognize at least M3U, M3U8, PLS, XSPF, and ASX patterns and report them explicitly if the current player cannot handle them.

The first payload bytes must be inspected carefully without destroying the original stream data. The pipeline must remain conceptually:

```text
network -> inspect/peek first bytes -> decoder receives those same bytes
```

### ICY, chunked transfer, and stream format diagnostics

The project must log whether the stream uses ICY metadata, if `icy-metaint` is supplied, and whether the server returns legacy `ICY 200 OK` responses. If the HTTP layer cannot handle legacy ICY framing, that limitation must be reported clearly.

The project must detect whether `Transfer-Encoding: chunked` is used and report invalid chunk framing explicitly when appropriate.

The project must examine the first bytes of the payload to detect likely stream types such as:

- MP3 (`FF FB`, `FF F3`, `FF F2`)
- AAC ADTS (`FF F1`, `FF F9`)
- Ogg (`OggS`)
- FLAC (`fLaC`)
- M3U (`#EXTM3U`)
- PLS (`[playlist]`)
- HTML (`<!DOCTYPE`, `<html`)

### Decoder and stream lifecycle diagnostics

Before selecting a decoder, log the decision inputs and the chosen decoder, including any mismatch or conflict between URL extension, `Content-Type`, and payload hint.

On decoder startup, log initialization success or failure and any relevant decoder-specific errors. When available, log sample rate, channels, bit depth, and bitrate.

When a read fails or a stream terminates, log the actual reason and distinguish between:

- timeout;
- clean EOF;
- remote disconnect;
- connection reset;
- TLS close;
- local cancellation;
- decoder stop.

At stream teardown, print a compact final summary including bytes received, uptime, last HTTP status, and last error.

### Memory and stack diagnostics

During station attempts, log free heap and minimum free heap usage before opening the stream, after HTTP/TLS setup, after decoder creation, after playback starts, and after teardown where practical.

If the decoder or streaming task may get close to stack exhaustion, log stack high-water marks only at meaningful transition points rather than continuously.

### Browser comparison checklist

The debug output must help compare the ESP32 behavior against a normal browser, especially for:

1. redirect handling;
2. HTTPS redirects;
3. modern TLS support;
4. User-Agent behavior;
5. Accept headers;
6. compressed HTTP content;
7. JavaScript-based web players;
8. playlist resolution;
9. cookies;
10. website/player URLs versus direct streams;
11. HLS;
12. DASH;
13. tolerance for malformed HTTP;
14. automatic retry;
15. IPv6 versus IPv4 fallback.

Do not assume that a URL that works in a browser is necessarily a direct audio stream. If the station returns HTML instead of audio, log that clearly.

### Validation constraints

- Do not hide errors.
- Do not silently guess.
- Do not add workarounds before the actual failure mechanism is visible.
- Do not rewrite the working player solely for debugging.
- Keep the network fetch and decoder/I2S path separated.
- Preserve current station functionality while adding observability.
- Use `idf.py build` after each meaningful debugging change and verify that known working stations still work.

## To Do List

### Implemented / covered

- [x] Add clear station-open banner and final stream summary.
- [x] Log parsed URL details (scheme, host, port, path).
- [x] Log DNS resolution and IPv4/IPv6 results.
- [x] Log HTTP status, headers, and content-type detection.
- [x] Add redirect detection and redirect target logging.
- [x] Follow HTTP redirects in the manual streaming read path (`fetch_task()` redirect loop, `max_redirects = 5`), since `esp_http_client_perform()`'s built-in redirect handling never runs on this code path.
- [x] Fix response-header reads (`Location`, `Content-Type`, `icy-metaint`, ...) to use `esp_http_client_get_response_header()` + `CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS=y`, since `esp_http_client_get_header()` only ever sees request headers.
- [x] Add a bounded, logged TLS-verification fallback (verified x3, then unverified once, HTTPS only) for CDNs whose chain terminates at a root missing from the certificate bundle.
- [x] Add retry logging without hiding original failure reasons.
- [x] Add User-Agent and ICY metadata request logging.
- [x] Detect HTML, playlist, and HLS/DASH-like payloads.
- [x] Inspect first payload bytes for audio vs non-audio patterns.
- [x] Log the fetch task and HTTP event flow for connection diagnostics.
- [x] Add a stall-reconnect watchdog: after ~5 s of continuous ring-buffer underruns, force-close and reopen the HTTP connection instead of waiting indefinitely on a stalled read (`STALL_UNDERRUN_THRESHOLD`, `s_reconnect_requested` in `radio_audio.c`; see requirement 15).
- [x] Add `radio_audio_set_stall_callback()` so an unexpected stream end shows a centered `"Stream stalled"` placeholder on line 1 on both the TFT and the web GUI instead of leaving stale text (see requirement 15).
- [x] Downgrade the high-frequency periodic diagnostics (`AUDIO_BUFFER: fill=...`, `HTTP_READ: bytes=...`) from `ESP_LOGI` to `ESP_LOGD` to cut monitor noise during normal playback; underrun/stall `ESP_LOGW`/`ESP_LOGE` lines are unchanged.
- [x] Root-caused a 320 kbps HTTPS station (BluesMusicFan Radio) stalling for multiple seconds to a single blocked `esp_http_client_read()` call, not a buffering bug; codified the 128 kbps ceiling as project policy (requirement 13).
- [x] Split ICY `StreamTitle` metadata into up to 3 lines instead of a fixed `Artist - Track` pair, recognizing more separator tokens and word-wrapping oversized segments (`build_icy_lines()`/`split_icy_segments()`/`emit_wrapped()` in `main/app_main.c`; see requirement 12).
- [x] Fixed the 3-line split allocating lines strictly left-to-right instead of pre-reserving a line for every remaining segment, so the leftmost/most important segment is never truncated to make room for a later one; only trailing content is dropped once the 3-line budget runs out.

### Remaining / high-priority follow-up work

- [ ] Enforce or at least warn when a `stations.json` entry's `icy-br` exceeds 128 kbps (requirement 13 is currently policy/documentation only, not validated in code).
- [ ] Add deeper TLS diagnostics with precise certificate and mbedTLS error reporting.
- [ ] Add explicit comparison of HTTP-to-HTTPS and HTTPS-to-HTTP redirect behavior.
- [ ] Add `Accept`/compression/cookie comparison logging where relevant.
- [ ] Add a more complete teardown summary with heap, uptime, and last error history.
- [ ] Add explicit malformed-HTTP / chunk-framing diagnostics for edge-case streams.
- [ ] Add browser-vs-ESP32 comparison notes for HLS, DASH, and playlist resolution.
- [ ] Add more complete validation for malformed station JSON entries and invalid URLs.
- [ ] Verify at least one known working station and one known failing station against the completed diagnostic output.

### Acceptance bar

The implementation is complete only when the developer can select any station, read the ESP-IDF monitor log, and determine whether the failure is caused by a wrong URL, a website/player URL, redirect behavior, TLS, browser-specific headers, playlists, HLS/DASH, malformed HTTP, ICY incompatibility, or decoder issues without guessing.
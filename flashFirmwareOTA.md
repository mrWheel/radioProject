# Flashing firmware over the air (OTA)

This project can receive new firmware over the local Wi-Fi network instead of
only over USB, using the `mrwheel/ota_upload` component (vendored under
`managed_components/mrwheel__ota_upload`). This document describes how OTA is
wired into this firmware and how to push a build to the device from your
computer.

**As with USB flashing, no OTA upload is ever performed automatically by an
assistant/tool in this repository. Every upload below is a manual, user-run
command.**

## Prerequisites

- The device must already be provisioned and connected to your Wi-Fi network
  (see the captive-portal setup in [README.md](README.md)).
- Your computer must be on the same local network/subnet as the device —
  the OTA service is only advertised and reachable over the LAN, never the
  internet.
- This project's active partition table is already OTA-capable:
  `partitions/radio_8mb.csv` (`otadata` + `ota_0` + `ota_1`, 3 MB each),
  selected via `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in
  [sdkconfig.defaults](sdkconfig.defaults), together with
  `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`. This matches the board's ESP32-S3
  **N8R8** (8 MB flash). No menuconfig changes are needed for a normal build.

## How OTA is started on the device

`main/app_main.c` starts the OTA receiver only once the Wi-Fi station has
actually obtained an IP address (`wifi_prov_wait_for_connection()` returning
`ESP_OK`, i.e. after `IP_EVENT_STA_GOT_IP`), because the component advertises
itself over mDNS and needs the network already up:

```c
ota_upload_config_t ota_cfg = OTA_UPLOAD_CONFIG_DEFAULT();
ESP_ERROR_CHECK(ota_upload_start(&ota_cfg));
```

`OTA_UPLOAD_CONFIG_DEFAULT()` uses the Kconfig defaults from the component's
`Kconfig` (**Component config → OTA Upload** in `idf.py menuconfig`):

| Setting | Default | Meaning |
|---|---|---|
| `OTA_UPLOAD_PORT` | `3232` | TCP port the device listens on for a firmware upload |
| `OTA_UPLOAD_ENABLE_MDNS` | `y` | Advertise the device over mDNS |
| `OTA_UPLOAD_REBOOT_AFTER_UPDATE` | `y` | Reboot automatically once the new image is validated |

The mDNS hostname used is `radioproject` (i.e. the device answers to
`radioproject.local`), matching the hostname this project already advertises
for the web GUI.

## Building a new firmware image

Build as usual; do **not** flash yet:

```sh
idf.py build
```

This produces `build/radioProject.bin`, the file the OTA upload sends to the
device.

## Uploading the firmware over the network

### Discover the device (optional)

Confirm the device is reachable and advertising the OTA service:

```sh
ping radioproject.local
dns-sd -B _esp-ota._tcp local     # macOS; shows OTA-capable devices on the LAN
```

### Option 1 — `idf.py ota` extension (if installed)

The upstream `mrwheel/ota_upload` project ships a companion Python package
(`host_tools/`) that adds an `idf.py ota` command. It is **not** vendored into
this repository's `managed_components/mrwheel__ota_upload` copy (only the
firmware-side component is). If you have obtained `host_tools/` separately
(e.g. from the component's GitHub repository) and installed it with:

```sh
python -m pip install -e ./host_tools
```

then a build can be uploaded with:

```sh
idf.py ota --host radioproject.local
```

### Option 2 — raw wire protocol (always available)

Without the host tooling, any client that speaks the component's simple wire
protocol (documented in `managed_components/mrwheel__ota_upload/docs/protocol.md`)
can perform the upload. The protocol is:

1. Open a TCP connection to the device on port `3232` (or your configured
   `OTA_UPLOAD_PORT`).
2. Send a 12-byte header: the ASCII magic `OTAMGR01` followed by the firmware
   size as an unsigned 32-bit **big-endian** integer.
3. Send the firmware bytes (`build/radioProject.bin`) immediately after.
4. Read one newline-terminated ASCII status line back, e.g. `OK rebooting`.

A minimal Python script for this (save as e.g. `tools/ota_upload.py` and run
manually — it is never invoked automatically):

```python
#!/usr/bin/env python3
import socket
import struct
import sys

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <host> <firmware.bin>")
        sys.exit(1)

    host, path = sys.argv[1], sys.argv[2]
    port = 3232

    with open(path, "rb") as f:
        firmware = f.read()

    header = b"OTAMGR01" + struct.pack(">I", len(firmware))

    with socket.create_connection((host, port), timeout=10) as sock:
        sock.sendall(header)
        sock.sendall(firmware)
        sock.settimeout(30)
        response = sock.makefile("rb").readline().decode().strip()
        print(response)
        if not response.startswith("OK"):
            sys.exit(1)

if __name__ == "__main__":
    main()
```

Usage:

```sh
python3 tools/ota_upload.py radioproject.local build/radioProject.bin
```

## Result

On a successful upload the device validates and marks the new OTA slot
bootable, prints `OK rebooting` back to the uploader, and (with
`OTA_UPLOAD_REBOOT_AFTER_UPDATE=y`, the default) reboots into the new
firmware automatically.

## Security notes

Per the component's own `docs/security.md`, the v0.1.0 wire protocol has **no
authentication or encryption**. Only use OTA upload on a trusted local
network; never port-forward the OTA port to the internet.

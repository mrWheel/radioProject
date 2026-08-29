# ftp_server

`ftp_server` exposes an already-mounted ESP-IDF VFS filesystem over standard
passive FTP. It is designed to work above LittleFS, FATFS and SD-card FATFS
without owning Wi-Fi, filesystem mounting or global network configuration.

## Status

Version `0.1.0` is a development-oriented release. FTP is unencrypted and must
only be used on a trusted local network. Authentication, FTPS and SFTP are out
of scope.

## Component use

```c
#include "ftp_server.h"

ftp_server_config_t config = FTP_SERVER_DEFAULT_CONFIG();
config.base_path = "/storage";
ESP_ERROR_CHECK(ftp_server_start(&config));
```

The application must initialize networking and mount the VFS path before
starting the server. The component exposes only paths below `base_path`.

## Supported protocol

The component supports passive `PASV` and `EPSV` transfers, directory listings,
file upload/download, restart downloads, directory and file management,
metadata queries, UTF-8 negotiation and the standard FTP session commands used
by common desktop and mobile clients. `USER` and `PASS` are compatibility
no-ops; no credentials are stored or required.

Active-mode `PORT` and TLS/FTPS are intentionally unsupported. Clients must use
passive FTP.

## Configuration

`FTP_SERVER_DEFAULT_CONFIG()` provides these defaults:

- control port: `21`;
- passive ports: `50000` through `50100`;
- transfer buffer: `4096` bytes;
- control timeout: five minutes;
- data timeout: two minutes;
- maximum clients: two in the component default, configurable by the example
  through menuconfig.

All configuration required after `ftp_server_start()` is copied by the
component.

## Testing

Host protocol tests:

```bash
make -C tests/host test
```

Hardware integration tests require a running device at `ftp-server.local` by
default:

```bash
python3 -m unittest discover -s tests/integration -p 'test_*.py'
```

The repository also contains `tests/ftp_stress.py` for repeatable concurrent
FTP workflows. See the repository README and `projectPrompt.md` for complete
curl and hardware validation instructions.

## License

GPL-3.0-or-later. See the repository [LICENSE](../../LICENSE).

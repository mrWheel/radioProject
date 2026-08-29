# Changelog

## 0.1.0

- Initial reusable ESP-IDF FTP server component.
- Added passive PASV/EPSV transfers over an already-mounted VFS filesystem.
- Added path normalization, root confinement, directory management and file
  transfers.
- Added host protocol tests and hardware integration/stress test tooling.
- Added lifecycle cleanup and an opt-in restart test hook for the example.

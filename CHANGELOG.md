# Changelog

## 0.1.1

- Keep the tray overview's widgets alive during background refreshes to prevent the open popup from becoming blank.
- Add a regression check that leaves the popup open across scheduled polling and repeated manual refreshes.
- Derive the executable version from CMake's project version.

## 0.1.0

- Initial Qt tray monitor, persistent health baselines, desktop alerts, systemd collector, and Debian packaging.

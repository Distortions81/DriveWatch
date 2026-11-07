# Changelog

## 0.1.3

- Increase the overview window height and reserve more space for drive details, with a font-scaled minimum height and a non-collapsing divider.

## 0.1.2

- Allow 90 seconds per SMART query and require 30 minutes of consecutive failed reads before raising a read-error alert. Successful reads reset confirmation; actual health faults still alert immediately.

## 0.1.1

- Keep the tray overview's widgets alive during background refreshes to prevent the open popup from becoming blank.
- Add a regression check that leaves the popup open across scheduled polling and repeated manual refreshes.
- Derive the executable version from CMake's project version.

## 0.1.0

- Initial Qt tray monitor, persistent health baselines, desktop alerts, systemd collector, and Debian packaging.

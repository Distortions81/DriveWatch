# Changelog

## 0.1.11

- Show interface type and negotiated connection speed in Summary.
- Read USB, NVMe/PCIe and SATA link speeds without changing device settings; leave unknown speeds unavailable.

## 0.1.10

- Show relative reading ages, with the exact collection time in a tooltip.

## 0.1.9

- Separate filesystem labels and drive models into distinct columns.
- Add alternating row backgrounds and padding to make individual drives easier to scan.

## 0.1.8

- Simplify the overview to four columns with drive labels first.
- Replace the raw details dump with a structured Summary and Technical details, including volume/counter tables and expandable diagnostics.
- Default unsupported or unreadable USB drives to monitoring off, preserving explicit choices.
- Keep the “SMART unavailable” status wording.

## 0.1.7

- Label inaccessible USB SMART monitoring as “SMART not supported via USB,” while explaining that the disk itself may support SMART.

## 0.1.6

- Fix the SMART JSON-output option so queries are accepted by smartctl.
- Verify command-line compatibility against the installed smartctl binary without accessing real drives.

## 0.1.5

- Add saved monitoring checkboxes for individual drives, retaining disabled drives in the overview without alerts.
- Recover names and serials from system metadata when SMART fails; display filesystem labels and mount locations.
- Retain original SMART diagnostic errors, including USB identity-command failures.
- Sort external USB drives below internal drives and distinguish unsupported SMART from unavailable access.

## 0.1.4

- Rename the app to M45 Drive Watcher, with an M45 Software desktop/D-Bus identity and m45-drive-watcher executable, package, and collector units.
- Preserve legacy DriveWatch user data and login preferences on first launch.

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

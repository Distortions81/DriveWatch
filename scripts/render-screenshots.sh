#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
binary=${1:-build/m45-drive-watcher}
mkdir -p docs/screenshots
export QT_QPA_PLATFORM=offscreen
# Screenshot mode uses a consistent dark palette regardless of the desktop theme.
"$binary" --demo --screenshot docs/screenshots/drives.png
"$binary" --demo --demo-warning --screenshot docs/screenshots/warning.png
"$binary" --demo --demo-warning --page history --screenshot docs/screenshots/history.png
"$binary" --demo --page settings --screenshot docs/screenshots/settings.png
"$binary" --demo --page tray --screenshot docs/screenshots/tray.png
"$binary" --demo --demo-warning --page tray --screenshot docs/screenshots/tray-warning.png

#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
if [ ! -x "$PIO" ]; then PIO=pio; fi
# ESP-IDF rejects spaces in source paths. This is a disposable build copy,
# not another Git checkout; edits stay in the original repository.
BUILD="${PICPAK_BUILD_DIR:-/tmp/picpak-firmware-build}"
case "$BUILD" in *' '*) echo "PICPAK_BUILD_DIR must not contain spaces" >&2; exit 1;; esac
mkdir -p "$BUILD"
rsync -a --exclude=.pio --exclude=build --exclude=sdkconfig.picpak firmware/ "$BUILD/"
"$PIO" run -d "$BUILD" -e picpak
python3 - "$BUILD/.pio/build/picpak/firmware.bin" <<'PY'
from pathlib import Path
import sys
image = Path(sys.argv[1])
assert image.stat().st_size <= 0x200000, 'Firmware exceeds the existing 2 MiB app partition'
print(f'Built {image} ({image.stat().st_size:,} bytes); existing partition layout retained.')
PY

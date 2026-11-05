#!/usr/bin/env bash
# Build the firmware for one board into firmware/build-<board>. Usage: tools/build_fw.sh <board> [idf.py args...]
# IDF env: uses idf.py from PATH if the env is already exported, else sources /opt/esp/idf. The component manager needs a writable
# IDF_TOOLS_PATH; on the read-only /opt/esp install a shadow dir of symlinks is used (docs/development-setup.md).
set -euo pipefail
board="${1:?usage: tools/build_fw.sh <board> [idf.py args]}"; shift
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v idf.py >/dev/null 2>&1; then
  if [ ! -w /opt/esp/tools ] && [ -d /opt/esp/tools ]; then
    mkdir -p "$HOME/.idf-shadow"
    for e in /opt/esp/tools/*; do ln -sfn "$e" "$HOME/.idf-shadow/$(basename "$e")"; done
    export IDF_TOOLS_PATH="$HOME/.idf-shadow"
  fi
  # shellcheck disable=SC1091
  . /opt/esp/idf/export.sh >/dev/null
fi
cd "$root/firmware"
idf.py -B "build-$board" -DIDF_TARGET=esp32s3 -DRU_BOARD="$board" "${@:-build}"

#!/usr/bin/env bash
# Build a bring-up probe into firmware/bringup/<name>/build. Usage: tools/build_probe.sh guition-probe [idf.py args...]
# Same IDF environment handling as tools/build_fw.sh.
set -euo pipefail
name="${1:?usage: tools/build_probe.sh <probe dir name> [idf.py args]}"; shift
case "$name" in ""|*[!a-z0-9-]*) echo "build_probe.sh: invalid probe name '$name' (allowed: a-z 0-9 -)" >&2; exit 2;; esac
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$root/firmware/bringup/$name/CMakeLists.txt" ] || { echo "build_probe.sh: no probe firmware/bringup/$name" >&2; exit 2; }
if ! command -v idf.py >/dev/null 2>&1; then
  if [ ! -w /opt/esp/tools ] && [ -d /opt/esp/tools ]; then
    mkdir -p "$HOME/.idf-shadow"
    for e in /opt/esp/tools/*; do ln -sfn "$e" "$HOME/.idf-shadow/$(basename "$e")"; done
    export IDF_TOOLS_PATH="$HOME/.idf-shadow"
  fi
  # shellcheck disable=SC1091
  . /opt/esp/idf/export.sh >/dev/null
fi
cd "$root/firmware/bringup/$name"
idf.py -B "build-$name" -DIDF_TARGET=esp32s3 "${@:-build}"

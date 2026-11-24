#!/usr/bin/env bash
# Build one flashable image and keep ONE loose copy of it in dist/ (dist/ is git-ignored).
#   tools/make_image.sh <board>               build (tools/build_fw.sh), merge, write dist/dzpanel-<board>-latest.bin,
#                                             its .sha256, refresh dist/SHA256SUMS, remove older loose builds of <board>
#   tools/make_image.sh --probe <board>       same for the bring-up probe: dist/<board>-probe-latest.bin
#   tools/make_image.sh --clean <board>       only the cleanup (add --probe for the probe images)
#   tools/make_image.sh --dry-run <board>     list what would be deleted, build and write nothing
#   tools/make_image.sh --skip-build <board>  merge the existing build without building
# Boards: amoled-175, amoled-216, guition-4848s040. The probe exists for guition-4848s040 only (firmware/bringup/guition-probe).
# The cleanup removes only dist/dzpanel-<board>-*-merged.bin, dist/dzpanel-<board>-merged.bin,
# dist/roundos-<board>-*-merged.bin, dist/roundos-<board>-merged.bin (probe: dist/<board>-probe-*-merged.bin) and a
# leftover .sha256 next to them. Names containing "factory" and every other file (recovery / reference images) are never touched.
set -euo pipefail
usage="usage: tools/make_image.sh [--probe] [--clean | --dry-run | --skip-build] <amoled-175|amoled-216|guition-4848s040>"
probe=0; mode=build; board=""
for arg in "$@"; do
  case "$arg" in
    --probe) probe=1 ;;
    --clean) mode=clean ;;
    --dry-run) mode=dry ;;
    --skip-build) mode=merge ;;
    -h|--help) echo "$usage"; exit 0 ;;
    -*) echo "make_image.sh: unknown option '$arg'. $usage" >&2; exit 2 ;;
    *) [ -z "$board" ] || { echo "make_image.sh: one board only. $usage" >&2; exit 2; }; board="$arg" ;;
  esac
done
[ -n "$board" ] || { echo "$usage" >&2; exit 2; }
case "$board" in amoled-175|amoled-216|guition-4848s040) ;; *) echo "make_image.sh: unknown board '$board'. $usage" >&2; exit 2;; esac
if [ "$probe" = 1 ] && [ "$board" != guition-4848s040 ]; then echo "make_image.sh: no probe for '$board'" >&2; exit 2; fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dist="$root/dist"
python="${RU_PYTHON:-/opt/esp/tools/python_env/idf5.5_py3.12_env/bin/python}"

if [ "$probe" = 1 ]; then
  latest="$board-probe-latest.bin"
  patterns=("$board-probe-*-merged.bin" "$board-probe-merged.bin")
  builddir="$root/firmware/bringup/guition-probe/build-guition-probe"
else
  latest="dzpanel-$board-latest.bin"
  patterns=("dzpanel-$board-*-merged.bin" "dzpanel-$board-merged.bin" "roundos-$board-*-merged.bin" "roundos-$board-merged.bin")
  builddir="$root/firmware/build-$board"
fi

cleanup() { # $1 = 1 to only list
  local p f n=0
  [ -d "$dist" ] || return 0
  for p in "${patterns[@]}"; do
    for f in "$dist"/$p; do
      [ -f "$f" ] || continue
      case "$(basename "$f")" in *factory*|"$latest") continue ;; esac
      if [ "$1" = 1 ]; then echo "would delete: dist/$(basename "$f")"; else rm -f -- "$f" "$f.sha256"; echo "deleted: dist/$(basename "$f")"; fi
      n=$((n + 1))
    done
  done
  [ "$n" -gt 0 ] || echo "nothing to delete"
}

case "$mode" in
  dry) cleanup 1; exit 0 ;;
  clean) cleanup 0; exit 0 ;;
esac

if [ "$mode" = build ]; then
  if [ "$probe" = 1 ]; then "$root/tools/build_probe.sh" guition-probe; else "$root/tools/build_fw.sh" "$board"; fi
fi
[ -f "$builddir/flash_args" ] || { echo "make_image.sh: $builddir/flash_args missing: build first" >&2; exit 1; }
mkdir -p "$dist"
tmp="$(mktemp "$dist/.make_image.XXXXXX")"
trap 'rm -f -- "$tmp"' EXIT
# flash_args: line 1 = flash options, then "<offset> <file>" lines (paths relative to the build dir)
parts=()
while read -r off file; do
  [ -n "$off" ] || continue
  case "$off" in 0x*) [ -f "$builddir/$file" ] || { echo "make_image.sh: $file missing in $builddir" >&2; exit 1; }; parts+=("$off" "$file") ;; esac
done < <(tail -n +2 "$builddir/flash_args")
[ "${#parts[@]}" -gt 0 ] || { echo "make_image.sh: no images in flash_args" >&2; exit 1; }
(cd "$builddir" && "$python" -m esptool --chip esp32s3 merge_bin -o "$tmp" --flash_mode dio --flash_size 16MB --flash_freq 80m "${parts[@]}")
mv -f -- "$tmp" "$dist/$latest"
trap - EXIT
sum="$(sha256sum "$dist/$latest" | cut -d' ' -f1)"
echo "$sum  $latest" > "$dist/$latest.sha256"
cleanup 0
# dist/SHA256SUMS: one line per image / elf now in dist/, sorted by name
(cd "$dist" && for f in *.bin *.elf; do [ -f "$f" ] && sha256sum -- "$f"; done | sort -k2 > SHA256SUMS.new && mv -f SHA256SUMS.new SHA256SUMS)
echo "image: dist/$latest ($(stat -c %s "$dist/$latest") bytes)"
echo "sha256: $sum"

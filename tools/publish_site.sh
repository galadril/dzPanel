#!/usr/bin/env bash
# Add one firmware build to a site tree and audit the result. Used by the release workflow and runnable locally.
#   tools/publish_site.sh --site DIR --board amoled-175 --channel stable|nightly --version 0.2.0 \
#                         --merged merged.bin [--vendor DIR] [--keep 5] [--date 2026-11-01]
# DIR is the existing published tree (a checkout of the gh-pages branch), or an empty directory for the first release.
# The page files always come from THIS checkout, so a release publishes the page that was tested with the firmware.
set -euo pipefail
cd "$(dirname "$0")/.."

site= board= channel= version= merged= vendor= keep=5 date=
while [ $# -gt 0 ]; do
  case "$1" in
    --site) site=$2; shift 2 ;; --board) board=$2; shift 2 ;; --channel) channel=$2; shift 2 ;;
    --version) version=$2; shift 2 ;; --merged) merged=$2; shift 2 ;; --vendor) vendor=$2; shift 2 ;;
    --keep) keep=$2; shift 2 ;; --date) date=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
for v in site board channel version merged; do [ -n "${!v}" ] || { echo "missing --$v" >&2; exit 2; }; done
[ -f "$merged" ] || { echo "no such image: $merged" >&2; exit 2; }

# refuse a wrong file before touching the site (make_manifest checks again; this keeps a failed run from leaving debris)
[ "$(head -c1 "$merged" | od -An -tx1 | tr -d ' \n')" = "e9" ] || { echo "error: $merged does not start with the ESP image magic 0xE9; is this the merged binary?" >&2; exit 2; }

mkdir -p "$site"
# 1. the page itself (never the vendor directory: that is added explicitly below)
for f in index.html app.js lib.js style.css; do install -m 0644 "site/$f" "$site/$f"; done
# 2. the installer component, built separately by CI
if [ -n "$vendor" ]; then
  [ -f "$vendor/install-button.js" ] || { echo "--vendor has no install-button.js" >&2; exit 2; }
  rm -rf "$site/vendor/esp-web-tools"; mkdir -p "$site/vendor/esp-web-tools"
  cp -r "$vendor/." "$site/vendor/esp-web-tools/"
  find "$site/vendor" -type d -exec chmod 0755 {} + ; find "$site/vendor" -type f -exec chmod 0644 {} +
fi
# 3. the firmware, its manifest, the index (pruned to the newest --keep per board and channel)
python3 -I tools/make_manifest.py --board "$board" --channel "$channel" --version "$version" \
  --merged "$merged" --out "$site" --keep "$keep" ${date:+--date "$date"} >/dev/null
# 4. refuse to hand back a tree the page could not use
if [ -n "$vendor" ] || [ -f "$site/vendor/esp-web-tools/install-button.js" ]; then
  node tools/check_site.mjs "$site" --require-vendor
else
  node tools/check_site.mjs "$site"
fi

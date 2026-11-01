#!/usr/bin/env bash
# Build ESP Web Tools from a PINNED commit and copy it into site/vendor/esp-web-tools/.
#
# Why vendored: the installer must not load code from a CDN at runtime (privacy, supply chain, offline).
# Why this commit: it is the revision whose source was read for docs/research/flashing.md section 12.
# STATUS: NOT RUN. The sandbox this was written in has no npm registry access, so the build steps below
# follow the repo's own script/build and rollup.config.mjs but are unverified. Run on a normal machine,
# check the output list. site/vendor/ is git-ignored: CI runs this script at release time and publishes the result; it is not committed.
#
# Needs: git, node (>=18), npm, jq. Run from the repo root.
set -euo pipefail

PIN="384b35b1c34b5e279bb47fc82fe2482c6a98f899"   # esphome/esp-web-tools, 2026-10-10
REPO="https://github.com/esphome/esp-web-tools.git"
DEST="site/vendor/esp-web-tools"

[ -f tools/vendor_esp_web_tools.sh ] || { echo "run from the repo root" >&2; exit 2; }
for c in git node npm jq; do command -v "$c" >/dev/null || { echo "missing: $c" >&2; exit 2; }; done

work="$(mktemp -d)"            # a fresh empty dir: downloaded code is untrusted and never built in-tree
trap 'rm -rf "$work"' EXIT
git clone --quiet "$REPO" "$work/src"
git -C "$work/src" checkout --quiet "$PIN"
test "$(git -C "$work/src" rev-parse HEAD)" = "$PIN"

cd "$work/src"
npm ci --ignore-scripts        # lockfile install, no package install scripts
script/build                   # tsc + rollup -> dist/web

cd - >/dev/null
rm -rf "$DEST"
mkdir -p "$DEST"
cp -r "$work/src/dist/web/." "$DEST/"
cp "$work/src/LICENSE" "$DEST/LICENSE"

{
  echo "esp-web-tools $PIN"
  echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ) with node $(node --version)"
  (cd "$DEST" && find . -type f ! -name SOURCE.txt -print0 | sort -z | xargs -0 sha256sum)
} > "$DEST/SOURCE.txt"

# The page imports ./vendor/esp-web-tools/install-button.js (site/app.js).
test -f "$DEST/install-button.js" || { echo "build did not produce install-button.js; check dist/web" >&2; exit 1; }
# A self-contained build must not import code from another host.
if grep -rlE "import\(?['\"]https?://" "$DEST" --include=*.js; then
  echo "WARNING: the bundle imports from another host (listed above). Do not publish until resolved." >&2
  exit 1
fi
echo "vendored into $DEST (see SOURCE.txt for hashes)"

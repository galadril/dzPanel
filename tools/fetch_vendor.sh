#!/usr/bin/env bash
# Fetch the pinned third-party sources listed in vendor.lock into vendor/. Idempotent.
# Downloaded code is untrusted data: it is only ever read or compiled, never executed from here.
set -euo pipefail
cd "$(dirname "$0")/.."
grep -vE '^\s*(#|$)' vendor.lock | while read -r dir url commit _lic _rest; do
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then echo "ok      $dir"; continue; fi
  [ -e "$dir" ] && [ -z "$(ls -A "$dir")" ] || { [ ! -e "$dir" ] || { echo "refusing: $dir exists and is not the pinned checkout" >&2; exit 1; }; }
  mkdir -p "$dir"
  git init -q "$dir"
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch -q --depth 1 origin "$commit"      # shallow, exactly the pinned commit
  git -C "$dir" checkout -q FETCH_HEAD
  test "$(git -C "$dir" rev-parse HEAD)" = "$commit"
  echo "fetched $dir @ ${commit:0:9}"
done

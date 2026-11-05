#!/usr/bin/env python3
"""Publish one merged firmware image into the installer site tree.

    make_manifest.py --board amoled-175 --channel stable --version 1.0.0 \
                     --merged build/dzpanel-merged.bin --out site-out [--keep 5]

Writes  <out>/firmware/<board>/<channel>/<version>/{manifest.json,<image>.bin}
and updates <out>/versions.json (schema 1). Standard library only. Run with `python3 -I`.
Everything the browser fetches is same-origin and relative, so there is no CORS or redirect problem.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile

BOARDS = {"amoled-175": "ESP32-S3", "amoled-216": "ESP32-S3", "guition-4848s040": "ESP32-S3"}
CHANNELS = ("stable", "nightly")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$")
MAX_IMAGE = 16 * 1024 * 1024  # the boards assume 16 MB flash
ESP_IMAGE_MAGIC = 0xE9
PUBLIC_MODE = 0o644  # everything published is meant to be world-readable static files


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def version_key(v):
    """Sort key following semver precedence: a release outranks its prereleases."""
    core, _, pre = v.partition("-")
    nums = tuple(int(x) for x in core.split("."))
    if not pre:
        return (nums, 1, ())
    ids = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split("."))
    return (nums, 0, ids)


def check_image(path):
    size = os.path.getsize(path)
    if size == 0 or size > MAX_IMAGE:
        fail(f"image size {size} is outside 1..{MAX_IMAGE}")
    with open(path, "rb") as f:
        head = f.read(1)
    # A merged image starts with the second-stage bootloader at offset 0.
    if head[0] != ESP_IMAGE_MAGIC:
        fail("image does not start with the ESP image magic 0xE9; is this the merged binary?")
    return size


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_json_atomic(path, obj):
    d = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
    with os.fdopen(fd, "w") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write("\n")
    os.chmod(tmp, PUBLIC_MODE)  # mkstemp creates 0600, which a web server running as another user cannot read
    os.replace(tmp, path)


def publish(board, channel, version, merged, out, date, keep):
    if board not in BOARDS:
        fail(f"unknown board {board!r}; known: {', '.join(BOARDS)}")
    if channel not in CHANNELS:
        fail(f"unknown channel {channel!r}")
    if not VERSION_RE.match(version):
        fail(f"bad version {version!r} (want 1.2.3 or 1.2.3-rc.1)")
    size = check_image(merged)
    digest = sha256(merged)

    rel_dir = f"firmware/{board}/{channel}/{version}"
    dest = os.path.join(out, rel_dir)
    os.makedirs(dest, exist_ok=True)
    image = f"dzpanel-{board}-{version}-merged.bin"
    shutil.copyfile(merged, os.path.join(dest, image))
    os.chmod(os.path.join(dest, image), PUBLIC_MODE)  # not at the mercy of the runner's umask

    manifest = {
        "name": "dzPanel",
        "version": version,
        # Verified in ESP Web Tools source: without Improv a device is ERASED by default on install.
        # Ask first, so a reinstall does not silently wipe stored Wi-Fi and Domoticz settings.
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 15,
        # serialType omitted on purpose: a build without it matches any detected serial type.
        "builds": [{"chipFamily": BOARDS[board], "parts": [{"path": image, "offset": 0}]}],
    }
    write_json_atomic(os.path.join(dest, "manifest.json"), manifest)

    vpath = os.path.join(out, "versions.json")
    doc = {"schema": 1, "builds": []}
    if os.path.exists(vpath):
        with open(vpath) as f:
            doc = json.load(f)
        if doc.get("schema") != 1:
            fail("existing versions.json has an unknown schema; refusing to overwrite")
    doc["builds"] = [b for b in doc["builds"] if (b["board"], b["channel"], b["version"]) != (board, channel, version)]
    doc["builds"].append({
        "board": board, "channel": channel, "version": version,
        "manifest": f"{rel_dir}/manifest.json", "file": f"{rel_dir}/{image}",
        "sha256": digest, "size": size, "date": date,
    })

    # keep the newest N per (board, channel); delete the rest from disk and from the index
    groups = {}
    for b in doc["builds"]:
        groups.setdefault((b["board"], b["channel"]), []).append(b)
    kept = []
    for items in groups.values():
        items.sort(key=lambda b: version_key(b["version"]), reverse=True)
        kept += items[:keep]
        for old in items[keep:]:
            shutil.rmtree(os.path.join(out, os.path.dirname(old["manifest"])), ignore_errors=True)
    doc["builds"] = sorted(kept, key=lambda b: (b["board"], b["channel"], version_key(b["version"])))
    write_json_atomic(vpath, doc)
    return digest


def remove(board, channel, version, out):
    """Pull a published build (a bad release): drop it from versions.json and delete its files."""
    vpath = os.path.join(out, "versions.json")
    if not os.path.exists(vpath):
        fail("no versions.json in --out")
    with open(vpath) as f:
        doc = json.load(f)
    if doc.get("schema") != 1:
        fail("existing versions.json has an unknown schema; refusing to modify it")
    hit = [b for b in doc["builds"] if (b["board"], b["channel"], b["version"]) == (board, channel, version)]
    if not hit:
        fail(f"{board}/{channel}/{version} is not in versions.json")
    doc["builds"] = [b for b in doc["builds"] if b not in hit]
    write_json_atomic(vpath, doc)  # index first: after this the page can no longer offer it
    shutil.rmtree(os.path.join(out, os.path.dirname(hit[0]["manifest"])), ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--board", required=True)
    p.add_argument("--channel", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--merged", help="merged .bin written at flash offset 0 (required unless --remove)")
    p.add_argument("--remove", action="store_true", help="pull this build from the index and delete its files")
    p.add_argument("--out", required=True)
    p.add_argument("--keep", type=int, default=5)
    p.add_argument("--date", default="", help="ISO date for the index (default: empty, set by CI)")
    a = p.parse_args()
    if a.remove:
        remove(a.board, a.channel, a.version, a.out)
        print(f"removed {a.board}/{a.channel}/{a.version}")
        return
    if not a.merged:
        fail("--merged is required unless --remove")
    if a.keep < 1:
        fail("--keep must be at least 1")
    print(publish(a.board, a.channel, a.version, a.merged, a.out, a.date, a.keep))


if __name__ == "__main__":
    main()

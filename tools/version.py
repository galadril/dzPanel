#!/usr/bin/env python3
"""Decide the version string for a build. Standard library only. Run with `python3 -I`.

    version.py --channel stable  [--tag v0.2.0]       -> 0.2.0   (and refuses a tag that disagrees with VERSION)
    version.py --channel nightly [--date 20261101]    -> 0.2.0-nightly.20261101

VERSION (repo root) holds the version IN DEVELOPMENT. Cut a release by tagging vX.Y.Z where X.Y.Z equals VERSION, then
bump VERSION to the next planned one. Nightlies of the next version sort BELOW its release and ABOVE the previous
release (semver: a prerelease of 0.2.0 is newer than 0.1.9 and older than 0.2.0), so the installer picks correctly.
The result always satisfies tools/make_manifest.py and the firmware's CMake check (31 characters at most).
"""
import argparse
import datetime
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$")
MAX_LEN = 31  # esp_app_desc_t.version


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def read_version_file(path=None):
    path = path or os.path.join(ROOT, "VERSION")
    try:
        with open(path) as f:
            v = f.readline().strip()
    except OSError as e:
        fail(f"cannot read VERSION: {e}")
    if not VERSION_RE.match(v):
        fail(f"VERSION {v!r} is not 1.2.3 or 1.2.3-rc.1")
    return v


def compute(channel, version, tag=None, date=None):
    if channel == "stable":
        if tag is not None:
            if not tag.startswith("v") or tag[1:] != version:
                fail(f"tag {tag!r} does not match VERSION {version!r}; refusing to release a mismatch")
        out = version
    elif channel == "nightly":
        if "-" in version:
            fail(f"VERSION {version!r} already has a prerelease part; nightlies need a plain X.Y.Z")
        if date is None:
            date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
        if not re.fullmatch(r"\d{8}", date):
            fail(f"--date must be YYYYMMDD, got {date!r}")
        out = f"{version}-nightly.{date}"
    else:
        fail(f"unknown channel {channel!r}")
    if not VERSION_RE.match(out) or len(out) > MAX_LEN:
        fail(f"computed version {out!r} is invalid or longer than {MAX_LEN} characters")
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--channel", required=True, choices=["stable", "nightly"])
    p.add_argument("--tag")
    p.add_argument("--date")
    p.add_argument("--version-file")
    a = p.parse_args()
    print(compute(a.channel, read_version_file(a.version_file), a.tag, a.date))


if __name__ == "__main__":
    main()

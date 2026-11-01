"""Run: python3 -I -W ignore tests/tools/test_release_tools.py   (version.py, make_manifest --remove, tools/check_site.mjs)"""
import json, os, shutil, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PY = [sys.executable, "-I"]
VERSION = PY + [os.path.join(ROOT, "tools", "version.py")]
MANIFEST = PY + [os.path.join(ROOT, "tools", "make_manifest.py")]
CHECK = ["node", os.path.join(ROOT, "tools", "check_site.mjs")]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


class Version(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(); self.addCleanup(shutil.rmtree, self.d)

    def vf(self, text):
        p = os.path.join(self.d, "VERSION"); open(p, "w").write(text); return ["--version-file", p]

    def test_stable_matches_tag(self):
        r = run(VERSION + ["--channel", "stable", "--tag", "v0.2.0"] + self.vf("0.2.0\n"))
        self.assertEqual((r.returncode, r.stdout.strip()), (0, "0.2.0"))

    def test_stable_refuses_a_mismatched_tag(self):
        for tag in ("v0.2.1", "0.2.0", "v0.2.0-rc.1"):
            r = run(VERSION + ["--channel", "stable", "--tag", tag] + self.vf("0.2.0\n"))
            self.assertEqual(r.returncode, 2, tag)
            self.assertIn("refusing", r.stderr)

    def test_prerelease_in_version_file_is_a_valid_stable_tag(self):
        r = run(VERSION + ["--channel", "stable", "--tag", "v0.2.0-rc.1"] + self.vf("0.2.0-rc.1\n"))
        self.assertEqual(r.stdout.strip(), "0.2.0-rc.1")

    def test_nightly_sorts_between_releases(self):
        r = run(VERSION + ["--channel", "nightly", "--date", "20261101"] + self.vf("0.2.0\n"))
        self.assertEqual(r.stdout.strip(), "0.2.0-nightly.20261101")
        self.assertLessEqual(len(r.stdout.strip()), 31)

    def test_nightly_needs_plain_version_and_good_date(self):
        self.assertEqual(run(VERSION + ["--channel", "nightly", "--date", "20261101"] + self.vf("0.2.0-rc.1\n")).returncode, 2)
        self.assertEqual(run(VERSION + ["--channel", "nightly", "--date", "2026-11-01"] + self.vf("0.2.0\n")).returncode, 2)

    def test_garbage_version_file(self):
        for t in ("", "banana\n", "1.2\n", "v1.2.3\n", "1.2.3 \n garbage"):
            r = run(VERSION + ["--channel", "stable"] + self.vf(t))
            if t.startswith("1.2.3 "):  # first line is stripped: "1.2.3" is fine
                continue
            self.assertEqual(r.returncode, 2, repr(t))

    def test_real_version_file_is_valid(self):
        r = run(VERSION + ["--channel", "nightly", "--date", "20261101"])
        self.assertEqual(r.returncode, 0, r.stderr)


class SiteBase(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(); self.addCleanup(shutil.rmtree, self.d)
        self.out = os.path.join(self.d, "out"); os.makedirs(self.out)
        for f in ("index.html", "app.js", "lib.js", "style.css"):
            shutil.copy(os.path.join(ROOT, "site", f), self.out)
        self.img = os.path.join(self.d, "fw.bin")
        open(self.img, "wb").write(b"\xe9" + b"\x00" * 2047)
        self.publish("1.0.0")

    def publish(self, ver, board="amoled-175", ch="stable"):
        r = run(MANIFEST + ["--board", board, "--channel", ch, "--version", ver, "--merged", self.img, "--out", self.out, "--date", "2026-11-01"])
        self.assertEqual(r.returncode, 0, r.stderr)

    def check(self, *extra):
        return run(CHECK + [self.out, *extra])

    def base(self, ver="1.0.0"):
        return os.path.join(self.out, "firmware/amoled-175/stable", ver)


class Site(SiteBase):
    def test_good_tree_passes(self):
        r = self.check(); self.assertEqual(r.returncode, 0, r.stderr); self.assertIn("1 build", r.stdout)

    def test_flipped_byte_is_caught(self):
        img = os.path.join(self.base(), "roundos-amoled-175-1.0.0-merged.bin")
        b = bytearray(open(img, "rb").read()); b[100] ^= 1; open(img, "wb").write(b)
        r = self.check(); self.assertEqual(r.returncode, 1); self.assertIn("sha256", r.stderr)

    def test_missing_files_are_caught(self):
        os.remove(os.path.join(self.base(), "manifest.json"))
        r = self.check(); self.assertEqual(r.returncode, 1); self.assertIn("manifest missing", r.stderr)

    def test_manifest_that_lost_the_erase_prompt_is_caught(self):
        p = os.path.join(self.base(), "manifest.json"); m = json.load(open(p)); m["new_install_prompt_erase"] = False
        json.dump(m, open(p, "w")); r = self.check(); self.assertEqual(r.returncode, 1); self.assertIn("new_install_prompt_erase", r.stderr)

    def test_version_disagreement_is_caught(self):
        p = os.path.join(self.base(), "manifest.json"); m = json.load(open(p)); m["version"] = "9.9.9"
        json.dump(m, open(p, "w")); self.assertEqual(self.check().returncode, 1)

    def test_unreadable_by_others_is_caught(self):
        os.chmod(os.path.join(self.base(), "manifest.json"), 0o600)
        r = self.check(); self.assertEqual(r.returncode, 1); self.assertIn("world-readable", r.stderr)

    def test_index_cannot_point_outside_the_site(self):
        v = os.path.join(self.out, "versions.json"); doc = json.load(open(v)); doc["builds"][0]["file"] = "../../etc/passwd"
        json.dump(doc, open(v, "w")); self.assertEqual(self.check().returncode, 1)

    def test_missing_page_files_and_vendor(self):
        self.assertEqual(self.check("--require-vendor").returncode, 1)   # the install button component is not there
        os.remove(os.path.join(self.out, "app.js")); self.assertEqual(self.check().returncode, 1)

    def test_no_index_at_all(self):
        os.remove(os.path.join(self.out, "versions.json")); self.assertEqual(self.check().returncode, 1)


class Remove(SiteBase):
    def test_remove_pulls_build_and_files(self):
        self.publish("1.1.0")
        r = run(MANIFEST + ["--board", "amoled-175", "--channel", "stable", "--version", "1.1.0", "--out", self.out, "--remove"])
        self.assertEqual(r.returncode, 0, r.stderr)
        vs = [b["version"] for b in json.load(open(os.path.join(self.out, "versions.json")))["builds"]]
        self.assertEqual(vs, ["1.0.0"])
        self.assertFalse(os.path.exists(self.base("1.1.0")))
        self.assertEqual(self.check().returncode, 0)

    def test_remove_unknown_build_fails_and_changes_nothing(self):
        before = open(os.path.join(self.out, "versions.json")).read()
        r = run(MANIFEST + ["--board", "amoled-175", "--channel", "stable", "--version", "7.7.7", "--out", self.out, "--remove"])
        self.assertEqual(r.returncode, 2)
        self.assertEqual(open(os.path.join(self.out, "versions.json")).read(), before)

    def test_publish_without_merged_is_refused(self):
        r = run(MANIFEST + ["--board", "amoled-175", "--channel", "stable", "--version", "1.2.0", "--out", self.out])
        self.assertEqual(r.returncode, 2); self.assertIn("--merged", r.stderr)


if __name__ == "__main__":
    unittest.main()

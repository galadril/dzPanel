"""Run: python3 -I tests/tools/test_make_manifest.py"""
import importlib.util, json, os, subprocess, sys, tempfile, unittest, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "make_manifest.py")
spec = importlib.util.spec_from_file_location("make_manifest", TOOL)
mm = importlib.util.module_from_spec(spec); spec.loader.exec_module(mm)


def stub(d, name="fw.bin", magic=0xE9, size=4096):
    p = os.path.join(d, name)
    with open(p, "wb") as f:
        f.write(bytes([magic]) + b"\x00" * (size - 1))
    return p


def run(*args):
    return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True)


class T(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.d = self.tmp.name
        self.out = os.path.join(self.d, "out"); os.makedirs(self.out)

    def tearDown(self):
        self.tmp.cleanup()

    def pub(self, ver, board="amoled-175", ch="stable", keep=5, img=None):
        return mm.publish(board, ch, ver, img or stub(self.d), self.out, "2026-10-31", keep)

    def test_manifest_matches_esp_web_tools_shape(self):
        digest = self.pub("1.0.0")
        m = json.load(open(os.path.join(self.out, "firmware/amoled-175/stable/1.0.0/manifest.json")))
        self.assertEqual(m["builds"][0]["chipFamily"], "ESP32-S3")
        self.assertEqual(m["builds"][0]["parts"][0]["offset"], 0)
        self.assertTrue(m["new_install_prompt_erase"])  # never silently wipe NVS
        self.assertNotIn("serialType", m["builds"][0])
        self.assertFalse(os.path.isabs(m["builds"][0]["parts"][0]["path"]))  # relative to the manifest
        v = json.load(open(os.path.join(self.out, "versions.json")))
        self.assertEqual(v["builds"][0]["sha256"], digest)
        img = os.path.join(self.out, v["builds"][0]["file"])
        self.assertEqual(hashlib.sha256(open(img, "rb").read()).hexdigest(), digest)

    def test_republish_replaces_not_duplicates(self):
        self.pub("1.0.0"); self.pub("1.0.0")
        self.assertEqual(len(json.load(open(os.path.join(self.out, "versions.json")))["builds"]), 1)

    def test_prune_keeps_newest_by_semver_and_deletes_files(self):
        for v in ["1.9.0", "1.10.0", "1.2.0", "2.0.0-rc.1", "2.0.0"]:
            self.pub(v, keep=2)
        vs = [b["version"] for b in json.load(open(os.path.join(self.out, "versions.json")))["builds"]]
        self.assertEqual(vs, ["2.0.0", "2.0.0-rc.1"][::-1])  # sorted ascending: rc before release
        self.assertFalse(os.path.exists(os.path.join(self.out, "firmware/amoled-175/stable/1.10.0")))
        self.assertTrue(os.path.exists(os.path.join(self.out, "firmware/amoled-175/stable/2.0.0")))

    def test_guition_board_and_product_name(self):
        self.pub("1.0.0", board="guition-4848s040")
        base = os.path.join(self.out, "firmware/guition-4848s040/stable/1.0.0")
        m = json.load(open(os.path.join(base, "manifest.json")))
        self.assertEqual(m["name"], "dzPanel")  # must equal the Improv fw_name in firmware/main/svc_glue.c
        self.assertEqual(m["builds"][0]["chipFamily"], "ESP32-S3")
        self.assertTrue(os.path.exists(os.path.join(base, "dzpanel-guition-4848s040-1.0.0-merged.bin")))

    def test_prune_is_per_board_and_channel(self):
        self.pub("1.0.0"); self.pub("1.0.0", board="amoled-216"); self.pub("1.0.0", ch="nightly")
        self.pub("1.1.0", keep=1)
        v = json.load(open(os.path.join(self.out, "versions.json")))["builds"]
        self.assertEqual(len(v), 3)  # other board and channel untouched

    def test_rejects_bad_input(self):
        good = stub(self.d)
        for args in [("amoled-999", "stable", "1.0.0"), ("amoled-175", "beta", "1.0.0"), ("amoled-175", "stable", "1.0"), ("amoled-175", "stable", "../1.0.0")]:
            r = run("--board", args[0], "--channel", args[1], "--version", args[2], "--merged", good, "--out", self.out)
            self.assertEqual(r.returncode, 2, args)
        r = run("--board", "amoled-175", "--channel", "stable", "--version", "1.0.0", "--merged", stub(self.d, "x.bin", magic=0x00), "--out", self.out)
        self.assertEqual(r.returncode, 2); self.assertIn("0xE9", r.stderr)
        empty = os.path.join(self.d, "empty.bin"); open(empty, "wb").close()
        r = run("--board", "amoled-175", "--channel", "stable", "--version", "1.0.0", "--merged", empty, "--out", self.out)
        self.assertEqual(r.returncode, 2); self.assertIn("size", r.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.out, "versions.json")))  # failures publish nothing

    def test_published_files_are_world_readable_even_under_a_strict_umask(self):
        old = os.umask(0o077)  # what a locked-down CI runner may use
        try:
            self.pub("1.0.0")
        finally:
            os.umask(old)
        base = os.path.join(self.out, "firmware/amoled-175/stable/1.0.0")
        for f in [os.path.join(base, "manifest.json"), os.path.join(base, "dzpanel-amoled-175-1.0.0-merged.bin"),
                  os.path.join(self.out, "versions.json")]:
            self.assertEqual(os.stat(f).st_mode & 0o777, 0o644, f)

    def test_refuses_foreign_schema(self):
        json.dump({"schema": 7, "builds": []}, open(os.path.join(self.out, "versions.json"), "w"))
        r = run("--board", "amoled-175", "--channel", "stable", "--version", "1.0.0", "--merged", stub(self.d), "--out", self.out)
        self.assertEqual(r.returncode, 2)
        self.assertEqual(json.load(open(os.path.join(self.out, "versions.json")))["schema"], 7)


if __name__ == "__main__":
    unittest.main()

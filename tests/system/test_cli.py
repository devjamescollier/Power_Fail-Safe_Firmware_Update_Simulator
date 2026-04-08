import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkimage

FWSIM = os.environ.get("FWSIM", os.path.join(ROOT, "build", "fwsim"))
SLOT_ADDR = {"A": 0x02000, "B": 0x12000}

BOOT_RE = re.compile(r"^boot: slot ([AB]) version 0x([0-9a-f]{8}) \((.*)\)$")
META_RE = re.compile(r"^metadata:  seq=(\d+) sector=META([01]) state=(\w+) last_event=([\w-]+)$",
                     re.M)


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.flash = os.path.join(self.tmp.name, "flash.img")

    def tearDown(self):
        self.tmp.cleanup()

    def image(self, name, version, size, seed, mutate=None):
        data = bytearray(mkimage.build_image(version, size, seed))
        if mutate:
            data = mutate(data)
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def fwsim(self, *args, expect=0):
        r = subprocess.run([FWSIM, "--flash", self.flash, *args],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, expect,
                         f"fwsim {' '.join(args)}\nstdout: {r.stdout}\nstderr: {r.stderr}")
        return r

    def boot(self):
        out = self.fwsim("boot").stdout.strip()
        m = BOOT_RE.match(out)
        self.assertIsNotNone(m, out)
        return m.group(1), int(m.group(2), 16), m.group(3)

    def meta(self):
        out = self.fwsim("status").stdout
        m = META_RE.search(out)
        self.assertIsNotNone(m, out)
        return {"seq": int(m.group(1)), "sector": int(m.group(2)),
                "state": m.group(3), "event": m.group(4), "text": out}

    def init(self):
        self.fwsim("init", self.image("v1.img", 1, 3000, seed=1))

    def corrupt_flash_byte(self, addr):
        with open(self.flash, "r+b") as f:
            f.seek(addr)
            b = f.read(1)[0]
            self.assertNotEqual(b, 0, "pick a byte with bits to clear")
            f.seek(addr)
            f.write(bytes([b & (b - 1)]))

    def test_successful_update(self):
        self.init()
        self.assertEqual(self.boot(), ("A", 1, "confirmed"))
        r = self.fwsim("install", self.image("v2.img", 2, 6000, seed=2))
        self.assertIn("staged in slot B, PENDING", r.stdout)
        self.assertEqual(self.meta()["state"], "PENDING")

        slot, ver, note = self.boot()
        self.assertEqual((slot, ver), ("B", 2))
        self.assertIn("TRIAL", note)
        self.assertEqual(self.meta()["state"], "TRIAL")

        self.assertIn("slot B version 0x00000002 is now confirmed", self.fwsim("confirm").stdout)
        m = self.meta()
        self.assertEqual((m["state"], m["event"]), ("CONFIRMED", "updated"))
        self.assertIn("confirmed: slot B version 0x00000002", m["text"])
        self.assertEqual(self.boot(), ("B", 2, "confirmed"))

    def test_unconfirmed_trial_rolls_back(self):
        self.init()
        self.fwsim("install", self.image("v2.img", 2, 6000, seed=2))
        self.assertEqual(self.boot()[:2], ("B", 2))
        slot, ver, note = self.boot()
        self.assertEqual((slot, ver), ("A", 1))
        self.assertIn("rolled back", note)
        m = self.meta()
        self.assertEqual((m["state"], m["event"]), ("CONFIRMED", "rolled-back"))
        self.assertIn("candidate: none", m["text"])
        r = self.fwsim("confirm", expect=1)
        self.assertIn("not allowed in current state", r.stderr)
        for _ in range(3):
            self.assertEqual(self.boot(), ("A", 1, "confirmed"))

    def test_confirmed_trial_persists(self):
        self.init()
        self.fwsim("install", self.image("v2.img", 2, 6000, seed=2))
        self.boot()
        self.fwsim("confirm")
        for _ in range(3):
            self.assertEqual(self.boot(), ("B", 2, "confirmed"))
        seq = self.meta()["seq"]
        self.boot()
        self.assertEqual(self.meta()["seq"], seq, "confirmed boots must not write metadata")

        r = self.fwsim("install", self.image("v3.img", 3, 1500, seed=3))
        self.assertIn("staged in slot A", r.stdout)
        self.assertEqual(self.boot()[:2], ("A", 3))
        self.assertEqual(self.boot()[:2], ("B", 2))

    def test_corrupt_candidate_rejected_at_install(self):
        self.init()
        before = self.meta()

        def flip_payload(d):
            d[24 + 1000] ^= 0x10
            return d
        r = self.fwsim("install", self.image("bad.img", 2, 6000, seed=2, mutate=flip_payload),
                       expect=1)
        self.assertIn("payload CRC mismatch", r.stderr)
        after = self.meta()
        self.assertEqual((after["state"], after["seq"]), ("CONFIRMED", before["seq"]))
        self.assertIn("candidate: none", after["text"])
        self.assertEqual(self.boot(), ("A", 1, "confirmed"))

    def test_malformed_images_rejected_at_install(self):
        self.init()
        good = mkimage.build_image(2, 6000, 2)
        cases = {
            "truncated": (good[:-1], "image: truncated"),
            "header only partial": (good[:10], "image: truncated"),
            "empty": (b"", "image: truncated"),
            "trailing byte": (good + b"\x00", "image: too large"),
            "bad magic": (b"X" + good[1:], "image: bad magic"),
            "header bit flip": (good[:9] + bytes([good[9] ^ 1]) + good[10:],
                                "image: header CRC mismatch"),
        }
        for name, (data, msg) in cases.items():
            with self.subTest(name):
                path = os.path.join(self.tmp.name, "case.img")
                with open(path, "wb") as f:
                    f.write(data)
                r = self.fwsim("install", path, expect=1)
                self.assertIn(msg, r.stderr)
                self.assertEqual(self.meta()["state"], "CONFIRMED")
                self.assertEqual(self.boot(), ("A", 1, "confirmed"))

    def test_staged_candidate_corrupted_before_boot_is_rejected(self):
        self.init()
        self.fwsim("install", self.image("v2.img", 2, 6000, seed=2))
        payload = mkimage.make_payload(6000, 2)
        off = next(i for i, b in enumerate(payload) if b != 0)
        self.corrupt_flash_byte(SLOT_ADDR["B"] + 24 + off)
        slot, ver, note = self.boot()
        self.assertEqual((slot, ver), ("A", 1))
        self.assertIn("failed verification", note)
        m = self.meta()
        self.assertEqual((m["state"], m["event"]), ("CONFIRMED", "rejected"))

    def test_install_refused_during_trial(self):
        self.init()
        self.fwsim("install", self.image("v2.img", 2, 6000, seed=2))
        self.boot()
        r = self.fwsim("install", self.image("v3.img", 3, 100, seed=3), expect=1)
        self.assertIn("not allowed in current state", r.stderr)
        self.assertEqual(self.meta()["state"], "TRIAL")

    def test_metadata_sectors_alternate(self):
        self.init()
        sectors = [self.meta()["sector"]]
        self.fwsim("install", self.image("v2.img", 2, 100, seed=2))
        sectors.append(self.meta()["sector"])
        self.boot()
        sectors.append(self.meta()["sector"])
        self.fwsim("confirm")
        sectors.append(self.meta()["sector"])
        self.assertEqual(sectors, [0, 1, 0, 1])

    def test_usage_and_missing_flash(self):
        r = self.fwsim("boot", expect=1)
        self.assertIn("hint: create it with", r.stderr)
        self.fwsim("install", expect=2)
        self.fwsim("boot", "extra", expect=2)
        self.assertIn("unknown command", self.fwsim("frobnicate", expect=2).stderr)
        self.assertIn("non-negative integer",
                      self.fwsim("--fail-at", "-1", "boot", expect=2).stderr)
        self.fwsim("init", os.path.join(self.tmp.name, "nope.img"), expect=1)
        self.assertFalse(os.path.exists(self.flash))

    def test_wrong_size_flash_file(self):
        with open(self.flash, "wb") as f:
            f.write(b"\xff" * 100)
        self.assertIn("is not a 139264-byte flash file", self.fwsim("status", expect=1).stderr)

    def test_init_with_bad_image_path_keeps_device(self):
        self.init()
        self.fwsim("install", self.image("v2.img", 2, 6000, seed=2))
        with open(self.flash, "rb") as f:
            before = f.read()
        r = self.fwsim("init", os.path.join(self.tmp.name, "typo.img"), expect=1)
        self.assertIn("cannot open image file", r.stderr)
        with open(self.flash, "rb") as f:
            self.assertEqual(f.read(), before, "a bad init path must not touch flash")

    def test_state_errors_give_next_step(self):
        self.init()
        r = self.fwsim("confirm", expect=1)
        self.assertIn("confirm: failed: not allowed in current state", r.stderr)
        self.assertIn("hint: no trial image is running", r.stderr)
        self.fwsim("install", self.image("v2.img", 2, 6000, seed=2))
        self.assertIn("hint: the candidate has not been booted yet",
                      self.fwsim("confirm", expect=1).stderr)
        self.boot()
        r = self.fwsim("install", self.image("v3.img", 3, 100, seed=3), expect=1)
        self.assertIn("hint: a trial image is running", r.stderr)


if __name__ == "__main__":
    unittest.main()

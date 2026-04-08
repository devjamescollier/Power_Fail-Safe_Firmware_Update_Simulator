import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MKIMAGE = os.path.join(ROOT, "tools", "mkimage.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkimage


def c_define(header: str, name: str) -> int:
    with open(os.path.join(ROOT, "include", "fwu", header)) as f:
        m = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)u?\b" % name, f.read())
    if m is None:
        raise AssertionError(f"{name} not found in {header}")
    return int(m.group(1), 0)


class MkimageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args):
        return subprocess.run([sys.executable, MKIMAGE, *args],
                              capture_output=True, text=True)

    def make(self, name, version, size, seed=0):
        path = os.path.join(self.tmp.name, name)
        r = self.run_tool("--fw-version", str(version), "--size", str(size),
                          "--seed", str(seed), "-o", path)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(path, "rb") as f:
            return f.read()

    def test_constants_match_c_headers(self):
        self.assertEqual(mkimage.FWIMG_MAGIC, c_define("image.h", "FWIMG_MAGIC"))
        self.assertEqual(mkimage.FWIMG_FORMAT_VERSION, c_define("image.h", "FWIMG_FORMAT_VERSION"))
        self.assertEqual(mkimage.FWIMG_HEADER_SIZE, c_define("image.h", "FWIMG_HEADER_SIZE"))
        self.assertEqual(mkimage.FLASH_SECTOR_SIZE, c_define("flash_layout.h", "FLASH_SECTOR_SIZE"))
        self.assertEqual(mkimage.FLASH_SLOT_SECTORS, c_define("flash_layout.h", "FLASH_SLOT_SECTORS"))

    def test_deterministic(self):
        a = self.make("a.img", 2, 4096, seed=7)
        b = self.make("b.img", 2, 4096, seed=7)
        self.assertEqual(hashlib.sha256(a).digest(), hashlib.sha256(b).digest())
        c = self.make("c.img", 2, 4096, seed=8)
        self.assertNotEqual(a[24:], c[24:])

    def test_header_fields(self):
        img = self.make("v.img", 0x01020003, 1000, seed=3)
        self.assertEqual(len(img), 24 + 1000)
        magic, fmt, hsize, ver, plen, pcrc, hcrc = struct.unpack("<IHHIIII", img[:24])
        self.assertEqual(img[:4], b"FWIM")
        self.assertEqual((magic, fmt, hsize), (0x4D495746, 1, 24))
        self.assertEqual((ver, plen), (0x01020003, 1000))
        self.assertEqual(pcrc, zlib.crc32(img[24:]))
        self.assertEqual(hcrc, zlib.crc32(img[:20]))

    def test_golden_vector_used_by_c_test(self):
        img = self.make("g.img", 0x01020003, 16, seed=1)
        self.assertEqual(img.hex(),
                         "4657494d010018000300020110000000"
                         "241336b738520da9"
                         "bc8287ea9bd3f0045ab85f3d015ccf4b")

    def test_size_limits(self):
        self.assertEqual(len(self.make("min.img", 1, 1)), 25)
        self.assertEqual(len(self.make("max.img", 1, mkimage.FWIMG_MAX_PAYLOAD)), 65536)
        for bad in ("0", str(mkimage.FWIMG_MAX_PAYLOAD + 1), "-5"):
            r = self.run_tool("--fw-version", "1", "--size", bad, "-o",
                              os.path.join(self.tmp.name, "bad.img"))
            self.assertEqual(r.returncode, 2, bad)
            self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "bad.img")))

    def test_bad_version_rejected(self):
        r = self.run_tool("--fw-version", "0x100000000", "--size", "4", "-o",
                          os.path.join(self.tmp.name, "bad.img"))
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkimage

NO_FAULT_BUT_COUNT = str(0xFFFFFFFE)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="fwsim latency and flash-op benchmark")
    p.add_argument("--fwsim", default=os.path.join(ROOT, "build", "fwsim"))
    p.add_argument("--runs", type=int, default=50)
    args = p.parse_args(argv)
    fwsim = os.path.abspath(args.fwsim)

    with tempfile.TemporaryDirectory(prefix="fwbench-") as d:
        flash = os.path.join(d, "flash.img")
        imgs = {}
        for name, ver, size, seed in (("v1", 1, 8192, 1), ("v2", 2, 12000, 2),
                                      ("max", 3, mkimage.FWIMG_MAX_PAYLOAD, 3)):
            path = os.path.join(d, name + ".img")
            with open(path, "wb") as f:
                f.write(mkimage.build_image(ver, size, seed))
            imgs[name] = path

        def fw(*a, count=False):
            cmd = [fwsim, "--flash", flash]
            if count:
                cmd += ["--fail-at", NO_FAULT_BUT_COUNT]
            r = subprocess.run(cmd + list(a), capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(f"fwsim {a} failed: {r.stderr}")
            return r

        def snapshot(*setup):
            if os.path.exists(flash):
                os.remove(flash)
            for step in setup:
                fw(*step)
            snap = os.path.join(d, "snap.bin")
            shutil.copyfile(flash, snap)
            return snap

        cases = [
            ("fwsim --help (start-up baseline)", None, None),
            ("init (8 KiB factory image)", [], ("init", imgs["v1"])),
            ("install (12 KiB image)", [("init", imgs["v1"])], ("install", imgs["v2"])),
            ("install (64 KiB max image)", [("init", imgs["v1"])], ("install", imgs["max"])),
            ("boot, CONFIRMED (verify only)", [("init", imgs["v1"])], ("boot",)),
            ("boot, PENDING -> TRIAL", [("init", imgs["v1"]), ("install", imgs["v2"])], ("boot",)),
            ("confirm, TRIAL -> CONFIRMED",
             [("init", imgs["v1"]), ("install", imgs["v2"]), ("boot",)], ("confirm",)),
            ("boot, TRIAL -> rollback",
             [("init", imgs["v1"]), ("install", imgs["v2"]), ("boot",)], ("boot",)),
            ("status (verifies both slots)", [("init", imgs["v1"]), ("install", imgs["v2"])],
             ("status",)),
        ]

        rows = []
        for label, setup, cmd in cases:
            samples = []
            ops = "-"
            if cmd is None:
                for _ in range(args.runs):
                    t0 = time.perf_counter()
                    subprocess.run([fwsim, "--help"], capture_output=True)
                    samples.append(time.perf_counter() - t0)
            else:
                snap = snapshot(*setup) if setup else None
                for i in range(args.runs + 1):
                    if snap:
                        shutil.copyfile(snap, flash)
                    elif os.path.exists(flash):
                        os.remove(flash)
                    t0 = time.perf_counter()
                    r = fw(*cmd, count=(i == 0))
                    dt = time.perf_counter() - t0
                    if i == 0:
                        pts = int(r.stderr.split("fault points passed:")[1].split()[0])
                        ops = str(pts // 2)
                    else:
                        samples.append(dt)
            samples.sort()
            p95 = samples[min(len(samples) - 1, int(0.95 * len(samples)))]
            rows.append((label, ops, statistics.median(samples) * 1e3, p95 * 1e3))

    print("| Command | Flash ops (erase+program) | Median (ms) | p95 (ms) |")
    print("|---|---:|---:|---:|")
    for label, ops, med, p95 in rows:
        print(f"| {label} | {ops} | {med:.2f} | {p95:.2f} |")
    print(f"\n{args.runs} fresh processes per row.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

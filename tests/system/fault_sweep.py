#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkimage

SLOT_ADDR = (0x02000, 0x12000)
META_ADDR = (0x00000, 0x01000)
META_SECTOR_SIZE = 4096
SLOT_LETTER = "AB"
EXIT_OK, EXIT_FAIL, EXIT_POWER_LOSS = 0, 1, 3

IMAGE_SPECS = {1: (2000, 1), 2: (3000, 2), 3: (1500, 3), 9: (500, 9)}

META_RE = re.compile(r"^metadata:  seq=(\d+) sector=META([01]) state=(\w+) last_event=([\w-]+)$",
                     re.M)
CONF_RE = re.compile(r"^confirmed: slot ([AB]) version 0x([0-9a-f]{8})$", re.M)
CAND_RE = re.compile(r"^candidate: slot ([AB]) version 0x([0-9a-f]{8})$", re.M)
BOOT_RE = re.compile(r"^boot: slot ([AB]) version 0x([0-9a-f]{8}) \((.*)\)$")
POINTS_RE = re.compile(r"fault points passed: (\d+)")

StateKey = Tuple[str, int, Optional[int]]


@dataclass
class Scenario:
    name: str
    setup: List[object]
    target: Tuple[str, ...]
    allowed: List[StateKey]
    allow_unprovisioned: bool = False
    candidate_corrupted: bool = False


@dataclass
class Result:
    name: str
    points: int = 0
    reported_points: Optional[int] = None
    processes: int = 0
    failures: List[str] = field(default_factory=list)


class Sweep:
    def __init__(self, fwsim: str, workdir: str, verbose: bool):
        self.fwsim_exe = fwsim
        self.dir = workdir
        self.verbose = verbose
        self.flash = os.path.join(workdir, "flash.img")
        self.processes = 0
        self.images = {}
        self.paths = {}
        for ver, (size, seed) in IMAGE_SPECS.items():
            data = mkimage.build_image(ver, size, seed)
            path = os.path.join(workdir, f"v{ver}.img")
            with open(path, "wb") as f:
                f.write(data)
            self.images[ver] = data
            self.paths[f"v{ver}"] = path

    def run(self, *args, fail_at=None):
        cmd = [self.fwsim_exe, "--flash", self.flash]
        if fail_at is not None:
            cmd += ["--fail-at", str(fail_at)]
        cmd += [self.paths.get(a, a) for a in args]
        self.processes += 1
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30)

    def read_flash(self, addr, length):
        with open(self.flash, "rb") as f:
            f.seek(addr)
            return f.read(length)

    def slot_holds(self, slot, version):
        img = self.images[version]
        return self.read_flash(SLOT_ADDR[slot], len(img)) == img

    def corrupt_candidate_b(self):
        addr = SLOT_ADDR[1] + 24 + 100
        with open(self.flash, "r+b") as f:
            f.seek(addr)
            b = f.read(1)[0]
            assert b != 0
            f.seek(addr)
            f.write(bytes([b & (b - 1)]))

    def status(self):
        r = self.run("status")
        if r.returncode != EXIT_OK:
            return r, None
        m = META_RE.search(r.stdout)
        if m is None:
            return r, None
        conf = CONF_RE.search(r.stdout)
        cand = CAND_RE.search(r.stdout)
        return r, {
            "seq": int(m.group(1)),
            "sector": int(m.group(2)),
            "state": m.group(3),
            "event": m.group(4),
            "conf_slot": SLOT_LETTER.index(conf.group(1)),
            "conf_ver": int(conf.group(2), 16),
            "cand_slot": SLOT_LETTER.index(cand.group(1)) if cand else None,
            "cand_ver": int(cand.group(2), 16) if cand else None,
        }

    def boot(self):
        r = self.run("boot")
        m = BOOT_RE.match(r.stdout.strip()) if r.returncode == EXIT_OK else None
        if m is None:
            return r, None
        return r, (SLOT_LETTER.index(m.group(1)), int(m.group(2), 16), m.group(3))

    def check_recovery(self, sc: Scenario, pre: dict, pre_meta_bytes: bytes) -> List[str]:
        errs: List[str] = []
        r, meta = self.status()
        if r.returncode != EXIT_OK:
            return [f"status exited {r.returncode}: {r.stderr.strip()}"]

        if meta is None:
            if not sc.allow_unprovisioned:
                return ["R1: no valid metadata after interruption"]
            rb = self.run("boot")
            if rb.returncode != EXIT_FAIL or "no valid metadata" not in rb.stderr:
                errs.append(f"R5: unprovisioned boot gave exit {rb.returncode}: {rb.stdout}{rb.stderr}")
            if self.run("init", "v1").returncode != EXIT_OK:
                errs.append("R6: re-init after interrupted init failed")
            _, b = self.boot()
            if b is None or b[:2] != (0, 1):
                errs.append(f"R6: boot after re-init gave {b}")
            return errs

        key = (meta["state"], meta["conf_ver"], meta["cand_ver"])
        if key not in sc.allowed:
            errs.append(f"R2: state {key} not in allowed {sc.allowed}")

        if pre is not None and meta["seq"] == pre["seq"]:
            now = self.read_flash(META_ADDR[pre["sector"]], META_SECTOR_SIZE)
            if now != pre_meta_bytes:
                errs.append("R3: current record's sector changed without a newer commit")

        conf_slot, conf_ver = meta["conf_slot"], meta["conf_ver"]
        if not self.slot_holds(conf_slot, conf_ver):
            errs.append(f"R4: confirmed slot {SLOT_LETTER[conf_slot]} does not hold v{conf_ver}")
        if (meta["state"] in ("PENDING", "TRIAL") and not sc.candidate_corrupted
                and not self.slot_holds(meta["cand_slot"], meta["cand_ver"])):
            errs.append(f"R4: {meta['state']} record names slot {SLOT_LETTER[meta['cand_slot']]} "
                        f"but it does not hold complete v{meta['cand_ver']}")

        if meta["state"] == "PENDING" and self.slot_holds(meta["cand_slot"], meta["cand_ver"]):
            expect = (meta["cand_slot"], meta["cand_ver"], True)
        else:
            expect = (conf_slot, conf_ver, False)

        rb, b = self.boot()
        if b is None:
            errs.append(f"R5: boot failed, exit {rb.returncode}: {rb.stderr.strip()}")
            return errs
        slot, ver, note = b
        is_trial = "TRIAL" in note
        if (slot, ver, is_trial) != expect:
            errs.append(f"R5: boot selected {(slot, ver, is_trial)}, expected {expect}")
        if not self.slot_holds(slot, ver):
            errs.append(f"R5: booted slot {SLOT_LETTER[slot]} does not hold complete v{ver}")

        if is_trial:
            _, b2 = self.boot()
            if b2 is None or b2[:2] != (conf_slot, conf_ver) or "rolled back" not in b2[2]:
                errs.append(f"R6: unconfirmed trial did not roll back to "
                            f"{SLOT_LETTER[conf_slot]}/v{conf_ver}: {b2}")
        ri = self.run("install", "v9")
        if ri.returncode != EXIT_OK:
            errs.append(f"R6: follow-up install failed: {ri.stderr.strip()}")
            return errs
        _, b3 = self.boot()
        if b3 is None or b3[1] != 9 or b3[0] != 1 - conf_slot or "TRIAL" not in b3[2]:
            errs.append(f"R6: follow-up trial boot gave {b3}")
        if self.run("confirm").returncode != EXIT_OK:
            errs.append("R6: follow-up confirm failed")
        _, b4 = self.boot()
        if b4 is None or b4[1] != 9 or "TRIAL" in b4[2]:
            errs.append(f"R6: follow-up confirmed boot gave {b4}")
        return errs

    def prepare(self, sc: Scenario) -> str:
        if os.path.exists(self.flash):
            os.remove(self.flash)
        for step in sc.setup:
            if callable(step):
                step(self)
                continue
            r = self.run(*step)
            if r.returncode != EXIT_OK:
                raise RuntimeError(f"{sc.name}: setup {step} failed: {r.stderr}")
        snap = os.path.join(self.dir, "snapshot.img")
        if os.path.exists(self.flash):
            shutil.copyfile(self.flash, snap)
        elif os.path.exists(snap):
            os.remove(snap)
        return snap

    def restore(self, snap):
        if os.path.exists(snap):
            shutil.copyfile(snap, self.flash)
        elif os.path.exists(self.flash):
            os.remove(self.flash)

    def run_scenario(self, sc: Scenario, max_points: int) -> Result:
        res = Result(sc.name)
        start_procs = self.processes
        try:
            snap = self.prepare(sc)
        except RuntimeError as e:
            res.failures.append(str(e))
            res.processes = self.processes - start_procs
            return res
        _, pre = self.status() if os.path.exists(self.flash) else (None, None)
        pre_meta = self.read_flash(META_ADDR[pre["sector"]], META_SECTOR_SIZE) if pre else b""
        if pre is not None and (pre["state"], pre["conf_ver"], pre["cand_ver"]) != sc.allowed[0]:
            res.failures.append(f"setup produced {pre}, expected {sc.allowed[0]}")
            res.processes = self.processes - start_procs
            return res

        k = 0
        while True:
            if k > max_points:
                res.failures.append(f"command did not complete within {max_points} points")
                break
            self.restore(snap)
            r = self.run(*sc.target, fail_at=k)
            if r.returncode == EXIT_OK:
                m = POINTS_RE.search(r.stderr)
                res.reported_points = int(m.group(1)) if m else None
                res.points = k
                _, meta = self.status()
                got = (meta["state"], meta["conf_ver"], meta["cand_ver"]) if meta else None
                if got != sc.allowed[-1]:
                    res.failures.append(f"completed run ended in {got}, expected {sc.allowed[-1]}")
                break
            if r.returncode != EXIT_POWER_LOSS:
                res.failures.append(f"K={k}: target exited {r.returncode}: {r.stderr.strip()}")
                k += 1
                continue
            for e in self.check_recovery(sc, pre, pre_meta):
                res.failures.append(f"K={k}: {e}")
            if self.verbose:
                print(f"  {sc.name} K={k}: {'ok' if not res.failures else res.failures[-1]}")
            k += 1

        if res.reported_points is None and not res.failures:
            res.failures.append("CLI did not report its fault point count")
        elif res.reported_points != res.points:
            res.failures.append(f"sweep found {res.points} points, CLI reported "
                                f"{res.reported_points}")
        res.processes = self.processes - start_procs
        return res


SCENARIOS = [
    Scenario("factory init", [], ("init", "v1"),
             [("CONFIRMED", 1, None)], allow_unprovisioned=True),
    Scenario("install", [("init", "v1")], ("install", "v2"),
             [("CONFIRMED", 1, None), ("PENDING", 1, 2)]),
    Scenario("install while pending", [("init", "v1"), ("install", "v2")], ("install", "v3"),
             [("PENDING", 1, 2), ("CONFIRMED", 1, None), ("PENDING", 1, 3)]),
    Scenario("first trial boot", [("init", "v1"), ("install", "v2")], ("boot",),
             [("PENDING", 1, 2), ("TRIAL", 1, 2)]),
    Scenario("boot rejects corrupt candidate",
             [("init", "v1"), ("install", "v2"), Sweep.corrupt_candidate_b], ("boot",),
             [("PENDING", 1, 2), ("CONFIRMED", 1, None)], candidate_corrupted=True),
    Scenario("confirm", [("init", "v1"), ("install", "v2"), ("boot",)], ("confirm",),
             [("TRIAL", 1, 2), ("CONFIRMED", 2, None)]),
    Scenario("rollback boot", [("init", "v1"), ("install", "v2"), ("boot",)], ("boot",),
             [("TRIAL", 1, 2), ("CONFIRMED", 1, None)]),
    Scenario("install after confirmed update",
             [("init", "v1"), ("install", "v2"), ("boot",), ("confirm",)], ("install", "v3"),
             [("CONFIRMED", 2, None), ("PENDING", 2, 3)]),
]


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="Deterministic power-loss sweep for fwsim.")
    p.add_argument("--fwsim", default=os.environ.get("FWSIM", os.path.join(ROOT, "build", "fwsim")))
    p.add_argument("--max-points", type=int, default=5000,
                   help="safety bound on points per scenario")
    p.add_argument("-k", "--filter", default="", help="run scenarios whose name contains this")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    if not os.access(args.fwsim, os.X_OK):
        print(f"fault_sweep: fwsim not found at {args.fwsim}", file=sys.stderr)
        return 2

    t0 = time.monotonic()
    results = []
    with tempfile.TemporaryDirectory(prefix="fwsweep-") as tmp:
        sweep = Sweep(args.fwsim, tmp, args.verbose)
        for sc in SCENARIOS:
            if args.filter in sc.name:
                results.append(sweep.run_scenario(sc, args.max_points))
    elapsed = time.monotonic() - t0

    print(f"{'scenario':34} {'points':>6} {'cli':>5} {'procs':>6}  result")
    for r in results:
        cli = "-" if r.reported_points is None else str(r.reported_points)
        print(f"{r.name:34} {r.points:6d} {cli:>5} {r.processes:6d}  "
              f"{'PASS' if not r.failures else 'FAIL'}")
    total_points = sum(r.points for r in results)
    total_procs = sum(r.processes for r in results)
    failed = [r for r in results if r.failures]
    print(f"{'TOTAL':34} {total_points:6d} {'':>5} {total_procs:6d}  "
          f"{'PASS' if not failed else 'FAIL'}")
    print(f"interruption points tested: {total_points} across {len(results)} scenarios; "
          f"{total_procs} simulator processes; {elapsed:.1f}s")

    for r in failed:
        print(f"\nFAILURES in '{r.name}' ({len(r.failures)}):")
        for f in r.failures[:20]:
            print(f"  {f}")
        if len(r.failures) > 20:
            print(f"  ... {len(r.failures) - 20} more")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

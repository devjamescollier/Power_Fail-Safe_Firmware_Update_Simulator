# Power-Fail-Safe Firmware Update Simulator
![Status](https://img.shields.io/badge/status-simulation%20tested-brightgreen.svg)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C11](https://img.shields.io/badge/C-C11-A8B9CC.svg)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C.svg)](https://cmake.org/)
![Tests](https://img.shields.io/badge/tests-9%2F9%20passing-brightgreen.svg)
![Fault Sweep](https://img.shields.io/badge/fault%20sweep-266%2F266%20points-brightgreen.svg)

This is a C11 simulation, run on a desktop computer, of how an embedded device
installs firmware into an inactive flash slot, trial-boots it, and returns to
the last confirmed image if the update is never confirmed. Storage is a file
that obeys NOR-flash rules. A deterministic fault injector cuts "power" at
every flash erase and program boundary, and at a point partway through each
operation. After every cut, fresh processes check that the device still boots
a complete, valid image.

The simulator reports which image would boot. It does not execute firmware.
CRC-32 is used to detect accidental corruption only. This project does
**not** implement secure boot, image signing, or authentication, and it has
not been run on microcontroller hardware.

## Table of Contents
- [Overview](#overview)
- [Specifications](#specifications)
- [Results](#results)
- [Software](#software)
- [Running the Software](#running-the-software)
- [Tech Stack](#tech-stack)
- [Equipment](#equipment)
- [Repository Structure](#repository-structure)
- [Contact](#contact)
- [License](#license)

## Overview

The project is a host-side model of a bootloader's A/B update path. The
update engine is portable C, written the way it would be written for a
microcontroller. It reaches storage only through a three-function flash
interface, which a file-backed simulator implements.

| Section                 | Description |
| ---                     | --- |
| **Update Engine**       | `src/core` handles image parsing and verification, the two-sector metadata commit protocol, and the CONFIRMED / PENDING / TRIAL state machine. It makes no file I/O or POSIX calls and never allocates from the heap. A CTest check enforces these rules. |
| **Flash Interface**     | `read`, `program` and `erase_sector`, passed in as function pointers (`include/fwu/flash_if.h`). Swapping the simulator for a real flash driver requires no engine changes. |
| **Flash Simulator**     | `src/sim` is a 139,264-byte file with NOR rules: erased bytes read `0xFF`, programming can only clear bits, and erase works on 4 KiB sectors. It also provides deterministic power-loss injection. |
| **Command-Line Tool**   | `fwsim` provides `init`, `status`, `install`, `boot` and `confirm`. Each invocation is one "power cycle" against the same flash file. |
| **Image Tool**          | `tools/mkimage.py` creates deterministic firmware images using only the Python standard library. |
| **Test Harness**        | C unit tests, multi-process CLI tests, and a power-loss sweep that checks recovery invariants after every interruption point. |

### Flash Layout

All geometry is defined in [`include/fwu/flash_layout.h`](include/fwu/flash_layout.h), and compile-time asserts check that every region is sector-aligned.

| Region     | Start     | Size   | Sectors | Purpose |
| ---        | ---       | ---:   | ---:    | --- |
| **META0**  | `0x00000` | 4 KiB  | 0       | Metadata record (A/B pair) |
| **META1**  | `0x01000` | 4 KiB  | 1       | Metadata record (A/B pair) |
| **SLOT A** | `0x02000` | 64 KiB | 2–17    | Firmware image: header + payload |
| **SLOT B** | `0x12000` | 64 KiB | 18–33   | Firmware image: header + payload |

### Update State Machine

```text
                 install (candidate verified)
  +-----------+ ----------------------------> +---------+
  | CONFIRMED |                               | PENDING |
  +-----------+ <---------------------------- +---------+
     ^     ^      boot: candidate fails            |
     |     |      verification (REJECTED)          | boot: candidate verifies
     |     |                                       v
     |     |      boot without confirm        +---------+
     |     +--------------------------------- |  TRIAL  |
     |            (ROLLED_BACK)               +---------+
     |                                             |
     +---------------------------------------------+
           confirm (candidate becomes the confirmed image; UPDATED)
```

- **TRIAL is saved before a trial boot is reported.** If power fails before
  the application confirms, the next boot finds TRIAL and rolls back.
- **Install only ever writes the inactive slot,** and only while the state is
  CONFIRMED. Installing while PENDING first commits a cancel.
- **Boot never selects an image it hasn't verified.** The image must pass its
  header CRC, length and payload CRC checks, and it must match the version
  and CRC that the metadata record names.

### Power-Fail Safety

Each metadata change writes a new 40-byte record to the sector that does
**not** hold the newest valid record, in this order:
1. Erase that sector.
2. Program the 36-byte body, which includes the record's CRC-32.
3. Program a 4-byte commit marker last, in its own write.
4. Read the record back and check it.

A record counts only if its magic, layout version, CRC, marker and every
field are valid. At startup the record with the higher sequence number wins,
and the comparison still works after the counter wraps around. Every
interruption therefore resolves to either the old record or the new one,
never a mix.

**Hardest edge case: installing over a PENDING candidate.** The new image has
to go into the slot the current record still names as the candidate. If that
slot were erased first and power failed, the newest record would point at a
half-erased image. The engine therefore commits a "cancel" record before it
erases anything.

Boot-time verification would also reject the broken candidate, which hides
this bug. A deliberately broken build passed the original sweep, so a direct
check was added: any PENDING or TRIAL record must name a complete candidate.

Full design notes: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Specifications

| Parameter                         | Value |
| --------------------------------- | ----- |
| **Language / Standard**           | C11 engine and CLI; Python 3 tooling (standard library only) |
| **Build / Test**                  | CMake ≥ 3.16, CTest |
| **Flash Model**                   | NOR: erased `0xFF`, 1→0 programming only, 4 KiB sector erase |
| **Flash Size**                    | 139,264 bytes (34 × 4 KiB sectors) |
| **Firmware Slots**                | 2 × 64 KiB (A/B) |
| **Metadata**                      | 2 × 4 KiB sectors, one 40-byte record each |
| **Image Header**                  | 24 bytes, little-endian, written and read field by field |
| **Maximum Payload**               | 65,512 bytes (a full image fills one slot exactly) |
| **Integrity Check**               | CRC-32 (reflected poly `0xEDB88320`, same as `zlib.crc32`); not a security check |
| **Update States**                 | CONFIRMED, PENDING, TRIAL |
| **Trial Attempts**                | 1 (an unconfirmed trial rolls back on the next boot) |
| **Engine RAM**                    | `fwu_ctx_t` = 80 bytes; no heap use |
| **Engine Code Size (host)**       | 7,342 B `__TEXT` at `-O3`; 5,074 B at `-Os` (arm64 macOS; not an MCU figure) |
| **Metadata Writes per Update**    | 3 record commits (PENDING, TRIAL, CONFIRMED or rollback), each 1 erase + 2 programs |
| **Writes on a Normal Boot**       | 0 |
| **Fault Points per Flash Op**     | 2 (partway through, and after completion) |
| **Compiler Warnings**             | `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef -Werror` |

## Results

The figures below were measured from clean build directories on a MacBook
Pro (Apple M1, 8 GB) running macOS 26.5.1, with Apple clang 21.0.0, CMake
4.4.2 and Python 3.14.7.

### Test Results

| Test                   | Result | Coverage |
| ---------------------- | ------ | -------- |
| **`unit_crc32`**       | 4 checks, 0 failures   | Standard check value `0xCBF43926`, streaming equals one-shot |
| **`unit_image`**       | 42 checks, 0 failures  | All 512 single-bit payload flips, all 192 single-bit header flips, and every truncation length of a 124-byte image rejected; oversized and zero-length payloads rejected; a 40-byte golden image produced by `mkimage.py` |
| **`unit_flash_sim`**   | 101 checks, 0 failures | 0→1 writes rejected without changing anything; sector erase; range and wraparound checks; persistence across reopen; partial and post-operation faults checked on disk afterwards |
| **`unit_meta`**        | 167 checks, 0 failures | All 288 single-bit record flips rejected; wraparound-safe selection; sector alternation; 43 hand-built interrupted-commit states |
| **`unit_update`**      | 215 checks, 0 failures | Every state transition with a reboot between steps; the confirmed slot left byte-for-byte unchanged by install; 7 rejected-install cases; a candidate corrupted or swapped before boot |
| **`core_portability`** | Passed                 | No stdio, stdlib, POSIX or heap calls in the engine |
| **`py_mkimage`**       | 6 tests OK             | Determinism, header fields, size limits, constants match the C headers |
| **`py_cli_e2e`**       | 12 tests OK            | Multi-process runs: successful update, unconfirmed rollback, confirmed image persists, corrupt and malformed images rejected, a bad `init` path leaves the device untouched |
| **`fault_sweep`**      | 266 / 266 points PASS  | Recovery invariants R1–R6 checked after every interruption |

| Build Configuration        | Warnings | CTest  | Total Test Time |
| -------------------------- | -------: | -----: | --------------: |
| **Debug**                  | 0        | 9 / 9  | 6.67 s          |
| **Release (`-O3`)**        | 0        | 9 / 9  | 6.21 s          |
| **ASan + UBSan (Debug)**   | 0        | 9 / 9  | 127.73 s        |

### Power-Loss Fault Sweep

For each scenario, the sweep restores the starting flash file, cuts power at
fault point K, and then starts **fresh processes** on the same file to check
recovery. It repeats for K = 0, 1, 2, … until the command completes.

| Scenario                            | Interruption Points | Simulator Processes | Result |
| ----------------------------------: | ------------------: | ------------------: | -----: |
| **Factory init**                    | 62                  | 314                 | PASS   |
| **Install**                         | 66                  | 467                 | PASS   |
| **Install while pending**           | 60                  | 431                 | PASS   |
| **First trial boot**                | 6                   | 52                  | PASS   |
| **Boot rejects corrupt candidate**  | 6                   | 47                  | PASS   |
| **Confirm**                         | 6                   | 48                  | PASS   |
| **Rollback boot**                   | 6                   | 48                  | PASS   |
| **Install after confirmed update**  | 54                  | 386                 | PASS   |
| **Total**                           | **266**             | **1,793**           | **PASS** |

The sweep took 4.2 s with the Debug build. Each scenario's point count
matched the count the simulator itself reported. After every interruption,
the sweep checks:

| Check | Meaning |
| ----- | ------- |
| R1 | The metadata is still valid |
| R2 | The state is the before or after state of the interrupted command, never halfway between |
| R3 | If nothing new was committed, the old record is unchanged |
| R4 | The confirmed slot holds its complete image, and a PENDING or TRIAL candidate is complete |
| R5 | Boot selects the expected slot, which holds a complete image |
| R6 | An unconfirmed trial rolls back, and a full new update then succeeds |

**Mutation testing.** Deliberately broken builds checked that the tests catch
real bugs:

| Change | Caught? |
| --- | --- |
| Overwrite the current record in place | Yes |
| Erase the candidate without cancelling it first | Yes, after the R4 candidate check was added |
| Don't save TRIAL on the first boot | Yes |
| Don't save the rollback | Yes |
| Write the commit marker before the body | **No** |

The last one is expected rather than a bug. Under this fault model the CRC
alone rejects every torn record, so the marker-last ordering is defense in
depth against a torn record that happens to match its CRC.

### Performance

Measured with `python3 -B tools/bench.py --fwsim build-release/fwsim --runs 50`
on a Release build. Every sample is a fresh process, so the times include
about 2.3 ms of process start-up, shown in the first row. The flash operation
counts come from the simulator's own counter.

| Command                              | Flash Ops (erase + program) | Median (ms) | p95 (ms) |
| ------------------------------------ | --------------------------: | ----------: | -------: |
| **`fwsim --help`** (start-up only)   | —                           | 2.35        | 7.78     |
| **`init`** (8 KiB factory image)     | 55                          | 3.16        | 3.50     |
| **`install`** (12 KiB image)         | 69                          | 3.05        | 4.03     |
| **`install`** (64 KiB maximum image) | 283                         | 4.32        | 4.77     |
| **`boot`**, CONFIRMED (verify only)  | 0                           | 2.23        | 2.66     |
| **`boot`**, PENDING → TRIAL          | 3                           | 2.21        | 2.81     |
| **`confirm`**, TRIAL → CONFIRMED     | 3                           | 2.20        | 2.61     |
| **`boot`**, TRIAL → rollback         | 3                           | 2.12        | 2.29     |
| **`status`** (verifies both slots)   | 0                           | 2.23        | 2.36     |

State changes (boot, confirm, rollback) cost exactly one record commit: one
erase and two programs. An install is dominated by erasing the 16-sector slot
and programming the image in chunks of up to 256 bytes. These are desktop
timings against a file and say nothing about MCU flash timing.

### Known Limitations

- **Simulation only.** Torn writes are modeled as "the first half was
  written", while real NOR can leave other bit patterns. Bit rot and read
  disturb are not modeled.
- **One trial boot, no watchdog.** The application confirming is represented
  by the `confirm` command.
- **Corrupt confirmed image.** Boot reports "no bootable image" and does not
  fall back to the other slot.
- **No anti-rollback.** `fw_version` is informational, so downgrades are
  accepted.
- **No authenticity.** CRC-32 is not a signature.
- **CI not yet run.** The GitHub Actions workflow (GCC and Clang, Debug,
  Release, ASan+UBSan) is configured but hasn't run. LeakSanitizer only runs
  in the Linux job.

## Software

The update engine, flash simulator and CLI are written in C11. The tooling
and system tests use only the Python 3 standard library.

### Building and Running

1. Install CMake ≥ 3.16, GCC or Clang, and Python 3.
2. Configure and build:
   ```sh
   cmake -S . -B build
   cmake --build build
   ```
3. Run the full test suite, including the fault sweep:
   ```sh
   ctest --test-dir build --output-on-failure
   ```
4. Run the update-and-rollback demo:
   ```sh
   sh tools/demo.sh build/fwsim
   ```
5. Optional: run the fault sweep on its own, the sanitizer build, or the
   benchmark:
   ```sh
   python3 -B tests/system/fault_sweep.py --fwsim build/fwsim

   cmake -S . -B build-asan -DFWU_SANITIZE=ON
   cmake --build build-asan
   ctest --test-dir build-asan --output-on-failure

   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
   cmake --build build-release
   python3 -B tools/bench.py --fwsim build-release/fwsim
   ```

### Demo: Successful Update and Rollback

Run these from the repository root after building:

```sh
python3 tools/mkimage.py --fw-version 0x00010000 --size 8192  --seed 1 -o v1.img
python3 tools/mkimage.py --fw-version 0x00010100 --size 12000 --seed 2 -o v2.img
python3 tools/mkimage.py --fw-version 0x00010200 --size 9000  --seed 3 -o v3.img

build/fwsim init v1.img      # factory image, confirmed, slot A
build/fwsim install v2.img   # stage v2 in slot B -> PENDING
build/fwsim boot             # trial-boot v2 -> TRIAL
build/fwsim confirm          # keep v2 -> CONFIRMED
build/fwsim install v3.img   # stage v3 in slot A
build/fwsim boot             # trial-boot v3
build/fwsim boot             # not confirmed -> rolls back to v2
build/fwsim status
```

The full output of every step is in
[Running the Software](#running-the-software).

### CLI Reference

```text
usage: fwsim [--flash PATH] [--fail-at N] <command> [args]
commands:
  init <factory.img>   create flash with a confirmed factory image
  status               show metadata and slot validity
  install <image>      stage a candidate into the inactive slot
  boot                 simulate a reboot and report the selected image
  confirm              confirm the running trial image
options:
  --flash PATH         flash file (default: flash.img)
  --fail-at N          simulate power loss at fault point N
```

| Exit Code | Meaning |
| --------: | ------- |
| **0**     | OK |
| **1**     | Operation failed (rejected image, wrong state, missing file) |
| **2**     | Usage error |
| **3**     | Simulated power loss (`--fail-at`) |

## Running the Software

Everything below is unedited terminal output from one session on the
machine listed under [Equipment](#equipment). It was run from the
repository root against a fresh Debug build (the default build type).
Absolute local paths are shortened to `…`. Commands that succeed exit with
0, and any other exit code is shown as `[exit N]`.

| Step | What It Shows |
| --- | --- |
| [1. Build](#1-build) | Clean configure and build of the engine, simulator, CLI and tests |
| [2. Test Suite](#2-test-suite) | All 9 CTest tests passing, plus the unit-test check counts |
| [3. Create Firmware Images](#3-create-firmware-images) | Three deterministic images with their CRCs |
| [4. Full Update Lifecycle](#4-full-update-lifecycle) | CONFIRMED → PENDING → TRIAL → CONFIRMED, with `status` after each step |
| [5. Rollback of an Unconfirmed Trial](#5-rollback-of-an-unconfirmed-trial) | A trial image that is never confirmed is abandoned on the next boot |
| [6. Power Loss During Install](#6-power-loss-during-install) | Power cut while the image is being written; the old firmware still boots |
| [7. Power Loss During Confirm](#7-power-loss-during-confirm) | Power cut before the confirm record is committed; the device rolls back safely |
| [8. Power Loss During Trial Boot](#8-power-loss-during-trial-boot) | Power cut while saving TRIAL; the update is still pending and resumes |
| [9. Corrupt Candidate Rejected at Boot](#9-corrupt-candidate-rejected-at-boot) | One flipped bit in flash; boot refuses the candidate |
| [10. Error Handling](#10-error-handling) | Wrong state, bad images, missing files, unknown commands |
| [11. Power-Loss Fault Sweep](#11-power-loss-fault-sweep) | Power cut at all 266 interruption points, with recovery checked after each |
| [12. Demo Script](#12-demo-script) | The scripted end-to-end demo exiting cleanly |

### 1. Build

```text
$ cmake -S . -B build
-- The C compiler identification is AppleClang 21.0.0.21000101
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Check for working C compiler: /usr/bin/cc - skipped
-- Detecting C compile features
-- Detecting C compile features - done
-- Found Python3: /opt/homebrew/Frameworks/Python.framework/Versions/3.14/bin/python3.14 (found version "3.14.7") found components: Interpreter
-- Configuring done (0.8s)
-- Generating done (0.1s)
-- Build files have been written to: …/failsafe-fwupdate/build

$ cmake --build build
[  5%] Building C object CMakeFiles/fwu_core.dir/src/core/crc32.c.o
[ 10%] Building C object CMakeFiles/fwu_core.dir/src/core/image.c.o
[ 15%] Building C object CMakeFiles/fwu_core.dir/src/core/meta.c.o
[ 20%] Building C object CMakeFiles/fwu_core.dir/src/core/status.c.o
[ 25%] Building C object CMakeFiles/fwu_core.dir/src/core/update.c.o
[ 30%] Linking C static library libfwu_core.a
[ 30%] Built target fwu_core
[ 35%] Building C object CMakeFiles/fwu_flashsim.dir/src/sim/flash_sim.c.o
[ 40%] Linking C static library libfwu_flashsim.a
[ 40%] Built target fwu_flashsim
[ 45%] Building C object CMakeFiles/fwsim.dir/src/app/fwsim_main.c.o
[ 50%] Linking C executable fwsim
[ 50%] Built target fwsim
[ 55%] Building C object CMakeFiles/test_crc32.dir/tests/unit/test_crc32.c.o
[ 60%] Linking C executable test_crc32
[ 60%] Built target test_crc32
[ 65%] Building C object CMakeFiles/test_image.dir/tests/unit/test_image.c.o
[ 70%] Linking C executable test_image
[ 70%] Built target test_image
[ 75%] Building C object CMakeFiles/test_flash_sim.dir/tests/unit/test_flash_sim.c.o
[ 80%] Linking C executable test_flash_sim
[ 80%] Built target test_flash_sim
[ 85%] Building C object CMakeFiles/test_meta.dir/tests/unit/test_meta.c.o
[ 90%] Linking C executable test_meta
[ 90%] Built target test_meta
[ 95%] Building C object CMakeFiles/test_update.dir/tests/unit/test_update.c.o
[100%] Linking C executable test_update
[100%] Built target test_update
```

The build finished with zero warnings under the full warning set, including `-Werror`.

### 2. Test Suite

```text
$ ctest --test-dir build --output-on-failure
Test project …/failsafe-fwupdate/build
    Start 1: unit_crc32
1/9 Test #1: unit_crc32 .......................   Passed    0.29 sec
    Start 2: unit_image
2/9 Test #2: unit_image .......................   Passed    0.22 sec
    Start 3: unit_flash_sim
3/9 Test #3: unit_flash_sim ...................   Passed    0.21 sec
    Start 4: unit_meta
4/9 Test #4: unit_meta ........................   Passed    0.20 sec
    Start 5: unit_update
5/9 Test #5: unit_update ......................   Passed    0.27 sec
    Start 6: core_portability
6/9 Test #6: core_portability .................   Passed    0.02 sec
    Start 7: py_mkimage
7/9 Test #7: py_mkimage .......................   Passed    0.53 sec
    Start 8: py_cli_e2e
8/9 Test #8: py_cli_e2e .......................   Passed    0.54 sec
    Start 9: fault_sweep
9/9 Test #9: fault_sweep ......................   Passed    4.43 sec

100% tests passed out of 9

Label Time Summary:
e2e       =   0.54 sec*proc (1 test)
fault     =   4.43 sec*proc (1 test)
python    =   5.50 sec*proc (3 tests)
static    =   0.02 sec*proc (1 test)
unit      =   1.17 sec*proc (5 tests)

Total Test time (real) =   6.70 sec
```

Each unit-test binary can also be run directly:

```text
$ build/test_crc32
test_crc32: 4 checks, 0 failures
$ build/test_image
test_image: 42 checks, 0 failures
$ build/test_flash_sim
test_flash_sim: 101 checks, 0 failures
$ build/test_meta
test_meta: 167 checks, 0 failures
$ build/test_update
test_update: 215 checks, 0 failures
```

### 3. Create Firmware Images

`mkimage.py` builds each image from a seed, so the same arguments always
produce the same bytes and the same CRC.

```text
$ python3 tools/mkimage.py --fw-version 0x00010000 --size 8192 --seed 1 -o v1.img
v1.img: fw_version=0x00010000 payload_len=8192 payload_crc=0xd48cbf4e total=8216

$ python3 tools/mkimage.py --fw-version 0x00010100 --size 12000 --seed 2 -o v2.img
v2.img: fw_version=0x00010100 payload_len=12000 payload_crc=0x29559baf total=12024

$ python3 tools/mkimage.py --fw-version 0x00010200 --size 9000 --seed 3 -o v3.img
v3.img: fw_version=0x00010200 payload_len=9000 payload_crc=0x41e68031 total=9024
```

### 4. Full Update Lifecycle

Each `fwsim` command is a separate process, the equivalent of a power cycle.
Each one reads all of its state back from `flash.img`. The metadata sequence
number goes up by one with every commit, and the record alternates between
META0 and META1.

**Factory state.** v1 is confirmed in slot A, and slot B is still erased.

```text
$ build/fwsim init v1.img
init: factory image version 0x00010000 in slot A, CONFIRMED

$ build/fwsim status
metadata:  seq=1 sector=META0 state=CONFIRMED last_event=none
confirmed: slot A version 0x00010000
candidate: none
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    invalid (image: bad magic) [inactive]

$ build/fwsim boot
boot: slot A version 0x00010000 (confirmed)
```

**Install.** v2 is written to the inactive slot, B, and verified. The state
becomes PENDING, and slot A is left untouched.

```text
$ build/fwsim install v2.img
install: version 0x00010100 staged in slot B, PENDING

$ build/fwsim status
metadata:  seq=2 sector=META1 state=PENDING last_event=none
confirmed: slot A version 0x00010000
candidate: slot B version 0x00010100
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [candidate]
```

**Trial boot.** TRIAL is saved to flash before the boot is reported. Slot A
is still the confirmed fallback.

```text
$ build/fwsim boot
boot: slot B version 0x00010100 (TRIAL, awaiting confirm)

$ build/fwsim status
metadata:  seq=3 sector=META0 state=TRIAL last_event=none
confirmed: slot A version 0x00010000
candidate: slot B version 0x00010100
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [candidate]
```

**Confirm.** The running application accepts v2, and slot B becomes the
confirmed image.

```text
$ build/fwsim confirm
confirm: slot B version 0x00010100 is now confirmed

$ build/fwsim boot
boot: slot B version 0x00010100 (confirmed)

$ build/fwsim status
metadata:  seq=4 sector=META1 state=CONFIRMED last_event=updated
confirmed: slot B version 0x00010100
candidate: none
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [inactive]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [confirmed]
```

### 5. Rollback of an Unconfirmed Trial

v3 goes into slot A, which is now the inactive slot, and is trial-booted.
The application never confirms it, so the next boot rolls back to v2.

```text
$ build/fwsim install v3.img
install: version 0x00010200 staged in slot A, PENDING

$ build/fwsim boot
boot: slot A version 0x00010200 (TRIAL, awaiting confirm)

$ build/fwsim boot
boot: slot B version 0x00010100 (confirmed; rolled back, trial was not confirmed)

$ build/fwsim status
metadata:  seq=7 sector=META0 state=CONFIRMED last_event=rolled-back
confirmed: slot B version 0x00010100
candidate: none
slot A:    valid   version 0x00010200 payload_len=9000 crc=0x41e68031 [inactive]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [confirmed]
```

### 6. Power Loss During Install

`--fail-at 40` cuts power at fault point 40. That falls after the 16-sector
slot erase and partway through programming the new image. Slot B is left
half-written, but the metadata was never changed, so the device still boots
v1. A retry then completes normally.

```text
$ build/fwsim init v1.img
init: factory image version 0x00010000 in slot A, CONFIRMED

$ build/fwsim --fail-at 40 install v2.img
fwsim: simulated power loss at fault point 40
[exit 3]

$ build/fwsim status
metadata:  seq=1 sector=META0 state=CONFIRMED last_event=none
confirmed: slot A version 0x00010000
candidate: none
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    invalid (image: payload CRC mismatch) [inactive]

$ build/fwsim boot
boot: slot A version 0x00010000 (confirmed)

$ build/fwsim install v2.img
install: version 0x00010100 staged in slot B, PENDING

$ build/fwsim boot
boot: slot B version 0x00010100 (TRIAL, awaiting confirm)

$ build/fwsim confirm
confirm: slot B version 0x00010100 is now confirmed
```

### 7. Power Loss During Confirm

`--fail-at 3` cuts power after the new record's body is written but before
its commit marker. The torn record is ignored, and the TRIAL record is still
the newest valid one. The confirm never took effect, so the next boot treats
v2 as an unconfirmed trial and rolls back to v1. The device boots a complete,
known image either way.

```text
$ build/fwsim init v1.img
init: factory image version 0x00010000 in slot A, CONFIRMED

$ build/fwsim install v2.img
install: version 0x00010100 staged in slot B, PENDING

$ build/fwsim boot
boot: slot B version 0x00010100 (TRIAL, awaiting confirm)

$ build/fwsim --fail-at 3 confirm
fwsim: simulated power loss at fault point 3
[exit 3]

$ build/fwsim status
metadata:  seq=3 sector=META0 state=TRIAL last_event=none
confirmed: slot A version 0x00010000
candidate: slot B version 0x00010100
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [candidate]

$ build/fwsim boot
boot: slot A version 0x00010000 (confirmed; rolled back, trial was not confirmed)
```

### 8. Power Loss During Trial Boot

`--fail-at 2` cuts power partway through writing the TRIAL record. The
PENDING record is still the newest valid one, so the update is not lost. The
next boot simply starts the trial again.

```text
$ build/fwsim init v1.img
init: factory image version 0x00010000 in slot A, CONFIRMED

$ build/fwsim install v2.img
install: version 0x00010100 staged in slot B, PENDING

$ build/fwsim --fail-at 2 boot
fwsim: simulated power loss at fault point 2
[exit 3]

$ build/fwsim status
metadata:  seq=2 sector=META1 state=PENDING last_event=none
confirmed: slot A version 0x00010000
candidate: slot B version 0x00010100
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [candidate]

$ build/fwsim boot
boot: slot B version 0x00010100 (TRIAL, awaiting confirm)
```

### 9. Corrupt Candidate Rejected at Boot

After v2 is staged, a single bit of its payload is flipped directly in
`flash.img`, at byte 100 of the payload in slot B. Boot re-verifies the
candidate, refuses it, records the rejection, and boots the confirmed image.

```text
$ build/fwsim install v2.img
install: version 0x00010100 staged in slot B, PENDING

$ (flip one bit of the candidate payload in flash.img)

$ build/fwsim status
metadata:  seq=2 sector=META1 state=PENDING last_event=none
confirmed: slot A version 0x00010000
candidate: slot B version 0x00010100
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    invalid (image: payload CRC mismatch) [candidate]

$ build/fwsim boot
boot: slot A version 0x00010000 (confirmed; pending candidate failed verification)

$ build/fwsim status
metadata:  seq=3 sector=META0 state=CONFIRMED last_event=rejected
confirmed: slot A version 0x00010000
candidate: none
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    invalid (image: payload CRC mismatch) [inactive]
```

### 10. Error Handling

Rejected operations exit with 1 and leave flash unchanged. Usage errors exit
with 2. Where it helps, the CLI prints a hint about what to do next.

```text
$ build/fwsim init v1.img
init: factory image version 0x00010000 in slot A, CONFIRMED

$ build/fwsim confirm
confirm: failed: not allowed in current state
confirm: hint: no trial image is running; 'install' and 'boot' an update first
[exit 1]

$ build/fwsim install v3-truncated.img
install: rejected: image: truncated
[exit 1]

$ build/fwsim install bad.img
install: rejected: image: truncated
[exit 1]

$ build/fwsim install missing.img
install: cannot open image file 'missing.img'
[exit 1]

$ build/fwsim frobnicate
fwsim: unknown command 'frobnicate'
usage: fwsim [--flash PATH] [--fail-at N] <command> [args]
commands:
  init <factory.img>   create flash with a confirmed factory image
  status               show metadata and slot validity
  install <image>      stage a candidate into the inactive slot
  boot                 simulate a reboot and report the selected image
  confirm              confirm the running trial image
options:
  --flash PATH         flash file (default: flash.img)
  --fail-at N          simulate power loss at fault point N
[exit 2]

$ build/fwsim --flash nonexistent.img status
status: cannot open flash file 'nonexistent.img'
status: hint: create it with 'fwsim --flash nonexistent.img init <factory.img>'
[exit 1]
```

The test inputs were made with `head -c 5000 v3.img > v3-truncated.img`,
which cuts v3 off partway through its payload, and with a 20-byte text file,
`bad.img`. That file is shorter than the 24-byte header, so it is also
reported as truncated.

### 11. Power-Loss Fault Sweep

The sweep cuts power at every fault point K of each scenario, from a known
starting state. After each cut, it starts fresh `fwsim` processes to check
recovery invariants R1–R6 (see [Power-Loss Fault Sweep](#power-loss-fault-sweep)).
The `points` and `cli` columns must agree: the first is the number of points
the sweep found, and the second is the count the simulator reported for an
uninterrupted run.

```text
$ python3 -B tests/system/fault_sweep.py --fwsim build/fwsim
scenario                           points   cli  procs  result
factory init                           62    62    314  PASS
install                                66    66    467  PASS
install while pending                  60    60    431  PASS
first trial boot                        6     6     52  PASS
boot rejects corrupt candidate          6     6     47  PASS
confirm                                 6     6     48  PASS
rollback boot                           6     6     48  PASS
install after confirmed update         54    54    386  PASS
TOTAL                                 266         1793  PASS
interruption points tested: 266 across 8 scenarios; 1793 simulator processes; 4.7s
```

### 12. Demo Script

`tools/demo.sh` runs steps 3–5 plus the truncated-image rejection in a
temporary directory, and it fails if any step behaves unexpectedly. CI runs
it on every push.

```text
$ sh tools/demo.sh build/fwsim
# Build three deterministic images.
$ mkimage --fw-version 0x00010000 --size 8192 --seed 1 -o v1.img
v1.img: fw_version=0x00010000 payload_len=8192 payload_crc=0xd48cbf4e total=8216

$ mkimage --fw-version 0x00010100 --size 12000 --seed 2 -o v2.img
v2.img: fw_version=0x00010100 payload_len=12000 payload_crc=0x29559baf total=12024

$ mkimage --fw-version 0x00010200 --size 9000 --seed 3 -o v3.img
v3.img: fw_version=0x00010200 payload_len=9000 payload_crc=0x41e68031 total=9024

# 1. Factory state: v1 confirmed in slot A.
$ fwsim init v1.img
init: factory image version 0x00010000 in slot A, CONFIRMED

$ fwsim boot
boot: slot A version 0x00010000 (confirmed)

# 2. Successful update: stage v2, trial-boot it, confirm it.
$ fwsim install v2.img
install: version 0x00010100 staged in slot B, PENDING

$ fwsim status
metadata:  seq=2 sector=META1 state=PENDING last_event=none
confirmed: slot A version 0x00010000
candidate: slot B version 0x00010100
slot A:    valid   version 0x00010000 payload_len=8192 crc=0xd48cbf4e [confirmed]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [candidate]

$ fwsim boot
boot: slot B version 0x00010100 (TRIAL, awaiting confirm)

$ fwsim confirm
confirm: slot B version 0x00010100 is now confirmed

$ fwsim boot
boot: slot B version 0x00010100 (confirmed)

# 3. Rollback: stage v3, trial-boot it, then reboot without confirming.
$ fwsim install v3.img
install: version 0x00010200 staged in slot A, PENDING

$ fwsim boot
boot: slot A version 0x00010200 (TRIAL, awaiting confirm)

$ fwsim boot
boot: slot B version 0x00010100 (confirmed; rolled back, trial was not confirmed)

$ fwsim status
metadata:  seq=7 sector=META0 state=CONFIRMED last_event=rolled-back
confirmed: slot B version 0x00010100
candidate: none
slot A:    valid   version 0x00010200 payload_len=9000 crc=0x41e68031 [inactive]
slot B:    valid   version 0x00010100 payload_len=12000 crc=0x29559baf [confirmed]

# 4. A truncated image is rejected; the confirmed image keeps booting.
$ fwsim install v3-truncated.img
install: rejected: image: truncated

$ fwsim boot
boot: slot B version 0x00010100 (confirmed)
```

## Tech Stack

| Language | Build System | Testing | Scripting | Sanitizers | CI | Version Control | Repository Hosting |
| :------: | :----------: | :-----: | :-------: | :--------: | :-: | :-------------: | :----------------: |
| ![C](https://img.shields.io/badge/C11-A8B9CC?logo=c&logoColor=white) | ![CMake](https://img.shields.io/badge/CMake-064F8C?logo=cmake&logoColor=white) | ![CTest](https://img.shields.io/badge/CTest-064F8C?logo=cmake&logoColor=white) | ![Python](https://img.shields.io/badge/Python-3776AB?logo=python&logoColor=white) | ![ASan/UBSan](https://img.shields.io/badge/ASan%20%2B%20UBSan-262D3A?logo=llvm&logoColor=white) | ![GitHub Actions](https://img.shields.io/badge/GitHub%20Actions-2088FF?logo=githubactions&logoColor=white) | ![Git](https://img.shields.io/badge/Git-F05032?logo=git&logoColor=white) | ![GitHub](https://img.shields.io/badge/GitHub-181717?logo=github&logoColor=white) |

## Equipment

| Tool                    | Model / Version                         | Purpose |
| ----------------------- | --------------------------------------- | ------- |
| **Development machine** | MacBook Pro (Apple M1, 8 GB RAM)        | Building, testing, fault sweep, and benchmarks |
| **Operating system**    | macOS 26.5.1                            | Host for all measured results |
| **Compiler**            | Apple clang 21.0.0                      | Debug, Release, and ASan + UBSan builds |
| **Build system**        | CMake 4.4.2 / CTest                     | Build configuration and test runner |
| **Python**              | Python 3.14.7                           | Image generation, CLI tests, fault sweep, benchmark |
| **Version control**     | Git 2.50.1                              | Source control |
| **CI runner**           | GitHub-hosted `ubuntu-latest`           | Linux GCC and Clang builds and tests (configured, not yet run) |

## Repository Structure

```text
Power_Fail-Safe_Firmware_Update_Simulator
├── .github                                  GitHub configuration
│   └── workflows
│       └── ci.yml                           Linux CI: GCC and Clang, Debug/Release, ASan + UBSan, tests, sweep, demo
├── cmake
│   └── check_core_portability.cmake         Fails the build's tests if the engine uses stdio, POSIX, or the heap
├── docs
│   └── ARCHITECTURE.md                      Flash map, formats, commit protocol, state machine, invariants, fault model
├── include                                  Public engine headers (portable)
│   └── fwu
│       ├── crc32.h                          CRC-32 API
│       ├── flash_if.h                       read / program / erase_sector interface
│       ├── flash_layout.h                   Flash map: the single source of truth for sizes and offsets
│       ├── image.h                          Image header format and verification
│       ├── meta.h                           Metadata record format and commit protocol
│       ├── status.h                         Result codes
│       └── update.h                         Update lifecycle API
├── src
│   ├── app
│   │   └── fwsim_main.c                     fwsim command-line tool
│   ├── core                                 Portable update engine (no file I/O, no heap)
│   │   ├── byteorder.h                      Explicit little-endian field access
│   │   ├── crc32.c                          CRC-32 implementation
│   │   ├── image.c                          Header encode/decode, buffer and slot verification
│   │   ├── meta.c                           Record encode/decode, selection, two-sector commit
│   │   ├── status.c                         Status, state, and event strings
│   │   └── update.c                         Factory init, streamed install, boot, confirm, rollback
│   └── sim                                  Desktop flash backend
│       ├── flash_sim.c                      File-backed NOR model with deterministic power-loss injection
│       └── flash_sim.h                      Simulator API
├── tests
│   ├── system                               Python system tests
│   │   ├── fault_sweep.py                   Power-loss sweep with recovery invariants R1–R6
│   │   ├── test_cli.py                      Multi-process CLI end-to-end tests
│   │   └── test_mkimage.py                  Image tool tests and C/Python constant sync
│   └── unit                                 C unit tests
│       ├── test_crc32.c                     CRC-32
│       ├── test_flash_sim.c                 Flash rules, persistence, fault injection
│       ├── test_image.c                     Image parsing, length rules, corruption sweeps
│       ├── test_meta.c                      Record validation, selection, interrupted commits
│       ├── test_update.c                    State-machine transitions
│       └── test_util.h                      Minimal assertion helpers
├── tools
│   ├── bench.py                             Per-command latency and flash-operation benchmark
│   ├── demo.sh                              Reproduces a successful update and a rollback
│   └── mkimage.py                           Deterministic firmware image generator
├── .gitignore
├── CMakeLists.txt                           Build, warning flags, sanitizer option, CTest registration
├── LICENSE                                  MIT License
└── README.md                                Project README
```

## Contact
- GitHub: [@devjamescollier](https://github.com/devjamescollier)
- Email: [jwcollie@usc.edu](mailto:jwcollie@usc.edu)

## License

Copyright © 2026 James Collier

This project is licensed under the MIT License. See [LICENSE](LICENSE).

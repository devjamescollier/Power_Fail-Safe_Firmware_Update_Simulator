# Architecture

This project simulates an A/B firmware update with a trial boot and automatic
rollback, on top of a file that behaves like NOR flash. The simulator reports
**which image would boot**. It never executes a payload.

CRC-32 is used only to detect accidental corruption, such as torn writes or
truncated files. It provides **no authenticity**. This project does not
implement secure boot or image signing.

---

## 1. Components

```
 +----------------------+     +----------------------------------+
 | fwsim CLI (src/app)  | --> | update engine (src/core)         |
 | file I/O for images, |     | image parse/verify, metadata,    |
 | argument parsing     |     | lifecycle state machine          |
 +----------------------+     +----------------+-----------------+
                                               | flash_dev_t
                                               | read / program / erase_sector
                              +----------------v-----------------+
                              | flash simulator (src/sim)        |
                              | file-backed NOR model,           |
                              | deterministic power-loss faults  |
                              +----------------------------------+
```

| Layer | Directory | May use |
|---|---|---|
| Update engine | `src/core`, `include/fwu` | `<stdint.h> <stdbool.h> <stddef.h> <string.h>` only. No stdio, no POSIX, no heap. |
| Flash simulator | `src/sim` | stdio file I/O |
| CLI | `src/app` | anything in the C standard library |

The `core_portability` CTest scans `src/core` and `include/fwu` and fails if
it finds stdio, stdlib, POSIX headers, or heap calls in either directory.

## 2. Flash model

The model follows NOR flash rules. The simulator enforces them, so an engine
bug causes a test failure.

- **Erased state:** an erased byte reads `0xFF`.
- **Program:** programming can only clear bits (1 to 0). Any attempt to set a
  bit (0 to 1) is rejected with `FLASH_ERR_PROGRAM_RULE`, and flash is left
  unchanged. Programming works at byte granularity.
- **Erase:** erase works on whole 4 KiB sectors at aligned addresses.
- **Durability:** a completed operation is durable. The model has no write
  cache.

## 3. Flash map

All geometry is defined in `include/fwu/flash_layout.h`. That file is the
single source of truth, and compile-time asserts check that every region is
sector-aligned.

| Region | Start | End (excl.) | Size | Sectors |
|---|---|---|---|---|
| META0 | `0x00000` | `0x01000` | 4 KiB | 0 |
| META1 | `0x01000` | `0x02000` | 4 KiB | 1 |
| SLOT A | `0x02000` | `0x12000` | 64 KiB | 2–17 |
| SLOT B | `0x12000` | `0x22000` | 64 KiB | 18–33 |

Total size is `0x22000` (139,264) bytes, or 34 sectors of 4,096 bytes.

Each slot stores an image exactly as it is laid out on disk: the header at
slot offset 0, followed by the payload.

## 4. Image format

All multi-byte fields are little-endian. Each field is serialized byte by byte
(`put_le32` / `get_le32`). No struct is ever copied to or from flash.

| Off | Size | Field | Value / rule |
|---|---|---|---|
| 0 | 4 | `magic` | `0x4D495746` (bytes `"FWIM"`) |
| 4 | 2 | `format_version` | `1` |
| 6 | 2 | `header_size` | `24` |
| 8 | 4 | `fw_version` | opaque, informational |
| 12 | 4 | `payload_len` | `1 ..= 65,512` (slot size − header size) |
| 16 | 4 | `payload_crc32` | CRC-32 of the payload bytes |
| 20 | 4 | `header_crc32` | CRC-32 of header bytes 0..19 |

The header CRC means a corrupted `payload_len` is caught before it is used to
size a read.

**Rejection rules.** Each rule maps to its own `fwu_status_t` code.

| Condition | Code |
|---|---|
| wrong magic | `FWU_ERR_IMG_MAGIC` |
| unknown `format_version` or `header_size` | `FWU_ERR_IMG_FORMAT` |
| header CRC mismatch | `FWU_ERR_IMG_HEADER_CRC` |
| `payload_len` is 0 | `FWU_ERR_IMG_FORMAT` |
| `payload_len` exceeds 65,512, or more bytes are supplied than `24 + payload_len` | `FWU_ERR_IMG_TOO_LARGE` |
| fewer bytes are supplied than `24 + payload_len` (including < 24) | `FWU_ERR_IMG_TRUNCATED` |
| payload CRC mismatch | `FWU_ERR_IMG_PAYLOAD_CRC` |
| slot verifies, but its `fw_version` or payload CRC differs from the metadata record (boot and confirm only) | `FWU_ERR_IMG_MISMATCH` |

**CRC-32 parameters:** reflected polynomial `0xEDB88320`, initial value
`0xFFFFFFFF`, final XOR `0xFFFFFFFF`. These are the same parameters as
Python's `zlib.crc32`. The check value for `"123456789"` is `0xCBF43926`, and
a unit test asserts it.

## 5. Metadata record

Each metadata sector holds at most one record, at sector offset 0.

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `0x444D5746` (bytes `"FWMD"`) |
| 4 | 2 | `layout_version` | `1` |
| 6 | 1 | `state` | `1`=CONFIRMED, `2`=PENDING, `3`=TRIAL |
| 7 | 1 | `last_event` | `0`=NONE, `1`=UPDATED, `2`=ROLLED_BACK, `3`=REJECTED (shown by `status` only) |
| 8 | 4 | `seq` | incremented by 1 on every commit |
| 12 | 1 | `confirmed_slot` | `0` (A) or `1` (B) |
| 13 | 1 | `candidate_slot` | other slot when PENDING/TRIAL, `0xFF` when CONFIRMED |
| 14 | 2 | `reserved` | `0x0000` |
| 16 | 4 | `confirmed_version` | `fw_version` of the confirmed image |
| 20 | 4 | `confirmed_crc` | `payload_crc32` of the confirmed image |
| 24 | 4 | `candidate_version` | `0` when there is no candidate |
| 28 | 4 | `candidate_crc` | `0` when there is no candidate |
| 32 | 4 | `record_crc` | CRC-32 of bytes 0..31 |
| 36 | 4 | `commit` | `0x54494D43` (bytes `"CMIT"`), programmed last |

State values deliberately avoid `0x00` and `0xFF`. An erased (`0xFF`) or
fully programmed (`0x00`) byte therefore cannot decode as a valid state.

The record stores the CRC of each image it refers to. Boot checks that the
image in the slot still matches that CRC. This catches a slot that was
rewritten after the record was committed.

## 6. Interrupted-write handling (metadata commit protocol)

The two metadata sectors take turns as an A/B pair. The newest valid record
is never touched while it is the newest.

**Commit of record N+1** (the current newest valid record is N, in sector S):

1. Set the target to the other sector, 1−S. If no record is valid, the target
   is sector 0.
2. **Erase** the target sector.
3. **Program** bytes 0..35 (the body, including `record_crc`) in one program
   operation.
4. **Program** bytes 36..39 (the commit marker) in a second program operation.
5. **Read back** the 40 bytes. They must match what was written and decode
   as a valid record.
6. Only then does the engine update its RAM copy and report success.

**A record is valid only if all of the following hold:**
- `magic` and `layout_version` match.
- `commit` equals `"CMIT"` exactly.
- `record_crc` matches.
- Every field is in range.

**Selection at startup:**
- If both records are valid, the one with the higher `seq` wins. The
  comparison uses serial-number arithmetic, so it still works after `seq`
  wraps past `0xFFFFFFFF`: `a` is newer when `(a − b) mod 2³²` is in
  `[1, 2³¹)`.
- If both have equal `seq`, sector 0 wins. The protocol never produces this
  case.
- If one record is valid, it wins.
- If none is valid, the result is `FWU_ERR_NO_METADATA`.

**What survives each interruption:**

| Power lost… | Target sector (1−S) | Sector S | Selected after reboot |
|---|---|---|---|
| before or during the erase (step 2) | older stale record, partially erased, or all `0xFF` | record N, untouched | N |
| during the body program (step 3) | partial body, marker still `0xFFFFFFFF` | N | N |
| after the body, before or during the marker (step 4) | full body, marker erased or torn (≠ `"CMIT"`) | N | N |
| after the marker | N+1, valid | N | **N+1** |

Every interruption therefore resolves to exactly N or N+1, never a mix of the
two. Any stale record left in the target sector always has `seq` < N, so it
cannot win.

**Why both a marker and a CRC?**
- **The CRC rejects partial or corrupted bodies,** but only with high
  probability. In about 1 case in 2³², a torn body could happen to match its
  CRC.
- **The marker covers that case by ordering, not probability.** It is
  programmed in its own operation, which starts only after the body program
  has returned. A torn body therefore never has a valid marker, whatever its
  CRC.

**What the tests can and cannot show.** Under the simulator's fault model,
every partial body already fails the magic or CRC check. The fault sweep
therefore cannot tell marker-last from marker-first, and both pass it. This
was verified by building a marker-first mutant. The marker is defense in depth
against a CRC collision, which no test can produce. Unit tests check that a
body without a complete marker is rejected (`tests/unit/test_meta.c`).

## 7. Update lifecycle

### States
| State | Meaning | Boots |
|---|---|---|
| CONFIRMED | No update in progress | `confirmed_slot` |
| PENDING | A verified candidate is staged in `candidate_slot` | next boot tries the candidate |
| TRIAL | The candidate has been booted once and is awaiting `confirm` | next boot rolls back |

### Transitions
| From | Event | Persistent writes, in order | To (`last_event`) |
|---|---|---|---|
| (none) | `init` (factory) | erase META0 and META1, erase and program slot A, verify, commit record seq=1 | CONFIRMED (NONE) |
| CONFIRMED | `install` OK | erase the inactive slot, program it, read back and verify, commit | PENDING |
| CONFIRMED | `install` rejected | inactive slot may hold garbage; **no metadata write** | CONFIRMED (unchanged) |
| PENDING | `install` | commit CONFIRMED (cancels the old candidate), then proceed as from CONFIRMED | PENDING or CONFIRMED |
| TRIAL | `install` | none, rejected with `FWU_ERR_STATE` | TRIAL |
| CONFIRMED | `boot` | none; verify the confirmed slot | CONFIRMED |
| PENDING | `boot`, candidate verifies | commit TRIAL **before** reporting the candidate | TRIAL |
| PENDING | `boot`, candidate fails verification | commit CONFIRMED, then boot the confirmed slot | CONFIRMED (REJECTED) |
| TRIAL | `boot` | commit CONFIRMED with the old confirmed slot, then boot it | CONFIRMED (ROLLED_BACK) |
| TRIAL | `confirm`, candidate verifies | commit CONFIRMED with `confirmed_slot` = candidate | CONFIRMED (UPDATED) |
| TRIAL | `confirm`, candidate fails verification | none, error returned | TRIAL (next boot rolls back) |
| CONFIRMED / PENDING | `confirm` | none, rejected with `FWU_ERR_STATE` | unchanged |

**Boot algorithm:**

```
load newest valid record (or fail NO_METADATA)
switch state:
  CONFIRMED: verify(confirmed_slot, confirmed_crc) ? boot confirmed : NO_BOOTABLE
  PENDING:   if verify(candidate_slot, candidate_crc):
                 commit TRIAL; report candidate (trial)
             else:
                 commit CONFIRMED/REJECTED; boot confirmed as above
  TRIAL:     commit CONFIRMED/ROLLED_BACK; boot confirmed as above
```

Here, `verify(slot, crc)` performs three checks:
1. It parses the header from flash, which includes the header CRC and length
   checks.
2. It recomputes the payload CRC in fixed-size chunks on the stack.
3. It requires that the image's `fw_version` and payload CRC equal the values
   stored in the record. A different but valid image in the slot fails with
   `FWU_ERR_IMG_MISMATCH`.

`confirm` runs the same check on the candidate before promoting it. If the
check fails, `confirm` returns the error, the state stays TRIAL, and the next
boot rolls back.

**Install streaming.** The CLI streams the file to the engine as received.
The engine does not pre-validate the whole file.
- `install_write` programs bytes as they arrive.
- It decodes the header once 24 bytes are in. From then on, it rejects any
  byte past `24 + payload_len` with `FWU_ERR_IMG_TOO_LARGE`, so it never
  writes outside the slot.
- `install_finish` reports a short stream as `FWU_ERR_IMG_TRUNCATED`.
- It then reads the entire slot back from flash and verifies it.
- Only after that does it commit the record.

## 8. Recovery invariants

These invariants are the design's safety argument. The fault sweep's checks
R1–R6 (`tests/system/fault_sweep.py`) test I1 and I3–I7 from the outside after
every injected interruption. I2 is tested directly by the metadata unit tests.

- **I1.** The sector holding the newest valid record is never erased or
  programmed.
- **I2.** A record counts only if its magic, layout version, CRC, commit
  marker, and every field check pass.
- **I3.** `install` never erases or programs the confirmed slot. It writes
  only `1 − confirmed_slot`, and only while the committed state is CONFIRMED,
  so no committed record names that slot as a candidate.
- **I4.** A PENDING record is committed only after the whole candidate has
  been read back from flash and verified.
- **I5.** TRIAL is committed before a candidate boot is reported. Any boot
  that finds TRIAL rolls back.
- **I6.** No slot is reported as booting unless it passes `verify`. An
  incomplete or corrupt image is therefore never selected.
- **I7.** At every interruption point, the newest valid record names a
  confirmed slot that holds a complete, valid image.

## 9. Fault model

- Power can be lost at any **fault point**. After a power loss, the process
  makes no further flash operations.
- Each valid erase or program contributes two fault points:
  - **(a) Partway.**
    - An erase leaves the first half of the sector set to `0xFF` and the
      second half unchanged.
    - A program of `n` bytes leaves the first `n/2` bytes programmed and the
      rest unchanged (a torn write).
  - **(b) After.** The operation completes durably, and then power is lost.
    The caller never learns that the operation succeeded.
- "After operation k" leaves flash in the same state as "before operation
  k+1" would. The only point not covered is "before the first operation".
  That point leaves flash identical to the starting snapshot. The sweep
  checks that the snapshot is in the expected "before" state but does not
  re-run the recovery checks on it.
- Calls that are rejected for range, alignment, or the 1-to-0 rule change
  nothing and use no fault points.
- After a power loss, every later flash call fails, reads included, as on a
  powered-off device.
- `--fail-at K` loses power at fault point K, counting from 0. A command that
  makes `N` flash operations has exactly `2N` points. The sweep runs
  K = 0, 1, 2, … until a run completes, so that K equals `2N`. The CLI also
  prints the count it passed, and the sweep checks that the two counts
  agree.
- After each interruption, the sweep starts **fresh processes** on the same
  flash file to check recovery (`tests/system/fault_sweep.py`).

## 10. Known limitations

- This is a desktop simulation. Nothing has been run on a microcontroller or
  real flash, and no timing or wear figures are claimed.
- The simulator models a torn program as a prefix of the data being written.
  On real NOR, the word in progress can be left in any intermediate bit
  pattern. The design rejects any partial pattern through the CRC and the
  exact marker check, but the sweep exercises only the prefix pattern.
- Bit rot and read disturb are not modeled. If the confirmed image itself
  becomes corrupt, boot reports `FWU_ERR_NO_BOOTABLE` and makes no attempt to
  boot from the other slot.
- The candidate gets one trial attempt. There is no boot counter and no
  watchdog. "The application confirms" is represented by the `confirm`
  command.
- There is no anti-rollback protection. `fw_version` is informational, so a
  downgrade is accepted.
- There is no authenticity check (see the CRC note at the top).
- Wear leveling is limited to alternating between the two metadata sectors.
  Boots in the CONFIRMED state write nothing. Each update costs a small,
  bounded number of record commits: PENDING, TRIAL, and then either
  CONFIRMED or ROLLED_BACK.
- `init` is a factory step. The CLI recreates the flash file and then
  provisions it. The engine also erases both metadata sectors before it
  writes slot A, so that an `init` on an existing device cannot leave a
  record that names a half-written slot.
  - In the sweep, every interrupted `init` left either an unprovisioned
    device (`FWU_ERR_NO_METADATA`, fixed by running `init` again) or a
    confirmed v1.
  - Running `init` with an image path that doesn't exist fails before flash
    is touched.

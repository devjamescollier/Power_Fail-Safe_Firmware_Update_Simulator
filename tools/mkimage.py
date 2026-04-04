#!/usr/bin/env python3
import argparse
import hashlib
import struct
import sys
import zlib

FWIMG_MAGIC = 0x4D495746
FWIMG_FORMAT_VERSION = 1
FWIMG_HEADER_SIZE = 24
FLASH_SECTOR_SIZE = 4096
FLASH_SLOT_SECTORS = 16
SLOT_SIZE = FLASH_SLOT_SECTORS * FLASH_SECTOR_SIZE
FWIMG_MAX_PAYLOAD = SLOT_SIZE - FWIMG_HEADER_SIZE

_HEADER_PREFIX = struct.Struct("<IHHIII")


def make_payload(length: int, seed: int) -> bytes:
    out = bytearray()
    counter = 0
    while len(out) < length:
        block = hashlib.sha256(b"fwsim-payload:%d:%d" % (seed, counter)).digest()
        out += block
        counter += 1
    return bytes(out[:length])


def encode_header(fw_version: int, payload: bytes) -> bytes:
    prefix = _HEADER_PREFIX.pack(
        FWIMG_MAGIC,
        FWIMG_FORMAT_VERSION,
        FWIMG_HEADER_SIZE,
        fw_version,
        len(payload),
        zlib.crc32(payload),
    )
    return prefix + struct.pack("<I", zlib.crc32(prefix))


def build_image(fw_version: int, payload_len: int, seed: int) -> bytes:
    if not 0 <= fw_version <= 0xFFFFFFFF:
        raise ValueError("fw_version must fit in 32 bits")
    if not 1 <= payload_len <= FWIMG_MAX_PAYLOAD:
        raise ValueError(f"payload length must be 1..{FWIMG_MAX_PAYLOAD}")
    payload = make_payload(payload_len, seed)
    return encode_header(fw_version, payload) + payload


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Create deterministic firmware images for the update simulator.")
    p.add_argument("--fw-version", type=lambda s: int(s, 0), required=True,
                   help="firmware version, e.g. 2 or 0x00010200")
    p.add_argument("--size", type=int, required=True,
                   help=f"payload length in bytes (1..{FWIMG_MAX_PAYLOAD})")
    p.add_argument("--seed", type=int, default=0, help="payload seed (default 0)")
    p.add_argument("-o", "--output", required=True)
    args = p.parse_args(argv)

    try:
        image = build_image(args.fw_version, args.size, args.seed)
    except ValueError as e:
        p.error(str(e))
    with open(args.output, "wb") as f:
        f.write(image)
    print(f"{args.output}: fw_version=0x{args.fw_version:08x} payload_len={args.size} "
          f"payload_crc=0x{zlib.crc32(image[FWIMG_HEADER_SIZE:]):08x} total={len(image)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

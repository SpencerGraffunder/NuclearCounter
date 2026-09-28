#!/usr/bin/env python3
"""Assert that a partitions.bin has an ota_1/app1 partition at an expected offset.

Usage: check_ota1.py <partitions.bin> <expected-offset-hex> <label>

Exits non-zero (and prints a clear message) if the ota_1 partition is missing
or at the wrong offset. This guards against packaging the wrong chip's
partition table (e.g. flashing an S3 table, ota_1 @ 0x1A0000, onto a C3 board
whose real ota_1 is @ 0x150000).

The ESP32 partition magic 0xAA50 is stored as the two bytes 0xAA 0x50, which
reads back as 0x50AA little-endian — so we match the raw bytes, not a u16.
"""
import struct
import sys


def find_ota1(path):
    d = open(path, "rb").read()
    for off in range(0, min(len(d), 160), 32):
        if d[off] != 0xAA or d[off + 1] != 0x50:  # magic bytes 0xAA 0x50
            break
        offset = struct.unpack("<I", d[off + 4 : off + 8])[0]
        label = d[off + 12 : off + 28].split(b"\0")[0].decode()
        if label in ("ota_1", "app1"):
            return offset, label
    return None, None


def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <partitions.bin> <expected-offset-hex> <label>", file=sys.stderr)
        return 2
    path, expect_s, label = sys.argv[1], sys.argv[2], sys.argv[3]
    expect = int(expect_s, 16)
    got, name = find_ota1(path)
    if got is None:
        print(f"FAIL {label}: no ota_1/app1 partition in {path}", file=sys.stderr)
        return 1
    if got != expect:
        print(f"FAIL {label}: {name} at 0x{got:x}, expected 0x{expect:x}", file=sys.stderr)
        return 1
    print(f"  OK {label}: {name} @ 0x{got:x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

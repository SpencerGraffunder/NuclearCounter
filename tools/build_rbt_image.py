#!/usr/bin/env python3
"""Build a "crash image" from a good ESP32-C3 firmware image.

The output image is byte-for-byte the good image except that the FIRST
INSTRUCTION AT THE ENTRY POINT is replaced with an illegal RISC-V encoding
(0x00000001), and the image footer (rolling checksum byte + SHA-256
validation digest) is recomputed. The result passes full image validation
(header, segment table, checksum, SHA-256) but traps on the very first
instruction after the bootloader jumps to it.

Used by the DEBUG_ROLLBACK_TEST build (env:RBTC3) to prove the bootloader's
automatic OTA rollback end-to-end: the debug firmware streams this image
into ota_1 over USB CDC, switches the boot slot to ota_1 (PENDING_VERIFY),
reboots -> crash -> bootloader reverts to ota_0.

Usage: build_rbt_image.py <good.bin> <out.bin>

Image format (ESP-IDF v1, as written by esptool):
  [0:1]   magic 0xE9
  [1:2]   segment count
  [2:3]   flash mode
  [3:4]   flash size/freq
  [4:8]   entry point (LE u32)
  [8:24]  extended header (16 B); byte [23] = append_digest flag (1)
  then per segment: [8 B header: addr LE u32, size LE u32] + data
  then: 1 B rolling checksum, then (if append_digest) 32 B SHA-256.

Checksum: XOR of every byte of every NON-FLASH-MAPPED segment, starting
from 0xEF, in file order. (Flash-mapped = IROM/DROM regions.)
Digest: SHA-256 of the whole file up to and including the checksum byte.
"""
import hashlib
import struct
import sys

ESP_MAGIC = 0xE9
ESP_CHECKSUM_MAGIC = 0xEF

# ESP32-C3 flash-mapped regions (IROM/DROM). Segments loaded here are
# excluded from the rolling checksum.
C3_IROM_MAP = (0x42000000, 0x44000000)
C3_DROM_MAP = (0x3C000000, 0x3E000000)


def parse_segments(data, nsegs):
    """Return list of (addr, size, data_offs) where data_offs is the file
    offset of the segment's DATA (after its 8-byte header)."""
    segs = []
    pos = 24  # common header (8) + extended header (16)
    for _ in range(nsegs):
        addr, size = struct.unpack_from("<II", data, pos)
        segs.append((addr, size, pos + 8))
        pos = pos + 8 + size
    return segs


def rolling_checksum(data, segs):
    # XOR over EVERY segment's data, in file order, starting from 0xEF.
    # (Verified against the good image: the stored footer byte equals this
    #  over all 6 segments, including the flash-mapped DROM/IROM ones. The
    #  esptool load path marks every segment include_in_checksum=True.)
    state = ESP_CHECKSUM_MAGIC
    for addr, size, off in segs:
        for b in data[off:off + size]:
            state ^= b
    return state & 0xFF


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src_path, dst_path = sys.argv[1], sys.argv[2]
    d = bytearray(open(src_path, "rb").read())
    L = len(d)

    if d[0] != ESP_MAGIC:
        sys.exit(f"bad magic {d[0]:#04x}")
    nsegs = d[1]
    entry = struct.unpack_from("<I", d, 4)[0]
    append_digest = d[23]  # extended-header byte 15
    print(f"entry={entry:#x} nsegs={nsegs} append_digest={append_digest} len={L}")

    if not append_digest:
        sys.exit("image has no appended digest; expected a modern esptool image")

    segs = parse_segments(d, nsegs)
    image_length = L - 32  # everything up to and incl. the checksum byte

    # --- verify the model against the ORIGINAL footer ---
    stored_csum = d[image_length - 1]
    calc_csum = rolling_checksum(d, segs)
    print(f"checksum stored={stored_csum:#04x} calc={calc_csum:#04x} "
          f"{'OK' if stored_csum == calc_csum else 'MISMATCH'}")
    stored_hash = bytes(d[image_length:L])
    calc_hash = hashlib.sha256(d[:image_length]).digest()
    print(f"sha256 {'OK' if stored_hash == calc_hash else 'MISMATCH'}")
    if stored_csum != calc_csum or stored_hash != calc_hash:
        sys.exit("footer model is wrong; aborting")

    # --- find the segment containing the entry point ---
    target = None
    for addr, size, off in segs:
        if addr <= entry < addr + size:
            target = (addr, size, off)
            break
    if target is None:
        sys.exit(f"entry {entry:#x} not inside any segment")
    t_addr, t_size, t_off = target
    patch_off = t_off + (entry - t_addr)
    print(f"patching entry instruction at file offset {patch_off:#x} "
          f"(seg {t_addr:#x} len {t_size:#x})")
    print(f"old bytes: {d[patch_off:patch_off+4].hex(' ')}")

    # 0x00000001: opcode 0b001 (custom-0), not implemented on the C3 ->
    # illegal-instruction trap on the first instruction executed.
    d[patch_off:patch_off + 4] = b"\x01\x00\x00\x00"

    # --- recompute footer ---
    d[image_length - 1] = rolling_checksum(d, segs)
    d[image_length:L] = hashlib.sha256(d[:image_length]).digest()

    open(dst_path, "wb").write(d)
    print(f"wrote {dst_path} ({L} bytes)")
    print("verify with: esptool --chip esp32c3 image-info " + dst_path)


if __name__ == "__main__":
    main()

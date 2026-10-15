#!/usr/bin/env python3
"""Robust C3 OTA-rollback cycle capture.

Survives the USB device disappearing during the ota_1 crash loop by
re-opening the port whenever it drops. Captures the full sequence:

    RBT -> boot ota_1 -> crash (Guru Meditation) [xN] -> recovery -> boot ota_0

and reports how many crash boots happened before the board settled back on
ota_0. Everything is echoed and saved to /tmp/rbt_cycle.log.
"""
import sys
import time

import serial

PORT = "/dev/cu.usbmodem1101"
LOG = "/tmp/rbt_cycle.log"


def open_stable():
    for _ in range(8):
        try:
            s = serial.Serial(PORT, 115200, timeout=0)
            s.dtr = False
            s.rts = False
            time.sleep(0.3)
            return s
        except Exception:
            time.sleep(1.0)
    return None


def reset_into_app(s):
    s.dtr = False
    s.rts = False
    time.sleep(0.3)
    s.rts = False
    time.sleep(0.2)
    s.rts = True
    time.sleep(0.5)


def log(logf, tag, t):
    print(f"[{tag}] {t}", flush=True)
    if logf is not None:
        logf.write(f"[{tag}] {t}\n")
        logf.flush()


def main():
    logf = open(LOG, "w")
    t_start = time.time()
    s = open_stable()
    if not s:
        print("could not open port"); sys.exit(2)

    # Phase 1: clean reset, confirm we are on ota_0.
    s.reset_input_buffer()
    log(None, "setup", "resetting into app ...")
    reset_into_app(s)
    end = time.time() + 12
    buf = b""
    saw_ota0 = False
    while time.time() < end:
        try:
            if s.in_waiting:
                buf += s.read(s.in_waiting)
                while b"\n" in buf:
                    l, buf = buf.split(b"\n", 1)
                    t = l.decode(errors="replace").rstrip("\r")
                    if t.strip():
                        log(logf, "boot", t)
                    if "Boot slot: ota_0" in t:
                        saw_ota0 = True
        except OSError:
            s = open_stable()
            if not s:
                break
            buf = b""
    if not saw_ota0:
        log(logf, "setup", "!! not on ota_0 after reset; aborting")
        sys.exit(2)
    log(logf, "setup", "confirmed ota_0; sending RBT")

    # Phase 2: trigger RBT, then capture the whole crash+recovery cycle.
    try:
        s.reset_input_buffer()
        s.write(b"RBT\n")
        s.flush()
    except OSError:
        s = open_stable()
        s.write(b"RBT\n")
        s.flush()

    crashes = 0
    ota0_boots = 0
    last_ota0 = 0
    stable_for = 0.0
    end = time.time() + 90
    buf = b""
    while time.time() < end:
        # Stop early once we've seen an ota_0 boot and 12 s of calm.
        if ota0_boots and (time.time() - last_ota0) > 12:
            log(logf, "cycle", "12 s calm after last ota_0 boot -> RECOVERED")
            break
        try:
            if s.in_waiting:
                buf += s.read(s.in_waiting)
                while b"\n" in buf:
                    l, buf = buf.split(b"\n", 1)
                    t = l.decode(errors="replace").rstrip("\r")
                    if not t.strip():
                        continue
                    log(logf, "cycle", t)
                    if "Guru Meditation" in t:
                        crashes += 1
                        log(logf, "cycle", f"  >>> crash #{crashes}")
                    if "Boot slot: ota_0" in t:
                        ota0_boots += 1
                        last_ota0 = time.time()
            else:
                time.sleep(0.02)
        except OSError:
            log(logf, "cycle", "  (port dropped; reopening ...)")
            s = open_stable()
            if not s:
                time.sleep(1.0)
                s = open_stable()
            buf = b""

    log(logf, "result", f"crash boots seen: {crashes}; ota_0 boots seen: {ota0_boots}")
    if ota0_boots:
        log(logf, "result", "PASS: board recovered to ota_0")
    else:
        log(logf, "result", "FAIL: no ota_0 boot observed")
    if s:
        s.close()
    logf.close()
    print(f"Log: {LOG}")
    sys.exit(0 if ota0_boots else 3)


if __name__ == "__main__":
    main()

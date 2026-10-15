#!/usr/bin/env python3
"""Drive the C3 OTA-rollback test over USB CDC and capture everything.

Sequence:
  1. Open the port, hard-reset the chip into APP mode (RTS pulse, the exact
     sequence esptool's HardReset uses for the C3 internal USB peripheral).
  2. Read ~12 s of boot output; confirm "Boot slot: ota_0" (the RBTC3 debug
     firmware must be running from ota_0 before we trigger anything).
  3. Send "RBT". The firmware calls esp_ota_set_boot_partition(ota_1) and
     reboots. The bootloader boots the crash image in ota_1, it traps on its
     first instruction, crash-loops, and the bootloader reverts otadata to
     backup=ota_0 and boots ota_0 again.
  4. Read ~75 s more. The crash loop + rollback are mostly silent on USB
     (bootloader/panic log to UART0, not USB-JTAG), but the SECOND clean boot
     of ota_0 is the success marker.

Everything is echoed to stdout and saved to /tmp/rbt_test.log.
"""
import sys
import time

import serial

PORT = "/dev/cu.usbmodem1101"
LOG = "/tmp/rbt_test.log"


def hard_reset_into_app(ser):
    # esptool HardReset for the C3 internal USB peripheral:
    #   RTS low (EN low) 0.2 s -> RTS high (EN high) -> re-enumerate.
    # Stabilize DTR/RTS to the app-boot (non-reset) state FIRST, then pulse EN.
    # Skipping the stabilize step is why an earlier attempt produced no output:
    # the open-time DTR state left EN in the wrong place for the pulse to bite.
    ser.dtr = False
    ser.rts = False
    time.sleep(0.3)
    ser.rts = False
    time.sleep(0.2)
    ser.rts = True
    time.sleep(0.5)


def read_lines(ser, duration, logf, tag):
    """Read for `duration` seconds, echo + log complete lines. Return lines."""
    end = time.time() + duration
    buf = b""
    lines = []
    # NOTE: do NOT reset_input_buffer() here. The caller clears it BEFORE the
    # reset pulse; clearing it here would wipe the boot banner that arrives
    # within ~0.2 s of the reset (faster than we get to this function).
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                txt = line.decode(errors="replace").rstrip("\r")
                if txt.strip():
                    print(f"[{tag}] {txt}", flush=True)
                    logf.write(f"[{tag}] {txt}\n")
                    logf.flush()
                    lines.append(txt)
        else:
            time.sleep(0.02)
    if buf.strip():
        txt = buf.decode(errors="replace").rstrip("\r")
        print(f"[{tag}] {txt}", flush=True)
        logf.write(f"[{tag}] {txt}\n")
        logf.flush()
        lines.append(txt)
    return lines


def main():
    ser = serial.Serial(PORT, 115200, timeout=0)
    logf = open(LOG, "w")
    logf.write("=== C3 OTA rollback test ===\n")

    # Clear any stale bytes BEFORE the reset pulse so the capture window is
    # clean; the banner that follows the reset must NOT be wiped.
    ser.reset_input_buffer()
    print("Resetting board into app mode ...", flush=True)
    hard_reset_into_app(ser)

    print("Reading boot output (15 s) ...", flush=True)
    boot = read_lines(ser, 15, logf, "boot")
    boot_text = "\n".join(boot)

    on_ota0 = "Boot slot: ota_0" in boot_text
    print(f"\n>>> board booted from ota_0: {on_ota0}\n", flush=True)
    logf.write(f">>> board booted from ota_0: {on_ota0}\n")

    if not on_ota0:
        print("!! Not running from ota_0 — aborting before trigger.", flush=True)
        logf.write("!! not ota_0, aborting\n")
        ser.close()
        logf.close()
        sys.exit(2)

    print("Sending RBT ...", flush=True)
    logf.write(">>> sending RBT\n")
    ser.write(b"RBT\n")
    ser.flush()

    print("Watching for crash-loop + rollback + ota_0 reboot (75 s) ...", flush=True)
    after = read_lines(ser, 75, logf, "after")
    after_text = "\n".join(after)

    rebooted_ota0 = "Boot slot: ota_0" in after_text
    print(f"\n>>> second ota_0 boot seen after RBT: {rebooted_ota0}\n", flush=True)
    logf.write(f">>> second ota_0 boot seen after RBT: {rebooted_ota0}\n")

    ser.close()
    logf.close()
    print(f"Log saved to {LOG}")
    sys.exit(0 if rebooted_ota0 else 3)


if __name__ == "__main__":
    main()

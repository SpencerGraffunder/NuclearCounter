#!/usr/bin/env bash
#
# flash.sh — flash a NuclearCounter + StarForgeOS dual-boot build.
#
# This script ships inside the CI artifact next to the .bin files. It reads
# `manifest.txt` (in the same directory) for the chip and the StarForgeOS
# ota_1 / UI offsets, then flashes every file with esptool:
#
#     bootloader.bin         0x0
#     partitions.bin         0x8000
#     nuclearcounter-ota0.bin  ota_0  (NuclearCounter)
#     starforge-ota1.bin     ota_1  (StarForgeOS)
#     nuclearcounter-ui.bin   spiffs (NuclearCounter INTEGRATED web UI, v3.0 packages)
#     hertzhunter-ui.bin     spiffs (same UI, older v3.0 packages)
#     starforge-ui.bin       spiffs (StarForgeOS web UI, v2.1 C3 package)
#
# Usage:
#   ./flash.sh                 # auto-detect the serial port
#   ./flash.sh /dev/ttyACM0    # or pass the port explicitly
#
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

[ -f manifest.txt ] || { echo "ERROR: manifest.txt not found in $DIR" >&2; exit 1; }

# ---------------------------------------------------------------- manifest
CHIP=""; OTA1_OFF=""; UI_OFF=""
while IFS='=' read -r k v; do
  case "$k" in
    CHIP)     CHIP="$v" ;;
    OTA1_OFF) OTA1_OFF="$v" ;;
    UI_OFF)   UI_OFF="$v" ;;
  esac
done < manifest.txt

[ -n "$CHIP" ]     || { echo "ERROR: manifest.txt missing CHIP" >&2; exit 1; }
[ -n "$OTA1_OFF" ] || { echo "ERROR: manifest.txt missing OTA1_OFF" >&2; exit 1; }
[ -n "$UI_OFF" ]   || { echo "ERROR: manifest.txt missing UI_OFF" >&2; exit 1; }

# ------------------------------------------------------------------- files
for f in bootloader.bin partitions.bin nuclearcounter-ota0.bin starforge-ota1.bin; do
  [ -f "$f" ] || { echo "ERROR: missing $f" >&2; exit 1; }
done
# Shared spiffs UI image: nuclearcounter-ui.bin (NC INTEGRATED build, v3.0
# packages), then hertzhunter-ui.bin (same UI under its old name), then
# starforge-ui.bin (SFOS build, v2.1 C3 package)
UI_BIN=""
[ -f nuclearcounter-ui.bin ] && UI_BIN="nuclearcounter-ui.bin"
[ -z "$UI_BIN" ] && [ -f hertzhunter-ui.bin ] && UI_BIN="hertzhunter-ui.bin"
[ -z "$UI_BIN" ] && [ -f starforge-ui.bin ] && UI_BIN="starforge-ui.bin"
[ -n "$UI_BIN" ] || { echo "ERROR: missing UI image (nuclearcounter-ui.bin, hertzhunter-ui.bin or starforge-ui.bin)" >&2; exit 1; }

# --------------------------------------------------------------------- port
PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.SLAB* /dev/ttyACM* /dev/ttyUSB* /dev/cu.wchusbserial* 2>/dev/null | head -n1 || true)"
fi
if [ -z "$PORT" ]; then
  echo "ERROR: no ESP32 serial port found." >&2
  echo "       Connect the board and pass the port explicitly:  ./flash.sh /dev/ttyACM0" >&2
  exit 1
fi

echo "============================================================"
echo " Chip        : $CHIP"
echo " Port        : $PORT"
echo " ota_0 (NC)  : 0x10000"
echo " ota_1 (SFOS): $OTA1_OFF"
echo " UI (spiffs) : $UI_OFF"
echo "============================================================"

# ------------------------------------------------------------------ esptool
if ! command -v esptool >/dev/null 2>&1; then
  if [ -x "$HOME/.local/bin/esptool" ]; then
    export PATH="$HOME/.local/bin:$PATH"
  else
    echo "esptool not found — installing..."
    python3 -m pip install --quiet --user esptool pyserial
    export PATH="$HOME/.local/bin:$PATH"
  fi
fi
command -v esptool >/dev/null 2>&1 || { echo "ERROR: esptool still not available" >&2; exit 1; }

esptool --chip "$CHIP" --port "$PORT" --baud 921600 \
  --before default-reset --after hard-reset \
  write_flash \
  0x0      bootloader.bin \
  0x8000   partitions.bin \
  0x10000  nuclearcounter-ota0.bin \
  "$OTA1_OFF" starforge-ota1.bin \
  "$UI_OFF"   "$UI_BIN"

echo ""
echo "Done. The board boots NuclearCounter (ota_0) after a fresh flash."
echo "V2.1 builds switch to StarForgeOS from the menu (Advanced > StarForge), and"
echo "StarForgeOS switches back with 'Boot Scanner Mode'. The V3.0 INTEGRATED build"
echo "has no menu switch (scanner + USB node + WiFi timer in one app); to boot the"
echo "StarForgeOS slot set otadata with esptool or re-flash that slot."

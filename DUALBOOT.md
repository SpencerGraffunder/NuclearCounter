# Dual Boot: NuclearCounter + StarForgeOS

The NuclearCounter board (V2.1, V3.0) can run **two independent firmwares** on
the same board, switchable from each app's menu, with the selection surviving
power cycles:

- **NuclearCounter** (this repo) — 5.8GHz scanner
- **StarForgeOS** ([SpencerGraffunder/StarForgeOS](https://github.com/SpencerGraffunder/StarForgeOS)) — drone race timing (standalone Wi-Fi or RotorHazard USB node)

Both apps live in separate OTA slots on the same 4MB flash. No reflashing and
no mode pins needed to switch.

## How it works

Flash layout (see `partitions.csv`):

| Partition | Offset  | Size      | Contents                  |
|-----------|---------|-----------|---------------------------|
| `nvs`     | 0x9000  | 20 KB     | settings (per-app)        |
| `otadata` | 0xE000  | 8 KB      | boot slot selection state |
| `ota_0`   | 0x10000 | 1568 KB   | **NuclearCounter**        |
| `ota_1`   | 0x1A0000| 1568 KB   | **StarForgeOS**           |
| `spiffs`  | 0x330000| 800 KB    | StarForgeOS web UI        |

That's the S3 (V3.0) layout. The C3 (V2.1) board uses the same labels with
smaller app slots: `ota_1` @ 0x150000 (1280 KB) and `spiffs` @ 0x290000
(1408 KB) — see `partitions_dualboot_c3.csv` in the StarForgeOS fork.

- Each app finds the *other* app's slot with
  `esp_partition_find_first(..., OTA_1/OTA_0, ...)` and switches with
  `esp_ota_set_boot_partition()` + `esp_restart()`.
- The ESP32 bootloader reads `otadata` at every boot, so the last-selected app
  boots after any power cycle.
- Both apps write to `nvs`/`spiffs` at the same offsets — that's fine because
  the boot partition selection is what determines which app owns them.

> Note: switching apps does **not** back up the other app's `nvs` settings.
> Each app keeps its own settings; they coexist in the same partition area.

## ⚠️ Critical: both apps must be built for the same flash size

The bench board has **4 MB** flash. **Both** apps must be built for 4 MB.
If one app is built for 8 MB (the ESP32-S3 default), `esp_ota_set_boot_partition()`
**fails with `ESP_ERR_OTA_VALIDATE_FAILED` (0x1503)** when that app tries to
switch to the other slot — the 8 MB runtime flash configuration breaks the
bootloader's image validation of the other partition. The switch in the
*other* direction still works, so it looks like a one-way boot failure.

This is set per-app by the board definition's `upload.flash_size`, **not** by
`board_build.flash_size` in `platformio.ini` (which is ignored when a board
file is present):

- **NuclearCounter** — `boards/esp32-s3-devkitc-1-4MB.json` (4 MB). ✓
- **StarForgeOS** — `boards/esp32-s3-devkitc-1-4MB.json` (4 MB), selected via
  `board = esp32-s3-devkitc-1-4MB` in `[env:nuclearcounter_s3]`. ✓

If you ever see `ESP_ERR_OTA_VALIDATE_FAILED` when switching, check both
apps' `esptool image-info firmware.bin | grep -i 'flash size'` — they must
match the board (4 MB).

Verify after building:

```sh
esptool.py image-info .pio/build/<env>/firmware.bin | grep -i 'flash size'
# => Flash size: 4MB
```

## Flashing

> [!TIP]
>
> **Easiest path:** run the **Build firmware (v2.1 C3 + v3.0 S3)** GitHub Action in this repo, download the `nuclearcounter-v2.1-c3` / `nuclearcounter-v3.0-s3` artifact, and run `flash.sh` inside it. See [README → Building & flashing from CI](README.md#building--flashing-from-ci-recommended). The manual steps below are what that artifact does for you.

Build/flash NuclearCounter first (this installs the bootloader, partition table
and app):

```sh
# V3.0 (ESP32-S3)
pio run -e NuclearCounterV3_0 -t upload

# V2.1 (ESP32-C3)
pio run -e NuclearCounterV2_1 -t upload
```

> [!WARNING]
>
> The C3 env in this repo ships the **S3** partition table (a historical
> quirk). For a C3 board the table must be the C3 one (`ota_1` @ 0x150000) —
> the StarForgeOS C3 build provides it (`partitions_dualboot_c3.csv`), and the
> CI artifact handles this automatically. The simplest path for C3 boards is
> the CI artifact above.

Then build + flash StarForgeOS from the fork
([SpencerGraffunder/StarForgeOS](https://github.com/SpencerGraffunder/StarForgeOS),
branch `main`). PlatformIO always uploads apps to the `ota_0`
offset, so the fork ships a helper that writes the app to
`ota_1` (0x1A0000 on S3, 0x150000 on C3) via esptool:

```sh
cd StarForgeOS
pio run -e nuclearcounter        # C3
# or: pio run -e nuclearcounter_s3   # S3
./flash_dualboot.sh /dev/cu.usbmodemXXXX
```

The StarForgeOS envs use `partitions_dualboot.csv`, which must stay **byte
identical** to this repo's `partitions.csv`.

> Both apps must be flashed with the *same* partition table. If a flash
> session fails halfway, the safest recovery is a full re-flash from the
> NuclearCounter side (always writes bootloader + table + app):
>
> ```sh
> pio run -e <env> -t upload
> ```
> then re-run `flash_dualboot.sh`.

## Switching apps

### NuclearCounter → StarForge

Main menu → hold `SEL` (hidden **Advanced** submenu) → ⭐ **StarForge** → confirm.

### StarForge → NuclearCounter

OLED menu (available in both standalone and USB node modes) →
**Boot Scanner Mode** → select to confirm.

## Buttons (StarForge standalone menu)

| Button  | Pin (C3/S3) | Function        |
|---------|-------------|-----------------|
| Prev    | 21          | menu up         |
| Next    | 10          | menu down       |
| Select  | 20          | activate / confirm |

Same wiring and behavior as the NuclearCounter firmware (active-high with
`INPUT_PULLDOWN`).

## Troubleshooting

- **`ESP_ERR_OTA_VALIDATE_FAILED` when switching (one-way boot failure):** the
  flash-size mismatch. The app you're switching *from* was built for a different
  flash size than the board (e.g. 8 MB default vs 4 MB board). Rebuild it for
  4 MB and re-flash both apps (see ⚠️ note above). This is the single most
  common cause of "StarForgeOS can't boot NuclearCounter" / vice-versa.
- **Black screen after switching:** the app in the other slot is missing or
  corrupt. Re-flash it (see above).
- **Wrong chip error on upload:** check you're flashing the right env for the
  connected board (C3 vs S3).
- **`otadata` out of sync / boot loop:** erase flash and re-flash:
  `esptool.py --port <port> erase_flash` then flash both apps.

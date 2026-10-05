# NuclearCounter

A poor-man's [RF Explorer](https://j3.rf-explorer.com/) for FPV drones — a cheap, DIY 5.8GHz spectrum scanner that shows which frequencies are in use, where background noise is occurring, and helps diagnose malfunctioning video transmitters (VTXs). Designed to be cheap (<$60 AUD) and easy to build yourself.

The NuclearCounter board also **dual-boots** [StarForgeOS](https://github.com/RaceFPV/StarForgeOS) (a drone race timing system), so one board is both a scanner and a race timer.

*Example of a soldered prototype*

<div align="center">
    <img src="./images/Device example.jpg" alt="Device example" width="40%" />
    <img src="./images/Scan example.jpg" alt="Scan example" width="40%" />
</div>

## Contents

1. [Introduction](#introduction)
2. [Fork & credits](#fork--credits)
3. [Features](#features)
    - [Potential future features](#potential-future-features)
4. [Hardware](#hardware)
    - [Components](#components)
    - [Wiring](#wiring)
5. [Software](#software)
    - [Environment setup](#environment-setup)
    - [Building & flashing](#building--flashing)
    - [Building & flashing from CI (recommended)](#building--flashing-from-ci-recommended)
    - [Dual boot with StarForgeOS](#dual-boot-with-starforgeos)
    - [Battery calibration](#battery-calibration)
6. [Usage](#usage)
    - [Menus](#menus)
    - [Scanning](#scanning)
    - [Wi-Fi hotspot](#wi-fi-hotspot)
    - [RSSI calibration](#rssi-calibration)
    - [Resetting](#resetting)

## Introduction

At a racing event I attended there was an issue with someone's damaged VTX broadcasting at full power on two channels, thus interfering with another pilot. A spectrum analyser was essential for diagnosing this issue, as two peaks at different frequencies could be seen in the spectrum graph when only the damaged VTX was powered on.

This project aims to make this useful tool more accessible to pilots and race organisers, and can be easily added to a race-day tool bag. It uses a common RX5808 video receiver to scan from 5645MHz to 5945MHz (and 5345MHz to 5645MHz for low-band channels) and displays a graph of the received signal strength (RSSI) on different frequencies within this range on a small OLED display.

## Fork & credits

The NuclearCounter firmware is a fork of [Hertz Hunter](https://github.com/odddollar/Hertz-Hunter) by **Simon Eason** (odddollar). All of the original functionality and design is retained, and credit for the original project remains with its author.

Changes made in this fork:

| Area | Change |
|---|---|
| Build environment | Migrated from the Arduino IDE to PlatformIO |
| Boards | Build targets for the NuclearCounter **V2.1** (ESP32-C3) and **V3.0** (ESP32-S3) hardware |
| Dual boot | The board can also run [StarForgeOS](https://github.com/RaceFPV/StarForgeOS) (drone race timing) from a second OTA slot, with boot switching from either firmware's menu — see [Dual boot with StarForgeOS](#dual-boot-with-starforgeos) |
| Wi-Fi | AP renamed to `NuclearCounter` (password `nuclearcounter`), moved to the `192.168.8.x` subnet, and a TX-power fix added so the S3's AP beacon transmits reliably |
| CI | A GitHub Action builds **both** firmwares and packages a flash-ready artifact per board — see below |

## Features

- Scanning of the RF spectrum commonly used for video by FPV racing drones (5645MHz to 5945MHz) and additional low-band (5345MHz to 5645MHz) frequencies
- Graphing RSSI to show which frequencies VTXs are broadcasting on
- Three buttons (`PREV`, `SEL`, `NEXT`) for navigating menus and controlling the device
- Selectable scanning interval
    - A 5MHz interval offers the highest resolution at the slowest update rate
    - A 10MHz interval offers a medium resolution at a medium update rate
    - A 20MHz interval offers the lowest resolution at the fastest update rate
- Battery voltage monitoring with a low battery alarm
- Calibration between known low and high RSSI values
- Displaying calibrated signal strength for the selected frequency
- Settings saved between reboots
- API accessible from a Wi-Fi hotspot for integration with other software ([Documentation](API.md))
- **Dual boot with StarForgeOS** — a race-timing firmware on the same board, switchable from either menu ([details](#dual-boot-with-starforgeos))

### Potential future features

> [!NOTE]
>
> No commitment is made to implementing these. They're things I think would be cool to do, but may never actually see the light of day.

- Custom PCB with integrated power management circuitry
- 3D printed case for a custom PCB
- Web interface to interact with the scanner and display more detailed graphs

## Hardware

### Components

These components can be connected together on a bread-board or soldered more permanently onto some type of perf-board. All prices are in Australian dollars (AUD).

- 1x [ESP32-C3 Super Mini](https://www.aliexpress.com/w/wholesale-esp32-c3-super-mini.html) (<$5)
- 1x [RX5808 with SPI mod](https://www.aliexpress.com/w/wholesale-rx5808-spi.html) (\$25 to \$50 depending on the seller)
- 1x [1.3" I<sup>2</sup>C 128x64 OLED](https://www.aliexpress.com/w/wholesale-1.3-oled.html) (<$5)
    - I use an OLED with the SH1106 controller chip, but the SSD1306 chip *should* work as well. The modifications that need to be made to the source code are explained at the end of [Building & flashing](#building--flashing)
- 1x [Active 3.3V buzzer](https://www.aliexpress.com/w/wholesale-active-buzzer.html) (<$3)
- 1x [TP4056 lithium battery charger module](https://zaitronics.com.au/products/tp4056-type-c-18650-lithium-battery-charger-protection) (<$2)
- 1x [5V boost converter](https://zaitronics.com.au/products/mt3608-step-up-module) (<$3)
    - If using an adjustable boost converter, set the output to 5V using a multimeter. Lock the potentiometer in place with a dab of super glue
- 3x Momentary buttons
- 2x 100kΩ resistors
- 1x Power switch
- 1x Li-ion/Li-po cell
- 1x 5.8GHz antenna
    - I've used a U.FL to SMA pigtail so I can connect an external antenna

### Wiring

<div align="center">
    <img src="./images/Wiring.png" alt="Wiring" />
</div>

## Software

### Environment setup

**1. Install PlatformIO**

Either the [PlatformIO IDE extension](https://platformio.org/install/integration) (VS Code recommended) or the [PlatformIO Core CLI](https://docs.platformio.org/en/latest/core/index.html):

```bash
pip install platformio
```

That's it — the ESP32 board support and libraries are installed automatically on the first build.

**2. (If necessary) Change display chip being used**

> [!IMPORTANT]
>
> This step is only necessary if using an OLED with the SSD1306 chip. OLEDs that use the SH1106 chip require no modification to the firmware.

As far as I can tell, most 0.96" I<sup>2</sup>C OLEDs use the SSD1306 chip, but I think a bigger 1.3" OLED is better for this project, which mostly seem to use the SH1106 controller. As such, the SH1106 controller is what this device has been developed for, but with some slight modifications it should be possible to use SSD1306 displays.

> [!NOTE]
>
> I haven't personally tested this. All the 1.3" OLEDs I've used have SH1106 chips.

Open `menu.h` and find the following line:

```cpp
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2;
```

Below this line there should be:

```cpp
// U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
```

Add `//` to the front of the first line and remove it from the front of the second line.

**3. (If necessary) Change SSID and password for the Wi-Fi hotspot**

> [!IMPORTANT]
>
> This step is only necessary if the default SSID `NuclearCounter` and password `nuclearcounter` isn't suitable for your use case. The Wi-Fi hotspot is only used for web-based interactions with the device, such as accessing the API.

In `platformio.ini`, the active build environment defines:

```ini
-D WIFI_SSID=\"NuclearCounter\"
-D WIFI_PASSWORD=\"nuclearcounter\"
```

Change these values to whatever you want, but note that text that is too long will run off the screen on the Wi-Fi menu.

### Building & flashing

**1. Connect the board**

Plug the board in with a USB-C cable. The firmware is built with USB-CDC-on-boot, so a serial port appears automatically — no BOOT-button tapping needed.

**2. Build**

`platformio.ini` has one build environment per board:

| Environment | Board |
|---|---|
| `NuclearCounterV2_1` | ESP32-C3 (NuclearCounter V2.1) |
| `NuclearCounterV3_0` | ESP32-S3 (NuclearCounter V3.0) |

```bash
pio run -e NuclearCounterV3_0          # build
pio run -e NuclearCounterV2_1          # …or the C3 board
```

**3. Flash**

```bash
pio run -e NuclearCounterV3_0 -t upload
```

> [!TIP]
>
> If the board doesn't appear, hold **BOOT** while it connects and press **EN** to release (forces download mode).

### Building & flashing from CI (recommended)

This repo has a GitHub Action (`.github/workflows/build.yml`) that builds the firmwares and packages a flash-ready artifact for each board, so you don't need a local toolchain or the StarForgeOS repo on hand:

| Artifact | Board | Contents |
|---|---|---|
| `nuclearcounter-v2.1-c3` | ESP32-C3 (NuclearCounter V2.1) | NuclearCounter (ota_0) + StarForgeOS (ota_1) + web UI + bootloader + partitions + `flash.sh` |
| `nuclearcounter-v3.0-c3` | ESP32-C3 (NuclearCounter V3.0, INTEGRATED) | same set, 4MB table (ota_1 @ 0x1A0000, UI @ 0x330000) |
| `nuclearcounter-v3.0-s3` | ESP32-S3 (NuclearCounter V3.0) | same set, S3 offsets |

**1. Run the action**

Go to the repo's **Actions** tab → select **Build firmware (v2.1 C3 + v3.0 C3 + v3.0 S3)** → **Run workflow**. It builds all three packages and uploads them. It also runs automatically on every push to `feat/dualboot` (or `main`) — a fresh artifact appears on each run's summary page.

**Tag-based release:** pushing a tag that points at a commit on this branch creates a **GitHub release** named after the tag, with all three packages attached as zip downloads (e.g. tag `DualBoot-1.0` → release `DualBoot-1.0` with `nuclearcounter-v2.1-c3-DualBoot-1.0.zip`, `nuclearcounter-v3.0-c3-DualBoot-1.0.zip` and `nuclearcounter-v3.0-s3-DualBoot-1.0.zip`). Tags on other branches (like `master`) don't trigger it, because the workflow only exists on this branch.

> [!NOTE]
>
> The action builds StarForgeOS from a fork: `SpencerGraffunder/StarForgeOS` @ branch `main`. Change the `starforgeos_repo` / `starforgeos_ref` inputs if you use a different fork/branch. Because the fork is public, no extra token is needed — if you ever point it at a *private* repo, add a read-only PAT to the repo secret `STARFORGEOS_TOKEN`.

**2. Download the artifact for your board**

On the run's summary page, download the artifact for your board (`nuclearcounter-v2.1-c3`, `nuclearcounter-v3.0-c3` or `nuclearcounter-v3.0-s3`). Unzip it — you'll get the `.bin` files plus `flash.sh` and a `manifest.txt` (which records the chip and offsets).

**3. Flash it**

Plug the board in via USB, then run `flash.sh`. It auto-detects the serial port and flashes everything with esptool at the correct offsets:

```bash
./flash.sh                 # auto-detect port
./flash.sh /dev/ttyACM0    # …or pass the port explicitly
```

> [!TIP]
>
> `flash.sh` installs `esptool` automatically if it's missing. The firmware is built with USB-CDC-on-boot, so the board auto-enters download mode on reset — no BOOT-button tapping needed. If your board doesn't auto-reset, hold **BOOT** while it connects and press **EN** to release.

**4. After flashing**

The board boots **NuclearCounter** (ota_0). Switch to **StarForgeOS** from the NuclearCounter menu (the ⭐ StarForge item in the hidden Advanced submenu), or back again from the StarForgeOS menu (**Boot Scanner Mode**). StarForgeOS's web UI is served from its AP (e.g. `http://192.168.8.1/`) in standalone mode.

### Dual boot with StarForgeOS

The NuclearCounter board runs two firmwares in two OTA slots:

| Slot | Firmware | Role |
|---|---|---|
| `ota_0` | **NuclearCounter** (this firmware) | 5.8GHz scanner |
| `ota_1` | [StarForgeOS](https://github.com/RaceFPV/StarForgeOS) (fork: [SpencerGraffunder/StarForgeOS](https://github.com/SpencerGraffunder/StarForgeOS)) | Drone race timing (standalone with Wi-Fi, or RotorHazard USB node) |

Key points:

- **Switching** is done from the OLED menu: the Advanced submenu (hold `SEL` on the main menu) has a ⭐ **StarForge** item that boots StarForgeOS, and the StarForgeOS menu has a **Boot Scanner Mode** item that boots this firmware. The selection survives power loss (stored in the `otadata` partition).
- **Shared hardware, shared settings** — both firmwares use the same buttons, OLED, buzzer, and battery. Buzzer on/off and the low-battery alarm threshold are configured in *this* firmware's menu (Settings) and are read by StarForgeOS from the shared NVS store, so one configuration covers both.
- **StarForgeOS standalone mode** serves a web UI from its Wi-Fi AP (`192.168.8.1`): live RSSI graph, lap list, stats, and race controls.
- The S3 board's Wi-Fi AP needs the TX-power fix (`esp_wifi_set_max_tx_power(20)`), which is already in the firmware; the AP SSID is `SFOS-<last 6 hex of MAC>` (open network) in StarForgeOS.

Full partition layout, manual esptool flashing, and troubleshooting (including the 4MB/8MB flash-size gotcha) are documented in [DUALBOOT.md](DUALBOOT.md).

### Battery calibration

Different boards, even of the same model, can have variations in their analog-to-digital converters, so performing a simple calibration is necessary to ensure the device reads the correct battery voltage.

Turn the device on, and in the bottom right corner of the main menu there will be a battery voltage readout, displaying, for example, `4.0v`. Take a multimeter and measure the raw battery voltage, rounded to 1 decimal place. The voltage on the multimeter and the voltage displayed on the main menu should ideally be the same, but it may be off by a small amount.

The value of `BATTERY_VOLTAGE_OFFSET` in `battery.h` can be increased or decreased, where a change of `1` in this value corresponds to a change of `0.1` in the displayed voltage.

For example, if the main menu is displaying `3.9v`, but the multimeter says the battery is at `4.0v`, then increase the value of `BATTERY_VOLTAGE_OFFSET` by `1`. If the menu displays a voltage higher than what the multimeter reads, then decrease the offset value.

Make the necessary changes, then compile and upload the firmware again.

## Usage

### Menus

There are three buttons used to operate the device:

- `PREV` - Go to the previous item
- `SEL` - Select an item
    - Press and hold `SEL` to go back
- `NEXT` - Go to the next item

The menu items can be navigated between with `PREV` and `NEXT`, and once the desired menu item is highlighted, `SEL` can be used to select it.

**Main**

This is the initial menu displayed when the device is powered on. It displays the options to navigate to the `Scan` menu, `Settings` submenus, `About` menu, and a hidden `Advanced` submenu. The current battery voltage is also displayed in the bottom right.

The hidden `Advanced` submenu can be accessed by pressing and holding `SEL`. It contains the `Wi-Fi` and `Calibration` menus, plus the ⭐ **StarForge** item, which reboots the board into the dual-booted StarForgeOS firmware.

**Scan**

This menu is where the graph of the scanned RSSI values is displayed and is covered more in [Scanning](#scanning).

**Scan interval**

Set the interval at which the spectrum will be scanned. A lower scan interval means that more frequencies are scanned, at the cost of taking longer to complete a full refresh, as each frequency takes about 30ms to scan. A higher scan interval means that fewer frequencies are scanned, but a full refresh is significantly faster.

Across the 300MHz spectrum being scanned (5645MHz to 5945MHz, and 5345MHz to 5645MHz):

- `5MHz` scans 61 frequencies every 5MHz
    - $(300/5)+1$ to also include the final frequency
- `10MHz` scans 31 frequencies every 10MHz
    - $(300/10)+1$ to also include the final frequency
- `20MHz` scans 16 frequencies every 20MHz
    - $(300/20)+1$ to also include the final frequency

The currently set option is displayed with the <img src="./icons/Selected.png" alt="Selected" /> icon.

**Buzzer**

Enable or disable the single beep that sounds on pressing a button, and the double beep that sounds on going back. This option doesn't affect the double beep on boot, nor the low battery alarm. These will always sound. The setting is shared with StarForgeOS via the NVS store (see [Dual boot with StarForgeOS](#dual-boot-with-starforgeos)).

The currently set option is displayed with the <img src="./icons/Selected.png" alt="Selected" /> icon.

**Battery alarm**

Set the voltage that the low battery alarm will go off at. The setting is shared with StarForgeOS via the NVS store (see [Dual boot with StarForgeOS](#dual-boot-with-starforgeos)).

The currently set option is displayed with the <img src="./icons/Selected.png" alt="Selected" /> icon.

**About**

Displays information about the device: the firmware name, current version, and credits — NuclearCounter is based on Hertz Hunter by Simon Eason.

**Wi-Fi**

Starts the Wi-Fi hotspot, displaying the SSID, password, and IP address of the device. Connect to this hotspot and use the provided IP to access web-based features, such as the API. Exiting this menu stops the hotspot and disconnects any connected devices. This feature is covered more in [Wi-Fi hotspot](#wi-fi-hotspot).

**Calibration**

Where calibration of known high and low RSSI values takes place. Helper text is displayed at the bottom to remind you which channel to set your VTX to when calibrating. This menu is covered more in [RSSI calibration](#rssi-calibration).

### Scanning

A column graph of the measured RSSI values is displayed in the `Scan` menu, where stronger signals are shown with a taller bar at the detected frequency. The device doesn't care what data is being sent on a frequency, only that there is something there, meaning that it isn't limited to just analog video signals. The graph will be updated live as the scanner goes through each frequency continuously. Once a scan of the entire spectrum has been completed it will start again and update the values.

The top left of the screen displays `HIGH` or `LOW` depending on the frequency range being scanned (`HIGH` for 5645MHz to 5945MHz, and `LOW` for 5345MHz to 5645MHz). These two scanning modes can be switched with `SEL`.

There is a cursor that can be moved along the spectrum using the `PREV` and `NEXT` buttons. The frequency the cursor is currently on is displayed in the top middle of the screen, and the signal strength on that frequency is reported as a percentage in the top right. More on how this percentage is calculated is covered in [RSSI calibration](#rssi-calibration).

In combination with the frequency markings along the bottom of the screen, this cursor can be used to find what frequency something is broadcasting on, and the strength of the broadcast.

*The cursor shows that something is broadcasting on R4*

<div align="center">
    <img src="./images/F4 signal.jpg" alt="F4 signal" width="40%"/>
</div>

### Wi-Fi hotspot

The Wi-Fi hotspot is provided as a means of accessing additional features through a web-based interface. Currently this includes an API that allows NuclearCounter to be integrated into other software, thus greatly extending the functionality beyond just the physical device.

The hotspot is started when the `Wi-Fi` menu is selected, and is stopped when this menu is exited. When the hotspot is running, the device scans the RF spectrum as it would when viewing the `Scan` menu, however it does it in the background and doesn't draw a graph on the display.

On this menu the configured SSID and password for the hotspot is displayed (default: `NuclearCounter` / `nuclearcounter`), which can be connected to from another device, such as a phone or computer. The IP is the address of the NuclearCounter device (`192.168.8.1`) and is where all requests should be sent to. The documentation for the API is available [here](API.md), and currently includes the following features:

- Requesting the current battery voltage
- Requesting up-to-date RSSI data
- Switching between high and low band scanning
- Requesting the current settings for the scan interval, buzzer state, and low battery alarm
- Updating the current settings for the scan interval, buzzer state, and low battery alarm
- Requesting the calibrated minimum and maximum signal strength values
- Setting the calibrated minimum and maximum signal strength values

<div align="center">
    <img src="./images/Wi-Fi.jpg" alt="Wi-Fi" width="40%"/>
</div>

### RSSI calibration

The scale of the graph and the signal strength readout in the `Scan` menu is controlled by the calibrated minimum and maximum RSSI values.

To calibrate:

1. Ensure no VTXs are transmitting on or near 5800MHz (F4)
2. Highlight `Calib. low` and press `SEL`
    - This saves an RSSI value that will be used for "nothing broadcasting" and allows for filtering out the base level of RF noise (i.e. the noise floor)
3. Plug in a VTX and set it to broadcast on 5800MHz (F4)
4. Highlight `Calib. high` and press `SEL`
    - This saves an RSSI value that will be used for "something broadcasting" and allows for proper scaling of the graph and signal strength readout


The signal strength readout will display `100%` for any RSSI that is at or higher than the RSSI captured when `Calib. high` was selected, and `0%` for any RSSI that is at or lower than the RSSI captured when `Calib. low` was selected. Any RSSI that falls between the calibrated high and low values will be mapped to a percentage based on its strength relative to the calibrated values.

*Helper text is present to remind you which channel to use for calibration*

<div align="center">
    <img src="./images/Calibration.jpg" alt="Calibration" width="40%" />
</div>

### Resetting

Due to the fact that the settings and calibration values are stored in non-volatile memory, flashing the firmware again won't wipe them. If, for some reason, the device needs to be completely reset, press `PREV`, `SEL` and `NEXT` simultaneously. The device should reboot with everything completely wiped and reset.

> [!CAUTION]
>
> On a dual-booted board this also clears the settings shared with StarForgeOS (buzzer on/off, battery alarm threshold). It does **not** erase the StarForgeOS app itself — that lives in the `ota_1` partition, not in NVS.

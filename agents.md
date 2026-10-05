# Agent working rules

## Always document non-obvious fixes in the code

When you figure out how to fix something that was surprising, subtle, or easy to
get wrong (a layout bug, a hardware quirk, a library gotcha, an ordering
constraint, etc.), **leave a comment at the fix site** explaining the root cause
and the invariant that must be preserved — so the same mistake doesn't happen
again in a future edit. State the *why* (the number, the constraint, the
mechanism), not just *what* changed. Prefer a short, precise comment over a long
one. If the gotcha spans a whole subsystem, put a short note here in agents.md
and point at the code.

## OLED (SH1306) display gotchas — NuclearCounter

- **u8g2 font baselines are NOT the top of the text.** For `u8g2_font_5x7`,
  `ascent_A = 6`: a `drawStr(x, y)` puts the top row of a cap/digit at `y-6`.
  Consequences:
  - Any 5x7 baseline **below y=6 clips the top of the text** off the panel
    (u8g2 clips to row 0). Keep the topmost baseline ≥ 7. (See `TP_ROW0_Y` in
    `src/menu.cpp` — it was moved from 4 to 7 for exactly this reason.)
  - A white **highlight box** behind 5x7 text must start at `baseline-6` (minus
    a margin) and extend to the baseline. Starting it at `baseline` (or `y-1`)
    leaves the top 5 rows of the glyph outside the box, so black-on-white text
    is drawn black-on-black above the box and the top of the value is
    unreadable. (See `tpDrawSeg` / `tpDrawControl` in `src/menu.cpp`.)
  - Reference: the working main-menu selection highlight (`drawSelectionMenu`)
    reserves 16px per row and baselines the 5x7 text 12px below the box top.
- **Safe horizontal margins:** left x=4, right-align around x=120. The panel has
  a ~2px left glass offset and a 6px right black guard (columns 122-127 are
  never drawn — see `Menu::begin`).
- The panel **retains the last frame across a reset/hang**, so a board that
  resets mid-transition looks frozen on the old screen.

## USB CDC serial gotcha (NuclearCounter C3/S3) — blocking writes starve the main loop

- With `ARDUINO_USB_CDC_ON_BOOT=1`, `Serial` is `USBCDC`, and `USBCDC::write()`
  **blocks up to `tx_timeout_ms` (default 250 ms)** when the host has the port open
  but is not draining the TX FIFO. Measured on hardware: the main loop ran at
  ~40 iterations/s while a host was reading, but dropped to ~4 iterations/s while
  the port was open and unread — i.e. ~250 ms per iteration. That is what made the
  board look dead to button presses while plugged into USB, while working fine
  unplugged (`tud_cdc_n_connected()` is false then, so `write()` returns at once).
- Fix: `Serial.setTxTimeoutMs(20)` in `setup()` — bounded, not 0 (0 drops frames
  outright, which can lose a RotorHazard protocol response). Guard it with
  `#if ARDUINO_USB_CDC_ON_BOOT`: `HardwareSerial` has no `setTxTimeoutMs`.
- Consequence: never put a periodic `Serial.print` in `loop()` in a shipping build.
  The loop heartbeat / button trace is now opt-in via `-D C3_DEBUG_HEARTBEAT=1`
  (see `include/menu.h`), not defined by default.

## WiFi timer page gotchas (INTEGRATED build)

- **WiFi is on only while the timer page is open.** `enterTimer()` brings the AP up
  and `exitTimer()` tears it down (`_web.stop()`, then `softAPdisconnect(true)`, then
  `WiFi.mode(WIFI_OFF)`). The full `WIFI_OFF` teardown is mandatory: a partial teardown
  crashes the next `WiFi.mode(WIFI_AP)` in `ieee80211_hostap_attach` (LoadProhibited,
  DEPC=0x0012). `WebServerManager` is split the same way: `begin()` once (routes +
  SPIFFS), `start()`/`stop()` per session — re-running `begin()` registers duplicate routes.
- **`esp_wifi_set_max_tx_power()` is in 0.25 dBm units, range [8, 84]** = 2–20 dBm.
  The inherited `20` was 5 dBm, which is why the AP beacon was invisible to some
  clients. See `src/settings/wifi_manager.cpp`.
- **`data/app.js` polls `/api/status` every 400 ms.** Measured on the single-core C3
  (repeated 60 s windows, `/api/jitter` bench endpoint): the 1 ms-interval sampling
  task (`TIMING_INTERVAL_MS`, target 1000 samples/s) holds ~996 samples/s with the AP
  idle, ~880 at 4 Hz polling, ~798 at 10 Hz and ~377 at 25 Hz.
  Raising the poll rate to make the RSSI bar look smoother directly taxes the radio loop.
  The remaining ~400 ms stall that appears under any HTTP load happens **outside** the
  critical section (`outside_max ≈ 400 ms` vs `wait_max ≈ 1.3 ms`, `work_max ≈ 1.8 ms`),
  so it is CPU preemption by the WiFi/lwIP stack, not lock contention — raising
  `TIMING_PRIORITY` above lwIP (18) does **not** help (4 Hz dropped to ~770 samples/s at
  priority 20 vs ~880 at priority 2).
- There is a residual **400 ms** stall (measured `max_us` = 400030 µs, always the same
  length) that appears under HTTP load roughly once per 75 s and roughly once per 300 s
  with the AP idle but a client associated. It is not lock contention and not the
  deactivated-task delay (`deact_iters = 0`), so it is a WiFi/lwIP stack event. It is not
  worth chasing further: lap detection is peak-capture based (`rssi_peak_time_ms` in
  `TimingCore::process`), so a 400 ms blind spot shifts a captured peak by at most a few
  hundred ms and does not drop the lap.
- **OTA rollback is deferred by 20 s** (`OTA_VERIFY_DELAY_MS` in `src/main.ino`) so a
  crash-looping image reverts to the other slot. `verifyRollbackLater()` must stay defined
  with **C linkage** — `initArduino()` calls it through `extern "C"`, so a plain C++
  definition links but never defers anything. `ota_1` is the rollback slot, **not**
  StarForgeOS: any web OTA overwrites ota_1 and dual-boot must be re-flashed with esptool.

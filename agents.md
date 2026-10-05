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

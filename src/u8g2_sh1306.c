/*
  SH1306 128x64 display support (I2C, full-frame buffer).

  The SH1306 is a SH1106-family chip: identical I2C protocol and init
  sequence, but its 130-column glass shows the 128 visible columns
  starting at column 0 (the two spare columns 128-129 are on the right),
  unlike the SH1106 whose visible window starts at column 2.

  U8g2 ships no SH1306 display, so register it here: reuse the SH1106
  display callback and I2C CAD as-is, with a display info whose
  default_x_offset is 0.
*/

#include "clib/u8g2.h" /* pulls in clib/u8x8.h with the display callback + CAD decls */

static u8x8_display_info_t sh1306_128x64_noname_display_info = {
  /* chip_enable_level = */ 0,
  /* chip_disable_level = */ 1,
  /* post_chip_enable_wait_ns = */ 20,
  /* pre_chip_disable_wait_ns = */ 10,
  /* reset_pulse_width_ms = */ 100,
  /* post_reset_wait_ms = */ 100,
  /* sda_setup_time_ns = */ 50,
  /* sck_pulse_width_ns = */ 50,
  /* sck_clock_hz = */ 4000000UL,
  /* spi_mode = */ 0,
  /* i2c_bus_clock_100kHz = */ 4,
  /* data_setup_time_ns = */ 40,
  /* write_pulse_width_ns = */ 150,
  /* tile_width = */ 16,
  /* tile_height = */ 8,
  /* default_x_offset = */ 0,   /* SH1306: visible window starts at column 0 */
  /* flipmode_x_offset = */ 0,
  /* pixel_width = */ 128,
  /* pixel_height = */ 64
};

void u8g2_Setup_sh1306_i2c_128x64_noname_f(u8g2_t *u8g2, const u8g2_cb_t *rotation, u8x8_msg_cb byte_cb, u8x8_msg_cb gpio_and_delay_cb) {
  uint8_t tile_buf_height;
  uint8_t *buf;
  /* SH1106 chip: same display callback and fast-mode I2C CAD */
  u8g2_SetupDisplay(u8g2, u8x8_d_sh1106_128x64_noname, u8x8_cad_ssd13xx_fast_i2c, byte_cb, gpio_and_delay_cb);
  /* Swap in the SH1306 geometry after the SH1106 setup has run (u8g2
     begin() only re-applies the init sequence, not display_info/x_offset) */
  u8g2->u8x8.display_info = &sh1306_128x64_noname_display_info;
  u8g2->u8x8.x_offset = 0;
  /* Full-frame buffer — the _2_ line buffer variants only drive the top 16px */
  buf = u8g2_m_16_8_f(&tile_buf_height);
  u8g2_SetupBuffer(u8g2, buf, tile_buf_height, u8g2_ll_hvline_vertical_top_lsb, rotation);
}

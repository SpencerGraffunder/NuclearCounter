#ifndef SH1306_H
#define SH1306_H

#include <U8g2lib.h>

#ifdef __cplusplus
extern "C" {
#endif

// SH1306 128x64 over I2C, full-frame buffer. Display registered in
// src/u8g2_sh1306.c (SH1106 driver with the visible window at column 0).
void u8g2_Setup_sh1306_i2c_128x64_noname_f(u8g2_t *u8g2, const u8g2_cb_t *rotation, u8x8_msg_cb byte_cb, u8x8_msg_cb gpio_and_delay_cb);

#ifdef __cplusplus
}
#endif

// Constructor following U8g2's naming pattern (HW I2C, full frame)
class U8G2_SH1306_128X64_NONAME_F_HW_I2C : public U8G2 {
  public: U8G2_SH1306_128X64_NONAME_F_HW_I2C(const u8g2_cb_t *rotation, uint8_t reset = U8X8_PIN_NONE, uint8_t clock = U8X8_PIN_NONE, uint8_t data = U8X8_PIN_NONE) : U8G2() {
    u8g2_Setup_sh1306_i2c_128x64_noname_f(&u8g2, rotation, u8x8_byte_arduino_hw_i2c, u8x8_gpio_and_delay_arduino);
    u8x8_SetPin_HW_I2C(getU8x8(), reset, clock, data);
  }
};

#endif

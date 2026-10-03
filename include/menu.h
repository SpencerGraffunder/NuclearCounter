#ifndef MENU_H
#define MENU_H

#include <Arduino.h>
#include <U8g2lib.h>
#include "about.h"
#include "sh1306.h"
#include "api.h"
#include "battery.h"
#include "bitmaps.h"
#include "buzzer.h"
#include "RX5808.h"
#include "settings.h"

#ifdef INTEGRATED
class IntegratedMode;
#endif

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

#define DEBOUNCE_DELAY 150

// How long button has to be held to be long-pressed
#define LONG_PRESS_DURATION (500 - DEBOUNCE_DELAY)

// Keeps small area at top and bottom for text display on scan menu
#define BAR_Y_MIN 14
#define BAR_Y_MAX 57

// Enum for different menus
// Order is important from initMenus()
enum MenuIndex {
  MAIN,
  SCAN,
  SETTINGS,
  ABOUT,
  ADVANCED,
  SCAN_INTERVAL,
  BUZZER,
  BATTERY_ALARM,
  WIFI,
  CALIBRATION,
  MENU_COUNT  // For array bounds checking
};

// Holds menu state, and navigation and drawing functions
class Menu {
public:
  Menu(uint8_t p_p, uint8_t s_p, uint8_t n_p, Settings *s, Buzzer *b, RX5808 *r, Api *a);
#ifdef INTEGRATED
  Menu(uint8_t p_p, uint8_t s_p, uint8_t n_p, Settings *s, Buzzer *b, RX5808 *r, IntegratedMode *i);
#endif
  void begin();
  void handleButtons();
  void clearBuffer();
  void sendBuffer();
  void drawMenu();
  void drawBatteryVoltage(int voltage);

private:
  // Menu data structures
  struct menuItemStruct {
    const char *name;
    const unsigned char *icon;
  };

  struct menuStruct {
    const char *title;
    menuItemStruct *menuItems;
    int menuItemsLength;
    int menuIndex;
  };

  void drawSelectionMenu();
  void drawScanMenu();
  void drawAboutMenu();
  void drawWifiMenu();
  void updateSettingsOptionIcons(menuStruct *menu, int selectedIndex);
  void initMenus();
  int xTextCentre(const char *text, int fontCharWidth);

  menuItemStruct mainMenuItems[3];
  menuItemStruct settingsMenuItems[3];
  menuItemStruct scanIntervalMenuItems[3];
  menuItemStruct buzzerMenuItems[2];
  menuItemStruct batteryAlarmMenuItems[3];
  menuItemStruct advancedMenuItems[3];
  menuItemStruct calibrationMenuItems[2];
  menuStruct menus[MENU_COUNT];

  MenuIndex menuIndex;

  uint8_t previous_pin;
  uint8_t select_pin;
  uint8_t next_pin;

  // Used to handle long-pressing SELECT to go back
  unsigned long selectButtonPressTime;
  bool selectButtonHeld;

  Settings *settings;
  Buzzer *buzzer;
  RX5808 *module;
  Api *api;

#ifdef INTEGRATED
  IntegratedMode *integrated;
  void drawTimerMenu();
  void drawTimerLapRow(const char *label, uint32_t ms, int y);
  void drawStartingSplash();
  void calibrateWithTiming(bool high);
#endif

#ifdef SH1106
  // SH1106: MUST use the F (full-frame, 64-row) buffer. The _2_ variants use a
  // 2-page (16-row) line buffer that only drives the top 16px of the panel —
  // everything below row 15 goes black (verified with an on-screen border test).
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2;
#elif defined(SH1306)
  // SH1306: SH1106 chip, 130-col glass, visible 128 cols start at column 0
  // (spare 128-129 on the right). Not in U8g2 — registered locally (see
  // include/sh1306.h, src/u8g2_sh1306.c). Full-frame buffer, same as SH1106.
  U8G2_SH1306_128X64_NONAME_F_HW_I2C u8g2;
#else
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
#endif
};

#endif

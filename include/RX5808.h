#ifndef RX5808_H
#define RX5808_H

#include <Arduino.h>
#include "settings.h"
#include "variable.h"

#define MAX_FREQUENCIES_SCANNED 60 + 1
#define HIGHBAND_MIN_FREQUENCY 5645
#define LOWBAND_MIN_FREQUENCY 5345
#define SCAN_FREQUENCY_RANGE 300

#define RSSI_STABILISATION_TIME 30
#define RSSI_SAMPLES 30

#define SCAN_STACK_SIZE 4096

// RX5808 receiver module
class RX5808 {
public:
  RX5808(uint8_t data, uint8_t le, uint8_t clk, uint8_t rssi, Settings *s);
  void startScan();
  void stopScan();
  void calibrate(bool high);

  // Frequency control (public: the integrated TimingCore delegates RF tuning to this object)
  void setFrequency(int frequency);

  VariableArrayRestricted<int, MAX_FREQUENCIES_SCANNED> rssiValues;
  Variable<bool> lowband;

  SemaphoreHandle_t scanMutex;

private:
  static void _scan(void *parameter);
  int readRSSI();
  void reset();
  void sendRegister(byte address, unsigned long data);
  void sendBit(bool bit);
  unsigned long frequencyToRegister(int frequency);

  uint8_t dataPin;
  uint8_t lePin;
  uint8_t clkPin;
  uint8_t rssiPin;

  TaskHandle_t scanHandle;

  Settings *settings;
};

#endif

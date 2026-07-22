#pragma once

#include <Wire.h>

class CST816D {
 public:
  CST816D(int8_t sda, int8_t scl, int8_t rst, int8_t interrupt);
  void begin();
  bool getTouch(uint16_t *x, uint16_t *y, uint8_t *gesture);
  bool isConnected();
  int touchCount();  // -1 means the I2C read failed.

 private:
  int8_t sda_, scl_, rst_, interrupt_;
  uint8_t readByte(uint8_t reg);
  bool readBytes(uint8_t reg, uint8_t *data, uint8_t count);
};

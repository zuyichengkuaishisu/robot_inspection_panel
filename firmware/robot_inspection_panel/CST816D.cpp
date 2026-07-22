#include "CST816D.h"

static constexpr uint8_t CST816_ADDRESS = 0x15;

CST816D::CST816D(int8_t sda, int8_t scl, int8_t rst, int8_t interrupt)
    : sda_(sda), scl_(scl), rst_(rst), interrupt_(interrupt) {}

void CST816D::begin() {
  Wire.begin(sda_, scl_);
  pinMode(rst_, OUTPUT);
  digitalWrite(rst_, LOW);
  delay(10);
  digitalWrite(rst_, HIGH);
  delay(300);

  // The ESP32-2424S012 reference firmware drives INT during CST816D startup.
  // Leaving it as an input prevents touch reports on some panels.
  pinMode(interrupt_, OUTPUT);
  digitalWrite(interrupt_, HIGH);
  delay(1);
  digitalWrite(interrupt_, LOW);
  delay(1);
  Wire.beginTransmission(CST816_ADDRESS);
  Wire.write(0xFE);
  Wire.write(0xFF);
  Wire.endTransmission();
}

bool CST816D::getTouch(uint16_t *x, uint16_t *y, uint8_t *gesture) {
  if (touchCount() <= 0) return false;
  uint8_t data[4];
  if (!readBytes(0x03, data, sizeof(data))) return false;
  if (gesture) *gesture = readByte(0x01);
  *x = ((data[0] & 0x0F) << 8) | data[1];
  *y = ((data[2] & 0x0F) << 8) | data[3];
  return true;
}

bool CST816D::isConnected() {
  Wire.beginTransmission(CST816_ADDRESS);
  return Wire.endTransmission() == 0;
}

int CST816D::touchCount() {
  Wire.beginTransmission(CST816_ADDRESS);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(CST816_ADDRESS, (uint8_t)1) != 1) return -1;
  return Wire.read() & 0x0F;
}

uint8_t CST816D::readByte(uint8_t reg) {
  uint8_t value = 0;
  return readBytes(reg, &value, 1) ? value : 0;
}

bool CST816D::readBytes(uint8_t reg, uint8_t *data, uint8_t count) {
  Wire.beginTransmission(CST816_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(CST816_ADDRESS, count) != count) return false;
  for (uint8_t i = 0; i < count; ++i) data[i] = Wire.read();
  return true;
}

#pragma once

// ============================================================
//  mod_compass - QMC5883L Magnetometer (I2C)
//
//  Verdrahtung (gleicher Bus wie RTC + Gyro):
//   VCC → 3.3V
//   GND → GND
//   SDA → GPIO45
//   SCL → GPIO21
//   I2C Adresse: 0x0D
// ============================================================

void  compass_init();
bool  compass_ok();
float compass_heading_deg();  // 0–360°, magnetisch Nord

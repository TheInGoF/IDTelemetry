#include "mod_compass.h"
#include "mod_logs.h"
#include "mod_config.h"
#include <Wire.h>
#include <math.h>
#include <Arduino.h>

// ============================================================
//  mod_compass - QMC5883L Magnetometer
// ============================================================

#define QMC_ADDR       0x0D
#define QMC_REG_DATA   0x00   // X_LSB, X_MSB, Y_LSB, Y_MSB, Z_LSB, Z_MSB
#define QMC_REG_STATUS 0x06
#define QMC_REG_CTRL1  0x09   // Mode, ODR, RNG, OSR
#define QMC_REG_SETRST 0x0B   // Set/Reset period

static bool s_ok = false;

static bool qmc_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMC_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

void compass_init() {
    if (!cfg_mod_compass()) return;

    // Prüfen ob Chip antwortet
    Wire.beginTransmission(QMC_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[COMPASS] FEHLER: QMC5883L nicht gefunden!");
        syslog("COMPASS", "FEHLER: QMC5883L nicht gefunden — Lötstelle/I2C prüfen");
        return;
    }

    // Set/Reset period
    qmc_write(QMC_REG_SETRST, 0x01);
    // Continuous mode, 10Hz, 8G range, 512 OSR
    qmc_write(QMC_REG_CTRL1, 0x1D);

    s_ok = true;
    Serial.println("[COMPASS] QMC5883L OK");
    syslog("COMPASS", "QMC5883L OK");
}

bool compass_ok() { return s_ok; }

float compass_heading_deg() {
    if (!s_ok) return 0.0f;

    Wire.beginTransmission(QMC_ADDR);
    Wire.write(QMC_REG_DATA);
    if (Wire.endTransmission() != 0) return 0.0f;

    Wire.requestFrom((uint8_t)QMC_ADDR, (uint8_t)6);
    if (Wire.available() < 6) return 0.0f;

    int16_t x = (int16_t)(Wire.read() | (Wire.read() << 8));
    int16_t y = (int16_t)(Wire.read() | (Wire.read() << 8));
    Wire.read(); Wire.read(); // Z verwerfen

    float heading = atan2f((float)y, (float)x) * (180.0f / M_PI);
    if (heading < 0) heading += 360.0f;
    return heading;
}

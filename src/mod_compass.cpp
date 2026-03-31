#include "mod_compass.h"
#include "mod_logs.h"
#include "mod_config.h"
#include <Wire.h>
#include <math.h>
#include <Arduino.h>
#include <Preferences.h>

#define QMC_ADDR       0x0D
#define QMC_REG_DATA   0x00
#define QMC_REG_CTRL1  0x09
#define QMC_REG_SETRST 0x0B

// ── Kalibrierungswerte ───────────────────────────────────────
static float s_off_x        = 0.0f;
static float s_off_y        = 0.0f;
static float s_drv_off      = 0.0f;
static bool  s_hi_ok        = false;   // Hard-Iron bereit

// Hard-Iron: kontinuierliche Min/Max-Sammlung (Rohwerte)
static float s_min_x =  32767.0f, s_max_x = -32768.0f;
static float s_min_y =  32767.0f, s_max_y = -32768.0f;

// Drive-Offset: EMA
static bool  s_drv_ema_init = false;
static float s_drv_ema      = 0.0f;
static const float EMA_ALPHA = 0.03f;   // ~33 Samples zum Einpendeln

static bool  s_ok           = false;
static bool  s_cal_changed  = false;    // seit letztem NVS-Save geändert
static uint32_t s_last_save_ms = 0;

#define NVS_NS "compass"
#define MIN_RANGE 400.0f   // Mindest-Rohwert-Spanne für valide Hard-Iron-Kal.
#define DRIVE_MIN_SPEED 20.0f

// ── NVS ──────────────────────────────────────────────────────
static void cal_load() {
    Preferences p;
    p.begin(NVS_NS, true);
    s_hi_ok  = p.getBool("hi_ok",  false);
    s_off_x  = p.getFloat("off_x", 0.0f);
    s_off_y  = p.getFloat("off_y", 0.0f);
    s_drv_off= p.getFloat("drv",   0.0f);
    // Gesammelte Min/Max wiederherstellen damit Kalibrierung weiterwächst
    s_min_x  = p.getFloat("mn_x",  32767.0f);
    s_max_x  = p.getFloat("mx_x", -32768.0f);
    s_min_y  = p.getFloat("mn_y",  32767.0f);
    s_max_y  = p.getFloat("mx_y", -32768.0f);
    p.end();
}

static void cal_save() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putBool ("hi_ok", s_hi_ok);
    p.putFloat("off_x", s_off_x);
    p.putFloat("off_y", s_off_y);
    p.putFloat("drv",   s_drv_off);
    p.putFloat("mn_x",  s_min_x);
    p.putFloat("mx_x",  s_max_x);
    p.putFloat("mn_y",  s_min_y);
    p.putFloat("mx_y",  s_max_y);
    p.end();
    s_cal_changed = false;
}

// ── I2C ──────────────────────────────────────────────────────
static bool qmc_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMC_ADDR);
    Wire.write(reg); Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool qmc_read_raw(int16_t& x, int16_t& y) {
    Wire.beginTransmission(QMC_ADDR);
    Wire.write(QMC_REG_DATA);
    if (Wire.endTransmission() != 0) return false;
    Wire.requestFrom((uint8_t)QMC_ADDR, (uint8_t)6);
    if (Wire.available() < 6) return false;
    x = (int16_t)(Wire.read() | (Wire.read() << 8));
    y = (int16_t)(Wire.read() | (Wire.read() << 8));
    Wire.read(); Wire.read();
    return true;
}

// ── Init ─────────────────────────────────────────────────────
void compass_init() {
    if (!cfg_mod_compass()) return;

    Wire.beginTransmission(QMC_ADDR);
    if (Wire.endTransmission() != 0) {
        syslog("COMPASS", "FEHLER: QMC5883L nicht gefunden");
        return;
    }
    qmc_write(QMC_REG_SETRST, 0x01);
    qmc_write(QMC_REG_CTRL1,  0x1D);  // Continuous, 10Hz, 8G, 512 OSR

    cal_load();
    s_ok = true;

    char msg[96];
    if (s_hi_ok) {
        snprintf(msg, sizeof(msg),
                 "QMC5883L OK · HI X%.0f/Y%.0f · Fahrtoff: %.1f°",
                 s_off_x, s_off_y, s_drv_off);
    } else {
        snprintf(msg, sizeof(msg), "QMC5883L OK · Kalibrierung läuft automatisch");
    }
    syslog("COMPASS", msg);
}

bool compass_ok() { return s_ok; }

// ── Heading lesen (kalibriert) ────────────────────────────────
float compass_heading_deg() {
    if (!s_ok) return 0.0f;
    int16_t rx, ry;
    if (!qmc_read_raw(rx, ry)) return 0.0f;

    float x = (float)rx - s_off_x;
    float y = (float)ry - s_off_y;

    float h = atan2f(y, x) * (180.0f / M_PI) + s_drv_off;
    h = fmodf(h + 360.0f, 360.0f);
    return h;
}

// ── Automatische Kalibrierung (1× pro Sekunde) ────────────────
void compass_auto_cal(float speed_kmh, float gps_course_deg, bool gps_ok) {
    if (!s_ok) return;

    int16_t rx, ry;
    if (!qmc_read_raw(rx, ry)) return;

    // ─ Hard-Iron: Min/Max erweitern ──────────────────────────
    bool hi_changed = false;
    if ((float)rx < s_min_x) { s_min_x = (float)rx; hi_changed = true; }
    if ((float)rx > s_max_x) { s_max_x = (float)rx; hi_changed = true; }
    if ((float)ry < s_min_y) { s_min_y = (float)ry; hi_changed = true; }
    if ((float)ry > s_max_y) { s_max_y = (float)ry; hi_changed = true; }

    float range_x = s_max_x - s_min_x;
    float range_y = s_max_y - s_min_y;

    if (hi_changed && range_x >= MIN_RANGE && range_y >= MIN_RANGE) {
        float new_off_x = (s_max_x + s_min_x) / 2.0f;
        float new_off_y = (s_max_y + s_min_y) / 2.0f;
        if (!s_hi_ok || fabsf(new_off_x - s_off_x) > 2.0f || fabsf(new_off_y - s_off_y) > 2.0f) {
            s_off_x = new_off_x;
            s_off_y = new_off_y;
            if (!s_hi_ok) {
                s_hi_ok = true;
                syslog("COMPASS", "Hard-Iron kalibriert");
            }
            s_cal_changed = true;
        }
    }

    // ─ Drive-Offset: EMA bei Geradeausfahrt ──────────────────
    if (gps_ok && speed_kmh >= DRIVE_MIN_SPEED) {
        // Rohes Heading ohne drive_offset
        float x = (float)rx - s_off_x;
        float y = (float)ry - s_off_y;
        float raw_h = atan2f(y, x) * (180.0f / M_PI);
        if (raw_h < 0) raw_h += 360.0f;

        // Differenz GPS-Kurs − Kompass-Rohwert (wrap-safe)
        float diff = gps_course_deg - raw_h;
        if (diff >  180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;

        if (!s_drv_ema_init) {
            s_drv_ema = diff;
            s_drv_ema_init = true;
        } else {
            s_drv_ema = EMA_ALPHA * diff + (1.0f - EMA_ALPHA) * s_drv_ema;
        }

        if (fabsf(s_drv_ema - s_drv_off) > 0.5f) {
            s_drv_off = s_drv_ema;
            s_cal_changed = true;
        }
    }

    // ─ NVS alle 5 Minuten wenn geändert ─────────────────────
    if (s_cal_changed && (millis() - s_last_save_ms) >= 300000UL) {
        cal_save();
        s_last_save_ms = millis();
    }
}

// ── Status + Reset ───────────────────────────────────────────
bool  compass_cal_has_hard_iron() { return s_hi_ok; }
float compass_cal_drive_offset()  { return s_drv_off; }

void compass_cal_reset() {
    s_off_x = s_off_y = s_drv_off = 0.0f;
    s_hi_ok = s_drv_ema_init = false;
    s_min_x = s_min_y =  32767.0f;
    s_max_x = s_max_y = -32768.0f;
    Preferences p;
    p.begin(NVS_NS, false);
    p.clear();
    p.end();
    syslog("COMPASS", "Kalibrierung zurückgesetzt");
}

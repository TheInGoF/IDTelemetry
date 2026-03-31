#include "mod_gps_ext.h"
#include "mod_logs.h"
#include "mod_sleep.h"
#include "config.h"
#include "shared.h"
#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_u-blox_GNSS_v3.h>

// ============================================================
//  mod_gps_ext — BLITZ Mini M10 GPS  (I2C / QWIIC, Adresse 0x42)
//  Wire-Bus: GPIO45 SDA / GPIO21 SCL  (geteilt mit RTC + MPU-6050)
// ============================================================

static SFE_UBLOX_GNSS s_gnss;
static bool  s_ok        = false;
static int   s_sat_count = -1;

// ── FreeRTOS Task ─────────────────────────────────────────
static void gps_ext_task(void*) {
    while (!g_shutdown) {
        s_gnss.checkUblox();
        s_gnss.checkCallbacks();

        if (s_gnss.getGnssFixOk()) {
            double lat = s_gnss.getLatitude()    / 1e7;
            double lon = s_gnss.getLongitude()   / 1e7;
            float  spd = s_gnss.getGroundSpeed() / 277.778f;  // mm/s → km/h
            float  crs = s_gnss.getHeading()     / 1e5f;      // 1e-5 deg → deg
            s_sat_count = s_gnss.getSIV();
            if (lat != 0.0 || lon != 0.0) {
                gps_update(lat, lon, spd, crs);
            } else {
                gps_invalidate();
            }
        } else {
            gps_invalidate();
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
    vTaskDelete(NULL);
}

// ── Öffentliche API ───────────────────────────────────────
void gps_ext_init() {
    if (!GPS_EXT_ENABLED) return;

    // Wire wurde bereits in mod_rtc via Wire.begin(45,21) gestartet
    if (!s_gnss.begin(Wire, 0x42)) {
        syslog("GPS_EXT", "FEHLER: BLITZ M10 nicht gefunden (I2C 0x42)");
        return;
    }

    s_gnss.setI2COutput(COM_TYPE_UBX);   // nur UBX, kein NMEA-Spam auf I2C
    s_gnss.setNavigationFrequency(5);    // 5 Hz
    s_gnss.setAutoPVT(true);
    s_gnss.saveConfiguration();

    s_ok = true;
    syslog("GPS_EXT", "BLITZ M10 GPS aktiv — I2C 0x42 QWIIC GPIO45/21 5Hz");

    xTaskCreatePinnedToCore(gps_ext_task, "GPS_EXT", 4096, NULL, 2, NULL, 1);
}

bool gps_ext_ok()        { return s_ok; }
int  gps_ext_sat_count() { return s_gnss.getSIV(); }

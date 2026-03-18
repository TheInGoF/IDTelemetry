// TINY_GSM_MODEM_SIM7080 muss VOR dem ersten TinyGSM-Include definiert
// sein.  Wird über platformio.ini build_flags gesetzt; hier als Fallback.
#ifndef TINY_GSM_MODEM_SIM7080
#define TINY_GSM_MODEM_SIM7080
#endif

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>

#include "mod_traccar.h"
#include "mod_modem.h"   // modem_ensure_connected()
#include "shared.h"      // g_gps
#include "mod_pmu.h"     // pmu_batt_pct()
#include "mod_logs.h"    // syslog()
#include "config.h"      // GPS_INTERVAL_MS
#include "mod_config.h"  // cfg_traccar_host(), cfg_traccar_id()
#include <Arduino.h>

// ---- Extern: TinyGsm-Instanz aus mod_modem ----------------
// modem_get() ist nicht in mod_modem.h — kein TinyGSM im öffentlichen Header.

extern TinyGsm& modem_get();

// ---- Letzte gesendete Position (Duplikat-Filter) ----------
static double s_last_sent_lat = 0.0;
static double s_last_sent_lon = 0.0;
static bool   s_ever_sent     = false;

// ~10 Meter Schwelle (≈0.0001°) — vermeidet GPS-Sprünge im Stand
static constexpr double POS_THRESHOLD = 0.0001;

static bool position_changed(const GpsSnapshot& fix) {
    if (!s_ever_sent) return true;
    double dlat = fix.lat - s_last_sent_lat;
    double dlon = fix.lon - s_last_sent_lon;
    if (dlat < 0) dlat = -dlat;
    if (dlon < 0) dlon = -dlon;
    return (dlat > POS_THRESHOLD || dlon > POS_THRESHOLD);
}

// ---- HTTPS GET an Traccar ---------------------------------

static void send_to_traccar(const GpsSnapshot& fix) {
    int batt = pmu_batt_pct();
    if (batt < 0) batt = 0;

    TinyGsmClientSecure client(modem_get());
    HttpClient          http(client, cfg_traccar_host(), 443);
    http.setHttpResponseTimeout(15000);

    char path[256];
    snprintf(path, sizeof(path),
             "/?id=%s&lat=%.6f&lon=%.6f&batt=%d",
             cfg_traccar_id(),
             (float)fix.lat, (float)fix.lon,
             batt);

    Serial.printf("[TRACCAR] → https://%s%s\n", cfg_traccar_host(), path);

    int err = http.get(path);
    if (err != 0) {
        Serial.printf("[TRACCAR] HTTP-Fehler: %d\n", err);
        http.stop();
        return;
    }

    int status = http.responseStatusCode();
    Serial.printf("[TRACCAR] HTTP %d  lat=%.6f lon=%.6f batt=%d%%\n",
                  status, (float)fix.lat, (float)fix.lon, batt);
    http.stop();

    char tlog[96];
    if (status == 200) {
        snprintf(tlog, sizeof(tlog), "gesendet · %.6f %.6f · Akku %d%%",
                 (float)fix.lat, (float)fix.lon, batt);
    } else {
        snprintf(tlog, sizeof(tlog), "Fehler HTTP %d · %.6f %.6f",
                 status, (float)fix.lat, (float)fix.lon);
    }
    syslog("TRACCAR", tlog);
}

// ---- Öffentliche API ---------------------------------------

void traccar_on_gps_tick() {
    traccar_on_gps_tick(gps_snapshot());
}

void traccar_on_gps_tick(const GpsSnapshot& fix) {
    if (!fix.valid) {
        Serial.println("[TRACCAR] Kein GPS-Fix — Versand übersprungen");
        return;
    }

    if (!position_changed(fix)) {
        Serial.println("[TRACCAR] Position unverändert — Versand übersprungen");
        return;
    }

    if (!modem_ensure_connected()) {
        Serial.println("[TRACCAR] Kein Netz — Versand übersprungen");
        return;
    }

    send_to_traccar(fix);

    // Letzte gesendete Position merken
    s_last_sent_lat = fix.lat;
    s_last_sent_lon = fix.lon;
    s_ever_sent     = true;
}


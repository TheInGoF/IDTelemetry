#include "mod_telemetry.h"
#include "mod_sleep.h"
#include "shared.h"
#include "mod_modem.h"
#include "mod_pmu.h"
#include "mod_gyro.h"
#include "mod_can.h"
#include "mod_rtc.h"
#include "mod_logs.h"
#include "mod_elm327.h"
#include "mod_gps_ext.h"
#include "config.h"
#include "mod_config.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>

// ── ECU-Adressen (VW MEB) ────────────────────────────────────
// 29-Bit extended IDs (VW MEB: Priority 0x17 + Adresse)
static constexpr uint32_t ECU_FC007B_REQ  = 0x17FC007B;
static constexpr uint32_t ECU_FC007B_RESP = 0x17FE007B;
static constexpr uint32_t ECU_FC0076_REQ  = 0x17FC0076;
static constexpr uint32_t ECU_FC0076_RESP = 0x17FE0076;
// 11-Bit standard IDs
static constexpr uint32_t ECU_710_REQ     = 0x710;
static constexpr uint32_t ECU_710_RESP    = 0x77A;
static constexpr uint32_t ECU_746_REQ     = 0x746;
static constexpr uint32_t ECU_746_RESP    = 0x7B0;

// ── UDS-Query Wrapper ────────────────────────────────────────
// Sendet UDS 0x22 DID-Anfrage, gibt Datenbytes in out[] zurück.
// A=out[0], B=out[1], ... (nach 0x62+DID_H+DID_L).
// Rückgabe: Anzahl Datenbytes oder -1 bei Fehler.
static int query_did(uint32_t req_id, uint32_t resp_id, uint16_t did,
                     uint8_t* out, uint16_t max_out) {
    uint8_t req[3]   = { 0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };
    uint8_t resp[64] = {};
    int n = can_isotp_query(req_id, resp_id, req, 3, resp, sizeof(resp), UDS_TIMEOUT_MS);
    if (n < 4)         return -1;   // mind. 0x62 + DID_H + DID_L + 1 Datenbyte
    if (resp[0] != 0x62) return -1; // Negativ-Response oder Protokollfehler
    int data_len = n - 3;           // 3 Header-Bytes (0x62, DID_H, DID_L) abziehen
    if (data_len > (int)max_out) data_len = (int)max_out;
    memcpy(out, resp + 3, data_len);
    return data_len;
}

// ── Plausibilitätsfilter ─────────────────────────────────────
static inline bool plausible(float v, float lo, float hi) {
    return (v >= lo && v <= hi);
}

// ============================================================
//  Interner Zustand
// ============================================================

static TelemetryPoint  s_cache[TELEM_FIELD_COUNT];   // letzter Wert pro Feld
static uint32_t        s_last_sent[TELEM_FIELD_COUNT]; // letzter Sendezeitpunkt (SPIFFS-Persistenz)
static SemaphoreHandle_t s_mutex = NULL;

static char  s_operator_str[32] = "";
static bool  s_influx_ok        = false;

// ── ig-Hysterese: 1→0 erst nach 60s ohne ext. GPS ───────────
static uint8_t   s_ig_value       = 0;       // aktuell gesendeter ig-Wert
static uint32_t  s_ig_loss_ms     = 0;       // Zeitpunkt des letzten ext-GPS-Verlusts
static const uint32_t IG_HYSTERESIS_MS = 60000UL;

// ── Zeilen-Ringpuffer (GPS-getriggert, PSRAM) ───────────────
static TelemetryRow* s_row_buf = nullptr;   // allokiert in telem_init()
static uint16_t     s_row_head      = 0;   // nächste Schreibposition
static uint16_t     s_row_send_tail = 0;   // älteste ungesendete Zeile
static uint16_t     s_row_pending   = 0;   // ausstehende (ungesendete) Zeilen

// GPS-Capture-State (Vergleichsbasis für Trigger-Schwellen)
static double   s_cap_lat = 0.0;
static double   s_cap_lon = 0.0;
static uint32_t s_cap_ms  = 0;
static float    s_yaw_peak           = 0.0f;   // max |yaw_dps| seit letztem Capture-Tick
static uint32_t s_curve_cooldown_ms  = 0;      // Kurven-Cooldown: Zeitpunkt letzter Kurven-Capture

// ── Delta-Kompression: nur geänderte Werte senden ──────────
static float s_prev_val[TELEM_FIELD_COUNT];  // letzte gesendete Werte
static bool  s_prev_has[TELEM_FIELD_COUNT];  // true = Feld wurde min. 1× gesendet

// Max-Alter bevor ein Wert als veraltet gilt (0 = kein Limit / immer senden)
// Faustregel: 3× Poll-Intervall
static const uint32_t s_max_age_ms[TELEM_FIELD_COUNT] = {
    90000,   // SOC          (30s poll)
    15000,   // VOLTAGE      (5s poll)
    15000,   // CURRENT      (5s poll)
    15000,   // POWER        (berechnet aus V×I)
    15000,   // SPEED        (5s poll)
    15000,   // IS_CHARGING  (5s — Status-Bit)
    15000,   // IS_DCFC      (5s — Status-Bit)
    45000,   // BATT_TEMP    (15s poll)
    45000,   // EXT_TEMP     (15s poll)
    45000,   // RANGE        (15s poll)
    0,       // GPS_LAT      (Trigger — immer)
    0,       // GPS_LON      (Trigger — immer)
    0,       // GPS_HEADING  (berechnet — immer)
    0,       // GPS_VALID    (nicht gesendet)
    45000,   // GYRO_G       (15s sensor)
    45000,   // LTE_SIGNAL   (15s sensor)
    0,       // LTE_OPERATOR (nicht gesendet)
    180000,  // CAPACITY     (60s poll)
    180000,  // KWH_CHARGED  (60s poll)
    90000,   // IS_PARKED    (30s poll)
    180000,  // ODOMETER     (60s poll)
    180000,  // BATT_DEVICE  (60s sensor)
};

// Mindest-Änderung damit ein Wert gesendet wird (0 = bei jeder Änderung)
static const float s_min_delta[TELEM_FIELD_COUNT] = {
    0.3f,    // SOC (%)
    0.5f,    // VOLTAGE (V)
    0.5f,    // CURRENT (A)
    0.1f,    // POWER (kW)
    0.5f,    // SPEED (km/h)
    0.0f,    // IS_CHARGING (bool)
    0.0f,    // IS_DCFC (bool)
    0.3f,    // BATT_TEMP (°C)
    0.3f,    // EXT_TEMP (°C)
    1.0f,    // RANGE (km)
    0.0f,    // GPS_LAT (immer)
    0.0f,    // GPS_LON (immer)
    0.0f,    // GPS_HEADING (immer)
    0.0f,    // GPS_VALID
    0.005f,  // GYRO_G
    0.0f,    // LTE_SIGNAL (int)
    0.0f,    // LTE_OPERATOR
    0.5f,    // CAPACITY (kWh)
    0.5f,    // KWH_CHARGED (kWh)
    0.0f,    // IS_PARKED (bool)
    0.5f,    // ODOMETER (km)
    0.0f,    // BATT_DEVICE (int %)
};

// ── Geo-Hilfsfunktionen ──────────────────────────────────────
// Haversine-Distanz in Metern zwischen zwei GPS-Koordinaten
static float haversine_m(double lat1, double lon1, double lat2, double lon2) {
    static const double R = 6371000.0;
    double dlat = (lat2 - lat1) * (M_PI / 180.0);
    double dlon = (lon2 - lon1) * (M_PI / 180.0);
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * (M_PI / 180.0)) * cos(lat2 * (M_PI / 180.0)) *
               sin(dlon / 2) * sin(dlon / 2);
    return (float)(R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));
}

// Kurs in Grad (0–360) von Punkt 1 → Punkt 2
static float bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double dlon  = (lon2 - lon1) * (M_PI / 180.0);
    double lat1r = lat1 * (M_PI / 180.0);
    double lat2r = lat2 * (M_PI / 180.0);
    double y = sin(dlon) * cos(lat2r);
    double x = cos(lat1r) * sin(lat2r) - sin(lat1r) * cos(lat2r) * cos(dlon);
    float  h = (float)(atan2(y, x) * (180.0 / M_PI));
    if (h < 0.0f) h += 360.0f;
    return h;
}

// ── GPS-getriggerter Zeilen-Capture ─────────────────────────
// Wird alle 3s aus telem_task aufgerufen.
// Sequentielle Prüfung: Distanz → Yaw-Peak → Zeit (first-match wins).
// s_yaw_peak wird im 1s-Loop akkumuliert und hier zurückgesetzt.
static void row_try_capture() {
    if (!gps_valid()) { s_yaw_peak = 0.0f; return; }

    double   lat     = gps_lat();
    double   lon     = gps_lon();
    uint32_t now     = millis();
    float    dist    = (s_cap_ms == 0) ? 9999.0f
                     : haversine_m(s_cap_lat, s_cap_lon, lat, lon);
    uint32_t elapsed = (s_cap_ms == 0) ? 99999UL : (now - s_cap_ms);

    // Geschwindigkeitsabhängige Distanzschwelle
    float spd = g_gps.speed_kmh;
    float dist_thresh = (spd > 110.0f) ? 250.0f :
                        (spd >  80.0f) ? 200.0f :
                        (spd >  50.0f) ? 150.0f : 100.0f;

    bool        do_capture = false;
    const char* reason     = "";

    // Kurven-Trigger hat Vorrang — unterbricht Distanz-Akkumulation
    if (spd >= TELEM_GPS_MIN_SPEED_KMH) {
        bool cooldown_ok = (now - s_curve_cooldown_ms) >= TELEM_CURVE_COOLDOWN_MS;
        if (s_yaw_peak >= (float)TELEM_YAW_TURN_DPS && cooldown_ok) {
            do_capture = true; reason = "Kurve";
            s_curve_cooldown_ms = now;
        }
    }
    if (!do_capture && dist >= dist_thresh) {
        do_capture = true; reason = "Distanz";
    }
    if (!do_capture && elapsed >= TELEM_GPS_MAX_INTERVAL_MS && g_gps.speed_kmh >= TELEM_GPS_MIN_SPEED_KMH) {
        do_capture = true; reason = "Zeit";
    }

    // Peak zurücksetzen
    s_yaw_peak = 0.0f;

    if (!do_capture) return;

    // RTC-Zeitstempel erforderlich (InfluxDB braucht gültigen Timestamp)
    uint64_t unix_s = rtc_unix_ms() / 1000ULL;
    if (unix_s == 0) return;

    // ── Zeile unter Mutex in Ringpuffer schreiben ──────────
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    TelemetryRow row;
    row.unix_s  = (uint32_t)unix_s;
    row.eq_mask = 0;
    row.na_mask = 0;
    if (pmu_is_vbus_in()) {
        s_ig_value   = 1;
        s_ig_loss_ms = 0;
    } else if (s_ig_value == 1) {
        if (s_ig_loss_ms == 0) s_ig_loss_ms = millis();
        if (millis() - s_ig_loss_ms >= IG_HYSTERESIS_MS) s_ig_value = 0;
    }
    row.ig = s_ig_value;

    for (int f = 0; f < TELEM_FIELD_COUNT; f++) {
        float val = s_cache[f].value;
        bool  ok  = s_cache[f].valid && (s_cache[f].timestamp_ms > 0);

        // Max-Age: veraltete Werte → na_mask
        if (ok && s_max_age_ms[f] > 0) {
            uint32_t age = now - s_cache[f].timestamp_ms;
            if (age > s_max_age_ms[f]) ok = false;
        }

        if (!ok) {
            row.valid[f]  = false;
            row.values[f] = 0;
            row.na_mask  |= (1UL << f);
        } else if (s_prev_has[f] && fabsf(val - s_prev_val[f]) <= s_min_delta[f]) {
            // Unverändert → eq_mask
            row.valid[f]  = false;
            row.values[f] = val;
            row.eq_mask  |= (1UL << f);
        } else {
            // Neuer/geänderter Wert → senden
            row.valid[f]  = true;
            row.values[f] = val;
        }
    }

    // GPS frisch überschreiben (Trigger — immer senden, nie in Masks)
    row.values[TELEM_GPS_LAT] = (float)lat;
    row.values[TELEM_GPS_LON] = (float)lon;
    row.valid[TELEM_GPS_LAT]  = true;
    row.valid[TELEM_GPS_LON]  = true;
    row.eq_mask &= ~((1UL << TELEM_GPS_LAT) | (1UL << TELEM_GPS_LON));
    row.na_mask &= ~((1UL << TELEM_GPS_LAT) | (1UL << TELEM_GPS_LON));
    // Heading: GPS Course-Over-Ground (GNRMC Feld 8) — zuverlässiger als Kompass
    if (g_gps.speed_kmh >= TELEM_GPS_MIN_SPEED_KMH) {
        row.values[TELEM_GPS_HEADING] = g_gps.course_deg;
        row.valid[TELEM_GPS_HEADING]  = true;
        row.eq_mask &= ~(1UL << TELEM_GPS_HEADING);
        row.na_mask &= ~(1UL << TELEM_GPS_HEADING);
    }

    // Prev-Werte aktualisieren für nächsten Delta-Vergleich
    for (int f = 0; f < TELEM_FIELD_COUNT; f++) {
        if (row.valid[f]) {
            s_prev_val[f] = row.values[f];
            s_prev_has[f] = true;
        }
    }

    s_row_buf[s_row_head] = row;
    s_row_head = (uint16_t)((s_row_head + 1) % TELEM_ROW_BUF_SIZE);
    if (s_row_pending < TELEM_ROW_BUF_SIZE) {
        s_row_pending++;
    } else {
        // Puffer voll → ältesten ungesendeten Punkt verwerfen
        s_row_send_tail = (uint16_t)((s_row_send_tail + 1) % TELEM_ROW_BUF_SIZE);
    }
    xSemaphoreGive(s_mutex);

    s_cap_lat = lat;
    s_cap_lon = lon;
    s_cap_ms  = now;
    {
        char _msg[64];
        snprintf(_msg, sizeof(_msg), "!!!! GPS-Capture: %s (%.0fm, %lus) · %u ausstehend",
                 reason, dist, (unsigned long)(elapsed / 1000UL), s_row_pending);
        syslog("TELEM", _msg);
    }
}

// ── Erzwungener Capture (Fahrtende / extern) ─────────────────
void telem_force_capture(const char* reason, bool force_ig_off) {
    if (!gps_valid()) return;
    if (force_ig_off) {
        s_ig_value   = 0;
        s_ig_loss_ms = 0;
    }
    // Zeit- und Distanz-Schwellen überspringen — direkt in row_try_capture einsteigen
    // indem wir s_cap_ms auf 0 setzen (gilt als "erster Fix")
    s_cap_ms = 0;
    row_try_capture();  // wird sofort feuern weil dist = 9999 (s_cap_ms == 0)
    syslog("TELEM", reason);
}

// ── Wert aktualisieren ──────────────────────────────────────
static void telem_update(TelemField field, float value, bool valid) {
    uint32_t now = millis();
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_cache[field].timestamp_ms = now;
        s_cache[field].value        = value;
        s_cache[field].valid        = valid;
        xSemaphoreGive(s_mutex);
    }
}

static TaskHandle_t s_telem_task_handle = nullptr;

// ── CAN-DID Scheduler: max. 1 Abfrage pro Sekunde, gestaffelt ──
struct CanQuerySlot {
    uint32_t req_id;
    uint32_t resp_id;
    uint16_t did;
    uint32_t interval_ms;
    uint32_t last_ms;
};

// Dekodierung: DID-Rohdaten → Telemetrie-Cache
static void decode_did_response(uint16_t did, const uint8_t* d, int n) {
    switch (did) {
        case 0x1E3B:  // Spannung: A:B / 4
            if (n >= 2) {
                float v = (d[0] * 256u + d[1]) / 4.f;
                telem_update(TELEM_VOLTAGE, v, plausible(v, 300.f, 500.f));
            }
            break;
        case 0x1E3D:  // Strom: -((INT32 - 150000) / 100)
            if (n >= 4) {
                int32_t raw = ((int32_t)d[0] << 24) | ((int32_t)d[1] << 16) |
                              ((int32_t)d[2] << 8)  |  (int32_t)d[3];
                float v = -((raw - 150000) / 100.f);
                telem_update(TELEM_CURRENT, v, plausible(v, -2000.f, 2000.f));
            }
            break;
        case 0xF40D:  // Geschwindigkeit: A
            if (n >= 1) {
                telem_update(TELEM_VEHICLE_SPEED, (float)d[0], plausible((float)d[0], 0.f, 200.f));
            }
            break;
        case 0x2A0B:  // Batterie-Temperatur: A/2 - 40
            if (n >= 1) {
                float v = d[0] / 2.f - 40.f;
                telem_update(TELEM_BATT_TEMP, v, plausible(v, -40.f, 80.f));
            }
            break;
        case 0x2609:  // Außentemperatur: A/2 - 50
            if (n >= 1) {
                float v = d[0] / 2.f - 50.f;
                telem_update(TELEM_EXT_TEMP, v, plausible(v, -40.f, 80.f));
            }
            break;
        case 0x0295:  // Reichweite: A:B
            if (n >= 2) {
                float v = (float)((d[0] << 8) | d[1]);
                telem_update(TELEM_RANGE, v, plausible(v, 0.f, 600.f));
            }
            break;
        case 0x028C:  // SoC: A * 0.4425 - 6.1947
            if (n >= 1) {
                float v = d[0] * 0.4425f - 6.1947f;
                telem_update(TELEM_SOC, v, plausible(v, -5.f, 105.f));
            }
            break;
        case 0x210E:  // Geparkt: B == 8
            if (n >= 2) {
                telem_update(TELEM_IS_PARKED, (d[1] == 8) ? 1.f : 0.f, true);
            }
            break;
        case 0x295A:  // Odometer: A:B:C (24-bit)
            if (n >= 3) {
                float v = (float)(((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2]);
                telem_update(TELEM_ODOMETER, v, plausible(v, 0.f, 999999.f));
            }
            break;
        case 0x2AB2:  // Kapazität: INT16 * 50/1000
            if (n >= 2) {
                float v = (float)((int16_t)((d[0] << 8) | d[1])) * 50.f / 1000.f;
                telem_update(TELEM_CAPACITY, v, plausible(v, 1.f, 200.f));
            }
            break;
        case 0x1E32:  // Geladen kWh: INT32[8:11] / 8583.07
            if (n >= 12) {
                int32_t raw = ((int32_t)d[8]  << 24) | ((int32_t)d[9]  << 16) |
                              ((int32_t)d[10] << 8)  |  (int32_t)d[11];
                float v = raw / 8583.07123641215f;
                telem_update(TELEM_KWH_CHARGED, v, plausible(v, 0.f, 99999999.f));
            }
            break;
        case 0x7448:  // Ladestatus: Bit 2 = charging, Bit 1+2 = dcfc
            if (n >= 1) {
                telem_update(TELEM_IS_CHARGING, (d[0] & 0x04) ? 1.f : 0.f, true);
                telem_update(TELEM_IS_DCFC,     (d[0] & 0x06) == 0x06 ? 1.f : 0.f, true);
            }
            break;
    }

    // Leistung nachberechnen wenn Spannung oder Strom aktualisiert
    if (did == 0x1E3B || did == 0x1E3D) {
        float volt_val = 0, curr_val = 0;
        uint32_t age = 0;
        bool v_ok = telem_get_latest(TELEM_VOLTAGE, &volt_val, &age);
        bool c_ok = telem_get_latest(TELEM_CURRENT, &curr_val, &age);
        if (v_ok && c_ok) {
            float kw = volt_val * curr_val / 1000.f;
            telem_update(TELEM_POWER, kw, plausible(kw, -500.f, 500.f));
        }
    }
}

// Scheduler-Tabelle: 12 CAN-DIDs, gestaffelt abgefragt.
// Reihenfolge bestimmt Startversatz (1s pro Eintrag beim Boot).
static CanQuerySlot s_can_schedule[] = {
    // ── Schnell (5s): Spannung, Strom, Speed ──
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0x1E3B,  5000, 0 },  // [0] Spannung
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0x1E3D,  5000, 0 },  // [1] Strom
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0xF40D,  5000, 0 },  // [2] Speed
    // ── Mittel (15s): Batt-Temp, Außen-Temp, Reichweite ──
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0x2A0B, 15000, 0 },  // [3] Batt-Temp
    { ECU_746_REQ,    ECU_746_RESP,    0x2609, 15000, 0 },   // [4] Außen-Temp
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0x0295, 15000, 0 },   // [5] Reichweite
    // ── Langsam (30s): SoC, Geparkt ──
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0x028C, 30000, 0 },   // [6] SoC
    { ECU_FC0076_REQ, ECU_FC0076_RESP, 0x210E, 30000, 0 },   // [7] Geparkt
    // ── Sehr langsam (60s): Odometer, Kapazität, kWh, Ladestatus ──
    { ECU_FC0076_REQ, ECU_FC0076_RESP, 0x295A, 60000, 0 },   // [8]  Odometer
    { ECU_710_REQ,    ECU_710_RESP,    0x2AB2, 60000, 0 },    // [9]  Kapazität
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0x1E32, 60000, 0 },   // [10] kWh geladen
    { ECU_FC007B_REQ, ECU_FC007B_RESP, 0x7448, 60000, 0 },   // [11] Ladestatus
};
static constexpr int CAN_SCHEDULE_COUNT = sizeof(s_can_schedule) / sizeof(s_can_schedule[0]);

// ── FreeRTOS-Task ────────────────────────────────────────────
static void telem_task(void* /*param*/) {
    // Erste Abfrage sofort nach 2 s (Modem-Init abwarten)
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Startversatz: jede DID 1 s nach der vorherigen fällig → kein Burst beim Boot
    uint32_t now = millis();
    for (int i = 0; i < CAN_SCHEDULE_COUNT; i++) {
        s_can_schedule[i].last_ms = now - s_can_schedule[i].interval_ms + (i * 1000UL);
    }

    uint32_t last_sensor_ms  = 0;   // GPS, Gyro, LTE (alle 15 s)
    uint32_t last_pmu_ms     = 0;   // PMU-Akku (alle 60 s)
    uint32_t last_gps_cap_ms = 0;   // GPS-Capture (alle 3 s)

    while (!g_shutdown) {
        now = millis();

        // ── CAN: max. 1 Abfrage pro Tick (1 s) ──────────────
        // Die am meisten überfällige DID wird zuerst bedient.
        if (can_hw_ok() && !scan_running && !monitor_mode) {
            int best = -1;
            uint32_t best_overdue = 0;
            for (int i = 0; i < CAN_SCHEDULE_COUNT; i++) {
                uint32_t elapsed = now - s_can_schedule[i].last_ms;
                if (elapsed >= s_can_schedule[i].interval_ms) {
                    uint32_t overdue = elapsed - s_can_schedule[i].interval_ms;
                    if (best < 0 || overdue > best_overdue) {
                        best = i;
                        best_overdue = overdue;
                    }
                }
            }
            if (best >= 0) {
                uint8_t d[32];
                int n = query_did(s_can_schedule[best].req_id,
                                  s_can_schedule[best].resp_id,
                                  s_can_schedule[best].did,
                                  d, sizeof(d));
                decode_did_response(s_can_schedule[best].did, d, n);
                s_can_schedule[best].last_ms = now;
            }
        }

        // ── Sensoren (kein CAN): alle 15 s ──────────────────
        if (now - last_sensor_ms >= 15000UL) {
            // GPS
            bool gv = gps_valid();
            telem_update(TELEM_GPS_LAT,   (float)gps_lat(), gv);
            telem_update(TELEM_GPS_LON,   (float)gps_lon(), gv);
            telem_update(TELEM_GPS_VALID, gv ? 1.0f : 0.0f, true);

            // Gyro
            bool gyro_avail = gyro_ok();
            telem_update(TELEM_GYRO_G, gyro_get_accel_g(), gyro_avail);

            // LTE-Signal
            int8_t sig = modem_signal_quality();
            bool sig_valid = (sig >= 0 && sig != 99);
            telem_update(TELEM_LTE_SIGNAL, sig_valid ? (float)sig : 0.0f, sig_valid);

            // LTE-Operator
            {
                const char* op = modem_operator();
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    strncpy(s_operator_str, op ? op : "", sizeof(s_operator_str) - 1);
                    s_operator_str[sizeof(s_operator_str) - 1] = '\0';
                    uint32_t ts = millis();
                    s_cache[TELEM_LTE_OPERATOR].timestamp_ms = ts;
                    s_cache[TELEM_LTE_OPERATOR].value        = 0.0f;
                    s_cache[TELEM_LTE_OPERATOR].valid        = true;
                    xSemaphoreGive(s_mutex);
                }
            }

            last_sensor_ms = now;
        }

        // ── PMU-Akku: alle 60 s ─────────────────────────────
        if (now - last_pmu_ms >= 60000UL) {
            int batt = pmu_batt_pct();
            telem_update(TELEM_BATT_DEVICE, (float)batt, batt >= 0);
            last_pmu_ms = now;
        }

        // Yaw-Peak jede Sekunde akkumulieren (für 3s-Capture-Tick)
        {
            float y = fabsf(gyro_get_yaw_dps());
            if (y > s_yaw_peak) s_yaw_peak = y;
        }
        // (COG-Kurvenerkennung entfernt — GPS-Drift erzeugt false positives im Stand)

        // GPS-Capture: extern 1s (M10 liefert 1Hz), intern 3s (SIM7080G 5s-Takt)
        // Bei Fahrtende (VBUS weg) keine Captures mehr — nur Kompass-Wackler
        if (!g_trip_ending) {
            uint32_t cap_interval = gps_ext_ok() ? 1000UL : 3000UL;
            if (now - last_gps_cap_ms >= cap_interval) {
                row_try_capture();
                last_gps_cap_ms = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    syslog("TELEM", "Task beendet (Shutdown)");
    vTaskDelete(NULL);
}

// ============================================================
//  Öffentliche API
// ============================================================

void telem_init() {
    s_mutex = xSemaphoreCreateMutex();
    memset(s_cache,    0, sizeof(s_cache));
    memset(s_last_sent, 0, sizeof(s_last_sent));
    // Zeilen-Ringpuffer in PSRAM allokieren (spart ~57 KB internes RAM)
    s_row_buf = (TelemetryRow*)ps_calloc(TELEM_ROW_BUF_SIZE, sizeof(TelemetryRow));
    if (!s_row_buf) {
        syslog("TELEM", "PSRAM alloc FEHLER — Fallback internes RAM");
        s_row_buf = (TelemetryRow*)calloc(TELEM_ROW_BUF_SIZE, sizeof(TelemetryRow));
    }
    s_row_head      = 0;
    s_row_send_tail = 0;
    s_row_pending   = 0;
    s_cap_lat       = 0.0;
    s_cap_lon       = 0.0;
    s_cap_ms        = 0;
    // Alle Felder initial als ungültig markieren
    for (int i = 0; i < TELEM_FIELD_COUNT; i++) {
        s_cache[i].valid = false;
    }
    s_influx_ok = false;
    syslog("TELEM", "init OK");
}

void telem_start_task() {
    xTaskCreatePinnedToCore(telem_task, "TELEM", 6144, NULL, 1, &s_telem_task_handle, 0);
    syslog("TELEM", "Task gestartet");
}

void telem_stop_task() {
    TaskHandle_t h = xTaskGetHandle("TELEM");
    if (h) vTaskDelete(h);
    s_telem_task_handle = nullptr;
}

bool telem_get_latest(TelemField field, float* out_value, uint32_t* out_age_ms) {
    if (field >= TELEM_FIELD_COUNT || !s_mutex) return false;
    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        const TelemetryPoint& p = s_cache[field];
        if (p.timestamp_ms > 0) {
            if (out_value)  *out_value  = p.value;
            if (out_age_ms) *out_age_ms = millis() - p.timestamp_ms;
            ok = p.valid;
        } else {
            // Feld wurde noch nie abgefragt — Sentinel-Wert
            if (out_value)  *out_value  = 0.0f;
            if (out_age_ms) *out_age_ms = 0xFFFFFFFFUL;
        }
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

const char* telem_get_operator_str() {
    static char buf[24];
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        memcpy(buf, s_operator_str, sizeof(buf));
        xSemaphoreGive(s_mutex);
    }
    return buf;
}

uint16_t telem_get_buf_pending() {
    return telem_get_row_pending();  // Legacy-API → Zeilen-Puffer
}

uint16_t telem_get_row_pending() {
    uint16_t n = 0;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        n = s_row_pending;
        xSemaphoreGive(s_mutex);
    }
    return n;
}

bool telem_influx_ok() {
    return s_influx_ok;
}

// ── Debug-Preview: letzte N gepufferte Zeilen als lesbarer Text ──
// Zeigt Timestamp + Kernwerte, damit man im Debug-Tab sieht was gebuffert wird.
const char* telem_preview_rows(int max_rows) {
    static char buf[2048];
    int pos = 0;

    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        snprintf(buf, sizeof(buf), "Mutex belegt");
        return buf;
    }

    uint16_t count = (s_row_pending < (uint16_t)max_rows) ? s_row_pending : (uint16_t)max_rows;
    if (count == 0) {
        xSemaphoreGive(s_mutex);
        snprintf(buf, sizeof(buf), "Keine Zeilen gepuffert");
        return buf;
    }

    // Die neuesten Zeilen anzeigen (rückwärts vom head)
    for (int i = count - 1; i >= 0 && pos < (int)sizeof(buf) - 200; i--) {
        uint16_t idx = (uint16_t)((s_row_head - 1 - i + TELEM_ROW_BUF_SIZE) % TELEM_ROW_BUF_SIZE);
        const TelemetryRow& row = s_row_buf[idx];

        // Timestamp als HH:MM:SS (aus unix_s)
        uint32_t tod = row.unix_s % 86400;
        int h = tod / 3600, m = (tod % 3600) / 60, s = tod % 60;

        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02d:%02d:%02d ", h, m, s);

        // Kernwerte anzeigen wenn gültig
        if (row.valid[TELEM_SOC])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "SoC=%.1f%% ", row.values[TELEM_SOC]);
        if (row.valid[TELEM_VOLTAGE])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "U=%.0fV ", row.values[TELEM_VOLTAGE]);
        if (row.valid[TELEM_CURRENT])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "I=%.1fA ", row.values[TELEM_CURRENT]);
        if (row.valid[TELEM_POWER])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "P=%.1fkW ", row.values[TELEM_POWER]);
        if (row.valid[TELEM_VEHICLE_SPEED])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "v=%.0fkm/h ", row.values[TELEM_VEHICLE_SPEED]);
        if (row.valid[TELEM_GPS_LAT] && row.valid[TELEM_GPS_LON])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "GPS=%.4f,%.4f ",
                            row.values[TELEM_GPS_LAT], row.values[TELEM_GPS_LON]);
        if (row.valid[TELEM_GPS_HEADING])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "HD=%.0f° ", row.values[TELEM_GPS_HEADING]);
        if (row.valid[TELEM_BATT_TEMP])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "BT=%.0f°C ", row.values[TELEM_BATT_TEMP]);
        if (row.valid[TELEM_EXT_TEMP])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "ET=%.0f°C ", row.values[TELEM_EXT_TEMP]);
        if (row.valid[TELEM_RANGE])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "R=%.0fkm ", row.values[TELEM_RANGE]);
        if (row.valid[TELEM_GYRO_G])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "G=%.2f ", row.values[TELEM_GYRO_G]);
        if (row.valid[TELEM_IS_CHARGING] && row.values[TELEM_IS_CHARGING] > 0.5f)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "AC ");
        if (row.valid[TELEM_IS_DCFC] && row.values[TELEM_IS_DCFC] > 0.5f)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "DC ");
        if (row.valid[TELEM_IS_PARKED] && row.values[TELEM_IS_PARKED] > 0.5f)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "P ");
        if (row.valid[TELEM_ODOMETER])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "ODO=%.0f ", row.values[TELEM_ODOMETER]);
        if (row.valid[TELEM_LTE_SIGNAL])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "LTE=%ddBm ", (int)row.values[TELEM_LTE_SIGNAL]);
        if (row.valid[TELEM_BATT_DEVICE])
            pos += snprintf(buf + pos, sizeof(buf) - pos, "BD=%d%% ", (int)row.values[TELEM_BATT_DEVICE]);

        if (pos < (int)sizeof(buf) - 2) buf[pos++] = '\n';
    }

    xSemaphoreGive(s_mutex);
    buf[pos] = '\0';
    return buf;
}

// ============================================================
//  InfluxDB v2 Sender — entfernt (ersetzt durch MQTT)
//  Siehe mod_mqtt.cpp für das neue Sendeformat.
// ============================================================

void telem_send_influx() {
    // InfluxDB entfernt — Daten gehen jetzt via MQTT (mod_mqtt.cpp)
}

// ============================================================
//  SPIFFS-Persistenz
// ============================================================

#define TELEM_PERSIST_FILE  "/telem.bin"

// ============================================================
//  MQTT: älteste ungesendete Zeile holen
// ============================================================

// ── MQTT: älteste ungesendete Zeile lesen (ohne entfernen) ──
bool telem_peek_row(TelemetryRow& out) {
    if (!s_mutex) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    if (s_row_pending == 0) {
        xSemaphoreGive(s_mutex);
        return false;
    }

    out = s_row_buf[s_row_send_tail];
    xSemaphoreGive(s_mutex);
    return true;
}

// ── MQTT: älteste Zeile als gesendet bestätigen ─────────────
void telem_ack_row() {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    if (s_row_pending > 0) {
        s_row_send_tail = (uint16_t)((s_row_send_tail + 1) % TELEM_ROW_BUF_SIZE);
        s_row_pending--;
    }

    xSemaphoreGive(s_mutex);
}

// ── Legacy: pop = peek + ack in einem (für pre-sleep flush) ─
bool telem_pop_row(TelemetryRow& out) {
    if (!telem_peek_row(out)) return false;
    telem_ack_row();
    return true;
}

#define TELEM_ROWS_FILE     "/telem_rows.bin"
#define TELEM_PERSIST_MAGIC 0x544C4D02UL  // 'TLM' + Version 2 (eq_mask/na_mask)
#define TELEM_ROWS_MAGIC    0x544C5201UL  // 'TLR' + Version 1

struct PersistHeader {
    uint32_t magic;
    uint32_t save_millis;    // millis() beim Speichern
    uint32_t ms_of_day;      // rtc_now_ms_of_day() beim Speichern
    uint32_t field_count;    // Sanity-Check = TELEM_FIELD_COUNT
};

void telem_persist_to_spiffs() {
    if (!s_mutex) return;

    // Cache + last_sent_ms snapshot
    TelemetryPoint   cache_snap[TELEM_FIELD_COUNT];
    uint32_t         last_sent[TELEM_FIELD_COUNT];

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    memcpy(cache_snap, s_cache, sizeof(cache_snap));
    for (int i = 0; i < TELEM_FIELD_COUNT; i++)
        last_sent[i] = s_last_sent[i];
    xSemaphoreGive(s_mutex);

    PersistHeader hdr = {
        TELEM_PERSIST_MAGIC,
        millis(),
        rtc_now_ms_of_day(),
        (uint32_t)TELEM_FIELD_COUNT
    };

    File f = SPIFFS.open(TELEM_PERSIST_FILE, "w");
    if (!f) {
        syslog("TELEM", "SPIFFS persist FEHLER: Datei nicht öffenbar");
        return;
    }
    f.write((uint8_t*)&hdr,        sizeof(hdr));
    f.write((uint8_t*)cache_snap,  sizeof(cache_snap));
    f.write((uint8_t*)last_sent,   sizeof(last_sent));
    f.close();

    syslog("TELEM", "SPIFFS persist OK → /telem.bin");

    // ── Ungesendete Zeilen persistieren ──────────────────────────
    uint16_t pending = 0;
    uint16_t tail    = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        pending = s_row_pending;
        tail    = s_row_send_tail;
        xSemaphoreGive(s_mutex);
    }
    if (pending > 0 && s_row_buf) {
        File fr = SPIFFS.open(TELEM_ROWS_FILE, "w");
        if (fr) {
            uint32_t rows_magic = TELEM_ROWS_MAGIC;
            fr.write((uint8_t*)&rows_magic, 4);
            fr.write((uint8_t*)&pending, 2);
            for (uint16_t i = 0; i < pending; i++) {
                uint16_t idx = (uint16_t)((tail + i) % TELEM_ROW_BUF_SIZE);
                fr.write((uint8_t*)&s_row_buf[idx], sizeof(TelemetryRow));
            }
            fr.close();
            char msg[48];
            snprintf(msg, sizeof(msg), "Rows persist: %u Zeilen", pending);
            syslog("TELEM", msg);
        }
    }
}

void telem_restore_from_spiffs() {
    if (!SPIFFS.exists(TELEM_PERSIST_FILE)) return;  // Kein Persist-File — normaler Start
    File f = SPIFFS.open(TELEM_PERSIST_FILE, "r");
    if (!f) return;

    PersistHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
        hdr.magic       != TELEM_PERSIST_MAGIC ||
        hdr.field_count != (uint32_t)TELEM_FIELD_COUNT) {
        f.close();
        SPIFFS.remove(TELEM_PERSIST_FILE);
        return;
    }

    TelemetryPoint cache_snap[TELEM_FIELD_COUNT];
    uint32_t       last_sent[TELEM_FIELD_COUNT];

    if (f.read((uint8_t*)cache_snap, sizeof(cache_snap)) != sizeof(cache_snap) ||
        f.read((uint8_t*)last_sent,  sizeof(last_sent))  != sizeof(last_sent)) {
        f.close();
        SPIFFS.remove(TELEM_PERSIST_FILE);
        syslog("TELEM", "SPIFFS restore: Lesefehler, Datei gelöscht");
        return;
    }
    f.close();
    SPIFFS.remove(TELEM_PERSIST_FILE);  // Nach Lesen sofort löschen (kein Doppel-Restore)

    // Timestamps in neuen millis()-Raum umrechnen
    // elapsed_ms = Zeit zwischen persist und jetzt (RTC-basiert)
    uint32_t now_ms       = millis();
    uint32_t new_msofday  = rtc_now_ms_of_day();
    uint32_t elapsed_ms;
    if (new_msofday >= hdr.ms_of_day) {
        elapsed_ms = new_msofday - hdr.ms_of_day;
    } else {
        // Mitternacht übersprungen
        elapsed_ms = (86400000UL - hdr.ms_of_day) + new_msofday;
    }
    // Sanity: maximal 7 Tage, sonst ist die RTC-Zeit falsch
    if (elapsed_ms > 7UL * 86400000UL) elapsed_ms = 0;

    for (int i = 0; i < TELEM_FIELD_COUNT; i++) {
        if (cache_snap[i].timestamp_ms == 0) continue;
        // Underflow-Schutz: timestamp darf nicht jünger als Speicherzeitpunkt sein
        if (cache_snap[i].timestamp_ms > hdr.save_millis) {
            cache_snap[i].timestamp_ms = 0;
            continue;
        }
        uint32_t age_at_save = hdr.save_millis - cache_snap[i].timestamp_ms;
        cache_snap[i].timestamp_ms = now_ms - (elapsed_ms + age_at_save);

        if (last_sent[i] != 0) {
            uint32_t sent_age = hdr.save_millis - last_sent[i];
            last_sent[i] = now_ms - (elapsed_ms + sent_age);
        }
    }

    // GPS-Felder aus Restore löschen — alte Koordinaten dürfen nie in neue Trip-Daten einfließen
    cache_snap[TELEM_GPS_LAT].valid        = false;
    cache_snap[TELEM_GPS_LAT].timestamp_ms = 0;
    cache_snap[TELEM_GPS_LON].valid        = false;
    cache_snap[TELEM_GPS_LON].timestamp_ms = 0;
    cache_snap[TELEM_GPS_HEADING].valid        = false;
    cache_snap[TELEM_GPS_HEADING].timestamp_ms = 0;

    // In Cache + Buffer schreiben
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(s_cache, cache_snap, sizeof(s_cache));
        for (int i = 0; i < TELEM_FIELD_COUNT; i++)
            s_last_sent[i] = last_sent[i];
        // prev_val für GPS zurücksetzen — kein Delta-Vergleich mit alten Koordinaten
        s_prev_has[TELEM_GPS_LAT]     = false;
        s_prev_has[TELEM_GPS_LON]     = false;
        s_prev_has[TELEM_GPS_HEADING] = false;
        xSemaphoreGive(s_mutex);
    }

    uint32_t elapsed_min = elapsed_ms / 60000UL;
    uint32_t eh = elapsed_min / 60;
    uint32_t em = elapsed_min % 60;
    char msg[64];
    snprintf(msg, sizeof(msg), "SPIFFS restore OK · Sleep-Zeit %lu:%02lu h", eh, em);
    syslog("TELEM", msg);

    // ── Ungesendete Zeilen wiederherstellen ──────────────────────
    if (!SPIFFS.exists(TELEM_ROWS_FILE)) return;
    File fr = SPIFFS.open(TELEM_ROWS_FILE, "r");
    if (!fr) return;
    uint32_t rows_magic = 0;
    uint16_t row_count  = 0;
    if (fr.read((uint8_t*)&rows_magic, 4) != 4 || rows_magic != TELEM_ROWS_MAGIC ||
        fr.read((uint8_t*)&row_count, 2) != 2 || row_count == 0 || row_count > TELEM_ROW_BUF_SIZE) {
        fr.close();
        SPIFFS.remove(TELEM_ROWS_FILE);
        return;
    }
    if (s_row_buf && s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        uint16_t restored = 0;
        for (uint16_t i = 0; i < row_count; i++) {
            TelemetryRow row;
            if (fr.read((uint8_t*)&row, sizeof(row)) != sizeof(row)) break;
            s_row_buf[s_row_head] = row;
            s_row_head = (uint16_t)((s_row_head + 1) % TELEM_ROW_BUF_SIZE);
            if (s_row_pending < TELEM_ROW_BUF_SIZE) s_row_pending++;
            restored++;
        }
        xSemaphoreGive(s_mutex);
        snprintf(msg, sizeof(msg), "Rows restore: %u Zeilen", restored);
        syslog("TELEM", msg);
    }
    fr.close();
    SPIFFS.remove(TELEM_ROWS_FILE);
}

// ============================================================
//  ELM327-Brücke: ABRP CAN-Antwort → Telemetrie-Cache
//  data[] = UDS-Datenbytes NACH 0x62+DID_H+DID_L
// ============================================================
void telem_feed_did(uint16_t did, const uint8_t* data, int data_len) {
    if (!data || data_len < 1) return;

    switch (did) {
        case 0x028C:  // SoC
            if (data_len >= 1) {
                float v = data[0] * 0.4425f - 6.1947f;
                telem_update(TELEM_SOC, v, plausible(v, -5.f, 105.f));
            }
            break;
        case 0x1E3B:  // Spannung
            if (data_len >= 2) {
                float v = (data[0] * 256u + data[1]) / 4.f;
                telem_update(TELEM_VOLTAGE, v, plausible(v, 300.f, 500.f));
            }
            break;
        case 0x1E3D:  // Strom
            if (data_len >= 4) {
                int32_t raw = ((int32_t)data[0] << 24) | ((int32_t)data[1] << 16) |
                              ((int32_t)data[2] << 8)  |  (int32_t)data[3];
                float v = -((raw - 150000) / 100.f);
                telem_update(TELEM_CURRENT, v, plausible(v, -2000.f, 2000.f));
            }
            break;
        case 0xF40D:  // Geschwindigkeit
            if (data_len >= 1) {
                float v = (float)data[0];
                telem_update(TELEM_VEHICLE_SPEED, v, plausible(v, 0.f, 200.f));
            }
            break;
        case 0x7448:  // Ladestatus
            if (data_len >= 1) {
                bool charging = (data[0] & 0x04) != 0;
                bool dcfc     = (data[0] & 0x06) == 0x06;
                telem_update(TELEM_IS_CHARGING, charging ? 1.f : 0.f, true);
                telem_update(TELEM_IS_DCFC,     dcfc     ? 1.f : 0.f, true);
            }
            break;
        case 0x2A0B:  // Batterie-Temperatur
            if (data_len >= 1) {
                float v = data[0] / 2.f - 40.f;
                telem_update(TELEM_BATT_TEMP, v, plausible(v, -40.f, 80.f));
            }
            break;
        case 0x2609:  // Außentemperatur
            if (data_len >= 1) {
                float v = data[0] / 2.f - 50.f;
                telem_update(TELEM_EXT_TEMP, v, plausible(v, -40.f, 80.f));
            }
            break;
        case 0x2AB2:  // Kapazität
            if (data_len >= 2) {
                float v = (float)((int16_t)((data[0] << 8) | data[1])) * 50.f / 1000.f;
                telem_update(TELEM_CAPACITY, v, plausible(v, 1.f, 200.f));
            }
            break;
        case 0x1E32:  // Geladen kWh (Bytes I:J:K:L = Offset 8-11)
            if (data_len >= 12) {
                int32_t raw = ((int32_t)data[8]  << 24) | ((int32_t)data[9]  << 16) |
                              ((int32_t)data[10] << 8)  |  (int32_t)data[11];
                float v = raw / 8583.07123641215f;
                telem_update(TELEM_KWH_CHARGED, v, plausible(v, 0.f, 99999999.f));
            }
            break;
        case 0x210E:  // Geparkt
            if (data_len >= 2) {
                telem_update(TELEM_IS_PARKED, (data[1] == 8) ? 1.f : 0.f, true);
            }
            break;
        case 0x295A:  // Odometer
            if (data_len >= 3) {
                float v = (float)(((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2]);
                telem_update(TELEM_ODOMETER, v, plausible(v, 0.f, 999999.f));
            }
            break;
        case 0x0295:  // Reichweite
            if (data_len >= 2) {
                float v = (float)((data[0] << 8) | data[1]);
                telem_update(TELEM_RANGE, v, plausible(v, 0.f, 600.f));
            }
            break;
        default:
            break;  // Unbekannte DID — ignorieren
    }

    // Power nach Strom/Spannungs-Update nachberechnen
    if (did == 0x1E3B || did == 0x1E3D) {
        float volt_val = 0, curr_val = 0;
        uint32_t age = 0;
        bool v_ok = telem_get_latest(TELEM_VOLTAGE, &volt_val, &age);
        bool c_ok = telem_get_latest(TELEM_CURRENT, &curr_val, &age);
        if (v_ok && c_ok) {
            float kw = volt_val * curr_val / 1000.f;
            telem_update(TELEM_POWER, kw, plausible(kw, -500.f, 500.f));
        }
    }
}

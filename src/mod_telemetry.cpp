// TINY_GSM_MODEM_SIM7080 wird ausschließlich über platformio.ini build_flags gesetzt.
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>

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
#include "config.h"
#include "mod_config.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>

// TinyGsm-Instanz aus mod_modem (nicht im öffentlichen Header)
extern TinyGsm& modem_get();

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

// ── Zeilen-Ringpuffer (GPS-getriggert, PSRAM) ───────────────
static TelemetryRow* s_row_buf = nullptr;   // allokiert in telem_init()
static uint16_t     s_row_head      = 0;   // nächste Schreibposition
static uint16_t     s_row_send_tail = 0;   // älteste ungesendete Zeile
static uint16_t     s_row_pending   = 0;   // ausstehende (ungesendete) Zeilen

// GPS-Capture-State (Vergleichsbasis für Trigger-Schwellen)
static double   s_cap_lat = 0.0;
static double   s_cap_lon = 0.0;
static uint32_t s_cap_ms  = 0;
static uint32_t s_yaw_last_cap_ms = 0;  // letzter Capture durch Yaw-Burst

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
// Wird jede Sekunde aus telem_task aufgerufen.
// Schreibt nur wenn GPS gültig UND mind. eine Schwelle überschritten.
// OBD-Daten ohne GPS werden verworfen (GPS hat Vorrang).
static void row_try_capture() {
    if (!gps_valid()) return;

    double   lat = gps_lat();
    double   lon = gps_lon();
    uint32_t now = millis();

    // Aktuelle Fahrzeuggeschwindigkeit für Distanz-Schwelle
    float speed_kmh = 0.0f;
    telem_get_latest(TELEM_VEHICLE_SPEED, &speed_kmh, nullptr);

    // Kein Capture im Stand: vermeidet identische Zeilen beim Parken
    float parked_val = 0.0f;
    telem_get_latest(TELEM_IS_PARKED, &parked_val, nullptr);
    if (parked_val > 0.5f && speed_kmh < 1.0f) return;

    // ── Trigger-Bedingungen prüfen ─────────────────────────
    bool do_capture = false;
    if (s_cap_ms == 0) {
        do_capture = true;  // erster Punkt immer speichern
    } else {
        uint32_t elapsed = now - s_cap_ms;
        float    dist    = haversine_m(s_cap_lat, s_cap_lon, lat, lon);

        if (elapsed >= TELEM_GPS_MAX_INTERVAL_MS && dist >= TELEM_GPS_MIN_MOVE_M) {
            do_capture = true;  // Zeitlimit + Mindestbewegung
        } else {
            float thresh = (speed_kmh >= TELEM_GPS_SPEED_THRESH_KMH) ? TELEM_GPS_DIST_HI_M : TELEM_GPS_DIST_LO_M;
            if (dist >= thresh) {
                do_capture = true;  // Distanz-Schwelle
            } else {
                // Yaw-Burst: sofort bei Kurve, dann alle 3s solange Drehung anhält
                int yaw = (int)fabsf(gyro_get_yaw_dps());
                if (yaw >= TELEM_YAW_TURN_DPS && dist >= 5.0f) {
                    if (s_yaw_last_cap_ms == 0 ||
                        (now - s_yaw_last_cap_ms) >= TELEM_YAW_INTERVAL_MS) {
                        do_capture = true;
                        s_yaw_last_cap_ms = now;
                    }
                } else {
                    s_yaw_last_cap_ms = 0;  // Kurve vorbei → Reset
                }
            }
        }
    }
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
    // Heading aus vorherigem → aktuellem Punkt berechnen
    if (s_cap_ms > 0 && haversine_m(s_cap_lat, s_cap_lon, lat, lon) >= 5.0f) {
        row.values[TELEM_GPS_HEADING] = bearing_deg(s_cap_lat, s_cap_lon, lat, lon);
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

    uint32_t last_sensor_ms = 0;   // GPS, Gyro, LTE (alle 15 s)
    uint32_t last_pmu_ms    = 0;   // PMU-Akku (alle 60 s)

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

        // GPS-getriggerter Zeilen-Capture (prüft intern Distanz/Kurs/Zeit-Schwellen)
        row_try_capture();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    Serial.println("[TELEM] Task beendet (Shutdown)");
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
        Serial.println("[TELEM] PSRAM alloc FEHLER — Fallback auf internes RAM");
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
    Serial.println("[TELEM] init OK");
}

void telem_start_task() {
    xTaskCreatePinnedToCore(telem_task, "TELEM", 6144, NULL, 1, &s_telem_task_handle, 0);
    Serial.println("[TELEM] Task gestartet");
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
//  Phase 3 — InfluxDB v2 Sender
// ============================================================

// ── Kurze Feldnamen (Datenvolumen-optimiert) ──────────────────
// Reihenfolge muss zu TelemField-Enum passen!
// nullptr = nicht senden
// fmt: 'f'=float(%.4g)  'b'=bool(0i/1i)  'i'=int(%di)
//      'g'=gps(%.6f)    '1'=1-Dezimale(%.1f)
struct InfluxField {
    const char* key;
    char        fmt;
};

static const InfluxField s_fields[TELEM_FIELD_COUNT] = {
    // Schnell (5s)
    { "s",  'f' },   // TELEM_SOC          — float %
    { "u",  'i' },   // TELEM_VOLTAGE      — int V
    { "i",  'i' },   // TELEM_CURRENT      — int A
    { "p",  '1' },   // TELEM_POWER        — 1 Dez kW
    { "v",  'f' },   // TELEM_VEHICLE_SPEED — float km/h
    { "c",  'b' },   // TELEM_IS_CHARGING  — bool
    { "dc", 'b' },   // TELEM_IS_DCFC      — bool
    // Mittel (10–15s)
    { "bt", 'i' },   // TELEM_BATT_TEMP    — int °C
    { "et", 'i' },   // TELEM_EXT_TEMP     — int °C
    { "r",  'f' },   // TELEM_RANGE        — float km
    { "la", 'g' },   // TELEM_GPS_LAT      — 6 Dez °
    { "lo", 'g' },   // TELEM_GPS_LON      — 6 Dez °
    { "hd", 'i' },   // TELEM_GPS_HEADING  — int °
    { nullptr, 0 },  // TELEM_GPS_VALID    — skip
    { nullptr, 0 },  // TELEM_GYRO_G       — skip (nicht senden)
    { nullptr, 0 },  // TELEM_LTE_SIGNAL   — manuell als ls
    { nullptr, 0 },  // TELEM_LTE_OPERATOR — String, skip
    // Langsam (30–60s)
    { "ca", 'f' },   // TELEM_CAPACITY     — float kWh
    { "kw", 'f' },   // TELEM_KWH_CHARGED  — float kWh
    { "pk", 'b' },   // TELEM_IS_PARKED    — bool
    { "od", 'f' },   // TELEM_ODOMETER     — float km
    { "bd", 'i' },   // TELEM_BATT_DEVICE  — int %
};

void telem_send_influx() {
    if (!s_mutex) return;

    // ── Schritt 1: Ausstehende Zeilen unter Mutex snapshot-en ────
    static TelemetryRow* rows_snap = nullptr;
    if (!rows_snap) {
        rows_snap = (TelemetryRow*)ps_calloc(INFLUX_ROWS_PER_SEND, sizeof(TelemetryRow));
        if (!rows_snap) rows_snap = (TelemetryRow*)calloc(INFLUX_ROWS_PER_SEND, sizeof(TelemetryRow));
        if (!rows_snap) return;
    }
    uint16_t rows_to_send = 0;
    uint16_t send_start   = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    rows_to_send = (s_row_pending < (uint16_t)INFLUX_ROWS_PER_SEND)
                   ? s_row_pending
                   : (uint16_t)INFLUX_ROWS_PER_SEND;
    send_start = s_row_send_tail;
    for (uint16_t i = 0; i < rows_to_send; i++) {
        rows_snap[i] = s_row_buf[(send_start + i) % TELEM_ROW_BUF_SIZE];
    }
    xSemaphoreGive(s_mutex);

    if (rows_to_send == 0) {
        Serial.println("[INFLUX] Keine Zeilen ausstehend");
        return;
    }

    // ── Schritt 2: Multi-Line-Protocol-Body aufbauen ─────────────
    // ~220 Bytes/Zeile Reserve + Puffer-Ende
    static const int BODY_SIZE = INFLUX_ROWS_PER_SEND * 240 + 16;
    static char* body = nullptr;
    if (!body) {
        body = (char*)ps_calloc(1, BODY_SIZE);
        if (!body) body = (char*)calloc(1, BODY_SIZE);
        if (!body) return;
    }
    int pos      = 0;
    int lines_ok = 0;

    for (uint16_t r = 0; r < rows_to_send; r++) {
        const TelemetryRow& row = rows_snap[r];
        int line_start = pos;
        int field_cnt  = 0;

        if (pos + 64 >= (int)BODY_SIZE) break;  // kein Platz mehr

        pos += snprintf(body + pos, BODY_SIZE - pos,
                        "v,d=%s ", cfg_influx_device());

        for (int f = 0; f < TELEM_FIELD_COUNT; f++) {
            if (!s_fields[f].key) continue;
            if (!row.valid[f])    continue;
            if (pos + 32 >= (int)BODY_SIZE) break;

            if (field_cnt > 0) body[pos++] = ',';

            switch (s_fields[f].fmt) {
                case 'b': pos += snprintf(body + pos, BODY_SIZE - pos, "%s=%di",
                              s_fields[f].key, (int)(row.values[f] > 0.5f ? 1 : 0)); break;
                case 'i': pos += snprintf(body + pos, BODY_SIZE - pos, "%s=%di",
                              s_fields[f].key, (int)row.values[f]); break;
                case 'g': pos += snprintf(body + pos, BODY_SIZE - pos, "%s=%.6f",
                              s_fields[f].key, (double)row.values[f]); break;
                case '1': pos += snprintf(body + pos, BODY_SIZE - pos, "%s=%.1f",
                              s_fields[f].key, row.values[f]); break;
                default:  pos += snprintf(body + pos, BODY_SIZE - pos, "%s=%.4g",
                              s_fields[f].key, row.values[f]); break;
            }
            field_cnt++;
        }

        // LTE-Signal manuell (war nullptr in s_fields[])
        if (row.valid[TELEM_LTE_SIGNAL] && pos + 16 < (int)BODY_SIZE) {
            if (field_cnt > 0) body[pos++] = ',';
            pos += snprintf(body + pos, BODY_SIZE - pos, "ls=%di",
                            (int)row.values[TELEM_LTE_SIGNAL]);
            field_cnt++;
        }

        // Delta-Status: _eq (gleich) und _na (fehlt) als Bitmask
        if (row.eq_mask && pos + 20 < (int)BODY_SIZE) {
            if (field_cnt > 0) body[pos++] = ',';
            pos += snprintf(body + pos, BODY_SIZE - pos, "_eq=%lui",
                            (unsigned long)row.eq_mask);
            field_cnt++;
        }
        if (row.na_mask && pos + 20 < (int)BODY_SIZE) {
            if (field_cnt > 0) body[pos++] = ',';
            pos += snprintf(body + pos, BODY_SIZE - pos, "_na=%lui",
                            (unsigned long)row.na_mask);
            field_cnt++;
        }

        if (field_cnt == 0) {
            pos = line_start;  // leere Zeile rückgängig machen
            continue;
        }

        pos += snprintf(body + pos, BODY_SIZE - pos, " %lu\n",
                        (unsigned long)row.unix_s);
        lines_ok++;
    }

    if (lines_ok == 0) {
        Serial.println("[INFLUX] Alle Zeilen leer — uebersprungen");
        return;
    }

    // ── Schritt 3: HTTP POST ─────────────────────────────────────
    TinyGsmClientSecure client(modem_get());
    HttpClient          http(client, cfg_influx_host(), 443);
    http.setHttpResponseTimeout(15000);

    char path[192];
    snprintf(path, sizeof(path), "/api/v2/write?org=%s&bucket=%s&precision=s",
             cfg_influx_org(), cfg_influx_bucket());

    Serial.printf("[INFLUX] %d/%d Zeilen · %d Bytes ausstehend: %d\n",
                  lines_ok, rows_to_send, pos, s_row_pending);
    Serial.printf("[INFLUX] POST https://%s%s\n", cfg_influx_host(), path);
    Serial.printf("[INFLUX] Body (%d bytes): %.80s%s\n", pos, body, pos > 80 ? "..." : "");

    http.beginRequest();
    http.post(path);
    { char auth[300]; snprintf(auth, sizeof(auth), "Token %s", cfg_influx_token()); http.sendHeader("Authorization", auth); }
    http.sendHeader("Content-Type", "text/plain; charset=utf-8");
    http.sendHeader("Content-Length", pos);
    http.beginBody();
    http.print(body);
    http.endRequest();

    int status = http.responseStatusCode();

    // ── Schritt 4: Buffer-Pointer bei Erfolg vorrücken ───────────
    if (status == 204) {
        s_influx_ok = true;
        http.stop();
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_row_send_tail = (uint16_t)((s_row_send_tail + rows_to_send) % TELEM_ROW_BUF_SIZE);
            s_row_pending  -= rows_to_send;
            xSemaphoreGive(s_mutex);
        }
        char msg[40];
        snprintf(msg, sizeof(msg), "OK · %d Zeilen", lines_ok);
        syslog("INFLUX", msg);
    } else {
        // Fehler: send_tail bleibt → nächster Zyklus versucht es erneut
        s_influx_ok = false;
        // Response-Body lesen (InfluxDB liefert JSON-Fehlermeldung)
        String rbody = http.responseBody();
        http.stop();
        rbody.trim();
        char msg[48];
        snprintf(msg, sizeof(msg), "Fehler HTTP %d", status);
        syslog("INFLUX", msg);
        Serial.printf("[INFLUX] Fehler: HTTP %d\n", status);
        if (rbody.length() > 0) {
            Serial.printf("[INFLUX] Response: %.200s\n", rbody.c_str());
        }
    }
}

// ============================================================
//  Phase 3 — SPIFFS-Persistenz
// ============================================================

#define TELEM_PERSIST_FILE  "/telem.bin"
#define TELEM_PERSIST_MAGIC 0x544C4D02UL  // 'TLM' + Version 2 (eq_mask/na_mask)

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
    Serial.printf("[TELEM] persist: %u Bytes nach /telem.bin\n",
                  (unsigned)(sizeof(hdr) + sizeof(cache_snap) + sizeof(last_sent)));
}

void telem_restore_from_spiffs() {
    File f = SPIFFS.open(TELEM_PERSIST_FILE, "r");
    if (!f) return;  // Keine gespeicherten Daten — normaler Start

    PersistHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
        hdr.magic       != TELEM_PERSIST_MAGIC ||
        hdr.field_count != (uint32_t)TELEM_FIELD_COUNT) {
        f.close();
        SPIFFS.remove(TELEM_PERSIST_FILE);
        syslog("TELEM", "SPIFFS restore: ungültige Datei gelöscht");
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

    // In Cache + Buffer schreiben
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(s_cache, cache_snap, sizeof(s_cache));
        for (int i = 0; i < TELEM_FIELD_COUNT; i++)
            s_last_sent[i] = last_sent[i];
        xSemaphoreGive(s_mutex);
    }

    uint32_t elapsed_min = elapsed_ms / 60000UL;
    uint32_t eh = elapsed_min / 60;
    uint32_t em = elapsed_min % 60;
    char msg[64];
    snprintf(msg, sizeof(msg), "SPIFFS restore OK · Sleep-Zeit %lu:%02lu h", eh, em);
    syslog("TELEM", msg);
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

#include "mod_gps_ext.h"
#include "mod_logs.h"
#include "mod_sleep.h"
#include "config.h"
#include "shared.h"
#include <Arduino.h>
#include <math.h>

// ============================================================
//  mod_gps_ext — BLITZ M10 GPS V2  (UART2, NMEA $GNRMC/$GNGGA)
// ============================================================

static HardwareSerial s_gps_uart(2);
static bool  s_ok        = false;
static int   s_sat_count = -1;

// ── NMEA-Koordinate DDMM.MMMM → Dezimalgrad ──────────────
static double nmea_deg(const char* s, char dir) {
    if (!s || s[0] == '\0') return 0.0;
    double val = atof(s);
    int    d   = (int)(val / 100);
    double min = val - d * 100.0;
    double dec = d + min / 60.0;
    if (dir == 'S' || dir == 'W') dec = -dec;
    return dec;
}

// ── NMEA-Feld aus kommageteiltem String extrahieren ───────
static const char* nmea_field(char* buf, int idx) {
    int i = 0;
    char* p = buf;
    while (*p) {
        if (*p == ',') {
            if (++i == idx) return p + 1;
        }
        p++;
    }
    return "";
}

// ── NMEA-Prüfsumme validieren ─────────────────────────────
static bool nmea_checksum_ok(const char* line) {
    const char* s = line + 1;  // nach '$'
    uint8_t csum = 0;
    while (*s && *s != '*') csum ^= (uint8_t)*s++;
    if (*s != '*') return false;
    uint8_t ref = (uint8_t)strtol(s + 1, nullptr, 16);
    return csum == ref;
}

// ── $GNRMC parsen ─────────────────────────────────────────
//  $GNRMC,HHMMSS.ss,A,DDMM.MMMM,N,DDDMM.MMMM,E,knots,course,DDMMYY,,,A*xx
static void parse_gnrmc(char* buf) {
    bool valid = (nmea_field(buf, 2)[0] == 'A');
    if (!valid) {
        gps_invalidate();
        return;
    }
    double lat   = nmea_deg(nmea_field(buf, 3), nmea_field(buf, 4)[0]);
    double lon   = nmea_deg(nmea_field(buf, 5), nmea_field(buf, 6)[0]);
    float  spd   = (float)atof(nmea_field(buf, 7)) * 1.852f;  // Knoten → km/h
    float  crs   = (float)atof(nmea_field(buf, 8));
    if (lat == 0.0 && lon == 0.0) { gps_invalidate(); return; }
    gps_update(lat, lon, spd, crs);
}

// ── $GNGGA parsen (nur Satellitenanzahl) ──────────────────
//  $GNGGA,HHMMSS.ss,lat,N,lon,E,fix,sats,hdop,alt,...
static void parse_gngga(char* buf) {
    int fix = atoi(nmea_field(buf, 6));
    if (fix > 0) s_sat_count = atoi(nmea_field(buf, 7));
}

// ── NMEA-Zeile verarbeiten ────────────────────────────────
static void process_line(char* line) {
    size_t len = strlen(line);
    // Trailing CR/LF entfernen
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
    if (len < 6) return;
    if (!nmea_checksum_ok(line)) return;

    if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0)
        parse_gnrmc(line);
    else if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0)
        parse_gngga(line);
}

// ── FreeRTOS Task ─────────────────────────────────────────
static void gps_ext_task(void*) {
    static char line[128];
    static uint8_t pos = 0;

    while (!g_shutdown) {
        while (s_gps_uart.available()) {
            char c = (char)s_gps_uart.read();
            if (c == '$') {
                pos = 0;                     // Neuer Satz
            }
            if (pos < sizeof(line) - 1) {
                line[pos++] = c;
                if (c == '\n') {
                    line[pos] = '\0';
                    process_line(line);
                    pos = 0;
                }
            } else {
                pos = 0;                     // Überlauf → verwerfen
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

// ── Öffentliche API ───────────────────────────────────────
void gps_ext_init() {
    if (!GPS_EXT_ENABLED_DEFAULT) return;

    s_gps_uart.begin(GPS_EXT_BAUD, SERIAL_8N1, GPS_EXT_RX_PIN, GPS_EXT_TX_PIN);
    s_ok = true;

    Serial.printf("[GPS_EXT] BLITZ M10 UART2  RX=GPIO%d TX=GPIO%d %dbaud\n",
                  GPS_EXT_RX_PIN, GPS_EXT_TX_PIN, GPS_EXT_BAUD);
    syslog("GPS_EXT", "BLITZ M10 GPS V2 aktiv — UART2 GPIO1/GPIO2 9600Bd");

    xTaskCreatePinnedToCore(gps_ext_task, "GPS_EXT", 3072, NULL, 2, NULL, 1);
}

bool gps_ext_ok()       { return s_ok; }
int  gps_ext_sat_count() { return s_sat_count; }

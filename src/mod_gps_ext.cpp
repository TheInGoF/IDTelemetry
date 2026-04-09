#include "mod_gps_ext.h"
#include "mod_logs.h"
#include "mod_sleep.h"
#include "mod_rtc.h"
#include "config.h"
#include "shared.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <string.h>

// ============================================================
//  mod_gps_ext — BLITZ Mini M10 GPS  (UART, GPIO43 TX / GPIO44 RX)
//  QWIIC-Connector auf T-SIM7080G-S3 S3 (H606) ist UART-verdrahtet
//  UBX-Assist: Zeit (RTC) + letzte Position (NVS) beim Start injizieren
// ============================================================

static HardwareSerial s_gps_uart(2);  // UART2 (UART0=USB-CDC, UART1=Modem)
static bool  s_ok        = false;
static int   s_sat_count = -1;

// ── UBX-Paket senden ─────────────────────────────────────
static void ubx_send(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    uint8_t ck_a = 0, ck_b = 0;
    auto sc = [&](uint8_t b) {
        ck_a += b; ck_b += ck_a;
        s_gps_uart.write(b);
    };
    s_gps_uart.write(0xB5);
    s_gps_uart.write(0x62);
    sc(cls); sc(id);
    sc((uint8_t)(len & 0xFF)); sc((uint8_t)(len >> 8));
    for (uint16_t i = 0; i < len; i++) sc(payload[i]);
    s_gps_uart.write(ck_a);
    s_gps_uart.write(ck_b);
}

// ── UBX-MGA-INI-TIME_UTC injizieren ──────────────────────
static void inject_time() {
    int y, mo, d, h, mi, s;
    if (!rtc_get_datetime(y, mo, d, h, mi, s)) return;
    if (y < 2020) return;  // RTC nicht gesetzt

    uint8_t p[24] = {};
    p[0]  = 0x10;           // type: TIME_UTC
    p[3]  = 18;             // aktuelle GPS-UTC Schaltsekunden
    p[4]  = (uint8_t)(y & 0xFF);
    p[5]  = (uint8_t)(y >> 8);
    p[6]  = (uint8_t)mo;
    p[7]  = (uint8_t)d;
    p[8]  = (uint8_t)h;
    p[9]  = (uint8_t)mi;
    p[10] = (uint8_t)s;
    p[16] = 60; p[17] = 0;  // tAccS = 60s — toleriert ungenaue RTC

    ubx_send(0x13, 0x40, p, 24);
    char msg[48];
    snprintf(msg, sizeof(msg), "UBX Zeit: %04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    syslog("GPS_EXT", msg);
}


// ── Aktive Antenne einschalten (UBX-CFG-VALSET, M10) ─────
// M10 verwendet neues Konfig-Interface; CFG-ANT (0x06/0x13) ist M8-Legacy.
// CFG-HW-ANT_CFG_VOLTCTRL = 0x10A3002E → 1 = Antennenspannung an
static void enable_active_antenna() {
    uint8_t p[13] = {};
    p[0] = 0x00;              // version = 0
    p[1] = 0x00;              // layers: RAM (0x01), BBR (0x02), Flash (0x04) — 0=RAM
    p[2] = 0x00; p[3] = 0x00; // reserved
    // Key-Value-Paar: CFG-HW-ANT_CFG_VOLTCTRL (U1)
    p[4]  = 0x2E; p[5]  = 0x00; p[6]  = 0xA3; p[7]  = 0x10;  // Key (little-endian)
    p[8]  = 0x01;              // Value = 1 (ein)
    ubx_send(0x06, 0x8A, p, 9);
}

// ── AssistNow Autonomous (AOP) aktivieren ────────────────
// M10: UBX-CFG-VALSET mit CFG-ANA-USE_ANA (0x10230001) = 1
// Modul sammelt Satellitendaten und erstellt eigene Orbit-Vorhersagen
static void enable_aop() {
    uint8_t p[9] = {};
    p[0] = 0x00;              // version
    p[1] = 0x01;              // layers: RAM
    p[2] = 0x00; p[3] = 0x00; // reserved
    // CFG-ANA-USE_ANA = 0x10230001 (U1)
    p[4] = 0x01; p[5] = 0x00; p[6] = 0x23; p[7] = 0x10;  // Key
    p[8] = 0x01;              // Value = 1
    ubx_send(0x06, 0x8A, p, 9);
}

// ── Alle 4 GNSS-Konstellationen aktivieren ──────────────
// M10 unterstützt GPS + GLONASS + Galileo + BeiDou gleichzeitig
// UBX-CFG-VALSET: mehrere Key-Value-Paare in einem Befehl
static void enable_all_gnss() {
    uint8_t p[4 + 5*4] = {};  // header(4) + 4× Key(4)+Value(1)
    p[0] = 0x00;              // version
    p[1] = 0x03;              // layers: RAM + BBR (überlebt Backup-Sleep)
    p[2] = 0x00; p[3] = 0x00; // reserved
    // CFG-SIGNAL-GPS_ENA   = 0x1031001F (sollte schon an sein)
    p[4]  = 0x1F; p[5]  = 0x00; p[6]  = 0x31; p[7]  = 0x10; p[8]  = 0x01;
    // CFG-SIGNAL-GLO_ENA   = 0x10310025 (sollte schon an sein)
    p[9]  = 0x25; p[10] = 0x00; p[11] = 0x31; p[12] = 0x10; p[13] = 0x01;
    // CFG-SIGNAL-GAL_ENA   = 0x10310021
    p[14] = 0x21; p[15] = 0x00; p[16] = 0x31; p[17] = 0x10; p[18] = 0x01;
    // CFG-SIGNAL-BDS_ENA   = 0x10310022
    p[19] = 0x22; p[20] = 0x00; p[21] = 0x31; p[22] = 0x10; p[23] = 0x01;
    ubx_send(0x06, 0x8A, p, 24);
    syslog("GPS_EXT", "GNSS: GPS+GLONASS+Galileo+BeiDou");
}

// ── TIMEPULSE ein/aus (PPS-LED) ──────────────────────────
// Nur RAM — M10 läuft durch, kein BBR nötig
static void set_timepulse(bool on) {
    uint8_t p[9] = {};
    p[0] = 0x00;
    p[1] = 0x01;              // nur RAM
    p[2] = 0x00; p[3] = 0x00;
    p[4] = 0x07; p[5] = 0x00; p[6] = 0x05; p[7] = 0x10;
    p[8] = on ? 0x01 : 0x00;
    ubx_send(0x06, 0x8A, p, 9);
}
static void disable_timepulse() { set_timepulse(false); }
static void enable_timepulse()  { set_timepulse(true);  }

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

static const char* nmea_field(char* buf, int idx) {
    int i = 0; char* p = buf;
    while (*p) { if (*p++ == ',') if (++i == idx) return p; }
    return "";
}

static bool nmea_checksum_ok(const char* line) {
    const char* s = line + 1;
    uint8_t csum = 0;
    while (*s && *s != '*') csum ^= (uint8_t)*s++;
    if (*s != '*') return false;
    return csum == (uint8_t)strtol(s + 1, nullptr, 16);
}

#define GPS_EXT_MIN_SATS     6   // Mindest-Satelliten für gültigen Fix
#define GPS_EXT_SETTLE_MS 20000  // 20s Einschwingzeit nach Boot ignorieren

static bool    s_rtc_synced   = false;
static bool    s_pos_settled  = false;  // Position nach Boot stabil?
static uint32_t s_first_fix_ms = 0;

static void parse_gnrmc(char* buf) {
    if (nmea_field(buf, 2)[0] != 'A') { gps_invalidate(); return; }
    double lat = nmea_deg(nmea_field(buf, 3), nmea_field(buf, 4)[0]);
    double lon = nmea_deg(nmea_field(buf, 5), nmea_field(buf, 6)[0]);
    float  spd = (float)atof(nmea_field(buf, 7)) * 1.852f;
    float  crs = (float)atof(nmea_field(buf, 8));
    if (lat == 0.0 && lon == 0.0) { gps_invalidate(); return; }

    // Qualitäts-Gate: Mindest-Sats + Einschwingzeit nach Boot
    if (s_sat_count < GPS_EXT_MIN_SATS) { gps_invalidate(); return; }
    if (!s_pos_settled) {
        if (s_first_fix_ms == 0) s_first_fix_ms = millis();
        if ((millis() - s_first_fix_ms) < GPS_EXT_SETTLE_MS) { gps_invalidate(); return; }
        s_pos_settled = true;
        syslog("GPS_EXT", "Position stabil — Tracking aktiv");
    }

    gps_update(lat, lon, spd, crs);

    // ── RTC einmalig aus GPS-UTC-Zeit setzen ──────────────────
    if (!s_rtc_synced) {
        // Feld 1: HHMMSS.ss  Feld 9: DDMMYY
        const char* tstr = nmea_field(buf, 1);
        const char* dstr = nmea_field(buf, 9);
        if (tstr[0] && dstr[0] && dstr[0] != ',') {
            int hh = (tstr[0]-'0')*10 + (tstr[1]-'0');
            int mi = (tstr[2]-'0')*10 + (tstr[3]-'0');
            int ss = (tstr[4]-'0')*10 + (tstr[5]-'0');
            int dd = (dstr[0]-'0')*10 + (dstr[1]-'0');
            int mo = (dstr[2]-'0')*10 + (dstr[3]-'0');
            int yy = (dstr[4]-'0')*10 + (dstr[5]-'0');
            if (hh < 24 && mi < 60 && ss < 60 && dd >= 1 && dd <= 31 && mo >= 1 && mo <= 12) {
                rtc_set_datetime(2000 + yy, mo, dd, hh, mi, ss);
                s_rtc_synced = true;
                char msg[48];
                snprintf(msg, sizeof(msg), "RTC sync via GPS: %04d-%02d-%02d %02d:%02d:%02d",
                         2000+yy, mo, dd, hh, mi, ss);
                syslog("GPS_EXT", msg);
            }
        }
    }
}

static void parse_gngga(char* buf) {
    if (atoi(nmea_field(buf, 6)) > 0)
        s_sat_count = atoi(nmea_field(buf, 7));
}

// ── GSV: Sichtbare Satelliten pro Konstellation zählen ───
// Feld 3 = "total SVs in view" — nur aus erster GSV-Nachricht (Feld 2 == "1")
static int s_sats_visible = 0;
static int s_gsv_accum    = 0;   // Akkumulator für laufenden GSV-Zyklus

static void parse_gsv(char* buf, const char* talker) {
    int msg_nr = atoi(nmea_field(buf, 2));  // aktuelle Nachricht (1..N)
    int total  = atoi(nmea_field(buf, 3));  // Sats in view für diese Konstellation
    (void)talker;
    if (msg_nr == 1) {
        // Erste Konstellation im Zyklus? Reset
        // Wir akkumulieren GP+GL+GA+GB und übernehmen am Ende
        // Einfach: bei GPGSV msg=1 → neuer Zyklus
    }
    // Jede Konstellation liefert nur in msg_nr=1 die Gesamtzahl
    if (msg_nr == 1) s_gsv_accum += total;
}

// Am Ende eines GSV-Zyklus (nach allen Konstellationen) Summe übernehmen
static void gsv_cycle_end() {
    if (s_gsv_accum > 0) s_sats_visible = s_gsv_accum;
    s_gsv_accum = 0;
}

static bool s_nmea_valid = false;  // erstes Paket mit gültiger Checksumme empfangen

static void process_line(char* line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
    if (len < 6 || !nmea_checksum_ok(line)) return;
    // Erst nach validem NMEA gilt Modul als bereit
    if (!s_nmea_valid) {
        s_nmea_valid = true;
        s_ok = true;  // gps_ext_ok() erst jetzt true
        syslog("GPS_EXT", "NMEA-Daten empfangen — Modul antwortet");
        inject_time();  // Immer — beschleunigt Fix von ~60s auf ~22s
    }
    if      (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) parse_gnrmc(line);
    else if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0) parse_gngga(line);
    else if (strncmp(line+3, "GSV", 3) == 0) {
        // $GPGSV kommt zuerst → neuer Zyklus
        if (strncmp(line, "$GPGSV", 6) == 0 && atoi(nmea_field(line, 2)) == 1)
            gsv_cycle_end();  // vorherigen Zyklus abschließen
        parse_gsv(line, line+1);
    }
}

// ── FreeRTOS Task ─────────────────────────────────────────
static void gps_ext_task(void*) {
    static char    line[128];
    static uint8_t pos = 0;
    static bool    fix_logged  = false;   // erster Fix geloggt?
    static uint32_t last_status_ms = 0;
    static int      last_sat   = -1;

    while (!g_shutdown) {
        while (s_gps_uart.available()) {
            char c = (char)s_gps_uart.read();
            if (c == '$') pos = 0;
            if (pos < sizeof(line) - 1) {
                line[pos++] = c;
                if (c == '\n') { line[pos] = '\0'; process_line(line); pos = 0; }
            } else { pos = 0; }
        }

        uint32_t now = millis();

        // Kein NMEA nach 5s → warnen
        if (!s_nmea_valid && now > 5000 && (now - last_status_ms) > 5000) {
            last_status_ms = now;
            syslog("GPS_EXT", "WARNUNG: Keine NMEA-Daten — Verdrahtung prüfen");
        }

        // Status alle 30s (immer loggen solange kein Fix, danach nur bei Änderung)
        if (s_nmea_valid && (now - last_status_ms) >= 30000UL) {
            last_status_ms = now;
            bool has_fix = gps_valid();
            if (s_sat_count != last_sat || !has_fix) {
                last_sat = s_sat_count;
                char msg[64];
                snprintf(msg, sizeof(msg), "Sats: %d sichtbar / %d im Fix · %s",
                         s_sats_visible, s_sat_count, has_fix ? "Fix" : "suche...");
                syslog("GPS_EXT", msg);
            }
        }

        // Erster Fix
        if (!fix_logged && gps_valid()) {
            fix_logged = true;
            char msg[64];
            snprintf(msg, sizeof(msg), "Fix! Sats: %d sichtbar / %d im Fix · %.5f / %.5f",
                     s_sats_visible, s_sat_count, gps_lat(), gps_lon());
            syslog("GPS_EXT", msg);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

// ── Öffentliche API ───────────────────────────────────────
// ── Frühes Wake: UART öffnen + Wake-Byte — sofort nach pmu_init() aufrufen ──
void gps_ext_wake() {
    if (!GPS_EXT_ENABLED) return;
    // UART öffnen — M10 läuft bereits (kein Backup-Mode), hat Fix
    s_gps_uart.begin(GPS_EXT_BAUD, SERIAL_8N1, GPS_EXT_RX_PIN, GPS_EXT_TX_PIN);
}

void gps_ext_init() {
    if (!GPS_EXT_ENABLED) return;
    if (!s_gps_uart) {
        gps_ext_wake();
    }
    delay(100);

    // Nur bei Cold Start konfigurieren — nach Idle-Sleep läuft M10 bereits
    if (!sleep_was_deep()) {
        enable_active_antenna();
        enable_all_gnss();
        enable_aop();
    }
    // PPS-LED wieder an (war im Sleep deaktiviert)
    enable_timepulse();

    // s_ok wird erst nach erstem validen NMEA-Paket gesetzt (in process_line)
    { char m[80]; snprintf(m, sizeof(m), "BLITZ Mini M10 — UART GPIO%d/%d %dBd (warte auf Fix)", GPS_EXT_RX_PIN, GPS_EXT_TX_PIN, GPS_EXT_BAUD); syslog("GPS_EXT", m); }
    xTaskCreatePinnedToCore(gps_ext_task, "GPS_EXT", 3072, NULL, 2, NULL, 1);
}

// ── GPS in Idle schicken — M10 trackt weiter, UART wird geschlossen ──
// Kein Backup-Befehl! M10 verliert bei Backup seine Nav-Daten auf diesem Modul.
// Stattdessen: M10 läuft weiter (~5-8mA), behält Fix + Ephemeris + AOP.
// Nach Wake: UART öffnen → sofortiger Fix.
void gps_ext_sleep() {
    if (!GPS_EXT_ENABLED) return;
    // PPS-LED aus (spart etwas Strom, kosmetisch)
    disable_timepulse();
    s_gps_uart.flush();
    s_gps_uart.end();
    syslog("GPS_EXT", "Idle-Sleep (M10 trackt weiter, UART closed)");
}

bool gps_ext_ok()        { return s_ok; }
int  gps_ext_sat_count()   { return s_sat_count; }
int  gps_ext_sat_visible() { return s_sats_visible; }

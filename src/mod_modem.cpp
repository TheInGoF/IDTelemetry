// TINY_GSM_MODEM_SIM7080 wird ausschließlich über platformio.ini build_flags gesetzt.
#include <TinyGsmClient.h>

#include "mod_modem.h"
#include "mod_sleep.h"
#include "mod_traccar.h"
#include "mod_telemetry.h"
#include "mod_rtc.h"
#include "shared.h"
#include "config.h"
#include "mod_config.h"
#include "mod_logs.h"
#include <Arduino.h>
#include <SPIFFS.h>

// ============================================================
//  mod_modem - SIM7080G Modem (LTE + GPS)
//
//  Eine FreeRTOS-Task verwaltet alles sequenziell:
//    1. Traccar-Tick (alle 60 s bei GPS-Fix → HTTPS GET)
//    2. GPS abfragen (AT+CGNSINF via TinyGSM) — alle 5 s
//    3. Modem-Status (Signal, Anbieter, SIM) — alle ~30 s
//
//  A-GPS: RTC-Zeit + letzte Position beim Start → schnellerer Fix
// ============================================================

// ---- Modem-Objekte ----------------------------------------

static HardwareSerial  s_serial(1);         // UART1 — exklusiv
static TinyGsm         s_modem(s_serial);

// ---- Zustand ----------------------------------------------

static volatile bool   s_connected   = false;
static char            s_operator[48] = "";
static volatile int8_t s_sig_quality  = -1;  // CSQ 0-31, 99=kein Signal, -1=unbekannt
static volatile bool   s_sim_ok       = false;

// ---- GPS-Details (vom Task geschrieben, per Getter lesbar) ----
static volatile float  s_gps_alt      = 0;
static volatile float  s_gps_speed    = 0;
static volatile float  s_gps_hdop     = 0;
static volatile int    s_gps_vsat     = 0;
static volatile int    s_gps_usat     = 0;
static volatile bool   s_gps_enabled  = false;
static volatile bool   s_gps_has_fix  = false;

// ---- Interne Linkage für mod_traccar ----------------------

TinyGsm& modem_get() { return s_modem; }

// ---- Modem Power-On ---------------------------------------



// PWRKEY-Impuls: genau wie im LilyGo-Beispiel (AllFunction/modem.cpp)
static void modem_pwrkey_pulse() {
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH);
    delay(1000);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
}



static TaskHandle_t s_modem_task_handle = nullptr;

// ---- A-GPS: RTC-Zeit + Warm Start → schnellerer Fix ----

// RTC-Zeit an Modem setzen (AT+CCLK) → GPS kennt aktuelle Uhrzeit
static void agps_set_time() {
    int y, mo, d, h, mi, s;
    if (!rtc_get_datetime(y, mo, d, h, mi, s)) return;

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "+CCLK=\"%02d/%02d/%02d,%02d:%02d:%02d+00\"",
             y % 100, mo, d, h, mi, s);
    s_modem.sendAT(cmd);
    s_modem.waitResponse(1000L);
    syslog("GPS", "A-GPS: RTC-Zeit an Modem gesetzt");
}

// Warm Start erzwingen — nutzt Rest-Almanac falls vorhanden,
// fällt automatisch auf Cold Start zurück wenn nichts da ist.
static void gnss_warm_start() {
    s_modem.sendAT("+CGNSWARM");
    s_modem.waitResponse(1000);
    syslog("GPS", "Warm Start angefordert");
}




// ---- FreeRTOS Task (State Machine) --------------------



enum ModemState {

    STATE_CHECK_SIM,

    STATE_WAIT_FOR_NETWORK,

    STATE_RUNNING

};






static void modem_task(void* /*param*/) {

    #ifdef LTE_DISABLED
    ModemState state           = STATE_RUNNING;
    syslog("MODEM", "GPS-Only · LTE/SIM uebersprungen");
    #else
    ModemState state           = STATE_CHECK_SIM;
    syslog("MODEM", "Task gestartet · SIM pruefen");
    #endif


    uint32_t   last_stat_ms    = 0;
    uint32_t   stat_interval   = 5000;  // erste Abfrage nach 5s, danach GPS_INTERVAL_MS

    uint32_t   last_traccar_ms = 0;
    bool       traccar_first   = true;  // erster Traccar-Versand nach TRACCAR_SEND_INTERVAL_MS

    int        scan_fail_count = 0;        // Fehlversuche STATE_WAIT_FOR_NETWORK
    bool       radio_initialized = false;  // CFUN-Sequenz bereits ausgeführt

    bool       first_fix       = true;     // Erster GPS-Fix noch nicht gemeldet
    bool       had_fix         = false;    // Zuletzt gültiger Fix
    uint32_t   no_fix_log_ms   = 0;        // Wann zuletzt "kein Fix" geloggt
    uint32_t   gps_log_ms      = 0;        // Wann zuletzt Position geloggt (30s)

    char       syslog_msg[128];



    for (;;) {
        if (g_shutdown) break;

        uint32_t now = millis();



        switch (state) {

            // =================================================================

            case STATE_CHECK_SIM:

            // =================================================================

                s_sim_ok = (s_modem.getSimStatus() == SIM_READY);

                if (s_sim_ok) {

                    syslog("MODEM", "SIM OK · Netzsuche");

                    state = STATE_WAIT_FOR_NETWORK;

                } else {

                    syslog("MODEM", "Keine SIM · GPS-only Modus");
                    bool gps_started = false;
                    for (int g = 0; g < 3 && !gps_started; g++) {
                        gps_started = s_modem.enableGPS();
                        if (!gps_started) vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (gps_started) {
                        s_gps_enabled = true;
                        syslog("GPS", "Aktiviert · suche Fix");
                        first_fix = true; had_fix = false; no_fix_log_ms = now;
                        state = STATE_RUNNING;
                        last_stat_ms = now;
                        stat_interval = 5000;
                    } else {
                        syslog("MODEM", "GPS-Aktivierung fehlgeschlagen");
                        state = STATE_RUNNING;
                    }

                }

                continue; // Nächste Iteration, um Zustand neu zu prüfen



            // =================================================================

            case STATE_WAIT_FOR_NETWORK:

            // =================================================================

                // SIM7080G: CMNB/CNMP einmalig setzen + Radio neu starten (CFUN=0/1)
                // Ohne Neustart ignoriert der Chip die Modusänderung!
                if (!radio_initialized) {
                    s_modem.sendAT("+CMNB=3");   // LTE-M + NB-IoT (alle Netze)
                    s_modem.waitResponse();
                    s_modem.sendAT("+CNMP=38");  // LTE only (kein 2G/3G Fallback)
                    s_modem.waitResponse();
                    // PDP-Kontext setzen — viele Carrier (esp. Business-SIM) brauchen das VOR dem Attach
                    { char _at[80]; snprintf(_at, sizeof(_at), "+CGDCONT=1,\"IP\",\"%s\"", cfg_apn()); s_modem.sendAT(_at); }
                    s_modem.waitResponse();
                    syslog("MODEM", "Radio-Neustart (CFUN=0/1)...");
                    s_modem.sendAT("+CFUN=0");
                    s_modem.waitResponse(10000L);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    s_modem.sendAT("+CFUN=1");
                    s_modem.waitResponse(10000L);
                    vTaskDelay(pdMS_TO_TICKS(3000));  // Modem braucht ~3s nach CFUN=1
                    s_modem.sendAT("+COPS=0");        // Automatische Netzwahl aktivieren
                    s_modem.waitResponse(10000L);
                    // Registrierungs-Status abfragen (Diagnose)
                    // CEREG: 0=nicht reg., 1=reg. Heimnetz, 2=sucht, 3=abgelehnt, 5=roaming
                    {
                        String cereg = "";
                        s_modem.sendAT("+CEREG?");
                        s_modem.waitResponse(3000L, cereg);
                        cereg.trim();
                        snprintf(syslog_msg, sizeof(syslog_msg), "CEREG: %s", cereg.c_str());
                        syslog("MODEM", syslog_msg);
                    }
                    radio_initialized = true;
                }
                syslog("MODEM", "Warte auf LTE-M/NB-IoT Netz (90s)...");

                if (s_modem.waitForNetwork(90000L, true)) {

                    s_sig_quality = (int8_t)s_modem.getSignalQuality();

                    String op = s_modem.getOperator();

                    strncpy(s_operator, op.c_str(), sizeof(s_operator) - 1);

                    scan_fail_count = 0;
                    int8_t bars = (s_sig_quality >= 20) ? 5 : (s_sig_quality >= 15) ? 4 : (s_sig_quality >= 10) ? 3 : (s_sig_quality >= 5) ? 2 : 1;
                    snprintf(syslog_msg, sizeof(syslog_msg), "LTE-M verbunden · %s · %d/5 Balken · CSQ %d", s_operator, bars, s_sig_quality);
                    syslog("MODEM", syslog_msg);



                    if (s_modem.enableGPS()) {

                        syslog("GPS", "Aktiviert · suche Fix");
                        first_fix = true; had_fix = false; no_fix_log_ms = now;

                        state = STATE_RUNNING;

                        last_stat_ms = now;  // erste Abfrage nach stat_interval (5s)
                        stat_interval = 5000;

                        last_traccar_ms = now;
                        traccar_first = true;

                    } else {

                        syslog("MODEM", "FEHLER bei GPS-Aktivierung. Neustart in 30s.");

                        vTaskDelay(pdMS_TO_TICKS(30000));

                    }

                } else {

                    scan_fail_count++;

                    if (scan_fail_count == 1 || scan_fail_count % 5 == 0) {
                        // Beim ersten Fehlversuch + alle 5 weiteren: AT+COPS=? (Netzwerk-Scan, bis 3 Min)
                        syslog("MODEM", "Netzwerk-Scan (AT+COPS=?, bis 3 Min)...");
                        s_modem.sendAT("+COPS=?");
                        String cops = "";
                        int r = s_modem.waitResponse(180000L, cops);
                        if (r == 1 && cops.length() > 5) {
                            int pos = 0, found = 0;
                            while (pos < (int)cops.length()) {
                                int ns = cops.indexOf('(', pos);
                                if (ns < 0) break;
                                int ne = cops.indexOf(')', ns);
                                if (ne < 0) break;
                                String entry = cops.substring(ns + 1, ne);
                                int q1s = entry.indexOf('"');
                                int q1e = (q1s >= 0) ? entry.indexOf('"', q1s + 1) : -1;
                                int lc  = entry.lastIndexOf(',');
                                if (q1s >= 0 && q1e > q1s && lc > q1e) {
                                    String name = entry.substring(q1s + 1, q1e);
                                    int act = entry.substring(lc + 1).toInt();
                                    const char* tech = (act == 7) ? "LTE-M" : (act == 9) ? "NB-IoT" : "unbekannt";
                                    char nl[80];
                                    snprintf(nl, sizeof(nl), "Netz: %-24s [%s]", name.c_str(), tech);
                                    syslog("MODEM", nl);
                                    found++;
                                }
                                pos = ne + 1;
                            }
                            if (found == 0) syslog("MODEM", "Keine Netze empfangen");
                        } else {
                            syslog("MODEM", "COPS-Scan Timeout");
                        }
                        // Nach COPS=? immer COPS=0 (auto) setzen — sonst bleibt Modem ohne Netzauswahl
                        s_modem.sendAT("+COPS=0");
                        s_modem.waitResponse(10000L);
                    } else {
                        syslog("MODEM", "Kein Netz gefunden. Warte 30s...");
                        vTaskDelay(pdMS_TO_TICKS(30000));
                    }

                }

                continue;



            // =================================================================

            case STATE_RUNNING:

            // =================================================================

                // Sleep-Entscheidung trifft mod_sleep (WiFi Guard)
                // Modem läuft einfach weiter bis Deep Sleep



                // --- Periodische Aktionen ---

                if ((now - last_stat_ms) >= stat_interval) {

                    // GPS-Daten abfragen — vollständig wie LilyGo AllFunction/modem.cpp
                    float lat = 0, lon = 0, speed = 0, alt = 0, accuracy = 0;
                    int   vsat = 0, usat = 0;
                    int   year = 0, month = 0, day = 0, hour = 0, gmin = 0, sec = 0;

                    if (s_modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                                       &year, &month, &day, &hour, &gmin, &sec)) {

                        gps_update((double)lat, (double)lon);

                        // Unplausible Werte bereinigen (TinyGSM gibt -9999 oder 0 bei leeren Feldern)
                        if (vsat < 0) vsat = 0;
                        if (usat < 0) usat = 0;
                        // SIM7080G liefert usat oft leer (=0) trotz Fix → vsat als Ersatz
                        if (usat == 0 && vsat > 0) usat = vsat;

                        // GPS-Details für Serial-Abfrage speichern
                        s_gps_alt     = alt;
                        s_gps_speed   = speed;
                        s_gps_hdop    = accuracy;
                        s_gps_vsat    = vsat;
                        s_gps_usat    = usat;
                        s_gps_has_fix = true;


                        if (first_fix) {
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Erster Fix · %.6f %.6f · Sat: %d/%d · HDOP: %.1f · Alt: %.0fm",
                                     lat, lon, usat, vsat, accuracy, alt);
                            syslog("GPS", syslog_msg);
                            first_fix = false;
                            had_fix   = true;
                        } else if (!had_fix) {
                            // Fix nach Verlust wieder da
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Fix wieder · %.6f %.6f · Sat: %d",
                                     lat, lon, usat);
                            syslog("GPS", syslog_msg);
                            had_fix = true;
                        }
                        // Im Normalbetrieb kein Log-Spam — g_gps wird still aktualisiert

                    } else {

                        gps_invalidate();
                        s_gps_has_fix = false;

                        if (had_fix) {
                            // Fix verloren — sofort melden
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Fix verloren · Sat sichtbar: %d", vsat);
                            syslog("GPS", syslog_msg);
                            had_fix      = false;
                            no_fix_log_ms = now;
                        } else if (now - no_fix_log_ms >= 30000UL) {
                            // Alle 30s Satelliten-Status loggen
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Suche Fix · Sat sichtbar: %d · verwendet: %d", vsat, usat);
                            syslog("GPS", syslog_msg);
                            no_fix_log_ms = now;
                        }
                    }



                    #ifndef LTE_DISABLED
                    // Modem-Status aktualisieren (nur wenn SIM vorhanden)
                    if (s_sim_ok) {
                        s_sig_quality = (int8_t)s_modem.getSignalQuality();

                        if (s_sig_quality < 0 || s_sig_quality == 99) {

                             syslog("GPS", "Fix verloren · Signal weg");
                             syslog("MODEM", "Signal verloren · Netzsuche");

                             radio_initialized = false;
                             state = STATE_WAIT_FOR_NETWORK;

                             gps_invalidate();

                             continue;

                        }
                    }
                    #endif // LTE_DISABLED

                    // Periodischer GPS-Positions-Log (alle 30s bei aktivem Fix)
                    {
                        GpsSnapshot snap = gps_snapshot();
                        if (snap.valid && now - gps_log_ms >= 30000UL) {
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Pos: %.6f %.6f · Sat: %d/%d · HDOP: %.1f",
                                     snap.lat, snap.lon, usat, vsat, accuracy);
                            syslog("GPS", syslog_msg);
                            gps_log_ms = now;
                        }
                    }

                    last_stat_ms = now;
                    stat_interval = GPS_INTERVAL_MS;

                }



                #ifndef LTE_DISABLED
                // LTE-Fenster: GPS deaktivieren → GPRS verbinden → senden → GPS reaktivieren
                // (SIM7080G: GPS und LTE-Daten können nicht gleichzeitig aktiv sein)
                // Ohne SIM: kein LTE → GPS läuft durchgehend

                if (s_sim_ok && (now - last_traccar_ms) >= TRACCAR_SEND_INTERVAL_MS) {

                    // GPS-Position JETZT sichern — nach disableGPS() ist g_gps ungültig
                    GpsSnapshot lte_fix = gps_snapshot();

                    syslog("MODEM", "LTE-Fenster · GPS stop");
                    s_modem.disableGPS();
                    gps_invalidate();
                    vTaskDelay(pdMS_TO_TICKS(200));

                    if (modem_ensure_connected()) {
                        traccar_on_gps_tick(lte_fix);
                        telem_send_influx();

                        s_modem.gprsDisconnect();
                        s_connected = false;
                    }

                    if (s_modem.enableGPS()) {
                        syslog("GPS", "Reaktiviert nach LTE");
                        first_fix = false; // kein erneutes "ERSTER FIX"-Log nach Reaktivierung
                    } else {
                        syslog("GPS", "FEHLER · Reaktivierung nach LTE");
                    }

                    last_traccar_ms = now;

                }
                #endif // LTE_DISABLED

                break;

        }



        vTaskDelay(pdMS_TO_TICKS(100)); // Kurze Pause in jedem Zustand

    }
    Serial.println("[MODEM] Task beendet (Shutdown)");
    vTaskDelete(NULL);
}



// ---- Öffentliche API ---------------------------------------



void modem_init() {

    char syslog_msg[128];

    snprintf(syslog_msg, sizeof(syslog_msg), "Init TX=GPIO%d RX=GPIO%d %dbaud",
             MODEM_TX_PIN, MODEM_RX_PIN, MODEM_BAUD);
    syslog("MODEM", syslog_msg);

    // 1. Pins vorbereiten — KEIN initialer PWRKEY-Puls (wie LilyGo-Beispiel)
    //    Puls nur wenn AT nach 5 Versuchen nicht antwortet.
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
    pinMode(MODEM_FLIGHT_PIN, OUTPUT);
    digitalWrite(MODEM_FLIGHT_PIN, LOW);   // Flight-Mode aus
    pinMode(MODEM_STATUS_PIN, INPUT);

    // 2. UART starten
    s_serial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

    // 3. AT-Handshake — nach 5 Fehlversuchen PWRKEY-Puls (1:1 aus LilyGo AllFunction)
    syslog("MODEM", "AT-Handshake...");
    {
        int retry      = 0;
        int pwrcycles  = 0;
        while (!s_modem.testAT(1000)) {
            retry++;
            if (retry > 6) {
                if (pwrcycles >= 3) {
                    syslog("MODEM", "WARNUNG: Modem antwortet nicht nach 3 Power-Cycles");
                    break;
                }
                syslog("MODEM", "Power-Cycle via PWRKEY");
                modem_pwrkey_pulse();
                delay(5000);   // Modem braucht ~3-5s zum Booten
                pwrcycles++;
                retry = 0;
            }
        }
    }
    syslog("MODEM", ("Modem: " + s_modem.getModemInfo()).c_str());

    // Multi-GNSS: GPS + GLONASS + BeiDou aktivieren (mehr Satelliten sichtbar)
    s_modem.sendAT("+CGNSMOD=1,1,0,1");
    s_modem.waitResponse(1000);

    // 4. A-GPS: RTC-Zeit + Warm Start für schnelleren Fix
    agps_set_time();
    gnss_warm_start();

    // 5. SIM prüfen
    SimStatus sim = s_modem.getSimStatus();
    if (sim == SIM_READY) {
        syslog("MODEM", "SIM OK");
        s_sim_ok = true;
    } else if (sim == SIM_LOCKED || sim == SIM_ANTITHEFT_LOCKED) {
        syslog("MODEM", "SIM · PIN benoetigt");
        s_sim_ok = false;
    } else {
        syslog("MODEM", "SIM nicht erkannt · wiederhole");
        s_sim_ok = false;
    }

    #ifdef LTE_DISABLED
    // GPS-Only Modus: direkt GPS aktivieren ohne SIM/Netz
    syslog("MODEM", "LTE_DISABLED: aktiviere GPS direkt...");
    if (s_modem.enableGPS()) {
        syslog("GPS", "Aktiviert · suche Fix");
    } else {
        syslog("GPS", "FEHLER · Aktivierung fehlgeschlagen");
    }
    #endif

    syslog("MODEM", "Init abgeschlossen");
}



void modem_start_task() {
    char syslog_msg[64];
    snprintf(syslog_msg, sizeof(syslog_msg), "Task gestartet, GPS alle %ds", GPS_INTERVAL_MS / 1000);
    syslog("MODEM", syslog_msg);

    xTaskCreatePinnedToCore(
        modem_task,
        "MODEM",
        12288,       // TLS-Handshake (TinyGsmClientSecure + HttpClient) braucht ~8-10 KB
        nullptr,
        1,
        &s_modem_task_handle,
        1       // Core 1
    );
}

bool modem_ensure_connected() {
    if (s_modem.isGprsConnected()) {
        return true;
    }

    if (!s_modem.isNetworkConnected()) {
        syslog("MODEM", "Warte auf Netzwerk (15 s)...");
        if (!s_modem.waitForNetwork(15000)) {
            syslog("MODEM", "Netzwerk nicht verfügbar");
            s_connected = false;
            return false;
        }
    }

    syslog("MODEM", "GPRS verbinden...");
    if (!s_modem.gprsConnect(cfg_apn(), cfg_apn_user(), cfg_apn_pass())) {
        syslog("MODEM", "GPRS fehlgeschlagen");
        s_connected = false;
        return false;
    }

    s_connected = true;
    char syslog_msg[64];
    snprintf(syslog_msg, sizeof(syslog_msg), "GPRS OK: %s", s_modem.getLocalIP().c_str());
    syslog("MODEM", syslog_msg);
    return true;
}

bool        modem_is_connected()    { return s_connected; }
int8_t      modem_signal_quality()  { return s_sig_quality; }
const char* modem_operator()        { return s_operator; }
bool        modem_sim_ok()          { return s_sim_ok; }

void modem_poweroff() {
    syslog("MODEM", "Power-Off fuer Deep Sleep...");

    // Task wurde bereits von sleep_update() per xTaskGetHandle() gelöscht,
    // oder ist noch aktiv wenn modem_poweroff() direkt aufgerufen wird.
    // xTaskGetHandle() gibt NULL zurück wenn der Task nicht mehr existiert.
    TaskHandle_t h = xTaskGetHandle("MODEM");
    if (h) {
        vTaskDelete(h);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_modem_task_handle = nullptr;


    // 2. GPS deaktivieren
    s_modem.disableGPS();

    // 3. GPRS trennen falls verbunden
    if (s_modem.isGprsConnected()) {
        s_modem.gprsDisconnect();
    }

    // 4. Modem software-seitig ausschalten (AT+CPOF)
    s_modem.sendAT("+CPOF");
    s_modem.waitResponse(3000L);
    delay(1500);  // CPOF braucht Zeit zum Abschalten

    // 5. PWRKEY-Puls nur wenn Modem noch an (STATUS=HIGH → aktiv)
    if (digitalRead(MODEM_STATUS_PIN) == HIGH) {
        Serial.println("[MODEM] STATUS noch HIGH → PWRKEY-Puls");
        modem_pwrkey_pulse();
        delay(1500);
    }

    // 6. UART deaktivieren — kein Strom mehr auf TX/RX-Leitungen
    s_serial.end();

    syslog("MODEM", "Power-Off OK");
}

void modem_print_lte_info() {
    Serial.println("┌─── LTE Info ─────────────────────────┐");
    Serial.printf( "│ SIM:        %s\n", s_sim_ok ? "OK" : "nicht erkannt");
    Serial.printf( "│ Signal:     %d/31 CSQ\n", (int)s_sig_quality);
    Serial.printf( "│ Anbieter:   %s\n", s_operator[0] ? s_operator : "---");
    Serial.printf( "│ Daten:      %s\n", s_connected ? "verbunden" : "getrennt");

    // Band abfragen
    s_modem.sendAT("+CBAND?");
    String band = "";
    if (s_modem.waitResponse(2000L, band) == 1 && band.length() > 0) {
        band.trim();
        Serial.printf("│ Band:       %s\n", band.c_str());
    }

    // CEREG Status
    s_modem.sendAT("+CEREG?");
    String cereg = "";
    if (s_modem.waitResponse(2000L, cereg) == 1) {
        cereg.trim();
        int stat = -1;
        int comma = cereg.indexOf(',');
        if (comma >= 0) stat = cereg.substring(comma + 1).toInt();
        const char* reg_str = "?";
        switch (stat) {
            case 0: reg_str = "nicht registriert"; break;
            case 1: reg_str = "Heimnetz"; break;
            case 2: reg_str = "sucht..."; break;
            case 3: reg_str = "abgelehnt"; break;
            case 5: reg_str = "Roaming"; break;
        }
        Serial.printf("│ Registr.:   %s\n", reg_str);
    }

    Serial.println("│ → 'lte scan' für Netzwerk-Scan");
    Serial.println("└──────────────────────────────────────┘");
}

void modem_print_lte_scan() {
    Serial.println("Netzwerk-Scan läuft (bis 3 Min)...");
    Serial.flush();

    s_modem.sendAT("+COPS=?");
    String cops = "";
    int r = s_modem.waitResponse(180000L, cops);
    if (r == 1 && cops.length() > 5) {
        int pos = 0, found = 0;
        while (pos < (int)cops.length()) {
            int ns = cops.indexOf('(', pos);
            if (ns < 0) break;
            int ne = cops.indexOf(')', ns);
            if (ne < 0) break;
            String entry = cops.substring(ns + 1, ne);
            int stat_e = entry.substring(0, entry.indexOf(',')).toInt();
            int q1s = entry.indexOf('"');
            int q1e = (q1s >= 0) ? entry.indexOf('"', q1s + 1) : -1;
            int lc  = entry.lastIndexOf(',');
            if (q1s >= 0 && q1e > q1s && lc > q1e) {
                String name = entry.substring(q1s + 1, q1e);
                int act = entry.substring(lc + 1).toInt();
                const char* tech = (act == 7) ? "LTE-M" : (act == 9) ? "NB-IoT" : "?";
                const char* mark = (stat_e == 2) ? " ◀" : "";
                Serial.printf("  %-20s [%s]%s\n", name.c_str(), tech, mark);
                found++;
            }
            pos = ne + 1;
        }
        if (found == 0) Serial.println("  Keine Netze gefunden");
    } else {
        Serial.println("  Scan Timeout / kein Ergebnis");
    }

    // Nach COPS=? immer COPS=0 (auto) setzen
    s_modem.sendAT("+COPS=0");
    s_modem.waitResponse(10000L);
}

int  modem_gps_vsat()    { return s_gps_vsat; }
int  modem_gps_usat()    { return s_gps_usat; }

void modem_print_gps_info() {
    GpsSnapshot snap = gps_snapshot();
    Serial.println("┌─── GPS Info ─────────────────────────┐");
    Serial.printf( "│ Fix:        %s\n", s_gps_has_fix ? "JA" : "NEIN");
    if (snap.valid) {
        Serial.printf( "│ Position:   %.6f, %.6f\n", snap.lat, snap.lon);
    } else {
        Serial.println("│ Position:   ---");
    }
    Serial.printf( "│ Satelliten: %d/%d (verwendet/sichtbar)\n", s_gps_usat, s_gps_vsat);
    Serial.printf( "│ HDOP:       %.1f\n", s_gps_hdop);
    Serial.printf( "│ Höhe:       %.0f m\n", s_gps_alt);
    Serial.printf( "│ Speed:      %.1f km/h\n", s_gps_speed);
    Serial.println("└──────────────────────────────────────┘");
}

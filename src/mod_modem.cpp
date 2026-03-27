// TINY_GSM_MODEM_SIM7080 wird ausschließlich über platformio.ini build_flags gesetzt.
#include <TinyGsmClient.h>

#include "mod_modem.h"
#include "mod_sleep.h"
#include "mod_traccar.h"
#include "mod_telemetry.h"
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
static bool            s_had_lte     = false;  // true nach erster erfolgreicher GPRS-Verbindung
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
static volatile bool s_task_paused = false;  // "at stop" → Modem-Task pausiert

// ---- A-GPS: RTC-Zeit + Warm Start → schnellerer Fix ----

// GPS aktivieren inkl. Multi-GNSS Konfiguration.
// +CGNSMOD muss NACH disableGPS / VOR enableGPS gesetzt werden.
// Nach erster LTE-Verbindung: Hot Start (AT+CGNSHOT) — nutzt gespeicherte letzte
// Position + Almanach + Modem-Zeit (automatisch via LTE synchronisiert).
static bool gps_enable_with_config() {
    s_modem.disableGPS();
    delay(300);
    // Multi-GNSS: GPS + GLONASS + BeiDou
    s_modem.sendAT("+CGNSMOD=1,1,0,1");
    s_modem.waitResponse(1000);
    bool ok = s_modem.enableGPS();          // ZUERST GPS einschalten
    if (ok && s_had_lte) {
        delay(200);                          // GPS-Engine braucht Moment zum Starten
        s_modem.sendAT("+CGNSHOT");         // DANN Hot Start (NVRAM-Pos + LTE-Zeit)
        s_modem.waitResponse(1000);
        syslog("GPS", "Hot Start (NVRAM + LTE-Zeit)");
    }
    return ok;
}




// ---- FreeRTOS Task (State Machine) --------------------



enum ModemState {

    STATE_CHECK_SIM,

    STATE_WAIT_FOR_NETWORK,

    STATE_RUNNING

};

// Hilfsfunktion: AT-Befehl senden, auf OK warten, Antwort loggen.
// Gibt true bei OK (rc=1) zurueck.
static bool at_ok(const char* cmd, long timeout_ms = 5000L) {
    s_modem.sendAT(cmd);
    String resp = "";
    int rc = s_modem.waitResponse(timeout_ms, resp);
    if (rc != 1) {
        char msg[128];
        resp.trim();
        snprintf(msg, sizeof(msg), "AT%s → ERROR (%s)", cmd, resp.c_str());
        syslog("MODEM", msg);
    }
    return rc == 1;
}

// ---- Europaeische LTE-M Operator-Tabelle ----
// Modem iteriert diese Liste und versucht COPS=1,2,"PLMN",7 (LTE-M).
// Operator ohne LTE-M scheitern sofort (ERROR) und werden soft-geblockt.
struct PlmnEntry { const char* plmn; const char* name; };
static const PlmnEntry PLMN_TABLE[] = {
    // Things Mobile LTE-M Roaming-Partner, sortiert nach Naehe zu Deutschland
    // Deutschland — O2 LTE-M+NB-IoT, T-Mobile LTE-M
    { "26203", "o2 DE"          },
    { "26201", "T-Mobile DE"    },
    // Luxemburg — POST LTE-M
    { "27001", "POST LU"        },
    // Tschechien — O2 LTE-M
    { "23002", "O2 CZ"          },
    // Niederlande — T-Mobile LTE-M, Vodafone LTE-M
    { "20416", "T-Mobile NL"    },
    { "20404", "Vodafone NL"    },
    // Belgien — Orange LTE-M, Base LTE-M
    { "20610", "Orange BE"      },
    { "20620", "Base BE"        },
    // Oesterreich — T-Mobile LTE-M
    { "23203", "T-Mobile AT"    },
    // Daenemark — TDC LTE-M, 3DK LTE-M, Telia LTE-M
    { "23801", "TDC DK"         },
    { "23806", "3 DK"           },
    { "23820", "Telia DK"       },
    // Schweiz — Swisscom LTE-M, Sunrise LTE-M
    { "22801", "Swisscom"       },
    { "22802", "Sunrise CH"     },
    // Slowakei — Orange LTE-M
    { "23101", "Orange SK"      },
    // Slowenien — A1 LTE-M
    { "29340", "A1 SI"          },
    // Frankreich — Orange LTE-M, Bouygues LTE-M
    { "20801", "Orange FR"      },
    { "20820", "Bouygues FR"    },
    // UK — Vodafone LTE-M
    { "23415", "Vodafone UK"    },
    // Ungarn — T-Mobile LTE-M
    { "21630", "T-Mobile HU"    },
    // Schweden — Telia LTE-M, Tele2 LTE-M, Telenor LTE-M
    { "24001", "Telia SE"       },
    { "24007", "Tele2 SE"       },
    { "24008", "Telenor SE"     },
    // Norwegen — Telenor LTE-M, Telia LTE-M
    { "24201", "Telenor NO"     },
    { "24202", "Telia NO"       },
    // Estland — Telia LTE-M, Elisa LTE-M
    { "24801", "Telia EE"       },
    { "24802", "Elisa EE"       },
    // Lettland — LMT LTE-M
    { "24701", "LMT LV"         },
    // Rumaenien — Vodafone LTE-M, Orange LTE-M
    { "22601", "Vodafone RO"    },
    { "22610", "Orange RO"      },
    // Finnland — Telia LTE-M, Elisa LTE-M
    { "24491", "Telia FI"       },
    { "24405", "Elisa FI"       },
    // Spanien — Movistar LTE-M
    { "21407", "Movistar ES"    },
    // Tuerkei — Turkcell LTE-M
    { "28601", "Turkcell TR"    },
    // Portugal — NOS LTE-M
    { "26803", "NOS PT"         },
    // Island — Siminn LTE-M
    { "27401", "Siminn IS"      },
};
static const int PLMN_COUNT = sizeof(PLMN_TABLE) / sizeof(PLMN_TABLE[0]);

// ---- PLMN-Sperrliste (Soft + Hard Block) ----
// Soft-Block: 30 Min (kein Signal, Timeout) → RAM
// Hard-Block: 30 Tage (abgelehnt, Daten gehen nicht) → SPIFFS
enum BlockType : uint8_t { BLOCK_SOFT = 0, BLOCK_HARD = 1 };
struct BlockedPlmn { char plmn[8]; uint32_t blocked_at; BlockType type; };
static const uint32_t SOFT_BLOCK_MS = 30UL * 60UL * 1000UL;         // 30 Minuten
static const uint32_t HARD_BLOCK_MS = 30UL * 24UL * 60UL * 60000UL; // 30 Tage (millis reicht ~49 Tage)
static const int      MAX_BLOCKED   = 20;
static BlockedPlmn    s_blocked[MAX_BLOCKED];
static int            s_blocked_count = 0;

static const char* HARD_BLOCK_FILE = "/plmn_block.txt";
static const char* GREEN_LIST_FILE = "/plmn_good.txt";

// ---- Green-List: bekannt funktionierende PLMNs ----
// Wird zuerst versucht → schnelle Verbindung ohne alle 38 durchzugehen.
// In SPIFFS gespeichert (ueberlebt Reboot), max 4 Eintraege.
static const int MAX_GREEN = 4;
static char s_green[MAX_GREEN][8];
static int  s_green_count = 0;

static void load_green_list() {
    if (!SPIFFS.exists(GREEN_LIST_FILE)) return;
    File f = SPIFFS.open(GREEN_LIST_FILE, "r");
    if (!f) return;
    s_green_count = 0;
    while (f.available() && s_green_count < MAX_GREEN) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() >= 5) {
            strncpy(s_green[s_green_count], line.c_str(), 7);
            s_green[s_green_count][7] = '\0';
            s_green_count++;
        }
    }
    f.close();
    if (s_green_count > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d Green-PLMNs geladen", s_green_count);
        syslog("MODEM", msg);
    }
}

static void save_green_list() {
    File f = SPIFFS.open(GREEN_LIST_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_green_count; i++) {
        f.printf("%s\n", s_green[i]);
    }
    f.close();
}

// PLMN als "gut" merken — an den Anfang der Liste (zuletzt gut = hoechste Prio)
static void green_plmn(const char* plmn) {
    // Schon drin? An Position 0 schieben
    for (int i = 0; i < s_green_count; i++) {
        if (strcmp(s_green[i], plmn) == 0) {
            if (i == 0) return;  // Schon an Position 0
            char tmp[8];
            memcpy(tmp, s_green[i], 8);
            memmove(&s_green[1], &s_green[0], i * 8);
            memcpy(s_green[0], tmp, 8);
            save_green_list();
            return;
        }
    }
    // Neu: an Position 0 einfuegen, Rest nach hinten
    if (s_green_count < MAX_GREEN) s_green_count++;
    memmove(&s_green[1], &s_green[0], (s_green_count - 1) * 8);
    strncpy(s_green[0], plmn, 7);
    s_green[0][7] = '\0';
    save_green_list();
    char msg[64];
    snprintf(msg, sizeof(msg), "Green-List: %s hinzugefuegt", plmn);
    syslog("MODEM", msg);
}

static bool is_green(const char* plmn) {
    for (int i = 0; i < s_green_count; i++) {
        if (strcmp(s_green[i], plmn) == 0) return true;
    }
    return false;
}

// Hard-Blocks beim Boot aus SPIFFS laden
static void load_hard_blocks() {
    if (!SPIFFS.exists(HARD_BLOCK_FILE)) return;
    File f = SPIFFS.open(HARD_BLOCK_FILE, "r");
    if (!f) return;
    s_blocked_count = 0;
    while (f.available() && s_blocked_count < MAX_BLOCKED) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() < 5) continue;
        // Format: "PLMN,boot_count" — wir speichern millis=0, wird beim ersten Check bereinigt
        int comma = line.indexOf(',');
        if (comma < 0) continue;
        String plmn = line.substring(0, comma);
        strncpy(s_blocked[s_blocked_count].plmn, plmn.c_str(), 7);
        s_blocked[s_blocked_count].plmn[7] = '\0';
        s_blocked[s_blocked_count].blocked_at = 0;  // = Boot-Zeitpunkt
        s_blocked[s_blocked_count].type = BLOCK_HARD;
        s_blocked_count++;
    }
    f.close();
    if (s_blocked_count > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d Hard-Blocks geladen", s_blocked_count);
        syslog("MODEM", msg);
    }
}

// Hard-Blocks in SPIFFS speichern
static void save_hard_blocks() {
    File f = SPIFFS.open(HARD_BLOCK_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_blocked_count; i++) {
        if (s_blocked[i].type == BLOCK_HARD) {
            f.printf("%s,1\n", s_blocked[i].plmn);
        }
    }
    f.close();
}

static bool is_plmn_blocked(const char* plmn) {
    uint32_t now = millis();
    for (int i = 0; i < s_blocked_count; i++) {
        if (strcmp(s_blocked[i].plmn, plmn) != 0) continue;
        uint32_t duration = (s_blocked[i].type == BLOCK_HARD) ? HARD_BLOCK_MS : SOFT_BLOCK_MS;
        if (now - s_blocked[i].blocked_at < duration) return true;
        // Abgelaufen — entfernen
        char msg[64];
        snprintf(msg, sizeof(msg), "Block abgelaufen: %s", plmn);
        syslog("MODEM", msg);
        s_blocked[i] = s_blocked[--s_blocked_count];
        if (s_blocked[i].type == BLOCK_HARD) save_hard_blocks();
        return false;
    }
    return false;
}

static void block_plmn(const char* plmn, BlockType type) {
    const char* label = (type == BLOCK_HARD) ? "HARD 30d" : "SOFT 30m";
    char msg[64];
    snprintf(msg, sizeof(msg), "Block %s: %s", label, plmn);
    syslog("MODEM", msg);
    // Update falls schon in Liste (Upgrade soft→hard moeglich)
    for (int i = 0; i < s_blocked_count; i++) {
        if (strcmp(s_blocked[i].plmn, plmn) == 0) {
            s_blocked[i].blocked_at = millis();
            if (type == BLOCK_HARD && s_blocked[i].type != BLOCK_HARD) {
                s_blocked[i].type = BLOCK_HARD;
                save_hard_blocks();
            }
            return;
        }
    }
    if (s_blocked_count < MAX_BLOCKED) {
        strncpy(s_blocked[s_blocked_count].plmn, plmn, 7);
        s_blocked[s_blocked_count].plmn[7] = '\0';
        s_blocked[s_blocked_count].blocked_at = millis();
        s_blocked[s_blocked_count].type = type;
        s_blocked_count++;
        if (type == BLOCK_HARD) save_hard_blocks();
    }
}

// SIM PIN pruefen und ggf. eingeben. Gibt true zurueck wenn SIM READY.
static bool ensure_sim_ready() {
    char syslog_msg[128];
    String cpin = "";
    s_modem.sendAT("+CPIN?");
    s_modem.waitResponse(5000L, cpin);
    cpin.trim();

    if (cpin.indexOf("READY") >= 0) return true;

    if (cpin.indexOf("SIM PIN") >= 0) {
        const char* pin = cfg_sim_pin();
        if (strlen(pin) == 0) {
            syslog("MODEM", "SIM braucht PIN, aber keiner konfiguriert!");
            return false;
        }
        snprintf(syslog_msg, sizeof(syslog_msg), "+CPIN=%s", pin);
        syslog("MODEM", "PIN eingeben...");
        s_modem.sendAT(syslog_msg);
        if (s_modem.waitResponse(10000L) != 1) {
            syslog("MODEM", "PIN abgelehnt!");
            return false;
        }
        syslog("MODEM", "PIN akzeptiert");

        // SIM braucht nach PIN+CFUN bis zu 15s — mehrfach pruefen
        for (int retry = 0; retry < 6; retry++) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            cpin = "";
            s_modem.sendAT("+CPIN?");
            s_modem.waitResponse(5000L, cpin);
            cpin.trim();
            if (cpin.indexOf("READY") >= 0) return true;
            snprintf(syslog_msg, sizeof(syslog_msg),
                     "SIM nach PIN: %s (Versuch %d/6)", cpin.c_str(), retry + 1);
            syslog("MODEM", syslog_msg);
        }

        syslog("MODEM", "SIM nach PIN nicht READY");
        return false;
    }

    snprintf(syslog_msg, sizeof(syslog_msg), "SIM-Status unerwartet: %s", cpin.c_str());
    syslog("MODEM", syslog_msg);
    return false;
}

// Radio konfigurieren: CFUN=0 → CNMP=38 / CMNB=3 / APN → CFUN=1.
// Offizieller LilyGo-Zyklus — immer vollstaendig ausfuehren.
// Gibt true zurueck wenn alles OK.
static bool configure_radio() {
    syslog("MODEM", "Radio konfigurieren (CFUN=0/1 Zyklus)...");

    // UART-Buffer leeren — nach PIN-Eingabe kommen URCs die den Parser stoeren
    while (s_serial.available()) s_serial.read();

    // CFUN=0 braucht auf SIM7080G nach PIN-Eingabe bis zu 35s — 45s Timeout
    if (!at_ok("+CFUN=0", 45000L)) {
        // Nochmal versuchen — manchmal kommt der Befehl durch aber die Antwort ist spaet
        while (s_serial.available()) s_serial.read();
        at_ok("+CFUN=0", 45000L);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    at_ok("+CNMP=2");                    // Auto (wie LilyGo-Beispiel: kein LTE-only Zwang)
    at_ok("+CMNB=3");                    // CAT-M + NB-IoT
    { char cmd[80]; snprintf(cmd, sizeof(cmd), "+CGDCONT=1,\"IP\",\"%s\"", cfg_apn()); at_ok(cmd); }
    { char cmd[80]; snprintf(cmd, sizeof(cmd), "+CNCFG=0,1,\"%s\"",       cfg_apn()); at_ok(cmd); }

    at_ok("+CFUN=1", 30000L);           // RF an
    vTaskDelay(pdMS_TO_TICKS(3000));    // SIM braucht Zeit um zurueckzukommen

    // PIN ggf. erneut eingeben (CFUN-Zyklus setzt SIM-State zurueck)
    if (!ensure_sim_ready()) {
        syslog("MODEM", "SIM nach CFUN=1 nicht READY");
        return false;
    }

    at_ok("+CEREG=2");
    at_ok("+CPSMS=0");
    at_ok("+CEDRXS=0");

    syslog("MODEM", "Radio konfiguriert: LTE CAT-M+NB-IoT");
    return true;
}

// Netz-Registrierung via getRegistrationStatus() Loop.
// Kein Timeout — wie LilyGo-Beispiel (MinimalModemNBIOTExample).
// Der Anrufer muss g_shutdown pruefen; der Task-Loop bricht bei Shutdown ab.
static bool try_register(char* operator_out, size_t op_size) {
    char syslog_msg[128];

    syslog("MODEM", "Netz-Registrierung (auto, warte unbegrenzt)...");

    // Auto-Modus sicherstellen
    s_modem.sendAT("+COPS=0");
    s_modem.waitResponse(10000L);

    RegStatus  reg_s      = REG_UNKNOWN;
    uint32_t   log_ms     = millis();
    uint32_t   elapsed_s  = 0;

    while (!g_shutdown) {
        reg_s = s_modem.getRegistrationStatus();
        if (reg_s == REG_OK_HOME || reg_s == REG_OK_ROAMING) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed_s++;
        // Alle 30s kurzer Status-Log
        if (millis() - log_ms >= 30000UL) {
            snprintf(syslog_msg, sizeof(syslog_msg),
                     "Netzsuche laeuft... (%lus)", (unsigned long)elapsed_s);
            syslog("MODEM", syslog_msg);
            log_ms = millis();
        }
    }
    if (g_shutdown || (reg_s != REG_OK_HOME && reg_s != REG_OK_ROAMING)) {
        return false;
    }

    // Registriert — Operator-Name abfragen
    String op = s_modem.getOperator();
    op.trim();
    if (op.length() > 0) {
        strncpy(operator_out, op.c_str(), op_size - 1);
        operator_out[op_size - 1] = '\0';
    } else {
        strncpy(operator_out, "Auto", op_size - 1);
        operator_out[op_size - 1] = '\0';
    }

    // PLMN fuer Green-List ermitteln (numerisches Format)
    s_modem.sendAT("+COPS=3,2");  // Format auf numerisch stellen
    s_modem.waitResponse(3000L);
    String resp = "";
    s_modem.sendAT("+COPS?");
    s_modem.waitResponse(3000L, resp);
    int q1 = resp.indexOf('"');
    int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) {
        String plmn = resp.substring(q1 + 1, q2);
        green_plmn(plmn.c_str());
        snprintf(syslog_msg, sizeof(syslog_msg), "PLMN: %s", plmn.c_str());
        syslog("MODEM", syslog_msg);
        // Name aus PLMN_TABLE nachschlagen
        for (int j = 0; j < PLMN_COUNT; j++) {
            if (strcmp(PLMN_TABLE[j].plmn, plmn.c_str()) == 0) {
                strncpy(operator_out, PLMN_TABLE[j].name, op_size - 1);
                operator_out[op_size - 1] = '\0';
                break;
            }
        }
    }
    // Format zurueck auf Langname
    s_modem.sendAT("+COPS=3,0");
    s_modem.waitResponse(3000L);

    return true;
}






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
    int        sig_fail_count = 0;         // Aufeinanderfolgende Signal-99-Abfragen

    bool       first_fix       = true;     // Erster GPS-Fix noch nicht gemeldet
    bool       had_fix         = false;    // Zuletzt gültiger Fix
    uint32_t   no_fix_log_ms   = 0;        // Wann zuletzt "kein Fix" geloggt
    uint32_t   gps_log_ms      = 0;        // Wann zuletzt Position geloggt (30s)

    char       syslog_msg[128];



    for (;;) {
        if (g_shutdown) break;

        // "at stop" → Task schläft bis "at start"
        while (s_task_paused && !g_shutdown) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
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
                        gps_started = gps_enable_with_config();
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

                // 1. SIM pruefen (PIN ggf. erneut eingeben nach CFUN-Reset)
                if (!ensure_sim_ready()) {
                    syslog("MODEM", "SIM nicht READY · warte 10s");
                    vTaskDelay(pdMS_TO_TICKS(10000));
                    state = STATE_CHECK_SIM;
                    continue;
                }

                // 2. Radio konfigurieren (CNMP/CMNB/APN/PSM) — CFUN nur bei Abweichung
                if (!radio_initialized) {
                    if (!configure_radio()) {
                        syslog("MODEM", "Radio-Konfiguration fehlgeschlagen · warte 30s");
                        vTaskDelay(pdMS_TO_TICKS(30000));
                        continue;
                    }
                    radio_initialized = true;
                }

                // 3. Netz-Registrierung: erst manuell (DE), dann Auto (Ausland)
                {
                    bool registered = try_register(s_operator, sizeof(s_operator));

                    if (registered) {
                        s_sig_quality = (int8_t)s_modem.getSignalQuality();
                        scan_fail_count = 0;

                        int8_t bars = (s_sig_quality >= 20) ? 5 : (s_sig_quality >= 15) ? 4 : (s_sig_quality >= 10) ? 3 : (s_sig_quality >= 5) ? 2 : 1;
                        snprintf(syslog_msg, sizeof(syslog_msg), "Netz verbunden · %s · %d/5 Balken · CSQ %d", s_operator, bars, s_sig_quality);
                        syslog("MODEM", syslog_msg);

                        if (gps_enable_with_config()) {
                            s_gps_enabled = true;
                            syslog("GPS", "Aktiviert · suche Fix");
                            first_fix = true; had_fix = false; no_fix_log_ms = now;
                            state = STATE_RUNNING;
                            last_stat_ms = now;
                            stat_interval = 5000;
                            last_traccar_ms = now;
                            traccar_first = true;
                        } else {
                            syslog("MODEM", "GPS-Aktivierung fehlgeschlagen · warte 30s");
                            vTaskDelay(pdMS_TO_TICKS(30000));
                        }
                    } else {
                        scan_fail_count++;
                        snprintf(syslog_msg, sizeof(syslog_msg),
                                 "Kein Operator erreichbar (Versuch %d) · warte 60s", scan_fail_count);
                        syslog("MODEM", syslog_msg);
                        vTaskDelay(pdMS_TO_TICKS(60000));
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
                            // 60s LTE-Timer startet ab erstem Fix — nicht ab Boot/Registrierung
                            last_traccar_ms = now;
                        } else if (!had_fix) {
                            // Fix nach LTE-Fenster wieder da → 60s Timer neu starten
                            snprintf(syslog_msg, sizeof(syslog_msg),
                                     "Fix wieder · %.6f %.6f · Sat: %d",
                                     lat, lon, usat);
                            syslog("GPS", syslog_msg);
                            had_fix         = true;
                            last_traccar_ms = now;  // 60s ab jetzt
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
                    // NICHT waehrend GPS aktiv — SIM7080G kann GPS und LTE
                    // nicht gleichzeitig, CSQ=99 waehrend GPS ist normal.
                    if (s_sim_ok && !s_gps_enabled) {
                        s_sig_quality = (int8_t)s_modem.getSignalQuality();

                        if (s_sig_quality < 0 || s_sig_quality == 99) {
                             sig_fail_count++;
                             if (sig_fail_count >= 6) {
                                 syslog("MODEM", "Signal verloren (6x) · Netzsuche");
                                 state = STATE_WAIT_FOR_NETWORK;
                                 gps_invalidate();
                                 sig_fail_count = 0;
                                 continue;
                             }
                        } else {
                             sig_fail_count = 0;
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

                if (s_sim_ok && had_fix && (now - last_traccar_ms) >= TRACCAR_SEND_INTERVAL_MS) {

                    last_traccar_ms = now;  // sofort setzen — verhindert Loop bei langer Ausfuehrung

                    // GPS-Position JETZT sichern — nach disableGPS() ist g_gps ungültig
                    GpsSnapshot lte_fix = gps_snapshot();

                    syslog("MODEM", "LTE-Fenster · GPS stop");
                    s_modem.disableGPS();
                    s_gps_enabled = false;
                    gps_invalidate();
                    vTaskDelay(pdMS_TO_TICKS(3000));  // RF-Umschaltung GPS→LTE braucht ~2-3s

                    if (modem_ensure_connected()) {
                        traccar_on_gps_tick(lte_fix);
                        telem_send_influx();
                        // GPRS bewusst NICHT trennen — Verbindung bleibt warm
                        // damit naechstes LTE-Fenster sofort senden kann
                    } else {
                        // GPRS fehlgeschlagen → Registrierungsstatus pruefen
                        RegStatus reg = s_modem.getRegistrationStatus();
                        if (reg == REG_OK_HOME || reg == REG_OK_ROAMING) {
                            // Noch registriert — GPS reaktivieren, naechstes Fenster erneut versuchen
                            syslog("MODEM", "GPRS fehlgeschlagen · noch registriert · GPS weiter");
                        } else {
                            // Netz verloren → Neuregistrierung
                            syslog("MODEM", "Netz verloren · Neuregistrierung");
                            state = STATE_WAIT_FOR_NETWORK;
                        }
                    }

                    // GPS nur reaktivieren wenn wir in STATE_RUNNING bleiben
                    if (state == STATE_RUNNING) {
                        if (gps_enable_with_config()) {
                            s_gps_enabled = true;
                            had_fix   = false;  // warte auf neuen Fix nach LTE
                            first_fix = false;  // aber kein "Erster Fix" Log mehr
                            syslog("GPS", "Reaktiviert nach LTE · warte auf Fix");
                        } else {
                            syslog("GPS", "FEHLER · Reaktivierung nach LTE");
                        }
                    }

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

    // 1. Pins vorbereiten
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
    pinMode(MODEM_FLIGHT_PIN, OUTPUT);
    digitalWrite(MODEM_FLIGHT_PIN, LOW);   // Flight-Mode aus
    pinMode(MODEM_STATUS_PIN, INPUT);

    // 2. UART starten
    s_serial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(100);

    // 3. AT-Handshake — wie LilyGo MinimalModemNBIOTExample:
    //    Alle 6 Fehlversuche PWRKEY-Puls senden bis Modem antwortet.
    //    Noetig nach Deep Sleep (AT+CPOF schaltet Modem aus, braucht Puls zum Neustart).
    syslog("MODEM", "AT-Handshake...");
    {
        int retry = 0;
        while (!s_modem.testAT(1000)) {
            if (++retry > 6) {
                syslog("MODEM", "Kein AT · PWRKEY-Puls...");
                modem_pwrkey_pulse();
                delay(3000);   // Modem Boot abwarten (~3-5s nach Puls)
                retry = 0;
            }
        }
    }
    syslog("MODEM", "AT OK");

    // Nach Handshake: warten bis Modem wirklich bereit ist (ATI liefert Modell-Info)
    // Nach PWRKEY antwortet AT oft schon nach 3s, aber SIM braucht laenger.
    {
        String info;
        for (int i = 0; i < 5; i++) {
            info = s_modem.getModemInfo();
            if (info.length() > 0) break;
            syslog("MODEM", "Warte auf Modem-Ready...");
            delay(2000);
        }
        syslog("MODEM", ("Modem: " + info).c_str());
    }

    // 4. SIM prüfen und ggf. entsperren
    //    Direkt AT+CPIN? senden — TinyGSM getSimStatus() erkennt manche Antworten nicht.
    //    Moegliche Antworten: +CPIN: READY / +CPIN: SIM PIN / +CME ERROR: ...
    //    SIM-Subsystem braucht nach Modem-Boot einige Sekunden.
    //    Bei Warmstart (ESP-Reset ohne Modem-Reset) bleibt SIM oft im ERROR-State
    //    → nach 3 Fehlversuchen PWRKEY-Puls erzwingen.
    syslog("MODEM", "Warte 5s auf SIM-Subsystem...");
    delay(5000);
    // UART-Buffer leeren (Reste von vorherigen Kommandos)
    while (s_serial.available()) s_serial.read();

    SimStatus sim = SIM_ERROR;
    for (int i = 0; i < 12; i++) {
        String cpin_resp = "";
        s_modem.sendAT("+CPIN?");
        int r = s_modem.waitResponse(5000L, cpin_resp);
        cpin_resp.trim();
        snprintf(syslog_msg, sizeof(syslog_msg), "CPIN? → rc=%d resp=\"%s\"", r, cpin_resp.c_str());
        syslog("MODEM", syslog_msg);

        if (cpin_resp.indexOf("READY") >= 0) {
            sim = SIM_READY;
            break;
        } else if (cpin_resp.indexOf("SIM PIN") >= 0) {
            sim = SIM_LOCKED;
            break;
        } else if (cpin_resp.indexOf("SIM PUK") >= 0) {
            sim = SIM_LOCKED;
            syslog("MODEM", "WARNUNG: SIM PUK erforderlich!");
            break;
        } else if (cpin_resp.indexOf("NOT INSERTED") >= 0) {
            syslog("MODEM", "Keine SIM eingelegt");
            sim = SIM_ERROR;
            break;
        }

        // Kein PWRKEY — sperrt die SIM. Einfach laenger warten (max 60s).
        snprintf(syslog_msg, sizeof(syslog_msg), "SIM nicht erkannt (%d/12), warte 5s...", i + 1);
        syslog("MODEM", syslog_msg);
        delay(5000);
    }

    snprintf(syslog_msg, sizeof(syslog_msg), "SIM Status: %d (0=err 1=ready 2=locked)", sim);
    syslog("MODEM", syslog_msg);

    if (sim == SIM_LOCKED) {
        const char* pin = cfg_sim_pin();
        size_t pinlen = strlen(pin);
        snprintf(syslog_msg, sizeof(syslog_msg),
                 "SIM gesperrt · PIN konfiguriert: %s (%d Zeichen)",
                 pinlen > 0 ? "ja" : "NEIN", (int)pinlen);
        syslog("MODEM", syslog_msg);

        if (pinlen > 0) {
            // Verbleibende PIN-Versuche pruefen (PUK-Schutz)
            // AT+CPINR nicht von allen SIM7080G-FW unterstuetzt — auch AT+SPIC versuchen
            int retries = -1;
            {
                String resp = "";
                s_modem.sendAT("+CPINR=\"SC\"");
                int r = s_modem.waitResponse(5000L, resp);
                resp.trim();
                if (r == 1 && resp.indexOf(',') >= 0) {
                    int comma = resp.lastIndexOf(',');
                    retries = resp.substring(comma + 1).toInt();
                }
                // Fallback: AT+SPIC (SIMCom-spezifisch)
                if (retries < 0) {
                    resp = "";
                    s_modem.sendAT("+SPIC");
                    r = s_modem.waitResponse(3000L, resp);
                    resp.trim();
                    // Antwort: +SPIC: 3   oder   +SPIC: pin1_times,puk1_times,...
                    if (r == 1) {
                        int colon = resp.indexOf(':');
                        if (colon >= 0) {
                            String val = resp.substring(colon + 1);
                            val.trim();
                            int comma = val.indexOf(',');
                            retries = (comma >= 0) ? val.substring(0, comma).toInt() : val.toInt();
                        }
                    }
                }
            }
            snprintf(syslog_msg, sizeof(syslog_msg),
                     "PIN-Versuche uebrig: %d%s", retries, retries < 0 ? " (unbekannt)" : "");
            syslog("MODEM", syslog_msg);

            if (retries >= 0 && retries < 2) {
                syslog("MODEM", "ABBRUCH: Weniger als 2 PIN-Versuche! PUK-Gefahr!");
                s_sim_ok = false;
            } else {
                // TinyGSM simUnlock sendet AT+CPIN="pin" — manuell ohne Anfuehrungszeichen versuchen
                // SIM7080G akzeptiert beides, aber manche FW-Versionen sind empfindlich
                snprintf(syslog_msg, sizeof(syslog_msg), "+CPIN=%s", pin);
                syslog("MODEM", "Entsperre SIM...");
                s_modem.sendAT(syslog_msg);
                int r = s_modem.waitResponse(10000L);
                snprintf(syslog_msg, sizeof(syslog_msg), "CPIN Antwort: %d (1=OK)", r);
                syslog("MODEM", syslog_msg);

                if (r == 1) {
                    syslog("MODEM", "PIN akzeptiert");
                    delay(5000);  // SIM braucht nach PIN-Eingabe Zeit
                    // Verifizieren dass SIM jetzt READY ist
                    SimStatus after = s_modem.getSimStatus();
                    if (after == SIM_READY) {
                        syslog("MODEM", "SIM READY nach PIN-Eingabe");
                        s_sim_ok = true;
                    } else {
                        snprintf(syslog_msg, sizeof(syslog_msg),
                                 "SIM nach PIN nicht READY (Status: %d)", after);
                        syslog("MODEM", syslog_msg);
                        s_sim_ok = false;
                    }
                } else {
                    syslog("MODEM", "FEHLER: PIN abgelehnt!");
                    s_sim_ok = false;
                }
            }
        } else {
            syslog("MODEM", "FEHLER: SIM braucht PIN, aber kein PIN konfiguriert!");
            syslog("MODEM", "PIN ueber Web-UI (Config > SIM PIN) oder secrets.h setzen");
            s_sim_ok = false;
        }
    } else if (sim == SIM_READY) {
        syslog("MODEM", "SIM bereit (kein PIN noetig)");
        s_sim_ok = true;
    } else {
        syslog("MODEM", "Keine SIM erkannt oder SIM-Fehler");
        s_sim_ok = false;
    }

    #ifdef LTE_DISABLED
    // GPS-Only Modus: direkt GPS aktivieren ohne SIM/Netz
    syslog("MODEM", "LTE_DISABLED: aktiviere GPS direkt...");
    if (gps_enable_with_config()) {
        syslog("GPS", "Aktiviert · suche Fix");
    } else {
        syslog("GPS", "FEHLER · Aktivierung fehlgeschlagen");
    }
    #endif

    // PLMN-Listen aus SPIFFS laden (ueberleben Reboot)
    load_hard_blocks();
    load_green_list();

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

    // gprsConnect: setzt APN, sendet +CNACT=0,1, wartet auf "+APP PDP: 0,ACTIVE" URC
    syslog("MODEM", "GPRS verbinden...");
    if (!s_modem.gprsConnect(cfg_apn(), cfg_apn_user(), cfg_apn_pass())) {
        syslog("MODEM", "GPRS fehlgeschlagen");
        s_connected = false;
        return false;
    }
    s_connected = true;
    s_had_lte   = true;
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

    // COPS=? setzt Modem in "kein Operator" — Modem-Task uebernimmt die manuelle Registrierung
}

void modem_print_lte_bands() {
    Serial.println("┌─── LTE Band-Konfiguration ───────────┐");

    // Aktuelle Band-Config abfragen
    s_modem.sendAT("+CBANDCFG?");
    String resp = "";
    if (s_modem.waitResponse(3000L, resp) == 1) {
        resp.trim();
        Serial.printf("│ %s\n", resp.c_str());
    } else {
        Serial.println("│ CBANDCFG nicht lesbar");
    }

    // Netz-Modus
    resp = "";
    s_modem.sendAT("+CMNB?");
    if (s_modem.waitResponse(2000L, resp) == 1) {
        resp.trim();
        int mode = -1;
        int idx = resp.indexOf(':');
        if (idx >= 0) mode = resp.substring(idx + 1).toInt();
        const char* ms = (mode == 1) ? "CAT-M only" : (mode == 2) ? "NB-IoT only" : (mode == 3) ? "CAT-M + NB-IoT" : "?";
        Serial.printf("│ Modus:      %s (CMNB=%d)\n", ms, mode);
    }

    // Bevorzugter RAT
    resp = "";
    s_modem.sendAT("+CNMP?");
    if (s_modem.waitResponse(2000L, resp) == 1) {
        resp.trim();
        Serial.printf("│ RAT:        %s\n", resp.c_str());
    }

    Serial.println("│");
    Serial.println("│ DE-Carrier LTE-M/NB-IoT Baender:");
    Serial.println("│   Telekom:  B8, B20");
    Serial.println("│   Vodafone: B8, B20");
    Serial.println("│   o2:       B8, B20");
    Serial.println("│   1NCE/TM:  B3, B8, B20 (Roaming)");
    Serial.println("│");
    Serial.println("│ Tippe 'lte bands fix' um B3,B8,B20 zu setzen");
    Serial.println("│ Tippe 'lte bands all' um alle Baender freizuschalten");
    Serial.println("└──────────────────────────────────────┘");
}

void modem_lte_bands_fix(bool all) {
    if (all) {
        Serial.println("[MODEM] Setze alle Baender (CAT-M + NB-IoT)...");
        s_modem.sendAT("+CBANDCFG=\"CAT-M\",1,2,3,4,5,8,12,13,14,17,18,19,20,25,26,27,28,66,85");
        s_modem.waitResponse(3000L);
        s_modem.sendAT("+CBANDCFG=\"NB-IOT\",1,2,3,5,8,12,13,17,18,19,20,25,26,28,66,71,85");
        s_modem.waitResponse(3000L);
    } else {
        Serial.println("[MODEM] Setze DE-Baender B3,B8,B20 (CAT-M + NB-IoT)...");
        s_modem.sendAT("+CBANDCFG=\"CAT-M\",3,8,20");
        s_modem.waitResponse(3000L);
        s_modem.sendAT("+CBANDCFG=\"NB-IOT\",3,8,20");
        s_modem.waitResponse(3000L);
    }

    // Radio-Neustart damit Aenderung greift
    Serial.println("[MODEM] Radio-Neustart (CFUN=0/1)...");
    at_ok("+CFUN=0", 10000L);
    delay(2000);
    at_ok("+CFUN=1", 10000L);
    delay(5000);

    // SIM-PIN ggf. erneut eingeben nach CFUN-Reset
    if (!ensure_sim_ready()) {
        Serial.println("[MODEM] WARNUNG: SIM nach CFUN nicht READY");
    }

    // CNMP/CMNB NACH CFUN=1 neu setzen — CFUN=0/1 setzt auf Default zurueck
    at_ok("+CNMP=38");
    at_ok("+CMNB=1");

    Serial.println("[MODEM] Baender gesetzt + Radio neu gestartet");
    Serial.println("[MODEM] Netzsuche laeuft ueber Modem-Task (manuell)...");

    // Neue Config anzeigen
    modem_print_lte_bands();
}

void modem_pause_task() {
    if (s_task_paused) {
        Serial.println("[MODEM] Task bereits pausiert");
        return;
    }
    s_task_paused = true;
    // Task sofort hart suspenden — bricht laufende AT-Timeouts ab
    if (s_modem_task_handle) vTaskSuspend(s_modem_task_handle);
    // UART-Buffer leeren (halbe Responses vom unterbrochenen AT-Befehl)
    delay(100);
    while (s_serial.available()) s_serial.read();
    Serial.println("[MODEM] Task pausiert — 'at start' zum Fortsetzen");
    Serial.println("[MODEM] Hinweis: Falls COPS=? lief, antwortet Modem erst nach Scan-Ende (bis 3 Min)");
}

void modem_resume_task() {
    if (!s_task_paused) {
        Serial.println("[MODEM] Task laeuft bereits");
        return;
    }
    s_task_paused = false;
    if (s_modem_task_handle) vTaskResume(s_modem_task_handle);
    Serial.println("[MODEM] Task fortgesetzt");
}

void modem_send_at(const char* cmd) {
    // Modem-Task pausieren um Kollisionen zu vermeiden (nur wenn nicht schon pausiert)
    bool need_resume = !s_task_paused && s_modem_task_handle;
    if (need_resume) vTaskSuspend(s_modem_task_handle);

    // COPS-Befehle brauchen langen Timeout (Netzsuche bis 120s)
    long timeout = 10000L;
    if (strncmp(cmd, "+COPS", 5) == 0) timeout = 120000L;

    if (s_task_paused) {
        // Raw-Modus: UART-Buffer leeren, direkt senden, roh lesen
        // Verhindert TinyGSM-Verwirrung durch alte COPS=?-Responses
        while (s_serial.available()) s_serial.read();

        Serial.printf("[AT] → AT%s\n", cmd);
        s_serial.printf("AT%s\r\n", cmd);

        // Antwort zeilenweise lesen bis OK/ERROR oder Timeout
        uint32_t start = millis();
        String line;
        while (millis() - start < (uint32_t)timeout) {
            if (s_serial.available()) {
                char c = s_serial.read();
                if (c == '\n') {
                    line.trim();
                    if (line.length() > 0) {
                        Serial.printf("[AT] ← %s\n", line.c_str());
                    }
                    if (line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR")) {
                        break;
                    }
                    line = "";
                } else if (c != '\r') {
                    line += c;
                }
            } else {
                delay(10);
            }
        }
        if (millis() - start >= (uint32_t)timeout) {
            Serial.println("[AT] TIMEOUT");
        }
    } else {
        // TinyGSM-Modus (Task laeuft normal)
        Serial.printf("[AT] → %s (timeout %lds)\n", cmd, timeout / 1000);
        s_modem.sendAT(cmd);
        String resp = "";
        int r = s_modem.waitResponse(timeout, resp);
        resp.trim();
        if (resp.length() > 0) {
            Serial.printf("[AT] ← %s\n", resp.c_str());
        }
        Serial.printf("[AT] rc=%d (%s)\n", r, r == 1 ? "OK" : r == 2 ? "ERROR" : "TIMEOUT/OTHER");
    }

    if (need_resume) vTaskResume(s_modem_task_handle);
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

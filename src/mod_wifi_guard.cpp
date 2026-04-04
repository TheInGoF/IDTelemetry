#include "mod_wifi_guard.h"
#include "mod_web.h"
#include "mod_sleep.h"
#include "mod_pmu.h"
#include "mod_config.h"
#include "mod_logs.h"
#include "mod_rtc.h"
#include "secrets.h"
#include <WiFi.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>


// Syslog-Queue: async-safe, kein SPIFFS-Write aus WebSocket-Callbacks
#define SYSLOG_QUEUE_LEN 8
static char  syslog_q[SYSLOG_QUEUE_LEN][128];
static char  syslog_q_cat[SYSLOG_QUEUE_LEN][12];
static volatile uint8_t syslog_q_head = 0;
static volatile uint8_t syslog_q_tail = 0;
static volatile bool syslog_flush_pending = false;

static void syslog_q_push(const char* cat, const char* msg) {
    uint8_t next = (syslog_q_head + 1) % SYSLOG_QUEUE_LEN;
    if (next == syslog_q_tail) return; // voll, verwerfen
    strncpy(syslog_q_cat[syslog_q_head], cat, 11);
    strncpy(syslog_q[syslog_q_head], msg, 127);
    syslog_q_head = next;
    syslog_flush_pending = true;
}

static void syslog_q_flush() {
    while (syslog_q_tail != syslog_q_head) {
        syslog(syslog_q_cat[syslog_q_tail], syslog_q[syslog_q_tail]);
        syslog_q_tail = (syslog_q_tail + 1) % SYSLOG_QUEUE_LEN;
    }
}

// RGB LED: ESP32-S3 DevKitC-1 WS2812 auf GPIO48
// neopixelWrite(pin,r,g,b): direkter RMT-Write ohne interne Brightness-Skalierung
// neopixelWrite() skaliert intern mit RGB_BRIGHTNESS aus pins_arduino.h -> immer 100%
// Werte direkt 0-255, keine interne Skalierung
#define RGB_DIM        76  // rot
#define RGB_DIM_GREEN  13  // gruen 5%
#define RGB_ORANGE_R   25  // orange R 10%
#define RGB_ORANGE_G   10  // orange G 10%
#define RGB_ORANGE_B    0
static void rgb_led_off() {
    neopixelWrite(48, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    neopixelWrite(48, 0, 0, 0);
}

static void rgb_led_orange() {
    neopixelWrite(48, RGB_ORANGE_R, RGB_ORANGE_G, RGB_ORANGE_B);
}


static void rgb_restore(); // forward declaration
extern bool guard_can_tx_allowed(); // forward declaration

static void rgb_flash_red(uint32_t ms = 500) {
    neopixelWrite(48, RGB_DIM, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(ms));
    rgb_led_off();    // sicher aus
    rgb_restore();    // Zustand wiederherstellen
}

// ============================================================
//  mod_wifi_guard - WiFi SSID Wächter
// ============================================================

typedef enum {
    WGUARD_IDLE,      // kein SSID gesetzt
    WGUARD_SCANNING,  // aktiver Scan
    WGUARD_LOCKED,    // SSID gefunden
    WGUARD_CHECK,     // Rescan nach 60s
    WGUARD_LOST       // nicht gesehen → TX gesperrt
} WGuardState;

static WGuardState  wstate          = WGUARD_IDLE;
static char         guard_ssid[64]  = "";
static int          rssi_threshold  = WIFI_RSSI_THRESHOLD_DEF;
static uint8_t      guard_mode      = GUARD_MODE_WIFI; // default: WiFi Guard
static bool         wifi_in_range   = false;
static int          wifi_rssi_last  = -999;
static uint32_t     lock_timer_ms   = 0;
static bool         g_manual_tx_unlock = false;  // manuell via Button entsperrt
static int          g_miss_count    = 0;   // aufeinanderfolgende Scan-Fehlschlaege
static uint32_t     check_timer_ms   = 0;
static uint32_t     runtime_base_s   = 0;    // Laufzeit-Basis aus SPIFFS
static uint32_t     runtime_boot_ms  = 0;    // millis() beim Start
static uint32_t     time_save_last_s = 0;    // wann zuletzt Laufzeit gespeichert
static Preferences  prefs;                    // NVS fuer Config (ueberlebt Filesystem-Upload)
static volatile bool scan_trigger    = false; // sofortiger Scan nach SSID-Aenderung
static uint8_t       ap_clients_last  = 0;    // letzter bekannter AP-Clientstand

static void rgb_restore() {
    if (WiFi.softAPgetStationNum() > 0) {
        // Client verbunden -> orange
        neopixelWrite(48, RGB_ORANGE_R, RGB_ORANGE_G, RGB_ORANGE_B);
    } else if (guard_can_tx_allowed()) {
        // TX erlaubt, kein Client -> gruen (10%)
        neopixelWrite(48, 0, RGB_DIM_GREEN, 0);
    } else {
        // TX gesperrt, kein Client -> aus
        neopixelWrite(48, 0, 0, 0);
    }
}


// ============================================================
//  SPIFFS
// ============================================================
static uint32_t runtime_now_s() {
    return runtime_base_s + (millis() - runtime_boot_ms) / 1000;
}

static void spiffs_save_time() {
    uint32_t rt = runtime_now_s();
    File ft = SPIFFS.open(SPIFFS_WIFI_TIME, "w");
    if (ft) { ft.println(rt); ft.close(); }
}


static void spiffs_load() {
    // Config aus NVS laden (ueberlebt Filesystem-Upload)
    prefs.begin("wguard", true);
    String ssid = prefs.getString("ssid", "");
    if (ssid.length() > 0) {
        strncpy(guard_ssid, ssid.c_str(), 63);
    } else if (strlen(SECRET_GUARD_SSID) > 0) {
        // Erster Start / NVS leer → Initial-SSID aus secrets.h übernehmen
        strncpy(guard_ssid, SECRET_GUARD_SSID, 63);
        Serial.printf("[WGUARD] NVS leer — Initial-SSID aus secrets.h: \"%s\"\n", guard_ssid);
        prefs.end();
        prefs.begin("wguard", false);
        prefs.putString("ssid", guard_ssid);  // sofort in NVS sichern
    }
    rssi_threshold = prefs.getInt("rssi",   WIFI_RSSI_THRESHOLD_DEF);
    guard_mode     = prefs.getUChar("mode", GUARD_MODE_WIFI);
    prefs.end();
    Serial.printf("[WGUARD] SSID geladen: \"%s\"\n", guard_ssid);
    Serial.printf("[WGUARD] Schwelle: %d dBm, Mode: %d\n", rssi_threshold, guard_mode);

    // Laufzeit aus SPIFFS laden
    runtime_boot_ms = millis();
    if (SPIFFS.exists(SPIFFS_WIFI_TIME)) {
        File ft = SPIFFS.open(SPIFFS_WIFI_TIME, "r");
        if (ft) {
            String l1 = ft.readStringUntil('\n'); l1.trim();
            if (l1.length()) {
                runtime_base_s = (uint32_t)l1.toInt();
                uint32_t rt = runtime_base_s;
                Serial.printf("[WGUARD] Laufzeit SPIFFS: %02d:%02d:%02d\n",
                              rt/3600, (rt%3600)/60, rt%60);
            }
            ft.close();
        }
    }
}

static void spiffs_save() {
    prefs.begin("wguard", false);
    prefs.putString("ssid",  guard_ssid);
    prefs.putInt("rssi",     rssi_threshold);
    prefs.putUChar("mode",   guard_mode);
    prefs.end();
}

static void spiffs_clear() {
    prefs.begin("wguard", false);
    prefs.remove("ssid");
    prefs.end();
    guard_ssid[0] = 0;
}

// ============================================================
//  WiFi Scan
// ============================================================
static bool wifi_scan_for_ssid() {
    if (strlen(guard_ssid) == 0) return false;

    // Async Scan starten
    Serial.println("[WGUARD] Starte WiFi Scan (async)...");
    WiFi.scanNetworks(true, true); // async, hidden auch

    // Warten bis Scan fertig — max 8s, in 200ms Schritten
    int waited = 0;
    int n = WIFI_SCAN_RUNNING;
    while (n == WIFI_SCAN_RUNNING && waited < 8000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        n = WiFi.scanComplete();
    }

    if (n == WIFI_SCAN_FAILED || n == WIFI_SCAN_RUNNING) {
        Serial.println("[WGUARD] Scan fehlgeschlagen");
        WiFi.scanDelete();
        return false;
    }

    bool found = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == String(guard_ssid)) {
            wifi_rssi_last = WiFi.RSSI(i);
            found = (wifi_rssi_last >= rssi_threshold);
            Serial.printf("[WGUARD] SSID \"%s\" gefunden: RSSI=%d dBm (%s)\n",
                          guard_ssid, wifi_rssi_last,
                          found ? "IN RANGE" : "ZU WEIT");
            wifi_in_range = found;
            break;
        }
    }
    WiFi.scanDelete();
    if (!found && wifi_in_range) {
        wifi_in_range = false;
        Serial.printf("[WGUARD] SSID \"%s\" nicht gefunden\n", guard_ssid);
    }
    return found;
}

static const char* wstate_name(WGuardState s) {
    switch(s) {
        case WGUARD_IDLE:     return "IDLE";
        case WGUARD_SCANNING: return "SCANNING";
        case WGUARD_LOCKED:   return "LOCKED";
        case WGUARD_CHECK:    return "CHECK";
        case WGUARD_LOST:     return "LOST";
        default:              return "?";
    }
}

static void set_wstate(WGuardState s) {
    if (wstate != s) {
        Serial.printf("[WGUARD] State: %s → %s\n",
                      wstate_name(wstate), wstate_name(s));
        wstate = s;
    }
}

// ============================================================
//  WiFi Guard Task
// ============================================================
static void wifi_log_entry(int n) {
    if (!cfg_log_wifi()) return;
    File f = SPIFFS.open("/wifi.log", "a");
    if (!f) return;

    f.printf("[%s]\n", syslog_timestr());

    // Dedup + sortiert nach RSSI → nur Top 4 loggen
    int logged = 0;
    // Sortierung: bubble sort auf RSSI (n ist meist klein)
    // Wir iterieren mehrfach und nehmen jeweils das beste noch nicht geloggte
    bool used[32] = {};
    for (int rank = 0; rank < 4 && rank < n; rank++) {
        int best_idx = -1;
        for (int i = 0; i < n && i < 32; i++) {
            if (used[i]) continue;
            // Dedup: gibt es diese SSID schon mit besserem RSSI?
            bool dup = false;
            for (int j = 0; j < n && j < 32; j++) {
                if (j != i && !used[j] && WiFi.SSID(i) == WiFi.SSID(j) && WiFi.RSSI(j) > WiFi.RSSI(i)) {
                    dup = true; break;
                }
            }
            if (dup) { used[i] = true; continue; }
            if (best_idx < 0 || WiFi.RSSI(i) > WiFi.RSSI(best_idx)) best_idx = i;
        }
        if (best_idx < 0) break;
        used[best_idx] = true;
        int rssi = WiFi.RSSI(best_idx);
        if (rssi < -70) break; // nur Netzwerke innerhalb ~15m loggen
        const char* dist = rssi > -55 ? "<5m" : rssi > -65 ? "~10m" : "<15m";
        bool is_guard = (strlen(guard_ssid) > 0 && WiFi.SSID(best_idx) == guard_ssid);
        f.printf("%d. %-28s %4d %s%s\n",
                 rank+1, WiFi.SSID(best_idx).c_str(), rssi, dist,
                 is_guard ? " [G]" : "");
        logged++;
    }

    // Ringpuffer 256KB
    if (f.size() > 256 * 1024) {
        f.close();
        SPIFFS.remove("/wifi.log");
        Serial.println("[WGUARD] wifi.log Ringpuffer zurückgesetzt");
        return;
    }
    f.close();
}

static void ap_monitor_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(3000)); // AP erst stabil
    const uint32_t AP_TIMEOUT_MS = (uint32_t)AP_TIMEOUT_MIN * 60UL * 1000UL;
    uint32_t ap_no_client_since = millis(); // Zeitpunkt seit dem kein Client verbunden

    while (!g_shutdown) {
        // AP-Timeout: kein Client für X min → abschalten (0 = deaktiviert)
        if (AP_TIMEOUT_MS > 0 && web_ap_active() && WiFi.softAPgetStationNum() == 0) {
            if (millis() - ap_no_client_since >= AP_TIMEOUT_MS) {
                web_ap_stop();
            }
        }

        uint8_t ap_now = (uint8_t)WiFi.softAPgetStationNum();
        if (ap_now != ap_clients_last) {
            if (ap_now > ap_clients_last) {
                ap_no_client_since = millis(); // Timer pausieren solange Client da
                neopixelWrite(48, RGB_ORANGE_R, RGB_ORANGE_G, RGB_ORANGE_B);
                char _m[44]; snprintf(_m, 44, "AP verbunden · aktiv: %d", ap_now);
                syslog("CLIENT", _m);
            } else {
                g_manual_tx_unlock = false;
                rgb_restore();
                char _m[44]; snprintf(_m, 44, "AP getrennt · aktiv: %d", ap_now);
                syslog("CLIENT", _m);
                if (ap_now == 0) {
                    // Letzter Client weg → sofort Guard-Scan + AP-Timeout neu starten
                    scan_trigger = true;
                    ap_no_client_since = millis();
                    Serial.println("[WGUARD] Client 0 → Guard-Rescan + AP-Timeout reset");
                }
            }
            ap_clients_last = ap_now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    Serial.println("[WGUARD] AP-Monitor beendet (Shutdown)");
    vTaskDelete(NULL);
}

static void wifi_scan_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000)); // AP erst stabil

    while (!g_shutdown) {
        // Gepufferte Syslog-Eintraege schreiben (thread-safe aus Callbacks)
        syslog_q_flush();


        // Laufzeit jede Minute in SPIFFS sichern
        uint32_t now_s = millis() / 1000;
        if (now_s - time_save_last_s >= 60) {
            spiffs_save_time();
            time_save_last_s = now_s;
        }

        bool client_active = (WiFi.softAPgetStationNum() > 0); // Live-Abfrage, kein Cache
        bool guard_set     = (strlen(guard_ssid) > 0);

        // Scan NIE während Client verbunden — AP-Interferenz
        // scan_trigger bleibt aktiv bis Client weg ist
        bool force_scan = scan_trigger && !client_active;
        if (force_scan) { scan_trigger = false; }

        // WiFi-Scan nur wenn explizit per Web-UI angefordert (scan_trigger)
        // Sleep + CAN TX hängen an VBUS — kein periodischer Scan nötig
        if (force_scan && !client_active) {
            WiFi.scanDelete();
            WiFi.scanNetworks(true, true);

            uint32_t t = millis();
            while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t < 8000) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_FAILED) {
                Serial.println("[WGUARD] Scan fehlgeschlagen — sofort retry");
                WiFi.scanDelete();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue; // Scan-Zyklus sofort neu starten
            }
            if (n >= 0) {
                // Dedup: gleiche SSID nur einmal (stärkster RSSI behalten)
                // Einfache Methode: beim Loggen und Guard-Check doppelte überspringen
                // Guard prüfen (bester RSSI der SSID)
                bool found = false;
                int  best_rssi = -999;
                for (int i = 0; i < n; i++) {
                    if (WiFi.SSID(i) == guard_ssid) {
                        if (WiFi.RSSI(i) > best_rssi) best_rssi = WiFi.RSSI(i);
                        if (WiFi.RSSI(i) >= rssi_threshold) found = true;
                    }
                }
                if (found) {
                    g_miss_count  = 0;
                    wifi_in_range = true;
                    if (wstate != WGUARD_LOCKED) { char _m[56]; snprintf(_m,56,"Locked · %.20s · %d dBm",guard_ssid,best_rssi); syslog("GUARD",_m); }
                    set_wstate(WGUARD_LOCKED);
                } else {
                    g_miss_count++;
                    if (g_miss_count >= 2) {
                        wifi_in_range = false;
                        if (wstate != WGUARD_LOST) { char _m[56]; snprintf(_m,56,"Lost · %.20s · %d dBm",guard_ssid,best_rssi); syslog("GUARD",_m); }
                        set_wstate(WGUARD_LOST);
                    }
                }

                wifi_log_entry(n);
                // Wenn kein Guard gesetzt: State IDLE
                if (!guard_set) {
                    set_wstate(WGUARD_IDLE);
                    wifi_in_range = true;
                }
            }
        }

        // Kein periodischer Scan → längeres Idle (30s), syslog-Queue trotzdem flushen
        int wait_slots = 60;
        for (int _w = 0; _w < wait_slots; _w++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (syslog_flush_pending) {
                syslog_flush_pending = false;
                syslog_q_flush();
            }
            if (scan_trigger) break; // sofortiger Scan wenn angefordert
        }
    }
    syslog("GUARD", "Task beendet (Shutdown)");
    vTaskDelete(NULL);
}

// ============================================================
//  Public API
// ============================================================
void wifi_guard_init() {
    // RGB LED: kurz rot blinken als Boot-Signal, danach dauerhaft aus
    // Behebt: Bootloader haelt LED manchmal dauerhaft gruen
    neopixelWrite(48, RGB_DIM, 0, 0); // rot an
    delay(150);
    neopixelWrite(48, 0, 0, 0);   // aus
    delay(50);
    neopixelWrite(48, 0, 0, 0);   // sicher aus
    // rgb_restore() erst nach spiffs_load() weiter unten
    spiffs_load();
    // Guard-Logik läuft jetzt über VBUS (mod_sleep + mod_can)
    // WiFi-Scan deaktiviert — nur AP-Monitor für Client-Tracking + syslog-Queue
    wstate        = WGUARD_IDLE;
    wifi_in_range = true;
    syslog("GUARD", "VBUS-Modus — kein WiFi-Scan");

    rgb_restore();
    // AP-Client-Monitor: LED + Client-Tracking
    xTaskCreatePinnedToCore(ap_monitor_task, "AP_MON", 3072, NULL, 1, NULL, 0);

    if (false) { // Toter Zweig — nur damit der Compiler nicht meckert
        wifi_in_range = true;
    }
}

void wifi_guard_set_ssid(const char* ssid, int rssi_thresh) {
    strncpy(guard_ssid, ssid, 63);
    guard_ssid[63]  = '\0';
    rssi_threshold  = rssi_thresh;
    wifi_in_range   = false;
    wstate          = WGUARD_LOST;
    g_miss_count    = 0;
    scan_trigger    = true;
    spiffs_save();
    Serial.printf("[WGUARD] SSID gesetzt: \"%s\" Schwelle: %d dBm\n",
                  guard_ssid, rssi_threshold);
}

void wifi_guard_clear_ssid() {
    guard_ssid[0]  = '\0';
    wifi_in_range  = true;
    wstate         = WGUARD_IDLE;
    spiffs_clear();
    Serial.println("[WGUARD] SSID gelöscht");
}

void wifi_guard_set_mode(uint8_t mode) {
    guard_mode = mode;
    spiffs_save();
    // WiFi-only oder AND: WiFi-Status zurücksetzen
    if (mode == GUARD_MODE_BLE) wifi_in_range = true;
    else wifi_in_range = false;
    wstate = WGUARD_LOST;
    Serial.printf("[WGUARD] Mode gesetzt: %d\n", mode);
}

const char* wifi_guard_get_ssid()      { return guard_ssid; }
int         wifi_guard_get_threshold() { return rssi_threshold; }
uint8_t     wifi_guard_get_mode()      { return guard_mode; }
bool        wifi_guard_active()        { return strlen(guard_ssid) > 0; }
bool        wifi_guard_in_range()      { return wifi_in_range; }
int         wifi_guard_rssi()          { return wifi_rssi_last; }
const char* wifi_guard_state_str()     { return wstate_name(wstate); }

// ============================================================
//  Haupt-Entscheidung CAN TX
// ============================================================
bool guard_can_tx_allowed() {
    // Manuell entsperrt (Button) → erlaubt solange Client da
    if (g_manual_tx_unlock) return true;

    // AP-Client verbunden → immer erlaubt (z.B. Diagnose-App über eigenen Hotspot)
    if (WiFi.softAPgetStationNum() > 0) return true;

    // CAN TX nur bei externer Spannung — Auto schläft ohne VBUS,
    // CAN-Writes könnten Steuergeräte aufwecken und Alarm auslösen
    return pmu_is_vbus_in();
}

void wifi_guard_manual_tx_unlock() {
    g_manual_tx_unlock = true;
    Serial.println("[WGUARD] TX manuell entsperrt");
    syslog_q_push("TX", "manuell entsperrt");
}

bool wifi_guard_manual_unlocked() { return g_manual_tx_unlock; }

void wifi_guard_set_time(int hour, int minute, int second) {
    (void)hour; (void)minute; (void)second; // Laufzeit wird nicht vom Browser gesetzt
}

void wifi_guard_client_connected() {
    // WebSocket-Connect — LED + Log, Zählung über ap_monitor_task
    neopixelWrite(48, RGB_ORANGE_R, RGB_ORANGE_G, RGB_ORANGE_B);
    uint8_t n = WiFi.softAPgetStationNum();
    { char _m[48]; snprintf(_m, 48, "WebSocket verbunden  (AP: %d)", n); syslog_q_push("CLIENT", _m); }
}

void wifi_guard_client_disconnected() {
    // WebSocket-Disconnect — LED + Log
    uint8_t n = WiFi.softAPgetStationNum();
    if (n == 0) {
        g_manual_tx_unlock = false;
        neopixelWrite(48, 0, 0, 0);
    }
    { char _m[48]; snprintf(_m, 48, "WebSocket getrennt   (AP: %d)", n); syslog_q_push("CLIENT", _m); }
}

bool wifi_guard_client_active() {
    return WiFi.softAPgetStationNum() > 0;
}

// ============================================================
//  Status JSON für Weboberfläche
// ============================================================
String wifi_guard_status_json() {
    JsonDocument doc;
    doc["ssid"]       = guard_ssid;
    doc["active"]     = wifi_guard_active();
    doc["in_range"]   = wifi_in_range;
    doc["rssi"]       = wifi_rssi_last;
    doc["threshold"]  = rssi_threshold;
    doc["state"]      = wstate_name(wstate);
    doc["mode"]       = guard_mode;
    const char* ms = "AUS";
    switch (guard_mode) {
        case GUARD_MODE_WIFI: ms = "WiFi Guard";    break;
        case GUARD_MODE_AND:  ms = "WiFi+BLE AND";  break;
        case GUARD_MODE_OR:   ms = "WiFi|BLE OR";   break;
        case GUARD_MODE_VBUS: ms = "VBUS Guard";    break;
    }
    doc["mode_str"]   = ms;
    doc["ble_ok"]        = true;
    doc["wifi_ok"]       = wifi_in_range;
    doc["vbus_ok"]       = pmu_is_vbus_in();
    doc["can_tx"]        = guard_can_tx_allowed();
    doc["manual_unlock"] = g_manual_tx_unlock;
    String out; serializeJson(doc, out);
    return out;
}
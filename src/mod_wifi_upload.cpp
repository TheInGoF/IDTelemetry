#include "mod_wifi_upload.h"
#include "mod_logs.h"
#include "mod_telem_store.h"
#include "mod_telemetry.h"
#include "mod_sleep.h"
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#if FEATURE_WIFI_UPLOAD

// ============================================================
//  Two-slot priority WiFi STA + future HTTP-POST drain.
//
//  Lifecycle each ~5 s tick:
//    if not associated:
//        passively scan the air
//        for slot in priority order:
//            if slot.ssid in scan results -> WiFi.begin(slot)
//        break
//    else:
//        TODO: drain telem_store via HTTP POST against slots[active].url
//
//  AP+STA coexistence:
//    web_init() brings the softAP up first. WiFi.begin() while in AP-only
//    mode auto-switches the radio to APSTA. The softAP channel will follow
//    the STA's channel, which can confuse phones already connected to the
//    AP. Acceptable for our use case — users typically open the AP only
//    for setup, then close the phone tab.
// ============================================================

struct Slot {
    char ssid[64];
    char pass[64];
    char url[128];
};
static Slot      s_slots[WIFI_UPLOAD_SLOTS] = {};
static int       s_active_slot               = -1;
static uint32_t  s_uploaded                  = 0;
static bool      s_task_running              = false;
static uint32_t  s_last_scan_ms              = 0;
static const uint32_t SCAN_INTERVAL_MS       = 30000UL;
static const uint32_t CONNECT_TIMEOUT_MS     = 12000UL;

void wifi_upload_configure_slot(int idx, const char* ssid, const char* pass,
                                const char* post_url) {
    if (idx < 0 || idx >= WIFI_UPLOAD_SLOTS) return;
    Slot& s = s_slots[idx];
    if (ssid)     { strncpy(s.ssid, ssid, sizeof(s.ssid) - 1); s.ssid[sizeof(s.ssid) - 1] = '\0'; }
    else          { s.ssid[0] = '\0'; }
    if (pass)     { strncpy(s.pass, pass, sizeof(s.pass) - 1); s.pass[sizeof(s.pass) - 1] = '\0'; }
    else          { s.pass[0] = '\0'; }
    if (post_url) { strncpy(s.url, post_url, sizeof(s.url) - 1); s.url[sizeof(s.url) - 1] = '\0'; }
    else          { s.url[0] = '\0'; }

    char m[96];
    snprintf(m, sizeof(m), "Slot %d: ssid=%s url=%s",
             idx,
             s.ssid[0] ? s.ssid : "(none)",
             s.url[0]  ? s.url  : "(none)");
    syslog("WIFI_UP", m);
}

bool wifi_upload_is_connected() {
    return (WiFi.getMode() & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED;
}

int wifi_upload_active_slot() { return s_active_slot; }
uint32_t wifi_upload_count()  { return s_uploaded; }

// Returns slot index (0/1) of strongest in-range configured SSID,
// preferring slot 0 over slot 1 on ties. -1 if none in range.
static int find_best_slot_in_range() {
    int n = WiFi.scanNetworks();   // synchronous, ~2-3 s
    if (n <= 0) return -1;

    int best = -1;
    int best_rssi = -127;
    for (int i = 0; i < n; i++) {
        String found = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        for (int s = 0; s < WIFI_UPLOAD_SLOTS; s++) {
            if (!s_slots[s].ssid[0]) continue;
            if (found != s_slots[s].ssid) continue;
            // Priority wins absolutely (slot 0 > slot 1), then signal.
            if (best < 0 || s < best || (s == best && rssi > best_rssi)) {
                best = s;
                best_rssi = rssi;
            }
        }
    }
    WiFi.scanDelete();
    return best;
}

static bool try_connect(int slot) {
    if (slot < 0 || slot >= WIFI_UPLOAD_SLOTS) return false;
    if (!s_slots[slot].ssid[0]) return false;

    char m[80];
    snprintf(m, sizeof(m), "Slot %d verbinden: %s", slot, s_slots[slot].ssid);
    syslog("WIFI_UP", m);

    WiFi.begin(s_slots[slot].ssid, s_slots[slot].pass);
    uint32_t start = millis();
    while (millis() - start < CONNECT_TIMEOUT_MS) {
        if (WiFi.status() == WL_CONNECTED) {
            s_active_slot = slot;
            snprintf(m, sizeof(m), "Slot %d STA verbunden · IP %s",
                     slot, WiFi.localIP().toString().c_str());
            syslog("WIFI_UP", m);
            return true;
        }
        if (g_shutdown) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    snprintf(m, sizeof(m), "Slot %d Timeout", slot);
    syslog("WIFI_UP", m);
    WiFi.disconnect();
    return false;
}

static bool any_slot_enabled() {
    for (int s = 0; s < WIFI_UPLOAD_SLOTS; s++) {
        if (s_slots[s].ssid[0]) return true;
    }
    return false;
}

static void wifi_upload_task(void*) {
    s_task_running = true;
    while (!g_shutdown) {
        if (!any_slot_enabled()) {
            // Nothing to do — sleep long, check again later in case user
            // configures via Web UI without a reboot.
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        if (!wifi_upload_is_connected()) {
            if (s_active_slot >= 0) {
                char m[48]; snprintf(m, sizeof(m), "Slot %d getrennt", s_active_slot);
                syslog("WIFI_UP", m);
                s_active_slot = -1;
            }
            // Rate-limit scans
            uint32_t now = millis();
            if (now - s_last_scan_ms >= SCAN_INTERVAL_MS) {
                s_last_scan_ms = now;
                int slot = find_best_slot_in_range();
                if (slot >= 0) {
                    try_connect(slot);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // TODO: connected — drain telem_store via HTTP POST against
        // s_slots[s_active_slot].url. See docs/algorithms.md pseudo-code.
        // Encrypt rows with AES-256-CBC (same format as MQTT path) and
        // increment s_uploaded on each accepted row.
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (wifi_upload_is_connected()) WiFi.disconnect(true);
    s_task_running = false;
    vTaskDelete(nullptr);
}

void wifi_upload_start_task() {
    if (s_task_running) return;
    xTaskCreatePinnedToCore(wifi_upload_task, "WIFI_UP", 4096, nullptr, 1, nullptr, 0);
    syslog("WIFI_UP", "Task gestartet (Slot-Scan aktiv, Upload-Loop TODO)");
}

#endif  // FEATURE_WIFI_UPLOAD

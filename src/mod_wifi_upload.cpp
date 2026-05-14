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
//  Scope (initial scaffolding)
//
//  This module currently:
//    - keeps STA credentials + endpoint URL in RAM
//    - starts a FreeRTOS task that periodically checks STA state
//    - logs status changes so we can observe the lifecycle
//
//  Not yet implemented (TODOs):
//    - WiFi.begin(ssid, pass) and reconnect/backoff
//    - HTTP(S) POST of rows from telem_store
//    - AES-256-CBC encryption of the binary payload, identical to the
//      MQTT side (see build_binary() / encrypt_payload() in mod_mqtt.cpp)
//    - Server endpoint contract (multipart? raw bytes? batch size?)
//    - Auth (presumably the AES key acts as proof-of-possession)
//
//  Both build variants compile this module. The full variant treats it
//  as a secondary path that drains the queue when LTE is down and the
//  user is on home WiFi. The lite variant has no LTE and uses this as
//  the only upload path.
// ============================================================

static char s_ssid[64]  = "";
static char s_pass[64]  = "";
static char s_url[128]  = "";
static volatile uint32_t s_uploaded = 0;
static volatile bool     s_task_running = false;

void wifi_upload_configure(const char* sta_ssid, const char* sta_pass,
                           const char* post_url) {
    if (sta_ssid) { strncpy(s_ssid, sta_ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = '\0'; }
    if (sta_pass) { strncpy(s_pass, sta_pass, sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = '\0'; }
    if (post_url) { strncpy(s_url,  post_url, sizeof(s_url)  - 1); s_url[sizeof(s_url)  - 1] = '\0'; }

    char m[96];
    snprintf(m, sizeof(m), "configure: ssid=%s url=%s",
             s_ssid[0] ? s_ssid : "(none)",
             s_url[0]  ? s_url  : "(none)");
    syslog("WIFI_UP", m);
}

bool wifi_upload_is_connected() {
    return WiFi.getMode() & WIFI_MODE_STA && WiFi.status() == WL_CONNECTED;
}

uint32_t wifi_upload_count() {
    return s_uploaded;
}

static void wifi_upload_task(void*) {
    s_task_running = true;
    bool was_connected = false;
    while (!g_shutdown) {
        bool connected = wifi_upload_is_connected();
        if (connected != was_connected) {
            syslog("WIFI_UP", connected ? "STA verbunden" : "STA getrennt");
            was_connected = connected;
        }
        // TODO: when connected, drain telem_store via HTTP POST.
        // Pseudo-code (see docs/algorithms.md):
        //   while telem_store_pending() > 0 and sent < BATCH:
        //       TelemetryRow row;
        //       if !telem_store_peek(row): break
        //       if !http_post_row(row): break
        //       telem_store_ack()
        //       sent++
        //       s_uploaded++
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    s_task_running = false;
    vTaskDelete(nullptr);
}

void wifi_upload_start_task() {
    if (s_task_running) return;
    xTaskCreatePinnedToCore(wifi_upload_task, "WIFI_UP", 4096, nullptr, 1, nullptr, 0);
    syslog("WIFI_UP", "Task gestartet (scaffolding — kein Upload aktiv)");
}

#endif  // FEATURE_WIFI_UPLOAD

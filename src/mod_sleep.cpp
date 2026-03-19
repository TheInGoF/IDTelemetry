#include "mod_sleep.h"
#include "mod_gyro.h"
#include "mod_logs.h"
#include "mod_telemetry.h"
#include "mod_modem.h"
#include "mod_can.h"
#include "mod_wifi_guard.h"
#include "mod_pmu.h"
#include "config.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>

// ============================================================
//  mod_sleep - Deep Sleep Management
// ============================================================

#define SLEEP_NO_GUARD_MS  (5UL * 60UL * 1000UL)  // 5 min ohne Guard → Sleep

// Globales Shutdown-Flag: wird von enter_deep_sleep() gesetzt.
// Alle FreeRTOS-Tasks prüfen dies in ihrer Loop und beenden sich sauber,
// damit shared Resources (I2C, CAN, WiFi) nicht im gesperrten Zustand bleiben.
volatile bool g_shutdown = false;

static uint32_t g_boot_ms = 0;
static uint32_t g_last_guard_seen_ms = 0;  // letzter Zeitpunkt mit Guard in Range
static esp_sleep_wakeup_cause_t g_wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;

static void enter_deep_sleep(const char* reason);

// ── Init: Wake-Grund loggen ───────────────────────────────
void sleep_init() {
    g_boot_ms = millis();

    g_wake_cause = esp_sleep_get_wakeup_cause();
    if (g_wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
        syslog("WAKE", "Deep Sleep beendet: Gyro-Interrupt (GPIO3)");
        Serial.printf("[SLEEP] Aufgewacht: Gyro-Motion-Interrupt GPIO%d\n", (int)GYRO_WAKE_PIN);
    } else {
        syslog("BOOT", "Normaler Start (PowerOn / Reset)");
        Serial.println("[SLEEP] Normaler Boot (kein Deep Sleep)");
    }
}

// ── Nach gyro_init(): Wake-Details loggen ─────────────────
void sleep_log_wake() {
    if (g_wake_cause != ESP_SLEEP_WAKEUP_EXT1) return;
    char msg[80];
    snprintf(msg, sizeof(msg), "Gyro-Wake · Wake-Schwelle: %.2fG (MOT_THR=%d)",
             gyro_get_mot_threshold() * 0.032f, gyro_get_mot_threshold());
    syslog("WAKE", msg);
}

// ── Update: im loop() aufrufen ────────────────────────────
void sleep_update() {
    uint32_t now = millis();

    // ─── Guard-Status ermitteln (WiFi oder VBUS) ──────────
    uint8_t mode = wifi_guard_get_mode();
    bool guard_active = false;
    bool guard_in_range = false;

    if (mode == GUARD_MODE_VBUS) {
        guard_active   = true;
        guard_in_range = pmu_is_vbus_in();  // ext. Spannung = "in Range"
    } else if (wifi_guard_active()) {
        guard_active   = true;
        guard_in_range = wifi_guard_in_range();
    }

    // Guard-Tracking: merken wann Guard zuletzt in Range war
    if (!guard_active || guard_in_range) {
        g_last_guard_seen_ms = now;
    }

    // Guard aktiv + in Range → wach bleiben
    if (guard_active && guard_in_range) return;

    // Guard aktiv aber nicht sichtbar → 5 min → Sleep
    bool guard_sleep = false;
    if (guard_active && !guard_in_range) {
        guard_sleep = (now - g_last_guard_seen_ms) >= SLEEP_NO_GUARD_MS;
    }

    // Kein Guard → Gyro-Fallback (10 min Ruhe)
    bool gyro_sleep = false;
    if (!guard_active && gyro_ok()) {
        uint32_t last_shake = gyro_last_shake_ms();
        uint32_t idle_since = (last_shake > 0) ? last_shake : g_boot_ms;
        gyro_sleep = (now - idle_since) >= SLEEP_INACTIVITY_MS;
    }

    if (!gyro_sleep && !guard_sleep) return;

    // ─── Sleep-Schwelle erreicht ─────────────────────────
    static uint32_t s_last_client_ms = 0;  // letzter Zeitpunkt mit Client
    if (WiFi.softAPgetStationNum() > 0) {
        s_last_client_ms = now;
        static uint32_t last_log_ms = 0;
        if (now - last_log_ms >= 60000UL) {
            last_log_ms = now;
            syslog("SLEEP", "Sleep verhindert: Client verbunden");
            Serial.println("[SLEEP] Sleep verhindert: Client verbunden");
        }
        return;
    }

    // Schonfrist: 30s nach letztem Client warten (Browser-Seitenwechsel)
    if (s_last_client_ms > 0 && (now - s_last_client_ms) < 30000UL) return;

    // ─── Kein Client → Deep Sleep ───────────────────────
    char msg[96];
    if (guard_sleep) {
        snprintf(msg, sizeof(msg), "Deep Sleep: Guard-SSID %lu min nicht sichtbar",
                 SLEEP_NO_GUARD_MS / 60000UL);
    } else {
        snprintf(msg, sizeof(msg), "Deep Sleep: %lu min keine Gyro-Bewegung",
                 SLEEP_INACTIVITY_MS / 60000UL);
    }
    enter_deep_sleep(msg);
}

// ── Force Sleep: sofort einschlafen (Serial-Befehl / Debug) ──
void sleep_force() {
    enter_deep_sleep("Deep Sleep: manuell ausgelöst (Serial)");
}

// ── Hilfsfunktion: prüfen ob ein Task noch existiert ──
static bool task_alive(const char* name) {
    return xTaskGetHandle(name) != nullptr;
}

// ── Gemeinsamer Sleep-Ablauf ─────────────────────────────
static void enter_deep_sleep(const char* reason) {
    syslog("SLEEP", reason);
    Serial.printf("[SLEEP] %s\n", reason);
    Serial.printf("[SLEEP] Wake-up via GPIO%d (MPU-6050 INT)\n", (int)GYRO_WAKE_PIN);

    // ── 1. Telemetrie sichern (noch im laufenden System) ──
    telem_persist_to_spiffs();
    Serial.println("[SLEEP] Telem gesichert");

    // ── 2. Shutdown-Signal an alle Tasks ──
    //    Tasks prüfen g_shutdown in ihrer Loop und beenden sich sauber,
    //    damit I2C/CAN/WiFi-Transaktionen ordentlich abgeschlossen werden.
    g_shutdown = true;
    Serial.println("[SLEEP] Shutdown-Signal gesetzt");

    // Warten bis alle Tasks sich beendet haben (max 2s)
    const char* task_names[] = {
        "TELEM", "MODEM", "GYRO", "WIFI_SCAN", "AP_MON", "MON", "RTC_SYNC", "elm_worker"
    };
    uint32_t wait_start = millis();
    bool all_stopped = false;
    while (millis() - wait_start < 2000) {
        all_stopped = true;
        for (auto name : task_names) {
            if (task_alive(name)) {
                all_stopped = false;
                break;
            }
        }
        if (all_stopped) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (all_stopped) {
        Serial.println("[SLEEP] Alle Tasks sauber beendet");
    } else {
        // Fallback: noch lebende Tasks brutal killen
        Serial.println("[SLEEP] WARNUNG: Timeout — erzwinge Task-Stop");
        for (auto name : task_names) {
            TaskHandle_t h = xTaskGetHandle(name);
            if (h && h != xTaskGetCurrentTaskHandle()) {
                Serial.printf("[SLEEP]   Kill: %s\n", name);
                vTaskDelete(h);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ── 3. Hardware herunterfahren ──
    modem_poweroff();
    Serial.println("[SLEEP] Modem aus");

    can_stop();
    Serial.println("[SLEEP] CAN aus");

    // BLE komplett stoppen — verhindert Zombie-BLE nach fehlgeschlagenem Sleep
    NimBLEDevice::deinit(true);
    Serial.println("[SLEEP] BLE aus");

    // WiFi komplett aus
    WiFi.scanDelete();
    WiFi.softAPdisconnect(false);
    WiFi.mode(WIFI_OFF);
    Serial.println("[SLEEP] WiFi aus");

    Serial.flush();
    delay(100);

    // ── 4. Gyro Wake-up konfigurieren ──
    gyro_configure_sleep_int();
    Serial.println("[SLEEP] Gyro INT konfiguriert");

    // GPIO3 als RTC-GPIO für Deep Sleep konfigurieren
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // RTC-Peripherie an
    rtc_gpio_init(GYRO_WAKE_PIN);
    rtc_gpio_set_direction(GYRO_WAKE_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en(GYRO_WAKE_PIN);   // Pull-Down: Pin ist LOW wenn kein INT
    rtc_gpio_pullup_dis(GYRO_WAKE_PIN);    // Pull-Up aus

    // Aktuellen Pin-Zustand prüfen (sollte LOW sein, MPU-6050 INT nicht aktiv)
    int pin_state = rtc_gpio_get_level(GYRO_WAKE_PIN);
    Serial.printf("[SLEEP] GPIO%d Zustand: %s\n", (int)GYRO_WAKE_PIN,
                  pin_state ? "HIGH (INT aktiv!)" : "LOW (bereit)");

    // EXT1 Wake-up: GPIO3 HIGH = MPU-6050 Motion Interrupt
    esp_sleep_enable_ext1_wakeup(1ULL << GYRO_WAKE_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
    Serial.println("[SLEEP] EXT1 Wake konfiguriert — schlafen...");
    Serial.flush();
    delay(100);  // MPU-6050 Zeit geben sich zu stabilisieren

    esp_deep_sleep_start();

    // ── Fallback: wenn esp_deep_sleep_start() nicht greift → Neustart ──
    Serial.println("[SLEEP] FEHLER: Deep Sleep fehlgeschlagen — Neustart!");
    Serial.flush();
    delay(100);
    ESP.restart();
}

#pragma once
#include "shared.h"

// ============================================================
//  mod_wifi_guard - WiFi SSID Wächter
//
//  Scannt alle 15s nach einer konfigurierten SSID (Fahrzeug-Hotspot)
//  RSSI-Schwelle konfigurierbar (default -75 dBm ≈ ~5m)
//
//  Guard Modes (in config.h):
//   GUARD_MODE_BLE  (0) → nur BLE entscheidet
//   GUARD_MODE_WIFI (1) → nur WiFi entscheidet
//   GUARD_MODE_AND  (2) → BLE UND WiFi müssen sichtbar sein
//   GUARD_MODE_OR   (3) → BLE ODER WiFi reicht
//
//  SPIFFS Persistenz:
//   /wifi_ssid.txt  → gespeicherte SSID
//   /wifi_cfg.txt   → RSSI-Schwelle + Guard Mode
// ============================================================

#define WIFI_GUARD_SCAN_INTERVAL_S  15
#define WIFI_GUARD_LOCK_MS      120000   // 2min nach Fund
#define WIFI_GUARD_CHECK_MS      60000   // 60s bis Rescan
#define WIFI_RSSI_THRESHOLD_DEF    -75   // dBm default

#define SPIFFS_WIFI_TIME   "/wifi_time.txt"

// Guard Modi
#define GUARD_MODE_BLE    0
#define GUARD_MODE_WIFI   1
#define GUARD_MODE_AND    2
#define GUARD_MODE_OR     3
#define GUARD_MODE_VBUS   4

void        wifi_guard_init();

void        wifi_guard_set_ssid(const char* ssid, int rssi_threshold);
void        wifi_guard_clear_ssid();
void        wifi_guard_set_mode(uint8_t mode);   // GUARD_MODE_*

const char* wifi_guard_get_ssid();
int         wifi_guard_get_threshold();
uint8_t     wifi_guard_get_mode();
bool        wifi_guard_active();
bool        wifi_guard_in_range();
int         wifi_guard_rssi();
const char* wifi_guard_state_str();

// Haupt-Entscheidung: darf CAN TX?
// Berücksichtigt Guard Mode + BLE + WiFi Status
bool        guard_can_tx_allowed();

// Manuelles TX-Entsperren (nur wenn Client verbunden)
// Wird automatisch zurückgesetzt wenn kein Client mehr da
void        wifi_guard_manual_tx_unlock();
bool        wifi_guard_manual_unlocked();

// Dashboard Client Tracking
void        wifi_guard_client_connected();
void        wifi_guard_set_time(int hour, int minute, int second); // Uhrzeit manuell setzen    // aus mod_web aufrufen
void        wifi_guard_client_disconnected();
bool        wifi_guard_client_active();       // gibt true wenn mind. 1 Client da

String      wifi_guard_status_json();
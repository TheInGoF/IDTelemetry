#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
//  mod_config — Laufzeit-Konfiguration via NVS (ESP32 Preferences)
//
//  cfg_init() beim Start aufrufen (vor web_init()).
//  Lädt alle Werte aus NVS; bei leerem NVS greifen die
//  Compile-Zeit-Defaults aus secrets.h.
//
//  Änderungen via /api/config POST → sofort in NVS + RAM.
//  AP-SSID/Passwort + APN: erst nach Neustart aktiv.
// ============================================================

void        cfg_init();

// WiFi Access Point
const char* cfg_ap_ssid();
const char* cfg_ap_pass();

// SIM / APN
const char* cfg_sim_pin();      // SIM PIN (leer = kein PIN)
const char* cfg_apn();
const char* cfg_apn_user();
const char* cfg_apn_pass();

// Traccar OsmAnd
const char* cfg_traccar_host();
const char* cfg_traccar_id();

// InfluxDB v2
const char* cfg_influx_host();
const char* cfg_influx_org();
const char* cfg_influx_bucket();
const char* cfg_influx_token();
const char* cfg_influx_device();

// MQTT Broker
const char* cfg_mqtt_host();
uint16_t    cfg_mqtt_port();
const char* cfg_mqtt_user();
const char* cfg_mqtt_pass();
const char* cfg_mqtt_topic();    // Topic-Prefix (z.B. "tele/id7")

// BLE Standby-Defaults (ABRP bekommt Werte auch ohne CAN)
bool        cfg_ble_standby();

// Sprache ("de", "en")
const char* cfg_lang();

// Modul-Schalter (0 = nicht vorhanden, 1 = vorhanden)
bool        cfg_mod_gps();      // Internes GPS (SIM7080G GNSS)

// Log-Schalter (SPIFFS-Logging ein/aus, zur Laufzeit änderbar)
bool        cfg_log_can();    // CAN/ELM SPIFFS-Log
bool        cfg_log_ble();    // BLE SPIFFS-Log
bool        cfg_log_wifi();   // WiFi Guard Scan-Log

// Web-API: POST-Body (JSON) in NVS schreiben + RAM aktualisieren
bool        cfg_save_json(const uint8_t* body, size_t len);

// Web-API: aktuellen Stand als JSON-String (statischer Puffer)
const char* cfg_to_json();

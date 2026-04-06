#pragma once

#include "mod_telemetry.h"

// ============================================================
//  mod_mqtt — MQTT via SIM7080G nativen AT-Client
//
//  Nutzt AT+SMCONF/SMCONN/SMPUB/SMSUB — kein TinyGSM TCP,
//  kein ArduinoHttpClient, kein TLS-Handshake im ESP32.
//
//  Der SIM7080G hält die MQTT-Verbindung persistent.
//  GPS-Punkte werden sofort gepublisht (kein Batching).
//
//  Alle Funktionen NUR aus dem modem_task() aufrufen
//  (AT-Befehle sind nicht thread-safe).
// ============================================================

// MQTT-Client konfigurieren (Host, Port, Auth aus mod_config).
// Muss nach GPRS-Verbindung aufgerufen werden.
void mqtt_configure();

// Verbindung zum Broker herstellen. Blockiert max ~15s.
bool mqtt_connect();

// Verbindungsstatus prüfen (AT+SMSTATE?)
bool mqtt_is_connected();

// Telemetrie-Zeile als JSON publishen.
// Topic: {prefix}/data   QoS 1
// Gibt true zurück wenn SMPUB mit OK beantwortet wurde (= Broker PUBACK).
bool mqtt_publish_row(const TelemetryRow& row);

// Eingehende Nachrichten verarbeiten (URC +SMSUB parsen).
// Ruft ggf. Callbacks auf. Im modem_task() periodisch aufrufen.
void mqtt_poll();

// Sauber trennen (AT+SMDISC). Vor Deep Sleep aufrufen.
void mqtt_disconnect();

// Status-Getter
bool        mqtt_ok();               // Letzter Publish erfolgreich?
uint32_t    mqtt_last_pub_ms();      // millis() des letzten erfolgreichen Publish
uint16_t    mqtt_pub_count();        // Anzahl erfolgreicher Publishes seit Boot
const char* mqtt_last_ack();         // Letzte Server-Antwort (Subscribe-Topic)

// Debug: Status auf Serial ausgeben
void mqtt_print_info();

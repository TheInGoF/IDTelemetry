#pragma once

// ============================================================
//  mod_modem - SIM7080G Modem (LTE + GPS)
//
//  Verwaltet UART1 (MODEM_TX/RX aus config.h) exklusiv.
//  GPS-Daten → g_gps (shared.h), Inline-API: gps_valid/lat/lon/location_str
//
//  Ablauf im Task (Core 1):
//    alle 60 s  → traccar_on_gps_tick() → Traccar-Versand (wenn GPS-Fix vorhanden)
//    alle  5 s  → AT+CGNSINF            → g_gps Cache aktualisieren
//    alle 30 s  → Signal/SIM            → Modem-Status Cache
// ============================================================

// Initialisiert Modem, SIM, GPS, GPRS.  Blockiert max. ~35 s.
void modem_init();

// Startet den FreeRTOS-Task.  Erst nach modem_init() aufrufen.
void modem_start_task();

// GPRS-Verbindung sicherstellen (reconnect falls nötig).
bool modem_ensure_connected();

// True wenn GPRS-Datenverbindung aktiv.
bool modem_is_connected();

// Modem-Status (SIM7080G) — alle ~30 s aktualisiert
int8_t      modem_signal_quality(); // CSQ 0-31, 99=kein Signal, -1=unbekannt
const char* modem_operator();       // Netzanbieter-Name oder ""
bool        modem_sim_ok();         // SIM vorhanden und bereit

// GPS + GPRS deaktivieren, dann AT+CPOF — vor Deep Sleep aufrufen.
// Nach Wake-up startet modem_init() das Modem automatisch neu (PWRKEY-Puls).
void modem_poweroff();
void modem_pre_sleep_flush();  // Letzter GPS-Punkt (ig=0) + InfluxDB leeren vor Sleep

// GPS Satelliten (SIM7080G liefert keine Aufschlüsselung nach Konstellation)
int  modem_gps_vsat();     // Gesamt sichtbar
int  modem_gps_usat();     // Gesamt verwendet

// Serial-Befehle: Info-Ausgabe auf Terminal
void modem_print_gps_info();   // "gps"       → GPS-Status
void modem_print_lte_info();   // "lte"       → LTE-Status (schnell)
void modem_print_lte_scan();   // "lte scan"  → Netzwerk-Scan (bis 3 Min!)
void modem_print_lte_bands();  // "lte bands"     → Band-Konfiguration anzeigen
void modem_lte_bands_fix(bool all); // "lte bands fix/all" → Baender setzen + Radio-Neustart
void modem_send_at(const char* cmd); // "at ..." → rohes AT-Kommando senden + Antwort anzeigen
void modem_pause_task();             // "at stop"  → Modem-Task pausieren (fuer manuelles AT-Testing)
void modem_resume_task();            // "at start" → Modem-Task fortsetzen

// MQTT-Status
bool modem_mqtt_connected();         // MQTT-Verbindung aktiv?

#pragma once

// ============================================================
//  mod_traccar - Traccar-Telemetrie via OsmAnd HTTPS GET
//
//  Sendet GPS-Position + Akkustand an einen Traccar-Server.
//  Intervall: 60 Sekunden (konfiguriert über TRACCAR_SEND_INTERVAL_MS).
//
//  Wird vom modem_task() (mod_modem) aufgerufen — das Modem
//  bleibt dadurch vollständig serialisiert.
//
//  Konfiguration → secrets.h: SECRET_TRACCAR_HOST, SECRET_TRACCAR_ID
//  GPS-Daten    → g_gps (shared.h)
// ============================================================

// Wird einmal pro GPS-Zyklus vom modem_task() aufgerufen.
// Variante ohne Argument: liest GPS intern (für Kompatibilität).
void traccar_on_gps_tick();

// Variante mit Snapshot: GPS-Position VOR disableGPS() sichern
// und dann im LTE-Fenster übergeben (löst Traccar-nie-sendet-Bug).
struct GpsSnapshot;
void traccar_on_gps_tick(const GpsSnapshot& fix);

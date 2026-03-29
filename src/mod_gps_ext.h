#pragma once

// ============================================================
//  mod_gps_ext — Externes GPS (BLITZ M10 GPS V2)
//
//  UART2: RX=GPIO1 (← GPS TX), TX=GPIO2 (→ GPS RX), 9600 Baud
//  Parst $GNRMC NMEA-Sätze → gps_update() / gps_invalidate()
//
//  Nur aktiv wenn cfg_mod_gps_ext() == true.
// ============================================================

void gps_ext_init();
bool gps_ext_ok();       // true = UART offen + mind. 1 gültiger Fix empfangen
int  gps_ext_sat_count(); // Satelliten aus letztem $GNGGA (oder -1)

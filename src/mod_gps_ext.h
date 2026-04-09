#pragma once

// ============================================================
//  mod_gps_ext — Externes GPS (BLITZ Mini M10)
//
//  I2C / QWIIC: SDA=GPIO45, SCL=GPIO21, Adresse 0x42
//  Geteilt mit RTC (DS1307 0x68) und MPU-6050 (0x69)
//
//  Nur aktiv wenn GPS_EXT_ENABLED == true (config.h).
// ============================================================

void gps_ext_wake();      // UART öffnen + Wake-Byte — so früh wie möglich im Boot aufrufen!
void gps_ext_init();      // Config + Task starten — kann später kommen
void gps_ext_sleep();     // Backup-Mode mit FORCE (µA) — PPS-LED aus, wacht per UART-Byte auf
bool gps_ext_ok();        // true = Modul gefunden + mind. 1 gültiger Fix
int  gps_ext_sat_count();     // Satelliten in use, -1 = noch kein Fix
int  gps_ext_sat_visible();   // Satelliten in view (alle Konstellationen)

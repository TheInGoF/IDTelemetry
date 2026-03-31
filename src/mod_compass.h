#pragma once

// ============================================================
//  mod_compass - QMC5883L Magnetometer (I2C)
//
//  Kalibrierung läuft vollautomatisch:
//   Hard-Iron:  Min/Max aus Fahrkurven → Offsets konvergieren mit der Zeit
//   Fahrtrichtung: EMA aus GPS-Kurs bei Geradeausfahrt (speed > 20 km/h)
//   NVS-Speicherung: automatisch alle 5 min wenn Werte geändert
// ============================================================

void  compass_init();
bool  compass_ok();
float compass_heading_deg();   // 0–360°, kalibriert

// Automatische Kalibrierung — einmal pro Sekunde aus Telemetrie-Task aufrufen
void  compass_auto_cal(float speed_kmh, float gps_course_deg, bool gps_valid);

// Status für WebUI
bool  compass_cal_has_hard_iron();
float compass_cal_drive_offset();
void  compass_cal_reset();     // Kalibrierung löschen (NVS + RAM)

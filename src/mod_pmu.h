#pragma once

// ============================================================
//  mod_pmu - AXP2101 PMU (LILYGO T-SIM7080G-S3)
//
//  I2C-Bus: Wire1  SDA=PMU_SDA_PIN (GPIO15)  SCL=PMU_SCL_PIN (GPIO7)
//  Adresse: 0x34
//
//  Liefert Akkustand 0-100% oder -1 wenn kein Akku eingelegt.
//  BLDO1 wird nicht angefasst (SIM7080G kommuniziert darüber).
// ============================================================

void pmu_init();
void pmu_update();    // frischen I2C-Abruf — im /status-Handler aufrufen
int  pmu_batt_pct();  // -1 = kein Akku / PMU nicht gefunden, 0-100 = %
bool pmu_is_vbus_in();   // true = externe Spannung am USB/VBUS angeschlossen
bool pmu_is_charging();  // true = Akku wird gerade geladen
void pmu_set_charging(bool on);  // Laden ein/ausschalten (fuer Deep Sleep)
void pmu_set_gps_power(bool on); // BLDO2 (GPS-Antenne) ein/ausschalten

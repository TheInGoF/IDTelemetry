#pragma once

// ============================================================
//  mod_sleep - Deep Sleep Management
//
//  Hardware-Voraussetzung:
//   MPU-6050 INT-Pin → GPIO3 (auf T-SIM7080G-S3 Header gelötet)
//   GPIO3 = RTC GPIO, unterstützt EXT1 Wake-up
//
//  Verhalten:
//   - 10 Minuten ohne Gyro-Bewegung → Deep Sleep
//   - Wake-up durch MPU-6050 Motion-Interrupt (INT-Pin HIGH)
//   - Boot/Sleep/Wake-Ereignisse werden in sys.log geloggt
//
//  Aufruf:
//   setup(): sleep_init() nach syslog_init()
//   loop():  sleep_update() (blockiert nicht, prüft nur Timer)
// ============================================================

void sleep_init();       // Wake-Grund ermitteln + in sys.log schreiben
void sleep_log_wake();   // Nach gyro_init(): Wake-Details mit G-Wert loggen
void sleep_update();     // 10-min-Inaktivitätsprüfung → ggf. Deep Sleep
void sleep_force();      // Sofort Deep Sleep (Serial-Befehl / Debug)

// Globales Shutdown-Flag: Tasks prüfen dies in ihrer Loop und beenden sich sauber.
// Verhindert vTaskDelete() während laufender I2C/SPI/UART-Transaktionen.
extern volatile bool g_shutdown;

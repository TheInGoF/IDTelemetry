#pragma once
#include "secrets.h"

// ELM327 Mock-Modus: Antwortet auf alle OBD-Anfragen mit Dummy-Werten
// Auskommentieren fuer normalen Betrieb
// #define ELM_MOCK_MODE

// ============================================================
//  IDTelemetry - Zentrale Konfiguration
//  Hardware: LILYGO T-SIM7080G-S3 (ESP32-S3-WROOM-1-N16R8) + SN65HVD230
// ============================================================

#define FW_VERSION  "1.0.0"

// --- WiFi Access Point ---
#define WIFI_SSID           SECRET_AP_SSID
#define WIFI_PASS           SECRET_AP_PASS

// --- CAN / SN65HVD230 ---
#define CAN_TX_PIN          GPIO_NUM_17  // -> SN65HVD230 CTX  (U1TXD, Kamera-Pin, frei ohne Kamera)
#define CAN_RX_PIN          GPIO_NUM_18  // -> SN65HVD230 CRX  (U1RXD, Kamera-Pin, frei ohne Kamera)
#define CAN_RS_PIN          -1           // -1 = RS am Modul auf GND gebrueckt
#define CAN_SPEED_KBPS      500          // 500kbps = OBD2 Standard

// --- RTC DS1307 + MPU-6050 (I2C, gleicher Bus) ---
// Kamera-Pins, ohne Kamera konfliktfrei. MPU-6050 AD0 an 3.3V (Adresse 0x69)
#define RTC_SDA_PIN         45           // GPIO45 -> SDA
#define RTC_SCL_PIN         21           // GPIO21 -> SCL

// --- AXP2101 PMU (T-SIM7080G-S3, interner I2C-Bus) ---
// GPIO15/16 intern auf PCB verdrahtet (BOARD_I2C_SDA/SCL) — kein externes Lötpad
#define PMU_SDA_PIN         15           // Wire1 → AXP2101 SDA (intern, LilyGo I2C_SDA)
#define PMU_SCL_PIN          7           // Wire1 → AXP2101 SCL (intern, LilyGo I2C_SCL)

// --- Gyro Schwelle ---
// Ab diesem G-Wert (Abweichung von Ruhelage) wird gelb angezeigt
#define GYRO_SHAKE_THRESHOLD 0.06f       // 0.06G = deutliche Bewegung
#define GYRO_HOLD_MS         30000       // SHAKE bleibt 30s aktiv — Autobahn-Vibrationen reichen alle ~15s

// --- Timeouts ---
#define OBD_TIMEOUT_MS      300
#define UDS_TIMEOUT_MS      400
#define SCAN_DELAY_MS       80

// --- Log (RAM) ---
#define MAX_LOG_ENTRIES     200          // RAM Ring-Buffer
#define LOG_INFO_LEN        48           // Max Info-String pro Eintrag

// --- Log-Defaults (via Web-UI / NVS zur Laufzeit änderbar) ---
#define LOG_CAN_ENABLED_DEFAULT   false  // CAN/ELM SPIFFS-Log initial aus
#define LOG_BLE_ENABLED_DEFAULT   false  // BLE SPIFFS-Log initial aus
#define LOG_WIFI_ENABLED_DEFAULT  false  // WiFi Guard Scan-Log initial aus

// --- LTE deaktivieren (GPS-Only Modus) ---
// Einkommentieren wenn keine SIM vorhanden: LTE wird übersprungen,
// GPS startet direkt ohne Netzverbindung. Kein Traccar-Versand.
// #define LTE_DISABLED

// --- SIM7080G Modem / GPS (UART1) ---
// T-SIM7080G-S3: Modem intern verdrahtet, kein externes Lötpad
#define MODEM_TX_PIN        5        // ESP32 TX → SIM7080G RX  (LilyGo: BOARD_MODEM_TXD_PIN)
#define MODEM_RX_PIN        4        // SIM7080G TX → ESP32 RX  (LilyGo: BOARD_MODEM_RXD_PIN)
#define MODEM_PWRKEY_PIN    41       // PWRKEY: HIGH=ein
#define MODEM_DTR_PIN       42
#define MODEM_STATUS_PIN    40
#define MODEM_FLIGHT_PIN    44
#define MODEM_BAUD          115200
#define GPS_INTERVAL_MS     5000     // GPS-Update alle 5 Sekunden

// --- Deep Sleep (Gyro Wake-up) ---
// MPU-6050 INT Pin → GPIO3 (RTC-fähig ESP32-S3 GPIO0-21, EXT1-Wake-up)
// Verdrahtung: MPU-6050 INT-Pin an GPIO3 Lötpad auf T-SIM7080G-S3 Header löten
#define GYRO_WAKE_PIN           GPIO_NUM_3              // RTC GPIO für EXT1 Wake-up
#define SLEEP_INACTIVITY_MS     (10UL * 60UL * 1000UL) // 10 Minuten Inaktivität

// ---- Telemetrie / LTE-Fenster ----
#define SEND_INTERVAL_S          60                          // GPS→LTE Wechselzyklus (einzige Quelle)
#define TRACCAR_SEND_INTERVAL_MS (SEND_INTERVAL_S * 1000UL) // abgeleitet — nicht separat ändern

// ---- Telemetrie Zeilen-Puffer ----
#define TELEM_ROW_BUF_SIZE       500    // max. gespeicherte Zeilen im RAM-Ringpuffer (~83 min bei 6/min)
#define INFLUX_ROWS_PER_SEND      50    // max. Zeilen pro LTE-Fenster (50 × ~220 B ≈ 11 kB Body)
// GPS-basierte Capture-Schwellen (mind. eine muss erfüllt sein):
#define TELEM_GPS_MAX_INTERVAL_MS  30000UL  // max. 30 s ohne neuen Punkt (Zeitlimit)
#define TELEM_GPS_MIN_MOVE_M        15.0f   // Mindestbewegung damit Zeitlimit greift (GPS-Drift-Filter)
#define TELEM_GPS_DIST_LO_M         100.0f   // Distanz-Schwelle bei Speed < SPEED_THRESHOLD
#define TELEM_GPS_DIST_HI_M         200.0f   // Distanz-Schwelle bei Speed >= SPEED_THRESHOLD
#define TELEM_GPS_SPEED_THRESH_KMH  70.0f   // Schwelle Stadt/Autobahn (km/h)
#define TELEM_YAW_TURN_DPS             7    // Drehrate-Schwelle für Kurven-Trigger (°/s, int)
#define TELEM_YAW_INTERVAL_MS      3000UL  // Wiederholrate während Kurve (3s)

// ---- WiFi Guard ----
#define BLE_RSSI_THRESHOLD    -72    // dBm (Legacy, nicht mehr genutzt)
#define GUARD_LOCK_S          120    // 2 min Lock nach Fund
#define GUARD_CHECK_S          60    // 60s bis Rescan
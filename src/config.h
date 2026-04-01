#pragma once
#include "secrets.h"

// ============================================================
//  Telemetry Stick — Zentrale Konfiguration
//  Hardware: LILYGO T-SIM7080G-S3 (ESP32-S3-WROOM-1-N16R8) + SN65HVD230
// ============================================================

#define FW_VERSION  "1.0.5"

// ============================================================
//  MODUL-SCHALTER  ← hier ein-/ausschalten
// ============================================================
#define GPS_EXT_ENABLED     true   // BLITZ Mini M10 GPS  (UART, QWIIC-Connector GPIO43/44)
#define GPS_EXT_RX_PIN       1     // GPIO1  ← GPS TX
#define GPS_EXT_TX_PIN       2     // GPIO2  → GPS RX
#define GPS_EXT_BAUD        115200
                                   // false = nur internes SIM7080G-GPS + LTE

// ELM327 Mock-Modus: Antwortet auf alle OBD-Anfragen mit Dummy-Werten
// #define ELM_MOCK_MODE

// LTE deaktivieren (GPS-Only Modus, keine SIM vorhanden)
// #define LTE_DISABLED

// ============================================================
//  GPS Capture-Schwellen
// ============================================================
#define TELEM_GPS_DIST_M          300.0f  // Distanz-Trigger: neuer Punkt ab X Metern
#define TELEM_GPS_MAX_INTERVAL_MS 60000UL // Zeit-Trigger: max. 1 Minute ohne Punkt
#define TELEM_GPS_MIN_SPEED_KMH    3.0f   // Zeit-Trigger nur ab dieser Geschwindigkeit
#define TELEM_COMPASS_TURN_DEG     10     // Kurven-Trigger (Kompass): Heading-Änderung in Grad
#define TELEM_YAW_TURN_DPS          7     // Kurven-Trigger (Gyro/Yaw): Drehrate in °/s

// ============================================================
//  Telemetrie / Senden
// ============================================================
#define SEND_INTERVAL_S          90                          // GPS→LTE Wechselzyklus
#define TRACCAR_SEND_INTERVAL_MS (SEND_INTERVAL_S * 1000UL) // abgeleitet — nicht ändern
#define EXT_GPS_INFLUX_INTERVAL_MS   30000UL   // InfluxDB-Puffer alle 30s (ext. GPS)
#define TELEM_ROW_BUF_SIZE       500    // max. Zeilen im RAM-Ringpuffer
#define INFLUX_ROWS_PER_SEND      50    // max. Zeilen pro LTE-Fenster
#define TRACCAR_MIN_MOVE_DEG    0.0001  // ~10 m — verhindert Traccar-Ping bei GPS-Drift

// ============================================================
//  Deep Sleep / Gyro
// ============================================================
#define GYRO_WAKE_PIN           GPIO_NUM_3              // MPU-6050 INT → GPIO3 (EXT1 Wake-up)
#define SLEEP_INACTIVITY_MS     (10UL * 60UL * 1000UL) // 10 Minuten Inaktivität
#define GYRO_SHAKE_THRESHOLD    0.05f   // 0.05G = deutliche Bewegung → gelb
#define GYRO_HOLD_MS            30000   // SHAKE bleibt 30s aktiv

// ============================================================
//  WiFi / Access Point / Guard
// ============================================================
#define AP_TIMEOUT_MIN         90    // AP abschalten nach X Minuten ohne Client (0 = nie)
#define BLE_RSSI_THRESHOLD    -72    // dBm (Legacy, nicht mehr genutzt)
#define GUARD_LOCK_S          120    // 2 min Lock nach Fund
#define GUARD_CHECK_S          60    // 60s bis Rescan

// ============================================================
//  Log
// ============================================================
#define MAX_LOG_ENTRIES     200   // RAM Ring-Buffer
#define LOG_INFO_LEN         48   // Max Info-String pro Eintrag
#define LOG_CAN_ENABLED_DEFAULT   false
#define LOG_BLE_ENABLED_DEFAULT   false
#define LOG_WIFI_ENABLED_DEFAULT  false

// ============================================================
//  OBD Timeouts
// ============================================================
#define OBD_TIMEOUT_MS      300
#define UDS_TIMEOUT_MS      400
#define SCAN_DELAY_MS        80

// ============================================================
//  Hardware-Pins (fest verdrahtet, normalerweise nicht ändern)
// ============================================================

// WiFi Access Point
#define WIFI_SSID           SECRET_AP_SSID
#define WIFI_PASS           SECRET_AP_PASS

// CAN / SN65HVD230
#define CAN_TX_PIN          GPIO_NUM_17
#define CAN_RX_PIN          GPIO_NUM_18
#define CAN_RS_PIN          -1           // -1 = RS am Modul auf GND gebrückt
#define CAN_SPEED_KBPS      500

// I2C Bus (extern): RTC DS1307 (0x68) + MPU-6050 (0x69) + QMC5883L (0x0D) + BLITZ M10 (0x42)
#define RTC_SDA_PIN         45
#define RTC_SCL_PIN         21

// I2C Bus (intern): AXP2101 PMU (auf PCB verdrahtet, kein Lötpad)
#define PMU_SDA_PIN         15
#define PMU_SCL_PIN          7
#define PMU_INT_PIN     GPIO_NUM_6

// SIM7080G Modem / internes GPS (intern verdrahtet)
#define MODEM_TX_PIN        5
#define MODEM_RX_PIN        4
#define MODEM_PWRKEY_PIN    41
#define MODEM_DTR_PIN       42
#define MODEM_STATUS_PIN    40
#define MODEM_FLIGHT_PIN    44   // SIM7080G FLIGHT/RFKILL — LOW = RF aktiv (intern verdrahtet)
#define MODEM_BAUD          115200
#define GPS_INTERVAL_MS     5000

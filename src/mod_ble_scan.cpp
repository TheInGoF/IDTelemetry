#include "mod_ble_scan.h"
#include "mod_elm327.h"
#include <NimBLEDevice.h>

// ============================================================
//  mod_ble_scan - BLE Server (NimBLE Init + Server-Instanz)
//  Guard-Funktion via WiFi Guard (mod_wifi_guard)
// ============================================================

static NimBLEServer* pServer = nullptr;

void ble_scan_init() {
    NimBLEDevice::init(ELM327_BLE_NAME);  // "OBDII" — ABRP/Torque suchen danach
    NimBLEDevice::setMTU(185);            // Größere Pakete erlauben (statt 23 default)
    pServer = NimBLEDevice::createServer();
    Serial.println("[BLE] NimBLE bereit");
}

NimBLEServer* ble_get_server() { return pServer; }

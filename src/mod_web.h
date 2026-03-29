#pragma once
void ws_broadcast_json(const char* json);
#include "shared.h"

// ============================================================
//  mod_web - WiFi AP + AsyncWebServer + WebSocket
// ============================================================

void web_init();
void web_ap_stop();   // AP + WebServer abschalten (Power-Saving nach Timeout)
bool web_ap_active(); // true solange AP noch läuft

void ble_web_routes_init();
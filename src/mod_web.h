#pragma once
void ws_broadcast_json(const char* json);
#include "shared.h"

// ============================================================
//  mod_web - WiFi AP + AsyncWebServer + WebSocket
// ============================================================

void web_init();

void ble_web_routes_init();
#include "config.h"
#include "mod_web.h"
#include "mod_can.h"
#include "mod_logs.h"
#include "mod_wifi_guard.h"
#include "mod_rtc.h"
#include "mod_gyro.h"
#include "mod_pmu.h"
#include "mod_modem.h"
#include "mod_headers.h"
#include "mod_telemetry.h"
#include "mod_config.h"
#include "mod_compass.h"
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Update.h>

// SPIFFS-Datei senden + alle Status-Header
static void send_html(AsyncWebServerRequest* r, const char* path) {
    auto* resp = r->beginResponse(SPIFFS, path, "text/html");
    headers_apply(resp);
    r->send(resp);
}

static void on_ws_event(AsyncWebSocket*, AsyncWebSocketClient* c,
                        AwsEventType t, void*, uint8_t*, size_t) {
    if (t == WS_EVT_CONNECT) {
        ws.cleanupClients(1);  // alte Verbindung sofort schließen
        Serial.printf("[WS] Client #%u verbunden\n", c->id());
        { char m[40]; snprintf(m, sizeof(m), "WebSocket #%u verbunden", c->id()); syslog("CLIENT", m); }
        wifi_guard_client_connected();
        JsonDocument doc;
        doc["type"]   = "status";
        doc["uptime"] = millis();
        doc["can_ok"] = can_running;
        String out; serializeJson(doc, out);
        c->text(out);
    } else if (t == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] Client #%u getrennt\n", c->id());
        { char m[40]; snprintf(m, sizeof(m), "WebSocket #%u getrennt", c->id()); syslog("CLIENT", m); }
        wifi_guard_client_disconnected();
    }
}

void ws_broadcast_json(const char* json) { if (ws.count() > 0) ws.textAll(json); }

void web_init() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[WEB] SPIFFS Fehler!");
        return;
    }
    Serial.println("[WEB] SPIFFS OK");

    WiFi.softAP(cfg_ap_ssid(), cfg_ap_pass());
    Serial.printf("[WiFi] SSID: %s  PW: %s\n", cfg_ap_ssid(), cfg_ap_pass());
    Serial.println("[WiFi] URL:  http://192.168.4.1");

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->redirect("/daten");
    });

    // ── Captive-Portal-Fake: iOS/Android denkt der AP hat Internet ──
    // iOS testet /hotspot-detect.html, Android /generate_204
    // Exakte Antwort nötig damit iOS "Internet OK" markiert und verbunden bleibt
    auto captive = [](AsyncWebServerRequest* r) {
        r->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    };
    server.on("/hotspot-detect.html",          HTTP_GET, captive);  // iOS
    server.on("/library/test/success.html",    HTTP_GET, captive);  // iOS alt
    server.on("/success.txt",                  HTTP_GET, captive);  // iOS neu
    server.on("/generate_204",                 HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send(204);  // Android/Chrome erwartet leere 204-Antwort
    });
    server.on("/ncsi.txt",                     HTTP_GET, captive);  // Windows

    server.on("/send", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            deserializeJson(doc, data, len);
            String id_s  = doc["id"]       | "7DF";
            String hex_s = doc["hex"]      | "02 01 00 00 00 00 00 00";
            String rx_s  = doc["rxFilter"] | "0";
            uint32_t tx_id   = strtoul(id_s.c_str(), nullptr, 16);
            uint32_t rx_filt = strtoul(rx_s.c_str(), nullptr, 16);
            uint8_t  bytes[8] = {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC};
            uint8_t  cnt = 0;
            char buf[64]; strncpy(buf, hex_s.c_str(), 63);
            char* tok = strtok(buf, " ");
            while (tok && cnt < 8) { bytes[cnt++] = strtoul(tok, nullptr, 16); tok = strtok(nullptr, " "); }
            char label[LOG_INFO_LEN]; snprintf(label, LOG_INFO_LEN, "→ Manual 0x%03X", tx_id);
            if (can_lock()) {
                can_tx(tx_id, bytes, cnt, label);
                uint8_t frames[8][8]; uint8_t lens[8]; uint32_t ids[8];
                can_rx_collect(rx_filt, UDS_TIMEOUT_MS, frames, lens, ids, 8);
                can_unlock();
            }
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/scan", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            if (scan_running) { r->send(200, "application/json", "{\"err\":\"laeuft\"}"); return; }
            JsonDocument doc; deserializeJson(doc, data, len);
            int      mode    = doc["mode"]   | 1;
            uint32_t req_id  = strtoul((doc["reqId"]  | "7E6"), nullptr, 16);
            uint32_t resp_id = strtoul((doc["respId"] | "7EE"), nullptr, 16);
            const char* name = doc["name"]   | "Modul";
            can_start_scan(mode, req_id, resp_id, name);
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/scan-abort", HTTP_POST, [](AsyncWebServerRequest* r) {
        scan_abort = true;
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/monitor", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc; deserializeJson(doc, data, len);
            monitor_mode = doc["enable"] | false;
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/restart-can", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc; deserializeJson(doc, data, len);
            uint32_t br = doc["bitrate"] | CAN_SPEED_KBPS;
            can_stop();
            vTaskDelay(pdMS_TO_TICKS(300));
            bool ok = can_init(br);
            r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        }
    );

    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* r) {
        r->send(200, "application/json", "{\"ok\":true}");
        delay(500);
        esp_restart();
    });

    server.on("/clear-log", HTTP_POST, [](AsyncWebServerRequest* r) {
        log_clear();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/download-log", HTTP_GET, [](AsyncWebServerRequest* r) {
        String txt = log_to_txt();
        auto* resp = r->beginResponse(200, "text/plain; charset=utf-8", txt);
        resp->addHeader("Content-Disposition", "attachment; filename=\"vw_can_log.txt\"");
        r->send(resp);
    });

    // Status — inkl. RTC + Gyro + PMU Akku + GPS
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["fw_version"]  = FW_VERSION;
        doc["uptime"]      = millis();
        doc["can_ok"]      = can_hw_ok();
        doc["log_cnt"]     = log_count;
        doc["scan"]        = scan_running;
        doc["can_tx_ok"]   = guard_can_tx_allowed();
        doc["rtc_time"]    = rtc_now_str();
        doc["rtc_ok"]      = rtc_is_running();
        { int y,mo,d,h,mi,s;
          if (rtc_get_datetime(y,mo,d,h,mi,s) && y >= 2024) {
            char dbuf[12]; snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", y, mo, d);
            doc["rtc_date"] = dbuf;
          }
        }
        doc["gyro_state"]     = (int)gyro_get_state();  // 0=err 1=still 2=shake
        doc["gyro_ok"]        = gyro_ok();
        doc["gyro_g"]         = gyro_get_accel_g();
        doc["gyro_threshold"] = gyro_get_threshold();
        doc["gyro_mot_thr"]   = gyro_get_mot_threshold();
        doc["batt_pct"]    = pmu_batt_pct();          // -1=kein Akku, 0-100=%
        doc["vbus_in"]     = pmu_is_vbus_in();        // externe Spannung?
        doc["charging"]    = pmu_is_charging();        // Akku wird geladen?
        doc["gps_valid"]   = gps_valid();
        doc["gps_lat"]     = serialized(String(gps_lat(), 6));
        doc["gps_lon"]     = serialized(String(gps_lon(), 6));
        doc["gps_loc"]     = gps_location_str();
        doc["gps_sat"]     = modem_gps_usat();
        doc["gps_vsat"]    = modem_gps_vsat();
        doc["compass_ok"]      = compass_ok();
        doc["compass_heading"] = compass_ok() ? compass_heading_deg() : 0.0f;
        doc["modem_ok"]    = modem_is_connected();
        doc["modem_sig"]   = (int)modem_signal_quality();
        doc["modem_op"]    = modem_operator();
        doc["modem_sim"]   = modem_sim_ok();
        String out; serializeJson(doc, out);
        auto* resp = r->beginResponse(200, "application/json", out);
        headers_apply(resp);
        r->send(resp);
    });

    // Gyro Schwelle setzen
    server.on("/gyro/set-threshold", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400, "application/json", "{\"ok\":false}"); return; }
            float thr = doc["threshold"] | -1.0f;
            if (thr >= 0.005f && thr <= 1.0f) {
                gyro_set_threshold(thr);
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(400, "application/json", "{\"ok\":false,\"err\":\"0.005–1.0\"}");
            }
        }
    );

    // Gyro Hardware-Aufwachschwelle setzen (MOT_THR, 1 Einheit = 32mg)
    server.on("/gyro/set-mot-threshold", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400, "application/json", "{\"ok\":false}"); return; }
            int v = doc["value"] | -1;
            if (v >= 1 && v <= 255) {
                gyro_set_mot_threshold((uint8_t)v);
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(400, "application/json", "{\"ok\":false,\"err\":\"1-255\"}");
            }
        }
    );

    // Gyro Baseline-Kalibrierung (Board muss ruhig stehen, ~3s Messung)
    server.on("/api/gyro/calibrate", HTTP_POST, [](AsyncWebServerRequest* r) {
        float baseline = 1.0f, stddev = 0.0f;
        bool ok = gyro_recalibrate(&baseline, &stddev);
        JsonDocument doc;
        doc["ok"]       = ok;
        doc["baseline"] = serialized(String(baseline, 4));
        doc["stddev"]   = serialized(String(stddev, 4));
        String out; serializeJson(doc, out);
        r->send(200, "application/json", out);
    });

    // GPS — Standort als JSON
    server.on("/gps", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["valid"] = gps_valid();
        doc["lat"]   = serialized(String(gps_lat(), 6));
        doc["lon"]   = serialized(String(gps_lon(), 6));
        doc["loc"]   = gps_location_str();
        String out; serializeJson(doc, out);
        auto* resp = r->beginResponse(200, "application/json", out);
        headers_apply(resp);
        r->send(resp);
    });

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);
    server.begin();
    Serial.println("[WEB] Server bereit: http://192.168.4.1");
}

static bool s_ap_active = true;

bool web_ap_active() { return s_ap_active; }

void web_ap_stop() {
    if (!s_ap_active) return;
    s_ap_active = false;
    ws.closeAll();
    server.end();
    WiFi.softAPdisconnect(true);
    Serial.println("[WEB] AP + WebServer abgeschaltet (Timeout)");
    syslog("WEB", "AP abgeschaltet — Timeout");
}

void ble_web_routes_init() {
    server.on("/wifi/status", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send(200, "application/json", wifi_guard_status_json());
    });

    server.on("/wifi/set-ssid", HTTP_POST,
        [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400, "application/json", "{\"ok\":false}"); return; }
            const char* ssid = doc["ssid"] | "";
            int thresh       = doc["rssi"] | WIFI_RSSI_THRESHOLD_DEF;
            if (strlen(ssid) == 0) { r->send(400, "application/json", "{\"ok\":false}"); return; }
            wifi_guard_set_ssid(ssid, thresh);
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/wifi/clear-ssid", HTTP_POST, [](AsyncWebServerRequest* r) {
        wifi_guard_clear_ssid();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/wifi/tx-unlock", HTTP_POST, [](AsyncWebServerRequest* r) {
        wifi_guard_manual_tx_unlock();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/wifi/set-mode", HTTP_POST,
        [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400, "application/json", "{\"ok\":false}"); return; }
            wifi_guard_set_mode(doc["mode"] | 0);
            r->send(200, "application/json", "{\"ok\":true}");
        }
    );

    server.on("/debug",  HTTP_GET, [](AsyncWebServerRequest* r) { send_html(r, "/debug.html");  });
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest* r) { send_html(r, "/config.html"); });
    server.on("/daten",  HTTP_GET, [](AsyncWebServerRequest* r) { send_html(r, "/daten.html");  });

    // i18n — Sprachdateien
    server.on("/i18n.js", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send(SPIFFS, "/i18n.js", "application/javascript");
    });
    server.serveStatic("/lang/", SPIFFS, "/lang/");

    // /api/telemetry — alle Telemetrie-Felder mit Wert + Alter
    server.on("/api/telemetry", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        doc["ts_ms"]        = (uint32_t)millis();
        doc["influx_ok"]     = telem_influx_ok();
        doc["lte_connected"] = modem_is_connected();
        doc["buf_pending"]   = telem_get_buf_pending();
        doc["row_pending"]   = telem_get_row_pending();

        JsonObject fields = doc["fields"].to<JsonObject>();

        // Hilfslambda: float-Feld eintragen
        auto addField = [&](const char* name, TelemField f) {
            float v = 0.0f; uint32_t age = 0;
            bool valid = telem_get_latest(f, &v, &age);
            JsonObject o = fields[name].to<JsonObject>();
            o["v"]      = serialized(String(v, 6));
            o["age_ms"] = age;
            o["valid"]  = valid;
        };

        addField("soc",           TELEM_SOC);
        addField("voltage",       TELEM_VOLTAGE);
        addField("current",       TELEM_CURRENT);
        addField("power",         TELEM_POWER);
        addField("vehicle_speed", TELEM_VEHICLE_SPEED);
        addField("is_charging",   TELEM_IS_CHARGING);
        addField("is_dcfc",       TELEM_IS_DCFC);
        addField("batt_temp",     TELEM_BATT_TEMP);
        addField("ext_temp",      TELEM_EXT_TEMP);
        addField("gps_lat",       TELEM_GPS_LAT);
        addField("gps_lon",       TELEM_GPS_LON);
        addField("gps_valid",     TELEM_GPS_VALID);
        addField("gyro_g",        TELEM_GYRO_G);
        addField("lte_signal",    TELEM_LTE_SIGNAL);
        addField("capacity",      TELEM_CAPACITY);
        addField("kwh_charged",   TELEM_KWH_CHARGED);
        addField("is_parked",     TELEM_IS_PARKED);
        addField("odometer",      TELEM_ODOMETER);
        addField("range",         TELEM_RANGE);
        addField("batt_device",   TELEM_BATT_DEVICE);

        // lte_operator: string-Feld
        {
            float v = 0.0f; uint32_t age = 0;
            bool valid = telem_get_latest(TELEM_LTE_OPERATOR, &v, &age);
            JsonObject o = fields["lte_operator"].to<JsonObject>();
            o["v"]      = 0;
            o["age_ms"] = age;
            o["valid"]  = valid;
            o["str"]    = telem_get_operator_str();
        }

        String out; serializeJson(doc, out);
        auto* resp = r->beginResponse(200, "application/json", out);
        headers_apply(resp);
        r->send(resp);
    });

    // ISO-TP CAN PID Abfrage — POST {txid, rxid, data} → {ok, data: "hex bytes"}
    server.on("/api/canpid", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* body, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, body, len)) {
                r->send(400, "application/json", "{\"ok\":false}"); return;
            }
            uint32_t tx_id = strtoul(doc["txid"] | "0", nullptr, 16);
            uint32_t rx_id = strtoul(doc["rxid"] | "0", nullptr, 16);
            const char* data_str = doc["data"] | "";

            uint8_t req[8] = {}; uint8_t req_len = 0;
            char tmp[64]; strncpy(tmp, data_str, 63); tmp[63] = 0;
            char* tok = strtok(tmp, " ");
            while (tok && req_len < 8) {
                req[req_len++] = (uint8_t)strtoul(tok, nullptr, 16);
                tok = strtok(nullptr, " ");
            }

            if (!tx_id || !req_len) {
                r->send(400, "application/json", "{\"ok\":false}"); return;
            }

            uint8_t resp[64];
            int n = can_isotp_query(tx_id, rx_id, req, req_len, resp, sizeof(resp), 400);
            if (n <= 0) {
                r->send(200, "application/json", "{\"ok\":false}"); return;
            }

            // Hex-String aufbauen
            char hex[200] = "";
            for (int i = 0; i < n; i++) {
                char h[4];
                snprintf(h, sizeof(h), i == 0 ? "%02x" : " %02x", resp[i]);
                strncat(hex, h, sizeof(hex) - strlen(hex) - 1);
            }

            JsonDocument out_doc;
            out_doc["ok"]   = true;
            out_doc["data"] = hex;
            String out; serializeJson(out_doc, out);
            r->send(200, "application/json", out);
        }
    );


    server.on("/wifi/set-time", HTTP_POST,
        [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) { r->send(400); return; }
            int h = doc["hour"] | -1, m = doc["minute"] | 0, s = doc["second"] | 0;
            int yr = doc["year"] | 0, mo = doc["month"] | 0, dy = doc["day"] | 0;
            if (h >= 0 && h < 24 && m >= 0 && m < 60) {
                wifi_guard_set_time(h, m, s);
                if (yr >= 2024 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {
                    rtc_set_datetime(yr, mo, dy, h, m, s);
                    char msg[48];
                    snprintf(msg, sizeof(msg), "Datum+Zeit gesetzt: %04d-%02d-%02d %02d:%02d:%02d", yr, mo, dy, h, m, s);
                    syslog("RTC", msg);
                } else {
                    rtc_set_time(h, m, s);
                }
                r->send(200, "application/json", "{\"ok\":true}");
            } else {
                r->send(400, "application/json", "{\"ok\":false}");
            }
        }
    );

    server.on("/wifi/scan-start", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (WiFi.softAPgetStationNum() > 0) {
            r->send(200, "application/json", "{\"ok\":false,\"err\":\"client_connected\"}"); return;
        }
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true);
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/wifi/scan-result", HTTP_GET, [](AsyncWebServerRequest* r) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) { r->send(200, "application/json", "{\"status\":\"running\"}"); return; }
        if (n < 0)                  { r->send(200, "application/json", "{\"status\":\"failed\"}");  return; }
        JsonDocument doc;
        doc["status"] = "done";
        doc["count"]  = n;
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            int rssi = WiFi.RSSI(i);
            if (rssi < -70) continue;
            JsonObject net = arr.add<JsonObject>();
            net["ssid"]     = WiFi.SSID(i);
            net["rssi"]     = rssi;
            net["channel"]  = WiFi.channel(i);
            net["dist"]     = rssi > -55 ? "<5m" : rssi > -65 ? "~10m" : rssi > -75 ? "~20m" : ">30m";
            net["is_guard"] = (String(wifi_guard_get_ssid()) == WiFi.SSID(i));
        }
        String out; serializeJson(doc, out);
        r->send(200, "application/json", out);
    });

    server.on("/download-wifi-log", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (SPIFFS.exists("/wifi.log")) r->send(SPIFFS, "/wifi.log", "text/plain");
        else r->send(200, "text/plain", "# WiFi Log leer\n");
    });
    server.on("/clear-wifi-log", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (SPIFFS.exists("/wifi.log")) SPIFFS.remove("/wifi.log");
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/download-sys-log", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (SPIFFS.exists("/sys.log")) r->send(SPIFFS, "/sys.log", "text/plain");
        else r->send(200, "text/plain", "# Sys Log leer");
    });
    server.on("/clear-sys-log", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (SPIFFS.exists("/sys.log")) SPIFFS.remove("/sys.log");
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/download-elm-log", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (SPIFFS.exists("/elm.log")) r->send(SPIFFS, "/elm.log", "text/plain");
        else r->send(200, "text/plain", "# ELM Log leer\n");
    });
    server.on("/clear-elm-log", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (SPIFFS.exists("/elm.log")) SPIFFS.remove("/elm.log");
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // ── InfluxDB Preview (Debug) ────────────────────────────────
    server.on("/api/influx-preview", HTTP_GET, [](AsyncWebServerRequest* r) {
        auto* resp = r->beginResponse(200, "text/plain", telem_preview_rows(10));
        headers_apply(resp);
        r->send(resp);
    });

    // ── Laufzeit-Config (NVS) ──────────────────────────────────
    server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
        auto* resp = r->beginResponse(200, "application/json", cfg_to_json());
        headers_apply(resp);
        r->send(resp);
    });

    server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* r) {},
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
            bool ok = cfg_save_json(data, len);
            r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        }
    );

    // ── OTA Firmware-Upload ────────────────────────────────────
    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* r) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp = r->beginResponse(
                200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
            resp->addHeader("Connection", "close");
            r->send(resp);
            if (ok) {
                syslog("OTA", "Firmware-Update OK \xe2\x80\x93 Neustart\xe2\x80\xa6");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        },
        [](AsyncWebServerRequest* r, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!index) {
                Serial.printf("[OTA] Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                    Serial.println("[OTA] begin() Fehler");
            }
            if (Update.write(data, len) != len)
                Serial.println("[OTA] write() Fehler");
            if (final) {
                if (Update.end(true))
                    Serial.printf("[OTA] OK: %u Bytes\n", index + len);
                else
                    Serial.println("[OTA] end() Fehler");
            }
        }
    );
}
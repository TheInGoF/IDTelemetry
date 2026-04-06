// TINY_GSM_MODEM_SIM7080 muss VOR dem ersten TinyGSM-Include definiert sein.
#ifndef TINY_GSM_MODEM_SIM7080
#define TINY_GSM_MODEM_SIM7080
#endif

#include <TinyGsmClient.h>

#include "mod_mqtt.h"
#include "mod_config.h"
#include "mod_pmu.h"
#include "mod_logs.h"
#include "mod_rtc.h"
#include "shared.h"
#include "config.h"
#include <Arduino.h>

// ---- Extern: TinyGsm + Serial aus mod_modem ----
extern TinyGsm&        modem_get();
extern HardwareSerial&  modem_serial();

// ---- Zustand ----
static bool     s_configured   = false;
static bool     s_connected    = false;
static bool     s_last_pub_ok  = false;
static uint32_t s_last_pub_ms  = 0;
static uint16_t s_pub_count    = 0;
static char     s_last_ack[128] = "";
static char     s_sub_topic[80] = "";   // Subscribe-Topic für Server-ACK

// ---- AT-Hilfe (wie in mod_modem) ----
static bool mqtt_at_ok(const char* cmd, long timeout_ms = 5000L) {
    TinyGsm& m = modem_get();
    m.sendAT(cmd);
    String resp = "";
    int rc = m.waitResponse(timeout_ms, resp);
    if (rc != 1) {
        char msg[128];
        resp.trim();
        snprintf(msg, sizeof(msg), "AT%s → ERROR (%s)", cmd, resp.c_str());
        syslog("MQTT", msg);
    }
    return rc == 1;
}

// ---- Konfiguration ----
void mqtt_configure() {
    TinyGsm& m = modem_get();
    char cmd[160];

    // Alte Verbindung trennen (falls noch offen)
    m.sendAT("+SMDISC");
    m.waitResponse(3000L);

    snprintf(cmd, sizeof(cmd), "+SMCONF=\"URL\",\"%s\",%d",
             cfg_mqtt_host(), cfg_mqtt_port());
    mqtt_at_ok(cmd);

    // Client-ID = MQTT-Username (eindeutig pro Gerät)
    snprintf(cmd, sizeof(cmd), "+SMCONF=\"CLIENTID\",\"%s\"",
             cfg_mqtt_user());
    mqtt_at_ok(cmd);

    if (cfg_mqtt_user()[0]) {
        snprintf(cmd, sizeof(cmd), "+SMCONF=\"USERNAME\",\"%s\"",
                 cfg_mqtt_user());
        mqtt_at_ok(cmd);
        snprintf(cmd, sizeof(cmd), "+SMCONF=\"PASSWORD\",\"%s\"",
                 cfg_mqtt_pass());
        mqtt_at_ok(cmd);
    }

    snprintf(cmd, sizeof(cmd), "+SMCONF=\"KEEPTIME\",%d", MQTT_KEEPALIVE_S);
    mqtt_at_ok(cmd);

    mqtt_at_ok("+SMCONF=\"CLEANSS\",1");   // Clean Session
    mqtt_at_ok("+SMCONF=\"QOS\",1");        // Default QoS 1

    // TLS nur bei Port 8883
    if (cfg_mqtt_port() == 8883) {
        mqtt_at_ok("+CSSLCFG=\"sslversion\",0,3");  // TLS 1.2
        mqtt_at_ok("+SMSSL=1,\"\"");                  // SSL on, kein Client-Cert
    } else {
        mqtt_at_ok("+SMSSL=0,\"\"");                  // SSL off
    }

    // Subscribe-Topic vorbereiten
    snprintf(s_sub_topic, sizeof(s_sub_topic), "%s/ack", cfg_mqtt_topic());

    s_configured = true;

    char msg[128];
    snprintf(msg, sizeof(msg), "Konfiguriert: %s:%d topic=%s",
             cfg_mqtt_host(), cfg_mqtt_port(), cfg_mqtt_topic());
    syslog("MQTT", msg);
}

// ---- Verbinden ----
bool mqtt_connect() {
    if (!s_configured) mqtt_configure();

    TinyGsm& m = modem_get();

    syslog("MQTT", "Verbinde...");
    m.sendAT("+SMCONN");
    String resp = "";
    int rc = m.waitResponse(15000L, resp);
    if (rc != 1) {
        resp.trim();
        char msg[128];
        snprintf(msg, sizeof(msg), "SMCONN fehlgeschlagen: %s", resp.c_str());
        syslog("MQTT", msg);
        s_connected = false;
        return false;
    }

    s_connected = true;
    syslog("MQTT", "Verbunden");

    // ACK-Topic abonnieren
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "+SMSUB=\"%s\",%d", s_sub_topic, MQTT_QOS);
    if (mqtt_at_ok(cmd, 5000L)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Subscribed: %s", s_sub_topic);
        syslog("MQTT", msg);
    }

    return true;
}

// ---- Verbindungsstatus ----
bool mqtt_is_connected() {
    if (!s_configured) return false;

    TinyGsm& m = modem_get();
    m.sendAT("+SMSTATE?");
    String resp = "";
    int rc = m.waitResponse(3000L, resp);
    if (rc != 1) {
        s_connected = false;
        return false;
    }
    // Antwort: +SMSTATE: 0 oder +SMSTATE: 1
    s_connected = (resp.indexOf("1") >= 0);
    return s_connected;
}

// ---- JSON-Payload für eine Telemetrie-Zeile bauen ----
// Kompaktes Format, gleiche Keys wie InfluxDB Line Protocol.
static int build_json(const TelemetryRow& row, char* buf, int buf_size) {
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "{\"ts\":%lu", (unsigned long)row.unix_s);

    // GPS (immer senden wenn valid)
    if (row.valid[TELEM_GPS_LAT])
        pos += snprintf(buf + pos, buf_size - pos, ",\"la\":%.6f", (double)row.values[TELEM_GPS_LAT]);
    if (row.valid[TELEM_GPS_LON])
        pos += snprintf(buf + pos, buf_size - pos, ",\"lo\":%.6f", (double)row.values[TELEM_GPS_LON]);
    if (row.valid[TELEM_GPS_HEADING])
        pos += snprintf(buf + pos, buf_size - pos, ",\"hd\":%d", (int)row.values[TELEM_GPS_HEADING]);

    // Fahrzeugdaten (nur wenn gültig und geändert)
    if (row.valid[TELEM_SOC])
        pos += snprintf(buf + pos, buf_size - pos, ",\"s\":%.1f", row.values[TELEM_SOC]);
    if (row.valid[TELEM_VOLTAGE])
        pos += snprintf(buf + pos, buf_size - pos, ",\"u\":%d", (int)row.values[TELEM_VOLTAGE]);
    if (row.valid[TELEM_CURRENT])
        pos += snprintf(buf + pos, buf_size - pos, ",\"i\":%d", (int)row.values[TELEM_CURRENT]);
    if (row.valid[TELEM_POWER])
        pos += snprintf(buf + pos, buf_size - pos, ",\"p\":%.1f", row.values[TELEM_POWER]);
    if (row.valid[TELEM_VEHICLE_SPEED])
        pos += snprintf(buf + pos, buf_size - pos, ",\"v\":%.1f", row.values[TELEM_VEHICLE_SPEED]);
    if (row.valid[TELEM_IS_CHARGING])
        pos += snprintf(buf + pos, buf_size - pos, ",\"c\":%d", (int)(row.values[TELEM_IS_CHARGING] > 0.5f));
    if (row.valid[TELEM_IS_DCFC])
        pos += snprintf(buf + pos, buf_size - pos, ",\"dc\":%d", (int)(row.values[TELEM_IS_DCFC] > 0.5f));
    if (row.valid[TELEM_BATT_TEMP])
        pos += snprintf(buf + pos, buf_size - pos, ",\"bt\":%d", (int)row.values[TELEM_BATT_TEMP]);
    if (row.valid[TELEM_EXT_TEMP])
        pos += snprintf(buf + pos, buf_size - pos, ",\"et\":%d", (int)row.values[TELEM_EXT_TEMP]);
    if (row.valid[TELEM_RANGE])
        pos += snprintf(buf + pos, buf_size - pos, ",\"r\":%.1f", row.values[TELEM_RANGE]);
    if (row.valid[TELEM_CAPACITY])
        pos += snprintf(buf + pos, buf_size - pos, ",\"ca\":%.1f", row.values[TELEM_CAPACITY]);
    if (row.valid[TELEM_KWH_CHARGED])
        pos += snprintf(buf + pos, buf_size - pos, ",\"kw\":%.1f", row.values[TELEM_KWH_CHARGED]);
    if (row.valid[TELEM_IS_PARKED])
        pos += snprintf(buf + pos, buf_size - pos, ",\"pk\":%d", (int)(row.values[TELEM_IS_PARKED] > 0.5f));
    if (row.valid[TELEM_ODOMETER])
        pos += snprintf(buf + pos, buf_size - pos, ",\"od\":%.1f", row.values[TELEM_ODOMETER]);
    if (row.valid[TELEM_LTE_SIGNAL])
        pos += snprintf(buf + pos, buf_size - pos, ",\"ls\":%d", (int)row.values[TELEM_LTE_SIGNAL]);

    // Akku-Stand (ESP32/PMU)
    int batt = pmu_batt_pct();
    if (batt >= 0)
        pos += snprintf(buf + pos, buf_size - pos, ",\"bd\":%d", batt);

    pos += snprintf(buf + pos, buf_size - pos, "}");

    return pos;
}

// ---- Publish einer Zeile ----
bool mqtt_publish_row(const TelemetryRow& row) {
    if (!s_connected) return false;

    TinyGsm& m = modem_get();
    HardwareSerial& ser = modem_serial();

    // JSON aufbauen
    static char payload[512];
    int plen = build_json(row, payload, sizeof(payload));
    if (plen <= 0 || plen >= (int)sizeof(payload)) return false;

    // Topic
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/data", cfg_mqtt_topic());

    // AT+SMPUB="topic",len,qos,retain
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "+SMPUB=\"%s\",%d,%d,0", topic, plen, MQTT_QOS);
    m.sendAT(cmd);

    // Warte auf ">" Prompt
    String resp = "";
    int rc = m.waitResponse(5000L, resp, ">", "ERROR");
    if (rc != 1) {
        syslog("MQTT", "SMPUB: kein > Prompt");
        s_connected = false;
        s_last_pub_ok = false;
        return false;
    }

    // Payload senden
    ser.write((const uint8_t*)payload, plen);

    // Warte auf OK (Broker PUBACK bei QoS 1)
    resp = "";
    rc = m.waitResponse(MQTT_PUBLISH_TIMEOUT_MS, resp);
    if (rc != 1) {
        resp.trim();
        char msg[128];
        snprintf(msg, sizeof(msg), "Publish fehlgeschlagen: %s", resp.c_str());
        syslog("MQTT", msg);
        s_connected = false;
        s_last_pub_ok = false;
        return false;
    }

    s_last_pub_ok = true;
    s_last_pub_ms = millis();
    s_pub_count++;

    Serial.printf("[MQTT] → %s (%d Bytes) OK #%u\n", topic, plen, s_pub_count);

    return true;
}

// ---- Eingehende Nachrichten verarbeiten ----
void mqtt_poll() {
    // URCs wie +SMSUB: "topic",len,"payload" werden vom TinyGSM-Parser
    // in den Stream-Buffer geschrieben. Wir lesen verfügbare Bytes.
    HardwareSerial& ser = modem_serial();
    static char urc_buf[256];
    static int  urc_pos = 0;

    while (ser.available()) {
        char c = ser.read();
        if (c == '\n') {
            urc_buf[urc_pos] = '\0';
            // +SMSUB: "topic",len,"payload"
            if (strncmp(urc_buf, "+SMSUB:", 7) == 0) {
                // Payload extrahieren (nach letztem Anführungszeichen)
                char* last_q = strrchr(urc_buf, '"');
                char* first_payload_q = nullptr;
                if (last_q) {
                    // Rückwärts zum öffnenden " des Payloads
                    *last_q = '\0';
                    first_payload_q = strrchr(urc_buf, '"');
                    if (first_payload_q) {
                        first_payload_q++;  // nach dem "
                        strncpy(s_last_ack, first_payload_q, sizeof(s_last_ack) - 1);
                        s_last_ack[sizeof(s_last_ack) - 1] = '\0';
                        Serial.printf("[MQTT] ← ACK: %s\n", s_last_ack);
                        syslog("MQTT", s_last_ack);
                    }
                }
            }
            urc_pos = 0;
        } else if (urc_pos < (int)sizeof(urc_buf) - 1) {
            urc_buf[urc_pos++] = c;
        }
        // Nicht zu lange blockieren — max 10 Bytes pro Aufruf
        // (Rest wird beim nächsten poll() gelesen)
    }
}

// ---- Disconnect ----
void mqtt_disconnect() {
    if (!s_connected) return;
    mqtt_at_ok("+SMDISC", 5000L);
    s_connected = false;
    syslog("MQTT", "Getrennt");
}

// ---- Getter ----
bool        mqtt_ok()           { return s_last_pub_ok; }
uint32_t    mqtt_last_pub_ms()  { return s_last_pub_ms; }
uint16_t    mqtt_pub_count()    { return s_pub_count; }
const char* mqtt_last_ack()     { return s_last_ack; }

// ---- Debug-Info ----
void mqtt_print_info() {
    Serial.printf("[MQTT] Host: %s:%d\n", cfg_mqtt_host(), cfg_mqtt_port());
    Serial.printf("[MQTT] Topic: %s\n", cfg_mqtt_topic());
    Serial.printf("[MQTT] Connected: %s\n", s_connected ? "ja" : "nein");
    Serial.printf("[MQTT] Publishes: %u\n", s_pub_count);
    Serial.printf("[MQTT] Last OK: %s\n", s_last_pub_ok ? "ja" : "nein");
    if (s_last_ack[0])
        Serial.printf("[MQTT] Last ACK: %s\n", s_last_ack);
    if (s_last_pub_ms > 0)
        Serial.printf("[MQTT] Seit letztem Publish: %lus\n",
                       (unsigned long)(millis() - s_last_pub_ms) / 1000UL);
}

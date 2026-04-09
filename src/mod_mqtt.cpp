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
#include "secrets.h"
#include <Arduino.h>
#include <esp_system.h>
#include <aes/esp_aes.h>

// ---- Extern: TinyGsm + Serial aus mod_modem ----
extern TinyGsm&        modem_get();
extern HardwareSerial&  modem_serial();

// ---- AES-256 Pre-Shared Key (Hex-String → 32 Bytes) ----
static uint8_t s_aes_key[32];
static bool    s_aes_ready = false;

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void init_aes_key() {
    const char* hex = SECRET_AES_KEY;
    for (int i = 0; i < 32; i++) {
        s_aes_key[i] = (hex_nibble(hex[i*2]) << 4) | hex_nibble(hex[i*2+1]);
    }
    s_aes_ready = true;
}

// Binäres Protokoll: Feld-Bit-Definitionen
enum BinField : uint32_t {
    BF_LAT       = (1 << 0),
    BF_LON       = (1 << 1),
    BF_HEADING   = (1 << 2),
    BF_SOC       = (1 << 3),
    BF_VOLTAGE   = (1 << 4),
    BF_CURRENT   = (1 << 5),
    BF_POWER     = (1 << 6),
    BF_SPEED     = (1 << 7),
    BF_CHARGING  = (1 << 8),   // Bool — kein Payload
    BF_DCFC      = (1 << 9),   // Bool — kein Payload
    BF_BATT_TEMP = (1 << 10),
    BF_EXT_TEMP  = (1 << 11),
    BF_RANGE     = (1 << 12),
    BF_CAPACITY  = (1 << 13),
    BF_KWH       = (1 << 14),
    BF_PARKED    = (1 << 15),  // Bool — kein Payload
    BF_ODOMETER  = (1 << 16),
    BF_LTE_SIG   = (1 << 17),
    BF_BATT_DEV  = (1 << 18),
};

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
    if (!s_aes_ready) init_aes_key();

    TinyGsm& m = modem_get();
    char cmd[160];

    // Alte Verbindung trennen (falls noch offen)
    m.sendAT("+SMDISC");
    m.waitResponse(3000L);

    // URL inkl. Port (SIM7080G: AT+SMCONF="URL","host","port")
    snprintf(cmd, sizeof(cmd), "+SMCONF=\"URL\",\"%s\",\"%d\"",
             cfg_mqtt_host(), cfg_mqtt_port());
    mqtt_at_ok(cmd);

    // Client-ID aus Topic-Prefix (z.B. "tele/vw_nox" → "vw_nox")
    {
        const char* topic = cfg_mqtt_topic();
        const char* slash = strrchr(topic, '/');
        const char* cid = slash ? slash + 1 : topic;
        snprintf(cmd, sizeof(cmd), "+SMCONF=\"CLIENTID\",\"%s\"", cid);
        mqtt_at_ok(cmd);
    }

    // Kein Username/Password — Auth über AES-Key im Payload

    snprintf(cmd, sizeof(cmd), "+SMCONF=\"KEEPTIME\",%d", MQTT_KEEPALIVE_S);
    mqtt_at_ok(cmd);

    mqtt_at_ok("+SMCONF=\"CLEANSS\",1");   // Clean Session
    mqtt_at_ok("+SMCONF=\"QOS\",1");        // Default QoS 1

    // Plain MQTT — kein TLS (SIM7080G FW R1951.07 hat kein MQTT-TLS).
    // Auth über Username/Password. Broker muss plain Listener haben.

    // Subscribe-Topic vorbereiten
    snprintf(s_sub_topic, sizeof(s_sub_topic), "%s/ack", cfg_mqtt_topic());

    s_configured = true;

    char msg[128];
    snprintf(msg, sizeof(msg), "Konfiguriert: %s:%d topic=%s",
             cfg_mqtt_host(), cfg_mqtt_port(), cfg_mqtt_topic());
    syslog("MQTT", msg);
}

// ---- Verbinden ----
// SMCONN mit eigenem harten Timeout — TinyGSM waitResponse kann hängen
// wenn das Modem kontinuierlich URCs sendet (innere Leseschleife blockiert
// die Timeout-Prüfung in der äußeren do-while-Schleife).
static bool mqtt_smconn(uint32_t hard_timeout_ms = 20000UL) {
    HardwareSerial& ser = modem_serial();

    // Serial-Puffer leeren (alte URCs entsorgen)
    while (ser.available()) ser.read();

    ser.print("AT+SMCONN\r\n");

    uint32_t start = millis();
    String   buf;
    buf.reserve(128);

    while (millis() - start < hard_timeout_ms) {
        while (ser.available()) {
            char c = (char)ser.read();
            if (c == '\n') {
                buf.trim();
                if (buf == "OK")    return true;
                if (buf == "ERROR") return false;
                buf = "";
            } else {
                buf += c;
                // Sicherheit: Puffer begrenzen (URCs können lang sein)
                if (buf.length() > 200) buf = buf.substring(buf.length() - 80);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;  // Hard-Timeout
}

bool mqtt_connect() {
    if (!s_configured) mqtt_configure();

    syslog("MQTT", "Verbinde...");
    bool ok = mqtt_smconn(20000UL);
    if (!ok) {
        syslog("MQTT", "Verbindung fehlgeschlagen");
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

// ---- Little-Endian Schreibhilfen ----
static inline void put_u8 (uint8_t* p, uint8_t  v) { p[0]=v; }
static inline void put_i8 (uint8_t* p, int8_t   v) { p[0]=(uint8_t)v; }
static inline void put_u16(uint8_t* p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static inline void put_i16(uint8_t* p, int16_t  v) { put_u16(p,(uint16_t)v); }
static inline void put_u32(uint8_t* p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static inline void put_i32(uint8_t* p, int32_t  v) { put_u32(p,(uint32_t)v); }

// ---- Binäre Payload bauen (Klartext, vor Verschlüsselung) ----
// Gibt Länge der Plaintext-Daten zurück (field_mask + ts + Felder).
static int build_binary(const TelemetryRow& row, uint8_t* buf, int buf_size) {
    if (buf_size < 8) return 0;

    uint32_t mask = 0;
    int pos = 8;   // Platz für mask (4B) + ts (4B)

    // GPS
    if (row.valid[TELEM_GPS_LAT] && pos + 4 <= buf_size) {
        mask |= BF_LAT;
        put_i32(buf + pos, (int32_t)(row.values[TELEM_GPS_LAT] * 1e6f));
        pos += 4;
    }
    if (row.valid[TELEM_GPS_LON] && pos + 4 <= buf_size) {
        mask |= BF_LON;
        put_i32(buf + pos, (int32_t)(row.values[TELEM_GPS_LON] * 1e6f));
        pos += 4;
    }
    if (row.valid[TELEM_GPS_HEADING] && pos + 2 <= buf_size) {
        mask |= BF_HEADING;
        put_u16(buf + pos, (uint16_t)(int)row.values[TELEM_GPS_HEADING]);
        pos += 2;
    }

    // Fahrzeugdaten
    if (row.valid[TELEM_SOC] && pos + 2 <= buf_size) {
        mask |= BF_SOC;
        put_u16(buf + pos, (uint16_t)(row.values[TELEM_SOC] * 10.0f));
        pos += 2;
    }
    if (row.valid[TELEM_VOLTAGE] && pos + 2 <= buf_size) {
        mask |= BF_VOLTAGE;
        put_u16(buf + pos, (uint16_t)row.values[TELEM_VOLTAGE]);
        pos += 2;
    }
    if (row.valid[TELEM_CURRENT] && pos + 2 <= buf_size) {
        mask |= BF_CURRENT;
        put_i16(buf + pos, (int16_t)row.values[TELEM_CURRENT]);
        pos += 2;
    }
    if (row.valid[TELEM_POWER] && pos + 2 <= buf_size) {
        mask |= BF_POWER;
        put_i16(buf + pos, (int16_t)(row.values[TELEM_POWER] * 10.0f));
        pos += 2;
    }
    if (row.valid[TELEM_VEHICLE_SPEED] && pos + 2 <= buf_size) {
        mask |= BF_SPEED;
        put_u16(buf + pos, (uint16_t)(row.values[TELEM_VEHICLE_SPEED] * 10.0f));
        pos += 2;
    }

    // Booleans (kein Payload — nur Bit in mask)
    if (row.valid[TELEM_IS_CHARGING] && row.values[TELEM_IS_CHARGING] > 0.5f)
        mask |= BF_CHARGING;
    if (row.valid[TELEM_IS_DCFC] && row.values[TELEM_IS_DCFC] > 0.5f)
        mask |= BF_DCFC;
    if (row.valid[TELEM_IS_PARKED] && row.values[TELEM_IS_PARKED] > 0.5f)
        mask |= BF_PARKED;

    // Temperaturen
    if (row.valid[TELEM_BATT_TEMP] && pos + 1 <= buf_size) {
        mask |= BF_BATT_TEMP;
        put_i8(buf + pos, (int8_t)row.values[TELEM_BATT_TEMP]);
        pos += 1;
    }
    if (row.valid[TELEM_EXT_TEMP] && pos + 1 <= buf_size) {
        mask |= BF_EXT_TEMP;
        put_i8(buf + pos, (int8_t)row.values[TELEM_EXT_TEMP]);
        pos += 1;
    }

    // Reichweite / Kapazität / Geladen
    if (row.valid[TELEM_RANGE] && pos + 2 <= buf_size) {
        mask |= BF_RANGE;
        put_u16(buf + pos, (uint16_t)(row.values[TELEM_RANGE] * 10.0f));
        pos += 2;
    }
    if (row.valid[TELEM_CAPACITY] && pos + 2 <= buf_size) {
        mask |= BF_CAPACITY;
        put_u16(buf + pos, (uint16_t)(row.values[TELEM_CAPACITY] * 10.0f));
        pos += 2;
    }
    if (row.valid[TELEM_KWH_CHARGED] && pos + 2 <= buf_size) {
        mask |= BF_KWH;
        put_u16(buf + pos, (uint16_t)(row.values[TELEM_KWH_CHARGED] * 10.0f));
        pos += 2;
    }

    // Odometer
    if (row.valid[TELEM_ODOMETER] && pos + 4 <= buf_size) {
        mask |= BF_ODOMETER;
        put_u32(buf + pos, (uint32_t)(row.values[TELEM_ODOMETER] * 10.0f));
        pos += 4;
    }

    // LTE Signal
    if (row.valid[TELEM_LTE_SIGNAL] && pos + 1 <= buf_size) {
        mask |= BF_LTE_SIG;
        put_u8(buf + pos, (uint8_t)row.values[TELEM_LTE_SIGNAL]);
        pos += 1;
    }

    // ESP-Akku
    int batt = pmu_batt_pct();
    if (batt >= 0 && pos + 1 <= buf_size) {
        mask |= BF_BATT_DEV;
        put_u8(buf + pos, (uint8_t)batt);
        pos += 1;
    }

    // Header schreiben (jetzt wo mask komplett ist)
    put_u32(buf + 0, mask);
    put_u32(buf + 4, row.unix_s);

    return pos;
}

// ---- AES-256-CBC verschlüsseln ----
// Erzeugt: [0x01][IV 16B][AES-CBC Ciphertext mit PKCS7 Padding]
// Gibt Gesamtlänge zurück, oder 0 bei Fehler.
static int encrypt_payload(const uint8_t* plain, int plain_len,
                           uint8_t* out, int out_size) {
    // PKCS7 Padding berechnen
    int pad = 16 - (plain_len % 16);
    int padded_len = plain_len + pad;

    // Brauchen: 1 (Version) + 16 (IV) + padded_len
    int total = 1 + 16 + padded_len;
    if (total > out_size) return 0;

    // Version
    out[0] = 0x01;

    // IV aus Hardware-RNG
    uint8_t iv[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        memcpy(iv + i * 4, &r, 4);
    }
    memcpy(out + 1, iv, 16);

    // Plaintext + PKCS7 Padding in temporären Buffer
    uint8_t padded[128];   // Max: 45 Felder + 8 Header + 16 Pad = ~70 Bytes
    if (padded_len > (int)sizeof(padded)) return 0;
    memcpy(padded, plain, plain_len);
    memset(padded + plain_len, pad, pad);

    // AES-256-CBC verschlüsseln (ESP32-S3 Hardware-Beschleuniger)
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, s_aes_key, 256);
    // esp_aes_crypt_cbc modifiziert IV in-place → Kopie übergeben
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    int ret = esp_aes_crypt_cbc(&ctx, ESP_AES_ENCRYPT, padded_len,
                                 iv_copy, padded, out + 17);
    esp_aes_free(&ctx);

    if (ret != 0) return 0;
    return total;
}

// ---- Publish einer Zeile ----
bool mqtt_publish_row(const TelemetryRow& row) {
    if (!s_connected) return false;

    TinyGsm& m = modem_get();
    HardwareSerial& ser = modem_serial();

    // Binär packen → AES verschlüsseln
    static uint8_t plain[128];
    int plain_len = build_binary(row, plain, sizeof(plain));
    if (plain_len <= 0) return false;

    static uint8_t payload[192];   // 1 + 16 + max 128 padded
    int plen = encrypt_payload(plain, plain_len, payload, sizeof(payload));
    if (plen <= 0) return false;

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
    ser.write(payload, plen);

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

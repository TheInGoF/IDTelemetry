#include "mod_config.h"
#include "mod_wifi_guard.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Arduino.h>

#define CFG_NS "cfg"

// ── Laufzeit-Speicher (vorinitialisiert mit secrets.h Werten) ──
static char s_ap_ssid   [64]  = SECRET_AP_SSID;
static char s_ap_pass   [64]  = SECRET_AP_PASS;
static char s_sim_pin   [12]  = SECRET_PIN;
static char s_apn       [64]  = SECRET_APN;
static char s_apn_user  [32]  = SECRET_APN_USER;
static char s_apn_pass  [32]  = SECRET_APN_PASS;
static char s_tc_host   [96]  = SECRET_TRACCAR_HOST;
static char s_tc_id     [64]  = SECRET_TRACCAR_ID;
static char s_ix_host   [96]  = SECRET_INFLUX_HOST;
static char s_ix_org    [64]  = SECRET_INFLUX_ORG;
static char s_ix_bucket [64]  = SECRET_INFLUX_BUCKET;
static char s_ix_token  [256] = SECRET_INFLUX_TOKEN;
static char s_ix_device [32]  = SECRET_INFLUX_DEVICE;
static bool s_ble_standby     = false;  // Default: aus
static bool s_log_can         = LOG_CAN_ENABLED_DEFAULT;
static bool s_log_ble         = LOG_BLE_ENABLED_DEFAULT;
static bool s_log_wifi        = LOG_WIFI_ENABLED_DEFAULT;
static char s_lang[8]         = "de";

static void pref_load(Preferences& p, const char* key,
                      char* dst, size_t sz, const char* def) {
    String v = p.getString(key, def);
    strncpy(dst, v.c_str(), sz - 1);
    dst[sz - 1] = '\0';
}

void cfg_init() {
    Preferences p;
    p.begin(CFG_NS, true);   // read-only
    pref_load(p, "ap_ssid",   s_ap_ssid,   sizeof(s_ap_ssid),   SECRET_AP_SSID);
    pref_load(p, "ap_pass",   s_ap_pass,   sizeof(s_ap_pass),   SECRET_AP_PASS);
    pref_load(p, "sim_pin",   s_sim_pin,   sizeof(s_sim_pin),   SECRET_PIN);
    pref_load(p, "apn",       s_apn,       sizeof(s_apn),       SECRET_APN);
    pref_load(p, "apn_user",  s_apn_user,  sizeof(s_apn_user),  SECRET_APN_USER);
    pref_load(p, "apn_pass",  s_apn_pass,  sizeof(s_apn_pass),  SECRET_APN_PASS);
    pref_load(p, "tc_host",   s_tc_host,   sizeof(s_tc_host),   SECRET_TRACCAR_HOST);
    pref_load(p, "tc_id",     s_tc_id,     sizeof(s_tc_id),     SECRET_TRACCAR_ID);
    pref_load(p, "ix_host",   s_ix_host,   sizeof(s_ix_host),   SECRET_INFLUX_HOST);
    pref_load(p, "ix_org",    s_ix_org,    sizeof(s_ix_org),    SECRET_INFLUX_ORG);
    pref_load(p, "ix_bucket", s_ix_bucket, sizeof(s_ix_bucket), SECRET_INFLUX_BUCKET);
    pref_load(p, "ix_token",  s_ix_token,  sizeof(s_ix_token),  SECRET_INFLUX_TOKEN);
    pref_load(p, "ix_device", s_ix_device, sizeof(s_ix_device), SECRET_INFLUX_DEVICE);
    s_ble_standby = p.getBool("ble_stdby", false);
    s_log_can     = p.getBool("log_can", LOG_CAN_ENABLED_DEFAULT);
    s_log_ble     = p.getBool("log_ble", LOG_BLE_ENABLED_DEFAULT);
    s_log_wifi    = p.getBool("log_wifi", LOG_WIFI_ENABLED_DEFAULT);
    pref_load(p, "lang", s_lang, sizeof(s_lang), "de");
    p.end();
    Serial.println("[CFG] init OK");
    Serial.printf("[CFG] Traccar: https://%s  ID=%s\n", s_tc_host, s_tc_id);
    Serial.printf("[CFG] InfluxDB: https://%s  org=%s  bucket=%s\n", s_ix_host, s_ix_org, s_ix_bucket);
}

const char* cfg_ap_ssid()       { return s_ap_ssid; }
const char* cfg_ap_pass()       { return s_ap_pass; }
const char* cfg_sim_pin()       { return s_sim_pin; }
const char* cfg_apn()           { return s_apn; }
const char* cfg_apn_user()      { return s_apn_user; }
const char* cfg_apn_pass()      { return s_apn_pass; }
const char* cfg_traccar_host()  { return s_tc_host; }
const char* cfg_traccar_id()    { return s_tc_id; }
const char* cfg_influx_host()   { return s_ix_host; }
const char* cfg_influx_org()    { return s_ix_org; }
const char* cfg_influx_bucket() { return s_ix_bucket; }
const char* cfg_influx_token()  { return s_ix_token; }
const char* cfg_influx_device() { return s_ix_device; }
bool        cfg_ble_standby()  { return s_ble_standby; }
bool        cfg_log_can()      { return s_log_can; }
bool        cfg_log_ble()      { return s_log_ble; }
bool        cfg_log_wifi()     { return s_log_wifi; }
const char* cfg_lang()         { return s_lang; }

bool cfg_save_json(const uint8_t* body, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, body, len)) return false;

    Preferences p;
    p.begin(CFG_NS, false);   // read-write

    auto save = [&](const char* key, char* dst, size_t sz, JsonVariant v) {
        if (v.isNull()) return;
        const char* s = v.as<const char*>();
        if (!s) return;
        strncpy(dst, s, sz - 1);
        dst[sz - 1] = '\0';
        p.putString(key, dst);
    };

    save("ap_ssid",   s_ap_ssid,   sizeof(s_ap_ssid),   doc["ap_ssid"]);
    save("ap_pass",   s_ap_pass,   sizeof(s_ap_pass),   doc["ap_pass"]);
    save("sim_pin",   s_sim_pin,   sizeof(s_sim_pin),   doc["sim_pin"]);
    save("apn",       s_apn,       sizeof(s_apn),       doc["apn"]);
    save("apn_user",  s_apn_user,  sizeof(s_apn_user),  doc["apn_user"]);
    save("apn_pass",  s_apn_pass,  sizeof(s_apn_pass),  doc["apn_pass"]);
    save("tc_host",   s_tc_host,   sizeof(s_tc_host),   doc["tc_host"]);
    save("tc_id",     s_tc_id,     sizeof(s_tc_id),     doc["tc_id"]);
    save("ix_host",   s_ix_host,   sizeof(s_ix_host),   doc["ix_host"]);
    save("ix_org",    s_ix_org,    sizeof(s_ix_org),    doc["ix_org"]);
    save("ix_bucket", s_ix_bucket, sizeof(s_ix_bucket), doc["ix_bucket"]);
    save("ix_token",  s_ix_token,  sizeof(s_ix_token),  doc["ix_token"]);
    save("ix_device", s_ix_device, sizeof(s_ix_device), doc["ix_device"]);
    save("lang",      s_lang,      sizeof(s_lang),      doc["lang"]);
    if (!doc["ble_standby"].isNull()) {
        s_ble_standby = doc["ble_standby"].as<bool>();
        p.putBool("ble_stdby", s_ble_standby);
    }
    if (!doc["log_can"].isNull()) {
        s_log_can = doc["log_can"].as<bool>();
        p.putBool("log_can", s_log_can);
    }
    if (!doc["log_ble"].isNull()) {
        s_log_ble = doc["log_ble"].as<bool>();
        p.putBool("log_ble", s_log_ble);
    }
    if (!doc["log_wifi"].isNull()) {
        s_log_wifi = doc["log_wifi"].as<bool>();
        p.putBool("log_wifi", s_log_wifi);
    }
    p.end();
    if (!doc["guard_mode"].isNull()) {
        wifi_guard_set_mode(doc["guard_mode"].as<uint8_t>());
    }

    Serial.println("[CFG] gespeichert");
    return true;
}

const char* cfg_to_json() {
    static char buf[900];
    JsonDocument doc;
    doc["ap_ssid"]   = s_ap_ssid;
    doc["ap_pass"]   = s_ap_pass;
    doc["sim_pin"]   = s_sim_pin;
    doc["apn"]       = s_apn;
    doc["apn_user"]  = s_apn_user;
    doc["apn_pass"]  = s_apn_pass;
    doc["tc_host"]   = s_tc_host;
    doc["tc_id"]     = s_tc_id;
    doc["ix_host"]   = s_ix_host;
    doc["ix_org"]    = s_ix_org;
    doc["ix_bucket"] = s_ix_bucket;
    doc["ix_token"]  = s_ix_token;
    doc["ix_device"]    = s_ix_device;
    doc["ble_standby"]  = s_ble_standby;
    doc["log_can"]      = s_log_can;
    doc["log_ble"]      = s_log_ble;
    doc["log_wifi"]     = s_log_wifi;
    doc["lang"]         = s_lang;
    doc["guard_mode"]   = wifi_guard_get_mode();
    serializeJson(doc, buf, sizeof(buf));
    return buf;
}

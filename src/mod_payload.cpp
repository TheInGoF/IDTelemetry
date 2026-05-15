#include "mod_payload.h"
#include "mod_logs.h"
#include <stdio.h>
#include "mod_telemetry.h"
#include "mod_config.h"
#include "mod_pmu.h"
#include "secrets.h"
#include <aes/esp_aes.h>
#include <esp_random.h>
#include <string.h>

// BF_* enum lives in mod_payload.h (shared with mod_mqtt for debug logging).

static uint8_t s_aes_key[32];
static bool    s_aes_ready = false;

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void payload_init_key() {
    const char* override_key = cfg_aes_key();
    const char* hex = (override_key && strlen(override_key) >= 64) ? override_key : SECRET_AES_KEY;
    for (int i = 0; i < 32; i++) {
        s_aes_key[i] = (hex_nibble(hex[i*2]) << 4) | hex_nibble(hex[i*2+1]);
    }
    s_aes_ready = true;
}

static inline void put_u8 (uint8_t* p, uint8_t  v) { p[0]=v; }
static inline void put_i8 (uint8_t* p, int8_t   v) { p[0]=(uint8_t)v; }
static inline void put_u16(uint8_t* p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static inline void put_i16(uint8_t* p, int16_t  v) { put_u16(p,(uint16_t)v); }
static inline void put_u32(uint8_t* p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static inline void put_i32(uint8_t* p, int32_t  v) { put_u32(p,(uint32_t)v); }

// Build the plaintext payload (mask + ts + fields) into buf.
static int build_plaintext(const TelemetryRow& row, uint8_t* buf, int buf_size) {
    if (buf_size < 8) return 0;
    uint32_t mask = 0;
    int pos = 8;

    if (row.valid[TELEM_GPS_LAT] && pos + 4 <= buf_size) {
        mask |= BF_LAT; put_i32(buf + pos, (int32_t)(row.values[TELEM_GPS_LAT] * 1e6f)); pos += 4;
    }
    if (row.valid[TELEM_GPS_LON] && pos + 4 <= buf_size) {
        mask |= BF_LON; put_i32(buf + pos, (int32_t)(row.values[TELEM_GPS_LON] * 1e6f)); pos += 4;
    }
    if (row.valid[TELEM_GPS_HEADING] && pos + 2 <= buf_size) {
        mask |= BF_HEADING; put_u16(buf + pos, (uint16_t)(int)row.values[TELEM_GPS_HEADING]); pos += 2;
    }
    if (row.valid[TELEM_SOC] && pos + 2 <= buf_size) {
        mask |= BF_SOC; put_u16(buf + pos, (uint16_t)(row.values[TELEM_SOC] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_VOLTAGE] && pos + 2 <= buf_size) {
        mask |= BF_VOLTAGE; put_u16(buf + pos, (uint16_t)row.values[TELEM_VOLTAGE]); pos += 2;
    }
    if (row.valid[TELEM_CURRENT] && pos + 2 <= buf_size) {
        mask |= BF_CURRENT; put_i16(buf + pos, (int16_t)row.values[TELEM_CURRENT]); pos += 2;
    }
    if (row.valid[TELEM_POWER] && pos + 2 <= buf_size) {
        mask |= BF_POWER; put_i16(buf + pos, (int16_t)(row.values[TELEM_POWER] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_VEHICLE_SPEED] && pos + 2 <= buf_size) {
        mask |= BF_SPEED; put_u16(buf + pos, (uint16_t)(row.values[TELEM_VEHICLE_SPEED] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_IS_CHARGING] && row.values[TELEM_IS_CHARGING] > 0.5f) mask |= BF_CHARGING;
    if (row.valid[TELEM_IS_DCFC]     && row.values[TELEM_IS_DCFC]     > 0.5f) mask |= BF_DCFC;
    if (row.valid[TELEM_IS_PARKED]   && row.values[TELEM_IS_PARKED]   > 0.5f) mask |= BF_PARKED;
    if (row.valid[TELEM_BATT_TEMP] && pos + 1 <= buf_size) {
        mask |= BF_BATT_TEMP; put_i8(buf + pos, (int8_t)row.values[TELEM_BATT_TEMP]); pos += 1;
    }
    if (row.valid[TELEM_EXT_TEMP] && pos + 1 <= buf_size) {
        mask |= BF_EXT_TEMP; put_i8(buf + pos, (int8_t)row.values[TELEM_EXT_TEMP]); pos += 1;
    }
    if (row.valid[TELEM_RANGE] && pos + 2 <= buf_size) {
        mask |= BF_RANGE; put_u16(buf + pos, (uint16_t)(row.values[TELEM_RANGE] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_CAPACITY] && pos + 2 <= buf_size) {
        mask |= BF_CAPACITY; put_u16(buf + pos, (uint16_t)(row.values[TELEM_CAPACITY] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_KWH_CHARGED] && pos + 2 <= buf_size) {
        mask |= BF_KWH; put_u16(buf + pos, (uint16_t)(row.values[TELEM_KWH_CHARGED] * 10.0f)); pos += 2;
    }
    if (row.valid[TELEM_ODOMETER] && pos + 4 <= buf_size) {
        mask |= BF_ODOMETER; put_u32(buf + pos, (uint32_t)(row.values[TELEM_ODOMETER] * 10.0f)); pos += 4;
    }
    if (row.valid[TELEM_LTE_SIGNAL] && pos + 1 <= buf_size) {
        mask |= BF_LTE_SIG; put_u8(buf + pos, (uint8_t)row.values[TELEM_LTE_SIGNAL]); pos += 1;
    }
    int batt = pmu_batt_pct();
    if (batt >= 0 && pos + 1 <= buf_size) {
        mask |= BF_BATT_DEV; put_u8(buf + pos, (uint8_t)batt); pos += 1;
    }
    if (row.valid[TELEM_LTE_OPERATOR] && pos + 2 <= buf_size) {
        uint32_t plmn = (uint32_t)row.values[TELEM_LTE_OPERATOR];
        if (plmn > 0 && plmn < 65536) {
            mask |= BF_LTE_PLMN; put_u16(buf + pos, (uint16_t)plmn); pos += 2;
        }
    }
    put_u32(buf + 0, mask);
    put_u32(buf + 4, row.unix_s);
    return pos;
}

int payload_encode(const TelemetryRow& row, uint8_t* out, int out_size) {
    if (!s_aes_ready) payload_init_key();

    // Debug: log the raw cache values for the fields the server reports as
    // out of range. One line per encoded row — small enough not to spam.
    {
        char dm[160];
        snprintf(dm, sizeof(dm),
            "ODO=%.1f(%d) RANGE=%.1f(%d) KWH=%.1f(%d) PLMN=%u(%d) LTE_SIG=%.0f(%d)",
            row.values[TELEM_ODOMETER],    row.valid[TELEM_ODOMETER] ? 1 : 0,
            row.values[TELEM_RANGE],       row.valid[TELEM_RANGE] ? 1 : 0,
            row.values[TELEM_KWH_CHARGED], row.valid[TELEM_KWH_CHARGED] ? 1 : 0,
            (uint32_t)row.values[TELEM_LTE_OPERATOR],
                                           row.valid[TELEM_LTE_OPERATOR] ? 1 : 0,
            row.values[TELEM_LTE_SIGNAL],  row.valid[TELEM_LTE_SIGNAL] ? 1 : 0);
        syslog("PAYLOAD", dm);
    }

    uint8_t plain[96];
    int plain_len = build_plaintext(row, plain, sizeof(plain));
    if (plain_len == 0) return 0;

    int pad = 16 - (plain_len % 16);
    int padded_len = plain_len + pad;
    int total = 1 + 16 + padded_len;
    if (total > out_size) return 0;

    out[0] = 0x01;
    uint8_t iv[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        memcpy(iv + i * 4, &r, 4);
    }
    memcpy(out + 1, iv, 16);

    uint8_t padded[128];
    if (padded_len > (int)sizeof(padded)) return 0;
    memcpy(padded, plain, plain_len);
    memset(padded + plain_len, pad, pad);

    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, s_aes_key, 256);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    int ret = esp_aes_crypt_cbc(&ctx, ESP_AES_ENCRYPT, padded_len,
                                 iv_copy, padded, out + 17);
    esp_aes_free(&ctx);
    return (ret == 0) ? total : 0;
}

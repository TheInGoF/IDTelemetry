#pragma once
#include <stdint.h>
#include "config.h"

// ============================================================
//  mod_wifi_upload — WiFi STA upload of buffered telemetry rows
//
//  Used by both s3_full (as a secondary path when at home / shop)
//  and s3_lite (as the only upload path).
//
//  Reads rows from mod_telem_store and POSTs them to a configured
//  HTTP(S) endpoint when WiFi STA is up.
//
//  Status: scaffolding only — the upload coroutine returns
//  immediately; full implementation follows once the receiving
//  side (IDMate server) has a matching endpoint.
// ============================================================

#if FEATURE_WIFI_UPLOAD

// Configure STA credentials (typically loaded from NVS / config).
// Empty SSID disables the uploader.
void wifi_upload_configure(const char* sta_ssid, const char* sta_pass,
                           const char* post_url);

// Start the upload task (FreeRTOS, Core 0, low priority).
void wifi_upload_start_task();

// Is the STA currently associated?
bool wifi_upload_is_connected();

// How many rows we have drained successfully since boot.
uint32_t wifi_upload_count();

#else

static inline void wifi_upload_configure(const char*, const char*, const char*) {}
static inline void wifi_upload_start_task()        {}
static inline bool wifi_upload_is_connected()      { return false; }
static inline uint32_t wifi_upload_count()         { return 0; }

#endif

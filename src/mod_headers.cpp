#include "mod_headers.h"
#include "mod_pmu.h"
#include "mod_gyro.h"
#include "mod_rtc.h"
#include "mod_can.h"  // can_hw_ok()
#include <Arduino.h>

void headers_apply(AsyncWebServerResponse* resp) {
    if (!resp) return;  // Response kann NULL sein bei RAM-Mangel oder abgebrochenem Client
    char buf[24];

    // GPS-Standort
    resp->addHeader("X-GPS-Location",
                    gps_valid() ? gps_location_str() : "no-fix");

    // Akku (PMU frisch auslesen)
    pmu_update();
    snprintf(buf, sizeof(buf), "%d", pmu_batt_pct());
    resp->addHeader("X-Battery", buf);

    // RTC-Zeit
    resp->addHeader("X-RTC-Time", rtc_now_str());

    // CAN-Status (echter TWAI-Hardware-Zustand, nicht nur Treiber-Flag)
    resp->addHeader("X-CAN-Ok", can_hw_ok() ? "1" : "0");

    // Uptime in Sekunden
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000UL);
    resp->addHeader("X-Uptime", buf);
}

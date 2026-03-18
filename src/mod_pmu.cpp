#include "mod_pmu.h"
#include "config.h"
#include <Arduino.h>
#include <XPowersLib.h>

// ============================================================
//  mod_pmu - AXP2101 PMU  (Wire1, SDA=GPIO15, SCL=GPIO7)
//  Verwendet XPowersLib (wie LilyGo AllFunction/power.cpp)
//  Wichtig: DC3 (3000mV) = Modem VDD, BLDO2 (3300mV) = Modem GPS
// ============================================================

static XPowersAXP2101 PMU;
static bool           s_pmu_ok  = false;
static int            s_batt_pct = -1;

void pmu_init() {
    if (!PMU.begin(Wire1, AXP2101_SLAVE_ADDRESS, PMU_SDA_PIN, PMU_SCL_PIN)) {
        Serial.println("[PMU] AXP2101 nicht gefunden!");
        return;
    }
    s_pmu_ok = true;
    Serial.printf("[PMU] AXP2101 OK  Pins: SDA=GPIO%d SCL=GPIO%d\n", PMU_SDA_PIN, PMU_SCL_PIN);

    // ---- Modem-Stromversorgung aktivieren (1:1 aus LilyGo AllFunction/power.cpp) ----
    // DC3  = Modem VDD   2700~3400mV → 3000mV
    PMU.setDC3Voltage(3000);
    PMU.enableDC3();
    // BLDO2 = Modem GPS  1400~3700mV → 3300mV
    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();
    Serial.println("[PMU] Modem-Power aktiviert: DC3=3000mV, BLDO2=3300mV");

    // ---- Lade-Parameter (wie LilyGo-Beispiel) ----
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_300MA);
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1);

    pmu_update();
}

void pmu_update() {
    if (!s_pmu_ok) return;
    if (!PMU.isBatteryConnect()) { s_batt_pct = -1; return; }
    s_batt_pct = (int)PMU.getBatteryPercent();
    if (s_batt_pct < 0)   s_batt_pct = 0;
    if (s_batt_pct > 100) s_batt_pct = 100;
}

int pmu_batt_pct() { return s_batt_pct; }

bool pmu_is_vbus_in() {
    if (!s_pmu_ok) return false;
    return PMU.isVbusIn();
}

bool pmu_is_charging() {
    if (!s_pmu_ok) return false;
    return PMU.isCharging();
}

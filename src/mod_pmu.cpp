#include "mod_pmu.h"
#include "config.h"
#include <Arduino.h>
#include <XPowersLib.h>
#include "esp_sleep.h"

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

    // ---- Modem-Stromversorgung aktivieren (wie LilyGo MinimalModemNBIOTExample) ----
    // Beim ersten Boot (kein Deep-Sleep-Wake): DC3 kurz abschalten → sauberer Power-Cycle.
    // Nach Deep Sleep: Modem war bereits via AT+CPOF abgeschaltet → kein Power-Cycle noetig.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
        PMU.disableDC3();
        delay(200);
        Serial.println("[PMU] Erster Boot: DC3 Power-Cycle");
    }

    // DC3  = Modem VDD   2700~3400mV → 3000mV
    PMU.setDC3Voltage(3000);
    PMU.enableDC3();
    // BLDO2 = Modem GPS  1400~3700mV → 3300mV
    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();
    Serial.println("[PMU] Modem-Power aktiviert: DC3=3000mV, BLDO2=3300mV");

    // TS-Pin Messung deaktivieren (wie LilyGo-Beispiel — sonst blockiert Laden)
    PMU.disableTSPinMeasure();

    // Nach EXT0-Wake (PMU INT): pending IRQ löschen, damit INT-Pin wieder HIGH geht
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        PMU.clearIRQ();
        Serial.println("[PMU] Wake-IRQ gelöscht (EXT0 VBUS-Insert)");
    }

    // ---- Lade-Parameter (wie LilyGo-Beispiel) ----
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_300MA);
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1);
    PMU.enableCellbatteryCharge();  // Laden aktivieren (war ggf. vor Deep Sleep deaktiviert)

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

void pmu_set_charging(bool on) {
    if (!s_pmu_ok) return;
    if (on) {
        PMU.enableCellbatteryCharge();
    } else {
        PMU.disableCellbatteryCharge();
    }
    Serial.printf("[PMU] Laden %s\n", on ? "aktiviert" : "deaktiviert");
}

void pmu_enable_vbus_wake() {
    if (!s_pmu_ok) return;
    PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);          // Slate clean
    PMU.enableIRQ(XPOWERS_AXP2101_VBUS_INSERT_IRQ);   // Nur VBUS-Insert
    Serial.println("[PMU] VBUS-Insert IRQ aktiviert");
}

void pmu_clear_wake_irq() {
    if (!s_pmu_ok) return;
    PMU.clearIRQ();
}

void pmu_set_gps_power(bool on) {
    if (!s_pmu_ok) return;
    if (on) {
        PMU.setBLDO2Voltage(3300);
        PMU.enableBLDO2();
    } else {
        PMU.disableBLDO2();
    }
    Serial.printf("[PMU] BLDO2 (GPS-Antenne) %s\n", on ? "an" : "aus");
}

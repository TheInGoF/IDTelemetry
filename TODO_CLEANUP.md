# Bereinigung — Offene Altlasten

Stand: 2026-04-04

## Guard-System: WiFi-Modi entfernen

CAN TX und Sleep hängen jetzt beide nur noch an VBUS.
Die WiFi-Guard-Modi (BLE, WiFi, AND, OR) werden nirgends mehr funktional genutzt.
WiFi-Guard-Task läuft aber noch (SSID-Scan, BLE-Check) — komplett toter Code.

### Entfernbar:
- `GUARD_MODE_BLE` / `GUARD_MODE_WIFI` / `GUARD_MODE_AND` / `GUARD_MODE_OR` → [mod_wifi_guard.h:29-32](src/mod_wifi_guard.h#L29-L32)
- `guard_mode` Variable + SPIFFS-Persistenz → [mod_wifi_guard.cpp:84](src/mod_wifi_guard.cpp#L84), [141](src/mod_wifi_guard.cpp#L141), [167](src/mod_wifi_guard.cpp#L167)
- `wifi_guard_set_mode()` / `wifi_guard_get_mode()` → [mod_wifi_guard.cpp:508-520](src/mod_wifi_guard.cpp#L508-L520)
- `wifi_in_range` Variable + `wifi_guard_in_range()` — nicht mehr für Sleep gebraucht
- `wifi_guard_active()` — nicht mehr für Sleep gebraucht
- Web-UI: `guard_mode` Dropdown/Anzeige + `doc["guard_mode"]` in [mod_config.cpp:144-145](src/mod_config.cpp#L144-L145), [175](src/mod_config.cpp#L175)
- Web-JSON: `mode`, `mode_str`, `wifi_ok` Felder → [mod_wifi_guard.cpp:588-598](src/mod_wifi_guard.cpp#L588-L598)
- WiFi-Scan-Loop im Guard-Task — nur noch für Web-UI-Anzeige "Guard in Range"
- Gesamtes `mod_wifi_guard` stark vereinfachbar: nur AP-Hotspot + SSID-Anzeige behalten

---

## PLMN-Tabelle: Phase 2 entfernt

Die 38-Netz-Tabelle (`PLMN_TABLE`) wird nicht mehr zum Durchprobieren genutzt
(Phase 2 entfernt). Sie dient jetzt nur noch als Namens-Lookup nach Auto-Modus-Verbindung.

### Prüfen:
- `PLMN_TABLE` + `PLMN_COUNT` → [mod_modem.cpp:157-219](src/mod_modem.cpp#L157-L219) — nur noch für `registered:`-Namensauflösung
- `is_green()` / `is_plmn_blocked()` — `is_plmn_blocked` wird noch in Phase 1 (Green-List) gebraucht
- Soft-Block-Logik (`BLOCK_SOFT`) — kann vereinfacht werden, da nur noch Green-List + Auto-Modus existiert
- Hard-Block-Logik (`BLOCK_HARD`, SPIFFS-Datei `/plmn_block.txt`) — prüfen ob noch sinnvoll (Auto-Modus blockt nicht manuell)

---

## Kompass: `compass_auto_cal` nicht mehr aufgerufen

`compass_auto_cal()` existiert in [mod_compass.cpp:155](src/mod_compass.cpp#L155) + [mod_compass.h:17](src/mod_compass.h#L17),
wird aber seit dem letzten Commit nirgends mehr aufgerufen (war in `telem_task`).
Heading kommt jetzt aus GPS COG.

Alle Aufrufe sind entfernt. Nur noch die Dateien selbst übrig.

### Entfernbar:
- `mod_compass.cpp` — komplett löschen
- `mod_compass.h` — komplett löschen

---

## Traccar: komplett entfernbar

Traccar wird nicht mehr genutzt (InfluxDB ist der einzige Telemetrie-Empfänger).

### Dateien:
- `mod_traccar.cpp` + `mod_traccar.h` — komplett entfernbar
- `TRACCAR_SEND_INTERVAL_MS`, `TRACCAR_MIN_MOVE_DEG` → [config.h](src/config.h)
- `SECRET_TRACCAR_HOST`, `SECRET_TRACCAR_PORT` → [secrets.h.example](src/secrets.h.example)
- `cfg_traccar_host()` / `cfg_traccar_port()` → [mod_config.h](src/mod_config.h) + [mod_config.cpp](src/mod_config.cpp)
- Traccar-Aufrufe in [mod_modem.cpp](src/mod_modem.cpp) (Send-Logik im LTE-Fenster)
- `g_traccar_ok` in [shared.h](src/shared.h)

---

## GPS Ext: `inject_assistnow` Deklaration verwaist

`gps_ext_inject_assistnow()` ist in [mod_gps_ext.h:17](src/mod_gps_ext.h#L17) deklariert,
die Implementierung existiert noch in `mod_gps_ext.cpp` (liest `/assistnow.bin`),
wird aber nirgends aufgerufen.

### Entfernbar:
- Header-Deklaration in `mod_gps_ext.h`
- Implementierung `gps_ext_inject_assistnow()` in `mod_gps_ext.cpp`
- SPIFFS-Datei `/assistnow.bin` (falls auf dem Gerät vorhanden)

# Bereinigung — Offene Altlasten

Stand: 2026-04-10

## Guard-System: WiFi-Modi entfernen

CAN TX und Sleep hängen jetzt beide nur noch an VBUS.
Die WiFi-Guard-Modi (BLE, WiFi, AND, OR) werden nirgends mehr funktional genutzt.
WiFi-Guard-Task läuft aber noch (SSID-Scan, BLE-Check) — komplett toter Code.

### Entfernbar:
- `guard_mode` Variable + SPIFFS-Persistenz
- `wifi_guard_set_mode()` / `wifi_guard_get_mode()`
- `wifi_in_range` Variable + `wifi_guard_in_range()` — nicht mehr für Sleep gebraucht
- `wifi_guard_active()` — nicht mehr für Sleep gebraucht
- Web-UI: `guard_mode` Dropdown/Anzeige + `doc["guard_mode"]` in mod_config.cpp
- Web-JSON: `mode`, `mode_str`, `wifi_ok` Felder
- WiFi-Scan-Loop im Guard-Task — nur noch für Web-UI-Anzeige "Guard in Range"
- Gesamtes `mod_wifi_guard` stark vereinfachbar: nur AP-Hotspot + SSID-Anzeige behalten

---

## PLMN-Tabelle: Phase 2 entfernt

Die 38-Netz-Tabelle (`PLMN_TABLE`) wird nicht mehr zum Durchprobieren genutzt
(Phase 2 entfernt). Sie dient jetzt nur noch als Namens-Lookup nach Auto-Modus-Verbindung.

### Prüfen:
- `PLMN_TABLE` + `PLMN_COUNT` — nur noch für `registered:`-Namensauflösung
- `is_green()` / `is_plmn_blocked()` — `is_plmn_blocked` wird noch in Phase 1 (Green-List) gebraucht
- Soft-Block-Logik (`BLOCK_SOFT`) — kann vereinfacht werden, da nur noch Green-List + Auto-Modus existiert
- Hard-Block-Logik (`BLOCK_HARD`, SPIFFS-Datei `/plmn_block.txt`) — prüfen ob noch sinnvoll

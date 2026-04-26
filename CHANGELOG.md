# Changelog

Alle nennenswerten Änderungen. Format: [Keep a Changelog](https://keepachangelog.com/),
Versionierung nach [SemVer](https://semver.org/).

## [1.1.0] – 2026-04-26

### Added
- **PLMN-Scan + Whitelist** (`AT+COPS=?`) als Eskalationsstufe wenn Auto-Mode kein registrierbares Netz findet. Telekom DE (26201) wird beim Boot bevorzugt mit 15 s Timeout, danach Fallback auf letzten erfolgreichen Provider, dann Auto, dann Scan.
- **Numerisches PLMN im MQTT-Payload** (Bit 19, u16) — ermöglicht serverseitige Provider-Statistik ohne Operator-Strings.
- **Coredump-Partition** (64 KB) — nach Boot werden PANIC-Backtrace, Task und PC ins Syslog geschrieben und das Image gelöscht. Neue Partition `coredump` in `partitions.csv`.
- **GPS-Glitch-Filter**: Positionssprünge >200 m/s (720 km/h) werden verworfen. Gedeckelt auf max. 2 km zwischen zwei Rows.
- **GPS-Zeit-Plausibilität**: UBX-Frames mit Jahr außerhalb 2024–2040 werden ignoriert (Schutz vor Cold-Start-Glitches mit Jahr 2093/2165).
- **PMU 0%-Glitch-Filter**: Wenn der AXP2101 Coulomb-Counter 0 % meldet, aber Zellspannung >3.3 V ist, wird der alte Wert beibehalten.
- **BLE-Diagnose**: Connect/Disconnect mit Peer-MAC, Conn-Interval, Supervision-Timeout, Uptime und Flap-Markierung (<500 ms).
- **AES-256-CBC Verschlüsselung** für MQTT-Payload (Pre-Shared Key in `secrets.h`).
- **SPIFFS-Row-Queue** — Telemetrie wird crash-sicher gepuffert und bei Verbindungsabriss bis zu Stunden-Lücken nachgesendet, row-by-row alt → neu.

### Changed
- **PMU-Lader**: Konstantstrom 300 mA → **1000 mA**, VBUS-Input-Limit explizit auf **2000 mA** (Default war 500 mA und limitierte tatsächlichen Ladestrom).
- **PMU-Update-Intervall**: Akkustand wird jetzt alle 10 s frisch per I2C gelesen (vorher nur bei Web-Hits — Cache blieb seit Boot stale).
- **MQTT-Watchdog 4-stufig**: (1) Reconnect alle 10 s → (2) Modem-Reset (PWRKEY) nach 3 Fails → (3) PLMN-Scan nach 2 Modem-Resets → (4) Reboot mit RTC-Counter (max. 3 in 30 min).
- **Reboot-Limit**: `RTC_DATA_ATTR`-Counter überlebt `esp_restart`. Verhindert endlose Reboot-Loops bei dauerhaftem Funkloch.
- **GPRS-IP-Validierung**: Erfolgreiche `getLocalIP()`-Rückgabe `0.0.0.0` wird abgelehnt, Verbindung neu aufgebaut.
- **SoC-Sample-Logik**: `min_delta` 0.3 → 0.1, `max_age` 90 s → 300 s — füllt Lücken im Server-Dashboard ohne Flut.
- **VBUS-Entprellung**: 3× Lesen mit Inversen-Flank-Erkennung verhindert PMU-Glitches als Fake-Sleep-Trigger.
- **Binäres MQTT-Protokoll**: Bitmask + Felder, AES-256-CBC. Ablöst HTTP/Influx-Direct.
- **AP-Timeout**: 10 min → 2 min ohne Client.

### Removed
- **WiFi Guard** komplett — VBUS-Detection als Sicherheits-Lock reicht aus.
- **Green-List PLMN**: ersetzt durch Whitelist im Scan.
- **Ignition-Logik (`ig=`)**: VBUS-Nachlaufzeit ist nicht reproduzierbar, hat keinen Mehrwert.
- **InfluxDB HTTP + Traccar HTTP**: durch persistenten MQTT-Push ersetzt.
- **Kompass-Modul**: redundant zu GPS-Heading.

### Fixed
- **eq_mask-Bug**: SoC-Plateaus auf dem Server kamen durch fälschlich `valid=false` markierte unveränderte Felder.
- **Recovery nach GPRS-Fail**: Premature State-Transition in `STATE_WAIT_FOR_NETWORK` → SIM-Re-Init → "GPS-only Modus" — entfernt, Eskalation greift jetzt natürlich.
- **uint32_t Underflow** beim Gyro-Sleep-Check (Race Condition).
- **Crash in `send_html`** bei fehlender SPIFFS-Datei.
- **Kurven-Trigger** hat Vorrang vor Distanz-Trigger (verhinderte sonst spätes Capture in scharfen Kurven).
- **Rows bei Publish-Fehler**: nicht mehr verworfen, gehen sauber in die SPIFFS-Queue.
- **MQTT Reconnect-Spam** bei dauerhaftem Verbindungsverlust.
- **AT+SMSSL** bei plain MQTT nicht mehr senden (SIM7080G FW R1951.07).

## [1.0.6] – 2026-03 (vorher)

- HTML-Cleanup, geteilte CSS/JS, README-Rewrite, i18n-Fixes.
- Frühere Entwicklung siehe `git log`.

[1.1.0]: https://github.com/TheInGoF/IDTelemetry-dev/releases/tag/v1.1.0

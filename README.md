# IDTelemetry

ESP32-S3 Telemetrie-Gerät entwickelt für den **VW ID.7 (2025)** mit OBD2/CAN-Bus, GPS, LTE und BLE.

## Hardware

| Komponente | Details |
| ---------- | ------- |
| MCU | LILYGO T-SIM7080G-S3 (ESP32-S3, 16MB Flash, 8MB PSRAM) |
| CAN-Transceiver | SN65HVD230 |
| Modem/GPS | SIM7080G (LTE-M/NB-IoT + GPS) |
| PMU | AXP2101 (onboard auf LILYGO) |
| RTC | DS1307 |
| Gyro/Lagesensor | MPU-6050 |

## Funktionen

- CAN-Bus Scanner (UDS/ISO-TP, OBD2)
- Echtzeit-Telemetrie via LTE → InfluxDB
- GPS-Tracking via Traccar (OsmAnd-Protokoll)
- BLE ELM327-Emulation für ABRP
- WiFi Access Point mit Web-UI
- Deep Sleep mit Gyro-Wake

## Web-UI

Gerät öffnet einen WiFi Access Point:

- SSID: `IDTelemetry`
- Passwort: `IDTelemetry1`
- URL: `http://192.168.4.1`

## Installationsanleitung

### Was du brauchst

| Teil | Beschreibung | ca. Preis |
| ---- | ------------ | --------- |
| LILYGO T-SIM7080G-S3 | ESP32-S3 Board mit LTE-M/NB-IoT Modem + GPS | ~35 € |
| SN65HVD230 CAN-Transceiver | 3.3V CAN-Bus Modul | ~3 € |
| OBD2-Stecker mit Kabel | 16-Pin OBD2 Stecker (Pin 6 + 14 für CAN) | ~5 € |
| DS1307 RTC Modul | Echtzeituhr (I2C) mit Knopfzelle | ~2 € |
| MPU-6050 Breakout | Beschleunigungs-/Lagesensor (I2C) | ~3 € |
| LTE-M SIM-Karte | z.B. ThingsMobile (Prepaid, kein Vertrag) | ~15 € |
| USB-C Kabel | Zum Flashen und für Stromversorgung | — |

### Schritt 1 — Software installieren

1. [VSCode](https://code.visualstudio.com) installieren
2. In VSCode die Extension **PlatformIO IDE** installieren
3. Dieses Repository klonen oder als ZIP herunterladen

### Schritt 2 — Zugangsdaten eintragen

```bash
cp src/secrets.h.example src/secrets.h
```

`src/secrets.h` öffnen und ausfüllen:

- **APN** eures Mobilfunkanbieters (z.B. `TM` für ThingsMobile)
- **Traccar** Host + Geräte-ID (falls GPS-Tracking gewünscht)
- **InfluxDB** Host, Org, Bucket, Token (falls Telemetrie gewünscht)
- **Guard SSID** des Fahrzeug-Hotspots (z.B. `"My VW 1747"`)

### Schritt 3 — Verdrahten

```text
LILYGO T-SIM7080G-S3          Externe Module
┌──────────────────┐
│ GPIO17 (TX) ─────┼──→ SN65HVD230 CTX ──→ OBD2 Pin 6  (CAN-H)
│ GPIO18 (RX) ─────┼──→ SN65HVD230 CRX ──→ OBD2 Pin 14 (CAN-L)
│                  │
│ GPIO45 (SDA) ────┼──→ DS1307 SDA + MPU-6050 SDA
│ GPIO21 (SCL) ────┼──→ DS1307 SCL + MPU-6050 SCL
│                  │
│ 3V3 ─────────────┼──→ VCC für alle Module
│ GND ─────────────┼──→ GND für alle Module
└──────────────────┘
```

SIM-Karte in den Slot auf der Unterseite des LILYGO einlegen (Nano-SIM).

### Schritt 4 — Firmware flashen

Board per USB-C anschließen, dann in VSCode/PlatformIO:

1. **Upload Filesystem Image** — lädt die Web-UI auf den ESP32 (nur einmalig nötig, bzw. bei HTML-Änderungen)
2. **Upload** — flasht die Firmware

Oder per Terminal:

```bash
pio run --target uploadfs   # Web-UI hochladen
pio run --target upload      # Firmware flashen
```

### Schritt 5 — Verbinden

1. Mit dem WLAN **IDTelemetry** verbinden (Passwort: `IDTelemetry1`)
2. Im Browser `http://192.168.4.1` öffnen
3. Auf der Config-Seite den WiFi Guard und Schwellenwerte einstellen

## Verdrahtung

```text
ESP32 GPIO17 → SN65HVD230 CTX
ESP32 GPIO18 → SN65HVD230 CRX
SN65HVD230 CANH → OBD2 Pin 6
SN65HVD230 CANL → OBD2 Pin 14

ESP32 GPIO45 → SDA  (DS1307 + MPU-6050)
ESP32 GPIO21 → SCL  (DS1307 + MPU-6050)

ESP32 GPIO4  → MODEM TX
ESP32 GPIO5  → MODEM RX
```

## Sicherheit & Energiesparen

### WiFi Guard

Der Guard ist **nur aktiv solange die konfigurierte WLAN-SSID des Fahrzeugs sichtbar ist**. Nur in diesem Zustand ist der CAN-Bus Sendebetrieb (TX) freigegeben. Ist die Fahrzeug-SSID nicht in Reichweite, bleibt TX gesperrt.

### Deep Sleep

Bei Inaktivität (kein Fahrbetrieb laut Gyro) fährt das Gerät in den Deep Sleep:

1. Letzte Telemetriedaten werden noch gesendet
2. ESP32-S3 wechselt in Deep Sleep (µA-Bereich)
3. Aufwachen automatisch sobald der Gyro Bewegung erkennt (Fahrzeug bewegt sich)

## Bekannte Einschränkungen (SIM7080G)

Der SIM7080G kann GPS und LTE-Datenverbindung **nicht gleichzeitig** betreiben.
Das Gerät wechselt daher alle 60 Sekunden zwischen beiden Modi:

```text
GPS aktiv (55s) → GPS aus → LTE an → Traccar + InfluxDB senden → LTE aus → GPS an
```

- Während der LTE-Phase (~5s) werden keine GPS-Koordinaten aktualisiert
- Während der GPS-Phase ist keine Datenverbindung aktiv
- Bei Stillstand (>3 min laut Gyro) pausiert der Zyklus komplett um Strom zu sparen

## Open-Source-Abhängigkeiten

| Bibliothek | Lizenz | Autor |
| ---------- | ------ | ----- |
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | Apache 2.0 | Espressif |
| [AsyncTCP](https://github.com/mathieucarbou/AsyncTCP) | LGPL v3 | mathieucarbou |
| [ESPAsyncWebServer](https://github.com/mathieucarbou/ESPAsyncWebServer) | LGPL v3 | mathieucarbou |
| [TinyGSM](https://github.com/vshymanskyy/TinyGSM) | LGPL v3 | vshymanskyy |
| [ArduinoJson](https://arduinojson.org) | MIT | Benoit Blanchon |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | Apache 2.0 | h2zero |
| [ArduinoHttpClient](https://github.com/arduino-libraries/ArduinoHttpClient) | Apache 2.0 | Arduino |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | MIT | lewis he |
| Space Mono & Syne | OFL | Google Fonts |

**Hinweise:**

- ELM327 AT-Befehlssatz: proprietäres Protokoll von [ELM Electronics](https://www.elmelectronics.com). Dieses Projekt implementiert eine Emulation für ABRP-Kompatibilität.
- OBD-II PIDs: SAE J1979 / ISO 15031 Standard, frei verwendbar.
- VW-spezifische UDS-DIDs (Service 0x22): Community-Reverse-Engineering, keine offizielle Dokumentation.

## Entwicklung

Dieses Projekt entstand nach dem **HITL-Prinzip (Human in the Loop)**:
Konzeption, Entscheidungen und Richtung kommen vom Menschen — der Code wird von [Claude](https://claude.ai) (Anthropic) geschrieben.

## Lizenz

GNU Affero General Public License v3.0 © 2026 Ingo F — siehe [LICENSE](LICENSE)

---

[![Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/lordvonbaum)

> Spenden sind freiwillig und dienen ausschliesslich der Unterstuetzung des Projekts. Sie haben keinen Einfluss auf die Priorisierung von Bugs, Feature-Wuenschen oder Support-Anfragen.

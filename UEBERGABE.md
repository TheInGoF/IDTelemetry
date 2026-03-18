# IDTelemetry — Übergabe an Server-Projekt

Dieses Dokument beschreibt die Daten die der ESP32 an InfluxDB sendet.
Es ist die einzige Referenz für das Server-Projekt (Grafana, Cronjobs, Geocoding).

---

## 1. InfluxDB-Verbindung

| Parameter | Wert |
| --- | --- |
| Protokoll | HTTPS POST |
| Endpoint | `/api/v2/write?org={org}&bucket={bucket}&precision=s` |
| Auth | Header `Authorization: Token {token}` |
| Format | InfluxDB Line Protocol |

---

## 2. Measurement und Tags

| Measurement | `v` |
| --- | --- |
| Tag `d` | Geräte-ID (z.B. `id7`) |

Ein Measurement für alle Werte. Keine getrennten Measurements.

---

## 3. Felder (Field Keys)

Der ESP sendet kurze Feldnamen um Datenvolumen zu sparen (ThingsMobile, ~3-5 MB/Monat).
Entschlüsselung auf dem Server via Grafana Alias/Transform.

### 3.1 Schnelle Werte (lokal alle 5s gesamplet)

| Feld | Bedeutung | Einheit | Typ | Wertebereich | Quelle |
| --- | --- | --- | --- | --- | --- |
| `s` | SoC (Ladestand) | % | float | -5 … 105 | CAN DID 0x028C |
| `u` | HV-Spannung | V | float | 300 … 500 | CAN DID 0x1E3B |
| `i` | HV-Strom | A | float | -2000 … 2000 | CAN DID 0x1E3D |
| `p` | Leistung | kW | float | -500 … 500 | berechnet u×i/1000 |
| `v` | Geschwindigkeit | km/h | float | 0 … 200 | CAN DID 0xF40D |
| `c` | Laden aktiv | — | int | 0 / 1 | CAN DID 0x7448 Bit 2 |
| `dc` | DC-Schnellladen | — | int | 0 / 1 | CAN DID 0x7448 Bit 1+2 |

### 3.2 Mittlere Werte (lokal alle 10s gesamplet)

| Feld | Bedeutung | Einheit | Typ | Wertebereich | Quelle |
| --- | --- | --- | --- | --- | --- |
| `bt` | Batterie-Temp | °C | float | -40 … 80 | CAN DID 0x2A0B |
| `et` | Außentemperatur | °C | float | -40 … 80 | CAN DID 0x2609 |
| `r` | Reichweite | km | float | 0 … 600 | CAN DID 0x0295 |
| `la` | GPS Latitude | ° | float | -90 … 90 | SIM7080G GPS |
| `lo` | GPS Longitude | ° | float | -180 … 180 | SIM7080G GPS |
| `g` | Beschleunigung | G | float | 0 … 16 | MPU-6050 Gyro |

### 3.3 Langsame Werte (lokal alle 30–60s gesamplet)

| Feld | Bedeutung | Einheit | Typ | Wertebereich | Quelle |
| --- | --- | --- | --- | --- | --- |
| `pk` | Geparkt | — | int | 0 / 1 | CAN DID 0x210E |
| `ca` | Kapazität | kWh | float | 1 … 200 | CAN DID 0x2AB2 |
| `kw` | Geladen gesamt | kWh | float | 0 … 99999999 | CAN DID 0x1E32 |
| `od` | Odometer | km | float | 0 … 999999 | CAN DID 0x295A |
| `ls` | LTE-Signal | CSQ | int | 0 … 31 | SIM7080G Modem |
| `bd` | ESP-Akku | % | int | 0 … 100 | AXP2101 PMU |

---

## 4. Sendeformat

Alle Werte werden **alle 60s** als ein Line-Protocol-String gesendet.
Nur gültige Felder werden gesendet (ungültig = kein CAN/GPS → Feld fehlt).

```text
v,d=id7 s=72.3,u=385.2,i=-12.5,p=-4.8,v=87,c=0i,dc=0i,bt=22.1,et=15.3,r=285,la=53.815277,lo=10.376152,g=1.02,pk=0i,ca=58.2,kw=12345.6,od=23456,ls=-85i,bd=87i 1710500000
```

- Timestamp: Unix-Sekunden (precision=s)
- Integer-Felder (c, dc, pk, ls, bd) haben Suffix `i`
- Fehlende Felder = Sensor nicht verfügbar (kein GPS-Fix, CAN offline, etc.)

---

## 5. Betriebszustände

| Zustand | Was sendet der ESP? | Intervall |
| --- | --- | --- |
| Fahrt | Alle Felder | 60s |
| Laden | Alle Felder (v=0, c=1 oder dc=1) | 60s |
| Parken | Kurz nach Abstellen, dann **Deep Sleep** — sendet nichts | — |
| Sleep | Nichts. Wake-up durch Vibration (Gyro) | — |

Der ESP schläft nach 5 Minuten ohne WiFi-Guard-Signal ein.
Im Sleep wird kein Datenvolumen verbraucht.

---

## 6. Fahrtenerkennung (Server-Seite)

### Logik

| Ereignis | Erkennung |
| --- | --- |
| Fahrt-Start | `v > 0` nach vorherigem `v == 0` oder `pk == 1` |
| Fahrt-Ende | `v == 0` für >3 Minuten oder `pk == 1` |
| Lade-Start | `c` wechselt 0→1 |
| Lade-Ende | `c` wechselt 1→0 |

### Berechnete Werte pro Fahrt

| Wert | Berechnung |
| --- | --- |
| Strecke (km) | `od(Ende) - od(Start)` |
| SoC-Verbrauch (%) | `s(Start) - s(Ende)` |
| Energie (kWh) | `∫ p dt` über Fahrtzeit (Summe der 60s-Leistungswerte / 60) |
| Verbrauch kWh/100km | `Energie / Strecke × 100` |
| Adresse Start | Reverse-Geocoding von `la/lo` bei Fahrt-Start |
| Adresse Ziel | Reverse-Geocoding von `la/lo` bei Fahrt-Ende |

### Reichweite bei 100% SoC (Batterie-Gesundheit)

Für Langzeit-Degradationsanalyse:

```text
Wenn s >= 99.5: max_range = r
```

Diesen Wert regelmäßig speichern (z.B. nach jedem Volllade-Vorgang).
Über Monate/Jahre zeigt der Trend die Zell-Degradation.

---

## 7. Reverse-Geocoding

- **API:** nominatim.openstreetmap.org oder photon.komoot.io (kostenlos, max 1 Req/s)
- **Wann:** Bei erkanntem Fahrt-Start und Fahrt-Ende die `la/lo`-Werte auflösen
- **Wohin:** SQLite oder PostgreSQL auf Synology 920
- **Volumen:** ~5-10 Requests/Tag, kostenlose API reicht

---

## 8. Geplante Grafana-Dashboards

### Dashboard 1: Live

- SoC Gauge, Spannung/Strom/Leistung
- Karte mit Position
- Batterie- und Außentemperatur
- Ladestatus, ESP-Akku, LTE-Signal

### Dashboard 2: Fahrten

- Tabelle: Datum, Start→Ziel, Strecke, Verbrauch, kWh/100km
- Klick → Detail: Track auf Karte, SoC/Speed/Leistung über Zeit

### Dashboard 3: Laden

- Ladevorgänge (AC/DC), SoC-Verlauf, Ladeleistung über Zeit
- Ladepunkte auf Karte

### Dashboard 4: Fahrtenbuch

- Fahrten mit Zweck (privat/dienstlich) — manuell zuweisbar
- Summen km privat/dienstlich, Anteil %
- CSV-Export

### Dashboard 5: Batterie-Gesundheit

- Reichweite bei 100% SoC über Zeit (Degradationskurve)
- Kapazität (ca) über Zeit
- Ladezyklen (kw-Differenz / ca)

---

## 9. Datenvolumen

| Szenario | Volumen |
| --- | --- |
| 1 Zeile | ~200 Bytes |
| 1 Stunde | ~12 KB (60 Zeilen × 200 B) |
| 8h Fahrt | ~96 KB |
| 1 Monat | ~3-5 MB |

ThingsMobile-kompatibel. Im Sleep: 0 Bytes.

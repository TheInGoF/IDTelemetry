# Changelog

All notable changes to this project will be documented in this file.
Format: [Keep a Changelog](https://keepachangelog.com/),
versioning per [SemVer](https://semver.org/).

## [1.0.0] – 2026-04-26

Initial public release.

### Highlights

- ESP32-S3 firmware for the LILYGO T-SIM7080G-S3 telemetry stick (VW ID family).
- CAN bus reader (UDS/ISO-TP) for SoC, voltage, current, power, battery temperature, range, odometer, charge state, and more.
- Persistent **MQTT** push over LTE-M with **AES-256-CBC** payload encryption (pre-shared key).
- **External GPS** support (BLITZ Mini M10) for continuous tracking without LTE/GPS multiplexing.
- **BLE ELM327 emulation** so ABRP and Torque see the car as a standard OBD reader.
- **Web UI** (live data, debug, config) on the device's WiFi AP.
- **Deep sleep** with motion-wake (MPU-6050) and VBUS-wake (AXP2101) — µA-range when parked.

### Reliability

- 4-stage modem watchdog: MQTT reconnect → modem PWRKEY reset → PLMN scan → controlled reboot with RTC-counter limit (max 3 reboots / 30 min).
- PLMN whitelist + scan fallback when auto-mode fails to find a registrable network.
- SPIFFS-backed row queue: telemetry survives power cycles and is replayed in chronological order after reconnect.
- GPS glitch filter: rejects positional jumps >200 m/s, capped at 2 km between consecutive rows.
- GPS time plausibility filter (year 2024–2040) — guards against cold-start glitches.
- AXP2101 fuel-gauge filter: 0 % readings with cell voltage above 3.3 V are discarded.
- VBUS debouncing (3× consecutive read) prevents spurious sleep triggers from PMU glitches.
- Coredump partition: PANIC backtrace, crashing task, and PC are logged on the next boot.

### Power

- Charge current 1000 mA, VBUS input limit 2000 mA — designed for 2200 mAh Li-Po with ~0.5 C charging while running on USB-PD.
- Buck-charger termination handled in hardware: no trickle, automatic recharge below ~95 %.

[1.0.0]: https://github.com/TheInGoF/IDTelemetry/releases/tag/v1.0.0

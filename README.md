<div align="center">

# AIO Transmitter (AIO-TX)

**An open-source, multi-protocol wireless control platform built around the ESP32-C6**

[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/MCU-ESP32--C6-00e5ff)](https://www.espressif.com/en/products/socs/esp32-c6)
[![Status](https://img.shields.io/badge/REV%202.0-in%20development-7c4dff)]()

[Website](https://aio-transmitter.vercel.app) · [Full Documentation](AIO-Transmitter-Documentation.md) · [StrawberryOS Ecosystem](https://github.com/periashutosh-prog/StrawberryOS) · [Smart_Connect Library](https://github.com/periashutosh-prog/Smart-Connect)

</div>

---

## What is AIO-TX?

AIO Transmitter is a universal wireless control interface — not a remote built for one job, but a modular platform that adapts to whatever you're building: an RC vehicle, a robot, a wireless HID peripheral, or a custom ESP-based project of your own.

It's built around the **ESP32-C6**, giving it native support for **WiFi 6, Bluetooth Low Energy, and ESP-NOW** in one board, plus a standalone **0.96" OLED interface** for configuring modes, profiles, and control mappings without ever needing a laptop or companion app.

The platform is designed for two audiences at once: makers and developers who want to fork the fully open hardware and firmware, and beginners who can start controlling something in minutes using preset protocol profiles with no wireless-protocol expertise required.

---

## ✅ Key Features

- **Multi-protocol wireless communication** — WiFi 6, BLE, and ESP-NOW on a single board, with CAN Bus planned for REV 3.0
- **Dual pairing modes** — a fast, unauthenticated **ESP-NOW** mode for trusted low-latency setups, and a PIN-authenticated **Smart Connect** mode for secured sessions
- **Standalone onboard UI** — a 0.96" OLED handles mode switching, WiFi setup, live telemetry, and built-in apps, with no external device required
- **BLE HID emulation** — the same hardware functions as a wireless mouse, keyboard, or media remote for PCs, TVs, and mobile devices
- **17 tactile inputs** — dual D-pads, dual simulated analog channels, and system controls, all read through a single I2C MCP23017 expander (plus one native-GPIO system button)
- **Live link-quality telemetry** — round-trip ping and packet-loss percentage, tracked and shown in real time during an active session
- **Command-silence watchdog** — both transmitter and receiver detect a stalled link and safely end the session, instead of a controlled device executing a stale last command indefinitely
- **Rechargeable battery system** — USB-C charging, with REV 2.0 adding integrated I2C battery telemetry
- **Open, extensible receiver ecosystem** — the companion [`Smart_Connect`](https://github.com/periashutosh-prog/Smart-Connect) library (ESP32 + ESP8266) lets anyone build a compatible receiver — for a robot, an RC car, or any project of their own — with almost no glue code

---

## 🧩 Hardware Revisions

| | REV 1.0 | REV 2.0 |
|---|---|---|
| **Approach** | Module-based (proves the concept) | Discrete-IC based (production-oriented redesign) |
| **MCU** | Seeed XIAO ESP32-C6 breakout | Bare ESP32-C6-MINI-1U module, custom PCB |
| **Power management** | TP4056 charger + MT3608 boost | IP5306-I2C (charge + boost + battery telemetry) |
| **Antenna** | Onboard PCB trace only | Onboard trace + external IPEX-to-SMA option |
| **Enclosure** | SLA resin | CNC-machined aluminum |

REV 2.0 exists specifically to bridge REV 1.0 and REV 3.0: REV 1.0 proved the concept using off-the-shelf modules, and REV 3.0's goal — full CAN Bus modularity with plug-and-play expansion — requires the discrete-IC design competency REV 2.0 is built to develop.

Full architecture, pairing flow, and design rationale: see [AIO-Transmitter-Documentation.md](AIO-Transmitter-Documentation.md).

---

## 🚗 Receiver Ecosystem

AIO-TX is only half the system — the [`Smart_Connect`](https://github.com/periashutosh-prog/Smart-Connect) library is the receiver-side counterpart, available for both **ESP32** and **ESP8266**. It handles advertising, handshake, PIN authentication, and telemetry replies, so a receiver project only needs to implement its own control logic (driving motors, actuators, etc.) on top of it.

An example receiver — an ESP8266-based RC car using an L298N motor driver — is part of this ecosystem, demonstrating full differential steering (forward/back/left/right), live motor-status telemetry back to the transmitter, and an automatic failsafe that cuts motors if the wireless link drops.

---

## 🚀 Getting Started

1. **Get the PCB files** — fabricate from the schematics in `Hardware Design Files/` via JLCPCB or any manufacturer.
2. **Assemble the board** — REV 1.0 is hand-solderable; a full BOM is provided.
3. **Flash the firmware** — compile `AIO_Transmitter.ino` yourself, or use the web-based flashing tool at [aio-transmitter.vercel.app](https://aio-transmitter.vercel.app).
4. **Print the enclosure** — design files included.

Full step-by-step build instructions: [AIO-Transmitter-Documentation.md](AIO-Transmitter-Documentation.md#-how-to-make-aio-transmitter-rev-10).

---

## 🌱 Part of the StrawberryOS Ecosystem

AIO-TX is built on shared firmware architecture and design language from [**StrawberryOS**](https://github.com/periashutosh-prog/StrawberryOS), an open ecosystem of modular embedded devices — including an earlier project, **StrawberryWatch** (originally QuantumWatch), on [OSHWLab](https://oshwlab.com/periashutosh/quantumwatch).

---

## 🏆 EasyEDA Spark 2026

This project is proudly participating in **[EasyEDA Spark 2026](https://oshwlab.com/activities/easyeda-spark-2026?inviter=periashutosh)** — an open-source hardware competition backed by an **$85,000 prize pool**, with free PCB, 3D printing, and CNC manufacturing support for eligible projects.

If you're building your own open-source hardware project — whether it's a fully custom board like AIO-TX or something just getting off the ground — it's well worth checking out. Sponsorship covers real manufacturing costs, not just recognition, which made a genuine difference in getting this project built.

👉 **[Join EasyEDA Spark 2026](https://oshwlab.com/activities/easyeda-spark-2026?inviter=periashutosh)**

---

## 📄 License

AIO-TX firmware and hardware are licensed under **GNU GPLv3**. Personal, educational, and non-commercial use only — see [LICENSE](LICENSE) for details.

This project is provided as-is for educational and experimental purposes: not certified for production/commercial use, no warranties, and users assume all risk associated with building and operating the device. Always follow local electrical and RF/radio regulations in your region.

---

## 📬 Links

- **Website:** [aio-transmitter.vercel.app](https://aio-transmitter.vercel.app)
- **Full Documentation:** [AIO-Transmitter-Documentation.md](AIO-Transmitter-Documentation.md)
- **Issues / Contributing:** [github.com/periashutosh-prog/AIO-Transmitter/issues](https://github.com/periashutosh-prog/AIO-Transmitter/issues)
- **Smart_Connect (receiver library):** [github.com/periashutosh-prog/Smart-Connect](https://github.com/periashutosh-prog/Smart-Connect)
- **StrawberryOS:** [github.com/periashutosh-prog/StrawberryOS](https://github.com/periashutosh-prog/StrawberryOS)

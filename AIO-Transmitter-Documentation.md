# AIO-Transmitter Documentation

## 📋 Table of Contents

- [What Exactly is AIO-Transmitter?](#what-exactly-is-aio-transmitter)
- [Introduction to the Idea](#introduction-to-the-idea)
- [Product Functions, Application Scenarios & Design Details](#-product-functions-application-scenarios--design-details)
- [System Architecture](#-system-architecture)
- [How AIO-Transmitter Actually Works](#-how-aio-transmitter-actually-works)
- [Hardware Revisions](#-hardware-revisions)
- [Technical Specifications](#-technical-specifications)
- [Getting Started — How to Make AIO-Transmitter (REV 1.0)](#-how-to-make-aio-transmitter-rev-10)
- [Firmware & Software](#-firmware--software)
- [Comparison with Other Systems](#-comparison-with-other-systems)
- [License & Disclaimer](#-license--disclaimer)
- [Documentation, Contributing, Contact & Acknowledgments](#-documentation-contributing-contact--acknowledgments)

---

## What Exactly is AIO-Transmitter?

AIO Transmitter, also known as AIO-TX, is a wireless transmitter platform. AIO stands for **All In One**, which describes the core philosophy of the project: although it functions as a transmitter, it is capable of far more than a typical single-protocol transmitter.

AIO-TX is powered by the **ESP32-C6**, a chip well known for its strength in wireless communication. It supports three wireless protocols and one wired protocol:

- **WiFi** (WiFi 6) — for internet connectivity, security, and high reliability
- **Bluetooth Low Energy (BLE)** — for connecting directly to smart devices and phones
- **ESP-NOW** — for ultra-low-latency, peer-to-peer communication
- **Controller Area Network (CAN)** — a wired, industrial-grade protocol (REV 3.0 exclusive)

This breadth means AIO-TX is designed to fit almost any wireless project. Whether you're building a low-latency RC car, a robot that demands high security, or simply want a transmitter that connects seamlessly to your phone, AIO Transmitter is built to handle it.

The platform is intentionally designed with two types of users in mind. Senior developers and experienced makers have the freedom to tailor the firmware and hardware to their exact needs, since both are fully open source. At the same time, beginners can start using AIO-TX almost immediately thanks to preset profiles for each protocol, meaning no deep wireless protocol knowledge is required to get started.

### ✅ Key Features

- ✅ Multi-protocol: WiFi 6, BLE, ESP-NOW, CAN (REV 3.0)
- ✅ Dual pairing modes — ESP-NOW (direct, no auth) and Smart Connect (PIN-authenticated)
- ✅ 17-button input (dual D-pad, dual analog, system controls) via MCP23017 I2C expander
- ✅ Onboard 0.96" OLED display with tile-based UI, no companion app or laptop required
- ✅ Rechargeable Li-ion battery with USB-C charging
- ✅ Fully open source — hardware and firmware
- ✅ Companion `Smart_Connect` receiver library (ESP32 + ESP8266) for building your own receivers
- ✅ No vendor lock-in

---

## Introduction to the Idea

The idea behind AIO-TX came from wanting something more than just another remote controller. Most transmitters are built to do one job and do it well, but they stop there. AIO-TX was designed to break out of that single-function mold and become a modular system that adapts to whatever you're building, whether that's controlling a robot, running an RC setup, acting as wireless gaming input, or reading data from sensors.

What really sets the project apart is how it brings ESP-NOW, BLE, and WiFi together on one board alongside modular hardware expansion. That combination means AIO-TX isn't limited to just talking to one device, it can handle direct one-to-one control just as easily as distributed communication across multiple nodes.

The onboard OLED display plays a big part in making this practical day-to-day, since it lets you configure settings and switch modes in real time without needing a laptop or any external software running alongside it. Looking ahead, the firmware is being built with room to grow, including structured control profiles and an early concept of OS-like behavior through StrawberryOS integration.

If you want to dig deeper into StrawberryOS, you can check it out here:
- [StrawberryOS GitHub Repository](https://github.com/periashutosh-prog/StrawberryOS)
- [StrawberryWatch on OSHWLab](https://oshwlab.com/periashutosh/quantumwatch)

And for everything related to the current project, the official page is here:
[AIO-Transmitter Website](https://aio-transmitter.vercel.app)

---

## 🎯 Product Functions, Application Scenarios & Design Details

### Product Functions

- **Multi-protocol wireless transmission** — ESP-NOW for ultra-low-latency peer-to-peer control, BLE for direct pairing with phones/tablets/PCs, WiFi 6 for network connectivity, and CAN (REV 3.0) for wired industrial-grade links.
- **Dual-mode receiver pairing** — an open ESP-NOW mode for fast, trusted-environment connections, and a PIN-authenticated Smart Connect mode for secured sessions.
- **Full physical control surface** — 17 tactile buttons total: 2 D-pads, 2 simulated analog channels (as up/down button pairs), and 3 system controls, nearly all read through a single I2C-based MCP23017 expander.
- **Standalone onboard UI** — a tile-based OLED interface for navigating modes, entering WiFi credentials, running built-in apps (e.g. Timer), and adjusting settings, with no laptop or companion app required.
- **Live link-quality telemetry** — round-trip ping time and packet-loss percentage are tracked and displayed in real time during an active session, alongside whatever telemetry (battery, speed, sensor data) the receiver reports back.
- **Command-silence watchdog** — both transmitter and receiver monitor for a stalled link and safely tear down the session if communication stops, instead of letting a controlled device keep executing a stale last command.
- **Power management** — automatic display dimming on inactivity, plus a manual long-press shutdown into light sleep with wake-on-button-press, so the device can be put to sleep and resumed without a full reboot.
- **Rechargeable battery system** — Li-ion battery with USB-C charging (REV 1.0: TP4056 linear charger; REV 2.0: IP5306-I2C with pass-through charge/discharge and I2C battery telemetry).
- **Open extensibility** — companion `Smart_Connect` Arduino library (ESP32 + ESP8266) lets anyone build a compatible receiver with almost no glue code, and exposed I2C/power test pads allow hardware expansion on REV 1.0.

### Application Scenarios

- **RC vehicles and drones** — ESP-NOW mode delivers near-instant control input where every millisecond of latency matters.
- **Robotics projects** — dual D-pad and analog channel input for driving or arm control, with live telemetry (battery level, speed, sensor readings) shown directly on the transmitter's own OLED.
- **Wireless gaming and general input** — BLE mode lets AIO-TX connect straight to a phone, tablet, or PC as a control peripheral, with no receiver hardware needed at all.
- **Distributed IoT/sensor setups** — WiFi connectivity combined with ESP-NOW's peer-to-peer nature lets a single transmitter relay commands to, or pull data from, multiple nodes.
- **Security-conscious deployments** — Smart Connect's PIN-authenticated pairing is the right choice anywhere an open, unauthenticated ESP-NOW broadcast would be a liability.
- **Education and maker projects** — preset protocol profiles mean beginners can get moving without wireless-protocol expertise, while the fully open firmware gives experienced developers a base to fork and customize.
- **Field use without a laptop** — since mode switching, WiFi setup, and telemetry all live on the onboard OLED, AIO-TX is usable entirely standalone, away from a desk.

### Design Details

- **Shared single I2C bus** — the SSD1306 OLED (`0x3C`) and MCP23017 expander (`0x20`) sit on the same bus, keeping wiring centralized and freeing up native ESP32-C6 GPIOs. The trade-off is that 16 of the 17 tactile buttons (both D-pads, both analog channels, and two of the three system controls) have to be read via I2C polling rather than direct GPIO interrupts, so the firmware does edge-detection in software to distinguish a fresh "just pressed" event from a button simply being held.
- **Power button on native GPIO, deliberately** — the System Center/Power button is the one control wired directly to a native ESP32-C6 pin instead of through the MCP23017. This is intentional: it lets the button keep working as a wake source even while the device is in light sleep and the I2C bus itself is powered down.
- **Channel pinning before any handshake** — both pairing modes require transmitter and receiver to lock to the same WiFi channel first. Skipping this step lets ESP-NOW broadcast packets through while silently breaking the unicast handshake, so channel pinning always happens before any other radio setup.
- **Screen-state-machine UI** — a single `ScreenState` variable plus one draw function per screen drives the entire interface. Transitions are done with simple offset-based slide animation rather than a graphics library, keeping the UI responsive within the ESP32-C6's flash/RAM budget.
- **Retry-based pairing reliability** — Smart Connect retransmits connection requests and PIN submissions every 700ms until acknowledged, so a single dropped packet on a noisy 2.4GHz link doesn't stall the pairing process.
- **Module-based REV 1.0, discrete-IC REV 2.0** — REV 1.0 deliberately uses pre-built modules (XIAO ESP32-C6 breakout, TP4056 charger) so it can be hand-soldered without specialized equipment. REV 2.0 moves to discrete ICs (bare ESP32-C6 module, IP5306-I2C power management) for a smaller, more production-oriented board, trading easy hand-assembly for a tighter, more integrated design.
- **Optional external antenna (REV 2.0)** — the IPEX-to-SMA antenna connector is provisioned but optional; the board still functions fully on its onboard PCB antenna, with the connector there for users who specifically need extended range.

---

## 🏗️ System Architecture

AIO-TX centers around the **ESP32-C6** as the main controller, with all physical inputs and the display sharing a single I2C bus:

- **ESP32-C6 (MCU)** — handles all wireless protocols (WiFi, BLE, ESP-NOW), runs the UI state machine, and drives the OLED.
- **MCP23017 (I2C GPIO expander, address `0x20`)** — 16 of the 17 tactile buttons (both D-pads, both analog channels, and two of the three system controls) route through this expander rather than direct MCU GPIOs, freeing up native pins and keeping wiring centralized.
- **SSD1306 OLED (I2C, 128×64)** — shares the same I2C bus as the MCP23017, drives the tile-based home screen and all sub-menus.
- **Power management** — REV 1.0 uses a TP4056 linear charger; REV 2.0 upgrades to an IP5306-I2C power-bank IC with pass-through charge/discharge and I2C battery telemetry.

**Pairing modes:**
- **ESP-NOW** — direct broadcast/unicast, no authentication, lowest latency. Best for quick setups and trusted environments.
- **Smart Connect** — PIN-authenticated pairing over the same ESP-NOW radio. The receiver generates a random PIN on its own display; the transmitter must submit the matching PIN before a session opens.

Any receiver built with the companion [`Smart_Connect`](https://github.com/periashutosh-prog/Smart_Connect) Arduino library (ESP32 and ESP8266 supported) pairs with AIO-TX out of the box, with no extra glue code required.

---

## ⚙️ How AIO-Transmitter Actually Works

This section walks through what the firmware is actually doing, from the moment you power the device on to an active control session.

### 1. Boot & Initialization

On power-up, the ESP32-C6 brings up the I2C bus first, since everything else on the board — the OLED and the MCP23017 — depends on it. The OLED initializes and immediately shows a boot screen while the MCP23017 is configured with all 16 of its pins set to input-pullup, so every button reads HIGH (unpressed) by default and pulls LOW only when physically pressed. A short ascending boot chime plays through the buzzer, and the firmware lands on the home screen — a tile grid for Transmitter, Applications, and Settings.

### 2. Reading Input

Rather than wiring buttons directly to the microcontroller, 16 of the 17 tactile buttons (both D-pads, both analog channels, and two of the three system controls) route through the MCP23017 I/O expander over I2C. Every pass through the main loop, the firmware reads the expander's two GPIO banks in one I2C transaction, compares the result against the previous read to detect "just pressed" edges (not just "currently held"), and updates the UI accordingly. This is why menu navigation feels immediate — the button state is fresh every single loop iteration, not polled on a slow timer. The System Center/Power button is the one exception: it's wired to a native ESP32 GPIO rather than the expander, specifically so it can double as a wake source when the device is asleep and the I2C bus itself is powered down.

### 3. The UI

The interface is a screen-state machine — one `ScreenState` value tracks exactly what's on screen at any moment, and each screen has its own draw function. Moving between screens isn't an instant cut; the firmware slides the outgoing and incoming screens past each other horizontally or vertically, giving the tile-based menu its animated feel without needing a graphics library beyond what the OLED driver already provides.

### 4. Pairing — Two Different Trust Models

AIO-TX supports two ways to connect to a receiver, both riding on the same ESP-NOW radio, both requiring transmitter and receiver to first pin themselves to the same WiFi channel (channel 1) — without this, ESP-NOW's unicast handshake silently fails even though broadcast packets still get through, which is why channel pinning happens before anything else in the radio setup.

- **ESP-NOW mode:** receivers running in this mode broadcast their own advertising packets; the transmitter passively scans and listens, building a list of nearby receivers by name — it does not broadcast first. You manually select one from that list, and only then does the transmitter send a unicast **Hello** packet directly to that specific receiver. The receiver replies with a **Hello_ACK**, followed by a **CONN_OK** once it's ready — the transmitter echoes CONN_OK back to confirm, and the session opens. No PIN, no encryption at any step — fastest to set up, intended for trusted environments where you already know what you're connecting to.
- **Smart Connect mode** builds on the same scan-and-select flow with an added authentication layer: after picking a receiver, the transmitter sends a connection request and waits for the receiver's acknowledgment that it's ready for a PIN. Once acknowledged, you're prompted to enter the 4-digit PIN the receiver is displaying on its own screen. The transmitter submits that PIN; only a match unlocks the session — a wrong PIN fails the connection outright. Every connection request and PIN submission is retransmitted every 700ms until a reply arrives, so a single dropped packet over a noisy link doesn't stall the pairing process.

### 5. Active Control & Telemetry

Once connected, the transmitter continuously packs the current DPAD and analog channel state into a formatted command packet and sends it to the receiver. The receiver replies with telemetry — battery level, speed, or whatever the receiver-side sketch chooses to report — which shows up on the transmitter's Telemetry screen. The firmware also tracks round-trip ping time and packet-loss percentage (sent vs. acknowledged) in real time, so you can see link quality directly on the Control screen rather than just guessing. Both sides run a command-silence watchdog — if the transmitter stops sending, the receiver notices and safely tears down the session on its own, rather than a vehicle or robot continuing to execute a stale last command forever.

### 6. Power Management

The firmware tracks inactivity and dims the display after a timeout, but the real power-saving step is manual: holding the System Center/Power button for 3 seconds from the Home or Applications Menu opens a shutdown confirmation. Confirming it plays a short "Shutting Down…" animation, then the ESP32-C6 drops into light sleep with the power button armed as a wake source. A single press wakes the chip back up — not a full reboot, but a resumed execution that replays the boot screen and chime for a consistent "freshly powered on" feel, before returning you exactly to where the sleep/wake cycle left off.

---

## 🧩 Hardware Revisions

| | REV 1.0 | REV 2.0 |
|---|---|---|
| **MCU Module** | Seeed XIAO ESP32-C6 (breakout board) | Bare ESP32-C6 module, directly on PCB |
| **Power Management** | TP4056 linear charger + MT3608 boost | IP5306-I2C (charge + boost + I2C battery telemetry) |
| **Charging** | USB-C, 5V/1A | USB-C, 5V/2A |
| **USB** | Via XIAO's onboard USB-C | Native USB-C sink, on-board CC negotiation |
| **Enclosure** | SLA 9000HE resin case, 8001 translucent resin top | Aluminum CNC case + 9000HE resin cover |
| **Antenna** | Onboard PCB trace (XIAO module) | External via IPEX-to-SMA, screw-on antenna |
| **Approach** | Module-based, hand-solderable | Discrete ICs, custom power/USB circuitry |

---

## 🔧 Technical Specifications

| Specification | Value |
|---|---|
| MCU | ESP32-C6 (WiFi 6, BLE 5, 802.15.4, ESP-NOW) |
| Display | 0.96" OLED, 128×64, SSD1306, I2C |
| I/O Expander | MCP23017, I2C address `0x20` |
| Inputs | 17 tactile buttons — 2 D-pads, 2 simulated analog channels, 3 system controls |
| Battery | Li-ion, rechargeable, 3800mAh |
| Charging (REV 1.0) | USB-C, 5V/1A (TP4056) |
| Charging (REV 2.0) | USB-C, 5V/2A (IP5306-I2C) |
| Wireless Protocols | WiFi 6, BLE, ESP-NOW |
| Wired Protocol | CAN (REV 3.0 exclusive) |
| Wireless Range | 500m (with external antenna) |
| Antenna | Onboard PCB antenna (standard) + external antenna provision via IPEX-to-SMA (optional, extended range) |
| Dimensions | 13.2 × 12.9 × 4 cm |
| Weight | 234g |
| Enclosure (REV 1.0) | SLA resin (9000HE body, 8001 translucent top) |
| Enclosure (REV 2.0) | Aluminum CNC + resin cover |

---

## 🚀 How to Make AIO-Transmitter (REV 1.0)

Since both the hardware design and firmware are open source, building your own AIO-TX REV 1.0 is straightforward.

### 1. Get the PCB Files

Export the PCB design files from the project repository and fabricate them through **JLCPCB**, or any PCB manufacturer of your choice.

### 2. Assemble the Board

REV 1.0 is built using modular components, which means it can be hand-soldered without specialized equipment. If you'd prefer not to solder it yourself, JLCPCB also offers professional assembly services.

A full component list (Bill of Materials) for REV 1.0 is provided separately — please refer to it before ordering parts, to ensure you have everything needed for assembly.

### 3. Flash the Firmware

Once your board is assembled, the next step is uploading the firmware to the **XIAO ESP32-C6** module. You have two options:

- **Compile it yourself** — Download the source code from the [GitHub repository](https://github.com/periashutosh-prog/AIO-Transmitter), compile it, and upload it using your preferred development environment.
- **Flash it directly** — Use the web-based flashing tool available on the [AIO-Transmitter website](https://aio-transmitter.vercel.app/) to flash the firmware straight to your board without needing to set up a development environment.

### 4. Print the Enclosure

Once your PCB is assembled and flashed, print the enclosure (design files provided in the repository) to complete your build.

At this point, your AIO-Transmitter REV 1.0 is fully built and ready to use.

---

## 💻 Firmware & Software

AIO-TX's firmware is a single Arduino sketch (`AIO_Transmitter.ino`) covering:

- Tile-based UI with slide animations, driven off a screen-state machine
- MCP23017 button polling with edge detection
- ESP-NOW transmitter logic (scan, connect, session management, telemetry)
- Smart Connect PIN-pairing flow
- WiFi credential management (scan, connect, saved networks)
- Built-in apps (Timer with ringing alert, and others)
- Power management: boot chime, power-button long-press → shutdown confirmation → light sleep, wake on button press

### Companion Library

Any receiver you build pairs with AIO-TX using the [`Smart_Connect`](https://github.com/periashutosh-prog/Smart_Connect) library — available for both **ESP32** (FreeRTOS task-based) and **ESP8266** (cooperative `tick()`-based). It handles advertising, handshake, PIN authentication, and telemetry, so you only need to write your own control logic.

### Repository Layout

```
aio-transmitter/
├── AIO_Transmitter.ino               # Main firmware
├── AIO-Transmitter-Documentation.md  # Full technical documentation
├── About.md                          # Feature overview
├── Materials.txt                     # Full Bill of Materials (BOM)
├── partitions.csv                    # Custom ESP32 partition table
├── LICENSE                           # GPLv3 (firmware)
└── Hardware Design Files/            # Schematics, Gerbers, BOM
```

---

## 📊 Comparison with Other Systems

| Feature | AIO-TX | LoRa Modules | nRF24L01 | Generic WiFi Modules | Generic DIY Modules |
|---|---|---|---|---|---|
| Onboard UI (display + buttons) | ✅ Built-in | ❌ None | ❌ None | ❌ Rarely | ❌ None |
| Latency | Very low (ESP-NOW) | High | Low | Moderate-high (full WiFi stack) | Depends on build |
| Range | Moderate (WiFi/ESP-NOW class) | Very long (km-scale) | Short-moderate (~100m open air) | Moderate (AP-dependent) | Depends on build |
| Multi-protocol on one board | ✅ WiFi + BLE + ESP-NOW + CAN (REV3) | ❌ LoRa only | ❌ 2.4GHz proprietary only | ❌ WiFi only | ❌ Whatever you wire |
| Pairing/Security | ✅ Smart Connect PIN pairing | Manual/none | Manual/none | Varies | Manual/none |
| Out-of-the-box receiver support | ✅ `Smart_Connect` library | ❌ Build your own stack | ❌ Build your own stack | ❌ Build your own stack | ❌ Build your own stack |
| Setup effort | Low (preset profiles) | High | High | Moderate | Very high |
| Best for | General-purpose control (robotics, RC, IoT) | Long-range, low-bandwidth telemetry | Cheap, simple point-to-point links | Internet-connected devices | Fully custom one-off builds |

AIO-TX isn't trying to out-perform any single radio technology at its own specialty — a dedicated LoRa link will always out-range it, and raw nRF24L01 will always be cheaper. What AIO-TX offers instead is a **complete, ready-to-use platform**: a physical UI, dual pairing modes, and a companion receiver library, so you're not rebuilding the transmitter side from scratch every time you start a new project.

---

## 📄 License & Disclaimer

**AIO-TX Firmware and Hardware** are both licensed under the **GNU General Public License v3 (GPLv3)**.

> ⚠️ **Strictly NO COMMERCIALIZATION TOLERATED.** This project is shared for personal, educational, and non-commercial use only.

### Disclaimer

This project is provided as-is for educational and experimental purposes.

- ⚠️ Not certified for production or commercial use
- ⚠️ No warranties or guarantees provided
- ⚠️ User assumes all risks associated with building and operating this device
- ⚠️ Always follow local electrical and RF/radio regulations in your region

---

## 📚 Documentation, Contributing, Contact & Acknowledgments

### Documentation
- Website: [aio-transmitter.vercel.app](https://aio-transmitter.vercel.app)
- Schematics, Gerbers & BOM: available in `Hardware Design Files/`
- 3D Print Files: available in `3D Print Files/`

### Contributing
Found a bug, have a feature idea, or want to contribute? Open an issue or pull request on GitHub:
[github.com/periashutosh-prog/AIO-Transmitter/issues](https://github.com/periashutosh-prog/AIO-Transmitter/issues)

### Contact
- GitHub: [github.com/periashutosh-prog/AIO-Transmitter](https://github.com/periashutosh-prog/AIO-Transmitter)
- Website: [aio-transmitter.vercel.app](https://aio-transmitter.vercel.app)

### Related Projects
- [StrawberryOS](https://github.com/periashutosh-prog/StrawberryOS)
- [Smart_Connect Library](https://github.com/periashutosh-prog/Smart_Connect)

### Acknowledgments
Built as part of the StrawberryOS ecosystem, with thanks to the open-source Arduino/ESP32 community whose libraries (Adafruit GFX, Adafruit SSD1306, Adafruit MCP23017) made this project possible.

---

*This documentation is a work in progress and will be updated as REV 1.0 development continues and REV 2.0 planning begins.*

Updates can be followed here: [aio-transmitter.vercel.app](https://aio-transmitter.vercel.app/)

![AIO-Transmitter](https://image.easyeda.com/oshwhub/pullImage/4c9f6af460d84e0d8fe04d1bb8e24602.jpg)

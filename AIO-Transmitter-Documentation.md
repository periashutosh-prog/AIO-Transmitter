# AIO-Transmitter Documentation

## What Exactly is AIO-Transmitter?

AIO Transmitter, also known as AIO-TX, is a wireless transmitter platform. AIO stands for **All In One**, which describes the core philosophy of the project: although it functions as a transmitter, it is capable of far more than a typical single-protocol transmitter.

AIO-TX is powered by the **ESP32-C6**, a chip well known for its strength in wireless communication. It supports three wireless protocols and one wired protocol:

- **WiFi** (WiFi 6) — for internet connectivity, security, and high reliability
- **Bluetooth Low Energy (BLE)** — for connecting directly to smart devices and phones
- **ESP-NOW** — for ultra-low-latency, peer-to-peer communication
- **Controlled Area Network (CAN)** — a wired, industrial-grade protocol (REV 3.0 exclusive)

This breadth means AIO-TX is designed to fit almost any wireless project. Whether you're building a low-latency RC car, a robot that demands high security, or simply want a transmitter that connects seamlessly to your phone, AIO Transmitter is built to handle it.

The platform is intentionally designed with two types of users in mind. Senior developers and experienced makers have the freedom to tailor the firmware and hardware to their exact needs, since both are fully open source. At the same time, beginners can start using AIO-TX almost immediately thanks to preset profiles for each protocol, meaning no deep wireless protocol knowledge is required to get started.

## Introduction to the Idea

The idea behind AIO-TX came from wanting something more than just another remote controller. Most transmitters are built to do one job and do it well, but they stop there. AIO-TX was designed to break out of that single-function mold and become a modular system that adapts to whatever you're building, whether that's controlling a robot, running an RC setup, acting as wireless gaming input, or reading data from sensors.

What really sets the project apart is how it brings ESP-NOW, BLE, and WiFi together on one board alongside modular hardware expansion. That combination means AIO-TX isn't limited to just talking to one device, it can handle direct one-to-one control just as easily as distributed communication across multiple nodes.

The onboard OLED display plays a big part in making this practical day-to-day, since it lets you configure settings and switch modes in real time without needing a laptop or any external software running alongside it. Looking ahead, the firmware is being built with room to grow, including structured control profiles and an early concept of OS-like behavior through StrawberryOS integration.

If you want to dig deeper into StrawberryOS, you can check it out here:
- [StrawberryOS GitHub Repository](https://github.com/periashutosh-prog/StrawberryOS)
- [StrawberryWatch on OSHWLab](https://oshwlab.com/periashutosh/quantumwatch)

And for everything related to the current project, the official page is here:
[AIO-Transmitter Website](https://aio-transmitter.vercel.app)

## How to Make AIO-Transmitter (REV 1.0)

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

*This documentation is a work in progress and will be updated as REV 1.0 development continues and REV 2.0 planning begins.*

# AIO-Transmitter UI Logic For Claude

### 

* #### Screen Definition



Page 1 --> Boot Screen

Page 2 --> Option Screen

Page 3 --> Transmitter

Page 4 --> Scanning

Page 5 --> Search Results

Page 6 --> Pairing Page

Page 7 --> Connected!

Page 8 --> Status

Page 9 --> Control Panel

Page 10 --> Telemetry

Page 11 --> Terminate Session

Page 12 --> Terminating Session

Page 13 --> Applications

Page 14 --> Connection Failed



* #### Button and Pin Definition

\*Bold indicates that the pin is directly wired to ESP32 GPIO.



ANALOG\_1\_UP GPB3

ANALOG\_1\_DOWN GPA7



DPAD\_1\_UP GPA0

DPAD\_1\_DOWN GPA4

DPAD\_1\_LEFT GPB6
DPAD\_1\_RIGHT GPA1

DPAD\_1\_CENTER GPA2



DPAD\_2\_UP GPB0

DPAD\_2\_DOWN GPB5

DPAD\_2\_LEFT GPB7
DPAD\_2\_RIGHT GPB1

DPAD\_2\_CENTER GPB2



ANALOG\_2\_UP GPA6

ANALOG\_2\_DOWN GPB4



SYS\_LEFT GPA5
**SYS\_CENTRE D3 OF XIAO C6**
SYS\_RIGHT GPA3



D0 --> BATTERY VOLTAGE DIVIDER

D1 --> 5V USB Sense

D2 --> Buzzer

D4 --> SDA

D5 --> SCL

D7 --> INTA (MCP23017 Interrupts)

D8 --> INTB (MCP23017 Interrupts)

##### Navigation:

Navigation in the User Interface is based on the left DPAD (DPAD\_1) and should use a focus based system, please refer to StrawberryOS for a deeper understanding

* #### Screen Logic

At the start of the void setup() quickly initialise the I2C and the display and show the boot screen, after that initialise the MCP23017 and other accessories.
After booting, clear the boot screen and display the Page 2 which has 3 options for the user to choose Transmitter, Applications and Settings. And there will be a top status bar for WiFi Bluetooth and ESPNOW (E) at the right of the status bar  there will be a battery icon which is based on the ADC Reading of the D0 pin. I want it to average the ADC reading 1000 times and then the processed output 10 times to give the perfect reading. WiFi and Bluetooth and some part of ESP\_NOW will be managed by the Radio app in the Applications screen (Check StrawberryOS for reference on Radio Application Note: UI in the AIO Transmitter will differ from StrawberryOS). In the Page 2 upon clicking the Applications, the Page 2 should clear and it should open the Applications Page. But if Transmitter is clicked open the Transmitter page which again has 3 options, (including the back button on the top right <--) (Back button in this page should take you back to Page 2) ESP\_NOW and SMART\_CONNECT are two linked but different protocols. ESP\_NOW especially in my case is truly connection free. First, let me explain for the ESP\_NOW communication protocol. After user presses the ESP\_NOW take him to the Scanning page in which the bottom loader should rotate in loops. The receiver will advertise his name with a struct with 3 things, an int, and two sentences. int 1 or 2 is for the ESP\_NOW or the Smart\_Connect. 1 is ESP\_NOW and 2 is Smart\_Connect. AIO-Transmitter reads all the advertised packets (mostly unique ones not try to read duplicate packets and stuck in an infinite loop). In the advertised packets if the int matches the user preference, in this case ESP\_NOW store it temporarily and scan the air till 6 seconds or all the packets are scanned. Now with the scan results go to the Search Results page in which show the Sentence one which is it's MAC Adress and sentence 2 which is receiver's short name. Back button in this page should take you to Page 2. In the top right corner, E is for ESP\_NOW and S is for Smart Connect. Now, in the Search Results page, let user go through the results in the focus navigation manner and if too many results are there use a scrolling algorithm to scroll through the results. When the user selects his preference take him to the Pairing Page. In the Pairing page back button takes you to the Scanning page and re-scan again and everywhere I want the Focus Nav to apply for the <-- button too. Anyways for the pairing logic for ESP\_NOW first the Transmitter sends a packet about itself like the mac address and its name AIO-Transmitter and then the receiver sends an acknowledgement, if no ACK then re-transmit till 3 times then throw an error (Page 14) for 5 seconds then come back to the Page 2. If ACK is received then wait till 10 seconds for the Receiver to send a Connection Ok! kinda packet (not exactly connection ok!, but you handle the machine language). Then take user to Connected! and every packet should have an int at top even if its receiver or transmitter so both devices can easily identify what the other device trying to tell, like lets say Transmitter sent the Connection?? packet and Receiver parses that expecting its the Command packet that shouldn't be the case. So Connection?? has int 1 (example) then the receiver reads that and processes and sends Connection Ok! (ID 1 example dont use same until I approve in the implementation plan) then after this Connection Ok! Transmitter uses ID xyz and transmits the commands. 


Wait in Connected! for 5 seconds then head to control panel. Here is where the actual transmission starts, under the transmission ID int, send an extremely light packet that contains bit values of the 10 DPAD buttons and 2 analog values 0-100. I did not develop the UI for settings yet, that is gonna have the frequency of transmission 10Hz 20Hz or extremely fast 50Hz. 

Now receiver should immediately send a telemetry packet after receiving the command packet. The ACK will be used as Raw Ping and the Telemetry Packet as normal Ping. To switch btwn the screens in the active transmitter mode we use the SYS LEFT and SYS RIGHT, default is Control Panel, If i click SYS Left I go to Status, if I click right I go to transmitter then if i click right I go to Telemetry Page. In this the telemetry packet gets parsed and the actual content the actual string gets displayed here. To terminate the session ie. stop the transmitter mode and return to page 2, I gotta long press the SYS\_CENTRE then it will show Terminate Session page then if yes clicked show the Terminating Session. In that send a packet to the receiver with all values set to default/0 and then send a closing packet and just end the transmitter mode and go back to page 2. If no clicked just resume normally. And one important logic part, if I am in Transmitter mode and in some other page like Status it should still transmit based on the inputs I give. 



So, Thats it for now, I will update this markdown file when new features are developed. 



Last Updated on: 03:05 14-06-2025. 

---

# ESP_NOW Transmitter Protocol (finalized 2026-06-27)

## Locked-in decisions

- **TX builds both ends.** Receiver firmware will be a `Smart_Connect.h` library, built later. For now: transmitter only.
- **Entering ESP_NOW or SMART_CONNECT forces WiFi STA OFF first** (so ESP-NOW is free to use any channel; avoids the shared-channel constraint with WiFi UDP mode).
- **Every packet's first field is `int pkt_id`** so each side knows which struct to parse it as. ESP-NOW transmits a raw byte buffer (<=250 B); we cast these structs to/from it. The sender's MAC arrives free in the receive callback, but advertising/hello packets also carry it in-payload for self-containment.
- **ESP_NOW and SMART_CONNECT have entirely separate struct sets.** The block below is the ESP_NOW set. Smart Connect's structs are TBD (defined when its flow is built).

## ESP_NOW packet IDs

| ID | Name | Direction | Meaning |
|----|------|-----------|---------|
| 10 | PKT_ADVERTISE | RX -> air (2Hz) | Receiver advertising {mode, mac, name} |
| 20 | PKT_HELLO | TX -> RX | "Connect??" — TX introduces itself |
| 21 | PKT_HELLO_ACK | RX -> TX | Receiver acknowledges the hello |
| 22 | PKT_CONN_OK | RX -> TX, then TX -> RX | RX: "Connection Ok!"; TX echoes 22 back to confirm receipt |
| 30 | PKT_CMD | TX -> RX | Live control frame (buttons + 2 analog) |
| 40 | PKT_TELEMETRY | RX -> TX | Telemetry string reply |
| 99 | PKT_CLOSE | TX -> RX | Session teardown |

`mode` field: 1 = ESP_NOW, 2 = SMART_CONNECT.

## ESP_NOW structs (ESP32, authoritative)

```cpp
#define PKT_ADVERTISE   10
#define PKT_HELLO       20
#define PKT_HELLO_ACK   21
#define PKT_CONN_OK     22
#define PKT_CMD         30
#define PKT_TELEMETRY   40
#define PKT_CLOSE       99

struct __attribute__((packed)) AdvertisePacket {
  int  pkt_id;       // = PKT_ADVERTISE (10)
  int  mode;         // 1 = ESP_NOW, 2 = SMART_CONNECT
  char mac[18];      // "AA:BB:CC:DD:EE:FF\0"
  char name[20];     // "RC-Car\0"
};

struct __attribute__((packed)) HelloPacket {
  int  pkt_id;       // = PKT_HELLO (20)
  char mac[18];      // TX own MAC
  char name[20];     // "AIO-Transmitter\0"
};

struct __attribute__((packed)) AckPacket {
  int  pkt_id;       // PKT_HELLO_ACK (21) or PKT_CONN_OK (22)
};

struct __attribute__((packed)) CommandPacket {
  int      pkt_id;   // = PKT_CMD (30)
  uint16_t buttons;  // bits 0-4: DPAD1, bits 5-9: DPAD2
  byte     analog1;  // 0-100
  byte     analog2;  // 0-100
};

struct __attribute__((packed)) TelemetryPacket {
  int  pkt_id;       // = PKT_TELEMETRY (40)
  char data[64];     // "BAT:87% SPD:42 DIR:FWD\0"
};

struct __attribute__((packed)) ClosePacket {
  int  pkt_id;       // = PKT_CLOSE (99)
};
```

## ESP_NOW flow (transmitter side)

1. **Enter** (ESP_NOW tile): WiFi STA off -> `esp_now_init` -> register broadcast peer -> Scanning page.
2. **Scanning:** listen for `AdvertisePacket` with `mode == 1`. Dedup by MAC (receiver repeats at 2Hz). Collect unique advertisers for a >=3s window. (Note: "until all packets read" isn't deterministic over the air, so 3s is the collection window.)
3. **Results:** real list of discovered `{mac, name}`, focus-nav + scroll. `<--` -> Page 2.
4. **Pairing** (after select): handshake below. `<--` -> back to Scanning (re-scan).
5. **Handshake:**
   - TX -> `HelloPacket(20)`. Wait 3s for `HELLO_ACK(21)`.
   - No ack in 3s -> retransmit. **Max 3 attempts.** All fail -> **Connect Failed** page (5s) -> Page 2.
   - On `HELLO_ACK(21)`: wait up to **10s** for `CONN_OK(22)`. Timeout -> Connect Failed -> Page 2.
   - On `CONN_OK(22)`: TX -> `AckPacket(22)` back to RX (confirm). -> **Connected!** (5s) -> Control Panel.
6. **Active session:** send `CommandPacket(30)` to paired MAC at `espNowFreqHz` (Settings). RX replies `TelemetryPacket(40)`.
   - **Raw Ping** = ESP-NOW send-callback ACK latency. **Ping** = command->telemetry round-trip.
   - SYS_LEFT/RIGHT switch Status / Control / Telemetry. Transmission continues on every page.
7. **Terminate:** long-press SYS_CENTRE -> Terminate? -> YES sends a zeroed `CommandPacket` + `ClosePacket(99)`, tears down ESP-NOW -> Page 2.

## SMART_CONNECT packet IDs

| ID | Name | Direction | Meaning |
|----|------|-----------|---------|
| 50 | SC_PKT_ADVERTISE | RX -> air (2Hz) | Receiver advertising {mode=2, mac, name} |
| 51 | SC_PKT_CONNECT_REQ | TX -> RX | "Connect??" — TX introduces itself |
| 52 | SC_PKT_PARTIAL_OK | RX -> TX | "Enter PIN" — RX generates & shows 4-digit code |
| 53 | SC_PKT_PIN_SUBMIT | TX -> RX | TX sends user-entered 4-digit PIN |
| 54 | SC_PKT_CONNECT_OK | RX -> TX | PIN matched, connection established |
| 55 | SC_PKT_CONNECT_FAIL | RX -> TX | Wrong PIN |
| 30 | PKT_CMD | TX -> RX | Reused — same CommandPacket as ESP_NOW |
| 40 | PKT_TELEMETRY | RX -> TX | Reused — same TelemetryPacket as ESP_NOW |
| 99 | PKT_CLOSE | TX -> RX | Reused — same ClosePacket as ESP_NOW |

## SMART_CONNECT structs (ESP32, authoritative)

```cpp
#define SC_PKT_ADVERTISE    50
#define SC_PKT_CONNECT_REQ  51
#define SC_PKT_PARTIAL_OK   52
#define SC_PKT_PIN_SUBMIT   53
#define SC_PKT_CONNECT_OK   54
#define SC_PKT_CONNECT_FAIL 55

struct __attribute__((packed)) ScAdvertisePacket {
  int  pkt_id;       // = SC_PKT_ADVERTISE (50)
  int  mode;         // always 2 (SMART_CONNECT)
  char mac[18];      // "AA:BB:CC:DD:EE:FF\0"
  char name[20];     // "RC-Car\0"
};

struct __attribute__((packed)) ScConnectReqPacket {
  int  pkt_id;       // = SC_PKT_CONNECT_REQ (51)
  char mac[18];      // TX own MAC
  char name[20];     // "AIO-Transmitter\0"
};

struct __attribute__((packed)) ScPartialOkPacket {
  int  pkt_id;       // = SC_PKT_PARTIAL_OK (52)
};

struct __attribute__((packed)) ScPinSubmitPacket {
  int  pkt_id;       // = SC_PKT_PIN_SUBMIT (53)
  int  pin;          // 4-digit code 0000-9999
};

struct __attribute__((packed)) ScConnectOkPacket {
  int  pkt_id;       // = SC_PKT_CONNECT_OK (54)
};

struct __attribute__((packed)) ScConnectFailPacket {
  int  pkt_id;       // = SC_PKT_CONNECT_FAIL (55)
};
```

## SMART_CONNECT flow (transmitter side)

1. **Enter** (SMART CONNECT tile): WiFi STA off -> `esp_now_init` -> register broadcast peer -> Scanning page.
2. **Scanning:** listen for `ScAdvertisePacket` with `mode == 2`. Dedup by MAC. Collect for >=3s.
3. **Results:** list of discovered `{mac, name}`, focus-nav + scroll. `<--` -> Page 2.
4. **Connecting (1st):** TX -> `ScConnectReqPacket(51)`. Wait for `SC_PARTIAL_OK(52)`. Timeout -> Failed.
5. **Enter PIN:** RX has generated a 4-digit code and is showing it on its own OLED. TX shows 4-digit entry screen.
6. **PIN submit:** User enters PIN -> TX -> `ScPinSubmitPacket(53)`.
7. **Authenticating (2nd):** Wait for response:
   - `SC_PKT_CONNECT_OK(54)` -> **Connected!** -> Control Panel.
   - `SC_PKT_CONNECT_FAIL(55)` -> **Failed** -> Page 2.
8. **Active session:** same as ESP_NOW — `CommandPacket(30)` at configured Hz, `TelemetryPacket(40)` replies.
9. **Terminate:** same as ESP_NOW — `ClosePacket(99)`.

## Resolved ambiguities (defaults — change if wrong)

- Final TX->RX confirmation after CONN_OK reuses `AckPacket{pkt_id = 22}` (no new ID), disambiguated by sender MAC.
- CONN_OK wait timeout = 10s (no retransmit; RX is the sender).
- Scan window = 3s, dedup by MAC.

Last Updated on: 2026-06-27 (Smart Connect protocol spec added).












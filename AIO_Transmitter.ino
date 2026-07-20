// =============================================================================
// AIO-Transmitter — StrawberryOS Integration
// ESP32-C6 based wireless transmitter with embedded StrawberryOS apps
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MCP23X17.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "time.h"
#include <LittleFS.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

// =============================================================================
// PACKET IDS — ESP_NOW
// =============================================================================
#define PKT_ADVERTISE   10
#define PKT_HELLO       20
#define PKT_HELLO_ACK   21
#define PKT_CONN_OK     22
#define PKT_CMD         30
#define PKT_TELEMETRY   40
#define PKT_CLOSE       99

// =============================================================================
// PACKET IDS — SMART CONNECT
// =============================================================================
#define SC_PKT_ADVERTISE    50
#define SC_PKT_CONNECT_REQ  51
#define SC_PKT_PARTIAL_OK   52
#define SC_PKT_PIN_SUBMIT   53
#define SC_PKT_CONNECT_OK   54
#define SC_PKT_CONNECT_FAIL 55

// =============================================================================
// STRUCTS — ESP_NOW
// =============================================================================
struct __attribute__((packed)) AdvertisePacket {
  int  pkt_id;       // PKT_ADVERTISE (10)
  int  mode;         // 1 = ESP_NOW
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) HelloPacket {
  int  pkt_id;       // PKT_HELLO (20)
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) AckPacket {
  int  pkt_id;       // PKT_HELLO_ACK (21) or PKT_CONN_OK (22)
};

struct __attribute__((packed)) CommandPacket {
  int      pkt_id;   // PKT_CMD (30)
  uint16_t buttons;  // bits 0-4: DPAD1, bits 5-9: DPAD2
  byte     analog1;  // 0-100
  byte     analog2;  // 0-100
};

struct __attribute__((packed)) TelemetryPacket {
  int  pkt_id;       // PKT_TELEMETRY (40)
  char data[64];
};

struct __attribute__((packed)) ClosePacket {
  int  pkt_id;       // PKT_CLOSE (99)
};

// =============================================================================
// STRUCTS — SMART CONNECT
// =============================================================================
struct __attribute__((packed)) ScAdvertisePacket {
  int  pkt_id;       // SC_PKT_ADVERTISE (50)
  int  mode;         // 2 = SMART_CONNECT
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) ScConnectReqPacket {
  int  pkt_id;       // SC_PKT_CONNECT_REQ (51)
  char mac[18];
  char name[20];
};

struct __attribute__((packed)) ScPartialOkPacket {
  int  pkt_id;       // SC_PKT_PARTIAL_OK (52)
};

struct __attribute__((packed)) ScPinSubmitPacket {
  int  pkt_id;       // SC_PKT_PIN_SUBMIT (53)
  int  pin;          // 4-digit code 0000-9999
};

struct __attribute__((packed)) ScConnectOkPacket {
  int  pkt_id;       // SC_PKT_CONNECT_OK (54)
};

struct __attribute__((packed)) ScConnectFailPacket {
  int  pkt_id;       // SC_PKT_CONNECT_FAIL (55)
};

// -----------------------------------------------------------------------------
// STATUS BAR ICON BITMAPS (8x8 pixels, stored in PROGMEM)
// -----------------------------------------------------------------------------
// WiFi — expanding fan, uses full 8px width so it reads wide, not elongated
//   ........
//   .XXXXXX.   outer arc
//   X......X
//   ..XXXX..   middle arc
//   .X....X.
//   ...XX...   inner arc
//   ........
//   ...XX...   source dot
static const uint8_t PROGMEM wifi_bmp[] = {
  0x00, 0x7E, 0x81, 0x3C, 0x42, 0x18, 0x00, 0x18
};
// Bluetooth — centered rune with continuous vertical spine + crossing diagonals
//   ...X....
//   .X.XX...
//   ..XX.X..
//   ...XX...
//   ..XX.X..
//   .X.XX...
//   ...X....
//   ........
static const uint8_t PROGMEM bt_bmp[] = {
  0x10, 0x58, 0x34, 0x18, 0x34, 0x58, 0x10, 0x00
};

// -----------------------------------------------------------------------------
// DISPLAY CONSTANTS
// -----------------------------------------------------------------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// -----------------------------------------------------------------------------
// MCP23017 CONFIGURATION
// -----------------------------------------------------------------------------
Adafruit_MCP23X17 mcp;

// MCP23017 Pin Mappings — Port A
#define DPAD_1_UP       0   // GPA0
#define DPAD_1_RIGHT    1   // GPA1
#define DPAD_1_CENTER   2   // GPA2
#define SYS_RIGHT       3   // GPA3
#define DPAD_1_DOWN     4   // GPA4
#define SYS_LEFT        5   // GPA5
#define ANALOG_2_UP     6   // GPA6
#define ANALOG_1_DOWN   7   // GPA7

// MCP23017 Pin Mappings — Port B
#define DPAD_2_UP       8   // GPB0
#define DPAD_2_RIGHT    9   // GPB1
#define DPAD_2_CENTER   10  // GPB2
#define ANALOG_1_UP     11  // GPB3
#define ANALOG_2_DOWN   12  // GPB4
#define DPAD_2_DOWN     13  // GPB5
#define DPAD_1_LEFT     14  // GPB6
#define DPAD_2_LEFT     15  // GPB7

// -----------------------------------------------------------------------------
// ESP32-C6 NATIVE PIN MAPPINGS
// -----------------------------------------------------------------------------
#define PIN_BATTERY     0
#define PIN_USB_SENSE   1
#define PIN_BUZZER      2
#define SYS_CENTRE      3
#define PIN_SDA         22   // XIAO ESP32-C6 silkscreen D4/SDA
#define PIN_SCL         23   // XIAO ESP32-C6 silkscreen D5/SCL
#define PIN_INTA        17
#define PIN_INTB        19
#define PIN_POWER       21   // Long-press (>3s) from home/apps menu = shutdown confirm; also deep-sleep wake source

// =============================================================================
// SCREEN STATE ENUM
// Apps entered from the APPS submenu only; Transmitter/Settings kept separate
// =============================================================================
enum ScreenState {
  // AIO Main Screens
  PAGE_BOOT,
  PAGE_OPTIONS,       // Main hub: TRANSMITTER | APPS | SETTINGS

  // Apps submenu (entered when APPS is selected)
  SCREEN_APPS_MENU,
  SCREEN_STOPWATCH,
  SCREEN_TIMER,
  SCREEN_TIMER_ALERT,

  // WiFi app (entered from Apps menu)
  SCREEN_WIFI_CONFIRM,
  SCREEN_WIFI_SCANNING,
  SCREEN_WIFI_RESULTS,
  SCREEN_WIFI_PASSWORD,
  SCREEN_WIFI_KEYBOARD,
  SCREEN_WIFI_HOME,
  SCREEN_WIFI_TOGGLE_CONFIRM,
  SCREEN_WIFI_NTP,
  SCREEN_WIFI_AUTO_CONFIRM,
  SCREEN_WIFI_FORGET_CONFIRM,

  // Calculator
  SCREEN_CALCULATOR,
  SCREEN_CALCULATOR_SCI,

  // Settings (entered from PAGE_OPTIONS)
  SCREEN_SETTINGS_MENU,
  SCREEN_SETTINGS_FREQ,
  SCREEN_SETTINGS_MANAGE_WIFI,

  // Transmitter Screens
  SCREEN_TX_MENU,
  // WiFi UDP transmit flow
  SCREEN_TX_UDP_NOWIFI,
  SCREEN_TX_UDP_MODE,
  SCREEN_TX_UDP_IP,
  SCREEN_TX_UDP_CONTROL,
  SCREEN_TX_SCANNING,
  SCREEN_TX_RESULTS,
  SCREEN_TX_PAIRING,
  SCREEN_TX_CONNECTED,
  // Smart Connect flow (dummy UI)
  SCREEN_SC_SCANNING,
  SCREEN_SC_RESULTS,
  SCREEN_SC_CONNECTING1,
  SCREEN_SC_PIN_ENTRY,
  SCREEN_SC_CONNECTING2,
  SCREEN_SC_CONNECTED,
  SCREEN_TX_STATUS,
  SCREEN_TX_CONTROL,
  SCREEN_TX_TELEMETRY,
  SCREEN_TX_TERMINATE,
  SCREEN_TX_TERMINATING,
  SCREEN_TX_FAILED,
  SCREEN_TX_RX_LOST,
  SCREEN_TX_WIFI_DISABLE,

  // Power button long-press (from PAGE_OPTIONS / SCREEN_APPS_MENU only)
  SCREEN_SHUTDOWN_CONFIRM
};

ScreenState currentScreen = PAGE_BOOT;

// =============================================================================
// GLOBAL STATE — AIO HUB
// =============================================================================
unsigned long bootStartTime  = 0;
unsigned long lastActivityTime = 0;
bool isDisplaySleep = false;
int page2Focus = 0; // 0=Transmitter, 1=Applications, 2=Settings

// Status bar icons — hidden (false) until Bluetooth / ESP-NOW are implemented
bool bleStatusActive    = false;
bool espNowStatusActive = false;

int simulated_analog_1 = 0;
int simulated_analog_2 = 0;

// =============================================================================
// GLOBAL STATE — TRANSMITTER
// =============================================================================
// 2x2 menu grid:  0=ESP_NOW  1=SMART_CONNECT  2=WiFi UDP  3=BLE   (-1=Back)
int txMenuFocus = 0;
int txMenuScroll = 0;       // internal vertical scroll px (animates toward target)
unsigned long txScanStart = 0;
int txResultScroll = 0;
int txResultIndex = 0;
String txTargetName = "";
unsigned long txPairStart = 0;
unsigned long txConnectedStart = 0;
int txControlPage = 1; // 0=Status, 1=Control, 2=Telemetry
int txStatusScroll = 0;
int txTerminateFocus = 1; // 0=YES, 1=NO
unsigned long txTerminatingStart = 0;

// ---- Smart Connect UI ----
unsigned long scConnectStart = 0;
unsigned long scAuthStart = 0;
unsigned long scConnectedStart = 0;
int scPin[4] = {0, 0, 0, 0};
int scPinCursor = 0; // -1=Back, 0-3=digit slots, 4=OK

// ---- ESP-NOW / Smart Connect real protocol ----
static const uint8_t ESPNOW_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
bool espNowInitialized = false;
int txActiveMode = 0; // 1=ESP_NOW, 2=SMART_CONNECT
uint8_t txOwnMacBytes[6] = {0};
char txOwnMacStr[18] = "";
uint8_t txTargetMacBytes[6] = {0};
bool txPeerRegistered = false;

// Scan results
#define MAX_SCAN_RESULTS 10
struct ScanResult {
  uint8_t macBytes[6];
  char mac[18];
  char name[20];
};
ScanResult scanResults[MAX_SCAN_RESULTS];
volatile int scanResultCount = 0;
bool txScanning = false;

// Handshake (ESP_NOW)
int txHandshakeStep = 0;   // 0=send hello, 1=wait ack, 2=wait connok
int txHandshakeAttempt = 0;
unsigned long txHandshakeTime = 0;
volatile bool txAckReceived = false;
volatile bool txConnOkReceived = false;

// Handshake (Smart Connect)
volatile bool txPartialOkReceived = false;
volatile int txScPinResult = 0; // 0=waiting, 54=ok, 55=fail
unsigned long scLastReqResend = 0;  // ConnectReq retransmit timer (CONNECTING1)
unsigned long scLastPinResend = 0;  // PinSubmit retransmit timer (CONNECTING2)

// "Disable WiFi?" gate before entering a transmit mode
int txWifiDisableFocus = 0;   // 0 = Yes, 1 = No
int txPendingMode = 0;        // 1 = ESP_NOW, 2 = SMART_CONNECT (mode to start after Yes)

// Power button (PIN_POWER) long-press -> shutdown confirm
unsigned long powerBtnPressStart = 0;   // 0 = not currently held
bool powerBtnLongPressHandled = false;  // gates re-trigger while still held past 3s
bool powerBtnWaitingRelease = false;    // true right after a light-sleep wake — the
                                         // wake press must be fully released before
                                         // it can start counting toward a fresh 3s
                                         // long-press again (otherwise a wake press
                                         // held through boot re-triggers shutdown
                                         // the instant PAGE_OPTIONS is reached)
int shutdownConfirmFocus = 0;           // 0 = Yes, 1 = No
ScreenState shutdownReturnScreen = PAGE_OPTIONS; // where "No" returns to

// Active session
bool txSessionActive = false;
unsigned long lastTxCmdSend = 0;

// Telemetry from receiver
char txTelemetryStr[64] = "NO DATA";
volatile bool txNewTelemetry = false;

// Ping / stats
volatile unsigned long txRawPingUs = 0;
unsigned long txCmdSendTimestamp = 0;
unsigned long txCmdSendTimestampUs = 0;
volatile unsigned long txLastTelemetryMs = 0; // last time any telemetry packet arrived
unsigned long txRxLostStart = 0;              // when the "RX Disconnected" page was entered
volatile unsigned long txTelemetryPingMs = 0;
unsigned long txSessionStartMs = 0;
volatile unsigned long txPktSentCount = 0;
volatile unsigned long txPktAckedCount = 0;
volatile int txLastRssi = 0;

// Connection failed
unsigned long txFailedStart = 0;
String txFailReason = "";

// ---- WiFi UDP transmit ----
WiFiUDP udp;
const uint16_t UDP_TX_PORT = 5000;
int  txUdpModeFocus = 0;    // 0=Broadcast, 1=Unicast (-1=Back)
int  txUdpIpCursor  = 1;    // -1=Back, 0=Keyboard, 1=Start
String udpTargetIP  = "192.168.1.100";
bool udpBroadcast   = true;
bool udpSessionActive = false;
unsigned long udpNoWifiStart = 0;
unsigned long lastUdpSend = 0;

// =============================================================================
// GLOBAL STATE — SETTINGS
// =============================================================================
const int NUM_SETTINGS_ITEMS = 5;
const char* settingsItems[NUM_SETTINGS_ITEMS] = {
  "WiFi UDP Freq", "ESP NOW Freq", "S-CONNECT Freq", "Device Name", "Manage WiFi"
};
int settingsMenuIndex  = 0;
int settingsMenuScroll = 0;

// Name advertised to receivers over ESP_NOW/Smart Connect (Hello/ConnectReq
// packets) — was hardcoded "AIO-Transmitter"; now user-editable via Settings.
String txDeviceName = "AIO-Transmitter";

int wifiUdpFreqHz   = 20;
int espNowFreqHz    = 20;
int sconnectFreqHz  = 20;

const int NUM_FREQ_OPTIONS = 3;
const int freqOptions[NUM_FREQ_OPTIONS] = {10, 20, 50};
int settingsFreqFocus  = 0;  // index into freqOptions
int settingsFreqTarget = 0;  // which settingsItems[] entry is being edited

// ---- Manage WiFi ----
// When ON, entering ESP-NOW / Smart Connect silently turns Wi-Fi off (instead of
// prompting via SCREEN_TX_WIFI_DISABLE) and restores it automatically on exit.
// When OFF, the legacy "Disable WiFi?" gate is shown and Wi-Fi stays off.
bool manageWifi = false;
bool wifiWasAutoDisabled = false;   // set when Manage WiFi auto-disabled Wi-Fi for a session
int  manageWifiCursor = -1;         // -1 = Back, 0 = YES, 1 = NO (current selection, set via L/R on the button row)
// Line-by-line scroll focus: -1 = Back (top), 0..N-1 = each wrapped text line,
// N = the YES/NO button row (the last stop). Up/Down move exactly one stop —
// i.e. one text line — at a time; the view scrolls to keep the focus visible.
int  manageWifiFocusLine = -1;
int  manageWifiScroll = 0;          // animated content scroll offset (px)
const char* MANAGE_WIFI_TEXT =
  "ESP-NOW/S-Connect and Wi-Fi can't be used at the same time. "
  "Manage Wi-Fi disables the Wi-Fi automatically when you start "
  "ESP-NOW/S-Connect and Wi-Fi is re-enabled when you exit";

// =============================================================================
// GLOBAL STATE — ANIMATION
// =============================================================================
bool isAnimating  = false;
int  animOffsetY  = 0;
int  animTargetY  = 0;
int  animOffsetX  = 0;
int  animTargetX  = 0;
ScreenState animNextScreen = PAGE_OPTIONS;

void startAnimation(ScreenState next, int targetY, int targetX = 0);

// =============================================================================
// GLOBAL STATE — BUTTON SYSTEM (MCP23017-based edge detection)
// =============================================================================
// buttonJustPressed[0]=UP  [1]=DOWN  [2]=LEFT  [3]=RIGHT  [4]=CENTER
bool buttonStates[5]      = {false,false,false,false,false};
bool lastButtonStates[5]  = {false,false,false,false,false};
bool buttonJustPressed[5] = {false,false,false,false,false};

// DPAD_2 raw state cache, read once per loop() — avoids hitting I2C on every render
// frame from inside drawTxControl(), which was causing animation stutter.
// dpad2States[0]=UP [1]=DOWN [2]=LEFT [3]=RIGHT [4]=CENTER
bool dpad2States[5] = {false,false,false,false,false};

// =============================================================================
// GLOBAL STATE — APPS MENU
// =============================================================================
// Apps: 0=Stopwatch, 1=Timer, 2=WiFi, 3=Calculator, 4=Exit to Boot
const int NUM_APP_ITEMS = 5;
const char* appItems[NUM_APP_ITEMS] = {"Stopwatch", "Timer", "WiFi", "Calculator", "Exit to Boot"};
int appMenuIndex = 0;
int appMenuScroll = 0;

// =============================================================================
// GLOBAL STATE — STOPWATCH
// =============================================================================
unsigned long swStartTime   = 0;
unsigned long swElapsedTime = 0;
bool swRunning  = false;
int  swFocus    = 0;

// =============================================================================
// GLOBAL STATE — TIMER
// =============================================================================
enum TimerMode { TM_SETTING, TM_READY, TM_RUNNING, TM_RINGING };
TimerMode tmMode     = TM_SETTING;
int  tmHours         = 0, tmMinutes = 0, tmSeconds = 0;
int  tmActiveUnit    = 2;
int  tmFocus         = 1;
unsigned long tmRemainingMillis   = 0;
unsigned long tmLastTick          = 0;
unsigned long tmSetPressStart     = 0;
bool tmLongPressTriggered         = false;

// Timer ring buzzer — non-blocking trill pattern, stepped from loop() so button
// polling (to dismiss) and animation keep running while it's sounding.
unsigned long tmRingNextStep = 0;
int tmRingPhase = 0;

// =============================================================================
// GLOBAL STATE — KEYBOARD
// =============================================================================
enum KBMode { KB_LOWER, KB_UPPER, KB_SYMBOL };
KBMode currentKBMode = KB_LOWER;
int kbCursorRow = 0, kbCursorCol = 0;
String kbInputBuffer = "";
int kbTextCursor   = 0;
int kbScrollOffset = 0;
ScreenState kbReturnScreen = SCREEN_WIFI_PASSWORD;

// Row 2 cols 7-8 are the cursor-move keys, shown as "<"/">" here. kbSymbol below
// keeps them labeled "CL"/"CR" instead — that grid has literal "<"/">" characters
// elsewhere (row 3), and giving the move-keys the same glyph there would make
// them indistinguishable from the typed symbol. The key handler in the WiFi
// keyboard input block mirrors this: "<"/">" only act as cursor moves when
// currentKBMode != KB_SYMBOL, so the literal "<"/">" character keys always just
// type their character.
const char* kbLower[4][11] = {
  {"q","w","e","r","t","y","u","i","o","p","."},
  {"a","s","d","f","g","h","j","k","l",",","?"},
  {"z","x","c","v","b","n","m","<",">","^","BS"},
  {"1","2","3","4","5","6","7","8","9"," ","EN"}
};
const char* kbUpper[4][11] = {
  {"Q","W","E","R","T","Y","U","I","O","P","."},
  {"A","S","D","F","G","H","J","K","L",",","?"},
  {"Z","X","C","V","B","N","M","<",">","^","BS"},
  {"!","@","#","$","%","^","&","*","("," ","EN"}
};
const char* kbSymbol[4][11] = {
  {"1","2","3","4","5","6","7","8","9","0","~"},
  {"@","$","%","&","*","-","+","=","!","|","\\"},
  {"/","?",";",":","`","(",")","CL","CR","^","BS"},
  {"[","]","{","}","<",">","#","_","."," ","EN"}
};

// =============================================================================
// GLOBAL STATE — WiFi
// =============================================================================
bool wifiEnabled      = false;
bool wifiAutoOn       = false;
int  wifiScanResults  = 0;
int  wifiListIndex    = -1;
int  wifiListScroll   = 0;
String wifiTargetSSID = "";
String wifiPassword   = "";
String wifiStatusMsg  = "";
bool wifiConnecting   = false;
unsigned long wifiScanStart        = 0;
unsigned long wifiConnectStartTime = 0;
int wifiHomeCursor    = -1;
int wifiConfirmCursor = 0;
int wifiToggleCursor  = 0;
int wifiAutoCursor    = 0;
int wifiForgetCursor  = 1;
String wifiForgetTargetSSID = "";
int wifiPasswordCursor = 1;
int wifiRetryCount    = 0;
bool internetOK       = false;
bool pingerStatus[5]  = {false, false, false, false, false};
bool pingerStarted    = false;
unsigned long ntpSyncStart = 0;
unsigned long ntpSyncEnd   = 0;
int  ntpSyncState = 0;

Preferences prefs;
const int MAX_SAVED_NETS = 5;
String savedSSIDs[MAX_SAVED_NETS];
String savedPWDs[MAX_SAVED_NETS];
int savedNetCount = 0;
String lastConnectedSSID = "";

const int MAX_FORBIDDEN_NETS = 5;
String forbiddenSSIDs[MAX_FORBIDDEN_NETS];
int forbiddenCount = 0;

const char* ntpServer         = "pool.ntp.org";
const long  gmtOffset_sec     = 19800;  // IST = UTC+5:30
const int   daylightOffset_sec = 0;

// =============================================================================
// GLOBAL STATE — CALCULATOR
// =============================================================================
int calcCursorRow = 0, calcCursorCol = 0;
String calcInput1 = "";
String calcInput2 = "";
char calcOp       = ' ';
bool calcIsOpSet  = false;
bool calcError    = false;

int calcSciRow = 0, calcSciCol = 0;

const char* sciGrid[3][2] = {
  {"x^2",  "sqrt"},
  {"pi",   "x^3"},
  {"cbrt", "BACK"}
};
const char* calcGrid[5][4] = {
  {"7",   "8",  "9",   "/"},
  {"4",   "5",  "6",   "*"},
  {"1",   "2",  "3",   "-"},
  {"0",   ".",  "+",   "="},
  {"<--", "C",  "DEL", "SCI"}
};

// =============================================================================
// FUNCTION PROTOTYPES
// =============================================================================
void drawBootScreen();
void drawStatusBar(int yOffset);
void drawPage2(int yOffset);
void readButtons();
void initMCP23017();
void printSpacedFakeBold(String text, int startX, int startY, int spacing);

void startAnimation(ScreenState next, int targetY, int targetX);
void updateDisplay();
void drawScreen(ScreenState screen, int yOffset, int xOffset = 0);
void drawHeader(int yOffset, const char* appName = nullptr, bool backFocused = false, bool showBackArrow = true);
void drawAppsMenu(int yOffset);
void drawSettingsMenu(int yOffset);
void drawSettingsFreq(int yOffset);
void drawManageWifi(int yOffset);
int  renderJustifiedText(const String &text, int xLeft, int colW, int yTop, bool draw);
int  manageWifiTotalLines();
void drawStopwatch(int yOffset);
void drawTimer(int yOffset);
void drawTimerAlert(int yOffset);
void drawCalculator(int yOffset);
void drawCalculatorSci(int yOffset);
void drawWifiConfirm(int yOffset);
void drawWifiScanning(int yOffset);
void drawWifiResults(int yOffset);
void drawWifiPassword(int yOffset);
void drawWifiKeyboard();
void drawWifiHome(int yOffset);
void drawWifiToggleConfirm(int yOffset);
void drawWifiAutoConfirm(int yOffset);
void drawWifiForgetConfirm(int yOffset);
void drawWifiNTP(int yOffset);

void drawTxMenu(int yOffset);
void drawTxUdpNoWifi(int yOffset);
void drawTxUdpMode(int yOffset);
void drawTxUdpIp(int yOffset);
void drawTxUdpControl(int yOffset);
void sendUdpControlPacket();
void drawTxScanning(int yOffset);
void drawTxResults(int yOffset);
void drawTxPairing(int yOffset);
void drawTxConnected(int yOffset);
void drawScScanning(int yOffset);
void drawScResults(int yOffset);
void drawScConnecting1(int yOffset);
void drawScPinEntry(int yOffset);
void drawScConnecting2(int yOffset);
void drawScConnected(int yOffset);
void drawTxStatus(int yOffset, int xOffset = 0);
void drawTxControl(int yOffset, int xOffset = 0);
void drawTxTelemetry(int yOffset, int xOffset = 0);
void drawTxTerminate(int yOffset);
void drawTxTerminating(int yOffset);
void drawTxFailed(int yOffset);
void drawTxRxLost(int yOffset);
void drawTxWifiDisable(int yOffset);
void drawShutdownConfirm(int yOffset);
void playBootTone();
void stepTimerRingTone();
void enterLightSleep();
void initEspNow();
void deinitEspNow();
void addTxPeer(const uint8_t* mac);
void removeTxPeer();
void sendTxCommand();
void startTxSession();

void drawBatteryIcon(int x, int y, int fillPercent);
void drawCenteredText(Adafruit_SSD1306 &d, const String &text, int16_t y, uint8_t size = 1, int xOffset = 0);
void drawBoxedCenteredText(Adafruit_SSD1306 &d, const char* text, int x, int y, int w, int h, bool inverted);

void loadCredentials();
void saveCredential(String ssid, String pwd);
void forgetCredential(String ssid);
String findSavedPwd(String ssid);
void loadForbidden();
void loadSettings();
void saveFreqSetting(int target, int hz);
void saveManageWifi(bool on);
void saveForbidden();
void addForbidden(String ssid);
void removeForbidden(String ssid);
bool isForbidden(String ssid);
void syncTimeWithNTP();
void startConnectivityPinger();
void connectivityTask(void *p);
void startWifiBackgroundTask();
void wifiBackgroundTask(void *p);

#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// =============================================================================
// ESP-NOW CALLBACKS & HELPERS
// =============================================================================
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 1)
void onTxEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac = info->src_addr;
  if (info->rx_ctrl) txLastRssi = info->rx_ctrl->rssi;
#else
void onTxEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len < (int)sizeof(int)) return;
  int pkt_id;
  memcpy(&pkt_id, data, sizeof(int));

  // ---- Scanning: collect advertise packets ----
  if (txScanning) {
    if (pkt_id == PKT_ADVERTISE && txActiveMode == 1 && len >= (int)sizeof(AdvertisePacket)) {
      AdvertisePacket adv;
      memcpy(&adv, data, sizeof(adv));
      if (adv.mode != 1) return;
      for (int i = 0; i < scanResultCount; i++) {
        if (memcmp(scanResults[i].macBytes, mac, 6) == 0) return; // dedup
      }
      if (scanResultCount < MAX_SCAN_RESULTS) {
        memcpy(scanResults[scanResultCount].macBytes, mac, 6);
        strncpy(scanResults[scanResultCount].mac, adv.mac, 17);
        scanResults[scanResultCount].mac[17] = '\0';
        strncpy(scanResults[scanResultCount].name, adv.name, 19);
        scanResults[scanResultCount].name[19] = '\0';
        scanResultCount++;
      }
    }
    else if (pkt_id == SC_PKT_ADVERTISE && txActiveMode == 2 && len >= (int)sizeof(ScAdvertisePacket)) {
      ScAdvertisePacket adv;
      memcpy(&adv, data, sizeof(adv));
      if (adv.mode != 2) return;
      for (int i = 0; i < scanResultCount; i++) {
        if (memcmp(scanResults[i].macBytes, mac, 6) == 0) return;
      }
      if (scanResultCount < MAX_SCAN_RESULTS) {
        memcpy(scanResults[scanResultCount].macBytes, mac, 6);
        strncpy(scanResults[scanResultCount].mac, adv.mac, 17);
        scanResults[scanResultCount].mac[17] = '\0';
        strncpy(scanResults[scanResultCount].name, adv.name, 19);
        scanResults[scanResultCount].name[19] = '\0';
        scanResultCount++;
      }
    }
    return;
  }

  // Reject anything not from our paired peer once one is registered — avoids
  // cross-talk if another AIO-Transmitter/receiver pair is on air nearby.
  if (txPeerRegistered && memcmp(mac, txTargetMacBytes, 6) != 0) {
    Serial.printf("[TX] Dropped pkt_id=%d from %02X:%02X:%02X:%02X:%02X:%02X (expected %02X:%02X:%02X:%02X:%02X:%02X)\n",
      pkt_id, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
      txTargetMacBytes[0],txTargetMacBytes[1],txTargetMacBytes[2],txTargetMacBytes[3],txTargetMacBytes[4],txTargetMacBytes[5]);
    return;
  }

  // ---- ESP_NOW handshake responses ----
  if (pkt_id == PKT_HELLO_ACK && len >= (int)sizeof(AckPacket)) {
    txAckReceived = true;
  }
  else if (pkt_id == PKT_CONN_OK && len >= (int)sizeof(AckPacket)) {
    txConnOkReceived = true;
  }

  // ---- Smart Connect handshake responses ----
  else if (pkt_id == SC_PKT_PARTIAL_OK && len >= (int)sizeof(ScPartialOkPacket)) {
    if (!txPartialOkReceived) Serial.println(F("[TX] PARTIAL_OK accepted"));
    txPartialOkReceived = true;
  }
  else if (pkt_id == SC_PKT_CONNECT_OK && len >= (int)sizeof(ScConnectOkPacket)) {
    txScPinResult = SC_PKT_CONNECT_OK;
  }
  else if (pkt_id == SC_PKT_CONNECT_FAIL && len >= (int)sizeof(ScConnectFailPacket)) {
    txScPinResult = SC_PKT_CONNECT_FAIL;
  }

  // ---- Telemetry during active session ----
  else if (pkt_id == PKT_TELEMETRY && len >= (int)sizeof(TelemetryPacket)) {
    TelemetryPacket tel;
    memcpy(&tel, data, sizeof(tel));
    txTelemetryPingMs = millis() - txCmdSendTimestamp;
    txLastTelemetryMs = millis();
    memcpy((void*)txTelemetryStr, tel.data, sizeof(tel.data));
    txNewTelemetry = true;
  }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 1)
void onTxEspNowSend(const wifi_tx_info_t* info, esp_now_send_status_t status) {
#else
void onTxEspNowSend(const uint8_t* mac, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) {
    txPktAckedCount++;
    txRawPingUs = micros() - txCmdSendTimestampUs;
  }
}

void initEspNow() {
  if (espNowInitialized) return;
  // Stop the STA from auto-reconnecting to a saved AP behind our back — a
  // reconnect re-tunes the radio to the AP's channel and silently breaks
  // ESP-NOW (which is pinned to ch1) until it drifts back. This was the cause
  // of mid-session "TX silent" drops even though sending code kept running.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_STA);
  delay(50);

  // Pin to channel 1 to match the receiver library. ESP-NOW unicast requires
  // both ends on the same channel, and without an explicit pin each side just
  // picks whatever its WiFi layer settled on — broadcasts still propagate
  // (so scanning works) but Hello/HELLO_ACK silently drops on mismatch.
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("[TX] esp_now_init failed"));
    return;
  }
  esp_now_register_recv_cb(onTxEspNowRecv);
  esp_now_register_send_cb(onTxEspNowSend);

  esp_now_peer_info_t bcast = {};
  memcpy(bcast.peer_addr, ESPNOW_BROADCAST, 6);
  bcast.channel = 0;
  bcast.encrypt = false;
  esp_now_add_peer(&bcast);

  WiFi.macAddress(txOwnMacBytes);
  snprintf(txOwnMacStr, sizeof(txOwnMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
    txOwnMacBytes[0], txOwnMacBytes[1], txOwnMacBytes[2],
    txOwnMacBytes[3], txOwnMacBytes[4], txOwnMacBytes[5]);

  espNowInitialized = true;
  espNowStatusActive = true;
  Serial.println(F("[TX] ESP-NOW initialized"));
}

void deinitEspNow() {
  if (!espNowInitialized) return;
  removeTxPeer();
  esp_now_del_peer(ESPNOW_BROADCAST);
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  espNowInitialized = false;
  espNowStatusActive = false;
  txSessionActive = false;
  txScanning = false;
  txActiveMode = 0;
  if (wifiWasAutoDisabled) {
    // Manage WiFi turned Wi-Fi off for this session — bring it back exactly as it
    // was and kick a reconnect to the last network so the user lands online again.
    wifiWasAutoDisabled = false;
    wifiEnabled = true;
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    String pwd = findSavedPwd(lastConnectedSSID);
    if (lastConnectedSSID.length() > 0 && pwd.length() > 0) {
      WiFi.begin(lastConnectedSSID.c_str(), pwd.c_str());
    }
    Serial.println(F("[TX] Manage WiFi: Wi-Fi re-enabled on exit"));
  } else if (wifiEnabled) {
    WiFi.mode(WIFI_STA);
  } else {
    WiFi.mode(WIFI_OFF);
  }
  Serial.println(F("[TX] ESP-NOW deinitialized"));
}

void addTxPeer(const uint8_t* mac) {
  if (txPeerRegistered) return;
  memcpy(txTargetMacBytes, mac, 6);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  txPeerRegistered = true;
}

void removeTxPeer() {
  if (!txPeerRegistered) return;
  esp_now_del_peer(txTargetMacBytes);
  memset(txTargetMacBytes, 0, 6);
  txPeerRegistered = false;
}

void sendTxCommand() {
  CommandPacket cmd;
  cmd.pkt_id = PKT_CMD;
  uint16_t buttons = 0;
  if (buttonStates[0]) buttons |= (1 << 0);
  if (buttonStates[1]) buttons |= (1 << 1);
  if (buttonStates[2]) buttons |= (1 << 2);
  if (buttonStates[3]) buttons |= (1 << 3);
  if (buttonStates[4]) buttons |= (1 << 4);
  if (dpad2States[0])  buttons |= (1 << 5);
  if (dpad2States[1])  buttons |= (1 << 6);
  if (dpad2States[2])  buttons |= (1 << 7);
  if (dpad2States[3])  buttons |= (1 << 8);
  if (dpad2States[4])  buttons |= (1 << 9);
  cmd.buttons = buttons;
  cmd.analog1 = (byte)simulated_analog_1;
  cmd.analog2 = (byte)simulated_analog_2;
  txCmdSendTimestamp = millis();
  txCmdSendTimestampUs = micros();
  txPktSentCount++;
  esp_err_t result = esp_now_send(txTargetMacBytes, (const uint8_t*)&cmd, sizeof(cmd));

  static uint16_t lastLoggedButtons = 0xFFFF;
  if (buttons != lastLoggedButtons) {
    lastLoggedButtons = buttons;
    Serial.printf("[TX] CMD send buttons=0x%04X a1=%d a2=%d result=%d\n", buttons, cmd.analog1, cmd.analog2, (int)result);
  }
}

// Begin the live command stream the moment a connection is acknowledged — called
// at connect time (not after the splash). Streaming during the "CONNECTED!"
// splash keeps the receiver's no-data watchdog fed and removes the fragile
// dependency on both sides' splash timers lining up.
void startTxSession() {
  txControlPage = 1;
  txSessionActive = true;
  txSessionStartMs = millis();
  txLastTelemetryMs = millis();
  lastTxCmdSend = 0;        // forces the first CMD out on the very next loop
  txPktSentCount = 0;
  txPktAckedCount = 0;
  memset((void*)txTelemetryStr, 0, sizeof(txTelemetryStr));
  strncpy(txTelemetryStr, "NO DATA", sizeof(txTelemetryStr));
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(PIN_SDA, PIN_SCL);
  // Hard cap on any single I2C transaction so a glitched bus (e.g. a slave
  // holding SDA low after a noisy edge on wake) can never freeze the whole
  // sketch. Without this the Wire ISR can spin forever waiting on the flag.
  Wire.setTimeOut(50);
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (1);
  }

  drawBootScreen();

  initMCP23017();
  Serial.println("[OK] MCP23017 done");

  pinMode(PIN_BATTERY,  INPUT);
  pinMode(PIN_USB_SENSE, INPUT);
  // GPIO2 is a strapping pin on classic WROOM-32 (crashes if driven at boot);
  // on ESP32-C6 (target hardware) it's a plain GPIO, safe to use as buzzer output.
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(SYS_CENTRE,   INPUT_PULLUP);
  pinMode(PIN_POWER,    INPUT_PULLUP);
  // GPIO6-11 are wired internally to the SPI flash chip on classic ESP32 (WROOM-32/DevKit)
  // modules — configuring them as GPIO corrupts the flash bus and causes a reboot loop.
  // Safe on ESP32-C3/C6 (target hardware), so only skip on classic ESP32 test rigs.
#if !CONFIG_IDF_TARGET_ESP32
  pinMode(PIN_INTA,     INPUT_PULLUP);
  pinMode(PIN_INTB,     INPUT_PULLUP);
#endif
  Serial.println("[OK] Pins done");

  playBootTone();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.disconnect(true);
  delay(100);
  Serial.println("[OK] WiFi done");

  loadCredentials();
  loadForbidden();
  loadSettings();
  Serial.println("[OK] Credentials done");

  prefs.begin("wifi_creds", true);
  wifiAutoOn = prefs.getBool("wifiAutoOn", false);
  lastConnectedSSID = prefs.getString("lastSSID", "");
  prefs.end();

  if (wifiAutoOn) {
    wifiEnabled = true;
    WiFi.mode(WIFI_STA);
  }

  startConnectivityPinger();
  startWifiBackgroundTask();

  // LittleFS.begin(true) with format-on-fail can hang the WDT on WROOM-32
  // if no LittleFS partition exists in the partition table.
  // Pass 'false' to skip auto-format and fail gracefully instead.
  if (!LittleFS.begin(false)) {
    Serial.println("[WARN] LittleFS not mounted (no partition or needs format).");
    Serial.println("[WARN] Use Tools > Partition Scheme > 'Default 4MB with spiffs' in Arduino IDE.");
  } else {
    Serial.println("[OK] LittleFS done");
  }

  Serial.println("Hardware Initialised!");

  bootStartTime  = millis();
  lastActivityTime = millis();
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  readButtons();

  // ---------- Activity + sleep logic ----------
  bool anyPress = buttonJustPressed[0] || buttonJustPressed[1] ||
                  buttonJustPressed[2] || buttonJustPressed[3] || buttonJustPressed[4];

  // Also reset sleep for any secondary button activity
  bool secondaryActive = (mcp.digitalRead(SYS_LEFT) == LOW) ||
                         (mcp.digitalRead(SYS_RIGHT) == LOW) ||
                         (digitalRead(SYS_CENTRE) == LOW) ||
                         (mcp.digitalRead(ANALOG_1_UP) == LOW) ||
                         (mcp.digitalRead(ANALOG_1_DOWN) == LOW) ||
                         (mcp.digitalRead(ANALOG_2_UP) == LOW) ||
                         (mcp.digitalRead(ANALOG_2_DOWN) == LOW);

  if (anyPress || secondaryActive) {
    lastActivityTime = millis();
    if (isDisplaySleep) {
      isDisplaySleep = false;
      display.ssd1306_command(SSD1306_DISPLAYON);
      return; // consume wake press
    }
  }

  bool inhibitSleep = (currentScreen == SCREEN_STOPWATCH && swRunning) ||
                      (currentScreen == SCREEN_TIMER && tmMode == TM_RINGING) ||
                      (currentScreen == SCREEN_TIMER_ALERT) ||
                      (currentScreen == SCREEN_WIFI_SCANNING) ||
                      (currentScreen == SCREEN_WIFI_PASSWORD && wifiConnecting) ||
                      (currentScreen == SCREEN_WIFI_NTP) ||
                      udpSessionActive || txSessionActive ||
                      txScanning ||
                      // Entire ESP_NOW / Smart Connect flow — these screens auto-advance
                      // on their own timers (handshake retries, splash screens, etc.)
                      // with no button press required, and the display-sleep early
                      // `return` below would otherwise freeze that state machine dead
                      // the moment the 10s idle timeout hit mid-flow.
                      (currentScreen >= SCREEN_TX_SCANNING && currentScreen <= SCREEN_TX_WIFI_DISABLE);

  if (!isDisplaySleep && currentScreen != PAGE_BOOT && !inhibitSleep) {
    if (millis() - lastActivityTime > 10000) {
      isDisplaySleep = true;
      display.ssd1306_command(SSD1306_DISPLAYOFF);
      return;
    }
  }
  if (isDisplaySleep) return;

  // ---------- Power button (long-press = shutdown confirm) ----------
  // Native GPIO (not on the MCP23017), so it needs its own held-duration
  // tracking rather than the buttonJustPressed[] edge array. Only arms the
  // confirm screen from the home hub or the apps menu, per spec.
  bool powerBtnDown = (digitalRead(PIN_POWER) == LOW);
  if (powerBtnWaitingRelease) {
    // Ignore entirely until the button that woke us from light sleep is
    // actually released — prevents a still-held wake press from being
    // re-read as a fresh long-press the moment boot reaches PAGE_OPTIONS.
    if (!powerBtnDown) powerBtnWaitingRelease = false;
    powerBtnPressStart = 0;
  } else if (powerBtnDown) {
    if (powerBtnPressStart == 0) powerBtnPressStart = millis();
    if (!powerBtnLongPressHandled && millis() - powerBtnPressStart > 3000 &&
        (currentScreen == PAGE_OPTIONS || currentScreen == SCREEN_APPS_MENU) && !isAnimating) {
      powerBtnLongPressHandled = true;
      shutdownReturnScreen = currentScreen;
      shutdownConfirmFocus = 0;
      lastActivityTime = millis();
      startAnimation(SCREEN_SHUTDOWN_CONFIRM, -64);
    }
  } else {
    powerBtnPressStart = 0;
    powerBtnLongPressHandled = false;
  }

  // ---------- Boot transition ----------
  // Do NOT return here — fall through so the render block below draws the boot
  // screen each frame and also renders the slide animation into PAGE_OPTIONS.
  if (currentScreen == PAGE_BOOT) {
    if (millis() - bootStartTime > 2000 && !isAnimating) {
      startAnimation(PAGE_OPTIONS, -64);
      lastActivityTime = millis();
    }
  }

  // ---------- WiFi scan polling ----------
  if (currentScreen == SCREEN_WIFI_SCANNING && !isAnimating) {
    lastActivityTime = millis();
    int result = WiFi.scanComplete();
    static bool scanRetried = false;
    if (result >= 0) {
      wifiScanResults = result;
      wifiListIndex = 0; wifiListScroll = 0;
      startAnimation(SCREEN_WIFI_RESULTS, -64);
      scanRetried = false;
    } else if (result == WIFI_SCAN_FAILED) {
      if (!scanRetried) {
        scanRetried = true;
        WiFi.scanNetworks(true, false, false, 80);
      } else {
        wifiScanResults = 0;
        startAnimation(SCREEN_WIFI_RESULTS, -64);
        scanRetried = false;
      }
    } else if (millis() - wifiScanStart > 8000) {
      WiFi.scanDelete();
      wifiScanResults = 0;
      startAnimation(SCREEN_WIFI_RESULTS, -64);
      scanRetried = false;
    }
  }

  // ---------- NTP sync polling ----------
  if (currentScreen == SCREEN_WIFI_NTP && !isAnimating) {
    lastActivityTime = millis();
    if (ntpSyncState == 0) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 0)) {
        ntpSyncState = 1;
        ntpSyncEnd   = millis();
      } else if (millis() - ntpSyncStart > 5000) {
        ntpSyncState = 2;
        ntpSyncEnd   = millis();
      }
    } else {
      if (millis() - ntpSyncEnd > 2000) {
        startAnimation(SCREEN_WIFI_HOME, -64);
        ntpSyncState = 0;
      }
    }
  }

  // ---------- WiFi UDP transmit (configurable Hz, runs regardless of screen/animation) ----------
  if (udpSessionActive && WiFi.status() == WL_CONNECTED) {
    unsigned long udpIntervalMs = 1000 / wifiUdpFreqHz;
    if (millis() - lastUdpSend >= udpIntervalMs) {
      lastUdpSend = millis();
      sendUdpControlPacket();
    }
  }

  // ---------- ESP-NOW transmit (runs regardless of screen/animation) ----------
  if (txSessionActive && espNowInitialized) {
    int freqHz = (txActiveMode == 1) ? espNowFreqHz : sconnectFreqHz;
    unsigned long intervalMs = 1000 / freqHz;
    if (millis() - lastTxCmdSend >= intervalMs) {
      lastTxCmdSend = millis();
      sendTxCommand();
    }

    // Telemetry watchdog — the receiver replies with telemetry to every command,
    // so no telemetry for 10s means it has dropped off (powered down / out of
    // range). Stop the session and surface the RX-lost page. Gated to the active
    // control screens so the teardown flow (terminate) isn't interrupted.
    if ((currentScreen == SCREEN_TX_CONTROL || currentScreen == SCREEN_TX_STATUS ||
         currentScreen == SCREEN_TX_TELEMETRY) &&
        (millis() - txLastTelemetryMs > 10000)) {
      Serial.println(F("[TX] No telemetry 10s — RX lost"));
      txSessionActive = false;
      removeTxPeer();
      txRxLostStart = millis();
      startAnimation(SCREEN_TX_RX_LOST, -64);
    }
  }

  // ---------- Timer tick ----------
  if (tmMode == TM_RUNNING) {
    unsigned long now   = millis();
    unsigned long delta = now - tmLastTick;
    if (tmRemainingMillis > delta) {
      tmRemainingMillis -= delta;
      tmLastTick = now;
    } else {
      tmRemainingMillis = 0;
      tmMode = TM_RINGING;
      currentScreen = SCREEN_TIMER_ALERT;
      if (isDisplaySleep) { display.ssd1306_command(SSD1306_DISPLAYON); isDisplaySleep = false; }
      lastActivityTime = millis();
      tmRingPhase = 0;
      tmRingNextStep = 0;
    }
  }

  // ---------- Timer ring buzzer ----------
  if (tmMode == TM_RINGING) stepTimerRingTone();

  // ---------- Input handling ----------
  if (!isAnimating) {

    // Update analog simulation based on MCP buttons
    if (mcp.digitalRead(ANALOG_1_UP) == LOW) { if (simulated_analog_1 < 100) simulated_analog_1++; }
    if (mcp.digitalRead(ANALOG_1_DOWN) == LOW) { if (simulated_analog_1 > 0) simulated_analog_1--; }
    if (mcp.digitalRead(ANALOG_2_UP) == LOW) { if (simulated_analog_2 < 100) simulated_analog_2++; }
    if (mcp.digitalRead(ANALOG_2_DOWN) == LOW) { if (simulated_analog_2 > 0) simulated_analog_2--; }

    // ===== PAGE_OPTIONS (Main Hub) =====
    if (currentScreen == PAGE_OPTIONS) {
      if (buttonJustPressed[3] && page2Focus == 0) page2Focus = 1;
      else if (buttonJustPressed[2] && page2Focus == 1) page2Focus = 0;
      else if (buttonJustPressed[1] && (page2Focus == 0 || page2Focus == 1)) page2Focus = 2;
      else if (buttonJustPressed[0] && page2Focus == 2) page2Focus = 0;

      if (buttonJustPressed[4]) {
        if (page2Focus == 1) {
          appMenuIndex = 0;
          startAnimation(SCREEN_APPS_MENU, -64);
        }
        else if (page2Focus == 0) { // Transmitter
          txMenuFocus = 0;
          txMenuScroll = 0;
          startAnimation(SCREEN_TX_MENU, -64);
        }
        else if (page2Focus == 2) { // Settings
          settingsMenuIndex = 0;
          settingsMenuScroll = 0;
          startAnimation(SCREEN_SETTINGS_MENU, -64);
        }
      }
    }

    // ===== SHUTDOWN CONFIRM (power button long-press) =====
    else if (currentScreen == SCREEN_SHUTDOWN_CONFIRM) {
      lastActivityTime = millis();
      if (buttonJustPressed[2]) shutdownConfirmFocus = 0; // Left -> Yes
      if (buttonJustPressed[3]) shutdownConfirmFocus = 1; // Right -> No
      if (buttonJustPressed[4]) {
        if (shutdownConfirmFocus == 0) {
          enterLightSleep(); // returns here on wake — no reboot
        } else {
          startAnimation(shutdownReturnScreen, 64);
        }
      }
    }

    // ===== SETTINGS MENU =====
    else if (currentScreen == SCREEN_SETTINGS_MENU) {
      if (buttonJustPressed[0]) { if (settingsMenuIndex > -1) settingsMenuIndex--; }
      if (buttonJustPressed[1]) { if (settingsMenuIndex < NUM_SETTINGS_ITEMS - 1) settingsMenuIndex++; }
      if (buttonJustPressed[2]) { if (settingsMenuIndex == -1) settingsMenuIndex = 0; }
      if (buttonJustPressed[4]) {
        if (settingsMenuIndex == -1) {
          startAnimation(PAGE_OPTIONS, 64);
        } else if (settingsMenuIndex == 3) { // Device Name
          kbInputBuffer  = txDeviceName; currentKBMode = KB_UPPER;
          kbCursorRow    = 0; kbCursorCol = 0;
          kbTextCursor   = kbInputBuffer.length();
          kbScrollOffset = 0;
          kbReturnScreen = SCREEN_SETTINGS_MENU;
          currentScreen  = SCREEN_WIFI_KEYBOARD;
        } else if (settingsMenuIndex == 4) { // Manage WiFi
          manageWifiFocusLine = -1;             // start on Back so the text reads from the top
          manageWifiCursor = manageWifi ? 0 : 1; // preselect the current setting
          manageWifiScroll = 0;
          startAnimation(SCREEN_SETTINGS_MANAGE_WIFI, -64);
        } else {
          settingsFreqTarget = settingsMenuIndex;
          int currentHz = (settingsFreqTarget == 0) ? wifiUdpFreqHz :
                          (settingsFreqTarget == 1) ? espNowFreqHz  : sconnectFreqHz;
          settingsFreqFocus = 1; // default to 20Hz if no match
          for (int i = 0; i < NUM_FREQ_OPTIONS; i++) {
            if (freqOptions[i] == currentHz) { settingsFreqFocus = i; break; }
          }
          startAnimation(SCREEN_SETTINGS_FREQ, -64);
        }
      }
    }

    // ===== SETTINGS FREQUENCY SELECT =====
    else if (currentScreen == SCREEN_SETTINGS_FREQ) {
      if (buttonJustPressed[0]) settingsFreqFocus = -1;            // Up to Back
      if (buttonJustPressed[1] && settingsFreqFocus == -1) settingsFreqFocus = 0;
      if (buttonJustPressed[2] && settingsFreqFocus >= 0) { if (settingsFreqFocus > 0) settingsFreqFocus--; }
      if (buttonJustPressed[3] && settingsFreqFocus >= 0) { if (settingsFreqFocus < NUM_FREQ_OPTIONS - 1) settingsFreqFocus++; }
      if (buttonJustPressed[4]) {
        if (settingsFreqFocus == -1) {
          startAnimation(SCREEN_SETTINGS_MENU, 64);
        } else {
          int hz = freqOptions[settingsFreqFocus];
          if (settingsFreqTarget == 0) wifiUdpFreqHz = hz;
          else if (settingsFreqTarget == 1) espNowFreqHz = hz;
          else sconnectFreqHz = hz;
          saveFreqSetting(settingsFreqTarget, hz);
          startAnimation(SCREEN_SETTINGS_MENU, 64);
        }
      }
    }

    // ===== MANAGE WIFI (line-by-line scrollable info + YES/NO toggle) =====
    // Focus stops: Back (-1) -> each wrapped text line (0..N-1), one stop per
    // line -> the YES/NO button row (N), the last stop.
    // Up/Down: one stop per tap; holding them down auto-repeats at a fixed rate
    // (this screen only) so scrolling through the paragraph doesn't need ten taps.
    // Left/Right: always set YES/NO, regardless of scroll position — so the
    // choice can be made from the top without scrolling all the way down first.
    else if (currentScreen == SCREEN_SETTINGS_MANAGE_WIFI) {
      int totalLines = manageWifiTotalLines();

      static unsigned long mwUpHoldStart = 0, mwDownHoldStart = 0;
      static unsigned long mwUpLastRepeat = 0, mwDownLastRepeat = 0;
      const unsigned long mwHoldDelay  = 350; // ms held before auto-repeat kicks in
      const unsigned long mwRepeatRate = 120; // ms between auto-repeat steps

      if (buttonJustPressed[0]) {
        mwUpHoldStart = mwUpLastRepeat = millis();
        if (manageWifiFocusLine > -1) manageWifiFocusLine--;
      } else if (buttonStates[0]) {
        if (millis() - mwUpHoldStart > mwHoldDelay && millis() - mwUpLastRepeat > mwRepeatRate) {
          mwUpLastRepeat = millis();
          if (manageWifiFocusLine > -1) manageWifiFocusLine--;
        }
      }

      if (buttonJustPressed[1]) {
        mwDownHoldStart = mwDownLastRepeat = millis();
        if (manageWifiFocusLine < totalLines) manageWifiFocusLine++;
      } else if (buttonStates[1]) {
        if (millis() - mwDownHoldStart > mwHoldDelay && millis() - mwDownLastRepeat > mwRepeatRate) {
          mwDownLastRepeat = millis();
          if (manageWifiFocusLine < totalLines) manageWifiFocusLine++;
        }
      }

      if (buttonJustPressed[2]) manageWifiCursor = 0; // Left -> YES (works anywhere)
      if (buttonJustPressed[3]) manageWifiCursor = 1; // Right -> NO (works anywhere)

      if (buttonJustPressed[4]) {
        if (manageWifiFocusLine == -1) {
          startAnimation(SCREEN_SETTINGS_MENU, 64);
        } else if (manageWifiFocusLine == totalLines) {
          manageWifi = (manageWifiCursor == 0);
          saveManageWifi(manageWifi);
          startAnimation(SCREEN_SETTINGS_MENU, 64);
        }
        // Focus on a plain text line: Center does nothing.
      }
    }

    // ===== TRANSMITTER FLOW =====
    // 2x2 grid: 0=ESP_NOW 1=SMART  /  2=WiFi UDP 3=BLE   (-1=Back)
    else if (currentScreen == SCREEN_TX_MENU) {
      if (buttonJustPressed[0]) { // Up
        if (txMenuFocus == 0 || txMenuFocus == 1) txMenuFocus = -1;
        else if (txMenuFocus == 2) txMenuFocus = 0;
        else if (txMenuFocus == 3) txMenuFocus = 1;
      }
      if (buttonJustPressed[1]) { // Down
        if (txMenuFocus == -1) txMenuFocus = 0;
        else if (txMenuFocus == 0) txMenuFocus = 2;
        else if (txMenuFocus == 1) txMenuFocus = 3;
      }
      if (buttonJustPressed[2]) { // Left
        if (txMenuFocus == 1) txMenuFocus = 0;
        else if (txMenuFocus == 3) txMenuFocus = 2;
      }
      if (buttonJustPressed[3]) { // Right
        if (txMenuFocus == 0) txMenuFocus = 1;
        else if (txMenuFocus == 2) txMenuFocus = 3;
      }
      if (buttonJustPressed[4]) {
        if (txMenuFocus == -1) {
          startAnimation(PAGE_OPTIONS, 64);
        } else if (txMenuFocus == 0 || txMenuFocus == 1) { // ESP_NOW / SMART_CONNECT
          txPendingMode = (txMenuFocus == 0) ? 1 : 2;
          if (wifiEnabled || WiFi.status() == WL_CONNECTED) {
            if (manageWifi) {
              // Manage WiFi ON — silently shut Wi-Fi down now (so it can't hop the
              // radio off channel 1 mid-session) and remember to restore it on exit.
              wifiWasAutoDisabled = true;
              wifiEnabled = false;
              WiFi.setAutoReconnect(false);
              WiFi.disconnect(true);
              initEspNow();
              txActiveMode = txPendingMode;
              scanResultCount = 0;
              txScanning = true;
              txScanStart = millis();
              startAnimation(txPendingMode == 1 ? SCREEN_TX_SCANNING : SCREEN_SC_SCANNING, -64);
            } else {
              // Manage WiFi OFF — make the user disable it first so it can't hop the
              // radio off channel 1 mid-session.
              txWifiDisableFocus = 0;
              startAnimation(SCREEN_TX_WIFI_DISABLE, -64);
            }
          } else {
            // WiFi already off — go straight in.
            initEspNow();
            txActiveMode = txPendingMode;
            scanResultCount = 0;
            txScanning = true;
            txScanStart = millis();
            startAnimation(txPendingMode == 1 ? SCREEN_TX_SCANNING : SCREEN_SC_SCANNING, -64);
          }
        } else if (txMenuFocus == 2) { // WiFi UDP
          if (WiFi.status() == WL_CONNECTED) {
            txUdpModeFocus = 0;
            startAnimation(SCREEN_TX_UDP_MODE, -64);
          } else {
            udpNoWifiStart = millis();
            startAnimation(SCREEN_TX_UDP_NOWIFI, -64);
          }
        }
        // txMenuFocus == 3 (BLE): functionless
      }
    }

    // ===== DISABLE WIFI? (gate before ESP_NOW / SMART_CONNECT) =====
    else if (currentScreen == SCREEN_TX_WIFI_DISABLE) {
      if (buttonJustPressed[2]) txWifiDisableFocus = 0; // Left -> Yes
      if (buttonJustPressed[3]) txWifiDisableFocus = 1; // Right -> No
      if (buttonJustPressed[4]) {
        if (txWifiDisableFocus == 0) {
          // YES — fully shut WiFi down so nothing touches the radio, then go.
          wifiEnabled = false;
          wifiAutoOn = false;
          WiFi.setAutoReconnect(false);
          WiFi.disconnect(true);
          initEspNow();
          txActiveMode = txPendingMode;
          scanResultCount = 0;
          txScanning = true;
          txScanStart = millis();
          startAnimation(txPendingMode == 1 ? SCREEN_TX_SCANNING : SCREEN_SC_SCANNING, -64);
        } else {
          // NO — abandon transmit, keep WiFi as it was.
          startAnimation(PAGE_OPTIONS, 64);
        }
      }
    }

    // ===== WiFi UDP: NO WIFI ERROR (timed redirect to WiFi app) =====
    else if (currentScreen == SCREEN_TX_UDP_NOWIFI) {
      if (millis() - udpNoWifiStart > 2000) {
        // Mirror the Apps-menu WiFi entry logic
        if (WiFi.status() == WL_CONNECTED) {
          wifiHomeCursor = 0;
          startAnimation(SCREEN_WIFI_HOME, -64);
        } else if (wifiEnabled) {
          WiFi.scanDelete(); WiFi.disconnect();
          delay(100);
          WiFi.scanNetworks(true, false, false, 250);
          wifiScanStart = millis();
          wifiScanResults = -1;
          startAnimation(SCREEN_WIFI_SCANNING, -64);
        } else {
          wifiConfirmCursor = 0;
          startAnimation(SCREEN_WIFI_CONFIRM, -64);
        }
      }
    }

    // ===== WiFi UDP: MODE SELECT (Broadcast / Unicast) =====
    else if (currentScreen == SCREEN_TX_UDP_MODE) {
      if (buttonJustPressed[0]) txUdpModeFocus = -1;            // Up to Back
      if (buttonJustPressed[1] && txUdpModeFocus == -1) txUdpModeFocus = 0;
      if (buttonJustPressed[2] && txUdpModeFocus >= 0) txUdpModeFocus = 0; // Left = Broadcast
      if (buttonJustPressed[3] && txUdpModeFocus >= 0) txUdpModeFocus = 1; // Right = Unicast
      if (buttonJustPressed[4]) {
        if (txUdpModeFocus == -1) {
          startAnimation(SCREEN_TX_MENU, 64);
        } else if (txUdpModeFocus == 0) { // Broadcast
          udpBroadcast = true;
          udpSessionActive = true;
          udp.begin(UDP_TX_PORT);
          startAnimation(SCREEN_TX_UDP_CONTROL, -64);
        } else { // Unicast — ask for IP first
          udpBroadcast = false;
          txUdpIpCursor = 1;
          startAnimation(SCREEN_TX_UDP_IP, -64);
        }
      }
    }

    // ===== WiFi UDP: IP ENTRY (Unicast) =====
    else if (currentScreen == SCREEN_TX_UDP_IP) {
      if (buttonJustPressed[0]) { if (txUdpIpCursor == 0 || txUdpIpCursor == 1) txUdpIpCursor = -1; }
      if (buttonJustPressed[1]) { if (txUdpIpCursor == -1) txUdpIpCursor = 0; }
      if (buttonJustPressed[2]) { if (txUdpIpCursor == 1) txUdpIpCursor = 0; }
      if (buttonJustPressed[3]) { if (txUdpIpCursor == 0) txUdpIpCursor = 1; }
      if (buttonJustPressed[4]) {
        if (txUdpIpCursor == -1) {
          startAnimation(SCREEN_TX_UDP_MODE, 64);
        } else if (txUdpIpCursor == 0) { // open keyboard to edit IP
          kbInputBuffer = udpTargetIP; currentKBMode = KB_LOWER;
          kbCursorRow = 0; kbCursorCol = 0;
          kbTextCursor   = kbInputBuffer.length();
          kbScrollOffset = 0;
          kbReturnScreen = SCREEN_TX_UDP_IP;
          currentScreen  = SCREEN_WIFI_KEYBOARD;
        } else if (txUdpIpCursor == 1) { // Start
          udpSessionActive = true;
          udp.begin(UDP_TX_PORT);
          startAnimation(SCREEN_TX_UDP_CONTROL, -64);
        }
      }
    }

    // ===== WiFi UDP: CONTROL PANEL (single screen, long-press SYS_CENTRE to end) =====
    else if (currentScreen == SCREEN_TX_UDP_CONTROL) {
      bool sysCentre = (digitalRead(SYS_CENTRE) == LOW);
      static bool lastSysCentreUdp = false;
      static unsigned long sysCentreUdpPress = 0;
      if (sysCentre && !lastSysCentreUdp) sysCentreUdpPress = millis();
      if (sysCentre && (millis() - sysCentreUdpPress > 1000)) {
        txTerminateFocus = 1;
        startAnimation(SCREEN_TX_TERMINATE, -64);
      }
      lastSysCentreUdp = sysCentre;
    }
    // ===== ESP_NOW SCANNING =====
    else if (currentScreen == SCREEN_TX_SCANNING) {
      lastActivityTime = millis();
      if (millis() - txScanStart > 3000) {
        txScanning = false;
        txResultIndex = (scanResultCount > 0) ? 0 : -1;
        txResultScroll = 0;
        startAnimation(SCREEN_TX_RESULTS, -64);
      }
    }
    // ===== ESP_NOW RESULTS =====
    else if (currentScreen == SCREEN_TX_RESULTS) {
      int maxIdx = scanResultCount - 1;
      if (buttonJustPressed[0]) { if (txResultIndex > -1) txResultIndex--; }
      if (buttonJustPressed[1]) { if (txResultIndex < maxIdx) txResultIndex++; }
      if (buttonJustPressed[4]) {
        if (txResultIndex == -1) {
          deinitEspNow();
          startAnimation(PAGE_OPTIONS, 64);
        } else if (txResultIndex >= 0 && txResultIndex < scanResultCount) {
          txTargetName = scanResults[txResultIndex].name;
          addTxPeer(scanResults[txResultIndex].macBytes);
          txHandshakeStep = 0;
          txHandshakeAttempt = 0;
          txAckReceived = false;
          txConnOkReceived = false;
          txPairStart = millis();
          startAnimation(SCREEN_TX_PAIRING, -64);
        }
      }
    }
    // ===== ESP_NOW PAIRING (real handshake state machine) =====
    else if (currentScreen == SCREEN_TX_PAIRING) {
      lastActivityTime = millis();
      // Back button (left = <--)
      if (buttonJustPressed[2]) {
        removeTxPeer();
        scanResultCount = 0;
        txScanning = true;
        txScanStart = millis();
        startAnimation(SCREEN_TX_SCANNING, 64);
      }
      // Step 0: send HelloPacket
      else if (txHandshakeStep == 0) {
        HelloPacket hello;
        hello.pkt_id = PKT_HELLO;
        strncpy(hello.mac, txOwnMacStr, sizeof(hello.mac));
        strncpy(hello.name, txDeviceName.c_str(), sizeof(hello.name));
        esp_now_send(txTargetMacBytes, (const uint8_t*)&hello, sizeof(hello));
        txHandshakeStep = 1;
        txHandshakeTime = millis();
        txAckReceived = false;
        txHandshakeAttempt++;
        Serial.printf("[TX] Hello attempt %d/3\n", txHandshakeAttempt);
      }
      // Step 1: wait for HELLO_ACK (or CONN_OK arriving early counts as implicit ack —
      // the receiver only sends CONN_OK after processing our Hello, so it's strictly
      // stronger proof than the ACK itself; treating it as equivalent avoids a race
      // where the ACK flag gets reset here just as CONN_OK lands in the same window)
      else if (txHandshakeStep == 1) {
        if (txAckReceived || txConnOkReceived) {
          txHandshakeStep = 2;
          txHandshakeTime = millis();
          Serial.println(F("[TX] ACK received, waiting for CONN_OK"));
        } else if (millis() - txHandshakeTime > 3000) {
          if (txHandshakeAttempt >= 3) {
            txFailReason = "ACK Not Received";
            txFailedStart = millis();
            removeTxPeer();
            startAnimation(SCREEN_TX_FAILED, -64);
          } else {
            txHandshakeStep = 0; // retry
          }
        }
      }
      // Step 2: wait for CONN_OK
      else if (txHandshakeStep == 2) {
        if (txConnOkReceived) {
          // Echo CONN_OK back to confirm
          AckPacket ack;
          ack.pkt_id = PKT_CONN_OK;
          esp_now_send(txTargetMacBytes, (const uint8_t*)&ack, sizeof(ack));
          startTxSession();
          txConnectedStart = millis();
          startAnimation(SCREEN_TX_CONNECTED, -64);
          Serial.println(F("[TX] CONN_OK received, connected!"));
        } else if (millis() - txHandshakeTime > 10000) {
          txFailReason = "CONN_OK Not Received";
          txFailedStart = millis();
          removeTxPeer();
          startAnimation(SCREEN_TX_FAILED, -64);
        }
      }
    }
    // ===== CONNECTED (splash 5s; session already streaming) =====
    else if (currentScreen == SCREEN_TX_CONNECTED) {
      if (millis() - txConnectedStart > 5000) {
        startAnimation(SCREEN_TX_CONTROL, -64);
      }
    }

    // ===== SMART CONNECT SCANNING =====
    else if (currentScreen == SCREEN_SC_SCANNING) {
      lastActivityTime = millis();
      if (millis() - txScanStart > 3000) {
        txScanning = false;
        txResultIndex = (scanResultCount > 0) ? 0 : -1;
        txResultScroll = 0;
        startAnimation(SCREEN_SC_RESULTS, -64);
      }
    }
    // ===== SMART CONNECT RESULTS =====
    else if (currentScreen == SCREEN_SC_RESULTS) {
      int maxIdx = scanResultCount - 1;
      if (buttonJustPressed[0]) { if (txResultIndex > -1) txResultIndex--; }
      if (buttonJustPressed[1]) { if (txResultIndex < maxIdx) txResultIndex++; }
      if (buttonJustPressed[4]) {
        if (txResultIndex == -1) {
          deinitEspNow();
          startAnimation(PAGE_OPTIONS, 64);
        } else if (txResultIndex >= 0 && txResultIndex < scanResultCount) {
          txTargetName = scanResults[txResultIndex].name;
          addTxPeer(scanResults[txResultIndex].macBytes);
          txPartialOkReceived = false;
          scConnectStart = millis();
          scLastReqResend = millis();
          // Send ScConnectReq immediately
          ScConnectReqPacket req;
          req.pkt_id = SC_PKT_CONNECT_REQ;
          strncpy(req.mac, txOwnMacStr, sizeof(req.mac));
          strncpy(req.name, txDeviceName.c_str(), sizeof(req.name));
          esp_now_send(txTargetMacBytes, (const uint8_t*)&req, sizeof(req));
          startAnimation(SCREEN_SC_CONNECTING1, -64);
        }
      }
    }
    // ===== SC CONNECTING1 (wait for PARTIAL_OK) =====
    else if (currentScreen == SCREEN_SC_CONNECTING1) {
      lastActivityTime = millis();
      if (txPartialOkReceived) {
        scPin[0] = scPin[1] = scPin[2] = scPin[3] = 0;
        scPinCursor = 0;
        startAnimation(SCREEN_SC_PIN_ENTRY, -64);
      } else if (millis() - scConnectStart > 10000) {
        txFailReason = "PARTIAL_OK Not Received";
        txFailedStart = millis();
        removeTxPeer();
        startAnimation(SCREEN_TX_FAILED, -64);
      } else if (millis() - scLastReqResend >= 700) {
        // Retransmit ConnectReq — a single dropped packet must not stall the
        // whole flow (the receiver only starts replying once it sees one).
        scLastReqResend = millis();
        ScConnectReqPacket req;
        req.pkt_id = SC_PKT_CONNECT_REQ;
        strncpy(req.mac, txOwnMacStr, sizeof(req.mac));
        strncpy(req.name, txDeviceName.c_str(), sizeof(req.name));
        esp_now_send(txTargetMacBytes, (const uint8_t*)&req, sizeof(req));
      }
    }
    // ===== SC PIN ENTRY =====
    else if (currentScreen == SCREEN_SC_PIN_ENTRY) {
      if (buttonJustPressed[0]) { if (scPinCursor >= 0 && scPinCursor <= 3) scPin[scPinCursor] = (scPin[scPinCursor] + 1) % 10; }
      if (buttonJustPressed[1]) { if (scPinCursor >= 0 && scPinCursor <= 3) scPin[scPinCursor] = (scPin[scPinCursor] + 9) % 10; }
      if (buttonJustPressed[2]) { if (scPinCursor > -1) scPinCursor--; }
      if (buttonJustPressed[3]) { if (scPinCursor < 4) scPinCursor++; }
      if (buttonJustPressed[4]) {
        if (scPinCursor == -1) {
          removeTxPeer();
          startAnimation(SCREEN_SC_RESULTS, 64);
        } else if (scPinCursor == 4) { // OK — send PIN
          ScPinSubmitPacket pin;
          pin.pkt_id = SC_PKT_PIN_SUBMIT;
          pin.pin = scPin[0] * 1000 + scPin[1] * 100 + scPin[2] * 10 + scPin[3];
          txScPinResult = 0;
          esp_now_send(txTargetMacBytes, (const uint8_t*)&pin, sizeof(pin));
          scAuthStart = millis();
          scLastPinResend = millis();
          startAnimation(SCREEN_SC_CONNECTING2, -64);
        }
      }
    }
    // ===== SC CONNECTING2 (authenticating — wait for OK/FAIL) =====
    else if (currentScreen == SCREEN_SC_CONNECTING2) {
      lastActivityTime = millis();
      if (txScPinResult == SC_PKT_CONNECT_OK) {
        startTxSession();
        scConnectedStart = millis();
        startAnimation(SCREEN_SC_CONNECTED, -64);
        Serial.println(F("[TX] SC PIN accepted, connected!"));
      } else if (txScPinResult == SC_PKT_CONNECT_FAIL) {
        txFailReason = "Wrong PIN";
        txFailedStart = millis();
        removeTxPeer();
        startAnimation(SCREEN_TX_FAILED, -64);
      } else if (millis() - scAuthStart > 10000) {
        txFailReason = "No Response (Auth)";
        txFailedStart = millis();
        removeTxPeer();
        startAnimation(SCREEN_TX_FAILED, -64);
      } else if (millis() - scLastPinResend >= 700) {
        // Retransmit PinSubmit — single-shot was the cause of intermittent
        // "No Response (Auth)": if the one packet dropped, the receiver never
        // saw the PIN and never replied. Resend until we get OK/FAIL.
        scLastPinResend = millis();
        ScPinSubmitPacket pin;
        pin.pkt_id = SC_PKT_PIN_SUBMIT;
        pin.pin = scPin[0] * 1000 + scPin[1] * 100 + scPin[2] * 10 + scPin[3];
        esp_now_send(txTargetMacBytes, (const uint8_t*)&pin, sizeof(pin));
      }
    }
    // ===== SC CONNECTED (splash 2.5s; session already streaming) =====
    else if (currentScreen == SCREEN_SC_CONNECTED) {
      if (millis() - scConnectedStart > 2500) {
        startAnimation(SCREEN_TX_CONTROL, -64);
      }
    }
    // ===== CONNECTION FAILED (5s then back to Page 2) =====
    else if (currentScreen == SCREEN_TX_FAILED) {
      if (millis() - txFailedStart > 5000) {
        deinitEspNow();
        startAnimation(PAGE_OPTIONS, 64);
      }
    }
    // ===== RX DISCONNECTED (telemetry lost — 5s or any press back to Page 2) =====
    else if (currentScreen == SCREEN_TX_RX_LOST) {
      bool pressed = buttonJustPressed[0] || buttonJustPressed[1] ||
                     buttonJustPressed[2] || buttonJustPressed[3] || buttonJustPressed[4];
      if (pressed || millis() - txRxLostStart > 5000) {
        deinitEspNow();
        startAnimation(PAGE_OPTIONS, 64);
      }
    }
    else if (currentScreen == SCREEN_TX_CONTROL || currentScreen == SCREEN_TX_STATUS || currentScreen == SCREEN_TX_TELEMETRY) {
      bool sysLeft = (mcp.digitalRead(SYS_LEFT) == LOW);
      bool sysRight = (mcp.digitalRead(SYS_RIGHT) == LOW);
      bool sysCentre = (digitalRead(SYS_CENTRE) == LOW);
      
      static bool lastSysLeft = false;
      static bool lastSysRight = false;
      static bool lastSysCentre = false;
      static unsigned long sysCentrePressTime = 0;
      
      if (sysLeft && !lastSysLeft) {
        if (txControlPage > 0) {
          txControlPage--;
          if (txControlPage == 0) startAnimation(SCREEN_TX_STATUS, 0, 128);
          if (txControlPage == 1) startAnimation(SCREEN_TX_CONTROL, 0, 128);
        }
      }
      if (sysRight && !lastSysRight) {
        if (txControlPage < 2) {
          txControlPage++;
          if (txControlPage == 1) startAnimation(SCREEN_TX_CONTROL, 0, -128);
          if (txControlPage == 2) startAnimation(SCREEN_TX_TELEMETRY, 0, -128);
        }
      }
      
      if (sysCentre && !lastSysCentre) {
        sysCentrePressTime = millis();
      }
      if (sysCentre && (millis() - sysCentrePressTime > 1000)) {
        txTerminateFocus = 1;
        startAnimation(SCREEN_TX_TERMINATE, -64);
      }
      
      lastSysLeft = sysLeft;
      lastSysRight = sysRight;
      lastSysCentre = sysCentre;
    }
    else if (currentScreen == SCREEN_TX_TERMINATE) {
      if (buttonJustPressed[2]) txTerminateFocus = 0;
      if (buttonJustPressed[3]) txTerminateFocus = 1;
      if (buttonJustPressed[4]) {
        if (txTerminateFocus == 0) {
          txTerminatingStart = millis();
          startAnimation(SCREEN_TX_TERMINATING, -64);
        } else {
          // Resume
          if (udpSessionActive) startAnimation(SCREEN_TX_UDP_CONTROL, 64);
          else if (txControlPage == 0) startAnimation(SCREEN_TX_STATUS, 64);
          else if (txControlPage == 1) startAnimation(SCREEN_TX_CONTROL, 64);
          else startAnimation(SCREEN_TX_TELEMETRY, 64);
        }
      }
    }
    else if (currentScreen == SCREEN_TX_TERMINATING) {
      if (millis() - txTerminatingStart > 2000) {
        if (udpSessionActive) {
          sendUdpControlPacket();
          udp.stop();
          udpSessionActive = false;
        }
        if (txSessionActive && espNowInitialized) {
          CommandPacket cmd;
          cmd.pkt_id = PKT_CMD;
          cmd.buttons = 0;
          cmd.analog1 = 0;
          cmd.analog2 = 0;
          esp_now_send(txTargetMacBytes, (const uint8_t*)&cmd, sizeof(cmd));
          delay(50);
          ClosePacket cls;
          cls.pkt_id = PKT_CLOSE;
          esp_now_send(txTargetMacBytes, (const uint8_t*)&cls, sizeof(cls));
          delay(50);
        }
        deinitEspNow();
        startAnimation(PAGE_OPTIONS, 64);
      }
    }

    // ===== APPS MENU =====
    else if (currentScreen == SCREEN_APPS_MENU) {
      if (buttonJustPressed[0]) appMenuIndex = (appMenuIndex - 1 + NUM_APP_ITEMS) % NUM_APP_ITEMS;
      if (buttonJustPressed[1]) appMenuIndex = (appMenuIndex + 1) % NUM_APP_ITEMS;
      if (buttonJustPressed[2]) startAnimation(PAGE_OPTIONS, 64); // back
      if (buttonJustPressed[4]) {
        if (appMenuIndex == 0) { // Stopwatch
          swFocus = 1;
          startAnimation(SCREEN_STOPWATCH, -64);
        } else if (appMenuIndex == 1) { // Timer
          tmMode = TM_SETTING; tmFocus = 1;
          startAnimation(SCREEN_TIMER, -64);
        } else if (appMenuIndex == 2) { // WiFi
          if (WiFi.status() == WL_CONNECTED) {
            wifiHomeCursor = 0;
            startAnimation(SCREEN_WIFI_HOME, -64);
          } else if (wifiEnabled) {
            WiFi.scanDelete(); WiFi.disconnect();
            delay(100);
            WiFi.scanNetworks(true, false, false, 250);
            wifiScanStart = millis();
            wifiScanResults = -1;
            startAnimation(SCREEN_WIFI_SCANNING, -64);
          } else {
            wifiConfirmCursor = 0;
            startAnimation(SCREEN_WIFI_CONFIRM, -64);
          }
        } else if (appMenuIndex == 3) { // Calculator
          calcCursorRow = 0; calcCursorCol = 0;
          calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
          startAnimation(SCREEN_CALCULATOR, -64);
        } else if (appMenuIndex == 4) { // Exit to Boot
          startAnimation(PAGE_OPTIONS, 64);
        }
      }
    }

    // ===== STOPWATCH =====
    else if (currentScreen == SCREEN_STOPWATCH) {
      if (buttonJustPressed[0]) { if (swFocus == 1 || swFocus == 2) swFocus = 0; }
      if (buttonJustPressed[1]) { if (swFocus == 0) swFocus = 1; }
      if (buttonJustPressed[2]) { if (swFocus == 2) swFocus = 1; }
      if (buttonJustPressed[3]) { if (swFocus == 1) swFocus = 2; }
      if (buttonJustPressed[4]) {
        if (swFocus == 0) {
          startAnimation(SCREEN_APPS_MENU, 64);
        } else if (swFocus == 1) {
          if (swRunning) { swElapsedTime += millis() - swStartTime; swRunning = false; }
          else { swStartTime = millis(); swRunning = true; }
        } else if (swFocus == 2) {
          if (swRunning) { swElapsedTime += millis() - swStartTime; swRunning = false; }
          else { swElapsedTime = 0; }
        }
      }
    }

    // ===== TIMER =====
    else if (currentScreen == SCREEN_TIMER) {
      if (buttonJustPressed[0]) { if (tmFocus > 0) tmFocus = 0; }
      if (buttonJustPressed[1]) { if (tmFocus == 0) tmFocus = 1; }
      if (buttonJustPressed[2]) {
        if (tmMode == TM_SETTING) {
          if (tmFocus == 2) tmFocus = 1;
          else if (tmFocus == 3) tmFocus = 2;
          else if (tmFocus == 1) tmFocus = 0;
        } else if (tmMode == TM_READY) {
          if (tmFocus == 3) tmFocus = 1;
          else if (tmFocus == 1) tmFocus = 0;
        }
      }
      if (buttonJustPressed[3]) {
        if (tmMode == TM_SETTING) {
          if (tmFocus == 1) tmFocus = 2;
          else if (tmFocus == 2) tmFocus = 3;
        } else if (tmMode == TM_READY) {
          if (tmFocus == 1) tmFocus = 3;
        }
      }

      // Long-press SET to confirm
      if (buttonStates[4] && tmFocus == 2 && tmMode == TM_SETTING) {
        if (tmSetPressStart == 0) tmSetPressStart = millis();
        if (millis() - tmSetPressStart > 2000 && !tmLongPressTriggered) {
          tmMode = TM_READY; tmFocus = 3; tmLongPressTriggered = true;
        }
      } else { tmSetPressStart = 0; tmLongPressTriggered = false; }

      if (buttonJustPressed[4]) {
        if (tmFocus == 0) { startAnimation(SCREEN_APPS_MENU, 64); }
        else if (tmMode == TM_SETTING) {
          if (tmFocus == 1) {
            if (tmActiveUnit == 0) tmHours   = (tmHours + 1) % 24;
            else if (tmActiveUnit == 1) tmMinutes = (tmMinutes + 1) % 60;
            else if (tmActiveUnit == 2) tmSeconds = (tmSeconds + 1) % 60;
          } else if (tmFocus == 2) {
            tmActiveUnit = (tmActiveUnit + 1) % 3;
          } else if (tmFocus == 3) {
            if (tmActiveUnit == 0) tmHours   = (tmHours + 23) % 24;
            else if (tmActiveUnit == 1) tmMinutes = (tmMinutes + 59) % 60;
            else if (tmActiveUnit == 2) tmSeconds = (tmSeconds + 59) % 60;
          }
        }
        else if (tmMode == TM_READY) {
          if (tmFocus == 1) tmMode = TM_SETTING;
          else if (tmFocus == 3) {
            tmRemainingMillis = ((unsigned long)tmHours * 3600 + tmMinutes * 60 + tmSeconds) * 1000;
            if (tmRemainingMillis > 0) { tmMode = TM_RUNNING; tmLastTick = millis(); }
          }
        }
        else if (tmMode == TM_RUNNING) {
          if (tmFocus == 1) tmMode = TM_READY;
        }
      }
    }

    // ===== TIMER ALERT =====
    else if (currentScreen == SCREEN_TIMER_ALERT) {
      if (anyPress) {
        noTone(PIN_BUZZER);
        tmMode = TM_SETTING;
        startAnimation(SCREEN_APPS_MENU, 64);
      }
    }

    // ===== WiFi CONFIRM (turn on WiFi?) =====
    else if (currentScreen == SCREEN_WIFI_CONFIRM) {
      if (buttonJustPressed[0]) wifiConfirmCursor = -1;
      if (buttonJustPressed[1] && wifiConfirmCursor == -1) wifiConfirmCursor = 0;
      if (buttonJustPressed[3]) wifiConfirmCursor = 1;
      if (buttonJustPressed[2] && wifiConfirmCursor == 1) wifiConfirmCursor = 0;
      if (buttonJustPressed[4]) {
        if (wifiConfirmCursor == 0) {
          wifiEnabled = true; wifiStatusMsg = ""; wifiRetryCount = 0;
          WiFi.mode(WIFI_STA); WiFi.scanDelete();
          if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
            wifiScanStart = millis(); wifiScanResults = -1;
            startAnimation(SCREEN_WIFI_SCANNING, -64);
          } else {
            WiFi.disconnect(); delay(100);
            WiFi.scanNetworks(true);
            wifiScanStart = millis(); wifiScanResults = -1;
            startAnimation(SCREEN_WIFI_SCANNING, -64);
          }
        } else {
          startAnimation(SCREEN_APPS_MENU, 64);
        }
      }
    }

    // ===== WiFi RESULTS =====
    else if (currentScreen == SCREEN_WIFI_RESULTS) {
      if (buttonJustPressed[0]) { if (wifiListIndex > -1) wifiListIndex--; }
      if (buttonJustPressed[1]) { if (wifiListIndex < wifiScanResults - 1) wifiListIndex++; }
      if (buttonJustPressed[2]) { if (wifiListIndex == -1) wifiListIndex = 0; }
      if (buttonJustPressed[4]) {
        if (wifiListIndex == -1) { WiFi.disconnect(); startAnimation(SCREEN_APPS_MENU, 64); }
        else {
          wifiTargetSSID  = WiFi.SSID(wifiListIndex);
          wifiPassword    = findSavedPwd(wifiTargetSSID);
          kbInputBuffer   = wifiPassword;
          wifiStatusMsg   = "";
          wifiRetryCount  = 0;
          wifiPasswordCursor = -1;
          startAnimation(SCREEN_WIFI_PASSWORD, -64);
        }
      }
    }

    // ===== WiFi PASSWORD =====
    else if (currentScreen == SCREEN_WIFI_PASSWORD) {
      if (buttonJustPressed[0]) {
        if (wifiPasswordCursor == 0 || wifiPasswordCursor == 1) wifiPasswordCursor = -1;
      }
      if (buttonJustPressed[1]) { if (wifiPasswordCursor == -1) wifiPasswordCursor = 0; }
      if (buttonJustPressed[2]) { if (wifiPasswordCursor == 1) wifiPasswordCursor = 0; }
      if (buttonJustPressed[3]) { if (wifiPasswordCursor == 0) wifiPasswordCursor = 1; }
      if (buttonJustPressed[4]) {
        if (wifiPasswordCursor == -1) { WiFi.disconnect(); startAnimation(SCREEN_WIFI_RESULTS, 64); }
        else if (wifiPasswordCursor == 0) {
          kbInputBuffer = wifiPassword; currentKBMode = KB_LOWER;
          kbCursorRow = 0; kbCursorCol = 0;
          kbTextCursor   = kbInputBuffer.length();
          kbScrollOffset = 0;
          kbReturnScreen = SCREEN_WIFI_PASSWORD;
          currentScreen  = SCREEN_WIFI_KEYBOARD;
        } else if (wifiPasswordCursor == 1) {
          WiFi.begin(wifiTargetSSID.c_str(), wifiPassword.c_str());
          wifiConnecting = true;
          wifiConnectStartTime = millis();
          wifiStatusMsg = "Connecting...";
        }
      }

      if (wifiConnecting) {
        lastActivityTime = millis();
        if (WiFi.status() == WL_CONNECTED) {
          wifiConnecting = false; wifiRetryCount = 0; WiFi.setSleep(true);
          saveCredential(wifiTargetSSID, wifiPassword);
          removeForbidden(wifiTargetSSID);
          lastConnectedSSID = wifiTargetSSID;
          prefs.begin("wifi_creds", false);
          prefs.putString("lastSSID", lastConnectedSSID);
          prefs.end();
          syncTimeWithNTP();
          startConnectivityPinger();
          wifiHomeCursor = 0;
          startAnimation(SCREEN_WIFI_HOME, -64);
        } else if (WiFi.status() == WL_CONNECT_FAILED) {
          wifiConnecting = false; wifiRetryCount++;
          WiFi.disconnect();
          wifiStatusMsg = (wifiRetryCount >= 5) ? "Check signal/pass" : "Failed! Wrong PW?";
        } else if (millis() - wifiConnectStartTime > 15000) {
          wifiConnecting = false; wifiRetryCount++;
          WiFi.disconnect();
          wifiStatusMsg = (wifiRetryCount >= 5) ? "Check Router dist." : "Try Again...";
        }
      }
    }

    // ===== WiFi KEYBOARD =====
    else if (currentScreen == SCREEN_WIFI_KEYBOARD) {
      if (buttonJustPressed[0]) { if (kbCursorRow > 0) kbCursorRow--; }
      if (buttonJustPressed[1]) { if (kbCursorRow < 3) kbCursorRow++; }
      // Left/Right wrap within the current row — only Up/Down change rows.
      if (buttonJustPressed[2]) {
        kbCursorCol = (kbCursorCol > 0) ? kbCursorCol - 1 : 10;
      }
      if (buttonJustPressed[3]) {
        kbCursorCol = (kbCursorCol < 10) ? kbCursorCol + 1 : 0;
      }
      if (buttonJustPressed[4]) {
        const char* key;
        if (currentKBMode == KB_UPPER) key = kbUpper[kbCursorRow][kbCursorCol];
        else if (currentKBMode == KB_SYMBOL) key = kbSymbol[kbCursorRow][kbCursorCol];
        else key = kbLower[kbCursorRow][kbCursorCol];

        if (strcmp(key, "EN") == 0) {
          if (kbReturnScreen == SCREEN_TX_UDP_IP) udpTargetIP = kbInputBuffer;
          else if (kbReturnScreen == SCREEN_SETTINGS_MENU) {
            if (kbInputBuffer.length() > 0) {
              txDeviceName = kbInputBuffer;
              saveDeviceName(txDeviceName);
            }
          }
          else wifiPassword = kbInputBuffer;
          currentScreen = kbReturnScreen;
        } else if (strcmp(key, "CL") == 0) {
          if (kbTextCursor > 0) kbTextCursor--;
        } else if (strcmp(key, "CR") == 0) {
          if (kbTextCursor < (int)kbInputBuffer.length()) kbTextCursor++;
        // "<"/">" are the cursor-move keys in kbLower/kbUpper (renamed from
        // CL/CR). Gated to non-symbol mode so the literal "<"/">" characters in
        // kbSymbol (row 3) always just type, never get mistaken for this key.
        } else if (strcmp(key, "<") == 0 && currentKBMode != KB_SYMBOL) {
          if (kbTextCursor > 0) kbTextCursor--;
        } else if (strcmp(key, ">") == 0 && currentKBMode != KB_SYMBOL) {
          if (kbTextCursor < (int)kbInputBuffer.length()) kbTextCursor++;
        } else if (strcmp(key, "BS") == 0) {
          if (kbTextCursor > 0 && kbInputBuffer.length() > 0) {
            kbInputBuffer.remove(kbTextCursor - 1, 1);
            kbTextCursor--;
          }
        } else if (strcmp(key, "^") == 0) {
          if (currentKBMode == KB_LOWER) currentKBMode = KB_UPPER;
          else if (currentKBMode == KB_UPPER) currentKBMode = KB_SYMBOL;
          else currentKBMode = KB_LOWER;
        } else {
          // Device name flows into a char[20] packet field — cap at 19 chars
          // (+null) so nothing gets silently truncated mid-name on the wire.
          bool atCap = (kbReturnScreen == SCREEN_SETTINGS_MENU && kbInputBuffer.length() >= 19);
          if (!atCap) {
            kbInputBuffer = kbInputBuffer.substring(0, kbTextCursor) + String(key) + kbInputBuffer.substring(kbTextCursor);
            kbTextCursor += String(key).length();
          }
        }
      }
    }

    // ===== WiFi HOME (connected screen) =====
    else if (currentScreen == SCREEN_WIFI_HOME) {
      if (buttonJustPressed[0]) { if (wifiHomeCursor > -1) wifiHomeCursor--; }
      if (buttonJustPressed[1]) { if (wifiHomeCursor < 2) wifiHomeCursor++; }
      if (buttonJustPressed[2]) { if (wifiHomeCursor == -1) wifiHomeCursor = 0; }
      if (buttonJustPressed[4]) {
        if (wifiHomeCursor == -1) startAnimation(SCREEN_APPS_MENU, 64);
        else if (wifiHomeCursor == 0) { wifiToggleCursor = 1; startAnimation(SCREEN_WIFI_TOGGLE_CONFIRM, -64); }
        else if (wifiHomeCursor == 1) {
          wifiAutoCursor = wifiAutoOn ? 1 : 0;
          startAnimation(SCREEN_WIFI_AUTO_CONFIRM, -64);
        }
        else if (wifiHomeCursor == 2) {
          wifiForgetTargetSSID = WiFi.SSID();
          wifiForgetCursor = 1;
          startAnimation(SCREEN_WIFI_FORGET_CONFIRM, -64);
        }
      }
    }

    // ===== WiFi FORGET NETWORK CONFIRM =====
    else if (currentScreen == SCREEN_WIFI_FORGET_CONFIRM) {
      if (buttonJustPressed[2]) wifiForgetCursor = 0;
      if (buttonJustPressed[3]) wifiForgetCursor = 1;
      if (buttonJustPressed[4]) {
        if (wifiForgetCursor == 0) {
          addForbidden(wifiForgetTargetSSID);
          if (lastConnectedSSID == wifiForgetTargetSSID) {
            lastConnectedSSID = "";
            prefs.begin("wifi_creds", false);
            prefs.putString("lastSSID", "");
            prefs.end();
          }
          WiFi.disconnect();
          pingerStarted = false;
          internetOK = false;
          for (int i = 0; i < 5; i++) pingerStatus[i] = false;
          startAnimation(SCREEN_APPS_MENU, 64);
        } else {
          startAnimation(SCREEN_WIFI_HOME, 64);
        }
      }
    }

    // ===== WiFi TOGGLE CONFIRM =====
    else if (currentScreen == SCREEN_WIFI_TOGGLE_CONFIRM) {
      if (buttonJustPressed[2]) wifiToggleCursor = 0;
      if (buttonJustPressed[3]) wifiToggleCursor = 1;
      if (buttonJustPressed[4]) {
        if (wifiToggleCursor == 0) { wifiEnabled = false; WiFi.disconnect(); WiFi.mode(WIFI_OFF); }
        startAnimation(SCREEN_APPS_MENU, 64);
      }
    }

    // ===== WiFi AUTO ON CONFIRM =====
    else if (currentScreen == SCREEN_WIFI_AUTO_CONFIRM) {
      if (buttonJustPressed[2]) wifiAutoCursor = 0;
      if (buttonJustPressed[3]) wifiAutoCursor = 1;
      if (buttonJustPressed[4]) {
        wifiAutoOn = (wifiAutoCursor == 0);
        prefs.begin("wifi_creds", false);
        prefs.putBool("wifiAutoOn", wifiAutoOn);
        prefs.end();
        startAnimation(SCREEN_WIFI_HOME, 64);
      }
    }

    // ===== CALCULATOR =====
    else if (currentScreen == SCREEN_CALCULATOR) {
      if (buttonJustPressed[0]) calcCursorRow = (calcCursorRow > 0) ? calcCursorRow - 1 : 4;
      if (buttonJustPressed[1]) calcCursorRow = (calcCursorRow < 4) ? calcCursorRow + 1 : 0;
      if (buttonJustPressed[2]) { calcCursorCol--; if (calcCursorCol < 0) calcCursorCol = 3; }
      if (buttonJustPressed[3]) { calcCursorCol++; if (calcCursorCol > 3) calcCursorCol = 0; }

      if (buttonJustPressed[4]) {
        const char* key = calcGrid[calcCursorRow][calcCursorCol];
        String keyStr = String(key);

        if (keyStr == "<--") {
          calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
          startAnimation(SCREEN_APPS_MENU, 64);
        } else if (keyStr == "C") {
          calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
        } else if (keyStr == "DEL") {
          if (calcError) { calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false; }
          else if (calcIsOpSet) {
            if (calcInput2.length() > 0) calcInput2.remove(calcInput2.length() - 1);
            else { calcIsOpSet = false; calcOp = ' '; }
          } else {
            if (calcInput1.length() > 0) calcInput1.remove(calcInput1.length() - 1);
          }
        } else if (keyStr == "=") {
          if (!calcError && calcIsOpSet && calcInput2.length() > 0) {
            float v1 = calcInput1.toFloat(), v2 = calcInput2.toFloat(), res = 0;
            bool dz = false;
            if (calcOp == '+') res = v1 + v2;
            else if (calcOp == '-') res = v1 - v2;
            else if (calcOp == '*') res = v1 * v2;
            else if (calcOp == '/') { if (v2 != 0) res = v1 / v2; else dz = true; }
            if (dz) { calcError = true; }
            else {
              calcInput1 = String(res, 4);
              while (calcInput1.endsWith("0") && calcInput1.indexOf(".") != -1) calcInput1.remove(calcInput1.length() - 1);
              if (calcInput1.endsWith(".")) calcInput1.remove(calcInput1.length() - 1);
              calcInput2 = ""; calcOp = ' '; calcIsOpSet = false;
            }
          }
        } else if (keyStr == "+" || keyStr == "-" || keyStr == "*" || keyStr == "/") {
          char op = key[0];
          if (!calcError && calcInput1.length() > 0) {
            if (calcIsOpSet && calcInput2.length() > 0) {
              float v1 = calcInput1.toFloat(), v2 = calcInput2.toFloat(), res = 0;
              bool dz = false;
              if (calcOp == '+') res = v1 + v2;
              else if (calcOp == '-') res = v1 - v2;
              else if (calcOp == '*') res = v1 * v2;
              else if (calcOp == '/') { if (v2 != 0) res = v1 / v2; else dz = true; }
              if (dz) { calcError = true; }
              else {
                calcInput1 = String(res, 4);
                while (calcInput1.endsWith("0") && calcInput1.indexOf(".") != -1) calcInput1.remove(calcInput1.length() - 1);
                if (calcInput1.endsWith(".")) calcInput1.remove(calcInput1.length() - 1);
                calcInput2 = ""; calcOp = op; calcIsOpSet = true;
              }
            } else { calcOp = op; calcIsOpSet = true; }
          }
        } else if (keyStr == "SCI") {
          calcSciRow = 0; calcSciCol = 0;
          startAnimation(SCREEN_CALCULATOR_SCI, -64);
        } else if (keyStr != "") {
          if (calcError) { calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false; }
          if (calcIsOpSet) {
            if (keyStr == "." && calcInput2.indexOf(".") != -1) {}
            else if (calcInput2.length() < 10) calcInput2 += keyStr;
          } else {
            if (keyStr == "." && calcInput1.indexOf(".") != -1) {}
            else if (calcInput1.length() < 10) calcInput1 += keyStr;
          }
        }
      }
    }

    // ===== CALCULATOR SCI =====
    else if (currentScreen == SCREEN_CALCULATOR_SCI) {
      if (buttonJustPressed[0]) calcSciRow = (calcSciRow > 0) ? calcSciRow - 1 : 2;
      if (buttonJustPressed[1]) calcSciRow = (calcSciRow < 2) ? calcSciRow + 1 : 0;
      if (buttonJustPressed[2]) calcSciCol = (calcSciCol > 0) ? calcSciCol - 1 : 1;
      if (buttonJustPressed[3]) calcSciCol = (calcSciCol < 1) ? calcSciCol + 1 : 0;

      if (buttonJustPressed[4]) {
        const char* key = sciGrid[calcSciRow][calcSciCol];
        if (strcmp(key, "BACK") == 0) { startAnimation(SCREEN_CALCULATOR, 64); return; }

        String* activeInput = calcIsOpSet ? &calcInput2 : &calcInput1;
        float v = activeInput->toFloat();
        bool hasVal = (activeInput->length() > 0 && !calcError);
        String result = "";
        bool sciError = false;

        if (strcmp(key, "x^2") == 0) { if (hasVal) result = String(v * v, 4); }
        else if (strcmp(key, "sqrt") == 0) { if (hasVal && v >= 0) result = String(sqrt(v), 4); else sciError = true; }
        else if (strcmp(key, "pi") == 0) { result = "3.14159265"; }
        else if (strcmp(key, "x^3") == 0) { if (hasVal) result = String(v * v * v, 4); }
        else if (strcmp(key, "cbrt") == 0) {
          if (hasVal) {
            float cb = (v >= 0) ? pow(v, 1.0f / 3.0f) : -pow(-v, 1.0f / 3.0f);
            result = String(cb, 4);
          }
        }

        if (sciError) { calcError = true; }
        else if (result.length() > 0) {
          while (result.endsWith("0") && result.indexOf(".") != -1) result.remove(result.length() - 1);
          if (result.endsWith(".")) result.remove(result.length() - 1);
          if (strcmp(key, "pi") == 0) {
            if (!calcIsOpSet) calcInput1 = result;
            else calcInput2 = result;
          } else { *activeInput = result; }
        }
        startAnimation(SCREEN_CALCULATOR, 64);
      }
    }

  } // end !isAnimating

  // ---------- Render ----------
  display.clearDisplay();

  // Unified animation + static render path
  if (isAnimating) {
    // Step the animation offset toward its target
    if (animOffsetY < animTargetY) animOffsetY += 8;
    else if (animOffsetY > animTargetY) animOffsetY -= 8;

    if (animOffsetX < animTargetX) animOffsetX += 16;
    else if (animOffsetX > animTargetX) animOffsetX -= 16;

    // Snap and finish when close enough
    if (abs(animOffsetY - animTargetY) < 8 && abs(animOffsetX - animTargetX) < 16) {
      animOffsetY   = 0;
      animOffsetX   = 0;
      currentScreen = animNextScreen;
      isAnimating   = false;
    }

    // Draw outgoing screen + incoming screen side-by-side
    if (animTargetY != 0) {
      if (animTargetY < 0) {
        drawScreen(currentScreen,  animOffsetY, 0);
        drawScreen(animNextScreen, animOffsetY + 64, 0);
      } else {
        drawScreen(currentScreen,  animOffsetY, 0);
        drawScreen(animNextScreen, animOffsetY - 64, 0);
      }
    } else if (animTargetX != 0) {
      if (animTargetX < 0) {
        drawScreen(currentScreen,  0, animOffsetX);
        drawScreen(animNextScreen, 0, animOffsetX + 128);
      } else {
        drawScreen(currentScreen,  0, animOffsetX);
        drawScreen(animNextScreen, 0, animOffsetX - 128);
      }
    }
  } else {
    drawScreen(currentScreen, 0, 0);
  }

  display.display();
  delay(5);
}

// =============================================================================
// ANIMATION
// =============================================================================
void startAnimation(ScreenState next, int targetY, int targetX) {
  isAnimating   = true;
  animNextScreen = next;
  animOffsetY   = 0;
  animTargetY   = targetY;
  animOffsetX   = 0;
  animTargetX   = targetX;
}

// =============================================================================
// DRAW DISPATCH
// =============================================================================
void drawScreen(ScreenState screen, int yOffset, int xOffset) {
  // PAGE_BOOT: re-draw the boot screen content with an offset (used during slide-out animation)
  if (screen == PAGE_BOOT) {
    int16_t x1, y1; uint16_t w, h;
    display.setFont(&FreeSansBold12pt7b);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.getTextBounds("AIO", 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 22 + yOffset);
    display.print("AIO");
    display.setFont(&FreeSansBold9pt7b);
    display.getTextBounds("Transmitter", 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 44 + yOffset);
    display.print("Transmitter");
    display.setFont();
    display.setTextSize(1);
    display.getTextBounds("StrawberryOS", 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 54 + yOffset);
    display.print("StrawberryOS");
  }
  // PAGE_OPTIONS: the main hub menu
  else if (screen == PAGE_OPTIONS) drawPage2(yOffset);
  // All other app screens
  else if (screen == SCREEN_APPS_MENU)           drawAppsMenu(yOffset);
  else if (screen == SCREEN_STOPWATCH)           drawStopwatch(yOffset);
  else if (screen == SCREEN_TIMER)               drawTimer(yOffset);
  else if (screen == SCREEN_TIMER_ALERT)         drawTimerAlert(yOffset);
  else if (screen == SCREEN_WIFI_CONFIRM)        drawWifiConfirm(yOffset);
  else if (screen == SCREEN_WIFI_SCANNING)       drawWifiScanning(yOffset);
  else if (screen == SCREEN_WIFI_RESULTS)        drawWifiResults(yOffset);
  else if (screen == SCREEN_WIFI_PASSWORD)       drawWifiPassword(yOffset);
  else if (screen == SCREEN_WIFI_KEYBOARD)       drawWifiKeyboard();
  else if (screen == SCREEN_WIFI_HOME)           drawWifiHome(yOffset);
  else if (screen == SCREEN_WIFI_TOGGLE_CONFIRM) drawWifiToggleConfirm(yOffset);
  else if (screen == SCREEN_WIFI_AUTO_CONFIRM)   drawWifiAutoConfirm(yOffset);
  else if (screen == SCREEN_WIFI_FORGET_CONFIRM) drawWifiForgetConfirm(yOffset);
  else if (screen == SCREEN_WIFI_NTP)            drawWifiNTP(yOffset);
  else if (screen == SCREEN_CALCULATOR)          drawCalculator(yOffset);
  else if (screen == SCREEN_CALCULATOR_SCI)      drawCalculatorSci(yOffset);
  else if (screen == SCREEN_SETTINGS_MENU)       drawSettingsMenu(yOffset);
  else if (screen == SCREEN_SETTINGS_FREQ)       drawSettingsFreq(yOffset);
  else if (screen == SCREEN_SETTINGS_MANAGE_WIFI) drawManageWifi(yOffset);
  else if (screen == SCREEN_TX_MENU)             drawTxMenu(yOffset);
  else if (screen == SCREEN_TX_UDP_NOWIFI)       drawTxUdpNoWifi(yOffset);
  else if (screen == SCREEN_TX_UDP_MODE)         drawTxUdpMode(yOffset);
  else if (screen == SCREEN_TX_UDP_IP)           drawTxUdpIp(yOffset);
  else if (screen == SCREEN_TX_UDP_CONTROL)      drawTxUdpControl(yOffset);
  else if (screen == SCREEN_TX_SCANNING)         drawTxScanning(yOffset);
  else if (screen == SCREEN_TX_RESULTS)          drawTxResults(yOffset);
  else if (screen == SCREEN_TX_PAIRING)          drawTxPairing(yOffset);
  else if (screen == SCREEN_TX_CONNECTED)        drawTxConnected(yOffset);
  else if (screen == SCREEN_SC_SCANNING)         drawScScanning(yOffset);
  else if (screen == SCREEN_SC_RESULTS)          drawScResults(yOffset);
  else if (screen == SCREEN_SC_CONNECTING1)      drawScConnecting1(yOffset);
  else if (screen == SCREEN_SC_PIN_ENTRY)        drawScPinEntry(yOffset);
  else if (screen == SCREEN_SC_CONNECTING2)      drawScConnecting2(yOffset);
  else if (screen == SCREEN_SC_CONNECTED)        drawScConnected(yOffset);
  else if (screen == SCREEN_TX_STATUS)           drawTxStatus(yOffset, xOffset);
  else if (screen == SCREEN_TX_CONTROL)          drawTxControl(yOffset, xOffset);
  else if (screen == SCREEN_TX_TELEMETRY)        drawTxTelemetry(yOffset, xOffset);
  else if (screen == SCREEN_TX_TERMINATE)        drawTxTerminate(yOffset);
  else if (screen == SCREEN_TX_TERMINATING)      drawTxTerminating(yOffset);
  else if (screen == SCREEN_TX_FAILED)          drawTxFailed(yOffset);
  else if (screen == SCREEN_TX_RX_LOST)         drawTxRxLost(yOffset);
  else if (screen == SCREEN_TX_WIFI_DISABLE)    drawTxWifiDisable(yOffset);
  else if (screen == SCREEN_SHUTDOWN_CONFIRM)   drawShutdownConfirm(yOffset);
}

// =============================================================================
// DRAW — BOOT SCREEN
// =============================================================================
void drawBootScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1; uint16_t w, h;

  display.setFont(&FreeSansBold12pt7b);
  display.setTextSize(1);
  display.getTextBounds("AIO", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 22);
  display.print("AIO");

  display.setFont(&FreeSansBold9pt7b);
  display.getTextBounds("Transmitter", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 44);
  display.print("Transmitter");

  display.setFont();
  display.setTextSize(1);
  display.getTextBounds("StrawberryOS", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 54);
  display.print("StrawberryOS");

  display.display();
}

// =============================================================================
// DRAW — STATUS BAR  (replaces the old drawStatusBar — always on top)
// =============================================================================
void drawStatusBar(int yOffset) {
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    // Connected — static icon
    display.drawBitmap(0, 2 + yOffset, wifi_bmp, 8, 8, SSD1306_WHITE);
  } else if (wifiEnabled) {
    // Searching — blink the icon every 500ms
    if ((millis() / 500) % 2 == 0)
      display.drawBitmap(0, 2 + yOffset, wifi_bmp, 8, 8, SSD1306_WHITE);
  }
  // WiFi off: nothing drawn

  if (bleStatusActive) {
    display.drawBitmap(16, 2 + yOffset, bt_bmp, 8, 8, SSD1306_WHITE);
  }

  if (espNowStatusActive) {
    display.setCursor(30, 2 + yOffset);
    display.print("E");
  }

  drawBatteryIcon(111, 2 + yOffset, 75);

  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
}

// =============================================================================
// DRAW — HELPER: fake bold spaced text
// =============================================================================
void printSpacedFakeBold(String text, int startX, int startY, int spacing) {
  int currentX = startX;
  for (int i = 0; i < (int)text.length(); i++) {
    display.setCursor(currentX, startY);
    display.print(text[i]);
    display.setCursor(currentX + 1, startY);
    display.print(text[i]);
    currentX += 6 + spacing;
  }
}

// =============================================================================
// DRAW — PAGE_OPTIONS (Main Hub: TRANSMIT | APPS | SETTINGS)
// =============================================================================
void drawPage2(int yOffset) {
  drawStatusBar(yOffset);

  display.setFont();
  display.setTextSize(1);

  int16_t x1, y1; uint16_t w, h;

  // TRANSMITTER box (top-left)
  int b1_x = 2, b1_y = 16 + yOffset, b1_w = 60, b1_h = 28;
  if (page2Focus == 0) {
    display.fillRect(b1_x, b1_y, b1_w, b1_h, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRect(b1_x, b1_y, b1_w, b1_h, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }
  int tx1 = b1_x + (b1_w - 56) / 2;
  printSpacedFakeBold("TRANSMIT", tx1, b1_y + 10, 1);

  // APPS box (top-right)
  int b2_x = 66, b2_y = 16 + yOffset, b2_w = 60, b2_h = 28;
  if (page2Focus == 1) {
    display.fillRect(b2_x, b2_y, b2_w, b2_h, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRect(b2_x, b2_y, b2_w, b2_h, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }
  int tx2 = b2_x + (b2_w - 28) / 2;
  printSpacedFakeBold("APPS", tx2, b2_y + 10, 1);

  // SETTINGS box (bottom-center)
  int b3_x = 34, b3_y = 48 + yOffset, b3_w = 60, b3_h = 16;
  if (page2Focus == 2) {
    display.fillRect(b3_x, b3_y, b3_w, b3_h, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRect(b3_x, b3_y, b3_w, b3_h, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }
  display.getTextBounds("SETTINGS", 0, 0, &x1, &y1, &w, &h);
  display.setCursor(b3_x + (b3_w - w) / 2, b3_y + (b3_h - h) / 2);
  display.print("SETTINGS");
}

// =============================================================================
// DRAW — SHARED HEADER (used by all app screens)
// =============================================================================
void drawHeader(int yOffset, const char* appName, bool backFocused, bool showBackArrow) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);

  if (appName != nullptr) {
    display.setTextSize(1);

    if (showBackArrow) {
      if (backFocused) {
        display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      display.setCursor(2, 2 + yOffset);
      display.print("<--");
    }

    display.setTextColor(SSD1306_WHITE);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(appName, 0, 0, &x1, &y1, &w, &h);
    int titleX = (128 - w) / 2 - x1;
    if (showBackArrow && titleX < 26) titleX = 26; // avoid colliding with "<--"
    display.setCursor(titleX, 12 / 2 - h / 2 - y1 + yOffset);
    display.print(appName);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
}

// =============================================================================
// DRAW — APPS MENU
// =============================================================================
void drawAppsMenu(int yOffset) {
  drawHeader(yOffset, "APPS", false, false);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int itemH = 12;
  int listY = 16 + yOffset;

  const int visibleItems = 4;
  if (appMenuIndex > -1) {
    if (appMenuIndex < appMenuScroll) appMenuScroll = appMenuIndex;
    if (appMenuIndex >= appMenuScroll + visibleItems) appMenuScroll = appMenuIndex - visibleItems + 1;
  }

  for (int i = 0; i < visibleItems; i++) {
    int appIdx = appMenuScroll + i;
    if (appIdx >= NUM_APP_ITEMS) break;
    int y = listY + (i * itemH);
    if (appIdx == appMenuIndex) {
      display.fillRect(0, y - 1, 128, itemH, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    display.print(appItems[appIdx]);
  }
  display.setTextColor(SSD1306_WHITE);
}

// =============================================================================
// DRAW — SETTINGS
// =============================================================================
void drawSettingsMenu(int yOffset) {
  drawHeader(yOffset, "SETTINGS", (settingsMenuIndex == -1), true);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int itemH = 12;
  int listY = 16 + yOffset;

  const int visibleItems = 4;
  if (settingsMenuIndex > -1) {
    if (settingsMenuIndex < settingsMenuScroll) settingsMenuScroll = settingsMenuIndex;
    if (settingsMenuIndex >= settingsMenuScroll + visibleItems) settingsMenuScroll = settingsMenuIndex - visibleItems + 1;
  }

  int freqVals[3] = {wifiUdpFreqHz, espNowFreqHz, sconnectFreqHz};

  for (int i = 0; i < visibleItems; i++) {
    int idx = settingsMenuScroll + i;
    if (idx >= NUM_SETTINGS_ITEMS) break;
    int y = listY + (i * itemH);
    if (idx == settingsMenuIndex) {
      display.fillRect(0, y - 1, 128, itemH, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    if (idx == 3) {
      // Long device names overflow the 128px row and Adafruit_GFX wraps them
      // onto the next line, overlapping the row below. Truncate to fit, with
      // the last visible char swapped for "." as a clipped-text indicator.
      String label = String(settingsItems[idx]) + ": " + txDeviceName;
      int16_t lx1, ly1; uint16_t lw, lh;
      display.getTextBounds(label, 0, 0, &lx1, &ly1, &lw, &lh);
      while (lw > 118 && label.length() > 1) {
        label.remove(label.length() - 1);
        label.setCharAt(label.length() - 1, '.');
        display.getTextBounds(label, 0, 0, &lx1, &ly1, &lw, &lh);
      }
      display.print(label);
    }
    else if (idx == 4) display.print(String(settingsItems[idx]) + ": " + (manageWifi ? "YES" : "NO"));
    else display.print(String(settingsItems[idx]) + ": " + String(freqVals[idx]) + "Hz");
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawSettingsFreq(int yOffset) {
  drawHeader(yOffset, settingsItems[settingsFreqTarget], (settingsFreqFocus == -1));

  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Select Frequency", 22 + yOffset, 1);

  for (int i = 0; i < NUM_FREQ_OPTIONS; i++) {
    int x = 4 + i * 41;
    char label[8];
    snprintf(label, sizeof(label), "%dHz", freqOptions[i]);
    drawBoxedCenteredText(display, label, x, 38 + yOffset, 37, 18, (settingsFreqFocus == i));
  }
}

// =============================================================================
// DRAW — MANAGE WIFI (scrollable word-wrapped info + YES/NO toggle)
// =============================================================================
// Word-wraps `text` into a `colW`-px column, left-aligned, single space between
// words — no word is ever split across lines. Returns the block height in px;
// pass draw=false to measure only.
int renderJustifiedText(const String &text, int xLeft, int colW, int yTop, bool draw) {
  const int charW = 6, lineH = 9, spaceW = 6;
  const int maxChars = colW / charW;        // chars per line incl. single spaces

  // Split into words.
  String words[48];
  int wordCount = 0;
  int i = 0, L = text.length();
  while (i < L && wordCount < 48) {
    while (i < L && text[i] == ' ') i++;
    if (i >= L) break;
    int j = i;
    while (j < L && text[j] != ' ') j++;
    words[wordCount++] = text.substring(i, j);
    i = j;
  }

  int line = 0, w = 0;
  while (w < wordCount) {
    // Greedily pack as many words as fit (with single spaces) onto this line.
    int start = w;
    int charsUsed = words[w].length();
    w++;
    while (w < wordCount && (charsUsed + 1 + (int)words[w].length()) <= maxChars) {
      charsUsed += 1 + words[w].length();
      w++;
    }
    int end = w;                       // exclusive

    if (draw) {
      int y = yTop + line * lineH;
      int x = xLeft;
      for (int k = start; k < end; k++) {
        display.setCursor(x, y);
        display.print(words[k]);
        x += words[k].length() * charW + spaceW;
      }
    }
    line++;
  }
  return line * lineH;
}

const int MANAGE_WIFI_LINE_H = 9;
const int MANAGE_WIFI_COL_W  = 124;

// Number of wrapped lines MANAGE_WIFI_TEXT occupies — shared by the input
// handler (to know where the button-row stop is) and the draw fn (scroll math).
int manageWifiTotalLines() {
  return renderJustifiedText(MANAGE_WIFI_TEXT, 0, MANAGE_WIFI_COL_W, 0, false) / MANAGE_WIFI_LINE_H;
}

void drawManageWifi(int yOffset) {
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const int xLeft = 2;
  const int contentTop = 14;                       // first row below the header line
  const int viewH = SCREEN_HEIGHT - contentTop;

  // Lay out: justified paragraph, then prompt, then the YES/NO row.
  int textH    = renderJustifiedText(MANAGE_WIFI_TEXT, xLeft, MANAGE_WIFI_COL_W, 0, false);
  int promptY  = textH + 4;
  int btnY     = promptY + 12;
  int contentH = btnY + 16;
  int maxScroll = max(0, contentH - viewH);

  // Each Up/Down press moves manageWifiFocusLine by one stop (one text line);
  // scroll the view by exactly one line's worth of pixels to follow it. The
  // button row (focusLine == totalLines) is clamped to the bottom of content.
  int target = constrain(manageWifiFocusLine * MANAGE_WIFI_LINE_H, 0, maxScroll);
  if (manageWifiScroll < target)      manageWifiScroll = min(manageWifiScroll + 4, target);
  else if (manageWifiScroll > target) manageWifiScroll = max(manageWifiScroll - 4, target);

  int baseY = contentTop + yOffset - manageWifiScroll;

  renderJustifiedText(MANAGE_WIFI_TEXT, xLeft, MANAGE_WIFI_COL_W, baseY, true);

  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Turn On Manage WiFi?", baseY + promptY, 1);

  drawBoxedCenteredText(display, "YES", 16, baseY + btnY, 40, 14, (manageWifiCursor == 0));
  drawBoxedCenteredText(display, "NO",  72, baseY + btnY, 40, 14, (manageWifiCursor == 1));

  // Mask the header band and redraw it on top so scrolled text never bleeds over.
  display.fillRect(0, yOffset, 128, 13, SSD1306_BLACK);
  drawHeader(yOffset, "Manage WiFi", (manageWifiFocusLine == -1));
}

// =============================================================================
// DRAW — STOPWATCH
// =============================================================================
void drawStopwatch(int yOffset) {
  drawHeader(yOffset, "STOPWATCH", (swFocus == 0));

  unsigned long currentTotal = swElapsedTime;
  if (swRunning) currentTotal += millis() - swStartTime;

  unsigned long ms        = (currentTotal % 1000) / 10;
  unsigned long totalSecs = currentTotal / 1000;
  unsigned long m         = totalSecs / 60;
  unsigned long s         = totalSecs % 60;

  char timeStr[10];
  sprintf(timeStr, "%02lu:%02lu", m, s);
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2 - x1, 10 + (42 - h) / 2 - y1 + yOffset);
  display.print(timeStr);

  char msStr[4];
  sprintf(msStr, "%02lu", ms);
  display.setTextSize(1);
  display.setCursor(SCREEN_WIDTH - 16, 16 + yOffset);
  display.print(msStr);

  const char* leftBtn = swRunning ? "PAUSE" : "START";
  int16_t lx1, ly1; uint16_t lw, lh;
  display.getTextBounds(leftBtn, 0, 0, &lx1, &ly1, &lw, &lh);
  drawBoxedCenteredText(display, leftBtn, (64 - (lw + 8)) / 2, SCREEN_HEIGHT - 12 + yOffset, lw + 8, 11, (swFocus == 1));

  const char* rightBtn = swRunning ? "STOP" : "RESET";
  int16_t rx1, ry1; uint16_t rw, rh;
  display.getTextBounds(rightBtn, 0, 0, &rx1, &ry1, &rw, &rh);
  drawBoxedCenteredText(display, rightBtn, 64 + (64 - (rw + 8)) / 2, SCREEN_HEIGHT - 12 + yOffset, rw + 8, 11, (swFocus == 2));
}

// =============================================================================
// DRAW — TIMER
// =============================================================================
void drawTimer(int yOffset) {
  drawHeader(yOffset, "TIMER", (tmFocus == 0));

  char timeStr[10];
  sprintf(timeStr, "%02d:%02d:%02d", tmHours, tmMinutes, tmSeconds);
  if (tmMode == TM_RUNNING) {
    unsigned long secs = tmRemainingMillis / 1000;
    sprintf(timeStr, "%02lu:%02lu:%02lu", secs / 3600, (secs % 3600) / 60, secs % 60);
  }

  display.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int16_t tx = (128 - w) / 2 - x1;
  int16_t ty = 16 + (28 - h) / 2 - y1 + yOffset;
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(tx, ty);
  display.print(timeStr);

  if (tmMode == TM_SETTING) {
    int charW = 12;
    int16_t unitX = tx + (tmActiveUnit * 3 * charW);
    display.drawFastHLine(unitX, ty + h + 2, 22, SSD1306_WHITE);
  }

  if (tmMode == TM_SETTING) {
    drawBoxedCenteredText(display, "+",   10, 48 + yOffset, 20, 11, (tmFocus == 1));
    drawBoxedCenteredText(display, "SET", 44, 48 + yOffset, 40, 11, (tmFocus == 2));
    drawBoxedCenteredText(display, "-",   98, 48 + yOffset, 20, 11, (tmFocus == 3));
  } else if (tmMode == TM_READY || tmMode == TM_RUNNING) {
    const char* mainBtn = (tmMode == TM_RUNNING) ? "RESET" : "START";
    drawBoxedCenteredText(display, "SET",    15, 48 + yOffset, 40, 11, (tmFocus == 1));
    drawBoxedCenteredText(display, mainBtn,  70, 48 + yOffset, 45, 11, (tmFocus == 3));
  }
}

// =============================================================================
// DRAW — TIMER ALERT
// =============================================================================
void drawTimerAlert(int yOffset) {
  bool flash = (millis() % 1000 < 500);
  if (flash) { display.fillRect(0, 0, 128, 64, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
  else { display.setTextColor(SSD1306_WHITE); }

  drawCenteredText(display, "TIMER DONE", 15 + yOffset, 2);
  drawCenteredText(display, "Press any key", 40 + yOffset, 1);

  int x = 51, y = 51 + yOffset, w = 26, h = 11;
  if (flash) { display.fillRect(x, y, w, h, SSD1306_BLACK); display.setTextColor(SSD1306_WHITE); }
  else       { display.fillRect(x, y, w, h, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds("OK", 0, 0, &x1, &y1, &tw, &th);
  display.setCursor(x + (w - tw) / 2 - x1, y + (h - th) / 2 - y1);
  display.print("OK");
  display.setTextColor(SSD1306_WHITE);
}

// =============================================================================
// DRAW — CALCULATOR (Standard)
// =============================================================================
void drawCalculator(int yOffset) {
  String disp;
  if (calcError) { disp = "Err: Div/0"; }
  else {
    disp = (calcInput1.length() > 0) ? calcInput1 : "0";
    if (calcIsOpSet) {
      disp += ' '; disp += calcOp;
      if (calcInput2.length() > 0) { disp += ' '; disp += calcInput2; }
    }
  }
  if ((int)disp.length() > 18) disp = disp.substring(disp.length() - 18);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int dispX = 128 - (int)disp.length() * 6 - 1;
  if (dispX < 0) dispX = 0;
  display.setCursor(dispX, 3 + yOffset);
  display.print(disp);
  display.drawFastHLine(0, 13 + yOffset, 128, SSD1306_WHITE);

  const int GY = 14 + yOffset, CW = 32, CH = 10;
  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 4; c++) {
      const char* lbl = calcGrid[r][c];
      if (lbl[0] == '\0') continue;
      int cx = c * CW, cy = GY + r * CH;
      bool sel = (r == calcCursorRow && c == calcCursorCol);
      if (sel) { display.fillRect(cx, cy, CW, CH, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
      else      { display.drawRect(cx, cy, CW, CH, SSD1306_WHITE); display.setTextColor(SSD1306_WHITE);
                  if (c == 3) display.drawFastVLine(cx + 1, cy, CH, SSD1306_WHITE); }
      int tw = strlen(lbl) * 6;
      display.setCursor(cx + (CW - tw) / 2, cy + (CH - 7) / 2);
      display.print(lbl);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

// =============================================================================
// DRAW — CALCULATOR (Scientific)
// =============================================================================
void drawCalculatorSci(int yOffset) {
  String activeVal = calcIsOpSet ? calcInput2 : calcInput1;
  if (activeVal.length() == 0) activeVal = "0";
  if (calcError) activeVal = "Error";

  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(1, 3 + yOffset);
  display.print("SCI");

  int valX = 128 - (int)activeVal.length() * 6 - 1;
  if (valX < 25) valX = 25;
  display.setCursor(valX, 3 + yOffset);
  display.print(activeVal);
  display.drawFastHLine(0, 13 + yOffset, 128, SSD1306_WHITE);

  const int GY = 15 + yOffset, CW = 64, CH = 16;
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 2; c++) {
      int cx = c * CW, cy = GY + r * CH;
      bool sel = (r == calcSciRow && c == calcSciCol);
      const char* lbl = sciGrid[r][c];
      if (lbl[0] == '\0') continue;
      if (sel) { display.fillRect(cx, cy, CW, CH, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
      else      { display.drawRect(cx, cy, CW, CH, SSD1306_WHITE); display.setTextColor(SSD1306_WHITE); }
      int tw = strlen(lbl) * 6;
      display.setCursor(cx + (CW - tw) / 2, cy + (CH - 7) / 2);
      display.print(lbl);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

// =============================================================================
// DRAW — WiFi CONFIRM
// =============================================================================
void drawWifiConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiConfirmCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "WiFi is OFF", 22 + yOffset, 1);
  drawCenteredText(display, "Turn it on?",  33 + yOffset, 1);
  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiConfirmCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiConfirmCursor == 1));
}

// =============================================================================
// DRAW — WiFi SCANNING
// =============================================================================
void drawWifiScanning(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "SCANNING...", 30 + yOffset, 1);
  int dots = ((millis() - wifiScanStart) / 500) % 4;
  String dotStr = "";
  for (int i = 0; i < dots; i++) dotStr += ".";
  drawCenteredText(display, dotStr, 42 + yOffset, 1);
}

// =============================================================================
// DRAW — WiFi RESULTS
// =============================================================================
void drawWifiResults(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiListIndex == -1));
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  if (wifiScanResults <= 0) {
    drawCenteredText(display, "No networks found", 30 + yOffset, 1);
    return;
  }

  const int visibleItems = 4;
  if (wifiListIndex > -1) {
    if (wifiListIndex < wifiListScroll) wifiListScroll = wifiListIndex;
    if (wifiListIndex >= wifiListScroll + visibleItems) wifiListScroll = wifiListIndex - visibleItems + 1;
  }

  for (int i = 0; i < visibleItems; i++) {
    int netIdx = wifiListScroll + i;
    if (netIdx >= wifiScanResults) break;
    int y = 16 + (i * 12) + yOffset;
    bool selected = (wifiListIndex == netIdx);
    if (selected) { display.fillRect(0, y - 1, 122, 12, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else          { display.setTextColor(SSD1306_WHITE); }
    display.setCursor(4, y);
    String ssid = WiFi.SSID(netIdx);
    if (ssid.length() == 0) ssid = "(hidden)";
    if (ssid.length() > 17) ssid = ssid.substring(0, 16) + "~";
    display.print(ssid);

    int rssi = WiFi.RSSI(netIdx);
    int bars = (rssi > -60) ? 3 : (rssi > -75) ? 2 : 1;
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(118, y);
    display.print(bars == 3 ? ":" : bars == 2 ? "." : ",");
  }
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(true);

  if (wifiScanResults > visibleItems) {
    int trackH = 48;
    int sbH    = max(4, (visibleItems * trackH) / wifiScanResults);
    int sbY    = 16 + ((wifiListScroll * (trackH - sbH)) / (wifiScanResults - visibleItems));
    display.drawFastVLine(126, 16 + yOffset, trackH, SSD1306_WHITE);
    display.fillRect(125, sbY + yOffset, 3, sbH, SSD1306_WHITE);
  }
}

// =============================================================================
// DRAW — WiFi PASSWORD
// =============================================================================
void drawWifiPassword(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiPasswordCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  display.setCursor(4, 16 + yOffset);
  String ssid = wifiTargetSSID;
  if (ssid.length() > 18) ssid = ssid.substring(0, 17) + "~";
  display.print(ssid);
  display.setTextWrap(true);

  display.drawRect(4, 25 + yOffset, 120, 13, SSD1306_WHITE);
  display.setCursor(8, 28 + yOffset);
  String pw = wifiPassword;
  if (pw.length() > 15) pw = pw.substring(pw.length() - 15);
  if (pw.length() == 0) { display.setTextColor(0xAAAA); display.print("[tap to enter]"); }
  else display.print(pw);
  display.setTextColor(SSD1306_WHITE);

  if (wifiStatusMsg.length() > 0) {
    display.setCursor(4, 40 + yOffset);
    display.print(wifiStatusMsg);
  }

  drawBoxedCenteredText(display, "KEYBOARD", 4,  51 + yOffset, 54, 12, (wifiPasswordCursor == 0));
  drawBoxedCenteredText(display, "CONNECT",  62, 51 + yOffset, 60, 12, (wifiPasswordCursor == 1));
}

// =============================================================================
// DRAW — WiFi KEYBOARD
// =============================================================================
void drawWifiKeyboard() {
  display.setTextColor(SSD1306_WHITE);
  display.drawRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setCursor(4, 3);

  if (kbTextCursor < kbScrollOffset) kbScrollOffset = kbTextCursor;
  if (kbTextCursor > kbScrollOffset + 17) kbScrollOffset = kbTextCursor - 17;

  String disp = kbInputBuffer.substring(kbScrollOffset);
  if (disp.length() > 18) disp = disp.substring(0, 18);

  if (kbInputBuffer.length() == 0) {
    display.setTextColor(0x5555);
    if (kbReturnScreen == SCREEN_SETTINGS_MENU) display.print("Type device name...");
    else if (kbReturnScreen == SCREEN_TX_UDP_IP) display.print("Type IP address...");
    else display.print("Type password...");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.print(disp);
  }

  if ((millis() / 500) % 2 == 0) {
    int cursorPx = 4 + ((kbTextCursor - kbScrollOffset) * 6);
    display.drawFastVLine(cursorPx, 2, 9, SSD1306_WHITE);
  }

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 11; c++) {
      const char* key;
      if (currentKBMode == KB_UPPER)   key = kbUpper[r][c];
      else if (currentKBMode == KB_SYMBOL) key = kbSymbol[r][c];
      else key = kbLower[r][c];

      int x = (c * 128) / 11;
      int w = ((c + 1) * 128) / 11 - x;
      int y = 13 + (r * 51) / 4;
      int h = (13 + ((r + 1) * 51) / 4) - y;

      drawBoxedCenteredText(display, key, x, y, w, h, (r == kbCursorRow && c == kbCursorCol));
    }
  }
}

// =============================================================================
// DRAW — WiFi HOME (connected panel)
// =============================================================================
void drawWifiHome(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiHomeCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  display.setCursor(4, 16 + yOffset);
  display.print("Internet: "); display.print(internetOK ? "OK" : "OFFLINE");
  display.setCursor(4, 25 + yOffset);
  display.print("SSID: ");
  String ssid = WiFi.SSID();
  if (ssid.length() > 12) ssid = ssid.substring(0, 11) + "~";
  display.print(ssid);
  display.setTextWrap(true);

  String autoLabel = String("Auto On: ") + (wifiAutoOn ? "YES" : "NO");
  const char* opts[3];
  opts[0] = "WiFi: ON";
  opts[1] = autoLabel.c_str();
  opts[2] = "Forget Network";
  for (int i = 0; i < 3; i++) {
    int y = 36 + (i * 10) + yOffset;
    bool sel = (wifiHomeCursor == i);
    if (sel) { display.fillRect(0, y - 1, 128, 10, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else      { display.setTextColor(SSD1306_WHITE); }
    display.setCursor(4, y);
    display.print(opts[i]);
  }
  display.setTextColor(SSD1306_WHITE);
}

// =============================================================================
// DRAW — WiFi TOGGLE CONFIRM
// =============================================================================
void drawWifiToggleConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Turn off WiFi?", 22 + yOffset, 1);
  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiToggleCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiToggleCursor == 1));
}

// =============================================================================
// DRAW — WiFi AUTO ON CONFIRM
// =============================================================================
void drawWifiAutoConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Auto On at Boot?", 22 + yOffset, 1);
  String current = String("Currently: ") + (wifiAutoOn ? "YES" : "NO");
  drawCenteredText(display, current, 34 + yOffset, 1);
  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiAutoCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiAutoCursor == 1));
}

// =============================================================================
// DRAW — WiFi FORGET NETWORK CONFIRM
// =============================================================================
void drawWifiForgetConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  drawCenteredText(display, "Forget Network?", 18 + yOffset, 1);

  String ssid = wifiForgetTargetSSID;
  if (ssid.length() > 18) ssid = ssid.substring(0, 17) + "~";
  String quoted = "\"" + ssid + "\"";
  drawCenteredText(display, quoted, 30 + yOffset, 1);
  display.setTextWrap(true);

  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiForgetCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiForgetCursor == 1));
}

// =============================================================================
// DRAW — WiFi NTP SYNC
// =============================================================================
void drawWifiNTP(int yOffset) {
  drawHeader(yOffset, "WiFi NTP");
  display.setTextColor(SSD1306_WHITE);

  if (ntpSyncState == 0) {
    drawCenteredText(display, "Syncing time...", 30 + yOffset, 1);
    int dots = ((millis() - ntpSyncStart) / 500) % 4;
    String dotStr = "";
    for (int i = 0; i < dots; i++) dotStr += ".";
    drawCenteredText(display, dotStr, 42 + yOffset, 1);
  } else if (ntpSyncState == 1) {
    drawCenteredText(display, "NTP Sync OK!", 30 + yOffset, 1);
  } else if (ntpSyncState == 2) {
    drawCenteredText(display, "NTP Sync Failed", 30 + yOffset, 1);
    drawCenteredText(display, "Timeout",         42 + yOffset, 1);
  }
}

// =============================================================================
// DRAW — TRANSMITTER SCREENS
// =============================================================================
// 2x2 protocol grid with smooth vertical scroll
//   row0:  ESP_NOW | SMART CONNECT
//   row1:  WiFi UDP | BLE
void drawTxMenu(int yOffset) {
  // Animate internal scroll toward the focused row
  int target = (txMenuFocus >= 2) ? -32 : 0;
  if (txMenuScroll < target)      txMenuScroll = min(txMenuScroll + 6, target);
  else if (txMenuScroll > target) txMenuScroll = max(txMenuScroll - 6, target);

  const int rowSpacing = 32, boxH = 26, boxW = 54;
  const int colX[2] = {6, 68};
  const int baseY = 15 + yOffset + txMenuScroll;

  const char* l1[4] = {"ESP_NOW", "SMART",   "WiFi UDP", "BLE"};
  const char* l2[4] = {"",        "CONNECT", "",         ""  };

  int16_t x1, y1; uint16_t w, h;
  for (int i = 0; i < 4; i++) {
    int row = i / 2, col = i % 2;
    int bx = colX[col], by = baseY + row * rowSpacing;
    bool focused = (txMenuFocus == i);
    if (focused) { display.fillRect(bx, by, boxW, boxH, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else         { display.drawRect(bx, by, boxW, boxH, SSD1306_WHITE); display.setTextColor(SSD1306_WHITE); }

    if (l2[i][0] == '\0') {
      display.getTextBounds(l1[i], 0, 0, &x1, &y1, &w, &h);
      display.setCursor(bx + (boxW - w) / 2, by + (boxH - 8) / 2);
      display.print(l1[i]);
    } else {
      display.getTextBounds(l1[i], 0, 0, &x1, &y1, &w, &h);
      display.setCursor(bx + (boxW - w) / 2, by + 6);
      display.print(l1[i]);
      display.getTextBounds(l2[i], 0, 0, &x1, &y1, &w, &h);
      display.setCursor(bx + (boxW - w) / 2, by + 16);
      display.print(l2[i]);
    }
  }
  display.setTextColor(SSD1306_WHITE);

  // Scroll hint chevron
  display.setCursor(122, (txMenuFocus >= 2 ? 14 : 54) + yOffset);
  display.print(txMenuFocus >= 2 ? "^" : "v");

  // Mask the top band and draw header on top so scrolled boxes never overlap it
  display.fillRect(0, yOffset, 128, 13, SSD1306_BLACK);
  drawHeader(yOffset, "TRANSMITTER", (txMenuFocus == -1));
}

// ---- WiFi UDP: no-WiFi error (auto-redirects to WiFi app) ----
void drawTxUdpNoWifi(int yOffset) {
  drawHeader(yOffset, "WiFi UDP");
  display.setTextColor(SSD1306_WHITE);
  // Centered block within the 13-64 content area (below the header line)
  drawCenteredText(display, "!WiFi",          17 + yOffset, 2);
  drawCenteredText(display, "Not connected",  39 + yOffset, 1);
  drawCenteredText(display, "Opening WiFi...", 53 + yOffset, 1);
}

// ---- WiFi UDP: mode select ----
void drawTxUdpMode(int yOffset) {
  drawHeader(yOffset, "WiFi UDP", (txUdpModeFocus == -1));
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Select UDP Mode", 22 + yOffset, 1);
  drawBoxedCenteredText(display, "BROADCAST", 4,  38 + yOffset, 58, 18, (txUdpModeFocus == 0));
  drawBoxedCenteredText(display, "UNICAST",   66, 38 + yOffset, 58, 18, (txUdpModeFocus == 1));
}

// ---- WiFi UDP: IP entry (unicast) ----
void drawTxUdpIp(int yOffset) {
  drawHeader(yOffset, "WiFi UDP", (txUdpIpCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  display.setCursor(4, 16 + yOffset);
  display.print("Target IP : 5000");

  display.drawRect(4, 26 + yOffset, 120, 13, SSD1306_WHITE);
  display.setCursor(8, 29 + yOffset);
  String ip = udpTargetIP;
  if (ip.length() == 0) { display.setTextColor(0xAAAA); display.print("[tap to enter]"); }
  else { if (ip.length() > 18) ip = ip.substring(ip.length() - 18); display.print(ip); }
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(true);

  drawBoxedCenteredText(display, "KEYBOARD", 4,  51 + yOffset, 54, 12, (txUdpIpCursor == 0));
  drawBoxedCenteredText(display, "START",    62, 51 + yOffset, 60, 12, (txUdpIpCursor == 1));
}

// ---- WiFi UDP: control panel (single screen — joystick gauges + dpads only) ----
void drawTxUdpControl(int yOffset) {
  display.fillRect(0, yOffset, 128, 64, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1; uint16_t w, h;
  String title = udpBroadcast ? String("UDP BCAST") : (String("UDP ") + udpTargetIP);
  if (title.length() > 21) title = title.substring(0, 21);
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 0 + yOffset);
  display.print(title);
  drawCenteredText(display, String(wifiUdpFreqHz) + "Hz  :5000", 10 + yOffset, 1);

  // Left gauge (analog 1)
  int bar1_x = 4;
  display.drawRoundRect(bar1_x, 14 + yOffset, 8, 38, 4, SSD1306_WHITE);
  int fillL = (simulated_analog_1 * 34) / 100;
  display.fillRoundRect(bar1_x + 2, 16 + (34 - fillL) + yOffset, 4, fillL, 2, SSD1306_WHITE);
  String v1 = String(simulated_analog_1);
  display.getTextBounds(v1, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(bar1_x + 4 - w / 2, 54 + yOffset); display.print(v1);

  // Right gauge (analog 2)
  int bar2_x = 116;
  display.drawRoundRect(bar2_x, 14 + yOffset, 8, 38, 4, SSD1306_WHITE);
  int fillR = (simulated_analog_2 * 34) / 100;
  display.fillRoundRect(bar2_x + 2, 16 + (34 - fillR) + yOffset, 4, fillR, 2, SSD1306_WHITE);
  String v2 = String(simulated_analog_2);
  display.getTextBounds(v2, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(bar2_x + 4 - w / 2, 54 + yOffset); display.print(v2);

  // DPAD left (cached states)
  int d1x = 24, d1y = 38 + yOffset;
  buttonStates[0] ? display.fillRect(d1x+10, d1y-10, 8, 8, SSD1306_WHITE) : display.drawRect(d1x+10, d1y-10, 8, 8, SSD1306_WHITE);
  buttonStates[1] ? display.fillRect(d1x+10, d1y+10, 8, 8, SSD1306_WHITE) : display.drawRect(d1x+10, d1y+10, 8, 8, SSD1306_WHITE);
  buttonStates[2] ? display.fillRect(d1x,    d1y,    8, 8, SSD1306_WHITE) : display.drawRect(d1x,    d1y,    8, 8, SSD1306_WHITE);
  buttonStates[3] ? display.fillRect(d1x+20, d1y,    8, 8, SSD1306_WHITE) : display.drawRect(d1x+20, d1y,    8, 8, SSD1306_WHITE);
  buttonStates[4] ? display.fillRect(d1x+10, d1y,    8, 8, SSD1306_WHITE) : display.drawRect(d1x+10, d1y,    8, 8, SSD1306_WHITE);

  // DPAD right (cached states)
  int d2x = 76, d2y = 38 + yOffset;
  dpad2States[0] ? display.fillRect(d2x+10, d2y-10, 8, 8, SSD1306_WHITE) : display.drawRect(d2x+10, d2y-10, 8, 8, SSD1306_WHITE);
  dpad2States[1] ? display.fillRect(d2x+10, d2y+10, 8, 8, SSD1306_WHITE) : display.drawRect(d2x+10, d2y+10, 8, 8, SSD1306_WHITE);
  dpad2States[2] ? display.fillRect(d2x,    d2y,    8, 8, SSD1306_WHITE) : display.drawRect(d2x,    d2y,    8, 8, SSD1306_WHITE);
  dpad2States[3] ? display.fillRect(d2x+20, d2y,    8, 8, SSD1306_WHITE) : display.drawRect(d2x+20, d2y,    8, 8, SSD1306_WHITE);
  dpad2States[4] ? display.fillRect(d2x+10, d2y,    8, 8, SSD1306_WHITE) : display.drawRect(d2x+10, d2y,    8, 8, SSD1306_WHITE);
}

// ---- WiFi UDP: build + send a 20Hz control packet ----
void sendUdpControlPacket() {
  // 10 DPAD bits (dpad1 -> 0..4, dpad2 -> 5..9) + 2 analog (0-100)
  uint16_t btns = 0;
  for (int i = 0; i < 5; i++) if (buttonStates[i]) btns |= (1 << i);
  for (int i = 0; i < 5; i++) if (dpad2States[i]) btns |= (1 << (i + 5));

  uint8_t pkt[6];
  pkt[0] = 0xA1;                            // packet id (control)
  pkt[1] = btns & 0xFF;
  pkt[2] = (btns >> 8) & 0xFF;
  pkt[3] = (uint8_t)simulated_analog_1;     // 0-100
  pkt[4] = (uint8_t)simulated_analog_2;     // 0-100
  pkt[5] = udpBroadcast ? 1 : 0;            // 1=Broadcast, 0=Unicast — lets the
                                             // receiver tell mode apart reliably,
                                             // since relying on which socket the
                                             // OS delivers the packet to is flaky.

  IPAddress dest;
  if (udpBroadcast) {
    IPAddress ip = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    dest = IPAddress((ip[0] & mask[0]) | (~mask[0] & 0xFF),
                     (ip[1] & mask[1]) | (~mask[1] & 0xFF),
                     (ip[2] & mask[2]) | (~mask[2] & 0xFF),
                     (ip[3] & mask[3]) | (~mask[3] & 0xFF));
  } else {
    if (!dest.fromString(udpTargetIP)) return; // invalid IP -> skip this frame
  }

  udp.beginPacket(dest, UDP_TX_PORT);
  udp.write(pkt, sizeof(pkt));
  udp.endPacket();
}

void drawTxScanning(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  display.setCursor(120, 2 + yOffset);
  display.print("E");

  drawCenteredText(display, "SCANNING...", 28 + yOffset, 1);

  // Windows-style indeterminate loading bar (ping-pong chunk)
  const int barX = 18, barW = 92, barH = 8;
  const int barY = 46 + yOffset;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);

  const int chunkW = 26;
  unsigned long t = millis() % 1800;
  float phase = t / 900.0f;            // 0..2
  if (phase > 1.0f) phase = 2.0f - phase; // ping-pong 0..1..0
  int travel = barW - 4 - chunkW;
  int chunkX = barX + 2 + (int)(phase * travel);
  display.fillRect(chunkX, barY + 2, chunkW, barH - 4, SSD1306_WHITE);
}

void drawTxResults(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);

  if (txResultIndex == -1) {
    display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }
  display.setCursor(2, 2 + yOffset);
  display.print("<--");

  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  display.setCursor(120, 2 + yOffset);
  display.print("E");

  if (scanResultCount == 0) {
    drawCenteredText(display, "No devices found", 30 + yOffset, 1);
  } else {
    int maxVisible = 5;
    if (txResultIndex >= txResultScroll + maxVisible) txResultScroll = txResultIndex - maxVisible + 1;
    if (txResultIndex >= 0 && txResultIndex < txResultScroll) txResultScroll = txResultIndex;
    for (int i = 0; i < maxVisible && (txResultScroll + i) < scanResultCount; i++) {
      int idx = txResultScroll + i;
      int y = 16 + (i * 10) + yOffset;
      if (idx == txResultIndex) {
        display.fillRect(0, y - 1, 128, 10, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      String name = scanResults[idx].name;
      int16_t nx1, ny1; uint16_t nw, nh;
      display.getTextBounds(name, 0, 0, &nx1, &ny1, &nw, &nh);
      display.setCursor((128 - nw) / 2, y);
      display.print(name);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawTxPairing(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 2 + yOffset);
  display.print("<--");
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  display.setCursor(120, 2 + yOffset);
  display.print("E");

  drawCenteredText(display, "Pairing With", 18 + yOffset, 1);
  drawCenteredText(display, txTargetName, 28 + yOffset, 1);

  if (txHandshakeStep <= 1) {
    String msg = "Hello " + String(txHandshakeAttempt) + "/3";
    drawCenteredText(display, msg, 40 + yOffset, 1);
  } else {
    drawCenteredText(display, "Waiting CONN_OK", 40 + yOffset, 1);
  }

  const int barX = 18, barW = 92, barH = 8;
  const int barY = 52 + yOffset;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  const int chunkW = 26;
  unsigned long t = millis() % 1800;
  float phase = t / 900.0f;
  if (phase > 1.0f) phase = 2.0f - phase;
  int travel = barW - 4 - chunkW;
  int chunkX = barX + 2 + (int)(phase * travel);
  display.fillRect(chunkX, barY + 2, chunkW, barH - 4, SSD1306_WHITE);
}

void drawTxConnected(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  
  drawCenteredText(display, "CONNECTED!", 20 + yOffset, 1);
  
  // Big checkmark
  display.fillCircle(64, 44 + yOffset, 14, SSD1306_WHITE);
  // draw inner black checkmark
  for(int i=0; i<3; i++) {
    display.drawLine(58, 44+i + yOffset, 62, 48+i + yOffset, SSD1306_BLACK);
    display.drawLine(62, 48+i + yOffset, 70, 38+i + yOffset, SSD1306_BLACK);
  }
}

// =============================================================================
// DRAW — SMART CONNECT SCREENS
// =============================================================================
void drawScScanning(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  display.setCursor(120, 2 + yOffset);
  display.print("S");

  drawCenteredText(display, "SCANNING...", 28 + yOffset, 1);

  const int barX = 18, barW = 92, barH = 8;
  const int barY = 46 + yOffset;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);

  const int chunkW = 26;
  unsigned long t = millis() % 1800;
  float phase = t / 900.0f;
  if (phase > 1.0f) phase = 2.0f - phase;
  int travel = barW - 4 - chunkW;
  int chunkX = barX + 2 + (int)(phase * travel);
  display.fillRect(chunkX, barY + 2, chunkW, barH - 4, SSD1306_WHITE);
}

void drawScResults(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);

  if (txResultIndex == -1) {
    display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }
  display.setCursor(2, 2 + yOffset);
  display.print("<--");

  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  display.setCursor(120, 2 + yOffset);
  display.print("S");

  if (scanResultCount == 0) {
    drawCenteredText(display, "No devices found", 30 + yOffset, 1);
  } else {
    int maxVisible = 5;
    if (txResultIndex >= txResultScroll + maxVisible) txResultScroll = txResultIndex - maxVisible + 1;
    if (txResultIndex >= 0 && txResultIndex < txResultScroll) txResultScroll = txResultIndex;
    for (int i = 0; i < maxVisible && (txResultScroll + i) < scanResultCount; i++) {
      int idx = txResultScroll + i;
      int y = 16 + (i * 10) + yOffset;
      if (idx == txResultIndex) {
        display.fillRect(0, y - 1, 128, 10, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      String name = scanResults[idx].name;
      int16_t nx1, ny1; uint16_t nw, nh;
      display.getTextBounds(name, 0, 0, &nx1, &ny1, &nw, &nh);
      display.setCursor((128 - nw) / 2, y);
      display.print(name);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawScConnecting1(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  display.setCursor(120, 2 + yOffset);
  display.print("S");

  drawCenteredText(display, "Connecting to", 22 + yOffset, 1);
  drawCenteredText(display, txTargetName, 34 + yOffset, 1);

  int dots = ((millis() - scConnectStart) / 500) % 4;
  String dotStr = "";
  for (int i = 0; i < dots; i++) dotStr += ".";
  drawCenteredText(display, dotStr, 46 + yOffset, 1);
}

void drawScPinEntry(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);

  if (scPinCursor == -1) {
    display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }
  display.setCursor(2, 2 + yOffset);
  display.print("<--");

  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("ENTER PIN", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("ENTER PIN");

  // 4 digit boxes
  const int boxW = 20, boxH = 22, gap = 6;
  const int totalW = boxW * 4 + gap * 3;
  const int startX = (128 - totalW) / 2;
  for (int i = 0; i < 4; i++) {
    int bx = startX + i * (boxW + gap);
    int by = 18 + yOffset;
    char digit[2];
    snprintf(digit, sizeof(digit), "%d", scPin[i]);
    drawBoxedCenteredText(display, digit, bx, by, boxW, boxH, (scPinCursor == i));
  }

  // OK button
  drawBoxedCenteredText(display, "OK", 52, 44 + yOffset, 24, 12, (scPinCursor == 4));
}

void drawScConnecting2(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  display.setCursor(120, 2 + yOffset);
  display.print("S");

  drawCenteredText(display, "Authenticating", 28 + yOffset, 1);

  int dots = ((millis() - scAuthStart) / 500) % 4;
  String dotStr = "";
  for (int i = 0; i < dots; i++) dotStr += ".";
  drawCenteredText(display, dotStr, 40 + yOffset, 1);

  const int barX = 18, barW = 92, barH = 8;
  const int barY = 50 + yOffset;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  const int chunkW = 26;
  unsigned long t = millis() % 1800;
  float phase = t / 900.0f;
  if (phase > 1.0f) phase = 2.0f - phase;
  int travel = barW - 4 - chunkW;
  int chunkX = barX + 2 + (int)(phase * travel);
  display.fillRect(chunkX, barY + 2, chunkW, barH - 4, SSD1306_WHITE);
}

void drawScConnected(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");

  drawCenteredText(display, "CONNECTED!", 20 + yOffset, 1);

  display.fillCircle(64, 44 + yOffset, 14, SSD1306_WHITE);
  for (int i = 0; i < 3; i++) {
    display.drawLine(58, 44 + i + yOffset, 62, 48 + i + yOffset, SSD1306_BLACK);
    display.drawLine(62, 48 + i + yOffset, 70, 38 + i + yOffset, SSD1306_BLACK);
  }
}

void drawTxStatus(int yOffset, int xOffset) {
  display.fillRect(xOffset, yOffset, 128, 64, SSD1306_BLACK); // Prevent sliding overlap
  // Wrap OFF: mid-slide this panel is drawn at a non-zero xOffset, and any line
  // crossing the physical x=127 edge would otherwise wrap to x=0 of the next row
  // — landing inside the adjacent panel and reading as screen overlap.
  display.setTextWrap(false);
  display.drawFastHLine(xOffset, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  display.fillCircle(6 + xOffset, 6 + yOffset, 4, SSD1306_WHITE); // Cyan dot
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("STATUS", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2 + xOffset, 2 + yOffset);
  display.print("STATUS");
  
  drawBatteryIcon(111 + xOffset, 2 + yOffset, 75);

  int freqHz = (txActiveMode == 1) ? espNowFreqHz : sconnectFreqHz;
  float pktLoss = (txPktSentCount > 0) ? (1.0f - (float)txPktAckedCount / (float)txPktSentCount) * 100.0f : 0.0f;
  unsigned long elapsed = (millis() - txSessionStartMs) / 1000;
  int mins = elapsed / 60;
  int secs = elapsed % 60;

  char line0[22]; snprintf(line0, sizeof(line0), "PKT Loss: %.1f%%", pktLoss);
  char line1[22]; snprintf(line1, sizeof(line1), "Transmission: %dHz", freqHz);
  char line2[22]; snprintf(line2, sizeof(line2), "Raw Ping: %.1fms", txRawPingUs / 1000.0f);
  char line3[22]; snprintf(line3, sizeof(line3), "Ping: %lums", txTelemetryPingMs);
  char line4[22]; snprintf(line4, sizeof(line4), "Time: %dm %ds", mins, secs);
  char line5[22]; snprintf(line5, sizeof(line5), "RSSI: %d dBm", txLastRssi);
  char line6[22]; snprintf(line6, sizeof(line6), "PKT Sent: %lu", txPktSentCount);
  char line7[22]; snprintf(line7, sizeof(line7), "PKT ACK: %lu", txPktAckedCount);
  char line8[22]; snprintf(line8, sizeof(line8), "Mode: %s", (txActiveMode == 1) ? "ESP_NOW" : "SC");
  const char* statusItems[] = { line0, line1, line2, line3, line4, line5, line6, line7, line8 };
  const int itemCount = 9;

  txStatusScroll = (millis() / 1500) % (itemCount > 4 ? itemCount - 3 : 1);

  for (int i = 0; i < 4; i++) {
    int idx = txStatusScroll + i;
    if (idx >= itemCount) break;
    int y = 16 + (i * 12) + yOffset;
    display.setCursor(4 + xOffset, y);
    display.print(statusItems[idx]);
  }

  int trackH = 44;
  int sbH = max(4, (4 * trackH) / itemCount);
  int sbY = 16 + ((txStatusScroll * (trackH - sbH)) / max(1, itemCount - 4));
  display.drawFastVLine(126 + xOffset, 16 + yOffset, trackH, SSD1306_WHITE);
  display.fillRect(125 + xOffset, sbY + yOffset, 3, sbH, SSD1306_WHITE);
  display.setTextWrap(true);
}

void drawTxControl(int yOffset, int xOffset) {
  display.fillRect(xOffset, yOffset, 128, 64, SSD1306_BLACK); // Prevent sliding overlap
  display.setTextWrap(false); // see note in drawTxStatus — prevents mid-slide bleed
  display.setTextColor(SSD1306_WHITE);

  String name = txTargetName;
  if(name == "") name = "Unknown";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(name, 0, 0, &x1, &y1, &w, &h);
  
  if (w > 64) {
    name = name.substring(0, 8) + "..";
    display.getTextBounds(name, 0, 0, &x1, &y1, &w, &h);
  }
  display.setCursor((128 - w) / 2 + xOffset, 0 + yOffset);
  display.print(name);
  
  float pktLoss = (txPktSentCount > 0) ? (1.0f - (float)txPktAckedCount / (float)txPktSentCount) * 100.0f : 0.0f;
  char lossStr[8]; snprintf(lossStr, sizeof(lossStr), "%.0f%%", 100.0f - pktLoss);
  display.setCursor(4 + xOffset, 0 + yOffset);
  display.print(lossStr);
  char pingStr[8]; snprintf(pingStr, sizeof(pingStr), "%lums", txTelemetryPingMs);
  display.setCursor(128 - (strlen(pingStr) * 6) - 2 + xOffset, 0 + yOffset);
  display.print(pingStr);

  display.getTextBounds("CONNECTED", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2 + xOffset, 10 + yOffset);
  display.print("CONNECTED");
  
  // Left Gauge (simulated_analog_1)
  int bar1_x = 4 + xOffset;
  display.drawRoundRect(bar1_x, 14 + yOffset, 8, 38, 4, SSD1306_WHITE);
  int fillL = (simulated_analog_1 * 34) / 100;
  display.fillRoundRect(bar1_x + 2, 16 + (34 - fillL) + yOffset, 4, fillL, 2, SSD1306_WHITE);
  
  String val1 = String(simulated_analog_1);
  display.getTextBounds(val1, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(bar1_x + 4 - w/2, 54 + yOffset);
  display.print(val1);
  
  // Right Gauge (simulated_analog_2)
  int bar2_x = 116 + xOffset;
  display.drawRoundRect(bar2_x, 14 + yOffset, 8, 38, 4, SSD1306_WHITE);
  int fillR = (simulated_analog_2 * 34) / 100;
  display.fillRoundRect(bar2_x + 2, 16 + (34 - fillR) + yOffset, 4, fillR, 2, SSD1306_WHITE);
  
  String val2 = String(simulated_analog_2);
  display.getTextBounds(val2, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(bar2_x + 4 - w/2, 54 + yOffset);
  display.print(val2);

  // DPAD left side (uses cached buttonStates[] from readButtons(), not live I2C reads)
  int dpad1_x = 24 + xOffset, dpad1_y = 38 + yOffset;
  if(buttonStates[0]) display.fillRect(dpad1_x+10, dpad1_y-10, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad1_x+10, dpad1_y-10, 8, 8, SSD1306_WHITE);

  if(buttonStates[1]) display.fillRect(dpad1_x+10, dpad1_y+10, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad1_x+10, dpad1_y+10, 8, 8, SSD1306_WHITE);

  if(buttonStates[2]) display.fillRect(dpad1_x, dpad1_y, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad1_x, dpad1_y, 8, 8, SSD1306_WHITE);

  if(buttonStates[3]) display.fillRect(dpad1_x+20, dpad1_y, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad1_x+20, dpad1_y, 8, 8, SSD1306_WHITE);

  if(buttonStates[4]) display.fillRect(dpad1_x+10, dpad1_y, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad1_x+10, dpad1_y, 8, 8, SSD1306_WHITE);

  // DPAD right side (uses cached dpad2States[] read once per loop() in readButtons())
  int dpad2_x = 76 + xOffset, dpad2_y = 38 + yOffset;
  if(dpad2States[0]) display.fillRect(dpad2_x+10, dpad2_y-10, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad2_x+10, dpad2_y-10, 8, 8, SSD1306_WHITE);

  if(dpad2States[1]) display.fillRect(dpad2_x+10, dpad2_y+10, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad2_x+10, dpad2_y+10, 8, 8, SSD1306_WHITE);

  if(dpad2States[2]) display.fillRect(dpad2_x, dpad2_y, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad2_x, dpad2_y, 8, 8, SSD1306_WHITE);

  if(dpad2States[3]) display.fillRect(dpad2_x+20, dpad2_y, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad2_x+20, dpad2_y, 8, 8, SSD1306_WHITE);

  if(dpad2States[4]) display.fillRect(dpad2_x+10, dpad2_y, 8, 8, SSD1306_WHITE);
  else display.drawRect(dpad2_x+10, dpad2_y, 8, 8, SSD1306_WHITE);
  display.setTextWrap(true);
}

void drawTxTelemetry(int yOffset, int xOffset) {
  display.fillRect(xOffset, yOffset, 128, 64, SSD1306_BLACK); // Prevent sliding overlap
  display.setTextWrap(false); // see note in drawTxStatus — prevents mid-slide bleed
  display.drawFastHLine(xOffset, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TELEMETRY", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2 + xOffset, 2 + yOffset);
  display.print("TELEMETRY");
  
  drawBatteryIcon(111 + xOffset, 2 + yOffset, 75);

  display.drawRect(2 + xOffset, 16 + yOffset, 124, 46, SSD1306_WHITE);

  char telBuf[64];
  strncpy(telBuf, (const char*)txTelemetryStr, sizeof(telBuf));
  telBuf[63] = '\0';

  int lineY = 20 + yOffset;
  char* line = telBuf;
  for (int row = 0; row < 4 && *line; row++) {
    char segment[22];
    int len = strlen(line);
    int take = (len > 20) ? 20 : len;
    strncpy(segment, line, take);
    segment[take] = '\0';
    display.setCursor(6 + xOffset, lineY);
    display.print(segment);
    lineY += 10;
    line += take;
  }
  display.setTextWrap(true);
}

void drawTxTerminate(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  
  drawCenteredText(display, "Terminate Session?", 20 + yOffset, 1);
  
  drawBoxedCenteredText(display, "YES", 16, 36 + yOffset, 40, 18, (txTerminateFocus == 0));
  drawBoxedCenteredText(display, "NO", 72, 36 + yOffset, 40, 18, (txTerminateFocus == 1));
}

void drawTxTerminating(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");
  
  drawCenteredText(display, "Terminating Session", 26 + yOffset, 1);
  
  display.drawRect(20, 40 + yOffset, 88, 8, SSD1306_WHITE);
  int fill = ((millis() - txTerminatingStart) * 84) / 2000;
  if (fill > 84) fill = 84;
  display.fillRect(22, 42 + yOffset, fill, 4, SSD1306_WHITE);
}

void drawTxFailed(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");

  drawCenteredText(display, "CONNECTION FAILED", 26 + yOffset, 1);
  display.drawFastHLine(20, 38 + yOffset, 88, SSD1306_WHITE);
  drawCenteredText(display, txFailReason, 46 + yOffset, 1);
}

void drawTxRxLost(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");

  drawCenteredText(display, "RX DISCONNECTED", 26 + yOffset, 1);
  display.drawFastHLine(20, 38 + yOffset, 88, SSD1306_WHITE);
  drawCenteredText(display, "No Telemetry!", 46 + yOffset, 1);
}

void drawTxWifiDisable(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("TRANSMITTER");

  drawCenteredText(display, "Disable WiFi?", 20 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 16, 36 + yOffset, 40, 18, (txWifiDisableFocus == 0));
  drawBoxedCenteredText(display, "NO", 72, 36 + yOffset, 40, 18, (txWifiDisableFocus == 1));
}

void drawShutdownConfirm(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("AIO TRANSMITTER", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 2 + yOffset);
  display.print("AIO TRANSMITTER");

  drawCenteredText(display, "Shutdown?", 20 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 16, 36 + yOffset, 40, 18, (shutdownConfirmFocus == 0));
  drawBoxedCenteredText(display, "NO", 72, 36 + yOffset, 40, 18, (shutdownConfirmFocus == 1));
}

// =============================================================================
// BUZZER — boot chime + deep sleep
// =============================================================================
void playBootTone() {
  // Short ascending 3-note chime, evocative of a Windows-style "startup" jingle.
  tone(PIN_BUZZER, 1568, 90);   // G6
  delay(100);
  tone(PIN_BUZZER, 2093, 90);   // C7
  delay(100);
  tone(PIN_BUZZER, 2637, 150);  // E7
  delay(160);
  noTone(PIN_BUZZER);
}

// Non-blocking timer-ring trill — call once per loop() while tmMode == TM_RINGING.
// Rapidly alternates two close pitches in short bursts (the "rrrr" trill), with a
// silent gap between bursts, mimicking an old analog alarm-clock/phone ring.
void stepTimerRingTone() {
  unsigned long now = millis();
  if (now < tmRingNextStep) return;

  int cyclePos = tmRingPhase % 9; // 0-7 = trill notes, 8 = silence gap
  if (cyclePos < 8) {
    int freq = (cyclePos % 2 == 0) ? 2200 : 2600;
    tone(PIN_BUZZER, freq, 70);
    tmRingNextStep = now + 80;
  } else {
    noTone(PIN_BUZZER);
    tmRingNextStep = now + 350;
  }
  tmRingPhase++;
}

void enterLightSleep() {
  // Deep sleep on ESP32-C6 can only wake on GPIO0-7 (the chip's 8-pin RTC/LP
  // domain — SOC_RTCIO_PIN_COUNT). PIN_POWER (21) is outside that range, so a
  // deep-sleep GPIO wakeup on it silently fails to arm. Light sleep has no such
  // restriction — the whole GPIO matrix stays powered — so it works on any pin,
  // at the cost of higher sleep current than true deep sleep.
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  gpio_wakeup_enable((gpio_num_t)PIN_POWER, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  // By default the C6 swaps every GPIO to its "sleep-mode" pad config on
  // sleep entry — that config has NO pullup, so PIN_POWER floats and a press
  // has to fight noise instead of pulling a firmly-high pin low. Disabling
  // the sleep-mode swap keeps the awake INPUT_PULLUP active through sleep,
  // which is what actually makes the wake press reliable.
  gpio_sleep_sel_dis((gpio_num_t)PIN_POWER);
  esp_light_sleep_start();
  // ---- execution resumes right here on wake — light sleep does NOT reset the
  //      chip, so (unlike deep sleep) nothing reruns setup() automatically. ----
  gpio_wakeup_disable((gpio_num_t)PIN_POWER);

  // Re-arm the I2C bus. A light-sleep entry/exit can leave the MCP23017 or
  // the OLED's I2C state machine mid-transaction (a stray clock edge during
  // the sleep/wake ramp is enough), which then jams SDA low on the very next
  // read — that was the cause of the random freezes on the transmit page
  // (~30 I2C ops per loop, so the jammed bus was always hit there first).
  Wire.end();
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setTimeOut(50);
  mcp.begin_I2C(0x20, &Wire);
  for (int i = 0; i < 16; i++) mcp.pinMode(i, INPUT_PULLUP);

  // Manually replay a "freshly powered on" experience for parity with a real reset.
  // drawBootScreen() (clear + redraw + push) runs BEFORE anything blocking, so the
  // OLED never shows a stale frame — DISPLAYON alone does not clear GDDRAM, so the
  // last-rendered screen (the Shutdown? confirm) would otherwise flash briefly.
  isDisplaySleep = false;
  display.ssd1306_command(SSD1306_DISPLAYON);
  currentScreen = PAGE_BOOT;
  bootStartTime = millis();
  lastActivityTime = millis();
  drawBootScreen();

  powerBtnPressStart = 0;
  powerBtnLongPressHandled = false;
  // The wake press is very likely still physically held right now — require a
  // full release before the long-press-shutdown logic can arm again.
  powerBtnWaitingRelease = true;

  playBootTone();
}

void drawBatteryIcon(int x, int y, int fillPercent) {
  display.drawRect(x, y, 14, 6, SSD1306_WHITE);
  display.fillRect(x + 14, y + 2, 2, 2, SSD1306_WHITE);
  int fillW = (fillPercent * 10) / 100;
  if(fillW > 0) display.fillRect(x + 2, y + 2, fillW, 2, SSD1306_WHITE);
}

void drawCenteredText(Adafruit_SSD1306 &d, const String &text, int16_t y, uint8_t size, int xOffset) {
  int16_t x1, y1; uint16_t w, h;
  d.setTextSize(size);
  d.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  d.setCursor((SCREEN_WIDTH - w) / 2 + xOffset, y);
  d.print(text);
}

void drawBoxedCenteredText(Adafruit_SSD1306 &d, const char* text, int x, int y, int w, int h, bool inverted) {
  int16_t x1, y1; uint16_t tw, th;
  d.setTextSize(1);
  d.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  if (inverted) { d.fillRect(x, y, w, h, SSD1306_WHITE); d.setTextColor(SSD1306_BLACK); }
  else          { d.drawRect(x, y, w, h, SSD1306_WHITE); d.setTextColor(SSD1306_WHITE); }
  d.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  d.print(text);
  d.setTextColor(SSD1306_WHITE);
}

// =============================================================================
// BUTTON READING (MCP23017, edge-detection, same as original AIO)
// =============================================================================
void readButtons() {
  bool currentUp     = (mcp.digitalRead(DPAD_1_UP)     == LOW);
  bool currentDown   = (mcp.digitalRead(DPAD_1_DOWN)   == LOW);
  bool currentLeft   = (mcp.digitalRead(DPAD_1_LEFT)   == LOW);
  bool currentRight  = (mcp.digitalRead(DPAD_1_RIGHT)  == LOW);
  bool currentCenter = (mcp.digitalRead(DPAD_1_CENTER) == LOW);

  buttonStates[0] = currentUp;
  buttonStates[1] = currentDown;
  buttonStates[2] = currentLeft;
  buttonStates[3] = currentRight;
  buttonStates[4] = currentCenter;

  buttonJustPressed[0] = currentUp     && !lastButtonStates[0];
  buttonJustPressed[1] = currentDown   && !lastButtonStates[1];
  buttonJustPressed[2] = currentLeft   && !lastButtonStates[2];
  buttonJustPressed[3] = currentRight  && !lastButtonStates[3];
  buttonJustPressed[4] = currentCenter && !lastButtonStates[4];

  lastButtonStates[0] = currentUp;
  lastButtonStates[1] = currentDown;
  lastButtonStates[2] = currentLeft;
  lastButtonStates[3] = currentRight;
  lastButtonStates[4] = currentCenter;

  dpad2States[0] = (mcp.digitalRead(DPAD_2_UP)     == LOW);
  dpad2States[1] = (mcp.digitalRead(DPAD_2_DOWN)   == LOW);
  dpad2States[2] = (mcp.digitalRead(DPAD_2_LEFT)   == LOW);
  dpad2States[3] = (mcp.digitalRead(DPAD_2_RIGHT)  == LOW);
  dpad2States[4] = (mcp.digitalRead(DPAD_2_CENTER) == LOW);
}

// =============================================================================
// WiFi CREDENTIAL PERSISTENCE  (Preferences NVS)
// =============================================================================
void loadCredentials() {
  prefs.begin("wifi_creds", true);
  savedNetCount = prefs.getInt("count", 0);
  for (int i = 0; i < savedNetCount && i < MAX_SAVED_NETS; i++) {
    savedSSIDs[i] = prefs.getString(("ssid" + String(i)).c_str(), "");
    savedPWDs[i]  = prefs.getString(("pwd"  + String(i)).c_str(), "");
  }
  prefs.end();
}

void saveCredential(String ssid, String pwd) {
  lastConnectedSSID = ssid;
  for (int i = 0; i < savedNetCount; i++) {
    if (savedSSIDs[i] == ssid) { savedPWDs[i] = pwd; goto writeAll; }
  }
  if (savedNetCount < MAX_SAVED_NETS) {
    savedSSIDs[savedNetCount] = ssid;
    savedPWDs[savedNetCount]  = pwd;
    savedNetCount++;
  }
  writeAll:
  prefs.begin("wifi_creds", false);
  prefs.putInt("count", savedNetCount);
  for (int i = 0; i < savedNetCount; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), savedSSIDs[i].c_str());
    prefs.putString(("pwd"  + String(i)).c_str(), savedPWDs[i].c_str());
  }
  prefs.end();
}

void forgetCredential(String ssid) {
  int found = -1;
  for (int i = 0; i < savedNetCount; i++) { if (savedSSIDs[i] == ssid) { found = i; break; } }
  if (found == -1) return;
  for (int i = found; i < savedNetCount - 1; i++) {
    savedSSIDs[i] = savedSSIDs[i+1];
    savedPWDs[i]  = savedPWDs[i+1];
  }
  savedNetCount--;
  prefs.begin("wifi_creds", false);
  prefs.putInt("count", savedNetCount);
  for (int i = 0; i < savedNetCount; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), savedSSIDs[i].c_str());
    prefs.putString(("pwd"  + String(i)).c_str(), savedPWDs[i].c_str());
  }
  prefs.end();
}

String findSavedPwd(String ssid) {
  for (int i = 0; i < savedNetCount; i++) { if (savedSSIDs[i] == ssid) return savedPWDs[i]; }
  return "";
}

void loadForbidden() {
  prefs.begin("wifi_forbid", true);
  forbiddenCount = prefs.getInt("count", 0);
  for (int i = 0; i < forbiddenCount && i < MAX_FORBIDDEN_NETS; i++) {
    forbiddenSSIDs[i] = prefs.getString(("f" + String(i)).c_str(), "");
  }
  prefs.end();
}

void saveForbidden() {
  prefs.begin("wifi_forbid", false);
  prefs.putInt("count", forbiddenCount);
  for (int i = 0; i < forbiddenCount; i++) {
    prefs.putString(("f" + String(i)).c_str(), forbiddenSSIDs[i].c_str());
  }
  prefs.end();
}

void addForbidden(String ssid) {
  if (isForbidden(ssid)) return;
  if (forbiddenCount >= MAX_FORBIDDEN_NETS) return;
  forbiddenSSIDs[forbiddenCount++] = ssid;
  saveForbidden();
}

void removeForbidden(String ssid) {
  int found = -1;
  for (int i = 0; i < forbiddenCount; i++) { if (forbiddenSSIDs[i] == ssid) { found = i; break; } }
  if (found == -1) return;
  for (int i = found; i < forbiddenCount - 1; i++) forbiddenSSIDs[i] = forbiddenSSIDs[i+1];
  forbiddenCount--;
  saveForbidden();
}

bool isForbidden(String ssid) {
  for (int i = 0; i < forbiddenCount; i++) { if (forbiddenSSIDs[i] == ssid) return true; }
  return false;
}

void loadSettings() {
  prefs.begin("settings", true);
  wifiUdpFreqHz  = prefs.getInt("udpHz",   wifiUdpFreqHz);
  espNowFreqHz   = prefs.getInt("espnowHz", espNowFreqHz);
  sconnectFreqHz = prefs.getInt("sconnHz",  sconnectFreqHz);
  txDeviceName   = prefs.getString("devName", txDeviceName);
  manageWifi     = prefs.getBool("manageWifi", false);
  prefs.end();
}

void saveManageWifi(bool on) {
  prefs.begin("settings", false);
  prefs.putBool("manageWifi", on);
  prefs.end();
}

void saveDeviceName(const String &name) {
  prefs.begin("settings", false);
  prefs.putString("devName", name);
  prefs.end();
}

void saveFreqSetting(int target, int hz) {
  prefs.begin("settings", false);
  if (target == 0) prefs.putInt("udpHz", hz);
  else if (target == 1) prefs.putInt("espnowHz", hz);
  else prefs.putInt("sconnHz", hz);
  prefs.end();
}

void syncTimeWithNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

// =============================================================================
// INTERNET CONNECTIVITY PINGER (adapted from StrawberryOS)
// =============================================================================
void connectivityTask(void *p) {
  const char* urls[] = {
    "http://www.gstatic.com/generate_204",
    "http://clients3.google.com/generate_204",
    "http://cp.cloudflare.com/generate_204",
    "http://edge-http.microsoft.com/captiveportal/generate_204",
    "http://connect.rom.miui.com/generate_204"
  };
  int urlIndex = 0;
  for (;;) {
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient h;
      h.setConnectTimeout(1200);
      h.setTimeout(1800);
      h.begin(urls[urlIndex]);
      ok = (h.GET() == 204);
      h.end();
    }

    pingerStatus[urlIndex] = ok;
    urlIndex = (urlIndex + 1) % 5;

    internetOK = pingerStatus[0] || pingerStatus[1] || pingerStatus[2] ||
                 pingerStatus[3] || pingerStatus[4];

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startConnectivityPinger() {
  if (pingerStarted) return;
  pingerStarted = true;
  xTaskCreate(connectivityTask, "NetPing", 4096, NULL, 1, NULL);
}

// =============================================================================
// WiFi BACKGROUND AUTO-CONNECT TASK
// =============================================================================
void wifiBackgroundTask(void *p) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));

    // CRITICAL: never touch WiFi while ESP-NOW owns the radio. WiFi.scanNetworks()
    // hops across every channel for several seconds and WiFi.begin() parks us on
    // the AP's channel — either one drags us off channel 1 and silently kills
    // ESP-NOW in BOTH directions mid-session. This 10s task firing during the
    // Smart Connect PIN-entry pause was why PinSubmit never reached the receiver.
    if (espNowInitialized || txScanning || txSessionActive) continue;

    if (!wifiEnabled || !wifiAutoOn) continue;

    if (WiFi.status() == WL_CONNECTED || wifiConnecting ||
        WiFi.scanComplete() == WIFI_SCAN_RUNNING) continue;

    if (currentScreen >= SCREEN_WIFI_CONFIRM &&
        currentScreen <= SCREEN_WIFI_FORGET_CONFIRM) continue;

    if (WiFi.getMode() != WIFI_STA) {
      WiFi.mode(WIFI_STA);
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    WiFi.scanNetworks(true);

    unsigned long scanWaitStart = millis();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - scanWaitStart < 10000) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    int n = WiFi.scanComplete();
    if (n > 0) {
      int bestIdx = -1;

      for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (isForbidden(ssid)) continue;
        String pwd = findSavedPwd(ssid);
        if (pwd.length() > 0) {
          if (lastConnectedSSID.length() > 0 && ssid == lastConnectedSSID) {
            bestIdx = i;
            break;
          }
          if (bestIdx == -1) bestIdx = i;
        }
      }

      if (bestIdx != -1) {
        String targetSSID = WiFi.SSID(bestIdx);
        String targetPWD = findSavedPwd(targetSSID);

        WiFi.begin(targetSSID.c_str(), targetPWD.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
          vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (WiFi.status() == WL_CONNECTED) {
          lastConnectedSSID = targetSSID;
          saveCredential(targetSSID, targetPWD);
          prefs.begin("wifi_creds", false);
          prefs.putString("lastSSID", lastConnectedSSID);
          prefs.end();
          syncTimeWithNTP();
          startConnectivityPinger();
        }
      }
    }

    if (currentScreen != SCREEN_WIFI_RESULTS) WiFi.scanDelete();
  }
}

void startWifiBackgroundTask() {
  xTaskCreate(wifiBackgroundTask, "WiFiBack", 4096, NULL, 1, NULL);
}

// =============================================================================
// MCP23017 INITIALISATION
// =============================================================================
void initMCP23017() {
  if (!mcp.begin_I2C(0x20, &Wire)) {
    Serial.println("Error initializing MCP23017.");
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.println("Hardware Error:");
    display.println("MCP23017 Not Found!");
    display.println("Check I2C Wiring.");
    display.display();
    while (1);
  }
  Serial.println("MCP23017 initialized successfully.");
  for (int i = 0; i < 16; i++) {
    mcp.pinMode(i, INPUT_PULLUP);
  }
}

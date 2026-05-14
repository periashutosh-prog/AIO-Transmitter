// StrawberryOS - A smartwatch OS for ESP32
// Copyright (C) 2026 Ashutosh
//
// See the LICENSE file for more details.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RTClib.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "time.h"
#include <NimBLEDevice.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

RTC_DS3231 rtc;

#define BTN_UP 6
#define BTN_DOWN 9
#define BTN_LEFT 8
#define BTN_RIGHT 7
#define BTN_CENTER 10

volatile bool buttonStates[5] = {false, false, false, false, false};
const uint8_t buttonPins[5] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CENTER};
uint8_t statusIndex = 0;
bool isStatusActive = false;

enum ScreenState {
  SCREEN_WATCHFACE, SCREEN_MENU, SCREEN_STOPWATCH, SCREEN_TIMER, SCREEN_TIMER_ALERT,

  SCREEN_RADIO,
  SCREEN_RADIO_WIFI, SCREEN_WIFI_CONFIRM, SCREEN_WIFI_SCANNING,
  SCREEN_WIFI_RESULTS, SCREEN_WIFI_PASSWORD, SCREEN_WIFI_KEYBOARD,
  SCREEN_WIFI_HOME, SCREEN_WIFI_TOGGLE_CONFIRM,
  SCREEN_RADIO_BLE, SCREEN_RADIO_BLE_CONFIRM, SCREEN_RADIO_ESPNOW,

  SCREEN_POWER_HOME, SCREEN_WIFI_AUTO_CONFIRM, SCREEN_ESPNOW_AUTO_CONFIRM,
  SCREEN_POWER_MODE, SCREEN_POWER_MODE_HELP, SCREEN_ECO_EXIT_CONFIRM,
  SCREEN_WIFI_NTP,

  SCREEN_AI_CHAT, SCREEN_AI_THINKING, SCREEN_AI_WIFI_ERROR,
  SCREEN_CALCULATOR, SCREEN_CALCULATOR_SCI
};
ScreenState currentScreen = SCREEN_WATCHFACE;

bool lastButtonStates[5] = {false, false, false, false, false};
bool buttonJustPressed[5] = {false, false, false, false, false};

const int NUM_MENU_ITEMS = 6;
const char* menuItems[NUM_MENU_ITEMS] = {"Stopwatch", "Timer", "Radio", "AI Chat", "Calculator", "Lock"};
int menuIndex = 0;

unsigned long swStartTime = 0;
unsigned long swElapsedTime = 0;
bool swRunning = false;
int swFocus = 0;

enum TimerMode { TM_SETTING, TM_READY, TM_RUNNING, TM_RINGING };
TimerMode tmMode = TM_SETTING;
int tmHours = 0, tmMinutes = 0, tmSeconds = 0;
int tmActiveUnit = 2;
int tmFocus = 1;
unsigned long tmRemainingMillis = 0;
unsigned long tmLastTick = 0;
unsigned long tmSetPressStart = 0;
bool tmLongPressTriggered = false;

bool isAnimating = false;
int animOffsetY = 0;
int animTargetY = 0;
ScreenState animNextScreen = SCREEN_WATCHFACE;

unsigned long lastActivityTime = 0;
const unsigned long DISPLAY_TIMEOUT_MS = 5000;
bool displayOn = true;

enum KBMode { KB_LOWER, KB_UPPER, KB_SYMBOL };
KBMode currentKBMode = KB_LOWER;
int kbCursorRow = 0, kbCursorCol = 0;

const char* kbLower[4][11] = {
  {"q","w","e","r","t","y","u","i","o","p","."},
  {"a","s","d","f","g","h","j","k","l",",","?"},
  {"z","x","c","v","b","n","m","CL","CR","^","BS"},
  {"1","2","3","4","5","6","7","8","9"," ","EN"}
};
const char* kbUpper[4][11] = {
  {"Q","W","E","R","T","Y","U","I","O","P","."},
  {"A","S","D","F","G","H","J","K","L",",","?"},
  {"Z","X","C","V","B","N","M","CL","CR","^","BS"},
  {"!","@","#","$","%","^","&","*","("," ","EN"}
};
const char* kbSymbol[4][11] = {
  {"1","2","3","4","5","6","7","8","9","0","~"},
  {"@","$","%","&","*","-","+","=","!","|","\\"},
  {"/","?",";",":","'","(",")","CL","CR","^","BS"},
  {"[","]","{","}","<",">","#","_","."," ","EN"}
};

bool wifiAutoOn = false;
bool espNowAutoOn = false;
String lastConnectedSSID = "";
bool wifiEnabled = false;
int wifiScanResults = 0;
int wifiListIndex = -1;
int wifiListScroll = 0;
String wifiTargetSSID = "";
String wifiPassword = "";
String wifiStatusMsg = "";
bool wifiConnecting = false;
unsigned long wifiScanStart = 0;
unsigned long wifiConnectStartTime = 0;
int wifiHomeCursor = -1;
int wifiConfirmCursor = 0;
int wifiToggleCursor = 0;
int wifiPasswordCursor = 1;
unsigned long ntpSyncStart = 0;
unsigned long ntpSyncEnd = 0;
int ntpSyncState = 0;
int wifiRetryCount = 0;

String kbInputBuffer = "";
int kbTextCursor = 0;
int kbScrollOffset = 0;
ScreenState kbReturnScreen = SCREEN_WIFI_PASSWORD;

NimBLEServer* pBleServer = NULL;
NimBLECharacteristic* pBleNameCharacteristic = NULL;
NimBLECharacteristic* pPhoneNameCharacteristic = NULL;
NimBLECharacteristic* pDisplayCharacteristic = NULL;
NimBLECharacteristic* pMirrorControlCharacteristic = NULL;
volatile bool bleConnected = false;
volatile bool mirroringActive = false;
unsigned long lastMirrorUpdate = 0;
const int MIRROR_INTERVAL = 100; // 10 FPS
String bleRemoteAddress = "";
String bleDeviceName = "Ashutosh's Smartwatch";
String bleRemoteDeviceName = "None";
bool bleSettingAccess = false;
int bleHomeCursor = 0;
int bleConfirmMode = 0;
int bleConfirmCursor = 0;
bool bleDisconnecting = false;

class PhoneNameCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            bleRemoteDeviceName = String(value.c_str());
            Serial.print("Phone Name updated: ");
            Serial.println(bleRemoteDeviceName);
        }
    }
};

Preferences prefs;
const int MAX_SAVED_NETS = 5;
String savedSSIDs[MAX_SAVED_NETS];
String savedPWDs[MAX_SAVED_NETS];
int savedNetCount = 0;

bool pingerStatus[5] = {false, false, false, false, false};
bool internetOK = false;
volatile bool statusChanged = false;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

int radioCursor = 0;
int powerHomeCursor = 0;

enum PowerModeType { POWER_NORMAL, POWER_ECO };
PowerModeType currentPowerMode = POWER_NORMAL;
int powerModeButtonCursor = 0;
bool ecoModeActive = false;
unsigned long ecoModeSleepStart = 0;
const unsigned long ECO_MODE_SLEEP_DURATION = 5000;
unsigned long ecoExitPressStart = 0;
const unsigned long ECO_EXIT_PRESS_DURATION = 5000;
int ecoExitConfirmCursor = 1;
unsigned long ecoDeepSleepStart = 0;
const unsigned long ECO_DEEP_SLEEP_DELAY = 500;

int aiMsgCount = 0;
int aiChatScroll = 0;
bool aiScrollMode = false;
int aiCursor = 0;
String aiInputText = "";
bool aiThinking = false;
unsigned long aiThinkStart = 0;
String aiResponse = "";
volatile bool aiResponseReady = false;
volatile bool aiResponseError = false;
int aiWifiErrorType = 0;
int aiWifiErrorCursor = 1;
const char* GROQ_API_KEY = "YOUR_GROQ_API_KEY";
const int AI_MAX_MESSAGES = 500;
String aiCache[5];
int aiCacheScrollUsed = -1;

// Calculator variables
int calcCursorRow = 0, calcCursorCol = 0;
String calcInput1 = "";
String calcInput2 = "";
char calcOp = ' ';
bool calcIsOpSet = false;
bool calcError = false;

int calcSciRow = 0, calcSciCol = 0;

// SCI functions grid: 3 rows x 2 cols
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

void updateDisplay();
void drawCalculator(int yOffset);
void drawCalculatorSci(int yOffset);
void drawScreen(ScreenState screen, int yOffset);
void drawHeader(int yOffset, const char* appName = nullptr, bool backFocused = false);
void drawWatchFace(int yOffset);
void drawMenu(int yOffset);
void drawStopwatch(int yOffset);
void drawTimer(int yOffset);
void drawTimerAlert(int yOffset);
void drawCenteredText(Adafruit_SSD1306 &d, const String &text, int16_t y, uint8_t size = 1);
void drawBoxedCenteredText(Adafruit_SSD1306 &d, const char* text, int x, int y, int w, int h, bool inverted);
void startAnimation(ScreenState next, int targetOffset);

int aiCountMessages();
void aiAddMessage(const String& msg);
void aiClearChat();
String aiGetMessage(int index);
void drawAiChat(int yOffset);
void drawAiThinking(int yOffset);
void drawAiWifiError(int yOffset);
void aiSendTask(void* param);
void aiUpdateCache();
void buttonReadTask(void *pvParameters);
void printButtonStates();

void drawRadioHome(int yOffset);
void drawRadioWifi(int yOffset);
void drawWifiConfirm(int yOffset);
void drawWifiScanning(int yOffset);
void drawWifiResults(int yOffset);
void drawWifiPassword(int yOffset);
void drawWifiKeyboard();
void drawWifiHome(int yOffset);
void drawWifiToggleConfirm(int yOffset);
void drawRadioStub(int yOffset, const char* name);
void drawRadioBleHome(int yOffset);
void drawBleConfirm(int yOffset);
void drawPowerModeSelection(int yOffset);
void drawPowerModeHelp(int yOffset);
void drawEcoExitConfirm(int yOffset);
void initBLE();
void loadCredentials();
void saveCredential(String ssid, String pwd);
void forgetCredential(String ssid);
String findSavedPwd(String ssid);
void syncTimeWithNTP();
void startWifiPingers();
void internetPinger1(void *p);
void internetPinger2(void *p);
void internetPinger3(void *p);
void internetPinger4(void *p);
void internetPinger5(void *p);
void connectivityAggregator(void *p);

void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(4, 5);
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.display();
  delay(100);

  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    display.clearDisplay();
    display.setTextSize(1);
    display.println("RTC Error!");
    display.display();
    while (1);
  }

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_CENTER, INPUT_PULLUP);

  xTaskCreate(
    buttonReadTask,
    "ButtonReader",
    2048,
    NULL,
    1,
    NULL
  );

  Serial.println("Display and RTC initialized!");

  loadCredentials();

  WiFi.mode(WIFI_STA);
	WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(true);
  WiFi.disconnect(true);
  delay(100);

  startWifiPingers();

  initBLE();

  prefs.begin("powermode", false);
  ecoModeActive = prefs.getBool("ecoMode", false);
  wifiAutoOn = prefs.getBool("wifiAutoOn", false);
  espNowAutoOn = prefs.getBool("espNowAutoOn", false);
  lastConnectedSSID = prefs.getString("lastSSID", "");
  prefs.end();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }
  aiMsgCount = aiCountMessages();
  aiUpdateCache();

  if (wifiAutoOn) {
    wifiEnabled = true;
    WiFi.mode(WIFI_STA);
    WiFi.begin();
  }

  xTaskCreate(
    wifiBackgroundTask,
    "WiFiBack",
    4096,
    NULL,
    1,
    NULL
  );

  lastActivityTime = millis();
}

void loop() {
  bool activityDetected = false;

  for (int i = 0; i < 5; i++) {
    buttonJustPressed[i] = buttonStates[i] && !lastButtonStates[i];
    lastButtonStates[i] = buttonStates[i];
    if (buttonStates[i]) activityDetected = true;
  }

  if (activityDetected) {
    lastActivityTime = millis();
    if (ecoModeActive) ecoModeSleepStart = millis();
    if (!displayOn) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      displayOn = true;
      currentScreen = SCREEN_WATCHFACE;
      animOffsetY = 0;
      isAnimating = false;

      for (int i = 0; i < 5; i++) buttonJustPressed[i] = false;
    }
  }

  bool inhibitSleep = (currentScreen == SCREEN_STOPWATCH && swRunning) ||
                      (currentScreen == SCREEN_TIMER && tmMode == TM_RINGING) ||
                      (currentScreen == SCREEN_TIMER_ALERT) ||
                      (currentScreen == SCREEN_WIFI_SCANNING) ||
                      (currentScreen == SCREEN_WIFI_PASSWORD && wifiConnecting) ||
                      (currentScreen == SCREEN_WIFI_NTP) ||
                      (currentScreen == SCREEN_AI_THINKING);

  if (displayOn && !inhibitSleep) {

    if (ecoModeActive && currentScreen == SCREEN_WATCHFACE) {
      if (millis() - ecoModeSleepStart > ECO_MODE_SLEEP_DURATION) {
        display.ssd1306_command(SSD1306_DISPLAYOFF);
        displayOn = false;
        ecoDeepSleepStart = millis();
      }
    } else {

      if (millis() - lastActivityTime > DISPLAY_TIMEOUT_MS) {
        display.ssd1306_command(SSD1306_DISPLAYOFF);
        displayOn = false;
      }
    }
  }

  if (!displayOn && ecoModeActive && currentScreen == SCREEN_WATCHFACE) {
    if (millis() - ecoDeepSleepStart > ECO_DEEP_SLEEP_DELAY) {

      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      esp_bt_controller_disable();

      gpio_set_direction((gpio_num_t)BTN_UP, GPIO_MODE_INPUT);
      gpio_set_direction((gpio_num_t)BTN_DOWN, GPIO_MODE_INPUT);
      gpio_set_direction((gpio_num_t)BTN_LEFT, GPIO_MODE_INPUT);
      gpio_set_direction((gpio_num_t)BTN_RIGHT, GPIO_MODE_INPUT);
      gpio_set_direction((gpio_num_t)BTN_CENTER, GPIO_MODE_INPUT);

      gpio_pullup_en((gpio_num_t)BTN_UP);
      gpio_pullup_en((gpio_num_t)BTN_DOWN);
      gpio_pullup_en((gpio_num_t)BTN_LEFT);
      gpio_pullup_en((gpio_num_t)BTN_RIGHT);
      gpio_pullup_en((gpio_num_t)BTN_CENTER);

      esp_sleep_enable_gpio_wakeup();
      gpio_wakeup_enable((gpio_num_t)BTN_UP, GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable((gpio_num_t)BTN_DOWN, GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable((gpio_num_t)BTN_LEFT, GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable((gpio_num_t)BTN_RIGHT, GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable((gpio_num_t)BTN_CENTER, GPIO_INTR_LOW_LEVEL);

      esp_deep_sleep_start();
    }
  }

  if (!displayOn && currentScreen == SCREEN_STOPWATCH && swRunning) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      displayOn = true;
      lastActivityTime = millis();
  }

  if (currentScreen == SCREEN_WIFI_SCANNING && !isAnimating) {
    lastActivityTime = millis();
    int result = WiFi.scanComplete();
    static bool scanRetried = false;

    if (result >= 0) {
      wifiScanResults = result;
      wifiListIndex = 0; wifiListScroll = 0;
      startAnimation(SCREEN_WIFI_RESULTS, -64);
      scanRetried = false;
      if (pBleServer && pBleServer->getConnectedCount() == 0) {
        NimBLEDevice::getAdvertising()->stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        if(!NimBLEDevice::getAdvertising()->isAdvertising()) { NimBLEDevice::getAdvertising()->start(); }
      }
    } else if (result == WIFI_SCAN_FAILED) {
      if (!scanRetried) {
        scanRetried = true;
        WiFi.scanNetworks(true, false, false, 80); // async scan
      } else {
        wifiScanResults = 0;
        startAnimation(SCREEN_WIFI_RESULTS, -64);
        scanRetried = false;
        if (pBleServer && pBleServer->getConnectedCount() == 0) {
          NimBLEDevice::getAdvertising()->stop();
          vTaskDelay(pdMS_TO_TICKS(50));
          if(!NimBLEDevice::getAdvertising()->isAdvertising()) { NimBLEDevice::getAdvertising()->start(); }
        }
      }
    } else if (millis() - wifiScanStart > 8000) {
      WiFi.scanDelete();
      wifiScanResults = 0;
      startAnimation(SCREEN_WIFI_RESULTS, -64);
      scanRetried = false;
      if (pBleServer && pBleServer->getConnectedCount() == 0) {
        NimBLEDevice::getAdvertising()->stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        if(!NimBLEDevice::getAdvertising()->isAdvertising()) { NimBLEDevice::getAdvertising()->start(); }
      }
    }
  }

  if (currentScreen == SCREEN_WIFI_NTP && !isAnimating) {
    lastActivityTime = millis();
    if (ntpSyncState == 0) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 0)) {
        rtc.adjust(DateTime(
          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
        ntpSyncState = 1;
        ntpSyncEnd = millis();
      } else if (millis() - ntpSyncStart > 5000) {
        ntpSyncState = 2;
        ntpSyncEnd = millis();
      }
    } else {
      if (millis() - ntpSyncEnd > 2000) {
        startAnimation(SCREEN_WIFI_HOME, -64);
        ntpSyncState = 0;
      }
    }
  }

  if (currentScreen == SCREEN_AI_THINKING && !isAnimating) {
    lastActivityTime = millis();
    if (aiResponseReady) {
      aiResponseReady = false;
      aiAddMessage("A:" + aiResponse);
      aiResponse = "";
      aiChatScroll = max(0, aiMsgCount - 4);
      aiUpdateCache();
      startAnimation(SCREEN_AI_CHAT, -64);
    } else if (aiResponseError) {
      aiResponseError = false;
      aiAddMessage("A:Internet Error");
      aiChatScroll = max(0, aiMsgCount - 4);
      startAnimation(SCREEN_AI_CHAT, -64);
    } else if (millis() - aiThinkStart > 10000) {
      aiAddMessage("A:Timeout (10s)");
      aiChatScroll = max(0, aiMsgCount - 4);
      startAnimation(SCREEN_AI_CHAT, -64);
    }
  }

  if (displayOn) {

    if (isAnimating) {
      if (animOffsetY < animTargetY) animOffsetY += 8;
      else if (animOffsetY > animTargetY) animOffsetY -= 8;

      if (abs(animOffsetY - animTargetY) < 8) {
        animOffsetY = 0;
        currentScreen = animNextScreen;
        isAnimating = false;
      }
    } else {

      if (currentScreen == SCREEN_WATCHFACE) {

        if (ecoModeActive && (buttonJustPressed[0] || buttonJustPressed[1] || buttonJustPressed[2]
                              || buttonJustPressed[3] || buttonJustPressed[4])) {
          ecoModeSleepStart = millis();
        }

        if (ecoModeActive && buttonStates[4]) {
          if (ecoExitPressStart == 0) {
            ecoExitPressStart = millis();
          }

          if (millis() - ecoExitPressStart >= ECO_EXIT_PRESS_DURATION) {
            ecoExitConfirmCursor = 1;
            startAnimation(SCREEN_ECO_EXIT_CONFIRM, -64);
            ecoExitPressStart = 0;
          }
        } else {

          ecoExitPressStart = 0;
        }

        if (!ecoModeActive && buttonJustPressed[0]) {
          startAnimation(SCREEN_MENU, -64);
        }
      }
      else if (currentScreen == SCREEN_MENU) {
        if (buttonJustPressed[0]) {
          menuIndex = (menuIndex - 1 + NUM_MENU_ITEMS) % NUM_MENU_ITEMS;
        }
        if (buttonJustPressed[1]) {
          menuIndex = (menuIndex + 1) % NUM_MENU_ITEMS;
        }
        if (buttonJustPressed[4]) {
          if (menuIndex == 0) {
          swFocus = 1;
          startAnimation(SCREEN_STOPWATCH, -64);
        } else if (menuIndex == 1) {
          tmMode = TM_SETTING;
          tmFocus = 1;
          startAnimation(SCREEN_TIMER, -64);
        } else if (menuIndex == 2) {
          radioCursor = 0;
          startAnimation(SCREEN_RADIO, -64);
        } else if (menuIndex == 3) {
          aiCursor = 0;
          aiScrollMode = false;
          aiChatScroll = max(0, aiMsgCount - 4);
          aiUpdateCache();
          WiFi.setSleep(false);
          startAnimation(SCREEN_AI_CHAT, -64);
        } else if (menuIndex == 4) {
          calcCursorRow = 0; calcCursorCol = 0;
          calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
          startAnimation(SCREEN_CALCULATOR, -64);
        } else if (menuIndex == 5) {
          startAnimation(SCREEN_WATCHFACE, 64);
        }
        }
      }
      else if (currentScreen == SCREEN_STOPWATCH) {

        if (buttonJustPressed[0]) {
          if (swFocus == 1 || swFocus == 2) swFocus = 0;
        }
        if (buttonJustPressed[1]) {
          if (swFocus == 0) swFocus = 1;
        }
        if (buttonJustPressed[2]) {
          if (swFocus == 2) swFocus = 1;
        }
        if (buttonJustPressed[3]) {
          if (swFocus == 1) swFocus = 2;
        }

        if (buttonJustPressed[4]) {
          if (swFocus == 0) {
            startAnimation(SCREEN_MENU, 64);
          } else if (swFocus == 1) {
            if (swRunning) {
              swElapsedTime += millis() - swStartTime;
              swRunning = false;
            } else {
              swStartTime = millis();
              swRunning = true;
            }
          } else if (swFocus == 2) {
            if (swRunning) {
              swElapsedTime += millis() - swStartTime;
              swRunning = false;
            } else {
              swElapsedTime = 0;
            }
          }
        }
      }
      else if (currentScreen == SCREEN_STOPWATCH) {

      if (buttonJustPressed[0]) {
        if (swFocus == 1 || swFocus == 2) swFocus = 0;
      }
      if (buttonJustPressed[1]) {
        if (swFocus == 0) swFocus = 1;
      }
      if (buttonJustPressed[2]) {
        if (swFocus == 2) swFocus = 1;
      }
      if (buttonJustPressed[3]) {
        if (swFocus == 1) swFocus = 2;
      }

      if (buttonJustPressed[4]) {
        if (swFocus == 0) {
          startAnimation(SCREEN_MENU, 64);
        } else if (swFocus == 1) {
          if (swRunning) {
            swElapsedTime += millis() - swStartTime;
            swRunning = false;
          } else {
            swStartTime = millis();
            swRunning = true;
          }
        } else if (swFocus == 2) {
          if (swRunning) {
            swElapsedTime += millis() - swStartTime;
            swRunning = false;
          } else {
            swElapsedTime = 0;
          }
        }
      }
    }
    else if (currentScreen == SCREEN_TIMER) {

        if (buttonJustPressed[0]) {
          if (tmFocus > 0) tmFocus = 0;
        }
        if (buttonJustPressed[1]) {
          if (tmFocus == 0) tmFocus = 1;
        }
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

        if (buttonStates[4] && tmFocus == 2 && tmMode == TM_SETTING) {
          if (tmSetPressStart == 0) tmSetPressStart = millis();
          if (millis() - tmSetPressStart > 2000 && !tmLongPressTriggered) {
            tmMode = TM_READY;
            tmFocus = 3;
            tmLongPressTriggered = true;

          }
        } else {
          tmSetPressStart = 0;
          tmLongPressTriggered = false;
        }

        if (buttonJustPressed[4]) {
          if (tmFocus == 0) {
            startAnimation(SCREEN_MENU, 64);
          }
          else if (tmMode == TM_SETTING) {
            if (tmFocus == 1) {
              if (tmActiveUnit == 0) tmHours = (tmHours + 1) % 24;
              else if (tmActiveUnit == 1) tmMinutes = (tmMinutes + 1) % 60;
              else if (tmActiveUnit == 2) tmSeconds = (tmSeconds + 1) % 60;
            } else if (tmFocus == 2) {
              tmActiveUnit = (tmActiveUnit + 1) % 3;
            } else if (tmFocus == 3) {
              if (tmActiveUnit == 0) tmHours = (tmHours + 23) % 24;
              else if (tmActiveUnit == 1) tmMinutes = (tmMinutes + 59) % 60;
              else if (tmActiveUnit == 2) tmSeconds = (tmSeconds + 59) % 60;
            }
          }
          else if (tmMode == TM_READY) {
            if (tmFocus == 1) tmMode = TM_SETTING;
            else if (tmFocus == 3) {
              tmRemainingMillis = ((unsigned long)tmHours * 3600 + tmMinutes * 60 + tmSeconds) * 1000;
              if (tmRemainingMillis > 0) {
                tmMode = TM_RUNNING;
                tmLastTick = millis();
              }
            }
          }
          else if (tmMode == TM_RUNNING) {
            if (tmFocus == 1) tmMode = TM_READY;
            else if (tmFocus == 3) {

            }
          }
        }
      }
      else if (currentScreen == SCREEN_TIMER_ALERT) {
        if (buttonJustPressed[0] || buttonJustPressed[1] || buttonJustPressed[2] ||
            buttonJustPressed[3] || buttonJustPressed[4]) {
          tmMode = TM_SETTING;
          startAnimation(SCREEN_WATCHFACE, 64);
        }
      }
    }

    if (tmMode == TM_RUNNING) {
      unsigned long now = millis();
      unsigned long delta = now - tmLastTick;
      if (tmRemainingMillis > delta) {
        tmRemainingMillis -= delta;
        tmLastTick = now;
      } else {
        tmRemainingMillis = 0;
        tmMode = TM_RINGING;
        currentScreen = SCREEN_TIMER_ALERT;
        if (!displayOn) {
          display.ssd1306_command(SSD1306_DISPLAYON);
          displayOn = true;
        }
        lastActivityTime = millis();
      }
    }

    else if (currentScreen == SCREEN_RADIO) {
      if (buttonJustPressed[0]) { if (radioCursor > -1) radioCursor--; }
      if (buttonJustPressed[1]) { if (radioCursor < 3) radioCursor++; }
      if (buttonJustPressed[2]) {
        if (radioCursor == -1) radioCursor = 0;
      }
      if (buttonJustPressed[4]) {
        if (radioCursor == -1) {
          startAnimation(SCREEN_MENU, 64);
        }
        else if (radioCursor == 0) {
          if (WiFi.status() == WL_CONNECTED) { wifiHomeCursor = 0; startAnimation(SCREEN_WIFI_HOME, -64); }
          else if (wifiAutoOn || wifiEnabled) {

            if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
              wifiScanStart = millis();
              wifiScanResults = -1;
              startAnimation(SCREEN_WIFI_SCANNING, -64);
            } else {
              WiFi.scanDelete();
              WiFi.disconnect();
              vTaskDelay(pdMS_TO_TICKS(100));
              WiFi.scanNetworks(true, false, false, 250);
              wifiScanStart = millis();
              wifiScanResults = -1;
              startAnimation(SCREEN_WIFI_SCANNING, -64);
            }
          }
          else { wifiConfirmCursor = 0; startAnimation(SCREEN_RADIO_WIFI, -64); }
        } else if (radioCursor == 1) { bleHomeCursor = 0; startAnimation(SCREEN_RADIO_BLE, -64); }
        else if (radioCursor == 2) startAnimation(SCREEN_RADIO_ESPNOW, -64);
        else if (radioCursor == 3) {
          powerHomeCursor = 0;
          startAnimation(SCREEN_POWER_HOME, -64);
        }
      }
    }
    else if (currentScreen == SCREEN_POWER_HOME) {
      if (buttonJustPressed[0]) { if (powerHomeCursor > -1) powerHomeCursor--; }
      if (buttonJustPressed[1]) { if (powerHomeCursor < 2) powerHomeCursor++; }
      if (buttonJustPressed[4]) {
        if (powerHomeCursor == -1) startAnimation(SCREEN_RADIO, 64);
        else if (powerHomeCursor == 0) {
          wifiConfirmCursor = wifiAutoOn ? 1 : 0;
          startAnimation(SCREEN_WIFI_AUTO_CONFIRM, -64);
        }
        else if (powerHomeCursor == 1) {
          wifiConfirmCursor = espNowAutoOn ? 1 : 0;
          startAnimation(SCREEN_ESPNOW_AUTO_CONFIRM, -64);
        }
        else if (powerHomeCursor == 2) {
          powerModeButtonCursor = 2;
          startAnimation(SCREEN_POWER_MODE, -64);
        }
      }
    }
    else if (currentScreen == SCREEN_WIFI_AUTO_CONFIRM) {
      if (buttonJustPressed[0]) wifiConfirmCursor = -1;
      if (buttonJustPressed[1] && wifiConfirmCursor == -1) wifiConfirmCursor = 0;
      if (buttonJustPressed[2] && wifiConfirmCursor != -1) wifiConfirmCursor = 0;
      if (buttonJustPressed[3] && wifiConfirmCursor != -1) wifiConfirmCursor = 1;
      if (buttonJustPressed[4]) {
        if (wifiConfirmCursor == -1) startAnimation(SCREEN_POWER_HOME, 64);
        else {
          wifiAutoOn = (wifiConfirmCursor == 0);
          prefs.begin("powermode", false);
          prefs.putBool("wifiAutoOn", wifiAutoOn);
          prefs.end();
          startAnimation(SCREEN_POWER_HOME, 64);
        }
      }
    }
    else if (currentScreen == SCREEN_ESPNOW_AUTO_CONFIRM) {
      if (buttonJustPressed[0]) wifiConfirmCursor = -1;
      if (buttonJustPressed[1] && wifiConfirmCursor == -1) wifiConfirmCursor = 0;
      if (buttonJustPressed[2] && wifiConfirmCursor != -1) wifiConfirmCursor = 0;
      if (buttonJustPressed[3] && wifiConfirmCursor != -1) wifiConfirmCursor = 1;
      if (buttonJustPressed[4]) {
        if (wifiConfirmCursor == -1) startAnimation(SCREEN_POWER_HOME, 64);
        else {
          espNowAutoOn = (wifiConfirmCursor == 0);
          prefs.begin("powermode", false);
          prefs.putBool("espNowAutoOn", espNowAutoOn);
          prefs.end();
          startAnimation(SCREEN_POWER_HOME, 64);
        }
      }
    }
    else if (currentScreen == SCREEN_RADIO_BLE) {
      bool bleIsConn = bleConnected || (pBleServer && pBleServer->getConnectedCount() > 0);
      if (buttonJustPressed[0]) { if (bleHomeCursor > -1) bleHomeCursor--; }
      if (buttonJustPressed[1]) {
        int maxCur = bleIsConn ? 1 : 0;
        if (bleHomeCursor < maxCur) bleHomeCursor++;
      }
      if (buttonJustPressed[4]) {
        if (bleHomeCursor == -1) startAnimation(SCREEN_RADIO, 64);
        else if (!bleIsConn) {
          startAnimation(SCREEN_RADIO, 64);
        } else {
          if (bleHomeCursor == 0) {
            kbInputBuffer = bleDeviceName;
            kbTextCursor = kbInputBuffer.length();
            kbScrollOffset = 0;
            kbReturnScreen = SCREEN_RADIO_BLE;
            kbCursorRow = 0; kbCursorCol = 0;
            currentKBMode = KB_LOWER;
            currentScreen = SCREEN_WIFI_KEYBOARD;
          } else if (bleHomeCursor == 1) {
            bleConfirmMode = 3; bleConfirmCursor = 0;
            startAnimation(SCREEN_RADIO_BLE_CONFIRM, -64);
          }
        }
      }
    }
    else if (currentScreen == SCREEN_RADIO_BLE_CONFIRM) {
      if (buttonJustPressed[0]) { bleConfirmCursor = -1; }
      if (buttonJustPressed[2]) { if (bleConfirmCursor != -1) bleConfirmCursor = 0; }
      if (buttonJustPressed[3]) { if (bleConfirmCursor != -1) bleConfirmCursor = 1; }
      if (buttonJustPressed[1]) { if (bleConfirmCursor == -1) bleConfirmCursor = 0; }
      if (buttonJustPressed[4]) {
        if (bleConfirmCursor == -1 || bleConfirmCursor == 1) {
          startAnimation(SCREEN_RADIO_BLE, 64);
        } else if (bleConfirmCursor == 0) {
          if (bleConfirmMode == 0) {
            bleDeviceName = kbInputBuffer;
            saveBleSettings();

            NimBLEDevice::getAdvertising()->stop();
            NimBLEAdvertisementData advData;
            advData.setName(bleDeviceName.c_str());
            advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
            NimBLEDevice::getAdvertising()->setAdvertisementData(advData);
            
            NimBLEAdvertisementData scanData;
            scanData.setName(bleDeviceName.c_str());
            NimBLEDevice::getAdvertising()->setScanResponseData(scanData);
            NimBLEDevice::getAdvertising()->start();
          } else if (bleConfirmMode == 1 || bleConfirmMode == 2) {

            bleConnected = false;
            bleHomeCursor = 0;
            if (pBleServer) {
              uint16_t numClients = pBleServer->getConnectedCount();
              for (uint16_t i = 0; i < numClients; i++) {
                pBleServer->disconnect(i);
              }
              NimBLEDevice::getAdvertising()->stop();
              vTaskDelay(pdMS_TO_TICKS(100));
              if(!NimBLEDevice::getAdvertising()->isAdvertising()) { NimBLEDevice::getAdvertising()->start(); }
            }
          } else if (bleConfirmMode == 3) {
            bleSettingAccess = !bleSettingAccess;
            saveBleSettings();
          }
          startAnimation(SCREEN_RADIO_BLE, 64);
        }
      }
    }
    else if (currentScreen == SCREEN_RADIO_WIFI) {
      if (buttonJustPressed[0]) wifiConfirmCursor = -1;
      if (buttonJustPressed[1]) { if (wifiConfirmCursor == -1) wifiConfirmCursor = 0; }
      if (buttonJustPressed[3]) wifiConfirmCursor = 1;
      if (buttonJustPressed[2]) { if (wifiConfirmCursor == 1) wifiConfirmCursor = 0; }
      if (buttonJustPressed[4]) {
        if (wifiConfirmCursor == 0) {
          wifiEnabled = true;
          wifiStatusMsg = "";
          wifiRetryCount = 0;
          WiFi.mode(WIFI_STA);

          WiFi.scanDelete();

          if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
            wifiScanStart = millis();
            wifiScanResults = -1;
            startAnimation(SCREEN_WIFI_SCANNING, -64);
          } else {
            WiFi.disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            WiFi.scanNetworks(true);
            wifiScanStart = millis();
            wifiScanResults = -1;
            startAnimation(SCREEN_WIFI_SCANNING, -64);
          }
        } else {
          startAnimation(SCREEN_RADIO, 64);
        }
      }
    }
    else if (currentScreen == SCREEN_WIFI_SCANNING) {
      if (wifiScanResults <= -1) {
        wifiScanResults = WiFi.scanComplete();
        if (wifiScanResults >= 0) {
          wifiListIndex = 0;
          wifiListScroll = 0;
          startAnimation(SCREEN_WIFI_RESULTS, -64);
        } else if (wifiScanResults == -2) {

           wifiScanResults = 0;
           startAnimation(SCREEN_WIFI_RESULTS, -64);
        } else if (millis() - wifiScanStart > 10000) {
          wifiScanResults = 0;
          startAnimation(SCREEN_WIFI_RESULTS, -64);
        }
      }
    }
    else if (currentScreen == SCREEN_RADIO_ESPNOW) {
      if (buttonJustPressed[4]) startAnimation(SCREEN_RADIO, 64);
    }

    else if (currentScreen == SCREEN_POWER_MODE) {
      if (buttonJustPressed[0]) powerModeButtonCursor = -1;
      if (buttonJustPressed[1] && powerModeButtonCursor == -1) powerModeButtonCursor = 2;
      if (buttonJustPressed[2]) {
        if (powerModeButtonCursor > 0) powerModeButtonCursor--;
      }
      if (buttonJustPressed[3]) {
        if (powerModeButtonCursor < 2) powerModeButtonCursor++;
      }
      if (buttonJustPressed[4]) {
        if (powerModeButtonCursor == -1) {
          startAnimation(SCREEN_POWER_HOME, 64);
        } else if (powerModeButtonCursor == 0) {
          currentPowerMode = POWER_ECO;
          ecoModeActive = true;

          prefs.begin("powermode", false);
          prefs.putBool("ecoMode", true);
          prefs.end();
          ecoModeSleepStart = millis();
          startAnimation(SCREEN_WATCHFACE, 64);
        } else if (powerModeButtonCursor == 1) {
          startAnimation(SCREEN_POWER_MODE_HELP, -64);
        } else if (powerModeButtonCursor == 2) {
          startAnimation(SCREEN_POWER_HOME, 64);
        }
      }
    }

    else if (currentScreen == SCREEN_POWER_MODE_HELP) {
      if (buttonJustPressed[4]) {
        startAnimation(SCREEN_POWER_MODE, 64);
      }
    }

    else if (currentScreen == SCREEN_ECO_EXIT_CONFIRM) {
      if (buttonJustPressed[0]) {
        if (ecoExitConfirmCursor != -1) ecoExitConfirmCursor = -1;
      }
      if (buttonJustPressed[1]) {
        if (ecoExitConfirmCursor == -1) ecoExitConfirmCursor = 1;
      }
      if (buttonJustPressed[2]) {
        if (ecoExitConfirmCursor != -1) ecoExitConfirmCursor = 0;
      }
      if (buttonJustPressed[3]) {
        if (ecoExitConfirmCursor != -1) ecoExitConfirmCursor = 1;
      }
      if (buttonJustPressed[4]) {
        if (ecoExitConfirmCursor == -1) {
          startAnimation(SCREEN_WATCHFACE, 64);
        } else if (ecoExitConfirmCursor == 0) {

          currentPowerMode = POWER_NORMAL;
          ecoModeActive = false;

          prefs.begin("powermode", false);
          prefs.putBool("ecoMode", false);
          prefs.end();
          ecoExitPressStart = 0;
          startAnimation(SCREEN_WATCHFACE, 64);
        } else if (ecoExitConfirmCursor == 1) {
          startAnimation(SCREEN_WATCHFACE, 64);
        }
      }
    }

    else if (currentScreen == SCREEN_AI_CHAT) {
      if (aiScrollMode) {

        if (buttonJustPressed[0]) {
          if (aiChatScroll > 0) { aiChatScroll--; aiUpdateCache(); }
        }
        if (buttonJustPressed[1]) {
          if (aiChatScroll < aiMsgCount - 1) { aiChatScroll++; aiUpdateCache(); }
        }
        if (buttonJustPressed[4]) { aiScrollMode = false; }
      } else {

        if (buttonJustPressed[0]) {
          if (aiCursor == 0) aiCursor = -2;
          else if (aiCursor == 1 || aiCursor == 2) aiCursor = 0;
        }
        if (buttonJustPressed[1]) {
          if (aiCursor == -2 || aiCursor == -1) aiCursor = 0;
          else if (aiCursor == 0) aiCursor = 1;
        }
        if (buttonJustPressed[2]) {
          if (aiCursor == -1) aiCursor = -2;
          else if (aiCursor == 2) aiCursor = 1;
        }
        if (buttonJustPressed[3]) {
          if (aiCursor == -2) aiCursor = -1;
          else if (aiCursor == 1) aiCursor = 2;
        }
        if (buttonJustPressed[4]) {
          if (aiCursor == -2) {
            WiFi.setSleep(true);
            startAnimation(SCREEN_MENU, 64);
          } else if (aiCursor == -1) {
            aiClearChat();
          } else if (aiCursor == 0) {
            aiScrollMode = true;
          } else if (aiCursor == 1) {
            kbInputBuffer = aiInputText;
            kbTextCursor = kbInputBuffer.length();
            kbScrollOffset = 0;
            kbReturnScreen = SCREEN_AI_CHAT;
            kbCursorRow = 0; kbCursorCol = 0;
            currentKBMode = KB_LOWER;
            currentScreen = SCREEN_WIFI_KEYBOARD;
          } else if (aiCursor == 2) {
            if (aiInputText.length() > 0) {
              if (!wifiEnabled) {
                aiWifiErrorType = 0; aiWifiErrorCursor = 1;
                startAnimation(SCREEN_AI_WIFI_ERROR, -64);
              } else if (WiFi.status() != WL_CONNECTED) {
                aiWifiErrorType = 1; aiWifiErrorCursor = 1;
                startAnimation(SCREEN_AI_WIFI_ERROR, -64);
              } else if (!internetOK) {
                aiWifiErrorType = 2; aiWifiErrorCursor = 1;
                startAnimation(SCREEN_AI_WIFI_ERROR, -64);
              } else if (aiMsgCount >= AI_MAX_MESSAGES) {

              } else {
                aiAddMessage("U:" + aiInputText);
                aiInputText = "";
                aiThinking = true;
                aiThinkStart = millis();
                aiResponseReady = false;
                aiResponseError = false;
                aiChatScroll = max(0, aiMsgCount - 4);
                aiUpdateCache();

                xTaskCreate(aiSendTask, "AISend", 12288, NULL, 1, NULL);
                startAnimation(SCREEN_AI_THINKING, -64);
              }
            }
          }
        }
      }
    }
    else if (currentScreen == SCREEN_AI_THINKING) {

    }
    else if (currentScreen == SCREEN_AI_WIFI_ERROR) {
      if (buttonJustPressed[0]) { aiWifiErrorCursor = 0; }
      if (buttonJustPressed[1]) { aiWifiErrorCursor = 1; }
      if (buttonJustPressed[4]) {
        startAnimation(SCREEN_AI_CHAT, 64);
      }
    }
    else if (currentScreen == SCREEN_CALCULATOR) {
      // Navigation: 5 rows x 4 cols, wrap around
      if (buttonJustPressed[0]) calcCursorRow = (calcCursorRow > 0) ? calcCursorRow - 1 : 4;
      if (buttonJustPressed[1]) calcCursorRow = (calcCursorRow < 4) ? calcCursorRow + 1 : 0;
      if (buttonJustPressed[2]) {
        calcCursorCol--;
        if (calcCursorCol < 0) calcCursorCol = 3;
      }
      if (buttonJustPressed[3]) {
        calcCursorCol++;
        if (calcCursorCol > 3) calcCursorCol = 0;
      }

      if (buttonJustPressed[4]) {
        const char* key = calcGrid[calcCursorRow][calcCursorCol];
        String keyStr = String(key);

        if (keyStr == "<--") {
          calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
          startAnimation(SCREEN_MENU, 64);

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
            if (calcOp=='+') res=v1+v2;
            else if (calcOp=='-') res=v1-v2;
            else if (calcOp=='*') res=v1*v2;
            else if (calcOp=='/') { if (v2!=0) res=v1/v2; else dz=true; }
            if (dz) { calcError = true; }
            else {
              calcInput1 = String(res, 4);
              while (calcInput1.endsWith("0") && calcInput1.indexOf(".")!=-1) calcInput1.remove(calcInput1.length()-1);
              if (calcInput1.endsWith(".")) calcInput1.remove(calcInput1.length()-1);
              calcInput2 = ""; calcOp = ' '; calcIsOpSet = false;
            }
          }

        } else if (keyStr=="+" || keyStr=="-" || keyStr=="*" || keyStr=="/") {
          char op = key[0];
          if (!calcError && calcInput1.length() > 0) {
            if (calcIsOpSet && calcInput2.length() > 0) {
              float v1=calcInput1.toFloat(), v2=calcInput2.toFloat(), res=0;
              bool dz=false;
              if (calcOp=='+') res=v1+v2;
              else if (calcOp=='-') res=v1-v2;
              else if (calcOp=='*') res=v1*v2;
              else if (calcOp=='/') { if (v2!=0) res=v1/v2; else dz=true; }
              if (dz) { calcError=true; }
              else {
                calcInput1=String(res,4);
                while(calcInput1.endsWith("0")&&calcInput1.indexOf(".")!=-1) calcInput1.remove(calcInput1.length()-1);
                if(calcInput1.endsWith(".")) calcInput1.remove(calcInput1.length()-1);
                calcInput2=""; calcOp=op; calcIsOpSet=true;
              }
            } else { calcOp=op; calcIsOpSet=true; }
          }

        } else if (keyStr == "SCI") {
          calcSciRow = 0; calcSciCol = 0;
          startAnimation(SCREEN_CALCULATOR_SCI, -64);

        } else if (keyStr != "") {
          // digit or .
          if (calcError) { calcInput1=""; calcInput2=""; calcOp=' '; calcIsOpSet=false; calcError=false; }
          if (calcIsOpSet) {
            if (keyStr=="." && calcInput2.indexOf(".")!=-1) {}
            else if (calcInput2.length()<10) calcInput2 += keyStr;
          } else {
            if (keyStr=="." && calcInput1.indexOf(".")!=-1) {}
            else if (calcInput1.length()<10) calcInput1 += keyStr;
          }
        }
      }
    }

    else if (currentScreen == SCREEN_CALCULATOR_SCI) {
      if (buttonJustPressed[0]) calcSciRow = (calcSciRow > 0) ? calcSciRow - 1 : 2;
      if (buttonJustPressed[1]) calcSciRow = (calcSciRow < 2) ? calcSciRow + 1 : 0;
      if (buttonJustPressed[2]) calcSciCol = (calcSciCol > 0) ? calcSciCol - 1 : 1;
      if (buttonJustPressed[3]) calcSciCol = (calcSciCol < 1) ? calcSciCol + 1 : 0;

      if (buttonJustPressed[4]) {
        const char* key = sciGrid[calcSciRow][calcSciCol];
        if (strcmp(key, "BACK") == 0) {
          startAnimation(SCREEN_CALCULATOR, 64);
          return;
        }
        
        // Apply SCI function to active input
        String* activeInput = calcIsOpSet ? &calcInput2 : &calcInput1;
        float v = activeInput->toFloat();
        bool hasVal = (activeInput->length() > 0 && !calcError);
        String result = "";
        bool sciError = false;

        if (strcmp(sciGrid[calcSciRow][calcSciCol], "x^2") == 0) {
          if (hasVal) result = String(v * v, 4);
        } else if (strcmp(sciGrid[calcSciRow][calcSciCol], "sqrt") == 0) {
          if (hasVal && v >= 0) result = String(sqrt(v), 4);
          else sciError = true;
        } else if (strcmp(sciGrid[calcSciRow][calcSciCol], "pi") == 0) {
          result = "3.14159265";
        } else if (strcmp(sciGrid[calcSciRow][calcSciCol], "x^3") == 0) {
          if (hasVal) result = String(v * v * v, 4);
        } else if (strcmp(sciGrid[calcSciRow][calcSciCol], "cbrt") == 0) {
          if (hasVal) {
            float cb = (v >= 0) ? pow(v, 1.0f/3.0f) : -pow(-v, 1.0f/3.0f);
            result = String(cb, 4);
          }
        }

        if (sciError) {
          calcError = true;
        } else if (result.length() > 0) {
          // Trim trailing zeros
          while (result.endsWith("0") && result.indexOf(".") != -1) result.remove(result.length()-1);
          if (result.endsWith(".")) result.remove(result.length()-1);
          // If pi was inserted into empty, set as calcInput1 directly
          if (strcmp(sciGrid[calcSciRow][calcSciCol], "pi") == 0) {
            if (!calcIsOpSet) calcInput1 = result;
            else calcInput2 = result;
          } else {
            *activeInput = result;
          }
        }
        startAnimation(SCREEN_CALCULATOR, 64);
      }
    }

    else if (currentScreen == SCREEN_WIFI_RESULTS) {
      if (buttonJustPressed[0]) { if (wifiListIndex > -1) wifiListIndex--; }
      if (buttonJustPressed[1]) { if (wifiListIndex < wifiScanResults - 1) wifiListIndex++; }
      if (buttonJustPressed[2]) { if (wifiListIndex == -1) wifiListIndex = 0; }
      if (buttonJustPressed[4]) {
        if (wifiListIndex == -1) {
          WiFi.disconnect();
          startAnimation(SCREEN_RADIO, 64);
        }
        else {
          wifiTargetSSID = WiFi.SSID(wifiListIndex);
          wifiPassword   = findSavedPwd(wifiTargetSSID);
          kbInputBuffer  = wifiPassword;
          wifiStatusMsg  = "";
          wifiRetryCount = 0;
          wifiPasswordCursor = -1;
          startAnimation(SCREEN_WIFI_PASSWORD, -64);
        }
      }
    }
    else if (currentScreen == SCREEN_WIFI_PASSWORD) {

      if (buttonJustPressed[0]) {
        if (wifiPasswordCursor == 0 || wifiPasswordCursor == 1) wifiPasswordCursor = -1;
      }
      if (buttonJustPressed[1]) {
        if (wifiPasswordCursor == -1) wifiPasswordCursor = 0;
      }
      if (buttonJustPressed[2]) {
        if (wifiPasswordCursor == 1) wifiPasswordCursor = 0;
      }
      if (buttonJustPressed[3]) {
        if (wifiPasswordCursor == 0) wifiPasswordCursor = 1;
      }
      if (buttonJustPressed[4]) {
        if (wifiPasswordCursor == -1) {
          WiFi.disconnect();
          startAnimation(SCREEN_WIFI_RESULTS, 64);
        }
        else if (wifiPasswordCursor == 0) {
          kbInputBuffer = wifiPassword; currentKBMode = KB_LOWER;
          kbCursorRow = 0; kbCursorCol = 0;
          kbTextCursor = kbInputBuffer.length();
          kbScrollOffset = 0;
          kbReturnScreen = SCREEN_WIFI_PASSWORD;
          currentScreen = SCREEN_WIFI_KEYBOARD;
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
          wifiConnecting = false;
          wifiRetryCount = 0;
          WiFi.setSleep(true);
          saveCredential(wifiTargetSSID, wifiPassword);
          syncTimeWithNTP();
          wifiHomeCursor = 0;
          startAnimation(SCREEN_WIFI_HOME, -64);
          lastActivityTime = millis();
        } else if (WiFi.status() == WL_CONNECT_FAILED) {
          wifiConnecting = false;
          wifiRetryCount++;
          WiFi.disconnect();
          if (wifiRetryCount >= 5) wifiStatusMsg = "Check signal/pass";
          else wifiStatusMsg = "Failed! Wrong PW?";
        } else if (millis() - wifiConnectStartTime > 15000) {

          wifiConnecting = false;
          wifiRetryCount++;
          WiFi.disconnect();
          if (wifiRetryCount >= 5) wifiStatusMsg = "Check Router distance";
          else wifiStatusMsg = "Try Again...";
        }
      }
    }
    else if (currentScreen == SCREEN_WIFI_KEYBOARD) {
      if (buttonJustPressed[0]) { if (kbCursorRow > 0) kbCursorRow--; }
      if (buttonJustPressed[1]) { if (kbCursorRow < 3) kbCursorRow++; }
      if (buttonJustPressed[2]) { if (kbCursorCol > 0) kbCursorCol--; else { kbCursorCol = 10; if (kbCursorRow > 0) kbCursorRow--; } }
      if (buttonJustPressed[3]) { if (kbCursorCol < 10) kbCursorCol++; else { kbCursorCol = 0; if (kbCursorRow < 3) kbCursorRow++; } }
      if (buttonJustPressed[4]) {
        const char* key;
        if (currentKBMode == KB_UPPER) key = kbUpper[kbCursorRow][kbCursorCol];
        else if (currentKBMode == KB_SYMBOL) key = kbSymbol[kbCursorRow][kbCursorCol];
        else key = kbLower[kbCursorRow][kbCursorCol];

        if (strcmp(key, "EN") == 0) {
          if (kbReturnScreen == SCREEN_RADIO_BLE) {
            if (kbInputBuffer == bleDeviceName) {
              currentScreen = SCREEN_RADIO_BLE;
            } else {
              bleConfirmMode = 0; bleConfirmCursor = 0;
              currentScreen = SCREEN_RADIO_BLE_CONFIRM;
            }
          } else if (kbReturnScreen == SCREEN_AI_CHAT) {
            aiInputText = kbInputBuffer;
            if (aiInputText.length() > 128) aiInputText = aiInputText.substring(0, 128);
            currentScreen = SCREEN_AI_CHAT;
          } else {
            wifiPassword = kbInputBuffer;
            currentScreen = kbReturnScreen;
          }
        }
        else if (strcmp(key, "CL") == 0) {
           if (kbTextCursor > 0) kbTextCursor--;
        }
        else if (strcmp(key, "CR") == 0) {
           if (kbTextCursor < (int)kbInputBuffer.length()) kbTextCursor++;
        }
        else if (strcmp(key, "BS") == 0) {
           if (kbTextCursor > 0 && kbInputBuffer.length() > 0) {
             kbInputBuffer.remove(kbTextCursor - 1, 1);
             kbTextCursor--;
           }
        }
        else if (strcmp(key, "^") == 0) {
          if (currentKBMode == KB_LOWER) currentKBMode = KB_UPPER;
          else if (currentKBMode == KB_UPPER) currentKBMode = KB_SYMBOL;
          else currentKBMode = KB_LOWER;
        }
        else {
           kbInputBuffer = kbInputBuffer.substring(0, kbTextCursor) + String(key) + kbInputBuffer.substring(kbTextCursor);
           kbTextCursor += String(key).length();
        }
      }
    }
    else if (currentScreen == SCREEN_WIFI_HOME) {
      if (buttonJustPressed[0]) { if (wifiHomeCursor > -1) wifiHomeCursor--; }
      if (buttonJustPressed[1]) { if (wifiHomeCursor < 2) wifiHomeCursor++; }
      if (buttonJustPressed[2]) { if (wifiHomeCursor == -1) wifiHomeCursor = 0; }
      if (buttonJustPressed[4]) {
        if (wifiHomeCursor == -1) startAnimation(SCREEN_RADIO, 64);
        else if (wifiHomeCursor == 0) { wifiToggleCursor = 1; startAnimation(SCREEN_WIFI_TOGGLE_CONFIRM, -64); }
        else if (wifiHomeCursor == 1) {
          if (WiFi.status() == WL_CONNECTED) {
            ntpSyncStart = millis();
            ntpSyncState = 0;
            configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
            startAnimation(SCREEN_WIFI_NTP, -64);
          }
        }
        else if (wifiHomeCursor == 2) { forgetCredential(WiFi.SSID()); WiFi.disconnect(); startAnimation(SCREEN_RADIO, 64); }
      }
    }
    else if (currentScreen == SCREEN_WIFI_TOGGLE_CONFIRM) {
      if (buttonJustPressed[2]) wifiToggleCursor = 0;
      if (buttonJustPressed[3]) wifiToggleCursor = 1;
      if (buttonJustPressed[4]) {
        if (wifiToggleCursor == 0) {
          wifiEnabled = false;
          WiFi.disconnect();
          WiFi.mode(WIFI_OFF);
        }
        startAnimation(SCREEN_RADIO, 64);
      }
    }
    else if (currentScreen == SCREEN_RADIO_ESPNOW) {
      if (buttonJustPressed[4]) startAnimation(SCREEN_RADIO, 64);
    }

    static unsigned long lastStatusUpdate = 0;
    if (millis() - lastStatusUpdate > 100) {
      statusIndex++;
      lastStatusUpdate = millis();
    }

    updateDisplay();
  }

  static unsigned long lastBleCheck = 0;
  if (millis() - lastBleCheck > 3000) {
    lastBleCheck = millis();
    if (pBleServer && pBleServer->getConnectedCount() == 0) {
      if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        Serial.println("BLE watchdog: Restarting advertising...");
        NimBLEDevice::getAdvertising()->start();
      }
    }
  }

  delay(5);
}
void startAnimation(ScreenState next, int targetOffset) {
  isAnimating = true;
  animNextScreen = next;
  animOffsetY = 0;
  animTargetY = targetOffset;
}

void updateDisplay() {
  display.clearDisplay();

  if (isAnimating) {
    if (animTargetY < 0) {
      drawScreen(currentScreen, animOffsetY);
      drawScreen(animNextScreen, animOffsetY + 64);
    } else {
      drawScreen(currentScreen, animOffsetY);
      drawScreen(animNextScreen, animOffsetY - 64);
    }
  } else {
    drawScreen(currentScreen, 0);
  }

  display.display();

  // Handle Display Mirroring
  if (bleConnected && mirroringActive) {
    if (millis() - lastMirrorUpdate >= MIRROR_INTERVAL) {
      uint8_t* buffer = display.getBuffer();
      int offset = 0;
      uint8_t chunkIdx = 0;
      while (offset < 1024) {
        int size = min(500, 1024 - offset);
        uint8_t packet[501];
        packet[0] = chunkIdx;
        memcpy(packet + 1, buffer + offset, size);
        pDisplayCharacteristic->setValue(packet, size + 1);
        pDisplayCharacteristic->notify();
        offset += size;
        chunkIdx++;
        vTaskDelay(pdMS_TO_TICKS(10)); // Allow NimBLE stack to process
      }
      lastMirrorUpdate = millis();
    }
  }
}

void drawScreen(ScreenState screen, int yOffset) {
  if (screen == SCREEN_WATCHFACE) drawWatchFace(yOffset);
  else if (screen == SCREEN_MENU) drawMenu(yOffset);
  else if (screen == SCREEN_STOPWATCH) drawStopwatch(yOffset);
  else if (screen == SCREEN_TIMER) drawTimer(yOffset);
  else if (screen == SCREEN_TIMER_ALERT) drawTimerAlert(yOffset);
  else if (screen == SCREEN_RADIO) drawRadioHome(yOffset);
  else if (screen == SCREEN_RADIO_WIFI) drawRadioWifi(yOffset);
  else if (screen == SCREEN_WIFI_CONFIRM) drawWifiConfirm(yOffset);
  else if (screen == SCREEN_WIFI_SCANNING) drawWifiScanning(yOffset);
  else if (screen == SCREEN_WIFI_RESULTS) drawWifiResults(yOffset);
  else if (screen == SCREEN_WIFI_PASSWORD) drawWifiPassword(yOffset);
  else if (screen == SCREEN_WIFI_KEYBOARD) drawWifiKeyboard();
  else if (screen == SCREEN_WIFI_HOME) drawWifiHome(yOffset);
  else if (screen == SCREEN_WIFI_TOGGLE_CONFIRM) drawWifiToggleConfirm(yOffset);
  else if (screen == SCREEN_RADIO_BLE) drawRadioBleHome(yOffset);
  else if (screen == SCREEN_RADIO_BLE_CONFIRM) drawBleConfirm(yOffset);
  else if (screen == SCREEN_RADIO_ESPNOW) drawRadioStub(yOffset, "ESP-NOW");
  else if (screen == SCREEN_POWER_HOME) drawPowerHome(yOffset);
  else if (screen == SCREEN_WIFI_AUTO_CONFIRM) drawWifiAutoConfirm(yOffset);
  else if (screen == SCREEN_ESPNOW_AUTO_CONFIRM) drawEspNowAutoConfirm(yOffset);
  else if (screen == SCREEN_POWER_MODE) drawPowerModeSelection(yOffset);
  else if (screen == SCREEN_POWER_MODE_HELP) drawPowerModeHelp(yOffset);
  else if (screen == SCREEN_ECO_EXIT_CONFIRM) drawEcoExitConfirm(yOffset);
  else if (screen == SCREEN_WIFI_NTP) drawWifiNTP(yOffset);
  else if (screen == SCREEN_AI_CHAT) drawAiChat(yOffset);
  else if (screen == SCREEN_AI_THINKING) drawAiThinking(yOffset);
  else if (screen == SCREEN_AI_WIFI_ERROR) drawAiWifiError(yOffset);
  else if (screen == SCREEN_CALCULATOR) drawCalculator(yOffset);
  else if (screen == SCREEN_CALCULATOR_SCI) drawCalculatorSci(yOffset);
}

void drawHeader(int yOffset, const char* appName, bool backFocused) {

  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);

  int currentIconX = (appName == nullptr) ? 4 : 28;
  int iconY = 2 + yOffset;

  if (WiFi.status() == WL_CONNECTED && (currentScreen == SCREEN_WATCHFACE || currentScreen == SCREEN_MENU)) {
    display.drawPixel(currentIconX+3, iconY+6, SSD1306_WHITE);
    display.drawPixel(currentIconX+4, iconY+6, SSD1306_WHITE);
    display.drawPixel(currentIconX+2, iconY+4, SSD1306_WHITE);
    display.drawPixel(currentIconX+3, iconY+3, SSD1306_WHITE);
    display.drawPixel(currentIconX+4, iconY+3, SSD1306_WHITE);
    display.drawPixel(currentIconX+5, iconY+4, SSD1306_WHITE);
    display.drawPixel(currentIconX+0, iconY+2, SSD1306_WHITE);
    display.drawPixel(currentIconX+1, iconY+1, SSD1306_WHITE);
    display.drawPixel(currentIconX+2, iconY+0, SSD1306_WHITE);
    display.drawPixel(currentIconX+5, iconY+0, SSD1306_WHITE);
    display.drawPixel(currentIconX+6, iconY+1, SSD1306_WHITE);
    display.drawPixel(currentIconX+7, iconY+2, SSD1306_WHITE);

    if (internetOK) {
      int tx = currentIconX + 11;
      int ty = iconY;
      display.drawFastVLine(tx + 0, ty + 6, 2, SSD1306_WHITE);
      display.drawFastVLine(tx + 2, ty + 4, 4, SSD1306_WHITE);
      display.drawFastVLine(tx + 4, ty + 2, 6, SSD1306_WHITE);
      display.drawFastVLine(tx + 6, ty + 0, 8, SSD1306_WHITE);
      currentIconX += 20;
    } else {
      currentIconX += 11;
    }
  }

  bool headerBleConnected = bleConnected || (pBleServer && pBleServer->getConnectedCount() > 0);
  if (headerBleConnected && (currentScreen == SCREEN_WATCHFACE || currentScreen == SCREEN_MENU)) {
    display.drawFastVLine(currentIconX + 3, iconY + 0, 9, SSD1306_WHITE);
    display.drawLine(currentIconX + 1, iconY + 2, currentIconX + 5, iconY + 6, SSD1306_WHITE);
    display.drawLine(currentIconX + 1, iconY + 6, currentIconX + 5, iconY + 2, SSD1306_WHITE);
    display.drawLine(currentIconX + 3, iconY + 0, currentIconX + 5, iconY + 2, SSD1306_WHITE);
    display.drawLine(currentIconX + 3, iconY + 8, currentIconX + 5, iconY + 6, SSD1306_WHITE);
  }

  if (appName != nullptr) {
    display.setTextSize(1);

    if (backFocused) {
      display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(2, 2 + yOffset);
    display.print("<--");

    display.setTextColor(SSD1306_WHITE);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(appName, 0, 0, &x1, &y1, &w, &h);

    display.setCursor((128 - w) / 2 - x1, 12/2 - h/2 - y1 + yOffset);
    display.print(appName);
  } else {
    display.setTextColor(SSD1306_WHITE);
    if (isStatusActive) {
      uint8_t dotPos = (statusIndex % 8) * 16;
      display.fillRect(dotPos, 2 + yOffset, 8, 4, SSD1306_WHITE);
    }
  }
}

void drawWatchFace(int yOffset) {

  drawHeader(yOffset);

  DateTime now = rtc.now();
  uint16_t hour = now.hour();
  bool isPM = hour >= 12;
  if (hour > 12) hour -= 12;
  else if (hour == 0) hour = 12;

  char timeBuf[6];
  sprintf(timeBuf, "%02d:%02d", hour, now.minute());
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeBuf, 0, 0, &x1, &y1, &w, &h);

  int16_t xPos = (128 - w + 1) / 2 - x1;
  int16_t yPos = 13 + (43 - h + 1) / 2 - y1 + yOffset;
  display.setCursor(xPos, yPos);
  display.print(timeBuf);

  display.setTextSize(1);
  display.setCursor(128 - 14, 56 + yOffset);
  display.print(isPM ? "PM" : "AM");

  display.setCursor(0, 56 + yOffset);
  const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  char dateBuf[16];
  sprintf(dateBuf, "%02d/%02d/%04d %s", now.day(), now.month(), now.year(), days[now.dayOfTheWeek()]);
  display.print(dateBuf);
}

void drawMenu(int yOffset) {
  drawHeader(yOffset);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int itemH = 12;
  int listY = 16 + yOffset;

  int startIdx = (menuIndex < 4) ? 0 : menuIndex - 3;
  int endIdx = min(startIdx + 4, (int)NUM_MENU_ITEMS);

  for (int i = startIdx; i < endIdx; i++) {
    int y = listY + ((i - startIdx) * itemH);

    if (i == menuIndex) {
      display.fillRect(0, y-1, 128, itemH, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(4, y);
    display.print(menuItems[i]);
  }
}

void drawStopwatch(int yOffset) {
  drawHeader(yOffset, "STOPWATCH", (swFocus == 0));

  unsigned long currentTotal = swElapsedTime;
  if (swRunning) {
    currentTotal += millis() - swStartTime;
  }

  unsigned long ms = (currentTotal % 1000) / 10;
  unsigned long totalSecs = currentTotal / 1000;
  unsigned long m = totalSecs / 60;
  unsigned long s = totalSecs % 60;

  char timeStr[10];
  sprintf(timeStr, "%02lu:%02lu", m, s);
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int16_t xPos = (SCREEN_WIDTH - w) / 2 - x1;
  int16_t yPos = 10 + (42 - h) / 2 - y1 + yOffset;
  display.setCursor(xPos, yPos);
  display.print(timeStr);

  char msStr[4];
  sprintf(msStr, "%02lu", ms);
  display.setTextSize(1);
  display.setCursor(SCREEN_WIDTH - 16, 16 + yOffset);
  display.print(msStr);

  display.setTextSize(1);

  const char* leftBtn = swRunning ? "PAUSE" : "START";
  int16_t lx1, ly1; uint16_t lw, lh;
  display.getTextBounds(leftBtn, 0, 0, &lx1, &ly1, &lw, &lh);
  int leftBtnX = (64 - (lw + 8)) / 2;
  drawBoxedCenteredText(display, leftBtn, leftBtnX, SCREEN_HEIGHT - 12 + yOffset, lw + 8, 11, (swFocus == 1));

  const char* rightBtn = swRunning ? "STOP" : "RESET";
  int16_t rx1, ry1; uint16_t rw, rh;
  display.getTextBounds(rightBtn, 0, 0, &rx1, &ry1, &rw, &rh);
  int rightBtnX = 64 + (64 - (rw + 8)) / 2;
  drawBoxedCenteredText(display, rightBtn, rightBtnX, SCREEN_HEIGHT - 12 + yOffset, rw + 8, 11, (swFocus == 2));
}

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

  display.setCursor(tx, ty);
  display.print(timeStr);

  if (tmMode == TM_SETTING) {
    int charW = 12;
    int16_t unitX = tx + (tmActiveUnit * 3 * charW);
    display.drawFastHLine(unitX, ty + h + 2, 22, SSD1306_WHITE);
  }

  if (tmMode == TM_SETTING) {

    drawBoxedCenteredText(display, "+", 10, 48 + yOffset, 20, 11, (tmFocus == 1));
    drawBoxedCenteredText(display, "SET", 44, 48 + yOffset, 40, 11, (tmFocus == 2));
    drawBoxedCenteredText(display, "-", 98, 48 + yOffset, 20, 11, (tmFocus == 3));
  } else if (tmMode == TM_READY || tmMode == TM_RUNNING) {

    const char* mainBtn = (tmMode == TM_RUNNING) ? "RESET" : "START";
    drawBoxedCenteredText(display, "SET", 15, 48 + yOffset, 40, 11, (tmFocus == 1));
    drawBoxedCenteredText(display, mainBtn, 70, 48 + yOffset, 45, 11, (tmFocus == 3));
  }
}

void drawTimerAlert(int yOffset) {
  bool isFlashOn = (millis() % 1000 < 500);

  if (isFlashOn) {

    display.fillRect(0, 0, 128, 64, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {

    display.setTextColor(SSD1306_WHITE);
  }

  drawCenteredText(display, "TIMER DONE", 15 + yOffset, 2);
  drawCenteredText(display, "Press any key", 40 + yOffset, 1);

  int x = 51, y = 51 + yOffset, w = 26, h = 11;
  if (isFlashOn) {

    display.fillRect(x, y, w, h, SSD1306_BLACK);
    display.setTextColor(SSD1306_WHITE);
  } else {

    display.fillRect(x, y, w, h, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }

  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds("OK", 0, 0, &x1, &y1, &tw, &th);
  display.setCursor(x + (w - tw) / 2 - x1, y + (h - th) / 2 - y1);
  display.print("OK");

  display.setTextColor(SSD1306_WHITE);
}

void buttonReadTask(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(50);

  while (1) {

    buttonStates[0] = !digitalRead(BTN_UP);
    buttonStates[1] = !digitalRead(BTN_DOWN);
    buttonStates[2] = !digitalRead(BTN_LEFT);
    buttonStates[3] = !digitalRead(BTN_RIGHT);
    buttonStates[4] = !digitalRead(BTN_CENTER);

    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

void drawCalculator(int yOffset) {
  // Build display string
  String disp;
  if (calcError) {
    disp = "Err: Div/0";
  } else {
    disp = (calcInput1.length() > 0) ? calcInput1 : "0";
    if (calcIsOpSet) {
      disp += ' '; disp += calcOp;
      if (calcInput2.length() > 0) { disp += ' '; disp += calcInput2; }
    }
  }
  // Clamp display to 18 chars (18*6=108px, leaving room)
  if ((int)disp.length() > 18) disp = disp.substring(disp.length() - 18);

  // --- Display bar: y=0..12 (13px tall) ---
  // Right-align the expression text. Each char = 6px at textSize 1.
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int dispX = 128 - (int)disp.length() * 6 - 1;
  if (dispX < 0) dispX = 0;
  display.setCursor(dispX, 3 + yOffset);
  display.print(disp);

  // Separator line
  display.drawFastHLine(0, 13 + yOffset, 128, SSD1306_WHITE);

  // --- Button grid: y=14..63 = 50px for 5 rows ---
  // CELL_W=32px (4 cols * 32 = 128), CELL_H=10px (5 rows * 10 = 50, fits in 50)
  const int GY   = 14 + yOffset;
  const int CW   = 32;
  const int CH   = 10;

  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 4; c++) {
      const char* lbl = calcGrid[r][c];
      if (lbl[0] == '\0') continue; // skip empty cell
      int cx = c * CW;
      int cy = GY + r * CH;
      bool sel = (r == calcCursorRow && c == calcCursorCol);

      if (sel) {
        display.fillRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.drawRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_WHITE);
        // Emphasize operator column (col 3) with an extra pixel width
        if (c == 3) display.drawFastVLine(cx + 1, cy, CH, SSD1306_WHITE);
      }
      // Center text: 6px per char at size 1
      int tw = strlen(lbl) * 6;
      int tx = cx + (CW - tw) / 2;
      int ty = cy + (CH - 7) / 2; // proper center padding (10px cell, 7px char = 1.5px top pad)
      display.setCursor(tx, ty);
      display.print(lbl);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawCalculatorSci(int yOffset) {
  // --- Display bar: show current active value (top 15px) ---
  String activeVal = calcIsOpSet ? calcInput2 : calcInput1;
  if (activeVal.length() == 0) activeVal = "0";
  if (calcError) activeVal = "Error";

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Left: "SCI" label
  display.setCursor(1, 3 + yOffset);
  display.print("SCI");

  // Right: active value right-aligned
  int valX = 128 - (int)activeVal.length() * 6 - 1;
  if (valX < 25) valX = 25;
  display.setCursor(valX, 3 + yOffset);
  display.print(activeVal);

  // Separator
  display.drawFastHLine(0, 13 + yOffset, 128, SSD1306_WHITE);

  // --- SCI grid: 3 rows x 2 cols ---
  // CELL_W=64px (2*64=128), CELL_H=16px (3*16=48, fits 14..63 = 50px)
  const int GY = 15 + yOffset;
  const int CW = 64;
  const int CH = 16;

  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 2; c++) {
      int cx = c * CW;
      int cy = GY + r * CH;
      bool sel = (r == calcSciRow && c == calcSciCol);
      const char* lbl = sciGrid[r][c];
      if (lbl[0] == '\0') continue;

      if (sel) {
        display.fillRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.drawRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_WHITE);
      }
      // Center label in cell
      int tw = strlen(lbl) * 6;
      int tx = cx + (CW - tw) / 2;
      int ty = cy + (CH - 7) / 2; // 7px = char height at size 1
      display.setCursor(tx, ty);
      display.print(lbl);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawCenteredText(Adafruit_SSD1306 &d, const String &text, int16_t y, uint8_t size) {
  int16_t x1, y1; uint16_t w, h;
  d.setTextSize(size);
  d.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  d.setCursor((SCREEN_WIDTH - w) / 2, y);
  d.print(text);
}

void drawBoxedCenteredText(Adafruit_SSD1306 &d, const char* text, int x, int y, int w, int h, bool inverted) {
  int16_t x1, y1; uint16_t tw, th;
  d.setTextSize(1);
  d.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  if (inverted) {
    d.fillRect(x, y, w, h, SSD1306_WHITE);
    d.setTextColor(SSD1306_BLACK);
  } else {
    d.drawRect(x, y, w, h, SSD1306_WHITE);
    d.setTextColor(SSD1306_WHITE);
  }
  d.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  d.print(text);
  d.setTextColor(SSD1306_WHITE);
}

class MyServerCallbacks: public NimBLEServerCallbacks {
public:

    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
      bleConnected = true;
      bleRemoteAddress = NimBLEAddress(desc->peer_ota_addr).toString().c_str();
      Serial.print("Device connected: ");
      Serial.println(bleRemoteAddress);
      
      if (pPhoneNameCharacteristic) {
        pPhoneNameCharacteristic->setValue("None");
        bleRemoteDeviceName = "None";
      }
      
      // Connection parameters update removed to let the central (phone) decide
      // and prevent immediate disconnections due to protocol collisions.
    };

    void onDisconnect(NimBLEServer* pServer) {
      bleConnected = false;
      bleRemoteDeviceName = "None";
      bleRemoteAddress = "";
      Serial.println("Device disconnected. Restarting advertising...");
      
      // Start advertising again - this is the key for reconnection
      if(!pServer->getAdvertising()->isAdvertising()) {
        pServer->getAdvertising()->start();
      }
    }

    void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
      bleConnected = false;
      bleRemoteDeviceName = "None";
      bleRemoteAddress = "";
      Serial.println("Device disconnected (desc). Restarting advertising...");
      
      if(!pServer->getAdvertising()->isAdvertising()) {
        pServer->getAdvertising()->start();
      }
    }
};

class MirrorControlCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            mirroringActive = (value[0] == 1);
            Serial.print("Mirroring state: ");
            Serial.println(mirroringActive ? "ON" : "OFF");
        }
    }
};

void initBLE() {
  prefs.begin("ble_settings", true);
  bleDeviceName = prefs.getString("name", "Ashutosh's Smartwatch");
  bleSettingAccess = prefs.getBool("access", false);
  prefs.end();

  NimBLEDevice::init(bleDeviceName.c_str());
  pBleServer = NimBLEDevice::createServer();
  pBleServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pBleServer->createService("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
  pBleNameCharacteristic = pService->createCharacteristic(
                             "beb5483e-36e1-4688-b7f5-ea07361b26a8",
                             NIMBLE_PROPERTY::READ |
                             NIMBLE_PROPERTY::WRITE
                           );
  pBleNameCharacteristic->setValue(bleDeviceName.c_str());

  pPhoneNameCharacteristic = pService->createCharacteristic(
                               "cba5483e-36e1-4688-b7f5-ea07361b26a9",
                               NIMBLE_PROPERTY::READ |
                               NIMBLE_PROPERTY::WRITE
                             );
  pPhoneNameCharacteristic->setCallbacks(new PhoneNameCallbacks());
  pPhoneNameCharacteristic->setValue("None");

  pMirrorControlCharacteristic = pService->createCharacteristic(
                                   "cba5483e-36e1-4688-b7f5-ea07361b26aa",
                                   NIMBLE_PROPERTY::WRITE
                                 );
  pMirrorControlCharacteristic->setCallbacks(new MirrorControlCallbacks());

  pDisplayCharacteristic = pService->createCharacteristic(
                             "cba5483e-36e1-4688-b7f5-ea07361b26ab",
                             NIMBLE_PROPERTY::READ |
                             NIMBLE_PROPERTY::NOTIFY
                           );

  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");

  // Enable scan response so the full name fits without overflowing the 31-byte
  // primary advertisement payload (flags + UUID + short name must stay <= 31B).
  pAdvertising->enableScanResponse(true);

  // Primary advert: flags only + a short name (<=8 chars) to stay well within
  // the 31-byte limit. Overfilling this causes malformed packets on many phones
  // which immediately triggers a disconnect right after connection.
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  String shortName = bleDeviceName.substring(0, min((int)bleDeviceName.length(), 8));
  advData.setShortName(shortName.c_str());
  pAdvertising->setAdvertisementData(advData);

  // Scan response: full device name (up to 29 chars fits here safely)
  NimBLEAdvertisementData scanData;
  scanData.setName(bleDeviceName.c_str());
  pAdvertising->setScanResponseData(scanData);

  // Start advertising
  pAdvertising->start();
}

void saveBleSettings() {
  prefs.begin("ble_settings", false);
  prefs.putString("name", bleDeviceName);
  prefs.putBool("access", bleSettingAccess);
  prefs.end();
}

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
  prefs.begin("powermode", false);
  prefs.putString("lastSSID", lastConnectedSSID);
  prefs.end();

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
  for (int i = 0; i < savedNetCount; i++) {
    if (savedSSIDs[i] == ssid) { found = i; break; }
  }
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
  for (int i = 0; i < savedNetCount; i++) {
    if (savedSSIDs[i] == ssid) return savedPWDs[i];
  }
  return "";
}

void syncTimeWithNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

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
    if (!displayOn || aiThinking) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    
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
    
    bool prev = internetOK;
    internetOK = pingerStatus[0] || pingerStatus[1] || pingerStatus[2] || pingerStatus[3] || pingerStatus[4];
    if (prev != internetOK) statusChanged = true;
    
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startWifiPingers() {
  xTaskCreate(connectivityTask, "NetPing", 4096, NULL, 1, NULL);
}

void drawRadioHome(int yOffset) {
  drawHeader(yOffset, "RADIO", (radioCursor == -1));
  const char* opts[] = {"WiFi", "Bluetooth", "ESP-NOW", "Power Mode"};
  for (int i = 0; i < 4; i++) {
    int y = 16 + (i * 12) + yOffset;
    bool sel = (radioCursor == i);
    if (sel) {
      display.fillRect(0, y - 1, 128, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    display.print(opts[i]);
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawRadioWifi(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiConfirmCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "WiFi is OFF", 22 + yOffset, 1);
  drawCenteredText(display, "Turn it on?", 33 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiConfirmCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiConfirmCursor == 1));
}

void drawWifiConfirm(int yOffset) {
  drawRadioWifi(yOffset);
}

void drawWifiScanning(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "SCANNING...", 30 + yOffset, 1);

  int dots = ((millis() - wifiScanStart) / 500) % 4;
  String dotStr = "";
  for (int i = 0; i < dots; i++) dotStr += ".";
  drawCenteredText(display, dotStr, 42 + yOffset, 1);
}

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
    if (selected) {
      display.fillRect(0, y - 1, 122, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
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
    int sbH = max(4, (visibleItems * trackH) / wifiScanResults);
    int sbY = 16 + ((wifiListScroll * (trackH - sbH)) / (wifiScanResults - visibleItems));
    display.drawFastVLine(126, 16 + yOffset, trackH, SSD1306_WHITE);
    display.fillRect(125, sbY + yOffset, 3, sbH, SSD1306_WHITE);
  }
}

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

void drawWifiKeyboard() {

  display.setTextColor(SSD1306_WHITE);
  display.drawRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setCursor(4, 3);

  if (kbTextCursor < kbScrollOffset) kbScrollOffset = kbTextCursor;
  if (kbTextCursor > kbScrollOffset + 17) kbScrollOffset = kbTextCursor - 17;

  String disp = kbInputBuffer.substring(kbScrollOffset);
  if (disp.length() > 18) disp = disp.substring(0, 18);

  if (kbInputBuffer.length() == 0) {
    display.setTextColor(0x5555); display.print("Type password...");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.print(disp);
  }

  if ((millis() / 500) % 2 == 0) {
    int cursorCharPos = kbTextCursor - kbScrollOffset;
    int cursorPx = 4 + (cursorCharPos * 6);
    display.drawFastVLine(cursorPx, 2, 9, SSD1306_WHITE);
  }

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 11; c++) {
      const char* key;
      if (currentKBMode == KB_UPPER) key = kbUpper[r][c];
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

void drawRadioBleHome(int yOffset) {
  drawHeader(yOffset, "Bluetooth", (bleHomeCursor == -1));
  display.setTextColor(SSD1306_WHITE);

  bool isConnected = bleConnected;
  if (pBleServer && pBleServer->getConnectedCount() > 0) isConnected = true;

  if (!isConnected) {
    
    // Center text layout
    drawCenteredText(display, "READY TO PAIR", 26 + yOffset, 1);
    
    String dn = bleDeviceName;
    if (dn.length() > 18) dn = dn.substring(0, 17) + "~";
    drawCenteredText(display, dn, 38 + yOffset, 1);

    if (bleHomeCursor == 0) {
      display.fillRect(46, 64 - 11 + yOffset, 36, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.drawRect(46, 64 - 11 + yOffset, 36, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_WHITE);
    }
    drawCenteredText(display, "BACK", 64 - 10 + yOffset, 1);
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(4, 15 + yOffset);
    display.print("Name: ");
    String dn = bleDeviceName;
    if (dn.length() > 14) dn = dn.substring(0, 13) + "..";
    display.print(dn);

    display.setCursor(4, 25 + yOffset);
    display.print("Device: ");
    String remote = (bleRemoteDeviceName != "None") ? bleRemoteDeviceName : bleRemoteAddress;
    if (remote.length() > 12) remote = remote.substring(0, 11) + "..";
    display.print(remote);

    // Options menu
    int optY = 38 + yOffset;
    const char* opts[] = {"Rename Watch", "Toggle Access"};
    for (int i = 0; i < 2; i++) {
        int y = optY + (i * 12);
        bool sel = (bleHomeCursor == i);
        if (sel) {
            display.fillRect(0, y-2, 128, 11, SSD1306_WHITE);
            display.setTextColor(SSD1306_BLACK);
        } else {
            display.setTextColor(SSD1306_WHITE);
        }
        display.setCursor(4, y);
        display.print(opts[i]);
    }
    display.setTextColor(SSD1306_WHITE);
  }
}

void drawBleConfirm(int yOffset) {
  drawHeader(yOffset, "Bluetooth", (bleConfirmCursor == -1));
  display.setTextColor(SSD1306_WHITE);

  // Decorative icon for confirmation
  int iconX = 58;
  int iconY = 16 + yOffset;
  display.drawFastVLine(iconX + 3, iconY, 5, SSD1306_WHITE);
  display.drawLine(iconX + 1, iconY + 1, iconX + 5, iconY + 4, SSD1306_WHITE);
  display.drawLine(iconX + 1, iconY + 4, iconX + 5, iconY + 1, SSD1306_WHITE);
  display.drawLine(iconX + 3, iconY, iconX + 5, iconY + 1, SSD1306_WHITE);
  display.drawLine(iconX + 3, iconY + 5, iconX + 5, iconY + 4, SSD1306_WHITE);

  const char* msg = "";
  if (bleConfirmMode == 0) msg = "Change Name?";
  else if (bleConfirmMode == 1) msg = "Disconnect?";
  else if (bleConfirmMode == 2) msg = "Forget Device?";
  else if (bleConfirmMode == 3) msg = bleSettingAccess ? "Turn OFF access?" : "Turn ON access?";

  drawCenteredText(display, msg, 28 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 10, 48 + yOffset, 46, 13, (bleConfirmCursor == 0));
  drawBoxedCenteredText(display, "NO",  72, 48 + yOffset, 46, 13, (bleConfirmCursor == 1));
}

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

  const char* opts[] = {"WiFi: ON", "Sync Time", "Forget Network"};
  for (int i = 0; i < 3; i++) {
    int y = 36 + (i * 10) + yOffset;
    bool sel = (wifiHomeCursor == i);
    if (sel) {
      display.fillRect(0, y - 1, 128, 10, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    display.print(opts[i]);
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawWifiToggleConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Turn off WiFi?", 22 + yOffset, 1);
  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiToggleCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiToggleCursor == 1));
}

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
    drawCenteredText(display, "NTP Sync Success!", 30 + yOffset, 1);
    char tBuf[16];
    DateTime now = rtc.now();
    sprintf(tBuf, "%02d:%02d", now.hour(), now.minute());
    drawCenteredText(display, tBuf, 42 + yOffset, 1);
  } else if (ntpSyncState == 2) {
    drawCenteredText(display, "NTP Sync Failed", 30 + yOffset, 1);
    drawCenteredText(display, "Timeout", 42 + yOffset, 1);
  }
}

void drawRadioStub(int yOffset, const char* name) {
  drawHeader(yOffset, name, true);
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, name, 28 + yOffset, 1);
  drawCenteredText(display, "Coming Soon", 40 + yOffset, 1);
}

void drawPowerHome(int yOffset) {
  drawHeader(yOffset, "Power Mode", (powerHomeCursor == -1));
  display.setTextColor(SSD1306_WHITE);

  const char* opts[] = {"WiFi Auto On:", "ESP-NOW Auto:", "Eco Mode"};
  for (int i = 0; i < 3; i++) {
    int y = 16 + (i * 12) + yOffset;
    if (powerHomeCursor == i) {
      display.fillRect(0, y - 1, 128, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    display.print(opts[i]);

    if (i == 0) {
      display.setCursor(90, y);
      display.print(wifiAutoOn ? "YES" : "NO");
    } else if (i == 1) {
      display.setCursor(90, y);
      display.print(espNowAutoOn ? "YES" : "NO");
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawWifiAutoConfirm(int yOffset) {
  drawHeader(yOffset, "Power Mode", (wifiConfirmCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Turn WiFi", 18 + yOffset, 1);
  drawCenteredText(display, "Auto On?", 29 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiConfirmCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiConfirmCursor == 1));
}

void drawEspNowAutoConfirm(int yOffset) {
  drawHeader(yOffset, "Power Mode", (wifiConfirmCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Turn ESP-NOW", 18 + yOffset, 1);
  drawCenteredText(display, "Auto On?", 29 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiConfirmCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiConfirmCursor == 1));
}

void drawPowerModeSelection(int yOffset) {
  drawHeader(yOffset, "Power Mode");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Enable Eco", 22 + yOffset, 1);
  drawCenteredText(display, "mode?", 33 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 5, 48 + yOffset, 33, 13, (powerModeButtonCursor == 0));
  drawBoxedCenteredText(display, "HELP", 47, 48 + yOffset, 33, 13, (powerModeButtonCursor == 1));
  drawBoxedCenteredText(display, "NO", 89, 48 + yOffset, 33, 13, (powerModeButtonCursor == 2));
}

void drawPowerModeHelp(int yOffset) {
  drawHeader(yOffset, "Power Mode", false);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(4, 16 + yOffset);
  display.setTextSize(1);
  display.println("Eco Mode: Ultra low");
  display.println("power, 31-day battery");
  display.println("5s sleep cycles with");
  display.println("time-only display");
  display.println("");
  display.println("Normal: Full features");

  display.setTextColor(SSD1306_WHITE);
}

void drawEcoExitConfirm(int yOffset) {
  drawHeader(yOffset, "Eco Mode", (ecoExitConfirmCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Exit Eco Mode?", 22 + yOffset, 1);

  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (ecoExitConfirmCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (ecoExitConfirmCursor == 1));
}

void wifiBackgroundTask(void *pvParameters) {
  for (;;) {

    vTaskDelay(pdMS_TO_TICKS(10000));

    if (wifiAutoOn && !ecoModeActive) {

      if (WiFi.status() == WL_CONNECTED || wifiConnecting || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        continue;
      }

      if (currentScreen == SCREEN_WIFI_SCANNING || currentScreen == SCREEN_WIFI_RESULTS ||
          currentScreen == SCREEN_WIFI_PASSWORD || currentScreen == SCREEN_WIFI_KEYBOARD ||
          currentScreen == SCREEN_RADIO_WIFI || currentScreen == SCREEN_AI_CHAT ||
          currentScreen == SCREEN_AI_THINKING) {
        continue;
      }

      if (WiFi.getMode() != WIFI_STA) {
        WiFi.mode(WIFI_STA);
        vTaskDelay(pdMS_TO_TICKS(100));
      }

      WiFi.scanNetworks(true); // Run scan async

      unsigned long scanWaitStart = millis();
      while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - scanWaitStart < 10000) {
          vTaskDelay(pdMS_TO_TICKS(100)); // Poll without blocking the radio completely
      }

      int n = WiFi.scanComplete();

      // Ensure BLE advertising is alive after scan operations
      if (pBleServer && pBleServer->getConnectedCount() == 0) {
        NimBLEDevice::getAdvertising()->stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        NimBLEDevice::getAdvertising()->start();
      }

      if (n > 0) {
        int bestIdx = -1;
        bool lastFound = false;

        for (int i = 0; i < n; i++) {
          String ssid = WiFi.SSID(i);
          String pwd = findSavedPwd(ssid);

          if (pwd.length() > 0) {

            if (lastConnectedSSID.length() > 0 && ssid == lastConnectedSSID) {
              bestIdx = i;
              lastFound = true;
              break;
            }

            if (bestIdx == -1) {
              bestIdx = i;
            }
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
            prefs.begin("powermode", false);
            prefs.putString("lastSSID", lastConnectedSSID);
            prefs.end();
            syncTimeWithNTP();

            // Restart BLE advertising after WiFi connects
            if (pBleServer && pBleServer->getConnectedCount() == 0) {
              NimBLEDevice::getAdvertising()->stop();
              vTaskDelay(pdMS_TO_TICKS(50));
              NimBLEDevice::getAdvertising()->start();
            }
          }
        }
      }
      if (currentScreen != SCREEN_WIFI_RESULTS) WiFi.scanDelete();
    }
  }
}

int aiCountMessages() {
  File f = LittleFS.open("/ai_chat.txt", "r");
  if (!f) return 0;
  int count = 0;
  while (f.available()) {
    f.readStringUntil('\n');
    count++;
  }
  f.close();
  return count;
}

void aiAddMessage(const String& msg) {
  File f = LittleFS.open("/ai_chat.txt", "a");
  if (f) {
    f.println(msg);
    f.close();
    aiMsgCount++;
  }
}

void aiClearChat() {
  LittleFS.remove("/ai_chat.txt");
  aiMsgCount = 0;
  aiChatScroll = 0;
}

String aiGetMessage(int index) {
  File f = LittleFS.open("/ai_chat.txt", "r");
  if (!f) return "";
  int cur = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (cur == index) { f.close(); return line; }
    cur++;
  }
  f.close();
  return "";
}

void drawAiChat(int yOffset) {

  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextSize(1);

  if (aiCursor == -2) {
    display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(2, 2 + yOffset);
  display.print("<--");

  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "AI Chatbot", 2 + yOffset, 1);

  if (aiCursor == -1) {
    display.fillRect(116, yOffset, 12, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(119, 2 + yOffset);
  display.print("C");
  display.setTextColor(SSD1306_WHITE);

  int chatY = 13 + yOffset;
  int chatH = 38;
  int lineH = 9;
  int maxVisibleLines = chatH / lineH;

  if (aiCursor == 0 && !aiScrollMode) {
    display.drawRect(0, chatY, 128, chatH, SSD1306_WHITE);
  }
  if (aiScrollMode) {

    display.fillRect(124, chatY, 4, chatH, SSD1306_BLACK);
    if (aiMsgCount > 0) {
      int sbH = max(4, (maxVisibleLines * chatH) / max(1, aiMsgCount));
      int sbY = chatY + ((aiChatScroll * (chatH - sbH)) / max(1, aiMsgCount - 1));
      display.fillRect(125, sbY, 3, sbH, SSD1306_WHITE);
    }
  }

  if (aiMsgCount >= AI_MAX_MESSAGES) {
    drawCenteredText(display, "Chat full!", chatY + 8, 1);
    drawCenteredText(display, "Press C to clear", chatY + 20, 1);
  } else if (aiMsgCount == 0) {
    drawCenteredText(display, "No messages yet", chatY + 14, 1);
  } else {

    int drawY = chatY + 1;
    display.setTextWrap(false);
    for (int i = 0; i < 5 && (aiChatScroll + i) < aiMsgCount && drawY < chatY + chatH - 2; i++) {
      String msg = aiCache[i];
      if (msg.length() == 0) continue;

      bool isUser = msg.startsWith("U:");
      String prefix = isUser ? "Me:" : "AI:";
      String content = msg.substring(2);
      String fullText = prefix + content;

      int pos = 0;
      int maxChars = 21;
      bool firstLine = true;
      while (pos < (int)fullText.length() && drawY < chatY + chatH - 2) {
        int remaining = fullText.length() - pos;
        int lineLen = min(remaining, maxChars);

        if (remaining > maxChars) {
          int lastSpace = -1;
          for (int j = pos; j < pos + lineLen; j++) {
            if (fullText[j] == ' ') lastSpace = j;
          }
          if (lastSpace > pos) lineLen = lastSpace - pos + 1;
        }

        display.setCursor(2, drawY);
        display.print(fullText.substring(pos, pos + lineLen));
        pos += lineLen;
        drawY += lineH;
      }
      drawY += 1;
    }
    display.setTextWrap(true);
  }

  int barY = 52 + yOffset;

  if (aiCursor == 1) {
    display.fillRect(0, barY, 99, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.drawRect(0, barY, 99, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(3, barY + 2);
  if (aiInputText.length() == 0) {
    display.print("Type message...");
  } else {
    String disp = aiInputText;
    if (disp.length() > 15) disp = disp.substring(disp.length() - 15);
    display.print(disp);
  }

  display.setTextColor(SSD1306_WHITE);
  drawBoxedCenteredText(display, "SEND", 101, barY, 27, 12, (aiCursor == 2));
}

void drawAiThinking(int yOffset) {

  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "AI Thinking", 26 + yOffset, 1);

  int dots = ((millis() - aiThinkStart) / 400) % 4;
  String dotStr = "";
  for (int i = 0; i < dots; i++) dotStr += ".";
  drawCenteredText(display, dotStr, 38 + yOffset, 1);
}

void drawAiWifiError(int yOffset) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextSize(1);

  if (aiWifiErrorCursor == 0) {
    display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(2, 2 + yOffset);
  display.print("<--");

  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "WiFi", 2 + yOffset, 1);

  const char* msg = "";
  if (aiWifiErrorType == 0) msg = "WiFi Turned Off";
  else if (aiWifiErrorType == 1) msg = "WiFi not connected";
  else if (aiWifiErrorType == 2) msg = "No Internet";

  drawCenteredText(display, msg, 30 + yOffset, 1);

  drawBoxedCenteredText(display, "OK", 48, 48 + yOffset, 32, 13, (aiWifiErrorCursor == 1));
}

void aiSendTask(void* param) {

  int contextCount = min(6, aiMsgCount);
  int startIdx = aiMsgCount - contextCount;

  String payload;
  payload.reserve(2048); // Pre-allocate memory to reduce fragmentation
  payload = "{\"model\":\"llama-3.3-70b-versatile\",\"messages\":[{\"role\":\"system\",\"content\":\"You are a helpful AI assistant on a smartwatch. Keep responses very short (under 100 chars) since the display is tiny (128x64 pixels). Be concise and direct.\"}";

  for (int i = startIdx; i < aiMsgCount; i++) {
    String msg = aiGetMessage(i);
    if (msg.length() < 3) continue;
    String role = msg.startsWith("U:") ? "user" : "assistant";
    String content = msg.substring(2);
    content.replace("\\", "\\\\");
    content.replace("\"", "\\\"");
    content.replace("\n", " ");
    payload += ",{\"role\":\"" + role + "\",\"content\":\"" + content + "\"}";
  }
  payload += "],\"max_tokens\":150}";

  vTaskDelay(pdMS_TO_TICKS(500));

  WiFiClientSecure *client = new WiFiClientSecure();
  if (!client) {
    aiResponse = "Memory Error";
    aiResponseReady = true;
    vTaskDelete(NULL);
    return;
  }
  client->setInsecure();
  client->setHandshakeTimeout(10000); // 10s handshake timeout

  HTTPClient *http = new HTTPClient();
  if (!http) {
    delete client;
    aiResponse = "Memory Error";
    aiResponseReady = true;
    vTaskDelete(NULL);
    return;
  }
  
  http->setConnectTimeout(10000);
  http->setTimeout(15000);
  http->setReuse(false); // Ensure a fresh connection to avoid socket issues
  http->begin(*client, "https://api.groq.com/openai/v1/chat/completions");
  http->addHeader("Content-Type", "application/json");
  http->addHeader("User-Agent", "ESP32-Smartwatch/1.0");
  http->addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);

  int code = http->POST(payload);
  if (code == 200) {
    String response = http->getString();

    int choicesIdx = response.indexOf("\"choices\"");
    if (choicesIdx != -1) {
      int contentIdx = response.indexOf("\"content\"", choicesIdx);
      if (contentIdx != -1) {
        int colonIdx = response.indexOf(":", contentIdx + 9);
        if (colonIdx != -1) {
          int qStart = response.indexOf("\"", colonIdx + 1);
          if (qStart != -1) {
            qStart++;
            int qEnd = qStart;
            while (qEnd < (int)response.length()) {
              if (response[qEnd] == '\\') { qEnd += 2; continue; }
              if (response[qEnd] == '"') break;
              qEnd++;
            }
            aiResponse = response.substring(qStart, qEnd);
            aiResponse.replace("\\n", " ");
            aiResponse.replace("\\\"", "\"");
            aiResponse.replace("\\\\", "\\");
            aiResponseReady = true;
          }
        }
      }
    }
    if (!aiResponseReady) aiResponseError = true;
  } else {
    aiResponse = "HTTP " + String(code);
    aiResponseReady = true;
  }
  http->end();
  delete http;
  delete client;
  vTaskDelete(NULL);
}

void aiUpdateCache() {

  for (int i = 0; i < 5; i++) aiCache[i] = "";

  if (aiMsgCount == 0) return;

  File f = LittleFS.open("/ai_chat.txt", "r");
  if (!f) return;

  int curIdx = 0;
  int cacheIdx = 0;
  while (f.available() && cacheIdx < 5) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (curIdx >= aiChatScroll) {
      aiCache[cacheIdx++] = line;
    }
    curIdx++;
  }
  f.close();
  aiCacheScrollUsed = aiChatScroll;
}

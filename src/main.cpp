// Maroubra Beach Dashboard

// base class GxEPD2_GFX can be used to pass references or pointers to the
// display instance as parameter, uses ~1.2k more code enable or disable
// GxEPD2_GFX base class.
// This must be defined before including GxEPD2_BW.h
#define ENABLE_GxEPD2_GFX 1

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_GFX.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_pm.h>
#include <esp_wifi.h>
#include <lwip/apps/sntp.h>
#include <sdkconfig.h>
#include <time.h>

#include "app.h"
#include "bus.h"
#include "client_utils.h"
#include "config.h"
#include "icons.h"
#include "renderer.h"
#include "secrets.h"

// copy the constructor from GxEPD2display_selection.h of GxEPD_Example to here
// and adapt it to the ESP32 Driver wiring, e.g.
// This is for GDEY075T7
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> display(
    GxEPD2_750_T7(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST,
                  PIN_EPD_BUSY));  // GDEW075T7 800x480, EK79655 (GD7965)

#if defined(DRIVER_WAVESHARE)
SPIClass hspi(HSPI);
#endif

Renderer renderer(display);
Bus bus(display, renderer);
IApp* app = &bus;

void initDisplay();
void sleep(bool forceDeepSleep = false);
void powerOffDisplay();

void handleFatalError(const uint8_t* bitmap_196x196, const String& errMsgLn1,
                      const String& errMsgLn2 = "");

bool displayInitialized = false;
uint32_t lastTimeSync = 0;
int partialRefreshCount = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("setup");

  // Not sure this does anything but copied from
  // https://github.com/espressif/esp-idf/tree/master/examples/wifi/power_save
  esp_pm_config_esp32_t pm_config = {
      .max_freq_mhz = CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = 80,
      .light_sleep_enable = true};
  esp_pm_configure(&pm_config);

  // WIFI
  wl_status_t wifiStatus = startWiFi();
  if (wifiStatus != WL_CONNECTED) {  // WiFi Connection Failed
    handleFatalError(epd_bitmap_wifi_off, wifiStatus == WL_NO_SSID_AVAIL
                                              ? "Network Not Available"
                                              : "Wifi Connection Failed");
    return;
  }

  // TIME SYNCHRONIZATION
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
  bool timeConfigured = waitForSNTPSync();
  if (!timeConfigured) {
    handleFatalError(epd_bitmap_wifi_off, "Time Synchronization Failed");
    return;
  }
  lastTimeSync = millis();

  Serial.println("setup done");
}

void loop() {
  uint32_t start = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Reconnecting to WiFi");
    WiFi.reconnect();
  }

  if (millis() - lastTimeSync > 60 * 60 * 1000) {
    Serial.println("Re-synchronizing time");
    waitForSNTPSync();
    lastTimeSync = millis();
  }

  // Fetch
  bool fetchSuccess = app->fetchData();

  uint32_t fetchComplete = millis();
  Serial.printf("Fetched data in %lu millis.\n", fetchComplete - start);

  // Render
  initDisplay();
  do {
    display.fillScreen(GxEPD_WHITE);
    // display.drawRect(X_MARGIN, Y_MARGIN, display.width() - X_MARGIN * 2,
    //                  display.height() - Y_MARGIN * 2, GxEPD_BLACK);
    if (fetchSuccess) {
      app->render();
    } else {
      renderer.drawError(epd_bitmap_mdi__error, "Data Fetch Failed");
    }
  } while (display.nextPage());

  uint32_t renderComplete = millis();
  Serial.printf("Rendered data in %lu millis. Total time taken: %lu millis.\n",
                renderComplete - fetchComplete, renderComplete - start);

  sleep();
}

/* Initialize e-paper display */
void initDisplay() {
  if (!displayInitialized) {
#ifdef DRIVER_WAVESHARE
    display.init(115200, true, 2, false);
    // remap spi for waveshare
    SPI.end();
    SPI.begin(PIN_EPD_SCK, PIN_EPD_MISO, PIN_EPD_MOSI, PIN_EPD_CS);
#endif
#ifdef DRIVER_DESPI_C02
    pinMode(PIN_EPD_PWR, OUTPUT);
    digitalWrite(PIN_EPD_PWR, HIGH);
    display.init(115200, true, 10, false);
#endif
    displayInitialized = true;
  }

  display.setRotation(3);
  display.setTextSize(1);
  display.setTextColor(GxEPD_BLACK);
  display.setTextWrap(false);
  if (partialRefreshCount == 0 || partialRefreshCount > 10) {
    Serial.println("Full refresh");
    display.setFullWindow();
    partialRefreshCount = 1;
  } else {
    Serial.println("Partial refresh " + String(partialRefreshCount));
    display.setPartialWindow(0, 0, display.width(), display.height());
    partialRefreshCount++;
  }
  display.firstPage();
  return;
}  // end initDisplay

void handleFatalError(const uint8_t* bitmap_196x196, const String& errMsgLn1,
                      const String& errMsgLn2) {
  Serial.println(errMsgLn1);
  initDisplay();
  do {
    renderer.drawError(bitmap_196x196, errMsgLn1, errMsgLn2);
  } while (display.nextPage());
  sleep(true);
}

void sleep(bool forceDeepSleep) {
  uint64_t sleepDuration = calculateSleepDuration();
  if (forceDeepSleep || sleepDuration > DEEP_SLEEP_THRESHOLD) {
    powerOffDisplay();
    Serial.println("Entering deep sleep for " + String(sleepDuration) + "s");
    esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
    esp_deep_sleep_start();
  } else {
    Serial.println("Entering delay for " + String(sleepDuration) + "s");
    // Light sleep will save a lot of power but seems to kill the wifi
    // connection permanently.
    // https://www.reddit.com/r/esp32/comments/rncows/light_sleep_and_wifi/
    // esp_wifi_stop();
    // esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
    // esp_light_sleep_start();
    delay(sleepDuration * 1000);
  }
}

/* Power-off e-paper display */
void powerOffDisplay() {
  display.hibernate();  // turns powerOff() and sets controller to deep sleep
                        // for minimum power use
  digitalWrite(PIN_EPD_PWR, LOW);
  return;
}  // end powerOffDisplay

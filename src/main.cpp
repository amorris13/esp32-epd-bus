// Sydney Busses Departure Board.

// Use HSPI for EPD (and VSPI for SD) with Waveshare ESP32 Driver Board
#define USE_HSPI_FOR_EPD

// base class GxEPD2_GFX can be used to pass references or pointers to the
// display instance as parameter, uses ~1.2k more code enable or disable
// GxEPD2_GFX base class
#define ENABLE_GxEPD2_GFX 0

// uncomment next line to use class GFX of library GFX_Root instead of
// Adafruit_GFX #include <GFX.h> Note: if you use this with ENABLE_GxEPD2_GFX 1:
//       uncomment it in GxEPD2_GFX.h too, or add #include <GFX.h> before any
//       #include <GxEPD2_GFX.h>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <GxEPD2_BW.h>
#include <HTTPClient.h>
#include <StreamUtils.h>
#include <WiFi.h>
#include <lwip/apps/sntp.h>
#include <time.h>

#include "icons.h"
#include "secrets.h"

// copy the constructor from GxEPD2_display_selection.h of GxEPD_Example to here
// and adapt it to the ESP32 Driver wiring, e.g.
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 2> display(
    GxEPD2_750_T7(/*CS=*/15, /*DC=D3*/ 27, /*RST=*/26,
                  /*BUSY=*/25));  // GDEW075T7 800x480, EK79655 (GD7965)

#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
SPIClass hspi(HSPI);
#endif

struct stopDescription {
  const char *name;
  std::vector<int> iconIds;
};

void connectWifi();
bool fetchData();
bool fetchForStopId(const char *stopId, JsonDocument &stopDoc);
void render();
void showBusStopDepartures(int16_t l, int16_t t, int16_t r, int16_t b);
int16_t showDeparturesForStop(JsonDocument &stopDoc, int16_t l, int16_t t,
                              int16_t r, int16_t b);
std::vector<JsonObject> getSortedStopEvents(JsonArray stopEventsJsonArray);
stopDescription getStopDescription(JsonDocument &stopDoc);
const unsigned char *getBitmapForIconId(int16_t iconId);
int16_t drawStopEvent(const JsonObject &stopEvent, int16_t l, int16_t t,
                      int16_t r, int16_t b);
time_t getDepartureTime(const JsonObject &stopEvent);
time_t parseTimeUtc(const char *utcTimeString);

int partialRefreshCount = 0;

// const char *stopIds[] = {"200060"};  // Central
// const char *stopIds[] = {"200020"};  // Circular Quay
// const char *stopIds[] = {"219610"};  // Punchbowl station
// const char *stopIds[] = {"203294"};  // Juniors Kingsford
// const char *stopIds[] = {"212110"};  // Epping
// const char *stopIds[] = {"2196291", "2196292"};  // Punchbowl platforms
const char *stopIds[] = {"2035144", "2035159"};  // Maroubra

std::vector<JsonDocument> stopDocs;

void setup() {
  Serial.begin(115200);
  Serial.println("setup");
  connectWifi();

  // *** special handling for Waveshare ESP32 Driver board *** //
  // ********************************************************* //
#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
  hspi.begin(13, 12, 14, 15);  // remap hspi for EPD (swap pins)
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#endif
  // *** end of special handling for Waveshare ESP32 Driver board *** //
  // **************************************************************** //

  display.init(115200);
  Serial.println("setup done");
}

void loop() {
  // Fetch
  fetchData();

  // Render
  render();

  // Delay until next run
  delay(30 * 1000);
}

void connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println("Establishing connection to WiFi..");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
  }

  Serial.printf("Connected to network %s\n", WIFI_SSID);

  // Sync SNTP
  sntp_setoperatingmode(SNTP_OPMODE_POLL);

  char server[] =
      "time.nist.gov";  // sntp_setservername takes a non-const char*, so use
                        // a non-const variable to avoid warning
  sntp_setservername(0, server);
  sntp_init();

  Serial.println("Waiting for NTP time sync...");
  Serial.printf("Syncing NTP time via %s...\n", server);
  time_t now;
  while (time(&now), now < 1625099485) {
    delay(1000);
  }

  setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
  tzset();
  char buf[256];
  strftime(buf, sizeof(buf), "Got time: %Y-%m-%d %H:%M:%S\n", localtime(&now));
  Serial.printf(buf);
}

bool fetchData() {
  stopDocs.clear();
  for (const char *stopId : stopIds) {
    JsonDocument stopDoc;
    if (!fetchForStopId(stopId, stopDoc)) {
      return false;
    }
    stopDocs.push_back(stopDoc);
  }
  return true;
}

bool fetchForStopId(const char *stopId, JsonDocument &stopDoc) {
  uint32_t start = millis();
  WiFiClientSecure client;
  HTTPClient http;

  // Construct the http request
  http.useHTTP10(true);
  http.setAuthorizationType("apiKey");
  http.setAuthorization(TFNSW_API_KEY);
  char url[256];
  snprintf(
      url, sizeof(url),
      "https://api.transport.nsw.gov.au/v1/tp/"
      "departure_mon?outputFormat=rapidJSON&coordOutputFormat=EPSG%%3A4326&"
      "mode="
      "direct&type_dm=stop&name_dm=%s"
      "&departureMonitorMacro=true&excludedMeans=11&TfNSWDM="
      "true&version=10.2.1.42",
      stopId);
  http.begin(url, (const char *)NULL);

  // Send the request as a GET
  int http_code = http.GET();

  Serial.printf("Finished request in %lu millis.\n", millis() - start);
  if (http_code > 0) {
    Serial.printf("Response code: %d Data length: %d\n", http_code,
                  http.getSize());

    // The filter: it contains "true" for each value we want to keep
    JsonDocument filter;
    filter["locations"][0]["disassembledName"] = true;
    filter["locations"][0]["assignedStops"][0]["modes"] = true;
    filter["stopEvents"][0]["departureTimePlanned"] = true;
    filter["stopEvents"][0]["departureTimeEstimated"] = true;
    filter["stopEvents"][0]["isRealtimeControlled"] = true;
    filter["stopEvents"][0]["isCancelled"] = true;
    filter["stopEvents"][0]["location"]["parent"]["disassembledName"] = true;
    filter["stopEvents"][0]["transportation"]["disassembledName"] = true;
    filter["stopEvents"][0]["transportation"]["product"]["iconId"] = true;
    filter["stopEvents"][0]["transportation"]["origin"]["name"] = true;
    filter["stopEvents"][0]["transportation"]["destination"]["name"] = true;

    DeserializationError err = deserializeJson(
        stopDoc, http.getStream(), DeserializationOption::Filter(filter));

    if (err) {
      Serial.printf("Error parsing response! %s\n", err.c_str());
      http.end();
      return false;
    }

    Serial.println("Parsing successful");
    http.end();

    // For some reason, this is necessary otherwise we get an IncompleteInput
    // error. This must affect compilation in some way that makes the code
    // work.
    serializeJsonPretty(stopDoc, Serial);

    return true;
  } else {
    Serial.printf("Error on HTTP request (%d): %s\n", http_code,
                  http.errorToString(http_code).c_str());
    http.end();
    return false;
  }
}

void render() {
  if (partialRefreshCount > 10) {
    display.setFullWindow();
    partialRefreshCount = 0;
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
    partialRefreshCount++;
  }
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
  display.firstPage();
  const int16_t xMargin = 28;
  const int16_t yMargin = 64;
  do {
    display.fillScreen(GxEPD_WHITE);

    showBusStopDepartures(xMargin, yMargin, display.width() - xMargin,
                          display.height() - yMargin);
  } while (display.nextPage());
}

void showBusStopDepartures(int16_t l, int16_t t, int16_t r, int16_t b) {
  const time_t now = time(NULL);

  // Show last updated time at the bottom
  display.setFont(&FreeSans9pt7b);
  char lastUpdatedTimeString[48];
  strftime(lastUpdatedTimeString, sizeof(lastUpdatedTimeString),
           "Last Updated: %d %b %H:%M", localtime(&now));
  int16_t lux, luy;
  uint16_t luw, luh;
  display.getTextBounds(lastUpdatedTimeString, 0, b - 4, &lux, &luy, &luw,
                        &luh);
  display.setCursor((l + r - luw) / 2, b - 4);
  display.print(lastUpdatedTimeString);

  int numStops = std::max<size_t>(1, stopDocs.size());
  int16_t heightPerStop = (luy - 4 - t) / numStops;
  int16_t y = t;
  for (JsonDocument stopDoc : stopDocs) {
    y = showDeparturesForStop(stopDoc, l, y, r, y + heightPerStop);
  }
}

int16_t showDeparturesForStop(JsonDocument &stopDoc, int16_t l, int16_t t,
                              int16_t r, int16_t b) {
  int16_t y = t;
  int xMargin = 8;

  // Bus Stop Name
  stopDescription stopDesc = getStopDescription(stopDoc);
  display.fillRoundRect(l, y, r - l, 36, 4, GxEPD_BLACK);
  int x = l + xMargin;
  for (int iconId : stopDesc.iconIds) {
    display.drawInvertedBitmap(x, y + 2, getBitmapForIconId(iconId), 32, 32,
                               GxEPD_WHITE);
    x += 32 + 4;
  }
  display.setTextColor(GxEPD_WHITE);
  display.setFont(&FreeSansBold9pt7b);
  display.setCursor(x, y + 20 + 4);
  const char *stopName = stopDesc.name;
  display.print(stopName);
  display.setTextColor(GxEPD_BLACK);
  y += 36 + 4;

  // Departures
  int16_t stopEventHeight = 0;
  std::vector<JsonObject> stopEvents =
      getSortedStopEvents(stopDoc["stopEvents"]);
  for (int i = 0; i < stopEvents.size(); i++) {
    JsonObject stopEvent = stopEvents[i];

    // Don't render if we are going to exceed the allocated height
    if (y + stopEventHeight > b - 8) {
      break;
    }

    // If not the first entry, draw a divider line
    if (stopEventHeight) {
      int16_t dividerLineMargin = xMargin + 4;
      display.drawFastHLine(l + dividerLineMargin, y,
                            (r - dividerLineMargin) - (l + dividerLineMargin),
                            GxEPD_BLACK);
      y += 4;
    }

    int16_t oldY = y;
    y = drawStopEvent(stopEvent, l + xMargin, y, r - xMargin, b);
    stopEventHeight = y - oldY;
  }
  y += 8;
  return y;
}

stopDescription getStopDescription(JsonDocument &stopDoc) {
  const char *fallbackStopName = stopDoc["locations"][0]["disassembledName"];
  std::vector<int> iconIds;
  JsonArray stopEvents = stopDoc["stopEvents"];
  if (stopEvents.size() > 0) {
    const char *stopName =
        stopEvents[0]["location"]["parent"]["disassembledName"];
    for (JsonObject stopEvent : stopEvents) {
      const char *eventStopName =
          stopEvent["location"]["parent"]["disassembledName"];
      if (!stopName || !eventStopName || strcmp(stopName, eventStopName) != 0) {
        stopName = fallbackStopName;
      }
      int eventIconId = stopEvent["transportation"]["product"]["iconId"];
      if (std::find(iconIds.begin(), iconIds.end(), eventIconId) ==
          iconIds.end()) {
        iconIds.push_back(eventIconId);
      }
    }
    std::sort(iconIds.begin(), iconIds.end());
    return {stopName, iconIds};
  } else {
    JsonArray modes = stopDoc["locations"][0]["assignedStops"][0]["modes"];
    for (int mode : modes) {
      iconIds.push_back(mode);
    }
    std::sort(iconIds.begin(), iconIds.end());
    return {fallbackStopName, iconIds};
  }
}

const unsigned char *getBitmapForIconId(int16_t iconId) {
  switch (iconId) {
    case 1:
      return epd_bitmap_trainmode;
    case 2:
      return epd_bitmap_metromode;
    case 4:
      return epd_bitmap_lightrailmode;
    case 9:
      return epd_bitmap_ferrymode;
    case 5:
    case 11:  // School bus
      return epd_bitmap_busmode;
    default:
      Serial.printf("unknown iconId: %d\n", iconId);
      return epd_bitmap_busmode;
  }
}

std::vector<JsonObject> getSortedStopEvents(JsonArray stopEventsJsonArray) {
  const time_t now = time(NULL);

  std::vector<JsonObject> filteredStopEvents;
  for (JsonObject stopEvent : stopEventsJsonArray) {
    // Ignore cancelled departures
    if (stopEvent["isCancelled"]) {
      continue;
    }

    // Only show departures within the next hour
    if (getDepartureTime(stopEvent) > now + 60 * 60) {
      continue;
    }

    filteredStopEvents.push_back(stopEvent);
  }

  std::sort(filteredStopEvents.begin(), filteredStopEvents.end(),
            [](const JsonObject &a, const JsonObject &b) {
              return getDepartureTime(a) < getDepartureTime(b);
            });
  return filteredStopEvents;
}

int16_t drawStopEvent(const JsonObject &stopEvent, int16_t l, int16_t t,
                      int16_t r, int16_t b) {
  const time_t now = time(NULL);
  int16_t y = t;

  bool isRealtime =
      stopEvent["isRealtimeControlled"] && stopEvent["departureTimeEstimated"];
  const char *busName = stopEvent["transportation"]["disassembledName"];
  const char *destination = stopEvent["transportation"]["destination"]["name"];

  time_t departureTime_t = getDepartureTime(stopEvent);

  char departureTimeHM[8];
  strftime(departureTimeHM, sizeof(departureTimeHM), "%H:%M",
           localtime(&departureTime_t));

  // Find the number of minutes until the next departure
  int nextDepartureMinutes = ((int)difftime(departureTime_t, now)) / 60;
  char nextDepartureMinutesString[8];
  snprintf(nextDepartureMinutesString, sizeof(nextDepartureMinutesString), "%d",
           nextDepartureMinutes);

  display.setTextWrap(false);
  display.setFont(&FreeSansBold24pt7b);
  y += 36;

  int16_t bnbx, bnby;
  uint16_t bnbw, bnbh;
  display.getTextBounds(busName, 0, 0, &bnbx, &bnby, &bnbw, &bnbh);
  display.setCursor(l, y);
  display.print(busName);

  int16_t ndmbx, ndmby;
  uint16_t ndmbw, ndmbh;
  display.getTextBounds(nextDepartureMinutesString, 0, 0, &ndmbx, &ndmby,
                        &ndmbw, &ndmbh);

  const char *minString = "  min";
  display.setFont(&FreeSansBold9pt7b);
  int16_t mbx, mby;
  uint16_t mbw, mbh;
  display.getTextBounds(minString, 0, 0, &mbx, &mby, &mbw, &mbh);

  display.setCursor(r - mbw, y);
  display.print(minString);
  display.setFont(&FreeSansBold24pt7b);
  display.setCursor(r - mbw - ndmbw, y);
  display.print(nextDepartureMinutesString);

  y += 8;

  display.setFont(&FreeSans9pt7b);
  y += 14;

  int16_t dbx, dby;
  uint16_t dbw, dbh;
  display.getTextBounds(destination, 0, 0, &dbx, &dby, &dbw, &dbh);
  display.setCursor(l, y);
  display.print(destination);

  int16_t rtbx, rtby;
  uint16_t rtbw, rtbh;
  display.getTextBounds(departureTimeHM, 0, 0, &rtbx, &rtby, &rtbw, &rtbh);
  display.setCursor(r - rtbw, y);
  display.print(departureTimeHM);

  if (isRealtime) {
    display.drawInvertedBitmap(r - rtbw - 8 - 16, y - 14, epd_bitmap_rssfeed,
                               16, 16, GxEPD_BLACK);
  }

  y += 8;

  return y;
}

time_t getDepartureTime(const JsonObject &stopEvent) {
  bool isRealtime =
      stopEvent["isRealtimeControlled"] && stopEvent["departureTimeEstimated"];

  const char *departureTimeString =
      stopEvent[isRealtime ? "departureTimeEstimated" : "departureTimePlanned"];
  return parseTimeUtc(departureTimeString);
}

time_t parseTimeUtc(const char *utcTimeString) {
  struct tm tm = {0};
  strptime(utcTimeString, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return mktime(&tm) - _timezone;
}
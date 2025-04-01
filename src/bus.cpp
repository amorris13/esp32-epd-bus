// Sydney Busses Departure Board.

#include "bus.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <GxEPD2_GFX.h>
#include <HTTPClient.h>
#include <StreamUtils.h>
#include <time.h>

#include "bus_icons.h"
#include "client_utils.h"
#include "renderer.h"
#include "secrets.h"

// const char *stopIds[] = {"200060"};  // Central
// const char *stopIds[] = {"200020"};  // Circular Quay
// const char *stopIds[] = {"219610"};  // Punchbowl station
// const char *stopIds[] = {"203294"};  // Juniors Kingsford
// const char *stopIds[] = {"212110"};  // Epping
// const char *stopIds[] = {"2196291", "2196292"};  // Punchbowl platforms
const char *stopIds[] = {"2035144", "2035159"};  // Maroubra

Bus::Bus(GxEPD2_GFX &_display, Renderer &renderer)
    : _display(_display), _renderer(renderer) {}

bool Bus::fetchData() {
  // WIFI
  wifiRSSI = WiFi.RSSI();  // get WiFi signal strength now, because the WiFi
                           // will be turned off to save power!

  updateTime = time(NULL);

  stopDocs.clear();
  uint64_t sleepDuration = calculateSleepDuration();
  if (calculateSleepDuration() > 5 * 60) {
    nextUpdateTime = updateTime + sleepDuration;
    Serial.println("Skipping data fetch");
  } else {
    for (const char *stopId : stopIds) {
      JsonDocument stopDoc;
      if (!fetchForStopId(stopId, stopDoc)) {
        return false;
      }
      stopDocs.push_back(stopDoc);
    }
  }

  // BATTERY
  uint32_t batVoltage = readBatteryVoltage();
  batPercent =
      calcBatPercent(batVoltage, CRIT_LOW_BATTERY_VOLTAGE, MAX_BATTERY_VOLTAGE);
  Serial.printf("Bat voltage: %d percent: %d\n", batVoltage, batPercent);

  return true;
}

bool Bus::fetchForStopId(const char *stopId, JsonDocument &stopDoc) {
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

void Bus::render() {
  showBusStopDepartures(X_MARGIN, Y_MARGIN, _display.width() - X_MARGIN,
                        _display.height() - Y_MARGIN);
}

void Bus::showBusStopDepartures(int16_t l, int16_t t, int16_t r, int16_t b) {
  const time_t now = time(NULL);

  // Show last updated time at the bottom
  int16_t statusHeight =
      _renderer.drawStatusBar(b, updateTime, wifiRSSI, batPercent);

  int numStops = stopDocs.size();
  if (stopDocs.size() == 0) {
    char nextUpdateAtString[48];
    strftime(nextUpdateAtString, sizeof(nextUpdateAtString),
             "Next Update: %H:%M", localtime(&nextUpdateTime));
    _renderer.drawError(epd_bitmap_sleep_schedule, nextUpdateAtString);
    return;
  }
  int16_t heightPerStop = (b - statusHeight - 4 - t) / numStops;
  int16_t y = t;
  for (JsonDocument stopDoc : stopDocs) {
    y = showDeparturesForStop(stopDoc, l, y, r, y + heightPerStop);
  }
}

int16_t Bus::showDeparturesForStop(JsonDocument &stopDoc, int16_t l, int16_t t,
                                   int16_t r, int16_t b) {
  int16_t y = t;
  int xMargin = 8;

  // Bus Stop Name
  stopDescription stopDesc = getStopDescription(stopDoc);
  _display.fillRoundRect(l, y, r - l, 36, 4, GxEPD_BLACK);
  int x = l + xMargin;
  for (int iconId : stopDesc.iconIds) {
    _display.drawInvertedBitmap(x, y + 2, getBitmapForIconId(iconId), 32, 32,
                                GxEPD_WHITE);
    x += 32 + 4;
  }
  _display.setTextColor(GxEPD_WHITE);
  _display.setFont(&FreeSansBold9pt7b);
  _display.setCursor(x, y + 20 + 4);
  const char *stopName = stopDesc.name;
  _display.print(stopName);
  _display.setTextColor(GxEPD_BLACK);
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
      _display.drawFastHLine(l + dividerLineMargin, y,
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

stopDescription Bus::getStopDescription(JsonDocument &stopDoc) {
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

const unsigned char *Bus::getBitmapForIconId(int16_t iconId) {
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

std::vector<JsonObject> Bus::getSortedStopEvents(
    JsonArray stopEventsJsonArray) {
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

int16_t Bus::drawStopEvent(const JsonObject &stopEvent, int16_t l, int16_t t,
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

  _display.setTextWrap(false);
  _display.setFont(&FreeSansBold24pt7b);
  y += 36;

  int16_t bnbx, bnby;
  uint16_t bnbw, bnbh;
  _display.getTextBounds(busName, 0, 0, &bnbx, &bnby, &bnbw, &bnbh);
  _display.setCursor(l, y);
  _display.print(busName);

  int16_t ndmbx, ndmby;
  uint16_t ndmbw, ndmbh;
  _display.getTextBounds(nextDepartureMinutesString, 0, 0, &ndmbx, &ndmby,
                         &ndmbw, &ndmbh);

  const char *minString = "  min";
  _display.setFont(&FreeSansBold9pt7b);
  int16_t mbx, mby;
  uint16_t mbw, mbh;
  _display.getTextBounds(minString, 0, 0, &mbx, &mby, &mbw, &mbh);

  _display.setCursor(r - mbw, y);
  _display.print(minString);
  _display.setFont(&FreeSansBold24pt7b);
  _display.setCursor(r - mbw - ndmbw, y);
  _display.print(nextDepartureMinutesString);

  y += 8;

  _display.setFont(&FreeSans9pt7b);
  y += 14;

  int16_t dbx, dby;
  uint16_t dbw, dbh;
  _display.getTextBounds(destination, 0, 0, &dbx, &dby, &dbw, &dbh);
  _display.setCursor(l, y);
  _display.print(destination);

  int16_t rtbx, rtby;
  uint16_t rtbw, rtbh;
  _display.getTextBounds(departureTimeHM, 0, 0, &rtbx, &rtby, &rtbw, &rtbh);
  _display.setCursor(r - rtbw, y);
  _display.print(departureTimeHM);

  if (isRealtime) {
    _display.drawInvertedBitmap(r - rtbw - 8 - 16, y - 14, epd_bitmap_rssfeed,
                                16, 16, GxEPD_BLACK);
  }

  y += 8;

  return y;
}

time_t Bus::getDepartureTime(const JsonObject &stopEvent) {
  bool isRealtime =
      stopEvent["isRealtimeControlled"] && stopEvent["departureTimeEstimated"];

  const char *departureTimeString =
      stopEvent[isRealtime ? "departureTimeEstimated" : "departureTimePlanned"];
  return parseTimeUtc(departureTimeString);
}

time_t Bus::parseTimeUtc(const char *utcTimeString) {
  struct tm tm = {0};
  strptime(utcTimeString, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return mktime(&tm) - _timezone;
}
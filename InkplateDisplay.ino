#include <Adafruit_MCP23017.h>
#include <Adafruit_SPITFT_Macros.h>
#include <Adafruit_SPITFT.h>
#include <Adafruit_GFX.h>
#include <gfxfont.h>
#include <Inkplate.h>

#include "driver/rtc_io.h"                          //ESP32 library used for deep sleep and RTC wake up pins
#include "WiFi.h"                   //Include library for WiFi
#include "credentials.h"
#include "PubSubClient.h"
#include <ESP32_FTPClient.h>
// patched ESP32_FTPClient.h lines 132 and 205: removed "timeout" parameter from connect call
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include "Fonts/FreeSans24pt7b.h"
#include "Fonts/FreeSans18pt7b.h"
#include "Fonts/FreeSans12pt7b.h"
#include "Fonts/FreeSans9pt7b.h"

#define MONOCHROME

#ifdef MONOCHROME
Inkplate display(INKPLATE_1BIT);    //Create an object on Inkplate library and also set library into 1 Bit mode (Monochrome)
#else
Inkplate display(INKPLATE_3BIT);    //Create an object on Inkplate library and also set library into 1 Bit mode (Monochrome)
#endif

int lineWidth = 2;
uint16_t lineColor = 255;
uint16_t buttonColor = 255;
enum fontAlign {
  LeftAlign, RightAlign, CenterAlign
};
enum smileKind {
  Smile, Neutral, Frown
};

#define uS_TO_S_FACTOR 1000000
RTC_DATA_ATTR int padCheckTime = 2;  // 1.6s till startup, so any value less than 1.6s will not work correctly
RTC_DATA_ATTR int clockUpdateInSeconds = 60;
RTC_DATA_ATTR int cyclesTillClockUpdate;
RTC_DATA_ATTR int dataUpdateInClockUpdates = 5;
RTC_DATA_ATTR int cyclesTillDataUpdate;
RTC_DATA_ATTR int cyclesTillFullUpdate;
RTC_DATA_ATTR int wlanOffTime;
RTC_DATA_ATTR int wlanOnTime;
struct Env {
  char temperature[7];
  char pressure[8];
  char humidity[6];
  char battery[5];
  char modTime[7];
};
RTC_DATA_ATTR Env indoor;
RTC_DATA_ATTR Env outdoor;
struct Power {
  char power[6];
  int assessment;
};
RTC_DATA_ATTR char powerday[16];
RTC_DATA_ATTR Power production;
RTC_DATA_ATTR Power consumption;
RTC_DATA_ATTR char station[5][64];
RTC_DATA_ATTR char motd[2][256];
RTC_DATA_ATTR unsigned long menuLevel;

WiFiClient espClient;
PubSubClient client(espClient);
const char* MQTT_BROKER = "192.168.178.47";

char ftp_server[] = "192.168.178.47";
char ftp_user[]   = "inkplate";
char ftp_pass[]   = "inkplate";

ESP32_FTPClient ftp_c (ftp_server,ftp_user,ftp_pass,       10000, 2);
ESP32_FTPClient ftp_m (ftp_server,ftp_user,ftp_pass,       10000, 2);

Adafruit_BME280 bme; // use I2C interface
Adafruit_Sensor *bme_temp = bme.getTemperatureSensor();
Adafruit_Sensor *bme_pressure = bme.getPressureSensor();
Adafruit_Sensor *bme_humidity = bme.getHumiditySensor();

void error (const char* aMessage)
{
  Serial.println (aMessage);
  display.setTextSize(3);
  display.setCursor(0, 200);
  display.print(aMessage);
  display.display();      //Put warning on display
}

void drawLine (uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t w = 1, uint16_t color = lineColor)
{
  if ((x0 != x1) && (y0 != y1)) {   // inclined line
    int dx = 0, dy = 0;
    if (abs (x1 - x0) > abs (y1 - y0)) dx = 1;
    else dy = 1;
    for (int i = 0; i < w; ++i)
      display.drawLine (x0 + i * dx, y0 + i * dy, x1 + i * dx, y1 + i * dy, color);
  } else if (x0 != x1) {   // horizontal line
    for (int i = 0; i < w; ++i)
      display.drawFastHLine (x0, y0 + i, abs(x1 - x0), color);
  } else {   // vertical line
    for (int i = 0; i < w; ++i)
      display.drawFastVLine (x0 + i, y0, abs(y1 - y0), color);
  }
}

void printAt (int aX, int aY, const GFXfont* aFont, const char* aText, const int aFactor = 1, fontAlign aAlign = LeftAlign, const int aWidth = -1)
{
  display.setFont(aFont);
  display.setTextSize(aFactor);

  int16_t  x1, y1;
  uint16_t w, h, t;
  display.getTextBounds("M", aX, aY, &x1, &y1, &w, &h);
  display.getTextBounds(aText, aX, aY, &x1, &y1, &w, &t);
  
  if (aAlign == RightAlign) display.setCursor(aX - w, aY + h);
  else if (aAlign == LeftAlign) display.setCursor(aX, aY + h);
  else display.setCursor((2 * aX + aWidth) / 2 - w / 2, aY + h);
  display.print(aText);
}

void printSmiley (int aX, int aY, int aRadius, smileKind aSmile, int aCount = 1)
{
  int distance = aRadius * 20 / 9;
  int offset = distance * (aCount - 1) / 2;
  for (int smileys = 0; smileys < aCount; ++smileys)
    printSingleSmiley (aX - offset + smileys * distance, aY, aRadius, aSmile);
}

void printSingleSmiley (int aX, int aY, int aRadius, smileKind aSmile)
{
  int thickness = aRadius / 10;

  // mouth
  if (aSmile == Smile) {
    for (int r = 0; r < thickness; ++r)
      display.drawCircle(aX, aY, aRadius * 2 / 3 - r, lineColor);
    display.fillRect(aX - aRadius, aY - aRadius, 2 * aRadius, aRadius * 6 / 5, 0);
  } else if (aSmile == Neutral) {
    drawLine(aX - aRadius * 3 / 5, aY + aRadius / 3, aX + aRadius * 3 / 5, aY + aRadius / 3, thickness);
  } else if (aSmile == Frown) {
    for (int r = 0; r < thickness; ++r)
      display.drawCircle(aX, aY + aRadius * 4 / 5, aRadius * 2 / 3 - r, lineColor);
    display.fillRect(aX - aRadius, aY + aRadius * 2 / 5, 2 * aRadius, aRadius * 6 / 5, 0);
  }

  // eyes
  display.fillCircle (aX - aRadius / 3, aY - aRadius / 5, thickness, lineColor);
  display.fillCircle (aX + aRadius / 3, aY - aRadius / 5, thickness, lineColor);

  // head
  for (int r = 0; r < thickness; ++r)
    display.drawCircle(aX, aY, aRadius - r, lineColor);
}

void updateClock()
{
  struct tm* time = getTime();
  char charBuf[32];
  strftime(charBuf, sizeof(charBuf), "%H:%M", time);
  printAt (0, 10, &FreeSans24pt7b, charBuf, 3, CenterAlign, 380);

  strftime(charBuf, sizeof(charBuf), "%d.%m.%Y", time);
  String date;
  switch (time->tm_wday) {
    case 0: date = "Sonntag, "; break;
    case 1: date = "Montag, "; break;
    case 2: date = "Dienstag, "; break;
    case 3: date = "Mittwoch, "; break;
    case 4: date = "Donnerstag, "; break;
    case 5: date = "Freitag, "; break;
    case 6: date = "Samstag, "; break;
  }
  date += charBuf;
  printAt (0, 129, &FreeSans18pt7b, date.c_str(), 1, CenterAlign, 400);
  drawLine(0, 179, 400, 179, lineWidth);
  drawLine(399, 0, 399, 180, lineWidth);
}

void buttonMarker (int aNumber)
{
//  display.fillRoundRect (100 + 133 * aNumber, 570, 70, 60, 17, buttonColor);
  display.fillRoundRect (120 + 133 * aNumber, 590, 30, 20, 17, buttonColor);
}

void updateStatus(bool statusWLAN, bool statusMQTT, int buttons)
{
  int offsetY = 567;
  printAt (2, offsetY + 3, &FreeSans12pt7b, statusWLAN ? "WLAN" : "----");
  printAt (102, offsetY + 3, &FreeSans12pt7b, statusMQTT ? "Daten" : "----");
  
  if (buttons & 1 == 1) buttonMarker (1);
  if (buttons & 2 == 2) buttonMarker (2);
  if (buttons & 4 == 4) buttonMarker (3);

  char charBuf[32];
  double battery = 125.0 * display.readBattery() - 375.0;
  snprintf (charBuf, sizeof (charBuf), "Batterie: %3.0f%%", battery);
  printAt (602, offsetY + 6, &FreeSans12pt7b, charBuf);
  drawLine(0, offsetY, 800, offsetY, lineWidth);
}

void showEnv (int aWhich)
{
  int width = 267;
  int height = 50;
  int offsetX = 187;
  int gapX = 10;
  int offsetY = 180;
  Env env;
  if (aWhich == 0) env = outdoor;
  else if (aWhich == 1) env = indoor;

  String where = (aWhich == 0) ? "Aussen   (" : "Innen   (";
  where += env.modTime + String(")");

  printAt (width * aWhich, offsetY + 5, &FreeSans9pt7b, where.c_str(), 1, CenterAlign, width);
  
  printAt (width * aWhich + offsetX, height + offsetY, &FreeSans24pt7b, env.temperature, 2, RightAlign);
  printAt (width * aWhich + offsetX + gapX, height + offsetY + 18, &FreeSans18pt7b, " C", 2);
  printAt (width * aWhich + offsetX + gapX, height + offsetY + 18, &FreeSans9pt7b, "o", 2);

  printAt (width * aWhich + offsetX, 3 * height + offsetY, &FreeSans18pt7b, env.pressure, 1, RightAlign);
  printAt (width * aWhich + offsetX + gapX, 3 * height + offsetY, &FreeSans18pt7b, "hPa", 1);
  
  printAt (width * aWhich + offsetX, 4 * height + offsetY, &FreeSans18pt7b, env.humidity, 1, RightAlign);
  printAt (width * aWhich + offsetX + gapX, 4 * height + offsetY, &FreeSans18pt7b, "%", 1);
  
  printAt (width * aWhich + offsetX, 5 * height + offsetY, &FreeSans18pt7b, env.battery, 1, RightAlign);
  printAt (width * aWhich + offsetX + gapX, 5 * height + offsetY, &FreeSans18pt7b, "V", 1);
  
  drawLine((aWhich + 1) * width, offsetY, (aWhich + 1) * width, offsetY + 283, lineWidth);
  drawLine(aWhich * width, offsetY + 283, (aWhich + 1) * width, offsetY + 283, lineWidth);
}

void showPower ()
{
  int offsetX = 177;
  int width = 267;
  int height = 50;
  int gapX = 10;
  int offsetY = 180;
  int smileyOffset = 20;
  String message = "Strom   (";
  message += powerday;
  message += ")";
  printAt (2 * width, offsetY + 5, &FreeSans9pt7b, message.c_str(), 1, CenterAlign, width);

  printAt (2 * width, height + offsetY, &FreeSans9pt7b, "Bezug:");
  printAt (2 * width + offsetX, height + offsetY, &FreeSans18pt7b, consumption.power, 1, RightAlign);
  printAt (2 * width + offsetX + gapX, height + offsetY, &FreeSans18pt7b, "kWh", 1);

  printSmiley (width * 5 / 2, 2 * height + offsetY + smileyOffset, height / 2, 
    consumption.assessment == 0 ? Neutral : consumption.assessment < 0 ? Frown : Smile,
    consumption.assessment == 0 ? 1 : abs (consumption.assessment));
  
  printAt (2 * width, 3 * height + offsetY + smileyOffset, &FreeSans9pt7b, "Spende:");
  printAt (2 * width + offsetX, 3 * height + offsetY + smileyOffset, &FreeSans18pt7b, production.power, 1, RightAlign);
  printAt (2 * width + offsetX + gapX, 3 * height + offsetY + smileyOffset, &FreeSans18pt7b, "kWh", 1);
  
  printSmiley (width * 5 / 2, 4 * height + offsetY + 2 * smileyOffset, height / 2, 
    production.assessment == 0 ? Neutral : production.assessment < 0 ? Frown : Smile,
    production.assessment == 0 ? 1 : abs (production.assessment));
    
  drawLine(2 * width, offsetY + 283, 800, offsetY + 283, lineWidth);
}

void showStation ()
{
  int height = 35;
  int offsetX = 410;
  int offsetY = 8;
  for (int i = 0; i < 5; ++i)
  printAt (offsetX, offsetY + i * height, &FreeSans12pt7b, station[i]);
  drawLine(400, 179, 800, 179, lineWidth);
}

void showMOTD ()
{
  int height = 44;
  int offsetY = 475;
  printAt (0, offsetY, &FreeSans18pt7b, motd[0]);
  printAt (1, offsetY + height, &FreeSans18pt7b, motd[1]);
}

bool showAnyContent(unsigned long menuLevel, unsigned char which)
{
  unsigned long x0, y0, width, height;
  String ftp_filename;
  unsigned long ftp_filesize;

  if (which == 0) {   // content
    x0 = 0;
    y0 = 180;
    width = 800;
    height = 386;
    ftp_filename = "content-" + String(menuLevel) + ".bmp";
#ifdef MONOCHROME
    ftp_filesize = 38662; // width * height / 8 + 62; // for monochrome
#else
    ftp_filesize = width * height * 3 + 54; // for 24bpp ???
#endif
  } else {   // menu
    x0 = 228;
    y0 = 569;
    width = 345; // ??? 344!!!
    height = 30;
    ftp_filename = "menu-" + String(menuLevel) + ".bmp";
#ifdef MONOCHROME
    ftp_filesize = 1382; // ?! width * height / 8 + 62; // for monochrome
#else
    ftp_filesize = width * height * 3 + 54; // for 24bpp ???
#endif
  }

  //Dynamically allocate buffer, enough for 24bit BMP incl. header
  unsigned char* downloaded_file = (unsigned char*) malloc(ftp_filesize);
  if (downloaded_file == NULL) return false;
  if (which == 0)
    ftp_c.DownloadFile(ftp_filename.c_str(), downloaded_file, ftp_filesize, false);
  else
    ftp_m.DownloadFile(ftp_filename.c_str(), downloaded_file, ftp_filesize, false);
  if ((downloaded_file[0] == 0x42) && (downloaded_file[1] == 0x4d)) {   // check header
    unsigned long offset = downloaded_file[10] + 256 * downloaded_file[11];
#ifdef MONOCHROME
    display.drawBitmap(x0, y0, (uint8_t*) &(downloaded_file[offset]), width, height, 0, 255);
#else
    display.drawBitmap3Bit(x0, y0, &(downloaded_file[offset]), width, height);
#endif
    free (downloaded_file);
    return true;
  }
  return false;
}

bool showMainContent(unsigned long menuLevel)
{
  return showAnyContent(menuLevel, 0);
}

bool showMenuIcons(unsigned long menuLevel)
{
  return showAnyContent(menuLevel, 1);
}
            
int splitPayload (const String& aPayload, String aParts[5])
{
  int parts = 0;
  int index = 0;
  do {
    aParts[parts] = "";
    while (index < aPayload.length() && aPayload[index] != '@') {
      if (aPayload[index] == 0xc3) {
        ++index;
        switch (aPayload[index]) {
          case 0xa4: aParts[parts] += "ae"; break;
          case 0xb6: aParts[parts] += "oe"; break;
          case 0xbc: aParts[parts] += "ue"; break;
          case 0x9f: aParts[parts] += "sz"; break;
          case 0x84: aParts[parts] += "Ae"; break;
          case 0x96: aParts[parts] += "Oe"; break;
          case 0x9c: aParts[parts] += "Ue"; break;
        }
      }
      if (aPayload[index] != '\n') aParts[parts] += aPayload[index];
      ++index;
    }
    ++index;
    ++parts;
  } while (index < aPayload.length() && parts < 5);
  return parts;
}

void handleEnv (int aWhich, char* aTitle, const String& aPayload)
{
  String parsed[5];
  int parts = splitPayload (aPayload, parsed);
  Env env;
  if (parts == 5) {
    strncpy (env.temperature, parsed[0].c_str(), sizeof (env.temperature));
    strncpy (env.pressure, parsed[1].c_str(), sizeof (env.pressure));
    strncpy (env.humidity, parsed[2].c_str(), sizeof (env.humidity));
    strncpy (env.battery, parsed[3].c_str(), sizeof (env.battery));
    strncpy (env.modTime, parsed[4].c_str(), sizeof (env.modTime));
  } else {
    strcpy (env.temperature, "----");
    strcpy (env.pressure, "----");
    strcpy (env.humidity, "----");
    strcpy (env.battery, "----");
    strcpy (env.modTime, "----");
  }
  if (aWhich == 0) outdoor = env;
  else if (aWhich == 1) indoor = env;
}

void handlePower (const String& aPayload)
{
  String parsed[5];
  int parts = splitPayload (aPayload, parsed);
  if (parts == 5) {
    strncpy (powerday, parsed[0].c_str(), sizeof (powerday));
    strncpy (consumption.power, parsed[1].c_str(), sizeof (consumption.power));
    consumption.assessment = parsed[2].toInt();
    strncpy (production.power, parsed[3].c_str(), sizeof (consumption.power));
    production.assessment = parsed[4].toInt();
  } else {
    strcpy (powerday, "----");
    strcpy (consumption.power, "----");
    consumption.assessment = 0;
    strcpy (production.power, "----");
    production.assessment = 0;
  }
}

void handleStation (const String& aPayload)
{
  String parsed[5];
  int parts = splitPayload (aPayload, parsed);
  for (int i = 0; i < parts; ++i) {
    strncpy (station[i], parsed[i].c_str(), sizeof (station[i]));
  }
}

void handleMOTD (const String& aPayload)
{
  String parsed[2];
  int parts = splitPayload (aPayload, parsed);
  for (int i = 0; i < 2; ++i)
    strncpy (motd[i], parsed[i].c_str(), sizeof (motd[i]));
}

void mqttCallback (char* aTopic, byte* aPayload, unsigned int aLength)
{
  String payload ((const char*) aPayload); payload = payload.substring (0, aLength);
  if (strcmp (aTopic, "/inkplate/in/padchecktime") == 0) {
    padCheckTime = payload.toInt();
    if (padCheckTime < 2) padCheckTime = 2;
    if (clockUpdateInSeconds < padCheckTime) clockUpdateInSeconds = padCheckTime;
  }
  if (strcmp (aTopic, "/inkplate/in/clockupdate") == 0) {
    clockUpdateInSeconds = payload.toInt();
    if (clockUpdateInSeconds < padCheckTime) clockUpdateInSeconds = padCheckTime;
  }
  if (strcmp (aTopic, "/inkplate/in/dataupdate") == 0) {
    dataUpdateInClockUpdates = payload.toInt();
  }
  if (strcmp (aTopic, "/inkplate/in/wlan-on") == 0) {
    wlanOnTime = (payload.substring(0, 2).toInt() * 60 + payload.substring(3, 5).toInt()) * 60;
  }
  if (strcmp (aTopic, "/inkplate/in/wlan-off") == 0) {
    wlanOffTime = (payload.substring(0, 2).toInt() * 60 + payload.substring(3, 5).toInt()) * 60;
  }
  if (strcmp (aTopic, "/inkplate/in/indoor") == 0) {
    handleEnv (1, "Innen", payload);
  }
  if (strcmp (aTopic, "/inkplate/in/outdoor") == 0) {
    handleEnv (0, "Aussen", payload);
  }
  if (strcmp (aTopic, "/inkplate/in/power") == 0) {
    handlePower (payload);
  }
  if (strcmp (aTopic, "/inkplate/in/station") == 0) {
    handleStation (payload);
  }
  if (strcmp (aTopic, "/inkplate/in/motd") == 0) {
    handleMOTD (payload);
  }
  if (strcmp (aTopic, "/inkplate/in/datetime") == 0) {
    struct tm newTime;
    newTime.tm_year = payload.substring (0, 4).toInt() - 1900;   // since 1900
    newTime.tm_mon = payload.substring (5, 7).toInt() - 1;    // Jan = 0, Feb = 1, ...
    newTime.tm_mday = payload.substring (8, 10).toInt();
    newTime.tm_hour = payload.substring (11, 13).toInt();
    newTime.tm_min = payload.substring (14, 16).toInt();
    newTime.tm_sec = payload.substring (17, 19).toInt();

    time_t timeToSet = mktime (&newTime);
    struct timeval now = { .tv_sec = timeToSet };
    settimeofday(&now, NULL);
  }
}

int getButtons() {
  int buttons = 0;
  if (display.readTouchpad(PAD1)) {     //Check if first pad has been touched.
    buttons += 1;
  }
  if (display.readTouchpad(PAD2)) {     //Check if second pad has been touched.
    buttons += 2;
  }
  if (display.readTouchpad(PAD3)) {     //Check if third pad has been touched.
    buttons += 4;
  }
  return buttons;
}

struct tm* getTime() {
  time_t now;
  time(&now);
  setenv("TZ", "CET", 1);
  tzset();
  return localtime (&now);
}

void setup() {
  Serial.begin(2000000);
  display.begin();        // Init Inkplate library (you should call this function ONLY ONCE AT THE BEGINNING!)
  int buttons = getButtons();
  int cyclesBetweenClockUpdates = clockUpdateInSeconds / padCheckTime;
  if (cyclesTillClockUpdate > cyclesBetweenClockUpdates) cyclesTillClockUpdate = cyclesBetweenClockUpdates;
  --cyclesTillClockUpdate;
  if (cyclesTillClockUpdate < 0) cyclesTillClockUpdate = 0;
  double secondsTillWakeUp = padCheckTime - 1.6;
  char charBuf[32];
  bool newMenuLevel = false;

Serial.print ("cTCU: "); Serial.print (cyclesTillClockUpdate);
Serial.print (" // cTDU: "); Serial.print (cyclesTillDataUpdate);
Serial.print (" // b: "); Serial.print (buttons);

  bool firstStart = false;
  time_t now;
  time(&now);
Serial.print (" // now: "); Serial.print ((long) now);
  if ((long) now < 86400) {
    firstStart = true;
    menuLevel = 0;
    newMenuLevel = true;
    cyclesTillFullUpdate = 0;
  }
Serial.print (" // fS: "); Serial.println (firstStart);

  bool bme280initialized = true;
  if (!bme.begin(0x76)) {
    Serial.println(F("Could not find a valid BME280 sensor, check wiring!"));
    bme280initialized = false;
  }

  if ((cyclesTillClockUpdate == 0) 
     || (cyclesTillDataUpdate == 0) 
     || (buttons != 0)
     || firstStart) {

    cyclesTillClockUpdate = cyclesBetweenClockUpdates;
    bool hideNormalContent = false;

    if ((!firstStart) && (buttons == 0) && (cyclesTillDataUpdate != 0))   // wake-up reason was clock update
    {
Serial.println ("do clock");
      --cyclesTillDataUpdate;
      if (cyclesTillDataUpdate < 0) cyclesTillDataUpdate = 0;
      updateStatus(false, false, 0);
      menuLevel /= 10;
      newMenuLevel = true;
      
      dtostrf(display.readBattery(), 4, 2, charBuf);
      strncpy (indoor.battery, charBuf, sizeof (indoor.battery));
      
      if (bme280initialized) {
        dtostrf(bme.readTemperature(), 4, 1, charBuf);
        strncpy (indoor.temperature, charBuf, sizeof (indoor.temperature));

        dtostrf(bme.readPressure() / 100.0, 6, 1, charBuf);
        strncpy (indoor.pressure, charBuf, sizeof (indoor.pressure));
        
        dtostrf(bme.readHumidity(), 4, 1, charBuf);
        strncpy (indoor.humidity, charBuf, sizeof (indoor.humidity));
        
        strftime(charBuf, sizeof(charBuf), "%H:%M", getTime());
        strncpy (indoor.modTime, charBuf, sizeof (indoor.modTime));
      }
    } else if ((!firstStart) && (buttons == 5)) {   // easter egg
Serial.println ("easter egg");
      printSmiley (400, 380, 120, Smile, 3);
      hideNormalContent = true;
    } else {   // handle pressed buttons or initial startup
Serial.println ("do buttons or initial");
      cyclesTillDataUpdate = dataUpdateInClockUpdates;
      bool statusWLAN = false;
      bool statusMQTT = false;

      if (buttons != 0) {
        if (menuLevel <= 999999) {   // maximum 6 levels
          menuLevel *= 10;
          if (buttons < 3) menuLevel += buttons;
          if (buttons == 4) menuLevel += 3;
          newMenuLevel = true;
        }
      }
      if (buttons == 7) {
        menuLevel == 0;
        newMenuLevel = true;
      }
      Serial.print ("Menu: "); Serial.println (menuLevel);

      // only try update, if WLAN is turned on
      struct tm* now = getTime();
      int now_s = (now->tm_hour * 60 + now->tm_min) * 60;
      if (firstStart || (getButtons() == 7) || ((now_s >= wlanOnTime) && ( now_s < wlanOffTime)))
      {
Serial.println ("WiFi on");
        //Connect to the WiFi network.
        WiFi.mode(WIFI_MODE_STA);
        WiFi.begin(ssid, password);
        int maxTries = 10;
        while ((WiFi.status() != WL_CONNECTED) && (maxTries > 0)) {
          delay(500);
          --maxTries;
        }

        if (WiFi.status() == WL_CONNECTED) {   // successful connection
          statusWLAN = true;

          if (menuLevel != 0) {
            bool success;
            ftp_c.OpenConnection();
            success = ftp_c.isConnected();
            if (success) {
              ftp_c.ChangeWorkDir("inkplate");
              ftp_c.InitFile("Type I");
            }
            if (success) {
              success = showMainContent(menuLevel);
              ftp_c.CloseConnection();
            }
            Serial.print ("showMainContent: "); Serial.println (success ? "OK" : "NOK");
            if (success) {
              ftp_m.OpenConnection();
              success = ftp_m.isConnected();          
            }
            if (success) {
              ftp_m.ChangeWorkDir("inkplate");
              ftp_m.InitFile("Type I");
            }
            if (success) {
              success = showMenuIcons(menuLevel);
              ftp_m.CloseConnection();
            }
            Serial.print ("showMenuIcons: "); Serial.println (success ? "OK" : "NOK");
            if (success) hideNormalContent = true;
          }

          client.setServer(MQTT_BROKER, 1883);
          client.setCallback(mqttCallback);
          client.setBufferSize(1024);

          maxTries = 10;
          while (!client.connected()) {
            if ((!client.connect("InkplateClient")) && (maxTries > 0)) {
              delay(500);
              --maxTries;
            }
          }

          if (client.connected()) {    // successful connection
            Serial.println ("MQTT connected");
            statusMQTT = true;
            client.subscribe("/inkplate/in/#");
            client.publish ("/inkplate/out/reset", "RST");
            char charBuf[32];
            dtostrf(display.readBattery(), 4, 2, charBuf);
            client.publish ("/inkplate/out/battery", charBuf);
            strncpy (indoor.battery, charBuf, sizeof (indoor.battery));

            if (bme280initialized) {
              dtostrf(bme.readTemperature(), 4, 2, charBuf);
              client.publish ("/inkplate/out/temperature", charBuf);
              strncpy (indoor.temperature, charBuf, sizeof (indoor.temperature));

              dtostrf(bme.readPressure() / 100.0, 6, 1, charBuf);
              client.publish ("/inkplate/out/pressure", charBuf);
              strncpy (indoor.pressure, charBuf, sizeof (indoor.pressure));
              
              dtostrf(bme.readHumidity(), 4, 2, charBuf);
              client.publish ("/inkplate/out/humidity", charBuf);
              strncpy (indoor.humidity, charBuf, sizeof (indoor.humidity));
              
              strftime(charBuf, sizeof(charBuf), "%H:%M", getTime());
              strncpy (indoor.modTime, charBuf, sizeof (indoor.modTime));
            }
            itoa(buttons, charBuf, 10);
            client.publish ("/inkplate/out/buttons", charBuf);
            if (newMenuLevel) {
              itoa(menuLevel, charBuf, 10);
              client.publish ("/inkplate/out/menulevel", charBuf);
            }

            maxTries = 40;
            while (maxTries > 0) {
              client.loop();
              delay(50);
              --maxTries;
            }
          }   // MQTT OK
        }   // WLAN OK
        WiFi.mode(WIFI_OFF);
        Serial.println ("WiFi off");
      }   // correct time slot for WLAN
      updateStatus(statusWLAN, statusMQTT, buttons);
    }
    updateClock();
    if (!hideNormalContent) {
      showEnv(0);
      showEnv(1);
      showPower();
      showStation();
      showMOTD();
    }

Serial.print ("cyclesTillFullUpdate"); Serial.println(cyclesTillFullUpdate);
    if (cyclesTillFullUpdate == 0) {
      display.display();   // update display
      cyclesTillFullUpdate = 20;
    } else {
      display.partialUpdate();          //Do partial update
      --cyclesTillFullUpdate;
    }
  }
  
  rtc_gpio_isolate(GPIO_NUM_12);                                       //Isolate/disable GPIO12 on ESP32 (only to reduce power consumption in sleep)
  if (display.readBattery() < 3.01) {   // nominal discharge cut-off voltage is 3.0V
    error("Batterie alle!");
    secondsTillWakeUp = 14 * 24 * 60 * 60;         //Sleep for 2 weeks to avoid deep discharge
  }  
  Serial.print ("=== Sleeping for ");
  Serial.println (secondsTillWakeUp);
  esp_sleep_enable_timer_wakeup(secondsTillWakeUp * uS_TO_S_FACTOR);     //Activate wake-up timer
  esp_deep_sleep_start();                                           //Put ESP32 into deep sleep. Program stops here.
}

void loop() {
}

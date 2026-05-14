// #include <Arduino.h>
// #define DEBUG_FRAME
//Frame wifi
#include "FrameWeb.h"
FrameWeb frame;

#include <time.h>
// Reset Reason 
#include <rom/rtc.h>

// LOGGER update 120 chars Max
#define LOG(format, ...) do { \
  if (Serial) { \
    char temp[120];\
    snprintf(temp, 120, format, ##__VA_ARGS__); \
    Serial.println(temp); \
  } \
} while (0)

const char VERSION[] ="0.0.1";

// Main variables
int8_t wifiLost = 0;
long previousMillis = 0;

// Time facilities
const long gmtOffset_sec     = 3600;
const int daylightOffset_sec = 3600; // heure d'ete 3600
struct tm timeinfo;            // time struct
const char* ntpServer        = "pool.ntp.org";

// Time HH:MM.ss
String getTime() {
  static char temp[10];
  snprintf(temp, 10, "%02d:%02d:%02d", timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec );
  return String(temp);
}

// Date as europeen format
String getDate(int sh = -1){
  static char temp[20];
  switch (sh) {
  case 0: 
    snprintf(temp, 20, "%02d/%02d/%04d", timeinfo.tm_mday, (timeinfo.tm_mon+1), (1900+timeinfo.tm_year) );
    break;
  case 1:
    snprintf(temp, 20, "%02d/%02d/%02d %02d:%02d", timeinfo.tm_mday, (timeinfo.tm_mon+1), (timeinfo.tm_year-100),  timeinfo.tm_hour,timeinfo.tm_min );
    break;
  default:
    snprintf(temp, 20, "%02d/%02d/%04d %02d:%02d:%02d", timeinfo.tm_mday, (timeinfo.tm_mon+1), (1900+timeinfo.tm_year),  timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec );
    break;
  }
  return String(temp);
}

// Frame option
void saveConfigCallback() {}
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {}
void configModeCallback (WiFiManager *myWiFiManager) {}

// setup -------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  Serial.printf("Start setup Ver:%s\n\r",VERSION);

  // Start framework
  frame.setup();

  frame.externalHtmlTools="Specific home page is visible at :<a class='button' href='/index'>Index</a>";
  // Init time and correct 
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); //init and get the time
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  // Start time
  getLocalTime(&timeinfo);

  // Wait get time delay
  delay(2000);

  // Get Reset Reason 
  RESET_REASON rr = rtc_get_reset_reason(0);
  LOG("%s -CPU REBOOT(%s)  IP:%s MAC:%s", getDate().c_str(), frame.resetReason((int)rr), WiFi.localIP().toString().c_str() , WiFi.macAddress().c_str() );
}

// Main loop -----------------------------------------------------------------
void loop() {

  // Call frame loop
  frame.loop();

  // Is alive executed every 1 sec.
  if ( millis() - previousMillis > 1000L) {
    previousMillis = millis();
    getLocalTime(&timeinfo);
   
    int wifistat = WiFi.status();

    // if wifi is down, try reconnecting every 60 seconds
    if (wifistat != WL_CONNECTED) {
      wifiLost++;
      if (wifiLost==10) {
        //LOG("%s -WiFi Lost:%s wifiLost:%d sec. localIP:%s", getDate().c_str(), frame.wifiStatus(wifistat), wifiLost, WiFi.localIP().toString().c_str() );
      }
      if (wifiLost == 50) {
        //LOG("%s -WiFi disconnect OK after 50s (%s).",getDate().c_str(), frame.wifiStatus(wifistat));
        WiFi.disconnect();
      }
      if (wifiLost == 60) {
        if (WiFi.reconnect()) {
         //LOG("%s -WiFi reconnect OK after 60s (%s).",getDate().c_str(), frame.wifiStatus(wifistat));
          wifistat = WL_CONNECTED;
        }
      }
    } else {
      wifiLost = 0;
    }
    
    LOG("%s -WiFi(%s) localIP:%s",getDate().c_str(), frame.wifiStatus(wifistat), WiFi.localIP().toString().c_str() );

  } // End second
}
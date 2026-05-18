// #include <Arduino.h>
// #define DEBUG_FRAME
//Frame wifi
#include "FrameWeb.h"
FrameWeb frame;

// Physical Ethernet
#include <ETH.h>
// Test server/client UDP
#include <WiFiServer.h>
WiFiServer server(5000);  // TCP server on port 5000
// Externel Seriel TX->GPIO4 RX->GPIO36
//HardwareSerial Serial1; // Use UART1 for external serial communication

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

volatile uint32_t tcpCharsReceived = 0;
volatile uint32_t tcpCharsSent = 0;
portMUX_TYPE tcpCountersMux = portMUX_INITIALIZER_UNLOCKED;

String formatIpAddress(const IPAddress& ip) {
  if (ip == IPAddress((uint32_t)0)) {
    return "not connected";
  }
  return ip.toString();
}

void addTcpCounters(uint32_t rxInc, uint32_t txInc) {
  portENTER_CRITICAL(&tcpCountersMux);
  tcpCharsReceived += rxInc;
  tcpCharsSent += txInc;
  portEXIT_CRITICAL(&tcpCountersMux);
}

void refreshExternalHtmlTools() {
  uint32_t rx = 0;
  uint32_t tx = 0;
  portENTER_CRITICAL(&tcpCountersMux);
  rx = tcpCharsReceived;
  tx = tcpCharsSent;
  portEXIT_CRITICAL(&tcpCountersMux);

  frame.externalHtmlTools =
    "<div class='action-item'><div>- Network addresses</div><div class='button-group'>"
    "<span>WiFi: <b>" + formatIpAddress(WiFi.localIP()) + "</b></span>"
    "<span>Cable: <b>" + formatIpAddress(ETH.localIP()) + "</b></span>"
    "</div></div>"
    "<div class='action-item'><div>- TCP chars</div><div class='button-group'>"
    "<span>Received: <b>" + String(rx) + "</b></span>"
    "<span>Sent: <b>" + String(tx) + "</b></span>"
    "</div></div>"
    "<div class='action-item'><div>- Specific home page is visible at :</div>"
    "<div class='button-group'><a class='button' href='/index'>Index</a></div></div>";
}

// Main variables
bool eth_connected = false;
uint16_t wifiLost = 0;
unsigned long previousMillis = 0;
// Timestamp of the last manual reconnect attempt.
unsigned long lastWifiReconnectMs = 0;
// Prevent repeated WiFi.disconnect() calls during the same outage.
bool wifiForcedDisconnectDone = false;

// Time facilities
const long gmtOffset_sec     = 3600;
const int daylightOffset_sec = 3600; // heure d'ete 3600
struct tm timeinfo;            // time struct
const char* ntpServer        = "pool.ntp.org";

/* Event handler (ESP32 core 3.x) */
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {

    case ARDUINO_EVENT_ETH_START:
      Serial.println("Ethernet started");
      ETH.setHostname("esp32-poe-iso");
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("Ethernet connected");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("Ethernet IP: ");
      Serial.println(ETH.localIP());
      // Mark Ethernet as operational only after the interface has a valid IP.
      eth_connected = true;
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("Ethernet disconnected");
      eth_connected = false;
      break;

    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("Ethernet stopped");
      eth_connected = false;
      break;

    default:
      break;
  }
}

// Time HH:MM:ss
String getTime() {
  char temp[12];
  snprintf(temp, sizeof(temp), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(temp);
}

// Format date in European format (DD/MM/YYYY HH:MM:ss)
// sh: 0=date only (DD/MM/YYYY), 1=date+time short (DD/MM/YY HH:MM), -1/default=full (DD/MM/YYYY HH:MM:ss)
String getDate(int sh = -1) {
  char temp[32];
  int month = timeinfo.tm_mon + 1;
  int year = 1900 + timeinfo.tm_year;
  int year_short = timeinfo.tm_year % 100;
  
  switch (sh) {
    case 0:  // Date only
      snprintf(temp, sizeof(temp), "%02d/%02d/%04d", timeinfo.tm_mday, month, year);
      break;
    case 1:  // Date + time short
      snprintf(temp, sizeof(temp), "%02d/%02d/%02d %02d:%02d", timeinfo.tm_mday, month, year_short, timeinfo.tm_hour, timeinfo.tm_min);
      break;
    default:  // Full date + time with seconds
      snprintf(temp, sizeof(temp), "%02d/%02d/%04d %02d:%02d:%02d", timeinfo.tm_mday, month, year, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      break;
  }
  return String(temp);
}

// Frame option
void saveConfigCallback() {}
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {}
void configModeCallback (WiFiManager *myWiFiManager) {}

// thread TCP client
#define TCP_TASK_PRIORITY 12 // Low numbers denote low priority tasks
TaskHandle_t tcpCxHandle = NULL;

void IRAM_ATTR tcpTask(void *pvParameter) {
  LOG("%s +Start tcpTask",getDate().c_str());
  // Only one TCP client is bridged at a time.
  WiFiClient ethClient;

  while (1) {
      // Accept a new client without blocking the task loop.
      WiFiClient incomingClient = server.available();
      if (incomingClient) {
        if (!ethClient || !ethClient.connected()) {
          ethClient = incomingClient;
          // Send a banner so terminal clients know the bridge is active.
          ethClient.write("; Welcome to ESP32 TCP Server <-> Prusa Mk3s\r\n");
          Serial.println("Client connecté:" + incomingClient.remoteIP().toString() + ":" + String(incomingClient.remotePort()));
        } else {
          // Refuse extra clients while one session is already attached to the serial bridge.
          Serial.println("Client refusé:" + incomingClient.remoteIP().toString() + ":" + String(incomingClient.remotePort()));
          incomingClient.stop();
        }
      }

      if (ethClient && ethClient.connected()) {
        uint32_t rxCount = 0;
        uint32_t txCount = 0;

        // Forward every received TCP byte directly to the UART.
        while (ethClient.available() > 0) {
          int c = ethClient.read();
          if (c >= 0) {
            Serial1.write((uint8_t)c);
            rxCount++;
          }
        }

        // Mirror every serial1 byte back to the active TCP client.
        while (Serial1.available() > 0) {
          int c = Serial1.read();
          if (c >= 0) {
            ethClient.write((uint8_t)c);
            txCount++;
          }
        }

        if (rxCount > 0 || txCount > 0) {
          addTcpCounters(rxCount, txCount);
        }
      } 

      vTaskDelay(pdMS_TO_TICKS(10));
      taskYIELD();
  } // end while
} // end tcpTask

// setup -------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  Serial.printf("Start setup Ver:%s\n\r",VERSION);

  Serial1.begin(115200, SERIAL_8N1, 36, 4); // Initialize external serial communication

  // Start framework
  frame.setup();

  // Explicitly power the external PHY before starting Ethernet.
  pinMode(ETH_PHY_POWER, OUTPUT);
  digitalWrite(ETH_PHY_POWER, HIGH);
  delay(10);

  /* Register Ethernet events */
  WiFi.onEvent(WiFiEvent);

  /* Start Ethernet */
  ETH.begin();

  // Server 
  server.begin();

  refreshExternalHtmlTools();
  // Init time and correct 
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); //init and get the time
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  // Start time
  getLocalTime(&timeinfo);

  // Start TCP clent thread
  xTaskCreate(&tcpTask, "startingTcpTaskGet", 4096, NULL, TCP_TASK_PRIORITY, &tcpCxHandle);

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

  // Is alive executed every 5 sec.
  if ( millis() - previousMillis > 5000L) {
    previousMillis = millis();
    getLocalTime(&timeinfo);
   
    int wifistat = WiFi.status();

    refreshExternalHtmlTools();

    // This recovery logic supervises the Wi-Fi station side independently from Ethernet.
    if (wifistat != WL_CONNECTED) {
      wifiLost++;
      if (wifiLost == 1 || (wifiLost % 10) == 0) {
        LOG("%s -WiFi Lost:%s down:%lus localIP:%s", getDate().c_str(), frame.wifiStatus(wifistat), (unsigned long)wifiLost * 5UL, WiFi.localIP().toString().c_str() );
      }

      if (!wifiForcedDisconnectDone && wifiLost >= 50) {
        // Force a clean disconnect once before switching to periodic reconnects.
        LOG("%s -WiFi still down after 50s (%s): forcing disconnect.", getDate().c_str(), frame.wifiStatus(wifistat));
        WiFi.disconnect();
        wifiForcedDisconnectDone = true;
      }

      if (millis() - lastWifiReconnectMs >= 60000UL) {
        lastWifiReconnectMs = millis();
        // Retry at a low rate to avoid hammering the radio stack.
        LOG("%s -WiFi reconnect attempt (%lus down).", getDate().c_str(), (unsigned long)wifiLost * 5UL);
        WiFi.reconnect();
      }
    } else {
      if (wifiLost > 0) {
        LOG("%s -WiFi restored after %lus.", getDate().c_str(), (unsigned long)wifiLost * 5UL);
      }
      wifiLost = 0;
      wifiForcedDisconnectDone = false;
    }
    
    LOG("%s -wifiIP:%s ethIP:%s",getDate().c_str(),  WiFi.localIP().toString().c_str(), ETH.localIP().toString().c_str() );

  } // End second
}
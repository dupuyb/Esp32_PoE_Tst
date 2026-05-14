#include <time.h>
// Reset Reason 
#include <rom/rtc.h>

// test Eth
#include <ETH.h>
#include <WiFi.h>
// Test server/client UDP
#include <WiFiUdp.h>
// Test server/client TCP 
#include <WiFiServer.h>
WiFiServer server(5000);

WiFiUDP udp;
char packetBuffer[255];
unsigned int localPort = 9999;

/* ESP32-POE-ISO Ethernet configuration */
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_PHY_ADDR    0
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_POWER   12
#define ETH_CLK_MODE    ETH_CLOCK_GPIO17_OUT

static bool eth_connected = false;

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

// setup -------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  Serial.printf("Start setup Ver:%s\n\r",VERSION);

  /* Power LAN8720 PHY */
  pinMode(ETH_PHY_POWER, OUTPUT);
  digitalWrite(ETH_PHY_POWER, HIGH);
  delay(10);

  /* Register Ethernet events */
  WiFi.onEvent(WiFiEvent);

  /* Start Ethernet */
  ETH.begin();

  // start UDP server
  udp.begin(localPort);

  // Server 
  server.begin();

  //mframe.externalHtmlTools="Specific home page is visible at :<a class='button' href='/index'>Index</a>";
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
  LOG("%s -CPU REBOOT(%d) MAC:%s", getDate().c_str(), (int)rr, ETH.macAddress().c_str() );
}

// Main loop -----------------------------------------------------------------
void loop() {
  
  // Test Udp
  if (udp.remoteIP()) {
    // receive incoming UDP packets
    int packetSize = udp.parsePacket();
    Serial.print(" Received packet from : "); Serial.println(udp.remoteIP());
    Serial.print(" Size : "); Serial.println(packetSize);
    if (packetSize) {
      int len = udp.read(packetBuffer, 255);
      if (len > 0) packetBuffer[len - 1] = 0;
      Serial.printf("Data : %s\n", packetBuffer);
      udp.beginPacket(udp.remoteIP(), udp.remotePort());
      udp.printf("UDP packet was received OK\r\n");
      udp.endPacket();
    
      // send UDP packet
      udp.beginPacket(udp.remoteIP(), localPort);
      udp.printf("Send millis: ");
      char buf[20];
      unsigned long testID = millis();
      sprintf(buf, "%lu", testID);
      udp.printf(buf);
      udp.printf("\r\n");
      udp.endPacket();
    }
  }

  // test TCP server
  WiFiClient client = server.available();   // listen for incoming clients

  if (client) {
    Serial.println("Client connecté");

    while (client.connected()) {

      while (client.available()) {
        char c = client.read();

        Serial.print(c);

        // réponse au client
        client.print(c);
      }
    }

    client.stop();
    Serial.println("Client déconnecté");
  }

  // Is alive executed every 2 sec.
  if ( millis() - previousMillis > 2000L) {
    previousMillis = millis();
    getLocalTime(&timeinfo);
   
    if (eth_connected) {
      String msg = "ETH OK, IP=" + ETH.localIP().toString();
      LOG("%s %s", getDate().c_str(), msg.c_str(), msg.c_str() );
    } else {
      LOG("%s -ETH DOWN", getDate().c_str() );
    }

  } // End 2 second
}
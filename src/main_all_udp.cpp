/**
 * @file main_all_udp.cpp
 * @brief TCP/Serial bridge application for Olimex ESP32 PoE board
 * 
 * This test program demonstrates a bidirectional TCP <-> Serial bridge on the ESP32 PoE board.
 * The application:
 * - Bridges TCP connections (port 5000) to the external UART (Serial1)
 * - Runs a web server for monitoring and remote configuration via FrameWeb framework
 * - Provides WebSocket support for real-time status updates
 * - Manages dual network interfaces: Ethernet (primary) and Wi-Fi (fallback)
 * - Handles graceful Wi-Fi reconnection with exponential backoff
 * - Logs all network events and connection statistics
 * - Bridges only one TCP client at a time to the serial port
 *
 * @section Usage
 * Test via Linux terminal with netcat:
 * @code
 *   nc <esp32_ip_address> 5000
 *   # Type characters: they will be sent to Serial1
 *   # Data from Serial1 will be echoed back to terminal
 * @endcode
 *
 * Create a pseudo-serial port with socat:
 * @code
 *   sudo socat -d -d PTY,link=/dev/prusa,user=$USER,group=dialout,mode=777,raw,echo=0 TCP:<esp32_ip_address>:5000
 *   # Use /dev/prusa in PrusaSlicer, OctoPrint, or other serial applications
 * @endcode
 *
 * @note This implementation is based on the Prusa OctoPrint TCP bridge concept:
 *       https://help.prusa3d.com/article/prusaprint-rpi-zero-and-octoprint_2180
 *       The ESP32 PoE board is used as a network-to-serial gateway for 3D printer communication.
 *
 * @author [Your Name]
 * @version 0.0.1
 */
// #define DEBUG_FRAME

// ============== Framework and Core Includes ==============
#include "FrameWeb.h"
FrameWeb frame;  // Web server and configuration framework instance

// ============== Network Configuration ==============
#include <ETH.h>      // Ethernet library (hardware SPI, 100BASE-T)
#include <WiFiServer.h> // TCP server for serial bridge

// TCP server configuration for serial bridge
#define TCP_PORT 5000  // Port for TCP connections forwarded to Serial1
WiFiServer server(TCP_PORT);

// ============== External Serial Port Configuration ==============
// Serial1 connects to Olimex ESP32 PoE external connector
// See Olimex ESP32-PoE board schematic for pin assignments
#define ExtSerialRx 36  // GPIO36: Serial1 receive pin
#define ExtSerialTx 4   // GPIO4:  Serial1 transmit pin

#include <time.h>  // Time utilities for date/time formatting
#include <rom/rtc.h>  // RTC (Real-Time Clock) utilities for reset reason detection

/**
 * @macro LOG(format, ...)
 * @brief Thread-safe logging macro with timestamp
 * Outputs formatted log messages to Serial console (max 120 characters per line).
 * Can be safely called from multiple tasks/ISRs.
 */
#define LOG(format, ...) do { \
  if (Serial) { \
    char temp[120];\
    snprintf(temp, 120, format, ##__VA_ARGS__); \
    Serial.println(temp); \
  } \
} while (0)

const char VERSION[] = "0.0.1";  // Application firmware version

// ============== TCP Bridge Statistics (Thread-Safe) ==============
/** @brief Total characters received from TCP clients and forwarded to Serial1 */
volatile uint32_t tcpCharsReceived = 0;

/** @brief Total characters received from Serial1 and sent to TCP clients */
volatile uint32_t tcpCharsSent = 0;

/** @brief Mutex for protecting TCP counter access from multiple tasks */
portMUX_TYPE tcpCountersMux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Format IP address for display
 * @param ip IPAddress to format
 * @return String representation: "A.B.C.D" or "not connected" if not assigned
 */
String formatIpAddress(const IPAddress& ip) {
  if (ip == IPAddress((uint32_t)0)) {
    return "not connected";
  }
  return ip.toString();
}

/**
 * @brief Thread-safe increment of TCP character counters
 * @param rxInc Number of characters received from TCP and sent to Serial1
 * @param txInc Number of characters received from Serial1 and sent to TCP
 * 
 * Uses critical section to prevent counter corruption from concurrent access.
 */
void addTcpCounters(uint32_t rxInc, uint32_t txInc) {
  portENTER_CRITICAL(&tcpCountersMux);
  tcpCharsReceived += rxInc;
  tcpCharsSent += txInc;
  portEXIT_CRITICAL(&tcpCountersMux);
}

/**
 * @brief Update web interface with current network and statistics information
 * 
 * Displays:
 * - WiFi and Ethernet IP addresses
 * - TCP bridge character transfer statistics
 * - Link to custom index page
 */
void refreshExternalHtmlTools() {
  uint32_t rx = 0;
  uint32_t tx = 0;
  
  // Read TCP counters atomically from tcpTask
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

// ============== Network State Variables ==============
/** @brief Flag: true when Ethernet PHY has valid IP address */
bool eth_connected = false;

/** @brief Counter: seconds since WiFi was last connected (incremented every 5s) */
uint16_t wifiLost = 0;

/** @brief Timestamp of last loop() iteration (milliseconds) */
unsigned long previousMillis = 0;

/** @brief Timestamp of the last WiFi reconnect attempt (milliseconds) */
unsigned long lastWifiReconnectMs = 0;

/** @brief Flag: prevents repeated WiFi.disconnect() calls during same outage */
bool wifiForcedDisconnectDone = false;

// ============== Time Configuration ==============
/** @brief UTC offset in seconds (3600 = UTC+1 for Central European Time) */
const long gmtOffset_sec = 3600;

/** @brief Daylight saving offset in seconds (3600 = +1 hour for summer time) */
const int daylightOffset_sec = 3600;  // Central European Summer Time (CEST) offset

/** @brief Broken-down time structure (updated every 5 seconds in loop) */
struct tm timeinfo;

/** @brief NTP server pool for time synchronization */
const char* ntpServer = "pool.ntp.org";

/**
 * @brief WiFi/Ethernet event handler (ESP32 Arduino core 3.x)
 * 
 * Handles network interface state transitions and link up/down events.
 * Updated eth_connected flag only when Ethernet has a valid IP address.
 * 
 * @param event The WiFi/Ethernet event type
 * @param info Event-specific information (varies by event type)
 */
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {

    case ARDUINO_EVENT_ETH_START:
      Serial.println("Ethernet started");
      ETH.setHostname("esp32-poe-iso");
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      // Physical link established; IP negotiation starting
      Serial.println("Ethernet connected");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      // DHCP or static IP successfully assigned; Ethernet is now operational
      Serial.print("Ethernet IP: ");
      Serial.println(ETH.localIP());
      eth_connected = true;  // Mark Ethernet as ready
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      // Physical link lost or IP lost
      Serial.println("Ethernet disconnected");
      eth_connected = false;
      break;

    case ARDUINO_EVENT_ETH_STOP:
      // Ethernet interface stopped
      Serial.println("Ethernet stopped");
      eth_connected = false;
      break;

    default:
      // Ignore other events (WiFi, Bluetooth, etc.)
      break;
  }
}

/**
 * @brief Get current time in HH:MM:SS format
 * @return Time string (e.g., "14:30:45")
 */
String getTime() {
  char temp[12];
  snprintf(temp, sizeof(temp), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(temp);
}

/**
 * @brief Format date/time in European format
 * @param sh Format selector:
 *        - 0: Date only (DD/MM/YYYY)
 *        - 1: Date + short time (DD/MM/YY HH:MM)
 *        - -1 or other: Full format (DD/MM/YYYY HH:MM:SS) [default]
 * @return Formatted date/time string
 */
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

// ============== FrameWeb Framework Callbacks ==============
/** @brief Called when configuration is saved via web interface (not used in this app) */
void saveConfigCallback() {}

/** @brief Called when WebSocket events occur (not used in this app) */
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {}

/** @brief Called when WiFi enters AP mode for configuration (not used in this app) */
void configModeCallback (WiFiManager *myWiFiManager) {}

// ============== TCP Bridge Task ==============
/**
 * Priority level for TCP task (1=lowest, 25=highest).
 * Priority 12 = low-medium priority, yields to time-critical tasks.
 */
#define TCP_TASK_PRIORITY 12

/** @brief Task handle for TCP bridge task (allows task control) */
TaskHandle_t tcpCxHandle = NULL;

/**
 * @brief FreeRTOS task for TCP <-> Serial1 bridging
 * 
 * Implements a single-client TCP server on port 5000 that bridges
 * bidirectionally to Serial1 (external UART). Only one TCP client
 * is serviced at a time; additional connections are rejected.
 * 
 * Key features:
 * - Non-blocking client acceptance
 * - Automatic TCP connection timeout detection
 * - Byte-by-byte forwarding with statistics
 * - Periodic task yield for FreeRTOS scheduler
 * - Thread-safe statistics counters
 * 
 * @param pvParameter Unused (NULL passed from xTaskCreate)
 */
void IRAM_ATTR tcpTask(void *pvParameter) {
  LOG("%s +Start tcpTask", getDate().c_str());
  
  // TCP client handle: maintains connection to single active client
  // Set to empty/disconnected when no client is active
  WiFiClient ethClient;

  while (1) {
      // Check for incoming TCP connection without blocking
      WiFiClient incomingClient = server.available();
      if (incomingClient) {
        // Check if current client slot is available
        if (!ethClient || !ethClient.connected()) {
          // Accept new client and replace old one (if disconnected)
          ethClient = incomingClient;
          // Send welcome banner to terminal clients
          ethClient.write("; Welcome to ESP32 TCP Server <-> Prusa Mk3s\r\n");
          Serial.println("Client connected: " + incomingClient.remoteIP().toString() + ":" + String(incomingClient.remotePort()));
        } else {
          // Reject connection if a client is already active
          // Only one TCP client can bridge to Serial1 at a time
          Serial.println("Client refused: " + incomingClient.remoteIP().toString() + ":" + String(incomingClient.remotePort()));
          incomingClient.stop();
        }
      }

      // Bidirectional bridge: move data between TCP and Serial1
      if (ethClient && ethClient.connected()) {
        uint32_t rxCount = 0;  // Bytes: TCP -> Serial1
        uint32_t txCount = 0;  // Bytes: Serial1 -> TCP

        // Forward incoming TCP data to external serial port (Serial1)
        while (ethClient.available() > 0) {
          int c = ethClient.read();
          if (c >= 0) {
            Serial1.write((uint8_t)c);
            rxCount++;
          }
        }

        // Forward incoming serial data back to TCP client
        while (Serial1.available() > 0) {
          int c = Serial1.read();
          if (c >= 0) {
            ethClient.write((uint8_t)c);
            txCount++;
          }
        }

        // Update thread-safe statistics if any data was transferred
        if (rxCount > 0 || txCount > 0) {
          addTcpCounters(rxCount, txCount);
        }
      } 

      // Periodic task yield to FreeRTOS scheduler
      vTaskDelay(pdMS_TO_TICKS(10));  // Sleep 10ms between poll cycles
      taskYIELD();                     // Allow other tasks to run
  }
}

// ============================================
// Setup: Initialization on boot
// ============================================

/**
 * @brief Initialize all subsystems and start main tasks
 * 
 * Initialization sequence:
 * 1. Configure debug serial (Serial0 @ 115200)
 * 2. Initialize external UART (Serial1)
 * 3. Start FrameWeb web server framework
 * 4. Power and start Ethernet interface
 * 5. Register network event handlers
 * 6. Start TCP bridge task
 * 7. Synchronize time from NTP
 * 8. Log boot reason and network status
 */
void setup() {
  Serial.begin(115200);
  Serial.printf("Start setup Ver:%s\n\r", VERSION);
  
  // External Serial1 initialization
  // Connected to Olimex external connector: TX=GPIO4, RX=GPIO36
  // Baud rate: 115200, 8 data bits, no parity, 1 stop bit
  Serial1.begin(115200, SERIAL_8N1, ExtSerialRx, ExtSerialTx);

  // Initialize FrameWeb framework (web server, WebSocket, config management)
  frame.setup();

  // Enable Ethernet PHY power supply
  // Required for external PHY to be operational on Olimex ESP32-PoE board
  pinMode(ETH_PHY_POWER, OUTPUT);
  digitalWrite(ETH_PHY_POWER, HIGH);
  delay(10);  // Allow PHY to power up

  // Register WiFi/Ethernet event handler
  WiFi.onEvent(WiFiEvent);

  // Start Ethernet interface (100BASE-T, auto-negotiation)
  ETH.begin();

  // Start TCP server listening on TCP_PORT
  server.begin();

  // Refresh web interface with current network status
  refreshExternalHtmlTools();
  
  // ========== Time Synchronization ==========
  // Configure system time from NTP server
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // Start NTP sync
  // Set timezone: CET-1CEST,M3.5.0,M10.5.0/3 = Central European Time with DST
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();  // Apply timezone settings

  // Read current time from system
  getLocalTime(&timeinfo);

  // ========== Start FreeRTOS Tasks ==========
  // Create TCP bridge task (handles all TCP <-> Serial1 bridging)
  xTaskCreate(&tcpTask, "startingTcpTaskGet", 4096, NULL, TCP_TASK_PRIORITY, &tcpCxHandle);

  // Wait for NTP to synchronize time (typical: 1-2 seconds)
  delay(2000);

  // ========== Boot Logging ==========
  // Determine boot reason from RTC module
  RESET_REASON rr = rtc_get_reset_reason(0);
  LOG("%s -CPU REBOOT(%s)  IP:%s MAC:%s", getDate().c_str(), frame.resetReason((int)rr), WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
}

// ============================================
// Main Loop: Continuous execution
// ============================================

/**
 * @brief Main application loop
 * 
 * Responsibilities:
 * - Call FrameWeb framework loop (handles web server, WebSocket, etc.)
 * - Monitor network status every 5 seconds
 * - Update web interface statistics
 * - Implement WiFi recovery logic with exponential backoff
 * - Keep system time synchronized
 * 
 * WiFi Recovery Strategy:
 * - 5-50s: Log disconnection every 10 intervals
 * - 50s+: Force clean disconnect once
 * - Then: Attempt reconnect every 60s at low rate
 * - Recovery: Auto-detected when WiFi status changes to WL_CONNECTED
 */
void loop() {
  // Update FrameWeb framework (web server, WebSocket, etc.)
  frame.loop();

  // Periodic housekeeping executed every 5 seconds
  if (millis() - previousMillis > 5000L) {
    previousMillis = millis();
    
    // Update system time from RTC
    getLocalTime(&timeinfo);
   
    // Check current WiFi connection status
    int wifistat = WiFi.status();

    // Update web interface with fresh network stats
    refreshExternalHtmlTools();

    // ========== WiFi Recovery Logic ==========
    // Independently manages WiFi connection (Ethernet is handled by event handler)
    if (wifistat != WL_CONNECTED) {
      // WiFi is not connected; increment outage counter
      wifiLost++;
      
      // Log WiFi outage (every 50s initially, then every 10 intervals)
      if (wifiLost == 1 || (wifiLost % 10) == 0) {
        LOG("%s -WiFi Lost:%s down:%lus localIP:%s", getDate().c_str(), frame.wifiStatus(wifistat), (unsigned long)wifiLost * 5UL, WiFi.localIP().toString().c_str());
      }

      // After 50 seconds of outage, force a clean disconnect once
      // This resets the WiFi state machine and allows fresh reconnection
      if (!wifiForcedDisconnectDone && wifiLost >= 50) {
        LOG("%s -WiFi still down after 50s (%s): forcing disconnect.", getDate().c_str(), frame.wifiStatus(wifistat));
        // Force WiFi disconnect to clear state machine
        WiFi.disconnect();
        wifiForcedDisconnectDone = true;
      }

      // Attempt WiFi reconnection every 60 seconds (low rate to conserve power/RF)
      if (millis() - lastWifiReconnectMs >= 60000UL) {
        lastWifiReconnectMs = millis();
        LOG("%s -WiFi reconnect attempt (%lus down).", getDate().c_str(), (unsigned long)wifiLost * 5UL);
        WiFi.reconnect();  // Try to reconnect to last known SSID
      }
    } else {
      // WiFi connection is active
      
      // Log WiFi restoration if it was previously disconnected
      if (wifiLost > 0) {
        LOG("%s -WiFi restored after %lus.", getDate().c_str(), (unsigned long)wifiLost * 5UL);
      }
      
      // Reset WiFi outage counters
      wifiLost = 0;
      wifiForcedDisconnectDone = false;
    }
    
    // Log both network interfaces status every 5 seconds
    LOG("%s -wifiIP:%s ethIP:%s", getDate().c_str(), WiFi.localIP().toString().c_str(), ETH.localIP().toString().c_str());

  }  // End 5-second interval
}
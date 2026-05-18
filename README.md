# Esp32_PoE_Tst

PlatformIO test project for the Olimex ESP32-POE board — version `0.0.1`.

The active firmware (`main_all_udp.cpp`) implements a bidirectional TCP ↔ Serial bridge, primarily targeting Prusa/OctoPrint network-to-serial gateway use cases. It runs on top of the FrameWeb framework with dual Ethernet/Wi-Fi networking.

## Features

- Ethernet startup with LAN8720 PHY (primary interface)
- Wi-Fi station (fallback interface) with automatic recovery
- FrameWeb integration (web server, WebSocket, configuration management)
- WebSocket support for real-time status updates
- Single-client TCP server on port `5000`
- Bidirectional bridge between TCP and external UART (Serial1)
  - Serial1 RX: GPIO36, TX: GPIO4 (Olimex external connector)
- TCP bridge statistics displayed on the web interface (chars received / sent)
- FreeRTOS task for TCP bridging (priority 12, 4 KB stack)
- NTP time synchronization (pool.ntp.org, CET/CEST timezone)
- Boot logging with RTC reset reason
- Multiple test entry points in `src/`

## Project Layout

- `src/main_all_udp.cpp`: main application currently selected by `build_src_filter`
- `src/main_eth.cpp`: Ethernet-specific experiments
- `src/main_wifi.cpp`: Wi-Fi-specific experiments
- `src/main_bt.cpp`: Bluetooth-specific experiments
- `platformio.ini`: build configuration, ports, dependencies, and source selection

## Build

```bash
platformio run --environment esp32-poe
```

## Upload

```bash
platformio run --environment esp32-poe --target upload
```

## Serial Monitor

```bash
platformio device monitor --environment esp32-poe
```

## TCP Bridge

Once the firmware is running, connect a TCP client to port `5000` on the board IP address.

- TCP input is forwarded to Serial1 (external UART).
- Serial1 input is forwarded back to the active TCP client.
- Only one TCP client is accepted at a time. Additional connections are refused.
- A welcome banner is sent to the client on connection.

Simple terminal test:

```bash
nc <device-ip> 5000
```

Create a virtual serial port for applications like OctoPrint or PrusaSlicer:

```bash
sudo socat -d -d PTY,link=/dev/prusa,user=$USER,group=dialout,mode=777,raw,echo=0 TCP:<device-ip>:5000
# Then configure OctoPrint / PrusaSlicer to use /dev/prusa
```

## Wi-Fi Recovery

The firmware independently monitors the Wi-Fi station link every 5 seconds:

| Outage duration | Action |
|---|---|
| 5 s → 50 s | Log status every 50 s |
| 50 s | Force `WiFi.disconnect()` once to reset the state machine |
| Every 60 s after | Call `WiFi.reconnect()` |
| On restoration | Reset all outage counters |

Ethernet connectivity is handled separately through the ESP32 Arduino core event handler.

## Notes

- This repository is intended to store the project source only.
- Local build artifacts and editor files are excluded through `.gitignore`.
- `platformio.ini` uses `build_src_filter` to keep only one `main_*.cpp` entry point in the build.
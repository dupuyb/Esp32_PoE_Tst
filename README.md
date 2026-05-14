# Esp32_PoE_Tst

PlatformIO test project for the Olimex ESP32-POE board.

This project currently focuses on a TCP-to-serial bridge running on top of the local FrameWeb stack and the ESP32 Ethernet interface.

## Features

- Ethernet startup with LAN8720 PHY
- FrameWeb integration
- Single-client TCP server on port `5000`
- Bidirectional bridge between TCP and UART
- Periodic Wi-Fi recovery logic for the station side
- Multiple test entry points in `src/`

## Project Layout

- `src/main_all_udp.cpp`: main test application currently selected by `build_src_filter`
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

- TCP input is forwarded to the serial port.
- Serial input is forwarded back to the active TCP client.
- Only one TCP client is accepted at a time. Additional clients are refused while a session is active.

Example:

```bash
nc <device-ip> 5000
```

## Notes

- This repository is intended to store the project source only.
- Local build artifacts and editor files are excluded through `.gitignore`.
- `platformio.ini` uses `build_src_filter` to keep only one `main_*.cpp` entry point in the build.
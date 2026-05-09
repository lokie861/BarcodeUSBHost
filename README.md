# BarcodeUSBHost

**ESP32-S3 USB HID Barcode Scanner Library**  
*Self-contained · No external HID headers · FreeRTOS dual-core · Arduino IDE ready*

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Arduino](https://img.shields.io/badge/Arduino-Compatible-brightgreen)](https://www.arduino.cc/)
[![License](https://img.shields.io/badge/license-MIT-orange)](LICENSE)
[![Version](https://img.shields.io/badge/version-2.0.0-informational)](library.properties)

---

## Overview

`BarcodeUSBHost` lets you connect any USB HID barcode scanner directly to an **ESP32-S3** board and receive decoded barcode strings in your Arduino sketch — with zero blocking in the main loop.

It works by driving the ESP-IDF USB Host stack (`usb/usb_host.h`) directly, walking the USB configuration descriptor manually to find the HID interrupt endpoint, and decoding raw HID keyboard boot reports into ASCII. No third-party HID host libraries or external headers are needed.

---

## Features

| Feature | Detail |
|---|---|
| **No external dependencies** | Uses only `usb/usb_host.h` from the ESP32 Arduino core |
| **FreeRTOS dual-core** | USB polling on Core 0, callback dispatch on Core 1 |
| **Keycode diff engine** | Fires only on *new* keypresses — no duplicate characters |
| **Ring queue** | Buffers up to 16 complete barcodes; oldest overwritten if full |
| **Hot-plug support** | Handles scanner connect/disconnect events gracefully |
| **Two usage modes** | Async callback (`onScan`) or synchronous poll (`read`) |
| **No `delay()` in USB path** | Transfer resubmit is immediate, inside the ISR-like callback |

---

## Hardware Requirements

- **Board:** ESP32-S3 (any variant with USB OTG hardware — GPIO 19/20)
- **USB OTG port:** The native USB-OTG port of the ESP32-S3 (not the UART/programmer port)
- **Barcode scanner:** Any USB HID scanner that uses the keyboard boot protocol (virtually all commercial scanners)

### Wiring

```
Barcode Scanner (USB Type-A)        ESP32-S3
──────────────────────────────      ─────────────────
USB D−  (white)              →      GPIO 19
USB D+  (green)              →      GPIO 20
USB VBUS (red)               →      5V
USB GND  (black)             →      GND
```

> **Note:** For debug output, connect your PC to the **UART port** (CH340/CP2102) on your board — not the OTG port. The OTG port is fully occupied by the scanner.

---

## Arduino IDE Board Settings

All settings below are **critical**. Wrong settings = no USB communication.

| Setting | Value |
|---|---|
| **Board** | ESP32S3 Dev Module |
| **USB CDC On Boot** | **Disabled** |
| **USB Mode** | **USB-OTG** |
| **Upload Mode** | UART0 |
| **CPU Frequency** | 240 MHz |

---

## Installation

### Option 1 — ZIP Install (Recommended)

1. Download the repository as a `.zip` file (GitHub → **Code → Download ZIP**)
2. Open Arduino IDE → **Sketch → Include Library → Add .ZIP Library**
3. Select the downloaded `BarcodeUSBHost.zip`

### Option 2 — Manual

Clone or copy the library folder into your Arduino libraries directory:

```
<Arduino sketchbook>/libraries/BarcodeUSBHost/
├── BarcodeUSBHost.h
├── BarcodeUSBHost.cpp
├── library.properties
└── examples/
    └── BasicScan/
        └── BasicScan.ino
```

---

## Quick Start

```cpp
#include "BarcodeUSBHost.h"

BarcodeUSBHost scanner;

void onBarcode(const char *barcode) {
  // Fires on Core 1 — safe to use Serial here
  Serial.print("[SCAN] ");
  Serial.println(barcode);
}

void setup() {
  Serial.begin(115200);   // UART serial — NOT USB CDC

  scanner.onScan(onBarcode);  // Register callback
  scanner.begin();            // Start USB host tasks
}

void loop() {
  // Your application logic — USB runs independently on Core 0
  delay(100);
}
```

---

## API Reference

### `void begin()`
Installs the USB host stack and starts three FreeRTOS tasks. Call once in `setup()`.

```cpp
scanner.begin();
```

---

### `void onScan(BarcodeCallback cb)`
Registers a callback function invoked on **Core 1** whenever a complete barcode is received (terminated by Enter/CR from the scanner).

```cpp
void myCallback(const char *barcode) {
  Serial.println(barcode);
}

scanner.onScan(myCallback);
```

> The callback fires asynchronously. Avoid long-blocking operations inside it.

---

### `bool read(char *outBuf, size_t bufLen)`
Non-blocking poll. Returns `true` and copies the next barcode into `outBuf` if one is available in the internal queue. Returns `false` immediately if the queue is empty.

```cpp
char buf[256];
if (scanner.read(buf, sizeof(buf))) {
  Serial.println(buf);
}
```

Use this in `loop()` if you prefer polling over callbacks.

---

### `bool isConnected()`
Returns `true` if a scanner is currently plugged in and the HID interface has been claimed successfully.

```cpp
if (scanner.isConnected()) {
  // Scanner is live
}
```

---

## How It Works

```
USB Interrupt IN Transfer (Core 0)
        │
        ▼
  _xferCallback()
        │
        ▼
  _processHIDReport()
  ┌─────────────────────────────────────────────┐
  │  1. Extract modifier byte (shift state)     │
  │  2. Diff 6-key array vs. last report        │
  │  3. Map new keycodes → ASCII via keymap     │
  │  4. Accumulate chars into barcode buffer    │
  │  5. CR/LF → push complete string to queue  │
  └─────────────────────────────────────────────┘
        │
        ▼
  FreeRTOS Queue (16 slots × 256 bytes)
        │
        ▼
  callbackTask (Core 1) → onScan() callback
```

### FreeRTOS Task Layout

| Task | Core | Priority | Stack | Purpose |
|---|---|---|---|---|
| `usb_lib` | 0 | 5 | 4 KB | Drives `usb_host_lib_handle_events` |
| `usb_client` | 0 | 5 | 4 KB | Handles connect/disconnect events, resubmits transfers |
| `barcode_cb` | 1 | 2 | 2 KB | Dispatches user `onScan` callback |

---

## Configuration

Defaults are defined at the top of `BarcodeUSBHost.h` and can be changed before including the header:

```cpp
#define BARCODE_MAX_LEN      256   // Max barcode string length (bytes)
#define BARCODE_QUEUE_DEPTH  16    // Ring queue depth (number of barcodes)
#define USB_HOST_TASK_PRIO   5     // FreeRTOS priority for USB lib task
#define USB_CLIENT_TASK_PRIO 5     // FreeRTOS priority for USB client task
#define CALLBACK_TASK_PRIO   2     // FreeRTOS priority for callback task
```

---

## Compatibility

| Item | Requirement |
|---|---|
| **SoC** | ESP32-S3 only (USB OTG hardware required) |
| **Arduino Core** | esp32 Arduino core (any recent version) |
| **Scanners** | Any USB HID keyboard-class scanner (boot protocol) |
| **IDE** | Arduino IDE 1.8+ or 2.x |

> ESP32, ESP32-S2, ESP32-C3, and other variants are **not supported** — they do not have the required USB OTG host hardware.

---

## Contributing

Pull requests and issues are welcome. When reporting a bug, please include:
- ESP32-S3 board variant and Arduino core version
- Scanner make and model
- Serial output from the UART port

---

*Built for industrial and embedded applications where a dedicated microcontroller handles scanner I/O independently from the main application processor.*

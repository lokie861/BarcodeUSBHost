/*
  ════════════════════════════════════════════════
  BarcodeUSBHost — BasicScan Example
  ESP32-S3 USB HID Barcode Scanner
  ════════════════════════════════════════════════

  INSTALL:
    Arduino IDE → Sketch → Include Library → Add .ZIP Library
    Select BarcodeUSBHost.zip

  BOARD SETTINGS (Tools menu) — ALL ARE CRITICAL:
    Board            → ESP32S3 Dev Module
    USB CDC On Boot  → Disabled
    USB Mode         → USB-OTG
    Upload Mode      → UART0
    CPU Frequency    → 240MHz

  WIRING:
    Scanner USB  →  ESP32-S3 OTG port (GPIO19 D−, GPIO20 D+)
    PC Serial    →  ESP32-S3 UART port (built-in CH340/CP2102)
  ════════════════════════════════════════════════
*/

#include "BarcodeUSBHost.h"

BarcodeUSBHost scanner;

void onBarcode(const char *barcode) {
  // This fires on Core 1 — never blocks USB polling
  Serial.print("[SCAN] ");
  Serial.println(barcode);
}

void setup() {
  // UART serial — NOT USB CDC
  Serial.begin(115200);
  delay(2000);

  Serial.println("====================================");
  Serial.println("  BarcodeUSBHost Library v1.0       ");
  Serial.println("====================================");

  scanner.onScan(onBarcode);  // Register callback
  scanner.begin();            // Start USB host tasks
}

void loop() {
  // loop() is free — USB runs on dedicated FreeRTOS tasks
  // You can do other work here, scan detection is automatic

  // Optional: check connection status
  static bool lastConn = false;
  bool conn = scanner.isConnected();
  if (conn != lastConn) {
    lastConn = conn;
    // Already printed by library, but you can add custom logic:
    // if (conn) digitalWrite(LED, HIGH);
  }

  delay(100);
}

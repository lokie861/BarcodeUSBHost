#pragma once
/*
  BarcodeUSBHost.h
  ─────────────────────────────────────────────────────────────────
  Self-contained ESP32-S3 USB HID Barcode Scanner Library
  
  NO external HID headers required — works with any ESP32 Arduino
  core version. All HID parsing done manually from raw USB reports.

  Features:
    • FreeRTOS dual-core: USB on Core 0, callback on Core 1
    • Keycode diff — only fires on NEW keypresses, never duplicates
    • Ring queue — buffers 16 full barcodes, nothing dropped
    • No delay() in USB hot path
    • Callback + manual poll modes
  ─────────────────────────────────────────────────────────────────
*/

#ifndef BARCODE_USB_HOST_H
#define BARCODE_USB_HOST_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"   // Only stable header we need

// ── Config ────────────────────────────────────────────────────────
#define BARCODE_MAX_LEN      256
#define BARCODE_QUEUE_DEPTH  16
#define USB_HOST_TASK_PRIO   5
#define USB_CLIENT_TASK_PRIO 5
#define CALLBACK_TASK_PRIO   2

// ── Callback type ─────────────────────────────────────────────────
typedef void (*BarcodeCallback)(const char *barcode);

// ── Class ─────────────────────────────────────────────────────────
class BarcodeUSBHost {
public:
  BarcodeUSBHost();

  // Start USB host + FreeRTOS tasks. Call once in setup().
  void begin();

  // Register callback — fires on Core 1 with complete barcode string
  void onScan(BarcodeCallback cb);

  // Non-blocking poll — returns true if a barcode was read into outBuf
  bool read(char *outBuf, size_t bufLen);

  // Returns true if a scanner is plugged in
  bool isConnected();

  // ── Internal — do not call ──────────────────────────────────────
  void _usbLibTask();
  void _clientTask();
  void _processHIDReport(const uint8_t *data, size_t len);
  void _onDeviceConnected(uint8_t dev_addr);
  void _onDeviceGone();

  // Public so static FreeRTOS task functions can access them
  QueueHandle_t   _scanQueue;
  BarcodeCallback _callback;

private:
  // ── ASCII keymaps ───────────────────────────────────────────────
  static const uint8_t _keymap[128];
  static const uint8_t _keymapShift[128];
  uint8_t _ascii(uint8_t keycode, bool shift);

  // ── Barcode buffer ──────────────────────────────────────────────
  char     _buf[BARCODE_MAX_LEN];
  uint16_t _bufIdx;

  // ── HID keycode diff ────────────────────────────────────────────
  uint8_t  _lastKeycodes[6];
  uint8_t  _lastModifier;

  // ── USB handles ─────────────────────────────────────────────────
  usb_host_client_handle_t _clientHandle;
  usb_device_handle_t      _devHandle;
  uint8_t                  _intfNum;
  uint8_t                  _epAddr;
  uint16_t                 _epMPS;        // max packet size
  usb_transfer_t          *_xfer;

  // ── State ───────────────────────────────────────────────────────
  volatile bool _connected;
  volatile bool _claimDone;

  // ── FreeRTOS ────────────────────────────────────────────────────
  SemaphoreHandle_t _mutex;

  // ── User callback ───────────────────────────────────────────────

  // ── Internal helpers ─────────────────────────────────────────────
  void _submitTransfer();
  static void _xferCallback(usb_transfer_t *xfer);
};

#endif

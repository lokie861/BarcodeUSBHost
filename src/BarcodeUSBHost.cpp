/*
  BarcodeUSBHost.cpp
  ─────────────────────────────────────────────────────────────────────────────
  Self-contained USB HID barcode scanner library for ESP32-S3.

  How it works (no hid_host.h needed):
  ┌─────────────────────────────────────────────────────────────┐
  │  1. usb_host_install()  — init USB host stack               │
  │  2. usb_host_client_register() — register as USB client     │
  │  3. Wait for USB_HOST_CLIENT_EVENT_NEW_DEV                  │
  │  4. usb_host_device_open() + get config descriptor          │
  │  5. Find HID interface + interrupt IN endpoint              │
  │  6. usb_host_interface_claim()                              │
  │  7. Submit interrupt transfer — fires _xferCallback()       │
  │  8. _xferCallback() → _processHIDReport() → queue → user   │
  └─────────────────────────────────────────────────────────────┘

  FreeRTOS layout:
    Core 0: _usbLibTask  (priority 5) — usb_host_lib_handle_events
    Core 0: _clientTask  (priority 5) — client events + xfer resubmit
    Core 1: callbackTask (priority 2) — fires user onScan() callback
  ─────────────────────────────────────────────────────────────────────────────
*/

#include "BarcodeUSBHost.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BarcodeUSBHost";

// Singleton for static callbacks
static BarcodeUSBHost *_inst = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  HID Keycode → ASCII tables
//  Index = USB HID keycode value (0x00–0x7F)
// ─────────────────────────────────────────────────────────────────────────────
const uint8_t BarcodeUSBHost::_keymap[128] = {
  // 0x00  0x01  0x02  0x03  0x04  0x05  0x06  0x07
     0,    0,    0,    0,   'a', 'b',  'c',  'd',   // 8
  // 0x08  0x09  0x0A  0x0B  0x0C  0x0D  0x0E  0x0F
    'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l',  // 16
  // 0x10  0x11  0x12  0x13  0x14  0x15  0x16  0x17
    'm',  'n',  'o',  'p',  'q',  'r',  's',  't',  // 24
  // 0x18  0x19  0x1A  0x1B  0x1C  0x1D  0x1E  0x1F
    'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2',  // 32
  // 0x20  0x21  0x22  0x23  0x24  0x25  0x26  0x27
    '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  // 40
  // 0x28  0x29  0x2A  0x2B  0x2C  0x2D  0x2E  0x2F
   '\r', '\x1b','\b', '\t', ' ',  '-',  '=',  '[',  // 48
  // 0x30  0x31  0x32  0x33  0x34  0x35  0x36  0x37
   ']',  '\\',   0,   ';', '\'',  '`',  ',',  '.',  // 56
  // 0x38  0x39  0x3A  0x3B  0x3C  0x3D  0x3E  0x3F
    '/',   0,    0,    0,    0,    0,    0,    0,    // 64
  // 0x40..0x47
     0,    0,    0,    0,    0,    0,    0,    0,    // 72
  // 0x48..0x4F
     0,    0,    0,    0,    0,    0,    0,    0,    // 80
  // 0x50..0x57
     0,    0,    0,    0,    0,    0,    0,    0,    // 88
  // 0x58..0x5F
     0,    0,    0,    0,    0,    0,    0,    0,    // 96
  // 0x60..0x67
     0,    0,    0,    0,    0,    0,    0,    0,    // 104
  // 0x68..0x6F
     0,    0,    0,    0,    0,    0,    0,    0,    // 112
  // 0x70..0x77
     0,    0,    0,    0,    0,    0,    0,    0,    // 120
  // 0x78..0x7F
     0,    0,    0,    0,    0,    0,    0,    0     // 128
};

const uint8_t BarcodeUSBHost::_keymapShift[128] = {
  // 0x00  0x01  0x02  0x03  0x04  0x05  0x06  0x07
     0,    0,    0,    0,   'A', 'B',  'C',  'D',   // 8
  // 0x08  0x09  0x0A  0x0B  0x0C  0x0D  0x0E  0x0F
    'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',  // 16
  // 0x10  0x11  0x12  0x13  0x14  0x15  0x16  0x17
    'M',  'N',  'O',  'P',  'Q',  'R',  'S',  'T',  // 24
  // 0x18  0x19  0x1A  0x1B  0x1C  0x1D  0x1E  0x1F
    'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@',  // 32
  // 0x20  0x21  0x22  0x23  0x24  0x25  0x26  0x27
    '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',  // 40
  // 0x28  0x29  0x2A  0x2B  0x2C  0x2D  0x2E  0x2F
   '\r', '\x1b','\b', '\t', ' ',  '_',  '+',  '{',  // 48
  // 0x30  0x31  0x32  0x33  0x34  0x35  0x36  0x37
    '}',  '|',   0,   ':',  '"',  '~',  '<',  '>',  // 56
  // 0x38  0x39  0x3A  0x3B  0x3C  0x3D  0x3E  0x3F
    '?',   0,    0,    0,    0,    0,    0,    0,    // 64
  // 0x40..0x47
     0,    0,    0,    0,    0,    0,    0,    0,    // 72
  // 0x48..0x4F
     0,    0,    0,    0,    0,    0,    0,    0,    // 80
  // 0x50..0x57
     0,    0,    0,    0,    0,    0,    0,    0,    // 88
  // 0x58..0x5F
     0,    0,    0,    0,    0,    0,    0,    0,    // 96
  // 0x60..0x67
     0,    0,    0,    0,    0,    0,    0,    0,    // 104
  // 0x68..0x6F
     0,    0,    0,    0,    0,    0,    0,    0,    // 112
  // 0x70..0x77
     0,    0,    0,    0,    0,    0,    0,    0,    // 120
  // 0x78..0x7F
     0,    0,    0,    0,    0,    0,    0,    0     // 128
};

// ─────────────────────────────────────────────────────────────────────────────
//  Static task wrapper functions
// ─────────────────────────────────────────────────────────────────────────────
static void _usbLibTaskFn(void *arg) {
  if (_inst) _inst->_usbLibTask();
  vTaskDelete(NULL);
}

static void _clientTaskFn(void *arg) {
  if (_inst) _inst->_clientTask();
  vTaskDelete(NULL);
}

static void _callbackTaskFn(void *arg) {
  char buf[BARCODE_MAX_LEN];
  while (true) {
    if (xQueueReceive(_inst->_scanQueue, buf, portMAX_DELAY) == pdTRUE) {
      if (_inst && _inst->_callback) {
        _inst->_callback(buf);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────
BarcodeUSBHost::BarcodeUSBHost() {
  _inst         = this;
  _bufIdx       = 0;
  _connected    = false;
  _claimDone    = false;
  _callback     = nullptr;
  _clientHandle = nullptr;
  _devHandle    = nullptr;
  _xfer         = nullptr;
  _intfNum      = 0;
  _epAddr       = 0;
  _epMPS        = 8;
  _lastModifier = 0;
  memset(_buf,          0, sizeof(_buf));
  memset(_lastKeycodes, 0, sizeof(_lastKeycodes));
}

// ─────────────────────────────────────────────────────────────────────────────
//  begin()
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::begin() {
  _scanQueue = xQueueCreate(BARCODE_QUEUE_DEPTH, BARCODE_MAX_LEN);
  _mutex     = xSemaphoreCreateMutex();

  // Install USB host library
  const usb_host_config_t cfg = {
    .skip_phy_setup = false,
    .intr_flags     = ESP_INTR_FLAG_LEVEL1,
  };
  ESP_ERROR_CHECK(usb_host_install(&cfg));

  // USB lib event task — Core 0
  xTaskCreatePinnedToCore(_usbLibTaskFn, "usb_lib",
    4096, nullptr, USB_HOST_TASK_PRIO, nullptr, 0);

  // USB client task — Core 0
  xTaskCreatePinnedToCore(_clientTaskFn, "usb_client",
    4096, nullptr, USB_CLIENT_TASK_PRIO, nullptr, 0);

  // Callback dispatch task — Core 1 (never blocks USB)
  xTaskCreatePinnedToCore(_callbackTaskFn, "barcode_cb",
    2048, nullptr, CALLBACK_TASK_PRIO, nullptr, 1);

  Serial.println("[BarcodeUSBHost] Started — waiting for scanner...");
}

// ─────────────────────────────────────────────────────────────────────────────
//  USB library event task (must run to keep host stack alive)
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::_usbLibTask() {
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  USB client task — device connect/disconnect + transfer resubmit
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::_clientTask() {
  const usb_host_client_config_t clientCfg = {
    .is_synchronous    = false,
    .max_num_event_msg = 5,
    .async             = {
      .client_event_callback = [](const usb_host_client_event_msg_t *msg, void *arg) {
        BarcodeUSBHost *self = (BarcodeUSBHost *)arg;
        if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
          self->_onDeviceConnected(msg->new_dev.address);
        } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
          self->_onDeviceGone();
        }
      },
      .callback_arg = this,
    },
  };

  ESP_ERROR_CHECK(usb_host_client_register(&clientCfg, &_clientHandle));

  while (true) {
    usb_host_client_handle_events(_clientHandle, portMAX_DELAY);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Device connected — find HID keyboard interface and claim it
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::_onDeviceConnected(uint8_t dev_addr) {
  ESP_LOGI(TAG, "Device connected at address %d", dev_addr);

  // Open the device
  if (usb_host_device_open(_clientHandle, dev_addr, &_devHandle) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open device");
    return;
  }

  // Get config descriptor
  const usb_config_desc_t *cfg_desc;
  usb_host_get_active_config_descriptor(_devHandle, &cfg_desc);

  // Walk descriptors to find HID keyboard interface + interrupt IN endpoint
  const uint8_t *p   = (const uint8_t *)cfg_desc;
  const uint8_t *end = p + cfg_desc->wTotalLength;
  bool foundHID      = false;

  while (p < end) {
    uint8_t len  = p[0];
    uint8_t type = p[1];
    if (len == 0) break;

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      // Interface descriptor
      // bInterfaceClass=0x03 (HID), bInterfaceSubClass=0x01 (Boot), bInterfaceProtocol=0x01 (Keyboard)
      if (p[5] == 0x03) {  // HID class
        _intfNum = p[2];
        foundHID = true;
        ESP_LOGI(TAG, "Found HID interface %d (subclass=0x%02x, proto=0x%02x)",
                 _intfNum, p[6], p[7]);
      }
    }

    if (foundHID && type == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      // Endpoint descriptor — find interrupt IN
      uint8_t ep_addr  = p[2];
      uint8_t ep_attr  = p[3];
      uint16_t ep_mps  = p[4] | (p[5] << 8);

      bool isIN        = (ep_addr & 0x80) != 0;
      bool isInterrupt = (ep_attr & 0x03) == 0x03;

      if (isIN && isInterrupt) {
        _epAddr = ep_addr;
        _epMPS  = ep_mps;
        ESP_LOGI(TAG, "Found interrupt IN endpoint 0x%02x, MPS=%d", _epAddr, _epMPS);
        break;
      }
    }

    p += len;
  }

  if (!foundHID || _epAddr == 0) {
    ESP_LOGW(TAG, "No HID keyboard endpoint found");
    usb_host_device_close(_clientHandle, _devHandle);
    _devHandle = nullptr;
    return;
  }

  // Claim the interface
  if (usb_host_interface_claim(_clientHandle, _devHandle, _intfNum, 0) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to claim interface");
    usb_host_device_close(_clientHandle, _devHandle);
    _devHandle = nullptr;
    return;
  }

  // Allocate transfer
  usb_host_transfer_alloc(sizeof(uint8_t) * _epMPS, 0, &_xfer);
  _xfer->device_handle = _devHandle;
  _xfer->bEndpointAddress = _epAddr;
  _xfer->callback = _xferCallback;
  _xfer->context  = this;
  _xfer->num_bytes = _epMPS;
  _xfer->timeout_ms = 0; // No timeout for interrupt transfers

  _connected = true;
  Serial.println("[BarcodeUSBHost] Scanner connected!");

  // Start polling
  _submitTransfer();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Device disconnected
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::_onDeviceGone() {
  ESP_LOGI(TAG, "Device disconnected");
  _connected = false;

  if (_xfer) {
    usb_host_transfer_free(_xfer);
    _xfer = nullptr;
  }
  if (_devHandle) {
    usb_host_interface_release(_clientHandle, _devHandle, _intfNum);
    usb_host_device_close(_clientHandle, _devHandle);
    _devHandle = nullptr;
  }

  // Reset state
  _bufIdx = 0;
  memset(_buf,          0, sizeof(_buf));
  memset(_lastKeycodes, 0, sizeof(_lastKeycodes));
  _lastModifier = 0;
  _epAddr       = 0;
  _claimDone    = false;

  Serial.println("[BarcodeUSBHost] Scanner disconnected!");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Submit next interrupt IN transfer
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::_submitTransfer() {
  if (!_xfer || !_devHandle) return;
  esp_err_t err = usb_host_transfer_submit(_xfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Transfer submit failed: %s", esp_err_to_name(err));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Transfer complete callback — called from USB host task context
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::_xferCallback(usb_transfer_t *xfer) {
  BarcodeUSBHost *self = (BarcodeUSBHost *)xfer->context;
  if (!self || !self->_connected) return;

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    self->_processHIDReport(xfer->data_buffer, xfer->actual_num_bytes);
  }

  // Resubmit immediately — this is the key: no delay, continuous polling
  self->_submitTransfer();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Process one HID keyboard boot report (8 bytes)
//  Byte 0: modifier
//  Byte 1: reserved
//  Bytes 2–7: up to 6 simultaneous keycodes
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::_processHIDReport(const uint8_t *data, size_t len) {
  if (len < 3) return;

  uint8_t modifier     = data[0];
  const uint8_t *codes = &data[2];  // keycodes start at byte 2
  bool shift           = (modifier & 0x22) != 0; // left/right shift

  for (int i = 0; i < 6; i++) {
    uint8_t kc = (i < (int)(len - 2)) ? codes[i] : 0;
    if (kc == 0x00 || kc == 0x01) continue; // 0=none, 1=rollover

    // ── KEY DIFF: skip keycodes already present in last report ────
    bool held = false;
    for (int j = 0; j < 6; j++) {
      if (_lastKeycodes[j] == kc) { held = true; break; }
    }
    if (held) continue;

    // ── Convert to ASCII ──────────────────────────────────────────
    uint8_t ascii = _ascii(kc, shift);
    if (ascii == 0) continue;

    // ── End of barcode (Enter/CR/LF) ──────────────────────────────
    if (ascii == '\r' || ascii == '\n') {
      if (_bufIdx > 0) {
        _buf[_bufIdx] = '\0';
        // Push to queue — non-blocking, drop oldest if full
        if (xQueueSend(_scanQueue, _buf, 0) != pdTRUE) {
          char tmp[BARCODE_MAX_LEN];
          xQueueReceive(_scanQueue, tmp, 0); // discard oldest
          xQueueSend(_scanQueue, _buf, 0);
        }
        _bufIdx = 0;
        memset(_buf, 0, sizeof(_buf));
      }
      continue;
    }

    // ── Store printable character ─────────────────────────────────
    if (ascii >= 0x20 && ascii <= 0x7E) {
      if (_bufIdx < BARCODE_MAX_LEN - 1) {
        _buf[_bufIdx++] = (char)ascii;
      }
    }
  }

  // Save this report for next diff
  memcpy(_lastKeycodes, codes, 6);
  _lastModifier = modifier;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ASCII lookup
// ─────────────────────────────────────────────────────────────────────────────
uint8_t BarcodeUSBHost::_ascii(uint8_t keycode, bool shift) {
  if (keycode >= 128) return 0;
  return shift ? _keymapShift[keycode] : _keymap[keycode];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────
void BarcodeUSBHost::onScan(BarcodeCallback cb) {
  _callback = cb;
}

bool BarcodeUSBHost::read(char *outBuf, size_t bufLen) {
  char tmp[BARCODE_MAX_LEN];
  if (xQueueReceive(_scanQueue, tmp, 0) == pdTRUE) {
    strncpy(outBuf, tmp, bufLen - 1);
    outBuf[bufLen - 1] = '\0';
    return true;
  }
  return false;
}

bool BarcodeUSBHost::isConnected() {
  return _connected;
}

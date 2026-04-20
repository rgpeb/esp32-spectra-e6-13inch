#include "DisplayAdapter.h"

// ============================================================
// DisplayAdapter: bridges Adafruit_GFX drawing into a PSRAM
// framebuffer, then flushes it to the 13.3" Spectra 6 panel
// via the manufacturer's QSPI driver (dual driver IC protocol).
// ============================================================

// Frame buffer size: each byte holds 2 pixels (4-bit nibbles)
static const size_t FRAME_BUFFER_SIZE =
    (EPD_NATIVE_WIDTH * EPD_NATIVE_HEIGHT) / 2; // 960000 bytes

namespace {
uint8_t getPackedPixel(const uint8_t *buffer, uint16_t x, uint16_t y) {
  const size_t index = ((size_t)y * EPD_NATIVE_WIDTH + x) / 2;
  if ((x & 1) == 0) {
    return (buffer[index] >> 4) & 0x0F;
  }
  return buffer[index] & 0x0F;
}

void setPackedPixel(uint8_t *buffer, uint16_t x, uint16_t y, uint8_t color) {
  const size_t index = ((size_t)y * EPD_NATIVE_WIDTH + x) / 2;
  if ((x & 1) == 0) {
    buffer[index] = (buffer[index] & 0x0F) | ((color & 0x0F) << 4);
  } else {
    buffer[index] = (buffer[index] & 0xF0) | (color & 0x0F);
  }
}
} // namespace

DisplayAdapter::DisplayAdapter()
    : Adafruit_GFX(EPD_NATIVE_WIDTH, EPD_NATIVE_HEIGHT), _frameBuffer(nullptr),
      _initialized(false) {}

void DisplayAdapter::init(uint32_t serial_diag_bitrate) {
  if (!_initialized) {
    // Allocate framebuffer in PSRAM
    _frameBuffer = (uint8_t *)ps_malloc(FRAME_BUFFER_SIZE);
    if (!_frameBuffer) {
      Serial.println(
          "FATAL: Failed to allocate PSRAM framebuffer for 13.3\" display!");
      return;
    }
    // Fill with white
    memset(_frameBuffer, (WHITE << 4) | WHITE, FRAME_BUFFER_SIZE);

    printf("Initializing Good-Display ESP32-133C02 QSPI hardware... \r\n");

    // Initialize GPIO and SPI bus using the manufacturer's driver
    initialGpio();
    initialSpi();
    setGpioLevel(LOAD_SW, GPIO_HIGH);
    epdHardwareReset();
    setPinCsAll(GPIO_HIGH);

    // Send display init sequence
    initEPD();

    _initialized = true;
    printf("Display initialized successfully. \r\n");
  } else {
    // Re-init the EPD registers (as the manufacturer does before each refresh)
    initEPD();
  }
}

void DisplayAdapter::setRotation(uint8_t r) { Adafruit_GFX::setRotation(r); }

void DisplayAdapter::setFullWindow() {
  // No-op for this driver; full-window is the default mode
}

void DisplayAdapter::fillScreen(uint16_t color) {
  if (!_frameBuffer)
    return;
  uint8_t packedColor = ((color & 0xFF) << 4) | (color & 0xFF);
  memset(_frameBuffer, packedColor, FRAME_BUFFER_SIZE);
}

void DisplayAdapter::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (!_frameBuffer)
    return;

  // Apply rotation (Adafruit_GFX gives us logical coordinates)
  int16_t t;
  switch (getRotation()) {
  case 0:
    break;
  case 1:
    t = x;
    x = EPD_NATIVE_WIDTH - 1 - y;
    y = t;
    break;
  case 2:
    x = EPD_NATIVE_WIDTH - 1 - x;
    y = EPD_NATIVE_HEIGHT - 1 - y;
    break;
  case 3:
    t = x;
    x = y;
    y = EPD_NATIVE_HEIGHT - 1 - t;
    break;
  }

  // Bounds check on physical coordinates
  if (x < 0 || x >= EPD_NATIVE_WIDTH || y < 0 || y >= EPD_NATIVE_HEIGHT)
    return;

  // Pack into the framebuffer (2 pixels per byte, high nibble = even x, low
  // nibble = odd x)
  size_t index = ((size_t)y * EPD_NATIVE_WIDTH + x) / 2;
  if (x % 2 == 0) {
    _frameBuffer[index] = (_frameBuffer[index] & 0x0F) | ((color & 0x0F) << 4);
  } else {
    _frameBuffer[index] = (_frameBuffer[index] & 0xF0) | (color & 0x0F);
  }
}

void DisplayAdapter::sendFrameBufferToDisplay() {
  if (!_frameBuffer)
    return;

  // The 13.3" display has TWO driver ICs, each handling half the width.
  // From the manufacturer's pic_display_test():
  //   Width  = EPD_NATIVE_WIDTH / 2 = 600 pixels per IC
  //   Width1 = 300 bytes per IC per row (600 pixels / 2 pixels per byte)
  //
  // CS0 (left half):  for each row, send bytes [0..Width1-1]
  // CS1 (right half): for each row, send bytes [Width1..Width-1]

  const unsigned int Width = EPD_NATIVE_WIDTH / 2; // 600 pixels per section
  const unsigned int Width1 = Width / 2; // 300 bytes per section per row
  const unsigned int Height = EPD_NATIVE_HEIGHT; // 1600 rows

  Serial.println("Sending framebuffer to display (dual-IC split)...");

  // --- Send left half (CS0) ---
  setPinCsAll(GPIO_HIGH);
  setPinCs(0, 0);
  writeEpdCommand(DTM);
  for (unsigned int row = 0; row < Height; row++) {
    writeEpdData(_frameBuffer + row * Width, Width1);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  setPinCsAll(GPIO_HIGH);

  // --- Send right half (CS1) ---
  setPinCs(1, 0);
  writeEpdCommand(DTM);
  for (unsigned int row = 0; row < Height; row++) {
    writeEpdData(_frameBuffer + row * Width + Width1, Width1);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  setPinCsAll(GPIO_HIGH);

  Serial.println("Framebuffer transfer complete.");
}


bool DisplayAdapter::loadNativeFrameBuffer(File &file, size_t expectedSize,
                                           uint8_t rotationTurnsCW) {
  if (!_frameBuffer) {
    Serial.println("Binary render failed: framebuffer not initialized");
    return false;
  }

  if (!file) {
    Serial.println("Binary render failed: file handle invalid");
    return false;
  }

  const size_t fileSize = file.size();
  if (fileSize != expectedSize) {
    Serial.printf("Binary render failed: unexpected file size %u (expected %u)\n",
                  (unsigned int)fileSize, (unsigned int)expectedSize);
    return false;
  }

  if (!file.seek(0)) {
    Serial.println("Binary render failed: unable to rewind file");
    return false;
  }

  size_t totalRead = 0;
  while (totalRead < expectedSize) {
    const size_t chunk = min((size_t)2048, expectedSize - totalRead);
    const size_t n = file.read(_frameBuffer + totalRead, chunk);
    if (n == 0) {
      Serial.printf("Binary render failed: short read at %u bytes\n",
                    (unsigned int)totalRead);
      return false;
    }
    totalRead += n;
  }

  Serial.printf("Binary framebuffer loaded: %u bytes\n", (unsigned int)totalRead);
  if (totalRead != expectedSize) {
    return false;
  }

  const uint8_t turns = rotationTurnsCW % 4;
  if (turns == 0) {
    return true;
  }
  if (turns == 1 || turns == 3) {
    Serial.println(
        "Binary render failed: 90/270 degree rotation unsupported for native binary (requires server-side remap).");
    return false;
  }

  uint8_t *rotated = (uint8_t *)ps_malloc(FRAME_BUFFER_SIZE);
  if (!rotated) {
    Serial.println("Binary render failed: rotation buffer allocation failed");
    return false;
  }

  memset(rotated, (WHITE << 4) | WHITE, FRAME_BUFFER_SIZE);

  for (uint16_t y = 0; y < EPD_NATIVE_HEIGHT; ++y) {
    for (uint16_t x = 0; x < EPD_NATIVE_WIDTH; ++x) {
      const uint8_t color = getPackedPixel(_frameBuffer, x, y);
      uint16_t dstX = x;
      uint16_t dstY = y;

      if (turns == 2) {
        dstX = EPD_NATIVE_WIDTH - 1 - x;
        dstY = EPD_NATIVE_HEIGHT - 1 - y;
      }

      if (dstX < EPD_NATIVE_WIDTH && dstY < EPD_NATIVE_HEIGHT) {
        setPackedPixel(rotated, dstX, dstY, color);
      }
    }
  }

  memcpy(_frameBuffer, rotated, FRAME_BUFFER_SIZE);
  free(rotated);
  Serial.printf("Binary framebuffer rotated: %u quarter-turn(s) CW\n",
                (unsigned int)turns);
  return true;
}

void DisplayAdapter::display(bool partial_update_mode) {
  if (partial_update_mode) {
    Serial.println(
        "DisplayAdapter: partial update requested, forcing full refresh.");
  }
  sendFrameBufferToDisplay();

  epdDisplay();
}

void DisplayAdapter::hibernate() {
  // Power off the display to save energy
  setPinCsAll(GPIO_LOW);
  writeEpd(POF, (unsigned char *)POF_V, sizeof(POF_V));
  checkBusyHigh();
  setPinCsAll(GPIO_HIGH);
  Serial.println("Display entered hibernate mode.");
}

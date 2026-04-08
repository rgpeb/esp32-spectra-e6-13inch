#include "ConfigurationScreen.h"

#include <memory>

#include "HardwareSerial.h"

ConfigurationScreen::ConfigurationScreen(DisplayType &display,
                                         const String &qrPayload,
                                         const String &titleText,
                                         const String &subtitleText)
    : display(display), qrPayload(qrPayload), titleText(titleText),
      subtitleText(subtitleText) {
  gfx.begin(display);
}

String ConfigurationScreen::buildWiFiPortalQrPayload(const String &portalUrl) {
  return portalUrl;
}

String ConfigurationScreen::buildPairingQrPayload(const String &pairingUrl) {
  return pairingUrl;
}

void ConfigurationScreen::drawQRCode(const String &payload, int x, int y,
                                     int scale) {
  const uint8_t qrCodeVersion10 = 10;
  uint8_t qrCodeDataBuffer[qrcode_getBufferSize(qrCodeVersion10)];
  QRCode qrCodeInstance;

  int qrGenerationResult =
      qrcode_initText(&qrCodeInstance, qrCodeDataBuffer, qrCodeVersion10,
                      ECC_MEDIUM, payload.c_str());

  if (qrGenerationResult != 0) {
    Serial.print("Failed to generate QR code, error: ");
    Serial.println(qrGenerationResult);
    return;
  }

  for (uint8_t qrModuleY = 0; qrModuleY < qrCodeInstance.size; qrModuleY++) {
    for (uint8_t qrModuleX = 0; qrModuleX < qrCodeInstance.size; qrModuleX++) {
      bool moduleIsBlack =
          qrcode_getModule(&qrCodeInstance, qrModuleX, qrModuleY);
      if (moduleIsBlack) {
        int rectX = x + (qrModuleX * scale);
        int rectY = y + (qrModuleY * scale);
        display.fillRect(rectX, rectY, scale, scale, GxEPD_BLACK);
      }
    }
  }
}

void ConfigurationScreen::render() {
  Serial.println("Displaying setup screen with QR code");

  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  Serial.printf("Display dimensions: %d x %d\n", display.width(),
                display.height());

  const int textLeftMargin = 40;
  const int lineSpacing = 55;
  const int qrCodeScale = 4;
  const int qrCodeModuleCount = 57;
  const int qrCodePixelSize = qrCodeModuleCount * qrCodeScale;
  const int qrCodeQuietZone = 20;

  int qrCodeX = display.width() - qrCodePixelSize - qrCodeQuietZone - 40;
  int qrCodeY = (display.height() - qrCodePixelSize) / 2;

  gfx.setFontMode(1);
  gfx.setBackgroundColor(GxEPD_BLUE);
  gfx.setForegroundColor(GxEPD_WHITE);

  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);

  display.fillRect(0, 0, display.width(), 80, GxEPD_BLUE);

  gfx.setFont(u8g2_font_open_iconic_embedded_4x_t);
  gfx.setCursor(textLeftMargin, 55);
  gfx.print((char)66);

  gfx.setFont(u8g2_font_fur17_tr);
  gfx.setCursor(textLeftMargin + 40, 50);
  gfx.print(titleText);

  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);

  int currentY = 140;

  gfx.setFont(u8g2_font_fur17_tr);
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print(subtitleText);
  currentY += lineSpacing;
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print("Scan the QR code with your phone.");
  currentY += lineSpacing;
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print("Follow the on-screen steps.");

  int qrBgX = qrCodeX - qrCodeQuietZone;
  int qrBgY = qrCodeY - qrCodeQuietZone;
  int qrBgSize = qrCodePixelSize + (2 * qrCodeQuietZone);

  display.fillRect(qrBgX - 5, qrBgY - 5, qrBgSize + 10, qrBgSize + 10,
                   GxEPD_RED);
  display.fillRect(qrBgX, qrBgY, qrBgSize, qrBgSize, GxEPD_WHITE);

  drawQRCode(qrPayload, qrCodeX, qrCodeY, qrCodeScale);

  display.display();
  display.hibernate();

  Serial.println("Setup screen rendered successfully");
}

int ConfigurationScreen::nextRefreshInSeconds() { return 600; }

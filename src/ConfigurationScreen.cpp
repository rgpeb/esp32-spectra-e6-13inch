#include "ConfigurationScreen.h"

#include <memory>

#include "HardwareSerial.h"

ConfigurationScreen::ConfigurationScreen(DisplayType &display,
                                         const String &qrPayload,
                                         const String &titleText,
                                         const String &subtitleText)
    : display(display), qrPayload(qrPayload), titleText(titleText),
      subtitleText(subtitleText), helperText("Scan with your phone to continue."),
      showQrCode(qrPayload.length() > 0) {
  gfx.begin(display);
}

String ConfigurationScreen::buildWiFiPortalQrPayload(const String &portalUrl) {
  return portalUrl;
}

String ConfigurationScreen::buildJoinWifiQrPayload(const String &ssid,
                                                   const String &password) {
  return "WIFI:T:WPA;S:" + ssid + ";P:" + password + ";;";
}

String ConfigurationScreen::buildPairingQrPayload(const String &pairingUrl) {
  return pairingUrl;
}

ConfigurationScreen ConfigurationScreen::createStatusScreen(
    DisplayType &display, const String &titleText, const String &subtitleText,
    const String &helperText) {
  ConfigurationScreen screen(display, "", titleText, subtitleText);
  screen.helperText = helperText;
  screen.showQrCode = false;
  return screen;
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
  Serial.println("Displaying setup screen");

  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  Serial.printf("Display dimensions: %d x %d\n", display.width(),
                display.height());

  const int textLeftMargin = 34;
  const int lineSpacing = 52;
  const int qrCodeScale = showQrCode ? 8 : 0;
  const int qrCodeModuleCount = 57;
  const int qrCodePixelSize = qrCodeModuleCount * qrCodeScale;
  const int qrCodeQuietZone = 22;
  const int maxTextWidth =
      showQrCode ? (display.width() / 2) - 12 : display.width() - 70;
  const int qrCodeX =
      showQrCode ? display.width() - qrCodePixelSize - qrCodeQuietZone - 36 : 0;
  const int qrCodeY = showQrCode ? (display.height() - qrCodePixelSize) / 2 : 0;

  gfx.setFontMode(1);
  gfx.setBackgroundColor(GxEPD_GREEN);
  gfx.setForegroundColor(GxEPD_WHITE);

  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);

  display.fillRect(0, 0, display.width(), 128, GxEPD_GREEN);

  gfx.setFont(u8g2_font_fur25_tr);
  gfx.setCursor(textLeftMargin, 86);
  gfx.print("Welcome");

  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);

  int currentY = 220;
  gfx.setFont(u8g2_font_fur25_tr);
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print(titleText);
  currentY += lineSpacing + 12;

  gfx.setFont(u8g2_font_helvB14_tr);
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print(
      subtitleText.substring(0, min((size_t)subtitleText.length(), (size_t)44)));
  currentY += lineSpacing;
  if (subtitleText.length() > 44) {
    gfx.setCursor(textLeftMargin, currentY);
    gfx.print(subtitleText.substring(44));
    currentY += lineSpacing;
  }
  gfx.setFont(u8g2_font_helvR14_tr);
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print(helperText.substring(0, min((size_t)helperText.length(), (size_t)46)));
  currentY += lineSpacing;
  if (helperText.length() > 46) {
    gfx.setCursor(textLeftMargin, currentY);
    gfx.print(helperText.substring(46));
  }

  if (showQrCode) {
    int qrBgX = qrCodeX - qrCodeQuietZone;
    int qrBgY = qrCodeY - qrCodeQuietZone;
    int qrBgSize = qrCodePixelSize + (2 * qrCodeQuietZone);

    display.drawRect(18, 160, maxTextWidth, 260, GxEPD_GREEN);
    display.fillRect(qrBgX, qrBgY, qrBgSize, qrBgSize, GxEPD_WHITE);
    display.drawRect(qrBgX - 4, qrBgY - 4, qrBgSize + 8, qrBgSize + 8, GxEPD_GREEN);
    drawQRCode(qrPayload, qrCodeX, qrCodeY, qrCodeScale);
  } else {
    display.drawRect(18, 160, display.width() - 36, 260, GxEPD_GREEN);
  }

  display.display();
  display.hibernate();

  Serial.println("Setup screen rendered successfully");
}

int ConfigurationScreen::nextRefreshInSeconds() { return 600; }

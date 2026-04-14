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
  Serial.println("Displaying setup screen with QR code");

  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  Serial.printf("Display dimensions: %d x %d\n", display.width(),
                display.height());

  const int textLeftMargin = 30;
  const int lineSpacing = 44;
  const int qrCodeScale = showQrCode ? 5 : 0;
  const int qrCodeModuleCount = 57;
  const int qrCodePixelSize = qrCodeModuleCount * qrCodeScale;
  const int qrCodeQuietZone = 20;
  const int maxTextWidth = showQrCode ? (display.width() / 2) - 10 : display.width() - 60;
  const int qrCodeX =
      showQrCode ? display.width() - qrCodePixelSize - qrCodeQuietZone - 28 : 0;
  const int qrCodeY = showQrCode ? (display.height() - qrCodePixelSize) / 2 : 0;

  gfx.setFontMode(1);
  gfx.setBackgroundColor(GxEPD_BLACK);
  gfx.setForegroundColor(GxEPD_WHITE);

  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);

  display.fillRect(0, 0, display.width(), 88, GxEPD_BLACK);

  gfx.setFont(u8g2_font_open_iconic_embedded_4x_t);
  gfx.setCursor(textLeftMargin, 55);
  gfx.print((char)66);

  gfx.setFont(u8g2_font_fur20_tr);
  gfx.setCursor(textLeftMargin + 40, 56);
  gfx.print("Welcome");

  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);

  int currentY = 138;
  gfx.setFont(u8g2_font_fur20_tr);
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print(titleText);
  currentY += lineSpacing + 4;

  gfx.setFont(u8g2_font_fur17_tr);
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print(subtitleText.substring(0, min((size_t)subtitleText.length(), (size_t)48)));
  currentY += lineSpacing;
  if (subtitleText.length() > 48) {
    gfx.setCursor(textLeftMargin, currentY);
    gfx.print(subtitleText.substring(48));
    currentY += lineSpacing;
  }
  gfx.setFont(u8g2_font_helvB14_tr);
  gfx.setCursor(textLeftMargin, currentY);
  gfx.print(helperText.substring(0, min((size_t)helperText.length(), (size_t)56)));
  currentY += lineSpacing - 6;
  if (helperText.length() > 56) {
    gfx.setCursor(textLeftMargin, currentY);
    gfx.print(helperText.substring(56));
  }

  if (showQrCode) {
    int qrBgX = qrCodeX - qrCodeQuietZone;
    int qrBgY = qrCodeY - qrCodeQuietZone;
    int qrBgSize = qrCodePixelSize + (2 * qrCodeQuietZone);

    display.drawRect(18, 102, maxTextWidth, 188, GxEPD_BLACK);
    display.fillRect(qrBgX, qrBgY, qrBgSize, qrBgSize, GxEPD_WHITE);
    display.drawRect(qrBgX - 3, qrBgY - 3, qrBgSize + 6, qrBgSize + 6, GxEPD_BLACK);
    drawQRCode(qrPayload, qrCodeX, qrCodeY, qrCodeScale);
  } else {
    display.drawRect(18, 102, display.width() - 36, 188, GxEPD_BLACK);
  }

  display.display();
  display.hibernate();

  Serial.println("Setup screen rendered successfully");
}

int ConfigurationScreen::nextRefreshInSeconds() { return 600; }

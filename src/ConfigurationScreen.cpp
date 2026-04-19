#include "ConfigurationScreen.h"

#include <memory>
#include <vector>

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

  const int screenWidth = display.width();
  const int screenHeight = display.height();
  const int outerMargin = 24;
  const int cardGap = 16;
  const int headerHeight = 116;
  const int maxQrCodeScale = 8;
  const int qrCodeModuleCount = 57;
  const int qrCodeQuietZone = 20;
  const int contentTop = headerHeight + 20;
  const int contentHeight = screenHeight - contentTop - outerMargin;
  const int leftColumnWidth = showQrCode ? (screenWidth * 57) / 100 : screenWidth - (2 * outerMargin);
  const int rightColumnX = outerMargin + leftColumnWidth + cardGap;
  const int rightColumnWidth = showQrCode ? screenWidth - rightColumnX - outerMargin : 0;
  const int qrAreaWidth = rightColumnWidth - 56;
  const int qrAreaHeight = contentHeight - 170;
  const int qrCodeScale =
      showQrCode ? max(3, min(maxQrCodeScale,
                              min(qrAreaWidth, qrAreaHeight) / qrCodeModuleCount))
                 : 0;
  const int qrCodePixelSize = qrCodeModuleCount * qrCodeScale;
  const int firstCardHeight = 236;
  const int secondCardY = contentTop + firstCardHeight + cardGap;
  const int secondCardHeight = contentTop + contentHeight - secondCardY;

  auto wrapText = [](const String& text, int maxCharsPerLine) {
    std::vector<String> lines;
    if (text.length() == 0) return lines;

    String remaining = text;
    while (remaining.length() > 0) {
      if (remaining.length() <= maxCharsPerLine) {
        lines.push_back(remaining);
        break;
      }

      int splitAt = maxCharsPerLine;
      while (splitAt > 0 && remaining.charAt(splitAt) != ' ') splitAt--;
      if (splitAt <= 0) splitAt = maxCharsPerLine;

      lines.push_back(remaining.substring(0, splitAt));
      remaining = remaining.substring(splitAt);
      remaining.trim();
    }
    return lines;
  };

  gfx.setFontMode(1);
  gfx.setBackgroundColor(GxEPD_GREEN);
  gfx.setForegroundColor(GxEPD_WHITE);

  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, screenWidth, headerHeight, GxEPD_GREEN);

  gfx.setFont(u8g2_font_fur25_tr);
  gfx.setCursor(outerMargin, 74);
  gfx.print("Welcome");

  gfx.setFont(u8g2_font_helvB14_tr);
  gfx.setCursor(outerMargin, 104);
  gfx.print("Frame Setup Portal");

  // Card 1: setup progress
  display.drawRect(outerMargin, contentTop, leftColumnWidth, firstCardHeight, GxEPD_GREEN);
  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);

  display.fillRect(outerMargin + 18, contentTop + 18, 250, 40, GxEPD_GREEN);
  gfx.setBackgroundColor(GxEPD_GREEN);
  gfx.setForegroundColor(GxEPD_WHITE);
  gfx.setFont(u8g2_font_helvB14_tr);
  gfx.setCursor(outerMargin + 30, contentTop + 46);
  gfx.print("SETUP PROGRESS");

  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);
  gfx.setFont(u8g2_font_helvB14_tr);
  gfx.setCursor(outerMargin + 18, contentTop + 90);
  gfx.print("1. Connect home WiFi");
  gfx.setCursor(outerMargin + 18, contentTop + 128);
  gfx.print("2. Connect account");
  gfx.setCursor(outerMargin + 18, contentTop + 166);
  gfx.print("3. Frame ready");

  // Card 2: active step details
  display.drawRect(outerMargin, secondCardY, leftColumnWidth, secondCardHeight, GxEPD_GREEN);
  const int subtitleWrapChars = max(28, (leftColumnWidth - 36) / 14);
  const int helperWrapChars = max(30, (leftColumnWidth - 36) / 13);
  int textY = secondCardY + 52;

  gfx.setFont(u8g2_font_fur20_tr);
  std::vector<String> titleLines = wrapText(titleText, max(18, subtitleWrapChars - 10));
  for (const String& line : titleLines) {
    gfx.setCursor(outerMargin + 18, textY);
    gfx.print(line);
    textY += 34;
  }

  textY += 10;
  gfx.setFont(u8g2_font_helvB14_tr);
  std::vector<String> subtitleLines = wrapText(subtitleText, subtitleWrapChars);
  for (const String& line : subtitleLines) {
    gfx.setCursor(outerMargin + 18, textY);
    gfx.print(line);
    textY += 30;
  }

  gfx.setFont(u8g2_font_helvR14_tr);
  std::vector<String> helperLines = wrapText(helperText, helperWrapChars);
  for (const String& line : helperLines) {
    gfx.setCursor(outerMargin + 18, textY + 6);
    gfx.print(line);
    textY += 28;
  }

  if (showQrCode) {
    int qrCardY = contentTop;
    int qrCardHeight = contentHeight;
    int qrBgSize = qrCodePixelSize + (2 * qrCodeQuietZone);
    int qrBgX = rightColumnX + (rightColumnWidth - qrBgSize) / 2;
    int qrBgY = qrCardY + 112;
    int qrCodeX = qrBgX + qrCodeQuietZone;
    int qrCodeY = qrBgY + qrCodeQuietZone;

    display.drawRect(rightColumnX, qrCardY, rightColumnWidth, qrCardHeight, GxEPD_GREEN);
    gfx.setFont(u8g2_font_helvB14_tr);
    gfx.setCursor(rightColumnX + 18, qrCardY + 50);
    gfx.print("Scan to continue");
    gfx.setFont(u8g2_font_helvR14_tr);
    gfx.setCursor(rightColumnX + 18, qrCardY + 84);
    gfx.print("Use your phone camera");

    display.fillRect(qrBgX, qrBgY, qrBgSize, qrBgSize, GxEPD_WHITE);
    display.drawRect(qrBgX - 4, qrBgY - 4, qrBgSize + 8, qrBgSize + 8, GxEPD_GREEN);
    drawQRCode(qrPayload, qrCodeX, qrCodeY, qrCodeScale);
  }

  display.display();
  display.hibernate();

  Serial.println("Setup screen rendered successfully");
}

int ConfigurationScreen::nextRefreshInSeconds() { return 600; }

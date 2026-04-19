#include "ConfigurationScreen.h"

#include <memory>
#include <vector>

#include "HardwareSerial.h"

ConfigurationScreen::ConfigurationScreen(DisplayType &display,
                                         const String &qrPayload,
                                         const String &titleText,
                                         const String &subtitleText,
                                         const std::vector<String> &timelineEntries,
                                         uint8_t setupStepSlot,
                                         bool appendOnlyMode)
    : display(display), qrPayload(qrPayload), titleText(titleText),
      subtitleText(subtitleText), helperText("Scan with your phone to continue."),
      timelineEntries(timelineEntries),
      setupStepSlot(setupStepSlot),
      appendOnlyMode(appendOnlyMode),
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
    const String &helperText, const std::vector<String> &timelineEntries,
    uint8_t setupStepSlot, bool appendOnlyMode) {
  ConfigurationScreen screen(display, "", titleText, subtitleText,
                             timelineEntries, setupStepSlot, appendOnlyMode);
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

void ConfigurationScreen::render() { renderWithCommit(true); }

void ConfigurationScreen::renderWithCommit(bool commitUpdate) {
  Serial.println("Displaying setup screen");

  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  Serial.printf("Display dimensions: %d x %d\n", display.width(),
                display.height());

  const int screenWidth = display.width();
  const int screenHeight = display.height();

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

  if (appendOnlyMode) {
    const int headerHeight = 138;
    const int stepAreaTop = 154;
    const int stepGap = 14;
    const int stepHeight = (screenHeight - stepAreaTop - 24 - (2 * stepGap)) / 3;
    const int stepY = stepAreaTop + (min((uint8_t)2, setupStepSlot) * (stepHeight + stepGap));
    const int stepX = 18;
    const int stepWidth = screenWidth - 36;

    if (setupStepSlot == 0) {
      display.setFullWindow();
      display.fillScreen(GxEPD_WHITE);
      display.fillRect(0, 0, screenWidth, headerHeight, GxEPD_GREEN);
      gfx.setFontMode(1);
      gfx.setBackgroundColor(GxEPD_GREEN);
      gfx.setForegroundColor(GxEPD_WHITE);
      gfx.setFont(u8g2_font_fur20_tr);
      gfx.setCursor(24, 66);
      gfx.print("Welcome");
      gfx.setFont(u8g2_font_helvB14_tr);
      gfx.setCursor(24, 106);
      gfx.print("Frame setup");

      const int statusBoxWidth = 360;
      const int statusBoxHeight = 104;
      const int statusBoxX = screenWidth - statusBoxWidth - 24;
      const int statusBoxY = 18;
      display.fillRect(statusBoxX, statusBoxY, statusBoxWidth, statusBoxHeight,
                       GxEPD_WHITE);
      display.drawRect(statusBoxX, statusBoxY, statusBoxWidth, statusBoxHeight,
                       GxEPD_GREEN);
      gfx.setBackgroundColor(GxEPD_WHITE);
      gfx.setForegroundColor(GxEPD_BLACK);
      gfx.setFont(u8g2_font_helvB12_tr);
      gfx.setCursor(statusBoxX + 16, statusBoxY + 30);
      gfx.print("1) Connect WiFi");
      gfx.setCursor(statusBoxX + 16, statusBoxY + 58);
      gfx.print("2) Open portal QR");
      gfx.setCursor(statusBoxX + 16, statusBoxY + 86);
      gfx.print("3) Link account");
    }

    display.drawRect(stepX, stepY, stepWidth, stepHeight, GxEPD_GREEN);
    gfx.setBackgroundColor(GxEPD_WHITE);
    gfx.setForegroundColor(GxEPD_BLACK);
    gfx.setFont(u8g2_font_helvB18_tr);
    gfx.setCursor(stepX + 18, stepY + 40);
    gfx.print(String("Step ") + String(min((uint8_t)3, (uint8_t)(setupStepSlot + 1))));
    gfx.setFont(u8g2_font_helvR14_tr);
    std::vector<String> titleLines = wrapText(titleText, 52);
    int textY = stepY + 78;
    for (const String &line : titleLines) {
      gfx.setCursor(stepX + 18, textY);
      gfx.print(line);
      textY += 26;
    }

    std::vector<String> subtitleLines = wrapText(subtitleText, 64);
    for (const String &line : subtitleLines) {
      gfx.setCursor(stepX + 18, textY);
      gfx.print(line);
      textY += 23;
    }

    if (showQrCode) {
      const int qrScale = 4;
      const int qrPixelSize = 57 * qrScale;
      const int qrQuietZone = 8;
      const int qrBgSize = qrPixelSize + (2 * qrQuietZone);
      const int qrBgX = stepX + stepWidth - qrBgSize - 16;
      const int qrBgY = stepY + 14;
      display.fillRect(qrBgX, qrBgY, qrBgSize, qrBgSize, GxEPD_WHITE);
      display.drawRect(qrBgX - 3, qrBgY - 3, qrBgSize + 6, qrBgSize + 6, GxEPD_GREEN);
      drawQRCode(qrPayload, qrBgX + qrQuietZone, qrBgY + qrQuietZone, qrScale);
    } else {
      std::vector<String> helperLines = wrapText(helperText, 64);
      for (const String &line : helperLines) {
        gfx.setCursor(stepX + 18, textY);
        gfx.print(line);
        textY += 23;
      }
    }

    if (commitUpdate) {
      display.display();
      display.hibernate();
      Serial.println("Setup screen rendered successfully");
    } else {
      Serial.println("Setup screen staged (display commit deferred)");
    }
    return;
  }

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
  const int helperWrapChars = max(30, (leftColumnWidth - 36) / 13);
  int textY = secondCardY + 52;

  std::vector<String> entries = timelineEntries;
  if (entries.empty()) {
    if (titleText.length() > 0) {
      entries.push_back(titleText);
    }
    if (subtitleText.length() > 0) {
      entries.push_back(subtitleText);
    }
    if (helperText.length() > 0) {
      entries.push_back(helperText);
    }
  }

  gfx.setFont(u8g2_font_helvB14_tr);
  gfx.setCursor(outerMargin + 18, textY);
  gfx.print("Stage updates");
  textY += 28;

  gfx.setFont(u8g2_font_helvR14_tr);
  for (size_t i = 0; i < entries.size(); ++i) {
    const String prefix = String(i + 1) + ". ";
    std::vector<String> wrapped = wrapText(entries[i], helperWrapChars - 4);
    bool firstLine = true;
    for (const String &line : wrapped) {
      gfx.setCursor(outerMargin + 18, textY);
      if (firstLine) {
        gfx.print(prefix + line);
        firstLine = false;
      } else {
        gfx.print("   " + line);
      }
      textY += 24;
      if (textY >= secondCardY + secondCardHeight - 18) {
        break;
      }
    }
    textY += 6;
    if (textY >= secondCardY + secondCardHeight - 18) {
      break;
    }
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

  if (commitUpdate) {
    display.display();
    display.hibernate();
    Serial.println("Setup screen rendered successfully");
  } else {
    Serial.println("Setup screen staged (display commit deferred)");
  }
}

int ConfigurationScreen::nextRefreshInSeconds() { return 600; }

#ifndef CONFIGURATION_SCREEN_H
#define CONFIGURATION_SCREEN_H

#include <U8g2_for_Adafruit_GFX.h>
#include <qrcode.h>

#include <memory>
#include <vector>

#include "ApplicationConfig.h"
#include "DisplayAdapter.h"
#include "Screen.h"

class ConfigurationScreen : public Screen {
 private:
  DisplayType& display;
  String qrPayload;
  String titleText;
  String subtitleText;
  String helperText;
  std::vector<String> timelineEntries;
  uint8_t setupStepSlot;
  bool appendOnlyMode;
  bool showQrCode;
  U8G2_FOR_ADAFRUIT_GFX gfx;

  void drawQRCode(const String& payload, int x, int y, int scale = 3);

 public:
  ConfigurationScreen(DisplayType& display, const String& qrPayload,
                      const String& titleText, const String& subtitleText,
                      const std::vector<String>& timelineEntries = {},
                      uint8_t setupStepSlot = 0, bool appendOnlyMode = false);
  static String buildJoinWifiQrPayload(const String& ssid,
                                       const String& password);
  static String buildWiFiPortalQrPayload(const String& portalUrl);
  static String buildPairingQrPayload(const String& pairingUrl);
  static ConfigurationScreen createStatusScreen(DisplayType& display,
                                                const String& titleText,
                                                const String& subtitleText,
                                                const String& helperText,
                                                const std::vector<String>& timelineEntries = {},
                                                uint8_t setupStepSlot = 0,
                                                bool appendOnlyMode = false);

  void render() override;
  void renderWithCommit(bool commitUpdate);
  int nextRefreshInSeconds() override;
};

#endif

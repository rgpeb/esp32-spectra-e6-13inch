#ifndef CONFIGURATION_SCREEN_H
#define CONFIGURATION_SCREEN_H

#include <U8g2_for_Adafruit_GFX.h>
#include <qrcode.h>

#include <memory>

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
  bool showQrCode;
  U8G2_FOR_ADAFRUIT_GFX gfx;

  void drawQRCode(const String& payload, int x, int y, int scale = 3);

 public:
  ConfigurationScreen(DisplayType& display, const String& qrPayload,
                      const String& titleText, const String& subtitleText);
  static String buildWiFiPortalQrPayload(const String& portalUrl);
  static String buildPairingQrPayload(const String& pairingUrl);
  static ConfigurationScreen createStatusScreen(DisplayType& display,
                                                const String& titleText,
                                                const String& subtitleText,
                                                const String& helperText);

  void render() override;
  int nextRefreshInSeconds() override;
};

#endif

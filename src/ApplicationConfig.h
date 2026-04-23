#pragma once

#include <Arduino.h>

#if __has_include("config_dev.h")
#include "config_dev.h"
#else
#include "config_default.h"
#endif

enum ScreenType { CONFIG_SCREEN = 0, IMAGE_SCREEN = 1, SCREEN_COUNT = 2 };

// Dithering algorithm options
enum DitherMode : uint8_t {
  DITHER_FLOYD_STEINBERG = 0,
  DITHER_ATKINSON = 1,
  DITHER_ORDERED = 2,
  DITHER_NONE = 3,
};

// Image Scaling Options
enum ScalingMode : uint8_t {
  SCALE_FIT = 0,  // Letterbox: keep entire image visible
  SCALE_FILL = 1, // Crop: completely fill screen, cutting off edges
};

enum PowerMode : uint8_t {
  POWER_MODE_SLEEP = 0,
  POWER_MODE_ALWAYS_AWAKE = 1,
};

enum CheckForNewImageMode : uint8_t {
  CHECK_MODE_MORE_RESPONSIVE = 0,
  CHECK_MODE_BALANCED = 1,
  CHECK_MODE_LONGER_BATTERY = 2,
};

struct ApplicationConfig {
  char wifiSSID[64];
  char wifiPassword[64];
  uint8_t ditherMode;  // DitherMode enum value
  uint8_t scalingMode; // ScalingMode enum value (0=fit, 1=fill)
  uint8_t powerMode;   // PowerMode enum value
  uint8_t checkForNewImageMode; // CheckForNewImageMode enum value
  uint16_t sleepMinutes;

  // Persisted binary update tracking metadata.
  char lastStatusVersion[64];
  char lastStatusEtag[128];
  char lastAppliedVersion[64];
  char lastAppliedImageId[128];
  char lastAppliedPhotoName[128];
  uint32_t lastSyncEpoch;
  char pairingToken[65];
  char assignedDeviceId[96];

  static const int DISPLAY_ROTATION = 2;

  ApplicationConfig() {
    memset(wifiSSID, 0, sizeof(wifiSSID));
    memset(wifiPassword, 0, sizeof(wifiPassword));

    strncpy(wifiSSID, DEFAULT_WIFI_SSID, sizeof(wifiSSID) - 1);
    strncpy(wifiPassword, DEFAULT_WIFI_PASSWORD, sizeof(wifiPassword) - 1);

    memset(lastStatusVersion, 0, sizeof(lastStatusVersion));
    memset(lastStatusEtag, 0, sizeof(lastStatusEtag));
    memset(lastAppliedVersion, 0, sizeof(lastAppliedVersion));
    memset(lastAppliedImageId, 0, sizeof(lastAppliedImageId));
    memset(lastAppliedPhotoName, 0, sizeof(lastAppliedPhotoName));
    lastSyncEpoch = 0;
    memset(pairingToken, 0, sizeof(pairingToken));
    memset(assignedDeviceId, 0, sizeof(assignedDeviceId));

    ditherMode = DITHER_FLOYD_STEINBERG;
    scalingMode = SCALE_FIT;
    powerMode = POWER_MODE_SLEEP;
    checkForNewImageMode = CHECK_MODE_BALANCED;
    sleepMinutes = 30;
  }

  bool hasValidWiFiCredentials() const {
    return strlen(wifiSSID) > 0 && strlen(wifiPassword) > 0;
  }

  bool hasPairingToken() const { return strlen(pairingToken) > 0; }
  bool hasAssignedDeviceId() const { return strlen(assignedDeviceId) > 0; }

  bool isAlwaysAwake() const { return powerMode == POWER_MODE_ALWAYS_AWAKE; }
};

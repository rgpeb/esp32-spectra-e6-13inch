#pragma once

#include <memory>

#include "ApplicationConfig.h"

class ApplicationConfigStorage {
 public:
  ApplicationConfigStorage();
  ~ApplicationConfigStorage();

  bool save(const ApplicationConfig& config);
  std::unique_ptr<ApplicationConfig> load();
  void clear();

 private:
  static const char* NVS_NAMESPACE;
  static const char* CONFIG_KEY;
  static const char* CONFIG_VERSION_KEY;
  static const uint32_t CONFIG_VERSION;

  static const char* KEY_WIFI_SSID;
  static const char* KEY_WIFI_PASSWORD;
  static const char* KEY_DITHER_MODE;
  static const char* KEY_SCALING_MODE;
  static const char* KEY_POWER_MODE;
  static const char* KEY_SLEEP_MINUTES;
  static const char* KEY_LAST_STATUS_VERSION;
  static const char* KEY_LAST_STATUS_ETAG;
  static const char* KEY_LAST_APPLIED_VERSION;
  static const char* KEY_LAST_APPLIED_IMAGE_ID;
  static const char* KEY_LAST_APPLIED_PHOTO_NAME;
  static const char* KEY_LAST_SYNC_EPOCH;
  static const char* KEY_PAIRING_TOKEN;
  static const char* KEY_ASSIGNED_DEVICE_ID;
};

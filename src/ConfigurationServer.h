#ifndef CONFIGURATION_SERVER_H
#define CONFIGURATION_SERVER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include <functional>

#include "ApplicationConfig.h"

struct Configuration {
  String ssid;
  String password;
  uint8_t powerMode = 0;
  String pairingToken;
  String pairingPageBaseUrl;
  String pairingStatusPath;

  Configuration() = default;

  Configuration(const String &ssid, const String &password, uint8_t powerMode,
                const String &pairingToken, const String &pairingPageBaseUrl,
                const String &pairingStatusPath)
      : ssid(ssid), password(password), powerMode(powerMode),
        pairingToken(pairingToken), pairingPageBaseUrl(pairingPageBaseUrl),
        pairingStatusPath(pairingStatusPath) {}
};

using OnSaveCallback = std::function<void(const Configuration &config)>;

enum ResetAction : uint8_t {
  RESET_ACTION_NONE = 0,
  RESET_ACTION_FORGET_WIFI = 1,
  RESET_ACTION_FACTORY_RESET = 2,
};

class ConfigurationServer {
public:
  static const char *WIFI_AP_NAME;
  static const char *WIFI_AP_PASSWORD;

  ConfigurationServer(const Configuration &currentConfig);
  void run(OnSaveCallback onSaveCallback, bool startAP = true);
  void stop();
  bool isRunning() const;
  void handleRequests();

  String getWifiAccessPointName() const;
  String getWifiAccessPointPassword() const;

  bool isRefreshRequested() const { return refreshRequested; }
  void clearRefreshRequest() { refreshRequested = false; }
  ResetAction getResetActionRequested() const { return resetActionRequested; }
  void clearResetActionRequest() { resetActionRequested = RESET_ACTION_NONE; }
  void setWifiConnectionStatus(bool connected);
  void setAccountLinkedStatus(bool linked);
  void setDeviceStatusSnapshot(const ApplicationConfig &configSnapshot,
                               bool statusFetchSucceeded,
                               bool updatePending,
                               unsigned long lastSuccessfulSyncMs,
                               const String &firmwareVersion,
                               uint32_t lastCheckInEpoch,
                               unsigned long lastCheckInMs);

private:
  String deviceName;
  String wifiAccessPointName;
  String wifiAccessPointPassword;

  Configuration currentConfiguration;

  AsyncWebServer *server;
  DNSServer *dnsServer;
  bool isServerRunning;
  bool refreshRequested = false;
  ResetAction resetActionRequested = RESET_ACTION_NONE;
  bool wifiConnected = false;
  bool accountLinked = false;
  bool lastStatusFetchSucceeded = false;
  bool isUpdatePending = false;
  String lastAppliedVersion;
  String lastAppliedImageId;
  String lastAppliedPhotoName;
  uint32_t lastSyncEpoch = 0;
  unsigned long lastSyncMillis = 0;
  String frameName;
  String deviceId;
  String firmwareVersionValue;
  uint32_t lastCheckInEpoch = 0;
  unsigned long lastCheckInMs = 0;

  String htmlTemplate;
  OnSaveCallback onSaveCallback;

  void setupWebServer();
  void setupDNSServer();
  String getConfigurationPage();
  bool loadHtmlTemplate();
  void handleRoot(AsyncWebServerRequest *request);
  void handleSave(AsyncWebServerRequest *request);
  void handleNotFound(AsyncWebServerRequest *request);
};

#endif

#ifndef CONFIGURATION_SERVER_H
#define CONFIGURATION_SERVER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include <functional>

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
  bool isResetSetupRequested() const { return resetSetupRequested; }
  bool shouldClearPairingOnReset() const { return clearPairingOnReset; }
  void clearResetSetupRequest() {
    resetSetupRequested = false;
    clearPairingOnReset = false;
  }

private:
  String deviceName;
  String wifiAccessPointName;
  String wifiAccessPointPassword;

  Configuration currentConfiguration;

  AsyncWebServer *server;
  DNSServer *dnsServer;
  bool isServerRunning;
  bool refreshRequested = false;
  bool resetSetupRequested = false;
  bool clearPairingOnReset = false;

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

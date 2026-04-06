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

  Configuration() = default;

  Configuration(const String &ssid, const String &password, uint8_t powerMode)
      : ssid(ssid), password(password), powerMode(powerMode) {}
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

private:
  String deviceName;
  String wifiAccessPointName;
  String wifiAccessPointPassword;

  Configuration currentConfiguration;

  AsyncWebServer *server;
  DNSServer *dnsServer;
  bool isServerRunning;
  bool refreshRequested = false;

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

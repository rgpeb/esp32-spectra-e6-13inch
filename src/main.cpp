#include <Arduino.h>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "ImageScreen.h"
#include "WiFiConnection.h"
#include <esp_sleep.h>

namespace {
constexpr unsigned long SERVER_LOOP_DELAY_MS = 10;
constexpr unsigned long AWAKE_AUTO_REFRESH_MS = 5UL * 60UL * 1000UL;
} // namespace

DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

void initializeDefaultConfig() {
  std::unique_ptr<ApplicationConfig> storedConfig = configStorage.load();
  if (storedConfig) {
    appConfig = std::move(storedConfig);
    printf("Configuration loaded from NVS. SSID='%s', powerMode=%u\n",
           appConfig->wifiSSID, appConfig->powerMode);
  } else {
    appConfig.reset(new ApplicationConfig());
    printf("Using default configuration.\n");
  }
}

void refreshDisplay(bool forceFreshFetch = false) {
  ImageScreen imageScreen(display, *appConfig, configStorage, forceFreshFetch);
  imageScreen.render();
}

void saveConfiguration(const Configuration &config) {
  strncpy(appConfig->wifiSSID, config.ssid.c_str(), sizeof(appConfig->wifiSSID) - 1);
  appConfig->wifiSSID[sizeof(appConfig->wifiSSID) - 1] = '\0';

  strncpy(appConfig->wifiPassword, config.password.c_str(),
          sizeof(appConfig->wifiPassword) - 1);
  appConfig->wifiPassword[sizeof(appConfig->wifiPassword) - 1] = '\0';

  appConfig->powerMode =
      (config.powerMode == POWER_MODE_ALWAYS_AWAKE) ? POWER_MODE_ALWAYS_AWAKE
                                                     : POWER_MODE_SLEEP;

  if (configStorage.save(*appConfig)) {
    printf("Configuration saved.\n");
  } else {
    printf("Failed to save configuration.\n");
  }
}

void runWebServer(bool useAP) {
  Configuration serverConfig(appConfig->wifiSSID, appConfig->wifiPassword,
                             appConfig->powerMode);
  ConfigurationServer server(serverConfig);
  server.run(saveConfiguration, useAP);

  printf("Web UI available at: http://%s\n",
         useAP ? "192.168.4.1" : WiFi.localIP().toString().c_str());

  const bool alwaysAwake = appConfig->isAlwaysAwake();
  const unsigned long timeoutMs = alwaysAwake ? 0UL : 10UL * 60UL * 1000UL;
  unsigned long start = millis();
  unsigned long lastAutoRefresh = millis();

  while (true) {
    server.handleRequests();

    if (server.isRefreshRequested()) {
      server.clearRefreshRequest();
      printf("Manual refresh requested from web UI.\n");
      refreshDisplay(true);
    }

    if (alwaysAwake && WiFi.status() == WL_CONNECTED &&
        (millis() - lastAutoRefresh >= AWAKE_AUTO_REFRESH_MS)) {
      printf("Always Awake auto refresh.\n");
      refreshDisplay();
      lastAutoRefresh = millis();
    }

    if (!alwaysAwake && timeoutMs > 0 && millis() - start >= timeoutMs) {
      printf("Web server timeout reached.\n");
      break;
    }

    delay(SERVER_LOOP_DELAY_MS);
  }

  server.stop();
}

void connectWifiIfConfigured() {
  if (!appConfig->hasValidWiFiCredentials()) {
    Serial.println("No WiFi credentials configured.");
    return;
  }

  WiFiConnection wifi(appConfig->wifiSSID, appConfig->wifiPassword);
  wifi.connect();

  int retry = 0;
  while (!wifi.isConnected() && retry < 20) {
    delay(1000);
    retry++;
  }

  if (wifi.isConnected()) {
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect failed.");
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && (millis() - start < 3000)) {
    delay(10);
  }

  initializeDefaultConfig();

  connectWifiIfConfigured();

  if (WiFi.status() == WL_CONNECTED) {
    refreshDisplay();
  }

  const bool useAP = WiFi.status() != WL_CONNECTED;
  runWebServer(useAP);

  if (appConfig->isAlwaysAwake()) {
    printf("Power mode: Always Awake. Staying in loop mode.\n");
    return;
  }

  uint64_t sleepUs = (uint64_t)appConfig->sleepMinutes * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleepUs);
  printf("Power mode: Sleep. Entering deep sleep for %u minute(s).\n",
         appConfig->sleepMinutes);
  esp_deep_sleep_start();
}

void loop() {
  if (appConfig && appConfig->isAlwaysAwake()) {
    delay(1000);
    return;
  }
  delay(1000);
}

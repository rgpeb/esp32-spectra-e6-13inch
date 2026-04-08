#include <Arduino.h>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "ImageScreen.h"
#include "WiFiConnection.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>

DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

namespace {
constexpr unsigned long SERVER_LOOP_DELAY_MS = 10;
constexpr unsigned long AWAKE_AUTO_REFRESH_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long PAIRING_POLL_INTERVAL_MS = 5000UL;

String generatePairingToken() {
  char token[65];
  for (size_t i = 0; i < 32; ++i) {
    const uint8_t value = static_cast<uint8_t>(esp_random() & 0xFF);
    sprintf(&token[i * 2], "%02x", value);
  }
  token[64] = '\0';
  return String(token);
}

String getPairingStatusUrl() {
  return String(DEVICE_SERVER_BASE_URL) + String(PAIRING_STATUS_PATH) +
         "?token=" + String(appConfig->pairingToken);
}

void attachLocalPortalHeaders(HTTPClient &http) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const String localIp = WiFi.localIP().toString();
  if (localIp.length() == 0 || localIp == "0.0.0.0") {
    return;
  }

  http.addHeader("X-Frame-Local-IP", localIp);
  http.addHeader("X-Frame-Portal-URL", "http://" + localIp + "/");
}

bool fetchAssignedDeviceIdFromPairing(String &assignedDeviceIdOut) {
  if (!appConfig || !appConfig->hasPairingToken() || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  const String url = getPairingStatusUrl();
  std::unique_ptr<WiFiClient> client;
  if (url.startsWith("https://")) {
    auto secureClient = new WiFiClientSecure;
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient);
  }

  HTTPClient http;
  http.begin(*client, url);
  http.setTimeout(10000);
  attachLocalPortalHeaders(http);
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return false;
  }

  JsonVariantConst deviceIdVar = doc["deviceId"];
  if (!deviceIdVar.is<const char *>()) {
    return false;
  }

  const String deviceId = String(deviceIdVar.as<const char *>());
  if (deviceId.length() == 0) {
    return false;
  }

  assignedDeviceIdOut = deviceId;
  return true;
}
} // namespace

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

  if (!appConfig->hasPairingToken()) {
    const String token = generatePairingToken();
    strncpy(appConfig->pairingToken, token.c_str(), sizeof(appConfig->pairingToken) - 1);
    appConfig->pairingToken[sizeof(appConfig->pairingToken) - 1] = '\0';
    if (configStorage.save(*appConfig)) {
      printf("Generated and saved pairing token.\n");
    } else {
      printf("Failed to persist generated pairing token.\n");
    }
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
                             appConfig->powerMode, appConfig->pairingToken,
                             PAIRING_PAGE_BASE_URL, PAIRING_STATUS_PATH);
  ConfigurationServer server(serverConfig);
  server.run(saveConfiguration, useAP);

  printf("Web UI available at: http://%s\n",
         useAP ? "192.168.4.1" : WiFi.localIP().toString().c_str());

  const bool alwaysAwake = appConfig->isAlwaysAwake();
  const unsigned long timeoutMs = alwaysAwake ? 0UL : 10UL * 60UL * 1000UL;
  unsigned long start = millis();
  unsigned long lastAutoRefresh = millis();
  unsigned long lastPairingPoll = 0;

  while (true) {
    server.handleRequests();

    if (server.isRefreshRequested()) {
      server.clearRefreshRequest();
      printf("Manual refresh requested from web UI.\n");
      refreshDisplay(true);
    }

    if (server.isResetSetupRequested()) {
      const bool clearPairing = server.shouldClearPairingOnReset();
      server.clearResetSetupRequest();
      Serial.printf("Reset setup requested from web UI (clearPairing=%s).\n",
                    clearPairing ? "true" : "false");
      memset(appConfig->wifiSSID, 0, sizeof(appConfig->wifiSSID));
      memset(appConfig->wifiPassword, 0, sizeof(appConfig->wifiPassword));
      memset(appConfig->lastStatusVersion, 0, sizeof(appConfig->lastStatusVersion));
      memset(appConfig->lastStatusEtag, 0, sizeof(appConfig->lastStatusEtag));
      if (clearPairing) {
        memset(appConfig->assignedDeviceId, 0, sizeof(appConfig->assignedDeviceId));
        memset(appConfig->pairingToken, 0, sizeof(appConfig->pairingToken));
      }
      configStorage.save(*appConfig);
      delay(300);
      ESP.restart();
    }

    if (alwaysAwake && WiFi.status() == WL_CONNECTED &&
        (millis() - lastAutoRefresh >= AWAKE_AUTO_REFRESH_MS)) {
      printf("Always Awake auto refresh.\n");
      refreshDisplay();
      lastAutoRefresh = millis();
    }

    if (!appConfig->hasAssignedDeviceId() && WiFi.status() == WL_CONNECTED &&
        (lastPairingPoll == 0 || millis() - lastPairingPoll >= PAIRING_POLL_INTERVAL_MS)) {
      lastPairingPoll = millis();
      String assignedDeviceId;
      if (fetchAssignedDeviceIdFromPairing(assignedDeviceId)) {
        strncpy(appConfig->assignedDeviceId, assignedDeviceId.c_str(),
                sizeof(appConfig->assignedDeviceId) - 1);
        appConfig->assignedDeviceId[sizeof(appConfig->assignedDeviceId) - 1] = '\0';
        if (configStorage.save(*appConfig)) {
          printf("Pairing complete. Assigned deviceId='%s'\n", appConfig->assignedDeviceId);
        }
      }
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

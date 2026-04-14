#include <Arduino.h>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationScreen.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "ImageScreen.h"
#include "WiFiConnection.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>

DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

namespace {
constexpr unsigned long SERVER_LOOP_DELAY_MS = 10;
constexpr unsigned long AWAKE_AUTO_REFRESH_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long PAIRING_POLL_INTERVAL_MS = 5000UL;
constexpr unsigned long STAGE_SUCCESS_HOLD_MS = 2500UL;
constexpr unsigned long INITIAL_FAST_REFRESH_INTERVAL_MS = 15000UL;
constexpr unsigned long INITIAL_FAST_REFRESH_WINDOW_MS = 2UL * 60UL * 1000UL;
constexpr const char *kBinaryCachePath = "/current.bin";
constexpr const char *kWifiSetupTitle = "Connect to WiFi";
constexpr const char *kWifiConnectedTitle = "Connected to WiFi";
constexpr const char *kPairingSetupTitle = "Connect to your account";
constexpr const char *kPairingConnectedTitle = "Frame ready";

enum SetupStage : uint8_t {
  SETUP_STAGE_WIFI = 0,
  SETUP_STAGE_WIFI_CONNECTED = 1,
  SETUP_STAGE_PAIRING = 2,
  SETUP_STAGE_ACCOUNT_CONNECTED = 3,
  SETUP_STAGE_READY = 4,
};

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

String getPairingPageUrl() {
  return String(PAIRING_PAGE_BASE_URL) + "?token=" + String(appConfig->pairingToken);
}

SetupStage getCurrentSetupStage() {
  if (WiFi.status() != WL_CONNECTED) {
    return SETUP_STAGE_WIFI;
  }
  if (!appConfig->hasAssignedDeviceId()) {
    return SETUP_STAGE_PAIRING;
  }
  return SETUP_STAGE_READY;
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

void showWiFiSetupScreen() {
  const String portalUrl = "http://192.168.4.1/";
  const String qrPayload = ConfigurationScreen::buildWiFiPortalQrPayload(portalUrl);
  ConfigurationScreen setupScreen(display, qrPayload, kWifiSetupTitle,
                                  "Scan to join your frame setup");
  setupScreen.render();
  Serial.printf("[Setup Stage] Welcome + WiFi setup QR shown (portal=%s)\n",
                portalUrl.c_str());
}

void showWiFiConnectedScreen() {
  auto successScreen = ConfigurationScreen::createStatusScreen(
      display, kWifiConnectedTitle, "Your frame is online.",
      "Great! Next, link your account.");
  successScreen.render();
  Serial.println("[Setup Stage] WiFi success screen shown");
}

void showAccountConnectedScreen() {
  auto successScreen = ConfigurationScreen::createStatusScreen(
      display, kPairingConnectedTitle, "Account connection complete.",
      "Your photo will appear in a moment.");
  successScreen.render();
  Serial.println("[Setup Stage] Account linked success screen shown");
}

void showPairingSetupScreen() {
  const String pairingUrl = getPairingPageUrl();
  const String qrPayload = ConfigurationScreen::buildPairingQrPayload(pairingUrl);
  ConfigurationScreen setupScreen(display, qrPayload, kPairingSetupTitle,
                                  "Scan to link this frame");
  setupScreen.render();
  Serial.printf("[Setup Stage] Pairing QR shown (url=%s)\n", pairingUrl.c_str());
}

void showSetupStageScreen(SetupStage stage) {
  switch (stage) {
  case SETUP_STAGE_WIFI:
    showWiFiSetupScreen();
    break;
  case SETUP_STAGE_WIFI_CONNECTED:
    showWiFiConnectedScreen();
    break;
  case SETUP_STAGE_PAIRING:
    showPairingSetupScreen();
    break;
  case SETUP_STAGE_ACCOUNT_CONNECTED:
    showAccountConnectedScreen();
    break;
  case SETUP_STAGE_READY:
  default:
    break;
  }
}

void clearCachedBinaryState() {
  if (!LittleFS.begin(true)) {
    Serial.println("Cache/version/etag cleared: LittleFS mount failed while clearing binary cache");
    return;
  }

  const bool removed = LittleFS.remove(kBinaryCachePath);
  Serial.printf("Cached image/binary state cleared: %s (%s)\n",
                removed ? "removed /current.bin" : "no /current.bin to remove",
                kBinaryCachePath);
  LittleFS.end();
}

void processResetAction(ResetAction resetAction) {
  if (resetAction == RESET_ACTION_NONE) {
    return;
  }

  const bool isFactoryReset = (resetAction == RESET_ACTION_FACTORY_RESET);
  Serial.printf("Reset setup requested from web UI (action=%s).\n",
                isFactoryReset ? "factory-reset" : "forget-wifi");

  memset(appConfig->wifiSSID, 0, sizeof(appConfig->wifiSSID));
  memset(appConfig->wifiPassword, 0, sizeof(appConfig->wifiPassword));
  Serial.println("WiFi credentials cleared");

  if (isFactoryReset) {
    memset(appConfig->assignedDeviceId, 0, sizeof(appConfig->assignedDeviceId));
    Serial.println("deviceId cleared");

    memset(appConfig->pairingToken, 0, sizeof(appConfig->pairingToken));
    Serial.println("Pairing token/state cleared");

    memset(appConfig->lastStatusVersion, 0, sizeof(appConfig->lastStatusVersion));
    memset(appConfig->lastStatusEtag, 0, sizeof(appConfig->lastStatusEtag));
    Serial.println("Cache/version/etag cleared");

    clearCachedBinaryState();
  }

  if (!configStorage.save(*appConfig)) {
    Serial.println("Failed to persist reset state before reboot");
  }

  if (isFactoryReset) {
    display.init(115200);
    display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.display(false);
    display.hibernate();
    Serial.println("Display cleared");

    showWiFiSetupScreen();
  }

  delay(300);
  ESP.restart();
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
      printf("Pairing token generated and saved: %s\n", appConfig->pairingToken);
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
  unsigned long lastFastRefresh = 0;
  const unsigned long fastRefreshWindowStart = millis();
  unsigned long lastPairingPoll = 0;
  unsigned long stageRenderedAt = 0;
  SetupStage displayedStage = getCurrentSetupStage();
  SetupStage stagedTarget = displayedStage;

  if (displayedStage != SETUP_STAGE_READY) {
    showSetupStageScreen(displayedStage);
    stageRenderedAt = millis();
  }
  server.setWifiConnectionStatus(WiFi.status() == WL_CONNECTED);
  server.setAccountLinkedStatus(appConfig->hasAssignedDeviceId());

  while (true) {
    server.handleRequests();
    server.setWifiConnectionStatus(WiFi.status() == WL_CONNECTED);

    if (server.isRefreshRequested()) {
      server.clearRefreshRequest();
      printf("Manual refresh requested from web UI.\n");
      if (appConfig->hasAssignedDeviceId() && WiFi.status() == WL_CONNECTED) {
        refreshDisplay(true);
      } else {
        printf("Manual refresh ignored: frame is not paired yet.\n");
      }
    }

    if (server.getResetActionRequested() != RESET_ACTION_NONE) {
      const ResetAction resetAction = server.getResetActionRequested();
      server.clearResetActionRequest();
      processResetAction(resetAction);
    }

    if (alwaysAwake && appConfig->hasAssignedDeviceId() &&
        WiFi.status() == WL_CONNECTED &&
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
          server.setAccountLinkedStatus(true);
        }
      }
    }

    const SetupStage derivedStage = getCurrentSetupStage();
    if (displayedStage == SETUP_STAGE_WIFI &&
        derivedStage == SETUP_STAGE_PAIRING) {
      displayedStage = SETUP_STAGE_WIFI_CONNECTED;
      stagedTarget = SETUP_STAGE_PAIRING;
      showSetupStageScreen(displayedStage);
      stageRenderedAt = millis();
    } else if (displayedStage == SETUP_STAGE_PAIRING &&
               derivedStage == SETUP_STAGE_READY) {
      displayedStage = SETUP_STAGE_ACCOUNT_CONNECTED;
      stagedTarget = SETUP_STAGE_READY;
      showSetupStageScreen(displayedStage);
      stageRenderedAt = millis();
    } else if (derivedStage != SETUP_STAGE_READY && derivedStage != displayedStage &&
               displayedStage != SETUP_STAGE_WIFI_CONNECTED &&
               displayedStage != SETUP_STAGE_ACCOUNT_CONNECTED) {
      displayedStage = derivedStage;
      stagedTarget = derivedStage;
      showSetupStageScreen(displayedStage);
      stageRenderedAt = millis();
    }

    if ((displayedStage == SETUP_STAGE_WIFI_CONNECTED ||
         displayedStage == SETUP_STAGE_ACCOUNT_CONNECTED) &&
        millis() - stageRenderedAt >= STAGE_SUCCESS_HOLD_MS) {
      displayedStage = stagedTarget;
      if (displayedStage != SETUP_STAGE_READY) {
        showSetupStageScreen(displayedStage);
        stageRenderedAt = millis();
      } else {
        Serial.println("[Setup Stage] Setup complete. Entering normal operation.");
        refreshDisplay(true);
      }
    }

    const bool withinFastRefreshWindow =
        millis() - fastRefreshWindowStart <= INITIAL_FAST_REFRESH_WINDOW_MS;
    if (appConfig->hasAssignedDeviceId() && WiFi.status() == WL_CONNECTED &&
        withinFastRefreshWindow &&
        (lastFastRefresh == 0 ||
         millis() - lastFastRefresh >= INITIAL_FAST_REFRESH_INTERVAL_MS)) {
      Serial.println("Initial fast refresh check.");
      refreshDisplay(lastFastRefresh == 0);
      lastFastRefresh = millis();
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
    Serial.printf("WiFi setup succeeded. Connected at %s\n",
                  WiFi.localIP().toString().c_str());
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
  const SetupStage setupStage = getCurrentSetupStage();
  if (setupStage == SETUP_STAGE_READY) {
    Serial.println("[Setup Stage] WiFi + pairing complete. Starting normal operation.");
    refreshDisplay(true);
  } else {
    Serial.printf("[Setup Stage] Waiting for setup stage=%u\n", setupStage);
    showSetupStageScreen(setupStage);
  }

  const bool useAP = WiFi.status() != WL_CONNECTED;
  runWebServer(useAP);
}

void loop() {}

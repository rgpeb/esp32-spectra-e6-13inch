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
constexpr unsigned long INITIAL_FAST_REFRESH_INTERVAL_MS = 10000UL;
constexpr unsigned long INITIAL_FAST_REFRESH_WINDOW_MS = 90UL * 1000UL;
constexpr unsigned long INITIAL_PROMPT_REFRESH_DELAY_MS = 1500UL;
constexpr const char *kBinaryCachePath = "/current.bin";
constexpr const char *kPairingUnlinkPath = "/pairing/unlink";
enum SetupUiState : uint8_t {
  SETUP_STATE_WELCOME_JOIN_WIFI = 0,
  SETUP_STATE_CONNECT_HOME_WIFI = 1,
  SETUP_STATE_WIFI_CONNECTED = 2,
  SETUP_STATE_CONNECT_ACCOUNT = 3,
  SETUP_STATE_ACCOUNT_CONNECTED = 4,
  SETUP_STATE_WAITING_FIRST_PHOTO = 5,
  SETUP_STATE_READY = 6,
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

SetupUiState getCurrentSetupStage(bool hasShownWifiSuccess, bool hasShownAccountSuccess,
                                  bool hasDisplayedFirstImage,
                                  bool hasSeenSetupClient) {
  if (WiFi.status() != WL_CONNECTED) {
    return hasSeenSetupClient ? SETUP_STATE_CONNECT_HOME_WIFI
                              : SETUP_STATE_WELCOME_JOIN_WIFI;
  }
  if (!appConfig->hasAssignedDeviceId()) {
    return SETUP_STATE_WIFI_CONNECTED;
  }
  if (hasShownAccountSuccess) {
    return SETUP_STATE_ACCOUNT_CONNECTED;
  }
  if (!hasDisplayedFirstImage) {
    return SETUP_STATE_WAITING_FIRST_PHOTO;
  }
  return SETUP_STATE_READY;
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

void showWelcomeJoinWifiScreen() {
  const String qrPayload = ConfigurationScreen::buildJoinWifiQrPayload(
      ConfigurationServer::WIFI_AP_NAME, ConfigurationServer::WIFI_AP_PASSWORD);
  ConfigurationScreen setupScreen(
      display, qrPayload, "Join frame WiFi",
      "On your phone, join the frame setup network.");
  setupScreen.render();
  Serial.println("[Setup Stage] Welcome + join frame WiFi shown");
}

void showConnectHomeWifiScreen() {
  const String portalUrl = "http://192.168.4.1/";
  const String qrPayload = ConfigurationScreen::buildWiFiPortalQrPayload(portalUrl);
  ConfigurationScreen setupScreen(display, qrPayload, "Connect this frame",
                                  "Open setup, then enter your home WiFi.");
  setupScreen.render();
  Serial.printf("[Setup Stage] Home WiFi portal step shown (portal=%s)\n",
                portalUrl.c_str());
}

void showWiFiConnectedScreen() {
  const String pairingUrl = getPairingPageUrl();
  const String qrPayload = ConfigurationScreen::buildPairingQrPayload(pairingUrl);
  ConfigurationScreen successScreen(display, qrPayload, "Connected to WiFi",
                                    "Next, connect this frame to your account");
  successScreen.render();
  Serial.printf("[Setup Stage] WiFi connected + connect-account QR shown (url=%s)\n",
                pairingUrl.c_str());
}

void showAccountConnectedScreen() {
  auto successScreen = ConfigurationScreen::createStatusScreen(
      display, "Your frame is connected", "Account linked successfully.",
      "Browse the library and choose a photo.");
  successScreen.render();
  Serial.println("[Setup Stage] Account linked success screen shown");
}

void showPairingSetupScreen() {
  const String pairingUrl = getPairingPageUrl();
  const String qrPayload = ConfigurationScreen::buildPairingQrPayload(pairingUrl);
  ConfigurationScreen setupScreen(display, qrPayload, "Connect to your account",
                                  "Scan this QR to finish setup.");
  setupScreen.render();
  Serial.printf("[Setup Stage] Pairing QR shown (url=%s)\n", pairingUrl.c_str());
}

void showWaitingForFirstPhotoScreen() {
  auto statusScreen = ConfigurationScreen::createStatusScreen(
      display, "Frame ready", "Waiting for your first photo.",
      "Browse the library and choose a photo.");
  statusScreen.render();
  Serial.println("[Setup Stage] Waiting for first photo screen shown");
}

void showSetupStageScreen(SetupUiState stage) {
  switch (stage) {
  case SETUP_STATE_WELCOME_JOIN_WIFI:
    showWelcomeJoinWifiScreen();
    break;
  case SETUP_STATE_CONNECT_HOME_WIFI:
    showConnectHomeWifiScreen();
    break;
  case SETUP_STATE_WIFI_CONNECTED:
    showWiFiConnectedScreen();
    break;
  case SETUP_STATE_CONNECT_ACCOUNT:
    showPairingSetupScreen();
    break;
  case SETUP_STATE_ACCOUNT_CONNECTED:
    showAccountConnectedScreen();
    break;
  case SETUP_STATE_WAITING_FIRST_PHOTO:
    showWaitingForFirstPhotoScreen();
    break;
  case SETUP_STATE_READY:
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

bool notifyServerBeforeReset() {
  if (!appConfig || WiFi.status() != WL_CONNECTED) {
    Serial.println("Reset unlink notify skipped: WiFi not connected.");
    return false;
  }
  if (!appConfig->hasPairingToken() && !appConfig->hasAssignedDeviceId()) {
    Serial.println("Reset unlink notify skipped: no pairing token or assigned deviceId.");
    return false;
  }

  const String url = String(DEVICE_SERVER_BASE_URL) + kPairingUnlinkPath;
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
  http.addHeader("Content-Type", "application/json");
  attachLocalPortalHeaders(http);

  JsonDocument payload;
  payload["token"] = String(appConfig->pairingToken);
  payload["deviceId"] = String(appConfig->assignedDeviceId);
  String body;
  serializeJson(payload, body);

  const int status = http.POST(body);
  http.end();
  const bool ok = (status >= 200 && status < 300);
  Serial.printf("Reset unlink notify %s (HTTP %d, url=%s)\n",
                ok ? "succeeded" : "failed", status, url.c_str());
  return ok;
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
    notifyServerBeforeReset();

    memset(appConfig->assignedDeviceId, 0, sizeof(appConfig->assignedDeviceId));
    Serial.println("deviceId cleared");

    memset(appConfig->pairingToken, 0, sizeof(appConfig->pairingToken));
    Serial.println("Pairing token/state cleared");

    memset(appConfig->lastStatusVersion, 0, sizeof(appConfig->lastStatusVersion));
    memset(appConfig->lastStatusEtag, 0, sizeof(appConfig->lastStatusEtag));
    memset(appConfig->lastAppliedVersion, 0, sizeof(appConfig->lastAppliedVersion));
    memset(appConfig->lastAppliedImageId, 0, sizeof(appConfig->lastAppliedImageId));
    memset(appConfig->lastAppliedPhotoName, 0, sizeof(appConfig->lastAppliedPhotoName));
    appConfig->lastSyncEpoch = 0;
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

ImageScreen::RefreshResult refreshDisplayWithResult(bool forceFreshFetch = false) {
  ImageScreen imageScreen(display, *appConfig, configStorage, forceFreshFetch);
  return imageScreen.refresh();
}

bool refreshDisplay(bool forceFreshFetch = false) {
  return refreshDisplayWithResult(forceFreshFetch).rendered;
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
  bool firstImageShown = false;
  bool wifiSuccessPending = false;
  bool accountSuccessPending = false;
  bool ranInitialPromptRefresh = false;
  unsigned long initialPromptAt = millis();
  SetupUiState lastRenderedSetupState = SETUP_STATE_READY;
  bool hasSeenSetupClient = false;
  bool lastStatusFetchSucceeded = false;
  bool lastUpdatePending = false;
  unsigned long lastSuccessfulSyncMs = 0;
  server.setWifiConnectionStatus(WiFi.status() == WL_CONNECTED);
  server.setAccountLinkedStatus(appConfig->hasAssignedDeviceId());
  server.setDeviceStatusSnapshot(*appConfig, lastStatusFetchSucceeded,
                                 lastUpdatePending, lastSuccessfulSyncMs);

  while (true) {
    server.handleRequests();
    server.setWifiConnectionStatus(WiFi.status() == WL_CONNECTED);
    if (WiFi.softAPgetStationNum() > 0) {
      hasSeenSetupClient = true;
    }

    if (server.isRefreshRequested()) {
      server.clearRefreshRequest();
      printf("Manual refresh requested from web UI.\n");
      if (appConfig->hasAssignedDeviceId() && WiFi.status() == WL_CONNECTED) {
        const auto refreshResult = refreshDisplayWithResult(false);
        firstImageShown = refreshResult.rendered || firstImageShown;
        lastStatusFetchSucceeded = refreshResult.statusFetchSucceeded;
        lastUpdatePending = refreshResult.updatePending;
        if (refreshResult.rendered) {
          lastSuccessfulSyncMs = millis();
        }
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
      const auto refreshResult = refreshDisplayWithResult();
      firstImageShown = refreshResult.rendered || firstImageShown;
      lastStatusFetchSucceeded = refreshResult.statusFetchSucceeded;
      lastUpdatePending = refreshResult.updatePending;
      if (refreshResult.rendered) {
        lastSuccessfulSyncMs = millis();
      }
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
          accountSuccessPending = true;
          initialPromptAt = millis();
          ranInitialPromptRefresh = false;
          const auto refreshResult = refreshDisplayWithResult(true);
          firstImageShown = refreshResult.rendered || firstImageShown;
          lastStatusFetchSucceeded = refreshResult.statusFetchSucceeded;
          lastUpdatePending = refreshResult.updatePending;
          if (refreshResult.rendered) {
            lastSuccessfulSyncMs = millis();
          }
        }
      }
    }

    if (WiFi.status() == WL_CONNECTED && !wifiSuccessPending &&
        !appConfig->hasAssignedDeviceId()) {
      wifiSuccessPending = true;
    }

    if (wifiSuccessPending &&
        lastRenderedSetupState != SETUP_STATE_WIFI_CONNECTED) {
      showSetupStageScreen(SETUP_STATE_WIFI_CONNECTED);
      lastRenderedSetupState = SETUP_STATE_WIFI_CONNECTED;
      stageRenderedAt = millis();
    } else if (accountSuccessPending &&
               lastRenderedSetupState != SETUP_STATE_ACCOUNT_CONNECTED) {
      showSetupStageScreen(SETUP_STATE_ACCOUNT_CONNECTED);
      lastRenderedSetupState = SETUP_STATE_ACCOUNT_CONNECTED;
      stageRenderedAt = millis();
    } else {
      const SetupUiState derivedStage =
          getCurrentSetupStage(wifiSuccessPending, accountSuccessPending,
                               firstImageShown, hasSeenSetupClient);
      if (derivedStage != SETUP_STATE_READY &&
          derivedStage != lastRenderedSetupState) {
        showSetupStageScreen(derivedStage);
        lastRenderedSetupState = derivedStage;
      }
    }

    if (wifiSuccessPending &&
        millis() - stageRenderedAt >= STAGE_SUCCESS_HOLD_MS) {
      wifiSuccessPending = false;
      lastRenderedSetupState = SETUP_STATE_READY;
    }

    if (accountSuccessPending &&
        millis() - stageRenderedAt >= STAGE_SUCCESS_HOLD_MS) {
      accountSuccessPending = false;
      lastRenderedSetupState = SETUP_STATE_READY;
    }

    const bool withinFastRefreshWindow =
        millis() - fastRefreshWindowStart <= INITIAL_FAST_REFRESH_WINDOW_MS;

    if (appConfig->hasAssignedDeviceId() && WiFi.status() == WL_CONNECTED &&
        !ranInitialPromptRefresh &&
        millis() - initialPromptAt >= INITIAL_PROMPT_REFRESH_DELAY_MS) {
      Serial.println("Initial prompt refresh check.");
      const auto refreshResult = refreshDisplayWithResult(true);
      firstImageShown = refreshResult.rendered || firstImageShown;
      lastStatusFetchSucceeded = refreshResult.statusFetchSucceeded;
      lastUpdatePending = refreshResult.updatePending;
      if (refreshResult.rendered) {
        lastSuccessfulSyncMs = millis();
      }
      ranInitialPromptRefresh = true;
      lastFastRefresh = millis();
    }

    if (appConfig->hasAssignedDeviceId() && WiFi.status() == WL_CONNECTED &&
        withinFastRefreshWindow &&
        (lastFastRefresh == 0 ||
         millis() - lastFastRefresh >= INITIAL_FAST_REFRESH_INTERVAL_MS)) {
      Serial.println("Initial fast refresh check.");
      const auto refreshResult = refreshDisplayWithResult(lastFastRefresh == 0);
      firstImageShown = refreshResult.rendered || firstImageShown;
      lastStatusFetchSucceeded = refreshResult.statusFetchSucceeded;
      lastUpdatePending = refreshResult.updatePending;
      if (refreshResult.rendered) {
        lastSuccessfulSyncMs = millis();
      }
      lastFastRefresh = millis();
    }

    server.setDeviceStatusSnapshot(*appConfig, lastStatusFetchSucceeded,
                                   lastUpdatePending, lastSuccessfulSyncMs);

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
  const bool ready = (WiFi.status() == WL_CONNECTED && appConfig->hasAssignedDeviceId());
  if (ready) {
    Serial.println("[Setup Stage] WiFi + pairing complete. Starting normal operation.");
    refreshDisplay(true);
  }

  const bool useAP = WiFi.status() != WL_CONNECTED;
  runWebServer(useAP);
}

void loop() {}

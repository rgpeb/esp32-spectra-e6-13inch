#include <Arduino.h>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationScreen.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "FirmwareInfo.h"
#include "FrameStatusHeaders.h"
#include "ImageScreen.h"
#include "WiFiConnection.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>

DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

namespace {
constexpr unsigned long SERVER_LOOP_DELAY_MS = 10;
constexpr unsigned long AWAKE_AUTO_REFRESH_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long PAIRING_POLL_INTERVAL_MS = 5000UL;
constexpr unsigned long SETUP_REFRESH_INTERVAL_MS = 30UL * 1000UL;
constexpr unsigned long SETUP_POST_COMPLETE_WINDOW_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long CHECK_INTERVAL_MORE_RESPONSIVE_MS = 15UL * 60UL * 1000UL;
constexpr unsigned long CHECK_INTERVAL_BALANCED_MS = 60UL * 60UL * 1000UL;
constexpr unsigned long CHECK_INTERVAL_LONGER_BATTERY_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr const char *kBinaryCachePath = "/current.bin";
constexpr const char *kFactoryResetPath = "/device/factory-reset";
constexpr const char *kFactoryResetByDevicePrefix = "/device/";
constexpr const char *kFactoryResetByTokenPrefix = "/device/by-token/";
enum SetupUiState : uint8_t {
  SETUP_STATE_CONNECT_HOME_WIFI = 0,
  SETUP_STATE_OPEN_AP_PORTAL = 1,
  SETUP_STATE_CONNECT_ACCOUNT = 2,
  SETUP_STATE_WAITING_FIRST_PHOTO = 3,
  SETUP_STATE_READY = 4,
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

String getPortalUrlForCurrentNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    const String localIp = WiFi.localIP().toString();
    if (localIp.length() > 0 && localIp != "0.0.0.0") {
      return "http://" + localIp + "/";
    }
  }

  if (WiFi.softAPgetStationNum() > 0) {
    const String apIp = WiFi.softAPIP().toString();
    if (apIp.length() > 0 && apIp != "0.0.0.0") {
      return "http://" + apIp + "/";
    }
  }

  return "";
}

unsigned long getNormalCheckIntervalMs(const ApplicationConfig &config) {
  switch (config.checkForNewImageMode) {
  case CHECK_MODE_MORE_RESPONSIVE:
    return CHECK_INTERVAL_MORE_RESPONSIVE_MS;
  case CHECK_MODE_LONGER_BATTERY:
    return CHECK_INTERVAL_LONGER_BATTERY_MS;
  case CHECK_MODE_BALANCED:
  default:
    return CHECK_INTERVAL_BALANCED_MS;
  }
}

SetupUiState getCurrentSetupStage(bool hasDisplayedFirstImage) {
  if (WiFi.status() != WL_CONNECTED) {
    if (WiFi.softAPgetStationNum() > 0) {
      return SETUP_STATE_OPEN_AP_PORTAL;
    }
    return SETUP_STATE_CONNECT_HOME_WIFI;
  }

  if (!appConfig->hasAssignedDeviceId()) {
    return SETUP_STATE_CONNECT_ACCOUNT;
  }

  if (!hasDisplayedFirstImage) {
    return SETUP_STATE_WAITING_FIRST_PHOTO;
  }

  return SETUP_STATE_READY;
}

void attachLocalPortalHeaders(HTTPClient &http) {
  if (!appConfig) {
    return;
  }
  const String resolvedDeviceId =
      appConfig->hasAssignedDeviceId() ? String(appConfig->assignedDeviceId)
                                       : String(DEVICE_ID);
  addFrameStatusHeaders(http, *appConfig, resolvedDeviceId);
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

void showConnectHomeWifiScreen(bool commitUpdate = true) {
  const String qrPayload = ConfigurationScreen::buildJoinWifiQrPayload(
      String(ConfigurationServer::WIFI_AP_NAME),
      String(ConfigurationServer::WIFI_AP_PASSWORD));
  const std::vector<String> timelineEntries = {
      "Step 1: Connect your phone to Framey-Config.",
      "Step 2: Scan QR to connect frame to your home network.",
      "Step 3: After WiFi save, switch your phone back to home WiFi."};
  ConfigurationScreen setupScreen(
      display, qrPayload, "Connect this frame",
      "Step 1: Scan this QR to join Framey-Config on your phone.",
      timelineEntries, 0, true);
  setupScreen.renderWithCommit(commitUpdate);
  Serial.printf(
      "[Setup Stage] Home WiFi setup shown with AP join QR (ssid=%s)\n",
      ConfigurationServer::WIFI_AP_NAME);
}

void showOpenApPortalScreen(bool commitUpdate = true) {
  const String apIp = WiFi.softAPIP().toString();
  const String portalUrl = (apIp.length() > 0 && apIp != "0.0.0.0")
                               ? "http://" + apIp + "/"
                               : "";
  const std::vector<String> timelineEntries = {
      "Step 1 done: Phone joined Framey-Config.",
      "Step 2: Scan QR to connect frame to your home network.",
      "Enter home WiFi, then switch phone back to home WiFi for Step 3."};
  if (portalUrl.length() > 0) {
    ConfigurationScreen setupScreen(
        display, ConfigurationScreen::buildWiFiPortalQrPayload(portalUrl),
        "Open setup portal",
        "Step 2: Scan QR to connect frame to your home network.",
        timelineEntries, 1, true);
    setupScreen.renderWithCommit(commitUpdate);
    Serial.printf("[Setup Stage] AP portal QR shown (url=%s)\n", portalUrl.c_str());
  } else {
    auto statusScreen = ConfigurationScreen::createStatusScreen(
        display, "Open setup portal",
        "Waiting for portal address.",
        "Stay connected to Framey-Config. This will refresh shortly.",
        timelineEntries, 1, true);
    statusScreen.renderWithCommit(commitUpdate);
    Serial.println("[Setup Stage] AP portal URL unavailable.");
  }
}

void showPairingSetupScreen(bool commitUpdate = true) {
  const bool onHomeWifi = WiFi.status() == WL_CONNECTED;
  const bool hasPairingToken = appConfig && appConfig->hasPairingToken();
  const String directPairingUrl =
      hasPairingToken
          ? String(PAIRING_PAGE_BASE_URL) + "?token=" + String(appConfig->pairingToken)
          : "";
  const String portalUrl = onHomeWifi ? getPortalUrlForCurrentNetwork() : "";
  const bool canShowDirectPairingQr = onHomeWifi && directPairingUrl.length() > 0;
  const bool canShowPortalFallbackQr = onHomeWifi && portalUrl.length() > 0;
  const std::vector<String> timelineEntries = {
      "Step 2 done: Home WiFi credentials were saved.",
      "Step 3: Switch phone to home WiFi, then scan QR to open account link.",
      "Open the account link and connect this frame."};
  if (canShowDirectPairingQr) {
    const String qrPayload =
        ConfigurationScreen::buildPairingQrPayload(directPairingUrl);
    ConfigurationScreen setupScreen(
        display, qrPayload, "Connect to your account",
        "Step 3: On home WiFi, scan QR to open the account link for this frame.",
        timelineEntries, 2, true);
    setupScreen.renderWithCommit(commitUpdate);
    Serial.printf("[Setup Stage] Direct pairing QR shown (url=%s)\n",
                  directPairingUrl.c_str());
  } else if (canShowPortalFallbackQr) {
    const String qrPayload =
        ConfigurationScreen::buildWiFiPortalQrPayload(portalUrl);
    ConfigurationScreen setupScreen(
        display, qrPayload, "Connect to your account",
        "Step 3: Scan QR to open the account link, then connect this frame.",
        timelineEntries, 2, true);
    setupScreen.renderWithCommit(commitUpdate);
    Serial.printf("[Setup Stage] Pairing fallback portal QR shown (url=%s)\n",
                  portalUrl.c_str());
  } else {
    auto statusScreen = ConfigurationScreen::createStatusScreen(
        display, "Connect to your account",
        "Waiting for pairing link.",
        "Switch your phone to home WiFi now. This will refresh shortly.",
        timelineEntries, 2, true);
    statusScreen.renderWithCommit(commitUpdate);
    Serial.println(
        "[Setup Stage] Pairing link unavailable (no token or local URL yet).");
  }
}

void showWaitingForFirstPhotoScreen() {
  const std::vector<String> timelineEntries = {
      "Home WiFi connected.",
      "Account connected to this frame.",
      "Waiting for first photo selection."};
  auto statusScreen = ConfigurationScreen::createStatusScreen(
      display, "Frame ready", "Waiting for your first photo.",
      "Browse the library and choose a photo.", timelineEntries, 3, true);
  statusScreen.render();
  Serial.println("[Setup Stage] Waiting for first photo screen shown");
}

void showSetupCompleteScreen() {
  const std::vector<String> timelineEntries = {
      "Home WiFi connected.",
      "Account connected to this frame.",
      "Setup complete."};
  auto statusScreen = ConfigurationScreen::createStatusScreen(
      display, "Congrats!", "Setup complete.",
      "Your frame is connected and ready to display photos.",
      timelineEntries, 3, true);
  statusScreen.render();
  Serial.println("[Setup Stage] Setup complete screen shown");
}

void showSetupStageScreen(SetupUiState stage) {
  switch (stage) {
  case SETUP_STATE_CONNECT_HOME_WIFI:
    showConnectHomeWifiScreen();
    break;
  case SETUP_STATE_OPEN_AP_PORTAL:
    showOpenApPortalScreen();
    break;
  case SETUP_STATE_CONNECT_ACCOUNT:
    showPairingSetupScreen();
    break;
  case SETUP_STATE_WAITING_FIRST_PHOTO:
    showWaitingForFirstPhotoScreen();
    break;
  case SETUP_STATE_READY:
    showSetupCompleteScreen();
    break;
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

bool postFactoryResetSignal(const String &url, const JsonDocument &payload) {
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
  if (appConfig && appConfig->hasPairingToken()) {
    http.addHeader("x-frame-reset-token", String(appConfig->pairingToken));
  }
  attachLocalPortalHeaders(http);

  String body;
  serializeJson(payload, body);

  const int status = http.POST(body);
  http.end();
  const bool ok = (status >= 200 && status < 300);
  Serial.printf("Factory reset notify %s (HTTP %d, url=%s)\n",
                ok ? "succeeded" : "failed", status, url.c_str());
  return ok;
}

bool notifyServerBeforeReset() {
  if (!appConfig || WiFi.status() != WL_CONNECTED) {
    Serial.println("Factory reset notify skipped: WiFi not connected.");
    return false;
  }
  if (!appConfig->hasPairingToken() && !appConfig->hasAssignedDeviceId()) {
    Serial.println(
        "Factory reset notify skipped: no pairing token or assigned deviceId.");
    return false;
  }

  JsonDocument payload;
  payload["reason"] = "factory-reset";

  if (appConfig->hasPairingToken()) {
    payload["token"] = String(appConfig->pairingToken);
    const String byTokenUrl = String(DEVICE_SERVER_BASE_URL) +
                              kFactoryResetByTokenPrefix +
                              String(appConfig->pairingToken) +
                              "/factory-reset";
    if (postFactoryResetSignal(byTokenUrl, payload)) {
      return true;
    }
  }

  if (appConfig->hasAssignedDeviceId()) {
    payload["deviceId"] = String(appConfig->assignedDeviceId);
    const String byDeviceUrl = String(DEVICE_SERVER_BASE_URL) +
                               kFactoryResetByDevicePrefix +
                               String(appConfig->assignedDeviceId) +
                               "/factory-reset";
    if (postFactoryResetSignal(byDeviceUrl, payload)) {
      return true;
    }
  }

  const String genericUrl = String(DEVICE_SERVER_BASE_URL) + kFactoryResetPath;
  return postFactoryResetSignal(genericUrl, payload);
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
  appConfig->checkForNewImageMode = config.checkForNewImageMode;

  if (configStorage.save(*appConfig)) {
    printf("Configuration saved.\n");
  } else {
    printf("Failed to save configuration.\n");
  }
}

void runWebServer(bool useAP) {
  Configuration serverConfig(appConfig->wifiSSID, appConfig->wifiPassword,
                             appConfig->powerMode, appConfig->checkForNewImageMode,
                             appConfig->pairingToken,
                             PAIRING_PAGE_BASE_URL, PAIRING_STATUS_PATH);
  ConfigurationServer server(serverConfig);
  server.run(saveConfiguration, useAP);

  const String webUiHost =
      useAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  printf("Web UI available at: http://%s\n", webUiHost.c_str());

  const bool alwaysAwake = appConfig->isAlwaysAwake();
  const unsigned long timeoutMs = alwaysAwake ? 0UL : 10UL * 60UL * 1000UL;
  unsigned long start = millis();
  unsigned long lastAutoRefresh = millis();
  unsigned long lastBackgroundRefreshMs = 0;
  unsigned long setupCompletedAtMs = 0;
  unsigned long lastPairingPoll = 0;
  bool firstImageShown = false;
  bool setupModeActive = true;
  bool setupCompletionWindowStarted = false;
  bool wasOffline = (WiFi.status() != WL_CONNECTED);
  bool reconnectImmediateCheckPending = false;
  SetupUiState lastRenderedSetupState = SETUP_STATE_READY;
  bool lastStatusFetchSucceeded = false;
  bool lastUpdatePending = false;
  unsigned long lastSuccessfulSyncMs = 0;
  unsigned long lastServerCheckInMs = 0;
  uint32_t lastServerCheckInEpoch = 0;
  auto applyRefreshResult = [&](const ImageScreen::RefreshResult &refreshResult) {
    firstImageShown = refreshResult.rendered || firstImageShown;
    lastStatusFetchSucceeded = refreshResult.statusFetchSucceeded;
    lastUpdatePending = refreshResult.updatePending;
    lastServerCheckInMs = millis();
    const time_t now = time(nullptr);
    lastServerCheckInEpoch = now > 0 ? static_cast<uint32_t>(now) : 0;
    if (refreshResult.rendered) {
      lastSuccessfulSyncMs = millis();
    }
    lastBackgroundRefreshMs = millis();
  };
  server.setWifiConnectionStatus(WiFi.status() == WL_CONNECTED);
  server.setAccountLinkedStatus(appConfig->hasAssignedDeviceId());
  server.setDeviceStatusSnapshot(*appConfig, lastStatusFetchSucceeded,
                                 lastUpdatePending, lastSuccessfulSyncMs,
                                 firmwareVersion(), lastServerCheckInEpoch,
                                 lastServerCheckInMs);

  while (true) {
    server.handleRequests();
    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    server.setWifiConnectionStatus(wifiConnected);

    if (!wifiConnected) {
      wasOffline = true;
    } else if (wasOffline) {
      wasOffline = false;
      reconnectImmediateCheckPending = true;
      Serial.println("WiFi connectivity restored: immediate reconnect check armed.");
    }

    if (server.isRefreshRequested()) {
      server.clearRefreshRequest();
      printf("Manual refresh requested from web UI.\n");
      if (appConfig->hasAssignedDeviceId() && wifiConnected) {
        applyRefreshResult(refreshDisplayWithResult(false));
      } else {
        printf("Manual refresh ignored: frame is not paired yet.\n");
      }
    }

    if (server.getResetActionRequested() != RESET_ACTION_NONE) {
      const ResetAction resetAction = server.getResetActionRequested();
      server.clearResetActionRequest();
      processResetAction(resetAction);
    }

    if (alwaysAwake && appConfig->hasAssignedDeviceId() && wifiConnected &&
        (millis() - lastAutoRefresh >= AWAKE_AUTO_REFRESH_MS)) {
      printf("Always Awake auto refresh.\n");
      applyRefreshResult(refreshDisplayWithResult());
      lastAutoRefresh = millis();
    }

    if (!appConfig->hasAssignedDeviceId() && wifiConnected &&
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
          start = millis();
          applyRefreshResult(refreshDisplayWithResult(true));
        }
      }
    }

    const SetupUiState derivedStage = getCurrentSetupStage(firstImageShown);
    if (derivedStage != lastRenderedSetupState) {
      showSetupStageScreen(derivedStage);
      lastRenderedSetupState = derivedStage;
    }

    if (derivedStage == SETUP_STATE_READY && !setupCompletionWindowStarted) {
      setupCompletionWindowStarted = true;
      setupCompletedAtMs = millis();
      Serial.println("Setup complete detected: 10-minute setup refresh window started.");
    }

    if (setupCompletionWindowStarted &&
        millis() - setupCompletedAtMs >= SETUP_POST_COMPLETE_WINDOW_MS) {
      setupModeActive = false;
    }

    if (appConfig->hasAssignedDeviceId() && wifiConnected && reconnectImmediateCheckPending) {
      Serial.println("Reconnect recovery check.");
      applyRefreshResult(refreshDisplayWithResult(false));
      reconnectImmediateCheckPending = false;
    } else if (appConfig->hasAssignedDeviceId() && wifiConnected) {
      const unsigned long intervalMs =
          setupModeActive ? SETUP_REFRESH_INTERVAL_MS : getNormalCheckIntervalMs(*appConfig);
      if (lastBackgroundRefreshMs == 0 ||
          millis() - lastBackgroundRefreshMs >= intervalMs) {
        if (setupModeActive) {
          Serial.println("Setup-mode background check.");
        } else {
          Serial.printf("Normal background check (mode=%u).\n",
                        appConfig->checkForNewImageMode);
        }
        applyRefreshResult(refreshDisplayWithResult(lastBackgroundRefreshMs == 0));
      }
    }

    server.setDeviceStatusSnapshot(*appConfig, lastStatusFetchSucceeded,
                                   lastUpdatePending, lastSuccessfulSyncMs,
                                   firmwareVersion(), lastServerCheckInEpoch,
                                   lastServerCheckInMs);

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

#include "ConfigurationServer.h"

#include "ApplicationConfig.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiAP.h>

const char *ConfigurationServer::WIFI_AP_NAME = "Framey-Config";
const char *ConfigurationServer::WIFI_AP_PASSWORD = "configure123";

ConfigurationServer::ConfigurationServer(const Configuration &currentConfig)
    : deviceName("E-Ink-Display"), wifiAccessPointName(WIFI_AP_NAME),
      wifiAccessPointPassword(WIFI_AP_PASSWORD),
      currentConfiguration(currentConfig), server(nullptr), dnsServer(nullptr),
      isServerRunning(false) {}

void ConfigurationServer::run(OnSaveCallback onSaveCallback, bool startAP) {
  this->onSaveCallback = onSaveCallback;

  Serial.println("Starting Configuration Server...");

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed - filesystem must be uploaded first");
    return;
  }

  if (!loadHtmlTemplate()) {
    Serial.println("Failed to load HTML template");
    LittleFS.end();
    return;
  }
  LittleFS.end();

  if (startAP) {
    WiFi.disconnect(true);
    delay(1000);

    WiFi.mode(WIFI_AP_STA);
    bool apStarted = WiFi.softAP(wifiAccessPointName.c_str(),
                                 wifiAccessPointPassword.c_str());

    if (apStarted) {
      setupDNSServer();
      setupWebServer();
      isServerRunning = true;
      Serial.printf("AP mode ready at http://%s\n",
                    WiFi.softAPIP().toString().c_str());
    } else {
      Serial.println("Failed to start Access Point!");
    }
  } else {
    setupWebServer();
    isServerRunning = true;
    Serial.printf("Web server ready at http://%s\n",
                  WiFi.localIP().toString().c_str());
  }
}

void ConfigurationServer::stop() {
  if (isServerRunning) {
    if (server) {
      delete server;
      server = nullptr;
    }
    if (dnsServer) {
      dnsServer->stop();
      delete dnsServer;
      dnsServer = nullptr;
    }
    WiFi.softAPdisconnect(true);
    isServerRunning = false;
    Serial.println("Configuration server stopped");
  }
}

void ConfigurationServer::handleRequests() {
  if (isServerRunning && dnsServer) {
    dnsServer->processNextRequest();
  }
}

void ConfigurationServer::setupDNSServer() {
  dnsServer = new DNSServer();
  const byte DNS_PORT = 53;
  dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());
}

void ConfigurationServer::setupWebServer() {
  server = new AsyncWebServer(80);

  server->on("/generate_204", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });
  server->on("/fwlink", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });
  server->on("/hotspot-detect.html", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });
  server->on("/connectivity-check.html", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });

  server->on("/", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });
  server->on("/config", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });
  server->on("/save", HTTP_POST,
             [this](AsyncWebServerRequest *request) { handleSave(request); });

  server->on("/api/refresh-display", HTTP_POST,
             [this](AsyncWebServerRequest *request) {
               refreshRequested = true;
               request->send(200, "application/json", "{\"ok\":true}");
             });
  server->on("/api/reset-setup", HTTP_POST,
             [this](AsyncWebServerRequest *request) {
               bool clearPairing = false;
               if (request->hasParam("clearPairing", true)) {
                 clearPairing =
                     request->getParam("clearPairing", true)->value() == "1";
               }
               clearPairingOnReset = clearPairing;
               resetSetupRequested = true;
               request->send(200, "application/json",
                             "{\"ok\":true,\"restarting\":true}");
             });

  server->onNotFound(
      [this](AsyncWebServerRequest *request) { handleNotFound(request); });

  server->begin();
}

void ConfigurationServer::handleRoot(AsyncWebServerRequest *request) {
  request->send(200, "text/html", getConfigurationPage());
}

void ConfigurationServer::handleSave(AsyncWebServerRequest *request) {
  if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
    Configuration config;
    config.ssid = request->getParam("ssid", true)->value();
    config.password = request->getParam("password", true)->value();
    if (request->hasParam("powerMode", true)) {
      config.powerMode = request->getParam("powerMode", true)->value().toInt();
    }

    currentConfiguration = config;
    onSaveCallback(config);

    bool wifiConnected = false;
    String stationIp = "";
    const unsigned long connectTimeoutMs = 20000;
    WiFi.disconnect(false, false);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    const unsigned long connectStart = millis();
    while (millis() - connectStart < connectTimeoutMs) {
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        stationIp = WiFi.localIP().toString();
        break;
      }
      delay(250);
    }

    const String pairingUrl = currentConfiguration.pairingPageBaseUrl +
                              "?token=" + currentConfiguration.pairingToken;

    JsonDocument doc;
    doc["ok"] = true;
    doc["wifiConnected"] = wifiConnected;
    doc["pairingToken"] = currentConfiguration.pairingToken;
    doc["pairingUrl"] = pairingUrl;
    if (wifiConnected) {
      doc["stationIp"] = stationIp;
    }

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
    return;
  }

  request->send(400, "text/plain", "Missing parameters");
}

void ConfigurationServer::handleNotFound(AsyncWebServerRequest *request) {
  request->redirect("/");
}

bool ConfigurationServer::loadHtmlTemplate() {
  File file = LittleFS.open("/config.html", "r");
  if (!file) {
    Serial.println("Failed to open config.html file");
    return false;
  }

  htmlTemplate = file.readString();
  file.close();

  return htmlTemplate.length() > 0;
}

static void setSelected(String &html, const String &placeholder,
                        bool selected) {
  html.replace(placeholder, selected ? "selected" : "");
}

String ConfigurationServer::getConfigurationPage() {
  String html = htmlTemplate;
  html.replace("{{CURRENT_SSID}}", currentConfiguration.ssid);
  html.replace("{{CURRENT_PASSWORD}}", currentConfiguration.password);
  html.replace("{{PAIRING_TOKEN}}", currentConfiguration.pairingToken);
  html.replace("{{PAIRING_URL}}",
               currentConfiguration.pairingPageBaseUrl + "?token=" +
                   currentConfiguration.pairingToken);
  setSelected(html, "{{POWER_SEL_SLEEP}}", currentConfiguration.powerMode == 0);
  setSelected(html, "{{POWER_SEL_AWAKE}}", currentConfiguration.powerMode == 1);
  return html;
}

String ConfigurationServer::getWifiAccessPointName() const {
  return wifiAccessPointName;
}

String ConfigurationServer::getWifiAccessPointPassword() const {
  return wifiAccessPointPassword;
}

bool ConfigurationServer::isRunning() const { return isServerRunning; }

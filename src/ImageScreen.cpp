#include "ImageScreen.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {
constexpr const char *kBinaryCachePath = "/current.bin";
constexpr size_t kNativeBinarySize = (EPD_NATIVE_WIDTH * EPD_NATIVE_HEIGHT) / 2;
constexpr size_t kDownloadChunkSize = 2048;

void copyToFixedBuffer(char *dst, size_t dstSize, const String &src) {
  if (dstSize == 0) {
    return;
  }
  strncpy(dst, src.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}
} // namespace

ImageScreen::ImageScreen(DisplayType &display, ApplicationConfig &config,
                         ApplicationConfigStorage &configStorage,
                         bool forceFreshFetch)
    : display(display), config(config), configStorage(configStorage),
      forceFreshFetch(forceFreshFetch) {}

String ImageScreen::getStatusUrl() const {
  return String(DEVICE_SERVER_BASE_URL) + "/device/" + String(DEVICE_ID) +
         "/status.json";
}

String ImageScreen::getBinaryUrl() const {
  return String(DEVICE_SERVER_BASE_URL) + "/device/" + String(DEVICE_ID) +
         "/current.bin";
}

ImageScreen::StatusMetadata ImageScreen::fetchStatusMetadata() {
  StatusMetadata status;
  const String statusUrl = getStatusUrl();
  Serial.println("Status fetch: " + statusUrl);

  std::unique_ptr<WiFiClient> client;
  if (statusUrl.startsWith("https://")) {
    auto secureClient = new WiFiClientSecure;
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient);
  }

  HTTPClient http;
  http.begin(*client, statusUrl);
  http.setTimeout(15000);
  const char *headerKeys[] = {"ETag"};
  http.collectHeaders(headerKeys, 1);

  status.httpCode = http.GET();
  if (status.httpCode != HTTP_CODE_OK) {
    Serial.printf("Status fetch failed: HTTP %d\n", status.httpCode);
    http.end();
    return status;
  }

  const String body = http.getString();
  const String statusHeaderEtag = http.header("ETag");
  http.end();

  Serial.println("Status response body: " + body);

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Status parse failed: %s\n", err.c_str());
    return status;
  }

  bool hasParsedVersionInt = false;
  int parsedVersionInt = -1;
  JsonVariantConst versionVar = doc["version"];
  if (!versionVar.isNull()) {
    if (versionVar.is<int>()) {
      parsedVersionInt = versionVar.as<int>();
      hasParsedVersionInt = true;
    } else if (versionVar.is<long>()) {
      parsedVersionInt = static_cast<int>(versionVar.as<long>());
      hasParsedVersionInt = true;
    } else if (versionVar.is<const char *>()) {
      parsedVersionInt = atoi(versionVar.as<const char *>());
      hasParsedVersionInt = true;
    }
  }
  if (hasParsedVersionInt) {
    status.version = String(parsedVersionInt);
  }

  JsonVariantConst etagVar = doc["etag"];
  if (etagVar.is<const char *>()) {
    status.etag = String(etagVar.as<const char *>());
  }

  JsonVariantConst assetUrlVar = doc["assetUrl"];
  if (assetUrlVar.is<const char *>()) {
    status.assetUrl = String(assetUrlVar.as<const char *>());
  }

  if (status.etag.length() == 0) {
    status.etag = statusHeaderEtag;
  }

  Serial.printf("Status parsed version int: %d\n", parsedVersionInt);
  Serial.println("Status parsed version string: '" + status.version + "'");
  Serial.println("Status parsed etag: '" + status.etag + "'");
  Serial.println("Status parsed assetUrl: '" + status.assetUrl + "'");
  return status;
}

bool ImageScreen::isUpdateNeeded(const StatusMetadata &status) const {
  if (status.httpCode != HTTP_CODE_OK) {
    return false;
  }

  const String storedVersion = String(config.lastStatusVersion);
  const String storedEtag = String(config.lastStatusEtag);

  const bool hasVersion = status.version.length() > 0;
  const bool hasEtag = status.etag.length() > 0;

  bool changed = false;
  if (hasVersion && status.version != storedVersion) {
    changed = true;
  }
  if (hasEtag && status.etag != storedEtag) {
    changed = true;
  }
  if (!hasVersion && !hasEtag) {
    Serial.println("Update check: status has no version/etag; skipping update.");
    return false;
  }

  Serial.printf("Update check: stored version='%s', stored etag='%s'\n",
                config.lastStatusVersion, config.lastStatusEtag);
  Serial.println(changed ? "Update check: update needed"
                         : "Update check: no update needed");
  return changed;
}

bool ImageScreen::downloadBinaryToLittleFS(const String &url,
                                           const String &ifNoneMatch) {
  const String binaryUrl = url.length() > 0 ? url : getBinaryUrl();
  Serial.println(String("Binary path audit [download target]: ") + kBinaryCachePath);
  Serial.println("Binary download URL: " + binaryUrl);
  Serial.println(String("Binary target path: ") + kBinaryCachePath);

  const bool fsMounted = LittleFS.begin(true);
  Serial.printf("LittleFS mount for binary download: %s\n",
                fsMounted ? "success" : "failed");
  if (!fsMounted) {
    return false;
  }

  std::unique_ptr<WiFiClient> client;
  if (binaryUrl.startsWith("https://")) {
    auto secureClient = new WiFiClientSecure;
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient);
  }

  HTTPClient http;
  http.begin(*client, binaryUrl);
  http.setTimeout(30000);
  if (ifNoneMatch.length() > 0) {
    http.addHeader("If-None-Match", ifNoneMatch);
  }

  const int httpCode = http.GET();
  if (httpCode == HTTP_CODE_NOT_MODIFIED) {
    Serial.println("Binary download: server returned 304 Not Modified.");
    http.end();
    return false;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Binary download failed: HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  if (LittleFS.exists(kBinaryCachePath)) {
    Serial.println(String("Binary path audit [exists check]: ") + kBinaryCachePath);
    LittleFS.remove(kBinaryCachePath);
  }

  Serial.println(String("Binary path audit [open write]: ") + kBinaryCachePath);
  File out = LittleFS.open(kBinaryCachePath, "w");
  if (!out) {
    Serial.println("Binary download failed: file open failed");
    http.end();
    return false;
  }
  Serial.println("Binary download: file open success");

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[kDownloadChunkSize];
  size_t totalWritten = 0;

  while (http.connected() || stream->available()) {
    const size_t available = stream->available();
    const size_t toRead = available > 0 ? min(available, kDownloadChunkSize)
                                        : kDownloadChunkSize;
    const size_t bytesRead = stream->readBytes(buffer, toRead);
    if (bytesRead == 0) {
      if (!http.connected()) {
        break;
      }
      delay(5);
      continue;
    }

    const size_t bytesWritten = out.write(buffer, bytesRead);
    if (bytesWritten != bytesRead) {
      Serial.printf("Binary download failed: short write (%u/%u)\n",
                    (unsigned int)bytesWritten, (unsigned int)bytesRead);
      out.close();
      Serial.println(String("Binary path audit [cleanup on error]: ") + kBinaryCachePath);
      http.end();
      LittleFS.remove(kBinaryCachePath);
      return false;
    }

    totalWritten += bytesWritten;
  }

  Serial.printf("Binary file write complete: %u bytes\n",
                (unsigned int)totalWritten);
  out.flush();
  out.close();
  const bool closeResult = !out;
  Serial.printf("Binary file close complete: %s\n",
                closeResult ? "success" : "failed");
  http.end();

  Serial.printf("Binary download completed: %u bytes\n", (unsigned int)totalWritten);
  if (totalWritten != kNativeBinarySize) {
    Serial.printf("Binary download failed: expected %u bytes\n",
                  (unsigned int)kNativeBinarySize);
    Serial.println(String("Binary path audit [cleanup size mismatch]: ") + kBinaryCachePath);
    LittleFS.remove(kBinaryCachePath);
    return false;
  }

  return true;
}

bool ImageScreen::renderBinaryFromLittleFS() {
  Serial.println(String("Binary path audit [render open read]: ") + kBinaryCachePath);
  Serial.println(String("Render start from ") + kBinaryCachePath);
  File in = LittleFS.open(kBinaryCachePath, FILE_READ);
  if (!in) {
    Serial.println("Render failure: unable to open cached binary");
    return false;
  }

  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  display.setFullWindow();

  const bool loaded = display.loadNativeFrameBuffer(in, kNativeBinarySize);
  in.close();

  if (!loaded) {
    Serial.println("Render failure: failed loading binary framebuffer");
    return false;
  }

  display.display();
  display.hibernate();
  Serial.println("Render success");
  return true;
}

void ImageScreen::persistStatusMetadata(const StatusMetadata &status) {
  copyToFixedBuffer(config.lastStatusVersion, sizeof(config.lastStatusVersion),
                    status.version);
  copyToFixedBuffer(config.lastStatusEtag, sizeof(config.lastStatusEtag),
                    status.etag);

  if (!configStorage.save(config)) {
    Serial.println("Persist metadata: failed saving status metadata");
  } else {
    Serial.println("Persist metadata: saved version/etag to NVS");
  }
}

void ImageScreen::render() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Render skipped: WiFi not connected");
    return;
  }

  const bool fsMounted = LittleFS.begin(true);
  Serial.printf("LittleFS mount in render(): %s\n",
                fsMounted ? "success" : "failed");
  if (!fsMounted) {
    Serial.println("Render skipped: LittleFS mount failed");
    return;
  }

  if (forceFreshFetch) {
    Serial.println("Manual refresh: forcing immediate status check.");
  }

  StatusMetadata status = fetchStatusMetadata();
  if (status.httpCode != HTTP_CODE_OK) {
    Serial.println("Render skipped: status fetch did not succeed");
    LittleFS.end();
    return;
  }

  const bool needsUpdate = isUpdateNeeded(status);
  if (!needsUpdate) {
    Serial.println("Update needed: no (keeping current display)");
    LittleFS.end();
    return;
  }

  const String binaryUrl =
      status.assetUrl.length() > 0 ? status.assetUrl : getBinaryUrl();
  Serial.println("Update needed: yes (downloading current.bin)");
  if (!downloadBinaryToLittleFS(binaryUrl, String(config.lastStatusEtag))) {
    Serial.println("Render skipped: binary download failed");
    LittleFS.end();
    return;
  }

  const bool rendered = renderBinaryFromLittleFS();
  if (rendered) {
    persistStatusMetadata(status);
  } else {
    Serial.println("Render failure");
  }
  Serial.println(String("Binary path audit [final cleanup]: ") + kBinaryCachePath);
  LittleFS.remove(kBinaryCachePath);
  LittleFS.end();
}

int ImageScreen::nextRefreshInSeconds() { return 1800; }

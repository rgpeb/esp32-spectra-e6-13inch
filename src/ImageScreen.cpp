#include "ImageScreen.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

namespace {
constexpr const char *kBinaryCachePath = "/current.bin";
constexpr size_t kNativeBinarySize = (EPD_NATIVE_WIDTH * EPD_NATIVE_HEIGHT) / 2;
constexpr size_t kDownloadChunkSize = 2048;
constexpr unsigned long kBinaryStreamStallTimeoutMs = 15000;

void copyToFixedBuffer(char *dst, size_t dstSize, const String &src) {
  if (dstSize == 0) {
    return;
  }
  strncpy(dst, src.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

void logBinaryPathStage(const char *functionName, const char *stage,
                        const char *path) {
  Serial.printf("[%s] %s path: %s\n", functionName, stage, path);
}

void addLocalPortalHeaders(HTTPClient &http) {
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
} // namespace

ImageScreen::ImageScreen(DisplayType &display, ApplicationConfig &config,
                         ApplicationConfigStorage &configStorage,
                         bool forceFreshFetch)
    : display(display), config(config), configStorage(configStorage),
      forceFreshFetch(forceFreshFetch) {}

String ImageScreen::getStatusUrl() const {
  return String(DEVICE_SERVER_BASE_URL) + "/device/" + getResolvedDeviceId() +
         "/status.json";
}

String ImageScreen::getBinaryUrl() const {
  return String(DEVICE_SERVER_BASE_URL) + "/device/" + getResolvedDeviceId() +
         "/current.bin";
}

String ImageScreen::getResolvedDeviceId() const {
  if (config.hasAssignedDeviceId()) {
    return String(config.assignedDeviceId);
  }
  return String(DEVICE_ID);
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
  addLocalPortalHeaders(http);
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
  JsonVariantConst imageIdVar = doc["imageId"];
  if (imageIdVar.is<const char *>()) {
    status.imageId = String(imageIdVar.as<const char *>());
  }
  JsonVariantConst photoNameVar = doc["photoName"];
  if (photoNameVar.is<const char *>()) {
    status.photoName = String(photoNameVar.as<const char *>());
  } else {
    JsonVariantConst nameVar = doc["name"];
    if (nameVar.is<const char *>()) {
      status.photoName = String(nameVar.as<const char *>());
    }
  }

  if (status.etag.length() == 0) {
    status.etag = statusHeaderEtag;
  }

  JsonVariantConst rotationVar = doc["rotationDegrees"];
  if (rotationVar.is<int>()) {
    status.rotationDegrees = rotationVar.as<int>();
  } else if (rotationVar.is<const char *>()) {
    status.rotationDegrees = atoi(rotationVar.as<const char *>());
  } else {
    JsonVariantConst orientationVar = doc["orientation"];
    if (orientationVar.is<const char *>()) {
      const String orientation = String(orientationVar.as<const char *>());
      if (orientation.equalsIgnoreCase("portrait")) {
        status.rotationDegrees = 0;
      } else if (orientation.equalsIgnoreCase("portrait-180")) {
        status.rotationDegrees = 180;
      } else if (orientation.equalsIgnoreCase("landscape") ||
                 orientation.equalsIgnoreCase("landscape-left")) {
        status.rotationDegrees = 90;
      } else if (orientation.equalsIgnoreCase("landscape-right")) {
        status.rotationDegrees = 270;
      }
    }
  }

  while (status.rotationDegrees < 0) {
    status.rotationDegrees += 360;
  }
  status.rotationDegrees %= 360;

  Serial.printf("Status parsed version int: %d\n", parsedVersionInt);
  Serial.println("Status parsed version string: '" + status.version + "'");
  Serial.println("Status parsed etag: '" + status.etag + "'");
  Serial.println("Status parsed assetUrl: '" + status.assetUrl + "'");
  Serial.println("Status parsed imageId: '" + status.imageId + "'");
  Serial.println("Status parsed photoName: '" + status.photoName + "'");
  Serial.printf("Status parsed rotationDegrees: %d\n", status.rotationDegrees);
  return status;
}

bool ImageScreen::isUpdateNeeded(const StatusMetadata &status) const {
  if (status.httpCode != HTTP_CODE_OK) {
    return false;
  }
  if (forceFreshFetch) {
    Serial.println(
        "Update check: forced fresh fetch enabled, downloading latest binary now.");
    return true;
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
  logBinaryPathStage(__func__, "download target", kBinaryCachePath);
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
  const char *headerKeys[] = {"Content-Length"};
  http.collectHeaders(headerKeys, 1);
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

  logBinaryPathStage(__func__, "pre-open remove", kBinaryCachePath);
  if (!LittleFS.remove(kBinaryCachePath)) {
    Serial.println("[downloadBinaryToLittleFS] pre-open remove: file did not exist");
  }

  logBinaryPathStage(__func__, "open write", kBinaryCachePath);
  File out = LittleFS.open(kBinaryCachePath, "w");
  if (!out) {
    Serial.println("Binary download failed: file open failed");
    http.end();
    return false;
  }
  Serial.println("Binary download: file open success");

  WiFiClient *stream = http.getStreamPtr();
  const String contentLengthHeader = http.header("Content-Length");
  const int reportedContentLength = http.getSize();
  size_t expectedContentLength = 0;
  bool hasExpectedContentLength = false;
  if (contentLengthHeader.length() > 0) {
    const long parsedContentLength = atol(contentLengthHeader.c_str());
    if (parsedContentLength > 0) {
      expectedContentLength = static_cast<size_t>(parsedContentLength);
      hasExpectedContentLength = true;
    }
  } else if (reportedContentLength > 0) {
    expectedContentLength = static_cast<size_t>(reportedContentLength);
    hasExpectedContentLength = true;
  }
  if (hasExpectedContentLength) {
    Serial.printf(
        "Binary download: Content-Length=%u bytes (header='%s', http.getSize()=%d)\n",
        (unsigned int)expectedContentLength, contentLengthHeader.c_str(),
        reportedContentLength);
  } else {
    Serial.printf(
        "Binary download: Content-Length missing/unknown (header='%s', http.getSize()=%d), using stall timeout fallback\n",
        contentLengthHeader.c_str(), reportedContentLength);
  }

  uint8_t buffer[kDownloadChunkSize];
  size_t totalWritten = 0;
  unsigned long lastProgressMs = millis();
  const unsigned long downloadLoopStartMs = lastProgressMs;
  bool abortedForStall = false;
  String loopExitReason = "loop condition false";

  while (hasExpectedContentLength ? (totalWritten < expectedContentLength)
                                  : (http.connected() || stream->available())) {
    const size_t available = stream->available();
    const size_t toRead = available > 0 ? min(available, kDownloadChunkSize)
                                        : kDownloadChunkSize;
    const bool connectedBeforeRead = http.connected();
    const size_t bytesRead = stream->readBytes(buffer, toRead);
    Serial.printf(
        "Binary download loop: available=%u bytesRead=%u totalWritten=%u connected=%s\n",
        (unsigned int)available, (unsigned int)bytesRead,
        (unsigned int)totalWritten, connectedBeforeRead ? "true" : "false");
    if (bytesRead == 0) {
      if (!http.connected()) {
        loopExitReason = "stream produced 0 bytes and HTTP disconnected";
        break;
      }
      const unsigned long nowMs = millis();
      const unsigned long stalledMs = nowMs - lastProgressMs;
      if (stalledMs >= kBinaryStreamStallTimeoutMs) {
        Serial.printf(
            "Binary download aborted: no bytes read for %lu ms (timeout %lu ms)\n",
            stalledMs, kBinaryStreamStallTimeoutMs);
        loopExitReason = "stalled: stream stopped producing bytes";
        abortedForStall = true;
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
      logBinaryPathStage(__func__, "cleanup on error", kBinaryCachePath);
      http.end();
      LittleFS.remove(kBinaryCachePath);
      return false;
    }

    totalWritten += bytesWritten;
    lastProgressMs = millis();
  }

  if (loopExitReason == "loop condition false") {
    if (hasExpectedContentLength && totalWritten >= expectedContentLength) {
      loopExitReason = "reached expected Content-Length";
      Serial.printf(
          "Binary download loop exit: reached expected Content-Length (%u/%u bytes)\n",
          (unsigned int)totalWritten, (unsigned int)expectedContentLength);
    } else {
      loopExitReason = "HTTP disconnected and stream had no available bytes";
    }
  }
  Serial.printf(
      "Binary download loop exit: reason='%s', totalWritten=%u, elapsedMs=%lu\n",
      loopExitReason.c_str(), (unsigned int)totalWritten,
      millis() - downloadLoopStartMs);

  if (abortedForStall) {
    out.close();
    Serial.println("[downloadBinaryToLittleFS] file close complete");
    http.end();
    logBinaryPathStage(__func__, "cleanup stall timeout", kBinaryCachePath);
    LittleFS.remove(kBinaryCachePath);
    return false;
  }

  Serial.printf("Binary file write complete: %u bytes\n",
                (unsigned int)totalWritten);
  out.flush();
  Serial.println("[downloadBinaryToLittleFS] file flush complete");
  out.close();
  Serial.println("[downloadBinaryToLittleFS] file close complete");
  http.end();

  Serial.printf("Binary download completed: %u bytes\n", (unsigned int)totalWritten);
  if (totalWritten != kNativeBinarySize) {
    Serial.printf("Binary download failed: expected %u bytes\n",
                  (unsigned int)kNativeBinarySize);
    logBinaryPathStage(__func__, "cleanup size mismatch", kBinaryCachePath);
    LittleFS.remove(kBinaryCachePath);
    return false;
  }

  return true;
}

bool ImageScreen::renderBinaryFromLittleFS(const StatusMetadata &status) {
  logBinaryPathStage(__func__, "render open read", kBinaryCachePath);
  Serial.println(String("Render start from ") + kBinaryCachePath);
  File in = LittleFS.open(kBinaryCachePath, FILE_READ);
  if (!in) {
    Serial.println("Render failure: unable to open cached binary");
    return false;
  }

  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  display.setFullWindow();

  logBinaryPathStage(__func__, "decode framebuffer read", kBinaryCachePath);
  uint8_t rotationTurnsCW = 0;
  if (status.rotationDegrees == 180) {
    rotationTurnsCW = 2;
  } else if (status.rotationDegrees == 90 || status.rotationDegrees == 270) {
    Serial.printf(
        "Render failure: unsupported rotationDegrees=%d for native 1200x1600 binary. Server must pre-rotate to portrait layout.\n",
        status.rotationDegrees);
    in.close();
    return false;
  }

  const bool loaded = display.loadNativeFrameBuffer(in, kNativeBinarySize,
                                                     rotationTurnsCW);
  in.close();

  if (!loaded) {
    Serial.println("Render failure: failed loading binary framebuffer");
    return false;
  }

  Serial.println("Render path: pushing new image with forced full refresh.");
  display.display(false);
  display.hibernate();
  Serial.println("[renderBinaryFromLittleFS] render success");
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

void ImageScreen::persistAppliedState(const StatusMetadata &status) {
  copyToFixedBuffer(config.lastAppliedVersion, sizeof(config.lastAppliedVersion),
                    status.version);

  const String appliedImageId =
      status.imageId.length() > 0 ? status.imageId : status.photoName;
  copyToFixedBuffer(config.lastAppliedImageId, sizeof(config.lastAppliedImageId),
                    appliedImageId);
  copyToFixedBuffer(config.lastAppliedPhotoName, sizeof(config.lastAppliedPhotoName),
                    status.photoName);

  const time_t now = time(nullptr);
  if (now > 0) {
    config.lastSyncEpoch = static_cast<uint32_t>(now);
  } else {
    config.lastSyncEpoch = 0;
  }

  if (!configStorage.save(config)) {
    Serial.println("Persist applied state: failed saving last applied state");
  } else {
    Serial.println("Persist applied state: saved applied version/image/sync time");
  }
}

ImageScreen::RefreshResult ImageScreen::refresh() {
  RefreshResult result;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Render skipped: WiFi not connected");
    return result;
  }

  const bool fsMounted = LittleFS.begin(true);
  Serial.printf("LittleFS mount in render(): %s\n",
                fsMounted ? "success" : "failed");
  if (!fsMounted) {
    Serial.println("Render skipped: LittleFS mount failed");
    return result;
  }

  if (forceFreshFetch) {
    Serial.println("Manual refresh: forcing immediate status check.");
  }

  StatusMetadata status = fetchStatusMetadata();
  result.statusFetchSucceeded = (status.httpCode == HTTP_CODE_OK);
  result.serverVersion = status.version;
  if (status.httpCode != HTTP_CODE_OK) {
    Serial.println("Render skipped: status fetch did not succeed");
    LittleFS.end();
    return result;
  }

  const bool needsUpdate = isUpdateNeeded(status);
  result.updatePending = needsUpdate;
  if (!needsUpdate) {
    Serial.println("Update needed: no (keeping current display)");
    LittleFS.end();
    return result;
  }

  const String binaryUrl =
      status.assetUrl.length() > 0 ? status.assetUrl : getBinaryUrl();
  Serial.println("Update needed: yes (downloading current.bin)");
  if (!downloadBinaryToLittleFS(binaryUrl, String(config.lastStatusEtag))) {
    Serial.println("Render skipped: binary download failed");
    LittleFS.end();
    return result;
  }

  const bool rendered = renderBinaryFromLittleFS(status);
  if (rendered) {
    persistStatusMetadata(status);
    persistAppliedState(status);
  } else {
    Serial.println("Render failure");
  }
  logBinaryPathStage(__func__, "final cleanup", kBinaryCachePath);
  LittleFS.remove(kBinaryCachePath);
  LittleFS.end();
  result.rendered = rendered;
  result.updatePending = !rendered;
  return result;
}

bool ImageScreen::renderAndReport() {
  return refresh().rendered;
}

void ImageScreen::render() {
  renderAndReport();
}

int ImageScreen::nextRefreshInSeconds() { return 1800; }

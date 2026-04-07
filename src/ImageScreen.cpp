#include "ImageScreen.h"

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

bool ImageScreen::parseJsonStringField(const String &json, const char *field,
                                       String &outValue) const {
  const String key = String("\"") + field + "\"";
  const int keyPos = json.indexOf(key);
  if (keyPos < 0) {
    return false;
  }

  const int colonPos = json.indexOf(':', keyPos + key.length());
  if (colonPos < 0) {
    return false;
  }

  int valueStart = json.indexOf('"', colonPos + 1);
  if (valueStart < 0) {
    return false;
  }
  valueStart += 1;

  const int valueEnd = json.indexOf('"', valueStart);
  if (valueEnd < 0) {
    return false;
  }

  outValue = json.substring(valueStart, valueEnd);
  return true;
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

  String parsedVersion;
  String parsedBodyEtag;
  parseJsonStringField(body, "version", parsedVersion);
  parseJsonStringField(body, "etag", parsedBodyEtag);

  status.version = parsedVersion;
  if (parsedBodyEtag.length() > 0) {
    status.etag = parsedBodyEtag;
  } else {
    status.etag = statusHeaderEtag;
  }

  Serial.println("Status parsed version: '" + status.version + "'");
  Serial.println("Status parsed etag: '" + status.etag + "'");
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

bool ImageScreen::downloadBinaryToLittleFS(const String &path,
                                           const String &ifNoneMatch) {
  const String binaryUrl = getBinaryUrl();
  Serial.println("Binary download: " + binaryUrl);

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

  if (LittleFS.exists(path)) {
    LittleFS.remove(path);
  }

  File out = LittleFS.open(path, FILE_WRITE);
  if (!out) {
    Serial.println("Binary download failed: cannot open cache file");
    http.end();
    return false;
  }

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
      http.end();
      LittleFS.remove(path);
      return false;
    }

    totalWritten += bytesWritten;
  }

  out.flush();
  out.close();
  http.end();

  Serial.printf("Binary download completed: %u bytes\n", (unsigned int)totalWritten);
  if (totalWritten != kNativeBinarySize) {
    Serial.printf("Binary download failed: expected %u bytes\n",
                  (unsigned int)kNativeBinarySize);
    LittleFS.remove(path);
    return false;
  }

  return true;
}

bool ImageScreen::renderBinaryFromLittleFS(const String &path) {
  File in = LittleFS.open(path, FILE_READ);
  if (!in) {
    Serial.println("Render failed: unable to open cached binary");
    return false;
  }

  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  display.setFullWindow();

  const bool loaded = display.loadNativeFrameBuffer(in, kNativeBinarySize);
  in.close();

  if (!loaded) {
    Serial.println("Render result: failed loading binary framebuffer");
    return false;
  }

  display.display();
  display.hibernate();
  Serial.println("Render result: success");
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

  if (!LittleFS.begin(true)) {
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

  Serial.println("Update needed: yes (downloading current.bin)");
  if (!downloadBinaryToLittleFS(kBinaryCachePath, String(config.lastStatusEtag))) {
    Serial.println("Render skipped: binary download failed");
    LittleFS.end();
    return;
  }

  const bool rendered = renderBinaryFromLittleFS(kBinaryCachePath);
  if (rendered) {
    persistStatusMetadata(status);
  }
  LittleFS.remove(kBinaryCachePath);
  LittleFS.end();
}

int ImageScreen::nextRefreshInSeconds() { return 1800; }

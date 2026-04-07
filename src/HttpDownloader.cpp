#include "HttpDownloader.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>

namespace {
constexpr const char *kRemoteImageCachePath = "/remote_image_download.img";
constexpr size_t kDownloadChunkSize = 2048;

String normalizeLittleFSPath(const char *path) {
  if (path == nullptr) {
    return String("/");
  }
  String normalized(path);
  if (!normalized.startsWith("/")) {
    normalized = "/" + normalized;
  }
  return normalized;
}
}

HttpDownloader::HttpDownloader() {}

HttpDownloader::~HttpDownloader() {}

std::unique_ptr<DownloadResult>
HttpDownloader::download(const String &url, const String &cachedETag) {
  HTTPClient http;

  std::unique_ptr<WiFiClient> client;
  if (url.startsWith("https://")) {
    auto secureClient = new WiFiClientSecure;
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient);
  }
  Serial.printf("Net: Free Heap: %d, Free PSRAM: %d\n", ESP.getFreeHeap(),
                ESP.getFreePsram());
  Serial.printf("Folder Listing Net: Free Heap: %d, Free PSRAM: %d\n",
                ESP.getFreeHeap(), ESP.getFreePsram());
  http.begin(*client, url);

  auto result = std::unique_ptr<DownloadResult>(new DownloadResult());

  Serial.println("Requesting data from: " + url);

  http.setTimeout(15000); // Increased timeout

  if (cachedETag.length() > 0) {
    http.addHeader("If-None-Match", cachedETag);
  }
  http.addHeader("Accept", "image/jpeg,image/jpg,image/png,image/*;q=0.8");

  const char *headerKeys[] = {"Content-Type", "Content-Length",
                              "Transfer-Encoding", "ETag"};
  size_t headerKeysSize = sizeof(headerKeys) / sizeof(char *);
  http.collectHeaders(headerKeys, headerKeysSize);

  int httpCode = http.GET();
  result->httpCode = httpCode;

  if (httpCode == HTTP_CODE_NOT_MODIFIED) {
    Serial.println("Content not modified (304), using cached version");
    http.end();
    return result;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP request failed with code: %d\n", httpCode);
    Serial.printf("HTTP error: %s\n", http.errorToString(httpCode).c_str());
    result->errorDetail = "HTTP request failed before download body";
    http.end();
    return result;
  }

  String newETag = http.header("ETag");
  String contentType = http.header("Content-Type");
  int contentLength = http.getSize();
  result->contentType = contentType;

  Serial.printf("HTTP success: %d\n", httpCode);
  Serial.printf("HTTP content type: %s\n", contentType.c_str());
  Serial.printf("HTTP declared content length: %d\n", contentLength);

  contentType.toLowerCase();
  if (!contentType.isEmpty() && contentType.indexOf("image") == -1 &&
      contentType.indexOf("text/html") == -1) {
    Serial.println("Unexpected content type: " + contentType);
    http.end();
    result->httpCode = -1;
    result->errorDetail = "Unexpected non-image content type";
    return result;
  }

  if (!LittleFS.begin(true)) {
    Serial.println(
        "LittleFS mount failed: failed to prepare remote download cache");
    result->httpCode = -1;
    result->errorDetail = "LittleFS mount failed while preparing download cache";
    http.end();
    return result;
  }
  Serial.printf("LittleFS mount success: total=%u used=%u\n",
                (unsigned int)LittleFS.totalBytes(),
                (unsigned int)LittleFS.usedBytes());

  const String cachePath = normalizeLittleFSPath(kRemoteImageCachePath);
  Serial.println("Temp file open path: " + cachePath);

  if (LittleFS.exists(cachePath.c_str())) {
    LittleFS.remove(cachePath.c_str());
  }

  fs::File outputFile = LittleFS.open(cachePath.c_str(), "w");
  if (!outputFile) {
    Serial.println("Temp file open failed: unable to create download cache");
    result->httpCode = -1;
    result->errorDetail = "Failed to open LittleFS cache file for HTTP body";
    http.end();
    return result;
  }
  Serial.println("Temp file open success");

  WiFiClient *stream = http.getStreamPtr();
  uint8_t ioBuffer[kDownloadChunkSize];
  size_t totalWritten = 0;

  while (http.connected() || stream->available()) {
    const size_t available = stream->available();
    const size_t bytesToRead =
        available > 0 ? min(available, kDownloadChunkSize) : kDownloadChunkSize;
    const size_t bytesRead = stream->readBytes(ioBuffer, bytesToRead);
    if (bytesRead == 0) {
      if (!http.connected()) {
        break;
      }
      delay(5);
      continue;
    }
    const size_t bytesWritten = outputFile.write(ioBuffer, bytesRead);
    if (bytesWritten != bytesRead) {
      Serial.printf("Render aborted safely: file write short (%u/%u)\n",
                    (unsigned int)bytesWritten, (unsigned int)bytesRead);
      result->httpCode = -1;
      result->errorDetail = "LittleFS write failed while streaming HTTP body";
      break;
    }
    totalWritten += bytesWritten;
    Serial.printf("Bytes written so far: %u\n", (unsigned int)totalWritten);
  }
  outputFile.flush();
  outputFile.close();
  Serial.println("Temp file close success");

  http.end();
  if (result->httpCode == -1) {
    LittleFS.remove(cachePath.c_str());
  } else {
    result->size = totalWritten;
    result->filePath = cachePath;
    Serial.printf("HTTP body streamed to LittleFS: %u bytes\n",
                  (unsigned int)totalWritten);
    Serial.printf("File saved successfully: %s\n", result->filePath.c_str());
    result->httpCode = httpCode;
  }

  if (result->httpCode < 0) {
    Serial.printf("Download pipeline failed after HTTP %d (internal error %d)\n",
                  httpCode, result->httpCode);
    if (result->errorDetail.length() > 0) {
      Serial.println("Download failure reason: " + result->errorDetail);
    }
  }
  result->etag = newETag;
  result->contentType = contentType;

  if (result->size > 0) {
    Serial.printf("Downloaded %d bytes\n", result->size);
  } else {
    Serial.println("Warning: Downloaded 0 bytes");
  }

  return result;
}

String HttpDownloader::urlEncode(const String &str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      if (c < 16)
        encoded += '0';
      encoded += String(c, HEX);
    }
  }
  return encoded;
}

#pragma once

#include <Arduino.h>
#include <HTTPClient.h>

#include <memory>

struct DownloadResult {
  uint8_t* data;
  size_t size;
  int httpCode;
  String etag;
  String contentType;
  String errorDetail;
  String filePath;

  DownloadResult()
      : data(nullptr), size(0), httpCode(0), etag(""), contentType(""),
        errorDetail(""), filePath("") {}

  ~DownloadResult() {
    if (data != nullptr) {
      free(data);
    }
  }
};

class HttpDownloader {
 public:
  HttpDownloader();
  ~HttpDownloader();

  std::unique_ptr<DownloadResult> download(const String& url, const String& cachedETag = "");
  String urlEncode(const String& str);
};

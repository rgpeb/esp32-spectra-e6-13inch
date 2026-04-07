#ifndef IMAGE_SCREEN_H
#define IMAGE_SCREEN_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <memory>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "DisplayAdapter.h"
#include "Screen.h"

class ImageScreen : public Screen {
private:
  DisplayType &display;
  ApplicationConfig &config;
  ApplicationConfigStorage &configStorage;
  bool forceFreshFetch;

  struct StatusMetadata {
    String version;
    String etag;
    String assetUrl;
    int httpCode = -1;
  };

  String getStatusUrl() const;
  String getBinaryUrl() const;
  StatusMetadata fetchStatusMetadata();
  bool isUpdateNeeded(const StatusMetadata &status) const;
  bool downloadBinaryToLittleFS(const String &url,
                                const String &ifNoneMatch = "");
  bool renderBinaryFromLittleFS();
  void persistStatusMetadata(const StatusMetadata &status);

public:
  ImageScreen(DisplayType &display, ApplicationConfig &config,
              ApplicationConfigStorage &configStorage,
              bool forceFreshFetch = false);
  void render() override;
  int nextRefreshInSeconds() override;
};

#endif

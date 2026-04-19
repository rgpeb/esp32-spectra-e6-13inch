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
  String getResolvedDeviceId() const;

  struct StatusMetadata {
    String version;
    String etag;
    String assetUrl;
    String imageId;
    String photoName;
    int rotationDegrees = 0;
    int httpCode = -1;
  };

public:
  struct RefreshResult {
    bool rendered = false;
    bool statusFetchSucceeded = false;
    bool updatePending = false;
    String serverVersion;
  };

private:

  String getStatusUrl() const;
  String getBinaryUrl() const;
  StatusMetadata fetchStatusMetadata();
  bool isUpdateNeeded(const StatusMetadata &status) const;
  bool downloadBinaryToLittleFS(const String &url,
                                const String &ifNoneMatch = "");
  bool renderBinaryFromLittleFS(const StatusMetadata &status);
  void persistStatusMetadata(const StatusMetadata &status);
  void persistAppliedState(const StatusMetadata &status);

public:
  ImageScreen(DisplayType &display, ApplicationConfig &config,
              ApplicationConfigStorage &configStorage,
              bool forceFreshFetch = false);
  RefreshResult refresh();
  bool renderAndReport();
  void render() override;
  int nextRefreshInSeconds() override;
};

#endif

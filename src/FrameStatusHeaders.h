#ifndef FRAME_STATUS_HEADERS_H
#define FRAME_STATUS_HEADERS_H

#include <Arduino.h>
#include <HTTPClient.h>

#include "ApplicationConfig.h"

void addFrameStatusHeaders(HTTPClient &http, const ApplicationConfig &config,
                           const String &resolvedDeviceId);

#endif

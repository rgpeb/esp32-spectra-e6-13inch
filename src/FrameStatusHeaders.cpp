#include "FrameStatusHeaders.h"

#include <WiFi.h>
#include <time.h>

#include "FirmwareInfo.h"

void addFrameStatusHeaders(HTTPClient &http, const ApplicationConfig &config,
                           const String &resolvedDeviceId) {
  http.addHeader("X-Frame-Device-Id", resolvedDeviceId);
  http.addHeader("X-Frame-Firmware-Version", firmwareVersion());
  http.addHeader("X-Frame-Wifi-Connected",
                 WiFi.status() == WL_CONNECTED ? "true" : "false");
  http.addHeader("X-Frame-Account-Linked",
                 config.hasAssignedDeviceId() ? "true" : "false");

  const time_t now = time(nullptr);
  if (now > 0) {
    http.addHeader("X-Frame-Check-In-Epoch", String(static_cast<uint32_t>(now)));
  } else {
    http.addHeader("X-Frame-Check-In-Millis", String(millis()));
  }

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

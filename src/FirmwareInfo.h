#ifndef FIRMWARE_INFO_H
#define FIRMWARE_INFO_H

#include <Arduino.h>

#ifndef FRAME_FIRMWARE_VERSION
#define FRAME_FIRMWARE_VERSION "dev"
#endif

inline String firmwareVersion() { return String(FRAME_FIRMWARE_VERSION); }

#endif

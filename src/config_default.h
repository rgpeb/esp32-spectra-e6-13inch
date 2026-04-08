#ifndef CONFIG_DEFAULT_H
#define CONFIG_DEFAULT_H

// Default configuration values (safe to commit to repository)
// For development, create config_dev.h with your actual credentials

const char DEFAULT_WIFI_SSID[] = "";
const char DEFAULT_WIFI_PASSWORD[] = "";

// Dedicated appliance update target (binary-first flow)
const char DEVICE_SERVER_BASE_URL[] =
    "https://photo-host-production.up.railway.app";
const char DEVICE_ID[] = "esp32-spectra-e6-13inch";
const char PAIRING_PAGE_BASE_URL[] =
    "https://photo-host-production.up.railway.app/pair";
const char PAIRING_STATUS_PATH[] = "/pairing/status";

#endif  // CONFIG_DEFAULT_H

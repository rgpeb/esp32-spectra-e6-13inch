#include "ApplicationConfigStorage.h"

#include <nvs.h>
#include <nvs_flash.h>

const char* ApplicationConfigStorage::NVS_NAMESPACE = "weather_config";
const char* ApplicationConfigStorage::CONFIG_KEY = "app_config";
const char* ApplicationConfigStorage::CONFIG_VERSION_KEY = "cfg_ver";
const uint32_t ApplicationConfigStorage::CONFIG_VERSION = 3;
const char* ApplicationConfigStorage::KEY_WIFI_SSID = "wifi_ssid";
const char* ApplicationConfigStorage::KEY_WIFI_PASSWORD = "wifi_pass";
const char* ApplicationConfigStorage::KEY_DITHER_MODE = "dither";
const char* ApplicationConfigStorage::KEY_SCALING_MODE = "scale";
const char* ApplicationConfigStorage::KEY_POWER_MODE = "pwr";
const char* ApplicationConfigStorage::KEY_CHECK_FOR_NEW_IMAGE_MODE = "img_chk";
const char* ApplicationConfigStorage::KEY_SLEEP_MINUTES = "sleep_min";
const char* ApplicationConfigStorage::KEY_LAST_STATUS_VERSION = "st_ver";
const char* ApplicationConfigStorage::KEY_LAST_STATUS_ETAG = "st_etag";
const char* ApplicationConfigStorage::KEY_LAST_APPLIED_VERSION = "ap_ver";
const char* ApplicationConfigStorage::KEY_LAST_APPLIED_IMAGE_ID = "ap_img";
const char* ApplicationConfigStorage::KEY_LAST_APPLIED_PHOTO_NAME = "ap_photo";
const char* ApplicationConfigStorage::KEY_LAST_SYNC_EPOCH = "sync_epoch";
const char* ApplicationConfigStorage::KEY_PAIRING_TOKEN = "pair_token";
const char* ApplicationConfigStorage::KEY_ASSIGNED_DEVICE_ID = "device_id";

namespace {
bool setString(nvs_handle_t handle, const char* key, const char* value) {
  return nvs_set_str(handle, key, value) == ESP_OK;
}

void getStringOrKeep(nvs_handle_t handle, const char* key, char* out, size_t outSize) {
  size_t required = outSize;
  if (nvs_get_str(handle, key, out, &required) != ESP_OK) {
    return;
  }
  out[outSize - 1] = '\0';
}

void getU8OrKeep(nvs_handle_t handle, const char* key, uint8_t* out) {
  uint8_t value = 0;
  if (nvs_get_u8(handle, key, &value) == ESP_OK) {
    *out = value;
  }
}

void getU16OrKeep(nvs_handle_t handle, const char* key, uint16_t* out) {
  uint16_t value = 0;
  if (nvs_get_u16(handle, key, &value) == ESP_OK) {
    *out = value;
  }
}

void getU32OrKeep(nvs_handle_t handle, const char* key, uint32_t* out) {
  uint32_t value = 0;
  if (nvs_get_u32(handle, key, &value) == ESP_OK) {
    *out = value;
  }
}
} // namespace

ApplicationConfigStorage::ApplicationConfigStorage() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

ApplicationConfigStorage::~ApplicationConfigStorage() {}

bool ApplicationConfigStorage::save(const ApplicationConfig& config) {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle);
  if (err != ESP_OK) {
    Serial.printf("Error opening NVS handle: %s\n", esp_err_to_name(err));
    return false;
  }

  bool ok = true;
  ok = ok && (nvs_set_u32(nvsHandle, CONFIG_VERSION_KEY, CONFIG_VERSION) == ESP_OK);
  ok = ok && setString(nvsHandle, KEY_WIFI_SSID, config.wifiSSID);
  ok = ok && setString(nvsHandle, KEY_WIFI_PASSWORD, config.wifiPassword);
  ok = ok && (nvs_set_u8(nvsHandle, KEY_DITHER_MODE, config.ditherMode) == ESP_OK);
  ok = ok && (nvs_set_u8(nvsHandle, KEY_SCALING_MODE, config.scalingMode) == ESP_OK);
  ok = ok && (nvs_set_u8(nvsHandle, KEY_POWER_MODE, config.powerMode) == ESP_OK);
  ok = ok && (nvs_set_u8(nvsHandle, KEY_CHECK_FOR_NEW_IMAGE_MODE,
                         config.checkForNewImageMode) == ESP_OK);
  ok = ok && (nvs_set_u16(nvsHandle, KEY_SLEEP_MINUTES, config.sleepMinutes) == ESP_OK);
  ok = ok && setString(nvsHandle, KEY_LAST_STATUS_VERSION, config.lastStatusVersion);
  ok = ok && setString(nvsHandle, KEY_LAST_STATUS_ETAG, config.lastStatusEtag);
  ok = ok && setString(nvsHandle, KEY_LAST_APPLIED_VERSION, config.lastAppliedVersion);
  ok = ok && setString(nvsHandle, KEY_LAST_APPLIED_IMAGE_ID, config.lastAppliedImageId);
  ok = ok && setString(nvsHandle, KEY_LAST_APPLIED_PHOTO_NAME, config.lastAppliedPhotoName);
  ok = ok && (nvs_set_u32(nvsHandle, KEY_LAST_SYNC_EPOCH, config.lastSyncEpoch) == ESP_OK);
  ok = ok && setString(nvsHandle, KEY_PAIRING_TOKEN, config.pairingToken);
  ok = ok && setString(nvsHandle, KEY_ASSIGNED_DEVICE_ID, config.assignedDeviceId);

  // Keep legacy blob writer for backwards compatibility with older firmware.
  ok = ok && (nvs_set_blob(nvsHandle, CONFIG_KEY, &config, sizeof(ApplicationConfig)) == ESP_OK);

  if (!ok) {
    Serial.println("Error saving config keys to NVS");
    nvs_close(nvsHandle);
    return false;
  }

  err = nvs_commit(nvsHandle);
  if (err != ESP_OK) {
    Serial.printf("Error committing to NVS: %s\n", esp_err_to_name(err));
    nvs_close(nvsHandle);
    return false;
  }

  nvs_close(nvsHandle);
  Serial.println("Configuration saved to NVS successfully");
  return true;
}

std::unique_ptr<ApplicationConfig> ApplicationConfigStorage::load() {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvsHandle);
  if (err != ESP_OK) {
    Serial.printf("Error opening NVS handle for reading: %s\n", esp_err_to_name(err));
    return nullptr;
  }

  uint32_t configVersion = 0;
  const bool hasVersionedConfig =
      (nvs_get_u32(nvsHandle, CONFIG_VERSION_KEY, &configVersion) == ESP_OK);

  if (hasVersionedConfig) {
    std::unique_ptr<ApplicationConfig> config(new ApplicationConfig());
    getStringOrKeep(nvsHandle, KEY_WIFI_SSID, config->wifiSSID, sizeof(config->wifiSSID));
    getStringOrKeep(nvsHandle, KEY_WIFI_PASSWORD, config->wifiPassword,
                    sizeof(config->wifiPassword));
    getU8OrKeep(nvsHandle, KEY_DITHER_MODE, &config->ditherMode);
    getU8OrKeep(nvsHandle, KEY_SCALING_MODE, &config->scalingMode);
    getU8OrKeep(nvsHandle, KEY_POWER_MODE, &config->powerMode);
    getU8OrKeep(nvsHandle, KEY_CHECK_FOR_NEW_IMAGE_MODE,
                &config->checkForNewImageMode);
    getU16OrKeep(nvsHandle, KEY_SLEEP_MINUTES, &config->sleepMinutes);
    getStringOrKeep(nvsHandle, KEY_LAST_STATUS_VERSION, config->lastStatusVersion,
                    sizeof(config->lastStatusVersion));
    getStringOrKeep(nvsHandle, KEY_LAST_STATUS_ETAG, config->lastStatusEtag,
                    sizeof(config->lastStatusEtag));
    getStringOrKeep(nvsHandle, KEY_LAST_APPLIED_VERSION, config->lastAppliedVersion,
                    sizeof(config->lastAppliedVersion));
    getStringOrKeep(nvsHandle, KEY_LAST_APPLIED_IMAGE_ID, config->lastAppliedImageId,
                    sizeof(config->lastAppliedImageId));
    getStringOrKeep(nvsHandle, KEY_LAST_APPLIED_PHOTO_NAME, config->lastAppliedPhotoName,
                    sizeof(config->lastAppliedPhotoName));
    getU32OrKeep(nvsHandle, KEY_LAST_SYNC_EPOCH, &config->lastSyncEpoch);
    getStringOrKeep(nvsHandle, KEY_PAIRING_TOKEN, config->pairingToken,
                    sizeof(config->pairingToken));
    getStringOrKeep(nvsHandle, KEY_ASSIGNED_DEVICE_ID, config->assignedDeviceId,
                    sizeof(config->assignedDeviceId));
    nvs_close(nvsHandle);
    Serial.printf("Configuration loaded from NVS key-value schema v%u.\n",
                  (unsigned int)configVersion);
    return config;
  }

  size_t blobSize = 0;
  err = nvs_get_blob(nvsHandle, CONFIG_KEY, nullptr, &blobSize);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(nvsHandle);
    Serial.println("No configuration found in NVS");
    return nullptr;
  }
  if (err != ESP_OK) {
    nvs_close(nvsHandle);
    Serial.printf("Error reading legacy config size from NVS: %s\n",
                  esp_err_to_name(err));
    return nullptr;
  }

  std::unique_ptr<ApplicationConfig> config(new ApplicationConfig());
  std::unique_ptr<uint8_t[]> raw(new uint8_t[blobSize]);

  size_t readSize = blobSize;
  err = nvs_get_blob(nvsHandle, CONFIG_KEY, raw.get(), &readSize);
  nvs_close(nvsHandle);
  if (err != ESP_OK) {
    Serial.printf("Error reading legacy config from NVS: %s\n", esp_err_to_name(err));
    return nullptr;
  }
  size_t copySize = (blobSize < sizeof(ApplicationConfig)) ? blobSize : sizeof(ApplicationConfig);
  memcpy(config.get(), raw.get(), copySize);
  if (blobSize < sizeof(ApplicationConfig)) {
    Serial.printf("Legacy blob migrated from older schema (%u -> %u bytes).\n",
                  (unsigned int)blobSize, (unsigned int)sizeof(ApplicationConfig));
  }
  // Re-save immediately using versioned key-value format for OTA-safe schema changes.
  save(*config);
  Serial.println("Configuration migrated from legacy blob to key-value schema.");

  return config;
}

void ApplicationConfigStorage::clear() {
  nvs_handle_t nvsHandle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle);
  if (err != ESP_OK) {
    Serial.printf("Error opening NVS handle: %s\n", esp_err_to_name(err));
    return;
  }

  err = nvs_erase_all(nvsHandle);
  if (err != ESP_OK) {
    Serial.printf("Error clearing config from NVS: %s\n", esp_err_to_name(err));
  } else {
    Serial.println("Configuration cleared from NVS");
  }

  nvs_commit(nvsHandle);
  nvs_close(nvsHandle);
}

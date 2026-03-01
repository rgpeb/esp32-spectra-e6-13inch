#pragma once

#include <memory>

#include "ApplicationConfig.h"

class ApplicationConfigStorage {
 public:
  ApplicationConfigStorage();
  ~ApplicationConfigStorage();

  bool save(const ApplicationConfig& config);
  std::unique_ptr<ApplicationConfig> load();
  void clear();

  // Image cycling index — stored separately because it changes every wake cycle
  uint16_t loadImageIndex();
  void saveImageIndex(uint16_t index);

 private:
  static const char* NVS_NAMESPACE;
  static const char* CONFIG_KEY;
  static const char* IMG_INDEX_KEY;
};
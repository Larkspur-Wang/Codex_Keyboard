#include "platform/nvs_store.h"

#include <vector>

#include "esp_log.h"
#include "keyboard/board_pins.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace easy_input {
namespace {

constexpr const char* kTag = "nvs_store";
constexpr const char* kPrefsNamespace = "ai_keyboard";
constexpr const char* kPrefsConfigKey = "config";
constexpr const char* kPrefsConfigKeyV2 = "config_v2";
constexpr const char* kPrefsBatteryFullKey = "bat_full_v1";
// bat_full_v2 may have been learned while SEN_CHRG polarity was inverted.
constexpr const char* kPrefsBatteryFullKeyV3 = "bat_full_v3";
constexpr const char* kPrefsGattSchemaRevisionKey = "gatt_rev_v1";
constexpr const char* kPrefsHostPlatformKey = "host_os_v1";

const char* prefs_config_key() {
#if defined(EASY_INPUT_BOARD_V2)
  return kPrefsConfigKeyV2;
#else
  return kPrefsConfigKey;
#endif
}

const char* prefs_battery_full_key() {
#if defined(EASY_INPUT_BOARD_V2)
  return kPrefsBatteryFullKeyV3;
#else
  return kPrefsBatteryFullKey;
#endif
}

void set_error(esp_err_t* out_err, esp_err_t err) {
  if (out_err != nullptr) {
    *out_err = err;
  }
}

}  // namespace

esp_err_t initialize_nvs_storage() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "NVS init returned %s, erasing and retrying", esp_err_to_name(err));
    const esp_err_t erase_err = nvs_flash_erase();
    if (erase_err != ESP_OK) {
      ESP_LOGE(kTag, "NVS erase failed: %s", esp_err_to_name(erase_err));
      return erase_err;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "NVS init failed: %s", esp_err_to_name(err));
  }
  return err;
}

bool NvsConfigStore::load_config(std::string* json, esp_err_t* out_err) const {
  if (json == nullptr) {
    set_error(out_err, ESP_ERR_INVALID_ARG);
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    set_error(out_err, err);
    return false;
  }

  std::size_t required_len = 0;
  const char* key = prefs_config_key();
  err = nvs_get_str(handle, key, nullptr, &required_len);
  if (err != ESP_OK) {
    nvs_close(handle);
    set_error(out_err, err);
    return false;
  }
  if (required_len == 0) {
    nvs_close(handle);
    set_error(out_err, ESP_ERR_INVALID_SIZE);
    return false;
  }

  std::vector<char> buffer(required_len);
  err = nvs_get_str(handle, key, buffer.data(), &required_len);
  nvs_close(handle);
  if (err != ESP_OK) {
    set_error(out_err, err);
    return false;
  }

  json->assign(buffer.data());
  ESP_LOGI(kTag,
           "loaded %s config key='%s' bytes=%u",
           ai_keyboard::kBoardName,
           key,
           static_cast<unsigned>(json->size()));
  set_error(out_err, ESP_OK);
  return !json->empty();
}

bool NvsConfigStore::save_config(const std::string& json, esp_err_t* out_err) const {
  if (json.empty()) {
    set_error(out_err, ESP_ERR_INVALID_ARG);
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    set_error(out_err, err);
    return false;
  }

  const char* key = prefs_config_key();
  err = nvs_set_str(handle, key, json.c_str());
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "saved %s config key='%s' bytes=%u",
             ai_keyboard::kBoardName,
             key,
             static_cast<unsigned>(json.size()));
  }
  set_error(out_err, err);
  return err == ESP_OK;
}

bool NvsConfigStore::load_battery_full_anchor_mv(std::int32_t* measured_mv,
                                                 esp_err_t* out_err) const {
  if (measured_mv == nullptr) {
    set_error(out_err, ESP_ERR_INVALID_ARG);
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    set_error(out_err, err);
    return false;
  }

  err = nvs_get_i32(handle, prefs_battery_full_key(), measured_mv);
  nvs_close(handle);
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "loaded %s battery full anchor key='%s' measured_mv=%ld",
             ai_keyboard::kBoardName,
             prefs_battery_full_key(),
             static_cast<long>(*measured_mv));
  }
  set_error(out_err, err);
  return err == ESP_OK;
}

bool NvsConfigStore::save_battery_full_anchor_mv(std::int32_t measured_mv,
                                                 esp_err_t* out_err) const {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    set_error(out_err, err);
    return false;
  }

  err = nvs_set_i32(handle, prefs_battery_full_key(), measured_mv);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "saved %s battery full anchor key='%s' measured_mv=%ld",
             ai_keyboard::kBoardName,
             prefs_battery_full_key(),
             static_cast<long>(measured_mv));
  }
  set_error(out_err, err);
  return err == ESP_OK;
}

bool NvsConfigStore::load_gatt_schema_revision(std::uint8_t* revision,
                                               esp_err_t* out_err) const {
  if (revision == nullptr) {
    set_error(out_err, ESP_ERR_INVALID_ARG);
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    set_error(out_err, err);
    return false;
  }

  err = nvs_get_u8(handle, kPrefsGattSchemaRevisionKey, revision);
  nvs_close(handle);
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "loaded %s GATT schema revision=%u",
             ai_keyboard::kBoardName,
             static_cast<unsigned>(*revision));
  }
  set_error(out_err, err);
  return err == ESP_OK;
}

bool NvsConfigStore::save_gatt_schema_revision(std::uint8_t revision,
                                               esp_err_t* out_err) const {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    set_error(out_err, err);
    return false;
  }

  err = nvs_set_u8(handle, kPrefsGattSchemaRevisionKey, revision);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "saved %s GATT schema revision=%u",
             ai_keyboard::kBoardName,
             static_cast<unsigned>(revision));
  }
  set_error(out_err, err);
  return err == ESP_OK;
}

bool NvsConfigStore::load_host_platform(ai_keyboard::HostPlatform* platform,
                                        esp_err_t* out_err) const {
  if (platform == nullptr) { set_error(out_err, ESP_ERR_INVALID_ARG); return false; }
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READONLY, &handle);
  std::uint8_t stored = 0;
  if (err == ESP_OK) err = nvs_get_u8(handle, kPrefsHostPlatformKey, &stored);
  if (handle != 0) nvs_close(handle);
  if (err == ESP_OK && stored <= 1) {
    *platform = stored == 1 ? ai_keyboard::HostPlatform::Windows
                            : ai_keyboard::HostPlatform::MacOS;
    set_error(out_err, ESP_OK);
    return true;
  }
  if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
  set_error(out_err, err);
  return false;
}

bool NvsConfigStore::save_host_platform(ai_keyboard::HostPlatform platform,
                                        esp_err_t* out_err) const {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_u8(handle, kPrefsHostPlatformKey,
                     platform == ai_keyboard::HostPlatform::Windows ? 1 : 0);
  }
  if (err == ESP_OK) err = nvs_commit(handle);
  if (handle != 0) nvs_close(handle);
  set_error(out_err, err);
  return err == ESP_OK;
}

bool NvsConfigStore::save_config_and_host_platform(
    const std::string& json,
    ai_keyboard::HostPlatform platform,
    esp_err_t* out_err) const {
  if (json.empty()) {
    set_error(out_err, ESP_ERR_INVALID_ARG);
    return false;
  }

  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(kPrefsNamespace, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, prefs_config_key(), json.c_str());
  }
  if (err == ESP_OK) {
    err = nvs_set_u8(handle,
                     kPrefsHostPlatformKey,
                     platform == ai_keyboard::HostPlatform::Windows ? 1 : 0);
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  if (handle != 0) {
    nvs_close(handle);
  }

  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "saved %s config+platform key='%s' bytes=%u platform=%s",
             ai_keyboard::kBoardName,
             prefs_config_key(),
             static_cast<unsigned>(json.size()),
             ai_keyboard::host_platform_name(platform));
  }
  set_error(out_err, err);
  return err == ESP_OK;
}

}  // namespace easy_input

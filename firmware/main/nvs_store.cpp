#include "nvs_store.hpp"

#include "esp_log.h"
#include "nvs.h"

namespace fw {

namespace {
const char* TAG = "settings";
const char* kNamespace = "leota";
const char* kKey = "settings";
}  // namespace

bool NvsStore::read(std::string& json) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;  // never written
    size_t len = 0;
    bool ok = nvs_get_blob(h, kKey, nullptr, &len) == ESP_OK && len > 0;
    if (ok) {
        json.assign(len, '\0');
        ok = nvs_get_blob(h, kKey, json.data(), &len) == ESP_OK;
        json.resize(len);
    }
    nvs_close(h);
    return ok;
}

bool NvsStore::write(const std::string& json) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, kKey, json.data(), json.size());
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Couldn't store the settings: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool NvsStore::erase() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const esp_err_t err = nvs_erase_key(h, kKey);
    nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

}  // namespace fw

#include "radio_storage.h"
#include "esp_littlefs.h"
#include "esp_log.h"
esp_err_t radio_storage_mount(void) {
    const esp_vfs_littlefs_conf_t cfg = {.base_path=RADIO_STORAGE_PATH,.partition_label="storage",.format_if_mount_failed=false,.dont_mount=false};
    esp_err_t err = esp_vfs_littlefs_register(&cfg);
    if (err != ESP_OK) ESP_LOGE("storage", "LittleFS mount failed: %s", esp_err_to_name(err));
    return err;
}


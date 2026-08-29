#pragma once
#include "esp_err.h"
#define RADIO_STORAGE_PATH "/littlefs"
esp_err_t radio_storage_mount(void);


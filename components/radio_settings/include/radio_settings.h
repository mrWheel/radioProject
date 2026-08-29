#pragma once
#include <stddef.h>
#include "esp_err.h"

//-- Persists the last-used station index across reboots (NVS). Volume is
//-- intentionally not persisted: it always resets to CONFIG_RADIO_DEFAULT_VOLUME.
esp_err_t radio_settings_init(void);
esp_err_t radio_settings_load(size_t *station);
esp_err_t radio_settings_save(size_t station);

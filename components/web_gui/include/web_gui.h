#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_gui_init(void);
void web_gui_deinit(void);

#ifdef __cplusplus
}
#endif

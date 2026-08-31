#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*web_gui_state_applied_cb_t)(size_t station_index, int volume, void *ctx);

esp_err_t web_gui_init(void);
void web_gui_deinit(void);
void web_gui_notify_title(const char *artist, const char *track);

//-- Called by app_main whenever the physical EC11/display path changes the
//-- playing station or volume, so connected web GUI clients stay in sync.
void web_gui_notify_device_state(size_t station_index);

//-- Registers a callback fired whenever a web GUI command (HTTP or WS) has
//-- applied a station/volume change, so app_main can mirror it onto the
//-- physical display and its own station/volume tracking.
void web_gui_set_state_applied_cb(web_gui_state_applied_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

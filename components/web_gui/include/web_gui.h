#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef enum
  {
    WEB_GUI_AUDIO_OWNER_DEVICE = 0,
    WEB_GUI_AUDIO_OWNER_BROWSER = 1
  } web_gui_audio_owner_t;

  typedef void (*web_gui_state_applied_cb_t)(size_t station_index, int volume, void* ctx);
  typedef void (*web_gui_audio_owner_cb_t)(web_gui_audio_owner_t owner, void* ctx);

  esp_err_t web_gui_init(void);
  void web_gui_deinit(void);
  void web_gui_notify_title(const char* line1, const char* line2, const char* line3);
  void web_gui_set_audio_owner(web_gui_audio_owner_t owner);
  web_gui_audio_owner_t web_gui_get_audio_owner(void);

  //-- Called by app_main whenever the physical EC11/display path changes the
  //-- playing station or volume, so connected web GUI clients stay in sync.
  void web_gui_notify_device_state(size_t station_index);

  //-- Registers a callback fired whenever a web GUI command (HTTP or WS) has
  //-- applied a station/volume change, so app_main can mirror it onto the
  //-- physical display and its own station/volume tracking.
  void web_gui_set_state_applied_cb(web_gui_state_applied_cb_t cb, void* ctx);
  void web_gui_set_audio_owner_cb(web_gui_audio_owner_cb_t cb, void* ctx);

#ifdef __cplusplus
}
#endif

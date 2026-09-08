#pragma once
#include "esp_err.h"
typedef enum
{
  RADIO_INPUT_ROTATE_LEFT,
  RADIO_INPUT_ROTATE_RIGHT,
  RADIO_INPUT_EN_PUSH,
  RADIO_INPUT_EN_LONG_PUSH,
  RADIO_INPUT_AUX_PUSH,
  RADIO_INPUT_AUX_MEDIUM_PUSH,
  RADIO_INPUT_AUX_LONG_PUSH
} radio_input_event_t;
typedef void (*radio_input_callback_t)(radio_input_event_t event, void* ctx);
esp_err_t radio_input_start(radio_input_callback_t cb, void* ctx);
//-- Backlight auto-off timeout in minutes (0-60, 0 = never dim). Defaults to
//-- 5 minutes until this is called; safe to call at any time after
//-- radio_input_start().
void radio_input_set_backlight_timeout_minutes(int minutes);

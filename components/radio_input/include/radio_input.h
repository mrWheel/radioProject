#pragma once
#include "esp_err.h"
typedef enum { RADIO_INPUT_ROTATE_LEFT, RADIO_INPUT_ROTATE_RIGHT, RADIO_INPUT_EN_PUSH, RADIO_INPUT_AUX_PUSH } radio_input_event_t;
typedef void (*radio_input_callback_t)(radio_input_event_t event, void *ctx);
esp_err_t radio_input_start(radio_input_callback_t cb, void *ctx);


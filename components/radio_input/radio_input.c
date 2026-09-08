#include "radio_input.h"
#include "esp32_s3_piggyback.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RADIO_BACKLIGHT_TIMEOUT_MS_DEFAULT (5 * 60 * 1000)
#define RADIO_BACKLIGHT_TIMEOUT_MAX_MIN 60

static radio_input_callback_t s_cb;
static void* s_ctx;
//-- 0 means "never dim"; set from the Settings menu (see radio_settings).
static volatile uint32_t s_backlight_timeout_ms = RADIO_BACKLIGHT_TIMEOUT_MS_DEFAULT;

void radio_input_set_backlight_timeout_minutes(int minutes)
{
  if (minutes < 0)
    minutes = 0;
  if (minutes > RADIO_BACKLIGHT_TIMEOUT_MAX_MIN)
    minutes = RADIO_BACKLIGHT_TIMEOUT_MAX_MIN;
  s_backlight_timeout_ms = minutes == 0 ? 0 : (uint32_t)minutes * 60 * 1000;
}

static void input_task(void* arg)
{
  (void)arg;
  tft_ec11_event_t event;
  TickType_t last_activity = xTaskGetTickCount();
  bool backlight_on = true;

  for (;;)
  {
    if (tft_ec11_get_event(&event, pdMS_TO_TICKS(100)))
    {
      last_activity = xTaskGetTickCount();
      if (!backlight_on)
      {
        tft_ec11_set_backlight(true);
        backlight_on = true;
      }

      if (event.type == TFT_EC11_EVENT_ROTATE)
      {
        bool right = event.steps > 0;
#if CONFIG_RADIO_ENCODER_REVERSED
        right = !right;
#endif
        s_cb(right ? RADIO_INPUT_ROTATE_RIGHT : RADIO_INPUT_ROTATE_LEFT, s_ctx);
      }
      else if (event.type == TFT_EC11_EVENT_ENCODER_BUTTON && event.press == TFT_EC11_PRESS_DOWN)
      {
        s_cb(RADIO_INPUT_EN_PUSH, s_ctx);
      }
      else if (event.type == TFT_EC11_EVENT_AUX_BUTTON && event.press == TFT_EC11_PRESS_SHORT)
      {
        s_cb(RADIO_INPUT_AUX_PUSH, s_ctx);
      }
      else if (event.type == TFT_EC11_EVENT_AUX_BUTTON && event.press == TFT_EC11_PRESS_MEDIUM)
      {
        s_cb(RADIO_INPUT_AUX_MEDIUM_PUSH, s_ctx);
      }
      else if (event.type == TFT_EC11_EVENT_AUX_BUTTON && event.press == TFT_EC11_PRESS_LONG)
      {
        s_cb(RADIO_INPUT_AUX_LONG_PUSH, s_ctx);
      }
    }

    if (backlight_on && s_backlight_timeout_ms != 0 &&
        xTaskGetTickCount() - last_activity >= pdMS_TO_TICKS(s_backlight_timeout_ms))
    {
      tft_ec11_set_backlight(false);
      backlight_on = false;
    }
  }
}

esp_err_t radio_input_start(radio_input_callback_t cb, void* ctx)
{
  if (!cb)
    return ESP_ERR_INVALID_ARG;
  s_cb = cb;
  s_ctx = ctx;
  ESP_RETURN_ON_ERROR(tft_ec11_inputs_start(16), "input", "piggyback inputs");
  return xTaskCreate(input_task, "radio_input", 3072, NULL, 8, NULL) == pdPASS ? ESP_OK
                                                                               : ESP_ERR_NO_MEM;
}

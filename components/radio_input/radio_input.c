#include "radio_input.h"
#include "esp32_s3_piggyback.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RADIO_BACKLIGHT_TIMEOUT_MS (5 * 60 * 1000)

static radio_input_callback_t s_cb;
static void* s_ctx;

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
      else if (event.type == TFT_EC11_EVENT_AUX_BUTTON &&
               (event.press == TFT_EC11_PRESS_MEDIUM || event.press == TFT_EC11_PRESS_LONG))
      {
        s_cb(RADIO_INPUT_AUX_LONG_PUSH, s_ctx);
      }
    }

    if (backlight_on &&
        xTaskGetTickCount() - last_activity >= pdMS_TO_TICKS(RADIO_BACKLIGHT_TIMEOUT_MS))
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

#include "radio_input.h"
#include "esp32_s3_piggyback.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static radio_input_callback_t s_cb;
static void *s_ctx;

static void input_task(void *arg)
{
	(void)arg;
	tft_ec11_event_t event;

	for (;;) {
		if (!tft_ec11_get_event(&event, portMAX_DELAY)) continue;

		if (event.type == TFT_EC11_EVENT_ROTATE) {
			bool right = event.steps > 0;
#if CONFIG_RADIO_ENCODER_REVERSED
			right = !right;
#endif
			s_cb(right ? RADIO_INPUT_ROTATE_RIGHT : RADIO_INPUT_ROTATE_LEFT, s_ctx);
		} else if (event.press == TFT_EC11_PRESS_DOWN) {
			if (event.type == TFT_EC11_EVENT_ENCODER_BUTTON) {
				s_cb(RADIO_INPUT_EN_PUSH, s_ctx);
			} else if (event.type == TFT_EC11_EVENT_AUX_BUTTON) {
				s_cb(RADIO_INPUT_AUX_PUSH, s_ctx);
			}
		}
	}
}

esp_err_t radio_input_start(radio_input_callback_t cb, void *ctx)
{
	if (!cb) return ESP_ERR_INVALID_ARG;
	s_cb = cb;
	s_ctx = ctx;
	ESP_RETURN_ON_ERROR(tft_ec11_inputs_start(16), "input", "piggyback inputs");
	return xTaskCreate(input_task, "radio_input", 3072, NULL, 8, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

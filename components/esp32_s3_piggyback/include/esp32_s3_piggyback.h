#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TFT_EC11_RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define TFT_EC11_BLACK 0x0000
#define TFT_EC11_WHITE 0xFFFF
#define TFT_EC11_RED 0xF800
#define TFT_EC11_GREEN 0x07E0
#define TFT_EC11_BLUE 0x001F
#define TFT_EC11_CYAN 0x07FF
#define TFT_EC11_MAGENTA 0xF81F
#define TFT_EC11_YELLOW 0xFFE0

typedef enum { TFT_EC11_PRESS_DOWN, TFT_EC11_PRESS_SHORT, TFT_EC11_PRESS_MEDIUM, TFT_EC11_PRESS_LONG } tft_ec11_press_t;
typedef enum { TFT_EC11_EVENT_ROTATE, TFT_EC11_EVENT_ENCODER_BUTTON, TFT_EC11_EVENT_AUX_BUTTON } tft_ec11_event_type_t;
typedef struct { tft_ec11_event_type_t type; int32_t steps; tft_ec11_press_t press; uint32_t duration_ms; } tft_ec11_event_t;
typedef struct { uint16_t background; uint16_t foreground; uint8_t font_scale; } tft_ec11_text_style_t;

esp_err_t tft_ec11_init(void);
esp_err_t tft_ec11_deinit(void);
esp_err_t tft_ec11_set_rotation(uint8_t rotation);
esp_err_t tft_ec11_set_inversion(bool enabled);
esp_err_t tft_ec11_set_backlight(bool enabled);
uint16_t tft_ec11_width(void);
uint16_t tft_ec11_height(void);
void tft_ec11_set_background(uint16_t color);
void tft_ec11_set_text_style(uint16_t foreground, uint8_t scale);
tft_ec11_text_style_t tft_ec11_get_text_style(void);
esp_err_t tft_ec11_clear(void);
esp_err_t tft_ec11_refresh(const uint16_t *frame_rgb565);
esp_err_t tft_ec11_fill_rect(int x, int y, int width, int height, uint16_t color);
esp_err_t tft_ec11_draw_text_simple(int x, int y, const char *text);
esp_err_t tft_ec11_draw_text_styled(int x, int y, const char *text, uint16_t background,
									uint8_t font_scale, uint16_t foreground);
esp_err_t tft_ec11_draw_text(int x, int y, size_t slot_chars, const char *text);
esp_err_t tft_ec11_inputs_start(size_t queue_length);
void tft_ec11_inputs_stop(void);
QueueHandle_t tft_ec11_event_queue(void);
bool tft_ec11_get_event(tft_ec11_event_t *event, TickType_t wait);

#ifdef __cplusplus
}
#endif

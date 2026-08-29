#include "radio_display.h"
#include "esp32_s3_piggyback.h"
#include "station_store.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_STATUS_MAX 64
#define DISPLAY_TITLE_MAX 64
#define DISPLAY_HEADER_H 36

typedef enum { DISPLAY_MODE_VOLUME, DISPLAY_MODE_STATION_SELECT, DISPLAY_MODE_STATUS, DISPLAY_MODE_TITLE } display_mode_t;

typedef struct {
	display_mode_t mode;
	int volume;
	char station[RADIO_NAME_MAX];
	size_t selected;
	char status[DISPLAY_STATUS_MAX];
	char title[DISPLAY_TITLE_MAX];
} display_message_t;

static QueueHandle_t s_queue;

//-- Last-drawn Volume-screen state, kept so a now-playing title arriving
//-- asynchronously from radio_audio can be merged into a redraw without the
//-- caller having to resend the volume/station it already sent
static display_mode_t s_current_mode = DISPLAY_MODE_STATUS;
static int s_cached_volume;
static char s_cached_station[RADIO_NAME_MAX];
static char s_cached_title[DISPLAY_TITLE_MAX];

static void draw_hint(const char *text)
{
	uint16_t height = tft_ec11_height();
	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_set_text_style(TFT_EC11_CYAN, 1);
	tft_ec11_draw_text_simple(4, height - 10, text);
}

static void draw_header(uint16_t width, const char *text, int font_scale)
{
	int glyph_h = 8 * font_scale;
	int y = (DISPLAY_HEADER_H - glyph_h) / 2;
	if (y < 2) y = 2;

	tft_ec11_fill_rect(0, 0, width, DISPLAY_HEADER_H, TFT_EC11_BLUE);
	tft_ec11_set_background(TFT_EC11_BLUE);
	tft_ec11_set_text_style(TFT_EC11_WHITE, font_scale);
	tft_ec11_draw_text(4, y, (width - 8) / (6 * font_scale), text ? text : "");
}

static void draw_volume(int volume, const char *station, const char *title)
{
	uint16_t width = tft_ec11_width();

	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_clear();

	draw_header(width, station, 3);

	char percent[8];
	snprintf(percent, sizeof(percent), "%d%%", volume);
	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_set_text_style(TFT_EC11_YELLOW, 3);
	int text_width = (int)strlen(percent) * 18;
	tft_ec11_draw_text_simple((width - text_width) / 2, 48, percent);

	int bar_x = 20, bar_y = 96, bar_w = width - 40, bar_h = 20;
	tft_ec11_fill_rect(bar_x, bar_y, bar_w, bar_h, TFT_EC11_WHITE);
	tft_ec11_fill_rect(bar_x + 2, bar_y + 2, bar_w - 4, bar_h - 4, TFT_EC11_BLACK);
	int fill_w = (bar_w - 4) * volume / 100;
	if (fill_w > 0) {
		tft_ec11_fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, TFT_EC11_GREEN);
	}

	if (title && title[0]) {
		tft_ec11_set_background(TFT_EC11_BLACK);
		tft_ec11_set_text_style(TFT_EC11_CYAN, 1);
		tft_ec11_draw_text(4, bar_y + bar_h + 8, (width - 8) / 6, title);
	}

	draw_hint("Push=Stations");
}

//-- Windowed so a station list longer than the screen still shows the selection, centered where possible
static void draw_station_list(size_t selected)
{
	uint16_t width = tft_ec11_width();
	uint16_t height = tft_ec11_height();
	size_t count = station_store_count();

	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_clear();

	draw_header(width, "Select Station", 2);

	const int title_h = DISPLAY_HEADER_H;
	const int row_h = 20;
	int visible_rows = (height - title_h - 12) / row_h;
	if (visible_rows < 1) visible_rows = 1;
	if ((size_t)visible_rows > count) visible_rows = (int)count;

	size_t max_start = count > (size_t)visible_rows ? count - (size_t)visible_rows : 0;
	size_t half = (size_t)visible_rows / 2;
	size_t start = selected > half ? selected - half : 0;
	if (start > max_start) start = max_start;

	size_t slot_chars = (width - 8) / 12;

	for (int row = 0; row < visible_rows; row++) {
		size_t index = start + (size_t)row;
		if (index >= count) break;
		const radio_station_t *entry = station_store_get(index);
		bool is_selected = (index == selected);
		tft_ec11_set_background(is_selected ? TFT_EC11_WHITE : TFT_EC11_BLACK);
		tft_ec11_set_text_style(is_selected ? TFT_EC11_BLACK : TFT_EC11_WHITE, 2);
		tft_ec11_draw_text(4, title_h + row * row_h, slot_chars, entry ? entry->name : "");
	}

	draw_hint("Turn=Browse  EN=Play");
}

static void draw_status(const char *status)
{
	uint16_t width = tft_ec11_width();
	uint16_t height = tft_ec11_height();
	size_t slot_chars = (width - 8) / 12;

	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_clear();
	tft_ec11_set_text_style(TFT_EC11_WHITE, 2);

	const char *text = status ? status : "";
	size_t len = strlen(text);
	if (len <= slot_chars) {
		tft_ec11_draw_text(4, (height - 16) / 2, slot_chars, text);
		return;
	}

	//-- Word-wrap onto a second line rather than truncating overlong status text
	size_t split = slot_chars;
	while (split > 0 && text[split] != ' ') split--;
	if (split == 0) split = slot_chars;

	char line1[DISPLAY_STATUS_MAX];
	size_t l1_len = split < sizeof(line1) - 1 ? split : sizeof(line1) - 1;
	memcpy(line1, text, l1_len);
	line1[l1_len] = '\0';

	const char *rest = text + split;
	while (*rest == ' ') rest++;

	tft_ec11_draw_text(4, (height / 2) - 20, slot_chars, line1);
	tft_ec11_draw_text(4, (height / 2) + 4, slot_chars, rest);
}

static void display_task(void *argument)
{
	display_message_t message;
	(void)argument;
	for (;;) {
		if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) continue;
		switch (message.mode) {
		case DISPLAY_MODE_VOLUME:
			s_current_mode = DISPLAY_MODE_VOLUME;
			s_cached_volume = message.volume;
			snprintf(s_cached_station, sizeof(s_cached_station), "%s", message.station);
			draw_volume(s_cached_volume, s_cached_station, s_cached_title);
			break;
		case DISPLAY_MODE_TITLE:
			snprintf(s_cached_title, sizeof(s_cached_title), "%s", message.title);
			if (s_current_mode == DISPLAY_MODE_VOLUME) {
				draw_volume(s_cached_volume, s_cached_station, s_cached_title);
			}
			break;
		case DISPLAY_MODE_STATION_SELECT:
			s_current_mode = DISPLAY_MODE_STATION_SELECT;
			draw_station_list(message.selected);
			break;
		case DISPLAY_MODE_STATUS:
			s_current_mode = DISPLAY_MODE_STATUS;
			draw_status(message.status);
			break;
		}
	}
}

esp_err_t radio_display_init(void)
{
	ESP_RETURN_ON_ERROR(tft_ec11_init(), "display", "piggyback init");
	s_queue = xQueueCreate(8, sizeof(display_message_t));
	if (!s_queue || xTaskCreate(display_task, "radio_display", 4096, NULL, 5, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

void radio_display_volume(int volume, const char *station)
{
	display_message_t message = {.mode = DISPLAY_MODE_VOLUME, .volume = volume};
	if (station) snprintf(message.station, sizeof(message.station), "%s", station);
	ESP_LOGI("display", "VOLUME %d%% | %s", volume, station ? station : "");
	if (s_queue) (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_now_playing(const char *title)
{
	display_message_t message = {.mode = DISPLAY_MODE_TITLE};
	if (title) snprintf(message.title, sizeof(message.title), "%s", title);
	if (s_queue) (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_station_list(size_t selected)
{
	display_message_t message = {.mode = DISPLAY_MODE_STATION_SELECT, .selected = selected};
	const radio_station_t *station = station_store_get(selected);
	ESP_LOGI("display", "SELECT > %s", station ? station->name : "none");
	if (s_queue) (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_status(const char *status)
{
	display_message_t message = {.mode = DISPLAY_MODE_STATUS};
	if (status) snprintf(message.status, sizeof(message.status), "%s", status);
	ESP_LOGI("display", "%s", status ? status : "");
	if (s_queue) (void)xQueueSend(s_queue, &message, 0);
}

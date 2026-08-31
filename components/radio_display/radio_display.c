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

#define DISPLAY_STATUS_MAX 96
#define DISPLAY_TITLE_MAX 64
#define DISPLAY_HEADER_H 36

typedef enum { DISPLAY_MODE_VOLUME, DISPLAY_MODE_STATION_SELECT, DISPLAY_MODE_STATUS, DISPLAY_MODE_TITLE } display_mode_t;

typedef struct {
	display_mode_t mode;
	int volume;
	char station[RADIO_NAME_MAX];
	size_t selected;
	char status[DISPLAY_STATUS_MAX];
	char line1[DISPLAY_TITLE_MAX];
	char line2[DISPLAY_TITLE_MAX];
	char line3[DISPLAY_TITLE_MAX];
} display_message_t;

static QueueHandle_t s_queue;

//-- Last-drawn Volume-screen state, kept so a now-playing artist/track pair
//-- arriving asynchronously from radio_audio can be merged into a redraw
//-- without the caller having to resend the volume/station it already sent,
//-- and so a rotation-only update can skip redrawing what hasn't changed
static display_mode_t s_current_mode = DISPLAY_MODE_STATUS;
static int s_cached_volume;
static char s_cached_station[RADIO_NAME_MAX];
static char s_cached_line1[DISPLAY_TITLE_MAX];
static char s_cached_line2[DISPLAY_TITLE_MAX];
static char s_cached_line3[DISPLAY_TITLE_MAX];

//-- Windowed-list redraw cache: SIZE_MAX means "not drawn yet", forcing a
//-- full redraw the first time the Select-Station screen is shown
static size_t s_cached_list_start = SIZE_MAX;
static size_t s_cached_list_selected = SIZE_MAX;

static void draw_hint(const char *text)
{
	uint16_t height = tft_ec11_height();
	int scale = 2;
	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_set_text_style(TFT_EC11_CYAN, scale);
	tft_ec11_draw_text_simple(4, (int)height - 8 * scale - 2, text);
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

//-- Fixed 4-char slot ("100%" is the widest possible value) so the field's
//-- own background-clear always wipes the previous digits, regardless of how
//-- many digits the new value has, and the number no longer jitters
//-- left/right as the digit count changes
static void draw_volume_percent(uint16_t width, int volume)
{
	char percent[8];
	snprintf(percent, sizeof(percent), "%d%%", volume);
	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_set_text_style(TFT_EC11_YELLOW, 3);
	const size_t slots = 4;
	int x = ((int)width - (int)slots * 6 * 3) / 2;
	tft_ec11_draw_text(x, 48, slots, percent);
}

//-- Only the bar's interior is touched; the static white border is drawn
//-- once by draw_volume_full()
static void draw_volume_bar(uint16_t width, int volume)
{
	int bar_x = 20, bar_y = 96, bar_w = width - 40, bar_h = 20;
	tft_ec11_fill_rect(bar_x + 2, bar_y + 2, bar_w - 4, bar_h - 4, TFT_EC11_BLACK);
	int fill_w = (bar_w - 4) * volume / 100;
	if (fill_w > 0) {
		tft_ec11_fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, TFT_EC11_GREEN);
	}
}

//-- Clears the full line width first so a shorter new value can never leave
//-- stale glyphs behind from a longer previous one (tft_ec11_draw_text only
//-- clears exactly the slot count it's given), then draws the centered text
//-- at a larger scale than the rest of the Volume screen.
static void draw_now_playing_line(uint16_t width, int y, int scale, const char *text)
{
	int ch = 8 * scale;
	tft_ec11_fill_rect(0, y, width, ch, TFT_EC11_BLACK);
	size_t max_chars = (size_t)((width - 8) / (6 * scale));
	size_t len = strlen(text);
	if (len > max_chars) len = max_chars;
	int x = ((int)width - (int)len * 6 * scale) / 2;
	if (x < 0) x = 0;
	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_set_text_style(TFT_EC11_CYAN, scale);
	tft_ec11_draw_text(x, y, len, text);
}

//-- Always draws (even for "-") so a line that disappears (station with no
//-- metadata) clears instead of leaving stale text behind
static void draw_now_playing(uint16_t width, const char *line1, const char *line2, const char *line3)
{
	draw_now_playing_line(width, 124, 2, line1 ? line1 : "-");
	draw_now_playing_line(width, 144, 2, line2 ? line2 : "-");
	draw_now_playing_line(width, 164, 2, line3 ? line3 : "-");
}

static void draw_volume_full(int volume, const char *station, const char *line1, const char *line2, const char *line3)
{
	uint16_t width = tft_ec11_width();

	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_clear();

	draw_header(width, station, 3);

	int bar_x = 20, bar_y = 96, bar_w = width - 40, bar_h = 20;
	tft_ec11_fill_rect(bar_x, bar_y, bar_w, bar_h, TFT_EC11_WHITE);

	draw_volume_percent(width, volume);
	draw_volume_bar(width, volume);
	draw_now_playing(width, line1, line2, line3);

	draw_hint("Push4Station");
}

static void draw_station_row(int title_h, int row_h, size_t slot_chars, int row, size_t index, bool is_selected)
{
	const radio_station_t *entry = station_store_get(index);
	tft_ec11_set_background(is_selected ? TFT_EC11_WHITE : TFT_EC11_BLACK);
	tft_ec11_set_text_style(is_selected ? TFT_EC11_BLACK : TFT_EC11_WHITE, 2);
	tft_ec11_draw_text(4, title_h + row * row_h, slot_chars, entry ? entry->name : "");
}

//-- Windowed so a station list longer than the screen still shows the selection, centered where
//-- possible. When the visible window (`start`) doesn't change between two calls, only the
//-- previously- and newly-highlighted rows are redrawn instead of the whole screen.
static void draw_station_list(size_t selected, bool force_full)
{
	uint16_t width = tft_ec11_width();
	uint16_t height = tft_ec11_height();
	size_t count = station_store_count();

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

	bool full = force_full || start != s_cached_list_start;
	if (full) {
		tft_ec11_set_background(TFT_EC11_BLACK);
		tft_ec11_clear();

		draw_header(width, "Select Station", 2);

		for (int row = 0; row < visible_rows; row++) {
			size_t index = start + (size_t)row;
			if (index >= count) break;
			draw_station_row(title_h, row_h, slot_chars, row, index, index == selected);
		}

		draw_hint("Turn:Browse EN:Play");
	} else if (s_cached_list_selected != selected) {
		if (s_cached_list_selected >= start && s_cached_list_selected - start < (size_t)visible_rows) {
			int old_row = (int)(s_cached_list_selected - start);
			draw_station_row(title_h, row_h, slot_chars, old_row, s_cached_list_selected, false);
		}
		int new_row = (int)(selected - start);
		draw_station_row(title_h, row_h, slot_chars, new_row, selected, true);
	}

	s_cached_list_start = start;
	s_cached_list_selected = selected;
}

//-- Splits on explicit '\n' first (so callers can lay out e.g. "SSID: .../IP:
//-- ..." on separate lines), then word-wraps any resulting segment that's
//-- still too wide for the screen. Each line is drawn horizontally centered,
//-- and the whole block is centered vertically.
static void draw_status(const char *status)
{
	uint16_t width = tft_ec11_width();
	uint16_t height = tft_ec11_height();
	size_t slot_chars = (width - 8) / 12;
	const int scale = 2;
	const int line_h = 8 * scale + 4;

	tft_ec11_set_background(TFT_EC11_BLACK);
	tft_ec11_clear();
	tft_ec11_set_text_style(TFT_EC11_WHITE, scale);

	const char *text = status ? status : "";

	char lines[4][DISPLAY_STATUS_MAX];
	int line_count = 0;
	const char *seg = text;
	while (*seg && line_count < 4) {
		const char *nl = strchr(seg, '\n');
		size_t seg_len = nl ? (size_t)(nl - seg) : strlen(seg);
		if (seg_len >= DISPLAY_STATUS_MAX) seg_len = DISPLAY_STATUS_MAX - 1;

		if (seg_len <= slot_chars) {
			memcpy(lines[line_count], seg, seg_len);
			lines[line_count][seg_len] = '\0';
			line_count++;
		} else {
			size_t split = slot_chars;
			while (split > 0 && seg[split] != ' ') split--;
			if (split == 0) split = slot_chars;
			memcpy(lines[line_count], seg, split);
			lines[line_count][split] = '\0';
			line_count++;

			if (line_count < 4) {
				const char *rest = seg + split;
				while (*rest == ' ') rest++;
				size_t rest_len = seg_len - (size_t)(rest - seg);
				if (rest_len >= DISPLAY_STATUS_MAX) rest_len = DISPLAY_STATUS_MAX - 1;
				memcpy(lines[line_count], rest, rest_len);
				lines[line_count][rest_len] = '\0';
				line_count++;
			}
		}

		seg = nl ? nl + 1 : seg + strlen(seg);
	}
	if (line_count == 0) {
		lines[0][0] = '\0';
		line_count = 1;
	}

	int total_h = line_count * line_h;
	int y = ((int)height - total_h) / 2;
	if (y < 0) y = 0;

	for (int i = 0; i < line_count; i++) {
		size_t len = strlen(lines[i]);
		int x = ((int)width - (int)len * 6 * scale) / 2;
		if (x < 0) x = 0;
		tft_ec11_draw_text(x, y + i * line_h, len, lines[i]);
	}
}

static void display_task(void *argument)
{
	display_message_t message;
	(void)argument;
	for (;;) {
		if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) continue;
		switch (message.mode) {
		case DISPLAY_MODE_VOLUME: {
			bool entering = s_current_mode != DISPLAY_MODE_VOLUME;
			bool station_changed = strcmp(s_cached_station, message.station) != 0;
			s_current_mode = DISPLAY_MODE_VOLUME;
			s_cached_volume = message.volume;
			snprintf(s_cached_station, sizeof(s_cached_station), "%s", message.station);
			if (entering || station_changed) {
				draw_volume_full(s_cached_volume, s_cached_station, s_cached_line1, s_cached_line2, s_cached_line3);
			} else {
				uint16_t width = tft_ec11_width();
				draw_volume_percent(width, s_cached_volume);
				draw_volume_bar(width, s_cached_volume);
			}
			break;
		}
		case DISPLAY_MODE_TITLE: {
			bool changed = strcmp(s_cached_line1, message.line1) != 0 ||
				strcmp(s_cached_line2, message.line2) != 0 ||
				strcmp(s_cached_line3, message.line3) != 0;
			snprintf(s_cached_line1, sizeof(s_cached_line1), "%s", message.line1);
			snprintf(s_cached_line2, sizeof(s_cached_line2), "%s", message.line2);
			snprintf(s_cached_line3, sizeof(s_cached_line3), "%s", message.line3);
			if (changed && s_current_mode == DISPLAY_MODE_VOLUME) {
				draw_now_playing(tft_ec11_width(), s_cached_line1, s_cached_line2, s_cached_line3);
			}
			break;
		}
		case DISPLAY_MODE_STATION_SELECT: {
			bool entering = s_current_mode != DISPLAY_MODE_STATION_SELECT;
			s_current_mode = DISPLAY_MODE_STATION_SELECT;
			draw_station_list(message.selected, entering);
			break;
		}
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

void radio_display_now_playing(const char *line1, const char *line2, const char *line3)
{
	display_message_t message = {.mode = DISPLAY_MODE_TITLE};
	if (line1) snprintf(message.line1, sizeof(message.line1), "%s", line1);
	if (line2) snprintf(message.line2, sizeof(message.line2), "%s", line2);
	if (line3) snprintf(message.line3, sizeof(message.line3), "%s", line3);
	if (s_queue) (void)xQueueSend(s_queue, &message, 0);
}

//-- Same width/scale used by draw_now_playing_line(), exposed so
//-- main/app_main.c can pre-wrap ICY text to the exact line budget instead
//-- of duplicating this formula.
size_t radio_display_title_max_chars(void)
{
	uint16_t width = tft_ec11_width();
	int scale = 2;
	return (size_t)((width - 8) / (6 * scale));
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

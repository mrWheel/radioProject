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
#define DISPLAY_TECH_VALUE_MAX 96
#define DISPLAY_ERROR_MAX_LINES 6

//-- Defined in main/app_main.c; shown right-aligned in the Volume header
extern const char* PROG_VERSION;

typedef enum
{
  DISPLAY_MODE_VOLUME,
  DISPLAY_MODE_STATION_SELECT,
  DISPLAY_MODE_STATUS,
  DISPLAY_MODE_TECHNICAL,
  DISPLAY_MODE_TITLE,
  DISPLAY_MODE_BUFFER_FILL,
  DISPLAY_MODE_ERROR,
  DISPLAY_MODE_SETTINGS,
  DISPLAY_MODE_EQUALIZER
} display_mode_t;

typedef struct
{
  display_mode_t mode;
  int volume;
  char station[RADIO_NAME_MAX];
  size_t selected;
  char status[DISPLAY_STATUS_MAX];
  char line1[DISPLAY_TITLE_MAX];
  char line2[DISPLAY_TITLE_MAX];
  char line3[DISPLAY_TITLE_MAX];
  int buffer_fill;
  char ssid[DISPLAY_TECH_VALUE_MAX];
  char ip[DISPLAY_TECH_VALUE_MAX];
  char mac[DISPLAY_TECH_VALUE_MAX];
  char hostname[DISPLAY_TECH_VALUE_MAX];
  size_t station_count;
  bool ota_in_progress;
  size_t settings_selected;
  bool settings_editing;
  uint16_t settings_hostname_num;
  int8_t settings_attenuation;
  uint8_t settings_backlight_minutes;
  bool settings_encoder_reversed;
  int16_t settings_i2s_bclk;
  int16_t settings_i2s_ws;
  int16_t settings_i2s_dout;
  int16_t settings_i2s_enable;
  size_t eq_selected;
  bool eq_editing;
  int8_t eq_bass_db;
  int8_t eq_mid_db;
  int8_t eq_treble_db;
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

static void draw_hint(const char* text)
{
  uint16_t height = tft_ec11_height();
  int scale = 2;
  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_set_text_style(TFT_EC11_CYAN, scale);
  tft_ec11_draw_text_simple(4, (int)height - 8 * scale - 2, text);
}

//-- Short stream-buffer fill bar, right-aligned on the bottom (hint) line,
//-- next to the left-aligned hint text. Border is static; only the interior
//-- is redrawn on updates. Color shifts green->yellow->red as the buffer
//-- empties so an approaching underrun is visible at a glance.
#define BUF_BAR_W 60
#define BUF_BAR_H 10
static int s_drawn_buf_fill = -1;
static void draw_buffer_bar(int percent)
{
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  if (percent == s_drawn_buf_fill)
    return;
  s_drawn_buf_fill = percent;
  uint16_t width = tft_ec11_width();
  uint16_t height = tft_ec11_height();
  int bar_x = (int)width - BUF_BAR_W - 4;
  int bar_y = (int)height - BUF_BAR_H - 4;
  tft_ec11_fill_rect(bar_x, bar_y, BUF_BAR_W, BUF_BAR_H, TFT_EC11_WHITE);
  tft_ec11_fill_rect(bar_x + 1, bar_y + 1, BUF_BAR_W - 2, BUF_BAR_H - 2, TFT_EC11_BLACK);
  int fill_w = (BUF_BAR_W - 2) * percent / 100;
  if (fill_w > 0)
  {
    uint16_t color =
        percent > 50 ? TFT_EC11_GREEN : (percent > 20 ? TFT_EC11_YELLOW : TFT_EC11_RED);
    tft_ec11_fill_rect(bar_x + 1, bar_y + 1, fill_w, BUF_BAR_H - 2, color);
  }
}

static void draw_header(uint16_t width, const char* text, int font_scale)
{
  int glyph_h = 8 * font_scale;
  int y = (DISPLAY_HEADER_H - glyph_h) / 2;
  if (y < 2)
    y = 2;

  tft_ec11_fill_rect(0, 0, width, DISPLAY_HEADER_H, TFT_EC11_BLUE);
  tft_ec11_set_background(TFT_EC11_BLUE);
  tft_ec11_set_text_style(TFT_EC11_WHITE, font_scale);
  tft_ec11_draw_text(4, y, (width - 8) / (6 * font_scale), text ? text : "");
}

//-- Right-aligned firmware version, drawn over the already-painted header.
//-- The background behind the version text is cleared first so a long station
//-- name never collides with or bleeds through the version number.
static void draw_header_version(uint16_t width)
{
  const char* version = PROG_VERSION ? PROG_VERSION : "";
  size_t len = strlen(version);
  if (len == 0)
    return;

  const int scale = 2;
  int glyph_h = 8 * scale;
  int y = (DISPLAY_HEADER_H - glyph_h) / 2;
  if (y < 2)
    y = 2;

  int text_w = (int)len * 6 * scale;
  int x = (int)width - text_w - 4;
  if (x < 0)
    x = 0;

  int clear_x = x > 4 ? x - 4 : 0;
  int clear_w = (int)width - clear_x;
  tft_ec11_fill_rect(clear_x, 0, clear_w, DISPLAY_HEADER_H, TFT_EC11_BLUE);

  tft_ec11_set_background(TFT_EC11_BLUE);
  tft_ec11_set_text_style(TFT_EC11_WHITE, scale);
  tft_ec11_draw_text(x, y, len, version);
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
  if (fill_w > 0)
  {
    tft_ec11_fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, TFT_EC11_GREEN);
  }
}

//-- Clears the full line width first so a shorter new value can never leave
//-- stale glyphs behind from a longer previous one (tft_ec11_draw_text only
//-- clears exactly the slot count it's given), then draws the centered text
//-- at a larger scale than the rest of the Volume screen.
static void draw_now_playing_line(uint16_t width, int y, int scale, const char* text)
{
  int ch = 8 * scale;
  tft_ec11_fill_rect(0, y, width, ch, TFT_EC11_BLACK);
  size_t max_chars = (size_t)((width - 8) / (6 * scale));
  size_t len = strlen(text);
  if (len > max_chars)
    len = max_chars;
  int x = ((int)width - (int)len * 6 * scale) / 2;
  if (x < 0)
    x = 0;
  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_set_text_style(TFT_EC11_CYAN, scale);
  tft_ec11_draw_text(x, y, len, text);
}

//-- Always draws (even for "-") so a line that disappears (station with no
//-- metadata) clears instead of leaving stale text behind
static void draw_now_playing(uint16_t width, const char* line1, const char* line2,
                             const char* line3)
{
  draw_now_playing_line(width, 124, 2, line1 ? line1 : "-");
  draw_now_playing_line(width, 144, 2, line2 ? line2 : "-");
  draw_now_playing_line(width, 164, 2, line3 ? line3 : "-");
}

static void draw_volume_full(int volume, const char* station, const char* line1, const char* line2,
                             const char* line3)
{
  uint16_t width = tft_ec11_width();

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_clear();

  draw_header(width, station, 3);
  draw_header_version(width);

  int bar_x = 20, bar_y = 96, bar_w = width - 40, bar_h = 20;
  tft_ec11_fill_rect(bar_x, bar_y, bar_w, bar_h, TFT_EC11_WHITE);

  draw_volume_percent(width, volume);
  draw_volume_bar(width, volume);
  draw_now_playing(width, line1, line2, line3);

  draw_hint("Push4Station");
  s_drawn_buf_fill = -1;
  draw_buffer_bar(0);
}

static void draw_station_row(int title_h, int row_h, size_t slot_chars, int row, size_t index,
                             bool is_selected)
{
  const radio_station_t* entry = station_store_get(index);
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
  if (visible_rows < 1)
    visible_rows = 1;
  if ((size_t)visible_rows > count)
    visible_rows = (int)count;

  size_t max_start = count > (size_t)visible_rows ? count - (size_t)visible_rows : 0;
  size_t half = (size_t)visible_rows / 2;
  size_t start = selected > half ? selected - half : 0;
  if (start > max_start)
    start = max_start;

  size_t slot_chars = (width - 8) / 12;

  bool full = force_full || start != s_cached_list_start;
  if (full)
  {
    tft_ec11_set_background(TFT_EC11_BLACK);
    tft_ec11_clear();

    draw_header(width, "Select Station", 2);

    for (int row = 0; row < visible_rows; row++)
    {
      size_t index = start + (size_t)row;
      if (index >= count)
        break;
      draw_station_row(title_h, row_h, slot_chars, row, index, index == selected);
    }

    draw_hint("Turn:Browse EN:Play");
  }
  else if (s_cached_list_selected != selected)
  {
    if (s_cached_list_selected >= start && s_cached_list_selected - start < (size_t)visible_rows)
    {
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
static void draw_status(const char* status)
{
  uint16_t width = tft_ec11_width();
  uint16_t height = tft_ec11_height();
  size_t slot_chars = (width - 8) / 12;
  const int scale = 2;
  const int line_h = 8 * scale + 4;

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_clear();
  tft_ec11_set_text_style(TFT_EC11_WHITE, scale);

  const char* text = status ? status : "";

  char lines[4][DISPLAY_STATUS_MAX];
  int line_count = 0;
  const char* seg = text;
  while (*seg && line_count < 4)
  {
    const char* nl = strchr(seg, '\n');
    size_t seg_len = nl ? (size_t)(nl - seg) : strlen(seg);
    if (seg_len >= DISPLAY_STATUS_MAX)
      seg_len = DISPLAY_STATUS_MAX - 1;

    if (seg_len <= slot_chars)
    {
      memcpy(lines[line_count], seg, seg_len);
      lines[line_count][seg_len] = '\0';
      line_count++;
    }
    else
    {
      size_t split = slot_chars;
      while (split > 0 && seg[split] != ' ')
        split--;
      if (split == 0)
        split = slot_chars;
      memcpy(lines[line_count], seg, split);
      lines[line_count][split] = '\0';
      line_count++;

      if (line_count < 4)
      {
        const char* rest = seg + split;
        while (*rest == ' ')
          rest++;
        size_t rest_len = seg_len - (size_t)(rest - seg);
        if (rest_len >= DISPLAY_STATUS_MAX)
          rest_len = DISPLAY_STATUS_MAX - 1;
        memcpy(lines[line_count], rest, rest_len);
        lines[line_count][rest_len] = '\0';
        line_count++;
      }
    }

    seg = nl ? nl + 1 : seg + strlen(seg);
  }
  if (line_count == 0)
  {
    lines[0][0] = '\0';
    line_count = 1;
  }

  int total_h = line_count * line_h;
  int y = ((int)height - total_h) / 2;
  if (y < 0)
    y = 0;

  for (int i = 0; i < line_count; i++)
  {
    size_t len = strlen(lines[i]);
    int x = ((int)width - (int)len * 6 * scale) / 2;
    if (x < 0)
      x = 0;
    tft_ec11_draw_text(x, y + i * line_h, len, lines[i]);
  }
}

//-- Same layout as draw_status() (word-wrap on '\n' and screen width) but a
//-- red "ERROR" header, so a malformed stations.json is unmistakable instead
//-- of looking like a normal status message.
static void draw_error(const char* status)
{
  uint16_t width = tft_ec11_width();
  uint16_t height = tft_ec11_height();
  const int scale = 3;
  size_t slot_chars = (width - 8) / (6 * scale);
  const int line_h = 8 * scale + 4;

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_clear();
  draw_header(width, "ERROR", scale);

  const char* text = status ? status : "";

  //-- Only the first '\n'-separated segment (the actual error, e.g. "JSON
  //-- syntax error") is drawn in red; any further segments (hints such as
  //-- "at line N" / "Load stations.json via the GUI") are white. Each
  //-- segment is fully word-wrapped across as many lines as it needs -
  //-- unlike a single split, so nothing runs off the right edge of the
  //-- screen or gets silently dropped.
  char lines[DISPLAY_ERROR_MAX_LINES][DISPLAY_STATUS_MAX];
  bool line_is_error[DISPLAY_ERROR_MAX_LINES];
  int line_count = 0;
  const char* seg = text;
  int seg_index = 0;
  while (*seg && line_count < DISPLAY_ERROR_MAX_LINES)
  {
    const char* nl = strchr(seg, '\n');
    size_t seg_len = nl ? (size_t)(nl - seg) : strlen(seg);
    if (seg_len >= DISPLAY_STATUS_MAX)
      seg_len = DISPLAY_STATUS_MAX - 1;

    size_t offset = 0;
    while (offset < seg_len && line_count < DISPLAY_ERROR_MAX_LINES)
    {
      size_t remaining = seg_len - offset;
      size_t take = remaining;
      if (take > slot_chars)
      {
        size_t split = slot_chars;
        while (split > 0 && seg[offset + split] != ' ')
          split--;
        take = (split == 0) ? slot_chars : split;
      }
      memcpy(lines[line_count], seg + offset, take);
      lines[line_count][take] = '\0';
      line_is_error[line_count] = (seg_index == 0);
      line_count++;
      offset += take;
      while (offset < seg_len && seg[offset] == ' ')
        offset++;
    }

    seg = nl ? nl + 1 : seg + strlen(seg);
    seg_index++;
  }
  if (line_count == 0)
  {
    lines[0][0] = '\0';
    line_is_error[0] = true;
    line_count = 1;
  }

  int content_top = DISPLAY_HEADER_H + 8;
  int total_h = line_count * line_h;
  int y = content_top + (((int)height - content_top - total_h) / 2);
  if (y < content_top)
    y = content_top;

  for (int i = 0; i < line_count; i++)
  {
    size_t len = strlen(lines[i]);
    int x = ((int)width - (int)len * 6 * scale) / 2;
    if (x < 0)
      x = 0;
    tft_ec11_set_text_style(line_is_error[i] ? TFT_EC11_RED : TFT_EC11_WHITE, scale);
    tft_ec11_draw_text(x, y + i * line_h, len, lines[i]);
  }
}

static void draw_technical(const display_message_t* message)
{
  uint16_t width = tft_ec11_width();
  uint16_t height = tft_ec11_height();
  const int scale = 2;
  const int line_h = 8 * scale + 5;
  char lines[6][DISPLAY_TECH_VALUE_MAX];
  if (message->ota_in_progress)
  {
    snprintf(lines[0], sizeof(lines[0]), "OTA in Progress");
  }
  else
  {
    snprintf(lines[0], sizeof(lines[0]), "InternetRadio %s", PROG_VERSION ? PROG_VERSION : "");
  }
  snprintf(lines[1], sizeof(lines[1]), "SSID: %.88s", message->ssid);
  snprintf(lines[2], sizeof(lines[2]), "IP: %.90s", message->ip);
  snprintf(lines[3], sizeof(lines[3]), "MAC: %.89s", message->mac);
  snprintf(lines[4], sizeof(lines[4]), "Host: %.88s", message->hostname);
  snprintf(lines[5], sizeof(lines[5]), "Stations: %u", (unsigned)message->station_count);

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_clear();
  for (size_t i = 0; i < 6; i++)
  {
    size_t max_chars = (width - 8) / (6 * scale);
    size_t length = strlen(lines[i]);
    if (length > max_chars)
      length = max_chars;
    int x = ((int)width - (int)length * 6 * scale) / 2;
    int y = ((int)height - 6 * line_h) / 2 + (int)i * line_h;
    if (x < 0)
      x = 0;
    uint16_t color = TFT_EC11_WHITE;
    if (i == 0)
    {
      color = message->ota_in_progress ? TFT_EC11_YELLOW : TFT_EC11_CYAN;
    }
    tft_ec11_set_text_style(color, scale);
    tft_ec11_draw_text(x, y, length, lines[i]);
  }
}

//-- [Settings] menu: 10 fixed rows (Hostname#, Attenuating, Backlight off
//-- time, Encoder Direction, PCM5102A BCLK/LRCLK/DATA/DAC-enable GPIO, Reset
//-- Radio, Exit), windowed/scrolled like the station list since they don't
//-- all fit on screen at once; always fully redrawn since updates are rare
//-- (rotation/edit).
#define SETTINGS_ITEM_COUNT 10
static const char* k_settings_labels[SETTINGS_ITEM_COUNT] = {
    "Hostname#",      "Attenuating",     "Backlight off",   "Encoder Direction",
    "PCM5102A BCLK",  "PCM5102A LRCLK",  "PCM5102A DATA",   "PCM5102A DAC en",
    "Reset Radio",    "Exit"};

//-- Rows that hold a live-editable value (long edit mode, red-on-white while
//-- editing); "Encoder Direction" toggles instantly on EN-push instead, and
//-- Reset Radio/Exit are plain actions - none of those three ever enter
//-- edit mode.
static bool settings_row_is_editable(size_t index)
{
  switch (index)
  {
  case 0: //-- Hostname#
  case 1: //-- Attenuating
  case 2: //-- Backlight off
  case 4: //-- PCM5102A BCLK
  case 5: //-- PCM5102A LRCLK
  case 6: //-- PCM5102A DATA
  case 7: //-- PCM5102A DAC enable
    return true;
  default:
    return false;
  }
}

static void format_settings_value(size_t index, const display_message_t* message, char* out,
                                  size_t out_len)
{
  switch (index)
  {
  case 0:
    if (message->settings_hostname_num == 0)
      snprintf(out, out_len, "auto");
    else
      snprintf(out, out_len, "%u", (unsigned)message->settings_hostname_num);
    break;
  case 1:
    snprintf(out, out_len, "%ddB", (int)message->settings_attenuation);
    break;
  case 2:
    if (message->settings_backlight_minutes == 0)
      snprintf(out, out_len, "never");
    else
      snprintf(out, out_len, "%um", (unsigned)message->settings_backlight_minutes);
    break;
  case 3:
    snprintf(out, out_len, "%s", message->settings_encoder_reversed ? "B->A" : "A->B");
    break;
  case 4:
    snprintf(out, out_len, "%d", (int)message->settings_i2s_bclk);
    break;
  case 5:
    snprintf(out, out_len, "%d", (int)message->settings_i2s_ws);
    break;
  case 6:
    snprintf(out, out_len, "%d", (int)message->settings_i2s_dout);
    break;
  case 7:
    if (message->settings_i2s_enable < 0)
      snprintf(out, out_len, "off");
    else
      snprintf(out, out_len, "%d", (int)message->settings_i2s_enable);
    break;
  default:
    out[0] = '\0';
    break;
  }
}

static void draw_settings(const display_message_t* message)
{
  uint16_t width = tft_ec11_width();
  uint16_t height = tft_ec11_height();
  const int title_h = DISPLAY_HEADER_H;
  const int row_h = 24;
  const int scale = 2;
  //-- Reserve space for the bottom hint line, same as draw_station_list().
  const int hint_h = 8 * 2 + 12;
  size_t slot_chars = (width - 8) / (6 * scale);

  int visible_rows = (height - title_h - hint_h) / row_h;
  if (visible_rows < 1)
    visible_rows = 1;
  if ((size_t)visible_rows > SETTINGS_ITEM_COUNT)
    visible_rows = SETTINGS_ITEM_COUNT;

  //-- Windowed the same way as draw_station_list(): center the selection in
  //-- the visible window where possible, clamped to the list's ends.
  size_t max_start = SETTINGS_ITEM_COUNT > (size_t)visible_rows
                         ? SETTINGS_ITEM_COUNT - (size_t)visible_rows
                         : 0;
  size_t half = (size_t)visible_rows / 2;
  size_t start = message->settings_selected > half ? message->settings_selected - half : 0;
  if (start > max_start)
    start = max_start;

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_clear();
  draw_header(width, "Settings", 2);
  draw_header_version(width);

  for (int row = 0; row < visible_rows; row++)
  {
    size_t i = start + (size_t)row;
    if (i >= SETTINGS_ITEM_COUNT)
      break;
    bool selected = i == message->settings_selected;
    int y = title_h + row * row_h + 2;
    char value[24];
    char line[48];
    format_settings_value(i, message, value, sizeof(value));
    if (value[0])
      snprintf(line, sizeof(line), "%s: %s", k_settings_labels[i], value);
    else
      snprintf(line, sizeof(line), "%s", k_settings_labels[i]);

    uint16_t bg = selected ? TFT_EC11_WHITE : TFT_EC11_BLACK;
    //-- Editing an active value is highlighted red-on-white so it's
    //-- unmistakably different from just having it selected.
    uint16_t fg = selected && message->settings_editing && settings_row_is_editable(i)
                      ? TFT_EC11_RED
                      : (selected ? TFT_EC11_BLACK : TFT_EC11_WHITE);

    tft_ec11_fill_rect(0, y, width, row_h, bg);
    tft_ec11_set_text_style(fg, scale);
    size_t len = strlen(line);
    if (len > slot_chars)
      len = slot_chars;
    tft_ec11_set_background(bg);
    tft_ec11_draw_text(4, y + 2, len, line);
  }

  draw_hint("Turn:Scroll EN:Edit");
}

//-- Equalizer screen: 3 fixed rows (Bass/Mid/Treble), always fully redrawn
//-- (updates are rare - one rotation click or one short press at a time).
//-- The selected row is highlighted the same way as the Settings menu
//-- (white background), so the two screens share one visual language.
#define EQ_ITEM_COUNT 3
static const char* k_eq_labels[EQ_ITEM_COUNT] = {"Bass", "Mid", "Treble"};

static void draw_equalizer(const display_message_t* message)
{
  uint16_t width = tft_ec11_width();
  const int title_h = DISPLAY_HEADER_H;
  const int row_h = 24;
  const int scale = 2;
  //-- One blank row's worth of space between the header and "Bass" so the
  //-- list doesn't start flush against the header, matching how the other
  //-- list-style screens leave breathing room below their header.
  const int top_gap = row_h;
  size_t slot_chars = (width - 8) / (6 * scale);
  int8_t values[EQ_ITEM_COUNT] = {message->eq_bass_db, message->eq_mid_db, message->eq_treble_db};

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_clear();
  draw_header(width, "Equalizer", 2);

  for (size_t i = 0; i < EQ_ITEM_COUNT; i++)
  {
    bool selected = i == message->eq_selected;
    int y = title_h + top_gap + (int)i * row_h + 2;
    char line[48];
    snprintf(line, sizeof(line), "%s %+d dB", k_eq_labels[i], (int)values[i]);

    uint16_t bg = selected ? TFT_EC11_WHITE : TFT_EC11_BLACK;
    //-- Editing the selected band is highlighted red-on-white, same
    //-- convention as the [Settings] menu's edit mode.
    uint16_t fg = selected && message->eq_editing ? TFT_EC11_RED
                                                   : (selected ? TFT_EC11_BLACK : TFT_EC11_WHITE);

    tft_ec11_fill_rect(0, y - 2, width, row_h, bg);
    tft_ec11_set_text_style(fg, scale);
    size_t len = strlen(line);
    if (len > slot_chars)
      len = slot_chars;
    tft_ec11_set_background(bg);
    tft_ec11_draw_text(4, y, len, line);
  }

  draw_hint(message->eq_editing ? "Turn:+/-1dB EN:Accept" : "Turn:Scroll EN:Edit");
}

static void display_task(void* argument)
{
  display_message_t message;
  (void)argument;
  for (;;)
  {
    if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE)
      continue;
    switch (message.mode)
    {
    case DISPLAY_MODE_VOLUME:
    {
      bool entering = s_current_mode != DISPLAY_MODE_VOLUME;
      bool station_changed = strcmp(s_cached_station, message.station) != 0;
      s_current_mode = DISPLAY_MODE_VOLUME;
      s_cached_volume = message.volume;
      snprintf(s_cached_station, sizeof(s_cached_station), "%s", message.station);
      if (entering || station_changed)
      {
        draw_volume_full(s_cached_volume, s_cached_station, s_cached_line1, s_cached_line2,
                         s_cached_line3);
      }
      else
      {
        uint16_t width = tft_ec11_width();
        draw_volume_percent(width, s_cached_volume);
        draw_volume_bar(width, s_cached_volume);
      }
      break;
    }
    case DISPLAY_MODE_TITLE:
    {
      bool changed = strcmp(s_cached_line1, message.line1) != 0 ||
                     strcmp(s_cached_line2, message.line2) != 0 ||
                     strcmp(s_cached_line3, message.line3) != 0;
      snprintf(s_cached_line1, sizeof(s_cached_line1), "%s", message.line1);
      snprintf(s_cached_line2, sizeof(s_cached_line2), "%s", message.line2);
      snprintf(s_cached_line3, sizeof(s_cached_line3), "%s", message.line3);
      if (changed && s_current_mode == DISPLAY_MODE_VOLUME)
      {
        draw_now_playing(tft_ec11_width(), s_cached_line1, s_cached_line2, s_cached_line3);
      }
      break;
    }
    case DISPLAY_MODE_STATION_SELECT:
    {
      bool entering = s_current_mode != DISPLAY_MODE_STATION_SELECT;
      s_current_mode = DISPLAY_MODE_STATION_SELECT;
      draw_station_list(message.selected, entering);
      break;
    }
    case DISPLAY_MODE_STATUS:
      s_current_mode = DISPLAY_MODE_STATUS;
      draw_status(message.status);
      break;
    case DISPLAY_MODE_ERROR:
      s_current_mode = DISPLAY_MODE_ERROR;
      draw_error(message.status);
      break;
    case DISPLAY_MODE_TECHNICAL:
      s_current_mode = DISPLAY_MODE_TECHNICAL;
      draw_technical(&message);
      break;
    case DISPLAY_MODE_SETTINGS:
      s_current_mode = DISPLAY_MODE_SETTINGS;
      draw_settings(&message);
      break;
    case DISPLAY_MODE_EQUALIZER:
      s_current_mode = DISPLAY_MODE_EQUALIZER;
      draw_equalizer(&message);
      break;
    case DISPLAY_MODE_BUFFER_FILL:
      //-- Only meaningful on the Volume screen where the bar lives;
      //-- dropped on any other screen so it never draws over a list
      if (s_current_mode == DISPLAY_MODE_VOLUME)
        draw_buffer_bar(message.buffer_fill);
      break;
    }
  }
}

esp_err_t radio_display_init(void)
{
  ESP_RETURN_ON_ERROR(tft_ec11_init(), "display", "piggyback init");
  s_queue = xQueueCreate(8, sizeof(display_message_t));
  if (!s_queue || xTaskCreate(display_task, "radio_display", 4096, NULL, 5, NULL) != pdPASS)
  {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void radio_display_volume(int volume, const char* station)
{
  display_message_t message = {.mode = DISPLAY_MODE_VOLUME, .volume = volume};
  if (station)
    snprintf(message.station, sizeof(message.station), "%s", station);
  ESP_LOGI("display", "VOLUME %d%% | %s", volume, station ? station : "");
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_now_playing(const char* line1, const char* line2, const char* line3)
{
  display_message_t message = {.mode = DISPLAY_MODE_TITLE};
  if (line1)
    snprintf(message.line1, sizeof(message.line1), "%s", line1);
  if (line2)
    snprintf(message.line2, sizeof(message.line2), "%s", line2);
  if (line3)
    snprintf(message.line3, sizeof(message.line3), "%s", line3);
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
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
  const radio_station_t* station = station_store_get(selected);
  ESP_LOGI("display", "SELECT > %s", station ? station->name : "none");
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_status(const char* status)
{
  display_message_t message = {.mode = DISPLAY_MODE_STATUS};
  if (status)
    snprintf(message.status, sizeof(message.status), "%s", status);
  ESP_LOGI("display", "%s", status ? status : "");
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_error(const char* message_text)
{
  display_message_t message = {.mode = DISPLAY_MODE_ERROR};
  if (message_text)
    snprintf(message.status, sizeof(message.status), "%s", message_text);
  ESP_LOGE("display", "%s", message_text ? message_text : "");
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_technical(const char* ssid, const char* ip, const char* mac,
                             const char* hostname, size_t station_count,
                             bool ota_in_progress)
{
  display_message_t message = {.mode = DISPLAY_MODE_TECHNICAL,
                               .station_count = station_count,
                               .ota_in_progress = ota_in_progress};
  snprintf(message.ssid, sizeof(message.ssid), "%s", ssid ? ssid : "-");
  snprintf(message.ip, sizeof(message.ip), "%s", ip ? ip : "-");
  snprintf(message.mac, sizeof(message.mac), "%s", mac ? mac : "-");
  snprintf(message.hostname, sizeof(message.hostname), "%s", hostname ? hostname : "-");
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_buffer_fill(int percent)
{
  display_message_t message = {.mode = DISPLAY_MODE_BUFFER_FILL, .buffer_fill = percent};
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_settings(size_t selected, bool editing, uint16_t hostname_num,
                            int8_t attenuation, uint8_t backlight_minutes, bool encoder_reversed,
                            int16_t i2s_bclk, int16_t i2s_ws, int16_t i2s_dout, int16_t i2s_enable)
{
  display_message_t message = {.mode = DISPLAY_MODE_SETTINGS,
                               .settings_selected = selected,
                               .settings_editing = editing,
                               .settings_hostname_num = hostname_num,
                               .settings_attenuation = attenuation,
                               .settings_backlight_minutes = backlight_minutes,
                               .settings_encoder_reversed = encoder_reversed,
                               .settings_i2s_bclk = i2s_bclk,
                               .settings_i2s_ws = i2s_ws,
                               .settings_i2s_dout = i2s_dout,
                               .settings_i2s_enable = i2s_enable};
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

void radio_display_equalizer(size_t selected, bool editing, int8_t bass_db, int8_t mid_db,
                             int8_t treble_db)
{
  display_message_t message = {.mode = DISPLAY_MODE_EQUALIZER,
                               .eq_selected = selected,
                               .eq_editing = editing,
                               .eq_bass_db = bass_db,
                               .eq_mid_db = mid_db,
                               .eq_treble_db = treble_db};
  if (s_queue)
    (void)xQueueSend(s_queue, &message, 0);
}

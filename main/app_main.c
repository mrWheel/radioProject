#include "wifi_provisioner.h"
#include "radio_storage.h"
#include "station_store.h"
#include "radio_input.h"
#include "radio_display.h"
#include "radio_audio.h"
#include "radio_settings.h"
#include "web_gui.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

//-- never remove this constant; it indicates the program version
const char* PROG_VERSION = "v1.2.2";

//-- How long the "Connected: SSID / IP" screen stays up before switching to
//-- the Volume/PLAY screen, so the user can actually read it.
#define WIFI_CONNECTED_SPLASH_MS 2500
typedef enum
{
  UI_VOLUME,
  UI_STATION_SELECT
} ui_mode_t;
typedef struct
{
  ui_mode_t mode;
  int volume;
  size_t playing;
  size_t selected;
  TickType_t last_rotation;
} app_state_t;
static QueueHandle_t s_events;
static app_state_t s = {.mode = UI_VOLUME, .volume = CONFIG_RADIO_DEFAULT_VOLUME};
static void input_cb(radio_input_event_t e, void* ctx)
{
  (void)ctx;
  xQueueSend(s_events, &e, 0);
}

//-- ICY "StreamTitle" often packs multiple fields (artist/track/album/station)
//-- into one string separated by "<space>TOKEN<space>" tokens; recognized
//-- tokens are checked left-to-right and the earliest match wins. Add more
//-- tokens here as new stations turn up.
static const char* k_icy_separators[] = {
    " - ", " | ", " / ", " ~ ", " :: ", " – ", " — ", " • ", " · "};

static const char* find_icy_separator(const char* text, size_t* match_len)
{
  const char* best = NULL;
  size_t best_len = 0;
  for (size_t i = 0; i < sizeof(k_icy_separators) / sizeof(k_icy_separators[0]); i++)
  {
    const char* hit = strstr(text, k_icy_separators[i]);
    if (hit && (!best || hit < best))
    {
      best = hit;
      best_len = strlen(k_icy_separators[i]);
    }
  }
  if (match_len)
    *match_len = best_len;
  return best;
}

//-- Splits `raw` into at most 3 segments (pointer+length into `raw`, no copy)
//-- at the first 2 separator matches; any further separators stay embedded as
//-- literal text in the 3rd segment instead of being lost. Empty segments
//-- (e.g. a leading " - Track") are dropped so they don't waste a line.
static size_t split_icy_segments(const char* raw, const char* seg_start[3], size_t seg_len[3])
{
  size_t count = 0;
  const char* cursor = raw;
  while (count < 2)
  {
    size_t match_len;
    const char* sep = find_icy_separator(cursor, &match_len);
    if (!sep)
      break;
    seg_start[count] = cursor;
    seg_len[count] = (size_t)(sep - cursor);
    count++;
    cursor = sep + match_len;
  }
  seg_start[count] = cursor;
  seg_len[count] = strlen(cursor);
  count++;

  size_t kept = 0;
  for (size_t i = 0; i < count; i++)
  {
    if (seg_len[i] == 0)
      continue;
    seg_start[kept] = seg_start[i];
    seg_len[kept] = seg_len[i];
    kept++;
  }
  return kept;
}

//-- Greedy single-space word-wrap of one segment into at most `max_lines`
//-- lines of at most `max_chars` characters, breaking on the last space that
//-- keeps a line within budget (hard-cut if a single word is wider than
//-- `max_chars`). Writes into lines_out[*out_count..] and advances
//-- *out_count; text left over once `max_lines` is used up is dropped, since
//-- 3 lines is the hard budget.
static void emit_wrapped(const char* text, size_t len, size_t max_chars, char lines_out[][160],
                         int* out_count, int max_lines)
{
  size_t pos = 0;
  for (int produced = 0; produced < max_lines && pos < len; produced++)
  {
    size_t remaining = len - pos;
    size_t take = remaining;
    if (take > max_chars)
    {
      take = max_chars;
      size_t brk = take;
      while (brk > 0 && text[pos + brk] != ' ')
        brk--;
      if (brk > 0)
        take = brk;
      //-- Never hard-cut inside a multi-byte UTF-8 character (e.g. an
      //-- accented letter like "e" in "arabo-egyptienne"): back off
      //-- over any continuation bytes (0x80-0xBF) at the cut point.
      while (take > 0 && ((unsigned char)text[pos + take] & 0xC0) == 0x80)
        take--;
    }
    char* dst = lines_out[*out_count];
    memcpy(dst, text + pos, take);
    dst[take] = '\0';
    (*out_count)++;
    pos += take;
    while (pos < len && text[pos] == ' ')
      pos++;
  }
}

//-- Splits ICY metadata into up to 3 display-ready lines, once here, so
//-- neither the display nor the web GUI has to re-decide it (they always end
//-- up showing the same 3 lines). Separators come first (split_icy_segments);
//-- segments are then laid out strictly left-to-right, each one taking as
//-- many of the remaining lines as it needs (word-wrapped on single spaces
//-- if it's wider than the display) before the next segment gets a turn. So
//-- the leftmost, most important segment is never sacrificed to make room for
//-- a later one; a later segment (or the tail of one) that no longer fits in
//-- the 3-line budget is simply dropped. Unused trailing lines fall back to "-".
static void build_icy_lines(const char* raw, char line1[160], char line2[160], char line3[160])
{
  char lines_out[3][160];
  int out_count = 0;
  size_t max_chars = radio_display_title_max_chars();
  if (max_chars == 0 || max_chars >= sizeof(lines_out[0]))
    max_chars = sizeof(lines_out[0]) - 1;

  const char* seg_start[3];
  size_t seg_len[3];
  size_t seg_count = split_icy_segments(raw ? raw : "", seg_start, seg_len);

  for (size_t i = 0; i < seg_count && out_count < 3; i++)
  {
    int budget = 3 - out_count;
    emit_wrapped(seg_start[i], seg_len[i], max_chars, lines_out, &out_count, budget);
  }
  while (out_count < 3)
  {
    snprintf(lines_out[out_count], sizeof(lines_out[out_count]), "-");
    out_count++;
  }

  snprintf(line1, 160, "%s", lines_out[0]);
  snprintf(line2, 160, "%s", lines_out[1]);
  snprintf(line3, 160, "%s", lines_out[2]);
}

//-- Last real (non-empty) title split, kept up to date even while muted so the
//-- very first StreamTitle of a station isn't lost: it can arrive from
//-- fetch_task() before radio_audio finishes its decode warm-up and unmutes.
static char s_last_line1[160] = "-";
static char s_last_line2[160] = "-";
static char s_last_line3[160] = "-";

static void title_cb(const char* title, void* ctx)
{
  (void)ctx;
  //-- fetch_task() fires a dummy title_cb("", ctx) as soon as it starts;
  //-- ignore only that empty placeholder, not real titles.
  if (!title || !title[0])
    return;
  char line1[160], line2[160], line3[160];
  build_icy_lines(title, line1, line2, line3);
  snprintf(s_last_line1, sizeof(s_last_line1), "%s", line1);
  snprintf(s_last_line2, sizeof(s_last_line2), "%s", line2);
  snprintf(s_last_line3, sizeof(s_last_line3), "%s", line3);
  //-- Stay silent while muted so "Switch Station .." (see
  //-- on_audio_mute_changed) stays on screen for the whole switch; the
  //-- cached title above is flushed the moment we unmute instead.
  if (radio_audio_is_muted())
    return;
  radio_display_now_playing(line1, line2, line3);
  web_gui_notify_title(line1, line2, line3);
}

//-- Fired by radio_audio the instant a station switch starts (still muted)
//-- and again once the new stream has actually started playing (unmuted),
//-- so both the physical display and the web GUI show a clear "switching"
//-- state instead of stale or blank artist/track text during the mute window.
static void on_audio_mute_changed(bool muted, void* ctx)
{
  (void)ctx;
  if (muted)
  {
    snprintf(s_last_line1, sizeof(s_last_line1), "-");
    snprintf(s_last_line2, sizeof(s_last_line2), "-");
    snprintf(s_last_line3, sizeof(s_last_line3), "-");
    radio_display_now_playing("Switch Station ..", "-", "-");
    web_gui_notify_title("Switch Station ..", "-", "-");
  }
  else
  {
    //-- Flush whatever real title already arrived during the mute
    //-- window (the first ICY metadata block often lands here) instead
    //-- of forcing a stale "-"/"-"/"-" that only a *second* title update
    //-- would have overwritten.
    radio_display_now_playing(s_last_line1, s_last_line2, s_last_line3);
    web_gui_notify_title(s_last_line1, s_last_line2, s_last_line3);
  }
}

static void show_volume(void)
{
  const radio_station_t* st = station_store_get(s.playing);
  radio_display_volume(s.volume, st ? st->name : "");
}

//-- Fired by radio_audio when fetch_task ends without a deliberate stop/switch
//-- (connection open failed after all retries, or a mid-stream read/reconnect
//-- failure) so the failure is visible instead of a stale "Switch Station .."
//-- or last-known artist/track lingering on both the physical display and the
//-- web GUI forever.
static void stall_cb(bool stalled, void* ctx)
{
  (void)ctx;
  if (!stalled)
    return;
  radio_display_now_playing("Stream stalled", "-", "-");
  web_gui_notify_title("Stream stalled", "-", "-");
}

//-- Fired by web_gui after a browser-issued command actually changed the
//-- playing station or volume, so the physical UI's own tracking (and the
//-- TFT) don't go stale when the change came from the web GUI instead of
//-- the EC11.
static void on_web_gui_state_applied(size_t station_index, int volume, void* ctx)
{
  (void)ctx;
  s.playing = s.selected = station_index;
  s.volume = volume;
  if (s.mode == UI_VOLUME)
    show_volume();
}

//-- Fired from inside wifi_prov_start() (stored-credential path) or from the
//-- portal's async event task (captive-portal path) once STA is up. Reads the
//-- SSID/IP back from the WiFi driver itself, since wifi_prov_on_connected_cb_t
//-- takes no arguments.
static void on_wifi_connected(void)
{
  wifi_ap_record_t ap_info;
  esp_netif_ip_info_t ip_info;
  char msg[96];
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && wifi_prov_get_ip_info(&ip_info) == ESP_OK)
  {
    snprintf(msg, sizeof(msg), "SSID: %s\nIP: " IPSTR, (const char*)ap_info.ssid,
             IP2STR(&ip_info.ip));
  }
  else
  {
    snprintf(msg, sizeof(msg), "WiFi connected");
  }
  radio_display_status(msg);
}

//-- Fired when no stored credentials worked and the captive portal AP starts,
//-- so the user sees which AP to join instead of a stale "connecting" screen.
static void on_wifi_portal_start(void)
{
  char msg[96];
  snprintf(msg, sizeof(msg), "Connect to WiFi '%s' to configure", CONFIG_WIFI_PROV_AP_SSID);
  radio_display_status(msg);
}
static void ui_task(void* arg)
{
  radio_input_event_t e;
  for (;;)
  {
    if (xQueueReceive(s_events, &e, pdMS_TO_TICKS(100)))
    {
      size_t count = station_store_count();
      if (e == RADIO_INPUT_AUX_PUSH)
      {
        ESP_LOGI("radio", "Reserved AUX button pressed");
        continue;
      }
      if (e == RADIO_INPUT_EN_PUSH)
      {
        if (s.mode == UI_VOLUME)
        {
          s.mode = UI_STATION_SELECT;
          s.selected = s.playing;
          s.last_rotation = xTaskGetTickCount();
          radio_display_station_list(s.selected);
        }
        else
        {
          s.playing = s.selected;
          s.mode = UI_VOLUME;
          radio_audio_play(station_store_get(s.playing));
          radio_settings_save(s.playing);
          show_volume();
          web_gui_notify_device_state(s.playing);
        }
        continue;
      }
      if (e == RADIO_INPUT_ROTATE_LEFT || e == RADIO_INPUT_ROTATE_RIGHT)
      {
        int d = e == RADIO_INPUT_ROTATE_RIGHT ? 1 : -1;
        if (s.mode == UI_VOLUME)
        {
          s.volume += d * 2;
          if (s.volume < 0)
            s.volume = 0;
          if (s.volume > 100)
            s.volume = 100;
          radio_audio_set_volume(s.volume);
          show_volume();
          web_gui_notify_device_state(s.playing);
        }
        else if (count)
        {
          long next = (long)s.selected + d;
          if (next < 0)
            next = 0;
          if (next >= (long)count)
            next = (long)count - 1;
          s.selected = (size_t)next;
          s.last_rotation = xTaskGetTickCount();
          radio_display_station_list(s.selected);
        }
      }
    }
    if (s.mode == UI_STATION_SELECT &&
        xTaskGetTickCount() - s.last_rotation >= pdMS_TO_TICKS(CONFIG_RADIO_SELECTION_TIMEOUT_MS))
    {
      s.mode = UI_VOLUME;
      show_volume();
    }
  }
}
//-- Polls the stream-buffer fill level and forwards it to the display, so
//-- the bottom-right bar tracks the buffer in near-real-time without
//-- coupling radio_audio to radio_display.
static void buffer_monitor_task(void* arg)
{
  (void)arg;
  int last_sent = -1;
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(500));
    int pct = radio_audio_get_buffer_fill_percent();
    if (pct != last_sent)
    {
      last_sent = pct;
      radio_display_buffer_fill(pct);
    }
  }
}

void app_main(void)
{
  ESP_ERROR_CHECK(radio_settings_init());
  ESP_ERROR_CHECK(radio_storage_mount());
  ESP_ERROR_CHECK(station_store_load());
  size_t saved_station = 0;
  radio_settings_load(&saved_station);
  size_t count = station_store_count();
  if (count && saved_station < count)
  {
    s.playing = s.selected = saved_station;
  }
  ESP_ERROR_CHECK(radio_display_init());
  radio_display_status("");
  ESP_ERROR_CHECK(radio_audio_init());
  radio_audio_set_volume(s.volume);
  radio_audio_set_title_callback(title_cb, NULL);
  radio_audio_set_mute_callback(on_audio_mute_changed, NULL);
  radio_audio_set_stall_callback(stall_cb, NULL);
  web_gui_set_state_applied_cb(on_web_gui_state_applied, NULL);
  s_events = xQueueCreate(12, sizeof(radio_input_event_t));
  ESP_ERROR_CHECK(radio_input_start(input_cb, NULL));
  wifi_prov_config_t wc = WIFI_PROV_DEFAULT_CONFIG();
  wc.on_connected = on_wifi_connected;
  wc.on_portal_start = on_wifi_portal_start;
  radio_display_status("Connecting to WiFi");
  ESP_ERROR_CHECK(wifi_prov_start(&wc));
  if (wifi_prov_wait_for_connection(portMAX_DELAY) == ESP_OK)
  {
    vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECTED_SPLASH_MS));
    radio_audio_play(station_store_get(s.playing));
    show_volume();
    ESP_ERROR_CHECK(web_gui_init());
    web_gui_notify_device_state(s.playing);
  }
  xTaskCreate(ui_task, "radio_ui", 4096, NULL, 6, NULL);
  xTaskCreate(buffer_monitor_task, "buf_mon", 2048, NULL, 5, NULL);
}

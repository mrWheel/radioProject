#include "wifi_provisioner.h"
#include "ota_upload.h"
#include "radio_storage.h"
#include "station_store.h"
#include "radio_input.h"
#include "radio_display.h"
#include "radio_audio.h"
#include "radio_board.h"
#include "radio_settings.h"
#include "web_gui.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

//-- never remove this constant; it indicates the program version
const char* PROG_VERSION = "v1.1.5";

//-- How long the "Connected: SSID / IP" screen stays up before switching to
//-- the Volume/PLAY screen, so the user can actually read it.
#define WIFI_CONNECTED_SPLASH_MS 2500
#define TECHNICAL_INFO_TIMEOUT_MS 30000
//-- [Settings] menu: return to Volume after 120s without input, and the
//-- bounds each editable item is clamped to (see radio_settings/radio_audio).
#define SETTINGS_TIMEOUT_MS 120000
#define SETTINGS_HOSTNAME_MAX 256
#define SETTINGS_BACKLIGHT_MAX_MIN 60
#define SETTINGS_ATTEN_DEFAULT_DB (-6)
#define SETTINGS_BACKLIGHT_DEFAULT_MIN 5
//-- PCM5102A I2S GPIO range: -1 disables (DAC enable only), 0-48 is a valid
//-- ESP32-S3 GPIO.
#define SETTINGS_I2S_GPIO_MIN (-1)
#define SETTINGS_I2S_GPIO_MAX 48
//-- Equalizer screen: return to Volume after 30s without input (rotation,
//-- EC-button press, or AUX-button press all reset this).
#define EQ_TIMEOUT_MS 30000
typedef enum
{
  UI_VOLUME,
  UI_STATION_SELECT,
  UI_TECHNICAL,
  UI_SETTINGS,
  UI_EQUALIZER
} ui_mode_t;
typedef enum
{
  SETTINGS_ITEM_HOSTNAME,
  SETTINGS_ITEM_ATTENUATION,
  SETTINGS_ITEM_BACKLIGHT,
  SETTINGS_ITEM_ENCODER_DIR,
  SETTINGS_ITEM_I2S_BCLK,
  SETTINGS_ITEM_I2S_WS,
  SETTINGS_ITEM_I2S_DOUT,
  SETTINGS_ITEM_I2S_ENABLE,
  SETTINGS_ITEM_RESET,
  SETTINGS_ITEM_EXIT,
  SETTINGS_ITEM_COUNT
} settings_item_t;
typedef enum
{
  EQ_ITEM_BASS,
  EQ_ITEM_MID,
  EQ_ITEM_TREBLE,
  EQ_ITEM_COUNT
} eq_item_t;
typedef struct
{
  ui_mode_t mode;
  int volume;
  size_t playing;
  size_t selected;
  TickType_t last_rotation;
  TickType_t technical_started;
  size_t settings_item;
  bool settings_editing;
  TickType_t settings_last_input;
  uint16_t hostname_num;
  int8_t attenuation;
  uint8_t backlight_minutes;
  bool encoder_reversed;
  //-- PCM5102A I2S GPIO overrides; boot-only, applied before radio_audio_init().
  int16_t i2s_bclk;
  int16_t i2s_ws;
  int16_t i2s_dout;
  int16_t i2s_enable;
  size_t eq_selected;
  bool eq_editing;
  int8_t eq_bass;
  int8_t eq_mid;
  int8_t eq_treble;
  //-- Last values actually written to NVS, so eq_save_if_changed() only
  //-- writes bands that were really touched while the screen was open.
  int8_t eq_bass_saved;
  int8_t eq_mid_saved;
  int8_t eq_treble_saved;
  TickType_t eq_last_input;
} app_state_t;
static QueueHandle_t s_events;
static volatile bool s_ota_in_progress = false;
static app_state_t s = {.mode = UI_VOLUME,
                       .volume = CONFIG_RADIO_DEFAULT_VOLUME,
                       .attenuation = SETTINGS_ATTEN_DEFAULT_DB,
                       .backlight_minutes = SETTINGS_BACKLIGHT_DEFAULT_MIN};
//-- Filled once in app_main() from the last 3 MAC bytes; the AP SSID uses
//-- colons, the mDNS hostname uses dashes since DNS labels can't hold colons.
static char s_ap_ssid[32];
static char s_mdns_hostname[32];

//-- Builds "Radio-<b3><sep><b2><sep><b1>" from the station MAC's last 3 bytes.
static void build_device_name(char* out, size_t out_len, char sep)
{
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, out_len, "Radio-%02X%c%02X%c%02X", mac[3], sep, mac[4], sep, mac[5]);
}
//-- Settings-menu "Hostname#" override: 0 keeps the MAC-derived name
//-- ("Radio-xx-yy-zz"/"Radio-xx:xx:xx"), 1-256 forces "Radio-<n>" for both
//-- the AP SSID and the mDNS hostname (no colon/dash distinction needed
//-- since it's purely numeric).
static void build_device_name_effective(char* out, size_t out_len, char sep,
                                        uint16_t hostname_num)
{
  if (hostname_num > 0)
    snprintf(out, out_len, "Radio-%u", (unsigned)hostname_num);
  else
    build_device_name(out, out_len, sep);
}
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

static void show_settings(void)
{
  radio_display_settings(s.settings_item, s.settings_editing, s.hostname_num, s.attenuation,
                         s.backlight_minutes, s.encoder_reversed, s.i2s_bclk, s.i2s_ws,
                         s.i2s_dout, s.i2s_enable);
}

static void show_equalizer(void)
{
  radio_display_equalizer(s.eq_selected, s.eq_editing, s.eq_bass, s.eq_mid, s.eq_treble);
}

//-- Only writes bands that actually changed since the last save, so turning
//-- the Equalizer screen on/off without touching a band never causes a
//-- needless NVS write.
static void eq_save_if_changed(void)
{
  if (s.eq_bass != s.eq_bass_saved)
  {
    radio_settings_save_eq_bass(s.eq_bass);
    s.eq_bass_saved = s.eq_bass;
  }
  if (s.eq_mid != s.eq_mid_saved)
  {
    radio_settings_save_eq_mid(s.eq_mid);
    s.eq_mid_saved = s.eq_mid;
  }
  if (s.eq_treble != s.eq_treble_saved)
  {
    radio_settings_save_eq_treble(s.eq_treble);
    s.eq_treble_saved = s.eq_treble;
  }
}

//-- Leaves the Equalizer screen (long-press or 30s-idle timeout): persist
//-- whatever changed, keep the audio EQ active (it was already applied live
//-- while adjusting), and return to Volume.
static void eq_exit(void)
{
  eq_save_if_changed();
  s.mode = UI_VOLUME;
  show_volume();
}

static void show_technical(void)
{
  wifi_ap_record_t ap_info;
  esp_netif_ip_info_t ip_info;
  uint8_t mac[6] = {0};
  char ssid[sizeof(ap_info.ssid) + 1] = "-";
  char ip[24] = "-";
  char mac_text[18];

  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    snprintf(ssid, sizeof(ssid), "%s", (const char*)ap_info.ssid);
  if (wifi_prov_get_ip_info(&ip_info) == ESP_OK)
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  radio_display_technical(ssid, ip, mac_text, s_mdns_hostname, station_store_count(),
                          s_ota_in_progress);
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

static esp_err_t prepare_ota_cb(void* ctx)
{
  (void)ctx;
  s_ota_in_progress = true;
  s.mode = UI_TECHNICAL;
  show_technical();
  return radio_audio_prepare_for_ota();
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
  snprintf(msg, sizeof(msg), "Connect to WiFi '%s' to configure", s_ap_ssid);
  radio_display_status(msg);
}
static void ui_task(void* arg)
{
  radio_input_event_t e;
  for (;;)
  {
    if (xQueueReceive(s_events, &e, pdMS_TO_TICKS(100)))
    {
      if (s_ota_in_progress)
        continue;

      size_t count = station_store_count();
      if (e == RADIO_INPUT_AUX_PUSH)
      {
        if (s.mode == UI_TECHNICAL)
        {
          s.mode = UI_VOLUME;
          show_volume();
        }
        else if (s.mode == UI_EQUALIZER)
        {
          //-- Counts as user input for the 30s idle timeout, but has no
          //-- other effect on this screen.
          s.eq_last_input = xTaskGetTickCount();
        }
        ESP_LOGI("radio", "AUX short press");
        continue;
      }
      if (e == RADIO_INPUT_AUX_MEDIUM_PUSH)
      {
        s.mode = UI_TECHNICAL;
        s.technical_started = xTaskGetTickCount();
        show_technical();
        continue;
      }
      if (e == RADIO_INPUT_AUX_LONG_PUSH)
      {
        s.mode = UI_SETTINGS;
        s.settings_item = SETTINGS_ITEM_HOSTNAME;
        s.settings_editing = false;
        s.settings_last_input = xTaskGetTickCount();
        show_settings();
        continue;
      }
      if (e == RADIO_INPUT_EN_LONG_PUSH)
      {
        if (s.mode == UI_VOLUME)
        {
          s.mode = UI_EQUALIZER;
          s.eq_selected = EQ_ITEM_BASS;
          s.eq_editing = false;
          s.eq_last_input = xTaskGetTickCount();
          show_equalizer();
        }
        else if (s.mode == UI_EQUALIZER)
        {
          eq_exit();
        }
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
        else if (s.mode == UI_EQUALIZER)
        {
          //-- First short press: select the highlighted band for editing.
          //-- Second short press: accept/lock the value back to scrolling.
          s.eq_editing = !s.eq_editing;
          s.eq_last_input = xTaskGetTickCount();
          show_equalizer();
        }
        else if (s.mode == UI_SETTINGS)
        {
          s.settings_last_input = xTaskGetTickCount();
          if (s.settings_item == SETTINGS_ITEM_EXIT)
          {
            s.mode = UI_VOLUME;
            show_volume();
          }
          else if (s.settings_item == SETTINGS_ITEM_RESET)
          {
            ESP_LOGI("radio", "Settings: Reset Radio selected, rebooting");
            esp_restart();
          }
          else if (s.settings_item == SETTINGS_ITEM_ENCODER_DIR)
          {
            //-- Toggle-style item: no separate edit mode, EN-push flips and
            //-- persists it immediately.
            s.encoder_reversed = !s.encoder_reversed;
            radio_input_set_encoder_reversed(s.encoder_reversed);
            radio_settings_save_encoder_reversed(s.encoder_reversed ? 1 : 0);
            show_settings();
          }
          else if (s.settings_editing)
          {
            //-- Second short-press: lock the value in and persist it.
            s.settings_editing = false;
            switch (s.settings_item)
            {
            case SETTINGS_ITEM_HOSTNAME:
              radio_settings_save_hostname_num(s.hostname_num);
              break;
            case SETTINGS_ITEM_ATTENUATION:
              radio_settings_save_attenuation(s.attenuation);
              break;
            case SETTINGS_ITEM_BACKLIGHT:
              radio_settings_save_backlight_minutes(s.backlight_minutes);
              break;
            case SETTINGS_ITEM_I2S_BCLK:
              radio_settings_save_i2s_bclk(s.i2s_bclk);
              break;
            case SETTINGS_ITEM_I2S_WS:
              radio_settings_save_i2s_ws(s.i2s_ws);
              break;
            case SETTINGS_ITEM_I2S_DOUT:
              radio_settings_save_i2s_dout(s.i2s_dout);
              break;
            case SETTINGS_ITEM_I2S_ENABLE:
              radio_settings_save_i2s_enable(s.i2s_enable);
              break;
            default:
              break;
            }
            show_settings();
          }
          else
          {
            //-- First short-press on an editable value: enter edit mode.
            s.settings_editing = true;
            show_settings();
          }
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
        else if (s.mode == UI_SETTINGS)
        {
          s.settings_last_input = xTaskGetTickCount();
          if (s.settings_editing)
          {
            switch (s.settings_item)
            {
            case SETTINGS_ITEM_HOSTNAME:
            {
              long next = (long)s.hostname_num + d;
              if (next < 0)
                next = 0;
              if (next > SETTINGS_HOSTNAME_MAX)
                next = SETTINGS_HOSTNAME_MAX;
              s.hostname_num = (uint16_t)next;
              break;
            }
            case SETTINGS_ITEM_ATTENUATION:
            {
              long next = (long)s.attenuation + d;
              if (next < RADIO_AUDIO_ATTEN_MIN_DB)
                next = RADIO_AUDIO_ATTEN_MIN_DB;
              if (next > RADIO_AUDIO_ATTEN_MAX_DB)
                next = RADIO_AUDIO_ATTEN_MAX_DB;
              s.attenuation = (int8_t)next;
              radio_audio_set_attenuation(s.attenuation);
              break;
            }
            case SETTINGS_ITEM_BACKLIGHT:
            {
              long next = (long)s.backlight_minutes + d;
              if (next < 0)
                next = 0;
              if (next > SETTINGS_BACKLIGHT_MAX_MIN)
                next = SETTINGS_BACKLIGHT_MAX_MIN;
              s.backlight_minutes = (uint8_t)next;
              radio_input_set_backlight_timeout_minutes(s.backlight_minutes);
              break;
            }
            //-- I2S GPIO pins are boot-only (latched once by
            //-- radio_audio_init()); adjusting/saving here only takes
            //-- effect after the next "Reset Radio" or power cycle.
            case SETTINGS_ITEM_I2S_BCLK:
            {
              long next = (long)s.i2s_bclk + d;
              if (next < SETTINGS_I2S_GPIO_MIN)
                next = SETTINGS_I2S_GPIO_MIN;
              if (next > SETTINGS_I2S_GPIO_MAX)
                next = SETTINGS_I2S_GPIO_MAX;
              s.i2s_bclk = (int16_t)next;
              break;
            }
            case SETTINGS_ITEM_I2S_WS:
            {
              long next = (long)s.i2s_ws + d;
              if (next < SETTINGS_I2S_GPIO_MIN)
                next = SETTINGS_I2S_GPIO_MIN;
              if (next > SETTINGS_I2S_GPIO_MAX)
                next = SETTINGS_I2S_GPIO_MAX;
              s.i2s_ws = (int16_t)next;
              break;
            }
            case SETTINGS_ITEM_I2S_DOUT:
            {
              long next = (long)s.i2s_dout + d;
              if (next < SETTINGS_I2S_GPIO_MIN)
                next = SETTINGS_I2S_GPIO_MIN;
              if (next > SETTINGS_I2S_GPIO_MAX)
                next = SETTINGS_I2S_GPIO_MAX;
              s.i2s_dout = (int16_t)next;
              break;
            }
            case SETTINGS_ITEM_I2S_ENABLE:
            {
              long next = (long)s.i2s_enable + d;
              if (next < SETTINGS_I2S_GPIO_MIN)
                next = SETTINGS_I2S_GPIO_MIN;
              if (next > SETTINGS_I2S_GPIO_MAX)
                next = SETTINGS_I2S_GPIO_MAX;
              s.i2s_enable = (int16_t)next;
              break;
            }
            default:
              break;
            }
          }
          else
          {
            long next = (long)s.settings_item + d;
            if (next < 0)
              next = 0;
            if (next >= (long)SETTINGS_ITEM_COUNT)
              next = (long)SETTINGS_ITEM_COUNT - 1;
            s.settings_item = (size_t)next;
          }
          show_settings();
        }
        else if (s.mode == UI_EQUALIZER)
        {
          s.eq_last_input = xTaskGetTickCount();
          if (!s.eq_editing)
          {
            //-- Not editing: rotation just scrolls Bass/Mid/Treble.
            long next = (long)s.eq_selected + d;
            if (next < 0)
              next = 0;
            if (next >= (long)EQ_ITEM_COUNT)
              next = (long)EQ_ITEM_COUNT - 1;
            s.eq_selected = (size_t)next;
          }
          else
          {
            int8_t* target = s.eq_selected == EQ_ITEM_BASS   ? &s.eq_bass
                             : s.eq_selected == EQ_ITEM_MID  ? &s.eq_mid
                                                              : &s.eq_treble;
            long next = (long)*target + d;
            if (next < RADIO_AUDIO_EQ_MIN_DB)
              next = RADIO_AUDIO_EQ_MIN_DB;
            if (next > RADIO_AUDIO_EQ_MAX_DB)
              next = RADIO_AUDIO_EQ_MAX_DB;
            *target = (int8_t)next;
            //-- Applied immediately so the effect is audible while turning.
            switch (s.eq_selected)
            {
            case EQ_ITEM_BASS:
              radio_audio_set_eq_bass(s.eq_bass);
              break;
            case EQ_ITEM_MID:
              radio_audio_set_eq_mid(s.eq_mid);
              break;
            case EQ_ITEM_TREBLE:
              radio_audio_set_eq_treble(s.eq_treble);
              break;
            default:
              break;
            }
          }
          show_equalizer();
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
    if (s_ota_in_progress)
      continue;

    if (s.mode == UI_STATION_SELECT &&
        xTaskGetTickCount() - s.last_rotation >= pdMS_TO_TICKS(CONFIG_RADIO_SELECTION_TIMEOUT_MS))
    {
      s.mode = UI_VOLUME;
      show_volume();
    }
    if (s.mode == UI_TECHNICAL &&
        xTaskGetTickCount() - s.technical_started >= pdMS_TO_TICKS(TECHNICAL_INFO_TIMEOUT_MS))
    {
      s.mode = UI_VOLUME;
      show_volume();
    }
    if (s.mode == UI_SETTINGS &&
        xTaskGetTickCount() - s.settings_last_input >= pdMS_TO_TICKS(SETTINGS_TIMEOUT_MS))
    {
      s.mode = UI_VOLUME;
      show_volume();
    }
    if (s.mode == UI_EQUALIZER &&
        xTaskGetTickCount() - s.eq_last_input >= pdMS_TO_TICKS(EQ_TIMEOUT_MS))
    {
      eq_exit();
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
  ESP_ERROR_CHECK(radio_display_init());

  //-- Input must be ready before the possible ERROR screen below, so a
  //-- button press or encoder rotation can dismiss it.
  s_events = xQueueCreate(12, sizeof(radio_input_event_t));
  ESP_ERROR_CHECK(radio_input_start(input_cb, NULL));

  //-- [Settings] menu values: read from NVS and fall back to their
  //-- documented defaults when never saved before (fresh device) or out of
  //-- bounds. Applied immediately so they take effect for the rest of boot
  //-- (hostname/AP name, output attenuation, backlight timeout).
  uint16_t hostname_num = 0;
  if (radio_settings_load_hostname_num(&hostname_num) != ESP_OK ||
      hostname_num > SETTINGS_HOSTNAME_MAX)
    hostname_num = 0;
  s.hostname_num = hostname_num;

  int8_t attenuation = SETTINGS_ATTEN_DEFAULT_DB;
  if (radio_settings_load_attenuation(&attenuation) != ESP_OK ||
      attenuation < RADIO_AUDIO_ATTEN_MIN_DB || attenuation > RADIO_AUDIO_ATTEN_MAX_DB)
    attenuation = SETTINGS_ATTEN_DEFAULT_DB;
  s.attenuation = attenuation;

  uint8_t backlight_minutes = SETTINGS_BACKLIGHT_DEFAULT_MIN;
  if (radio_settings_load_backlight_minutes(&backlight_minutes) != ESP_OK ||
      backlight_minutes > SETTINGS_BACKLIGHT_MAX_MIN)
    backlight_minutes = SETTINGS_BACKLIGHT_DEFAULT_MIN;
  s.backlight_minutes = backlight_minutes;
  radio_input_set_backlight_timeout_minutes(s.backlight_minutes);

  uint8_t encoder_reversed = RADIO_ENCODER_REVERSED_DEFAULT;
  if (radio_settings_load_encoder_reversed(&encoder_reversed) != ESP_OK)
    encoder_reversed = RADIO_ENCODER_REVERSED_DEFAULT;
  s.encoder_reversed = encoder_reversed != 0;
  radio_input_set_encoder_reversed(s.encoder_reversed);

  //-- PCM5102A I2S GPIO overrides: default is the "Radio hardware" Kconfig
  //-- value, applied only once here (before radio_audio_init()) since the
  //-- pins are latched at I2S channel setup, same boot-only pattern as
  //-- Hostname#.
  int16_t i2s_bclk = RADIO_I2S_BCLK;
  if (radio_settings_load_i2s_bclk(&i2s_bclk) != ESP_OK || i2s_bclk < SETTINGS_I2S_GPIO_MIN ||
      i2s_bclk > SETTINGS_I2S_GPIO_MAX)
    i2s_bclk = RADIO_I2S_BCLK;
  s.i2s_bclk = i2s_bclk;

  int16_t i2s_ws = RADIO_I2S_WS;
  if (radio_settings_load_i2s_ws(&i2s_ws) != ESP_OK || i2s_ws < SETTINGS_I2S_GPIO_MIN ||
      i2s_ws > SETTINGS_I2S_GPIO_MAX)
    i2s_ws = RADIO_I2S_WS;
  s.i2s_ws = i2s_ws;

  int16_t i2s_dout = RADIO_I2S_DOUT;
  if (radio_settings_load_i2s_dout(&i2s_dout) != ESP_OK || i2s_dout < SETTINGS_I2S_GPIO_MIN ||
      i2s_dout > SETTINGS_I2S_GPIO_MAX)
    i2s_dout = RADIO_I2S_DOUT;
  s.i2s_dout = i2s_dout;

  int16_t i2s_enable = RADIO_I2S_ENABLE;
  if (radio_settings_load_i2s_enable(&i2s_enable) != ESP_OK || i2s_enable < SETTINGS_I2S_GPIO_MIN ||
      i2s_enable > SETTINGS_I2S_GPIO_MAX)
    i2s_enable = RADIO_I2S_ENABLE;
  s.i2s_enable = i2s_enable;
  radio_audio_set_i2s_pins(s.i2s_bclk, s.i2s_ws, s.i2s_dout, s.i2s_enable);

  //-- Equalizer: default/fallback is 0dB (no correction) per band, same as
  //-- a missing NVS value or a value outside -12..+12 (defensive against a
  //-- corrupted/foreign NVS entry).
  int8_t eq_bass = 0;
  if (radio_settings_load_eq_bass(&eq_bass) != ESP_OK || eq_bass < RADIO_AUDIO_EQ_MIN_DB ||
      eq_bass > RADIO_AUDIO_EQ_MAX_DB)
    eq_bass = 0;
  s.eq_bass = s.eq_bass_saved = eq_bass;

  int8_t eq_mid = 0;
  if (radio_settings_load_eq_mid(&eq_mid) != ESP_OK || eq_mid < RADIO_AUDIO_EQ_MIN_DB ||
      eq_mid > RADIO_AUDIO_EQ_MAX_DB)
    eq_mid = 0;
  s.eq_mid = s.eq_mid_saved = eq_mid;

  int8_t eq_treble = 0;
  if (radio_settings_load_eq_treble(&eq_treble) != ESP_OK || eq_treble < RADIO_AUDIO_EQ_MIN_DB ||
      eq_treble > RADIO_AUDIO_EQ_MAX_DB)
    eq_treble = 0;
  s.eq_treble = s.eq_treble_saved = eq_treble;

  //-- A malformed/missing stations.json must never crash the device: show an
  //-- ERROR screen with the reason (e.g. JSON line/char) and boot on with an
  //-- empty station list instead. The web GUI's stations import/Manage
  //-- Stations can still be used to fix the file over the network. The
  //-- screen stays up until the user presses a button or turns the encoder.
  char station_err[96];
  esp_err_t station_err_code = station_store_load(station_err, sizeof(station_err));
  if (station_err_code != ESP_OK)
  {
    ESP_LOGE("app_main", "stations.json: %s", station_err);
    char station_err_screen[160];
    snprintf(station_err_screen, sizeof(station_err_screen), "%s\nLoad stations.json via the GUI",
             station_err);
    radio_display_error(station_err_screen);
    radio_input_event_t e;
    xQueueReceive(s_events, &e, portMAX_DELAY);
    //-- Drain any extra events (e.g. a burst of rotation ticks) so they
    //-- don't leak into the Volume screen right after boot.
    while (xQueueReceive(s_events, &e, 0) == pdTRUE)
    {
    }
  }

  size_t saved_station = 0;
  radio_settings_load(&saved_station);
  size_t count = station_store_count();
  if (count && saved_station < count)
  {
    s.playing = s.selected = saved_station;
  }
  radio_display_status("");
  ESP_ERROR_CHECK(radio_audio_init());
  radio_audio_set_volume(s.volume);
  radio_audio_set_attenuation(s.attenuation);
  //-- Applied as soon as the audio chain (I2S/decoder) is ready, so the
  //-- loaded EQ settings are active from the very first played frame.
  radio_audio_set_eq_bass(s.eq_bass);
  radio_audio_set_eq_mid(s.eq_mid);
  radio_audio_set_eq_treble(s.eq_treble);
  radio_audio_set_title_callback(title_cb, NULL);
  radio_audio_set_mute_callback(on_audio_mute_changed, NULL);
  radio_audio_set_stall_callback(stall_cb, NULL);
  web_gui_set_state_applied_cb(on_web_gui_state_applied, NULL);
  build_device_name_effective(s_ap_ssid, sizeof(s_ap_ssid), ':', s.hostname_num);
  build_device_name_effective(s_mdns_hostname, sizeof(s_mdns_hostname), '-', s.hostname_num);
  wifi_prov_config_t wc = WIFI_PROV_DEFAULT_CONFIG();
  wc.ap_ssid = s_ap_ssid;
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
    //-- Started only after IP_EVENT_STA_GOT_IP has fired (guaranteed here,
    //-- since wifi_prov_wait_for_connection() only returns ESP_OK once that
    //-- event has been handled), so mDNS advertisement can succeed.
    ota_upload_config_t ota_cfg = OTA_UPLOAD_CONFIG_DEFAULT();
    ota_cfg.hostname = s_mdns_hostname;
    ota_cfg.prepare_cb = prepare_ota_cb;
    ota_cfg.prepare_ctx = NULL;
    ESP_ERROR_CHECK(ota_upload_start(&ota_cfg));
  }
  xTaskCreate(ui_task, "radio_ui", 4096, NULL, 6, NULL);
  xTaskCreate(buffer_monitor_task, "buf_mon", 2048, NULL, 5, NULL);
}

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

//-- How long the "Connected: SSID / IP" screen stays up before switching to
//-- the Volume/PLAY screen, so the user can actually read it.
#define WIFI_CONNECTED_SPLASH_MS 2500
typedef enum { UI_VOLUME, UI_STATION_SELECT } ui_mode_t;
typedef struct {ui_mode_t mode;int volume;size_t playing;size_t selected;TickType_t last_rotation;} app_state_t;
static QueueHandle_t s_events; static app_state_t s={.mode=UI_VOLUME,.volume=CONFIG_RADIO_DEFAULT_VOLUME};
static void input_cb(radio_input_event_t e,void*ctx){(void)ctx;xQueueSend(s_events,&e,0);}

//-- ICY "StreamTitle" is conventionally "Artist - Track"; split it once here so
//-- neither the display nor the web GUI has to re-decide the "-" fallback rule.
static void split_icy_title(const char *raw, char *artist, size_t artist_sz, char *track, size_t track_sz)
{
	const char *sep = raw ? strstr(raw, " - ") : NULL;
	if (sep) {
		size_t artist_len = (size_t)(sep - raw);
		if (artist_len >= artist_sz) artist_len = artist_sz - 1;
		memcpy(artist, raw, artist_len);
		artist[artist_len] = '\0';
		snprintf(track, track_sz, "%s", sep + 3);
	} else {
		artist[0] = '\0';
		snprintf(track, track_sz, "%s", raw ? raw : "");
	}
	if (!artist[0]) snprintf(artist, artist_sz, "-");
	if (!track[0]) snprintf(track, track_sz, "-");
}

//-- Last real (non-empty) title split, kept up to date even while muted so the
//-- very first StreamTitle of a station isn't lost: it can arrive from
//-- fetch_task() before radio_audio finishes its decode warm-up and unmutes.
static char s_last_artist[160] = "-";
static char s_last_track[160] = "-";

static void title_cb(const char *title, void *ctx)
{
	(void)ctx;
	//-- fetch_task() fires a dummy title_cb("", ctx) as soon as it starts;
	//-- ignore only that empty placeholder, not real titles.
	if (!title || !title[0]) return;
	char artist[160], track[160];
	split_icy_title(title, artist, sizeof(artist), track, sizeof(track));
	snprintf(s_last_artist, sizeof(s_last_artist), "%s", artist);
	snprintf(s_last_track, sizeof(s_last_track), "%s", track);
	//-- Stay silent while muted so "Switch Station .." (see
	//-- on_audio_mute_changed) stays on screen for the whole switch; the
	//-- cached title above is flushed the moment we unmute instead.
	if (radio_audio_is_muted()) return;
	radio_display_now_playing(artist, track);
	web_gui_notify_title(artist, track);
}

//-- Fired by radio_audio the instant a station switch starts (still muted)
//-- and again once the new stream has actually started playing (unmuted),
//-- so both the physical display and the web GUI show a clear "switching"
//-- state instead of stale or blank artist/track text during the mute window.
static void on_audio_mute_changed(bool muted, void *ctx)
{
	(void)ctx;
	if (muted) {
		snprintf(s_last_artist, sizeof(s_last_artist), "-");
		snprintf(s_last_track, sizeof(s_last_track), "-");
		radio_display_now_playing("Switch Station ..", "-");
		web_gui_notify_title("Switch Station ..", "-");
	} else {
		//-- Flush whatever real title already arrived during the mute
		//-- window (the first ICY metadata block often lands here) instead
		//-- of forcing a stale "-"/"-" that only a *second* title update
		//-- would have overwritten.
		radio_display_now_playing(s_last_artist, s_last_track);
		web_gui_notify_title(s_last_artist, s_last_track);
	}
}

static void show_volume(void){const radio_station_t*st=station_store_get(s.playing);radio_display_volume(s.volume,st?st->name:"");}

//-- Fired by radio_audio when fetch_task ends without a deliberate stop/switch
//-- (connection open failed after all retries, or a mid-stream read/reconnect
//-- failure) so the failure is visible instead of a stale "Switch Station .."
//-- or last-known artist/track lingering on both the physical display and the
//-- web GUI forever.
static void stall_cb(bool stalled, void *ctx)
{
	(void)ctx;
	if (!stalled) return;
	radio_display_now_playing("Stream stalled", "-");
	web_gui_notify_title("Stream stalled", "-");
}

//-- Fired by web_gui after a browser-issued command actually changed the
//-- playing station or volume, so the physical UI's own tracking (and the
//-- TFT) don't go stale when the change came from the web GUI instead of
//-- the EC11.
static void on_web_gui_state_applied(size_t station_index, int volume, void *ctx)
{
	(void)ctx;
	s.playing = s.selected = station_index;
	s.volume = volume;
	if (s.mode == UI_VOLUME) show_volume();
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
	if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && wifi_prov_get_ip_info(&ip_info) == ESP_OK) {
		snprintf(msg, sizeof(msg), "SSID: %s\nIP: " IPSTR, (const char *)ap_info.ssid, IP2STR(&ip_info.ip));
	} else {
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
static void ui_task(void*arg){radio_input_event_t e;for(;;){if(xQueueReceive(s_events,&e,pdMS_TO_TICKS(100))){size_t count=station_store_count();if(e==RADIO_INPUT_AUX_PUSH){ESP_LOGI("radio","Reserved AUX button pressed");continue;}if(e==RADIO_INPUT_EN_PUSH){if(s.mode==UI_VOLUME){s.mode=UI_STATION_SELECT;s.selected=s.playing;s.last_rotation=xTaskGetTickCount();radio_display_station_list(s.selected);}else{ s.playing=s.selected;s.mode=UI_VOLUME;radio_audio_play(station_store_get(s.playing));radio_settings_save(s.playing);show_volume();web_gui_notify_device_state(s.playing);}continue;}if(e==RADIO_INPUT_ROTATE_LEFT||e==RADIO_INPUT_ROTATE_RIGHT){int d=e==RADIO_INPUT_ROTATE_RIGHT?1:-1;if(s.mode==UI_VOLUME){s.volume+=d*2;if(s.volume<0)s.volume=0;if(s.volume>100)s.volume=100;radio_audio_set_volume(s.volume);show_volume();web_gui_notify_device_state(s.playing);}else if(count){long next=(long)s.selected+d;if(next<0)next=0;if(next>=(long)count)next=(long)count-1;s.selected=(size_t)next;s.last_rotation=xTaskGetTickCount();radio_display_station_list(s.selected);}}}if(s.mode==UI_STATION_SELECT&&xTaskGetTickCount()-s.last_rotation>=pdMS_TO_TICKS(CONFIG_RADIO_SELECTION_TIMEOUT_MS)){s.mode=UI_VOLUME;show_volume();}}}
void app_main(void){ESP_ERROR_CHECK(radio_settings_init());ESP_ERROR_CHECK(radio_storage_mount());ESP_ERROR_CHECK(station_store_load());size_t saved_station=0;radio_settings_load(&saved_station);size_t count=station_store_count();if(count&&saved_station<count){s.playing=s.selected=saved_station;}ESP_ERROR_CHECK(radio_display_init());radio_display_status("");ESP_ERROR_CHECK(radio_audio_init());radio_audio_set_volume(s.volume);radio_audio_set_title_callback(title_cb,NULL);radio_audio_set_mute_callback(on_audio_mute_changed,NULL);radio_audio_set_stall_callback(stall_cb,NULL);web_gui_set_state_applied_cb(on_web_gui_state_applied,NULL);s_events=xQueueCreate(12,sizeof(radio_input_event_t));ESP_ERROR_CHECK(radio_input_start(input_cb,NULL));wifi_prov_config_t wc=WIFI_PROV_DEFAULT_CONFIG();wc.on_connected=on_wifi_connected;wc.on_portal_start=on_wifi_portal_start;radio_display_status("Connecting to WiFi");ESP_ERROR_CHECK(wifi_prov_start(&wc));if(wifi_prov_wait_for_connection(portMAX_DELAY)==ESP_OK){vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECTED_SPLASH_MS));radio_audio_play(station_store_get(s.playing));show_volume();ESP_ERROR_CHECK(web_gui_init());web_gui_notify_device_state(s.playing);}xTaskCreate(ui_task,"radio_ui",4096,NULL,6,NULL);}

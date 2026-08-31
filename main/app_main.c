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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
typedef enum { UI_VOLUME, UI_STATION_SELECT } ui_mode_t;
typedef struct {ui_mode_t mode;int volume;size_t playing;size_t selected;TickType_t last_rotation;} app_state_t;
static QueueHandle_t s_events; static app_state_t s={.mode=UI_VOLUME,.volume=CONFIG_RADIO_DEFAULT_VOLUME};
static void input_cb(radio_input_event_t e,void*ctx){(void)ctx;xQueueSend(s_events,&e,0);}
static void title_cb(const char*title,void*ctx){(void)ctx;radio_display_now_playing(title);web_gui_notify_title(title);}
static void show_volume(void){const radio_station_t*st=station_store_get(s.playing);radio_display_volume(s.volume,st?st->name:"");}
static void ui_task(void*arg){radio_input_event_t e;for(;;){if(xQueueReceive(s_events,&e,pdMS_TO_TICKS(100))){size_t count=station_store_count();if(e==RADIO_INPUT_AUX_PUSH){ESP_LOGI("radio","Reserved AUX button pressed");continue;}if(e==RADIO_INPUT_EN_PUSH){if(s.mode==UI_VOLUME){s.mode=UI_STATION_SELECT;s.selected=s.playing;s.last_rotation=xTaskGetTickCount();radio_display_station_list(s.selected);}else{ s.playing=s.selected;s.mode=UI_VOLUME;radio_audio_play(station_store_get(s.playing));radio_settings_save(s.playing);show_volume();}continue;}if(e==RADIO_INPUT_ROTATE_LEFT||e==RADIO_INPUT_ROTATE_RIGHT){int d=e==RADIO_INPUT_ROTATE_RIGHT?1:-1;if(s.mode==UI_VOLUME){s.volume+=d*2;if(s.volume<0)s.volume=0;if(s.volume>100)s.volume=100;radio_audio_set_volume(s.volume);show_volume();}else if(count){long next=(long)s.selected+d;if(next<0)next=0;if(next>=(long)count)next=(long)count-1;s.selected=(size_t)next;s.last_rotation=xTaskGetTickCount();radio_display_station_list(s.selected);}}}if(s.mode==UI_STATION_SELECT&&xTaskGetTickCount()-s.last_rotation>=pdMS_TO_TICKS(CONFIG_RADIO_SELECTION_TIMEOUT_MS)){s.mode=UI_VOLUME;show_volume();}}}
void app_main(void){ESP_ERROR_CHECK(radio_settings_init());ESP_ERROR_CHECK(radio_storage_mount());ESP_ERROR_CHECK(station_store_load());size_t saved_station=0;radio_settings_load(&saved_station);size_t count=station_store_count();if(count&&saved_station<count){s.playing=s.selected=saved_station;}ESP_ERROR_CHECK(radio_display_init());ESP_ERROR_CHECK(radio_audio_init());radio_audio_set_volume(s.volume);radio_audio_set_title_callback(title_cb,NULL);s_events=xQueueCreate(12,sizeof(radio_input_event_t));ESP_ERROR_CHECK(radio_input_start(input_cb,NULL));show_volume();wifi_prov_config_t wc=WIFI_PROV_DEFAULT_CONFIG();ESP_ERROR_CHECK(wifi_prov_start(&wc));radio_display_status("Connect WiFi or provisioning AP");if(wifi_prov_wait_for_connection(portMAX_DELAY)==ESP_OK){radio_audio_play(station_store_get(s.playing));show_volume();ESP_ERROR_CHECK(web_gui_init());}xTaskCreate(ui_task,"radio_ui",4096,NULL,6,NULL);}

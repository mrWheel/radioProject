#pragma once
#include "esp_err.h"
#include "station_store.h"
typedef void (*radio_audio_title_cb_t)(const char *title, void *ctx);
esp_err_t radio_audio_init(void);
esp_err_t radio_audio_play(const radio_station_t *station);
void radio_audio_set_volume(int percent);
int radio_audio_get_volume(void);
void radio_audio_set_paused(bool paused);
bool radio_audio_is_paused(void);
void radio_audio_set_title_callback(radio_audio_title_cb_t cb, void *ctx);


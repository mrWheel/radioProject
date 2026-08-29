#pragma once
#include "esp_err.h"
#include "station_store.h"
typedef void (*radio_audio_title_cb_t)(const char *title, void *ctx);
esp_err_t radio_audio_init(void);
esp_err_t radio_audio_play(const radio_station_t *station);
void radio_audio_set_volume(int percent);
void radio_audio_set_title_callback(radio_audio_title_cb_t cb, void *ctx);


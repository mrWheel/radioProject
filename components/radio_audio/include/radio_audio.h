#pragma once
#include "esp_err.h"
#include "station_store.h"
typedef void (*radio_audio_title_cb_t)(const char* title, void* ctx);
typedef void (*radio_audio_mute_cb_t)(bool muted, void* ctx);
typedef void (*radio_audio_stall_cb_t)(bool stalled, void* ctx);
esp_err_t radio_audio_init(void);
esp_err_t radio_audio_play(const radio_station_t* station);
void radio_audio_set_volume(int percent);
int radio_audio_get_volume(void);
void radio_audio_set_paused(bool paused);
bool radio_audio_is_paused(void);
bool radio_audio_is_muted(void);
void radio_audio_set_title_callback(radio_audio_title_cb_t cb, void* ctx);
void radio_audio_set_mute_callback(radio_audio_mute_cb_t cb, void* ctx);
void radio_audio_set_stall_callback(radio_audio_stall_cb_t cb, void* ctx);
//-- Live fill level of the network->decoder ring buffer, 0-100%
int radio_audio_get_buffer_fill_percent(void);

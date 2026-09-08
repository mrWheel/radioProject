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
//-- Output attenuation percent (1-100, default 50) applied on top of the
//-- 0-100 volume percent; see the Settings menu's "Attenuating" item.
void radio_audio_set_attenuation(int percent);
int radio_audio_get_attenuation(void);
void radio_audio_set_paused(bool paused);
bool radio_audio_is_paused(void);
bool radio_audio_is_muted(void);
void radio_audio_set_title_callback(radio_audio_title_cb_t cb, void* ctx);
void radio_audio_set_mute_callback(radio_audio_mute_cb_t cb, void* ctx);
void radio_audio_set_stall_callback(radio_audio_stall_cb_t cb, void* ctx);
//-- Live fill level of the network->decoder ring buffer, 0-100%
int radio_audio_get_buffer_fill_percent(void);

//-- 3-band software equalizer (bass/mid/treble), applied to decoded PCM
//-- before software volume and before I2S. Range -12..+12 dB, 0 = no
//-- correction. Coefficients update live and are thread-safe to call from
//-- the UI task while audio is playing; see the Equalizer screen.
#define RADIO_AUDIO_EQ_MIN_DB (-12)
#define RADIO_AUDIO_EQ_MAX_DB (12)
void radio_audio_set_eq_bass(int db);
void radio_audio_set_eq_mid(int db);
void radio_audio_set_eq_treble(int db);
int radio_audio_get_eq_bass(void);
int radio_audio_get_eq_mid(void);
int radio_audio_get_eq_treble(void);

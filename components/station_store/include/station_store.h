#pragma once
#include <stddef.h>
#include "esp_err.h"
#define RADIO_MAX_STATIONS 32
#define RADIO_NAME_MAX 64
#define RADIO_URL_MAX 256
typedef enum { RADIO_CODEC_MP3, RADIO_CODEC_AAC } radio_codec_t;
typedef struct { char name[RADIO_NAME_MAX]; char url[RADIO_URL_MAX]; radio_codec_t codec; } radio_station_t;
esp_err_t station_store_load(void);
size_t station_store_count(void);
const radio_station_t *station_store_get(size_t index);


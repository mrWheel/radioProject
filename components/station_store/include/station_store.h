#pragma once
#include <stddef.h>
#include "esp_err.h"
#define RADIO_MAX_STATIONS 32
#define RADIO_NAME_MAX 64
#define RADIO_URL_MAX 256
typedef enum { RADIO_CODEC_MP3, RADIO_CODEC_AAC } radio_codec_t;
typedef struct { char name[RADIO_NAME_MAX]; char url[RADIO_URL_MAX]; radio_codec_t codec; } radio_station_t;
esp_err_t station_store_load(void);
esp_err_t station_store_save(void);
//-- Replaces the in-memory station list and the on-disk stations.json from an
//-- uploaded JSON document (null-terminated). Validated before anything is
//-- overwritten: if the document is malformed or contains zero valid stations,
//-- both the file on disk and the current in-memory list are left untouched.
esp_err_t station_store_import(const char *json_text);
size_t station_store_count(void);
const radio_station_t *station_store_get(size_t index);
bool station_store_valid(const radio_station_t *station);
bool station_store_add(const radio_station_t *station);
bool station_store_edit(size_t index, const radio_station_t *station);
bool station_store_delete(size_t index);


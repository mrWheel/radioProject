#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t radio_display_init(void);
void radio_display_volume(int volume,const char *station);
void radio_display_now_playing(const char *artist, const char *track);
void radio_display_station_list(size_t selected);
void radio_display_status(const char *status);


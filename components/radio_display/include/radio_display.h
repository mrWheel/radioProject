#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t radio_display_init(void);
void radio_display_volume(int volume, const char* station);
void radio_display_now_playing(const char* line1, const char* line2, const char* line3);
void radio_display_station_list(size_t selected);
void radio_display_status(const char* status);
//-- Red-header ERROR screen (never used for normal status text), so a
//-- malformed stations.json is clearly visible instead of silently leaving
//-- an empty station list. Wraps/word-wraps like radio_display_status().
void radio_display_error(const char* message);
void radio_display_technical(const char* ssid, const char* ip, const char* mac,
                             const char* hostname, size_t station_count);
//-- Live stream-buffer fill indicator, drawn as a short bar at the bottom-right
//-- of the Volume screen (0-100). No-op on any other screen.
void radio_display_buffer_fill(int percent);

//-- Character budget of a single now-playing line at the scale
//-- draw_now_playing_line() actually draws with, so callers that pre-wrap
//-- text (main/app_main.c) don't have to duplicate the width formula.
size_t radio_display_title_max_chars(void);

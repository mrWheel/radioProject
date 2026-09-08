#pragma once
#include <stddef.h>
#include <stdint.h>
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

//-- [Settings] menu: Hostname#, Attenuating (dB), Backlight off time,
//-- Encoder Direction, PCM5102A BCLK/LRCLK/DATA/DAC-enable GPIO overrides,
//-- Reset Radio, Exit. `editing` highlights the currently-selected value
//-- (red-on-white) while the user is adjusting it; a second EN-push commits
//-- and clears it. "Encoder Direction" instead toggles instantly on
//-- EN-push (`encoder_reversed`: false = "A->B", true = "B->A") without
//-- ever entering edit mode. Rows scroll (windowed) if they don't all fit
//-- the screen at once.
void radio_display_settings(size_t selected, bool editing, uint16_t hostname_num,
                            int8_t attenuation, uint8_t backlight_minutes, bool encoder_reversed,
                            int16_t i2s_bclk, int16_t i2s_ws, int16_t i2s_dout,
                            int16_t i2s_enable);

//-- Equalizer screen: Bass/Mid/Treble, each -12..+12 dB. Rotating scrolls
//-- the highlighted row (`selected`) when not editing; a short EC11 press
//-- enters edit mode (`editing`, highlighted red-on-white) where rotating
//-- adjusts that row's value instead, and a second short press locks it in.
void radio_display_equalizer(size_t selected, bool editing, int8_t bass_db, int8_t mid_db,
                             int8_t treble_db);

//-- Character budget of a single now-playing line at the scale
//-- draw_now_playing_line() actually draws with, so callers that pre-wrap
//-- text (main/app_main.c) don't have to duplicate the width formula.
size_t radio_display_title_max_chars(void);

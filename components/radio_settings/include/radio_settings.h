#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

//-- Persists the last-used station index across reboots (NVS). Volume is
//-- intentionally not persisted: it always resets to CONFIG_RADIO_DEFAULT_VOLUME.
esp_err_t radio_settings_init(void);
esp_err_t radio_settings_load(size_t *station);
esp_err_t radio_settings_save(size_t station);

//-- [Settings] menu values, all persisted in the same NVS namespace as the
//-- station index. A load call returning anything other than ESP_OK means
//-- the value was never saved (fresh device) - callers apply their own
//-- documented default in that case.

//-- Hostname/AP-name numeric override: 0 = derive from the MAC address
//-- ("Radio-xx-yy-zz", the pre-existing behavior), 1-256 = "Radio-<n>".
esp_err_t radio_settings_load_hostname_num(uint16_t *value);
esp_err_t radio_settings_save_hostname_num(uint16_t value);

//-- Output attenuation in dB applied on top of the 0-100 volume (-24..0).
//-- Default when not present in NVS: -6.
esp_err_t radio_settings_load_attenuation(int8_t *value);
esp_err_t radio_settings_save_attenuation(int8_t value);

//-- Backlight auto-off timeout in minutes (0-60, 0 = never dim). Default
//-- when not present in NVS: 5.
esp_err_t radio_settings_load_backlight_minutes(uint8_t *value);
esp_err_t radio_settings_save_backlight_minutes(uint8_t value);

//-- Encoder rotation direction override: false = A->B (Kconfig default),
//-- true = B->A (reversed). Toggled directly by an EN-push on the Settings
//-- menu's "Encoder Direction" item (no separate edit mode). Default when
//-- not present in NVS: CONFIG_RADIO_ENCODER_REVERSED.
esp_err_t radio_settings_load_encoder_reversed(uint8_t *value);
esp_err_t radio_settings_save_encoder_reversed(uint8_t value);

//-- 3-band equalizer gains in dB (-12..+12). Default when not present in
//-- NVS, or when the stored value is out of range: 0 (no correction).
esp_err_t radio_settings_load_eq_bass(int8_t *value);
esp_err_t radio_settings_save_eq_bass(int8_t value);
esp_err_t radio_settings_load_eq_mid(int8_t *value);
esp_err_t radio_settings_save_eq_mid(int8_t value);
esp_err_t radio_settings_load_eq_treble(int8_t *value);
esp_err_t radio_settings_save_eq_treble(int8_t value);

//-- PCM5102A I2S GPIO overrides (-1..48, DAC enable also allows -1 to
//-- disable). Default when not present in NVS: the Kconfig-configured
//-- "Radio hardware" default (CONFIG_RADIO_I2S_*_GPIO). Only applied at
//-- boot (before radio_audio_init()), same as Hostname#.
esp_err_t radio_settings_load_i2s_bclk(int16_t *value);
esp_err_t radio_settings_save_i2s_bclk(int16_t value);
esp_err_t radio_settings_load_i2s_ws(int16_t *value);
esp_err_t radio_settings_save_i2s_ws(int16_t value);
esp_err_t radio_settings_load_i2s_dout(int16_t *value);
esp_err_t radio_settings_save_i2s_dout(int16_t value);
esp_err_t radio_settings_load_i2s_enable(int16_t *value);
esp_err_t radio_settings_save_i2s_enable(int16_t value);

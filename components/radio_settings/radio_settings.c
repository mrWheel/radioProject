#include "radio_settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define NVS_NAMESPACE   "radio"
#define NVS_KEY_STATION "station"
#define NVS_KEY_HOSTNAME_NUM  "host_num"
#define NVS_KEY_ATTENUATION   "atten_db"
#define NVS_KEY_BACKLIGHT_MIN "bl_min"
#define NVS_KEY_ENCODER_REV   "enc_rev"
#define NVS_KEY_EQ_BASS   "eq_bass"
#define NVS_KEY_EQ_MID    "eq_mid"
#define NVS_KEY_EQ_TREBLE "eq_treble"
#define NVS_KEY_I2S_BCLK   "i2s_bclk"
#define NVS_KEY_I2S_WS     "i2s_ws"
#define NVS_KEY_I2S_DOUT   "i2s_dout"
#define NVS_KEY_I2S_ENABLE "i2s_en"

static const char *TAG = "radio_settings";

esp_err_t radio_settings_init(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	return err;
}

esp_err_t radio_settings_load(size_t *station)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) {
		ESP_LOGD(TAG, "No stored settings (nvs_open: %s)", esp_err_to_name(err));
		return err;
	}

	uint32_t stored_station;
	err = nvs_get_u32(handle, NVS_KEY_STATION, &stored_station);
	if (err == ESP_OK) *station = (size_t)stored_station;

	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save(size_t station)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}

	err = nvs_set_u32(handle, NVS_KEY_STATION, (uint32_t)station);
	if (err == ESP_OK) err = nvs_commit(handle);

	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_hostname_num(uint16_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_u16(handle, NVS_KEY_HOSTNAME_NUM, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_hostname_num(uint16_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_u16(handle, NVS_KEY_HOSTNAME_NUM, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_attenuation(int8_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i8(handle, NVS_KEY_ATTENUATION, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_attenuation(int8_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i8(handle, NVS_KEY_ATTENUATION, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_backlight_minutes(uint8_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_u8(handle, NVS_KEY_BACKLIGHT_MIN, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_backlight_minutes(uint8_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_u8(handle, NVS_KEY_BACKLIGHT_MIN, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_encoder_reversed(uint8_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_u8(handle, NVS_KEY_ENCODER_REV, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_encoder_reversed(uint8_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_u8(handle, NVS_KEY_ENCODER_REV, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_eq_bass(int8_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i8(handle, NVS_KEY_EQ_BASS, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_eq_bass(int8_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i8(handle, NVS_KEY_EQ_BASS, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_eq_mid(int8_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i8(handle, NVS_KEY_EQ_MID, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_eq_mid(int8_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i8(handle, NVS_KEY_EQ_MID, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_eq_treble(int8_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i8(handle, NVS_KEY_EQ_TREBLE, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_eq_treble(int8_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i8(handle, NVS_KEY_EQ_TREBLE, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_i2s_bclk(int16_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i16(handle, NVS_KEY_I2S_BCLK, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_i2s_bclk(int16_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i16(handle, NVS_KEY_I2S_BCLK, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_i2s_ws(int16_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i16(handle, NVS_KEY_I2S_WS, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_i2s_ws(int16_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i16(handle, NVS_KEY_I2S_WS, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_i2s_dout(int16_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i16(handle, NVS_KEY_I2S_DOUT, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_i2s_dout(int16_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i16(handle, NVS_KEY_I2S_DOUT, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_load_i2s_enable(int16_t *value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (err != ESP_OK) return err;
	err = nvs_get_i16(handle, NVS_KEY_I2S_ENABLE, value);
	nvs_close(handle);
	return err;
}

esp_err_t radio_settings_save_i2s_enable(int16_t value)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return err;
	}
	err = nvs_set_i16(handle, NVS_KEY_I2S_ENABLE, value);
	if (err == ESP_OK) err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

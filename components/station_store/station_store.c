#include "station_store.h"
#include "radio_storage.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

static radio_station_t s_stations[RADIO_MAX_STATIONS];
static size_t s_count;

static void station_store_trim(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\n' || dst[len - 1] == '\r' || dst[len - 1] == '\t')) {
        dst[--len] = '\0';
    }
}

bool station_store_valid(const radio_station_t *station)
{
    if (!station) return false;
    if (station->name[0] == '\0' || station->url[0] == '\0') return false;
    if (strlen(station->name) >= sizeof(station->name)) return false;
    if (strlen(station->url) >= sizeof(station->url)) return false;
    if (strncasecmp(station->url, "http://", 7) != 0 && strncasecmp(station->url, "https://", 8) != 0) return false;
    return true;
}

esp_err_t station_store_load(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/stations.json", RADIO_STORAGE_PATH);
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 32768) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_ERR_INVALID_ARG;

    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "stations");
    s_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "url");
        cJSON *codec = cJSON_GetObjectItemCaseSensitive(item, "codec");
        if (s_count >= RADIO_MAX_STATIONS || !cJSON_IsString(name) || !cJSON_IsString(url)) continue;

        station_store_trim(s_stations[s_count].name, sizeof(s_stations[s_count].name), name->valuestring);
        station_store_trim(s_stations[s_count].url, sizeof(s_stations[s_count].url), url->valuestring);
        s_stations[s_count].codec = (cJSON_IsString(codec) && !strcasecmp(codec->valuestring, "aac")) ? RADIO_CODEC_AAC : RADIO_CODEC_MP3;
        if (station_store_valid(&s_stations[s_count])) {
            s_count++;
        }
    }

    cJSON_Delete(root);
    return s_count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t station_store_save(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/stations.json", RADIO_STORAGE_PATH);
    char tmp_path[96];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(array);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "name", s_stations[i].name);
        cJSON_AddStringToObject(item, "url", s_stations[i].url);
        cJSON_AddStringToObject(item, "codec", s_stations[i].codec == RADIO_CODEC_AAC ? "aac" : "mp3");
        cJSON_AddItemToArray(array, item);
    }
    cJSON_AddItemToObject(root, "stations", array);

    char *buf = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = strlen(buf);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }
    size_t written = fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    if (written != len) return ESP_FAIL;

    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

size_t station_store_count(void)
{
    return s_count;
}

const radio_station_t *station_store_get(size_t i)
{
    return i < s_count ? &s_stations[i] : NULL;
}

bool station_store_add(const radio_station_t *station)
{
    if (!station || !station_store_valid(station) || s_count >= RADIO_MAX_STATIONS) return false;
    s_stations[s_count++] = *station;
    return true;
}

bool station_store_edit(size_t index, const radio_station_t *station)
{
    if (!station || !station_store_valid(station) || index >= s_count) return false;
    s_stations[index] = *station;
    return true;
}

bool station_store_delete(size_t index)
{
    if (index >= s_count) return false;
    for (size_t i = index + 1; i < s_count; ++i) {
        s_stations[i - 1] = s_stations[i];
    }
    s_count--;
    return true;
}


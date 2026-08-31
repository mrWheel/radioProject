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

//-- Walks a parsed {"stations":[...]} document into `out`, skipping entries
//-- that are malformed or fail station_store_valid(). Shared by
//-- station_store_load() (reads stations.json at boot) and
//-- station_store_import() (validates an uploaded document before it is
//-- allowed to touch disk or the live in-memory list).
static size_t station_store_parse_json(const cJSON *root, radio_station_t *out, size_t max_out)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "stations");
    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "url");
        cJSON *codec = cJSON_GetObjectItemCaseSensitive(item, "codec");
        if (count >= max_out || !cJSON_IsString(name) || !cJSON_IsString(url)) continue;

        station_store_trim(out[count].name, sizeof(out[count].name), name->valuestring);
        station_store_trim(out[count].url, sizeof(out[count].url), url->valuestring);
        out[count].codec = (cJSON_IsString(codec) && !strcasecmp(codec->valuestring, "aac")) ? RADIO_CODEC_AAC : RADIO_CODEC_MP3;
        if (station_store_valid(&out[count])) {
            count++;
        }
    }
    return count;
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

    s_count = station_store_parse_json(root, s_stations, RADIO_MAX_STATIONS);
    cJSON_Delete(root);
    return s_count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t station_store_import(const char *json_text)
{
    if (!json_text || json_text[0] == '\0') return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_text);
    if (!root) return ESP_ERR_INVALID_ARG;

    //-- Heap-allocated, not a stack array: this function runs on the httpd
    //-- worker task, whose stack (4KB, HTTPD_DEFAULT_CONFIG's stack_size) is
    //-- far too small for a ~10KB radio_station_t[RADIO_MAX_STATIONS] frame.
    radio_station_t *scratch = malloc(RADIO_MAX_STATIONS * sizeof(radio_station_t));
    if (!scratch) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    size_t scratch_count = station_store_parse_json(root, scratch, RADIO_MAX_STATIONS);
    cJSON_Delete(root);
    if (scratch_count == 0) {
        free(scratch);
        return ESP_ERR_NOT_FOUND;
    }

    //-- Only persist/apply once the uploaded document is known to contain at
    //-- least one valid station, so a malformed upload can never wipe out the
    //-- working file or the currently playing list.
    char path[64];
    snprintf(path, sizeof(path), "%s/stations.json", RADIO_STORAGE_PATH);
    char tmp_path[96];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        free(scratch);
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strlen(json_text);
    size_t written = fwrite(json_text, 1, len, f);
    fclose(f);
    if (written != len) {
        remove(tmp_path);
        free(scratch);
        return ESP_FAIL;
    }
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        free(scratch);
        return ESP_FAIL;
    }

    memcpy(s_stations, scratch, scratch_count * sizeof(scratch[0]));
    s_count = scratch_count;
    free(scratch);
    return ESP_OK;
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


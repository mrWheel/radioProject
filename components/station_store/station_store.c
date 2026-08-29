#include "station_store.h"
#include "radio_storage.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static radio_station_t s_stations[RADIO_MAX_STATIONS];
static size_t s_count;
esp_err_t station_store_load(void) {
    char path[64]; snprintf(path,sizeof(path),"%s/stations.json",RADIO_STORAGE_PATH);
    FILE *f=fopen(path,"rb"); if(!f) return ESP_ERR_NOT_FOUND;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    if(n<=0 || n>32768){fclose(f);return ESP_ERR_INVALID_SIZE;}
    char *buf=malloc((size_t)n+1); if(!buf){fclose(f);return ESP_ERR_NO_MEM;}
    if(fread(buf,1,(size_t)n,f)!=(size_t)n){free(buf);fclose(f);return ESP_FAIL;} fclose(f); buf[n]=0;
    cJSON *root=cJSON_Parse(buf); free(buf); if(!root)return ESP_ERR_INVALID_ARG;
    cJSON *array=cJSON_GetObjectItemCaseSensitive(root,"stations"); s_count=0;
    cJSON *item=NULL; cJSON_ArrayForEach(item,array) {
        cJSON *name=cJSON_GetObjectItemCaseSensitive(item,"name"); cJSON *url=cJSON_GetObjectItemCaseSensitive(item,"url"); cJSON *codec=cJSON_GetObjectItemCaseSensitive(item,"codec");
        if(s_count>=RADIO_MAX_STATIONS || !cJSON_IsString(name) || !cJSON_IsString(url)) continue;
        strlcpy(s_stations[s_count].name,name->valuestring,sizeof(s_stations[s_count].name));
        strlcpy(s_stations[s_count].url,url->valuestring,sizeof(s_stations[s_count].url));
        s_stations[s_count].codec=(cJSON_IsString(codec)&&!strcasecmp(codec->valuestring,"aac"))?RADIO_CODEC_AAC:RADIO_CODEC_MP3; s_count++;
    }
    cJSON_Delete(root); return s_count?ESP_OK:ESP_ERR_NOT_FOUND;
}
size_t station_store_count(void){return s_count;}
const radio_station_t *station_store_get(size_t i){return i<s_count?&s_stations[i]:NULL;}


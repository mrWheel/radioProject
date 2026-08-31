idf_component_register(
    SRCS "src/web_gui.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi cjson station_store radio_audio radio_settings radio_storage
)

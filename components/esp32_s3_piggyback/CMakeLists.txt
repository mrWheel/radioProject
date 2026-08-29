idf_component_register(SRCS "esp32_s3_piggyback.c" "esp32_s3_piggyback_font.c"
  INCLUDE_DIRS "include" PRIV_REQUIRES esp_lcd driver esp_timer)

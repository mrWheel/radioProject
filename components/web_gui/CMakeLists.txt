idf_component_register(
    SRCS "src/web_gui.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi cjson station_store radio_audio
)

set(WEB_GUI_HTML "${CMAKE_CURRENT_SOURCE_DIR}/assets/index.html")
set(WEB_GUI_CSS "${CMAKE_CURRENT_SOURCE_DIR}/assets/style.css")
set(WEB_GUI_JS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.js")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WEB_GUI_HTML}" "${WEB_GUI_CSS}" "${WEB_GUI_JS}"
)

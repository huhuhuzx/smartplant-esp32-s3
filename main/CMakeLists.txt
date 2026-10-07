idf_component_register(SRCS "main.c" "board.c" "plant.c" "store.c" "net.c" "web.c" "screen.c" "ui_font_20.c" "ui_font_28.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp32_s3_lcd_ev_board lvgl esp_http_server esp_wifi esp_netif esp_timer nvs_flash mdns spiffs
    EMBED_FILES "../web/index.html" "../web/styles.css" "../web/app.js")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

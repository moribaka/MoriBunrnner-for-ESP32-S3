idf_component_register(SRCS  "wifi_manager.c" "file_system.c" "ws_server.c" "ws_server_http_content.c" "ws_server_http_device.c" "ws_server_http_maintenance.c" "ws_server_http_burn.c" "ws_server_httpd.c" "mcu_debug.c" "lcd_display.c" "lvgl_port.c" "ui.c" "usb_msc_tf.c" "mori_i2c.c" "ip5306.c" "tca9555.c" "main.c"
                       PRIV_REQUIRES driver esp_driver_i2c fatfs sdmmc spi_flash esp_wifi esp_http_server nvs_flash esp_lcd esp_timer esp_tinyusb app_update esp_psram "lvgl-9.4.0"
                       INCLUDE_DIRS "")

# Work around an xtensa GCC 14.2 ICE in esp-idf's esp_lcd/rgb/esp_lcd_panel_rgb.c
# when the project is built in Debug (-Og). Keep the rest of the project in
# Debug, but compile the esp_lcd component with -Os instead.
idf_component_get_property(esp_lcd_lib esp_lcd COMPONENT_LIB)
if(TARGET ${esp_lcd_lib})
    target_compile_options(${esp_lcd_lib} PRIVATE -Os)
endif()

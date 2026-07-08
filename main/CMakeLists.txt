idf_component_register(SRCS  "wifi_manager.c" "file_system.c" "smb_client.c" "app/app_registry.c" "app/app_config.c" "music/music_player.c" "boot_partition.c" "burner/db/burner_nor_db_data.c" "burner/db/burner_nor_db_lookup.c" "burner/core/ws_server.c" "burner/core/burner_status.c" "burner/core/burner_task.c" "burner/core/burner_backend.c" "burner/core/burner_spi_backend.c" "burner/http/ws_server_http_content.c" "burner/http/ws_server_http_device.c" "burner/http/ws_server_http_maintenance.c" "burner/http/ws_server_http_burn.c" "burner/http/ws_server_httpd.c" "mcu_debug.c" "lcd_display.c" "lvgl_port.c" "ui.c" "ui_text.c" "ui_text_en.c" "ui_text_zh.c" "usb_msc_tf.c" "mori_i2c.c" "ip5306.c" "axp209.c" "power_manager.c" "tca9555.c" "main.c" "reader/epub_native.cpp" "reader/third_party/tinyxml2/tinyxml2.cpp" "reader/third_party/miniz/miniz.c"
                       PRIV_REQUIRES driver esp_driver_i2c esp_driver_i2s fatfs sdmmc spi_flash esp_wifi esp_http_server nvs_flash esp_lcd esp_timer esp_tinyusb esp_psram esp_app_format app_update bootloader_support spiffs json esp_audio_codec libsmb2 "lvgl-9.4.0"
                       INCLUDE_DIRS "." "app" "music" "reader" "burner" "burner/core" "burner/http" "burner/db")

set(WEB_ASSETS_SOURCE_DIR "${PROJECT_DIR}/assets")
set(WEB_ASSETS_STAGE_DIR "${CMAKE_BINARY_DIR}/web_assets")
set(WEB_MAIN_HTML_FILE "${PROJECT_DIR}/.web/main.html")

add_custom_target(web_assets_stage
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${WEB_ASSETS_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WEB_ASSETS_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${WEB_ASSETS_SOURCE_DIR}" "${WEB_ASSETS_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WEB_MAIN_HTML_FILE}" "${WEB_ASSETS_STAGE_DIR}/main.html"
    DEPENDS "${WEB_MAIN_HTML_FILE}"
    VERBATIM)

spiffs_create_partition_image(assets "${WEB_ASSETS_STAGE_DIR}" FLASH_IN_PROJECT)
if(TARGET spiffs_assets_bin)
    add_dependencies(spiffs_assets_bin web_assets_stage)
endif()

# Work around an xtensa GCC 14.2 ICE in esp-idf's esp_lcd/rgb/esp_lcd_panel_rgb.c
# when the project is built in Debug (-Og). Keep the rest of the project in
# Debug, but compile the esp_lcd component with -Os instead.
idf_component_get_property(esp_lcd_lib esp_lcd COMPONENT_LIB)
if(TARGET ${esp_lcd_lib})
    target_compile_options(${esp_lcd_lib} PRIVATE -Os)
endif()

# Keep LVGL's built-in heap in PSRAM instead of consuming internal DRAM.
idf_component_get_property(lvgl_lib "lvgl-9.4.0" COMPONENT_LIB)
if(TARGET ${lvgl_lib})
    target_compile_definitions(
        ${lvgl_lib}
        PRIVATE "CONFIG_LV_ATTRIBUTE_LARGE_RAM_ARRAY=__attribute__((section(\".ext_ram.bss\")))")
endif()

# libsmb2 3.0.1 trips GCC 14's -Warray-parameter under ESP-IDF's -Werror=all.
# Keep the warning visible but do not fail the whole firmware build on it.
idf_component_get_property(libsmb2_lib sahlberg__libsmb2 COMPONENT_LIB)
if(TARGET ${libsmb2_lib})
    target_compile_options(${libsmb2_lib} PRIVATE -Wno-error=array-parameter -include esp_random.h)
endif()

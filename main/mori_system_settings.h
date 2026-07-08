#ifndef MORI_SYSTEM_SETTINGS_H
#define MORI_SYSTEM_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MORI_SYSTEM_LANGUAGE_ZH_INI "lang_zh_cn.ini"
#define MORI_SYSTEM_LANGUAGE_EN_INI "lang_en_us.ini"

uint16_t mori_screen_idle_off_minutes(void);
void mori_set_screen_idle_off_minutes(uint16_t minutes);
bool mori_wifi_idle_disconnect_enabled(void);
void mori_set_wifi_idle_disconnect_enabled(bool enabled);
uint16_t mori_wifi_idle_off_minutes(void);
void mori_set_wifi_idle_off_minutes(uint16_t minutes);

uint8_t mori_load_ui_language_from_system_ini(bool *found_out);
esp_err_t mori_save_language_settings_to_system_ini(const char *language_ini, uint8_t ui_language);
esp_err_t mori_save_power_idle_settings_to_system_ini(uint16_t screen_minutes, uint16_t wifi_minutes);
uint8_t mori_load_display_brightness_from_system_ini(bool *found_out);
uint8_t mori_load_audio_volume_from_system_ini(bool *found_out);
esp_err_t mori_save_av_settings_to_system_ini(uint8_t brightness, uint8_t volume_percent);

#endif /* MORI_SYSTEM_SETTINGS_H */

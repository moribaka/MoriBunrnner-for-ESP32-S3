/* Persistent brightness and temporary sleep are separate state. Serialize
 * updates from UI/network and the idle task before touching PWM. */
esp_err_t lcd_display_set_brightness(uint8_t brightness)
{
    if (!s_backlight_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_backlight_lock, portMAX_DELAY);
    esp_err_t err = lcd_backlight_pwm_init();
    if (err == ESP_OK) {
        s_backlight_brightness = brightness;
        lcd_backlight_set(!s_backlight_suspended);
    }
    xSemaphoreGive(s_backlight_lock);
    return err;
}

esp_err_t lcd_display_set_backlight_suspended(bool suspended)
{
    if (!s_backlight_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_backlight_lock, portMAX_DELAY);
    esp_err_t err = lcd_backlight_pwm_init();
    if (err == ESP_OK) {
        s_backlight_suspended = suspended;
        lcd_backlight_set(!suspended);
    }
    xSemaphoreGive(s_backlight_lock);
    return err;
}

uint8_t lcd_display_get_brightness(void)
{
    if (!s_backlight_lock) return s_backlight_brightness;
    xSemaphoreTake(s_backlight_lock, portMAX_DELAY);
    uint8_t brightness = s_backlight_brightness;
    xSemaphoreGive(s_backlight_lock);
    return brightness;
}

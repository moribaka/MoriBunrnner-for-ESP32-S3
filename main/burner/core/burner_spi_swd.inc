/* SWD borrows the manual CS1 pin while retaining the SPI bus/device. */
esp_err_t burner_spi_enter_swd_mode(void)
{
#if BURNER_SPI_ENABLE
    burner_spi_lock_take();
    bool was_swd = s_swd_mode;
    s_swd_mode = true;
    if (!was_swd) burner_spi_release_cs();
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << MORI_PIN_MCU_SWDIO) | (1ULL << MORI_PIN_MCU_SWCLK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) return ESP_OK; /* Caller owns the lock until leave. */
    if (!was_swd) {
        esp_err_t restore_err = burner_spi_restore_cs_pins();
        if (restore_err == ESP_OK) s_swd_mode = false;
        else {
            s_swd_restore_blocked = true;
            ESP_LOGE(BURNER_TAG, "SWD pin rollback failed: %s", esp_err_to_name(restore_err));
        }
    }
    burner_spi_lock_give();
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t burner_spi_leave_swd_mode(bool restore_spi)
{
#if BURNER_SPI_ENABLE
    esp_err_t err = ESP_OK;
    if (restore_spi && !s_swd_restore_blocked) {
        err = gpio_set_direction(MORI_PIN_MCU_SWCLK, GPIO_MODE_INPUT);
        if (err == ESP_OK) err = gpio_set_pull_mode(MORI_PIN_MCU_SWCLK, GPIO_FLOATING);
        if (err == ESP_OK) err = burner_spi_restore_cs_pins();
        if (err == ESP_OK) {
            s_swd_mode = false;
            burner_reset_cart_probe_state();
            ag32_mcu_link_invalidate();
            err = burner_spi_init();
        }
        if (err != ESP_OK) {
            s_swd_mode = true;
            s_swd_restore_blocked = true;
        }
    }
    burner_spi_lock_give();
    return err;
#else
    (void)restore_spi;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void burner_spi_block_swd_restore(void) { s_swd_restore_blocked = true; }
void burner_spi_allow_swd_restore(void) { s_swd_restore_blocked = false; }
bool burner_spi_swd_restore_blocked(void) { return s_swd_restore_blocked; }

static int s_cart_power_mv = -1; /* Last successful rail command; not an ADC reading. */

int burner_bacon_cart_power_mv(void) { return s_cart_power_mv; }

static uint8_t burner_bacon_option_byte1(
    bool power_en,
    bool power_5v,
    bool power_3v,
    bool dir_cs2,
    uint8_t phi_div)
{
    uint8_t ret = 0u;

    if (power_en) {
        ret |= 0x40u;
    }
    if (power_5v) {
        ret |= 0x20u;
    }
    if (power_3v) {
        ret |= 0x10u;
    }
    if (dir_cs2) {
        ret |= 0x04u;
    }
    ret |= (uint8_t)(phi_div & 0x03u);
    return ret;
}

esp_err_t burner_bacon_gba_power_cmd(bool power_5v, bool power_3v3)
{
    uint8_t cmd;
    if (power_5v && power_3v3) return ESP_ERR_INVALID_ARG;

    /*
     * Host dual-CS power command (spi_cs=1):
     * option byte with batch=1, cs2/cs1/rd/wr=1, dir_a=5V, dir_ad=3V3.
     * This is decoded in example/logic/bacon.v.
     */
    cmd = burner_bacon_option_byte1(
        power_5v || power_3v3,
        power_5v,
        power_3v3,
        true,
        0u);
    esp_err_t err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_1, &cmd, NULL, 1u);
    if (err == ESP_OK) s_cart_power_mv = power_5v ? 5000 : power_3v3 ? 3300 : 0;
    else s_cart_power_mv = -1;
    return err;
}

esp_err_t burner_bacon_gba_power_cycle_3v3_locked(void)
{
    esp_err_t err;

    err = burner_bacon_gba_release_bus_idle();
    if (err != ESP_OK) return err;
    err = burner_bacon_gba_power_cmd(false, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(s_bacon_power_settle_ms));

    err = burner_bacon_gba_power_cmd(false, true);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(s_bacon_power_settle_ms));
    return ESP_OK;
}

esp_err_t burner_bacon_gba_release_bus_idle(void)
{
    uint8_t cmd;

    /*
     * Match the host's one-byte interface init after 3V3 is enabled:
     * optionByte0(0, ai, adi, cs2h, cs1h, rdh, wrh).
     * This restores the GBA-side shadow outputs to a known released state
     * before short command transactions such as read-ID/autoselect.
     */
    cmd = burner_bacon_option_byte0(0u, false, false, true, true, true, true);
    return burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, &cmd, NULL, 1u);
}

esp_err_t burner_bacon_gba_prepare_power(void)
{
    esp_err_t err = ESP_OK;

    err = burner_bacon_gba_power_cycle_3v3_locked();
    if (err != ESP_OK) {
        return err;
    }

    return burner_bacon_gba_release_bus_idle();
}

esp_err_t burner_bacon_mbc5_prepare_power(void)
{
    esp_err_t err;
    bool use_5v = (s_mbc5_power_5v_enabled != 0u);

    /* Match host mission: power rails off first, then enable the selected GBC rail. */
    err = burner_bacon_gba_release_bus_idle();
    if (err != ESP_OK) return err;
    err = burner_bacon_gba_power_cmd(false, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(s_bacon_power_settle_ms));

    err = burner_bacon_gba_power_cmd(use_5v, !use_5v);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(BURNER_TAG, "MBC5 power rail: %s", use_5v ? "5V" : "3V3");
    vTaskDelay(pdMS_TO_TICKS(s_bacon_power_settle_ms));
    return ESP_OK;
}

esp_err_t burner_bacon_finish_cart_access(void)
{
    /* Leave the selected rail in place. Idle timeout owns power-off; the next
     * prepare owns break-before-make when changing rails. */
    return burner_bacon_gba_release_bus_idle();
}

#include "mori_i2c.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "mori_i2c";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static bool s_i2c_ready = false;

esp_err_t mori_i2c_init(gpio_num_t sda, gpio_num_t scl, uint32_t bus_freq_hz)
{
    esp_err_t err;
    i2c_master_bus_config_t bus_cfg = {0};
    uint32_t freq_hz = bus_freq_hz;

    if (s_i2c_ready) {
        return ESP_OK;
    }
    if (sda == GPIO_NUM_NC || scl == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    if (freq_hz == 0) {
        freq_hz = MORI_I2C_DEFAULT_FREQ_HZ;
    }

    bus_cfg.i2c_port = -1;
    bus_cfg.sda_io_num = sda;
    bus_cfg.scl_io_num = scl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.intr_priority = 0;
    /* Keep synchronous mode (same behavior as moriesp32 sample). */
    bus_cfg.trans_queue_depth = 0;
    bus_cfg.flags.enable_internal_pullup = 0;
    bus_cfg.flags.allow_pd = 0;

    err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create i2c bus failed: %s", esp_err_to_name(err));
        return err;
    }

    s_i2c_ready = true;
    (void)i2c_master_bus_reset(s_i2c_bus);
    ESP_LOGI(TAG, "I2C bus ready. SDA=%d, SCL=%d, freq=%" PRIu32, (int)sda, (int)scl, freq_hz);
    return ESP_OK;
}

i2c_master_bus_handle_t mori_i2c_get_bus(void)
{
    return s_i2c_bus;
}

bool mori_i2c_ready(void)
{
    return s_i2c_ready;
}

esp_err_t mori_i2c_probe_addr(uint8_t address_7bit)
{
    if (!s_i2c_ready || s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (address_7bit > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_probe(s_i2c_bus, address_7bit, MORI_I2C_PROBE_TIMEOUT_MS);
}

esp_err_t mori_i2c_scan_7bit(uint8_t *out_addrs, size_t max_count, size_t *found_count)
{
    size_t found = 0;

    if (!s_i2c_ready || s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((out_addrs == NULL && max_count > 0) || found_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, MORI_I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
            if (found < max_count) {
                out_addrs[found] = addr;
            }
            found++;
        }
    }

    *found_count = found;
    return ESP_OK;
}

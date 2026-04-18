#ifndef MORI_I2C_H
#define MORI_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MORI_I2C_DEFAULT_FREQ_HZ 400000U
#define MORI_I2C_PROBE_TIMEOUT_MS 50

esp_err_t mori_i2c_init(gpio_num_t sda, gpio_num_t scl, uint32_t bus_freq_hz);
i2c_master_bus_handle_t mori_i2c_get_bus(void);
bool mori_i2c_ready(void);

esp_err_t mori_i2c_probe_addr(uint8_t address_7bit);
esp_err_t mori_i2c_scan_7bit(uint8_t *out_addrs, size_t max_count, size_t *found_count);

#ifdef __cplusplus
}
#endif

#endif

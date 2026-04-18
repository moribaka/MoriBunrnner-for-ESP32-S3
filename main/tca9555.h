#ifndef TCA9555_H
#define TCA9555_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCA9555_I2C_ADDR_MIN 0x20
#define TCA9555_I2C_ADDR_MAX 0x27
#define TCA9555_I2C_ADDR_DEFAULT 0x20

/* TCA9555 register map. */
#define TCA9555_REG_INPUT_PORT0 0x00
#define TCA9555_REG_INPUT_PORT1 0x01
#define TCA9555_REG_OUTPUT_PORT0 0x02
#define TCA9555_REG_OUTPUT_PORT1 0x03
#define TCA9555_REG_POLARITY_PORT0 0x04
#define TCA9555_REG_POLARITY_PORT1 0x05
#define TCA9555_REG_CONFIG_PORT0 0x06
#define TCA9555_REG_CONFIG_PORT1 0x07

/* Pin masks */
#define TCA9555_IO0_0 (1U << 0)
#define TCA9555_IO0_1 (1U << 1)
#define TCA9555_IO0_2 (1U << 2)
#define TCA9555_IO0_3 (1U << 3)
#define TCA9555_IO0_4 (1U << 4)
#define TCA9555_IO0_5 (1U << 5)
#define TCA9555_IO0_6 (1U << 6)
#define TCA9555_IO0_7 (1U << 7)
#define TCA9555_IO1_0 (1U << 8)
#define TCA9555_IO1_1 (1U << 9)
#define TCA9555_IO1_2 (1U << 10)
#define TCA9555_IO1_3 (1U << 11)
#define TCA9555_IO1_4 (1U << 12)
#define TCA9555_IO1_5 (1U << 13)
#define TCA9555_IO1_6 (1U << 14)
#define TCA9555_IO1_7 (1U << 15)

typedef void (*tca9555_input_cb_t)(uint16_t pin_mask, int level, void *user_ctx);

esp_err_t tca9555_probe(i2c_master_bus_handle_t bus, uint8_t address_7bit);
esp_err_t tca9555_init(i2c_master_bus_handle_t bus, uint8_t address_7bit);
bool tca9555_ready(void);
uint8_t tca9555_address(void);

esp_err_t tca9555_read_word(uint8_t reg, uint16_t *value);
esp_err_t tca9555_write_word(uint8_t reg, uint16_t value);

esp_err_t tca9555_read_inputs(uint16_t *inputs);
esp_err_t tca9555_read_outputs(uint16_t *outputs);
esp_err_t tca9555_write_outputs(uint16_t outputs);

esp_err_t tca9555_read_config(uint16_t *config);
esp_err_t tca9555_write_config(uint16_t config);
esp_err_t tca9555_set_pin_mode(uint16_t pin_mask, bool input_mode);

esp_err_t tca9555_set_polarity_invert(uint16_t invert_mask);
esp_err_t tca9555_pin_read(uint16_t pin_mask, int *level);
esp_err_t tca9555_pin_write(uint16_t pin_mask, bool level_high);
esp_err_t tca9555_enable_irq(gpio_num_t irq_gpio, tca9555_input_cb_t cb, void *user_ctx);
esp_err_t tca9555_disable_irq(void);

#ifdef __cplusplus
}
#endif

#endif

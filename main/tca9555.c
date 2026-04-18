#include "tca9555.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "tca9555";

static i2c_master_dev_handle_t s_tca9555_dev = NULL;
static uint8_t s_tca9555_addr = TCA9555_I2C_ADDR_DEFAULT;
static bool s_tca9555_ready = false;
static uint16_t s_output_shadow = 0;
static bool s_output_shadow_valid = false;
static EventGroupHandle_t s_irq_event_group = NULL;
static TaskHandle_t s_irq_task = NULL;
static gpio_num_t s_irq_gpio = GPIO_NUM_NC;
static tca9555_input_cb_t s_irq_cb = NULL;
static void *s_irq_user_ctx = NULL;

#define TCA9555_XFER_TIMEOUT_MS 500
#define TCA9555_SCL_HZ 100000
#define TCA9555_IRQ_EVENT_BIT BIT0

static void IRAM_ATTR tca9555_int_handler(void *param)
{
    BaseType_t task_woken = pdFALSE;

    (void)param;
    if (s_irq_event_group == NULL) {
        return;
    }

    xEventGroupSetBitsFromISR(s_irq_event_group, TCA9555_IRQ_EVENT_BIT, &task_woken);
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void tca9555_irq_task(void *param)
{
    uint16_t last_input = 0;
    bool last_input_valid = false;

    (void)param;

    if (tca9555_read_inputs(&last_input) == ESP_OK) {
        last_input_valid = true;
    }

    while (1) {
        EventBits_t ev = xEventGroupWaitBits(
            s_irq_event_group,
            TCA9555_IRQ_EVENT_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);

        if ((ev & TCA9555_IRQ_EVENT_BIT) == 0) {
            continue;
        }

        /* Match sample debounce strategy. */
        esp_rom_delay_us(10000);
        if (s_irq_gpio != GPIO_NUM_NC && gpio_get_level(s_irq_gpio) != 0) {
            continue;
        }

        {
            uint16_t input = 0;
            if (tca9555_read_inputs(&input) != ESP_OK) {
                continue;
            }

            if (!last_input_valid) {
                last_input = input;
                last_input_valid = true;
                continue;
            }

            {
                uint16_t changed = (uint16_t)(input ^ last_input);
                if (changed != 0 && s_irq_cb != NULL) {
                    for (int i = 0; i < 16; i++) {
                        uint16_t pin_mask = (uint16_t)(1U << i);
                        int level = 0;
                        if ((changed & pin_mask) == 0) {
                            continue;
                        }
                        level = ((input & pin_mask) != 0U) ? 1 : 0;
                        s_irq_cb(pin_mask, level, s_irq_user_ctx);
                    }
                }
            }

            last_input = input;
        }
    }
}

esp_err_t tca9555_probe(i2c_master_bus_handle_t bus, uint8_t address_7bit)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (address_7bit > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_probe(bus, address_7bit, TCA9555_XFER_TIMEOUT_MS);
}

esp_err_t tca9555_init(i2c_master_bus_handle_t bus, uint8_t address_7bit)
{
    esp_err_t err;
    i2c_device_config_t dev_cfg = {0};

    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tca9555_ready) {
        return ESP_OK;
    }
    if (address_7bit > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }

    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address_7bit;
    dev_cfg.scl_speed_hz = TCA9555_SCL_HZ;
    dev_cfg.scl_wait_us = 0;
    dev_cfg.flags.disable_ack_check = 0;

    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_tca9555_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add i2c device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_tca9555_addr = address_7bit;
    s_tca9555_ready = true;
    s_output_shadow_valid = false;
    ESP_LOGI(TAG, "TCA9555 ready at 0x%02X", s_tca9555_addr);
    return ESP_OK;
}

bool tca9555_ready(void)
{
    return s_tca9555_ready;
}

uint8_t tca9555_address(void)
{
    return s_tca9555_addr;
}

esp_err_t tca9555_write_word(uint8_t reg, uint16_t value)
{
    uint8_t tx[3] = {reg, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF)};

    if (!s_tca9555_ready || s_tca9555_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit(s_tca9555_dev, tx, sizeof(tx), TCA9555_XFER_TIMEOUT_MS);
}

esp_err_t tca9555_read_word(uint8_t reg, uint16_t *value)
{
    esp_err_t err;
    uint8_t addr[1] = {reg};
    uint16_t raw = 0;

    if (!s_tca9555_ready || s_tca9555_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = i2c_master_transmit_receive(
        s_tca9555_dev, addr, sizeof(addr), (uint8_t *)&raw, sizeof(raw), TCA9555_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    *value = raw;
    return ESP_OK;
}

esp_err_t tca9555_read_inputs(uint16_t *inputs)
{
    return tca9555_read_word(TCA9555_REG_INPUT_PORT0, inputs);
}

esp_err_t tca9555_read_outputs(uint16_t *outputs)
{
    esp_err_t err = tca9555_read_word(TCA9555_REG_OUTPUT_PORT0, outputs);
    if (err == ESP_OK && outputs != NULL) {
        s_output_shadow = *outputs;
        s_output_shadow_valid = true;
    }
    return err;
}

esp_err_t tca9555_write_outputs(uint16_t outputs)
{
    esp_err_t err = tca9555_write_word(TCA9555_REG_OUTPUT_PORT0, outputs);
    if (err == ESP_OK) {
        s_output_shadow = outputs;
        s_output_shadow_valid = true;
    }
    return err;
}

esp_err_t tca9555_read_config(uint16_t *config)
{
    return tca9555_read_word(TCA9555_REG_CONFIG_PORT0, config);
}

esp_err_t tca9555_write_config(uint16_t config)
{
    return tca9555_write_word(TCA9555_REG_CONFIG_PORT0, config);
}

esp_err_t tca9555_set_pin_mode(uint16_t pin_mask, bool input_mode)
{
    esp_err_t err;
    uint16_t config = 0;

    err = tca9555_read_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    if (input_mode) {
        config |= pin_mask;
    } else {
        config &= (uint16_t)(~pin_mask);
    }

    return tca9555_write_config(config);
}

esp_err_t tca9555_set_polarity_invert(uint16_t invert_mask)
{
    return tca9555_write_word(TCA9555_REG_POLARITY_PORT0, invert_mask);
}

esp_err_t tca9555_pin_read(uint16_t pin_mask, int *level)
{
    esp_err_t err;
    uint16_t inputs = 0;

    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = tca9555_read_inputs(&inputs);
    if (err != ESP_OK) {
        return err;
    }

    *level = (inputs & pin_mask) ? 1 : 0;
    return ESP_OK;
}

esp_err_t tca9555_pin_write(uint16_t pin_mask, bool level_high)
{
    esp_err_t err;
    uint16_t outputs = 0;

    if (s_output_shadow_valid) {
        outputs = s_output_shadow;
    } else {
        err = tca9555_read_outputs(&outputs);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (level_high) {
        outputs |= pin_mask;
    } else {
        outputs &= (uint16_t)(~pin_mask);
    }

    return tca9555_write_outputs(outputs);
}

esp_err_t tca9555_disable_irq(void)
{
    if (s_irq_gpio != GPIO_NUM_NC) {
        gpio_isr_handler_remove(s_irq_gpio);
    }

    if (s_irq_task != NULL) {
        vTaskDelete(s_irq_task);
        s_irq_task = NULL;
    }

    if (s_irq_event_group != NULL) {
        vEventGroupDelete(s_irq_event_group);
        s_irq_event_group = NULL;
    }

    s_irq_gpio = GPIO_NUM_NC;
    s_irq_cb = NULL;
    s_irq_user_ctx = NULL;
    return ESP_OK;
}

esp_err_t tca9555_enable_irq(gpio_num_t irq_gpio, tca9555_input_cb_t cb, void *user_ctx)
{
    esp_err_t err;
    gpio_config_t int_cfg = {0};
    BaseType_t task_ok = pdFAIL;

    if (!s_tca9555_ready || s_tca9555_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (irq_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Reconfigure when already enabled. */
    (void)tca9555_disable_irq();

    s_irq_event_group = xEventGroupCreate();
    if (s_irq_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_irq_gpio = irq_gpio;
    s_irq_cb = cb;
    s_irq_user_ctx = user_ctx;

    int_cfg.intr_type = GPIO_INTR_NEGEDGE;
    int_cfg.mode = GPIO_MODE_INPUT;
    int_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    int_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_cfg.pin_bit_mask = (1ULL << (uint32_t)irq_gpio);
    err = gpio_config(&int_cfg);
    if (err != ESP_OK) {
        (void)tca9555_disable_irq();
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)tca9555_disable_irq();
        return err;
    }

    err = gpio_isr_handler_add(irq_gpio, tca9555_int_handler, (void *)(uintptr_t)irq_gpio);
    if (err != ESP_OK) {
        (void)tca9555_disable_irq();
        return err;
    }

    task_ok = xTaskCreatePinnedToCore(
        tca9555_irq_task,
        "tca9555_irq",
        4096,
        NULL,
        3,
        &s_irq_task,
        1);
    if (task_ok != pdPASS) {
        (void)tca9555_disable_irq();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IRQ enabled on GPIO %d", (int)irq_gpio);
    return ESP_OK;
}

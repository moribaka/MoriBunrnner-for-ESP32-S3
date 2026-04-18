#include "ip5306.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ip5306";

static i2c_master_dev_handle_t s_ip5306_dev = NULL;
static uint8_t s_ip5306_addr = IP5306_I2C_ADDR_DEFAULT;
static bool s_ip5306_ready = false;

#define IP5306_XFER_TIMEOUT_MS 100

esp_err_t ip5306_probe(i2c_master_bus_handle_t bus, uint8_t address_7bit)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (address_7bit > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_probe(bus, address_7bit, IP5306_XFER_TIMEOUT_MS);
}

esp_err_t ip5306_init(i2c_master_bus_handle_t bus, uint8_t address_7bit)
{
    esp_err_t err;
    i2c_device_config_t dev_cfg = {0};
    uint8_t probe_val = 0;

    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ip5306_ready) {
        return ESP_OK;
    }
    if (address_7bit > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }

    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address_7bit;
    dev_cfg.scl_speed_hz = 400000;
    dev_cfg.scl_wait_us = 0;
    dev_cfg.flags.disable_ack_check = 0;

    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_ip5306_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add i2c device failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_transmit_receive(
        s_ip5306_dev, (const uint8_t[]){IP5306_REG_SYS_CTL0}, 1, &probe_val, 1, IP5306_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read SYS_CTL0 failed after attach: %s", esp_err_to_name(err));
        (void)i2c_master_bus_rm_device(s_ip5306_dev);
        s_ip5306_dev = NULL;
        return err;
    }

    s_ip5306_addr = address_7bit;
    s_ip5306_ready = true;
    ESP_LOGI(TAG, "IP5306 ready at 0x%02X SYS_CTL0=0x%02X", s_ip5306_addr, probe_val);
    return ESP_OK;
}

bool ip5306_ready(void)
{
    return s_ip5306_ready;
}

uint8_t ip5306_address(void)
{
    return s_ip5306_addr;
}

esp_err_t ip5306_read_reg(uint8_t reg, uint8_t *value)
{
    if (!s_ip5306_ready || s_ip5306_dev == NULL || value == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(
        s_ip5306_dev, &reg, 1, value, 1, IP5306_XFER_TIMEOUT_MS);
}

esp_err_t ip5306_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};

    if (!s_ip5306_ready || s_ip5306_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit(s_ip5306_dev, tx, sizeof(tx), IP5306_XFER_TIMEOUT_MS);
}

esp_err_t ip5306_update_bits(uint8_t reg, uint8_t mask, bool set_bits)
{
    esp_err_t err;
    uint8_t value = 0;

    err = ip5306_read_reg(reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    if (set_bits) {
        value |= mask;
    } else {
        value &= (uint8_t)~mask;
    }

    return ip5306_write_reg(reg, value);
}

static esp_err_t ip5306_update_field(uint8_t reg, uint8_t mask, uint8_t value_shifted)
{
    esp_err_t err;
    uint8_t value = 0;

    err = ip5306_read_reg(reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    value = (uint8_t)((value & (uint8_t)~mask) | (value_shifted & mask));
    return ip5306_write_reg(reg, value);
}

esp_err_t ip5306_get_sys_ctrl(uint8_t *ctl0, uint8_t *ctl1, uint8_t *ctl2)
{
    esp_err_t err;
    uint8_t val = 0;

    if (ctl0 != NULL) {
        err = ip5306_read_reg(IP5306_REG_SYS_CTL0, &val);
        if (err != ESP_OK) {
            return err;
        }
        *ctl0 = val;
    }

    if (ctl1 != NULL) {
        err = ip5306_read_reg(IP5306_REG_SYS_CTL1, &val);
        if (err != ESP_OK) {
            return err;
        }
        *ctl1 = val;
    }

    if (ctl2 != NULL) {
        err = ip5306_read_reg(IP5306_REG_SYS_CTL2, &val);
        if (err != ESP_OK) {
            return err;
        }
        *ctl2 = val;
    }

    return ESP_OK;
}

esp_err_t ip5306_set_boost_enable(bool enabled)
{
    return ip5306_update_bits(IP5306_REG_SYS_CTL0, IP5306_SYS_CTL0_BOOST_EN_BIT, enabled);
}

esp_err_t ip5306_set_charging_enable(bool enabled)
{
    return ip5306_update_bits(IP5306_REG_SYS_CTL0, IP5306_SYS_CTL0_CHG_EN_BIT, enabled);
}

esp_err_t ip5306_set_boost_keep_on(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL0, IP5306_SYS_CTL0_BOOST_KEEP_ON_BIT, enabled);
}

esp_err_t ip5306_set_insert_load_boot_enable(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL0, IP5306_SYS_CTL0_INSERT_LOAD_BOOT_EN_BIT, enabled);
}

esp_err_t ip5306_set_key_shutdown_enable(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL0, IP5306_SYS_CTL0_KEY_SHUTDOWN_EN_BIT, enabled);
}

esp_err_t ip5306_set_boost_off_by_long_press(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_BOOST_OFF_BY_LONG_PRESS_BIT, enabled);
}

esp_err_t ip5306_set_wled_toggle_by_double_press(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_WLED_TOGGLE_BY_DOUBLE_PRESS_BIT, enabled);
}

esp_err_t ip5306_set_short_press_boost_toggle(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_SHORT_PRESS_BOOST_TOGGLE_BIT, enabled);
}

esp_err_t ip5306_set_keep_boost_on_vin_remove(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_KEEP_BOOST_ON_VIN_REMOVE_BIT, enabled);
}

esp_err_t ip5306_set_batlow_shutdown_enable(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_BATLOW_SHUTDOWN_EN_BIT, enabled);
}

esp_err_t ip5306_set_light_load_shutdown_time(ip5306_light_load_shutdown_t setting)
{
    uint8_t encoded = ((uint8_t)setting & 0x03U) << 2;
    return ip5306_update_field(
        IP5306_REG_SYS_CTL2, IP5306_SYS_CTL2_LIGHT_LOAD_SHUTDOWN_MASK, encoded);
}

esp_err_t ip5306_set_boost_ctrl_signal(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_BOOST_CTRL_SIGNAL_SELECTION_BIT, enabled);
}

esp_err_t ip5306_set_flashlight_ctrl_signal(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_FLASHLIGHT_CTRL_SIGNAL_SELECTION_BIT, enabled);
}

esp_err_t ip5306_set_boost_after_vin(bool enabled)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL1, IP5306_SYS_CTL1_BOOST_AFTER_VIN_BIT, enabled);
}

esp_err_t ip5306_set_long_press_time(ip5306_long_press_time_t setting)
{
    return ip5306_update_bits(
        IP5306_REG_SYS_CTL2, IP5306_SYS_CTL2_LONG_PRESS_TIME_BIT, setting == IP5306_LONG_PRESS_3S);
}

esp_err_t ip5306_set_charging_stop_voltage(ip5306_chg_full_stop_voltage_t setting)
{
    uint8_t encoded = ((uint8_t)setting & 0x03U);
    return ip5306_update_field(
        IP5306_REG_CHG_CTL0, IP5306_CHG_CTL0_CHARGING_FULL_STOP_VOLTAGE_MASK, encoded);
}

esp_err_t ip5306_set_end_charge_current_detection(ip5306_end_charge_current_t setting)
{
    uint8_t encoded = (((uint8_t)setting & 0x03U) << 6);
    return ip5306_update_field(
        IP5306_REG_CHG_CTL1, IP5306_CHG_CTL1_END_CHARGE_CURRENT_DETECTION_MASK, encoded);
}

esp_err_t ip5306_set_charge_under_voltage_loop(ip5306_chg_under_voltage_loop_t setting)
{
    uint8_t encoded = (((uint8_t)setting & 0x07U) << 2);
    return ip5306_update_field(
        IP5306_REG_CHG_CTL1, IP5306_CHG_CTL1_CHARGE_UNDER_VOLTAGE_LOOP_MASK, encoded);
}

esp_err_t ip5306_set_battery_voltage(ip5306_battery_voltage_t setting)
{
    uint8_t encoded = (((uint8_t)setting & 0x03U) << 2);
    return ip5306_update_field(
        IP5306_REG_CHG_CTL2, IP5306_CHG_CTL2_BATTERY_VOLTAGE_MASK, encoded);
}

esp_err_t ip5306_set_voltage_pressure(ip5306_voltage_pressure_t setting)
{
    uint8_t encoded = ((uint8_t)setting & 0x03U);
    return ip5306_update_field(
        IP5306_REG_CHG_CTL2, IP5306_CHG_CTL2_VOLTAGE_PRESSURE_MASK, encoded);
}

esp_err_t ip5306_set_charge_cc_loop_vin(bool use_vin_loop)
{
    return ip5306_update_bits(
        IP5306_REG_CHG_CTL3, IP5306_CHG_CTL3_CHARGE_CC_LOOP_BIT, use_vin_loop);
}

esp_err_t ip5306_get_light_load_shutdown_time(ip5306_light_load_shutdown_t *setting)
{
    esp_err_t err;
    uint8_t value = 0;

    if (setting == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ip5306_read_reg(IP5306_REG_SYS_CTL2, &value);
    if (err != ESP_OK) {
        return err;
    }

    *setting = (ip5306_light_load_shutdown_t)((value & IP5306_SYS_CTL2_LIGHT_LOAD_SHUTDOWN_MASK) >> 2);
    return ESP_OK;
}

esp_err_t ip5306_get_status(ip5306_status_t *status)
{
    esp_err_t err;
    uint8_t read0 = 0;
    uint8_t read1 = 0;
    uint8_t read2 = 0;
    uint8_t read3 = 0;

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ip5306_read_reg(IP5306_REG_READ0, &read0);
    if (err != ESP_OK) {
        return err;
    }
    err = ip5306_read_reg(IP5306_REG_READ1, &read1);
    if (err != ESP_OK) {
        return err;
    }
    err = ip5306_read_reg(IP5306_REG_READ2, &read2);
    if (err != ESP_OK) {
        return err;
    }
    err = ip5306_read_reg(IP5306_REG_READ3, &read3);
    if (err != ESP_OK) {
        return err;
    }

    status->charge_enable = (read0 & IP5306_READ0_CHARGE_ENABLE_BIT) != 0;
    status->charge_full = (read1 & IP5306_READ1_CHARGE_FULL_BIT) != 0;
    status->light_load = (read2 & IP5306_READ2_LIGHT_LOAD_BIT) != 0;
    status->key_double_press = (read3 & IP5306_READ3_KEY_DOUBLE_PRESS_BIT) != 0;
    status->key_long_press = (read3 & IP5306_READ3_KEY_LONG_PRESS_BIT) != 0;
    status->key_short_press = (read3 & IP5306_READ3_KEY_SHORT_PRESS_BIT) != 0;
    return ESP_OK;
}

esp_err_t ip5306_clear_key_event_flags(bool clear_short, bool clear_long, bool clear_double)
{
    uint8_t value = 0;

    if (clear_short) {
        value |= IP5306_READ3_KEY_SHORT_PRESS_BIT;
    }
    if (clear_long) {
        value |= IP5306_READ3_KEY_LONG_PRESS_BIT;
    }
    if (clear_double) {
        value |= IP5306_READ3_KEY_DOUBLE_PRESS_BIT;
    }
    if (value == 0) {
        return ESP_OK;
    }

    /* REG_READ3 bit[2:0] are write-1-to-clear event flags. */
    return ip5306_write_reg(IP5306_REG_READ3, value);
}

esp_err_t ip5306_get_battery_level_percent(uint8_t *percent)
{
    esp_err_t err;
    uint8_t raw = 0;

    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ip5306_read_reg(IP5306_REG_BAT_LEVEL, &raw);
    if (err != ESP_OK) {
        return err;
    }

    switch (raw & 0xF0U) {
        case 0xE0:
            *percent = 25;
            break;
        case 0xC0:
            *percent = 50;
            break;
        case 0x80:
            *percent = 75;
            break;
        case 0x00:
            *percent = 100;
            break;
        default:
            *percent = 0;
            break;
    }

    return ESP_OK;
}

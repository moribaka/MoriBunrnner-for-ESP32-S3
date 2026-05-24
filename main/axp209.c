#include "axp209.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "axp209";

static i2c_master_dev_handle_t s_axp209_dev = NULL;
static uint8_t s_axp209_addr = AXP209_I2C_ADDR_DEFAULT;
static bool s_axp209_ready = false;

#define AXP209_XFER_TIMEOUT_MS 100

#define AXP209_REG_POWER_STATUS     0x00u
#define AXP209_REG_CHARGE_STATUS    0x01u
#define AXP209_REG_SHUTDOWN_BATMON  0x32u
#define AXP209_REG_CHARGE_CTRL1     0x33u
#define AXP209_REG_CHARGE_CTRL2     0x34u
#define AXP209_REG_IRQ_STATUS1      0x48u
#define AXP209_REG_IRQ_STATUS_COUNT 5u
#define AXP209_REG_ACIN_VOLT_H      0x56u
#define AXP209_REG_VBUS_VOLT_H      0x5Au
#define AXP209_REG_TEMP_H           0x5Eu
#define AXP209_REG_BATTERY_VOLT_H   0x78u
#define AXP209_REG_ADC_ENABLE1      0x82u
#define AXP209_REG_ADC_ENABLE2      0x83u

#define AXP209_ADC_ENABLE1_BATTERY_VOLTAGE (1u << 7)
#define AXP209_ADC_ENABLE1_BATTERY_CURRENT (1u << 6)
#define AXP209_ADC_ENABLE1_ACIN_VOLTAGE    (1u << 5)
#define AXP209_ADC_ENABLE1_VBUS_VOLTAGE    (1u << 3)
#define AXP209_ADC_ENABLE1_APS_VOLTAGE     (1u << 1)
#define AXP209_ADC_ENABLE2_INTERNAL_TEMP   (1u << 7)
#define AXP209_SHUTDOWN_BATMON_ENABLE      (1u << 6)

static uint16_t axp209_combine_12bit(uint8_t high, uint8_t low)
{
    return (uint16_t)(((uint16_t)high << 4) | (uint16_t)(low & 0x0Fu));
}

static uint16_t axp209_combine_13bit(uint8_t high, uint8_t low)
{
    return (uint16_t)(((uint16_t)high << 5) | (uint16_t)(low & 0x1Fu));
}

static esp_err_t axp209_update_reg_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    esp_err_t err;
    uint8_t reg_value = 0u;

    err = axp209_read_reg(reg, &reg_value);
    if (err != ESP_OK) {
        return err;
    }
    reg_value = (uint8_t)((reg_value & (uint8_t)~mask) | (value & mask));
    return axp209_write_reg(reg, reg_value);
}

static uint16_t axp209_decode_charge_target_mv(uint8_t reg33)
{
    switch ((reg33 >> 5) & 0x03u) {
        case 0u:
            return 4100u;
        case 1u:
            return 4150u;
        case 2u:
            return 4200u;
        default:
            return 4360u;
    }
}

static bool axp209_encode_charge_target_mv(uint16_t millivolts, uint8_t *field_value)
{
    if (field_value == NULL) {
        return false;
    }
    switch (millivolts) {
        case 4100u:
            *field_value = 0u;
            return true;
        case 4150u:
            *field_value = 1u;
            return true;
        case 4200u:
            *field_value = 2u;
            return true;
        case 4360u:
            *field_value = 3u;
            return true;
        default:
            return false;
    }
}

esp_err_t axp209_probe(i2c_master_bus_handle_t bus, uint8_t address_7bit)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (address_7bit > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_probe(bus, address_7bit, AXP209_XFER_TIMEOUT_MS);
}

esp_err_t axp209_init(i2c_master_bus_handle_t bus, uint8_t address_7bit)
{
    esp_err_t err;
    i2c_device_config_t dev_cfg = {0};
    uint8_t adc_enable1 = 0u;
    uint8_t adc_enable2 = 0u;
    uint8_t shutdown_batmon = 0u;

    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_axp209_ready) {
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

    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_axp209_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add i2c device failed: %s", esp_err_to_name(err));
        return err;
    }

    err = axp209_read_reg(AXP209_REG_ADC_ENABLE1, &adc_enable1);
    if (err != ESP_OK) {
        goto fail;
    }
    err = axp209_read_reg(AXP209_REG_ADC_ENABLE2, &adc_enable2);
    if (err != ESP_OK) {
        goto fail;
    }
    err = axp209_read_reg(AXP209_REG_SHUTDOWN_BATMON, &shutdown_batmon);
    if (err != ESP_OK) {
        goto fail;
    }

    adc_enable1 |= (AXP209_ADC_ENABLE1_BATTERY_VOLTAGE |
                    AXP209_ADC_ENABLE1_BATTERY_CURRENT |
                    AXP209_ADC_ENABLE1_ACIN_VOLTAGE |
                    AXP209_ADC_ENABLE1_VBUS_VOLTAGE |
                    AXP209_ADC_ENABLE1_APS_VOLTAGE);
    adc_enable2 |= AXP209_ADC_ENABLE2_INTERNAL_TEMP;
    shutdown_batmon |= AXP209_SHUTDOWN_BATMON_ENABLE;

    err = axp209_write_reg(AXP209_REG_ADC_ENABLE1, adc_enable1);
    if (err != ESP_OK) {
        goto fail;
    }
    err = axp209_write_reg(AXP209_REG_ADC_ENABLE2, adc_enable2);
    if (err != ESP_OK) {
        goto fail;
    }
    err = axp209_write_reg(AXP209_REG_SHUTDOWN_BATMON, shutdown_batmon);
    if (err != ESP_OK) {
        goto fail;
    }
    err = axp209_set_charge_current(500u);
    if (err != ESP_OK) {
        goto fail;
    }
    err = axp209_acknowledge_irq();
    if (err != ESP_OK) {
        goto fail;
    }

    s_axp209_addr = address_7bit;
    s_axp209_ready = true;
    ESP_LOGI(TAG, "AXP209 ready at 0x%02X, charge current fixed to 500mA", s_axp209_addr);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "AXP209 init failed: %s", esp_err_to_name(err));
    s_axp209_ready = false;
    if (s_axp209_dev != NULL) {
        (void)i2c_master_bus_rm_device(s_axp209_dev);
        s_axp209_dev = NULL;
    }
    return err;
}

bool axp209_ready(void)
{
    return s_axp209_ready;
}

uint8_t axp209_address(void)
{
    return s_axp209_addr;
}

esp_err_t axp209_read_reg(uint8_t reg, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_axp209_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(
        s_axp209_dev, &reg, 1, value, 1, AXP209_XFER_TIMEOUT_MS);
}

esp_err_t axp209_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};

    if (s_axp209_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(s_axp209_dev, tx, sizeof(tx), AXP209_XFER_TIMEOUT_MS);
}

esp_err_t axp209_acknowledge_irq(void)
{
    esp_err_t err;
    uint8_t irq_status[AXP209_REG_IRQ_STATUS_COUNT] = {0};
    bool has_pending = false;

    err = i2c_master_transmit_receive(
        s_axp209_dev,
        (const uint8_t[]){AXP209_REG_IRQ_STATUS1},
        1,
        irq_status,
        sizeof(irq_status),
        AXP209_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < sizeof(irq_status); ++i) {
        if (irq_status[i] != 0u) {
            has_pending = true;
            break;
        }
    }
    if (!has_pending) {
        return ESP_OK;
    }

    uint8_t tx[1 + AXP209_REG_IRQ_STATUS_COUNT] = {0};
    tx[0] = AXP209_REG_IRQ_STATUS1;
    memcpy(&tx[1], irq_status, sizeof(irq_status));
    return i2c_master_transmit(s_axp209_dev, tx, sizeof(tx), AXP209_XFER_TIMEOUT_MS);
}

esp_err_t axp209_read_snapshot(axp209_snapshot_t *snapshot)
{
    esp_err_t err;
    uint8_t status_regs[2] = {0};
    uint8_t analog_regs[10] = {0};
    uint8_t battery_regs[8] = {0};

    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));

    err = i2c_master_transmit_receive(
        s_axp209_dev,
        (const uint8_t[]){AXP209_REG_POWER_STATUS},
        1,
        status_regs,
        sizeof(status_regs),
        AXP209_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit_receive(
        s_axp209_dev,
        (const uint8_t[]){AXP209_REG_ACIN_VOLT_H},
        1,
        analog_regs,
        sizeof(analog_regs),
        AXP209_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit_receive(
        s_axp209_dev,
        (const uint8_t[]){AXP209_REG_BATTERY_VOLT_H},
        1,
        battery_regs,
        sizeof(battery_regs),
        AXP209_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    snapshot->power_status = status_regs[0];
    snapshot->charge_status = status_regs[1];
    snapshot->acin_voltage_mv = ((uint32_t)axp209_combine_12bit(analog_regs[0], analog_regs[1]) * 17u) / 10u;
    snapshot->vbus_voltage_mv = ((uint32_t)axp209_combine_12bit(analog_regs[4], analog_regs[5]) * 17u) / 10u;
    snapshot->battery_voltage_mv =
        ((uint32_t)axp209_combine_12bit(battery_regs[0], battery_regs[1]) * 11u) / 10u;
    snapshot->battery_charge_current_ma_x10 =
        (uint32_t)axp209_combine_12bit(battery_regs[2], battery_regs[3]) * 5u;
    snapshot->battery_discharge_current_ma_x10 =
        (uint32_t)axp209_combine_13bit(battery_regs[4], battery_regs[5]) * 5u;
    snapshot->ipsout_voltage_mv =
        ((uint32_t)axp209_combine_12bit(battery_regs[6], battery_regs[7]) * 14u) / 10u;
    snapshot->internal_temp_deci_c =
        (int32_t)axp209_combine_12bit(analog_regs[8], analog_regs[9]) - 1447;
    return ESP_OK;
}

esp_err_t axp209_read_charge_control(axp209_charge_control_t *control)
{
    esp_err_t err;
    uint8_t regs[2] = {0};
    uint8_t precharge_sel;
    uint8_t cc_timeout_sel;

    if (control == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(control, 0, sizeof(*control));

    err = i2c_master_transmit_receive(
        s_axp209_dev,
        (const uint8_t[]){AXP209_REG_CHARGE_CTRL1},
        1,
        regs,
        sizeof(regs),
        AXP209_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    control->enabled = (regs[0] & (1u << 7)) != 0u;
    control->target_voltage_mv = axp209_decode_charge_target_mv(regs[0]);
    control->end_current_percent = ((regs[0] & (1u << 4)) != 0u) ? 15u : 10u;
    control->charge_current_ma = (uint16_t)(300u + ((uint16_t)(regs[0] & 0x0Fu) * 100u));
    precharge_sel = (uint8_t)((regs[1] >> 6) & 0x03u);
    cc_timeout_sel = (uint8_t)(regs[1] & 0x03u);
    control->precharge_timeout_min = (uint16_t)(40u + ((uint16_t)precharge_sel * 10u));
    control->constant_current_timeout_hours = (uint8_t)(6u + (cc_timeout_sel * 2u));
    control->chgled_blink_when_charging = (regs[1] & (1u << 4)) != 0u;
    return ESP_OK;
}

void axp209_decode_status(uint8_t power_status, uint8_t charge_status, axp209_status_flags_t *flags)
{
    if (flags == NULL) {
        return;
    }
    memset(flags, 0, sizeof(*flags));

    flags->acin_present = (power_status & (1u << 7)) != 0u;
    flags->acin_usable = (power_status & (1u << 6)) != 0u;
    flags->vbus_present = (power_status & (1u << 5)) != 0u;
    flags->vbus_usable = (power_status & (1u << 4)) != 0u;
    flags->vbus_above_vhold = (power_status & (1u << 3)) != 0u;
    flags->battery_charging = (power_status & (1u << 2)) != 0u;
    flags->acin_vbus_short = (power_status & (1u << 1)) != 0u;
    flags->boot_source_external = (power_status & (1u << 0)) != 0u;

    flags->over_temperature = (charge_status & (1u << 7)) != 0u;
    flags->charging = (charge_status & (1u << 6)) != 0u;
    flags->battery_present = (charge_status & (1u << 5)) != 0u;
    flags->battery_activated = (charge_status & (1u << 3)) != 0u;
    flags->charge_current_limited = (charge_status & (1u << 2)) != 0u;
}

esp_err_t axp209_set_charge_enabled(bool enabled)
{
    return axp209_update_reg_bits(
        AXP209_REG_CHARGE_CTRL1,
        (uint8_t)(1u << 7),
        enabled ? (uint8_t)(1u << 7) : 0u);
}

esp_err_t axp209_set_charge_current(uint16_t milliamps)
{
    uint8_t field_value;

    if (milliamps < 300u || milliamps > 1800u || ((milliamps - 300u) % 100u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    field_value = (uint8_t)((milliamps - 300u) / 100u);
    return axp209_update_reg_bits(AXP209_REG_CHARGE_CTRL1, 0x0Fu, field_value);
}

esp_err_t axp209_set_charge_target_voltage(uint16_t millivolts)
{
    uint8_t field_value = 0u;

    if (!axp209_encode_charge_target_mv(millivolts, &field_value)) {
        return ESP_ERR_INVALID_ARG;
    }
    return axp209_update_reg_bits(
        AXP209_REG_CHARGE_CTRL1,
        (uint8_t)(0x03u << 5),
        (uint8_t)(field_value << 5));
}

esp_err_t axp209_set_charge_end_percent(uint8_t percent)
{
    if (percent == 10u) {
        return axp209_update_reg_bits(AXP209_REG_CHARGE_CTRL1, (uint8_t)(1u << 4), 0u);
    }
    if (percent == 15u) {
        return axp209_update_reg_bits(
            AXP209_REG_CHARGE_CTRL1,
            (uint8_t)(1u << 4),
            (uint8_t)(1u << 4));
    }
    return ESP_ERR_INVALID_ARG;
}

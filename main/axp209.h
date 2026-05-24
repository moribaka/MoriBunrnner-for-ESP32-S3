#ifndef AXP209_H
#define AXP209_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AXP209_I2C_ADDR_DEFAULT 0x34

typedef struct {
    uint8_t power_status;
    uint8_t charge_status;
    uint32_t battery_voltage_mv;
    uint32_t acin_voltage_mv;
    uint32_t vbus_voltage_mv;
    uint32_t battery_charge_current_ma_x10;
    uint32_t battery_discharge_current_ma_x10;
    uint32_t ipsout_voltage_mv;
    int32_t internal_temp_deci_c;
} axp209_snapshot_t;

typedef struct {
    bool acin_present;
    bool acin_usable;
    bool vbus_present;
    bool vbus_usable;
    bool vbus_above_vhold;
    bool battery_charging;
    bool acin_vbus_short;
    bool boot_source_external;
    bool over_temperature;
    bool charging;
    bool battery_present;
    bool battery_activated;
    bool charge_current_limited;
} axp209_status_flags_t;

typedef struct {
    bool enabled;
    uint16_t target_voltage_mv;
    uint8_t end_current_percent;
    uint16_t charge_current_ma;
    uint16_t precharge_timeout_min;
    uint8_t constant_current_timeout_hours;
    bool chgled_blink_when_charging;
} axp209_charge_control_t;

esp_err_t axp209_probe(i2c_master_bus_handle_t bus, uint8_t address_7bit);
esp_err_t axp209_init(i2c_master_bus_handle_t bus, uint8_t address_7bit);
bool axp209_ready(void);
uint8_t axp209_address(void);

esp_err_t axp209_read_reg(uint8_t reg, uint8_t *value);
esp_err_t axp209_write_reg(uint8_t reg, uint8_t value);

esp_err_t axp209_acknowledge_irq(void);
esp_err_t axp209_read_snapshot(axp209_snapshot_t *snapshot);
esp_err_t axp209_read_charge_control(axp209_charge_control_t *control);
void axp209_decode_status(uint8_t power_status, uint8_t charge_status, axp209_status_flags_t *flags);

esp_err_t axp209_set_charge_enabled(bool enabled);
esp_err_t axp209_set_charge_current(uint16_t milliamps);
esp_err_t axp209_set_charge_target_voltage(uint16_t millivolts);
esp_err_t axp209_set_charge_end_percent(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif

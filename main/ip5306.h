#ifndef IP5306_H
#define IP5306_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IP5306_I2C_ADDR_DEFAULT 0x75

/* Common system control registers. */
#define IP5306_REG_SYS_CTL0 0x00
#define IP5306_REG_SYS_CTL1 0x01
#define IP5306_REG_SYS_CTL2 0x02
#define IP5306_REG_CHG_CTL0 0x20
#define IP5306_REG_CHG_CTL1 0x21
#define IP5306_REG_CHG_CTL2 0x22
#define IP5306_REG_CHG_CTL3 0x23

/* Common read-only telemetry registers. */
#define IP5306_REG_READ0 0x70
#define IP5306_REG_READ1 0x71
#define IP5306_REG_READ2 0x72
#define IP5306_REG_READ3 0x77
#define IP5306_REG_BAT_LEVEL 0x78
#define IP5306_REG_CHG_DIG_CTL0 0x24

/*
 * Bit definitions are based on IP5306-I2C register document v1.21.
 * Keep reserved bits unchanged: use read-modify-write on control registers.
 */
#define IP5306_SYS_CTL0_BOOST_EN_BIT 0x20
#define IP5306_SYS_CTL0_CHG_EN_BIT 0x10
#define IP5306_SYS_CTL0_INSERT_LOAD_BOOT_EN_BIT 0x04
#define IP5306_SYS_CTL0_BOOST_KEEP_ON_BIT 0x02
#define IP5306_SYS_CTL0_KEY_SHUTDOWN_EN_BIT 0x01

#define IP5306_SYS_CTL1_BOOST_OFF_BY_LONG_PRESS_BIT 0x80
#define IP5306_SYS_CTL1_WLED_TOGGLE_BY_DOUBLE_PRESS_BIT 0x40
#define IP5306_SYS_CTL1_SHORT_PRESS_BOOST_TOGGLE_BIT 0x20
#define IP5306_SYS_CTL1_KEEP_BOOST_ON_VIN_REMOVE_BIT 0x04
#define IP5306_SYS_CTL1_BATLOW_SHUTDOWN_EN_BIT 0x01

#define IP5306_SYS_CTL2_LIGHT_LOAD_SHUTDOWN_MASK 0x0C
#define IP5306_SYS_CTL2_LONG_PRESS_TIME_BIT 0x10

/* Compatibility aliases from common Arduino examples. */
#define IP5306_SYS_CTL1_BOOST_CTRL_SIGNAL_SELECTION_BIT IP5306_SYS_CTL1_BOOST_OFF_BY_LONG_PRESS_BIT
#define IP5306_SYS_CTL1_FLASHLIGHT_CTRL_SIGNAL_SELECTION_BIT \
    IP5306_SYS_CTL1_WLED_TOGGLE_BY_DOUBLE_PRESS_BIT
#define IP5306_SYS_CTL1_BOOST_AFTER_VIN_BIT IP5306_SYS_CTL1_KEEP_BOOST_ON_VIN_REMOVE_BIT

/* Charger control field definitions. */
#define IP5306_CHG_CTL0_CHARGING_FULL_STOP_VOLTAGE_MASK 0x03
#define IP5306_CHG_CTL1_CHARGE_UNDER_VOLTAGE_LOOP_MASK 0x1C
#define IP5306_CHG_CTL1_END_CHARGE_CURRENT_DETECTION_MASK 0xC0
#define IP5306_CHG_CTL2_VOLTAGE_PRESSURE_MASK 0x03
#define IP5306_CHG_CTL2_BATTERY_VOLTAGE_MASK 0x0C
#define IP5306_CHG_CTL3_CHARGE_CC_LOOP_BIT 0x20

/* Status bits from REG_READ0/1/2/3 (datasheet v1.21). */
#define IP5306_READ0_CHARGE_ENABLE_BIT 0x08
#define IP5306_READ1_CHARGE_FULL_BIT 0x08
#define IP5306_READ2_LIGHT_LOAD_BIT 0x04
#define IP5306_READ3_KEY_DOUBLE_PRESS_BIT 0x04
#define IP5306_READ3_KEY_LONG_PRESS_BIT 0x02
#define IP5306_READ3_KEY_SHORT_PRESS_BIT 0x01

typedef enum {
    IP5306_LIGHT_LOAD_SHUTDOWN_8S = 0,
    IP5306_LIGHT_LOAD_SHUTDOWN_32S = 1,
    IP5306_LIGHT_LOAD_SHUTDOWN_16S = 2,
    IP5306_LIGHT_LOAD_SHUTDOWN_64S = 3,
} ip5306_light_load_shutdown_t;

typedef enum {
    IP5306_LONG_PRESS_2S = 0,
    IP5306_LONG_PRESS_3S = 1,
} ip5306_long_press_time_t;

typedef enum {
    IP5306_CHG_FULL_STOP_VOLTAGE_0 = 0,
    IP5306_CHG_FULL_STOP_VOLTAGE_1 = 1,
    IP5306_CHG_FULL_STOP_VOLTAGE_2 = 2,
    IP5306_CHG_FULL_STOP_VOLTAGE_3 = 3,
} ip5306_chg_full_stop_voltage_t;

typedef enum {
    IP5306_END_CHARGE_CURRENT_200MA = 0,
    IP5306_END_CHARGE_CURRENT_400MA = 1,
    IP5306_END_CHARGE_CURRENT_500MA = 2,
    IP5306_END_CHARGE_CURRENT_600MA = 3,
} ip5306_end_charge_current_t;

typedef enum {
    IP5306_CHARGE_UNDER_VOLTAGE_4P45V = 0,
    IP5306_CHARGE_UNDER_VOLTAGE_4P50V = 1,
    IP5306_CHARGE_UNDER_VOLTAGE_4P55V = 2,
    IP5306_CHARGE_UNDER_VOLTAGE_4P60V = 3,
    IP5306_CHARGE_UNDER_VOLTAGE_4P65V = 4,
    IP5306_CHARGE_UNDER_VOLTAGE_4P70V = 5,
    IP5306_CHARGE_UNDER_VOLTAGE_4P75V = 6,
    IP5306_CHARGE_UNDER_VOLTAGE_4P80V = 7,
} ip5306_chg_under_voltage_loop_t;

typedef enum {
    IP5306_BATTERY_VOLTAGE_4P20V = 0,
    IP5306_BATTERY_VOLTAGE_4P30V = 1,
    IP5306_BATTERY_VOLTAGE_4P35V = 2,
    IP5306_BATTERY_VOLTAGE_4P40V = 3,
} ip5306_battery_voltage_t;

typedef enum {
    IP5306_VOLTAGE_PRESSURE_NONE = 0,
    IP5306_VOLTAGE_PRESSURE_14MV = 1,
    IP5306_VOLTAGE_PRESSURE_28MV = 2,
    IP5306_VOLTAGE_PRESSURE_42MV = 3,
} ip5306_voltage_pressure_t;

typedef struct {
    bool charge_enable;
    bool charge_full;
    bool light_load;
    bool key_short_press;
    bool key_long_press;
    bool key_double_press;
} ip5306_status_t;

esp_err_t ip5306_probe(i2c_master_bus_handle_t bus, uint8_t address_7bit);
esp_err_t ip5306_init(i2c_master_bus_handle_t bus, uint8_t address_7bit);
bool ip5306_ready(void);
uint8_t ip5306_address(void);

esp_err_t ip5306_read_reg(uint8_t reg, uint8_t *value);
esp_err_t ip5306_write_reg(uint8_t reg, uint8_t value);
esp_err_t ip5306_update_bits(uint8_t reg, uint8_t mask, bool set_bits);

esp_err_t ip5306_get_sys_ctrl(uint8_t *ctl0, uint8_t *ctl1, uint8_t *ctl2);
esp_err_t ip5306_set_boost_enable(bool enabled);
esp_err_t ip5306_set_charging_enable(bool enabled);
esp_err_t ip5306_set_boost_keep_on(bool enabled);
esp_err_t ip5306_set_insert_load_boot_enable(bool enabled);
esp_err_t ip5306_set_key_shutdown_enable(bool enabled);
esp_err_t ip5306_set_boost_off_by_long_press(bool enabled);
esp_err_t ip5306_set_wled_toggle_by_double_press(bool enabled);
esp_err_t ip5306_set_short_press_boost_toggle(bool enabled);
esp_err_t ip5306_set_keep_boost_on_vin_remove(bool enabled);
esp_err_t ip5306_set_batlow_shutdown_enable(bool enabled);
esp_err_t ip5306_set_light_load_shutdown_time(ip5306_light_load_shutdown_t setting);
esp_err_t ip5306_set_boost_ctrl_signal(bool enabled);
esp_err_t ip5306_set_flashlight_ctrl_signal(bool enabled);
esp_err_t ip5306_set_boost_after_vin(bool enabled);
esp_err_t ip5306_set_long_press_time(ip5306_long_press_time_t setting);
esp_err_t ip5306_set_charging_stop_voltage(ip5306_chg_full_stop_voltage_t setting);
esp_err_t ip5306_set_end_charge_current_detection(ip5306_end_charge_current_t setting);
esp_err_t ip5306_set_charge_under_voltage_loop(ip5306_chg_under_voltage_loop_t setting);
esp_err_t ip5306_set_battery_voltage(ip5306_battery_voltage_t setting);
esp_err_t ip5306_set_voltage_pressure(ip5306_voltage_pressure_t setting);
esp_err_t ip5306_set_charge_cc_loop_vin(bool use_vin_loop);
esp_err_t ip5306_get_light_load_shutdown_time(ip5306_light_load_shutdown_t *setting);
esp_err_t ip5306_get_status(ip5306_status_t *status);
esp_err_t ip5306_clear_key_event_flags(bool clear_short, bool clear_long, bool clear_double);
esp_err_t ip5306_get_battery_level_percent(uint8_t *percent);

#ifdef __cplusplus
}
#endif

#endif

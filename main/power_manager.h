#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "axp209.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "ip5306.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POWER_CHIP_NONE = 0,
    POWER_CHIP_IP5306,
    POWER_CHIP_AXP209,
} power_chip_type_t;

typedef struct {
    power_chip_type_t chip_type;
    bool ready;
    bool battery_percent_valid;
    uint8_t battery_percent;
    bool charging;
    bool charge_full;
    bool vbus_present;
    bool battery_present;
    uint16_t charge_current_limit_ma;
    uint32_t battery_voltage_mv;
    uint32_t vbus_voltage_mv;
    uint32_t ipsout_voltage_mv;
    int32_t internal_temp_deci_c;
    const char *chip_name;
    const char *charge_state;
    ip5306_status_t ip5306_status;
    axp209_snapshot_t axp209_snapshot;
    axp209_status_flags_t axp209_flags;
    axp209_charge_control_t axp209_charge_control;
    bool axp209_snapshot_ok;
    bool axp209_charge_control_ok;
} power_manager_telemetry_t;

esp_err_t power_manager_init(i2c_master_bus_handle_t bus);
esp_err_t power_manager_cpu_freq_init(void);
esp_err_t power_manager_perf_lock_acquire(const char *owner);
void power_manager_perf_lock_release(const char *owner);
power_chip_type_t power_manager_chip_type(void);
bool power_manager_ready(void);
const char *power_manager_chip_name(void);
esp_err_t power_manager_get_telemetry(power_manager_telemetry_t *telemetry);

#ifdef __cplusplus
}
#endif

#endif

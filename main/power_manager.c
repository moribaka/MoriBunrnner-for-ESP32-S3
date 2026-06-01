#include "power_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "power_manager";

static power_chip_type_t s_power_chip_type = POWER_CHIP_NONE;
static bool s_power_ready = false;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_perf_lock = NULL;
static SemaphoreHandle_t s_perf_lock_mutex = NULL;
static uint32_t s_perf_lock_refs = 0u;
#endif

static uint8_t power_manager_estimate_battery_percent_from_mv(uint32_t battery_mv)
{
    if (battery_mv <= 3300u) {
        return 0u;
    }
    if (battery_mv >= 4200u) {
        return 100u;
    }
    return (uint8_t)(((battery_mv - 3300u) * 100u) / 900u);
}

esp_err_t power_manager_cpu_freq_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err;

    err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CPU DFS configure failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_perf_lock == NULL) {
        err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "mori_perf", &s_perf_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CPU perf lock create failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (s_perf_lock_mutex == NULL) {
        s_perf_lock_mutex = xSemaphoreCreateMutex();
        if (s_perf_lock_mutex == NULL) {
            ESP_LOGW(TAG, "CPU perf lock mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "CPU DFS enabled: idle=80MHz max=160MHz light_sleep=off");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "CPU DFS disabled in sdkconfig; fixed CPU=%dMHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t power_manager_perf_lock_acquire(const char *owner)
{
#if CONFIG_PM_ENABLE
    esp_err_t err = ESP_OK;

    if (s_perf_lock == NULL) {
        err = power_manager_cpu_freq_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_perf_lock_mutex != NULL) {
        xSemaphoreTake(s_perf_lock_mutex, portMAX_DELAY);
    }
    if (s_perf_lock_refs == 0u) {
        err = esp_pm_lock_acquire(s_perf_lock);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "CPU perf lock acquired by %s", owner != NULL ? owner : "unknown");
        }
    }
    if (err == ESP_OK) {
        s_perf_lock_refs++;
    }
    if (s_perf_lock_mutex != NULL) {
        xSemaphoreGive(s_perf_lock_mutex);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CPU perf lock acquire failed for %s: %s",
            owner != NULL ? owner : "unknown",
            esp_err_to_name(err));
    }
    return err;
#else
    (void)owner;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void power_manager_perf_lock_release(const char *owner)
{
#if CONFIG_PM_ENABLE
    esp_err_t err = ESP_OK;

    if (s_perf_lock == NULL) {
        return;
    }
    if (s_perf_lock_mutex != NULL) {
        xSemaphoreTake(s_perf_lock_mutex, portMAX_DELAY);
    }
    if (s_perf_lock_refs > 0u) {
        s_perf_lock_refs--;
        if (s_perf_lock_refs == 0u) {
            err = esp_pm_lock_release(s_perf_lock);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "CPU perf lock released by %s", owner != NULL ? owner : "unknown");
            }
        }
    }
    if (s_perf_lock_mutex != NULL) {
        xSemaphoreGive(s_perf_lock_mutex);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CPU perf lock release failed for %s: %s",
            owner != NULL ? owner : "unknown",
            esp_err_to_name(err));
    }
#else
    (void)owner;
#endif
}

esp_err_t power_manager_init(i2c_master_bus_handle_t bus)
{
    esp_err_t err;

    s_power_chip_type = POWER_CHIP_NONE;
    s_power_ready = false;

    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = axp209_probe(bus, AXP209_I2C_ADDR_DEFAULT);
    if (err == ESP_OK) {
        err = axp209_init(bus, AXP209_I2C_ADDR_DEFAULT);
        if (err == ESP_OK) {
            s_power_chip_type = POWER_CHIP_AXP209;
            s_power_ready = true;
            ESP_LOGI(TAG, "detected AXP209 at 0x%02X", AXP209_I2C_ADDR_DEFAULT);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "AXP209 init failed: %s", esp_err_to_name(err));
    }

    err = ip5306_probe(bus, IP5306_I2C_ADDR_DEFAULT);
    if (err == ESP_OK) {
        err = ip5306_init(bus, IP5306_I2C_ADDR_DEFAULT);
        if (err == ESP_OK) {
            s_power_chip_type = POWER_CHIP_IP5306;
            s_power_ready = true;
            ESP_LOGI(TAG, "detected IP5306 at 0x%02X", IP5306_I2C_ADDR_DEFAULT);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "IP5306 init failed: %s", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "no supported power chip detected");
    return ESP_ERR_NOT_FOUND;
}

power_chip_type_t power_manager_chip_type(void)
{
    return s_power_chip_type;
}

bool power_manager_ready(void)
{
    return s_power_ready;
}

const char *power_manager_chip_name(void)
{
    switch (s_power_chip_type) {
        case POWER_CHIP_AXP209:
            return "AXP209";
        case POWER_CHIP_IP5306:
            return "IP5306";
        default:
            return "NONE";
    }
}

esp_err_t power_manager_get_telemetry(power_manager_telemetry_t *telemetry)
{
    esp_err_t err;

    if (telemetry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(telemetry, 0, sizeof(*telemetry));

    telemetry->chip_type = s_power_chip_type;
    telemetry->ready = s_power_ready;
    telemetry->chip_name = power_manager_chip_name();
    telemetry->charge_state = "unknown";

    if (!s_power_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_power_chip_type == POWER_CHIP_IP5306) {
        telemetry->charge_current_limit_ma = 450u;
        err = ip5306_get_battery_level_percent(&telemetry->battery_percent);
        telemetry->battery_percent_valid = (err == ESP_OK);
        err = ip5306_get_status(&telemetry->ip5306_status);
        if (err == ESP_OK) {
            telemetry->charging =
                telemetry->ip5306_status.charge_enable && !telemetry->ip5306_status.charge_full;
            telemetry->charge_full = telemetry->ip5306_status.charge_full;
            if (telemetry->ip5306_status.charge_enable) {
                telemetry->charge_state = telemetry->ip5306_status.charge_full ? "charge_full" : "charging";
            } else {
                telemetry->charge_state =
                    telemetry->ip5306_status.light_load ? "discharging_light_load" : "discharging";
            }
        }
        return ESP_OK;
    }

    if (s_power_chip_type == POWER_CHIP_AXP209) {
        err = axp209_read_snapshot(&telemetry->axp209_snapshot);
        if (err != ESP_OK) {
            return err;
        }
        telemetry->axp209_snapshot_ok = true;
        axp209_decode_status(
            telemetry->axp209_snapshot.power_status,
            telemetry->axp209_snapshot.charge_status,
            &telemetry->axp209_flags);
        telemetry->battery_voltage_mv = telemetry->axp209_snapshot.battery_voltage_mv;
        telemetry->vbus_voltage_mv = telemetry->axp209_snapshot.vbus_voltage_mv;
        telemetry->ipsout_voltage_mv = telemetry->axp209_snapshot.ipsout_voltage_mv;
        telemetry->internal_temp_deci_c = telemetry->axp209_snapshot.internal_temp_deci_c;
        telemetry->charging = telemetry->axp209_flags.charging;
        telemetry->charge_full = false;
        telemetry->vbus_present = telemetry->axp209_flags.vbus_present || telemetry->axp209_flags.acin_present;
        telemetry->battery_present = telemetry->axp209_flags.battery_present;
        telemetry->battery_percent = power_manager_estimate_battery_percent_from_mv(telemetry->battery_voltage_mv);
        telemetry->battery_percent_valid = telemetry->battery_present;
        telemetry->charge_state = telemetry->charging ? "charging" : "discharging";

        err = axp209_read_charge_control(&telemetry->axp209_charge_control);
        if (err == ESP_OK) {
            telemetry->axp209_charge_control_ok = true;
            telemetry->charge_current_limit_ma = telemetry->axp209_charge_control.charge_current_ma;
            if (!telemetry->axp209_charge_control.enabled && !telemetry->battery_present && telemetry->vbus_present) {
                telemetry->charge_state = "no_battery_external_power";
            }
        } else {
            telemetry->charge_current_limit_ma = 500u;
        }
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

#include "power_manager.h"
#include "esp_sleep.h"
#include "pin_map.h"
#include "battery_estimator.h"
#include "battery_learning.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "nvs.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "power_manager";

static power_chip_type_t s_power_chip_type = POWER_CHIP_NONE;
static bool s_power_ready = false;
static SemaphoreHandle_t s_telemetry_mutex = NULL;
static power_manager_telemetry_t s_telemetry_cache;
static battery_estimator_t s_battery_estimator;
static uint64_t s_telemetry_at_ms;
static bool s_telemetry_cached;
static bool s_telemetry_attempted;
static uint64_t s_telemetry_attempt_ms;
static esp_err_t s_telemetry_last_error;
static battery_learning_t s_battery_learning;
static bool s_learning_loaded;
static bool s_learning_storage_ok = true;
static uint64_t s_learning_saved_ms, s_learning_attempt_ms;

static void power_manager_load_battery_learning(void)
{
    if (s_learning_loaded) return;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("battery_learn", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) return;
    s_learning_loaded = true;
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) { s_learning_storage_ok = false; return; }
    battery_learning_profile_t profile;
    size_t size = sizeof(profile);
    err = nvs_get_blob(handle, "profile", &profile, &size);
    if (err == ESP_OK && size == sizeof(profile) && battery_learning_profile_valid(&profile)) {
        s_battery_learning.profile = profile;
        s_battery_learning.initialized = false;
        s_battery_learning.full_pending = s_battery_learning.low_pending = false;
        s_battery_learning.full_latched = s_battery_learning.low_latched = false;
        s_battery_learning.dirty = s_battery_learning.checkpoint = false;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "battery learning profile invalid; start fresh");
    }
    nvs_close(handle);
}

static void power_manager_save_battery_learning(uint64_t now_ms)
{
    if (!s_learning_loaded || !s_battery_learning.dirty) return;
    if (!s_battery_learning.checkpoint && now_ms - s_learning_saved_ms < 300000u) return;
    if (s_learning_attempt_ms && now_ms - s_learning_attempt_ms < 10000u) return;
    s_learning_attempt_ms = now_ms;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("battery_learn", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "profile", &s_battery_learning.profile, sizeof(s_battery_learning.profile));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    s_learning_storage_ok = err == ESP_OK;
    if (err == ESP_OK) {
        s_battery_learning.dirty = s_battery_learning.checkpoint = false;
        s_learning_saved_ms = now_ms;
    } else ESP_LOGW(TAG, "battery learning save failed: %s", esp_err_to_name(err));
}

static const char *power_manager_learning_state(bool counters_valid)
{
    const battery_learning_profile_t *profile = &s_battery_learning.profile;
    if (!counters_valid) return "counter_unavailable";
    if (profile->capacity_uah) {
        if (!profile->discharge_cycles) return "provisional";
        return profile->soc_trusted ? "learned" : "reanchor";
    }
    return profile->anchor_kind == 1u ? "learning_discharge" :
           profile->anchor_kind == 2u ? "learning_charge" : "waiting_reference";
}

#define POWER_SAMPLE_INTERVAL_MS 1000u
#define POWER_SAMPLE_STALE_LIMIT_MS 15000u

static portMUX_TYPE s_sleep_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_sleep_count;
static uint64_t s_sleep_time_us;

void power_manager_sleep_stats(uint32_t *count, uint64_t *time_us)
{
    portENTER_CRITICAL(&s_sleep_stats_lock);
    *count = s_sleep_count;
    *time_us = s_sleep_time_us;
    portEXIT_CRITICAL(&s_sleep_stats_lock);
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static esp_err_t IRAM_ATTR power_sleep_exit(int64_t time_us, void *arg)
{
    (void)arg;
    if (time_us > 0) {
        portENTER_CRITICAL(&s_sleep_stats_lock);
        ++s_sleep_count;
        s_sleep_time_us += time_us;
        portEXIT_CRITICAL(&s_sleep_stats_lock);
    }
    return ESP_OK;
}
#endif

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_perf_lock = NULL;
static esp_pm_lock_handle_t s_usb_awake_lock = NULL;
static bool s_usb_awake_held;
static esp_pm_lock_handle_t s_display_awake_lock = NULL;
static bool s_display_awake_held;
static SemaphoreHandle_t s_perf_lock_mutex = NULL;
static uint32_t s_perf_lock_refs = 0u;
#endif

static const char *power_manager_axp209_current_direction(const power_manager_telemetry_t *telemetry)
{
    if (telemetry == NULL) {
        return "unknown";
    }
    if (!telemetry->battery_present) {
        return telemetry->vbus_present ? "external" : "unknown";
    }
    if (telemetry->axp209_flags.battery_charging) {
        return "charge";
    }
    if (telemetry->vbus_present && telemetry->battery_discharge_current_ma_x10 < 10u) return "external";
    return "discharge";
}

static const char *power_manager_axp209_charge_mode(const power_manager_telemetry_t *telemetry)
{
    if (telemetry == NULL) {
        return "unknown";
    }
    if (!telemetry->battery_present) {
        return telemetry->vbus_present ? "external_only" : "no_battery";
    }
    if (telemetry->charge_full) return "charge_full";
    if (telemetry->charging) {
        return telemetry->axp209_flags.charge_current_limited ? "charging_limited" : "charging";
    }
    if (telemetry->axp209_charge_control_ok && !telemetry->axp209_charge_control.enabled) {
        return "charge_disabled";
    }
    if (telemetry->vbus_present) {
        return "charge_ready";
    }
    return "battery_only";
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
    if (s_usb_awake_lock == NULL) {
        err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "mori_usb", &s_usb_awake_lock);
        if (err != ESP_OK) return err;
        err = esp_pm_lock_acquire(s_usb_awake_lock);
        if (err != ESP_OK) return err;
        s_usb_awake_held = true;
    }
    if (s_display_awake_lock == NULL) {
        err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "mori_display", &s_display_awake_lock);
        if (err != ESP_OK) return err;
        err = esp_pm_lock_acquire(s_display_awake_lock);
        if (err != ESP_OK) return err;
        s_display_awake_held = true;
    }
    ESP_LOGI(TAG, "CPU DFS enabled: idle=80MHz max=160MHz light_sleep=off");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "CPU DFS disabled in sdkconfig; fixed CPU=%dMHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t power_manager_enable_idle_sleep(void)
{
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
    if (!s_display_awake_lock) return ESP_ERR_INVALID_STATE;
    /* BOOT 和扩展按键中断均可唤醒轻睡眠；硬件库仑计量保持运行。 */
    esp_err_t err = gpio_wakeup_enable(MORI_PIN_KEY_BOOT, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK) err = gpio_wakeup_enable(MORI_PIN_I2C_IRQ, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) return err;
    esp_pm_config_t config = {.max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true};
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    static bool registered;
    if (!registered) {
        esp_pm_sleep_cbs_register_config_t callbacks = {.exit_cb = power_sleep_exit};
        err = esp_pm_light_sleep_register_cbs(&callbacks);
        if (err != ESP_OK) return err;
        registered = true;
    }
#endif
    return esp_pm_configure(&config);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool power_manager_display_awake(void)
{
#if CONFIG_PM_ENABLE
    return s_display_awake_held;
#else
    return true;
#endif
}

esp_err_t power_manager_set_display_awake(bool awake)
{
#if CONFIG_PM_ENABLE
    if (!s_display_awake_lock || !s_perf_lock_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_perf_lock_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (awake != s_display_awake_held) {
        err = awake ? esp_pm_lock_acquire(s_display_awake_lock) : esp_pm_lock_release(s_display_awake_lock);
        if (err == ESP_OK) s_display_awake_held = awake;
    }
    xSemaphoreGive(s_perf_lock_mutex);
    return err;
#else
    (void)awake;
    return ESP_OK;
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
    if (s_telemetry_mutex == NULL) s_telemetry_mutex = xSemaphoreCreateMutex();
    if (s_telemetry_mutex == NULL) return ESP_ERR_NO_MEM;
    s_telemetry_cached = false;
    s_telemetry_attempted = false;
    memset(&s_battery_estimator, 0, sizeof(s_battery_estimator));
    battery_learning_reset(&s_battery_learning);
    s_battery_learning.dirty = s_battery_learning.checkpoint = false;
    s_learning_loaded = false;

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

static esp_err_t power_manager_read_telemetry(power_manager_telemetry_t *telemetry)
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
    telemetry->current_direction = "unknown";
    telemetry->charge_mode = "unknown";
    telemetry->battery_percent_source = "unknown";
    telemetry->calibration_state = "unsupported";

    if (!s_power_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_power_chip_type == POWER_CHIP_IP5306) {
        telemetry->charge_current_limit_ma = 450u;
        err = ip5306_get_battery_level_percent(&telemetry->battery_percent);
        telemetry->battery_percent_valid = (err == ESP_OK);
        telemetry->battery_percent_raw = telemetry->battery_percent;
        telemetry->battery_percent_source = "ip5306";
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
            telemetry->current_direction = telemetry->charging ? "charge" : "discharge";
            telemetry->charge_mode = telemetry->charge_state;
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
        telemetry->acin_voltage_mv = telemetry->axp209_snapshot.acin_voltage_mv;
        telemetry->vbus_voltage_mv = telemetry->axp209_snapshot.vbus_voltage_mv;
        telemetry->battery_charge_current_ma_x10 = telemetry->axp209_snapshot.battery_charge_current_ma_x10;
        telemetry->battery_discharge_current_ma_x10 =
            telemetry->axp209_snapshot.battery_discharge_current_ma_x10;
        telemetry->ipsout_voltage_mv = telemetry->axp209_snapshot.ipsout_voltage_mv;
        telemetry->internal_temp_deci_c = telemetry->axp209_snapshot.internal_temp_deci_c;
        telemetry->charging = telemetry->axp209_flags.charging && telemetry->axp209_flags.battery_present;
        telemetry->charge_full = false;
        telemetry->vbus_present = telemetry->axp209_flags.vbus_present || telemetry->axp209_flags.acin_present;
        telemetry->battery_present = telemetry->axp209_flags.battery_present;
        if (!telemetry->battery_present && telemetry->vbus_present) {
            telemetry->charge_state = "no_battery_external_power";
        } else {
            telemetry->charge_state = telemetry->charging ? "charging" : "discharging";
        }

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
        telemetry->charge_full = telemetry->battery_present &&
            (telemetry->axp209_flags.vbus_usable || telemetry->axp209_flags.acin_usable) &&
            !telemetry->axp209_flags.charging && !telemetry->axp209_flags.battery_charging &&
            !telemetry->axp209_flags.over_temperature && !telemetry->axp209_flags.battery_activated &&
            telemetry->axp209_snapshot.fuel_gauge_valid && telemetry->axp209_snapshot.fuel_gauge_percent == 100u &&
            telemetry->axp209_charge_control_ok && telemetry->axp209_charge_control.enabled;
        if (telemetry->charge_full) telemetry->charge_state = "charge_full";
        else if (telemetry->battery_present && telemetry->vbus_present && !telemetry->charging &&
                 telemetry->battery_discharge_current_ma_x10 < 10u) telemetry->charge_state = "charge_ready";
        uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000u;
        power_manager_load_battery_learning();
        const axp209_snapshot_t *snapshot = &telemetry->axp209_snapshot;
        battery_learning_sample_t learning_sample = {
            .now_ms = now_ms, .present = telemetry->battery_present,
            .external_power = telemetry->vbus_present, .charging = telemetry->charging, .full = telemetry->charge_full,
            .gauge_valid = snapshot->fuel_gauge_valid, .gauge_percent = snapshot->fuel_gauge_percent,
            .voltage_mv = telemetry->battery_voltage_mv,
            .charge_ma_x10 = snapshot->battery_charge_current_ma_x10,
            .discharge_ma_x10 = snapshot->battery_discharge_current_ma_x10,
            .target_mv = telemetry->axp209_charge_control_ok ? telemetry->axp209_charge_control.target_voltage_mv : 0u,
            .shutdown_mv = snapshot->shutdown_voltage_mv,
            .counters_valid = snapshot->coulomb_valid, .fresh_epoch = snapshot->coulomb_fresh_epoch,
            .sample_hz = snapshot->coulomb_sample_hz, .charge_raw = snapshot->coulomb_charge_raw,
            .discharge_raw = snapshot->coulomb_discharge_raw,
        };
        battery_learning_result_t learned = battery_learning_update(&s_battery_learning, &learning_sample);
        bool reference_valid = learned.percent_valid || snapshot->fuel_gauge_valid || learned.curve_estimate;
        uint8_t reference = learned.percent_valid ? learned.percent :
            snapshot->fuel_gauge_valid ? snapshot->fuel_gauge_percent : learned.percent;
        battery_percent_source_t reference_source = learned.percent_valid ? BATTERY_PERCENT_COULOMB :
            snapshot->fuel_gauge_valid ? BATTERY_PERCENT_FUEL_GAUGE : BATTERY_PERCENT_LEARNED_CURVE;
        battery_estimate_t estimate = battery_estimator_update(&s_battery_estimator,
            now_ms, telemetry->battery_present, telemetry->battery_voltage_mv,
            reference_valid, reference, telemetry->charging, telemetry->charge_full, reference_source);
        telemetry->battery_percent = estimate.percent;
        telemetry->battery_percent_raw = estimate.raw_percent;
        telemetry->battery_percent_valid = estimate.valid;
        telemetry->battery_filtered_voltage_mv = estimate.filtered_voltage_mv;
        telemetry->battery_percent_source = estimate.source == BATTERY_PERCENT_FUEL_GAUGE ? "fuel_gauge" :
            estimate.source == BATTERY_PERCENT_COULOMB ? "coulomb_learned" :
            estimate.source == BATTERY_PERCENT_LEARNED_CURVE ? "learned_curve" :
            estimate.source == BATTERY_PERCENT_VOLTAGE ? "voltage_estimate" : "unknown";
        power_manager_save_battery_learning(now_ms);
        telemetry->learned_capacity_mah = (s_battery_learning.profile.capacity_uah + 500u) / 1000u;
        telemetry->learning_cycles = s_battery_learning.profile.cycles;
        telemetry->learning_discharge_cycles = s_battery_learning.profile.discharge_cycles;
        telemetry->learning_observed_mah = learned.observed_uah / 1000u;
        telemetry->learning_curve_points = learned.curve_points;
        telemetry->calibration_state = power_manager_learning_state(snapshot->coulomb_valid);
        telemetry->calibration_storage_ok = s_learning_storage_ok;
        telemetry->current_direction = power_manager_axp209_current_direction(telemetry);
        telemetry->charge_mode = power_manager_axp209_charge_mode(telemetry);
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t power_manager_get_telemetry(power_manager_telemetry_t *telemetry)
{
    if (telemetry == NULL) return ESP_ERR_INVALID_ARG;
    memset(telemetry, 0, sizeof(*telemetry));
    if (!s_power_ready || s_telemetry_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000u;
    uint64_t age_ms = s_telemetry_cached ? now_ms - s_telemetry_at_ms : 0u;
    esp_err_t err = s_telemetry_last_error;
    if (!s_telemetry_attempted || now_ms - s_telemetry_attempt_ms >= POWER_SAMPLE_INTERVAL_MS) {
        power_manager_telemetry_t sample;
        err = power_manager_read_telemetry(&sample);
        s_telemetry_attempted = true;
        s_telemetry_attempt_ms = now_ms;
        s_telemetry_last_error = err;
        if (err == ESP_OK) {
            s_telemetry_cache = sample;
#if CONFIG_PM_ENABLE
            /* 外部供电时保留 USB 调试；纯电池才允许自动轻睡眠。 */
            if (s_usb_awake_lock && sample.vbus_present != s_usb_awake_held) {
                esp_err_t pm_err = sample.vbus_present ? esp_pm_lock_acquire(s_usb_awake_lock) : esp_pm_lock_release(s_usb_awake_lock);
                if (pm_err == ESP_OK) s_usb_awake_held = sample.vbus_present;
            }
#endif
            s_telemetry_at_ms = now_ms;
            s_telemetry_cached = true;
            age_ms = 0;
        }
    }
    /* 短暂读取失败保留最近结果，过期后明确返回不可用。 */
    if (s_telemetry_cached && age_ms <= POWER_SAMPLE_STALE_LIMIT_MS) {
        *telemetry = s_telemetry_cache;
        telemetry->sample_age_ms = (uint32_t)age_ms;
        telemetry->sample_stale = err != ESP_OK;
        err = ESP_OK;
    } else {
        memset(telemetry, 0, sizeof(*telemetry));
        if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_telemetry_mutex);
    return err;
}

esp_err_t power_manager_reset_battery_learning(void)
{
    if (!s_power_ready || s_power_chip_type != POWER_CHIP_AXP209 || s_telemetry_mutex == NULL) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("battery_learn", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, "profile");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        battery_learning_reset(&s_battery_learning);
        s_battery_learning.dirty = s_battery_learning.checkpoint = false;
        memset(&s_battery_estimator, 0, sizeof(s_battery_estimator));
        s_learning_loaded = true; s_learning_storage_ok = true;
        s_telemetry_cached = s_telemetry_attempted = false;
    }
    xSemaphoreGive(s_telemetry_mutex);
    return err;
}

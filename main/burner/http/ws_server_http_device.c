#include "ws_server_http_device.h"
#include "ws_server_http_content.h"
#include "ws_server_http_maintenance.h"
#include "power_manager.h"

#define BURNER_POWER_STATUS_RESP_LEN 6000U

esp_err_t burner_power_charge_current_handler(httpd_req_t *req)
{
    power_chip_type_t chip_type = power_manager_chip_type();

    if (chip_type == POWER_CHIP_AXP209) {
        httpd_resp_set_status(req, "403 Forbidden");
        return burner_send_json(
            req,
            "{\"ok\":false,\"message\":\"charge current is fixed at 500mA by firmware\"}");
    }

    httpd_resp_set_status(req, "403 Forbidden");
    return burner_send_json(
        req,
        "{\"ok\":false,\"message\":\"charge current is fixed at 450mA by firmware\"}");
}

esp_err_t burner_power_status_handler(httpd_req_t *req)
{
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    power_manager_telemetry_t power = {0};
    bool power_ok = (power_manager_get_telemetry(&power) == ESP_OK);
    const char *chip_type = "none";
    bool ip_ready = ip5306_ready();
    bool tca_ready = tca9555_ready();
    uint8_t ip_sys0 = 0;
    uint8_t ip_sys1 = 0;
    uint8_t ip_sys2 = 0;
    uint8_t ip_read0 = 0;
    uint8_t ip_read1 = 0;
    uint8_t ip_read2 = 0;
    uint8_t ip_read3 = 0;
    uint8_t ip_bat_level = 0;
    uint8_t ip_chg_dig0 = 0;
    bool ip_sys0_ok = false;
    bool ip_sys1_ok = false;
    bool ip_sys2_ok = false;
    bool ip_read0_ok = false;
    bool ip_read1_ok = false;
    bool ip_read2_ok = false;
    bool ip_read3_ok = false;
    bool ip_bat_level_ok = false;
    bool ip_chg_dig0_ok = false;
    bool boost_cfg = false;
    bool charging_cfg = false;
    bool boost_keep_on_cfg = false;
    bool key_shutdown_cfg = false;
    bool batlow_shutdown_cfg = false;
    bool wled_toggle_cfg = false;
    bool charge_enabled_flag = false;
    bool charge_full_flag = false;
    bool light_load_flag = false;
    bool key_double_press_flag = false;
    bool key_long_press_flag = false;
    bool key_short_press_flag = false;
    uint16_t charge_current_cfg_ma = 0;
    uint8_t battery_level_percent = 0;
    bool battery_level_percent_ok = false;
    bool battery_level_code_known = false;
    bool battery_absent_estimated = false;
    bool battery_present_estimated = true;
    const char *charge_state = "unknown";
    uint16_t tca_input = 0;
    uint16_t tca_output = 0;
    uint16_t tca_config = 0;
    bool tca_input_ok = false;
    bool tca_output_ok = false;
    bool tca_config_ok = false;
    char *resp = NULL;
    int n;
    esp_err_t send_err;

    web_ws_mark_activity();

    resp = (char *)heap_caps_malloc(BURNER_POWER_STATUS_RESP_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resp == NULL) {
        resp = (char *)heap_caps_malloc(BURNER_POWER_STATUS_RESP_LEN, MALLOC_CAP_8BIT);
    }
    if (resp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    if (power.chip_type == POWER_CHIP_AXP209) {
        chip_type = "axp209";
    } else if (power.chip_type == POWER_CHIP_IP5306) {
        chip_type = "ip5306";
    }

    if (ip_ready) {
        ip_sys0_ok = (ip5306_read_reg(IP5306_REG_SYS_CTL0, &ip_sys0) == ESP_OK);
        ip_sys1_ok = (ip5306_read_reg(IP5306_REG_SYS_CTL1, &ip_sys1) == ESP_OK);
        ip_sys2_ok = (ip5306_read_reg(IP5306_REG_SYS_CTL2, &ip_sys2) == ESP_OK);
        ip_read0_ok = (ip5306_read_reg(IP5306_REG_READ0, &ip_read0) == ESP_OK);
        ip_read1_ok = (ip5306_read_reg(IP5306_REG_READ1, &ip_read1) == ESP_OK);
        ip_read2_ok = (ip5306_read_reg(IP5306_REG_READ2, &ip_read2) == ESP_OK);
        ip_read3_ok = (ip5306_read_reg(IP5306_REG_READ3, &ip_read3) == ESP_OK);
        ip_bat_level_ok = (ip5306_read_reg(IP5306_REG_BAT_LEVEL, &ip_bat_level) == ESP_OK);
        ip_chg_dig0_ok = (ip5306_read_reg(IP5306_REG_CHG_DIG_CTL0, &ip_chg_dig0) == ESP_OK);
        battery_level_percent_ok = (ip5306_get_battery_level_percent(&battery_level_percent) == ESP_OK);
        if (ip_bat_level_ok) {
            battery_level_code_known = burner_ip5306_battery_level_code_known(ip_bat_level);
        }
        /*
         * If BAT_LEVEL code is unknown, treat battery percent as invalid.
         * This allows UI/logic to classify "no battery / voltage undetectable".
         */
        battery_level_percent_ok = battery_level_percent_ok && battery_level_code_known;

        if (ip_sys0_ok) {
            boost_cfg = (ip_sys0 & IP5306_SYS_CTL0_BOOST_EN_BIT) != 0;
            charging_cfg = (ip_sys0 & IP5306_SYS_CTL0_CHG_EN_BIT) != 0;
            boost_keep_on_cfg = (ip_sys0 & IP5306_SYS_CTL0_BOOST_KEEP_ON_BIT) != 0;
            key_shutdown_cfg = (ip_sys0 & IP5306_SYS_CTL0_KEY_SHUTDOWN_EN_BIT) != 0;
        }
        if (ip_sys1_ok) {
            batlow_shutdown_cfg = (ip_sys1 & IP5306_SYS_CTL1_BATLOW_SHUTDOWN_EN_BIT) != 0;
            wled_toggle_cfg = (ip_sys1 & IP5306_SYS_CTL1_WLED_TOGGLE_BY_DOUBLE_PRESS_BIT) != 0;
        }

        if (ip_read0_ok) {
            charge_enabled_flag = (ip_read0 & IP5306_READ0_CHARGE_ENABLE_BIT) != 0;
        }
        if (ip_read1_ok) {
            charge_full_flag = (ip_read1 & IP5306_READ1_CHARGE_FULL_BIT) != 0;
        }
        if (ip_read2_ok) {
            light_load_flag = (ip_read2 & IP5306_READ2_LIGHT_LOAD_BIT) != 0;
        }
        if (ip_read3_ok) {
            key_double_press_flag = (ip_read3 & IP5306_READ3_KEY_DOUBLE_PRESS_BIT) != 0;
            key_long_press_flag = (ip_read3 & IP5306_READ3_KEY_LONG_PRESS_BIT) != 0;
            key_short_press_flag = (ip_read3 & IP5306_READ3_KEY_SHORT_PRESS_BIT) != 0;
        }
        if (ip_chg_dig0_ok) {
            charge_current_cfg_ma = burner_ip5306_charge_current_cfg_ma(ip_chg_dig0);
        }

        /*
         * "No battery, external power only" heuristic:
         * - BAT_LEVEL nibble is not one of known battery level encodings
         * - not charging and not charge-full
         */
        if (ip_read0_ok && ip_read1_ok &&
            (!battery_level_percent_ok || !ip_bat_level_ok) &&
            !charge_enabled_flag && !charge_full_flag) {
            battery_absent_estimated = true;
            battery_present_estimated = false;
            charge_state = "no_battery_external_power";
        } else if (ip_read0_ok && ip_read1_ok && ip_read2_ok) {
            if (charge_enabled_flag) {
                charge_state = charge_full_flag ? "charge_full" : "charging";
            } else {
                charge_state = light_load_flag ? "discharging_light_load" : "discharging";
            }
        }
    }

    if (tca_ready) {
        tca_input_ok = (tca9555_read_inputs(&tca_input) == ESP_OK);
        tca_output_ok = (tca9555_read_outputs(&tca_output) == ESP_OK);
        tca_config_ok = (tca9555_read_config(&tca_config) == ESP_OK);
    }

    n = snprintf(
        resp,
        BURNER_POWER_STATUS_RESP_LEN,
        "{\"ok\":true,"
        "\"note\":\"Power values come from PMIC telemetry when available; unsupported items remain 0.\","
        "\"uptime_ms\":%" PRIu64 ",\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 ","
        "\"power\":{\"ready\":%s,\"chip_type\":\"%s\",\"chip_name\":\"%s\","
        "\"battery_percent_valid\":%s,\"battery_percent\":%u,"
        "\"charging\":%s,\"charge_full\":%s,\"vbus_present\":%s,\"battery_present\":%s,"
        "\"charge_current_limit_ma\":%u,\"battery_voltage_mv\":%" PRIu32 ",\"acin_voltage_mv\":%" PRIu32
        ",\"vbus_voltage_mv\":%" PRIu32 ",\"battery_charge_current_ma_x10\":%" PRIu32
        ",\"battery_discharge_current_ma_x10\":%" PRIu32 ",\"ipsout_voltage_mv\":%" PRIu32
        ",\"internal_temp_deci_c\":%" PRIi32 ",\"charge_state\":\"%s\",\"current_direction\":\"%s\""
        ",\"charge_mode\":\"%s\"},"
        "\"axp209\":{\"ready\":%s,\"addr\":\"0x%02X\","
        "\"snapshot_ok\":%s,\"charge_control_ok\":%s,"
        "\"power_status\":\"0x%02X\",\"charge_status\":\"0x%02X\","
        "\"battery_voltage_mv\":%" PRIu32 ",\"acin_voltage_mv\":%" PRIu32 ",\"vbus_voltage_mv\":%" PRIu32
        ",\"battery_charge_current_ma_x10\":%" PRIu32 ",\"battery_discharge_current_ma_x10\":%" PRIu32
        ",\"ipsout_voltage_mv\":%" PRIu32 ",\"internal_temp_deci_c\":%" PRIi32 ","
        "\"acin_present\":%s,\"acin_usable\":%s,\"vbus_present\":%s,\"vbus_usable\":%s,"
        "\"battery_present\":%s,\"battery_activated\":%s,\"charging\":%s,"
        "\"charge_current_limited\":%s,\"charge_enabled_cfg\":%s,\"charge_current_cfg_ma\":%u,"
        "\"target_voltage_cfg_mv\":%u,\"end_current_percent_cfg\":%u},"
        "\"ip5306\":{\"ready\":%s,\"addr\":\"0x%02X\","
        "\"sys_ctl0_ok\":%s,\"sys_ctl0\":\"0x%02X\","
        "\"sys_ctl1_ok\":%s,\"sys_ctl1\":\"0x%02X\","
        "\"sys_ctl2_ok\":%s,\"sys_ctl2\":\"0x%02X\","
        "\"read0_ok\":%s,\"read0\":\"0x%02X\","
        "\"read1_ok\":%s,\"read1\":\"0x%02X\","
        "\"read2_ok\":%s,\"read2\":\"0x%02X\","
        "\"read3_ok\":%s,\"read3\":\"0x%02X\","
        "\"bat_level_ok\":%s,\"bat_level\":\"0x%02X\","
        "\"chg_dig_ctl0_ok\":%s,\"chg_dig_ctl0\":\"0x%02X\","
        "\"battery_level_percent_ok\":%s,\"battery_level_percent\":%u,"
        "\"battery_level_code_known\":%s,"
        "\"battery_present_estimated\":%s,\"battery_absent_estimated\":%s,"
        "\"charge_state\":\"%s\","
        "\"charge_enabled_flag\":%s,"
        "\"charge_full_flag\":%s,"
        "\"light_load_flag\":%s,"
        "\"key_double_press_flag\":%s,"
        "\"key_long_press_flag\":%s,"
        "\"key_short_press_flag\":%s,"
        "\"charge_current_cfg_ma\":%u,"
        "\"charge_current_formula\":\"I=0.05+b0*0.1+b1*0.2+b2*0.4+b3*0.8+b4*1.6 A\","
        "\"boost_enable_cfg\":%s,\"charging_enable_cfg\":%s,\"boost_keep_on_cfg\":%s,"
        "\"key_shutdown_enable_cfg\":%s,\"low_power_shutdown_cfg\":%s,\"wled_toggle_cfg\":%s,"
        "\"boost_enable\":%s,\"key_enable\":%s,\"low_power_shutdown\":%s,\"led_enable\":%s},"
        "\"tca9555\":{\"ready\":%s,\"addr\":\"0x%02X\","
        "\"input_ok\":%s,\"input\":\"0x%04X\","
        "\"output_ok\":%s,\"output\":\"0x%04X\","
        "\"config_ok\":%s,\"config\":\"0x%04X\"}}",
        uptime_ms,
        free_heap,
        min_heap,
        burner_json_bool(power_ok),
        chip_type,
        power.chip_name != NULL ? power.chip_name : "NONE",
        burner_json_bool(power.battery_percent_valid),
        power.battery_percent,
        burner_json_bool(power.charging),
        burner_json_bool(power.charge_full),
        burner_json_bool(power.vbus_present),
        burner_json_bool(power.battery_present),
        power.charge_current_limit_ma,
        power.battery_voltage_mv,
        power.acin_voltage_mv,
        power.vbus_voltage_mv,
        power.battery_charge_current_ma_x10,
        power.battery_discharge_current_ma_x10,
        power.ipsout_voltage_mv,
        power.internal_temp_deci_c,
        power.charge_state != NULL ? power.charge_state : "unknown",
        power.current_direction != NULL ? power.current_direction : "unknown",
        power.charge_mode != NULL ? power.charge_mode : "unknown",
        burner_json_bool(axp209_ready()),
        axp209_address(),
        burner_json_bool(power.axp209_snapshot_ok),
        burner_json_bool(power.axp209_charge_control_ok),
        power.axp209_snapshot.power_status,
        power.axp209_snapshot.charge_status,
        power.axp209_snapshot.battery_voltage_mv,
        power.axp209_snapshot.acin_voltage_mv,
        power.axp209_snapshot.vbus_voltage_mv,
        power.axp209_snapshot.battery_charge_current_ma_x10,
        power.axp209_snapshot.battery_discharge_current_ma_x10,
        power.axp209_snapshot.ipsout_voltage_mv,
        power.axp209_snapshot.internal_temp_deci_c,
        burner_json_bool(power.axp209_flags.acin_present),
        burner_json_bool(power.axp209_flags.acin_usable),
        burner_json_bool(power.axp209_flags.vbus_present),
        burner_json_bool(power.axp209_flags.vbus_usable),
        burner_json_bool(power.axp209_flags.battery_present),
        burner_json_bool(power.axp209_flags.battery_activated),
        burner_json_bool(power.axp209_flags.charging),
        burner_json_bool(power.axp209_flags.charge_current_limited),
        burner_json_bool(power.axp209_charge_control.enabled),
        power.axp209_charge_control.charge_current_ma,
        power.axp209_charge_control.target_voltage_mv,
        (unsigned)power.axp209_charge_control.end_current_percent,
        burner_json_bool(ip_ready),
        ip5306_address(),
        burner_json_bool(ip_sys0_ok),
        ip_sys0,
        burner_json_bool(ip_sys1_ok),
        ip_sys1,
        burner_json_bool(ip_sys2_ok),
        ip_sys2,
        burner_json_bool(ip_read0_ok),
        ip_read0,
        burner_json_bool(ip_read1_ok),
        ip_read1,
        burner_json_bool(ip_read2_ok),
        ip_read2,
        burner_json_bool(ip_read3_ok),
        ip_read3,
        burner_json_bool(ip_bat_level_ok),
        ip_bat_level,
        burner_json_bool(ip_chg_dig0_ok),
        ip_chg_dig0,
        burner_json_bool(battery_level_percent_ok),
        battery_level_percent,
        burner_json_bool(battery_level_code_known),
        burner_json_bool(battery_present_estimated),
        burner_json_bool(battery_absent_estimated),
        charge_state,
        burner_json_bool(charge_enabled_flag),
        burner_json_bool(charge_full_flag),
        burner_json_bool(light_load_flag),
        burner_json_bool(key_double_press_flag),
        burner_json_bool(key_long_press_flag),
        burner_json_bool(key_short_press_flag),
        charge_current_cfg_ma,
        burner_json_bool(boost_cfg),
        burner_json_bool(charging_cfg),
        burner_json_bool(boost_keep_on_cfg),
        burner_json_bool(key_shutdown_cfg),
        burner_json_bool(batlow_shutdown_cfg),
        burner_json_bool(wled_toggle_cfg),
        burner_json_bool(boost_cfg),
        burner_json_bool(key_shutdown_cfg),
        burner_json_bool(batlow_shutdown_cfg),
        burner_json_bool(wled_toggle_cfg),
        burner_json_bool(tca_ready),
        tca9555_address(),
        burner_json_bool(tca_input_ok),
        tca_input,
        burner_json_bool(tca_output_ok),
        tca_output,
        burner_json_bool(tca_config_ok),
        tca_config);
    if (n < 0 || n >= (int)BURNER_POWER_STATUS_RESP_LEN) {
        heap_caps_free(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    send_err = httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    heap_caps_free(resp);
    return send_err;
}

static volatile uint32_t s_cpu_tick_total[portNUM_PROCESSORS];
static volatile uint32_t s_cpu_tick_idle[portNUM_PROCESSORS];
static uint32_t s_cpu_last_total[portNUM_PROCESSORS];
static uint32_t s_cpu_last_idle[portNUM_PROCESSORS];
static uint8_t s_cpu_usage_percent;
static bool s_cpu_usage_valid;
static bool s_cpu_monitor_ready;
static bool s_cpu_monitor_init_in_progress;
static portMUX_TYPE s_cpu_monitor_lock = portMUX_INITIALIZER_UNLOCKED;

static void burner_cpu_tick_hook(void)
{
    BaseType_t core_id = xPortGetCoreID();

    if (core_id < 0 || core_id >= portNUM_PROCESSORS) {
        return;
    }

    s_cpu_tick_total[core_id]++;
    if (xTaskGetCurrentTaskHandleForCore(core_id) == xTaskGetIdleTaskHandleForCore(core_id)) {
        s_cpu_tick_idle[core_id]++;
    }
}

static void burner_cpu_monitor_ensure_ready(void)
{
    bool need_init = false;
    bool registered[portNUM_PROCESSORS] = {0};
    esp_err_t err = ESP_OK;

    portENTER_CRITICAL(&s_cpu_monitor_lock);
    if (!s_cpu_monitor_ready && !s_cpu_monitor_init_in_progress) {
        s_cpu_monitor_init_in_progress = true;
        need_init = true;
    }
    portEXIT_CRITICAL(&s_cpu_monitor_lock);

    if (!need_init) {
        return;
    }

    for (UBaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
        err = esp_register_freertos_tick_hook_for_cpu(burner_cpu_tick_hook, core);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "register cpu tick hook failed on core %" PRIu32 ": %s",
                     (uint32_t)core, esp_err_to_name(err));
            break;
        }
        registered[core] = true;
    }

    if (err != ESP_OK) {
        for (UBaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
            if (registered[core]) {
                esp_deregister_freertos_tick_hook_for_cpu(burner_cpu_tick_hook, core);
            }
        }
    }

    portENTER_CRITICAL(&s_cpu_monitor_lock);
    s_cpu_monitor_ready = (err == ESP_OK);
    s_cpu_monitor_init_in_progress = false;
    portEXIT_CRITICAL(&s_cpu_monitor_lock);
}

static bool burner_cpu_monitor_update(uint8_t *out_usage_percent)
{
    uint64_t total_delta_sum = 0;
    uint64_t idle_delta_sum = 0;
    bool has_delta = false;

    burner_cpu_monitor_ensure_ready();
    if (!s_cpu_monitor_ready) {
        return false;
    }

    for (UBaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
        uint32_t total_now = s_cpu_tick_total[core];
        uint32_t idle_now = s_cpu_tick_idle[core];
        uint32_t total_delta = total_now - s_cpu_last_total[core];
        uint32_t idle_delta = idle_now - s_cpu_last_idle[core];

        s_cpu_last_total[core] = total_now;
        s_cpu_last_idle[core] = idle_now;
        total_delta_sum += total_delta;
        idle_delta_sum += idle_delta;
        if (total_delta > 0) {
            has_delta = true;
        }
    }

    if (has_delta && total_delta_sum > 0) {
        if (idle_delta_sum > total_delta_sum) {
            idle_delta_sum = total_delta_sum;
        }
        s_cpu_usage_percent = (uint8_t)(100U - (uint32_t)((idle_delta_sum * 100U) / total_delta_sum));
        s_cpu_usage_valid = true;
    }

    if (!s_cpu_usage_valid) {
        return false;
    }
    if (out_usage_percent != NULL) {
        *out_usage_percent = s_cpu_usage_percent;
    }
    return true;
}

static bool burner_storage_get_tf_capacity(uint64_t *out_total_bytes,
                                           uint64_t *out_used_bytes,
                                           uint64_t *out_free_bytes)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    if (out_total_bytes != NULL) {
        *out_total_bytes = 0;
    }
    if (out_used_bytes != NULL) {
        *out_used_bytes = 0;
    }
    if (out_free_bytes != NULL) {
        *out_free_bytes = 0;
    }

    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        return false;
    }

    err = esp_vfs_fat_info(mount_point, &total_bytes, &free_bytes);
    if (err != ESP_OK || free_bytes > total_bytes) {
        return false;
    }

    if (out_total_bytes != NULL) {
        *out_total_bytes = total_bytes;
    }
    if (out_used_bytes != NULL) {
        *out_used_bytes = total_bytes - free_bytes;
    }
    if (out_free_bytes != NULL) {
        *out_free_bytes = free_bytes;
    }
    return true;
}

esp_err_t burner_device_info_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    esp_chip_info_t chip_info;
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    uint8_t cpu_usage_percent = 0;
    char cpu_usage_text[32] = "sampling / 采样中";
    char resp[896];

    web_ws_mark_activity();

    esp_chip_info(&chip_info);
    if (burner_cpu_monitor_update(&cpu_usage_percent)) {
        snprintf(cpu_usage_text, sizeof(cpu_usage_text), "%u%%", (unsigned int)cpu_usage_percent);
    }
    snprintf(
        resp,
        sizeof(resp),
        "Project name / 项目名: %s\n"
        "App version / 固件版本: %s\n"
        "Build time / 构建时间: %s %s\n"
        "IDF version / IDF版本: %s\n"
        "Chip cores / 核心数: %d\n"
        "Chip revision / 芯片修订: v%d.%d\n"
        "Features / 特性: %s%s%s\n"
        "Current free heap / 当前空闲内存: %" PRIu32 " bytes\n"
        "Minimum free heap / 最小空闲内存: %" PRIu32 " bytes\n"
        "CPU usage / CPU占用: %s\n",
        app_desc ? app_desc->project_name : "unknown",
        app_desc ? app_desc->version : "unknown",
        app_desc ? app_desc->date : "unknown",
        app_desc ? app_desc->time : "unknown",
        app_desc ? app_desc->idf_ver : "unknown",
        chip_info.cores,
        chip_info.revision / 100,
        chip_info.revision % 100,
        (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
        (chip_info.features & CHIP_FEATURE_BLE) ? "BLE " : "",
        (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
        free_heap,
        min_heap,
        cpu_usage_text);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_device_restart_handler(httpd_req_t *req)
{
    (void)req;
    burner_schedule_restart();
    return burner_send_json(req, "{\"ok\":true,\"rebooting\":true}");
}

esp_err_t burner_device_brightness_get_handler(httpd_req_t *req)
{
    char resp[96];
    int n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"brightness\":%u,\"min\":0,\"max\":255}",
        (unsigned int)lcd_display_get_brightness());
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_device_brightness_post_handler(httpd_req_t *req)
{
    char body[POWER_JSON_BODY_MAX] = {0};
    char resp[96];
    int brightness = 0;
    esp_err_t err;
    int n;

    err = burner_read_request_body(req, body, sizeof(body), NULL);
    if (err != ESP_OK) {
        return burner_send_json(req, "{\"ok\":false,\"message\":\"invalid request body\"}");
    }

    if (!burner_json_get_int(body, "brightness", &brightness)) {
        return burner_send_json(req, "{\"ok\":false,\"message\":\"brightness is required\"}");
    }
    if (brightness < 0 || brightness > 255) {
        return burner_send_json(req, "{\"ok\":false,\"message\":\"brightness out of range (0..255)\"}");
    }

    err = lcd_display_set_brightness((uint8_t)brightness);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "set brightness failed: %s", esp_err_to_name(err));
        return burner_send_json(req, "{\"ok\":false,\"message\":\"set brightness failed\"}");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"brightness\":%u}",
        (unsigned int)lcd_display_get_brightness());
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_storage_status_handler(httpd_req_t *req)
{
    uint64_t tf_total_bytes = 0;
    uint64_t tf_used_bytes = 0;
    uint64_t tf_free_bytes = 0;
    bool tf_capacity_ok = burner_storage_get_tf_capacity(&tf_total_bytes, &tf_used_bytes, &tf_free_bytes);
    char resp[384];
    int n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"tf_ready\":%s,\"usb_msc_ready\":%s,"
        "\"usb_passthrough_enabled\":%s,\"tf_busy\":%s,"
        "\"tf_capacity_ok\":%s,\"tf_total_bytes\":%" PRIu64 ","
        "\"tf_used_bytes\":%" PRIu64 ",\"tf_free_bytes\":%" PRIu64 "}",
        (card != NULL) ? "true" : "false",
        usb_msc_tf_ready() ? "true" : "false",
        usb_msc_tf_enabled() ? "true" : "false",
        usb_msc_tf_in_use_by_host() ? "true" : "false",
        tf_capacity_ok ? "true" : "false",
        tf_total_bytes,
        tf_used_bytes,
        tf_free_bytes);

    web_ws_mark_activity();

    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_storage_usb_msc_handler(httpd_req_t *req)
{
    char enable_arg[24] = {0};
    bool enable = false;
    esp_err_t err;

    if (!burner_get_query_arg(req, "enable", enable_arg, sizeof(enable_arg), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enable query");
    }
    if (!burner_parse_bool_text(enable_arg, &enable)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid enable value");
    }
    if (enable && card == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "TF card not ready");
    }
    if (!usb_msc_tf_ready()) {
        if (enable) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "USB MSC manager not ready");
        }
        return burner_storage_status_handler(req);
    }

    err = usb_msc_tf_set_enabled(enable);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    return burner_storage_status_handler(req);
}

esp_err_t burner_lang_handler(httpd_req_t *req)
{
    typedef struct {
        const char *key;
        const char *value;
    } burner_lang_item_t;

    burner_lang_pack_t *lang = NULL;
    burner_lang_meta_t meta = {0};
    bool system_loaded = false;
    bool lang_loaded = false;
    char esc_version[WEB_LANG_VERSION_MAX * 2 + 16] = {0};
    char esc_ini[WEB_LANG_FILE_NAME_MAX * 2 + 16] = {0};
    char header[320] = {0};
    int n = 0;
    esp_err_t err = ESP_OK;

    const burner_lang_item_t items[] = {
        {"page_title", NULL},
        {"page_header", NULL},
        {"page_tip", NULL},
        {"business_title", NULL},
        {"btn_open_business", NULL},
        {"business_tip", NULL},
        {"recovery_title", NULL},
        {"recovery_tip", NULL},
        {"btn_upload_main", NULL},
        {"system_migrate_title", NULL},
        {"system_migrate_tip", NULL},
        {"btn_system_migrate", NULL},
        {"system_deploy_title", NULL},
        {"system_deploy_tip", NULL},
        {"btn_system_deploy", NULL},
        {"firmware_title", NULL},
        {"firmware_tip", NULL},
        {"btn_upload_firmware", NULL},
        {"firmware_idle", NULL},
        {"upload_idle", NULL},
        {"usb_title", NULL},
        {"usb_tip", NULL},
        {"btn_enable_usb", NULL},
        {"btn_disable_usb", NULL},
        {"btn_refresh_storage", NULL},
        {"storage_loading", NULL},
        {"device_title", NULL},
        {"btn_refresh_device", NULL},
        {"device_loading", NULL},
        {"power_title", NULL},
        {"btn_refresh_power", NULL},
        {"power_loading", NULL},
        {"msg_select_main", NULL},
        {"msg_select_deploy_zip", NULL},
        {"msg_select_firmware", NULL},
        {"msg_deploying_prefix", NULL},
        {"msg_deploy_success_prefix", NULL},
        {"msg_deploy_failed_prefix", NULL},
        {"msg_uploading_firmware_prefix", NULL},
        {"msg_firmware_success_prefix", NULL},
        {"msg_uploading_prefix", NULL},
        {"msg_upload_success_prefix", NULL},
        {"msg_upload_failed_prefix", NULL},
        {"msg_storage_status_error_prefix", NULL},
        {"msg_set_mode_error_prefix", NULL},
        {"msg_device_info_error_prefix", NULL},
        {"msg_power_status_error_prefix", NULL},
        {"msg_applying", NULL},
        {"language_title", NULL},
        {"language_tip", NULL},
        {"btn_read_lang_list", NULL},
        {"btn_apply_language", NULL},
        {"language_idle", NULL},
        {"language_loading", NULL},
        {"language_none", NULL},
        {"ip5306_title", NULL},
        {"ip5306_tip", NULL},
        {"btn_read_ip5306_ini", NULL},
        {"btn_save_ip5306_ini", NULL},
        {"ip5306_idle", NULL},
        {"ip5306_loading", NULL},
        {"msg_lang_select_required", NULL},
        {"msg_lang_list_error_prefix", NULL},
        {"msg_lang_apply_success_prefix", NULL},
        {"msg_lang_apply_error_prefix", NULL},
        {"msg_ip5306_load_ok", NULL},
        {"msg_ip5306_load_error_prefix", NULL},
        {"msg_ip5306_save_ok_prefix", NULL},
        {"msg_ip5306_save_error_prefix", NULL},
        {"msg_upload_item_ok", NULL},
        {"msg_upload_item_fail", NULL},
        {"msg_http_error_prefix", NULL},
        {"msg_invalid_json_prefix", NULL},
    };
    const char *values[sizeof(items) / sizeof(items[0])] = {0};

    lang = (burner_lang_pack_t *)calloc(1, sizeof(*lang));
    if (lang == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    burner_lang_load(lang, &meta, &system_loaded, &lang_loaded);

    if (!burner_json_escape(meta.language_version, esc_version, sizeof(esc_version)) ||
        !burner_json_escape(meta.language_ini, esc_ini, sizeof(esc_ini))) {
        free(lang);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    {
        size_t vi = 0;
#define BURNER_LANG_PUSH_VALUE(v) values[vi++] = (v)
        BURNER_LANG_PUSH_VALUE(lang->page_title);
        BURNER_LANG_PUSH_VALUE(lang->page_header);
        BURNER_LANG_PUSH_VALUE(lang->page_tip);
        BURNER_LANG_PUSH_VALUE(lang->business_title);
        BURNER_LANG_PUSH_VALUE(lang->btn_open_business);
        BURNER_LANG_PUSH_VALUE(lang->business_tip);
        BURNER_LANG_PUSH_VALUE(lang->recovery_title);
        BURNER_LANG_PUSH_VALUE(lang->recovery_tip);
        BURNER_LANG_PUSH_VALUE(lang->btn_upload_main);
        BURNER_LANG_PUSH_VALUE(lang->system_migrate_title);
        BURNER_LANG_PUSH_VALUE(lang->system_migrate_tip);
        BURNER_LANG_PUSH_VALUE(lang->btn_system_migrate);
        BURNER_LANG_PUSH_VALUE(lang->system_deploy_title);
        BURNER_LANG_PUSH_VALUE(lang->system_deploy_tip);
        BURNER_LANG_PUSH_VALUE(lang->btn_system_deploy);
        BURNER_LANG_PUSH_VALUE(lang->firmware_title);
        BURNER_LANG_PUSH_VALUE(lang->firmware_tip);
        BURNER_LANG_PUSH_VALUE(lang->btn_upload_firmware);
        BURNER_LANG_PUSH_VALUE(lang->firmware_idle);
        BURNER_LANG_PUSH_VALUE(lang->upload_idle);
        BURNER_LANG_PUSH_VALUE(lang->usb_title);
        BURNER_LANG_PUSH_VALUE(lang->usb_tip);
        BURNER_LANG_PUSH_VALUE(lang->btn_enable_usb);
        BURNER_LANG_PUSH_VALUE(lang->btn_disable_usb);
        BURNER_LANG_PUSH_VALUE(lang->btn_refresh_storage);
        BURNER_LANG_PUSH_VALUE(lang->storage_loading);
        BURNER_LANG_PUSH_VALUE(lang->device_title);
        BURNER_LANG_PUSH_VALUE(lang->btn_refresh_device);
        BURNER_LANG_PUSH_VALUE(lang->device_loading);
        BURNER_LANG_PUSH_VALUE(lang->power_title);
        BURNER_LANG_PUSH_VALUE(lang->btn_refresh_power);
        BURNER_LANG_PUSH_VALUE(lang->power_loading);
        BURNER_LANG_PUSH_VALUE(lang->msg_select_main);
        BURNER_LANG_PUSH_VALUE(lang->msg_select_deploy_zip);
        BURNER_LANG_PUSH_VALUE(lang->msg_select_firmware);
        BURNER_LANG_PUSH_VALUE(lang->msg_deploying_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_deploy_success_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_deploy_failed_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_uploading_firmware_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_firmware_success_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_uploading_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_upload_success_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_upload_failed_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_storage_status_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_set_mode_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_device_info_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_power_status_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_applying);
        BURNER_LANG_PUSH_VALUE(lang->language_title);
        BURNER_LANG_PUSH_VALUE(lang->language_tip);
        BURNER_LANG_PUSH_VALUE(lang->btn_read_lang_list);
        BURNER_LANG_PUSH_VALUE(lang->btn_apply_language);
        BURNER_LANG_PUSH_VALUE(lang->language_idle);
        BURNER_LANG_PUSH_VALUE(lang->language_loading);
        BURNER_LANG_PUSH_VALUE(lang->language_none);
        BURNER_LANG_PUSH_VALUE(lang->ip5306_title);
        BURNER_LANG_PUSH_VALUE(lang->ip5306_tip);
        BURNER_LANG_PUSH_VALUE(lang->btn_read_ip5306_ini);
        BURNER_LANG_PUSH_VALUE(lang->btn_save_ip5306_ini);
        BURNER_LANG_PUSH_VALUE(lang->ip5306_idle);
        BURNER_LANG_PUSH_VALUE(lang->ip5306_loading);
        BURNER_LANG_PUSH_VALUE(lang->msg_lang_select_required);
        BURNER_LANG_PUSH_VALUE(lang->msg_lang_list_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_lang_apply_success_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_lang_apply_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_ip5306_load_ok);
        BURNER_LANG_PUSH_VALUE(lang->msg_ip5306_load_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_ip5306_save_ok_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_ip5306_save_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_upload_item_ok);
        BURNER_LANG_PUSH_VALUE(lang->msg_upload_item_fail);
        BURNER_LANG_PUSH_VALUE(lang->msg_http_error_prefix);
        BURNER_LANG_PUSH_VALUE(lang->msg_invalid_json_prefix);
#undef BURNER_LANG_PUSH_VALUE
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    n = snprintf(
        header,
        sizeof(header),
        "{\"ok\":true,\"system_loaded\":%s,\"lang_loaded\":%s,"
        "\"language_version\":\"%s\",\"language_ini\":\"%s\",\"strings\":{",
        burner_json_bool(system_loaded),
        burner_json_bool(lang_loaded),
        esc_version,
        esc_ini);
    if (n <= 0 || n >= (int)sizeof(header)) {
        free(lang);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    err = httpd_resp_send_chunk(req, header, n);
    for (size_t i = 0; err == ESP_OK && i < (sizeof(items) / sizeof(items[0])); i++) {
        err = burner_send_lang_string_chunk(
            req,
            items[i].key,
            values[i],
            (i + 1U) < (sizeof(items) / sizeof(items[0])));
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "}}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }

    free(lang);
    return err;
}

esp_err_t burner_wifi_status_handler(httpd_req_t *req)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_ap_record_t ap_info = {0};
    bool mode_ok = (esp_wifi_get_mode(&mode) == ESP_OK);
    bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    bool provisioning = mode_ok && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    char ssid_raw[33] = {0};
    char ssid[96] = {0};
    char ip_raw[32] = {0};
    char ip[64] = {0};
    bool ip_ok = false;
    int n;
    char resp[420];

    if (connected) {
        snprintf(ssid_raw, sizeof(ssid_raw), "%s", (const char *)ap_info.ssid);
        if (!burner_json_escape(ssid_raw, ssid, sizeof(ssid))) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
        }

        ip_ok = (wifi_maneger_get_sta_ip(ip_raw, sizeof(ip_raw)) == ESP_OK);
        if (ip_ok) {
            if (!burner_json_escape(ip_raw, ip, sizeof(ip))) {
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
            }
        }
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"connected\":%s,\"provisioning\":%s,"
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"channel\":%u,\"authmode\":%d}",
        burner_json_bool(connected),
        burner_json_bool(provisioning),
        connected ? ssid : "",
        ip_ok ? ip : "",
        connected ? ap_info.rssi : 0,
        connected ? ap_info.primary : 0,
        connected ? (int)ap_info.authmode : (int)WIFI_AUTH_OPEN);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_wifi_scan_handler(httpd_req_t *req)
{
    wifi_ap_record_t *ap_list = NULL;
    uint16_t ap_count = 0;
    uint16_t ap_num = WIFI_SCAN_AP_MAX;
    esp_err_t err;
    esp_err_t send_err;

    ap_list = (wifi_ap_record_t *)calloc((size_t)ap_num, sizeof(wifi_ap_record_t));
    if (ap_list == NULL) {
        return burner_send_json(req, "{\"ok\":false,\"networks\":[],\"message\":\"no memory\"}");
    }

    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        free(ap_list);
        return burner_send_json(req, "{\"ok\":false,\"networks\":[],\"message\":\"scan start failed\"}");
    }

    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        free(ap_list);
        return burner_send_json(req, "{\"ok\":false,\"networks\":[],\"message\":\"scan result unavailable\"}");
    }

    if (ap_count < ap_num) {
        ap_num = ap_count;
    }

    err = esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    if (err != ESP_OK) {
        free(ap_list);
        return burner_send_json(req, "{\"ok\":false,\"networks\":[],\"message\":\"scan read failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    send_err = httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"networks\":[");
    if (send_err == ESP_OK) {
        bool first = true;
        for (uint16_t i = 0; i < ap_num; i++) {
            char ssid_esc[96] = {0};
            char line[220];
            int line_len;

            if (ap_list[i].ssid[0] == '\0') {
                continue;
            }
            if (!burner_json_escape((const char *)ap_list[i].ssid, ssid_esc, sizeof(ssid_esc))) {
                continue;
            }

            line_len = snprintf(
                line,
                sizeof(line),
                "%s{\"ssid\":\"%s\",\"rssi\":%d,\"encryption\":%d,\"channel\":%u}",
                first ? "" : ",",
                ssid_esc,
                ap_list[i].rssi,
                (int)ap_list[i].authmode,
                ap_list[i].primary);
            if (line_len <= 0 || line_len >= (int)sizeof(line)) {
                continue;
            }
            send_err = httpd_resp_sendstr_chunk(req, line);
            if (send_err != ESP_OK) {
                break;
            }
            first = false;
        }
    }
    if (send_err == ESP_OK) {
        send_err = httpd_resp_sendstr_chunk(req, "]}");
    }
    if (send_err == ESP_OK) {
        send_err = httpd_resp_sendstr_chunk(req, NULL);
    }

    free(ap_list);
    return send_err;
}

esp_err_t burner_wifi_connect_handler(httpd_req_t *req)
{
    char body[WIFI_JSON_BODY_MAX] = {0};
    char ssid[33] = {0};
    char password[65] = {0};
    bool save = true;
    esp_err_t err;

    err = burner_read_request_body(req, body, sizeof(body), NULL);
    if (err != ESP_OK) {
        return burner_send_json(req, "{\"success\":false,\"message\":\"invalid request body\"}");
    }

    if (!burner_json_get_string(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return burner_send_json(req, "{\"success\":false,\"message\":\"ssid is required\"}");
    }

    (void)burner_json_get_string(body, "password", password, sizeof(password));
    (void)burner_json_get_bool(body, "save", &save);

    if (save) {
        err = wifi_maneger_save_sta_config(ssid, password);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "save wifi config failed: %s", esp_err_to_name(err));
            return burner_send_json(req, "{\"success\":false,\"message\":\"save wifi config failed\"}");
        }
    }

    wifi_maneger_connect(ssid, password);
    return burner_send_json(req, "{\"success\":true,\"message\":\"connecting\"}");
}

esp_err_t burner_wifi_ap_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_maneger_ap();
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "enable ap mode failed: %s", esp_err_to_name(err));
        return burner_send_json(req, "{\"success\":false,\"message\":\"enable ap failed\"}");
    }
    return burner_send_json(req, "{\"success\":true}");
}

esp_err_t burner_wifi_disconnect_handler(httpd_req_t *req)
{
    (void)req;
    wifi_maneger_disconnect();
    return burner_send_json(req, "{\"success\":true}");
}

esp_err_t burner_wifi_forget_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_maneger_clear_sta_config();
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "clear wifi config failed: %s", esp_err_to_name(err));
        return burner_send_json(req, "{\"success\":false,\"message\":\"clear config failed\"}");
    }

    wifi_maneger_disconnect();
    return burner_send_json(req, "{\"success\":true}");
}

esp_err_t burner_web_upload_file(
    httpd_req_t *req,
    const char *default_name,
    bool require_name_query)
{
    char raw_name[96] = {0};
    char file_name[96] = {0};
    char target_rel[TF_PATH_LEN_MAX] = {0};
    char tmp_rel[TF_PATH_LEN_MAX] = {0};
    char target_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char tmp_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char esc_rel[TF_PATH_LEN_MAX * 2 + 8] = {0};
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    int remaining = 0;
    uint32_t written_total = 0;
    bool failed = false;
    bool replaced = false;
    bool cancelled = false;
    esp_err_t err;
    char resp[TF_PATH_LEN_MAX * 2 + 96];
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }
    if (!burner_get_query_arg(req, "name", raw_name, sizeof(raw_name), require_name_query)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid file name query");
    }
    if (raw_name[0] == '\0') {
        if (default_name == NULL || default_name[0] == '\0') {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file name query");
        }
        if (!burner_validate_file_name(default_name, file_name, sizeof(file_name))) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "invalid default file name");
        }
    } else {
        if (!burner_validate_file_name(raw_name, file_name, sizeof(file_name))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid file name");
        }
    }

    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty upload body");
    }
    if (req->content_len > WEB_FILE_UPLOAD_MAX_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web file too large");
    }

    err = burner_mkdirs_rel(WEB_ROOT_DIR_REL);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "create .web dir failed");
    }

    if (snprintf(target_rel, sizeof(target_rel), WEB_ROOT_DIR_REL "/%s", file_name) >=
            (int)sizeof(target_rel) ||
        snprintf(tmp_rel, sizeof(tmp_rel), WEB_ROOT_DIR_REL "/%s.upload_tmp", file_name) >=
            (int)sizeof(tmp_rel) ||
        !burner_build_full_path(target_rel, target_path, sizeof(target_path)) ||
        !burner_build_full_path(tmp_rel, tmp_path, sizeof(tmp_path))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web file path too long");
    }

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open temp file failed");
    }

    buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(fp);
        unlink(tmp_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    burner_cancel_reset();
    burner_status_update(
        BURNER_STATE_RECEIVING,
        0,
        0,
        (uint32_t)req->content_len,
        "web upload started",
        file_name,
        target_rel);

    remaining = req->content_len;
    while (remaining > 0) {
        int to_recv = remaining > TF_IO_CHUNK_SIZE ? TF_IO_CHUNK_SIZE : remaining;
        int recv_len;

        if (burner_cancel_is_requested()) {
            cancelled = true;
            failed = true;
            burner_status_update(
                BURNER_STATE_CANCELLED,
                burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
                written_total,
                (uint32_t)req->content_len,
                "web upload cancelled",
                file_name,
                target_rel);
            break;
        }

        recv_len = httpd_req_recv(req, (char *)buf, to_recv);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            failed = true;
            break;
        }
        if (fwrite(buf, 1, (size_t)recv_len, fp) != (size_t)recv_len) {
            failed = true;
            break;
        }

        remaining -= recv_len;
        written_total += (uint32_t)recv_len;
        burner_status_update(
            BURNER_STATE_RECEIVING,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "web uploading",
            file_name,
            target_rel);
    }

    free(buf);
    fclose(fp);

    if (!cancelled && burner_cancel_is_requested()) {
        cancelled = true;
        failed = true;
        burner_status_update(
            BURNER_STATE_CANCELLED,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "web upload cancelled",
            file_name,
            target_rel);
    }

    if (failed) {
        unlink(tmp_path);
        burner_cancel_reset();
        if (cancelled) {
            return httpd_resp_send_custom_err(req, "409 Conflict", "upload cancelled");
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }

    if (rename(tmp_path, target_path) == 0) {
        replaced = true;
    } else if (errno == EEXIST) {
        if (unlink(target_path) == 0 && rename(tmp_path, target_path) == 0) {
            replaced = true;
        }
    }

    if (!replaced) {
        unlink(tmp_path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "replace web file failed");
    }

    if (!burner_json_escape(target_rel, esc_rel, sizeof(esc_rel))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"path\":\"%s\",\"written\":%" PRIu32 "}",
        esc_rel,
        written_total);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    burner_status_update(
        BURNER_STATE_DONE,
        100,
        written_total,
        written_total,
        "web upload complete",
        file_name,
        target_rel);
    burner_cancel_reset();

    return burner_send_json(req, resp);
}


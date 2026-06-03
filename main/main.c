/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "power_manager.h"
#include "file_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ip5306.h"
#include "lvgl_port.h"
#include "mcu_debug.h"
#include "mori_i2c.h"
#include "nvs_flash.h"
#include "pin_map.h"
#include "sdkconfig.h"
#include "ui.h"
#include "usb_msc_tf.h"
#include "wifi_manager.h"
#include "burner/core/ws_server_internal.h"
#include "tca9555.h"

#define SD_INIT_MAX_RETRY 5
#define STA_CONNECT_TIMEOUT_MS 15000
#define UI_IP_BUF_LEN 32
#define WEB_START_TASK_STACK_SIZE 6144
#define WEB_START_TASK_PRIORITY 4
#define WEB_START_TASK_CORE_ID 0
#define MORI_I2C_BOOT_FREQ_HZ 100000U
#define MORI_TCA9555_BOOT_INPUT_CFG 0xFFFFU
#define MORI_TCA9555_IRQ_DEBOUNCE_MS 25U
#define MORI_TCA9555_IRQ_LOG_RELEASE 0
#define MORI_BOOT_KEY_TASK_STACK_SIZE 2048
#define MORI_BOOT_KEY_TASK_PRIORITY 3
#define MORI_BOOT_KEY_TASK_CORE_ID 0
#define MORI_BOOT_KEY_POLL_MS 20U
#define MORI_BOOT_KEY_DEBOUNCE_MS 40U
#define MORI_BOOT_KEY_LONG_PRESS_MS 700U
#define MORI_IDLE_MONITOR_TASK_STACK_SIZE 3072
#define MORI_IDLE_MONITOR_TASK_PRIORITY 2
#define MORI_IDLE_MONITOR_TASK_CORE_ID 0
#define MORI_IDLE_MONITOR_POLL_MS 500U
#define MORI_WIFI_IDLE_OFF_MS (60U * 1000U)
#define MORI_WIFI_RECONNECT_TASK_STACK_SIZE 4096
#define MORI_WIFI_RECONNECT_TASK_PRIORITY 3
#define MORI_WIFI_RECONNECT_TASK_CORE_ID 0
#define MORI_SETTING_DIR_PATH mount_point "/.setting"
#define MORI_SYSTEM_INI_PATH MORI_SETTING_DIR_PATH "/mori_system.ini"
#define MORI_LANG_ZH_INI_PATH MORI_SETTING_DIR_PATH "/lang_zh_cn.ini"
#define MORI_LANG_EN_INI_PATH MORI_SETTING_DIR_PATH "/lang_en_us.ini"
#define MORI_IP5306_INI_PATH MORI_SETTING_DIR_PATH "/ip5306.ini"
#define MORI_TCA9555_INI_PATH MORI_SETTING_DIR_PATH "/tca9555.ini"
#define MORI_BURN_CONFIG_INI_PATH MORI_SETTING_DIR_PATH "/burn_config.ini"
#define MORI_WIFI_IMPORT_TXT_PATH MORI_SETTING_DIR_PATH "/wifi.txt"
#define MORI_WIFI_SSID_BUF_LEN 33
#define MORI_WIFI_PASS_BUF_LEN 65
#define MORI_WIFI_CONNECT_CHECK_MS 200
#define MORI_INI_LINE_MAX 192
#define MORI_LOCAL_TZ "CST-8"
#define MORI_NTP_SERVER_MAX 96
#define MORI_NTP_DEFAULT_SERVER "ntp.aliyun.com"
#define MORI_NTP_LEGACY_DEFAULT_SERVER "pool.ntp.org"
#define MORI_NTP_SYNC_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define MORI_IP5306_FIXED_CHARGE_CURRENT_MA 450U
#define MORI_IP5306_FIXED_CHG_DIG_BITS 0x04U

static volatile bool s_web_started = false;
static volatile bool s_web_starting = false;
static volatile bool s_wifi_idle_suspended = false;
static volatile bool s_wifi_reconnect_running = false;
static char s_ntp_active_server[MORI_NTP_SERVER_MAX] = "";
static bool s_ntp_active = false;
static TaskHandle_t s_boot_key_task = NULL;
static TaskHandle_t s_idle_monitor_task = NULL;

static void wifi_reconnect_task(void *arg);

static void mori_apply_timezone(void)
{
    setenv("TZ", MORI_LOCAL_TZ, 1);
    tzset();
    ESP_LOGI("main", "timezone configured: %s", MORI_LOCAL_TZ);
}

static const char *main_reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "unknown";
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "other_wdt";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "power_glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu_lockup";
        default:
            return "other";
    }
}

static const char s_default_system_ini[] =
    "# MORI system settings\n"
    "language_version=1\n"
    "language_ini=lang_zh_cn.ini\n"
    "ui_language=1\n"
    "ntp_enable=1\n"
    "ntp_server=" MORI_NTP_DEFAULT_SERVER "\n";

static const char s_default_lang_zh_ini[] =
    "# MORI Chinese language preset\n"
    "page_title=MORI 基础设置\n"
    "page_header=MORI 基础设置（固件内置）\n"
    "page_tip=此页面不依赖 TF 业务网页，始终可用于调试和恢复。\n"
    "business_title=业务网页（TF）\n"
    "btn_open_business=打开业务网页\n"
    "business_tip=业务页面来源：/sdcard/.web/main.html\n"
    "recovery_title=业务网页恢复\n"
    "recovery_tip=可一次上传多个文件到 /sdcard/.web/，用于救砖恢复（main.html/app.js/styles.css 等）。\n"
    "btn_upload_main=上传到 .web\n"
    "system_migrate_title=系统迁移 ZIP\n"
    "system_migrate_tip=将 /sdcard/.setting 和 /sdcard/.web 打包导出为一个 ZIP，便于 TF 卡迁移和恢复。\n"
    "btn_system_migrate=下载迁移 ZIP\n"
    "system_deploy_title=系统部署 ZIP\n"
    "system_deploy_tip=上传迁移 ZIP，将其中内容部署到 /sdcard/.setting 和 /sdcard/.web。\n"
    "btn_system_deploy=部署 ZIP\n"
    "firmware_title=固件升级\n"
    "firmware_tip=上传 moriburnner.bin 进行 OTA 升级，成功后设备会自动重启，不要上传 .elf。\n"
    "btn_upload_firmware=上传固件\n"
    "firmware_idle=空闲\n"
    "upload_idle=空闲\n"
    "usb_title=TF USB 直通\n"
    "usb_tip=启用后：PC 可把 TF 当 U 盘；禁用后：ESP 可访问 TF/Web API。启用时 TF API 会返回 503 以保证安全。\n"
    "btn_enable_usb=启用 USB 直通\n"
    "btn_disable_usb=禁用 USB 直通\n"
    "btn_refresh_storage=刷新存储状态\n"
    "storage_loading=加载中...\n"
    "device_title=设备信息\n"
    "btn_refresh_device=刷新设备信息\n"
    "device_loading=加载中...\n"
    "msg_select_main=请至少选择一个文件\n"
    "msg_select_deploy_zip=请选择一个 ZIP 文件\n"
    "msg_select_firmware=请选择一个 .bin 固件文件\n"
    "msg_deploying_prefix=正在部署\n"
    "msg_deploy_success_prefix=部署成功\n"
    "msg_deploy_failed_prefix=部署失败\n"
    "msg_uploading_firmware_prefix=正在上传固件\n"
    "msg_firmware_success_prefix=固件上传成功，即将重启\n"
    "msg_uploading_prefix=正在上传\n"
    "msg_upload_success_prefix=上传成功\n"
    "msg_upload_failed_prefix=上传失败\n"
    "msg_storage_status_error_prefix=存储状态错误：\n"
    "msg_set_mode_error_prefix=设置模式错误：\n"
    "msg_device_info_error_prefix=设备信息错误：\n"
    "msg_applying=正在应用...\n"
    "language_title=语言\n"
    "language_tip=读取语言 INI 列表，选择后点击应用。\n"
    "btn_read_lang_list=读取 INI 列表\n"
    "btn_apply_language=应用语言\n"
    "language_idle=空闲\n"
    "language_loading=加载中...\n"
    "language_none=（没有可用语言 INI）\n"
    "msg_lang_select_required=请选择语言 INI\n"
    "msg_lang_list_error_prefix=语言列表错误：\n"
    "msg_lang_apply_success_prefix=语言已切换：\n"
    "msg_lang_apply_error_prefix=语言切换失败：\n"
    "msg_upload_item_ok=[成功]\n"
    "msg_upload_item_fail=[失败]\n"
    "msg_http_error_prefix=HTTP 错误：\n"
    "msg_invalid_json_prefix=JSON 解析失败：\n";
static const char s_default_lang_en_ini[] =
    "# MORI English language preset\n"
    "page_title=MORI Base Settings\n"
    "page_header=MORI Base Settings (Firmware Built-in)\n"
    "page_tip=This page does not depend on TF business web and is always available for debug/recovery.\n"
    "business_title=Business Web (TF)\n"
    "btn_open_business=Open Business Web\n"
    "business_tip=Business page source: /sdcard/.web/main.html\n"
    "recovery_title=Business Web Recovery\n"
    "recovery_tip=Upload one or more files to /sdcard/.web/ for recovery (main.html/app.js/styles.css, etc.).\n"
    "btn_upload_main=Upload to .web\n"
    "system_migrate_title=System Migration ZIP\n"
    "system_migrate_tip=Export /sdcard/.setting + /sdcard/.web in one ZIP for TF card migration and recovery.\n"
    "btn_system_migrate=Download Migration ZIP\n"
    "system_deploy_title=System Deploy ZIP\n"
    "system_deploy_tip=Upload a migration ZIP to deploy its contents into /sdcard/.setting and /sdcard/.web.\n"
    "btn_system_deploy=Deploy ZIP\n"
    "firmware_title=Firmware Upgrade\n"
    "firmware_tip=Upload moriburnner.bin for OTA upgrade. Device will reboot automatically when successful. Do not upload .elf.\n"
    "btn_upload_firmware=Upload Firmware\n"
    "firmware_idle=Idle\n"
    "upload_idle=Idle\n"
    "usb_title=TF USB Pass-Through\n"
    "usb_tip=Enable: PC accesses TF as USB disk. Disable: ESP accesses TF/Web APIs. TF APIs return 503 when enabled.\n"
    "btn_enable_usb=Enable USB Pass-Through\n"
    "btn_disable_usb=Disable USB Pass-Through\n"
    "btn_refresh_storage=Refresh Storage Status\n"
    "storage_loading=Loading...\n"
    "device_title=Device Info\n"
    "btn_refresh_device=Refresh Device Info\n"
    "device_loading=Loading...\n"
    "msg_select_main=Please select one or more files\n"
    "msg_select_deploy_zip=Please select a ZIP file\n"
    "msg_select_firmware=Please select a .bin firmware file\n"
    "msg_deploying_prefix=Deploying\n"
    "msg_deploy_success_prefix=Deploy success\n"
    "msg_deploy_failed_prefix=Deploy failed\n"
    "msg_uploading_firmware_prefix=Uploading firmware\n"
    "msg_firmware_success_prefix=Firmware upload success, rebooting\n"
    "msg_uploading_prefix=Uploading\n"
    "msg_upload_success_prefix=Upload success\n"
    "msg_upload_failed_prefix=Upload failed\n"
    "msg_storage_status_error_prefix=Storage status error: \n"
    "msg_set_mode_error_prefix=Set mode error: \n"
    "msg_device_info_error_prefix=Device info error: \n"
    "msg_applying=Applying...\n";

static const char s_default_ip5306_ini[] =
    "# IP5306 register config\n"
    "# apply=1 to enable writing registers on boot.\n"
    "# IMPORTANT: IP5306-I2C datasheet recommends read-modify-write and keeping\n"
    "# reserved bits unchanged. Prefer bit keys below instead of raw reg values.\n"
    "apply=0\n"
    "# Legacy full-register mode (use carefully):\n"
    "# reg_00=0x37  ; SYS_CTL0\n"
    "# reg_01=0x1D  ; SYS_CTL1\n"
    "# reg_02=0xA2  ; SYS_CTL2\n"
    "# reg_20=0x00  ; CHG_CTL0\n"
    "# reg_21=0x00  ; CHG_CTL1\n"
    "# reg_22=0x00  ; CHG_CTL2\n"
    "# reg_23=0x00  ; CHG_CTL3\n"
    "# Recommended bit mode:\n"
    "# boost_enable=1\n"
    "# charge_enable=1\n"
    "# insert_load_boot_enable=1\n"
    "# boost_keep_on=1\n"
    "# key_shutdown_enable=0\n"
    "# boost_off_by_long_press=0\n"
    "# wled_toggle_by_double_press=0\n"
    "# short_press_boost_toggle=0\n"
    "# keep_boost_on_vin_remove=1\n"
    "# batlow_shutdown_enable=1\n"
    "# light_load_shutdown=32    ; 8/16/32/64 or 0/1/2/3\n"
    "# Example-compatible aliases (same bits):\n"
    "# boost_mode=1\n"
    "# charger_mode=1\n"
    "# power_on_load=1\n"
    "# boost_output=1\n"
    "# button_shutdown=0\n"
    "# boost_ctrl_signal=0\n"
    "# flashlight_ctrl_signal=0\n"
    "# short_press_boost=0\n"
    "# boost_after_vin=1\n"
    "# low_battery_shutdown=1\n"
    "# long_press_time=2          ; 2/3 (seconds) or 0/1\n"
    "# charging_stop_voltage=3    ; 0..3\n"
    "# NOTE: charge current is fixed by firmware to 450mA (CHG_DIG_CTL0).\n"
    "# end_charge_current=400     ; end-charge detection only, 200/400/500/600 or 0..3\n"
    "# charger_under_voltage=5    ; 0..7 (4.45V..4.80V)\n"
    "# battery_voltage=4.2        ; 4.2/4.3/4.35/4.4 or 0..3\n"
    "# voltage_pressure=14        ; 0/14/28/42 (mV) or 0..3\n"
    "# cc_loop=1                  ; 1: VIN end CC, 0: BAT end CC\n";

static const char s_default_tca9555_ini[] =
    "# TCA9555 register config\n"
    "# apply=1 to enable writing registers on boot.\n"
    "apply=0\n"
    "# output=0x0000    ; OUTPUT_PORT1:PORT0\n"
    "# polarity=0x0000  ; POLARITY_PORT1:PORT0\n"
    "# config=0xFFFF    ; CONFIG_PORT1:PORT0 (1=input,0=output)\n";

static const char s_default_burn_config_ini[] =
    "# MORI burn config\n"
    "erase_mode=smart\n"
    "gbc_voltage=3v3\n"
    "power_settle_ms=100\n"
    "write_path=psram\n"
    "psram_window_mb=auto\n"
    "mbc5_chunk_kb=16\n"
    "dump_chunk_kb=64\n"
    "gba_fixed_erase_window=1\n"
    "erase_core=cpu1\n"
    "tf_core=cpu1\n"
    "psram_core=cpu1\n";

typedef struct {
    const char *key;
    const char *line;
} mori_ini_kv_line_t;

static const mori_ini_kv_line_t s_lang_zh_upgrade_items[] = {
    {"firmware_title", "firmware_title=固件升级\n"},
    {
        "firmware_tip",
        "firmware_tip=上传 moriburnner.bin 进行 OTA 升级，成功后设备会自动重启，不要上传 .elf。\n",
    },
    {"btn_upload_firmware", "btn_upload_firmware=上传固件\n"},
    {"firmware_idle", "firmware_idle=空闲\n"},
    {"msg_select_firmware", "msg_select_firmware=请选择一个 .bin 固件文件\n"},
    {"msg_uploading_firmware_prefix", "msg_uploading_firmware_prefix=正在上传固件\n"},
    {"msg_firmware_success_prefix", "msg_firmware_success_prefix=固件上传成功，即将重启\n"},
    {"language_title", "language_title=语言\n"},
    {"language_tip", "language_tip=读取语言 INI 列表，选择后点击应用。\n"},
    {"btn_read_lang_list", "btn_read_lang_list=读取 INI 列表\n"},
    {"btn_apply_language", "btn_apply_language=应用语言\n"},
    {"language_idle", "language_idle=空闲\n"},
    {"language_loading", "language_loading=加载中...\n"},
    {"language_none", "language_none=（没有可用语言 INI）\n"},
    {"msg_lang_select_required", "msg_lang_select_required=请选择语言 INI\n"},
    {"msg_lang_list_error_prefix", "msg_lang_list_error_prefix=语言列表错误：\n"},
    {"msg_lang_apply_success_prefix", "msg_lang_apply_success_prefix=语言已切换：\n"},
    {"msg_lang_apply_error_prefix", "msg_lang_apply_error_prefix=语言切换失败：\n"},
    {"ip5306_title", "ip5306_title=IP5306 配置（INI）\n"},
    {
        "ip5306_tip",
        "ip5306_tip=编辑 /sdcard/.setting/ip5306.ini，保存后下次开机或电源初始化时生效。\n",
    },
    {"btn_read_ip5306_ini", "btn_read_ip5306_ini=读取 IP5306 INI\n"},
    {"btn_save_ip5306_ini", "btn_save_ip5306_ini=保存 IP5306 INI\n"},
    {"ip5306_idle", "ip5306_idle=空闲\n"},
    {"ip5306_loading", "ip5306_loading=加载中...\n"},
    {"msg_ip5306_load_ok", "msg_ip5306_load_ok=IP5306 INI 已加载\n"},
    {"msg_ip5306_load_error_prefix", "msg_ip5306_load_error_prefix=IP5306 INI 读取失败：\n"},
    {"msg_ip5306_save_ok_prefix", "msg_ip5306_save_ok_prefix=IP5306 INI 已保存，字节数=\n"},
    {"msg_ip5306_save_error_prefix", "msg_ip5306_save_error_prefix=IP5306 INI 保存失败：\n"},
    {"msg_upload_item_ok", "msg_upload_item_ok=[成功]\n"},
    {"msg_upload_item_fail", "msg_upload_item_fail=[失败]\n"},
    {"msg_http_error_prefix", "msg_http_error_prefix=HTTP 错误：\n"},
    {"msg_invalid_json_prefix", "msg_invalid_json_prefix=JSON 解析失败：\n"},
};
static const mori_ini_kv_line_t s_lang_en_upgrade_items[] = {
    {"firmware_title", "firmware_title=Firmware Upgrade\n"},
    {
        "firmware_tip",
        "firmware_tip=Upload moriburnner.bin for OTA upgrade. Device will reboot automatically when successful. Do not upload .elf.\n",
    },
    {"btn_upload_firmware", "btn_upload_firmware=Upload Firmware\n"},
    {"firmware_idle", "firmware_idle=Idle\n"},
    {"msg_select_firmware", "msg_select_firmware=Please select a .bin firmware file\n"},
    {"msg_uploading_firmware_prefix", "msg_uploading_firmware_prefix=Uploading firmware\n"},
    {
        "msg_firmware_success_prefix",
        "msg_firmware_success_prefix=Firmware upload success, rebooting\n",
    },
};

static const mori_ini_kv_line_t s_lang_common_upgrade_items[] = {
    {"language_title", "language_title=Language\n"},
    {
        "language_tip",
        "language_tip=Read available language INI files, choose one, then apply.\n",
    },
    {"btn_read_lang_list", "btn_read_lang_list=Read INI List\n"},
    {"btn_apply_language", "btn_apply_language=Apply Language\n"},
    {"language_idle", "language_idle=Idle\n"},
    {"language_loading", "language_loading=Loading...\n"},
    {"language_none", "language_none=(No language ini)\n"},
    {"system_migrate_title", "system_migrate_title=System Migration ZIP\n"},
    {
        "system_migrate_tip",
        "system_migrate_tip=Export /sdcard/.setting + /sdcard/.web in one ZIP for TF card migration and recovery.\n",
    },
    {"btn_system_migrate", "btn_system_migrate=Download Migration ZIP\n"},
    {"system_deploy_title", "system_deploy_title=System Deploy ZIP\n"},
    {
        "system_deploy_tip",
        "system_deploy_tip=Upload a migration ZIP to deploy its contents into /sdcard/.setting and /sdcard/.web.\n",
    },
    {"btn_system_deploy", "btn_system_deploy=Deploy ZIP\n"},
    {"msg_select_deploy_zip", "msg_select_deploy_zip=Please select a ZIP file\n"},
    {"msg_deploying_prefix", "msg_deploying_prefix=Deploying\n"},
    {"msg_deploy_success_prefix", "msg_deploy_success_prefix=Deploy success\n"},
    {"msg_deploy_failed_prefix", "msg_deploy_failed_prefix=Deploy failed\n"},
    {"ip5306_title", "ip5306_title=IP5306 Config (INI)\n"},
    {
        "ip5306_tip",
        "ip5306_tip=Edit /sdcard/.setting/ip5306.ini. Save takes effect on next boot or power init.\n",
    },
    {"btn_read_ip5306_ini", "btn_read_ip5306_ini=Read IP5306 INI\n"},
    {"btn_save_ip5306_ini", "btn_save_ip5306_ini=Save IP5306 INI\n"},
    {"ip5306_idle", "ip5306_idle=Idle\n"},
    {"ip5306_loading", "ip5306_loading=Loading...\n"},
    {"msg_lang_select_required", "msg_lang_select_required=Please select a language ini\n"},
    {"msg_lang_list_error_prefix", "msg_lang_list_error_prefix=Language list error: \n"},
    {"msg_lang_apply_success_prefix", "msg_lang_apply_success_prefix=Language applied: \n"},
    {"msg_lang_apply_error_prefix", "msg_lang_apply_error_prefix=Language apply error: \n"},
    {"msg_ip5306_load_ok", "msg_ip5306_load_ok=IP5306 INI loaded\n"},
    {"msg_ip5306_load_error_prefix", "msg_ip5306_load_error_prefix=IP5306 INI load error: \n"},
    {"msg_ip5306_save_ok_prefix", "msg_ip5306_save_ok_prefix=IP5306 INI saved, bytes=\n"},
    {"msg_ip5306_save_error_prefix", "msg_ip5306_save_error_prefix=IP5306 INI save error: \n"},
    {"msg_upload_item_ok", "msg_upload_item_ok=[OK]\n"},
    {"msg_upload_item_fail", "msg_upload_item_fail=[FAIL]\n"},
    {"msg_http_error_prefix", "msg_http_error_prefix=HTTP error: \n"},
    {"msg_invalid_json_prefix", "msg_invalid_json_prefix=Invalid JSON: \n"},
};

static const mori_ini_kv_line_t s_lang_zh_core_upgrade_items[] = {
    {"page_title", "page_title=MORI 基础设置\n"},
    {"page_header", "page_header=MORI 基础设置（固件内置）\n"},
    {"page_tip", "page_tip=此页面不依赖 TF 业务网页，始终可用于调试和恢复。\n"},
    {"business_title", "business_title=业务网页（TF）\n"},
    {"btn_open_business", "btn_open_business=打开业务网页\n"},
    {"business_tip", "business_tip=业务页面来源：/sdcard/.web/main.html\n"},
    {"recovery_title", "recovery_title=业务网页恢复\n"},
    {
        "recovery_tip",
        "recovery_tip=可一次上传多个文件到 /sdcard/.web/，用于救砖恢复（main.html/app.js/styles.css 等）。\n",
    },
    {"btn_upload_main", "btn_upload_main=上传到 .web\n"},
    {"system_migrate_title", "system_migrate_title=系统迁移 ZIP\n"},
    {
        "system_migrate_tip",
        "system_migrate_tip=将 /sdcard/.setting 和 /sdcard/.web 打包导出为一个 ZIP，便于 TF 卡迁移和恢复。\n",
    },
    {"btn_system_migrate", "btn_system_migrate=下载迁移 ZIP\n"},
    {"system_deploy_title", "system_deploy_title=系统部署 ZIP\n"},
    {
        "system_deploy_tip",
        "system_deploy_tip=上传迁移 ZIP，将其中内容部署到 /sdcard/.setting 和 /sdcard/.web。\n",
    },
    {"btn_system_deploy", "btn_system_deploy=部署 ZIP\n"},
    {"upload_idle", "upload_idle=空闲\n"},
    {"usb_title", "usb_title=TF USB 直通\n"},
    {
        "usb_tip",
        "usb_tip=启用后：PC 可把 TF 当 U 盘；禁用后：ESP 可访问 TF/Web API。启用时 TF API 会返回 503 以保证安全。\n",
    },
    {"btn_enable_usb", "btn_enable_usb=启用 USB 直通\n"},
    {"btn_disable_usb", "btn_disable_usb=禁用 USB 直通\n"},
    {"btn_refresh_storage", "btn_refresh_storage=刷新存储状态\n"},
    {"storage_loading", "storage_loading=加载中...\n"},
    {"device_title", "device_title=设备信息\n"},
    {"btn_refresh_device", "btn_refresh_device=刷新设备信息\n"},
    {"device_loading", "device_loading=加载中...\n"},
    {"msg_select_main", "msg_select_main=请至少选择一个文件\n"},
    {"msg_select_deploy_zip", "msg_select_deploy_zip=请选择一个 ZIP 文件\n"},
    {"msg_deploying_prefix", "msg_deploying_prefix=正在部署\n"},
    {"msg_deploy_success_prefix", "msg_deploy_success_prefix=部署成功\n"},
    {"msg_deploy_failed_prefix", "msg_deploy_failed_prefix=部署失败\n"},
    {"msg_uploading_prefix", "msg_uploading_prefix=正在上传\n"},
    {"msg_upload_success_prefix", "msg_upload_success_prefix=上传成功\n"},
    {"msg_upload_failed_prefix", "msg_upload_failed_prefix=上传失败\n"},
    {"msg_storage_status_error_prefix", "msg_storage_status_error_prefix=存储状态错误：\n"},
    {"msg_set_mode_error_prefix", "msg_set_mode_error_prefix=设置模式错误：\n"},
    {"msg_device_info_error_prefix", "msg_device_info_error_prefix=设备信息错误：\n"},
    {"msg_applying", "msg_applying=正在应用...\n"},
};

static const mori_ini_kv_line_t s_system_upgrade_items[] = {
    {"ui_language", "ui_language=1\n"},
    {"ntp_enable", "ntp_enable=1\n"},
    {"ntp_server", "ntp_server=" MORI_NTP_DEFAULT_SERVER "\n"},
};

static esp_err_t write_text_file_if_missing(const char *path, const char *content)
{
    struct stat st;
    FILE *fp = NULL;
    size_t len;

    if (path == NULL || content == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, &st) == 0) {
        if (S_ISREG(st.st_mode)) {
            return ESP_OK;
        }
        return ESP_FAIL;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    len = strlen(content);
    if (fwrite(content, 1, len, fp) != len) {
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}

static esp_err_t write_text_file_force(const char *path, const char *content)
{
    FILE *fp = NULL;
    size_t len = 0;

    if (path == NULL || content == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    len = strlen(content);
    if (fwrite(content, 1, len, fp) != len) {
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}

static bool text_file_read_small(const char *path, char **buf_out, size_t *len_out)
{
    FILE *fp = NULL;
    char *buf = NULL;
    long file_len = 0;
    size_t read_len = 0;

    if (buf_out == NULL || len_out == NULL || path == NULL) {
        return false;
    }

    *buf_out = NULL;
    *len_out = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    file_len = ftell(fp);
    if (file_len <= 0 || file_len > 16384) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    buf = (char *)calloc((size_t)file_len + 1U, 1U);
    if (buf == NULL) {
        fclose(fp);
        return false;
    }

    read_len = fread(buf, 1, (size_t)file_len, fp);
    fclose(fp);
    if (read_len != (size_t)file_len) {
        free(buf);
        return false;
    }

    *buf_out = buf;
    *len_out = (size_t)file_len;
    return true;
}

static bool text_file_contains(const char *path, const char *needle)
{
    char *buf = NULL;
    size_t file_len = 0;
    bool found = false;

    if (path == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    if (!text_file_read_small(path, &buf, &file_len)) {
        return false;
    }

    found = (strstr(buf, needle) != NULL);
    free(buf);
    return found;
}

static bool text_buffer_has_utf16_bom(const unsigned char *buf, size_t len)
{
    if (buf == NULL || len < 2U) {
        return false;
    }

    return ((buf[0] == 0xFFU && buf[1] == 0xFEU) || (buf[0] == 0xFEU && buf[1] == 0xFFU));
}

static bool text_buffer_looks_utf16(const unsigned char *buf, size_t len)
{
    size_t pair_count = 0;
    size_t even_nul_count = 0;
    size_t odd_nul_count = 0;

    if (buf == NULL || len < 16U) {
        return false;
    }

    pair_count = len / 2U;
    if (pair_count == 0U) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0') {
            if ((i & 1U) == 0U) {
                even_nul_count++;
            } else {
                odd_nul_count++;
            }
        }
    }

    return (even_nul_count >= (pair_count / 3U)) || (odd_nul_count >= (pair_count / 3U));
}

static bool text_buffer_is_valid_utf8(const unsigned char *buf, size_t len)
{
    size_t i = 0;

    if (buf == NULL) {
        return false;
    }

    while (i < len) {
        unsigned char ch = buf[i];
        size_t cont_len = 0;

        if (ch == '\0') {
            return false;
        }
        if (ch < 0x80U) {
            i++;
            continue;
        }
        if (ch < 0xC2U) {
            return false;
        }

        if ((ch & 0xE0U) == 0xC0U) {
            cont_len = 1U;
        } else if ((ch & 0xF0U) == 0xE0U) {
            cont_len = 2U;
        } else if ((ch & 0xF8U) == 0xF0U) {
            if (ch > 0xF4U) {
                return false;
            }
            cont_len = 3U;
        } else {
            return false;
        }

        if ((i + cont_len) >= len) {
            return false;
        }

        for (size_t j = 1; j <= cont_len; j++) {
            if ((buf[i + j] & 0xC0U) != 0x80U) {
                return false;
            }
        }

        if (cont_len == 2U) {
            if (ch == 0xE0U && buf[i + 1] < 0xA0U) {
                return false;
            }
            if (ch == 0xEDU && buf[i + 1] >= 0xA0U) {
                return false;
            }
        } else if (cont_len == 3U) {
            if (ch == 0xF0U && buf[i + 1] < 0x90U) {
                return false;
            }
            if (ch == 0xF4U && buf[i + 1] > 0x8FU) {
                return false;
            }
        }

        i += cont_len + 1U;
    }

    return true;
}

static bool text_file_has_invalid_utf8_encoding(const char *path)
{
    char *buf = NULL;
    size_t file_len = 0U;
    bool invalid = false;

    if (path == NULL) {
        return false;
    }

    if (!text_file_read_small(path, &buf, &file_len)) {
        return false;
    }

    invalid = text_buffer_has_utf16_bom((const unsigned char *)buf, file_len) ||
              text_buffer_looks_utf16((const unsigned char *)buf, file_len) ||
              !text_buffer_is_valid_utf8((const unsigned char *)buf, file_len);
    free(buf);
    return invalid;
}

static bool lang_zh_ini_looks_mojibake(const char *path)
{
    bool has_prefix = false;
    bool has_good_cn = false;

    if (path == NULL) {
        return false;
    }

    has_prefix = text_file_contains(path, "page_title=MORI ");
    has_good_cn = text_file_contains(path, "基础设置");
    return has_prefix && !has_good_cn;
}

static bool lang_zh_ini_looks_english_default(const char *path)
{
    if (path == NULL) {
        return false;
    }

    if (text_file_contains(path, "page_title=MORI Base Settings")) {
        return true;
    }
    if (text_file_contains(path, "page_header=MORI Base Settings (Firmware Built-in)")) {
        return true;
    }
    if (text_file_contains(path, "business_title=Business Web (TF)")) {
        return true;
    }
    if (text_file_contains(path, "btn_open_business=Open Business Web")) {
        return true;
    }
    if (text_file_contains(
            path,
            "page_tip=This page does not depend on TF and is always available for debug/configuration.")) {
        return true;
    }
    if (text_file_contains(path, "business_tip=Business web is loaded from /sdcard/.web/main.html")) {
        return true;
    }
    if (text_file_contains(path, "recovery_title=Business Web Recovery")) {
        return true;
    }
    if (text_file_contains(
            path,
            "recovery_tip=Upload one or more files to /sdcard/.web/ for recovery (main.html/app.js/styles.css, etc.).")) {
        return true;
    }

    return false;
}

static bool lang_zh_ini_needs_repair(const char *path)
{
    if (path == NULL) {
        return false;
    }

    return lang_zh_ini_looks_mojibake(path) || lang_zh_ini_looks_english_default(path) ||
           text_file_has_invalid_utf8_encoding(path);
}

static char *mori_trim_inplace(char *text)
{
    char *start = text;
    char *end = NULL;

    if (text == NULL) {
        return NULL;
    }

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (*start == '\0') {
        return start;
    }

    end = start + strlen(start) - 1;
    while (end >= start && isspace((unsigned char)*end)) {
        *end = '\0';
        if (end == start) {
            break;
        }
        end--;
    }

    return start;
}

static bool mori_ini_split_line(char *line, char **key, char **value)
{
    char *eq = NULL;
    char *trimmed = NULL;

    if (line == NULL || key == NULL || value == NULL) {
        return false;
    }

    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
        memmove(line, line + 3, strlen(line + 3) + 1);
    }

    trimmed = mori_trim_inplace(line);
    if (trimmed == NULL || trimmed[0] == '\0') {
        return false;
    }

    if (trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
        return false;
    }

    eq = strchr(trimmed, '=');
    if (eq == NULL) {
        return false;
    }

    *eq = '\0';
    *key = mori_trim_inplace(trimmed);
    *value = mori_trim_inplace(eq + 1);
    if (*key == NULL || *value == NULL || (*key)[0] == '\0') {
        return false;
    }

    return true;
}

static bool mori_parse_bool_text(const char *text, bool *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcasecmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
        strcasecmp(text, "yes") == 0 || strcasecmp(text, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(text, "0") == 0 || strcasecmp(text, "false") == 0 ||
        strcasecmp(text, "no") == 0 || strcasecmp(text, "off") == 0) {
        *out = false;
        return true;
    }

    return false;
}

static bool mori_load_wifi_txt(
    const char *path,
    char *ssid,
    size_t ssid_len,
    char *password,
    size_t password_len)
{
    FILE *fp = NULL;
    char line[MORI_INI_LINE_MAX];

    if (path == NULL || ssid == NULL || password == NULL || ssid_len == 0 || password_len == 0) {
        return false;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *trimmed = NULL;
        char *eq = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        trimmed = mori_trim_inplace(line);
        if (trimmed != NULL && strlen(trimmed) >= 3 && (unsigned char)trimmed[0] == 0xEF &&
            (unsigned char)trimmed[1] == 0xBB && (unsigned char)trimmed[2] == 0xBF) {
            memmove(trimmed, trimmed + 3, strlen(trimmed + 3) + 1);
            trimmed = mori_trim_inplace(trimmed);
        }
        if (trimmed == NULL || trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';' ||
            trimmed[0] == '[') {
            goto next_line;
        }

        eq = strchr(trimmed, '=');
        if (eq != NULL) {
            char *key = NULL;
            char *value = NULL;
            *eq = '\0';
            key = mori_trim_inplace(trimmed);
            value = mori_trim_inplace(eq + 1);
            if (key != NULL && value != NULL) {
                if (strcasecmp(key, "ssid") == 0) {
                    snprintf(ssid, ssid_len, "%s", value);
                } else if (
                    strcasecmp(key, "password") == 0 || strcasecmp(key, "pass") == 0 ||
                    strcasecmp(key, "psk") == 0) {
                    snprintf(password, password_len, "%s", value);
                }
            }
            goto next_line;
        }

        if (ssid[0] == '\0') {
            snprintf(ssid, ssid_len, "%s", trimmed);
        } else if (password[0] == '\0') {
            snprintf(password, password_len, "%s", trimmed);
        }

    next_line:
        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    fclose(fp);

    if (ssid[0] == '\0') {
        return false;
    }
    if (strlen(ssid) > 32 || strlen(password) > 63) {
        ESP_LOGW("main", "wifi.txt invalid length (ssid=%u, pass=%u)", (unsigned)strlen(ssid), (unsigned)strlen(password));
        return false;
    }

    return true;
}

static esp_err_t mori_wait_sta_ip(uint32_t timeout_ms, char *ip, size_t ip_len)
{
    uint32_t elapsed = 0;

    if (ip != NULL && ip_len > 0) {
        ip[0] = '\0';
    }
    if (timeout_ms == 0) {
        timeout_ms = STA_CONNECT_TIMEOUT_MS;
    }

    while (elapsed <= timeout_ms) {
        esp_err_t err = wifi_maneger_get_sta_ip(ip, ip_len);
        if (err == ESP_OK && ip != NULL && ip[0] != '\0') {
            return ESP_OK;
        }
        if (elapsed == timeout_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(MORI_WIFI_CONNECT_CHECK_MS));
        if ((timeout_ms - elapsed) < MORI_WIFI_CONNECT_CHECK_MS) {
            elapsed = timeout_ms;
        } else {
            elapsed += MORI_WIFI_CONNECT_CHECK_MS;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static bool mori_try_connect_wifi_from_tf_txt(void)
{
    struct stat st;
    char ssid[MORI_WIFI_SSID_BUF_LEN] = {0};
    char password[MORI_WIFI_PASS_BUF_LEN] = {0};
    char ip[UI_IP_BUF_LEN] = {0};
    esp_err_t wait_err;
    esp_err_t save_err;

    if (card == NULL) {
        return false;
    }

    if (stat(MORI_WIFI_IMPORT_TXT_PATH, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    if (!mori_load_wifi_txt(
            MORI_WIFI_IMPORT_TXT_PATH,
            ssid,
            sizeof(ssid),
            password,
            sizeof(password))) {
        ESP_LOGW("main", "wifi.txt exists but parse failed");
        return false;
    }

    ui_set_status_text("connecting wifi.txt");
    ESP_LOGI("main", "Found wifi.txt, connecting SSID=\"%s\"", ssid);
    wifi_maneger_connect(ssid, password);

    wait_err = mori_wait_sta_ip(STA_CONNECT_TIMEOUT_MS, ip, sizeof(ip));
    if (wait_err != ESP_OK) {
        ESP_LOGW("main", "wifi.txt connect failed: %s", esp_err_to_name(wait_err));
        return false;
    }

    save_err = wifi_maneger_save_sta_config(ssid, password);
    if (save_err != ESP_OK) {
        ESP_LOGW("main", "wifi.txt connected but save config failed: %s", esp_err_to_name(save_err));
        return true;
    }

    if (remove(MORI_WIFI_IMPORT_TXT_PATH) != 0) {
        ESP_LOGW("main", "wifi.txt connected but delete failed: errno=%d", errno);
    } else {
        ESP_LOGI("main", "wifi.txt applied and deleted");
    }

    ESP_LOGI("main", "Connected from wifi.txt, IP=%s", ip);
    return true;
}

static bool mori_parse_u8_text(const char *text, uint8_t *out)
{
    unsigned long val;
    char *end = NULL;

    if (text == NULL || out == NULL) {
        return false;
    }

    errno = 0;
    val = strtoul(text, &end, 0);
    if (errno != 0 || end == text || val > 0xFFUL) {
        return false;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    *out = (uint8_t)val;
    return true;
}

static bool mori_parse_u16_text(const char *text, uint16_t *out)
{
    unsigned long val;
    char *end = NULL;

    if (text == NULL || out == NULL) {
        return false;
    }

    errno = 0;
    val = strtoul(text, &end, 0);
    if (errno != 0 || end == text || val > 0xFFFFUL) {
        return false;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    *out = (uint16_t)val;
    return true;
}

typedef struct {
    bool enable;
    char server[MORI_NTP_SERVER_MAX];
} mori_ntp_cfg_t;

static bool mori_ntp_server_valid(const char *server)
{
    size_t len = 0;

    if (server == NULL) {
        return false;
    }

    len = strlen(server);
    if (len == 0 || len >= MORI_NTP_SERVER_MAX) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)server[i];
        if (!(isalnum(ch) || ch == '.' || ch == '-' || ch == '_')) {
            return false;
        }
    }

    return true;
}

static void mori_ntp_cfg_set_defaults(mori_ntp_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->enable = true;
    snprintf(cfg->server, sizeof(cfg->server), "%s", MORI_NTP_DEFAULT_SERVER);
}

static void mori_load_ntp_cfg_from_system_ini(mori_ntp_cfg_t *cfg)
{
    FILE *fp = NULL;
    char line[MORI_INI_LINE_MAX];

    if (cfg == NULL) {
        return;
    }

    mori_ntp_cfg_set_defaults(cfg);

    if (card == NULL) {
        return;
    }

    fp = fopen(MORI_SYSTEM_INI_PATH, "rb");
    if (fp == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (mori_ini_split_line(line, &key, &value)) {
            if (strcmp(key, "ntp_enable") == 0) {
                (void)mori_parse_bool_text(value, &cfg->enable);
            } else if (strcmp(key, "ntp_server") == 0 && mori_ntp_server_valid(value)) {
                snprintf(cfg->server, sizeof(cfg->server), "%s", value);
            }
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    fclose(fp);
}

static void mori_ntp_sync_notification_cb(struct timeval *tv)
{
    time_t now = 0;
    struct tm tm_local = {0};
    char buf[32] = {0};

    (void)tv;
    time(&now);
    localtime_r(&now, &tm_local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);
    ESP_LOGI("main", "NTP sync completed (local): %s", buf);
}

static void mori_apply_ntp_service(void)
{
    mori_ntp_cfg_t cfg = {0};

    mori_load_ntp_cfg_from_system_ini(&cfg);

    if (!cfg.enable) {
        if (esp_sntp_enabled()) {
            esp_sntp_stop();
        }
        s_ntp_active = false;
        s_ntp_active_server[0] = '\0';
        ESP_LOGI("main", "NTP disabled by mori_system.ini");
        return;
    }

    if (!mori_ntp_server_valid(cfg.server)) {
        snprintf(cfg.server, sizeof(cfg.server), "%s", MORI_NTP_DEFAULT_SERVER);
    }
    if (strcmp(cfg.server, MORI_NTP_LEGACY_DEFAULT_SERVER) == 0) {
        snprintf(cfg.server, sizeof(cfg.server), "%s", MORI_NTP_DEFAULT_SERVER);
    }

    if (s_ntp_active && esp_sntp_enabled() &&
        strcmp(s_ntp_active_server, cfg.server) == 0) {
        return;
    }

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    snprintf(s_ntp_active_server, sizeof(s_ntp_active_server), "%s", cfg.server);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    /* lwIP keeps this hostname pointer instead of copying it. Keep it in static storage. */
    esp_sntp_setservername(0, s_ntp_active_server);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_sync_interval(MORI_NTP_SYNC_INTERVAL_MS);
    esp_sntp_set_time_sync_notification_cb(mori_ntp_sync_notification_cb);
    esp_sntp_init();

    s_ntp_active = true;
    ESP_LOGI("main", "NTP started, server=%s interval=%" PRIu32 "ms", s_ntp_active_server, MORI_NTP_SYNC_INTERVAL_MS);
}

static bool mori_parse_ip5306_light_load_setting(
    const char *text,
    ip5306_light_load_shutdown_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcasecmp(text, "8") == 0 || strcasecmp(text, "8s") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_8S;
        return true;
    }
    if (strcasecmp(text, "32") == 0 || strcasecmp(text, "32s") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_32S;
        return true;
    }
    if (strcasecmp(text, "16") == 0 || strcasecmp(text, "16s") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_16S;
        return true;
    }
    if (strcasecmp(text, "64") == 0 || strcasecmp(text, "64s") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_64S;
        return true;
    }

    if (strcasecmp(text, "0") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_8S;
        return true;
    }
    if (strcasecmp(text, "1") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_32S;
        return true;
    }
    if (strcasecmp(text, "2") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_16S;
        return true;
    }
    if (strcasecmp(text, "3") == 0) {
        *out = IP5306_LIGHT_LOAD_SHUTDOWN_64S;
        return true;
    }

    return false;
}

static bool mori_parse_ip5306_long_press_setting(const char *text, ip5306_long_press_time_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcasecmp(text, "2") == 0 || strcasecmp(text, "2s") == 0 || strcasecmp(text, "0") == 0) {
        *out = IP5306_LONG_PRESS_2S;
        return true;
    }
    if (strcasecmp(text, "3") == 0 || strcasecmp(text, "3s") == 0 || strcasecmp(text, "1") == 0) {
        *out = IP5306_LONG_PRESS_3S;
        return true;
    }

    return false;
}

static bool mori_parse_ip5306_chg_stop_voltage_setting(
    const char *text,
    ip5306_chg_full_stop_voltage_t *out)
{
    uint8_t parsed = 0;

    if (text == NULL || out == NULL) {
        return false;
    }
    if (!mori_parse_u8_text(text, &parsed) || parsed > 3U) {
        return false;
    }

    *out = (ip5306_chg_full_stop_voltage_t)parsed;
    return true;
}

static bool mori_parse_ip5306_end_charge_current_setting(
    const char *text,
    ip5306_end_charge_current_t *out)
{
    uint8_t parsed = 0;

    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcasecmp(text, "200") == 0 || strcasecmp(text, "200ma") == 0) {
        *out = IP5306_END_CHARGE_CURRENT_200MA;
        return true;
    }
    if (strcasecmp(text, "400") == 0 || strcasecmp(text, "400ma") == 0) {
        *out = IP5306_END_CHARGE_CURRENT_400MA;
        return true;
    }
    if (strcasecmp(text, "500") == 0 || strcasecmp(text, "500ma") == 0) {
        *out = IP5306_END_CHARGE_CURRENT_500MA;
        return true;
    }
    if (strcasecmp(text, "600") == 0 || strcasecmp(text, "600ma") == 0) {
        *out = IP5306_END_CHARGE_CURRENT_600MA;
        return true;
    }

    if (!mori_parse_u8_text(text, &parsed) || parsed > 3U) {
        return false;
    }
    *out = (ip5306_end_charge_current_t)parsed;
    return true;
}

static bool mori_parse_ip5306_charge_under_voltage_setting(
    const char *text,
    ip5306_chg_under_voltage_loop_t *out)
{
    uint8_t parsed = 0;

    if (text == NULL || out == NULL) {
        return false;
    }

    if (!mori_parse_u8_text(text, &parsed) || parsed > 7U) {
        return false;
    }

    *out = (ip5306_chg_under_voltage_loop_t)parsed;
    return true;
}

static bool mori_parse_ip5306_battery_voltage_setting(
    const char *text,
    ip5306_battery_voltage_t *out)
{
    uint8_t parsed = 0;

    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcasecmp(text, "4.2") == 0 || strcasecmp(text, "4.20") == 0) {
        *out = IP5306_BATTERY_VOLTAGE_4P20V;
        return true;
    }
    if (strcasecmp(text, "4.3") == 0 || strcasecmp(text, "4.30") == 0) {
        *out = IP5306_BATTERY_VOLTAGE_4P30V;
        return true;
    }
    if (strcasecmp(text, "4.35") == 0) {
        *out = IP5306_BATTERY_VOLTAGE_4P35V;
        return true;
    }
    if (strcasecmp(text, "4.4") == 0 || strcasecmp(text, "4.40") == 0) {
        *out = IP5306_BATTERY_VOLTAGE_4P40V;
        return true;
    }

    if (!mori_parse_u8_text(text, &parsed) || parsed > 3U) {
        return false;
    }

    *out = (ip5306_battery_voltage_t)parsed;
    return true;
}

static bool mori_parse_ip5306_voltage_pressure_setting(
    const char *text,
    ip5306_voltage_pressure_t *out)
{
    uint8_t parsed = 0;

    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcasecmp(text, "none") == 0) {
        *out = IP5306_VOLTAGE_PRESSURE_NONE;
        return true;
    }
    if (strcasecmp(text, "14") == 0 || strcasecmp(text, "14mv") == 0) {
        *out = IP5306_VOLTAGE_PRESSURE_14MV;
        return true;
    }
    if (strcasecmp(text, "28") == 0 || strcasecmp(text, "28mv") == 0) {
        *out = IP5306_VOLTAGE_PRESSURE_28MV;
        return true;
    }
    if (strcasecmp(text, "42") == 0 || strcasecmp(text, "42mv") == 0) {
        *out = IP5306_VOLTAGE_PRESSURE_42MV;
        return true;
    }

    if (!mori_parse_u8_text(text, &parsed) || parsed > 3U) {
        return false;
    }

    *out = (ip5306_voltage_pressure_t)parsed;
    return true;
}

static bool mori_ini_key_exists(const char *path, const char *key)
{
    FILE *fp = NULL;
    char line[MORI_INI_LINE_MAX];
    bool found = false;

    if (path == NULL || key == NULL) {
        return false;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *parsed_key = NULL;
        char *parsed_value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (mori_ini_split_line(line, &parsed_key, &parsed_value) &&
            strcmp(parsed_key, key) == 0) {
            found = true;
            break;
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    fclose(fp);
    return found;
}

static esp_err_t mori_ini_append_line(const char *path, const char *line)
{
    FILE *check_fp = NULL;
    FILE *append_fp = NULL;
    long file_size = 0;
    int last_ch = '\n';
    size_t line_len = 0;

    if (path == NULL || line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    check_fp = fopen(path, "rb");
    if (check_fp == NULL) {
        return ESP_FAIL;
    }

    if (fseek(check_fp, 0, SEEK_END) == 0) {
        file_size = ftell(check_fp);
        if (file_size > 0 && fseek(check_fp, -1, SEEK_END) == 0) {
            last_ch = fgetc(check_fp);
        }
    }
    fclose(check_fp);

    append_fp = fopen(path, "ab");
    if (append_fp == NULL) {
        return ESP_FAIL;
    }

    if (file_size > 0 && last_ch != '\n' && last_ch != '\r') {
        if (fputc('\n', append_fp) == EOF) {
            fclose(append_fp);
            return ESP_FAIL;
        }
    }

    line_len = strlen(line);
    if (line_len == 0 || fwrite(line, 1, line_len, append_fp) != line_len) {
        fclose(append_fp);
        return ESP_FAIL;
    }

    fclose(append_fp);
    return ESP_OK;
}

static void mori_upgrade_lang_ini_file(
    const char *path,
    const mori_ini_kv_line_t *items,
    size_t item_count)
{
    for (size_t i = 0; i < item_count; i++) {
        if (!mori_ini_key_exists(path, items[i].key)) {
            esp_err_t err = mori_ini_append_line(path, items[i].line);
            if (err != ESP_OK) {
                ESP_LOGW("main", "append %s to %s failed: %s", items[i].key, path, esp_err_to_name(err));
            }
        }
    }
}

static uint8_t mori_load_ui_language_from_system_ini(bool *found_out)
{
    FILE *fp = NULL;
    char line[MORI_INI_LINE_MAX];
    uint8_t language = UI_LANGUAGE_DEFAULT;
    bool found = false;

    if (card == NULL) {
        if (found_out != NULL) {
            *found_out = false;
        }
        return language;
    }

    fp = fopen(MORI_SYSTEM_INI_PATH, "rb");
    if (fp == NULL) {
        if (found_out != NULL) {
            *found_out = false;
        }
        return language;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (mori_ini_split_line(line, &key, &value) && strcmp(key, "ui_language") == 0) {
            uint8_t parsed = UI_LANGUAGE_DEFAULT;
            if (mori_parse_u8_text(value, &parsed)) {
                language = (parsed == UI_LANGUAGE_EN) ? UI_LANGUAGE_EN : UI_LANGUAGE_ZH;
                found = true;
            }
            break;
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    fclose(fp);
    if (found_out != NULL) {
        *found_out = found;
    }
    return language;
}

static void mori_sync_system_language_ini_with_ui_default(void)
{
    uint8_t language = mori_load_ui_language_from_system_ini(NULL);
    mori_ntp_cfg_t ntp = {0};
    char content[256] = {0};

    if (card == NULL || language != UI_LANGUAGE_ZH) {
        return;
    }
    if (text_file_contains(MORI_SYSTEM_INI_PATH, "language_ini=lang_zh_cn.ini")) {
        return;
    }
    if (!text_file_contains(MORI_SYSTEM_INI_PATH, "language_ini=lang_en_us.ini")) {
        return;
    }

    mori_load_ntp_cfg_from_system_ini(&ntp);
    snprintf(
        content,
        sizeof(content),
        "# MORI system settings\nlanguage_version=1\nlanguage_ini=lang_zh_cn.ini\nui_language=1\nntp_enable=%u\nntp_server=%s\n",
        ntp.enable ? 1U : 0U,
        ntp.server);
    if (write_text_file_force(MORI_SYSTEM_INI_PATH, content) == ESP_OK) {
        ESP_LOGI("main", "system language default migrated to zh");
    }
}

static void mori_fix_empty_ntp_server_for_ui_language(void)
{
    char *file = NULL;
    char *out = NULL;
    size_t file_len = 0;
    size_t out_cap = 0;
    size_t out_len = 0;
    bool changed = false;
    bool ntp_server_empty = false;
    uint8_t language;

    if (card == NULL) {
        return;
    }

    language = mori_load_ui_language_from_system_ini(NULL);
    if (language != UI_LANGUAGE_ZH) {
        return;
    }
    if (!text_file_read_small(MORI_SYSTEM_INI_PATH, &file, &file_len)) {
        return;
    }

    out_cap = file_len + strlen(MORI_NTP_DEFAULT_SERVER) + 32U;
    out = (char *)calloc(1, out_cap);
    if (out == NULL) {
        free(file);
        return;
    }

    for (size_t pos = 0; pos < file_len;) {
        size_t line_start = pos;
        size_t line_len = 0;
        size_t copy_len = 0;
        bool has_newline = false;
        char line[MORI_INI_LINE_MAX];
        char *key = NULL;
        char *value = NULL;

        while (pos < file_len && file[pos] != '\n') {
            pos++;
        }
        line_len = pos - line_start;
        if (pos < file_len && file[pos] == '\n') {
            has_newline = true;
            pos++;
        }

        copy_len = line_len;
        while (copy_len > 0U && file[line_start + copy_len - 1U] == '\r') {
            copy_len--;
        }

        if (copy_len < sizeof(line)) {
            memcpy(line, file + line_start, copy_len);
            line[copy_len] = '\0';
            if (mori_ini_split_line(line, &key, &value) &&
                strcmp(key, "ntp_server") == 0 && value[0] == '\0') {
                int n = snprintf(out + out_len, out_cap - out_len, "ntp_server=%s", MORI_NTP_DEFAULT_SERVER);
                if (n <= 0 || (size_t)n >= out_cap - out_len) {
                    free(out);
                    free(file);
                    return;
                }
                out_len += (size_t)n;
                if (line_len > copy_len) {
                    if (out_len + 1U >= out_cap) {
                        free(out);
                        free(file);
                        return;
                    }
                    out[out_len++] = '\r';
                }
                if (has_newline) {
                    if (out_len + 1U >= out_cap) {
                        free(out);
                        free(file);
                        return;
                    }
                    out[out_len++] = '\n';
                }
                changed = true;
                ntp_server_empty = true;
                continue;
            }
        }

        if (out_len + line_len + (has_newline ? 1U : 0U) >= out_cap) {
            free(out);
            free(file);
            return;
        }
        memcpy(out + out_len, file + line_start, line_len);
        out_len += line_len;
        if (has_newline) {
            out[out_len++] = '\n';
        }
    }

    if (changed) {
        out[out_len] = '\0';
        if (write_text_file_force(MORI_SYSTEM_INI_PATH, out) == ESP_OK && ntp_server_empty) {
            ESP_LOGI("main", "empty ntp_server fixed for zh UI: %s", MORI_NTP_DEFAULT_SERVER);
        }
    }

    free(out);
    free(file);
}

static void mori_apply_ui_language_from_system_ini(void)
{
    uint8_t language = mori_load_ui_language_from_system_ini(NULL);

    ui_set_language(language);
    ESP_LOGI("main", "UI language=%u (%s)", (unsigned)language, language == UI_LANGUAGE_ZH ? "zh" : "en");
}

static void apply_ip5306_ini_if_present(void)
{
    typedef struct {
        bool apply;
        bool has_reg0;
        bool has_reg1;
        bool has_reg2;
        bool has_reg20;
        bool has_reg21;
        bool has_reg22;
        bool has_reg23;
        uint8_t reg0;
        uint8_t reg1;
        uint8_t reg2;
        uint8_t reg20;
        uint8_t reg21;
        uint8_t reg22;
        uint8_t reg23;
        bool has_boost_enable;
        bool boost_enable;
        bool has_chg_enable;
        bool chg_enable;
        bool has_insert_load_boot_enable;
        bool insert_load_boot_enable;
        bool has_boost_keep_on;
        bool boost_keep_on;
        bool has_key_shutdown_enable;
        bool key_shutdown_enable;
        bool has_boost_off_by_long_press;
        bool boost_off_by_long_press;
        bool has_wled_toggle_by_double_press;
        bool wled_toggle_by_double_press;
        bool has_short_press_boost_toggle;
        bool short_press_boost_toggle;
        bool has_keep_boost_on_vin_remove;
        bool keep_boost_on_vin_remove;
        bool has_batlow_shutdown_enable;
        bool batlow_shutdown_enable;
        bool has_long_press_time;
        ip5306_long_press_time_t long_press_time;
        bool has_light_load_shutdown;
        ip5306_light_load_shutdown_t light_load_shutdown;
        bool has_chg_stop_voltage;
        ip5306_chg_full_stop_voltage_t chg_stop_voltage;
        bool has_end_charge_current;
        ip5306_end_charge_current_t end_charge_current;
        bool has_charge_under_voltage;
        ip5306_chg_under_voltage_loop_t charge_under_voltage;
        bool has_battery_voltage;
        ip5306_battery_voltage_t battery_voltage;
        bool has_voltage_pressure;
        ip5306_voltage_pressure_t voltage_pressure;
        bool has_cc_loop_vin;
        bool cc_loop_vin;
    } ip5306_ini_cfg_t;

    FILE *fp = NULL;
    char line[MORI_INI_LINE_MAX];
    ip5306_ini_cfg_t cfg = {0};
    int applied = 0;
    uint8_t ctl0 = 0;
    uint8_t ctl1 = 0;
    uint8_t ctl2 = 0;

    if (card == NULL) {
        return;
    }

    fp = fopen(MORI_IP5306_INI_PATH, "rb");
    if (fp == NULL) {
        ESP_LOGW("main", "open ip5306.ini failed");
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (mori_ini_split_line(line, &key, &value)) {
            if (strcmp(key, "apply") == 0) {
                (void)mori_parse_bool_text(value, &cfg.apply);
            } else if (strcmp(key, "reg_00") == 0 || strcmp(key, "sys_ctl0") == 0) {
                if (mori_parse_u8_text(value, &cfg.reg0)) {
                    cfg.has_reg0 = true;
                }
            } else if (strcmp(key, "reg_01") == 0 || strcmp(key, "sys_ctl1") == 0) {
                if (mori_parse_u8_text(value, &cfg.reg1)) {
                    cfg.has_reg1 = true;
                }
            } else if (strcmp(key, "reg_02") == 0 || strcmp(key, "sys_ctl2") == 0) {
                if (mori_parse_u8_text(value, &cfg.reg2)) {
                    cfg.has_reg2 = true;
                }
            } else if (strcmp(key, "reg_20") == 0 || strcmp(key, "charger_ctl0") == 0) {
                if (mori_parse_u8_text(value, &cfg.reg20)) {
                    cfg.has_reg20 = true;
                }
            } else if (strcmp(key, "reg_21") == 0 || strcmp(key, "charger_ctl1") == 0) {
                if (mori_parse_u8_text(value, &cfg.reg21)) {
                    cfg.has_reg21 = true;
                }
            } else if (strcmp(key, "reg_22") == 0 || strcmp(key, "charger_ctl2") == 0) {
                if (mori_parse_u8_text(value, &cfg.reg22)) {
                    cfg.has_reg22 = true;
                }
            } else if (strcmp(key, "reg_23") == 0 || strcmp(key, "charger_ctl3") == 0) {
                if (mori_parse_u8_text(value, &cfg.reg23)) {
                    cfg.has_reg23 = true;
                }
            } else if (strcmp(key, "boost_mode") == 0 || strcmp(key, "boost_enable") == 0) {
                if (mori_parse_bool_text(value, &cfg.boost_enable)) {
                    cfg.has_boost_enable = true;
                }
            } else if (strcmp(key, "charger_mode") == 0 || strcmp(key, "charge_enable") == 0 ||
                       strcmp(key, "charger_enable") == 0) {
                if (mori_parse_bool_text(value, &cfg.chg_enable)) {
                    cfg.has_chg_enable = true;
                }
            } else if (strcmp(key, "power_on_load") == 0 ||
                       strcmp(key, "insert_load_boot_enable") == 0) {
                if (mori_parse_bool_text(value, &cfg.insert_load_boot_enable)) {
                    cfg.has_insert_load_boot_enable = true;
                }
            } else if (strcmp(key, "boost_output") == 0 || strcmp(key, "boost_keep_on") == 0) {
                if (mori_parse_bool_text(value, &cfg.boost_keep_on)) {
                    cfg.has_boost_keep_on = true;
                }
            } else if (strcmp(key, "button_shutdown") == 0 ||
                       strcmp(key, "key_shutdown_enable") == 0) {
                if (mori_parse_bool_text(value, &cfg.key_shutdown_enable)) {
                    cfg.has_key_shutdown_enable = true;
                }
            } else if (strcmp(key, "boost_ctrl_signal") == 0 ||
                       strcmp(key, "boost_off_by_long_press") == 0) {
                if (mori_parse_bool_text(value, &cfg.boost_off_by_long_press)) {
                    cfg.has_boost_off_by_long_press = true;
                }
            } else if (strcmp(key, "flashlight_ctrl_signal") == 0 ||
                       strcmp(key, "wled_toggle_by_double_press") == 0) {
                if (mori_parse_bool_text(value, &cfg.wled_toggle_by_double_press)) {
                    cfg.has_wled_toggle_by_double_press = true;
                }
            } else if (strcmp(key, "short_press_boost") == 0 ||
                       strcmp(key, "short_press_boost_toggle") == 0) {
                if (mori_parse_bool_text(value, &cfg.short_press_boost_toggle)) {
                    cfg.has_short_press_boost_toggle = true;
                }
            } else if (strcmp(key, "boost_after_vin") == 0 ||
                       strcmp(key, "keep_boost_on_vin_remove") == 0) {
                if (mori_parse_bool_text(value, &cfg.keep_boost_on_vin_remove)) {
                    cfg.has_keep_boost_on_vin_remove = true;
                }
            } else if (strcmp(key, "low_battery_shutdown") == 0 ||
                       strcmp(key, "batlow_shutdown_enable") == 0) {
                if (mori_parse_bool_text(value, &cfg.batlow_shutdown_enable)) {
                    cfg.has_batlow_shutdown_enable = true;
                }
            } else if (strcmp(key, "long_press_time") == 0 ||
                       strcmp(key, "long_press_time_s") == 0) {
                if (mori_parse_ip5306_long_press_setting(value, &cfg.long_press_time)) {
                    cfg.has_long_press_time = true;
                }
            } else if (strcmp(key, "light_load_shutdown") == 0 ||
                       strcmp(key, "light_load_shutdown_s") == 0) {
                if (mori_parse_ip5306_light_load_setting(value, &cfg.light_load_shutdown)) {
                    cfg.has_light_load_shutdown = true;
                }
            } else if (strcmp(key, "charging_stop_voltage") == 0 ||
                       strcmp(key, "charging_full_stop_voltage") == 0 ||
                       strcmp(key, "cut_off_voltage") == 0) {
                if (mori_parse_ip5306_chg_stop_voltage_setting(value, &cfg.chg_stop_voltage)) {
                    cfg.has_chg_stop_voltage = true;
                }
            } else if (strcmp(key, "end_charge_current") == 0 ||
                       strcmp(key, "end_charge_current_detection") == 0) {
                if (mori_parse_ip5306_end_charge_current_setting(value, &cfg.end_charge_current)) {
                    cfg.has_end_charge_current = true;
                }
            } else if (strcmp(key, "charger_under_voltage") == 0 ||
                       strcmp(key, "charge_under_voltage_loop") == 0) {
                if (mori_parse_ip5306_charge_under_voltage_setting(value, &cfg.charge_under_voltage)) {
                    cfg.has_charge_under_voltage = true;
                }
            } else if (strcmp(key, "battery_voltage") == 0) {
                if (mori_parse_ip5306_battery_voltage_setting(value, &cfg.battery_voltage)) {
                    cfg.has_battery_voltage = true;
                }
            } else if (strcmp(key, "voltage_pressure") == 0) {
                if (mori_parse_ip5306_voltage_pressure_setting(value, &cfg.voltage_pressure)) {
                    cfg.has_voltage_pressure = true;
                }
            } else if (strcmp(key, "cc_loop") == 0 ||
                       strcmp(key, "cc_loop_vin") == 0 ||
                       strcmp(key, "charge_cc_loop_vin") == 0) {
                if (mori_parse_bool_text(value, &cfg.cc_loop_vin)) {
                    cfg.has_cc_loop_vin = true;
                }
            }
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    fclose(fp);

    if (!cfg.apply) {
        ESP_LOGI("main", "ip5306.ini apply=0, skip register apply");
        return;
    }

    if (cfg.has_reg0 && ip5306_write_reg(IP5306_REG_SYS_CTL0, cfg.reg0) == ESP_OK) {
        applied++;
    }
    if (cfg.has_reg1 && ip5306_write_reg(IP5306_REG_SYS_CTL1, cfg.reg1) == ESP_OK) {
        applied++;
    }
    if (cfg.has_reg2 && ip5306_write_reg(IP5306_REG_SYS_CTL2, cfg.reg2) == ESP_OK) {
        applied++;
    }
    if (cfg.has_reg20 && ip5306_write_reg(IP5306_REG_CHG_CTL0, cfg.reg20) == ESP_OK) {
        applied++;
    }
    if (cfg.has_reg21 && ip5306_write_reg(IP5306_REG_CHG_CTL1, cfg.reg21) == ESP_OK) {
        applied++;
    }
    if (cfg.has_reg22 && ip5306_write_reg(IP5306_REG_CHG_CTL2, cfg.reg22) == ESP_OK) {
        applied++;
    }
    if (cfg.has_reg23 && ip5306_write_reg(IP5306_REG_CHG_CTL3, cfg.reg23) == ESP_OK) {
        applied++;
    }
    if (cfg.has_boost_enable && ip5306_set_boost_enable(cfg.boost_enable) == ESP_OK) {
        applied++;
    }
    if (cfg.has_chg_enable && ip5306_set_charging_enable(cfg.chg_enable) == ESP_OK) {
        applied++;
    }
    if (cfg.has_insert_load_boot_enable &&
        ip5306_set_insert_load_boot_enable(cfg.insert_load_boot_enable) == ESP_OK) {
        applied++;
    }
    if (cfg.has_boost_keep_on && ip5306_set_boost_keep_on(cfg.boost_keep_on) == ESP_OK) {
        applied++;
    }
    if (cfg.has_key_shutdown_enable &&
        ip5306_set_key_shutdown_enable(cfg.key_shutdown_enable) == ESP_OK) {
        applied++;
    }
    if (cfg.has_boost_off_by_long_press &&
        ip5306_set_boost_off_by_long_press(cfg.boost_off_by_long_press) == ESP_OK) {
        applied++;
    }
    if (cfg.has_wled_toggle_by_double_press &&
        ip5306_set_wled_toggle_by_double_press(cfg.wled_toggle_by_double_press) == ESP_OK) {
        applied++;
    }
    if (cfg.has_short_press_boost_toggle &&
        ip5306_set_short_press_boost_toggle(cfg.short_press_boost_toggle) == ESP_OK) {
        applied++;
    }
    if (cfg.has_keep_boost_on_vin_remove &&
        ip5306_set_keep_boost_on_vin_remove(cfg.keep_boost_on_vin_remove) == ESP_OK) {
        applied++;
    }
    if (cfg.has_batlow_shutdown_enable &&
        ip5306_set_batlow_shutdown_enable(cfg.batlow_shutdown_enable) == ESP_OK) {
        applied++;
    }
    if (cfg.has_long_press_time &&
        ip5306_set_long_press_time(cfg.long_press_time) == ESP_OK) {
        applied++;
    }
    if (cfg.has_light_load_shutdown &&
        ip5306_set_light_load_shutdown_time(cfg.light_load_shutdown) == ESP_OK) {
        applied++;
    }
    if (cfg.has_chg_stop_voltage &&
        ip5306_set_charging_stop_voltage(cfg.chg_stop_voltage) == ESP_OK) {
        applied++;
    }
    if (cfg.has_end_charge_current &&
        ip5306_set_end_charge_current_detection(cfg.end_charge_current) == ESP_OK) {
        applied++;
    }
    if (cfg.has_charge_under_voltage &&
        ip5306_set_charge_under_voltage_loop(cfg.charge_under_voltage) == ESP_OK) {
        applied++;
    }
    if (cfg.has_battery_voltage &&
        ip5306_set_battery_voltage(cfg.battery_voltage) == ESP_OK) {
        applied++;
    }
    if (cfg.has_voltage_pressure &&
        ip5306_set_voltage_pressure(cfg.voltage_pressure) == ESP_OK) {
        applied++;
    }
    if (cfg.has_cc_loop_vin &&
        ip5306_set_charge_cc_loop_vin(cfg.cc_loop_vin) == ESP_OK) {
        applied++;
    }

    if (ip5306_get_sys_ctrl(&ctl0, &ctl1, &ctl2) == ESP_OK) {
        ESP_LOGI(
            "main",
            "IP5306 ini applied=%d ctl regs: 0x%02X 0x%02X 0x%02X",
            applied,
            ctl0,
            ctl1,
            ctl2);
    }
}

static void apply_ip5306_fixed_charge_current(void)
{
    uint8_t old_reg = 0;
    uint8_t new_reg = 0;
    esp_err_t err;

    if (!ip5306_ready()) {
        return;
    }

    err = ip5306_read_reg(IP5306_REG_CHG_DIG_CTL0, &old_reg);
    if (err != ESP_OK) {
        ESP_LOGW("main", "read CHG_DIG_CTL0 failed: %s", esp_err_to_name(err));
        return;
    }

    new_reg = (uint8_t)((old_reg & 0xE0U) | MORI_IP5306_FIXED_CHG_DIG_BITS);
    if (new_reg == old_reg) {
        ESP_LOGI(
            "main",
            "IP5306 charge current already fixed at %umA (CHG_DIG_CTL0=0x%02X)",
            MORI_IP5306_FIXED_CHARGE_CURRENT_MA,
            old_reg);
        return;
    }

    err = ip5306_write_reg(IP5306_REG_CHG_DIG_CTL0, new_reg);
    if (err != ESP_OK) {
        ESP_LOGW(
            "main",
            "set fixed charge current %umA failed: %s",
            MORI_IP5306_FIXED_CHARGE_CURRENT_MA,
            esp_err_to_name(err));
        return;
    }

    ESP_LOGI(
        "main",
        "IP5306 charge current fixed at %umA (CHG_DIG_CTL0 0x%02X -> 0x%02X)",
        MORI_IP5306_FIXED_CHARGE_CURRENT_MA,
        old_reg,
        new_reg);
}

static void apply_tca9555_ini_if_present(void)
{
    typedef struct {
        bool apply;
        bool has_output;
        bool has_polarity;
        bool has_config;
        uint16_t output;
        uint16_t polarity;
        uint16_t config;
    } tca9555_ini_cfg_t;

    FILE *fp = NULL;
    char line[MORI_INI_LINE_MAX];
    tca9555_ini_cfg_t cfg = {0};
    int applied = 0;
    uint16_t inputs = 0;
    uint16_t outputs = 0;
    uint16_t polarity = 0;
    uint16_t config = 0;

    if (card == NULL) {
        return;
    }

    fp = fopen(MORI_TCA9555_INI_PATH, "rb");
    if (fp == NULL) {
        ESP_LOGW("main", "open tca9555.ini failed");
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (mori_ini_split_line(line, &key, &value)) {
            if (strcmp(key, "apply") == 0) {
                (void)mori_parse_bool_text(value, &cfg.apply);
            } else if (strcmp(key, "output") == 0 || strcmp(key, "output_port") == 0) {
                if (mori_parse_u16_text(value, &cfg.output)) {
                    cfg.has_output = true;
                }
            } else if (strcmp(key, "polarity") == 0 || strcmp(key, "invert") == 0) {
                if (mori_parse_u16_text(value, &cfg.polarity)) {
                    cfg.has_polarity = true;
                }
            } else if (strcmp(key, "config") == 0 || strcmp(key, "direction") == 0) {
                if (mori_parse_u16_text(value, &cfg.config)) {
                    cfg.has_config = true;
                }
            }
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    fclose(fp);

    if (!cfg.apply) {
        ESP_LOGI("main", "tca9555.ini apply=0, skip register apply");
        return;
    }

    if (cfg.has_output && tca9555_write_outputs(cfg.output) == ESP_OK) {
        applied++;
    }
    if (cfg.has_polarity && tca9555_set_polarity_invert(cfg.polarity) == ESP_OK) {
        applied++;
    }
    if (cfg.has_config && tca9555_write_config(cfg.config) == ESP_OK) {
        applied++;
    }

    if (tca9555_read_inputs(&inputs) == ESP_OK &&
        tca9555_read_outputs(&outputs) == ESP_OK &&
        tca9555_read_word(TCA9555_REG_POLARITY_PORT0, &polarity) == ESP_OK &&
        tca9555_read_config(&config) == ESP_OK) {
        ESP_LOGI(
            "main",
            "TCA9555 ini applied=%d input=0x%04X out=0x%04X pol=0x%04X cfg=0x%04X",
            applied,
            inputs,
            outputs,
            polarity,
            config);
    }
}

static void ensure_setting_files(void)
{
    struct stat st;
    esp_err_t err;

    if (card == NULL) {
        return;
    }

    if (stat(MORI_SETTING_DIR_PATH, &st) != 0) {
        if (mkdir(MORI_SETTING_DIR_PATH, 0775) != 0 && errno != EEXIST) {
            ESP_LOGW("main", "create .setting failed: errno=%d", errno);
            return;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        ESP_LOGW("main", ".setting exists but is not a directory");
        return;
    }

    err = write_text_file_if_missing(MORI_SYSTEM_INI_PATH, s_default_system_ini);
    if (err != ESP_OK) {
        ESP_LOGW("main", "init mori_system.ini failed: %s", esp_err_to_name(err));
    }
    mori_upgrade_lang_ini_file(
        MORI_SYSTEM_INI_PATH,
        s_system_upgrade_items,
        sizeof(s_system_upgrade_items) / sizeof(s_system_upgrade_items[0]));
    mori_sync_system_language_ini_with_ui_default();
    mori_fix_empty_ntp_server_for_ui_language();

    err = write_text_file_if_missing(MORI_LANG_ZH_INI_PATH, s_default_lang_zh_ini);
    if (err != ESP_OK) {
        ESP_LOGW("main", "init lang_zh_cn.ini failed: %s", esp_err_to_name(err));
    }
    if (lang_zh_ini_needs_repair(MORI_LANG_ZH_INI_PATH)) {
        err = write_text_file_force(MORI_LANG_ZH_INI_PATH, s_default_lang_zh_ini);
        if (err != ESP_OK) {
            ESP_LOGW("main", "repair lang_zh_cn.ini failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW("main", "repaired legacy lang_zh_cn.ini");
        }
    }

    err = write_text_file_if_missing(MORI_LANG_EN_INI_PATH, s_default_lang_en_ini);
    if (err != ESP_OK) {
        ESP_LOGW("main", "init lang_en_us.ini failed: %s", esp_err_to_name(err));
    }

    err = write_text_file_if_missing(MORI_IP5306_INI_PATH, s_default_ip5306_ini);
    if (err != ESP_OK) {
        ESP_LOGW("main", "init ip5306.ini failed: %s", esp_err_to_name(err));
    }

    err = write_text_file_if_missing(MORI_TCA9555_INI_PATH, s_default_tca9555_ini);
    if (err != ESP_OK) {
        ESP_LOGW("main", "init tca9555.ini failed: %s", esp_err_to_name(err));
    }

    err = write_text_file_if_missing(MORI_BURN_CONFIG_INI_PATH, s_default_burn_config_ini);
    if (err != ESP_OK) {
        ESP_LOGW("main", "init burn_config.ini failed: %s", esp_err_to_name(err));
    }

    mori_upgrade_lang_ini_file(
        MORI_LANG_ZH_INI_PATH,
        s_lang_zh_upgrade_items,
        sizeof(s_lang_zh_upgrade_items) / sizeof(s_lang_zh_upgrade_items[0]));
    mori_upgrade_lang_ini_file(
        MORI_LANG_ZH_INI_PATH,
        s_lang_zh_core_upgrade_items,
        sizeof(s_lang_zh_core_upgrade_items) / sizeof(s_lang_zh_core_upgrade_items[0]));
    mori_upgrade_lang_ini_file(
        MORI_LANG_ZH_INI_PATH,
        s_lang_common_upgrade_items,
        sizeof(s_lang_common_upgrade_items) / sizeof(s_lang_common_upgrade_items[0]));
    mori_upgrade_lang_ini_file(
        MORI_LANG_EN_INI_PATH,
        s_lang_en_upgrade_items,
        sizeof(s_lang_en_upgrade_items) / sizeof(s_lang_en_upgrade_items[0]));
    mori_upgrade_lang_ini_file(
        MORI_LANG_EN_INI_PATH,
        s_lang_common_upgrade_items,
        sizeof(s_lang_common_upgrade_items) / sizeof(s_lang_common_upgrade_items[0]));
}

static const char *tca9555_pin_name(uint16_t pin_mask)
{
    switch (pin_mask) {
        case TCA9555_IO0_0:
            return "BTN_LEFT";
        case TCA9555_IO0_1:
            return "BTN_DOWN";
        case TCA9555_IO0_2:
            return "BTN_UP";
        case TCA9555_IO0_3:
            return "BTN_RIGHT";
        case TCA9555_IO0_4:
            return "BTN_SELECT";
        case TCA9555_IO0_5:
            return "BTN_START";
        case TCA9555_IO0_6:
            return "BTN_B";
        case TCA9555_IO0_7:
            return "BTN_A";
        case TCA9555_IO1_0:
            return "BTN_JOY";
        case TCA9555_IO1_1:
            return "BTN_VOL_UP";
        case TCA9555_IO1_2:
            return "BTN_MENU";
        case TCA9555_IO1_3:
            return "BTN_VOL_DOWN";
        case TCA9555_IO1_4:
            return "IO1_4";
        case TCA9555_IO1_5:
            return "IO1_5";
        case TCA9555_IO1_6:
            return "IO1_6";
        case TCA9555_IO1_7:
            return "IO1_7";
        default:
            return "UNKNOWN";
    }
}

static bool tca9555_is_known_button(uint16_t pin_mask)
{
    switch (pin_mask) {
        case TCA9555_IO0_0:
        case TCA9555_IO0_1:
        case TCA9555_IO0_2:
        case TCA9555_IO0_3:
        case TCA9555_IO0_4:
        case TCA9555_IO0_5:
        case TCA9555_IO0_6:
        case TCA9555_IO0_7:
        case TCA9555_IO1_0:
        case TCA9555_IO1_1:
        case TCA9555_IO1_2:
        case TCA9555_IO1_3:
            return true;
        default:
            return false;
    }
}

static bool tca9555_button_to_ui_button(uint16_t pin_mask, ui_button_t *button)
{
    if (button == NULL) {
        return false;
    }

    switch (pin_mask) {
        case TCA9555_IO0_0:
            *button = UI_BUTTON_LEFT;
            return true;
        case TCA9555_IO0_1:
        case TCA9555_IO1_3:
            *button = UI_BUTTON_DOWN;
            return true;
        case TCA9555_IO0_2:
        case TCA9555_IO1_1:
            *button = UI_BUTTON_UP;
            return true;
        case TCA9555_IO0_3:
            *button = UI_BUTTON_RIGHT;
            return true;
        case TCA9555_IO0_4:
            *button = UI_BUTTON_PANEL_TOGGLE;
            return true;
        case TCA9555_IO0_5:
        case TCA9555_IO0_7:
        case TCA9555_IO1_0:
            *button = UI_BUTTON_SELECT;
            return true;
        case TCA9555_IO0_6:
            *button = UI_BUTTON_BACK;
            return true;
        case TCA9555_IO1_2:
            *button = UI_BUTTON_MENU;
            return true;
        default:
            return false;
    }
}

static void tca9555_input_irq_cb(uint16_t pin_mask, int level, void *user_ctx)
{
    static uint32_t s_last_ms[16];
    static uint8_t s_last_level[16];
    static bool s_inited = false;
    int pin_index = -1;
    uint32_t now_ms = 0;
    const char *name = NULL;

    (void)user_ctx;

    if (!s_inited) {
        for (int i = 0; i < 16; i++) {
            s_last_ms[i] = 0;
            s_last_level[i] = 0xFF;
        }
        s_inited = true;
    }

    for (int i = 0; i < 16; i++) {
        if (pin_mask == (uint16_t)(1U << i)) {
            pin_index = i;
            break;
        }
    }
    if (pin_index < 0) {
        return;
    }

    now_ms = esp_log_timestamp();
    if (s_last_level[pin_index] == (uint8_t)level) {
        return;
    }
    if (level == 0 && (now_ms - s_last_ms[pin_index]) < MORI_TCA9555_IRQ_DEBOUNCE_MS) {
        return;
    }

    s_last_ms[pin_index] = now_ms;
    s_last_level[pin_index] = (uint8_t)level;
    if (!tca9555_is_known_button(pin_mask)) {
        return;
    }
    {
        ui_button_t button = UI_BUTTON_SELECT;
        if (tca9555_button_to_ui_button(pin_mask, &button)) {
            ui_post_button(button, level == 0);
        }
    }
#if !MORI_TCA9555_IRQ_LOG_RELEASE
    if (level != 0) {
        return;
    }
#endif
    name = tca9555_pin_name(pin_mask);
    ESP_LOGI("main", "TCA9555 IRQ %s pin=0x%04X level=%d", name, pin_mask, level);
}

static void boot_key_task(void *arg)
{
    gpio_config_t key_cfg = {
        .pin_bit_mask = (1ULL << MORI_PIN_KEY_BOOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    int last_raw = 1;
    int stable_level = 1;
    uint32_t last_change_ms = 0;

    (void)arg;

    if (gpio_config(&key_cfg) != ESP_OK) {
        ESP_LOGW("main", "BOOT key gpio config failed");
        s_boot_key_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    last_raw = gpio_get_level(MORI_PIN_KEY_BOOT);
    stable_level = last_raw;
    last_change_ms = esp_log_timestamp();

    while (1) {
        uint32_t now_ms = esp_log_timestamp();
        int raw = gpio_get_level(MORI_PIN_KEY_BOOT);

        if (raw != last_raw) {
            last_raw = raw;
            last_change_ms = now_ms;
        }

        if (raw != stable_level && (now_ms - last_change_ms) >= MORI_BOOT_KEY_DEBOUNCE_MS) {
            stable_level = raw;
            ui_post_button(UI_BUTTON_RIGHT, stable_level == 0);
        }

        vTaskDelay(pdMS_TO_TICKS(MORI_BOOT_KEY_POLL_MS));
    }
}

static void start_boot_key_task(void)
{
    if (s_boot_key_task != NULL) {
        return;
    }

    if (xTaskCreatePinnedToCore(
            boot_key_task,
            "boot_key",
            MORI_BOOT_KEY_TASK_STACK_SIZE,
            NULL,
            MORI_BOOT_KEY_TASK_PRIORITY,
            &s_boot_key_task,
            MORI_BOOT_KEY_TASK_CORE_ID) != pdPASS) {
        s_boot_key_task = NULL;
        ESP_LOGW("main", "create BOOT key task failed");
    }
}

static void init_i2c_peripherals(void)
{
    esp_err_t err;
    i2c_master_bus_handle_t i2c_bus = NULL;
    uint16_t tca_input = 0;
    uint16_t tca_cfg = 0;
    uint8_t tca_addr = TCA9555_I2C_ADDR_DEFAULT;
    bool tca_found = false;

    err = mori_i2c_init(MORI_PIN_I2C_SDA, MORI_PIN_I2C_SCL, MORI_I2C_BOOT_FREQ_HZ);
    if (err != ESP_OK) {
        ESP_LOGW("main", "I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    i2c_bus = mori_i2c_get_bus();
    if (i2c_bus == NULL) {
        ESP_LOGW("main", "I2C bus handle is null");
        return;
    }

    err = power_manager_init(i2c_bus);
    if (err == ESP_OK) {
        if (power_manager_chip_type() == POWER_CHIP_IP5306) {
            uint8_t ctl0 = 0;
            uint8_t ctl1 = 0;
            uint8_t ctl2 = 0;

            apply_ip5306_ini_if_present();
            apply_ip5306_fixed_charge_current();
            if (ip5306_get_sys_ctrl(&ctl0, &ctl1, &ctl2) == ESP_OK) {
                ESP_LOGI("main", "IP5306 ctl regs: 0x%02X 0x%02X 0x%02X", ctl0, ctl1, ctl2);
            }
        } else if (power_manager_chip_type() == POWER_CHIP_AXP209) {
            ESP_LOGI("main", "AXP209 power manager ready at 0x%02X", axp209_address());
        }
    } else {
        ESP_LOGW("main", "no supported power chip found");
    }

    /* Keep sample behavior priority: fixed address first, then fallback scan. */
    if (tca9555_probe(i2c_bus, TCA9555_I2C_ADDR_DEFAULT) == ESP_OK) {
        tca_addr = TCA9555_I2C_ADDR_DEFAULT;
        tca_found = true;
    } else {
        for (uint8_t addr = TCA9555_I2C_ADDR_MIN; addr <= TCA9555_I2C_ADDR_MAX; addr++) {
            if (addr == TCA9555_I2C_ADDR_DEFAULT) {
                continue;
            }
            if (tca9555_probe(i2c_bus, addr) == ESP_OK) {
                tca_addr = addr;
                tca_found = true;
                break;
            }
        }
    }

    if (!tca_found) {
        ESP_LOGW(
            "main",
            "TCA9555 not found in [0x%02X..0x%02X]",
            TCA9555_I2C_ADDR_MIN,
            TCA9555_I2C_ADDR_MAX);
        return;
    }

    err = tca9555_init(i2c_bus, tca_addr);
    if (err != ESP_OK) {
        ESP_LOGW("main", "TCA9555 init failed: %s", esp_err_to_name(err));
        return;
    }

    /*
     * Follow moriesp32 sample behavior: ensure all 16 pins are inputs first,
     * then allow ini overrides (if apply=1).
     */
    for (int i = 0; i < 3; i++) {
        err = tca9555_write_config(MORI_TCA9555_BOOT_INPUT_CFG);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (err != ESP_OK) {
        ESP_LOGW("main", "TCA9555 default input config failed: %s", esp_err_to_name(err));
    }

    apply_tca9555_ini_if_present();

    if (tca9555_read_inputs(&tca_input) == ESP_OK &&
        tca9555_read_config(&tca_cfg) == ESP_OK) {
        ESP_LOGI("main", "TCA9555@0x%02X input=0x%04X cfg=0x%04X", tca_addr, tca_input, tca_cfg);
    }

    err = tca9555_enable_irq(MORI_PIN_I2C_IRQ, tca9555_input_irq_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW("main", "TCA9555 IRQ enable failed: %s", esp_err_to_name(err));
    }
}

static void web_start_task(void *arg)
{
    (void)arg;
    esp_err_t err;

    err = web_ws_start(NULL);
    if (err == ESP_OK) {
        s_web_started = true;
    } else {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        ui_set_status_text("web service start failed");
        ESP_LOGE(
            wifi_manager_tag,
            "Burner web start failed: %s (internal_free=%u largest=%u)",
            esp_err_to_name(err),
            (unsigned)internal_free,
            (unsigned)internal_largest);
    }

    s_web_starting = false;
    vTaskDelete(NULL);
}

static void trigger_web_start_async(void)
{
    BaseType_t create_ret;

    if (s_web_started || s_web_starting) {
        return;
    }

    s_web_starting = true;
    create_ret = xTaskCreatePinnedToCore(
        web_start_task,
        "web_start",
        WEB_START_TASK_STACK_SIZE,
        NULL,
        WEB_START_TASK_PRIORITY,
        NULL,
        WEB_START_TASK_CORE_ID);
    if (create_ret != pdPASS) {
        s_web_starting = false;
        ui_set_status_text("web task create failed");
        ESP_LOGE(wifi_manager_tag, "Create web start task failed");
    }
}

static void stop_web_service_if_running(void)
{
    if (s_web_started || s_web_starting) {
        web_ws_stop();
        s_web_started = false;
        s_web_starting = false;
    }
}

static void idle_monitor_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t last_activity_ms = ui_get_last_activity_ms();
        uint32_t now_ms = esp_log_timestamp();

        if (last_activity_ms != 0U &&
            !burner_task_is_running_snapshot() &&
            !s_wifi_idle_suspended &&
            (now_ms - last_activity_ms) >= MORI_WIFI_IDLE_OFF_MS) {
            stop_web_service_if_running();
            wifi_maneger_disconnect();
            s_wifi_idle_suspended = true;
            ESP_LOGI("main", "idle timeout reached, Wi-Fi disconnected");
        }

        vTaskDelay(pdMS_TO_TICKS(MORI_IDLE_MONITOR_POLL_MS));
    }
}

static void start_idle_monitor_task(void)
{
    if (s_idle_monitor_task != NULL) {
        return;
    }

    if (xTaskCreatePinnedToCore(
            idle_monitor_task,
            "idle_monitor",
            MORI_IDLE_MONITOR_TASK_STACK_SIZE,
            NULL,
            MORI_IDLE_MONITOR_TASK_PRIORITY,
            &s_idle_monitor_task,
            MORI_IDLE_MONITOR_TASK_CORE_ID) != pdPASS) {
        s_idle_monitor_task = NULL;
        ESP_LOGW("main", "create idle monitor task failed");
    }
}

static void activity_reconnect_wifi_if_needed(void)
{
    BaseType_t create_ret;

    if (!s_wifi_idle_suspended || s_wifi_reconnect_running) {
        return;
    }
    if (burner_task_is_running_snapshot()) {
        return;
    }
    if (!wifi_maneger_ready() || !wifi_maneger_has_saved_sta()) {
        s_wifi_idle_suspended = false;
        return;
    }

    s_wifi_reconnect_running = true;
    create_ret = xTaskCreatePinnedToCore(
        wifi_reconnect_task,
        "wifi_reconnect",
        MORI_WIFI_RECONNECT_TASK_STACK_SIZE,
        NULL,
        MORI_WIFI_RECONNECT_TASK_PRIORITY,
        NULL,
        MORI_WIFI_RECONNECT_TASK_CORE_ID);
    if (create_ret != pdPASS) {
        s_wifi_reconnect_running = false;
    }
}

static void wifi_reconnect_task(void *arg)
{
    (void)arg;

    ui_set_status_text("connecting saved wifi");
    s_wifi_idle_suspended = false;
    wifi_maneger_connect_saved(STA_CONNECT_TIMEOUT_MS);
    s_wifi_reconnect_running = false;
    vTaskDelete(NULL);
}

static void ui_update_sta_ip(void)
{
    char ip[UI_IP_BUF_LEN] = {0};
    if (wifi_maneger_get_sta_ip(ip, sizeof(ip)) == ESP_OK) {
        ui_set_ip_text(ip);
    } else {
        ui_set_ip_text("--");
    }
}

static void wifi_state_handler(WIFI_STATE state)
{
    if (state == WIFI_STATE_CONNECTED) {
        char ip[UI_IP_BUF_LEN] = {0};
        char status[64] = {0};

        ui_set_wifi_state(UI_WIFI_STATE_CONNECTED);
        ui_update_sta_ip();
        if (wifi_maneger_get_sta_ip(ip, sizeof(ip)) == ESP_OK) {
            snprintf(status, sizeof(status), "wifi connected %s", ip);
            ui_set_status_text(status);
        } else {
            ui_set_status_text("wifi connected");
        }
        s_wifi_idle_suspended = false;
        ESP_LOGI(wifi_manager_tag, "Wi-Fi connected");
        mori_apply_ntp_service();
        trigger_web_start_async();
    } else if (state == WIFI_STATE_PROVISIONING_CONNECTED) {
        char ip[UI_IP_BUF_LEN] = {0};
        char status[96] = {0};

        ui_set_wifi_state(UI_WIFI_STATE_PROVISIONING);
        ui_update_sta_ip();
        if (wifi_maneger_get_sta_ip(ip, sizeof(ip)) == ESP_OK) {
            snprintf(status, sizeof(status), "wifi connected %s", ip);
            ui_set_status_text(status);
        } else {
            ui_set_status_text("wifi connected");
        }
        s_wifi_idle_suspended = false;
        ESP_LOGI(wifi_manager_tag, "Provisioning connected, waiting user confirm");
        mori_apply_ntp_service();
    } else if (state == WIFI_STATE_DISCONNECTED) {
        stop_web_service_if_running();
        ui_set_wifi_state(UI_WIFI_STATE_DISCONNECTED);
        ui_set_ip_text("--");
        ui_set_status_text("wifi disconnected");
        ESP_LOGW(wifi_manager_tag, "Wi-Fi disconnected");
    } else if (state == WIFI_STATE_PROVISIONING) {
        ui_set_wifi_state(UI_WIFI_STATE_PROVISIONING);
        ui_set_ip_text("192.168.4.1");
        ui_set_status_text("wifi provisioning mode");
        s_wifi_idle_suspended = false;
        ESP_LOGI(wifi_manager_tag, "Provision AP enabled");
    }
}

static void mount_sdcard_if_possible(void)
{
    bool sd_ready = false;
    uint64_t size = 0;

    for (int i = 0; i < SD_INIT_MAX_RETRY; i++) {
        if (sdmmc_init() == ESP_OK) {
            sd_ready = true;
            break;
        }
        ESP_LOGW(sdcard_tag, "SD card init failed, retry %d/%d", i + 1, SD_INIT_MAX_RETRY);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!sd_ready) {
        ESP_LOGW(sdcard_tag, "SD card is unavailable, continue without TF");
        return;
    }

    ESP_LOGI(sdcard_tag, "SD card mounted");
    if (card != NULL) {
        size = ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024);
        ESP_LOGI(sdcard_tag, "Total: %" PRIu64 "MB", size);
    }

    ensure_setting_files();
    {
        esp_err_t burn_cfg_err = burner_load_burn_config();
        if (burn_cfg_err != ESP_OK) {
            ESP_LOGW("main", "load burn_config.ini failed: %s", esp_err_to_name(burn_cfg_err));
        }
    }
    ESP_LOGI("main", "GBX cache auto rebuild disabled; update manually from system menu");
    mori_apply_ui_language_from_system_ini();
}

static void init_nvs_storage(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    bool wifi_txt_connected = false;
    esp_err_t connect_err;
    esp_err_t debug_init_err;
    esp_err_t lvgl_err;
    esp_err_t usb_msc_err;
    esp_err_t wifi_init_err;
    esp_reset_reason_t reset_reason = esp_reset_reason();

    ESP_LOGI("main", "boot start, reset_reason=%s (%d)", main_reset_reason_str(reset_reason), (int)reset_reason);
    (void)power_manager_cpu_freq_init();
    mori_apply_timezone();

    ui_set_status_text("system initializing");
    ui_set_wifi_state(UI_WIFI_STATE_UNKNOWN);
    ui_set_ip_text("--");
    ui_set_burn_progress(0, 0, 0);

    ESP_LOGI("main", "boot step: mount assets");
    if (assets_fs_init() != ESP_OK) {
        ESP_LOGW("main", "assets partition unavailable, continue without built-in assets");
    }

    ESP_LOGI("main", "boot step: mount sdcard");
    mount_sdcard_if_possible();
    ESP_LOGI("main", "boot step: init i2c peripherals");
    init_i2c_peripherals();
    if (card != NULL) {
        ESP_LOGI("main", "boot step: init usb msc manager");
        usb_msc_err = usb_msc_tf_init();
        if (usb_msc_err != ESP_OK) {
            ESP_LOGW("main", "USB MSC init failed: %s", esp_err_to_name(usb_msc_err));
        } else {
            ESP_LOGI("main", "USB MSC manager ready, pass-through default is disabled");
        }
    }
    ESP_LOGI("main", "boot step: init nvs");
    init_nvs_storage();
    ESP_LOGI("main", "boot step: init mcu debug");
    debug_init_err = mcu_debug_init();
    if (debug_init_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI("main", "MCU SWD debug disabled");
    } else if (debug_init_err != ESP_OK) {
        ESP_LOGW("main", "MCU debug init failed: %s", esp_err_to_name(debug_init_err));
    }

    ESP_LOGI("main", "boot step: init lvgl/display");
    lvgl_err = lvgl_port_init();
    if (lvgl_err != ESP_OK) {
        ESP_LOGW("main", "LVGL/display init failed, continuing headless: %s", esp_err_to_name(lvgl_err));
    } else {
        ui_set_activity_callback(activity_reconnect_wifi_if_needed);
        start_boot_key_task();
        start_idle_monitor_task();
        ui_mark_activity();
        ui_set_status_text("system initialized");
    }

    ESP_LOGI("main", "boot step: init wifi manager");
    wifi_init_err = wifi_maneger_init(wifi_state_handler);
    if (wifi_init_err != ESP_OK || !wifi_maneger_ready()) {
        ui_set_status_text("wifi manager init failed");
        ESP_LOGE("main", "Wi-Fi manager init failed: %s", esp_err_to_name(wifi_init_err));
        return;
    }

    wifi_txt_connected = mori_try_connect_wifi_from_tf_txt();
    if (!wifi_txt_connected) {
        if (wifi_maneger_has_saved_sta()) {
            ui_set_status_text("connecting saved wifi");
            ESP_LOGI(wifi_manager_tag, "Found saved Wi-Fi config, connecting...");
            connect_err = wifi_maneger_connect_saved(STA_CONNECT_TIMEOUT_MS);
            if (connect_err == ESP_OK) {
                ESP_LOGI(wifi_manager_tag, "Connected with saved Wi-Fi profile");
            } else {
                ui_set_status_text("saved wifi failed, provisioning");
                ESP_LOGW(
                    wifi_manager_tag,
                    "Saved Wi-Fi connect failed: %s. Enter provisioning mode.",
                    esp_err_to_name(connect_err));
                {
                    esp_err_t ap_err = wifi_maneger_ap();
                    if (ap_err != ESP_OK) {
                        ui_set_status_text("provisioning start failed");
                        ESP_LOGE("main", "enter provisioning failed: %s", esp_err_to_name(ap_err));
                    }
                }
            }
        } else {
            ui_set_status_text("no saved wifi, provisioning");
            ESP_LOGI(wifi_manager_tag, "No saved Wi-Fi profile. Enter provisioning mode.");
            {
                esp_err_t ap_err = wifi_maneger_ap();
                if (ap_err != ESP_OK) {
                    ui_set_status_text("provisioning start failed");
                    ESP_LOGE("main", "enter provisioning failed: %s", esp_err_to_name(ap_err));
                }
            }
        }
    } else {
        ESP_LOGI("main", "Connected with /sdcard/.setting/wifi.txt");
    }
}


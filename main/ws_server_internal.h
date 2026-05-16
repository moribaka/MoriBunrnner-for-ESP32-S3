#ifndef WS_SERVER_INTERNAL_H
#define WS_SERVER_INTERNAL_H

#include "ws_server.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <utime.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_psram.h"
#include "esp_freertos_hooks.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "file_system.h"
#include "ip5306.h"
#include "lcd_display.h"
#include "mcu_debug.h"
#include "pin_map.h"
#include "tca9555.h"
#include "ui.h"
#include "usb_msc_tf.h"
#include "wifi_manager.h"

#define BURNER_TAG "burner_web"

#define ROM_DIR_PATH mount_point "/roms"
#define ROM_OUTPUT_DIR_PATH mount_point "/ROM_OUTPUT"
#define DUMP_DIR_PATH mount_point "/dumps"
#define UPLOAD_CHUNK_SIZE 2048
#define BURN_GBC_READ_SPI_OVERHEAD_BYTES 4U
#define BURN_GBC_READ_SPI_BYTES_PER_DATA_BYTE 3U
#define BURN_CART_WRITE_MAX_BYTES (BURNER_SPI_MAX_XFER / 6U)
#define BURN_CART_READ_MAX_BYTES ((BURNER_SPI_MAX_XFER - BURN_GBC_READ_SPI_OVERHEAD_BYTES) / BURN_GBC_READ_SPI_BYTES_PER_DATA_BYTE)
#define BURN_MBC5_PROGRAM_CHUNK_BYTES (16U * 1024U)
#define BURN_MBC5_PROGRAM_CHUNK_MIN_BYTES (4U * 1024U)
#define BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES (128U * 1024U)
#define BURN_MBC5_DUMP_CHUNK_BYTES (64U * 1024U)
#define BURN_ROM_DUMP_CHUNK_MIN_BYTES (32U * 1024U)
#define BURN_ROM_DUMP_CHUNK_MAX_BYTES (256U * 1024U)
#define BURN_ERASE_ALWAYS_DEFAULT 0U
#define BURN_MBC5_ERASE_PROBE_BYTES 512U
#define BURN_ERASE_BLANK_SAMPLE_BYTES 512U
#define BURN_MBC5_RAM_CHUNK_BYTES 4096U
#define BURN_GBA_PROGRAM_CHUNK_BYTES 65536U
#define BURN_GBA_DUMP_CHUNK_BYTES 65536U
#define BURN_GBA_LINEAR_ADDR_BYTES (32U * 1024U * 1024U)
#define BURN_GBA_BANK_BYTES BURN_GBA_LINEAR_ADDR_BYTES
#define BURN_PSRAM_WINDOW_BYTES_PER_MB (1024U * 1024U)
#define BURN_PSRAM_WINDOW_AUTO_MB 0U
#define BURN_PSRAM_WINDOW_MIN_MB 1U
#define BURN_PSRAM_WINDOW_MAX_MB 8U
#define BURN_PSRAM_WINDOW_DEFAULT_MB BURN_PSRAM_WINDOW_AUTO_MB
#define BURN_PSRAM_WINDOW_RESERVE_BYTES (256U * 1024U)
#define BURN_GBA_FIXED_ERASE_WINDOW_ENABLED_DEFAULT 1U
#define BURN_GBA_FIXED_ERASE_WINDOW_MB 4U
#define BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES 0U
#define BURN_READ_PSRAM_FRAGMENT_MB 7U
#define BURN_READ_PSRAM_FRAGMENT_BYTES (BURN_READ_PSRAM_FRAGMENT_MB * BURN_PSRAM_WINDOW_BYTES_PER_MB)
#define BURN_VERIFY_PSRAM_WINDOW_MB 7U
#define BURN_VERIFY_PSRAM_WINDOW_BYTES (BURN_VERIFY_PSRAM_WINDOW_MB * BURN_PSRAM_WINDOW_BYTES_PER_MB)
#define VERIFY_LOG_DIR_REL ".log"
#define ROM_OUTPUT_TEMP_ROOT_REL ".temp/ROM_OUTPUT"
#define TF_PATH_LEN_MAX 240
#define TF_QUERY_LEN_MAX 320
#define BURNER_FILE_NAME_LEN TF_PATH_LEN_MAX
#define BURNER_FILE_PATH_LEN (TF_PATH_LEN_MAX + 64)
#define BURNER_JSON_RESP_LEN (BURNER_FILE_PATH_LEN + 192)
#define BURNER_VALID_WALLCLOCK_MIN ((time_t)1700000000)
#define BURNER_SUSPECT_FILE_MTIME_MAX ((time_t)631152000)
#define BURNER_GBA_TITLE_OFFSET 0xA0u
#define BURNER_GBA_TITLE_LEN 12u
#define BURNER_MBC5_TITLE_OFFSET 0x134u
#define BURNER_MBC5_TITLE_LEN 16u
#define TF_IO_CHUNK_SIZE 2048
#define BURN_TF_STDIO_BUFFER_BYTES (128U * 1024U)
#define WEB_HTTPD_STACK_SIZE 8192
#define WEB_ROOT_DIR_REL ".web"
#define WEB_MAIN_FILE_REL ".web/main.html"
#define WEB_FILE_PATH_LEN_MAX 320
#define WEB_MAIN_UPLOAD_MAX_SIZE (512 * 1024)
#define WEB_FILE_UPLOAD_MAX_SIZE (1024 * 1024)
#define FW_UPLOAD_CHUNK_SIZE 4096
#define WIFI_JSON_BODY_MAX 512
#define POWER_JSON_BODY_MAX 160
#define WIFI_SCAN_AP_MAX 24
#define WEB_LANG_DIR_REL ".setting"
#define WEB_LANG_SYSTEM_INI_REL WEB_LANG_DIR_REL "/mori_system.ini"
#define WEB_LANG_DEFAULT_INI "lang_zh_cn.ini"
#define WEB_LANG_FALLBACK_EN_INI "lang_en_us.ini"
#define WEB_LANG_FILE_NAME_MAX 64
#define WEB_LANG_VERSION_MAX 24
#define WEB_LANG_TEXT_MAX 192
#define WEB_LANG_LINE_MAX 512
#define WEB_NTP_SERVER_MAX 96
#define WEB_NTP_ENABLE_MAX 8
#define WEB_NTP_SERVER_DEFAULT "ntp.aliyun.com"
#define SYSTEM_MIGRATE_ZIP_NAME "mori_system_migration.zip"
#define ZIP_VERSION_NEEDED 20U
#define ZIP_GP_FLAG_DATA_DESCRIPTOR 0x0008U
#define ZIP_METHOD_STORE 0U
#define ZIP_DOS_DATE_MIN 0x0021U
#define ZIP_ENTRY_NAME_MAX TF_PATH_LEN_MAX
#define ZIP_EOCD_MIN_SIZE 22U
#define ZIP_EOCD_MAX_SEARCH (ZIP_EOCD_MIN_SIZE + 0xFFFFU)
#define SYSTEM_DEPLOY_ZIP_MAX_SIZE (32U * 1024U * 1024U)
#define SYSTEM_DEPLOY_TMP_ROOT_REL ".tmp/system_deploy"
#define SYSTEM_DEPLOY_TMP_ZIP_REL SYSTEM_DEPLOY_TMP_ROOT_REL "/upload.zip"
#define SYSTEM_DEPLOY_STAGE_REL SYSTEM_DEPLOY_TMP_ROOT_REL "/stage"
#define SYSTEM_DEPLOY_STAGE_WEB_REL SYSTEM_DEPLOY_STAGE_REL "/.web"
#define SYSTEM_DEPLOY_STAGE_SETTING_REL SYSTEM_DEPLOY_STAGE_REL "/.setting"
#define SYSTEM_DEPLOY_REL_MAX_LEN 180U
#define BURNER_SYSTEM_MIGRATE_REL_DIR_COUNT 2U

typedef struct {
    char esc_path[TF_PATH_LEN_MAX * 2 + 8];
    char head[TF_PATH_LEN_MAX * 2 + 120];
    char child_rel[TF_PATH_LEN_MAX];
    char child_full[TF_PATH_LEN_MAX + 64];
    char esc_name[TF_PATH_LEN_MAX * 2 + 8];
    char esc_child[TF_PATH_LEN_MAX * 2 + 8];
    char line[TF_PATH_LEN_MAX * 4 + 128];
} burner_tf_list_buf_t;

/* SPI burn path: ESP32-S3 SPI2 master -> AG32 CPLD core (Bacon compatible). */
#define BURNER_SPI_ENABLE 1
#define BURNER_SPI_HOST SPI2_HOST
#define BURNER_SPI_CLOCK_HZ (40 * 1000 * 1000)
/* Optional fallback profiles */
/* #define BURNER_SPI_CLOCK_HZ (60 * 1000 * 1000) */
/* #define BURNER_SPI_CLOCK_HZ (20 * 1000 * 1000) */
#define BURNER_SPI_MAX_XFER (16 * 1024)
#define BURNER_SPI_STREAM_CHUNK_BYTES 2048U
#define BURNER_ROM_POLL_TIMEOUT_MS 2000
#define BURNER_ROM_POLL_INTERVAL_US 50
#define BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS 20000U
#define BURNER_ROM_ERASE_TIMEOUT_MAX_MS (5U * 60U * 1000U)
#define BURNER_ROM_CHIP_ERASE_TIMEOUT_MS BURNER_ROM_ERASE_TIMEOUT_MAX_MS
#define BURNER_GBA_HOST_UNLOCK_ADDR0 0x555u
#define BURNER_GBA_HOST_UNLOCK_ADDR1 0x2AAu
#define BURNER_GBA_HOST_CFI_ENTER_ADDR 0x055u
#define BURNER_GBA_CFI_RETRY_COUNT 3U
#define BURNER_GBA_BANK_SWITCH_SETTLE_MS 8U
#define BURNER_GBA_READ_TURNAROUND_HOLD_BYTES 0U
#define BURNER_GBA_CMD_WRITE_SETUP_HOLD_BYTES 2U
#define BURNER_GBA_CMD_WRITE_STROBE_HOLD_BYTES 4U
#define BURNER_GBA_SPI_CS_SETUP_US 5U
#define BURNER_GBA_SPI_CS_HOLD_US 5U
#define BURNER_GBA_ROM_PHASE_GAP_US 1U
#define BURNER_SPEED_WARMUP_US (1000ULL * 1000ULL)
#define BURNER_POWER_SETTLE_MS 100
#define BURNER_IDLE_POWER_TIMEOUT_MS 5000
#define BURNER_IDLE_MONITOR_INTERVAL_MS 500
#define BURNER_MBC5_SLOT_MAX 17U
#define BURNER_CART_ID_DEBUG_SAMPLE_DEFAULT 32U
#define BURNER_CART_ID_DEBUG_SAMPLE_MAX 64U

typedef enum {
    BURNER_GBA_CMD_ADDR_WORD = 0, /* unlock: 0x555/0x2AA, CFI enter: 0x055 */
    BURNER_GBA_CMD_ADDR_BYTE,     /* unlock: 0xAAA/0x555, CFI enter: 0x0AA */
    BURNER_GBA_CMD_ADDR_BYTE_X16, /* unlock: 0xAAA/0x554, CFI enter: 0x0AA */
} burner_gba_cmd_addr_mode_t;

typedef enum {
    BURNER_GBA_CMD_DATA_LOW = 0, /* command byte on D7..D0 */
    BURNER_GBA_CMD_DATA_HIGH,    /* command byte on D15..D8 */
} burner_gba_cmd_data_lane_t;

typedef struct {
    bool prepared;
    uint16_t current_bank;
    uint16_t buffer_write_bytes;
    uint32_t sector_size;
    uint32_t device_size;
    uint8_t mbc5_id[4];
    burner_gba_cmd_addr_mode_t gba_cmd_addr_mode;
    burner_gba_cmd_data_lane_t gba_cmd_data_lane;
} burner_cart_ctx_t;

typedef enum {
    BURNER_SPI_CS_MODE_0 = 0, /* {spi_cs1,spi_cs0}=10 */
    BURNER_SPI_CS_MODE_1,     /* {spi_cs1,spi_cs0}=01 */
    BURNER_SPI_CS_MODE_2,     /* {spi_cs1,spi_cs0}=00 */
} burner_spi_cs_mode_t;

_Static_assert(MORI_PIN_MCU_SPI_CS < 32, "SPI CS0 must be on GPIO0-31");
_Static_assert(MORI_PIN_MCU_SPI_CS1 < 32, "SPI CS1 must be on GPIO0-31");

typedef enum {
    BURNER_JOB_WRITE_ROM = 0,
    BURNER_JOB_READ_ROM,
    BURNER_JOB_VERIFY_ROM,
    BURNER_JOB_ERASE_ROM,
    BURNER_JOB_WRITE_RAM,
    BURNER_JOB_READ_RAM,
    BURNER_JOB_VERIFY_RAM,
} burner_job_mode_t;

typedef enum {
    BURNER_CART_MODE_MBC5 = 0,
    BURNER_CART_MODE_GBA,
} burner_cart_mode_t;

typedef struct {
    burner_cart_mode_t cart_mode;
    bool cfi_ok;
    uint32_t device_size;
    uint32_t sector_size;
    uint16_t buffer_write_bytes;
    uint32_t sector_count;
    uint32_t ppb_needs_unlock_before;
    uint32_t ppb_needs_unlock_after;
    uint16_t gba_lock_status;
    uint8_t mbc5_lock_status;
    uint8_t gba_id[8];
    uint8_t mbc5_id[4];
} burner_ppb_unlock_report_t;

typedef enum {
    BURNER_WRITE_PATH_DIRECT = 0,
    BURNER_WRITE_PATH_PSRAM,
    BURNER_WRITE_PATH_PIPELINE,
} burner_write_path_t;

typedef enum {
    BURNER_CORE_AFFINITY_AUTO = 0,
    BURNER_CORE_AFFINITY_CPU0,
    BURNER_CORE_AFFINITY_CPU1,
} burner_core_affinity_t;

typedef enum {
    BURNER_STATE_IDLE = 0,
    BURNER_STATE_RECEIVING,
    BURNER_STATE_BURNING,
    BURNER_STATE_DONE,
    BURNER_STATE_ERROR,
    BURNER_STATE_CANCELLED,
} burner_state_t;

typedef struct {
    burner_core_affinity_t erase_core;
    burner_core_affinity_t tf_core;
    burner_core_affinity_t psram_core;
} burner_core_config_t;

typedef struct {
    burner_state_t state;
    int progress;
    uint32_t total_bytes;
    uint32_t processed_bytes;
    uint64_t speed_start_us;
    uint64_t speed_warmup_until_us;
    uint64_t speed_last_us;
    uint32_t speed_start_bytes;
    uint32_t speed_last_bytes;
    uint32_t speed_current_bps;
    uint32_t speed_avg_bps;
    uint32_t speed_min_bps;
    uint32_t speed_max_bps;
    uint64_t task_start_us;
    uint64_t task_elapsed_us;
    uint32_t erase_sector_count;
    uint32_t erase_sector_size;
    uint32_t erase_phase_total_sectors;
    uint32_t erase_phase_done_sectors;
    uint64_t erase_start_us;
    uint64_t erase_elapsed_us;
    uint64_t write_start_us;
    uint64_t write_elapsed_us;
    uint32_t tf_to_psram_speed_current_bps;
    uint32_t tf_to_psram_speed_avg_bps;
    uint32_t tf_to_psram_speed_min_bps;
    uint32_t tf_to_psram_speed_max_bps;
    uint32_t dump_read_speed_current_bps;
    uint32_t dump_read_speed_avg_bps;
    uint32_t dump_read_speed_min_bps;
    uint32_t dump_read_speed_max_bps;
    uint32_t dump_write_speed_current_bps;
    uint32_t dump_write_speed_avg_bps;
    uint32_t dump_write_speed_min_bps;
    uint32_t dump_write_speed_max_bps;
    uint32_t mbc5_buffer_write_ok_count;
    uint32_t mbc5_buffer_fallback_count;
    uint32_t tf_to_psram_total_bytes;
    uint64_t tf_to_psram_total_us;
    uint32_t dump_read_total_bytes;
    uint64_t dump_read_total_us;
    uint32_t dump_write_total_bytes;
    uint64_t dump_write_total_us;
    uint64_t dump_wait_total_us;
    uint64_t dump_finalize_total_us;
    uint32_t verify_sample_addr;
    uint8_t verify_sample_file_byte;
    uint8_t verify_sample_cart_byte;
    bool verify_sample_valid;
    bool verify_sample_equal;
    burner_cart_mode_t probe_cart_mode;
    bool probe_valid;
    bool probe_cfi_ok;
    bool probe_gba_multi;
    bool probe_gba_force_multi;
    uint32_t probe_device_size;
    uint32_t probe_sector_size;
    uint16_t probe_buffer_write_bytes;
    uint8_t probe_id[8];
    char rom_name[BURNER_FILE_NAME_LEN];
    char rom_path[BURNER_FILE_PATH_LEN];
    char message[96];
    bool erase_phase_planned;
    bool erase_phase_active;
    bool cancel_requested;
} burner_status_t;

typedef struct {
    char safe_name[BURNER_FILE_NAME_LEN];
    char full_path[BURNER_FILE_PATH_LEN];
    uint32_t effective_size;
    uint32_t psram_mb;
    uint32_t mbc5_chunk_kb;
    bool gba_force_no_cfi;
} burner_task_start_result_t;

typedef struct {
    burner_job_mode_t mode;
    burner_cart_mode_t cart_mode;
    burner_write_path_t write_path;
    uint32_t mbc5_program_chunk_bytes;
    uint32_t read_chunk_bytes;
    uint32_t psram_window_bytes;
    char rom_name[BURNER_FILE_NAME_LEN];
    char rom_path[BURNER_FILE_PATH_LEN];
    uint32_t addr_begin;
    uint32_t total_bytes;
    bool ram_fram;
    uint8_t ram_latency;
    bool gba_force_multi;
    bool gba_force_no_cfi;
    bool task_with_caps;
} burner_task_param_t;

typedef struct {
    int fd;
    const uint8_t *src;
    size_t bytes;
    size_t written;
    esp_err_t err;
    bool stop;
    bool running;
    TaskHandle_t task;
    SemaphoreHandle_t request;
    SemaphoreHandle_t done;
} burner_tf_writer_ctx_t;

typedef struct {
    char page_title[WEB_LANG_TEXT_MAX];
    char page_header[WEB_LANG_TEXT_MAX];
    char page_tip[WEB_LANG_TEXT_MAX];
    char business_title[WEB_LANG_TEXT_MAX];
    char btn_open_business[WEB_LANG_TEXT_MAX];
    char business_tip[WEB_LANG_TEXT_MAX];
    char recovery_title[WEB_LANG_TEXT_MAX];
    char recovery_tip[WEB_LANG_TEXT_MAX];
    char btn_upload_main[WEB_LANG_TEXT_MAX];
    char system_migrate_title[WEB_LANG_TEXT_MAX];
    char system_migrate_tip[WEB_LANG_TEXT_MAX];
    char btn_system_migrate[WEB_LANG_TEXT_MAX];
    char system_deploy_title[WEB_LANG_TEXT_MAX];
    char system_deploy_tip[WEB_LANG_TEXT_MAX];
    char btn_system_deploy[WEB_LANG_TEXT_MAX];
    char firmware_title[WEB_LANG_TEXT_MAX];
    char firmware_tip[WEB_LANG_TEXT_MAX];
    char btn_upload_firmware[WEB_LANG_TEXT_MAX];
    char firmware_idle[WEB_LANG_TEXT_MAX];
    char upload_idle[WEB_LANG_TEXT_MAX];
    char usb_title[WEB_LANG_TEXT_MAX];
    char usb_tip[WEB_LANG_TEXT_MAX];
    char btn_enable_usb[WEB_LANG_TEXT_MAX];
    char btn_disable_usb[WEB_LANG_TEXT_MAX];
    char btn_refresh_storage[WEB_LANG_TEXT_MAX];
    char storage_loading[WEB_LANG_TEXT_MAX];
    char device_title[WEB_LANG_TEXT_MAX];
    char btn_refresh_device[WEB_LANG_TEXT_MAX];
    char device_loading[WEB_LANG_TEXT_MAX];
    char msg_select_main[WEB_LANG_TEXT_MAX];
    char msg_select_deploy_zip[WEB_LANG_TEXT_MAX];
    char msg_select_firmware[WEB_LANG_TEXT_MAX];
    char msg_deploying_prefix[WEB_LANG_TEXT_MAX];
    char msg_deploy_success_prefix[WEB_LANG_TEXT_MAX];
    char msg_deploy_failed_prefix[WEB_LANG_TEXT_MAX];
    char msg_uploading_firmware_prefix[WEB_LANG_TEXT_MAX];
    char msg_firmware_success_prefix[WEB_LANG_TEXT_MAX];
    char msg_uploading_prefix[WEB_LANG_TEXT_MAX];
    char msg_upload_success_prefix[WEB_LANG_TEXT_MAX];
    char msg_upload_failed_prefix[WEB_LANG_TEXT_MAX];
    char msg_storage_status_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_set_mode_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_device_info_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_applying[WEB_LANG_TEXT_MAX];
    char language_title[WEB_LANG_TEXT_MAX];
    char language_tip[WEB_LANG_TEXT_MAX];
    char btn_read_lang_list[WEB_LANG_TEXT_MAX];
    char btn_apply_language[WEB_LANG_TEXT_MAX];
    char language_idle[WEB_LANG_TEXT_MAX];
    char language_loading[WEB_LANG_TEXT_MAX];
    char language_none[WEB_LANG_TEXT_MAX];
    char ip5306_title[WEB_LANG_TEXT_MAX];
    char ip5306_tip[WEB_LANG_TEXT_MAX];
    char btn_read_ip5306_ini[WEB_LANG_TEXT_MAX];
    char btn_save_ip5306_ini[WEB_LANG_TEXT_MAX];
    char ip5306_idle[WEB_LANG_TEXT_MAX];
    char ip5306_loading[WEB_LANG_TEXT_MAX];
    char msg_lang_select_required[WEB_LANG_TEXT_MAX];
    char msg_lang_list_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_lang_apply_success_prefix[WEB_LANG_TEXT_MAX];
    char msg_lang_apply_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_ip5306_load_ok[WEB_LANG_TEXT_MAX];
    char msg_ip5306_load_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_ip5306_save_ok_prefix[WEB_LANG_TEXT_MAX];
    char msg_ip5306_save_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_upload_item_ok[WEB_LANG_TEXT_MAX];
    char msg_upload_item_fail[WEB_LANG_TEXT_MAX];
    char msg_http_error_prefix[WEB_LANG_TEXT_MAX];
    char msg_invalid_json_prefix[WEB_LANG_TEXT_MAX];
} burner_lang_pack_t;

typedef struct {
    char language_version[WEB_LANG_VERSION_MAX];
    char language_ini[WEB_LANG_FILE_NAME_MAX];
} burner_lang_meta_t;

typedef struct {
    char *full_path;
    char *zip_path;
    uint32_t size;
    uint32_t crc32;
    uint32_t local_offset;
} burner_zip_item_t;

typedef struct {
    burner_zip_item_t *items;
    size_t count;
    size_t cap;
} burner_zip_item_list_t;

typedef struct {
    httpd_req_t *req;
    uint32_t offset;
} burner_zip_stream_t;

typedef struct {
    char zip_name[ZIP_ENTRY_NAME_MAX];
    uint16_t gp_flags;
    uint16_t method;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_offset;
    bool is_dir;
} burner_zip_central_entry_t;

typedef struct {
    burner_zip_central_entry_t *items;
    size_t count;
    size_t cap;
} burner_zip_central_list_t;

extern const char *s_upload_html_override;
extern ws_receive_cb s_receive_cb;
extern httpd_handle_t s_httpd;
extern SemaphoreHandle_t s_status_lock;
extern SemaphoreHandle_t s_spi_lock;
extern TaskHandle_t s_burn_task;
extern TaskHandle_t s_bacon_idle_task;
extern spi_device_handle_t s_mcu_spi;
extern bool s_mcu_spi_ready;
extern uint8_t *s_mcu_spi_tx_shadow;
extern uint8_t *s_mcu_spi_rw_shadow;
extern const uint32_t s_mcu_spi_clock_hz;
extern uint32_t s_mcu_spi_actual_hz;
extern burner_core_config_t s_burn_core_cfg;
extern uint8_t s_burn_erase_always;
extern uint8_t s_gba_fixed_erase_window_enabled;
extern uint8_t s_mbc5_power_5v_enabled;
extern TickType_t s_bacon_last_active_tick;
extern bool s_bacon_idle_powered_down;
extern burner_cart_ctx_t s_cart_ctx;
extern burner_status_t s_status;
extern const char *const s_system_migrate_rel_dirs[BURNER_SYSTEM_MIGRATE_REL_DIR_COUNT];
extern const char s_base_settings_html[];

esp_err_t burner_reject_if_tf_busy(httpd_req_t *req);
void burner_schedule_restart(void);
void burner_bacon_mark_activity_locked(void);
void burner_bacon_idle_task_entry(void *param);
esp_err_t burner_bacon_gba_power_cmd(bool power_5v, bool power_3v3);
esp_err_t burner_spi_transfer_active(const uint8_t *tx, uint8_t *rx, size_t len);
void burner_spi_release_cs(void);
esp_err_t burner_spi_config_get_handler(httpd_req_t *req);
esp_err_t burner_core_config_get_handler(httpd_req_t *req);
esp_err_t burner_core_config_post_handler(httpd_req_t *req);
uint8_t *burner_spi_alloc_rw_buffer(size_t len, bool *needs_free);
uint8_t *burner_spi_alloc_tx_buffer(size_t len, bool *needs_free);
esp_err_t burner_tf_write_exact(int fd, const uint8_t *src, size_t bytes);
void burner_tf_writer_task(void *arg);
esp_err_t burner_tf_writer_start(burner_tf_writer_ctx_t *ctx, int fd);
esp_err_t burner_tf_writer_submit(burner_tf_writer_ctx_t *ctx, const uint8_t *src, size_t bytes);
esp_err_t burner_tf_writer_wait(burner_tf_writer_ctx_t *ctx);
void burner_tf_writer_stop(burner_tf_writer_ctx_t *ctx);
const char *burner_mbc5_chip_name(const uint8_t id[4]);
const char *burner_rom_dump_ext_for_mode(burner_cart_mode_t cart_mode);
bool burner_task_is_running_snapshot(void);
bool burner_build_full_path(const char *rel_path, char *full_path, size_t full_path_len);
esp_err_t burner_mkdirs_rel(const char *rel_path);
void burner_build_output_timestamp(char *buf, size_t buf_len);
esp_err_t burner_backend_init(void);
void burner_status_update(
    burner_state_t state,
    int progress,
    uint32_t processed,
    uint32_t total,
    const char *message,
    const char *rom_name,
    const char *rom_path);
esp_err_t burner_spi_init(void);
void burner_spi_lock_take(void);
void burner_spi_lock_give(void);
void burner_bacon_restore_3v3_power(void);
esp_err_t burner_bacon_gba_read_block(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card);
esp_err_t burner_spi_prepare_burn_mbc5(const burner_task_param_t *job);
esp_err_t burner_spi_prepare_burn_gba(const burner_task_param_t *job);
esp_err_t burner_bacon_mbc5_read_block(uint8_t *out, size_t len, uint32_t offset);
const char *burner_state_to_str(burner_state_t state);
bool burner_parse_bool_text(const char *value, bool *out);
bool burner_parse_size_text(const char *value, uint32_t *out_bytes);
bool burner_parse_u32_text(const char *value, uint32_t *out_value);
bool burner_parse_cart_mode_text(const char *text, burner_cart_mode_t *mode_out);
const char *burner_write_path_to_str(burner_write_path_t path);
bool burner_parse_write_path_text(const char *text, burner_write_path_t *path_out);
uint32_t burner_clamp_mbc5_program_chunk_bytes(uint32_t bytes);
uint32_t burner_mbc5_program_chunk_kb_to_bytes(uint32_t kb);
bool burner_is_supported_dump_chunk_bytes(uint32_t bytes);
uint32_t burner_dump_chunk_kb_to_bytes(uint32_t kb);
uint32_t burner_dump_chunk_bytes_to_kb(uint32_t bytes);
uint32_t burner_psram_auto_window_mb(void);
uint32_t burner_psram_window_mb_to_bytes(uint32_t mb);
uint32_t burner_psram_window_bytes_to_mb(uint32_t bytes);
const char *burner_core_affinity_to_str(burner_core_affinity_t affinity);
bool burner_parse_core_affinity_text(const char *text, burner_core_affinity_t *affinity_out);
BaseType_t burner_core_affinity_to_task_core_id(burner_core_affinity_t affinity);
BaseType_t burner_create_task_with_affinity(
    TaskFunction_t task_fn,
    const char *name,
    uint32_t stack_bytes,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *task_out,
    burner_core_affinity_t affinity);
bool burner_get_mbc5_slot_range(bool ram_range, uint32_t slot, uint32_t *addr_begin, uint32_t *addr_end);
esp_err_t burner_apply_mbc5_slot_limit(
    bool ram_range,
    uint32_t slot,
    uint32_t requested_size,
    uint32_t *addr_begin,
    uint32_t *effective_size);
esp_err_t burner_apply_gba_slot_limit(
    uint32_t slot,
    uint32_t requested_size,
    uint32_t *addr_begin,
    uint32_t *effective_size,
    bool *force_multi);
bool burner_parse_ram_mode(const char *ram_type_text, bool *fram_mode);
esp_err_t burner_start_write_from_tf(
    const char *raw_name,
    burner_cart_mode_t cart_mode,
    uint32_t slot,
    burner_write_path_t write_path,
    uint32_t psram_mb,
    uint32_t mbc5_chunk_kb,
    bool gba_force_no_cfi,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len);
esp_err_t burner_start_verify_from_tf(
    const char *raw_name,
    burner_cart_mode_t cart_mode,
    uint32_t slot,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len);
esp_err_t burner_start_ram_write_from_tf(
    const char *raw_name,
    uint32_t slot,
    bool fram_mode,
    uint8_t ram_latency,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len);
esp_err_t burner_start_ram_verify_from_tf(
    const char *raw_name,
    uint32_t slot,
    bool fram_mode,
    uint8_t ram_latency,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len);
bool burner_status_tracks_speed(burner_state_t state);
bool burner_status_is_operation_active_state(burner_state_t state);
bool burner_status_is_operation_active_locked(void);
void burner_cancel_reset_locked(void);
void burner_cancel_reset(void);
bool burner_cancel_is_requested(void);
esp_err_t burner_cancel_poll(void);
bool burner_cancel_request(void);
uint32_t burner_us_to_ms_clamped(uint64_t us);
void burner_status_speed_reset_locked(void);
void burner_status_verify_sample_reset_locked(void);
void burner_status_phase_reset_locked(void);
void burner_status_probe_reset_locked(void);
void burner_status_set_probe_info(
    burner_cart_mode_t cart_mode,
    const uint8_t *id,
    size_t id_len,
    uint32_t device_size,
    uint32_t sector_size,
    uint16_t buffer_write_bytes,
    bool cfi_ok,
    bool gba_multi,
    bool gba_force_multi);
void burner_status_set_verify_sample(uint32_t addr, uint8_t file_byte, uint8_t cart_byte, bool equal);
void burner_status_plan_erase_phase(uint32_t total_sectors, uint32_t sector_size);
void burner_status_begin_erase_phase(uint32_t total_sectors, uint32_t sector_size);
void burner_status_advance_erase_phase(uint32_t sectors_done);
void burner_status_mark_erase_begin(void);
void burner_status_mark_erase_end(void);
void burner_status_mark_write_begin(void);
void burner_status_mark_write_end(void);
void burner_status_mark_task_begin(void);
void burner_status_mark_task_end(void);
uint32_t burner_erase_sector_count_from_bytes(uint64_t bytes, uint32_t sector_size);
uint32_t burner_erase_sector_count_from_range(uint32_t addr_begin, uint32_t addr_end, uint32_t sector_size);
void burner_status_record_erase_sectors(uint32_t sector_count, uint32_t sector_size);
void burner_status_record_speed_sample_locked(
    uint32_t bytes,
    uint64_t elapsed_us,
    uint32_t *current_bps,
    uint32_t *avg_bps,
    uint32_t *min_bps,
    uint32_t *max_bps,
    uint32_t *total_bytes,
    uint64_t *total_us);
void burner_status_record_tf_to_psram_copy(uint32_t bytes, uint64_t elapsed_us);
void burner_status_record_dump_read(uint32_t bytes, uint64_t elapsed_us);
void burner_status_record_dump_write(uint32_t bytes, uint64_t elapsed_us);
void burner_status_record_elapsed_total(uint64_t *total_us, uint64_t elapsed_us);
void burner_status_record_dump_wait(uint64_t elapsed_us);
void burner_status_record_dump_finalize(uint64_t elapsed_us);
void burner_status_record_mbc5_buffer_write(bool fallback);
void burner_status_speed_update_locked(
    burner_state_t prev_state,
    uint32_t prev_total,
    uint32_t processed,
    uint32_t total);
void burner_status_reset(void);
int burner_calc_progress_percent_u64(uint64_t processed, uint64_t total);
void burner_status_update(
    burner_state_t state,
    int progress,
    uint32_t processed,
    uint32_t total,
    const char *message,
    const char *rom_name,
    const char *rom_path);
void burner_status_snapshot(burner_status_t *out);
bool burner_sanitize_filename(const char *input, char *output, size_t output_len);
bool burner_validate_file_name(const char *input, char *output, size_t output_len);
bool burner_wallclock_time_valid(time_t now);
bool burner_get_wallclock_time(time_t *now_out, struct tm *tm_out);
bool burner_extract_ascii_cart_title(
    const uint8_t *raw,
    size_t raw_len,
    char *title,
    size_t title_len);
bool burner_apply_current_file_mtime(const char *path, time_t *applied_time_out);
long long burner_fixup_file_mtime_for_api(const char *path, const struct stat *st);
bool burner_try_probe_cart_title(
    burner_cart_mode_t cart_mode,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool gba_force_multi,
    char *title,
    size_t title_len);
void burner_build_default_dump_name(
    burner_cart_mode_t cart_mode,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool gba_force_multi,
    char *raw_name,
    size_t raw_name_len);
void burner_build_output_timestamp(char *buf, size_t buf_len);
bool burner_build_mbc5_verify_log_rel_path(
    const uint8_t id[4],
    char *rel_path,
    size_t rel_path_len);
FILE *burner_open_mbc5_verify_log(
    const burner_task_param_t *job,
    char *log_rel,
    size_t log_rel_len);
bool burner_build_indexed_file_name(
    const char *preferred_name,
    uint32_t index,
    char *output,
    size_t output_len);
esp_err_t burner_resolve_unique_output_path(
    const char *dir_path,
    const char *preferred_name,
    char *resolved_name,
    size_t resolved_name_len,
    char *resolved_full_path,
    size_t resolved_full_path_len);
const char *burner_rom_dump_ext_for_mode(burner_cart_mode_t cart_mode);
bool burner_force_file_extension(
    const char *input_name,
    const char *target_ext,
    char *output,
    size_t output_len);
bool burner_hex_to_nibble(char ch, uint8_t *nibble);
bool burner_url_decode(const char *src, char *dst, size_t dst_len);
bool burner_get_query_arg(
    httpd_req_t *req,
    const char *key,
    char *out,
    size_t out_len,
    bool required);
esp_err_t burner_send_json(httpd_req_t *req, const char *json_text);
esp_err_t burner_read_request_body(
    httpd_req_t *req,
    char *out,
    size_t out_len,
    size_t *actual_len);
const char *burner_json_locate_value(const char *json, const char *key);
bool burner_json_get_string(const char *json, const char *key, char *out, size_t out_len);
bool burner_json_get_bool(const char *json, const char *key, bool *out);
bool burner_json_get_int(const char *json, const char *key, int *out);
bool burner_normalize_rel_path(
    const char *input,
    char *output,
    size_t output_len,
    bool allow_empty);
bool burner_json_escape(const char *src, char *dst, size_t dst_len);
const char *burner_basename(const char *path);
esp_err_t burner_remove_recursive(const char *full_path);
bool burner_uri_to_web_rel_path(const char *uri, char *rel_path, size_t rel_path_len);
esp_err_t burner_send_static_file(httpd_req_t *req, const char *rel_path);
void burner_lang_load(
    burner_lang_pack_t *lang,
    burner_lang_meta_t *meta,
    bool *system_loaded,
    bool *lang_loaded);
esp_err_t burner_send_lang_string_chunk(
    httpd_req_t *req,
    const char *key,
    const char *value,
    bool trailing_comma);
esp_err_t burner_bacon_gba_prepare_power(void);
esp_err_t burner_bacon_mbc5_prepare_power(void);
const char *burner_gba_cmd_addr_mode_name(burner_gba_cmd_addr_mode_t mode);
const char *burner_gba_cmd_data_lane_name(burner_gba_cmd_data_lane_t lane);
void burner_format_hex_bytes(const uint8_t *data, size_t len, char *out, size_t out_len);
const char *burner_gba_chip_name(const uint8_t id[8]);
bool burner_gba_id_looks_like_rom_header(const uint8_t id[8]);
esp_err_t burner_bacon_gba_probe_locked(
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out);
bool burner_mbc5_geometry_from_id(
    const uint8_t id[4],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes);
esp_err_t burner_bacon_mbc5_get_id(uint8_t id_out[4]);
esp_err_t burner_bacon_mbc5_get_cfi(
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes);
esp_err_t burner_bacon_gba_verify_read_block_hoststyle(
    uint8_t *out,
    size_t len,
    uint32_t offset,
    bool is_multi_card);
esp_err_t burner_bacon_mbc5_read_block_hoststyle(uint8_t *out, size_t len, uint32_t offset);
esp_err_t burner_ensure_rom_dir(void);
esp_err_t burner_probe_cart_capacity_bytes(burner_cart_mode_t cart_mode, uint32_t *device_size_out);
esp_err_t burner_cart_unlock_ppb_locked(
    burner_cart_mode_t cart_mode,
    burner_ppb_unlock_report_t *report);
esp_err_t burner_ensure_dump_dir(void);
esp_err_t burner_ensure_rom_output_dir(void);
esp_err_t burner_start_task_ex(
    burner_job_mode_t mode,
    burner_cart_mode_t cart_mode,
    burner_write_path_t write_path,
    bool gba_force_multi,
    bool gba_force_no_cfi,
    uint32_t mbc5_program_chunk_bytes,
    uint32_t read_chunk_bytes,
    uint32_t psram_window_bytes,
    const char *rom_name,
    const char *rom_path,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool ram_fram,
    uint8_t ram_latency);
esp_err_t burner_start_task(
    burner_job_mode_t mode,
    const char *rom_name,
    const char *rom_path,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool ram_fram,
    uint8_t ram_latency);

#endif


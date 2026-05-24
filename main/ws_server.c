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
#include "esp_attr.h"
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
#include "burner_nor_db.h"
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
#define BURN_MBC5_ROM_BANK_BYTES (16U * 1024U)
#define BURN_GBC_READ_SPI_OVERHEAD_BYTES 4U
#define BURN_GBC_READ_SPI_BYTES_PER_DATA_BYTE 3U
#define BURN_CART_WRITE_MAX_BYTES (BURNER_SPI_MAX_XFER / 6U)
#define BURN_CART_READ_MAX_BYTES ((BURNER_SPI_MAX_XFER - BURN_GBC_READ_SPI_OVERHEAD_BYTES) / BURN_GBC_READ_SPI_BYTES_PER_DATA_BYTE)
#define BURN_MBC5_PROGRAM_CHUNK_BYTES BURN_MBC5_ROM_BANK_BYTES
#define BURN_MBC5_PROGRAM_CHUNK_MIN_BYTES (4U * 1024U)
#define BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES (128U * 1024U)
#define BURN_MBC5_DUMP_CHUNK_BYTES (64U * 1024U)
#define BURN_ROM_DUMP_CHUNK_MIN_BYTES (32U * 1024U)
#define BURN_ROM_DUMP_CHUNK_MAX_BYTES (256U * 1024U)
#define BURN_ERASE_ALWAYS_DEFAULT 0U
#define BURN_BLANK_HEAD_CHECK_BYTES 512U
#define BURN_BLANK_SAMPLE_BYTES 2U
#define BURN_BLANK_SAMPLE_POINTS 4U
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
#define BURN_TASK_STACK_BYTES (16U * 1024U)
#define BURNER_TASK_CORE_ID 1
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
#define BURNER_GBA_SAVE_HEADER_SCAN_BYTES (64u * 1024u)
#define BURNER_GBA_ANALYSIS_HEAD_BYTES (1u * 1024u * 1024u)
#define BURNER_GBA_SAVE_SCAN_STEP_BYTES 0x1000u
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

typedef struct {
    char esc_path[TF_PATH_LEN_MAX * 2 + 8];
    char head[TF_PATH_LEN_MAX * 2 + 72];
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
#define BURNER_CPU_YIELD_INTERVAL_US 20000ULL
#define BURNER_PROBE_SCAN_WINDOW_BYTES (64u * 1024u)
#define BURNER_ROM_POLL_TIMEOUT_MS 2000
#define BURNER_ROM_POLL_INTERVAL_US 50
#define BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS 20000U
#define BURNER_ROM_ERASE_TIMEOUT_MAX_MS (5U * 60U * 1000U)
#define BURNER_ROM_CHIP_ERASE_TIMEOUT_MS BURNER_ROM_ERASE_TIMEOUT_MAX_MS
#define BURNER_GBA_FALLBACK_DEVICE_SIZE BURN_GBA_LINEAR_ADDR_BYTES
#define BURNER_GBA_FALLBACK_SECTOR_SIZE (128U * 1024U)
#define BURNER_GBA_FALLBACK_BUFFER_WRITE_BYTES 0U
#define BURNER_GBA_INTEL_RUNTIME_BUFFER_DEFAULT_BYTES 512U
#define BURNER_GBA_INTEL_RUNTIME_BUFFER_MIN_BYTES 64U
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

#define BURNER_NOR_GEOMETRY_REGION_MAX 4U

typedef struct {
    uint32_t addr_begin;
    uint32_t addr_end;
    uint32_t sector_size;
} burner_nor_region_t;

typedef struct {
    uint8_t region_count;
    uint8_t reserved[3];
    uint32_t uniform_sector_size;
    uint32_t smallest_sector_size;
    uint32_t largest_sector_size;
    burner_nor_region_t regions[BURNER_NOR_GEOMETRY_REGION_MAX];
} burner_nor_geometry_t;

typedef struct {
    uint8_t region_index;
    uint8_t reserved[3];
    uint32_t addr_begin;
    uint32_t addr_end;
    uint32_t sector_size;
} burner_nor_region_cursor_t;

typedef struct {
    bool prepared;
    uint16_t current_bank;
    uint16_t buffer_write_bytes;
    uint16_t program_buffer_write_bytes;
    uint32_t sector_size;
    uint32_t device_size;
    burner_nor_geometry_t geometry;
    uint8_t mbc5_id[4];
    burner_nor_cmdset_t gba_cmdset;
    burner_gba_cmd_addr_mode_t gba_cmd_addr_mode;
    burner_gba_cmd_data_lane_t gba_cmd_data_lane;
    bool d0d1_known;   /* D0/D1 detection completed */
    bool d0d1_swapped; /* D0/D1 data lines swapped */
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
    BURNER_JOB_WRITE_GBA_SAVE_NEW,
    BURNER_JOB_READ_GBA_SAVE_NEW,
    BURNER_JOB_VERIFY_GBA_SAVE_NEW,
} burner_job_mode_t;

typedef enum {
    BURNER_GBA_SAVE_TYPE_SRAM = 0,
    BURNER_GBA_SAVE_TYPE_EEPROM,
    BURNER_GBA_SAVE_TYPE_FLASH,
    BURNER_GBA_SAVE_TYPE_BATTERYLESS,
} burner_gba_save_type_t;

typedef enum {
    BURNER_GBA_SRAM_PATCH_NONE = 0,
    BURNER_GBA_SRAM_PATCH_GBATA,
    BURNER_GBA_SRAM_PATCH_FLASH1M_REPRO,
} burner_gba_sram_patch_kind_t;

typedef enum {
    BURNER_CART_MODE_MBC5 = 0,
    BURNER_CART_MODE_GBA,
} burner_cart_mode_t;

typedef enum {
    BURNER_GB_MAPPER_UNKNOWN = 0,
    BURNER_GB_MAPPER_MBC3,
    BURNER_GB_MAPPER_MBC5,
} burner_gb_mapper_t;

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
    bool gba_d0d1_known;
    bool gba_d0d1_swapped;
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
    bool active;
    bool multi_card;
    bool erase_always;
    bool pre_erased_valid;
    burner_nor_region_cursor_t cursor;
    uint32_t range_end;
    uint32_t erased_sector_addr;
    uint32_t pre_erased_sector_addr;
} burner_gba_sector_erase_ctx_t;

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
    uint32_t erase_phase_total_bytes;
    uint32_t erase_phase_done_bytes;
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
    bool probe_gba_d0d1_known;
    bool probe_gba_d0d1_swapped;
    burner_gba_save_type_t probe_gba_save_type;
    uint32_t probe_gba_save_size;
    bool probe_gba_save_detected;
    burner_gba_sram_patch_kind_t probe_gba_sram_patch_kind;
    bool probe_gba_sram_patch_scanned;
    bool probe_gba_sram_patch_detected;
    uint32_t probe_device_size;
    uint32_t probe_sector_size;
    uint16_t probe_buffer_write_bytes;
    uint8_t probe_id[8];
    char probe_chip_name[48];
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
    bool erase_always;
    uint32_t mbc5_program_chunk_bytes;
    uint32_t read_chunk_bytes;
    uint32_t psram_window_bytes;
    char rom_name[BURNER_FILE_NAME_LEN];
    char rom_path[BURNER_FILE_PATH_LEN];
    uint32_t addr_begin;
    uint32_t total_bytes;
    bool ram_fram;
    uint8_t ram_latency;
    burner_gba_save_type_t gba_save_type;
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

const char *s_upload_html_override = NULL;
ws_receive_cb s_receive_cb = NULL;
httpd_handle_t s_httpd = NULL;
SemaphoreHandle_t s_status_lock = NULL;
SemaphoreHandle_t s_spi_lock = NULL;
TaskHandle_t s_burn_task = NULL;
TaskHandle_t s_bacon_idle_task = NULL;
spi_device_handle_t s_mcu_spi = NULL;
bool s_mcu_spi_ready = false;
static DMA_ATTR uint8_t s_mcu_spi_tx_shadow_storage[BURNER_SPI_STREAM_CHUNK_BYTES];
static DMA_ATTR uint8_t s_mcu_spi_rw_shadow_storage[BURNER_SPI_STREAM_CHUNK_BYTES];
uint8_t *s_mcu_spi_tx_shadow = s_mcu_spi_tx_shadow_storage;
uint8_t *s_mcu_spi_rw_shadow = s_mcu_spi_rw_shadow_storage;
static const size_t s_mcu_spi_tx_shadow_size = sizeof(s_mcu_spi_tx_shadow_storage);
static const size_t s_mcu_spi_rw_shadow_size = sizeof(s_mcu_spi_rw_shadow_storage);
const uint32_t s_mcu_spi_clock_hz = BURNER_SPI_CLOCK_HZ;
uint32_t s_mcu_spi_actual_hz = BURNER_SPI_CLOCK_HZ;
burner_core_config_t s_burn_core_cfg = {
    .erase_core = BURNER_CORE_AFFINITY_CPU1,
    .tf_core = BURNER_CORE_AFFINITY_CPU1,
    .psram_core = BURNER_CORE_AFFINITY_CPU1,
};
uint8_t s_burn_erase_always = BURN_ERASE_ALWAYS_DEFAULT;
uint8_t s_gba_fixed_erase_window_enabled = BURN_GBA_FIXED_ERASE_WINDOW_ENABLED_DEFAULT;
uint8_t s_mbc5_power_5v_enabled = 1;
TickType_t s_bacon_last_active_tick = 0;
bool s_bacon_idle_powered_down = false;
burner_cart_ctx_t s_cart_ctx = {
    .prepared = false,
    .current_bank = UINT16_MAX,
    .buffer_write_bytes = 0,
    .program_buffer_write_bytes = 0,
    .sector_size = 0,
    .device_size = 0,
    .geometry = {0},
    .mbc5_id = {0},
    .gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN,
    .gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD,
    .gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW,
};
static burner_gba_sector_erase_ctx_t s_gba_sector_erase_ctx = {0};
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
const char *burner_rom_dump_ext_for_mode(burner_cart_mode_t cart_mode);
bool burner_task_is_running_snapshot(void);
bool burner_build_full_path(const char *rel_path, char *full_path, size_t full_path_len);
esp_err_t burner_mkdirs_rel(const char *rel_path);
void burner_build_output_timestamp(char *buf, size_t buf_len);
void burner_status_update(
    burner_state_t state,
    int progress,
    uint32_t processed,
    uint32_t total,
    const char *message,
    const char *rom_name,
    const char *rom_path);
esp_err_t burner_spi_init(void);
void burner_task_yield_if_due(void);
void burner_spi_lock_take(void);
void burner_spi_lock_give(void);
void burner_bacon_restore_3v3_power(void);
esp_err_t burner_bacon_gba_read_block(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card);
esp_err_t burner_spi_prepare_burn_mbc5(const burner_task_param_t *job);
esp_err_t burner_spi_prepare_burn_gba(const burner_task_param_t *job);
esp_err_t burner_probe_cart_capacity_bytes(burner_cart_mode_t cart_mode, uint32_t *device_size_out);
esp_err_t burner_bacon_mbc5_read_block(uint8_t *out, size_t len, uint32_t offset);
static esp_err_t burner_bacon_mbc5_read_block_program_window(uint8_t *out, size_t len, uint32_t offset);
uint32_t burner_psram_auto_window_mb(void);
static bool burner_is_gba_multi_card(const burner_task_param_t *job);
static const char *burner_gb_mapper_name(burner_gb_mapper_t mapper);
static esp_err_t burner_buffer_all_ff(const uint8_t *buf, size_t len, bool *all_ff_out);
burner_status_t s_status = {
    .state = BURNER_STATE_IDLE,
    .progress = 0,
    .total_bytes = 0,
    .processed_bytes = 0,
    .rom_name = "",
    .rom_path = "",
    .message = "idle",
    .cancel_requested = false,
};
static uint64_t s_burn_task_last_yield_us = 0;
static burner_gb_mapper_t s_gb_mapper_kind = BURNER_GB_MAPPER_UNKNOWN;
const char *const s_system_migrate_rel_dirs[] = {
    WEB_LANG_DIR_REL,
    WEB_ROOT_DIR_REL,
};

/* Host mission_mbc5.cs multi-cart ROM ranges (slot 1..17). */
const uint32_t s_mbc5_multi_rom_range[BURNER_MBC5_SLOT_MAX + 1u][2] = {
    {0x00000000u, 0x00000000u},
    {0x00000000u, 0x000FFFFFu},
    {0x00100000u, 0x001FFFFFu},
    {0x00200000u, 0x003FFFFFu},
    {0x00400000u, 0x005FFFFFu},
    {0x00600000u, 0x007FFFFFu},
    {0x00800000u, 0x009FFFFFu},
    {0x00A00000u, 0x00BFFFFFu},
    {0x00C00000u, 0x00DFFFFFu},
    {0x00E00000u, 0x00FFFFFFu},
    {0x01000000u, 0x011FFFFFu},
    {0x01200000u, 0x013FFFFFu},
    {0x01400000u, 0x015FFFFFu},
    {0x01600000u, 0x017FFFFFu},
    {0x01800000u, 0x019FFFFFu},
    {0x01A00000u, 0x01BFFFFFu},
    {0x01C00000u, 0x01DFFFFFu},
    {0x01E00000u, 0x01FFFFFFu},
};

/* Host mission_mbc5.cs multi-cart RAM ranges (slot 1..17). */
const uint32_t s_mbc5_multi_ram_range[BURNER_MBC5_SLOT_MAX + 1u][2] = {
    {0x00000u, 0x00000u},
    {0x00000u, 0x07FFFu},
    {0x00000u, 0x07FFFu},
    {0x08000u, 0x0FFFFu},
    {0x10000u, 0x17FFFu},
    {0x18000u, 0x1FFFFu},
    {0x20000u, 0x27FFFu},
    {0x28000u, 0x2FFFFu},
    {0x30000u, 0x37FFFu},
    {0x38000u, 0x3FFFFu},
    {0x40000u, 0x47FFFu},
    {0x48000u, 0x4FFFFu},
    {0x50000u, 0x57FFFu},
    {0x58000u, 0x5FFFFu},
    {0x60000u, 0x67FFFu},
    {0x68000u, 0x6FFFFu},
    {0x70000u, 0x77FFFu},
    {0x78000u, 0x7FFFFu},
};

const char s_base_settings_html[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>...</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:900px;margin:20px auto;padding:0 14px;background:#f4f7fb;color:#172032;}"
    "h2{margin:0 0 8px;}"
    ".tip{margin:0 0 14px;color:#4f5e79;font-size:13px;}"
    ".card{background:#fff;border:1px solid #d8e0ef;border-radius:10px;padding:12px;margin-bottom:12px;}"
    ".row{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0;}"
    "select{padding:8px 10px;border-radius:8px;border:1px solid #bfcbe0;background:#fff;color:#13203a;}"
    "textarea{width:100%;min-height:220px;padding:8px 10px;border-radius:8px;border:1px solid #bfcbe0;background:#fff;color:#13203a;resize:vertical;font-family:Consolas,monospace;font-size:12px;line-height:1.45;}"
    "a.btn,button{display:inline-block;padding:8px 12px;border-radius:8px;border:1px solid #bfcbe0;background:#fff;color:#13203a;text-decoration:none;cursor:pointer;}"
    "button.primary,a.btn.primary{background:#0d6efd;border-color:#0d6efd;color:#fff;}"
    "button.warn{background:#ff9f1a;border-color:#ff9f1a;color:#fff;}"
    "pre{background:#0f1726;color:#dbe7ff;border-radius:10px;padding:10px;white-space:pre-wrap;word-break:break-word;min-height:72px;}"
    "</style></head><body>"
    "<h2 id='txt_page_header'>...</h2>"
    "<p id='txt_page_tip' class='tip'>...</p>"
    "<div class='card'>"
    "<h3 id='txt_business_title'>...</h3>"
    "<div class='row'>"
    "<a id='btn_open_business' class='btn primary' href='/tf'>...</a>"
    "</div>"
    "<p id='txt_business_tip' class='tip'>...</p>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_recovery_title'>...</h3>"
    "<p id='txt_recovery_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<input id='main_file' type='file' multiple style='flex:1;min-width:240px'>"
    "<button id='btn_main_upload' class='primary'>...</button>"
    "</div>"
    "<pre id='main_upload'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_system_migrate_title'>...</h3>"
    "<p id='txt_system_migrate_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<a id='btn_system_migrate' class='btn primary' href='/api/system/migrate_zip' download='" SYSTEM_MIGRATE_ZIP_NAME "'>...</a>"
    "</div>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_system_deploy_title'>...</h3>"
    "<p id='txt_system_deploy_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<input id='deploy_zip_file' type='file' accept='.zip,application/zip' style='flex:1;min-width:240px'>"
    "<button id='btn_deploy_zip' class='warn'>...</button>"
    "</div>"
    "<pre id='deploy_zip_status'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_fw_title'>...</h3>"
    "<p id='txt_fw_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<input id='fw_file' type='file' accept='.bin,application/octet-stream' style='flex:1;min-width:240px'>"
    "<button id='btn_fw_upload' class='primary'>...</button>"
    "</div>"
    "<pre id='fw_upload'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_language_title'>...</h3>"
    "<p id='txt_language_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<button id='btn_lang_load'>...</button>"
    "<select id='lang_select' style='flex:1;min-width:220px'></select>"
    "<button id='btn_lang_apply' class='primary'>...</button>"
    "</div>"
    "<pre id='lang_status'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_usb_title'>...</h3>"
    "<p id='txt_usb_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<button id='btn_enable' class='primary'>...</button>"
    "<button id='btn_disable' class='warn'>...</button>"
    "<button id='btn_refresh'>...</button>"
    "</div>"
    "<pre id='storage'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_device_title'>...</h3>"
    "<div class='row'><button id='btn_dev'>...</button></div>"
    "<pre id='dev'>...</pre>"
    "</div>"
    "<script>"
    "const storageEl=document.getElementById('storage');"
    "const devEl=document.getElementById('dev');"
    "const mainUploadEl=document.getElementById('main_upload');"
    "const deployZipStatusEl=document.getElementById('deploy_zip_status');"
    "const fwUploadEl=document.getElementById('fw_upload');"
    "const langStatusEl=document.getElementById('lang_status');"
    "const langSelectEl=document.getElementById('lang_select');"
    "const lang={};"
    "let mainUploadState='idle';"
    "let deployZipState='idle';"
    "let fwUploadState='idle';"
    "let storageState='loading';"
    "let deviceState='loading';"
    "let langState='idle';"
    "function tr(key){"
    "const v=lang[key];"
    "return(typeof v==='string'&&v.length>0)?v:'';"
    "}"
    "function setTextByKey(id,key){"
    "const el=document.getElementById(id);"
    "if(el){el.textContent=tr(key);}"
    "}"
    "function refreshIdleTexts(){"
    "if(mainUploadState==='idle'){mainUploadEl.textContent=tr('upload_idle');}"
    "if(deployZipState==='idle'){deployZipStatusEl.textContent=tr('upload_idle')||'Idle';}"
    "if(fwUploadState==='idle'){fwUploadEl.textContent=tr('firmware_idle');}"
    "if(storageState==='loading'){storageEl.textContent=tr('storage_loading');}"
    "if(deviceState==='loading'){devEl.textContent=tr('device_loading');}"
    "if(langState==='idle'){langStatusEl.textContent=tr('language_idle');}"
    "}"
    "function applyLang(){"
    "document.title=tr('page_title');"
    "setTextByKey('txt_page_header','page_header');"
    "setTextByKey('txt_page_tip','page_tip');"
    "setTextByKey('txt_business_title','business_title');"
    "setTextByKey('btn_open_business','btn_open_business');"
    "setTextByKey('txt_business_tip','business_tip');"
    "setTextByKey('txt_recovery_title','recovery_title');"
    "setTextByKey('txt_recovery_tip','recovery_tip');"
    "setTextByKey('btn_main_upload','btn_upload_main');"
    "setTextByKey('txt_system_migrate_title','system_migrate_title');"
    "setTextByKey('txt_system_migrate_tip','system_migrate_tip');"
    "setTextByKey('btn_system_migrate','btn_system_migrate');"
    "setTextByKey('txt_system_deploy_title','system_deploy_title');"
    "setTextByKey('txt_system_deploy_tip','system_deploy_tip');"
    "setTextByKey('btn_deploy_zip','btn_system_deploy');"
    "setTextByKey('txt_fw_title','firmware_title');"
    "setTextByKey('txt_fw_tip','firmware_tip');"
    "setTextByKey('btn_fw_upload','btn_upload_firmware');"
    "setTextByKey('txt_language_title','language_title');"
    "setTextByKey('txt_language_tip','language_tip');"
    "setTextByKey('btn_lang_load','btn_read_lang_list');"
    "setTextByKey('btn_lang_apply','btn_apply_language');"
    "setTextByKey('txt_usb_title','usb_title');"
    "setTextByKey('txt_usb_tip','usb_tip');"
    "setTextByKey('btn_enable','btn_enable_usb');"
    "setTextByKey('btn_disable','btn_disable_usb');"
    "setTextByKey('btn_refresh','btn_refresh_storage');"
    "setTextByKey('txt_device_title','device_title');"
    "setTextByKey('btn_dev','btn_refresh_device');"
    "refreshIdleTexts();"
    "}"
    "function formatBytes(value){"
    "const n=Number(value);"
    "if(!Number.isFinite(n)||n<0){return 'n/a';}"
    "const units=['B','KiB','MiB','GiB','TiB'];"
    "let v=n;"
    "let idx=0;"
    "while(v>=1024&&idx<units.length-1){v/=1024;idx++;}"
    "const digits=(idx===0||v>=100)?0:(v>=10?1:2);"
    "return v.toFixed(digits)+' '+units[idx]+' ('+Math.round(n)+' bytes)';"
    "}"
    "function boolText(v){return v?'yes':'no';}"
    "function setStorageText(obj){"
    "storageState='custom';"
    "if(!obj||typeof obj!=='object'){storageEl.textContent=String(obj);return;}"
    "const lines=[];"
    "lines.push('TF ready: '+boolText(!!obj.tf_ready));"
    "lines.push('USB MSC ready: '+boolText(!!obj.usb_msc_ready));"
    "lines.push('USB passthrough enabled: '+boolText(!!obj.usb_passthrough_enabled));"
    "lines.push('TF busy: '+boolText(!!obj.tf_busy));"
    "if(obj.tf_capacity_ok){"
    "lines.push('TF total: '+formatBytes(obj.tf_total_bytes));"
    "lines.push('TF used: '+formatBytes(obj.tf_used_bytes));"
    "lines.push('TF free: '+formatBytes(obj.tf_free_bytes));"
    "}else{"
    "lines.push('TF capacity: unavailable');"
    "}"
    "storageEl.textContent=lines.join('\\n');"
    "}"
    "function setLangOptions(files,current){"
    "langSelectEl.innerHTML='';"
    "if(!Array.isArray(files)||files.length===0){"
    "const opt=document.createElement('option');"
    "opt.value='';"
    "opt.textContent=tr('language_none');"
    "langSelectEl.appendChild(opt);"
    "return;"
    "}"
    "for(let i=0;i<files.length;i++){"
    "const name=files[i];"
    "const opt=document.createElement('option');"
    "opt.value=name;"
    "opt.textContent=name;"
    "langSelectEl.appendChild(opt);"
    "}"
    "if(typeof current==='string'&&current.length>0){langSelectEl.value=current;}"
    "if(!langSelectEl.value&&langSelectEl.options.length>0){langSelectEl.selectedIndex=0;}"
    "}"
    "async function requestJson(url,opt){"
    "const r=await fetch(url,opt);"
    "const t=await r.text();"
    "if(!r.ok){throw new Error(tr('msg_http_error_prefix')+r.status+(t?(' '+t):''));}"
    "try{return JSON.parse(t);}catch(e){throw new Error(tr('msg_invalid_json_prefix')+t);}"
    "}"
    "async function requestText(url,opt){"
    "const r=await fetch(url,opt);"
    "const t=await r.text();"
    "if(!r.ok){throw new Error(tr('msg_http_error_prefix')+r.status+(t?(' '+t):''));}"
    "return t;"
    "}"
    "async function loadLang(){"
    "try{"
    "const d=await requestJson('/api/lang');"
    "if(d&&d.strings&&typeof d.strings==='object'){Object.assign(lang,d.strings);}"
    "}catch(e){}"
    "applyLang();"
    "}"
    "async function loadLangList(){"
    "langState='loading';"
    "langStatusEl.textContent=tr('language_loading');"
    "try{"
    "const d=await requestJson('/api/lang/list');"
    "const files=(d&&Array.isArray(d.files))?d.files:[];"
    "const current=(d&&typeof d.current==='string')?d.current:'';"
    "setLangOptions(files,current);"
    "langState='custom';"
    "langStatusEl.textContent=JSON.stringify(d);"
    "}catch(e){"
    "langState='custom';"
    "langStatusEl.textContent=tr('msg_lang_list_error_prefix')+String(e);"
    "}"
    "}"
    "async function refreshStorage(){"
    "try{setStorageText(await requestJson('/api/storage/status'));}"
    "catch(e){storageState='custom';storageEl.textContent=tr('msg_storage_status_error_prefix')+String(e);}"
    "}"
    "async function setUsbMode(enable){"
    "try{"
    "setStorageText({ok:false,message:tr('msg_applying')});"
    "setStorageText(await requestJson('/api/storage/usb_msc?enable='+(enable?'1':'0'),{method:'POST'}));"
    "}catch(e){storageEl.textContent=tr('msg_set_mode_error_prefix')+String(e);}"
    "}"
    "async function loadDeviceInfo(){"
    "try{deviceState='custom';devEl.textContent=await requestText('/api/device/info');}"
    "catch(e){deviceState='custom';devEl.textContent=tr('msg_device_info_error_prefix')+String(e);}"
    "}"
    "async function uploadMainHtml(){"
    "const fi=document.getElementById('main_file');"
    "const files=(fi&&fi.files)?Array.from(fi.files):[];"
    "if(files.length===0){mainUploadState='custom';mainUploadEl.textContent=tr('msg_select_main');return;}"
    "let ok=0,failed=0;"
    "const lines=[];"
    "for(let i=0;i<files.length;i++){"
    "const f=files[i];"
    "mainUploadEl.textContent=tr('msg_uploading_prefix')+' ('+(i+1)+'/'+files.length+') '+f.name+' ...';"
    "try{"
    "const r=await requestJson('/api/web/upload?name='+encodeURIComponent(f.name),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "ok++;"
    "lines.push(tr('msg_upload_item_ok')+' '+JSON.stringify(r));"
    "}catch(e){"
    "failed++;"
    "lines.push(tr('msg_upload_item_fail')+' '+f.name+': '+String(e));"
    "}"
    "}"
    "mainUploadState='custom';"
    "if(failed>0){"
    "mainUploadEl.textContent=tr('msg_upload_failed_prefix')+' '+ok+'/'+files.length+'\\n'+lines.join('\\n');"
    "}else{"
    "mainUploadEl.textContent=tr('msg_upload_success_prefix')+' '+ok+'/'+files.length+'\\n'+lines.join('\\n');"
    "}"
    "}"
    "async function uploadFirmware(){"
    "const fi=document.getElementById('fw_file');"
    "const f=(fi&&fi.files&&fi.files[0])?fi.files[0]:null;"
    "if(!f){fwUploadState='custom';fwUploadEl.textContent=tr('msg_select_firmware');return;}"
    "fwUploadEl.textContent=tr('msg_uploading_firmware_prefix')+' '+f.name+' ...';"
    "try{"
    "const r=await requestJson('/api/fw/upgrade',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "fwUploadState='custom';"
    "fwUploadEl.textContent=tr('msg_firmware_success_prefix')+'\\n'+JSON.stringify(r,null,2);"
    "}catch(e){"
    "fwUploadState='custom';"
    "fwUploadEl.textContent=tr('msg_upload_failed_prefix')+': '+String(e);"
    "}"
    "}"
    "async function uploadSystemDeployZip(){"
    "const fi=document.getElementById('deploy_zip_file');"
    "const f=(fi&&fi.files&&fi.files[0])?fi.files[0]:null;"
    "if(!f){deployZipState='custom';deployZipStatusEl.textContent=tr('msg_select_deploy_zip');return;}"
    "deployZipStatusEl.textContent=tr('msg_deploying_prefix')+': '+f.name+' ...';"
    "try{"
    "const r=await requestJson('/api/system/deploy_zip',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "deployZipState='custom';"
    "deployZipStatusEl.textContent=tr('msg_deploy_success_prefix')+'\\n'+JSON.stringify(r,null,2);"
    "if(fi){fi.value='';}"
    "}catch(e){"
    "deployZipState='custom';"
    "deployZipStatusEl.textContent=tr('msg_deploy_failed_prefix')+': '+String(e);"
    "}"
    "}"
    "async function applyLanguage(){"
    "const ini=(langSelectEl&&typeof langSelectEl.value==='string')?langSelectEl.value:'';"
    "if(!ini){langState='custom';langStatusEl.textContent=tr('msg_lang_select_required');return;}"
    "langState='loading';"
    "langStatusEl.textContent=tr('msg_applying');"
    "try{"
    "const d=await requestJson('/api/lang/apply?ini='+encodeURIComponent(ini),{method:'POST'});"
    "await loadLang();"
    "await loadLangList();"
    "langState='custom';"
    "langStatusEl.textContent=tr('msg_lang_apply_success_prefix')+d.language_ini;"
    "}catch(e){"
    "langState='custom';"
    "langStatusEl.textContent=tr('msg_lang_apply_error_prefix')+String(e);"
    "}"
    "}"
    "document.getElementById('btn_refresh').onclick=refreshStorage;"
    "document.getElementById('btn_enable').onclick=()=>setUsbMode(true);"
    "document.getElementById('btn_disable').onclick=()=>setUsbMode(false);"
    "document.getElementById('btn_dev').onclick=loadDeviceInfo;"
    "document.getElementById('btn_main_upload').onclick=uploadMainHtml;"
    "document.getElementById('btn_deploy_zip').onclick=uploadSystemDeployZip;"
    "document.getElementById('btn_fw_upload').onclick=uploadFirmware;"
    "document.getElementById('btn_lang_load').onclick=loadLangList;"
    "document.getElementById('btn_lang_apply').onclick=applyLanguage;"
    "async function refreshOverview(){"
    "await refreshStorage();"
    "await loadDeviceInfo();"
    "}"
    "async function initPage(){"
    "await loadLang();"
    "setLangOptions([], '');"
    "refreshIdleTexts();"
    "await loadLangList();"
    "await refreshOverview();"
    "setInterval(refreshOverview,3000);"
    "}"
    "initPage();"
    "</script></body></html>";

#if 0
/* Legacy unused inline pages kept only for reference. */
const char __attribute__((unused)) s_tf_html[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TF File Manager</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:980px;margin:24px auto;padding:0 16px;background:#f7f9fc;color:#101323;}"
    "a{margin-right:10px;}"
    ".toolbar{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0;}"
    "input,button{padding:8px 10px;border:1px solid #c8d2e4;border-radius:8px;background:#fff;}"
    "button{cursor:pointer;}"
    "button.primary{background:#0d6efd;color:#fff;border-color:#0d6efd;}"
    "button.danger{background:#dc3545;color:#fff;border-color:#dc3545;}"
    ".pathline{display:flex;align-items:center;gap:10px;margin:10px 0;padding:10px;border:1px solid #d7deec;border-radius:10px;background:#fff;}"
    ".pathline .dots{font-family:Consolas,monospace;font-weight:700;font-size:20px;line-height:1;text-decoration:none;}"
    ".pathline .dots.disabled{color:#93a0ba;pointer-events:none;}"
    ".mono{font-family:Consolas,monospace;}"
    ".list{background:#fff;border:1px solid #d7deec;border-radius:10px;padding:8px;}"
    ".item{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:8px 6px;border-bottom:1px solid #eef2f8;}"
    ".item:last-child{border-bottom:none;}"
    ".entry{display:inline-block;max-width:78%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
    ".dir-link{color:#0b5ed7;text-decoration:none;font-weight:600;}"
    ".file-link{color:#101323;text-decoration:none;}"
    ".meta{font-size:12px;color:#6c7891;white-space:nowrap;}"
    "#msg{margin:10px 0;padding:8px 10px;background:#eef3ff;border:1px solid #d6e3ff;border-radius:8px;min-height:20px;}"
    ".modal{position:fixed;inset:0;background:rgba(8,16,36,0.45);display:none;align-items:center;justify-content:center;padding:16px;z-index:9999;}"
    ".modal.show{display:flex;}"
    ".dialog{width:min(92vw,420px);background:#fff;border-radius:12px;padding:16px;border:1px solid #d7deec;}"
    ".dialog h3{margin:0 0 8px;font-size:16px;word-break:break-word;}"
    ".dialog p{margin:0 0 12px;font-size:12px;color:#6c7891;word-break:break-all;}"
    ".dlg-actions{display:flex;gap:8px;flex-wrap:wrap;}"
    "</style></head><body>"
    "<h2>TF File Manager</h2>"
    "<p><a href='/'>Home</a><a href='/cart'>Cartridge</a><a href='/settings'>Settings</a></p>"
    "<div class='toolbar'>"
    "<input id='path' class='mono' type='text' value='' style='flex:1;min-width:240px' placeholder='relative path, empty means /sdcard root'>"
    "<button id='go'>Go</button>"
    "<button id='refresh'>Refresh</button>"
    "</div>"
    "<div class='pathline'>"
    "<a id='up_link' class='dots' href='#' title='Parent folder'>...</a>"
    "<span id='path_view' class='mono'>/sdcard</span>"
    "</div>"
    "<div class='toolbar'>"
    "<input id='new_dir' type='text' placeholder='new folder name'>"
    "<button id='mkdir'>Create Folder</button>"
    "</div>"
    "<div class='toolbar'>"
    "<input id='upload_file' type='file' multiple style='flex:1;min-width:220px'>"
    "<button id='upload' class='primary'>Upload To Current Folder</button>"
    "</div>"
    "<div id='msg'>Loading...</div>"
    "<div id='list' class='list'></div>"
    "<div id='file_modal' class='modal'>"
    "<div class='dialog'>"
    "<h3 id='dlg_name'>File</h3>"
    "<p id='dlg_path'></p>"
    "<div class='dlg-actions'>"
    "<button id='dlg_download'>Download</button>"
    "<button id='dlg_rename'>Rename</button>"
    "<button id='dlg_delete' class='danger'>Delete</button>"
    "<button id='dlg_close'>Close</button>"
    "</div>"
    "</div>"
    "</div>"
    "<script>"
    "const listEl=document.getElementById('list');"
    "const msgEl=document.getElementById('msg');"
    "const pathEl=document.getElementById('path');"
    "const pathViewEl=document.getElementById('path_view');"
    "const upLink=document.getElementById('up_link');"
    "const modalEl=document.getElementById('file_modal');"
    "const dlgNameEl=document.getElementById('dlg_name');"
    "const dlgPathEl=document.getElementById('dlg_path');"
    "let currentPath='';"
    "let selectedEntry=null;"
    "function setMsg(text,isErr){"
    "msgEl.textContent=text;"
    "msgEl.style.background=isErr?'#fff2f2':'#eef3ff';"
    "msgEl.style.borderColor=isErr?'#ffcdcd':'#d6e3ff';"
    "}"
    "function joinPath(base,name){return base?base+'/'+name:name;}"
    "function parentPath(path){const i=path.lastIndexOf('/');return i<0?'':path.slice(0,i);}"
    "function humanSize(bytes){"
    "const n=Number(bytes)||0;"
    "if(n<1024)return n+' B';"
    "if(n<1024*1024)return (n/1024).toFixed(1)+' KB';"
    "return (n/1024/1024).toFixed(1)+' MB';"
    "}"
    "async function requestJson(url,opt){"
    "const r=await fetch(url,opt);"
    "const t=await r.text();"
    "if(!r.ok){throw new Error(t||('HTTP '+r.status));}"
    "try{return JSON.parse(t);}catch(e){throw new Error('Invalid response: '+t);}"
    "}"
    "function updatePathUi(){"
    "pathEl.value=currentPath;"
    "pathViewEl.textContent='/sdcard'+(currentPath?('/'+currentPath):'');"
    "if(currentPath){"
    "upLink.classList.remove('disabled');"
    "}else{"
    "upLink.classList.add('disabled');"
    "}"
    "}"
    "function showModal(entry){"
    "selectedEntry=entry;"
    "dlgNameEl.textContent=entry.name||'file';"
    "dlgPathEl.textContent=entry.path||'';"
    "modalEl.classList.add('show');"
    "}"
    "function closeModal(){"
    "selectedEntry=null;"
    "modalEl.classList.remove('show');"
    "}"
    "async function loadList(path){"
    "const target=(typeof path==='string')?path:currentPath;"
    "const data=await requestJson('/api/tf/list?path='+encodeURIComponent(target));"
    "currentPath=data.path||'';"
    "updatePathUi();"
    "renderRows(Array.isArray(data.entries)?data.entries:[]);"
    "setMsg('Path: /sdcard'+(currentPath?('/'+currentPath):'')+', items: '+(data.entries?data.entries.length:0),false);"
    "}"
    "function renderRows(entries){"
    "listEl.innerHTML='';"
    "entries.sort((a,b)=>{const ra=Number.isFinite(+a.sort_rank)?+a.sort_rank:(a.is_dir?0:4);const rb=Number.isFinite(+b.sort_rank)?+b.sort_rank:(b.is_dir?0:4);if(ra!==rb)return ra-rb;return String(a.name).localeCompare(String(b.name),undefined,{sensitivity:'base',numeric:true});});"
    "if(entries.length===0){"
    "const empty=document.createElement('div');"
    "empty.className='item';"
    "empty.textContent='(empty)';"
    "listEl.appendChild(empty);"
    "return;"
    "}"
    "for(const e of entries){"
    "const row=document.createElement('div');"
    "const left=document.createElement('div');"
    "const right=document.createElement('div');"
    "const link=document.createElement('a');"
    "row.className='item';"
    "left.className='entry';"
    "right.className='meta';"
    "link.href='#';"
    "if(e.is_dir){"
    "link.className='dir-link';"
    "link.textContent='[DIR] '+(e.name||'');"
    "link.onclick=async(ev)=>{"
    "ev.preventDefault();"
    "try{await loadList(e.path||'');}catch(err){setMsg(String(err),true);}"
    "};"
    "right.textContent='folder';"
    "}else{"
    "link.className='file-link';"
    "link.textContent=e.name||'';"
    "link.onclick=(ev)=>{ev.preventDefault();showModal(e);};"
    "right.textContent=humanSize(e.size);"
    "}"
    "left.appendChild(link);"
    "row.appendChild(left);"
    "row.appendChild(right);"
    "listEl.appendChild(row);"
    "}"
    "}"
    "document.getElementById('go').onclick=()=>loadList(pathEl.value.trim()).catch(e=>setMsg(String(e),true));"
    "upLink.onclick=(ev)=>{"
    "ev.preventDefault();"
    "if(!currentPath)return;"
    "loadList(parentPath(currentPath)).catch(e=>setMsg(String(e),true));"
    "};"
    "document.getElementById('refresh').onclick=()=>loadList().catch(e=>setMsg(String(e),true));"
    "document.getElementById('mkdir').onclick=async()=>{"
    "const name=document.getElementById('new_dir').value.trim();"
    "if(!name){setMsg('Folder name is empty',true);return;}"
    "const target=joinPath(currentPath,name);"
    "try{"
    "await requestJson('/api/tf/mkdir?path='+encodeURIComponent(target),{method:'POST'});"
    "document.getElementById('new_dir').value='';"
    "await loadList();"
    "}catch(err){setMsg(String(err),true);}"
    "};"
    "document.getElementById('upload').onclick=async()=>{"
    "const input=document.getElementById('upload_file');"
    "const files=input.files;"
    "if(!files||files.length===0){setMsg('Choose one or more files first',true);return;}"
    "try{"
    "for(let i=0;i<files.length;i++){"
    "const f=files[i];"
    "setMsg('Uploading '+f.name+' ('+(i+1)+'/'+files.length+') ...',false);"
    "await requestJson('/api/tf/upload?dir='+encodeURIComponent(currentPath)+'&name='+encodeURIComponent(f.name),{"
    "method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f"
    "});"
    "}"
    "input.value='';"
    "await loadList();"
    "setMsg('Upload completed',false);"
    "}catch(err){setMsg(String(err),true);}"
    "};"
    "document.getElementById('dlg_close').onclick=closeModal;"
    "modalEl.onclick=(ev)=>{if(ev.target===modalEl)closeModal();};"
    "document.getElementById('dlg_download').onclick=()=>{"
    "if(!selectedEntry)return;"
    "window.location='/api/tf/download?path='+encodeURIComponent(selectedEntry.path||'');"
    "};"
    "document.getElementById('dlg_rename').onclick=async()=>{"
    "if(!selectedEntry)return;"
    "const newName=prompt('New name',selectedEntry.name||'');"
    "if(!newName||newName===selectedEntry.name)return;"
    "const target=joinPath(currentPath,newName);"
    "try{"
    "await requestJson('/api/tf/rename?from='+encodeURIComponent(selectedEntry.path||'')+'&to='+encodeURIComponent(target),{method:'POST'});"
    "closeModal();"
    "await loadList();"
    "}catch(err){setMsg(String(err),true);}"
    "};"
    "document.getElementById('dlg_delete').onclick=async()=>{"
    "if(!selectedEntry)return;"
    "if(!confirm('Delete '+(selectedEntry.path||'')+' ?'))return;"
    "try{"
    "await requestJson('/api/tf/delete?path='+encodeURIComponent(selectedEntry.path||''),{method:'DELETE'});"
    "closeModal();"
    "await loadList();"
    "}catch(err){setMsg(String(err),true);}"
    "};"
    "updatePathUi();"
    "loadList('').catch(e=>setMsg(String(e),true));"
    "</script></body></html>";

const char __attribute__((unused)) s_settings_html[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MORI Device Settings</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:760px;margin:24px auto;padding:0 16px;}"
    "a{margin-right:10px;}"
    "button{padding:8px 12px;margin-right:8px;margin-bottom:8px;}"
    "pre{background:#111;color:#e6e6e6;padding:12px;white-space:pre-wrap;min-height:120px;}"
    "</style></head><body>"
    "<h2>Device Settings</h2>"
    "<p><a href='/'>Home</a><a href='/tf'>TF Files</a><a href='/cart'>Cartridge</a></p>"
    "<button id='refresh'>Refresh Device Info</button>"
    "<pre id='info'>Loading...</pre>"
    "<script>"
    "async function loadInfo(){"
    "const r=await fetch('/api/device/info');"
    "document.getElementById('info').textContent=await r.text();"
    "}"
    "document.getElementById('refresh').onclick=loadInfo;"
    "loadInfo();"
    "</script></body></html>";

const char __attribute__((unused)) s_default_upload_html[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Cartridge Manager</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:640px;margin:24px auto;padding:0 16px;line-height:1.45;}"
    "a{margin-right:10px;}"
    "input,button,select{padding:10px;margin:8px 0;width:100%;box-sizing:border-box;}"
    "button{cursor:pointer;}"
    "pre{background:#111;color:#e6e6e6;padding:12px;white-space:pre-wrap;}"
    "</style></head><body>"
    "<h2>Cartridge Manager</h2>"
    "<p><a href='/'>Home</a><a href='/tf'>TF Files</a><a href='/settings'>Settings</a></p>"
    "<p>Upload ROM to TF first, then start burning by selecting TF file name.</p>"
    "<input id='rom' type='file' accept='.gba,.bin,.rom'>"
    "<button id='upload'>Upload To TF</button>"
    "<input id='tf_name' type='text' placeholder='TF ROM file name, e.g. game.gba'>"
    "<select id='mode'><option value='gba'>GBA</option><option value='mbc5'>MBC5</option></select>"
    "<input id='slot' type='number' min='0' value='0' placeholder='slot'>"
    "<button id='burn_tf'>Burn From TF File</button>"
    "<pre id='status'>Loading...</pre>"
    "<script>"
    "const statusEl=document.getElementById('status');"
    "function show(s){"
    "statusEl.textContent='State: '+s.state+'\\nProgress: '+s.progress+'%\\nProcessed: '+s.processed+'/'+s.total+' bytes\\nROM: '+s.rom+'\\nMessage: '+s.message;"
    "}"
    "async function poll(){"
    "try{const r=await fetch('/api/status');if(r.ok){show(await r.json());}}catch(e){}"
    "}"
    "setInterval(poll,1000);poll();"
    "document.getElementById('upload').onclick=async()=>{"
    "const f=document.getElementById('rom').files[0];"
    "if(!f){alert('Please choose a ROM file first.');return;}"
    "const mode=document.getElementById('mode').value||'gba';"
    "const r=await fetch('/api/upload?name='+encodeURIComponent(f.name)+'&mode='+encodeURIComponent(mode),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "const t=await r.text();"
    "alert(t);"
    "document.getElementById('tf_name').value=f.name;"
    "poll();"
    "};"
    "document.getElementById('burn_tf').onclick=async()=>{"
    "const name=document.getElementById('tf_name').value.trim();"
    "const mode=document.getElementById('mode').value||'gba';"
    "const slot=document.getElementById('slot').value.trim();"
    "if(!name){alert('Please enter a TF ROM file name.');return;}"
    "let url='/api/write?name='+encodeURIComponent(name)+'&mode='+encodeURIComponent(mode);"
    "if(slot!==''){url+='&slot='+encodeURIComponent(slot);}"
    "const r=await fetch(url,{method:'POST'});"
    "const t=await r.text();"
    "alert(t);"
    "poll();"
    "};"
    "</script></body></html>";
#endif

const char *burner_state_to_str(burner_state_t state)
{
    switch (state) {
        case BURNER_STATE_IDLE:
            return "idle";
        case BURNER_STATE_RECEIVING:
            return "receiving";
        case BURNER_STATE_BURNING:
            return "burning";
        case BURNER_STATE_DONE:
            return "done";
        case BURNER_STATE_ERROR:
            return "error";
        case BURNER_STATE_CANCELLED:
            return "cancelled";
        default:
            return "unknown";
    }
}

bool burner_parse_bool_text(const char *value, bool *out)
{
    if (value == NULL || out == NULL) {
        return false;
    }

    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "on") == 0 || strcasecmp(value, "enable") == 0 ||
        strcasecmp(value, "enabled") == 0) {
        *out = true;
        return true;
    }

    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "off") == 0 || strcasecmp(value, "disable") == 0 ||
        strcasecmp(value, "disabled") == 0) {
        *out = false;
        return true;
    }

    return false;
}

bool burner_parse_size_text(const char *value, uint32_t *out_bytes)
{
    char *end = NULL;
    unsigned long parsed;
    unsigned long long bytes = 0;
    const unsigned long long kbytes = 1024ULL;
    const unsigned long long mbytes = 1024ULL * 1024ULL;

    if (value == NULL || out_bytes == NULL || value[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value) {
        return false;
    }

    if (*end == '\0') {
        bytes = parsed;
    } else if ((end[0] == 'k' || end[0] == 'K') && end[1] == '\0') {
        bytes = (unsigned long long)parsed * kbytes;
    } else if ((end[0] == 'k' || end[0] == 'K') && (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
        bytes = (unsigned long long)parsed * kbytes;
    } else if ((end[0] == 'm' || end[0] == 'M') && end[1] == '\0') {
        bytes = (unsigned long long)parsed * mbytes;
    } else if ((end[0] == 'm' || end[0] == 'M') && (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
        bytes = (unsigned long long)parsed * mbytes;
    } else {
        return false;
    }

    if (bytes == 0ULL || bytes > UINT32_MAX) {
        return false;
    }

    *out_bytes = (uint32_t)bytes;
    return true;
}

bool burner_parse_u32_text(const char *value, uint32_t *out_value)
{
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || out_value == NULL || value[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *out_value = (uint32_t)parsed;
    return true;
}

bool burner_parse_cart_mode_text(const char *text, burner_cart_mode_t *mode_out)
{
    if (mode_out == NULL) {
        return false;
    }

    if (text == NULL || text[0] == '\0' || strcasecmp(text, "mbc5") == 0) {
        *mode_out = BURNER_CART_MODE_MBC5;
        return true;
    }
    if (strcasecmp(text, "gba") == 0) {
        *mode_out = BURNER_CART_MODE_GBA;
        return true;
    }

    return false;
}

const char *burner_write_path_to_str(burner_write_path_t path)
{
    switch (path) {
    case BURNER_WRITE_PATH_PIPELINE:
        return "pipeline";
    case BURNER_WRITE_PATH_PSRAM:
        return "psram";
    case BURNER_WRITE_PATH_DIRECT:
    default:
        return "direct";
    }
}

bool burner_parse_write_path_text(const char *text, burner_write_path_t *path_out)
{
    if (path_out == NULL) {
        return false;
    }
    if (text == NULL || text[0] == '\0' || strcasecmp(text, "direct") == 0) {
        *path_out = BURNER_WRITE_PATH_DIRECT;
        return true;
    }
    if (strcasecmp(text, "psram") == 0) {
        *path_out = BURNER_WRITE_PATH_PSRAM;
        return true;
    }
    if (strcasecmp(text, "pipeline") == 0 || strcasecmp(text, "pipe") == 0) {
        *path_out = BURNER_WRITE_PATH_PIPELINE;
        return true;
    }
    return false;
}

uint32_t burner_clamp_mbc5_program_chunk_bytes(uint32_t bytes)
{
    if (bytes < BURN_MBC5_PROGRAM_CHUNK_MIN_BYTES) {
        return BURN_MBC5_PROGRAM_CHUNK_MIN_BYTES;
    }
    if (bytes > BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES) {
        return BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES;
    }
    return bytes;
}

uint32_t burner_mbc5_program_chunk_kb_to_bytes(uint32_t kb)
{
    uint64_t bytes;

    if (kb == 0u) {
        return BURN_MBC5_PROGRAM_CHUNK_BYTES;
    }

    bytes = (uint64_t)kb * 1024ULL;
    if (bytes > UINT32_MAX) {
        return BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES;
    }
    return burner_clamp_mbc5_program_chunk_bytes((uint32_t)bytes);
}

bool burner_is_supported_dump_chunk_bytes(uint32_t bytes)
{
    return bytes == BURN_ROM_DUMP_CHUNK_MIN_BYTES || bytes == BURN_MBC5_DUMP_CHUNK_BYTES ||
           bytes == (128U * 1024U) || bytes == BURN_ROM_DUMP_CHUNK_MAX_BYTES;
}

uint32_t burner_dump_chunk_kb_to_bytes(uint32_t kb)
{
    uint64_t bytes;

    if (kb == 0u) {
        return BURN_GBA_DUMP_CHUNK_BYTES;
    }

    bytes = (uint64_t)kb * 1024ULL;
    if (bytes > UINT32_MAX) {
        return BURN_GBA_DUMP_CHUNK_BYTES;
    }
    if (!burner_is_supported_dump_chunk_bytes((uint32_t)bytes)) {
        return BURN_GBA_DUMP_CHUNK_BYTES;
    }
    return (uint32_t)bytes;
}

uint32_t burner_dump_chunk_bytes_to_kb(uint32_t bytes)
{
    if (!burner_is_supported_dump_chunk_bytes(bytes)) {
        return BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
    }
    return bytes / 1024U;
}

uint32_t burner_psram_window_mb_to_bytes(uint32_t mb)
{
    uint32_t max_mb = BURN_PSRAM_WINDOW_MAX_MB;

    if (esp_psram_is_initialized()) {
        size_t psram_size = esp_psram_get_size();

        if (psram_size >= BURN_PSRAM_WINDOW_BYTES_PER_MB) {
            max_mb = (uint32_t)(psram_size / BURN_PSRAM_WINDOW_BYTES_PER_MB);
        } else {
            max_mb = BURN_PSRAM_WINDOW_MIN_MB;
        }
        if (max_mb < BURN_PSRAM_WINDOW_MIN_MB) {
            max_mb = BURN_PSRAM_WINDOW_MIN_MB;
        } else if (max_mb > BURN_PSRAM_WINDOW_MAX_MB) {
            max_mb = BURN_PSRAM_WINDOW_MAX_MB;
        }
    }

    if (mb < BURN_PSRAM_WINDOW_MIN_MB || mb > BURN_PSRAM_WINDOW_MAX_MB) {
        mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    }
    if (mb == BURN_PSRAM_WINDOW_AUTO_MB) {
        mb = burner_psram_auto_window_mb();
    }
    if (mb > max_mb) {
        mb = max_mb;
    }
    return mb * BURN_PSRAM_WINDOW_BYTES_PER_MB;
}

uint32_t burner_psram_auto_window_mb(void)
{
    size_t free_bytes;
    size_t largest_bytes;
    size_t usable_bytes;
    uint32_t mb;

    if (!esp_psram_is_initialized()) {
        return BURN_PSRAM_WINDOW_MIN_MB;
    }

    free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    largest_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    usable_bytes = (largest_bytes < free_bytes) ? largest_bytes : free_bytes;
    if (usable_bytes > BURN_PSRAM_WINDOW_RESERVE_BYTES) {
        usable_bytes -= BURN_PSRAM_WINDOW_RESERVE_BYTES;
    }

    mb = (uint32_t)(usable_bytes / BURN_PSRAM_WINDOW_BYTES_PER_MB);
    if (mb < BURN_PSRAM_WINDOW_MIN_MB) {
        mb = BURN_PSRAM_WINDOW_MIN_MB;
    } else if (mb > BURN_PSRAM_WINDOW_MAX_MB) {
        mb = BURN_PSRAM_WINDOW_MAX_MB;
    }

    ESP_LOGI(
        BURNER_TAG,
        "PSRAM auto window: free=%u largest=%u reserve=%u -> %" PRIu32 "MB",
        (unsigned)free_bytes,
        (unsigned)largest_bytes,
        (unsigned)BURN_PSRAM_WINDOW_RESERVE_BYTES,
        mb);
    return mb;
}

uint32_t burner_psram_window_bytes_to_mb(uint32_t bytes)
{
    uint32_t mb;

    if (bytes == 0u) {
        return BURN_PSRAM_WINDOW_AUTO_MB;
    }
    if ((bytes % BURN_PSRAM_WINDOW_BYTES_PER_MB) != 0u) {
        return BURN_PSRAM_WINDOW_DEFAULT_MB;
    }

    mb = bytes / BURN_PSRAM_WINDOW_BYTES_PER_MB;
    if (mb < BURN_PSRAM_WINDOW_MIN_MB || mb > BURN_PSRAM_WINDOW_MAX_MB) {
        return BURN_PSRAM_WINDOW_DEFAULT_MB;
    }
    return mb;
}

const char *burner_core_affinity_to_str(burner_core_affinity_t affinity)
{
    switch (affinity) {
    case BURNER_CORE_AFFINITY_CPU0:
        return "cpu0";
    case BURNER_CORE_AFFINITY_CPU1:
        return "cpu1";
    case BURNER_CORE_AFFINITY_AUTO:
    default:
        return "auto";
    }
}

bool burner_parse_core_affinity_text(const char *text, burner_core_affinity_t *affinity_out)
{
    if (affinity_out == NULL) {
        return false;
    }
    if (text == NULL || text[0] == '\0' || strcasecmp(text, "auto") == 0) {
        *affinity_out = BURNER_CORE_AFFINITY_AUTO;
        return true;
    }
    if (strcasecmp(text, "cpu0") == 0 || strcmp(text, "0") == 0) {
        *affinity_out = BURNER_CORE_AFFINITY_CPU0;
        return true;
    }
    if (strcasecmp(text, "cpu1") == 0 || strcmp(text, "1") == 0) {
        *affinity_out = BURNER_CORE_AFFINITY_CPU1;
        return true;
    }
    return false;
}

BaseType_t burner_core_affinity_to_task_core_id(burner_core_affinity_t affinity)
{
    switch (affinity) {
    case BURNER_CORE_AFFINITY_CPU0:
        return 0;
    case BURNER_CORE_AFFINITY_CPU1:
        return 1;
    case BURNER_CORE_AFFINITY_AUTO:
    default:
        return tskNO_AFFINITY;
    }
}

BaseType_t burner_create_task_with_affinity(
    TaskFunction_t task_fn,
    const char *name,
    uint32_t stack_bytes,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *task_out,
    burner_core_affinity_t affinity)
{
    BaseType_t core_id = burner_core_affinity_to_task_core_id(affinity);

    if (core_id == tskNO_AFFINITY) {
        return xTaskCreate(task_fn, name, stack_bytes, arg, priority, task_out);
    }
    return xTaskCreatePinnedToCore(task_fn, name, stack_bytes, arg, priority, task_out, core_id);
}

bool burner_get_mbc5_slot_range(bool ram_range, uint32_t slot, uint32_t *addr_begin, uint32_t *addr_end)
{
    const uint32_t (*table)[2] = ram_range ? s_mbc5_multi_ram_range : s_mbc5_multi_rom_range;

    if (addr_begin == NULL || addr_end == NULL || slot > BURNER_MBC5_SLOT_MAX) {
        return false;
    }

    *addr_begin = table[slot][0];
    *addr_end = table[slot][1];
    return true;
}

esp_err_t burner_apply_mbc5_slot_limit(
    bool ram_range,
    uint32_t slot,
    uint32_t requested_size,
    uint32_t *addr_begin,
    uint32_t *effective_size)
{
    uint32_t slot_begin;
    uint32_t slot_end;
    uint64_t slot_size;

    if (addr_begin == NULL || effective_size == NULL || requested_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot > BURNER_MBC5_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (slot == 0u) {
        *addr_begin = 0u;
        *effective_size = requested_size;
        return ESP_OK;
    }

    if (!burner_get_mbc5_slot_range(ram_range, slot, &slot_begin, &slot_end) || slot_end < slot_begin) {
        return ESP_ERR_INVALID_ARG;
    }
    slot_size = (uint64_t)slot_end - (uint64_t)slot_begin + 1u;
    if ((uint64_t)requested_size > slot_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    *addr_begin = slot_begin;
    *effective_size = requested_size;
    return ESP_OK;
}

esp_err_t burner_apply_gba_slot_limit(
    uint32_t slot,
    uint32_t requested_size,
    uint32_t *addr_begin,
    uint32_t *effective_size,
    bool *force_multi)
{
    uint64_t base = 0;

    if (addr_begin == NULL || effective_size == NULL || force_multi == NULL || requested_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (slot == 0u) {
        base = 0u;
        *force_multi = false;
    } else if (slot == 1u) {
        base = 0u;
        *force_multi = true;
    } else {
        /*
         * Host mission_gba.cs:
         * index >= 2 => base = (8 + 4 * (index - 2)) MB.
         */
        base = (uint64_t)(8u + (4u * (slot - 2u))) * 1024u * 1024u;
        *force_multi = true;
    }

    if (base > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *addr_begin = (uint32_t)base;
    *effective_size = requested_size;
    return ESP_OK;
}

bool burner_parse_ram_mode(const char *ram_type_text, bool *fram_mode)
{
    if (fram_mode == NULL) {
        return false;
    }

    *fram_mode = false;
    if (ram_type_text == NULL || ram_type_text[0] == '\0') {
        return true;
    }

    if (strcasecmp(ram_type_text, "sram") == 0) {
        *fram_mode = false;
        return true;
    }
    if (strcasecmp(ram_type_text, "fram") == 0) {
        *fram_mode = true;
        return true;
    }

    return false;
}

bool burner_status_tracks_speed(burner_state_t state)
{
    return (state == BURNER_STATE_RECEIVING || state == BURNER_STATE_BURNING);
}

bool burner_status_is_operation_active_state(burner_state_t state)
{
    return (state == BURNER_STATE_RECEIVING || state == BURNER_STATE_BURNING);
}

bool burner_status_is_operation_active_locked(void)
{
    return burner_status_is_operation_active_state(s_status.state);
}

void burner_cancel_reset_locked(void)
{
    s_status.cancel_requested = false;
}

void burner_cancel_reset(void)
{
    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        burner_cancel_reset_locked();
        xSemaphoreGive(s_status_lock);
    } else {
        s_status.cancel_requested = false;
    }
}

bool burner_cancel_is_requested(void)
{
    bool requested;

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        requested = s_status.cancel_requested;
        xSemaphoreGive(s_status_lock);
    } else {
        requested = s_status.cancel_requested;
    }

    return requested;
}

esp_err_t burner_cancel_poll(void)
{
    return burner_cancel_is_requested() ? ESP_ERR_INVALID_STATE : ESP_OK;
}

bool burner_cancel_request(void)
{
    burner_status_t snap = {0};
    bool active = false;

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        active = burner_status_is_operation_active_locked();
        if (active) {
            s_status.cancel_requested = true;
            snap = s_status;
        }
        xSemaphoreGive(s_status_lock);
    } else {
        active = burner_status_is_operation_active_state(s_status.state);
        if (active) {
            s_status.cancel_requested = true;
            snap = s_status;
        }
    }

    if (active) {
        burner_status_update(
            snap.state,
            snap.progress,
            snap.processed_bytes,
            snap.total_bytes,
            "cancel requested",
            snap.rom_name,
            snap.rom_path);
    }

    return active;
}

uint32_t burner_us_to_ms_clamped(uint64_t us)
{
    uint64_t ms = us / 1000ULL;
    if (ms > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)ms;
}

void burner_status_speed_reset_locked(void)
{
    s_status.speed_start_us = 0u;
    s_status.speed_warmup_until_us = 0u;
    s_status.speed_last_us = 0u;
    s_status.speed_start_bytes = 0u;
    s_status.speed_last_bytes = 0u;
    s_status.speed_current_bps = 0u;
    s_status.speed_avg_bps = 0u;
    s_status.speed_min_bps = 0u;
    s_status.speed_max_bps = 0u;
}

void burner_status_verify_sample_reset_locked(void)
{
    s_status.verify_sample_addr = 0u;
    s_status.verify_sample_file_byte = 0u;
    s_status.verify_sample_cart_byte = 0u;
    s_status.verify_sample_valid = false;
    s_status.verify_sample_equal = false;
}

void burner_status_phase_reset_locked(void)
{
    s_status.task_start_us = 0u;
    s_status.task_elapsed_us = 0u;
    s_status.erase_sector_count = 0u;
    s_status.erase_sector_size = 0u;
    s_status.erase_phase_total_sectors = 0u;
    s_status.erase_phase_done_sectors = 0u;
    s_status.erase_phase_total_bytes = 0u;
    s_status.erase_phase_done_bytes = 0u;
    s_status.erase_phase_planned = false;
    s_status.erase_phase_active = false;
    s_status.erase_start_us = 0u;
    s_status.erase_elapsed_us = 0u;
    s_status.write_start_us = 0u;
    s_status.write_elapsed_us = 0u;
    s_status.tf_to_psram_speed_current_bps = 0u;
    s_status.tf_to_psram_speed_avg_bps = 0u;
    s_status.tf_to_psram_speed_min_bps = 0u;
    s_status.tf_to_psram_speed_max_bps = 0u;
    s_status.dump_read_speed_current_bps = 0u;
    s_status.dump_read_speed_avg_bps = 0u;
    s_status.dump_read_speed_min_bps = 0u;
    s_status.dump_read_speed_max_bps = 0u;
    s_status.dump_write_speed_current_bps = 0u;
    s_status.dump_write_speed_avg_bps = 0u;
    s_status.dump_write_speed_min_bps = 0u;
    s_status.dump_write_speed_max_bps = 0u;
    s_status.mbc5_buffer_write_ok_count = 0u;
    s_status.mbc5_buffer_fallback_count = 0u;
    s_status.tf_to_psram_total_bytes = 0u;
    s_status.tf_to_psram_total_us = 0u;
    s_status.dump_read_total_bytes = 0u;
    s_status.dump_read_total_us = 0u;
    s_status.dump_write_total_bytes = 0u;
    s_status.dump_write_total_us = 0u;
    s_status.dump_wait_total_us = 0u;
    s_status.dump_finalize_total_us = 0u;
    burner_status_verify_sample_reset_locked();
}

void burner_status_probe_reset_locked(void)
{
    s_status.probe_cart_mode = BURNER_CART_MODE_MBC5;
    s_status.probe_valid = false;
    s_status.probe_cfi_ok = false;
    s_status.probe_gba_multi = false;
    s_status.probe_gba_force_multi = false;
    s_status.probe_gba_d0d1_known = false;
    s_status.probe_gba_d0d1_swapped = false;
    s_status.probe_gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    s_status.probe_gba_save_size = 0u;
    s_status.probe_gba_save_detected = false;
    s_status.probe_gba_sram_patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
    s_status.probe_gba_sram_patch_scanned = false;
    s_status.probe_gba_sram_patch_detected = false;
    s_status.probe_device_size = 0u;
    s_status.probe_sector_size = 0u;
    s_status.probe_buffer_write_bytes = 0u;
    memset(s_status.probe_id, 0, sizeof(s_status.probe_id));
    s_status.probe_chip_name[0] = '\0';
}

void burner_status_set_probe_info(
    burner_cart_mode_t cart_mode,
    const uint8_t *id,
    size_t id_len,
    uint32_t device_size,
    uint32_t sector_size,
    uint16_t buffer_write_bytes,
    bool cfi_ok,
    bool gba_multi,
    bool gba_force_multi,
    bool gba_d0d1_known,
    bool gba_d0d1_swapped,
    const char *chip_name)
{
    size_t copy_len;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.probe_cart_mode = cart_mode;
    s_status.probe_valid = true;
    s_status.probe_cfi_ok = cfi_ok;
    s_status.probe_gba_multi = gba_multi;
    s_status.probe_gba_force_multi = gba_force_multi;
    s_status.probe_gba_d0d1_known = (cart_mode == BURNER_CART_MODE_GBA) ? gba_d0d1_known : false;
    s_status.probe_gba_d0d1_swapped = (cart_mode == BURNER_CART_MODE_GBA && gba_d0d1_known) ? gba_d0d1_swapped : false;
    if (cart_mode != BURNER_CART_MODE_GBA) {
        s_status.probe_gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
        s_status.probe_gba_save_size = 0u;
        s_status.probe_gba_save_detected = false;
        s_status.probe_gba_sram_patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
        s_status.probe_gba_sram_patch_scanned = false;
        s_status.probe_gba_sram_patch_detected = false;
    }
    s_status.probe_device_size = device_size;
    s_status.probe_sector_size = sector_size;
    s_status.probe_buffer_write_bytes = buffer_write_bytes;
    memset(s_status.probe_id, 0, sizeof(s_status.probe_id));
    if (id != NULL && id_len > 0u) {
        copy_len = (id_len < sizeof(s_status.probe_id)) ? id_len : sizeof(s_status.probe_id);
        memcpy(s_status.probe_id, id, copy_len);
    }
    snprintf(
        s_status.probe_chip_name,
        sizeof(s_status.probe_chip_name),
        "%s",
        (chip_name != NULL && chip_name[0] != '\0') ? chip_name : "unknown");
    xSemaphoreGive(s_status_lock);
}

void burner_status_set_gba_save_probe(
    burner_gba_save_type_t save_type,
    uint32_t save_size,
    bool detected)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.probe_gba_save_type = save_type;
    s_status.probe_gba_save_size = save_size;
    s_status.probe_gba_save_detected = detected;
    xSemaphoreGive(s_status_lock);
}

void burner_status_set_gba_sram_patch_probe(
    burner_gba_sram_patch_kind_t patch_kind,
    bool scanned,
    bool detected)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.probe_gba_sram_patch_scanned = scanned;
    s_status.probe_gba_sram_patch_kind = detected ? patch_kind : BURNER_GBA_SRAM_PATCH_NONE;
    s_status.probe_gba_sram_patch_detected = detected;
    xSemaphoreGive(s_status_lock);
}

void burner_status_set_verify_sample(uint32_t addr, uint8_t file_byte, uint8_t cart_byte, bool equal)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.verify_sample_addr = addr;
    s_status.verify_sample_file_byte = file_byte;
    s_status.verify_sample_cart_byte = cart_byte;
    s_status.verify_sample_valid = true;
    s_status.verify_sample_equal = equal;
    xSemaphoreGive(s_status_lock);
}

void burner_status_begin_erase_phase(uint32_t total_sectors, uint32_t total_bytes, uint32_t sector_size)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (!s_status.erase_phase_planned) {
        s_status.erase_phase_total_sectors = total_sectors;
        s_status.erase_phase_done_sectors = 0u;
        s_status.erase_phase_total_bytes = total_bytes;
        s_status.erase_phase_done_bytes = 0u;
    } else if (s_status.erase_phase_total_sectors == 0u) {
        s_status.erase_phase_total_sectors = total_sectors;
        s_status.erase_phase_total_bytes = total_bytes;
    } else if (s_status.erase_phase_total_bytes == 0u) {
        s_status.erase_phase_total_bytes = total_bytes;
    }
    s_status.erase_sector_size = sector_size;
    s_status.erase_phase_active = false;
    xSemaphoreGive(s_status_lock);
}

void burner_status_plan_erase_phase(uint32_t total_sectors, uint32_t total_bytes, uint32_t sector_size)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.erase_phase_total_sectors = total_sectors;
    s_status.erase_phase_done_sectors = 0u;
    s_status.erase_phase_total_bytes = total_bytes;
    s_status.erase_phase_done_bytes = 0u;
    s_status.erase_phase_planned = (total_sectors > 0u) || (total_bytes > 0u);
    s_status.erase_sector_size = sector_size;
    s_status.erase_phase_active = false;
    xSemaphoreGive(s_status_lock);
}

void burner_status_advance_erase_phase(uint32_t sectors_done, uint32_t bytes_done)
{
    uint64_t total_done;
    uint64_t total_done_bytes;

    if (s_status_lock == NULL || (sectors_done == 0u && bytes_done == 0u)) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    total_done = (uint64_t)s_status.erase_phase_done_sectors + (uint64_t)sectors_done;
    if (s_status.erase_phase_total_sectors > 0u && total_done > (uint64_t)s_status.erase_phase_total_sectors) {
        s_status.erase_phase_done_sectors = s_status.erase_phase_total_sectors;
    } else if (total_done > UINT32_MAX) {
        s_status.erase_phase_done_sectors = UINT32_MAX;
    } else {
        s_status.erase_phase_done_sectors = (uint32_t)total_done;
    }
    total_done_bytes = (uint64_t)s_status.erase_phase_done_bytes + (uint64_t)bytes_done;
    if (s_status.erase_phase_total_bytes > 0u && total_done_bytes > (uint64_t)s_status.erase_phase_total_bytes) {
        s_status.erase_phase_done_bytes = s_status.erase_phase_total_bytes;
    } else if (total_done_bytes > UINT32_MAX) {
        s_status.erase_phase_done_bytes = UINT32_MAX;
    } else {
        s_status.erase_phase_done_bytes = (uint32_t)total_done_bytes;
    }
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_erase_begin(void)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.erase_start_us = (uint64_t)esp_timer_get_time();
    s_status.erase_phase_active = true;
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_erase_end(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (s_status.erase_start_us > 0u) {
        now_us = (uint64_t)esp_timer_get_time();
        if (now_us > s_status.erase_start_us) {
            s_status.erase_elapsed_us += now_us - s_status.erase_start_us;
        }
        s_status.erase_start_us = 0u;
    }
    if (!s_status.erase_phase_planned &&
        s_status.erase_phase_total_sectors > 0u &&
        s_status.erase_phase_done_sectors < s_status.erase_phase_total_sectors) {
        s_status.erase_phase_done_sectors = s_status.erase_phase_total_sectors;
    }
    if (!s_status.erase_phase_planned &&
        s_status.erase_phase_total_bytes > 0u &&
        s_status.erase_phase_done_bytes < s_status.erase_phase_total_bytes) {
        s_status.erase_phase_done_bytes = s_status.erase_phase_total_bytes;
    }
    s_status.erase_phase_active = false;
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_write_begin(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.write_start_us = now_us;
    burner_status_speed_reset_locked();
    s_status.speed_warmup_until_us = now_us + BURNER_SPEED_WARMUP_US;
    s_status.speed_last_bytes = s_status.processed_bytes;
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_write_end(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (s_status.write_start_us > 0u) {
        now_us = (uint64_t)esp_timer_get_time();
        if (now_us > s_status.write_start_us) {
            s_status.write_elapsed_us += now_us - s_status.write_start_us;
        }
        s_status.write_start_us = 0u;
    }
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_task_begin(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_phase_reset_locked();
    burner_status_speed_reset_locked();
    s_status.task_start_us = now_us;
    xSemaphoreGive(s_status_lock);
    s_burn_task_last_yield_us = now_us;
}

void burner_status_mark_task_end(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (s_status.task_start_us > 0u) {
        now_us = (uint64_t)esp_timer_get_time();
        if (now_us > s_status.task_start_us) {
            s_status.task_elapsed_us += now_us - s_status.task_start_us;
        }
        s_status.task_start_us = 0u;
    }
    xSemaphoreGive(s_status_lock);
}

void burner_task_yield_if_due(void)
{
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    if (s_burn_task_last_yield_us == 0u) {
        s_burn_task_last_yield_us = now_us;
        return;
    }
    if (now_us - s_burn_task_last_yield_us < BURNER_CPU_YIELD_INTERVAL_US) {
        return;
    }

    s_burn_task_last_yield_us = now_us;
    vTaskDelay(1);
}

uint32_t burner_erase_sector_count_from_bytes(uint64_t bytes, uint32_t sector_size)
{
    uint64_t sectors;

    if (bytes == 0u || sector_size == 0u) {
        return 0u;
    }

    sectors = (bytes + (uint64_t)sector_size - 1u) / (uint64_t)sector_size;
    if (sectors > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)sectors;
}

uint32_t burner_erase_sector_count_from_range(uint32_t addr_begin, uint32_t addr_end, uint32_t sector_size)
{
    uint32_t sector_mask;
    uint32_t aligned_begin;
    uint32_t aligned_end;

    if (addr_end < addr_begin) {
        return 0u;
    }
    if (sector_size == 0u) {
        return 0u;
    }
    if ((sector_size & (sector_size - 1u)) != 0u) {
        return burner_erase_sector_count_from_bytes((uint64_t)addr_end - (uint64_t)addr_begin + 1u, sector_size);
    }
    sector_mask = sector_size - 1u;
    aligned_begin = addr_begin & ~sector_mask;
    aligned_end = addr_end & ~sector_mask;
    if (aligned_end < aligned_begin) {
        return 0u;
    }
    return burner_erase_sector_count_from_bytes((uint64_t)aligned_end - (uint64_t)aligned_begin + (uint64_t)sector_size, sector_size);
}

static uint32_t burner_planned_stage_erase_sector_count(
    uint32_t addr_begin,
    uint32_t total_bytes,
    uint32_t sector_size,
    uint32_t stage_capacity)
{
    uint32_t processed = 0u;
    uint64_t total_sectors = 0u;

    if (total_bytes == 0u || sector_size == 0u || stage_capacity == 0u ||
        (sector_size & (sector_size - 1u)) != 0u) {
        return 0u;
    }

    while (processed < total_bytes) {
        uint32_t stage_addr = addr_begin + processed;
        uint32_t stage_bytes = total_bytes - processed;
        uint32_t stage_erase_begin = stage_addr;
        uint32_t stage_erase_end;

        if (stage_bytes > stage_capacity) {
            stage_bytes = stage_capacity;
        }
        stage_erase_end = stage_addr + stage_bytes - 1u;
        if (processed > 0u) {
            uint32_t mask = sector_size - 1u;
            uint64_t ceil_begin_u64 = (uint64_t)stage_addr + (uint64_t)mask;
            if (ceil_begin_u64 > UINT32_MAX) {
                stage_erase_begin = UINT32_MAX;
            } else {
                stage_erase_begin = (uint32_t)ceil_begin_u64 & ~mask;
            }
            if (stage_erase_begin > stage_erase_end) {
                stage_erase_begin = stage_erase_end;
            }
        }

        total_sectors += burner_erase_sector_count_from_range(stage_erase_begin, stage_erase_end, sector_size);
        if (total_sectors > UINT32_MAX) {
            return UINT32_MAX;
        }
        processed += stage_bytes;
    }

    return (uint32_t)total_sectors;
}

static void burner_nor_geometry_clear(burner_nor_geometry_t *geometry)
{
    if (geometry == NULL) {
        return;
    }
    memset(geometry, 0, sizeof(*geometry));
}

static bool burner_nor_geometry_is_valid(const burner_nor_geometry_t *geometry)
{
    uint32_t prev_end = 0u;

    if (geometry == NULL || geometry->region_count == 0u ||
        geometry->region_count > BURNER_NOR_GEOMETRY_REGION_MAX ||
        geometry->largest_sector_size == 0u) {
        return false;
    }

    for (uint32_t i = 0u; i < geometry->region_count; ++i) {
        const burner_nor_region_t *region = &geometry->regions[i];

        if (region->sector_size == 0u || region->addr_end <= region->addr_begin) {
            return false;
        }
        if (((region->addr_end - region->addr_begin) % region->sector_size) != 0u) {
            return false;
        }
        if (i > 0u && region->addr_begin != prev_end) {
            return false;
        }
        prev_end = region->addr_end;
    }

    return true;
}

static bool burner_nor_geometry_is_uniform(const burner_nor_geometry_t *geometry)
{
    return burner_nor_geometry_is_valid(geometry) && geometry->uniform_sector_size > 0u;
}

static uint32_t burner_nor_geometry_display_sector_size(const burner_nor_geometry_t *geometry)
{
    return burner_nor_geometry_is_uniform(geometry) ? geometry->uniform_sector_size : 0u;
}

static uint32_t burner_nor_geometry_largest_sector_size(const burner_nor_geometry_t *geometry)
{
    return burner_nor_geometry_is_valid(geometry) ? geometry->largest_sector_size : 0u;
}

static uint32_t burner_nor_geometry_report_sector_size(const burner_nor_geometry_t *geometry)
{
    uint32_t sector_size = burner_nor_geometry_display_sector_size(geometry);

    if (sector_size == 0u) {
        sector_size = burner_nor_geometry_largest_sector_size(geometry);
    }
    return sector_size;
}

static const char *burner_gb_mapper_name(burner_gb_mapper_t mapper)
{
    switch (mapper) {
        case BURNER_GB_MAPPER_MBC3:
            return "mbc3";
        case BURNER_GB_MAPPER_MBC5:
            return "mbc5";
        default:
            return "unknown";
    }
}

static bool burner_nor_geometry_equal(
    const burner_nor_geometry_t *left,
    const burner_nor_geometry_t *right)
{
    if (!burner_nor_geometry_is_valid(left) || !burner_nor_geometry_is_valid(right) ||
        left->region_count != right->region_count ||
        left->uniform_sector_size != right->uniform_sector_size ||
        left->smallest_sector_size != right->smallest_sector_size ||
        left->largest_sector_size != right->largest_sector_size) {
        return false;
    }

    for (uint32_t i = 0u; i < left->region_count; ++i) {
        if (left->regions[i].addr_begin != right->regions[i].addr_begin ||
            left->regions[i].addr_end != right->regions[i].addr_end ||
            left->regions[i].sector_size != right->regions[i].sector_size) {
            return false;
        }
    }
    return true;
}


static esp_err_t burner_nor_geometry_set_uniform(
    burner_nor_geometry_t *geometry,
    uint32_t device_size,
    uint32_t sector_size)
{
    burner_nor_geometry_clear(geometry);
    if (geometry == NULL || device_size == 0u || sector_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    geometry->region_count = 1u;
    geometry->uniform_sector_size = sector_size;
    geometry->smallest_sector_size = sector_size;
    geometry->largest_sector_size = sector_size;
    geometry->regions[0].addr_begin = 0u;
    geometry->regions[0].addr_end = device_size;
    geometry->regions[0].sector_size = sector_size;
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_build(
    burner_nor_geometry_t *geometry,
    uint32_t device_size,
    const uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX],
    const uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX],
    uint32_t region_count,
    bool reverse_order)
{
    uint32_t current_addr = 0u;
    uint32_t uniform_sector_size = 0u;
    uint32_t smallest_sector_size = UINT32_MAX;
    uint32_t largest_sector_size = 0u;
    uint64_t total_size = 0u;

    burner_nor_geometry_clear(geometry);
    if (geometry == NULL || sector_counts == NULL || sector_sizes == NULL ||
        region_count == 0u || region_count > BURNER_NOR_GEOMETRY_REGION_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uniform_sector_size = sector_sizes[0];
    for (uint32_t i = 0u; i < region_count; ++i) {
        uint64_t region_size;

        if (sector_counts[i] == 0u || sector_sizes[i] == 0u) {
            return ESP_ERR_INVALID_SIZE;
        }
        region_size = (uint64_t)sector_counts[i] * (uint64_t)sector_sizes[i];
        if (region_size == 0u || region_size > UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_size += region_size;
        if (total_size > UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (sector_sizes[i] < smallest_sector_size) {
            smallest_sector_size = sector_sizes[i];
        }
        if (sector_sizes[i] > largest_sector_size) {
            largest_sector_size = sector_sizes[i];
        }
        if (sector_sizes[i] != uniform_sector_size) {
            uniform_sector_size = 0u;
        }
    }

    if (device_size == 0u) {
        device_size = (uint32_t)total_size;
    } else if ((uint64_t)device_size != total_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    geometry->region_count = (uint8_t)region_count;
    geometry->uniform_sector_size = uniform_sector_size;
    geometry->smallest_sector_size = smallest_sector_size;
    geometry->largest_sector_size = largest_sector_size;

    if (reverse_order) {
        current_addr = device_size;
        for (uint32_t i = 0u; i < region_count; ++i) {
            uint32_t region_span = sector_counts[i] * sector_sizes[i];
            uint32_t dst = region_count - 1u - i;

            if (region_span > current_addr) {
                burner_nor_geometry_clear(geometry);
                return ESP_ERR_INVALID_SIZE;
            }
            current_addr -= region_span;
            geometry->regions[dst].addr_begin = current_addr;
            geometry->regions[dst].addr_end = current_addr + region_span;
            geometry->regions[dst].sector_size = sector_sizes[i];
        }
    } else {
        current_addr = 0u;
        for (uint32_t i = 0u; i < region_count; ++i) {
            uint32_t region_span = sector_counts[i] * sector_sizes[i];

            geometry->regions[i].addr_begin = current_addr;
            geometry->regions[i].addr_end = current_addr + region_span;
            geometry->regions[i].sector_size = sector_sizes[i];
            current_addr += region_span;
        }
    }

    if (!burner_nor_geometry_is_valid(geometry) ||
        geometry->regions[geometry->region_count - 1u].addr_end != device_size) {
        burner_nor_geometry_clear(geometry);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t burner_nor_geometry_sector_bounds(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    uint32_t *sector_begin_out,
    uint32_t *sector_end_out,
    uint32_t *sector_size_out)
{
    if (!burner_nor_geometry_is_valid(geometry)) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t i = 0u; i < geometry->region_count; ++i) {
        const burner_nor_region_t *region = &geometry->regions[i];

        if (addr >= region->addr_begin && addr < region->addr_end) {
            uint32_t offset = addr - region->addr_begin;
            uint32_t sector_index = offset / region->sector_size;
            uint32_t sector_begin = region->addr_begin + (sector_index * region->sector_size);
            uint32_t sector_end = sector_begin + region->sector_size;

            if (sector_begin_out != NULL) {
                *sector_begin_out = sector_begin;
            }
            if (sector_end_out != NULL) {
                *sector_end_out = sector_end;
            }
            if (sector_size_out != NULL) {
                *sector_size_out = region->sector_size;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t burner_nor_geometry_next_sector_begin(
    const burner_nor_geometry_t *geometry,
    uint32_t sector_begin,
    uint32_t *next_sector_begin_out)
{
    uint32_t current_sector_begin = 0u;
    uint32_t current_sector_end = 0u;

    if (next_sector_begin_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_nor_geometry_sector_bounds(
            geometry,
            sector_begin,
            &current_sector_begin,
            &current_sector_end,
            NULL) != ESP_OK ||
        current_sector_begin != sector_begin) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_nor_geometry_sector_bounds(geometry, current_sector_end, next_sector_begin_out, NULL, NULL) == ESP_OK) {
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t burner_nor_geometry_sector_begin_ceil(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    uint32_t *sector_begin_out)
{
    uint32_t sector_begin = 0u;
    uint32_t sector_end = 0u;
    esp_err_t err;

    if (sector_begin_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_nor_geometry_sector_bounds(geometry, addr, &sector_begin, &sector_end, NULL);
    if (err != ESP_OK) {
        return err;
    }
    if (addr == sector_begin) {
        *sector_begin_out = sector_begin;
        return ESP_OK;
    }
    return burner_nor_geometry_next_sector_begin(geometry, sector_begin, sector_begin_out);
}

static void burner_nor_region_cursor_clear(burner_nor_region_cursor_t *cursor)
{
    if (cursor == NULL) {
        return;
    }
    memset(cursor, 0, sizeof(*cursor));
}

static bool burner_nor_region_cursor_is_valid(const burner_nor_region_cursor_t *cursor)
{
    return cursor != NULL &&
           cursor->region_index < BURNER_NOR_GEOMETRY_REGION_MAX &&
           cursor->sector_size > 0u &&
           cursor->addr_end > cursor->addr_begin;
}

static esp_err_t burner_nor_geometry_region_cursor_load(
    const burner_nor_geometry_t *geometry,
    uint32_t region_index,
    burner_nor_region_cursor_t *cursor)
{
    const burner_nor_region_t *region;

    if (!burner_nor_geometry_is_valid(geometry) || cursor == NULL || region_index >= geometry->region_count) {
        return ESP_ERR_INVALID_ARG;
    }

    region = &geometry->regions[region_index];
    cursor->region_index = (uint8_t)region_index;
    cursor->addr_begin = region->addr_begin;
    cursor->addr_end = region->addr_end;
    cursor->sector_size = region->sector_size;
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_region_cursor_begin(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    burner_nor_region_cursor_t *cursor)
{
    if (!burner_nor_geometry_is_valid(geometry) || cursor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0u; i < geometry->region_count; ++i) {
        const burner_nor_region_t *region = &geometry->regions[i];

        if (addr >= region->addr_begin && addr < region->addr_end) {
            return burner_nor_geometry_region_cursor_load(geometry, i, cursor);
        }
    }

    burner_nor_region_cursor_clear(cursor);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t burner_nor_geometry_region_cursor_advance(
    const burner_nor_geometry_t *geometry,
    burner_nor_region_cursor_t *cursor)
{
    if (!burner_nor_region_cursor_is_valid(cursor)) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_nor_geometry_region_cursor_load(geometry, (uint32_t)cursor->region_index + 1u, cursor);
}

static esp_err_t burner_nor_geometry_region_cursor_seek_forward(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    burner_nor_region_cursor_t *cursor)
{
    esp_err_t err;

    if (cursor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_nor_region_cursor_is_valid(cursor)) {
        return burner_nor_geometry_region_cursor_begin(geometry, addr, cursor);
    }
    if (addr >= cursor->addr_begin && addr < cursor->addr_end) {
        return ESP_OK;
    }
    if (addr < cursor->addr_begin) {
        return burner_nor_geometry_region_cursor_begin(geometry, addr, cursor);
    }

    while (addr >= cursor->addr_end) {
        err = burner_nor_geometry_region_cursor_advance(geometry, cursor);
        if (err != ESP_OK) {
            burner_nor_region_cursor_clear(cursor);
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_sector_bounds_in_cursor(
    const burner_nor_region_cursor_t *cursor,
    uint32_t addr,
    uint32_t *sector_begin_out,
    uint32_t *sector_end_out,
    uint32_t *sector_size_out)
{
    uint32_t offset;
    uint32_t sector_begin;
    uint32_t sector_end;

    if (!burner_nor_region_cursor_is_valid(cursor) || addr < cursor->addr_begin || addr >= cursor->addr_end) {
        return ESP_ERR_INVALID_ARG;
    }

    offset = addr - cursor->addr_begin;
    sector_begin = cursor->addr_begin + ((offset / cursor->sector_size) * cursor->sector_size);
    sector_end = sector_begin + cursor->sector_size;
    if (sector_begin_out != NULL) {
        *sector_begin_out = sector_begin;
    }
    if (sector_end_out != NULL) {
        *sector_end_out = sector_end;
    }
    if (sector_size_out != NULL) {
        *sector_size_out = cursor->sector_size;
    }
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_stage_bytes_in_cursor(
    const burner_nor_region_cursor_t *cursor,
    uint32_t addr,
    uint32_t remaining_bytes,
    uint32_t *stage_bytes_out)
{
    uint32_t sector_end = 0u;
    uint32_t stage_bytes;
    esp_err_t err;

    if (stage_bytes_out == NULL || remaining_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_nor_geometry_sector_bounds_in_cursor(cursor, addr, NULL, &sector_end, NULL);
    if (err != ESP_OK || sector_end <= addr) {
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }

    stage_bytes = sector_end - addr;
    if (stage_bytes > remaining_bytes) {
        stage_bytes = remaining_bytes;
    }
    if (stage_bytes == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *stage_bytes_out = stage_bytes;
    return ESP_OK;
}

static esp_err_t burner_nor_geometry_stage_bytes_for_addr(
    const burner_nor_geometry_t *geometry,
    uint32_t addr,
    uint32_t remaining_bytes,
    uint32_t *stage_bytes_out)
{
    burner_nor_region_cursor_t cursor = {0};
    esp_err_t err;

    err = burner_nor_geometry_region_cursor_begin(geometry, addr, &cursor);
    if (err != ESP_OK) {
        return err;
    }
    return burner_nor_geometry_stage_bytes_in_cursor(&cursor, addr, remaining_bytes, stage_bytes_out);
}

static esp_err_t burner_nor_geometry_limit_prefix(
    burner_nor_geometry_t *geometry,
    uint32_t device_size_limit)
{
    burner_nor_geometry_t src;
    uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t remaining_bytes;
    uint32_t region_count = 0u;

    if (geometry == NULL || !burner_nor_geometry_is_valid(geometry) || device_size_limit == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (device_size_limit >= geometry->regions[geometry->region_count - 1u].addr_end) {
        return ESP_OK;
    }

    src = *geometry;
    remaining_bytes = device_size_limit;
    for (uint32_t i = 0u; i < src.region_count && remaining_bytes > 0u; ++i) {
        const burner_nor_region_t *region = &src.regions[i];
        uint32_t region_bytes = region->addr_end - region->addr_begin;
        uint32_t take_bytes = (region_bytes < remaining_bytes) ? region_bytes : remaining_bytes;

        if (take_bytes == 0u || (take_bytes % region->sector_size) != 0u ||
            region_count >= BURNER_NOR_GEOMETRY_REGION_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }

        sector_counts[region_count] = take_bytes / region->sector_size;
        sector_sizes[region_count] = region->sector_size;
        remaining_bytes -= take_bytes;
        ++region_count;
    }

    if (remaining_bytes != 0u || region_count == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    return burner_nor_geometry_build(
        geometry,
        device_size_limit,
        sector_counts,
        sector_sizes,
        region_count,
        false);
}

static uint32_t burner_gb_mapper_device_size_limit(burner_gb_mapper_t mapper)
{
    return (mapper == BURNER_GB_MAPPER_MBC3) ? (2u * 1024u * 1024u) : 0u;
}

static uint8_t burner_gb_mapper_normalize_rom_bank(burner_gb_mapper_t mapper, uint16_t bank)
{
    if (mapper == BURNER_GB_MAPPER_MBC3 && bank == 0u) {
        return 1u;
    }
    return (uint8_t)(bank & 0xFFu);
}

static void burner_mbc5_addr_to_program_window(
    uint32_t flash_addr,
    uint16_t *bank_out,
    uint16_t *cart_addr_out,
    uint32_t *bank_off_out)
{
    uint32_t bank = flash_addr / BURN_MBC5_ROM_BANK_BYTES;
    uint32_t bank_off = flash_addr % BURN_MBC5_ROM_BANK_BYTES;
    uint16_t cart_addr = (uint16_t)(0x4000u + bank_off);

    if (s_gb_mapper_kind == BURNER_GB_MAPPER_MBC3 && bank == 0u) {
        cart_addr = (uint16_t)bank_off;
    }

    if (bank_out != NULL) {
        *bank_out = (uint16_t)bank;
    }
    if (cart_addr_out != NULL) {
        *cart_addr_out = cart_addr;
    }
    if (bank_off_out != NULL) {
        *bank_off_out = bank_off;
    }
}

static esp_err_t burner_nor_geometry_largest_sector_size_in_range(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t total_bytes,
    uint32_t *largest_sector_size_out)
{
    uint32_t addr_end;
    uint32_t largest_sector_size = 0u;
    burner_nor_region_cursor_t cursor = {0};
    esp_err_t err;

    if (largest_sector_size_out == NULL || total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (addr_begin > (UINT32_MAX - (total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    addr_end = addr_begin + total_bytes - 1u;
    err = burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    while (cursor.addr_begin <= addr_end) {
        if (cursor.sector_size > largest_sector_size) {
            largest_sector_size = cursor.sector_size;
        }
        if (cursor.addr_end > addr_end) {
            break;
        }
        if (burner_nor_geometry_region_cursor_advance(geometry, &cursor) != ESP_OK) {
            break;
        }
    }

    if (largest_sector_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *largest_sector_size_out = largest_sector_size;
    return ESP_OK;
}

static uint32_t burner_nor_geometry_sector_count_from_range(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t addr_end)
{
    uint64_t total_sectors = 0u;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr;
    uint64_t range_end_exclusive;

    if (addr_end < addr_begin ||
        burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return 0u;
    }

    sector_addr = cursor.addr_begin + (((addr_begin - cursor.addr_begin) / cursor.sector_size) * cursor.sector_size);
    range_end_exclusive = (uint64_t)addr_end + 1u;
    while ((uint64_t)sector_addr < range_end_exclusive) {
        uint64_t region_end_exclusive =
            ((uint64_t)cursor.addr_end < range_end_exclusive) ? (uint64_t)cursor.addr_end : range_end_exclusive;
        uint64_t region_sectors;

        if (region_end_exclusive <= (uint64_t)sector_addr) {
            break;
        }
        region_sectors = ((region_end_exclusive - (uint64_t)sector_addr) + (uint64_t)cursor.sector_size - 1u) /
                         (uint64_t)cursor.sector_size;
        total_sectors += region_sectors;
        if (total_sectors > UINT32_MAX) {
            return UINT32_MAX;
        }
        if (region_end_exclusive >= range_end_exclusive) {
            break;
        }
        if (burner_nor_geometry_region_cursor_advance(geometry, &cursor) != ESP_OK) {
            break;
        }
        sector_addr = cursor.addr_begin;
    }

    return (uint32_t)total_sectors;
}

static uint32_t burner_nor_geometry_erase_bytes_from_range(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t addr_end)
{
    uint64_t total_bytes = 0u;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr;
    uint64_t range_end_exclusive;

    if (addr_end < addr_begin ||
        burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return 0u;
    }

    sector_addr = cursor.addr_begin + (((addr_begin - cursor.addr_begin) / cursor.sector_size) * cursor.sector_size);
    range_end_exclusive = (uint64_t)addr_end + 1u;
    while ((uint64_t)sector_addr < range_end_exclusive) {
        uint64_t region_end_exclusive =
            ((uint64_t)cursor.addr_end < range_end_exclusive) ? (uint64_t)cursor.addr_end : range_end_exclusive;
        uint64_t region_sectors;

        if (region_end_exclusive <= (uint64_t)sector_addr) {
            break;
        }
        region_sectors = ((region_end_exclusive - (uint64_t)sector_addr) + (uint64_t)cursor.sector_size - 1u) /
                         (uint64_t)cursor.sector_size;
        total_bytes += region_sectors * (uint64_t)cursor.sector_size;
        if (total_bytes > UINT32_MAX) {
            return UINT32_MAX;
        }
        if (region_end_exclusive >= range_end_exclusive) {
            break;
        }
        if (burner_nor_geometry_region_cursor_advance(geometry, &cursor) != ESP_OK) {
            break;
        }
        sector_addr = cursor.addr_begin;
    }

    return (uint32_t)total_bytes;
}

static uint32_t burner_nor_geometry_planned_stage_erase_sector_count(
    const burner_nor_geometry_t *geometry,
    uint32_t addr_begin,
    uint32_t total_bytes,
    uint32_t stage_capacity)
{
    uint32_t processed = 0u;
    uint64_t total_sectors = 0u;

    if (!burner_nor_geometry_is_valid(geometry) || total_bytes == 0u || stage_capacity == 0u) {
        return 0u;
    }

    while (processed < total_bytes) {
        uint32_t stage_addr = addr_begin + processed;
        uint32_t stage_bytes = total_bytes - processed;
        uint32_t stage_erase_begin = stage_addr;
        uint32_t stage_erase_end;

        if (stage_bytes > stage_capacity) {
            stage_bytes = stage_capacity;
        }
        stage_erase_end = stage_addr + stage_bytes - 1u;
        if (processed > 0u) {
            if (burner_nor_geometry_sector_begin_ceil(geometry, stage_addr, &stage_erase_begin) != ESP_OK ||
                stage_erase_begin > stage_erase_end) {
                processed += stage_bytes;
                continue;
            }
        }

        total_sectors += burner_nor_geometry_sector_count_from_range(
            geometry,
            stage_erase_begin,
            stage_erase_end);
        if (total_sectors > UINT32_MAX) {
            return UINT32_MAX;
        }
        processed += stage_bytes;
    }

    return (uint32_t)total_sectors;
}

void burner_status_record_erase_sectors(uint32_t sector_count, uint32_t sector_size)
{
    uint64_t total;

    if (s_status_lock == NULL || sector_count == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    total = (uint64_t)s_status.erase_sector_count + (uint64_t)sector_count;
    if (total > UINT32_MAX) {
        s_status.erase_sector_count = UINT32_MAX;
    } else {
        s_status.erase_sector_count = (uint32_t)total;
    }
    s_status.erase_sector_size = sector_size;
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_speed_sample_locked(
    uint32_t bytes,
    uint64_t elapsed_us,
    uint32_t *current_bps,
    uint32_t *avg_bps,
    uint32_t *min_bps,
    uint32_t *max_bps,
    uint32_t *total_bytes,
    uint64_t *total_us)
{
    uint32_t instant_bps;
    uint64_t total_bytes64;
    uint64_t total_us64;

    if (bytes == 0u || elapsed_us == 0u || current_bps == NULL || avg_bps == NULL || min_bps == NULL ||
        max_bps == NULL || total_bytes == NULL || total_us == NULL) {
        return;
    }

    instant_bps = (uint32_t)(((uint64_t)bytes * 1000000ULL) / elapsed_us);

    *current_bps = instant_bps;
    if (*min_bps == 0u || instant_bps < *min_bps) {
        *min_bps = instant_bps;
    }
    if (instant_bps > *max_bps) {
        *max_bps = instant_bps;
    }

    total_bytes64 = (uint64_t)(*total_bytes) + (uint64_t)bytes;
    total_us64 = (*total_us) + elapsed_us;
    if (total_bytes64 > (uint64_t)UINT32_MAX) {
        *total_bytes = UINT32_MAX;
    } else {
        *total_bytes = (uint32_t)total_bytes64;
    }
    *total_us = total_us64;

    if (total_us64 > 0u) {
        uint64_t avg_bps64 = (total_bytes64 * 1000000ULL) / total_us64;
        if (avg_bps64 > (uint64_t)UINT32_MAX) {
            *avg_bps = UINT32_MAX;
        } else {
            *avg_bps = (uint32_t)avg_bps64;
        }
    }
}

void burner_status_record_tf_to_psram_copy(uint32_t bytes, uint64_t elapsed_us)
{
    if (s_status_lock == NULL || bytes == 0u || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_speed_sample_locked(
        bytes,
        elapsed_us,
        &s_status.tf_to_psram_speed_current_bps,
        &s_status.tf_to_psram_speed_avg_bps,
        &s_status.tf_to_psram_speed_min_bps,
        &s_status.tf_to_psram_speed_max_bps,
        &s_status.tf_to_psram_total_bytes,
        &s_status.tf_to_psram_total_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_dump_read(uint32_t bytes, uint64_t elapsed_us)
{
    if (s_status_lock == NULL || bytes == 0u || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_speed_sample_locked(
        bytes,
        elapsed_us,
        &s_status.dump_read_speed_current_bps,
        &s_status.dump_read_speed_avg_bps,
        &s_status.dump_read_speed_min_bps,
        &s_status.dump_read_speed_max_bps,
        &s_status.dump_read_total_bytes,
        &s_status.dump_read_total_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_dump_write(uint32_t bytes, uint64_t elapsed_us)
{
    if (s_status_lock == NULL || bytes == 0u || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_speed_sample_locked(
        bytes,
        elapsed_us,
        &s_status.dump_write_speed_current_bps,
        &s_status.dump_write_speed_avg_bps,
        &s_status.dump_write_speed_min_bps,
        &s_status.dump_write_speed_max_bps,
        &s_status.dump_write_total_bytes,
        &s_status.dump_write_total_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_elapsed_total(uint64_t *total_us, uint64_t elapsed_us)
{
    if (total_us == NULL || elapsed_us == 0u) {
        return;
    }

    if (*total_us > UINT64_MAX - elapsed_us) {
        *total_us = UINT64_MAX;
    } else {
        *total_us += elapsed_us;
    }
}

void burner_status_record_dump_wait(uint64_t elapsed_us)
{
    if (s_status_lock == NULL || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_elapsed_total(&s_status.dump_wait_total_us, elapsed_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_dump_finalize(uint64_t elapsed_us)
{
    if (s_status_lock == NULL || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_elapsed_total(&s_status.dump_finalize_total_us, elapsed_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_mbc5_buffer_write(bool fallback)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (fallback) {
        if (s_status.mbc5_buffer_fallback_count < UINT32_MAX) {
            s_status.mbc5_buffer_fallback_count++;
        }
    } else {
        if (s_status.mbc5_buffer_write_ok_count < UINT32_MAX) {
            s_status.mbc5_buffer_write_ok_count++;
        }
    }
    xSemaphoreGive(s_status_lock);
}

void burner_status_speed_update_locked(
    burner_state_t prev_state,
    uint32_t prev_total,
    uint32_t processed,
    uint32_t total)
{
    bool prev_tracks = burner_status_tracks_speed(prev_state);
    bool now_tracks = burner_status_tracks_speed(s_status.state);
    uint64_t now_us;

    if (!now_tracks) {
        if (s_status.state == BURNER_STATE_IDLE) {
            burner_status_speed_reset_locked();
        }
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();
    if (!prev_tracks || processed < s_status.speed_last_bytes || (processed == 0u && total != prev_total)) {
        burner_status_speed_reset_locked();
        if (s_status.write_start_us > 0u) {
            s_status.speed_warmup_until_us = s_status.write_start_us + BURNER_SPEED_WARMUP_US;
        }
        if (s_status.speed_warmup_until_us == 0u || now_us >= s_status.speed_warmup_until_us) {
            s_status.speed_start_us = now_us;
            s_status.speed_start_bytes = processed;
            s_status.speed_last_us = now_us;
        }
        s_status.speed_last_bytes = processed;
        return;
    }

    if (s_status.speed_warmup_until_us > 0u && now_us < s_status.speed_warmup_until_us) {
        s_status.speed_current_bps = 0u;
        s_status.speed_last_bytes = processed;
        return;
    }
    if (s_status.speed_start_us == 0u || s_status.speed_last_us == 0u) {
        s_status.speed_start_us = now_us;
        s_status.speed_start_bytes = processed;
        s_status.speed_last_us = now_us;
        s_status.speed_last_bytes = processed;
        s_status.speed_current_bps = 0u;
        return;
    }

    if (processed >= s_status.speed_last_bytes && now_us > s_status.speed_last_us) {
        uint32_t delta_bytes = processed - s_status.speed_last_bytes;
        uint64_t delta_us = now_us - s_status.speed_last_us;

        if (delta_bytes > 0u && delta_us > 0u) {
            uint32_t instant_bps = (uint32_t)(((uint64_t)delta_bytes * 1000000ULL) / delta_us);
            s_status.speed_current_bps = instant_bps;
            if (s_status.speed_min_bps == 0u || instant_bps < s_status.speed_min_bps) {
                s_status.speed_min_bps = instant_bps;
            }
            if (instant_bps > s_status.speed_max_bps) {
                s_status.speed_max_bps = instant_bps;
            }
        }
        s_status.speed_last_us = now_us;
        s_status.speed_last_bytes = processed;
    }

    if (processed > s_status.speed_start_bytes && s_status.speed_start_us > 0u && now_us > s_status.speed_start_us) {
        uint32_t measured_bytes = processed - s_status.speed_start_bytes;
        uint64_t total_us = now_us - s_status.speed_start_us;
        s_status.speed_avg_bps = (uint32_t)(((uint64_t)measured_bytes * 1000000ULL) / total_us);
        if (s_status.speed_max_bps == 0u && s_status.speed_avg_bps > 0u) {
            s_status.speed_max_bps = s_status.speed_avg_bps;
        }
        if (s_status.speed_min_bps == 0u && s_status.speed_avg_bps > 0u) {
            s_status.speed_min_bps = s_status.speed_avg_bps;
        }
    }
}

void burner_status_reset(void)
{
    s_status.state = BURNER_STATE_IDLE;
    s_status.progress = 0;
    s_status.total_bytes = 0;
    s_status.processed_bytes = 0;
    burner_status_phase_reset_locked();
    burner_status_speed_reset_locked();
    burner_status_probe_reset_locked();
    burner_cancel_reset_locked();
    s_status.rom_name[0] = '\0';
    s_status.rom_path[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message), "%s", "idle");
}

int burner_calc_progress_percent_u64(uint64_t processed, uint64_t total)
{
    uint64_t progress = 0u;

    if (total == 0u) {
        return 0;
    }
    if (processed >= total) {
        return 100;
    }

    progress = (processed * 100ULL) / total;
    if (progress > 100ULL) {
        progress = 100ULL;
    }
    return (int)progress;
}

void burner_status_update(
    burner_state_t state,
    int progress,
    uint32_t processed,
    uint32_t total,
    const char *message,
    const char *rom_name,
    const char *rom_path)
{
    char ui_message[96];
    burner_state_t prev_state;
    uint32_t prev_total;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    prev_state = s_status.state;
    prev_total = s_status.total_bytes;
    s_status.state = state;
    s_status.progress = progress;
    s_status.processed_bytes = processed;
    s_status.total_bytes = total;
    burner_status_speed_update_locked(prev_state, prev_total, processed, total);

    if (message != NULL) {
        snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    }
    if (rom_name != NULL) {
        snprintf(s_status.rom_name, sizeof(s_status.rom_name), "%s", rom_name);
    }
    if (rom_path != NULL) {
        snprintf(s_status.rom_path, sizeof(s_status.rom_path), "%s", rom_path);
    }
    xSemaphoreGive(s_status_lock);

    ui_set_burn_progress(progress, processed, total);
    if (message != NULL && message[0] != '\0') {
        snprintf(ui_message, sizeof(ui_message), "burner %s: %s", burner_state_to_str(state), message);
    } else {
        snprintf(ui_message, sizeof(ui_message), "burner state: %s", burner_state_to_str(state));
    }
    ui_set_status_text(ui_message);
}

void burner_status_snapshot(burner_status_t *out)
{
    uint64_t now_us;

    if (out == NULL) {
        return;
    }

    if (s_status_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);

    now_us = (uint64_t)esp_timer_get_time();
    if (out->task_start_us > 0u && now_us > out->task_start_us) {
        out->task_elapsed_us += now_us - out->task_start_us;
    }
    if (out->erase_start_us > 0u && now_us > out->erase_start_us) {
        out->erase_elapsed_us += now_us - out->erase_start_us;
    }
    if (out->write_start_us > 0u && now_us > out->write_start_us) {
        out->write_elapsed_us += now_us - out->write_start_us;
    }
}

bool burner_sanitize_filename(const char *input, char *output, size_t output_len)
{
    size_t out_idx = 0;
    size_t i;

    if (input == NULL || output == NULL || output_len < 2) {
        return false;
    }

    for (i = 0; input[i] != '\0' && out_idx + 1 < output_len; i++) {
        unsigned char ch = (unsigned char)input[i];
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            output[out_idx++] = (char)ch;
        } else if (ch == ' ') {
            output[out_idx++] = '_';
        }
    }

    if (out_idx == 0) {
        return false;
    }

    output[out_idx] = '\0';
    return true;
}

bool burner_validate_file_name(const char *input, char *output, size_t output_len)
{
    size_t out_idx = 0;

    if (input == NULL || output == NULL || output_len < 2) {
        return false;
    }

    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)input[i];

        if (ch < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '\"' || ch == '<' || ch == '>' || ch == '|') {
            return false;
        }

        if (out_idx + 1 >= output_len) {
            return false;
        }
        output[out_idx++] = (char)ch;
    }

    if (out_idx == 0) {
        return false;
    }

    output[out_idx] = '\0';
    if (strcmp(output, ".") == 0 || strcmp(output, "..") == 0) {
        return false;
    }

    return true;
}

bool burner_wallclock_time_valid(time_t now)
{
    return now >= BURNER_VALID_WALLCLOCK_MIN;
}

bool burner_get_wallclock_time(time_t *now_out, struct tm *tm_out)
{
    time_t now = 0;
    struct tm tm_now = {0};

    time(&now);
    if (!burner_wallclock_time_valid(now)) {
        return false;
    }
    if (tm_out != NULL && localtime_r(&now, &tm_now) == NULL) {
        return false;
    }

    if (now_out != NULL) {
        *now_out = now;
    }
    if (tm_out != NULL) {
        *tm_out = tm_now;
    }
    return true;
}

bool burner_extract_ascii_cart_title(
    const uint8_t *raw,
    size_t raw_len,
    char *title,
    size_t title_len)
{
    size_t out_idx = 0u;
    size_t trim_len = 0u;
    size_t i;

    if (raw == NULL || title == NULL || title_len < 2u) {
        return false;
    }

    title[0] = '\0';
    for (i = 0u; i < raw_len && out_idx + 1u < title_len; ++i) {
        uint8_t ch = raw[i];

        if (ch == 0x00u || ch == 0xFFu) {
            break;
        }
        if (ch < 0x20u || ch > 0x7Eu) {
            break;
        }

        title[out_idx++] = (char)ch;
        if (ch != ' ') {
            trim_len = out_idx;
        }
    }

    if (trim_len == 0u) {
        return false;
    }

    title[trim_len] = '\0';
    return true;
}

bool burner_apply_current_file_mtime(const char *path, time_t *applied_time_out)
{
    struct utimbuf times = {0};
    time_t now = 0;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (!burner_get_wallclock_time(&now, NULL)) {
        return false;
    }

    times.actime = now;
    times.modtime = now;
    if (utime(path, &times) != 0) {
        ESP_LOGW(BURNER_TAG, "set mtime failed for %s: errno=%d", path, errno);
        return false;
    }

    if (applied_time_out != NULL) {
        *applied_time_out = now;
    }
    return true;
}

long long burner_fixup_file_mtime_for_api(const char *path, const struct stat *st)
{
    struct stat refreshed = {0};
    time_t repaired_time = 0;
    time_t current_mtime = 0;

    if (st == NULL) {
        return 0;
    }

    current_mtime = st->st_mtime;
    if (current_mtime > BURNER_SUSPECT_FILE_MTIME_MAX) {
        return (long long)current_mtime;
    }
    if (path == NULL || path[0] == '\0') {
        return (long long)current_mtime;
    }
    if (!burner_apply_current_file_mtime(path, &repaired_time)) {
        return (long long)current_mtime;
    }
    if (stat(path, &refreshed) == 0) {
        return (long long)refreshed.st_mtime;
    }
    return (long long)repaired_time;
}

bool burner_try_probe_cart_title(
    burner_cart_mode_t cart_mode,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool gba_force_multi,
    char *title,
    size_t title_len)
{
    burner_task_param_t probe_job = {0};
    uint8_t header[BURNER_MBC5_TITLE_LEN] = {0};
    size_t header_len = 0u;
    esp_err_t err = ESP_OK;

    if (title == NULL || title_len < 2u || total_bytes == 0u) {
        return false;
    }
    if (burner_task_is_running_snapshot()) {
        return false;
    }

    title[0] = '\0';
    err = burner_spi_init();
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "dump title probe init failed: %s", esp_err_to_name(err));
        return false;
    }

    probe_job.mode = BURNER_JOB_READ_ROM;
    probe_job.cart_mode = cart_mode;
    probe_job.addr_begin = addr_begin;
    probe_job.total_bytes = total_bytes;
    probe_job.gba_force_multi = gba_force_multi;

    burner_spi_lock_take();
    if (cart_mode == BURNER_CART_MODE_GBA) {
        header_len = BURNER_GBA_TITLE_LEN;
        err = burner_spi_prepare_burn_gba(&probe_job);
        if (err == ESP_OK) {
            err = burner_bacon_gba_read_block(
                header,
                header_len,
                addr_begin + BURNER_GBA_TITLE_OFFSET,
                burner_is_gba_multi_card(&probe_job));
        }
    } else {
        header_len = BURNER_MBC5_TITLE_LEN;
        err = burner_spi_prepare_burn_mbc5(&probe_job);
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_read_block(
                header,
                header_len,
                addr_begin + BURNER_MBC5_TITLE_OFFSET);
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "dump title probe failed mode=%s addr=0x%08" PRIX32 ": %s",
            (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
            addr_begin,
            esp_err_to_name(err));
        return false;
    }

    return burner_extract_ascii_cart_title(header, header_len, title, title_len);
}

static bool burner_memmem_ascii(
    const uint8_t *buf,
    size_t buf_len,
    const char *needle)
{
    size_t needle_len;
    size_t i;

    if (buf == NULL || needle == NULL) {
        return false;
    }
    needle_len = strlen(needle);
    if (needle_len == 0u || buf_len < needle_len) {
        return false;
    }

    for (i = 0u; i + needle_len <= buf_len; ++i) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static void burner_probe_scan_yield_if_needed(uint64_t *last_yield_us)
{
    uint64_t now_us;

    if (last_yield_us == NULL) {
        return;
    }

    now_us = esp_timer_get_time();
    if (*last_yield_us != 0u && (now_us - *last_yield_us) < BURNER_CPU_YIELD_INTERVAL_US) {
        return;
    }

    *last_yield_us = now_us;
    vTaskDelay(1);
}

static bool burner_memmem_ascii_windowed(
    const uint8_t *buf,
    size_t buf_len,
    const char *needle)
{
    size_t needle_len;
    size_t offset = 0u;
    uint64_t last_yield_us = 0u;

    if (buf == NULL || needle == NULL) {
        return false;
    }
    needle_len = strlen(needle);
    if (needle_len == 0u || buf_len < needle_len) {
        return false;
    }

    while (offset < buf_len) {
        size_t scan_start = (offset > (needle_len - 1u)) ? (offset - (needle_len - 1u)) : 0u;
        size_t covered = offset - scan_start;
        size_t scan_len = buf_len - scan_start;
        size_t window_len = BURNER_PROBE_SCAN_WINDOW_BYTES + covered;

        if (scan_len > window_len) {
            scan_len = window_len;
        }
        if (burner_memmem_ascii(buf + scan_start, scan_len, needle)) {
            return true;
        }
        offset += BURNER_PROBE_SCAN_WINDOW_BYTES;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    return false;
}

static bool burner_detect_gba_save_type_in_span(
    const uint8_t *buf,
    size_t buf_len,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out)
{
    if (save_type_out == NULL || save_size_out == NULL) {
        return false;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;

    if (buf == NULL || buf_len == 0u) {
        return false;
    }

    if (burner_memmem_ascii_windowed(buf, buf_len, "FLASH1M_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_FLASH;
        *save_size_out = 128u * 1024u;
        return true;
    }
    if (burner_memmem_ascii_windowed(buf, buf_len, "FLASH512_V") ||
        burner_memmem_ascii_windowed(buf, buf_len, "FLASH_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_FLASH;
        *save_size_out = 64u * 1024u;
        return true;
    }
    if (burner_memmem_ascii_windowed(buf, buf_len, "EEPROM_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_EEPROM;
        *save_size_out = 8u * 1024u;
        return true;
    }
    if (burner_memmem_ascii_windowed(buf, buf_len, "SRAM_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
        *save_size_out = 32u * 1024u;
        return true;
    }

    return false;
}

static bool burner_detect_gba_save_type_from_rom_locked(
    const burner_task_param_t *job,
    const uint8_t *prefix,
    size_t prefix_len,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out)
{
    static const char *const s_save_signatures[] = {
        "FLASH1M_V",
        "FLASH512_V",
        "FLASH_V",
        "EEPROM_V",
        "SRAM_V",
    };
    uint8_t *scan_buf = NULL;
    uint32_t scan_total = 0u;
    uint32_t offset = 0u;
    size_t carry_len = 0u;
    size_t overlap = 0u;
    bool detected = false;
    uint64_t last_yield_us = 0u;
    size_t i;

    if (job == NULL || save_type_out == NULL || save_size_out == NULL) {
        return false;
    }

    scan_total = job->total_bytes;
    if (scan_total == 0u) {
        return false;
    }

    for (i = 0u; i < sizeof(s_save_signatures) / sizeof(s_save_signatures[0]); ++i) {
        size_t needle_len = strlen(s_save_signatures[i]);
        if (needle_len > 0u && overlap < needle_len - 1u) {
            overlap = needle_len - 1u;
        }
    }

    if (prefix != NULL && prefix_len > overlap) {
        prefix += prefix_len - overlap;
        prefix_len = overlap;
    }

    scan_buf = (uint8_t *)malloc(BURNER_GBA_SAVE_SCAN_STEP_BYTES + overlap);
    if (scan_buf == NULL) {
        return false;
    }
    if (prefix != NULL && prefix_len > 0u) {
        memcpy(scan_buf, prefix, prefix_len);
        carry_len = prefix_len;
    }

    while (offset < scan_total) {
        size_t chunk = scan_total - offset;
        size_t scan_len;
        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }

        if (burner_bacon_gba_read_block(
                scan_buf + carry_len,
                chunk,
                job->addr_begin + offset,
                burner_is_gba_multi_card(job)) != ESP_OK) {
            break;
        }

        scan_len = carry_len + chunk;
        if (burner_detect_gba_save_type_in_span(scan_buf, scan_len, save_type_out, save_size_out)) {
            detected = true;
            break;
        }

        if (scan_len <= overlap) {
            carry_len = scan_len;
        } else {
            carry_len = overlap;
            memmove(scan_buf, scan_buf + scan_len - carry_len, carry_len);
        }
        offset += (uint32_t)chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    free(scan_buf);
    return detected;
}

static bool burner_memmem_binary(const uint8_t *haystack, size_t haystack_len, const uint8_t *needle, size_t needle_len)
{
    size_t i;

    if (haystack == NULL || needle == NULL || needle_len == 0u || haystack_len < needle_len) {
        return false;
    }
    for (i = 0u; i + needle_len <= haystack_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool burner_memmem_binary_windowed(
    const uint8_t *haystack,
    size_t haystack_len,
    const uint8_t *needle,
    size_t needle_len)
{
    size_t offset = 0u;
    uint64_t last_yield_us = 0u;

    if (haystack == NULL || needle == NULL || needle_len == 0u || haystack_len < needle_len) {
        return false;
    }

    while (offset < haystack_len) {
        size_t scan_start = (offset > (needle_len - 1u)) ? (offset - (needle_len - 1u)) : 0u;
        size_t covered = offset - scan_start;
        size_t scan_len = haystack_len - scan_start;
        size_t window_len = BURNER_PROBE_SCAN_WINDOW_BYTES + covered;

        if (scan_len > window_len) {
            scan_len = window_len;
        }
        if (burner_memmem_binary(haystack + scan_start, scan_len, needle, needle_len)) {
            return true;
        }
        offset += BURNER_PROBE_SCAN_WINDOW_BYTES;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    return false;
}

static bool burner_detect_gbata_sram_patch_in_span(const uint8_t *buf, size_t buf_len)
{
    static const uint8_t s_gbata_flash512_stub1[] = {
        0x70, 0xB5, 0xA0, 0xB0, 0x00, 0x03, 0x40, 0x18,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x08, 0x78,
        0x10, 0x70, 0x01, 0x3B, 0x01, 0x32, 0x01, 0x31,
        0x00, 0x2B, 0xF8, 0xD1, 0x00, 0x20, 0x20, 0xB0,
    };
    static const uint8_t s_gbata_flash_v121_stub[] = {
        0x70, 0xB5, 0x90, 0xB0, 0x00, 0x03, 0x0A, 0x1C,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x01, 0x23,
        0x1B, 0x03, 0x10, 0x78, 0x08, 0x70, 0x01, 0x3B,
        0x01, 0x32, 0x01, 0x31, 0x00, 0x2B, 0xF8, 0xD1,
    };
    static const uint8_t s_gbata_flash512_ret_stub[] = {
        0x00, 0x03, 0x40, 0x18, 0xE0, 0x21, 0x09, 0x05,
        0x09, 0x18, 0x08, 0x78, 0x10, 0x70, 0x01, 0x3B,
        0x01, 0x32, 0x01, 0x31, 0x00, 0x2B, 0xF8, 0xD1,
        0x00, 0x20, 0x20, 0xB0, 0x70, 0xBC, 0x02, 0xBC,
        0x08, 0x47,
    };
    static const uint8_t s_gbata_flash_v121_ret_stub[] = {
        0x00, 0x03, 0x0A, 0x1C, 0xE0, 0x21, 0x09, 0x05,
        0x09, 0x18, 0x01, 0x23, 0x1B, 0x03, 0x10, 0x78,
        0x08, 0x70, 0x01, 0x3B, 0x01, 0x32, 0x01, 0x31,
        0x00, 0x2B, 0xF8, 0xD1, 0x00, 0x20, 0x10, 0xB0,
        0x7C, 0xBC, 0x02, 0xBC, 0x08, 0x47,
    };

    if (buf == NULL || buf_len < sizeof(s_gbata_flash_v121_stub)) {
        return false;
    }

    return burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash512_stub1, sizeof(s_gbata_flash512_stub1)) ||
           burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash_v121_stub, sizeof(s_gbata_flash_v121_stub)) ||
           burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash512_ret_stub, sizeof(s_gbata_flash512_ret_stub)) ||
           burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash_v121_ret_stub, sizeof(s_gbata_flash_v121_ret_stub));
}

static bool burner_detect_gbata_sram_patch_from_rom_locked(const burner_task_param_t *job)
{
    static const uint8_t s_gbata_flash512_stub1[] = {
        0x70, 0xB5, 0xA0, 0xB0, 0x00, 0x03, 0x40, 0x18,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x08, 0x78,
        0x10, 0x70, 0x01, 0x3B, 0x01, 0x32, 0x01, 0x31,
        0x00, 0x2B, 0xF8, 0xD1, 0x00, 0x20, 0x20, 0xB0,
    };
    static const uint8_t s_gbata_flash_v121_stub[] = {
        0x70, 0xB5, 0x90, 0xB0, 0x00, 0x03, 0x0A, 0x1C,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x01, 0x23,
        0x1B, 0x03, 0x10, 0x78, 0x08, 0x70, 0x01, 0x3B,
        0x01, 0x32, 0x01, 0x31, 0x00, 0x2B, 0xF8, 0xD1,
    };
    uint8_t *buf = NULL;
    uint32_t scan_total;
    uint32_t offset = 0u;
    size_t carry_len = 0u;
    size_t overlap;
    bool detected = false;
    uint64_t last_yield_us = 0u;

    if (job == NULL || job->total_bytes < sizeof(s_gbata_flash512_stub1)) {
        return false;
    }

    scan_total = job->total_bytes;
    overlap = sizeof(s_gbata_flash512_stub1) - 1u;
    if (overlap < sizeof(s_gbata_flash_v121_stub) - 1u) {
        overlap = sizeof(s_gbata_flash_v121_stub) - 1u;
    }
    buf = (uint8_t *)malloc(BURNER_GBA_SAVE_SCAN_STEP_BYTES + overlap);
    if (buf == NULL) {
        return false;
    }

    while (offset < scan_total) {
        size_t chunk = scan_total - offset;
        size_t scan_len;
        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }
        if (burner_bacon_gba_read_block(buf + carry_len, chunk, job->addr_begin + offset, burner_is_gba_multi_card(job)) != ESP_OK) {
            break;
        }
        scan_len = carry_len + chunk;
        if (burner_detect_gbata_sram_patch_in_span(buf, scan_len)) {
            detected = true;
            break;
        }
        if (scan_len <= overlap) {
            carry_len = scan_len;
        } else {
            carry_len = overlap;
            memmove(buf, buf + scan_len - carry_len, carry_len);
        }
        offset += (uint32_t)chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    free(buf);
    return detected;
}

static bool burner_detect_flash1m_repro_sram_patch_in_span(const uint8_t *buf, size_t buf_len)
{
    static const uint8_t s_flash1m_repro_patch_a[] = {
        0x80, 0x21, 0x09, 0x02, 0x09, 0x22, 0x12, 0x06,
        0x9F, 0x44, 0x90, 0x21, 0x09, 0x05, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x70, 0x70, 0x47, 0xFE, 0xFF,
        0xFF, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_b[] = {
        0x0E, 0x21, 0x09, 0x06, 0xFF, 0x24, 0x80, 0x22,
        0x13, 0x4B, 0x52, 0x02, 0x01, 0x3A, 0x8C, 0x54,
        0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_c[] = {
        0x08, 0x22, 0x00, 0x00, 0x52, 0x02, 0x01, 0x3A,
        0xA5, 0x54, 0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const char *const s_flash1m_repro_source_signatures[] = {
        "FLASH1M_V",
        "FLASH512_V",
    };
    size_t i;
    bool found_source_signature = false;

    if (buf == NULL || buf_len < sizeof(s_flash1m_repro_patch_a)) {
        return false;
    }

    for (i = 0u; i < sizeof(s_flash1m_repro_source_signatures) / sizeof(s_flash1m_repro_source_signatures[0]); ++i) {
        if (burner_memmem_ascii_windowed(buf, buf_len, s_flash1m_repro_source_signatures[i])) {
            found_source_signature = true;
            break;
        }
    }
    if (!found_source_signature) {
        return false;
    }

    return burner_memmem_binary_windowed(buf, buf_len, s_flash1m_repro_patch_a, sizeof(s_flash1m_repro_patch_a)) &&
           burner_memmem_binary_windowed(buf, buf_len, s_flash1m_repro_patch_b, sizeof(s_flash1m_repro_patch_b)) &&
           burner_memmem_binary_windowed(buf, buf_len, s_flash1m_repro_patch_c, sizeof(s_flash1m_repro_patch_c));
}

static bool burner_detect_flash1m_repro_sram_patch_from_rom_locked(const burner_task_param_t *job)
{
    static const uint8_t s_flash1m_repro_patch_a[] = {
        0x80, 0x21, 0x09, 0x02, 0x09, 0x22, 0x12, 0x06,
        0x9F, 0x44, 0x90, 0x21, 0x09, 0x05, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x70, 0x70, 0x47, 0xFE, 0xFF,
        0xFF, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_b[] = {
        0x0E, 0x21, 0x09, 0x06, 0xFF, 0x24, 0x80, 0x22,
        0x13, 0x4B, 0x52, 0x02, 0x01, 0x3A, 0x8C, 0x54,
        0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_c[] = {
        0x08, 0x22, 0x00, 0x00, 0x52, 0x02, 0x01, 0x3A,
        0xA5, 0x54, 0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const char *const s_flash1m_repro_source_signatures[] = {
        "FLASH1M_V",
        "FLASH512_V",
    };
    uint8_t *buf = NULL;
    uint32_t scan_total;
    uint32_t offset = 0u;
    size_t carry_len = 0u;
    size_t overlap = sizeof(s_flash1m_repro_patch_a) - 1u;
    size_t i;
    uint64_t last_yield_us = 0u;

    if (job == NULL || job->total_bytes < sizeof(s_flash1m_repro_patch_a)) {
        return false;
    }

    if (overlap < sizeof(s_flash1m_repro_patch_b) - 1u) {
        overlap = sizeof(s_flash1m_repro_patch_b) - 1u;
    }
    if (overlap < sizeof(s_flash1m_repro_patch_c) - 1u) {
        overlap = sizeof(s_flash1m_repro_patch_c) - 1u;
    }
    for (i = 0u; i < sizeof(s_flash1m_repro_source_signatures) / sizeof(s_flash1m_repro_source_signatures[0]); ++i) {
        size_t needle_len = strlen(s_flash1m_repro_source_signatures[i]);
        if (overlap + 1u < needle_len) {
            overlap = needle_len - 1u;
        }
    }

    scan_total = job->total_bytes;
    buf = (uint8_t *)malloc(BURNER_GBA_SAVE_SCAN_STEP_BYTES + overlap);
    if (buf == NULL) {
        return false;
    }

    while (offset < scan_total) {
        size_t chunk = scan_total - offset;
        size_t scan_len;

        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }
        if (burner_bacon_gba_read_block(buf + carry_len, chunk, job->addr_begin + offset, burner_is_gba_multi_card(job)) != ESP_OK) {
            break;
        }

        scan_len = carry_len + chunk;
        if (burner_detect_flash1m_repro_sram_patch_in_span(buf, scan_len)) {
            free(buf);
            return true;
        }

        if (scan_len <= overlap) {
            carry_len = scan_len;
        } else {
            carry_len = overlap;
            memmove(buf, buf + scan_len - carry_len, carry_len);
        }
        offset += (uint32_t)chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    free(buf);
    return false;
}

static bool burner_detect_gba_sram_patch_in_span(
    const uint8_t *buf,
    size_t buf_len,
    burner_gba_sram_patch_kind_t *patch_kind_out)
{
    if (patch_kind_out == NULL) {
        return false;
    }

    *patch_kind_out = BURNER_GBA_SRAM_PATCH_NONE;
    if (burner_detect_gbata_sram_patch_in_span(buf, buf_len)) {
        *patch_kind_out = BURNER_GBA_SRAM_PATCH_GBATA;
        return true;
    }
    if (burner_detect_flash1m_repro_sram_patch_in_span(buf, buf_len)) {
        *patch_kind_out = BURNER_GBA_SRAM_PATCH_FLASH1M_REPRO;
        return true;
    }
    return false;
}

static esp_err_t burner_read_gba_rom_to_buffer_locked(
    const burner_task_param_t *job,
    uint8_t *dst,
    uint32_t offset,
    size_t total_len)
{
    size_t done = 0u;
    uint64_t last_yield_us = 0u;

    if (job == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (done < total_len) {
        size_t chunk = total_len - done;
        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }
        if (burner_bacon_gba_read_block(
                dst + done,
                chunk,
                job->addr_begin + offset + (uint32_t)done,
                burner_is_gba_multi_card(job)) != ESP_OK) {
            return ESP_FAIL;
        }
        done += chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    return ESP_OK;
}

esp_err_t burner_probe_gba_rom_analysis_locked(
    uint32_t device_size,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *save_detected_out,
    burner_gba_sram_patch_kind_t *patch_kind_out,
    bool *patch_detected_out)
{
    uint8_t *head_cache = NULL;
    uint32_t head_scan_bytes = 0u;
    burner_gba_save_type_t save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t save_size = 0u;
    bool save_detected = false;
    burner_gba_sram_patch_kind_t patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
    bool patch_detected = false;
    esp_err_t err;

    if (save_type_out == NULL || save_size_out == NULL || save_detected_out == NULL ||
        patch_kind_out == NULL || patch_detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;
    *save_detected_out = false;
    *patch_kind_out = BURNER_GBA_SRAM_PATCH_NONE;
    *patch_detected_out = false;

    if (device_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    head_scan_bytes = device_size;
    if (head_scan_bytes > BURNER_GBA_ANALYSIS_HEAD_BYTES) {
        head_scan_bytes = BURNER_GBA_ANALYSIS_HEAD_BYTES;
    }

    err = ESP_OK;
    if (head_scan_bytes > 0u) {
        head_cache = (uint8_t *)heap_caps_malloc(head_scan_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (head_cache != NULL) {
            err = burner_read_gba_rom_to_buffer_locked(
                &(burner_task_param_t){
                    .cart_mode = BURNER_CART_MODE_GBA,
                    .addr_begin = 0u,
                    .total_bytes = device_size,
                },
                head_cache,
                0u,
                head_scan_bytes);
            if (err == ESP_OK) {
                save_detected = burner_detect_gba_save_type_in_span(head_cache, head_scan_bytes, &save_type, &save_size);
                patch_detected = burner_detect_gba_sram_patch_in_span(head_cache, head_scan_bytes, &patch_kind);
            }
        } else {
            burner_task_param_t head_job = {
                .cart_mode = BURNER_CART_MODE_GBA,
                .addr_begin = 0u,
                .total_bytes = device_size,
            };

            head_job.total_bytes = head_scan_bytes;
            save_detected = burner_detect_gba_save_type_from_rom_locked(&head_job, NULL, 0u, &save_type, &save_size);
            patch_detected = burner_detect_gbata_sram_patch_from_rom_locked(&head_job);
            if (patch_detected) {
                patch_kind = BURNER_GBA_SRAM_PATCH_GBATA;
            } else if (burner_detect_flash1m_repro_sram_patch_from_rom_locked(&head_job)) {
                patch_detected = true;
                patch_kind = BURNER_GBA_SRAM_PATCH_FLASH1M_REPRO;
            }
        }
    }
    if (head_cache != NULL) {
        free(head_cache);
    }
    if (err != ESP_OK) {
        return err;
    }

    *save_type_out = save_type;
    *save_size_out = save_size;
    *save_detected_out = save_detected;
    *patch_kind_out = patch_detected ? patch_kind : BURNER_GBA_SRAM_PATCH_NONE;
    *patch_detected_out = patch_detected;
    return ESP_OK;
}

esp_err_t burner_probe_gba_rom_analysis(
    uint32_t device_size,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *save_detected_out,
    burner_gba_sram_patch_kind_t *patch_kind_out,
    bool *patch_detected_out)
{
    burner_task_param_t probe_job = {0};
    esp_err_t err;

    if (device_size == 0u) {
        err = burner_probe_cart_capacity_bytes(BURNER_CART_MODE_GBA, &device_size);
        if (err != ESP_OK) {
            return err;
        }
    }

    probe_job.cart_mode = BURNER_CART_MODE_GBA;
    probe_job.addr_begin = 0u;
    probe_job.total_bytes = device_size;

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(&probe_job);
    if (err == ESP_OK) {
        err = burner_probe_gba_rom_analysis_locked(
            device_size,
            save_type_out,
            save_size_out,
            save_detected_out,
            patch_kind_out,
            patch_detected_out);
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();
    return err;
}

esp_err_t burner_probe_gba_save_type_head_locked(
    uint32_t device_size,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *detected_out)
{
    uint8_t *head_cache = NULL;
    uint32_t head_scan_bytes = 0u;
    burner_gba_save_type_t save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t save_size = 0u;
    bool detected = false;
    esp_err_t err = ESP_OK;

    if (save_type_out == NULL || save_size_out == NULL || detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;
    *detected_out = false;

    if (device_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    head_scan_bytes = device_size;
    if (head_scan_bytes > BURNER_GBA_SAVE_HEADER_SCAN_BYTES) {
        head_scan_bytes = BURNER_GBA_SAVE_HEADER_SCAN_BYTES;
    }

    if (head_scan_bytes == 0u) {
        return ESP_OK;
    }

    head_cache = (uint8_t *)heap_caps_malloc(head_scan_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (head_cache != NULL) {
        err = burner_read_gba_rom_to_buffer_locked(
            &(burner_task_param_t){
                .cart_mode = BURNER_CART_MODE_GBA,
                .addr_begin = 0u,
                .total_bytes = device_size,
            },
            head_cache,
            0u,
            head_scan_bytes);
        if (err == ESP_OK) {
            detected = burner_detect_gba_save_type_in_span(head_cache, head_scan_bytes, &save_type, &save_size);
        }
        free(head_cache);
    } else {
        burner_task_param_t head_job = {
            .cart_mode = BURNER_CART_MODE_GBA,
            .addr_begin = 0u,
            .total_bytes = head_scan_bytes,
        };

        detected = burner_detect_gba_save_type_from_rom_locked(&head_job, NULL, 0u, &save_type, &save_size);
    }

    if (err != ESP_OK) {
        return err;
    }

    *save_type_out = save_type;
    *save_size_out = save_size;
    *detected_out = detected;
    return ESP_OK;
}

esp_err_t burner_probe_gba_save_type(
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *detected_out)
{
    burner_gba_sram_patch_kind_t patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
    bool patch_detected = false;
    esp_err_t err;

    if (save_type_out == NULL || save_size_out == NULL || detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;
    *detected_out = false;

    err = burner_probe_gba_rom_analysis(
        0u,
        save_type_out,
        save_size_out,
        detected_out,
        &patch_kind,
        &patch_detected);
    return err;
}

esp_err_t burner_probe_gba_sram_patch(
    burner_gba_sram_patch_kind_t *patch_kind_out,
    bool *detected_out)
{
    burner_gba_save_type_t save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t save_size = 0u;
    bool save_detected = false;
    esp_err_t err;

    if (patch_kind_out == NULL || detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *patch_kind_out = BURNER_GBA_SRAM_PATCH_NONE;
    *detected_out = false;

    err = burner_probe_gba_rom_analysis(
        0u,
        &save_type,
        &save_size,
        &save_detected,
        patch_kind_out,
        detected_out);
    return err;
}

void burner_build_default_dump_name(
    burner_cart_mode_t cart_mode,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool gba_force_multi,
    char *raw_name,
    size_t raw_name_len)
{
    char timestamp_text[32] = {0};
    char title[32] = {0};
    char title_safe[32] = {0};
    const char *ext = burner_rom_dump_ext_for_mode(cart_mode);

    if (raw_name == NULL || raw_name_len < 2u) {
        return;
    }

    raw_name[0] = '\0';
    burner_build_output_timestamp(timestamp_text, sizeof(timestamp_text));
    if (burner_try_probe_cart_title(
            cart_mode,
            addr_begin,
            total_bytes,
            gba_force_multi,
            title,
            sizeof(title)) &&
        burner_sanitize_filename(title, title_safe, sizeof(title_safe))) {
        (void)snprintf(raw_name, raw_name_len, "%s_%s%s", title_safe, timestamp_text, ext);
        return;
    }

    (void)snprintf(raw_name, raw_name_len, "dump_%s%s", timestamp_text, ext);
}


void burner_build_output_timestamp(char *buf, size_t buf_len)
{
    struct tm tm_now = {0};

    if (buf == NULL || buf_len < 2u) {
        return;
    }

    buf[0] = '\0';
    if (burner_get_wallclock_time(NULL, &tm_now) &&
        strftime(buf, buf_len, "%Y%m%d_%H%M%S", &tm_now) > 0u) {
        return;
    }

    (void)snprintf(buf, buf_len, "ts%lu", (unsigned long)esp_log_timestamp());
}

bool burner_build_mbc5_verify_log_rel_path(
    const uint8_t id[4],
    char *rel_path,
    size_t rel_path_len)
{
    char chip_name_safe[48] = {0};
    char timestamp_text[32] = {0};
    const char *chip_name;
    int n;

    if (rel_path == NULL || rel_path_len < 16u) {
        return false;
    }

    chip_name = burner_mbc5_chip_name(id);
    if (!burner_sanitize_filename(chip_name, chip_name_safe, sizeof(chip_name_safe))) {
        (void)snprintf(chip_name_safe, sizeof(chip_name_safe), "unknown");
    }
    burner_build_output_timestamp(timestamp_text, sizeof(timestamp_text));
    if (timestamp_text[0] == '\0') {
        return false;
    }

    n = snprintf(
        rel_path,
        rel_path_len,
        VERIFY_LOG_DIR_REL "/MBC5_%s_%s.log",
        chip_name_safe,
        timestamp_text);
    return n > 0 && n < (int)rel_path_len;
}

FILE *burner_open_mbc5_verify_log(
    const burner_task_param_t *job,
    char *log_rel,
    size_t log_rel_len)
{
    char log_full[TF_PATH_LEN_MAX + 64] = {0};
    FILE *fp = NULL;
    const char *chip_name;

    if (job == NULL || log_rel == NULL || log_rel_len < 16u) {
        return NULL;
    }

    if (burner_mkdirs_rel(VERIFY_LOG_DIR_REL) != ESP_OK) {
        return NULL;
    }
    if (!burner_build_mbc5_verify_log_rel_path(s_cart_ctx.mbc5_id, log_rel, log_rel_len) ||
        !burner_build_full_path(log_rel, log_full, sizeof(log_full))) {
        return NULL;
    }

    fp = fopen(log_full, "wb");
    if (fp == NULL) {
        return NULL;
    }

    chip_name = burner_mbc5_chip_name(s_cart_ctx.mbc5_id);
    (void)fprintf(
        fp,
        "mode=MBC5\nchip=%s\nrom=%s\npath=%s\naddr_begin=0x%08" PRIX32 "\ntotal=%" PRIu32
        "\nid=%02X %02X %02X %02X\n\n",
        chip_name,
        job->rom_name,
        job->rom_path,
        job->addr_begin,
        job->total_bytes,
        s_cart_ctx.mbc5_id[0],
        s_cart_ctx.mbc5_id[1],
        s_cart_ctx.mbc5_id[2],
        s_cart_ctx.mbc5_id[3]);
    return fp;
}

bool burner_build_indexed_file_name(
    const char *preferred_name,
    uint32_t index,
    char *output,
    size_t output_len)
{
    const char *dot;
    size_t stem_len;
    size_t ext_len;
    char index_buf[16] = {0};
    char ext_buf[24] = {0};
    size_t index_len = 0u;
    size_t reserve;

    if (preferred_name == NULL || preferred_name[0] == '\0' || output == NULL || output_len < 8u) {
        return false;
    }

    dot = strrchr(preferred_name, '.');
    if (dot == NULL || dot == preferred_name) {
        ext_len = 0u;
        stem_len = strlen(preferred_name);
    } else {
        ext_len = strlen(dot);
        if (ext_len + 1u > sizeof(ext_buf)) {
            return false;
        }
        memcpy(ext_buf, dot, ext_len + 1u);
        stem_len = (size_t)(dot - preferred_name);
    }

    {
        int n = snprintf(index_buf, sizeof(index_buf), "_%03" PRIu32, index);
        if (n <= 0 || n >= (int)sizeof(index_buf)) {
            return false;
        }
        index_len = (size_t)n;
    }

    reserve = index_len + ext_len + 1u;
    if (output_len <= reserve) {
        return false;
    }
    if (stem_len > output_len - reserve - 1u) {
        stem_len = output_len - reserve - 1u;
    }
    if (stem_len == 0u) {
        return false;
    }

    memcpy(output, preferred_name, stem_len);
    output[stem_len] = '\0';
    (void)snprintf(output + stem_len, output_len - stem_len, "%s%s", index_buf, ext_buf);
    return true;
}

esp_err_t burner_resolve_unique_output_path(
    const char *dir_path,
    const char *preferred_name,
    char *resolved_name,
    size_t resolved_name_len,
    char *resolved_full_path,
    size_t resolved_full_path_len)
{
    struct stat st;
    uint32_t index;

    if (dir_path == NULL || preferred_name == NULL || preferred_name[0] == '\0' || resolved_name == NULL ||
        resolved_name_len < 2u || resolved_full_path == NULL || resolved_full_path_len < 4u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(resolved_full_path, resolved_full_path_len, "%s/%s", dir_path, preferred_name) >=
        (int)resolved_full_path_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(resolved_full_path, &st) != 0) {
        if (snprintf(resolved_name, resolved_name_len, "%s", preferred_name) >= (int)resolved_name_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    for (index = 1u; index < 1000u; ++index) {
        if (!burner_build_indexed_file_name(preferred_name, index, resolved_name, resolved_name_len)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (snprintf(resolved_full_path, resolved_full_path_len, "%s/%s", dir_path, resolved_name) >=
            (int)resolved_full_path_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(resolved_full_path, &st) != 0) {
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

const char *burner_rom_dump_ext_for_mode(burner_cart_mode_t cart_mode)
{
    return (cart_mode == BURNER_CART_MODE_GBA) ? ".gba" : ".gbc";
}

bool burner_force_file_extension(
    const char *input_name,
    const char *target_ext,
    char *output,
    size_t output_len)
{
    const char *dot;
    size_t stem_len;
    size_t ext_len;

    if (input_name == NULL || input_name[0] == '\0' || target_ext == NULL || target_ext[0] != '.' || output == NULL ||
        output_len < 4u) {
        return false;
    }

    dot = strrchr(input_name, '.');
    if (dot == NULL || dot == input_name) {
        stem_len = strlen(input_name);
    } else {
        stem_len = (size_t)(dot - input_name);
    }
    ext_len = strlen(target_ext);

    if (stem_len + ext_len + 1u > output_len) {
        return false;
    }

    memcpy(output, input_name, stem_len);
    memcpy(output + stem_len, target_ext, ext_len + 1u);
    return true;
}

bool burner_hex_to_nibble(char ch, uint8_t *nibble)
{
    if (nibble == NULL) {
        return false;
    }
    if (ch >= '0' && ch <= '9') {
        *nibble = (uint8_t)(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        *nibble = (uint8_t)(10 + (ch - 'a'));
        return true;
    }
    if (ch >= 'A' && ch <= 'F') {
        *nibble = (uint8_t)(10 + (ch - 'A'));
        return true;
    }
    return false;
}

bool burner_url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t si = 0;
    size_t di = 0;

    if (src == NULL || dst == NULL || dst_len < 1) {
        return false;
    }

    while (src[si] != '\0') {
        char ch = src[si];
        if (ch == '%') {
            uint8_t hi;
            uint8_t lo;
            if (src[si + 1] == '\0' || src[si + 2] == '\0') {
                return false;
            }
            if (!burner_hex_to_nibble(src[si + 1], &hi) || !burner_hex_to_nibble(src[si + 2], &lo)) {
                return false;
            }
            if (di + 1 >= dst_len) {
                return false;
            }
            dst[di++] = (char)((hi << 4) | lo);
            si += 3;
            continue;
        }

        if (ch == '+') {
            ch = ' ';
        }

        if (di + 1 >= dst_len) {
            return false;
        }
        dst[di++] = ch;
        si++;
    }

    dst[di] = '\0';
    return true;
}

bool burner_get_query_arg(
    httpd_req_t *req,
    const char *key,
    char *out,
    size_t out_len,
    bool required)
{
    char query[TF_QUERY_LEN_MAX] = {0};
    char raw[TF_QUERY_LEN_MAX] = {0};

    if (req == NULL || key == NULL || out == NULL || out_len < 1) {
        return false;
    }

    out[0] = '\0';

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return !required;
    }

    if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
        return !required;
    }

    return burner_url_decode(raw, out, out_len);
}

esp_err_t burner_send_json(httpd_req_t *req, const char *json_text)
{
    if (req == NULL || json_text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json_text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_read_request_body(
    httpd_req_t *req,
    char *out,
    size_t out_len,
    size_t *actual_len)
{
    int remaining = 0;
    int received = 0;

    if (req == NULL || out == NULL || out_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0 || req->content_len >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    remaining = req->content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, out + received, remaining);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        remaining -= ret;
        received += ret;
    }

    out[received] = '\0';
    if (actual_len != NULL) {
        *actual_len = (size_t)received;
    }
    return ESP_OK;
}

const char *burner_json_locate_value(const char *json, const char *key)
{
    char pattern[64];
    const char *pos = NULL;
    int n = 0;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return NULL;
    }

    pos = strstr(json, pattern);
    if (pos == NULL) {
        return NULL;
    }
    pos += n;

    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ':') {
        return NULL;
    }
    pos++;
    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        pos++;
    }

    return pos;
}

bool burner_json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    const char *pos = burner_json_locate_value(json, key);
    size_t di = 0;

    if (out == NULL || out_len < 1) {
        return false;
    }
    out[0] = '\0';

    if (pos == NULL || *pos != '\"') {
        return false;
    }
    pos++;

    while (*pos != '\0') {
        char ch = *pos++;

        if (ch == '\"') {
            out[di] = '\0';
            return true;
        }

        if (ch == '\\') {
            char esc = *pos++;
            switch (esc) {
                case '\"':
                    ch = '\"';
                    break;
                case '\\':
                    ch = '\\';
                    break;
                case '/':
                    ch = '/';
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'u': {
                    uint8_t nibble = 0;
                    bool valid = true;
                    for (int i = 0; i < 4; i++) {
                        if (pos[i] == '\0') {
                            valid = false;
                            break;
                        }
                        if (!burner_hex_to_nibble(pos[i], &nibble)) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) {
                        return false;
                    }
                    pos += 4;
                    ch = '?';
                    break;
                }
                default:
                    return false;
            }
        }

        if (di + 1 >= out_len) {
            return false;
        }
        out[di++] = ch;
    }

    return false;
}

bool burner_json_get_bool(const char *json, const char *key, bool *out)
{
    const char *pos = burner_json_locate_value(json, key);

    if (pos == NULL || out == NULL) {
        return false;
    }

    if (strncasecmp(pos, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncasecmp(pos, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (*pos == '1') {
        *out = true;
        return true;
    }
    if (*pos == '0') {
        *out = false;
        return true;
    }
    return false;
}

bool burner_json_get_int(const char *json, const char *key, int *out)
{
    const char *pos = burner_json_locate_value(json, key);
    char num_buf[24];
    size_t di = 0;
    long parsed = 0;
    char *end = NULL;

    if (pos == NULL || out == NULL) {
        return false;
    }

    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        pos++;
    }

    if (*pos == '\"') {
        pos++;
        while (*pos != '\0' && *pos != '\"') {
            if (di + 1 >= sizeof(num_buf)) {
                return false;
            }
            num_buf[di++] = *pos++;
        }
        if (*pos != '\"') {
            return false;
        }
    } else {
        while (*pos != '\0' && !isspace((unsigned char)*pos) && *pos != ',' && *pos != '}') {
            if (di + 1 >= sizeof(num_buf)) {
                return false;
            }
            num_buf[di++] = *pos++;
        }
    }

    if (di == 0) {
        return false;
    }
    num_buf[di] = '\0';

    errno = 0;
    parsed = strtol(num_buf, &end, 10);
    if (errno != 0 || end == num_buf || *end != '\0') {
        return false;
    }
    if (parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }

    *out = (int)parsed;
    return true;
}

bool burner_normalize_rel_path(
    const char *input,
    char *output,
    size_t output_len,
    bool allow_empty)
{
    size_t out_idx = 0;
    size_t seg_start = 0;
    size_t i = 0;
    const char *p = NULL;

    if (input == NULL || output == NULL || output_len < 2) {
        return false;
    }

    p = input;
    while (*p == '/') {
        p++;
    }

    if (*p == '\0') {
        if (!allow_empty) {
            return false;
        }
        output[0] = '\0';
        return true;
    }

    for (i = 0;; i++) {
        char ch = p[i];
        if (ch == '/' || ch == '\0') {
            size_t seg_len = i - seg_start;
            if (seg_len == 0 || (seg_len == 1 && p[seg_start] == '.')) {
                seg_start = i + 1;
                if (ch == '\0') {
                    break;
                }
                continue;
            }
            if (seg_len == 2 && p[seg_start] == '.' && p[seg_start + 1] == '.') {
                return false;
            }

            for (size_t j = seg_start; j < i; j++) {
                unsigned char seg_ch = (unsigned char)p[j];
                if (seg_ch < 0x20 || seg_ch == '\\' || seg_ch == ':' || seg_ch == '*' || seg_ch == '?' ||
                    seg_ch == '\"' || seg_ch == '<' || seg_ch == '>' || seg_ch == '|') {
                    return false;
                }
            }

            if (out_idx > 0) {
                if (out_idx + 1 >= output_len) {
                    return false;
                }
                output[out_idx++] = '/';
            }

            if (out_idx + seg_len + 1 >= output_len) {
                return false;
            }

            memcpy(output + out_idx, p + seg_start, seg_len);
            out_idx += seg_len;
            seg_start = i + 1;
            if (ch == '\0') {
                break;
            }
            continue;
        }
    }

    if (out_idx == 0) {
        if (!allow_empty) {
            return false;
        }
        output[0] = '\0';
        return true;
    }

    output[out_idx] = '\0';
    return true;
}

bool burner_build_full_path(const char *rel_path, char *full_path, size_t full_path_len)
{
    int n;

    if (rel_path == NULL || full_path == NULL || full_path_len < 2) {
        return false;
    }

    if (rel_path[0] == '\0') {
        n = snprintf(full_path, full_path_len, "%s", mount_point);
    } else {
        n = snprintf(full_path, full_path_len, "%s/%s", mount_point, rel_path);
    }

    return n > 0 && n < (int)full_path_len;
}

bool burner_json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t di = 0;

    if (src == NULL || dst == NULL || dst_len < 2) {
        return false;
    }

    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)src[i];
        const char *esc = NULL;
        char esc_buf[7] = {0};

        switch (ch) {
            case '\\':
                esc = "\\\\";
                break;
            case '\"':
                esc = "\\\"";
                break;
            case '\n':
                esc = "\\n";
                break;
            case '\r':
                esc = "\\r";
                break;
            case '\t':
                esc = "\\t";
                break;
            default:
                break;
        }

        if (esc != NULL) {
            size_t esc_len = strlen(esc);
            if (di + esc_len + 1 >= dst_len) {
                return false;
            }
            memcpy(dst + di, esc, esc_len);
            di += esc_len;
            continue;
        }

        if (ch < 0x20) {
            int n = snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", ch);
            if (n <= 0 || di + (size_t)n + 1 >= dst_len) {
                return false;
            }
            memcpy(dst + di, esc_buf, (size_t)n);
            di += (size_t)n;
            continue;
        }

        if (di + 2 >= dst_len) {
            return false;
        }
        dst[di++] = (char)ch;
    }

    dst[di] = '\0';
    return true;
}

static char *burner_trim_inplace(char *text)
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

static bool burner_ini_split_line(char *line, char **key, char **value)
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

    trimmed = burner_trim_inplace(line);
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

    *key = burner_trim_inplace(trimmed);
    *value = burner_trim_inplace(eq + 1);
    if (*key == NULL || *value == NULL || (*key)[0] == '\0') {
        return false;
    }

    if (((*value)[0] == '\"' || (*value)[0] == '\'') && strlen(*value) >= 2) {
        char quote = (*value)[0];
        size_t len = strlen(*value);
        if ((*value)[len - 1] == quote) {
            (*value)[len - 1] = '\0';
            (*value)++;
        }
    }

    return true;
}

static bool burner_lang_ini_name_valid(const char *name)
{
    size_t len = 0;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);
    if (len == 0 || len >= WEB_LANG_FILE_NAME_MAX) {
        return false;
    }
    if (name[0] == '.') {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }

    {
        const char *ext = strrchr(name, '.');
        if (ext == NULL || strcasecmp(ext, ".ini") != 0) {
            return false;
        }
    }

    return true;
}

static void burner_lang_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    snprintf(dst, dst_len, "%s", src);
}

static void burner_lang_set_defaults(burner_lang_pack_t *lang)
{
    if (lang == NULL) {
        return;
    }

    burner_lang_copy(lang->page_title, sizeof(lang->page_title), "MORI Base Settings");
    burner_lang_copy(
        lang->page_header,
        sizeof(lang->page_header),
        "MORI Base Settings (Firmware Built-in)");
    burner_lang_copy(
        lang->page_tip,
        sizeof(lang->page_tip),
        "This page does not depend on TF and is always available for debug/configuration.");
    burner_lang_copy(lang->business_title, sizeof(lang->business_title), "Business Web (TF)");
    burner_lang_copy(lang->btn_open_business, sizeof(lang->btn_open_business), "Open Business Web");
    burner_lang_copy(
        lang->business_tip,
        sizeof(lang->business_tip),
        "Business web is loaded from /sdcard/.web/main.html");
    burner_lang_copy(
        lang->recovery_title,
        sizeof(lang->recovery_title),
        "Business Web Recovery");
    burner_lang_copy(
        lang->recovery_tip,
        sizeof(lang->recovery_tip),
        "Upload one or more files to /sdcard/.web/ for recovery (main.html/app.js/styles.css, etc.).");
    burner_lang_copy(lang->btn_upload_main, sizeof(lang->btn_upload_main), "Upload to .web");
    burner_lang_copy(
        lang->system_migrate_title,
        sizeof(lang->system_migrate_title),
        "System Migration ZIP");
    burner_lang_copy(
        lang->system_migrate_tip,
        sizeof(lang->system_migrate_tip),
        "Export /sdcard/.setting + /sdcard/.web in one ZIP for TF card migration and recovery.");
    burner_lang_copy(
        lang->btn_system_migrate,
        sizeof(lang->btn_system_migrate),
        "Download Migration ZIP");
    burner_lang_copy(
        lang->system_deploy_title,
        sizeof(lang->system_deploy_title),
        "System Deploy ZIP");
    burner_lang_copy(
        lang->system_deploy_tip,
        sizeof(lang->system_deploy_tip),
        "Upload a migration ZIP to deploy its contents into /sdcard/.setting and /sdcard/.web.");
    burner_lang_copy(
        lang->btn_system_deploy,
        sizeof(lang->btn_system_deploy),
        "Deploy ZIP");
    burner_lang_copy(lang->firmware_title, sizeof(lang->firmware_title), "Firmware Upgrade");
    burner_lang_copy(
        lang->firmware_tip,
        sizeof(lang->firmware_tip),
        "Upload moriburnner.bin for OTA upgrade. Device will reboot automatically when successful. Do not upload .elf.");
    burner_lang_copy(
        lang->btn_upload_firmware,
        sizeof(lang->btn_upload_firmware),
        "Upload Firmware");
    burner_lang_copy(lang->firmware_idle, sizeof(lang->firmware_idle), "Idle");
    burner_lang_copy(lang->upload_idle, sizeof(lang->upload_idle), "Idle");
    burner_lang_copy(lang->usb_title, sizeof(lang->usb_title), "TF USB Pass-Through");
    burner_lang_copy(
        lang->usb_tip,
        sizeof(lang->usb_tip),
        "Enable: PC can access TF as USB disk. Disable: ESP can access TF/web APIs. When enabled, ESP TF APIs return 503 for safety.");
    burner_lang_copy(
        lang->btn_enable_usb,
        sizeof(lang->btn_enable_usb),
        "Enable USB Pass-Through");
    burner_lang_copy(
        lang->btn_disable_usb,
        sizeof(lang->btn_disable_usb),
        "Disable USB Pass-Through");
    burner_lang_copy(
        lang->btn_refresh_storage,
        sizeof(lang->btn_refresh_storage),
        "Refresh Storage Status");
    burner_lang_copy(lang->storage_loading, sizeof(lang->storage_loading), "Loading...");
    burner_lang_copy(lang->device_title, sizeof(lang->device_title), "Device Info");
    burner_lang_copy(
        lang->btn_refresh_device,
        sizeof(lang->btn_refresh_device),
        "Refresh Device Info");
    burner_lang_copy(lang->device_loading, sizeof(lang->device_loading), "Loading...");
    burner_lang_copy(
        lang->msg_select_main,
        sizeof(lang->msg_select_main),
        "Please select one or more files");
    burner_lang_copy(
        lang->msg_select_deploy_zip,
        sizeof(lang->msg_select_deploy_zip),
        "Please select a ZIP file");
    burner_lang_copy(
        lang->msg_select_firmware,
        sizeof(lang->msg_select_firmware),
        "Please select a .bin firmware file");
    burner_lang_copy(
        lang->msg_deploying_prefix,
        sizeof(lang->msg_deploying_prefix),
        "Deploying");
    burner_lang_copy(
        lang->msg_deploy_success_prefix,
        sizeof(lang->msg_deploy_success_prefix),
        "Deploy success");
    burner_lang_copy(
        lang->msg_deploy_failed_prefix,
        sizeof(lang->msg_deploy_failed_prefix),
        "Deploy failed");
    burner_lang_copy(
        lang->msg_uploading_firmware_prefix,
        sizeof(lang->msg_uploading_firmware_prefix),
        "Uploading firmware");
    burner_lang_copy(
        lang->msg_firmware_success_prefix,
        sizeof(lang->msg_firmware_success_prefix),
        "Firmware upload success, rebooting");
    burner_lang_copy(lang->msg_uploading_prefix, sizeof(lang->msg_uploading_prefix), "Uploading");
    burner_lang_copy(
        lang->msg_upload_success_prefix,
        sizeof(lang->msg_upload_success_prefix),
        "Upload success");
    burner_lang_copy(
        lang->msg_upload_failed_prefix,
        sizeof(lang->msg_upload_failed_prefix),
        "Upload failed");
    burner_lang_copy(
        lang->msg_storage_status_error_prefix,
        sizeof(lang->msg_storage_status_error_prefix),
        "Storage status error: ");
    burner_lang_copy(
        lang->msg_set_mode_error_prefix,
        sizeof(lang->msg_set_mode_error_prefix),
        "Set mode error: ");
    burner_lang_copy(
        lang->msg_device_info_error_prefix,
        sizeof(lang->msg_device_info_error_prefix),
        "Device info error: ");
    burner_lang_copy(lang->msg_applying, sizeof(lang->msg_applying), "Applying...");
    burner_lang_copy(lang->language_title, sizeof(lang->language_title), "Language");
    burner_lang_copy(
        lang->language_tip,
        sizeof(lang->language_tip),
        "Read available language INI files, choose one, then apply.");
    burner_lang_copy(
        lang->btn_read_lang_list,
        sizeof(lang->btn_read_lang_list),
        "Read INI List");
    burner_lang_copy(
        lang->btn_apply_language,
        sizeof(lang->btn_apply_language),
        "Apply Language");
    burner_lang_copy(lang->language_idle, sizeof(lang->language_idle), "Idle");
    burner_lang_copy(lang->language_loading, sizeof(lang->language_loading), "Loading...");
    burner_lang_copy(lang->language_none, sizeof(lang->language_none), "(No language ini)");
    burner_lang_copy(lang->ip5306_title, sizeof(lang->ip5306_title), "IP5306 Config (INI)");
    burner_lang_copy(
        lang->ip5306_tip,
        sizeof(lang->ip5306_tip),
        "Edit /sdcard/.setting/ip5306.ini. Save takes effect on next boot or power init.");
    burner_lang_copy(
        lang->btn_read_ip5306_ini,
        sizeof(lang->btn_read_ip5306_ini),
        "Read IP5306 INI");
    burner_lang_copy(
        lang->btn_save_ip5306_ini,
        sizeof(lang->btn_save_ip5306_ini),
        "Save IP5306 INI");
    burner_lang_copy(lang->ip5306_idle, sizeof(lang->ip5306_idle), "Idle");
    burner_lang_copy(lang->ip5306_loading, sizeof(lang->ip5306_loading), "Loading...");
    burner_lang_copy(
        lang->msg_lang_select_required,
        sizeof(lang->msg_lang_select_required),
        "Please select a language ini");
    burner_lang_copy(
        lang->msg_lang_list_error_prefix,
        sizeof(lang->msg_lang_list_error_prefix),
        "Language list error: ");
    burner_lang_copy(
        lang->msg_lang_apply_success_prefix,
        sizeof(lang->msg_lang_apply_success_prefix),
        "Language applied: ");
    burner_lang_copy(
        lang->msg_lang_apply_error_prefix,
        sizeof(lang->msg_lang_apply_error_prefix),
        "Language apply error: ");
    burner_lang_copy(
        lang->msg_ip5306_load_ok,
        sizeof(lang->msg_ip5306_load_ok),
        "IP5306 INI loaded");
    burner_lang_copy(
        lang->msg_ip5306_load_error_prefix,
        sizeof(lang->msg_ip5306_load_error_prefix),
        "IP5306 INI load error: ");
    burner_lang_copy(
        lang->msg_ip5306_save_ok_prefix,
        sizeof(lang->msg_ip5306_save_ok_prefix),
        "IP5306 INI saved, bytes=");
    burner_lang_copy(
        lang->msg_ip5306_save_error_prefix,
        sizeof(lang->msg_ip5306_save_error_prefix),
        "IP5306 INI save error: ");
    burner_lang_copy(lang->msg_upload_item_ok, sizeof(lang->msg_upload_item_ok), "[OK]");
    burner_lang_copy(lang->msg_upload_item_fail, sizeof(lang->msg_upload_item_fail), "[FAIL]");
    burner_lang_copy(lang->msg_http_error_prefix, sizeof(lang->msg_http_error_prefix), "HTTP error: ");
    burner_lang_copy(
        lang->msg_invalid_json_prefix,
        sizeof(lang->msg_invalid_json_prefix),
        "Invalid JSON: ");
}

static void burner_lang_apply_pair(burner_lang_pack_t *lang, const char *key, const char *value)
{
    if (lang == NULL || key == NULL || value == NULL) {
        return;
    }

#define BURNER_LANG_SET_FIELD(k, f) \
    if (strcmp(key, (k)) == 0) {     \
        burner_lang_copy(lang->f, sizeof(lang->f), value); \
        return;                      \
    }

    BURNER_LANG_SET_FIELD("page_title", page_title);
    BURNER_LANG_SET_FIELD("page_header", page_header);
    BURNER_LANG_SET_FIELD("page_tip", page_tip);
    BURNER_LANG_SET_FIELD("business_title", business_title);
    BURNER_LANG_SET_FIELD("btn_open_business", btn_open_business);
    BURNER_LANG_SET_FIELD("business_tip", business_tip);
    BURNER_LANG_SET_FIELD("recovery_title", recovery_title);
    BURNER_LANG_SET_FIELD("recovery_tip", recovery_tip);
    BURNER_LANG_SET_FIELD("btn_upload_main", btn_upload_main);
    BURNER_LANG_SET_FIELD("system_migrate_title", system_migrate_title);
    BURNER_LANG_SET_FIELD("system_migrate_tip", system_migrate_tip);
    BURNER_LANG_SET_FIELD("btn_system_migrate", btn_system_migrate);
    BURNER_LANG_SET_FIELD("system_deploy_title", system_deploy_title);
    BURNER_LANG_SET_FIELD("system_deploy_tip", system_deploy_tip);
    BURNER_LANG_SET_FIELD("btn_system_deploy", btn_system_deploy);
    BURNER_LANG_SET_FIELD("firmware_title", firmware_title);
    BURNER_LANG_SET_FIELD("firmware_tip", firmware_tip);
    BURNER_LANG_SET_FIELD("btn_upload_firmware", btn_upload_firmware);
    BURNER_LANG_SET_FIELD("firmware_idle", firmware_idle);
    BURNER_LANG_SET_FIELD("upload_idle", upload_idle);
    BURNER_LANG_SET_FIELD("usb_title", usb_title);
    BURNER_LANG_SET_FIELD("usb_tip", usb_tip);
    BURNER_LANG_SET_FIELD("btn_enable_usb", btn_enable_usb);
    BURNER_LANG_SET_FIELD("btn_disable_usb", btn_disable_usb);
    BURNER_LANG_SET_FIELD("btn_refresh_storage", btn_refresh_storage);
    BURNER_LANG_SET_FIELD("storage_loading", storage_loading);
    BURNER_LANG_SET_FIELD("device_title", device_title);
    BURNER_LANG_SET_FIELD("btn_refresh_device", btn_refresh_device);
    BURNER_LANG_SET_FIELD("device_loading", device_loading);
    BURNER_LANG_SET_FIELD("msg_select_main", msg_select_main);
    BURNER_LANG_SET_FIELD("msg_select_deploy_zip", msg_select_deploy_zip);
    BURNER_LANG_SET_FIELD("msg_select_firmware", msg_select_firmware);
    BURNER_LANG_SET_FIELD("msg_deploying_prefix", msg_deploying_prefix);
    BURNER_LANG_SET_FIELD("msg_deploy_success_prefix", msg_deploy_success_prefix);
    BURNER_LANG_SET_FIELD("msg_deploy_failed_prefix", msg_deploy_failed_prefix);
    BURNER_LANG_SET_FIELD("msg_uploading_firmware_prefix", msg_uploading_firmware_prefix);
    BURNER_LANG_SET_FIELD("msg_firmware_success_prefix", msg_firmware_success_prefix);
    BURNER_LANG_SET_FIELD("msg_uploading_prefix", msg_uploading_prefix);
    BURNER_LANG_SET_FIELD("msg_upload_success_prefix", msg_upload_success_prefix);
    BURNER_LANG_SET_FIELD("msg_upload_failed_prefix", msg_upload_failed_prefix);
    BURNER_LANG_SET_FIELD("msg_storage_status_error_prefix", msg_storage_status_error_prefix);
    BURNER_LANG_SET_FIELD("msg_set_mode_error_prefix", msg_set_mode_error_prefix);
    BURNER_LANG_SET_FIELD("msg_device_info_error_prefix", msg_device_info_error_prefix);
    BURNER_LANG_SET_FIELD("msg_applying", msg_applying);
    BURNER_LANG_SET_FIELD("language_title", language_title);
    BURNER_LANG_SET_FIELD("language_tip", language_tip);
    BURNER_LANG_SET_FIELD("btn_read_lang_list", btn_read_lang_list);
    BURNER_LANG_SET_FIELD("btn_apply_language", btn_apply_language);
    BURNER_LANG_SET_FIELD("language_idle", language_idle);
    BURNER_LANG_SET_FIELD("language_loading", language_loading);
    BURNER_LANG_SET_FIELD("language_none", language_none);
    BURNER_LANG_SET_FIELD("ip5306_title", ip5306_title);
    BURNER_LANG_SET_FIELD("ip5306_tip", ip5306_tip);
    BURNER_LANG_SET_FIELD("btn_read_ip5306_ini", btn_read_ip5306_ini);
    BURNER_LANG_SET_FIELD("btn_save_ip5306_ini", btn_save_ip5306_ini);
    BURNER_LANG_SET_FIELD("ip5306_idle", ip5306_idle);
    BURNER_LANG_SET_FIELD("ip5306_loading", ip5306_loading);
    BURNER_LANG_SET_FIELD("msg_lang_select_required", msg_lang_select_required);
    BURNER_LANG_SET_FIELD("msg_lang_list_error_prefix", msg_lang_list_error_prefix);
    BURNER_LANG_SET_FIELD("msg_lang_apply_success_prefix", msg_lang_apply_success_prefix);
    BURNER_LANG_SET_FIELD("msg_lang_apply_error_prefix", msg_lang_apply_error_prefix);
    BURNER_LANG_SET_FIELD("msg_ip5306_load_ok", msg_ip5306_load_ok);
    BURNER_LANG_SET_FIELD("msg_ip5306_load_error_prefix", msg_ip5306_load_error_prefix);
    BURNER_LANG_SET_FIELD("msg_ip5306_save_ok_prefix", msg_ip5306_save_ok_prefix);
    BURNER_LANG_SET_FIELD("msg_ip5306_save_error_prefix", msg_ip5306_save_error_prefix);
    BURNER_LANG_SET_FIELD("load_item_ok", msg_upload_item_ok);
    BURNER_LANG_SET_FIELD("msg_upload_item_ok", msg_upload_item_ok);
    BURNER_LANG_SET_FIELD("msg_upload_item_fail", msg_upload_item_fail);
    BURNER_LANG_SET_FIELD("msg_http_error_prefix", msg_http_error_prefix);
    BURNER_LANG_SET_FIELD("msg_invalid_json_prefix", msg_invalid_json_prefix);

#undef BURNER_LANG_SET_FIELD
}

static esp_err_t burner_lang_parse_system_ini(const char *full_path, burner_lang_meta_t *meta)
{
    FILE *fp = NULL;
    char line[WEB_LANG_LINE_MAX];

    if (full_path == NULL || meta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (burner_ini_split_line(line, &key, &value)) {
            if (strcmp(key, "language_version") == 0) {
                burner_lang_copy(meta->language_version, sizeof(meta->language_version), value);
            } else if (strcmp(key, "language_ini") == 0 && burner_lang_ini_name_valid(value)) {
                burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), value);
            }
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}

static esp_err_t burner_lang_parse_pack_ini(const char *full_path, burner_lang_pack_t *lang)
{
    FILE *fp = NULL;
    char line[WEB_LANG_LINE_MAX];

    if (full_path == NULL || lang == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (burner_ini_split_line(line, &key, &value)) {
            burner_lang_apply_pair(lang, key, value);
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}

static bool burner_lang_build_rel_path(const char *ini_name, char *rel_path, size_t rel_path_len)
{
    int n = 0;

    if (ini_name == NULL || rel_path == NULL || rel_path_len < 2) {
        return false;
    }
    if (!burner_lang_ini_name_valid(ini_name)) {
        return false;
    }

    n = snprintf(rel_path, rel_path_len, WEB_LANG_DIR_REL "/%s", ini_name);
    return n > 0 && n < (int)rel_path_len;
}

static bool burner_lang_try_load_ini(const char *ini_name, burner_lang_pack_t *lang)
{
    char rel_path[TF_PATH_LEN_MAX] = {0};
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};

    if (lang == NULL || ini_name == NULL) {
        return false;
    }
    if (!burner_lang_build_rel_path(ini_name, rel_path, sizeof(rel_path))) {
        return false;
    }
    if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        return false;
    }

    return burner_lang_parse_pack_ini(full_path, lang) == ESP_OK;
}

void burner_lang_load(
    burner_lang_pack_t *lang,
    burner_lang_meta_t *meta,
    bool *system_loaded,
    bool *lang_loaded)
{
    char system_path[WEB_FILE_PATH_LEN_MAX] = {0};
    bool sys_ok = false;
    bool lang_ok = false;

    if (lang == NULL || meta == NULL) {
        return;
    }

    burner_lang_set_defaults(lang);
    burner_lang_copy(meta->language_version, sizeof(meta->language_version), "1");
    burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);

    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        if (system_loaded != NULL) {
            *system_loaded = false;
        }
        if (lang_loaded != NULL) {
            *lang_loaded = false;
        }
        return;
    }

    if (burner_build_full_path(WEB_LANG_SYSTEM_INI_REL, system_path, sizeof(system_path))) {
        sys_ok = (burner_lang_parse_system_ini(system_path, meta) == ESP_OK);
    }

    if (!burner_lang_ini_name_valid(meta->language_ini)) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
    }

    lang_ok = burner_lang_try_load_ini(meta->language_ini, lang);
    if (!lang_ok && strcmp(meta->language_ini, WEB_LANG_DEFAULT_INI) != 0) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
        lang_ok = burner_lang_try_load_ini(meta->language_ini, lang);
    }
    if (!lang_ok && strcmp(meta->language_ini, WEB_LANG_FALLBACK_EN_INI) != 0) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_FALLBACK_EN_INI);
        lang_ok = burner_lang_try_load_ini(meta->language_ini, lang);
    }

    if (system_loaded != NULL) {
        *system_loaded = sys_ok;
    }
    if (lang_loaded != NULL) {
        *lang_loaded = lang_ok;
    }
}

static bool burner_lang_version_valid(const char *version)
{
    if (version == NULL || version[0] == '\0') {
        return false;
    }

    for (size_t i = 0; version[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)version[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }

    return true;
}

static bool burner_ntp_server_name_valid(const char *name)
{
    size_t len = 0;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);
    if (len == 0 || len >= WEB_NTP_SERVER_MAX) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '.' || ch == '-' || ch == '_')) {
            return false;
        }
    }

    return true;
}

static bool burner_bool_text_valid(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }

    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        return true;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        return true;
    }

    return false;
}

static void burner_system_ini_load_ntp(
    const char *system_ini_path,
    char *ntp_enable,
    size_t ntp_enable_len,
    char *ntp_server,
    size_t ntp_server_len)
{
    FILE *fp = NULL;
    char line[WEB_LANG_LINE_MAX];

    if (ntp_enable == NULL || ntp_enable_len < 2 || ntp_server == NULL || ntp_server_len < 2) {
        return;
    }

    snprintf(ntp_enable, ntp_enable_len, "1");
    snprintf(ntp_server, ntp_server_len, "%s", WEB_NTP_SERVER_DEFAULT);

    if (system_ini_path == NULL) {
        return;
    }

    fp = fopen(system_ini_path, "rb");
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

        if (burner_ini_split_line(line, &key, &value)) {
            if (strcmp(key, "ntp_enable") == 0 && burner_bool_text_valid(value)) {
                snprintf(ntp_enable, ntp_enable_len, "%s", value);
            } else if (strcmp(key, "ntp_server") == 0 && burner_ntp_server_name_valid(value)) {
                snprintf(ntp_server, ntp_server_len, "%s", value);
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

static bool burner_lang_is_reserved_ini(const char *name)
{
    if (name == NULL) {
        return true;
    }

    if (strcasecmp(name, "mori_system.ini") == 0) {
        return true;
    }
    if (strcasecmp(name, "ip5306.ini") == 0) {
        return true;
    }
    if (strcasecmp(name, "tca9555.ini") == 0) {
        return true;
    }

    return false;
}

static bool burner_lang_is_selectable_ini(const char *name)
{
    if (!burner_lang_ini_name_valid(name)) {
        return false;
    }
    return !burner_lang_is_reserved_ini(name);
}

static void burner_lang_load_meta_defaults(burner_lang_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }

    burner_lang_copy(meta->language_version, sizeof(meta->language_version), "1");
    burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
}

static void burner_lang_load_meta_from_system(burner_lang_meta_t *meta)
{
    char system_path[WEB_FILE_PATH_LEN_MAX] = {0};

    if (meta == NULL) {
        return;
    }

    burner_lang_load_meta_defaults(meta);
    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        return;
    }

    if (burner_build_full_path(WEB_LANG_SYSTEM_INI_REL, system_path, sizeof(system_path))) {
        (void)burner_lang_parse_system_ini(system_path, meta);
    }

    if (!burner_lang_version_valid(meta->language_version)) {
        burner_lang_copy(meta->language_version, sizeof(meta->language_version), "1");
    }
    if (!burner_lang_is_selectable_ini(meta->language_ini)) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
    }
}

static uint8_t burner_lang_ui_language_for_ini(const char *language_ini)
{
    if (language_ini != NULL && strcmp(language_ini, WEB_LANG_FALLBACK_EN_INI) == 0) {
        return UI_LANGUAGE_EN;
    }
    if (language_ini != NULL && strcmp(language_ini, WEB_LANG_DEFAULT_INI) == 0) {
        return UI_LANGUAGE_ZH;
    }
    return ui_get_language();
}

static esp_err_t burner_lang_write_system_ini(
    const char *language_ini,
    burner_lang_meta_t *saved_meta)
{
    burner_lang_meta_t meta = {0};
    uint8_t ui_language = UI_LANGUAGE_DEFAULT;
    char ntp_enable[WEB_NTP_ENABLE_MAX] = {0};
    char ntp_server[WEB_NTP_SERVER_MAX] = {0};
    char setting_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char system_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char tmp_path[WEB_FILE_PATH_LEN_MAX] = {0};
    FILE *fp = NULL;
    bool write_ok = false;
    struct stat st = {0};

    if (!burner_lang_is_selectable_ini(language_ini)) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_lang_load_meta_from_system(&meta);
    burner_lang_copy(meta.language_ini, sizeof(meta.language_ini), language_ini);
    ui_language = burner_lang_ui_language_for_ini(meta.language_ini);

    if (!burner_build_full_path(WEB_LANG_DIR_REL, setting_path, sizeof(setting_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mkdir(setting_path, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (stat(setting_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ESP_FAIL;
    }

    if (!burner_build_full_path(WEB_LANG_SYSTEM_INI_REL, system_path, sizeof(system_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", system_path) >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    burner_system_ini_load_ntp(system_path, ntp_enable, sizeof(ntp_enable), ntp_server, sizeof(ntp_server));

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    if (fprintf(
            fp,
            "# MORI system settings\nlanguage_version=%s\nlanguage_ini=%s\nui_language=%u\nntp_enable=%s\nntp_server=%s\n",
            meta.language_version,
            meta.language_ini,
            (unsigned)ui_language,
            ntp_enable,
            ntp_server) > 0 &&
        fflush(fp) == 0) {
        write_ok = true;
    }

    if (fclose(fp) != 0) {
        write_ok = false;
    }
    fp = NULL;

    if (!write_ok) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    if (unlink(system_path) != 0 && errno != ENOENT) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    if (rename(tmp_path, system_path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    if (saved_meta != NULL) {
        *saved_meta = meta;
    }
    ui_set_language(ui_language);
    return ESP_OK;
}

esp_err_t burner_lang_list_handler(httpd_req_t *req)
{
    char setting_path[WEB_FILE_PATH_LEN_MAX] = {0};
    burner_lang_meta_t meta = {0};
    char esc_current[WEB_LANG_FILE_NAME_MAX * 2 + 16] = {0};
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    bool first = true;
    esp_err_t send_err = ESP_OK;
    char head[256] = {0};
    int n = 0;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    burner_lang_load_meta_from_system(&meta);
    if (!burner_json_escape(meta.language_ini, esc_current, sizeof(esc_current))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    if (!burner_build_full_path(WEB_LANG_DIR_REL, setting_path, sizeof(setting_path))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "path build failed");
    }

    dir = opendir(setting_path);
    if (dir == NULL) {
        char resp[256] = {0};
        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":false,\"current\":\"%s\",\"files\":[],\"message\":\"open .setting failed\"}",
            esc_current);
        if (n <= 0 || n >= (int)sizeof(resp)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
        }
        return burner_send_json(req, resp);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    n = snprintf(head, sizeof(head), "{\"ok\":true,\"current\":\"%s\",\"files\":[", esc_current);
    if (n <= 0 || n >= (int)sizeof(head)) {
        closedir(dir);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    send_err = httpd_resp_send_chunk(req, head, n);
    while (send_err == ESP_OK && (entry = readdir(dir)) != NULL) {
        char esc_name[WEB_LANG_FILE_NAME_MAX * 2 + 16] = {0};
        char line[WEB_LANG_FILE_NAME_MAX * 2 + 24] = {0};

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!burner_lang_is_selectable_ini(entry->d_name)) {
            continue;
        }
        if (!burner_json_escape(entry->d_name, esc_name, sizeof(esc_name))) {
            continue;
        }
        n = snprintf(line, sizeof(line), "%s\"%s\"", first ? "" : ",", esc_name);
        if (n <= 0 || n >= (int)sizeof(line)) {
            continue;
        }
        send_err = httpd_resp_send_chunk(req, line, n);
        if (send_err == ESP_OK) {
            first = false;
        }
    }

    closedir(dir);
    if (send_err == ESP_OK) {
        send_err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (send_err == ESP_OK) {
        send_err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return send_err;
}

esp_err_t burner_lang_apply_handler(httpd_req_t *req)
{
    char ini_name[WEB_LANG_FILE_NAME_MAX] = {0};
    char body[WIFI_JSON_BODY_MAX] = {0};
    burner_lang_pack_t *probe = NULL;
    burner_lang_meta_t saved_meta = {0};
    char esc_version[WEB_LANG_VERSION_MAX * 2 + 16] = {0};
    char esc_ini[WEB_LANG_FILE_NAME_MAX * 2 + 16] = {0};
    char resp[256] = {0};
    esp_err_t err = ESP_OK;
    int n = 0;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_get_query_arg(req, "ini", ini_name, sizeof(ini_name), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ini query");
    }

    if (ini_name[0] == '\0' && req->content_len > 0) {
        err = burner_read_request_body(req, body, sizeof(body), NULL);
        if (err == ESP_OK) {
            (void)burner_json_get_string(body, "ini", ini_name, sizeof(ini_name));
        }
    }

    if (ini_name[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ini");
    }
    if (!burner_lang_is_selectable_ini(ini_name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid language ini");
    }

    probe = (burner_lang_pack_t *)calloc(1, sizeof(*probe));
    if (probe == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    if (!burner_lang_try_load_ini(ini_name, probe)) {
        free(probe);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "language ini not found");
    }
    free(probe);

    err = burner_lang_write_system_ini(ini_name, &saved_meta);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write system ini failed");
    }

    if (!burner_json_escape(saved_meta.language_version, esc_version, sizeof(esc_version)) ||
        !burner_json_escape(saved_meta.language_ini, esc_ini, sizeof(esc_ini))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"language_version\":\"%s\",\"language_ini\":\"%s\"}",
        esc_version,
        esc_ini);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_send_lang_string_chunk(
    httpd_req_t *req,
    const char *key,
    const char *value,
    bool trailing_comma)
{
    char esc_value[WEB_LANG_TEXT_MAX * 2 + 32] = {0};
    char line[WEB_LANG_TEXT_MAX * 2 + 96] = {0};
    int n = 0;

    if (req == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value == NULL) {
        value = "";
    }
    if (!burner_json_escape(value, esc_value, sizeof(esc_value))) {
        return ESP_ERR_INVALID_SIZE;
    }

    n = snprintf(line, sizeof(line), "\"%s\":\"%s\"%s", key, esc_value, trailing_comma ? "," : "");
    if (n <= 0 || n >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return httpd_resp_send_chunk(req, line, n);
}

const char *burner_basename(const char *path)
{
    const char *slash = NULL;

    if (path == NULL || path[0] == '\0') {
        return "";
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return path;
    }

    return slash + 1;
}

esp_err_t burner_remove_recursive(const char *full_path)
{
    struct stat st;

    if (full_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(full_path, &st) != 0) {
        return ESP_FAIL;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(full_path);
        struct dirent *entry;

        if (dir == NULL) {
            return ESP_FAIL;
        }

        while ((entry = readdir(dir)) != NULL) {
            char child_path[TF_PATH_LEN_MAX + 64];
            int n;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            n = snprintf(child_path, sizeof(child_path), "%s/%s", full_path, entry->d_name);
            if (n <= 0 || n >= (int)sizeof(child_path)) {
                closedir(dir);
                return ESP_ERR_INVALID_SIZE;
            }

            if (burner_remove_recursive(child_path) != ESP_OK) {
                closedir(dir);
                return ESP_FAIL;
            }
        }

        closedir(dir);
        if (rmdir(full_path) != 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (unlink(full_path) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t burner_mkdirs_rel(const char *rel_path)
{
    char rel_copy[TF_PATH_LEN_MAX] = {0};
    char current_rel[TF_PATH_LEN_MAX] = {0};
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    char *ctx = NULL;
    char *token = NULL;

    if (rel_path == NULL || rel_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(rel_path) >= sizeof(rel_copy)) {
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(rel_copy, sizeof(rel_copy), "%s", rel_path);
    token = strtok_r(rel_copy, "/", &ctx);
    while (token != NULL) {
        char next_rel[TF_PATH_LEN_MAX] = {0};
        struct stat st;

        if (current_rel[0] == '\0') {
            if (snprintf(next_rel, sizeof(next_rel), "%s", token) >= (int)sizeof(next_rel)) {
                return ESP_ERR_INVALID_SIZE;
            }
        } else {
            if (snprintf(next_rel, sizeof(next_rel), "%s/%s", current_rel, token) >= (int)sizeof(next_rel)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        snprintf(current_rel, sizeof(current_rel), "%s", next_rel);

        if (!burner_build_full_path(current_rel, full_path, sizeof(full_path))) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (mkdir(full_path, 0775) != 0) {
            if (errno != EEXIST) {
                return ESP_FAIL;
            }
            if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                return ESP_FAIL;
            }
        }

        token = strtok_r(NULL, "/", &ctx);
    }

    return ESP_OK;
}

esp_err_t burner_ensure_rom_dir(void)
{
    struct stat st;
    if (stat(ROM_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(ROM_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t burner_spi_init(void)
{
#if BURNER_SPI_ENABLE
    esp_err_t err;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = MORI_PIN_MCU_SPI_MOSI,
        .miso_io_num = MORI_PIN_MCU_SPI_MISO,
        .sclk_io_num = MORI_PIN_MCU_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = BURNER_SPI_MAX_XFER,
        .flags = 0,
        .intr_flags = 0,
    };
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0, /* Host CH347 is configured as SPI mode0 */
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = (int)s_mcu_spi_clock_hz,
        .input_delay_ns = 0,
        .spics_io_num = -1, /* manual dual-CS control for bacon protocol */
        .flags = 0,
        .queue_size = 1,
        .pre_cb = NULL,
        .post_cb = NULL,
    };

    if (s_mcu_spi_ready && s_mcu_spi != NULL) {
        burner_bacon_mark_activity_locked();
        return ESP_OK;
    }

    ESP_LOGI(
        BURNER_TAG,
        "MCU SPI init: host=%d cs0=%d cs1=%d clk=%d miso=%d mosi=%d freq=%" PRIu32 "Hz",
        (int)BURNER_SPI_HOST,
        MORI_PIN_MCU_SPI_CS,
        MORI_PIN_MCU_SPI_CS1,
        MORI_PIN_MCU_SPI_CLK,
        MORI_PIN_MCU_SPI_MISO,
        MORI_PIN_MCU_SPI_MOSI,
        s_mcu_spi_clock_hz);

    {
        gpio_config_t cs_cfg = {
            .pin_bit_mask = (1ULL << MORI_PIN_MCU_SPI_CS) | (1ULL << MORI_PIN_MCU_SPI_CS1),
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&cs_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(BURNER_TAG, "gpio_config(cs) failed: %s", esp_err_to_name(err));
            return err;
        }
        err = gpio_set_drive_capability(MORI_PIN_MCU_SPI_CS1, GPIO_DRIVE_CAP_3);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "gpio_set_drive_capability(cs1) failed: %s", esp_err_to_name(err));
        }
        err = gpio_pullup_en(MORI_PIN_MCU_SPI_CS1);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "gpio_pullup_en(cs1) failed: %s", esp_err_to_name(err));
        }
        burner_spi_release_cs();
        ESP_LOGI(
            BURNER_TAG,
            "MCU SPI CS idle readback: cs0=%d cs1=%d",
            gpio_get_level(MORI_PIN_MCU_SPI_CS),
            gpio_get_level(MORI_PIN_MCU_SPI_CS1));
    }

    err = spi_bus_initialize(BURNER_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    err = spi_bus_add_device(BURNER_SPI_HOST, &dev_cfg, &s_mcu_spi);
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        (void)spi_bus_free(BURNER_SPI_HOST);
        return err;
    }

    s_cart_ctx.prepared = false;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_cart_ctx.buffer_write_bytes = 0;
    s_cart_ctx.program_buffer_write_bytes = 0;
    s_cart_ctx.sector_size = 0;
    s_cart_ctx.device_size = 0;
    burner_nor_geometry_clear(&s_cart_ctx.geometry);
    memset(s_cart_ctx.mbc5_id, 0, sizeof(s_cart_ctx.mbc5_id));
    s_gb_mapper_kind = BURNER_GB_MAPPER_UNKNOWN;
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_cart_ctx.gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD;
    s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    s_mcu_spi_ready = true;
    if (s_mcu_spi != NULL) {
        int actual_khz = 0;
        if (spi_device_get_actual_freq(s_mcu_spi, &actual_khz) == ESP_OK && actual_khz > 0) {
            s_mcu_spi_actual_hz = (uint32_t)actual_khz * 1000u;
        } else {
            s_mcu_spi_actual_hz = s_mcu_spi_clock_hz;
        }
    } else {
        s_mcu_spi_actual_hz = s_mcu_spi_clock_hz;
    }
    ESP_LOGI(
        BURNER_TAG,
        "MCU SPI ready: actual=%" PRIu32 "Hz dma_scratch_tx=%u dma_scratch_rw=%u",
        s_mcu_spi_actual_hz,
        (unsigned)s_mcu_spi_tx_shadow_size,
        (unsigned)s_mcu_spi_rw_shadow_size);
    burner_bacon_mark_activity_locked();
    return ESP_OK;
#else
    static bool warned = false;
    if (!warned) {
        ESP_LOGW(BURNER_TAG, "SPI skeleton running in mock mode (BURNER_SPI_ENABLE=0)");
        ESP_LOGI(
            BURNER_TAG,
            "SPI2 pins reserved: CS0=%d CS1=%d CLK=%d MISO=%d MOSI=%d",
            MORI_PIN_MCU_SPI_CS,
            MORI_PIN_MCU_SPI_CS1,
            MORI_PIN_MCU_SPI_CLK,
            MORI_PIN_MCU_SPI_MISO,
            MORI_PIN_MCU_SPI_MOSI);
        warned = true;
    }
    return ESP_OK;
#endif
}

static uint8_t burner_bacon_option_byte0(
    uint8_t batch_size,
    bool dir_a,
    bool dir_ad,
    bool cs2,
    bool cs1,
    bool rd,
    bool wr)
{
    uint8_t ret = (uint8_t)((batch_size & 0x03u) << 6);

    if (dir_a) {
        ret |= 0x20u;
    }
    if (dir_ad) {
        ret |= 0x10u;
    }
    if (cs2) {
        ret |= 0x08u;
    }
    if (cs1) {
        ret |= 0x04u;
    }
    if (rd) {
        ret |= 0x02u;
    }
    if (wr) {
        ret |= 0x01u;
    }

    return ret;
}

void burner_spi_lock_take(void)
{
    if (s_spi_lock != NULL) {
        xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    }
}

void burner_spi_lock_give(void)
{
    if (s_spi_lock != NULL) {
        xSemaphoreGive(s_spi_lock);
    }
}

void burner_bacon_mark_activity_locked(void)
{
    s_bacon_last_active_tick = xTaskGetTickCount();
    s_bacon_idle_powered_down = false;
}

bool burner_task_is_running_snapshot(void)
{
    bool is_busy = false;

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        is_busy = (s_burn_task != NULL);
        xSemaphoreGive(s_status_lock);
    } else {
        is_busy = (s_burn_task != NULL);
    }
    return is_busy;
}

esp_err_t burner_backend_init(void)
{
    if (s_status_lock == NULL) {
        s_status_lock = xSemaphoreCreateMutex();
        if (s_status_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_spi_lock == NULL) {
        s_spi_lock = xSemaphoreCreateMutex();
        if (s_spi_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_bacon_last_active_tick = xTaskGetTickCount();
    s_bacon_idle_powered_down = false;

    if (s_bacon_idle_task == NULL) {
        if (xTaskCreatePinnedToCore(
                burner_bacon_idle_task_entry,
                "bacon_idle",
                3072,
                NULL,
                2,
                &s_bacon_idle_task,
                BURNER_TASK_CORE_ID) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t burner_spi_config_get_handler(httpd_req_t *req)
{
    char resp[240];
    int n;

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"configured_hz\":%" PRIu32 ",\"actual_hz\":%" PRIu32
        ",\"min_hz\":%u,\"max_hz\":%u,\"fixed\":true}",
        s_mcu_spi_clock_hz,
        s_mcu_spi_actual_hz,
        (unsigned)BURNER_SPI_CLOCK_HZ,
        (unsigned)BURNER_SPI_CLOCK_HZ);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_core_config_get_handler(httpd_req_t *req)
{
    char resp[320];
    int n;

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"erase\":\"%s\",\"tf\":\"%s\",\"psram\":\"%s\","
        "\"options\":[\"auto\",\"cpu0\",\"cpu1\"]}",
        burner_core_affinity_to_str(s_burn_core_cfg.erase_core),
        burner_core_affinity_to_str(s_burn_core_cfg.tf_core),
        burner_core_affinity_to_str(s_burn_core_cfg.psram_core));
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_core_config_post_handler(httpd_req_t *req)
{
    char erase_arg[16] = {0};
    char tf_arg[16] = {0};
    char psram_arg[16] = {0};
    burner_core_affinity_t erase_val = s_burn_core_cfg.erase_core;
    burner_core_affinity_t tf_val = s_burn_core_cfg.tf_core;
    burner_core_affinity_t psram_val = s_burn_core_cfg.psram_core;
    bool update_erase = false;
    bool update_tf = false;
    bool update_psram = false;
    char resp[320];
    int n;

    if (!burner_get_query_arg(req, "erase", erase_arg, sizeof(erase_arg), false) ||
        !burner_get_query_arg(req, "tf", tf_arg, sizeof(tf_arg), false) ||
        !burner_get_query_arg(req, "psram", psram_arg, sizeof(psram_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query");
    }
    if (erase_arg[0] != '\0') {
        if (!burner_parse_core_affinity_text(erase_arg, &erase_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "erase must be auto/cpu0/cpu1");
        }
        update_erase = true;
    }
    if (tf_arg[0] != '\0') {
        if (!burner_parse_core_affinity_text(tf_arg, &tf_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tf must be auto/cpu0/cpu1");
        }
        update_tf = true;
    }
    if (psram_arg[0] != '\0') {
        if (!burner_parse_core_affinity_text(psram_arg, &psram_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "psram must be auto/cpu0/cpu1");
        }
        update_psram = true;
    }
    if (!update_erase && !update_tf && !update_psram) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "erase/tf/psram query is required");
    }

    if (burner_task_is_running_snapshot()) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "burn task is running");
    }

    if (update_erase) {
        s_burn_core_cfg.erase_core = erase_val;
    }
    if (update_tf) {
        s_burn_core_cfg.tf_core = tf_val;
    }
    if (update_psram) {
        s_burn_core_cfg.psram_core = psram_val;
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"erase\":\"%s\",\"tf\":\"%s\",\"psram\":\"%s\","
        "\"options\":[\"auto\",\"cpu0\",\"cpu1\"]}",
        burner_core_affinity_to_str(s_burn_core_cfg.erase_core),
        burner_core_affinity_to_str(s_burn_core_cfg.tf_core),
        burner_core_affinity_to_str(s_burn_core_cfg.psram_core));
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

void burner_bacon_idle_task_entry(void *param)
{
    const TickType_t interval_ticks = pdMS_TO_TICKS(BURNER_IDLE_MONITOR_INTERVAL_MS);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(BURNER_IDLE_POWER_TIMEOUT_MS);

    (void)param;

    for (;;) {
        TickType_t now_tick;
        TickType_t last_tick;
        esp_err_t err;
        bool should_sleep = false;

        vTaskDelay((interval_ticks == 0) ? 1 : interval_ticks);

        if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
            continue;
        }
        if (burner_task_is_running_snapshot()) {
            continue;
        }

        now_tick = xTaskGetTickCount();
        last_tick = s_bacon_last_active_tick;
        if ((TickType_t)(now_tick - last_tick) < timeout_ticks) {
            continue;
        }
        if (s_bacon_idle_powered_down) {
            continue;
        }

        if (s_spi_lock == NULL) {
            continue;
        }
        if (xSemaphoreTake(s_spi_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }

        do {
            now_tick = xTaskGetTickCount();
            last_tick = s_bacon_last_active_tick;
            if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
                break;
            }
            if (s_bacon_idle_powered_down) {
                break;
            }
            if (s_burn_task != NULL) {
                break;
            }
            if ((TickType_t)(now_tick - last_tick) < timeout_ticks) {
                break;
            }
            should_sleep = true;
        } while (0);

        if (should_sleep) {
            err = burner_bacon_gba_power_cmd(false, false);
            if (err == ESP_OK) {
                s_bacon_idle_powered_down = true;
                s_cart_ctx.prepared = false;
                s_cart_ctx.current_bank = UINT16_MAX;
                s_cart_ctx.buffer_write_bytes = 0;
                s_cart_ctx.program_buffer_write_bytes = 0;
                s_cart_ctx.sector_size = 0;
                s_cart_ctx.device_size = 0;
                burner_nor_geometry_clear(&s_cart_ctx.geometry);
                memset(s_cart_ctx.mbc5_id, 0, sizeof(s_cart_ctx.mbc5_id));
                s_gb_mapper_kind = BURNER_GB_MAPPER_UNKNOWN;
                s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
                s_cart_ctx.gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD;
                s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
                ESP_LOGI(BURNER_TAG, "bacon idle timeout reached, cartridge power rails off");
            } else {
                /* Avoid retry storms when transport is unstable. */
                burner_bacon_mark_activity_locked();
                ESP_LOGW(BURNER_TAG, "idle power off failed: %s", esp_err_to_name(err));
            }
        }

        xSemaphoreGive(s_spi_lock);
    }
}

static void burner_spi_apply_cs_mode(burner_spi_cs_mode_t mode)
{
#if BURNER_SPI_ENABLE
    switch (mode) {
    case BURNER_SPI_CS_MODE_0:
        gpio_set_level(MORI_PIN_MCU_SPI_CS, 0);
        gpio_set_level(MORI_PIN_MCU_SPI_CS1, 1);
        break;
    case BURNER_SPI_CS_MODE_1:
        gpio_set_level(MORI_PIN_MCU_SPI_CS, 1);
        gpio_set_level(MORI_PIN_MCU_SPI_CS1, 0);
        break;
    case BURNER_SPI_CS_MODE_2:
    default:
        gpio_set_level(MORI_PIN_MCU_SPI_CS, 0);
        gpio_set_level(MORI_PIN_MCU_SPI_CS1, 0);
        break;
    }
#else
    (void)mode;
#endif
}

void burner_spi_release_cs(void)
{
#if BURNER_SPI_ENABLE
    gpio_set_level(MORI_PIN_MCU_SPI_CS, 1);
    gpio_set_level(MORI_PIN_MCU_SPI_CS1, 1);
#endif
}

static uint32_t burner_spi_cs_setup_delay_us(burner_spi_cs_mode_t mode)
{
    switch (mode) {
    case BURNER_SPI_CS_MODE_0:
    case BURNER_SPI_CS_MODE_1:
        return BURNER_GBA_SPI_CS_SETUP_US;
    case BURNER_SPI_CS_MODE_2:
    default:
        return 0u;
    }
}

static uint32_t burner_spi_cs_hold_delay_us(burner_spi_cs_mode_t mode)
{
    switch (mode) {
    case BURNER_SPI_CS_MODE_0:
    case BURNER_SPI_CS_MODE_1:
        return BURNER_GBA_SPI_CS_HOLD_US;
    case BURNER_SPI_CS_MODE_2:
    default:
        return 0u;
    }
}

static esp_err_t burner_spi_begin_cs(burner_spi_cs_mode_t mode)
{
#if BURNER_SPI_ENABLE
    uint32_t cs_setup_delay_us;

    if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    burner_bacon_mark_activity_locked();
    burner_spi_apply_cs_mode(mode);
    cs_setup_delay_us = burner_spi_cs_setup_delay_us(mode);
    if (cs_setup_delay_us > 0u) {
        esp_rom_delay_us(cs_setup_delay_us);
    }
    return ESP_OK;
#else
    (void)mode;
    return ESP_OK;
#endif
}

static void burner_spi_end_cs(burner_spi_cs_mode_t mode)
{
#if BURNER_SPI_ENABLE
    uint32_t cs_hold_delay_us = burner_spi_cs_hold_delay_us(mode);

    if (cs_hold_delay_us > 0u) {
        esp_rom_delay_us(cs_hold_delay_us);
    }
    burner_spi_release_cs();
#else
    (void)mode;
#endif
}

static esp_err_t burner_spi_transfer_cs(
    burner_spi_cs_mode_t mode,
    const uint8_t *tx,
    uint8_t *rx,
    size_t len)
{
#if BURNER_SPI_ENABLE
    size_t offset;
    size_t chunk_limit;
    esp_err_t err;

    if (tx == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    chunk_limit = BURNER_SPI_STREAM_CHUNK_BYTES;
    if (chunk_limit == 0u || chunk_limit > BURNER_SPI_MAX_XFER) {
        chunk_limit = BURNER_SPI_MAX_XFER;
    }

    err = burner_spi_begin_cs(mode);
    if (err != ESP_OK) {
        return err;
    }
    for (offset = 0u; offset < len; offset += chunk_limit) {
        size_t chunk_len = len - offset;

        if (chunk_len > chunk_limit) {
            chunk_len = chunk_limit;
        }

        err = burner_spi_transfer_active(tx + offset, (rx != NULL) ? (rx + offset) : NULL, chunk_len);
        if (err != ESP_OK) {
            break;
        }
    }

    burner_spi_end_cs(mode);
    return err;
#else
    (void)mode;
    (void)tx;
    (void)rx;
    (void)len;
    return ESP_OK;
#endif
}

static esp_err_t burner_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    return burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, tx, rx, len);
}

/*
 * GBA command path regression guard:
 * use a direct single-transaction transfer that matches the old stable path.
 * Keep this scoped to GBA command read/write sequences only.
 */
static esp_err_t burner_spi_transfer_cs_legacy(
    burner_spi_cs_mode_t mode,
    const uint8_t *tx,
    uint8_t *rx,
    size_t len)
{
#if BURNER_SPI_ENABLE
    esp_err_t err = ESP_OK;
    spi_transaction_t trans = {0};
    const uint8_t *tx_buf = tx;
    uint8_t *rx_buf = rx;
    bool use_tx_shadow = false;
    bool use_rx_shadow = false;

    if (tx == NULL || len == 0u || len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mcu_spi_tx_shadow != NULL && len <= s_mcu_spi_tx_shadow_size) {
        memcpy(s_mcu_spi_tx_shadow, tx, len);
        tx_buf = s_mcu_spi_tx_shadow;
        use_tx_shadow = true;
    }
    if (rx != NULL && s_mcu_spi_rw_shadow != NULL && len <= s_mcu_spi_rw_shadow_size) {
        rx_buf = s_mcu_spi_rw_shadow;
        use_rx_shadow = true;
    }

    trans.length = (uint32_t)(len * 8u);
    trans.tx_buffer = tx_buf;
    trans.rx_buffer = rx_buf;

    burner_bacon_mark_activity_locked();
    burner_spi_apply_cs_mode(mode);
    err = spi_device_polling_transmit(s_mcu_spi, &trans);
    burner_spi_release_cs();
    if (err == ESP_OK && use_rx_shadow) {
        memcpy(rx, s_mcu_spi_rw_shadow, len);
    }
    (void)use_tx_shadow;
    return err;
#else
    (void)mode;
    (void)tx;
    (void)rx;
    (void)len;
    return ESP_OK;
#endif
}

static esp_err_t burner_spi_write_read(uint8_t *io_buf, size_t len)
{
    esp_err_t err = ESP_OK;
    uint8_t *tx_buf = s_mcu_spi_tx_shadow;
    bool use_shadow = (tx_buf != NULL && len <= s_mcu_spi_tx_shadow_size);

    if (io_buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!use_shadow) {
        tx_buf = (uint8_t *)malloc(len);
        if (tx_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memcpy(tx_buf, io_buf, len);

    err = burner_spi_transfer(tx_buf, io_buf, len);
    if (!use_shadow) {
        free(tx_buf);
    }
    return err;
}

esp_err_t burner_spi_transfer_active(const uint8_t *tx, uint8_t *rx, size_t len)
{
#if BURNER_SPI_ENABLE
    esp_err_t err;
    spi_transaction_t trans = {0};
    uint8_t *tx_buf = NULL;
    uint8_t *rx_buf = NULL;
    bool free_tx_buf = false;
    bool free_rx_buf = false;

    if (tx == NULL || len == 0u || len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    trans.length = (uint32_t)(len * 8u);
    if (len <= sizeof(trans.tx_data)) {
        trans.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(trans.tx_data, tx, len);
        if (rx != NULL) {
            trans.flags |= SPI_TRANS_USE_RXDATA;
        }
    } else {
        tx_buf = burner_spi_alloc_tx_buffer(len, &free_tx_buf);
        if (tx_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(tx_buf, tx, len);
        trans.tx_buffer = tx_buf;
        if (rx != NULL) {
            rx_buf = burner_spi_alloc_rw_buffer(len, &free_rx_buf);
            if (rx_buf == NULL) {
                if (free_tx_buf) {
                    free(tx_buf);
                }
                return ESP_ERR_NO_MEM;
            }
            trans.rx_buffer = rx_buf;
        }
    }

    burner_bacon_mark_activity_locked();
    err = spi_device_polling_transmit(s_mcu_spi, &trans);
    if (err == ESP_OK && rx != NULL) {
        if (trans.flags & SPI_TRANS_USE_RXDATA) {
            memcpy(rx, trans.rx_data, len);
        } else if (rx_buf != NULL) {
            memcpy(rx, rx_buf, len);
        }
    }
    if (free_tx_buf) {
        free(tx_buf);
    }
    if (free_rx_buf) {
        free(rx_buf);
    }
    return err;
#else
    (void)tx;
    (void)rx;
    (void)len;
    return ESP_OK;
#endif
}

uint8_t *burner_spi_alloc_rw_buffer(size_t len, bool *needs_free)
{
    uint8_t *buf = NULL;

    if (needs_free != NULL) {
        *needs_free = false;
    }
    if (len == 0u || len > BURNER_SPI_MAX_XFER) {
        return NULL;
    }

    if (s_mcu_spi_rw_shadow != NULL && len <= s_mcu_spi_rw_shadow_size) {
        return s_mcu_spi_rw_shadow;
    }

    buf = (uint8_t *)heap_caps_malloc(
        len,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf != NULL && needs_free != NULL) {
        *needs_free = true;
    }
    return buf;
}

uint8_t *burner_spi_alloc_tx_buffer(size_t len, bool *needs_free)
{
    uint8_t *buf = NULL;

    if (needs_free != NULL) {
        *needs_free = false;
    }
    if (len == 0u || len > BURNER_SPI_MAX_XFER) {
        return NULL;
    }

    if (s_mcu_spi_tx_shadow != NULL && len <= s_mcu_spi_tx_shadow_size) {
        return s_mcu_spi_tx_shadow;
    }

    buf = (uint8_t *)heap_caps_malloc(
        len,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf != NULL && needs_free != NULL) {
        *needs_free = true;
    }
    return buf;
}

static uint8_t burner_bacon_option_byte2(
    uint8_t batch_size,
    bool dir_a,
    bool dir_ad,
    bool ad_incr,
    bool cs1,
    bool rd,
    bool wr)
{
    uint8_t ret = (uint8_t)((batch_size & 0x03u) << 6);

    if (dir_a) {
        ret |= 0x20u;
    }
    if (dir_ad) {
        ret |= 0x10u;
    }
    if (ad_incr) {
        ret |= 0x08u;
    }
    if (cs1) {
        ret |= 0x04u;
    }
    if (rd) {
        ret |= 0x02u;
    }
    if (wr) {
        ret |= 0x01u;
    }

    return ret;
}

static uint8_t burner_bacon_option_byte1(
    bool power_en,
    bool power_5v,
    bool power_3v,
    bool dir_cs2,
    uint8_t phi_div)
{
    uint8_t ret = 0u;

    if (power_en) {
        ret |= 0x40u;
    }
    if (power_5v) {
        ret |= 0x20u;
    }
    if (power_3v) {
        ret |= 0x10u;
    }
    if (dir_cs2) {
        ret |= 0x04u;
    }
    ret |= (uint8_t)(phi_div & 0x03u);
    return ret;
}

static esp_err_t burner_bacon_gbc_write(uint16_t addr, const uint8_t *buf, size_t len)
{
    size_t i;
    size_t seq_len;
    esp_err_t err;
    uint8_t *sequence;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Host CartAdapter_bacon.cs: spi_cs=2 + optionByte2 stream. */
    seq_len = 4u + 4u * len;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

    sequence = (uint8_t *)malloc(seq_len);
    if (sequence == NULL) {
        return ESP_ERR_NO_MEM;
    }

    sequence[0] = burner_bacon_option_byte2(3, true, true, false, false, true, true);
    sequence[1] = 0x00u; /* a[7:0] */
    sequence[2] = (uint8_t)(addr & 0xFFu);
    sequence[3] = (uint8_t)((addr >> 8) & 0xFFu);

    for (i = 0; i < len; ++i) {
        size_t base = 4u + 4u * i;
        sequence[base + 0u] = burner_bacon_option_byte2(1, true, true, false, false, true, true);
        sequence[base + 1u] = buf[i];
        sequence[base + 2u] = burner_bacon_option_byte2(0, true, true, false, false, true, false);
        sequence[base + 3u] = burner_bacon_option_byte2(0, true, true, true, true, true, true);
    }

    err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, sequence, NULL, seq_len);
    free(sequence);
    return err;
}

static esp_err_t burner_bacon_gbc_read(uint16_t addr, uint8_t *buf, size_t len)
{
    size_t i;
    size_t seq_len;
    esp_err_t err;
    uint8_t *tx_sequence;
    uint8_t *rx_sequence;
    bool free_tx_sequence = false;
    bool free_rx_sequence = false;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Host CartAdapter_bacon.cs: spi_cs=2 + optionByte2 stream. */
    seq_len = 4u + 3u * len;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_sequence = burner_spi_alloc_tx_buffer(seq_len, &free_tx_sequence);
    rx_sequence = burner_spi_alloc_rw_buffer(seq_len, &free_rx_sequence);
    if (tx_sequence == NULL || rx_sequence == NULL) {
        if (free_tx_sequence && tx_sequence != NULL) {
            free(tx_sequence);
        }
        if (free_rx_sequence && rx_sequence != NULL) {
            free(rx_sequence);
        }
        return ESP_ERR_NO_MEM;
    }

    tx_sequence[0] = burner_bacon_option_byte2(3, false, true, false, false, true, true);
    tx_sequence[1] = 0x00u; /* a[7:0] */
    tx_sequence[2] = (uint8_t)(addr & 0xFFu);
    tx_sequence[3] = (uint8_t)((addr >> 8) & 0xFFu);

    for (i = 0; i < len; ++i) {
        size_t base = 4u + 3u * i;
        tx_sequence[base + 0u] = burner_bacon_option_byte2(0, false, true, false, false, false, true);
        tx_sequence[base + 1u] = burner_bacon_option_byte2(1, false, true, true, true, true, true);
        tx_sequence[base + 2u] = 0x00u;
    }

    err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, tx_sequence, rx_sequence, seq_len);
    if (err == ESP_OK) {
        for (i = 0; i < len; ++i) {
            buf[i] = rx_sequence[4u + i * 3u + 2u];
        }
    }
    if (free_tx_sequence) {
        free(tx_sequence);
    }
    if (free_rx_sequence) {
        free(rx_sequence);
    }
    return err;
}

static esp_err_t burner_bacon_gbc_read_stream_hoststyle(uint16_t addr, uint8_t *buf, size_t len)
{
    size_t bytes_done = 0u;
    size_t chunk_len_limit;
    size_t chunk_bytes_limit;
    uint8_t setup[4];
    uint8_t *tx_chunk = NULL;
    uint8_t *rx_chunk = NULL;
    bool free_tx_chunk = false;
    bool free_rx_chunk = false;
    bool session_open = false;
    esp_err_t err = ESP_OK;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    chunk_len_limit = BURNER_SPI_STREAM_CHUNK_BYTES;
    if (chunk_len_limit == 0u || chunk_len_limit > BURNER_SPI_MAX_XFER) {
        chunk_len_limit = BURNER_SPI_MAX_XFER;
    }
    chunk_len_limit -= (chunk_len_limit % 3u);
    if (chunk_len_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    chunk_bytes_limit = chunk_len_limit / 3u;
    if (chunk_bytes_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_chunk = burner_spi_alloc_tx_buffer(chunk_len_limit, &free_tx_chunk);
    rx_chunk = burner_spi_alloc_rw_buffer(chunk_len_limit, &free_rx_chunk);
    if (tx_chunk == NULL || rx_chunk == NULL) {
        if (free_tx_chunk && tx_chunk != NULL) {
            free(tx_chunk);
        }
        if (free_rx_chunk && rx_chunk != NULL) {
            free(rx_chunk);
        }
        return ESP_ERR_NO_MEM;
    }

    /* Match host bacon_gbc_read(): set base address once, then stream byte reads. */
    setup[0] = burner_bacon_option_byte2(3, false, true, false, false, true, true);
    setup[1] = 0x00u;
    setup[2] = (uint8_t)(addr & 0xFFu);
    setup[3] = (uint8_t)((addr >> 8) & 0xFFu);

    err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_2);
    if (err != ESP_OK) {
        goto gbc_read_stream_out;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto gbc_read_stream_out;
    }

    while (bytes_done < len) {
        size_t chunk_bytes = len - bytes_done;
        size_t chunk_len;

        if (chunk_bytes > chunk_bytes_limit) {
            chunk_bytes = chunk_bytes_limit;
        }
        chunk_len = chunk_bytes * 3u;

        for (size_t i = 0u; i < chunk_bytes; ++i) {
            size_t base = i * 3u;
            tx_chunk[base + 0u] = burner_bacon_option_byte2(0, false, true, false, false, false, true);
            tx_chunk[base + 1u] = burner_bacon_option_byte2(1, false, true, true, true, true, true);
            tx_chunk[base + 2u] = 0x00u;
        }

        err = burner_spi_transfer_active(tx_chunk, rx_chunk, chunk_len);
        if (err != ESP_OK) {
            goto gbc_read_stream_out;
        }

        for (size_t i = 0u; i < chunk_bytes; ++i) {
            buf[bytes_done + i] = rx_chunk[i * 3u + 2u];
        }

        bytes_done += chunk_bytes;
    }

gbc_read_stream_out:
    if (session_open) {
        burner_spi_end_cs(BURNER_SPI_CS_MODE_2);
    }
    if (free_tx_chunk && tx_chunk != NULL) {
        free(tx_chunk);
    }
    if (free_rx_chunk && rx_chunk != NULL) {
        free(rx_chunk);
    }
    return err;
}

static esp_err_t burner_bacon_ram_write(uint16_t addr, const uint8_t *buf, size_t len)
{
    size_t i;
    size_t seq_len;
    esp_err_t err;
    uint8_t *sequence;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    seq_len = len * 6u;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

    sequence = (uint8_t *)malloc(seq_len);
    if (sequence == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < len; ++i) {
        uint16_t ram_addr = (uint16_t)(addr + (uint16_t)i);
        size_t base = i * 6u;

        sequence[base + 0u] = burner_bacon_option_byte0(3, true, true, false, true, true, true);
        sequence[base + 1u] = (uint8_t)(ram_addr & 0xFFu);
        sequence[base + 2u] = (uint8_t)((ram_addr >> 8) & 0xFFu);
        sequence[base + 3u] = buf[i];
        sequence[base + 4u] = burner_bacon_option_byte0(0, true, true, false, true, true, false);
        sequence[base + 5u] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
    }

    err = burner_spi_transfer(sequence, NULL, seq_len);
    free(sequence);
    return err;
}

static esp_err_t burner_bacon_gbc_read_u8(uint16_t addr, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_gbc_read(addr, value, 1);
}

static esp_err_t burner_bacon_gbc_write_for_fram(uint16_t addr, const uint8_t *buf, size_t len, uint8_t latency)
{
    /* Host bacon adapter currently aliases FRAM path to normal GBC write. */
    (void)latency;
    return burner_bacon_gbc_write(addr, buf, len);
}

static esp_err_t burner_bacon_gbc_read_for_fram(uint16_t addr, uint8_t *buf, size_t len, uint8_t latency)
{
    /* Host bacon adapter currently aliases FRAM path to normal GBC read. */
    (void)latency;
    return burner_bacon_gbc_read(addr, buf, len);
}

esp_err_t burner_bacon_gba_power_cmd(bool power_5v, bool power_3v3)
{
    uint8_t cmd;

    /*
     * Host dual-CS power command (spi_cs=1):
     * option byte with batch=1, cs2/cs1/rd/wr=1, dir_a=5V, dir_ad=3V3.
     * This is decoded in example/logic/bacon.v.
     */
    cmd = burner_bacon_option_byte1(
        power_5v || power_3v3,
        power_5v,
        power_3v3,
        true,
        0u);
    return burner_spi_transfer_cs(BURNER_SPI_CS_MODE_1, &cmd, NULL, 1u);
}

static esp_err_t burner_bacon_gba_power_cycle_3v3_locked(void)
{
    esp_err_t err;

    err = burner_bacon_gba_power_cmd(false, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BURNER_POWER_SETTLE_MS));

    err = burner_bacon_gba_power_cmd(false, true);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BURNER_POWER_SETTLE_MS));
    return ESP_OK;
}

static esp_err_t burner_bacon_gba_release_bus_idle(void)
{
    uint8_t cmd;

    /*
     * Match the host's one-byte interface init after 3V3 is enabled:
     * optionByte0(0, ai, adi, cs2h, cs1h, rdh, wrh).
     * This restores the GBA-side shadow outputs to a known released state
     * before short command transactions such as read-ID/autoselect.
     */
    cmd = burner_bacon_option_byte0(0u, false, false, true, true, true, true);
    return burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, &cmd, NULL, 1u);
}

esp_err_t burner_bacon_gba_prepare_power(void)
{
    esp_err_t err = ESP_OK;

    err = burner_bacon_gba_power_cycle_3v3_locked();
    if (err != ESP_OK) {
        return err;
    }

    return burner_bacon_gba_release_bus_idle();
}

esp_err_t burner_bacon_mbc5_prepare_power(void)
{
    esp_err_t err;
    bool use_5v = (s_mbc5_power_5v_enabled != 0u);

    /* Match host mission: power rails off first, then enable the selected GBC rail. */
    err = burner_bacon_gba_power_cmd(false, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BURNER_POWER_SETTLE_MS));

    err = burner_bacon_gba_power_cmd(use_5v, !use_5v);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(BURNER_TAG, "MBC5 power rail: %s", use_5v ? "5V" : "3V3");
    vTaskDelay(pdMS_TO_TICKS(BURNER_POWER_SETTLE_MS));
    return ESP_OK;
}

void burner_bacon_restore_3v3_power(void)
{
    (void)burner_bacon_gba_power_cmd(false, true);
    vTaskDelay(pdMS_TO_TICKS(BURNER_POWER_SETTLE_MS));
    (void)burner_bacon_gba_release_bus_idle();
}

static esp_err_t burner_bacon_rom_write_u16(uint32_t word_addr, uint16_t value)
{
    uint8_t seq[11];

    seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    seq[1] = (uint8_t)(word_addr & 0xFFu);
    seq[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    seq[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[5] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
    seq[6] = (uint8_t)(value & 0xFFu);
    seq[7] = (uint8_t)((value >> 8) & 0xFFu);
    seq[8] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
    seq[9] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[10] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
    return burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0, seq, NULL, sizeof(seq));
}

static esp_err_t burner_bacon_rom_read_u16(uint32_t word_addr, uint16_t *out_value);
static esp_err_t burner_bacon_gba_command_write_u16(uint32_t word_addr, uint16_t value);
static esp_err_t burner_bacon_gba_reset_to_read_mode(void);
static bool burner_gba_nor_is_intel_active(void);
static uint32_t burner_erase_timeout_ms_for_bytes(uint32_t bytes);
static esp_err_t burner_bacon_gba_erase_sector(uint32_t flash_addr, bool is_multi_card, uint32_t timeout_ms);

static esp_err_t burner_bacon_rom_read_packed(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    size_t read_len_word;
    size_t chunk_len_limit;
    size_t chunk_words_limit;
    size_t words_done = 0u;
    uint8_t setup[5];
    uint8_t rd_low;
    uint8_t release;
    uint8_t *tx_chunk = NULL;
    uint8_t *rx_chunk = NULL;
    bool free_tx_chunk = false;
    bool free_rx_chunk = false;
    bool session_open = false;
    bool release_sent = false;
    esp_err_t err;

    if (buf == NULL || len == 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    read_len_word = len / 2u;
    chunk_len_limit = BURNER_SPI_STREAM_CHUNK_BYTES;
    if (chunk_len_limit == 0u || chunk_len_limit > BURNER_SPI_MAX_XFER) {
        chunk_len_limit = BURNER_SPI_MAX_XFER;
    }
    chunk_len_limit -= (chunk_len_limit % 3u);
    if (chunk_len_limit == 0u) {
        chunk_len_limit = 3u;
    }
    chunk_words_limit = chunk_len_limit / 3u;
    if (chunk_words_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_chunk = burner_spi_alloc_tx_buffer(chunk_len_limit, &free_tx_chunk);
    rx_chunk = burner_spi_alloc_rw_buffer(chunk_len_limit, &free_rx_chunk);
    if (tx_chunk == NULL || rx_chunk == NULL) {
        if (free_tx_chunk && tx_chunk != NULL) {
            free(tx_chunk);
        }
        if (free_rx_chunk && rx_chunk != NULL) {
            free(rx_chunk);
        }
        return ESP_ERR_NO_MEM;
    }

    setup[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    setup[1] = (uint8_t)((addr_byte >> 1) & 0xFFu);
    setup[2] = (uint8_t)(((addr_byte >> 1) >> 8) & 0xFFu);
    setup[3] = (uint8_t)(((addr_byte >> 1) >> 16) & 0xFFu);
    setup[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    rd_low = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    release = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_0);
    if (err != ESP_OK) {
        goto rom_read_out;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto rom_read_out;
    }

    err = burner_spi_transfer_active(&rd_low, NULL, 1u);
    if (err != ESP_OK) {
        goto rom_read_out;
    }
    if (BURNER_GBA_ROM_PHASE_GAP_US > 0u) {
        esp_rom_delay_us(BURNER_GBA_ROM_PHASE_GAP_US);
    }

    while (words_done < read_len_word) {
        size_t chunk_words = read_len_word - words_done;
        size_t chunk_len;

        if (chunk_words > chunk_words_limit) {
            chunk_words = chunk_words_limit;
        }
        chunk_len = chunk_words * 3u;

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 3u;
            tx_chunk[base + 0u] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
            tx_chunk[base + 1u] = 0x00u;
            tx_chunk[base + 2u] = 0x00u;
        }

        err = burner_spi_transfer_active(tx_chunk, rx_chunk, chunk_len);
        if (err != ESP_OK) {
            goto rom_read_out;
        }

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 3u;
            buf[(words_done + i) * 2u + 0u] = rx_chunk[base + 1u];
            buf[(words_done + i) * 2u + 1u] = rx_chunk[base + 2u];
        }

        words_done += chunk_words;
    }

    if (BURNER_GBA_ROM_PHASE_GAP_US > 0u) {
        esp_rom_delay_us(BURNER_GBA_ROM_PHASE_GAP_US);
    }
    err = burner_spi_transfer_active(&release, NULL, 1u);
    if (err == ESP_OK) {
        release_sent = true;
    }

rom_read_out:
    if (session_open) {
        if (!release_sent) {
            (void)burner_spi_transfer_active(&release, NULL, 1u);
        }
        burner_spi_end_cs(BURNER_SPI_CS_MODE_0);
    }
    if (free_tx_chunk && tx_chunk != NULL) {
        free(tx_chunk);
    }
    if (free_rx_chunk && rx_chunk != NULL) {
        free(rx_chunk);
    }
    return err;
}

#if 0
/* Legacy GBA verify path kept only for reference. */
static esp_err_t burner_bacon_rom_verify_read_packed(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    const size_t bytes_per_word = 10u;
    size_t words_done = 0u;
    size_t total_words;
    size_t chunk_words_limit;
    esp_err_t err = ESP_OK;

    if (buf == NULL || len == 0u || (len & 0x1u) != 0u || (addr_byte & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    total_words = len / 2u;
    chunk_words_limit = BURNER_SPI_MAX_XFER / bytes_per_word;
    if (chunk_words_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (words_done < total_words) {
        size_t chunk_words = total_words - words_done;
        size_t seq_len;
        uint8_t *tx_sequence = NULL;
        uint8_t *rx_sequence = NULL;
        bool free_tx_sequence = false;
        bool free_rx_sequence = false;

        if (chunk_words > chunk_words_limit) {
            chunk_words = chunk_words_limit;
        }
        seq_len = chunk_words * bytes_per_word;

        tx_sequence = burner_spi_alloc_tx_buffer(seq_len, &free_tx_sequence);
        rx_sequence = burner_spi_alloc_rw_buffer(seq_len, &free_rx_sequence);
        if (tx_sequence == NULL || rx_sequence == NULL) {
            if (free_tx_sequence && tx_sequence != NULL) {
                free(tx_sequence);
            }
            if (free_rx_sequence && rx_sequence != NULL) {
                free(rx_sequence);
            }
            return ESP_ERR_NO_MEM;
        }

        for (size_t i = 0u; i < chunk_words; ++i) {
            uint32_t word_addr = (addr_byte >> 1) + (uint32_t)words_done + (uint32_t)i;
            size_t base = i * bytes_per_word;

            tx_sequence[base + 0u] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            tx_sequence[base + 1u] = (uint8_t)(word_addr & 0xFFu);
            tx_sequence[base + 2u] = (uint8_t)((word_addr >> 8) & 0xFFu);
            tx_sequence[base + 3u] = (uint8_t)((word_addr >> 16) & 0xFFu);
            tx_sequence[base + 4u] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            tx_sequence[base + 5u] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
            tx_sequence[base + 6u] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
            tx_sequence[base + 7u] = 0x00u;
            tx_sequence[base + 8u] = 0x00u;
            tx_sequence[base + 9u] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
        }

        err = burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0, tx_sequence, rx_sequence, seq_len);
        if (err == ESP_OK) {
            for (size_t i = 0u; i < chunk_words; ++i) {
                size_t base = i * bytes_per_word;
                buf[(words_done + i) * 2u + 0u] = rx_sequence[base + 7u];
                buf[(words_done + i) * 2u + 1u] = rx_sequence[base + 8u];
            }
        }

        if (free_tx_sequence) {
            free(tx_sequence);
        }
        if (free_rx_sequence) {
            free(rx_sequence);
        }
        if (err != ESP_OK) {
            return err;
        }

        words_done += chunk_words;
    }

    return ESP_OK;
}
#endif

static esp_err_t burner_bacon_rom_verify_read_packed_hoststyle(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    size_t read_len_word;
    size_t chunk_len_limit;
    size_t chunk_words_limit;
    size_t words_done = 0u;
    uint8_t setup[5];
    uint8_t release;
    uint8_t *tx_chunk = NULL;
    uint8_t *rx_chunk = NULL;
    bool free_tx_chunk = false;
    bool free_rx_chunk = false;
    bool session_open = false;
    bool release_sent = false;
    esp_err_t err;

    if (buf == NULL || len == 0u || (len & 0x1u) != 0u || (addr_byte & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    read_len_word = len / 2u;
    chunk_len_limit = BURNER_SPI_STREAM_CHUNK_BYTES;
    if (chunk_len_limit == 0u || chunk_len_limit > BURNER_SPI_MAX_XFER) {
        chunk_len_limit = BURNER_SPI_MAX_XFER;
    }
    chunk_len_limit -= (chunk_len_limit % 4u);
    if (chunk_len_limit == 0u) {
        chunk_len_limit = 4u;
    }
    chunk_words_limit = chunk_len_limit / 4u;
    if (chunk_words_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_chunk = burner_spi_alloc_tx_buffer(chunk_len_limit, &free_tx_chunk);
    rx_chunk = burner_spi_alloc_rw_buffer(chunk_len_limit, &free_rx_chunk);
    if (tx_chunk == NULL || rx_chunk == NULL) {
        if (free_tx_chunk && tx_chunk != NULL) {
            free(tx_chunk);
        }
        if (free_rx_chunk && rx_chunk != NULL) {
            free(rx_chunk);
        }
        return ESP_ERR_NO_MEM;
    }

    /* Match Bacon host bacon_romRead(): setup once, then per-word RD low + 16-bit sample. */
    setup[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    setup[1] = (uint8_t)((addr_byte >> 1) & 0xFFu);
    setup[2] = (uint8_t)(((addr_byte >> 1) >> 8) & 0xFFu);
    setup[3] = (uint8_t)(((addr_byte >> 1) >> 16) & 0xFFu);
    setup[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    release = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_0);
    if (err != ESP_OK) {
        goto rom_verify_hoststyle_out;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto rom_verify_hoststyle_out;
    }

    while (words_done < read_len_word) {
        size_t chunk_words = read_len_word - words_done;
        size_t chunk_len;

        if (chunk_words > chunk_words_limit) {
            chunk_words = chunk_words_limit;
        }
        chunk_len = chunk_words * 4u;

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 4u;
            tx_chunk[base + 0u] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
            tx_chunk[base + 1u] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
            tx_chunk[base + 2u] = 0x00u;
            tx_chunk[base + 3u] = 0x00u;
        }

        err = burner_spi_transfer_active(tx_chunk, rx_chunk, chunk_len);
        if (err != ESP_OK) {
            goto rom_verify_hoststyle_out;
        }

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 4u;
            buf[(words_done + i) * 2u + 0u] = rx_chunk[base + 2u];
            buf[(words_done + i) * 2u + 1u] = rx_chunk[base + 3u];
        }

        words_done += chunk_words;
    }

    err = burner_spi_transfer_active(&release, NULL, 1u);
    if (err == ESP_OK) {
        release_sent = true;
    }

rom_verify_hoststyle_out:
    if (session_open) {
        if (!release_sent) {
            (void)burner_spi_transfer_active(&release, NULL, 1u);
        }
        burner_spi_end_cs(BURNER_SPI_CS_MODE_0);
    }
    if (free_tx_chunk && tx_chunk != NULL) {
        free(tx_chunk);
    }
    if (free_rx_chunk && rx_chunk != NULL) {
        free(rx_chunk);
    }
    return err;
}

static esp_err_t burner_bacon_rom_read(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_read_packed(addr_byte, buf, len);
}

static esp_err_t burner_bacon_rom_read_u16(uint32_t word_addr, uint16_t *out_value)
{
    uint8_t tx_seq[10];
    uint8_t rx_seq[10] = {0};
    esp_err_t err;

    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tx_seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    tx_seq[1] = (uint8_t)(word_addr & 0xFFu);
    tx_seq[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    tx_seq[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    tx_seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    tx_seq[5] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    tx_seq[6] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
    tx_seq[7] = 0x00u;
    tx_seq[8] = 0x00u;
    tx_seq[9] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0, tx_seq, rx_seq, sizeof(tx_seq));
    if (err != ESP_OK) {
        return err;
    }
    *out_value = (uint16_t)((uint16_t)rx_seq[7] | ((uint16_t)rx_seq[8] << 8));
    return ESP_OK;
}

static esp_err_t burner_bacon_rom_read_u16_batched(uint32_t start_word_addr, uint8_t *out, size_t word_count)
{
    if (out == NULL || word_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_read(start_word_addr << 1, out, word_count * 2u);
}

#if 0
/* Legacy GBA verify wrapper kept only for reference. */
static esp_err_t burner_bacon_rom_verify_read_u16_batched(uint32_t start_word_addr, uint8_t *out, size_t word_count)
{
    if (out == NULL || word_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_verify_read_packed(start_word_addr << 1, out, word_count * 2u);
}
#endif

static esp_err_t burner_bacon_rom_verify_read_u16_batched_hoststyle(uint32_t start_word_addr, uint8_t *out, size_t word_count)
{
    if (out == NULL || word_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_verify_read_packed_hoststyle(start_word_addr << 1, out, word_count * 2u);
}

static esp_err_t burner_bacon_wait_u16(uint32_t byte_addr, uint16_t expected, uint32_t timeout_ms)
{
    int64_t deadline_us;
    uint16_t read_back = 0;
    esp_err_t err = ESP_OK;

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(byte_addr >> 1, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == expected) {
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    ESP_LOGW(
        BURNER_TAG,
        "GBA program wait timeout @0x%08" PRIX32 ": expected=0x%04X read=0x%04X timeout=%" PRIu32 "ms",
        byte_addr,
        expected,
        read_back,
        timeout_ms);
    (void)burner_bacon_gba_reset_to_read_mode();
    return ESP_ERR_TIMEOUT;
}

static inline uint16_t burner_apply_d0d1_swap_on_read(uint16_t data, bool is_swapped);
static inline uint16_t burner_apply_d0d1_swap_on_write(uint16_t data, bool is_swapped);
static esp_err_t burner_bacon_gba_read_id(uint8_t id_out[8], bool is_swapped);
static esp_err_t burner_bacon_gba_read_id_with_cmdset(
    uint8_t id_out[8],
    bool is_swapped,
    burner_nor_cmdset_t cmdset,
    const char *trace_name);

static esp_err_t burner_bacon_wait_u16_mask(
    uint32_t byte_addr,
    uint16_t mask,
    uint16_t expected,
    uint32_t timeout_ms,
    uint16_t *read_back_out)
{
    int64_t deadline_us;
    uint16_t read_back = 0;
    esp_err_t err = ESP_OK;

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(byte_addr >> 1, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        read_back = burner_apply_d0d1_swap_on_read(read_back, s_cart_ctx.d0d1_swapped);
        if ((read_back & mask) == expected) {
            if (read_back_out != NULL) {
                *read_back_out = read_back;
            }
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    if (read_back_out != NULL) {
        *read_back_out = read_back;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gba_intel_reset(void)
{
    esp_err_t err;

    err = burner_bacon_gba_command_write_u16(0x000u, 0x0050u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gba_command_write_u16(0x000u, 0x00FFu);
}

static esp_err_t burner_bacon_gba_intel_read_array(void)
{
    return burner_bacon_gba_command_write_u16(0x000u, 0x00FFu);
}

static esp_err_t burner_bacon_gba_reset_to_read_mode_for_cmdset(burner_nor_cmdset_t cmdset)
{
    if (cmdset == BURNER_NOR_CMDSET_INTEL) {
        return burner_bacon_gba_intel_reset();
    }
    return burner_bacon_gba_command_write_u16(0x000u, 0x00F0u);
}

static esp_err_t burner_bacon_gba_reset_to_read_mode(void)
{
    return burner_bacon_gba_reset_to_read_mode_for_cmdset(s_cart_ctx.gba_cmdset);
}

static esp_err_t burner_bacon_gba_intel_wait_ready(uint32_t flash_addr, uint16_t command, uint32_t timeout_ms, uint16_t *status_out)
{
    esp_err_t err;

    if (command != 0u) {
        err = burner_bacon_gba_command_write_u16(flash_addr >> 1, command);
        if (err != ESP_OK) {
            return err;
        }
    }
    return burner_bacon_wait_u16_mask(flash_addr, 0x0080u, 0x0080u, timeout_ms, status_out);
}

static uint16_t burner_gba_program_buffer_write_bytes(uint16_t reported_bytes, burner_nor_cmdset_t cmdset)
{
    if (cmdset == BURNER_NOR_CMDSET_INTEL && reported_bytes == 0u) {
        return BURNER_GBA_INTEL_RUNTIME_BUFFER_DEFAULT_BYTES;
    }
    return reported_bytes;
}

static bool burner_gba_intel_program_buffer_needs_runtime_fallback(
    burner_nor_cmdset_t cmdset,
    uint16_t reported_bytes,
    uint16_t active_bytes)
{
    return cmdset == BURNER_NOR_CMDSET_INTEL &&
           reported_bytes == 0u &&
           active_bytes >= BURNER_GBA_INTEL_RUNTIME_BUFFER_MIN_BYTES;
}

static uint16_t burner_gba_intel_next_program_buffer_write_bytes(uint16_t current_bytes)
{
    if (current_bytes > BURNER_GBA_INTEL_RUNTIME_BUFFER_DEFAULT_BYTES) {
        current_bytes = BURNER_GBA_INTEL_RUNTIME_BUFFER_DEFAULT_BYTES;
    }
    if (current_bytes > 256u) {
        return 256u;
    }
    if (current_bytes > 128u) {
        return 128u;
    }
    if (current_bytes > BURNER_GBA_INTEL_RUNTIME_BUFFER_MIN_BYTES) {
        return BURNER_GBA_INTEL_RUNTIME_BUFFER_MIN_BYTES;
    }
    return 0u;
}

static void burner_gba_sector_erase_ctx_reset(void)
{
    memset(&s_gba_sector_erase_ctx, 0, sizeof(s_gba_sector_erase_ctx));
}

static esp_err_t burner_gba_sector_is_blank(
    uint32_t sector_addr,
    uint32_t sector_size,
    bool is_multi_card,
    bool *blank_out);

static void burner_gba_sector_erase_ctx_begin(
    uint32_t range_begin,
    uint32_t range_end,
    uint32_t sector_size,
    bool multi_card,
    bool erase_always)
{
    burner_gba_sector_erase_ctx_reset();
    (void)sector_size;
    if (!burner_nor_geometry_is_valid(&s_cart_ctx.geometry) || range_end < range_begin) {
        return;
    }
    if (burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry, range_begin, &s_gba_sector_erase_ctx.cursor) !=
        ESP_OK) {
        burner_gba_sector_erase_ctx_reset();
        return;
    }

    s_gba_sector_erase_ctx.active = true;
    s_gba_sector_erase_ctx.multi_card = multi_card;
    s_gba_sector_erase_ctx.erase_always = erase_always;
    s_gba_sector_erase_ctx.range_end = range_end;
    s_gba_sector_erase_ctx.erased_sector_addr = UINT32_MAX;
    s_gba_sector_erase_ctx.pre_erased_sector_addr = UINT32_MAX;
    s_gba_sector_erase_ctx.pre_erased_valid = false;
}

static bool burner_gba_sector_erase_ctx_should_handle(void)
{
    return s_gba_sector_erase_ctx.active && burner_nor_geometry_is_valid(&s_cart_ctx.geometry);
}

static esp_err_t burner_gba_sector_erase_ctx_sync_cursor(
    uint32_t byte_addr,
    uint32_t *sector_addr_out,
    uint32_t *sector_end_out,
    uint32_t *sector_size_out)
{
    esp_err_t err;

    err = burner_nor_geometry_region_cursor_seek_forward(&s_cart_ctx.geometry, byte_addr, &s_gba_sector_erase_ctx.cursor);
    if (err != ESP_OK) {
        return err;
    }
    return burner_nor_geometry_sector_bounds_in_cursor(
        &s_gba_sector_erase_ctx.cursor,
        byte_addr,
        sector_addr_out,
        sector_end_out,
        sector_size_out);
}

static esp_err_t burner_gba_sector_erase_now(uint32_t sector_addr, uint32_t sector_size)
{
    esp_err_t err;

    if (sector_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_gba_sector_erase_ctx.erase_always) {
        bool blank = false;

        err = burner_gba_sector_is_blank(
            sector_addr,
            sector_size,
            s_gba_sector_erase_ctx.multi_card,
            &blank);
        if (err != ESP_OK) {
            return err;
        }
        if (blank) {
            burner_status_advance_erase_phase(1u, sector_size);
            return ESP_OK;
        }
    }

    burner_status_mark_erase_begin();
    err = burner_bacon_gba_erase_sector(
        sector_addr,
        s_gba_sector_erase_ctx.multi_card,
        burner_erase_timeout_ms_for_bytes(sector_size));
    burner_status_mark_erase_end();
    if (err == ESP_OK) {
        burner_status_advance_erase_phase(1u, sector_size);
    }
    return err;
}

static esp_err_t burner_gba_sector_erase_prepare_current(uint32_t byte_addr)
{
    uint32_t sector_addr;
    uint32_t sector_size = 0u;
    esp_err_t err;

    if (!burner_gba_sector_erase_ctx_should_handle()) {
        return ESP_OK;
    }

    err = burner_gba_sector_erase_ctx_sync_cursor(byte_addr, &sector_addr, NULL, &sector_size);
    if (err != ESP_OK || sector_size == 0u) {
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }
    if (s_gba_sector_erase_ctx.erased_sector_addr == sector_addr) {
        return ESP_OK;
    }

    if (s_gba_sector_erase_ctx.pre_erased_valid &&
        s_gba_sector_erase_ctx.pre_erased_sector_addr == sector_addr) {
        s_gba_sector_erase_ctx.erased_sector_addr = sector_addr;
        s_gba_sector_erase_ctx.pre_erased_valid = false;
        s_gba_sector_erase_ctx.pre_erased_sector_addr = UINT32_MAX;
        return ESP_OK;
    }

    err = burner_gba_sector_erase_now(sector_addr, sector_size);
    if (err != ESP_OK) {
        return err;
    }
    s_gba_sector_erase_ctx.erased_sector_addr = sector_addr;
    return ESP_OK;
}

static esp_err_t burner_gba_sector_erase_prefetch_next(uint32_t byte_addr, size_t len)
{
    uint32_t current_sector_addr;
    uint32_t current_sector_end;
    uint32_t next_sector_addr;
    uint32_t current_sector_size = 0u;
    uint32_t next_sector_size = 0u;
    uint32_t chunk_end;
    burner_nor_region_cursor_t next_cursor;
    esp_err_t err;

    if (!burner_gba_sector_erase_ctx_should_handle() || len == 0u) {
        return ESP_OK;
    }

    err = burner_gba_sector_erase_ctx_sync_cursor(
        byte_addr,
        &current_sector_addr,
        &current_sector_end,
        &current_sector_size);
    if (err != ESP_OK || current_sector_size == 0u) {
        return err;
    }
    current_sector_end -= 1u;
    chunk_end = byte_addr + (uint32_t)len - 1u;
    if (chunk_end < current_sector_end) {
        return ESP_OK;
    }

    next_cursor = s_gba_sector_erase_ctx.cursor;
    next_sector_addr = current_sector_addr + current_sector_size;
    next_sector_size = current_sector_size;
    if (next_sector_addr >= next_cursor.addr_end) {
        err = burner_nor_geometry_region_cursor_advance(&s_cart_ctx.geometry, &next_cursor);
        if (err != ESP_OK) {
            return ESP_OK;
        }
        next_sector_addr = next_cursor.addr_begin;
        next_sector_size = next_cursor.sector_size;
    }
    if (next_sector_addr > s_gba_sector_erase_ctx.range_end || next_sector_size == 0u) {
        return ESP_OK;
    }
    if (s_gba_sector_erase_ctx.pre_erased_valid &&
        s_gba_sector_erase_ctx.pre_erased_sector_addr == next_sector_addr) {
        return ESP_OK;
    }
    if (s_gba_sector_erase_ctx.erased_sector_addr == next_sector_addr) {
        return ESP_OK;
    }

    err = burner_gba_sector_erase_now(next_sector_addr, next_sector_size);
    if (err != ESP_OK) {
        return err;
    }
    s_gba_sector_erase_ctx.pre_erased_valid = true;
    s_gba_sector_erase_ctx.pre_erased_sector_addr = next_sector_addr;
    s_gba_sector_erase_ctx.erased_sector_addr = current_sector_addr;
    return ESP_OK;
}

static bool burner_gba_nor_is_intel_active(void)
{
    return s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL;
}

static esp_err_t burner_bacon_gba_rom_switch_bank(uint8_t bank)
{
    uint8_t high = (uint8_t)((bank & 0x0Fu) << 4);
    uint8_t low = 0x40u;
    esp_err_t err = ESP_OK;

    err = burner_bacon_ram_write(0x0002u, &high, 1u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_ram_write(0x0003u, &low, 1u);
}

static size_t burner_gba_program_safe_chunk_bytes(uint32_t byte_addr, size_t requested_len, size_t write_unit_bytes)
{
    size_t chunk = requested_len;
    uint32_t boundary_remain;

    if (chunk == 0u) {
        return 0u;
    }

    if (write_unit_bytes >= 2u && (write_unit_bytes & (write_unit_bytes - 1u)) == 0u) {
        boundary_remain = (uint32_t)(write_unit_bytes - (byte_addr & (uint32_t)(write_unit_bytes - 1u)));
        if (boundary_remain > 0u && chunk > boundary_remain) {
            chunk = boundary_remain;
        }
    }

    boundary_remain = BURN_GBA_BANK_BYTES - (byte_addr % BURN_GBA_BANK_BYTES);
    if (boundary_remain > 0u && chunk > boundary_remain) {
        chunk = boundary_remain;
    }

    chunk &= ~((size_t)0x1u);
    return (chunk == 0u) ? 2u : chunk;
}

static uint32_t burner_gba_sector_begin_for_addr(uint32_t byte_addr)
{
    uint32_t sector_begin = byte_addr;

    if (burner_nor_geometry_sector_bounds(&s_cart_ctx.geometry, byte_addr, &sector_begin, NULL, NULL) == ESP_OK) {
        return sector_begin;
    }
    if (s_cart_ctx.sector_size > 0u && (s_cart_ctx.sector_size & (s_cart_ctx.sector_size - 1u)) == 0u) {
        return byte_addr & ~(s_cart_ctx.sector_size - 1u);
    }
    return byte_addr;
}

static esp_err_t burner_bacon_gba_intel_buffered_program_once(
    uint32_t starting_address,
    const uint8_t *buf,
    size_t remain_len,
    uint16_t buffer_write_bytes,
    size_t *written_out)
{
    uint32_t starting_word_address;
    size_t write_len;
    size_t write_words;
    size_t max_write_words_by_spi = 1u;
    size_t seq_len;
    uint8_t *seq;
    bool free_seq = false;
    uint8_t addr0;
    uint8_t addr1;
    uint8_t addr2;
    uint32_t sector_address;
    uint32_t sector_word_address;
    uint16_t status = 0u;
    uint16_t confirm_cmd;
    uint16_t write_count_word;
    size_t wr;
    esp_err_t err;

    if (written_out != NULL) {
        *written_out = 0u;
    }
    if (buf == NULL || written_out == NULL || remain_len < 2u || buffer_write_bytes < 2u ||
        (starting_address & 0x1u) != 0u || (remain_len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    starting_word_address = starting_address >> 1;
    write_len = remain_len;
    if (write_len > buffer_write_bytes) {
        write_len = buffer_write_bytes;
    }
    write_len = burner_gba_program_safe_chunk_bytes(
        starting_address,
        write_len,
        (size_t)buffer_write_bytes);
    if (write_len < 2u) {
        write_len = 2u;
    }
    write_words = write_len / 2u;
    if (BURNER_SPI_MAX_XFER > 28u) {
        max_write_words_by_spi = (BURNER_SPI_MAX_XFER - 28u) / 5u;
    }
    if (max_write_words_by_spi == 0u) {
        max_write_words_by_spi = 1u;
    }
    if (write_words > max_write_words_by_spi) {
        write_words = max_write_words_by_spi;
        write_len = write_words * 2u;
    }

    sector_address = burner_gba_sector_begin_for_addr(starting_address);
    sector_word_address = sector_address >> 1;
    err = burner_bacon_gba_intel_wait_ready(sector_address, 0x00E8u, BURNER_ROM_POLL_TIMEOUT_MS, &status);
    if (err != ESP_OK) {
        return err;
    }

    seq_len = 28u + 5u * write_words;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }
    seq = burner_spi_alloc_tx_buffer(seq_len, &free_seq);
    if (seq == NULL) {
        return ESP_ERR_NO_MEM;
    }

    confirm_cmd = burner_apply_d0d1_swap_on_write(0x00D0u, s_cart_ctx.d0d1_swapped);
    addr0 = (uint8_t)(sector_word_address & 0xFFu);
    addr1 = (uint8_t)((sector_word_address >> 8) & 0xFFu);
    addr2 = (uint8_t)((sector_word_address >> 16) & 0xFFu);
    write_count_word = burner_apply_d0d1_swap_on_write(
        (uint16_t)(write_words - 1u),
        s_cart_ctx.d0d1_swapped);

    seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    seq[1] = addr0;
    seq[2] = addr1;
    seq[3] = addr2;
    seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[5] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
    seq[6] = (uint8_t)(write_count_word & 0xFFu);
    seq[7] = (uint8_t)((write_count_word >> 8) & 0xFFu);
    seq[8] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
    seq[9] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[10] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
    seq[11] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    seq[12] = (uint8_t)(starting_word_address & 0xFFu);
    seq[13] = (uint8_t)((starting_word_address >> 8) & 0xFFu);
    seq[14] = (uint8_t)((starting_word_address >> 16) & 0xFFu);
    seq[15] = burner_bacon_option_byte0(0, true, true, true, false, true, true);

    for (wr = 0u; wr < write_words; ++wr) {
        size_t base = 16u + 5u * wr;
        seq[base + 0u] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
        seq[base + 1u] = buf[wr * 2u];
        seq[base + 2u] = buf[wr * 2u + 1u];
        seq[base + 3u] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
        seq[base + 4u] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    }

    seq[16u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
    seq[17u + 5u * write_words] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    seq[18u + 5u * write_words] = addr0;
    seq[19u + 5u * write_words] = addr1;
    seq[20u + 5u * write_words] = addr2;
    seq[21u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[22u + 5u * write_words] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
    seq[23u + 5u * write_words] = (uint8_t)(confirm_cmd & 0xFFu);
    seq[24u + 5u * write_words] = (uint8_t)((confirm_cmd >> 8) & 0xFFu);
    seq[25u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
    seq[26u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[27u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_transfer(seq, NULL, seq_len);
    if (free_seq) {
        free(seq);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_gba_intel_wait_ready(sector_address, 0x0070u, BURNER_ROM_POLL_TIMEOUT_MS, &status);
    if (err != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA intel buffered program timeout @0x%08" PRIX32 " status=0x%04X",
            starting_address,
            status);
        return err;
    }
    err = burner_bacon_gba_intel_read_array();
    if (err != ESP_OK) {
        return err;
    }

    *written_out = write_len;
    return ESP_OK;
}

static void burner_gba_resolve_write_addr(
    uint32_t rom_addr,
    bool is_multi_card,
    uint32_t *bank_out,
    uint32_t *bank_remain_out)
{
    uint32_t bank = 0u;
    uint32_t bank_remain = UINT32_MAX - rom_addr;
    uint32_t bank_off;

    if (is_multi_card) {
        bank = rom_addr / BURN_GBA_BANK_BYTES;
        bank_off = rom_addr % BURN_GBA_BANK_BYTES;
        bank_remain = BURN_GBA_BANK_BYTES - bank_off;
    }

    if (bank_out != NULL) {
        *bank_out = bank;
    }
    if (bank_remain_out != NULL) {
        *bank_remain_out = bank_remain;
    }
}

static esp_err_t burner_gba_switch_bank_if_needed(uint32_t bank)
{
    esp_err_t err;

    if (bank > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (bank == (uint32_t)s_cart_ctx.current_bank) {
        return ESP_OK;
    }

    err = burner_bacon_gba_rom_switch_bank((uint8_t)bank);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BURNER_GBA_BANK_SWITCH_SETTLE_MS));
    (void)burner_bacon_gba_reset_to_read_mode();
    vTaskDelay(pdMS_TO_TICKS(BURNER_GBA_BANK_SWITCH_SETTLE_MS));
    s_cart_ctx.current_bank = (uint16_t)bank;
    return ESP_OK;
}

static bool burner_gba_should_log_program_boundary(uint32_t byte_addr, size_t bytes, uint32_t processed, uint32_t total)
{
    uint32_t chunk_end;

    if (bytes == 0u) {
        return false;
    }
    if (processed == 0u || processed + (uint32_t)bytes >= total) {
        return true;
    }
    if ((byte_addr % BURN_GBA_BANK_BYTES) == 0u) {
        return true;
    }
    chunk_end = byte_addr + (uint32_t)bytes;
    return (chunk_end % BURN_GBA_BANK_BYTES) == 0u;
}

const char *burner_gba_cmd_addr_mode_name(burner_gba_cmd_addr_mode_t mode)
{
    switch (mode) {
    case BURNER_GBA_CMD_ADDR_BYTE_X16:
        return "byte-x16";
    case BURNER_GBA_CMD_ADDR_BYTE:
        return "byte";
    case BURNER_GBA_CMD_ADDR_WORD:
    default:
        return "word";
    }
}

const char *burner_gba_cmd_data_lane_name(burner_gba_cmd_data_lane_t lane)
{
    switch (lane) {
    case BURNER_GBA_CMD_DATA_HIGH:
        return "high";
    case BURNER_GBA_CMD_DATA_LOW:
    default:
        return "low";
    }
}

static uint32_t burner_gba_unlock_addr0(void)
{
    return BURNER_GBA_HOST_UNLOCK_ADDR0;
}

static uint32_t burner_gba_unlock_addr1(void)
{
    return BURNER_GBA_HOST_UNLOCK_ADDR1;
}

static uint32_t burner_gba_cfi_enter_addr(void)
{
    return BURNER_GBA_HOST_CFI_ENTER_ADDR;
}

static esp_err_t burner_gba_diag_read_word_raw(
    burner_spi_cs_mode_t mode,
    uint32_t word_addr,
    uint16_t *value_out,
    uint8_t *raw_out,
    size_t raw_len)
{
    size_t turnaround_hold_bytes;
    size_t seq_len;
    size_t data_base;
    uint8_t tx_sequence[16] = {0};
    uint8_t rx_sequence[16] = {0};
    esp_err_t err;

    if (value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    turnaround_hold_bytes = BURNER_GBA_READ_TURNAROUND_HOLD_BYTES;
    seq_len = 4u + 1u + (4u + turnaround_hold_bytes) + 1u;
    data_base = 5u + 1u + turnaround_hold_bytes;
    if (seq_len > sizeof(tx_sequence) || seq_len > sizeof(rx_sequence)) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_sequence[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    tx_sequence[1] = (uint8_t)(word_addr & 0xFFu);
    tx_sequence[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    tx_sequence[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    tx_sequence[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    tx_sequence[5] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    for (size_t hold = 0u; hold < turnaround_hold_bytes; ++hold) {
        tx_sequence[6u + hold] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    }
    tx_sequence[6u + turnaround_hold_bytes] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
    tx_sequence[7u + turnaround_hold_bytes] = 0x00u;
    tx_sequence[8u + turnaround_hold_bytes] = 0x00u;
    tx_sequence[seq_len - 1u] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_transfer_cs(mode, tx_sequence, rx_sequence, seq_len);
    if (err != ESP_OK) {
        return err;
    }

    *value_out = (uint16_t)((uint16_t)rx_sequence[data_base + 1u] |
                            ((uint16_t)rx_sequence[data_base + 2u] << 8));
    if (raw_out != NULL && raw_len > 0u) {
        if (raw_len > seq_len) {
            raw_len = seq_len;
        }
        memcpy(raw_out, rx_sequence, raw_len);
    }
    return ESP_OK;
}

static size_t burner_gba_diag_raw_seq_len(void)
{
    return 4u + 1u + (4u + BURNER_GBA_READ_TURNAROUND_HOLD_BYTES) + 1u;
}

static esp_err_t burner_gba_diag_read_word_segmented(
    burner_spi_cs_mode_t mode,
    uint32_t word_addr,
    uint16_t *value_out,
    uint8_t raw_read[3])
{
    uint8_t setup[5];
    uint8_t rd_low;
    uint8_t tx_read[3];
    uint8_t rx_read[3] = {0};
    uint8_t release;
    bool session_open = false;
    bool release_sent = false;
    esp_err_t err;

    if (value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    setup[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    setup[1] = (uint8_t)(word_addr & 0xFFu);
    setup[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    setup[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    setup[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    rd_low = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    tx_read[0] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
    tx_read[1] = 0x00u;
    tx_read[2] = 0x00u;
    release = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_begin_cs(mode);
    if (err != ESP_OK) {
        return err;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto segmented_out;
    }

    err = burner_spi_transfer_active(&rd_low, NULL, 1u);
    if (err != ESP_OK) {
        goto segmented_out;
    }
    if (BURNER_GBA_ROM_PHASE_GAP_US > 0u) {
        esp_rom_delay_us(BURNER_GBA_ROM_PHASE_GAP_US);
    }

    err = burner_spi_transfer_active(tx_read, rx_read, sizeof(tx_read));
    if (err != ESP_OK) {
        goto segmented_out;
    }

    err = burner_spi_transfer_active(&release, NULL, 1u);
    if (err == ESP_OK) {
        release_sent = true;
    }

segmented_out:
    if (session_open) {
        if (!release_sent) {
            (void)burner_spi_transfer_active(&release, NULL, 1u);
        }
        burner_spi_end_cs(mode);
    }
    if (raw_read != NULL) {
        memcpy(raw_read, rx_read, sizeof(rx_read));
    }
    *value_out = (uint16_t)((uint16_t)rx_read[1] | ((uint16_t)rx_read[2] << 8));
    return err;
}

void burner_format_hex_bytes(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t i;
    size_t pos = 0u;

    if (out == NULL || out_len == 0u) {
        return;
    }

    out[0] = '\0';
    if (data == NULL) {
        return;
    }

    for (i = 0u; i < len && pos + 1u < out_len; ++i) {
        int written = snprintf(out + pos, out_len - pos, (i + 1u < len) ? "%02X " : "%02X", data[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= (out_len - pos)) {
            pos = out_len - 1u;
            break;
        }
        pos += (size_t)written;
    }

    out[out_len - 1u] = '\0';
}

static void burner_gba_diag_capture_cs_levels(burner_spi_cs_mode_t mode, int *cs0_level, int *cs1_level)
{
#if BURNER_SPI_ENABLE
    uint32_t cs_setup_delay_us = burner_spi_cs_setup_delay_us(mode);

    if (cs0_level != NULL) {
        *cs0_level = -1;
    }
    if (cs1_level != NULL) {
        *cs1_level = -1;
    }

    burner_spi_apply_cs_mode(mode);
    if (cs_setup_delay_us > 0u) {
        esp_rom_delay_us(cs_setup_delay_us);
    }
    if (cs0_level != NULL) {
        *cs0_level = gpio_get_level(MORI_PIN_MCU_SPI_CS);
    }
    if (cs1_level != NULL) {
        *cs1_level = gpio_get_level(MORI_PIN_MCU_SPI_CS1);
    }
    burner_spi_release_cs();
#else
    (void)mode;
    if (cs0_level != NULL) {
        *cs0_level = -1;
    }
    if (cs1_level != NULL) {
        *cs1_level = -1;
    }
#endif
}

static void burner_log_gba_diag_compare_word(const char *phase, uint32_t word_addr)
{
    uint8_t raw_mode0[16] = {0};
    uint8_t raw_mode2[16] = {0};
    uint8_t seg_mode0[3] = {0};
    uint8_t seg_mode2[3] = {0};
    char raw_mode0_hex[sizeof(raw_mode0) * 3u] = {0};
    char raw_mode2_hex[sizeof(raw_mode2) * 3u] = {0};
    char seg_mode0_hex[sizeof(seg_mode0) * 3u] = {0};
    char seg_mode2_hex[sizeof(seg_mode2) * 3u] = {0};
    size_t raw_len = burner_gba_diag_raw_seq_len();
    uint16_t value_mode0 = 0;
    uint16_t value_mode2 = 0;
    uint16_t seg_value_mode0 = 0;
    uint16_t seg_value_mode2 = 0;
    uint16_t seg_alt01_mode0 = 0;
    uint16_t seg_alt01_mode2 = 0;
    int cs0_mode0 = -1;
    int cs1_mode0 = -1;
    int cs0_mode2 = -1;
    int cs1_mode2 = -1;
    esp_err_t err_mode0;
    esp_err_t err_mode2;
    esp_err_t seg_err_mode0;
    esp_err_t seg_err_mode2;

    if (phase == NULL) {
        phase = "raw";
    }
    if (raw_len > sizeof(raw_mode0) || raw_len > sizeof(raw_mode2)) {
        ESP_LOGW(BURNER_TAG, "GBA raw %s diag buffer too small", phase);
        return;
    }

    burner_gba_diag_capture_cs_levels(BURNER_SPI_CS_MODE_0, &cs0_mode0, &cs1_mode0);
    burner_gba_diag_capture_cs_levels(BURNER_SPI_CS_MODE_2, &cs0_mode2, &cs1_mode2);

    err_mode0 = burner_gba_diag_read_word_raw(
        BURNER_SPI_CS_MODE_0, word_addr, &value_mode0, raw_mode0, raw_len);
    err_mode2 = burner_gba_diag_read_word_raw(
        BURNER_SPI_CS_MODE_2, word_addr, &value_mode2, raw_mode2, raw_len);
    seg_err_mode0 = burner_gba_diag_read_word_segmented(
        BURNER_SPI_CS_MODE_0, word_addr, &seg_value_mode0, seg_mode0);
    seg_err_mode2 = burner_gba_diag_read_word_segmented(
        BURNER_SPI_CS_MODE_2, word_addr, &seg_value_mode2, seg_mode2);
    if (err_mode0 != ESP_OK || err_mode2 != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA raw %s @%03" PRIX32 " failed: mode0=%s mode2=%s",
            phase,
            word_addr,
            esp_err_to_name(err_mode0),
            esp_err_to_name(err_mode2));
        return;
    }

    burner_format_hex_bytes(raw_mode0, raw_len, raw_mode0_hex, sizeof(raw_mode0_hex));
    burner_format_hex_bytes(raw_mode2, raw_len, raw_mode2_hex, sizeof(raw_mode2_hex));
    burner_format_hex_bytes(seg_mode0, sizeof(seg_mode0), seg_mode0_hex, sizeof(seg_mode0_hex));
    burner_format_hex_bytes(seg_mode2, sizeof(seg_mode2), seg_mode2_hex, sizeof(seg_mode2_hex));
    seg_alt01_mode0 = (uint16_t)((uint16_t)seg_mode0[0] | ((uint16_t)seg_mode0[1] << 8));
    seg_alt01_mode2 = (uint16_t)((uint16_t)seg_mode2[0] | ((uint16_t)seg_mode2[1] << 8));
    ESP_LOGI(
        BURNER_TAG,
        "GBA raw %s @%03" PRIX32 ": mode0 cs0=%d cs1=%d val=%04X rx=%s | mode2 cs0=%d cs1=%d val=%04X rx=%s",
        phase,
        word_addr,
        cs0_mode0,
        cs1_mode0,
        value_mode0,
        raw_mode0_hex,
        cs0_mode2,
        cs1_mode2,
        value_mode2,
        raw_mode2_hex);
    if (seg_err_mode0 == ESP_OK && seg_err_mode2 == ESP_OK) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA seg %s @%03" PRIX32 ": mode0 val12=%04X val01=%04X rx=%s | mode2 val12=%04X val01=%04X rx=%s",
            phase,
            word_addr,
            seg_value_mode0,
            seg_alt01_mode0,
            seg_mode0_hex,
            seg_value_mode2,
            seg_alt01_mode2,
            seg_mode2_hex);
    } else {
        ESP_LOGW(
            BURNER_TAG,
            "GBA seg %s @%03" PRIX32 " failed: mode0=%s mode2=%s",
            phase,
            word_addr,
            esp_err_to_name(seg_err_mode0),
            esp_err_to_name(seg_err_mode2));
    }

    if (cs0_mode0 != 0 || cs1_mode0 != 1) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA raw %s @%03" PRIX32 " mode0 CS readback unexpected: cs0=%d cs1=%d",
            phase,
            word_addr,
            cs0_mode0,
            cs1_mode0);
    }
    if (memcmp(raw_mode0, raw_mode2, raw_len) == 0) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA raw %s @%03" PRIX32 " mode0 and mode2 returned identical SPI bytes",
            phase,
            word_addr);
    }
}

/*
 * Some repro GBA flash carts swap D0/D1 between the cart edge and the flash
 * chip. Commands and chip-generated data (ID/CFI/PPB) must be translated, but
 * normal ROM payload bytes are left as the cart-edge logical data.
 */
#define SWAP_D0D1_U8(data) (((data) & 0xFCU) | (((data) & 0x01U) << 1) | (((data) & 0x02U) >> 1))
#define SWAP_D0D1_U16(data) ((SWAP_D0D1_U8((data) & 0xFFU)) | ((uint16_t)SWAP_D0D1_U8(((data) >> 8) & 0xFFU) << 8))

static inline uint16_t burner_apply_d0d1_swap_on_read(uint16_t data, bool is_swapped)
{
    return is_swapped ? SWAP_D0D1_U16(data) : data;
}

static inline uint16_t burner_apply_d0d1_swap_on_write(uint16_t data, bool is_swapped)
{
    return is_swapped ? SWAP_D0D1_U16(data) : data;
}

static esp_err_t burner_bacon_gba_command_write_u16(uint32_t word_addr, uint16_t value)
{
    return burner_bacon_rom_write_u16(
        word_addr,
        burner_apply_d0d1_swap_on_write(value, s_cart_ctx.d0d1_swapped));
}

static void burner_readid_trace_begin(const char *name, int64_t *start_us, int64_t *last_us)
{
    int64_t now_us = esp_timer_get_time();

    if (start_us != NULL) {
        *start_us = now_us;
    }
    if (last_us != NULL) {
        *last_us = now_us;
    }
    ESP_LOGI(BURNER_TAG, "%s begin t=%" PRId64 "us", name, now_us);
}

static void burner_readid_trace_log_u16(
    const char *name,
    const char *op,
    uint32_t addr,
    uint16_t data,
    int64_t *last_us)
{
    int64_t now_us = esp_timer_get_time();
    int64_t dt_us = 0;
    int64_t dt_sec = 0;
    int64_t dt_sub_us = 0;

    if (last_us != NULL) {
        dt_us = now_us - *last_us;
        *last_us = now_us;
    }
    dt_sec = dt_us / 1000000;
    dt_sub_us = dt_us % 1000000;
    if (dt_sub_us < 0) {
        dt_sub_us = -dt_sub_us;
    }

    ESP_LOGI(
        BURNER_TAG,
        "%s %s addr=0x%03" PRIX32 " data=0x%04X dt=%" PRId64 "us (%" PRId64 ".%06" PRId64 "s)",
        name,
        op,
        addr,
        data,
        dt_us,
        dt_sec,
        dt_sub_us);
}

static void burner_readid_trace_log_u8(
    const char *name,
    const char *op,
    uint32_t addr,
    uint8_t data,
    int64_t *last_us)
{
    int64_t now_us = esp_timer_get_time();
    int64_t dt_us = 0;
    int64_t dt_sec = 0;
    int64_t dt_sub_us = 0;

    if (last_us != NULL) {
        dt_us = now_us - *last_us;
        *last_us = now_us;
    }
    dt_sec = dt_us / 1000000;
    dt_sub_us = dt_us % 1000000;
    if (dt_sub_us < 0) {
        dt_sub_us = -dt_sub_us;
    }

    ESP_LOGI(
        BURNER_TAG,
        "%s %s addr=0x%04" PRIX32 " data=0x%02X dt=%" PRId64 "us (%" PRId64 ".%06" PRId64 "s)",
        name,
        op,
        addr,
        data,
        dt_us,
        dt_sec,
        dt_sub_us);
}

static void burner_readid_trace_end(const char *name, int64_t start_us, esp_err_t err)
{
    int64_t total_us = esp_timer_get_time() - start_us;
    int64_t total_sec = total_us / 1000000;
    int64_t total_sub_us = total_us % 1000000;

    if (total_sub_us < 0) {
        total_sub_us = -total_sub_us;
    }

    ESP_LOGI(
        BURNER_TAG,
        "%s end status=%s total=%" PRId64 "us (%" PRId64 ".%06" PRId64 "s)",
        name,
        esp_err_to_name(err),
        total_us,
        total_sec,
        total_sub_us);
}

static void burner_log_gba_id_window(void)
{
    static const uint32_t probe_words[] = {0x000u, 0x001u, 0x002u, 0x003u, 0x00Eu, 0x00Fu, 0x010u, 0x011u};
    uint16_t values[sizeof(probe_words) / sizeof(probe_words[0])] = {0};
    esp_err_t err = ESP_OK;
    bool entered_id = false;
    size_t i;

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug ID window enter step1 failed: %s", esp_err_to_name(err));
        return;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug ID window enter step2 failed: %s", esp_err_to_name(err));
        goto id_window_out;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0090u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug ID window enter step3 failed: %s", esp_err_to_name(err));
        goto id_window_out;
    }
    entered_id = true;

    for (i = 0u; i < (sizeof(probe_words) / sizeof(probe_words[0])); ++i) {
        err = burner_bacon_rom_read_u16(probe_words[i], &values[i]);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA debug ID window read failed at 0x%03" PRIX32 ": %s",
                probe_words[i],
                esp_err_to_name(err));
            goto id_window_out;
        }
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA ID window: [000]=%04X [001]=%04X [002]=%04X [003]=%04X [00E]=%04X [00F]=%04X [010]=%04X [011]=%04X",
        values[0],
        values[1],
        values[2],
        values[3],
        values[4],
        values[5],
        values[6],
        values[7]);

id_window_out:
    if (entered_id) {
        burner_log_gba_diag_compare_word("id", 0x000u);
    }
    (void)burner_bacon_gba_reset_to_read_mode();
}

static void burner_log_gba_cfi_window(void)
{
    static const uint32_t probe_words[] = {0x010u, 0x011u, 0x012u, 0x027u, 0x02Au, 0x02Du, 0x02Eu, 0x02Fu, 0x030u};
    uint16_t values[sizeof(probe_words) / sizeof(probe_words[0])] = {0};
    esp_err_t err = ESP_OK;
    bool entered_cfi = false;
    size_t i;

    err = burner_bacon_gba_command_write_u16(burner_gba_cfi_enter_addr(), 0x0098u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug CFI window enter failed: %s", esp_err_to_name(err));
        return;
    }
    entered_cfi = true;

    for (i = 0u; i < (sizeof(probe_words) / sizeof(probe_words[0])); ++i) {
        err = burner_bacon_rom_read_u16(probe_words[i], &values[i]);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA debug CFI window read failed at 0x%03" PRIX32 ": %s",
                probe_words[i],
                esp_err_to_name(err));
            goto cfi_window_out;
        }
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA CFI window: [010]=%04X [011]=%04X [012]=%04X [027]=%04X [02A]=%04X [02D]=%04X [02E]=%04X [02F]=%04X [030]=%04X",
        values[0],
        values[1],
        values[2],
        values[3],
        values[4],
        values[5],
        values[6],
        values[7],
        values[8]);

cfi_window_out:
    if (entered_cfi) {
        burner_log_gba_diag_compare_word("cfi", 0x010u);
    }
    (void)burner_bacon_gba_reset_to_read_mode();
}

static bool burner_gba_detect_qry_words(uint16_t q, uint16_t r, uint16_t y, bool *is_swapped_out, bool *high_byte_lane_out)
{
    for (uint8_t lane = 0u; lane < 2u; ++lane) {
        bool high_byte_lane = (lane != 0u);
        uint8_t q_byte = high_byte_lane ? (uint8_t)((q >> 8) & 0xFFu) : (uint8_t)(q & 0xFFu);
        uint8_t r_byte = high_byte_lane ? (uint8_t)((r >> 8) & 0xFFu) : (uint8_t)(r & 0xFFu);
        uint8_t y_byte = high_byte_lane ? (uint8_t)((y >> 8) & 0xFFu) : (uint8_t)(y & 0xFFu);
        uint8_t q_swap = SWAP_D0D1_U8(q_byte);
        uint8_t r_swap = SWAP_D0D1_U8(r_byte);
        uint8_t y_swap = SWAP_D0D1_U8(y_byte);

        if (q_byte == 0x51u && r_byte == 0x52u && y_byte == 0x59u) {
            *is_swapped_out = false;
            *high_byte_lane_out = high_byte_lane;
            return true;
        }
        if (q_swap == 0x51u && r_swap == 0x52u && y_swap == 0x59u) {
            *is_swapped_out = true;
            *high_byte_lane_out = high_byte_lane;
            return true;
        }
    }
    return false;
}

/*
 * Detect D0/D1 swap by reading the CFI "QRY" signature. Keep this aligned with
 * burner_bacon_gba_get_cfi(): GBA flash is probed in word-address mode, so the
 * canonical signature words are 0x010/0x011/0x012 after entering CFI at 0x055.
 */
static esp_err_t burner_gba_detect_d0d1_swap(
    bool *is_swapped_out,
    burner_gba_cmd_data_lane_t *lane_out)
{
    esp_err_t err;
    uint16_t q = 0;
    uint16_t r = 0;
    uint16_t y = 0;
    bool high_byte_lane = false;

    if (is_swapped_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(BURNER_TAG, "D0/D1 swap detection: entering CFI mode (word addr 0x%03" PRIX32 ")", burner_gba_cfi_enter_addr());

    err = burner_bacon_gba_command_write_u16(burner_gba_cfi_enter_addr(), 0x0098u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "D0/D1 swap detection: CFI entry write failed: %s", esp_err_to_name(err));
        goto reset_out;
    }

    err = burner_bacon_rom_read_u16(0x010u, &q);
    if (err != ESP_OK) {
        goto reset_out;
    }
    err = burner_bacon_rom_read_u16(0x011u, &r);
    if (err != ESP_OK) {
        goto reset_out;
    }
    err = burner_bacon_rom_read_u16(0x012u, &y);
    if (err != ESP_OK) {
        goto reset_out;
    }

    if (burner_gba_detect_qry_words(q, r, y, is_swapped_out, &high_byte_lane)) {
        if (lane_out != NULL) {
            *lane_out = high_byte_lane ? BURNER_GBA_CMD_DATA_HIGH : BURNER_GBA_CMD_DATA_LOW;
        }
        ESP_LOGI(
            BURNER_TAG,
            "D0/D1 swap detection: CFI 'QRY' detected (%s, %s-byte lane) [010]=%04X [011]=%04X [012]=%04X",
            *is_swapped_out ? "SWAPPED" : "normal",
            high_byte_lane ? "high" : "low",
            q,
            r,
            y);
        err = ESP_OK;
        goto reset_out;
    }

    ESP_LOGW(BURNER_TAG, "D0/D1 swap detection: CFI 'QRY' not detected [010]=%04X [011]=%04X [012]=%04X", q, r, y);
    err = ESP_ERR_NOT_FOUND;

reset_out:
    (void)burner_bacon_gba_reset_to_read_mode();
    return err;
}

static esp_err_t burner_bacon_gba_read_id_with_cmdset(
    uint8_t id_out[8],
    bool is_swapped,
    burner_nor_cmdset_t cmdset,
    const char *trace_name)
{
    esp_err_t err;
    uint16_t w0 = 0;
    uint16_t w1 = 0;
    uint16_t w2 = 0;
    uint16_t w3 = 0;
    int64_t trace_start_us = 0;
    int64_t trace_last_us = 0;
    const char *name = (trace_name != NULL) ? trace_name : "GBA ReadID trace";
    uint16_t reset_word = 0x00F0u;
    bool entered_id_mode = false;

    if (id_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_readid_trace_begin(name, &trace_start_us, &trace_last_us);

    if (cmdset == BURNER_NOR_CMDSET_INTEL) {
        reset_word = 0x00FFu;

        err = burner_bacon_gba_command_write_u16(0x000u, 0x0050u);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s write addr=0x000 data=0x0050 failed: %s", name, esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", 0x000u, 0x0050u, &trace_last_us);

        err = burner_bacon_gba_command_write_u16(0x000u, 0x00FFu);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s write addr=0x000 data=0x00FF failed: %s", name, esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", 0x000u, 0x00FFu, &trace_last_us);

        err = burner_bacon_gba_command_write_u16(0x000u, 0x0090u);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s write addr=0x000 data=0x0090 failed: %s", name, esp_err_to_name(err));
            goto reset_out;
        }
        burner_readid_trace_log_u16(name, "write", 0x000u, 0x0090u, &trace_last_us);
        entered_id_mode = true;
    } else {
        /*
         * Match Bacon host exactly:
         * 1. write  0x555 <- 0x00AA
         * 2. write  0x2AA <- 0x0055
         * 3. write  0x555 <- 0x0090
         * 4. read   word 0x000
         * 5. read   word 0x001
         * 6. read   word 0x00E
         * 7. read   word 0x00F
         * 8. write  0x000 <- 0x00F0
         */
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "%s write addr=0x%03" PRIX32 " data=0x00AA failed: %s",
                name,
                burner_gba_unlock_addr0(),
                esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", burner_gba_unlock_addr0(), 0x00AAu, &trace_last_us);
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "%s write addr=0x%03" PRIX32 " data=0x0055 failed: %s",
                name,
                burner_gba_unlock_addr1(),
                esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", burner_gba_unlock_addr1(), 0x0055u, &trace_last_us);
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0090u);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "%s write addr=0x%03" PRIX32 " data=0x0090 failed: %s",
                name,
                burner_gba_unlock_addr0(),
                esp_err_to_name(err));
            goto reset_out;
        }
        burner_readid_trace_log_u16(name, "write", burner_gba_unlock_addr0(), 0x0090u, &trace_last_us);
        entered_id_mode = true;
    }

    err = burner_bacon_rom_read_u16(0x000u, &w0);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x000 failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x000u, w0, &trace_last_us);
    err = burner_bacon_rom_read_u16(0x001u, &w1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x001 failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x001u, w1, &trace_last_us);
    err = burner_bacon_rom_read_u16(0x00Eu, &w2);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x00E failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x00Eu, w2, &trace_last_us);
    err = burner_bacon_rom_read_u16(0x00Fu, &w3);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x00F failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x00Fu, w3, &trace_last_us);

    /* Apply D0/D1 swap to read data */
    w0 = burner_apply_d0d1_swap_on_read(w0, is_swapped);
    w1 = burner_apply_d0d1_swap_on_read(w1, is_swapped);
    w2 = burner_apply_d0d1_swap_on_read(w2, is_swapped);
    w3 = burner_apply_d0d1_swap_on_read(w3, is_swapped);

    id_out[0] = (uint8_t)(w0 & 0xFFu);
    id_out[1] = (uint8_t)((w0 >> 8) & 0xFFu);
    id_out[2] = (uint8_t)(w1 & 0xFFu);
    id_out[3] = (uint8_t)((w1 >> 8) & 0xFFu);
    id_out[4] = (uint8_t)(w2 & 0xFFu);
    id_out[5] = (uint8_t)((w2 >> 8) & 0xFFu);
    id_out[6] = (uint8_t)(w3 & 0xFFu);
    id_out[7] = (uint8_t)((w3 >> 8) & 0xFFu);

reset_out:
    {
        esp_err_t reset_err = ESP_OK;

        if (entered_id_mode) {
            reset_err = burner_bacon_gba_reset_to_read_mode_for_cmdset(cmdset);
        }
        if (entered_id_mode && reset_err == ESP_OK) {
            if (cmdset == BURNER_NOR_CMDSET_INTEL) {
                burner_readid_trace_log_u16(name, "write", 0x000u, 0x0050u, &trace_last_us);
            }
            burner_readid_trace_log_u16(name, "write", 0x000u, reset_word, &trace_last_us);
        } else if (entered_id_mode && reset_err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s reset failed: %s", name, esp_err_to_name(reset_err));
        }
    }
    burner_readid_trace_end(name, trace_start_us, err);
    return err;
}

static esp_err_t burner_bacon_gba_read_id(uint8_t id_out[8], bool is_swapped)
{
    burner_nor_cmdset_t cmdset = s_cart_ctx.gba_cmdset;

    if (cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
        cmdset = BURNER_NOR_CMDSET_AMD;
    }
    return burner_bacon_gba_read_id_with_cmdset(id_out, is_swapped, cmdset, "GBA ReadID trace");
}

static esp_err_t burner_bacon_gba_cfi_read_u8(
    uint32_t word_addr,
    bool high_byte_lane,
    uint8_t *out)
{
    uint16_t word = 0u;
    esp_err_t err;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_rom_read_u16(word_addr, &word);
    if (err != ESP_OK) {
        return err;
    }
    word = burner_apply_d0d1_swap_on_read(word, s_cart_ctx.d0d1_swapped);
    *out = high_byte_lane ? (uint8_t)((word >> 8) & 0xFFu) : (uint8_t)(word & 0xFFu);
    return ESP_OK;
}

static esp_err_t burner_bacon_gba_get_cfi(
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry,
    burner_nor_cmdset_t *cmdset_out,
    uint16_t *primary_cmdset_id_out)
{
    esp_err_t err;
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    uint16_t primary_cmdset_id = 0u;
    uint16_t w10 = 0;
    uint16_t w11 = 0;
    uint16_t w12 = 0;
    uint8_t cmdset_lo = 0u;
    uint8_t cmdset_hi = 0u;
    uint8_t cfi27 = 0;
    uint8_t cfi2a = 0;
    uint8_t cfi2c = 0;
    bool high_byte_lane = false;
    uint32_t enter_addrs[2];
    uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    size_t enter_idx = 0u;
    bool reverse_sector_region = false;
    uint32_t region_count = 0u;

    if (device_size == NULL || sector_size == NULL || buffer_write_bytes == NULL || geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }
    if (primary_cmdset_id_out != NULL) {
        *primary_cmdset_id_out = 0u;
    }
    burner_nor_geometry_clear(geometry);

    enter_addrs[0] = burner_gba_cfi_enter_addr();
    enter_addrs[1] = 0x000u;

    for (enter_idx = 0u; enter_idx < (sizeof(enter_addrs) / sizeof(enter_addrs[0])); ++enter_idx) {
        err = burner_bacon_gba_reset_to_read_mode();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(enter_addrs[enter_idx], 0x0098u);
        if (err != ESP_OK) {
            return err;
        }

        err = burner_bacon_rom_read_u16(0x010u, &w10);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_rom_read_u16(0x011u, &w11);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_rom_read_u16(0x012u, &w12);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        if (burner_gba_detect_qry_words(
                burner_apply_d0d1_swap_on_read(w10, s_cart_ctx.d0d1_swapped),
                burner_apply_d0d1_swap_on_read(w11, s_cart_ctx.d0d1_swapped),
                burner_apply_d0d1_swap_on_read(w12, s_cart_ctx.d0d1_swapped),
                &(bool){false},
                &high_byte_lane)) {
            break;
        }
    }

    if (enter_idx >= (sizeof(enter_addrs) / sizeof(enter_addrs[0]))) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA CFI signature mismatch: [010]=%04X [011]=%04X [012]=%04X",
            w10,
            w11,
            w12);
        err = ESP_FAIL;
        goto cfi_reset;
    }

    err = burner_bacon_gba_cfi_read_u8(0x013u, high_byte_lane, &cmdset_lo);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x014u, high_byte_lane, &cmdset_hi);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    primary_cmdset_id = (uint16_t)(((uint16_t)cmdset_hi << 8) | (uint16_t)cmdset_lo);
    cfi_cmdset = burner_nor_cmdset_from_cfi_primary_id(primary_cmdset_id);
    if (cmdset_out != NULL) {
        *cmdset_out = cfi_cmdset;
    }
    if (primary_cmdset_id_out != NULL) {
        *primary_cmdset_id_out = primary_cmdset_id;
    }

    err = burner_bacon_gba_cfi_read_u8(0x027u, high_byte_lane, &cfi27);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x02Au, high_byte_lane, &cfi2a);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x02Cu, high_byte_lane, &cfi2c);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    s_cart_ctx.gba_cmd_data_lane = high_byte_lane ? BURNER_GBA_CMD_DATA_HIGH : BURNER_GBA_CMD_DATA_LOW;

    if (cfi27 >= 31u) {
        err = ESP_FAIL;
        goto cfi_reset;
    }
    *device_size = (1u << cfi27);

    if (cfi2a == 0u) {
        *buffer_write_bytes = 0u;
    } else {
        if (cfi2a >= 16u) {
            err = ESP_FAIL;
            goto cfi_reset;
        }
        *buffer_write_bytes = (uint16_t)(1u << cfi2a);
    }

    region_count = (uint32_t)cfi2c;
    if (region_count == 0u || region_count > BURNER_NOR_GEOMETRY_REGION_MAX) {
        err = ESP_ERR_INVALID_SIZE;
        goto cfi_reset;
    }

    for (uint32_t i = 0u; i < region_count; ++i) {
        uint8_t count_lo = 0u;
        uint8_t count_hi = 0u;
        uint8_t size_lo = 0u;
        uint8_t size_hi = 0u;
        uint32_t base = 0x02Du + (i * 4u);

        err = burner_bacon_gba_cfi_read_u8(base + 0u, high_byte_lane, &count_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 1u, high_byte_lane, &count_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 2u, high_byte_lane, &size_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 3u, high_byte_lane, &size_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }

        sector_counts[i] = (((uint32_t)count_hi << 8) | (uint32_t)count_lo) + 1u;
        sector_sizes[i] = ((((uint32_t)size_hi << 8) | (uint32_t)size_lo) * 256u);
        if (sector_counts[i] == 0u || sector_sizes[i] == 0u) {
            err = ESP_ERR_INVALID_SIZE;
            goto cfi_reset;
        }
    }

    {
        uint8_t pri_lo = 0u;
        uint8_t pri_hi = 0u;
        uint32_t pri_word_addr = 0u;

        err = burner_bacon_gba_cfi_read_u8(0x015u, high_byte_lane, &pri_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(0x016u, high_byte_lane, &pri_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }

        pri_word_addr = ((uint32_t)pri_hi << 8) | (uint32_t)pri_lo;
        if (pri_word_addr + 0x1Eu >= 0x200u) {
            pri_word_addr = 0x040u;
        }

        if (pri_word_addr + 0x1Eu < 0x200u) {
            uint8_t pri_p = 0u;
            uint8_t pri_r = 0u;
            uint8_t pri_i = 0u;

            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 0u, high_byte_lane, &pri_p);
            if (err != ESP_OK) {
                goto cfi_reset;
            }
            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 1u, high_byte_lane, &pri_r);
            if (err != ESP_OK) {
                goto cfi_reset;
            }
            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 2u, high_byte_lane, &pri_i);
            if (err != ESP_OK) {
                goto cfi_reset;
            }

            if (pri_p == 'P' && pri_r == 'R' && pri_i == 'I') {
                uint8_t tb_boot_sector_raw = 0u;

                err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 0x1Eu, high_byte_lane, &tb_boot_sector_raw);
                if (err != ESP_OK) {
                    goto cfi_reset;
                }
                reverse_sector_region = (tb_boot_sector_raw == 0x03u);
            }
        }
    }

    err = burner_nor_geometry_build(
        geometry,
        *device_size,
        sector_counts,
        sector_sizes,
        region_count,
        reverse_sector_region);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    *sector_size = burner_nor_geometry_report_sector_size(geometry);
    err = ESP_OK;

cfi_reset:
    (void)burner_bacon_gba_reset_to_read_mode();
    return err;
}

static bool burner_bacon_gba_is_s70gl02(const uint8_t id[8])
{
    return burner_gba_nor_has_flag(id, BURNER_NOR_FLAG_DUAL_DIE);
}

static void burner_gba_select_probe_id(
    uint8_t id_out[8],
    const uint8_t amd_id[8],
    bool amd_id_valid,
    const uint8_t intel_id[8],
    bool intel_id_valid,
    burner_nor_cmdset_t cmdset)
{
    if (id_out == NULL) {
        return;
    }
    if (cmdset == BURNER_NOR_CMDSET_INTEL && intel_id_valid) {
        memcpy(id_out, intel_id, 8u);
        return;
    }
    if (amd_id_valid) {
        memcpy(id_out, amd_id, 8u);
        return;
    }
    if (intel_id_valid) {
        memcpy(id_out, intel_id, 8u);
        return;
    }
    memset(id_out, 0, 8u);
}

static bool burner_gba_probe_load_entry_geometry(
    const burner_nor_entry_t *entry,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry)
{
    uint32_t entry_device_size;
    uint32_t entry_sector_size;
    uint16_t entry_buffer_write_bytes;

    if (entry == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || geometry == NULL) {
        return false;
    }

    entry_device_size = burner_nor_entry_device_size(entry);
    entry_sector_size = burner_nor_entry_sector_size(entry);
    entry_buffer_write_bytes = burner_nor_entry_buffer_write_bytes(entry);
    if (entry_device_size == 0u || entry_sector_size == 0u) {
        return false;
    }

    *device_size = entry_device_size;
    *sector_size = entry_sector_size;
    *buffer_write_bytes = entry_buffer_write_bytes;
    if (burner_nor_geometry_set_uniform(geometry, entry_device_size, entry_sector_size) != ESP_OK) {
        burner_nor_geometry_clear(geometry);
        return false;
    }
    return true;
}

static bool burner_mbc5_id_is_mx29lv640eb(const uint8_t id[4])
{
    return id != NULL && id[0] == 0xC2u && id[1] == 0xCBu;
}

static bool burner_mbc5_id_is_mx29lv640et(const uint8_t id[4])
{
    return id != NULL && id[0] == 0xC2u && id[1] == 0xC9u;
}

static bool burner_mbc5_probe_load_entry_geometry(
    const uint8_t id[4],
    const burner_nor_entry_t *entry,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry)
{
    uint32_t entry_device_size = 0u;
    uint32_t entry_sector_size = 0u;
    uint16_t entry_buffer_write_bytes = 0u;
    bool entry_ok;

    entry_ok = burner_gba_probe_load_entry_geometry(
        entry,
        &entry_device_size,
        &entry_sector_size,
        &entry_buffer_write_bytes,
        geometry);
    if (!entry_ok || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || geometry == NULL) {
        return false;
    }

    if ((burner_mbc5_id_is_mx29lv640eb(id) || burner_mbc5_id_is_mx29lv640et(id)) &&
        entry_device_size == (8u * 1024u * 1024u) &&
        entry_sector_size == (64u * 1024u)) {
        uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
        uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};

        if (burner_mbc5_id_is_mx29lv640eb(id)) {
            sector_counts[0] = 8u;
            sector_sizes[0] = 8u * 1024u;
            sector_counts[1] = 127u;
            sector_sizes[1] = 64u * 1024u;
        } else {
            sector_counts[0] = 127u;
            sector_sizes[0] = 64u * 1024u;
            sector_counts[1] = 8u;
            sector_sizes[1] = 8u * 1024u;
        }
        if (burner_nor_geometry_build(
                geometry,
                entry_device_size,
                sector_counts,
                sector_sizes,
                2u,
                false) != ESP_OK) {
            return false;
        }
    }

    *device_size = entry_device_size;
    *sector_size = burner_nor_geometry_report_sector_size(geometry);
    *buffer_write_bytes = entry_buffer_write_bytes;
    return true;
}

static bool burner_mbc5_geometry_should_prefer_id(
    const burner_nor_geometry_t *cfi_geometry,
    uint32_t cfi_device_size,
    uint32_t cfi_sector_size,
    const burner_nor_geometry_t *id_geometry,
    uint32_t id_device_size,
    uint32_t id_sector_size)
{
    uint32_t cfi_largest;
    uint32_t id_largest;

    if (!burner_nor_geometry_is_valid(id_geometry) ||
        id_device_size == 0u || id_sector_size == 0u) {
        return false;
    }
    if (!burner_nor_geometry_is_valid(cfi_geometry) ||
        cfi_device_size == 0u || cfi_sector_size == 0u) {
        return true;
    }
    if (cfi_device_size != id_device_size) {
        return false;
    }
    cfi_largest = burner_nor_geometry_largest_sector_size(cfi_geometry);
    id_largest = burner_nor_geometry_largest_sector_size(id_geometry);
    if (cfi_largest > 0u && id_largest > cfi_largest &&
        cfi_largest <= BURN_MBC5_ROM_BANK_BYTES) {
        return true;
    }
    if (!burner_nor_geometry_is_uniform(id_geometry) &&
        !burner_nor_geometry_equal(cfi_geometry, id_geometry)) {
        return true;
    }
    return false;
}

static void burner_gba_build_auto_profile_name(
    char *buf,
    size_t buf_len,
    const uint8_t id[8],
    burner_nor_cmdset_t cmdset)
{
    const char *known_profile = burner_gba_profile_name(id);

    if (buf == NULL || buf_len == 0u) {
        return;
    }
    if (known_profile != NULL && strcmp(known_profile, "unknown") != 0) {
        (void)snprintf(buf, buf_len, "%s", known_profile);
        return;
    }
    (void)snprintf(buf, buf_len, "AGB:%s:auto-cfi", burner_nor_cmdset_name(cmdset));
}

bool burner_gba_id_looks_like_rom_header(const uint8_t id[8])
{
    if (id == NULL) {
        return false;
    }
    /* 01 00 00 EA 08 00 .. .. commonly matches normal ROM vector data. */
    return id[0] == 0x01u &&
           id[1] == 0x00u &&
           id[2] == 0x00u &&
           id[3] == 0xEAu &&
           id[4] == 0x08u &&
           id[5] == 0x00u;
}

static bool burner_gba_id_matches_plain_rom_data(const uint8_t id[8])
{
    uint16_t w0 = 0;
    uint16_t w1 = 0;
    uint16_t w2 = 0;
    uint16_t w3 = 0;
    uint8_t plain[8];
    bool first4_match;

    if (id == NULL) {
        return false;
    }
    if (burner_bacon_rom_read_u16(0x000u, &w0) != ESP_OK ||
        burner_bacon_rom_read_u16(0x001u, &w1) != ESP_OK ||
        burner_bacon_rom_read_u16(0x002u, &w2) != ESP_OK ||
        burner_bacon_rom_read_u16(0x003u, &w3) != ESP_OK) {
        return false;
    }

    plain[0] = (uint8_t)(w0 & 0xFFu);
    plain[1] = (uint8_t)((w0 >> 8) & 0xFFu);
    plain[2] = (uint8_t)(w1 & 0xFFu);
    plain[3] = (uint8_t)((w1 >> 8) & 0xFFu);
    plain[4] = (uint8_t)(w2 & 0xFFu);
    plain[5] = (uint8_t)((w2 >> 8) & 0xFFu);
    plain[6] = (uint8_t)(w3 & 0xFFu);
    plain[7] = (uint8_t)((w3 >> 8) & 0xFFu);
    first4_match = (memcmp(id, plain, 4u) == 0);
    if (memcmp(id, plain, sizeof(plain)) == 0) {
        return true;
    }
    return first4_match;
}

static esp_err_t burner_bacon_gba_probe_after_power_locked(
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out)
{
    esp_err_t err;
    esp_err_t id_err;
    esp_err_t intel_err;
    bool id_looks_like_header = false;
    bool id_matches_plain_rom = false;
    char chip_name[48] = {0};
    char profile_name[48] = {0};
    uint32_t probe_hz = (s_mcu_spi_actual_hz > 0u) ? s_mcu_spi_actual_hz : s_mcu_spi_clock_hz;
    uint32_t attempt = 0u;
    burner_nor_geometry_t cfi_geometry = {0};
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    uint16_t cfi_primary_cmdset_id = 0u;
    uint8_t amd_id[8] = {0};
    uint8_t intel_id[8] = {0};
    uint8_t last_amd_id[8] = {0};
    uint8_t last_intel_id[8] = {0};
    bool amd_id_valid = false;
    bool intel_id_valid = false;
    bool last_amd_id_valid = false;
    bool last_intel_id_valid = false;
    const burner_nor_entry_t *amd_entry = NULL;
    const burner_nor_entry_t *intel_entry = NULL;
    const burner_nor_entry_t *known_entry = NULL;
    burner_nor_cmdset_t resolved_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Lock GBA probe to the legacy command lane/address mapping. */
    s_cart_ctx.gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD;
    s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_cart_ctx.d0d1_known = false;
    s_cart_ctx.d0d1_swapped = false; /* Default: no swap */
    burner_nor_geometry_clear(&s_cart_ctx.geometry);

    memset(id_out, 0, 8u);
    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;

    /* Detect D0/D1 swap before reading ID */
    ESP_LOGI(BURNER_TAG, "GBA D0/D1 swap detection starting...");
    err = burner_gba_detect_d0d1_swap(&s_cart_ctx.d0d1_swapped, &s_cart_ctx.gba_cmd_data_lane);
    if (err == ESP_OK) {
        s_cart_ctx.d0d1_known = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBA D0/D1 swap detection: %s, lane=%s",
            s_cart_ctx.d0d1_swapped ? "SWAPPED (D0<->D1)" : "NORMAL (no swap)",
            burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane));
    } else {
        ESP_LOGW(BURNER_TAG, "GBA D0/D1 swap detection failed, assuming normal (no swap)");
        s_cart_ctx.d0d1_known = false;
        s_cart_ctx.d0d1_swapped = false;
        s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    }

    for (attempt = 0u; attempt < BURNER_GBA_CFI_RETRY_COUNT; ++attempt) {
        uint32_t cfi_device_size = 0u;
        uint32_t cfi_sector_size = 0u;
        uint16_t cfi_buffer_write_bytes = 0u;

        memset(amd_id, 0, sizeof(amd_id));
        memset(intel_id, 0, sizeof(intel_id));
        amd_id_valid = false;
        intel_id_valid = false;
        amd_entry = NULL;
        intel_entry = NULL;
        known_entry = NULL;
        resolved_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
        cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
        cfi_primary_cmdset_id = 0u;
        burner_nor_geometry_clear(&cfi_geometry);

        if (attempt > 0u) {
            err = burner_bacon_gba_power_cycle_3v3_locked();
            if (err != ESP_OK) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA word-address probe retry %" PRIu32 " power-cycle failed @%" PRIu32 "Hz: %s",
                    attempt + 1u,
                    probe_hz,
                    esp_err_to_name(err));
                return err;
            }
            err = burner_bacon_gba_release_bus_idle();
            if (err != ESP_OK) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA word-address probe retry %" PRIu32 " release failed @%" PRIu32 "Hz: %s",
                    attempt + 1u,
                    probe_hz,
                    esp_err_to_name(err));
                return err;
            }
        }

        id_err = burner_bacon_gba_read_id_with_cmdset(
            amd_id,
            s_cart_ctx.d0d1_swapped,
            BURNER_NOR_CMDSET_AMD,
            "GBA ReadID trace");
        if (id_err == ESP_OK) {
            amd_id_valid = true;
            amd_entry = burner_nor_db_lookup_gba(amd_id);
            memcpy(last_amd_id, amd_id, sizeof(last_amd_id));
            last_amd_id_valid = true;
            memcpy(id_out, amd_id, sizeof(amd_id));
            id_looks_like_header = burner_gba_id_looks_like_rom_header(id_out);
            id_matches_plain_rom = burner_gba_id_matches_plain_rom_data(id_out);
            ESP_LOGI(
                BURNER_TAG,
                "GBA word-address probe @%" PRIu32 "Hz try=%" PRIu32
                " id=%02X %02X %02X %02X %02X %02X %02X %02X (%s)",
                probe_hz,
                attempt + 1u,
                amd_id[0],
                amd_id[1],
                amd_id[2],
                amd_id[3],
                amd_id[4],
                amd_id[5],
                amd_id[6],
                amd_id[7],
                id_looks_like_header ? "looks-like-rom-header" :
                (id_matches_plain_rom ? "looks-like-plain-rom-data" : "candidate-id"));
            if (amd_entry != NULL && burner_nor_entry_cmdset(amd_entry) == BURNER_NOR_CMDSET_AMD) {
                known_entry = amd_entry;
            }
        } else {
            ESP_LOGW(
                BURNER_TAG,
                "GBA word-address ID read failed (%s) @%" PRIu32 "Hz try=%" PRIu32 ": %s",
                burner_nor_cmdset_name(BURNER_NOR_CMDSET_AMD),
                probe_hz,
                attempt + 1u,
                esp_err_to_name(id_err));
        }

        intel_err = burner_bacon_gba_read_id_with_cmdset(
            intel_id,
            s_cart_ctx.d0d1_swapped,
            BURNER_NOR_CMDSET_INTEL,
            "GBA Intel ReadID trace");
        if (intel_err == ESP_OK) {
            intel_id_valid = true;
            intel_entry = burner_nor_db_lookup_gba(intel_id);
            memcpy(last_intel_id, intel_id, sizeof(last_intel_id));
            last_intel_id_valid = true;
            if (intel_entry != NULL && burner_nor_entry_cmdset(intel_entry) == BURNER_NOR_CMDSET_INTEL) {
                known_entry = intel_entry;
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA intel-id probe @%" PRIu32 "Hz try=%" PRIu32
                    " id=%02X %02X %02X %02X %02X %02X %02X %02X (candidate-id)",
                    probe_hz,
                    attempt + 1u,
                    intel_id[0],
                    intel_id[1],
                    intel_id[2],
                    intel_id[3],
                    intel_id[4],
                    intel_id[5],
                    intel_id[6],
                    intel_id[7]);
            }
        } else if (known_entry == NULL) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA intel-id read failed @%" PRIu32 "Hz try=%" PRIu32 ": %s",
                probe_hz,
                attempt + 1u,
                esp_err_to_name(intel_err));
        }

        err = burner_bacon_gba_get_cfi(
            &cfi_device_size,
            &cfi_sector_size,
            &cfi_buffer_write_bytes,
            &cfi_geometry,
            &cfi_cmdset,
            &cfi_primary_cmdset_id);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA CFI probe failed in word-address mode @%" PRIu32 "Hz try=%" PRIu32 ": %s",
                probe_hz,
                attempt + 1u,
                esp_err_to_name(err));
            if (known_entry != NULL &&
                burner_gba_probe_load_entry_geometry(
                    known_entry,
                    device_size,
                    sector_size,
                    buffer_write_bytes,
                    &s_cart_ctx.geometry)) {
                resolved_cmdset = burner_nor_entry_cmdset(known_entry);
                burner_gba_select_probe_id(
                    id_out,
                    amd_id,
                    amd_id_valid,
                    intel_id,
                    intel_id_valid,
                    resolved_cmdset);
                *cfi_ok_out = false;
                s_cart_ctx.gba_cmdset = resolved_cmdset;
                burner_nor_format_chip_name(
                    chip_name,
                    sizeof(chip_name),
                    burner_gba_chip_name(id_out),
                    resolved_cmdset,
                    *device_size);
                burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, resolved_cmdset);
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA probe fallback @%" PRIu32 "Hz try=%" PRIu32
                    ": chip=%s profile=%s flash=%" PRIu32 " sector=%" PRIu32
                    " buf=%u cmdset=%s source=library-no-cfi",
                    probe_hz,
                    attempt + 1u,
                    chip_name,
                    profile_name,
                    *device_size,
                    *sector_size,
                    (unsigned)*buffer_write_bytes,
                    burner_nor_cmdset_name(resolved_cmdset));
                return ESP_OK;
            }
            continue;
        }

        if (known_entry != NULL) {
            resolved_cmdset = burner_nor_entry_cmdset(known_entry);
            if (cfi_cmdset != BURNER_NOR_CMDSET_UNKNOWN && cfi_cmdset != resolved_cmdset) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA CFI cmdset mismatch @%" PRIu32 "Hz try=%" PRIu32
                    ": primary=0x%04X cfi=%s library=%s, using library",
                    probe_hz,
                    attempt + 1u,
                    cfi_primary_cmdset_id,
                    burner_nor_cmdset_name(cfi_cmdset),
                    burner_nor_cmdset_name(resolved_cmdset));
            }

            {
                uint32_t entry_device_size = 0u;
                uint32_t entry_sector_size = 0u;
                uint16_t entry_buffer_write_bytes = 0u;
                burner_nor_geometry_t entry_geometry = {0};

                if (burner_gba_probe_load_entry_geometry(
                        known_entry,
                        &entry_device_size,
                        &entry_sector_size,
                        &entry_buffer_write_bytes,
                        &entry_geometry)) {
                    if (cfi_device_size == 0u) {
                        cfi_device_size = entry_device_size;
                    }
                    if (cfi_sector_size == 0u) {
                        cfi_sector_size = entry_sector_size;
                    }
                    if (cfi_buffer_write_bytes == 0u) {
                        cfi_buffer_write_bytes = entry_buffer_write_bytes;
                    }
                    if (!burner_nor_geometry_is_valid(&cfi_geometry)) {
                        cfi_geometry = entry_geometry;
                    }
                }
            }
            burner_gba_select_probe_id(
                id_out,
                amd_id,
                amd_id_valid,
                intel_id,
                intel_id_valid,
                resolved_cmdset);

            burner_nor_format_chip_name(
                chip_name,
                sizeof(chip_name),
                burner_gba_chip_name(id_out),
                resolved_cmdset,
                cfi_device_size);
            burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, resolved_cmdset);
            ESP_LOGI(
                BURNER_TAG,
                "GBA profile match @%" PRIu32 "Hz try=%" PRIu32 ": chip=%s profile=%s cmdset=%s",
                probe_hz,
                attempt + 1u,
                chip_name,
                profile_name,
                burner_nor_cmdset_name(resolved_cmdset));
        } else if (cfi_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA CFI primary cmdset unsupported @%" PRIu32 "Hz try=%" PRIu32
                ": primary=0x%04X (no library match, defaulting to amd)",
                probe_hz,
                attempt + 1u,
                cfi_primary_cmdset_id);
            resolved_cmdset = BURNER_NOR_CMDSET_AMD;
            burner_gba_select_probe_id(
                id_out,
                amd_id,
                amd_id_valid,
                intel_id,
                intel_id_valid,
                resolved_cmdset);
        } else {
            resolved_cmdset = cfi_cmdset;
            burner_gba_select_probe_id(
                id_out,
                amd_id,
                amd_id_valid,
                intel_id,
                intel_id_valid,
                resolved_cmdset);
        }

        if (cfi_device_size == 0u || cfi_sector_size == 0u) {
            if (known_entry != NULL &&
                burner_gba_probe_load_entry_geometry(
                    known_entry,
                    &cfi_device_size,
                    &cfi_sector_size,
                    &cfi_buffer_write_bytes,
                    &cfi_geometry)) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA probe geometry repaired from library @%" PRIu32 "Hz try=%" PRIu32,
                    probe_hz,
                    attempt + 1u);
            } else {
                cfi_device_size = BURNER_GBA_FALLBACK_DEVICE_SIZE;
                cfi_sector_size = BURNER_GBA_FALLBACK_SECTOR_SIZE;
                cfi_buffer_write_bytes = BURNER_GBA_FALLBACK_BUFFER_WRITE_BYTES;
                (void)burner_nor_geometry_set_uniform(&cfi_geometry, cfi_device_size, cfi_sector_size);
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA probe geometry fallback @%" PRIu32 "Hz try=%" PRIu32
                    ": flash=%" PRIu32 " sector=%" PRIu32 " buf=%u",
                    probe_hz,
                    attempt + 1u,
                    cfi_device_size,
                    cfi_sector_size,
                    (unsigned)cfi_buffer_write_bytes);
            }
        }

        *device_size = cfi_device_size;
        *sector_size = cfi_sector_size;
        *buffer_write_bytes = cfi_buffer_write_bytes;
        *cfi_ok_out = true;
        s_cart_ctx.geometry = cfi_geometry;
        s_cart_ctx.gba_cmdset = resolved_cmdset;
        id_looks_like_header = burner_gba_id_looks_like_rom_header(id_out);
        id_matches_plain_rom = burner_gba_id_matches_plain_rom_data(id_out);

        ESP_LOGI(
            BURNER_TAG,
            "GBA auto probe @%" PRIu32 "Hz try=%" PRIu32
            " id=%02X %02X %02X %02X %02X %02X %02X %02X (%s)",
            probe_hz,
            attempt + 1u,
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3],
            id_out[4],
            id_out[5],
            id_out[6],
            id_out[7],
            id_looks_like_header ? "looks-like-rom-header" :
            (id_matches_plain_rom ? "looks-like-plain-rom-data" : "candidate-id"));
        burner_nor_format_chip_name(
            chip_name,
            sizeof(chip_name),
            burner_gba_chip_name(id_out),
            resolved_cmdset,
            *device_size);
        burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, resolved_cmdset);
        ESP_LOGI(
            BURNER_TAG,
            "GBA CFI ok in word-address mode @%" PRIu32 "Hz try=%" PRIu32
            ": chip=%s profile=%s flash=%" PRIu32 " sector=%" PRIu32 " geom=%s largest=%" PRIu32
            " regions=%u buf=%u cmdset=%s lane=%s primary=0x%04X",
            probe_hz,
            attempt + 1u,
            chip_name,
            profile_name,
            *device_size,
            *sector_size,
            burner_nor_geometry_is_uniform(&s_cart_ctx.geometry) ? "uniform" : "mixed",
            burner_nor_geometry_largest_sector_size(&s_cart_ctx.geometry),
            (unsigned)s_cart_ctx.geometry.region_count,
            (unsigned)*buffer_write_bytes,
            burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
            burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane),
            cfi_primary_cmdset_id);
        return ESP_OK;
    }

    burner_log_gba_id_window();
    burner_log_gba_cfi_window();
    burner_gba_select_probe_id(
        id_out,
        last_amd_id,
        last_amd_id_valid,
        last_intel_id,
        last_intel_id_valid,
        BURNER_NOR_CMDSET_AMD);
    *device_size = BURNER_GBA_FALLBACK_DEVICE_SIZE;
    *sector_size = BURNER_GBA_FALLBACK_SECTOR_SIZE;
    *buffer_write_bytes = BURNER_GBA_FALLBACK_BUFFER_WRITE_BYTES;
    *cfi_ok_out = false;
    (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_AMD;
    burner_nor_format_chip_name(
        chip_name,
        sizeof(chip_name),
        burner_gba_chip_name(id_out),
        s_cart_ctx.gba_cmdset,
        *device_size);
    burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, s_cart_ctx.gba_cmdset);
    ESP_LOGW(
        BURNER_TAG,
        "GBA probe fallback after retries: chip=%s profile=%s flash=%" PRIu32
        " sector=%" PRIu32 " buf=%u cmdset=%s source=default-amd",
        chip_name,
        profile_name,
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        burner_nor_cmdset_name(s_cart_ctx.gba_cmdset));
    return ESP_OK;
}

esp_err_t burner_bacon_gba_probe_locked(
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out)
{
    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return burner_bacon_gba_probe_after_power_locked(
        id_out,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok_out);
}

void burner_bacon_gba_d0d1_status(bool *known_out, bool *swapped_out)
{
    if (known_out != NULL) {
        *known_out = s_cart_ctx.d0d1_known;
    }
    if (swapped_out != NULL) {
        *swapped_out = s_cart_ctx.d0d1_swapped;
    }
}

static esp_err_t burner_bacon_gba_prepare(const burner_task_param_t *job)
{
    uint8_t id[8];
    char chip_name[48] = {0};
    char profile_name[48] = {0};
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    uint16_t program_buffer_write_bytes = 0;
    uint64_t requested_top64 = 0;
    bool cfi_ok = false;
    esp_err_t err;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    requested_top64 = (uint64_t)job->addr_begin + (uint64_t)job->total_bytes;
    if (requested_top64 == 0u || requested_top64 > ((uint64_t)UINT32_MAX + 1u)) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = burner_bacon_gba_probe_locked(
        id,
        &device_size,
        &sector_size,
        &buffer_write_bytes,
        &cfi_ok);
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "GBA probe failed: %s", esp_err_to_name(err));
        return err;
    }
    if (device_size == 0u || sector_size == 0u) {
        ESP_LOGE(BURNER_TAG, "GBA probe returned incomplete geometry");
        return ESP_ERR_INVALID_SIZE;
    }
    if (!cfi_ok) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA prepare continuing without CFI: flash=%" PRIu32 " sector=%" PRIu32
            " buf=%u cmdset=%s",
            device_size,
            sector_size,
            (unsigned)buffer_write_bytes,
            burner_nor_cmdset_name(s_cart_ctx.gba_cmdset));
    }
    if (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
        s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_AMD;
    }

    if (job->total_bytes > device_size) {
        ESP_LOGE(
            BURNER_TAG,
            "GBA ROM larger than flash: rom=%" PRIu32 " flash=%" PRIu32,
            job->total_bytes,
            device_size);
        return ESP_ERR_INVALID_SIZE;
    }

    s_cart_ctx.prepared = true;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_cart_ctx.buffer_write_bytes = buffer_write_bytes;
    s_cart_ctx.program_buffer_write_bytes =
        burner_gba_program_buffer_write_bytes(buffer_write_bytes, s_cart_ctx.gba_cmdset);
    s_cart_ctx.sector_size = sector_size;
    s_cart_ctx.device_size = device_size;
    program_buffer_write_bytes = s_cart_ctx.program_buffer_write_bytes;
    burner_nor_format_chip_name(
        chip_name,
        sizeof(chip_name),
        burner_gba_chip_name(id),
        s_cart_ctx.gba_cmdset,
        device_size);
    burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id, s_cart_ctx.gba_cmdset);

    if (program_buffer_write_bytes != buffer_write_bytes) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA program buffer cap: cmdset=%s probe_buf=%u actual_buf=%u",
            burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
            (unsigned)buffer_write_bytes,
            (unsigned)program_buffer_write_bytes);
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA prepared: chip=%s profile=%s flash=%" PRIu32 " sector=%" PRIu32
        " geom=%s largest=%" PRIu32 " regions=%u buf=%u prog_buf=%u cfi=%s nor=%s cmd=%s-address %s-lane id=%02X %02X %02X %02X %02X %02X %02X %02X",
        chip_name,
        profile_name,
        device_size,
        sector_size,
        burner_nor_geometry_is_uniform(&s_cart_ctx.geometry) ? "uniform" : "mixed",
        burner_nor_geometry_largest_sector_size(&s_cart_ctx.geometry),
        (unsigned)s_cart_ctx.geometry.region_count,
        (unsigned)buffer_write_bytes,
        (unsigned)program_buffer_write_bytes,
        cfi_ok ? "ok" : "unavailable",
        burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
        burner_gba_cmd_addr_mode_name(s_cart_ctx.gba_cmd_addr_mode),
        burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane),
        id[0],
        id[1],
        id[2],
        id[3],
        id[4],
        id[5],
        id[6],
        id[7]);

    ESP_LOGI(
        BURNER_TAG,
        "GBA mapping: %s bank=%uMB force_multi=%d range=0x%08" PRIX32 "-0x%08" PRIX32,
        burner_is_gba_multi_card(job) ? "32MB-bank multicart" : "linear single-card",
        (unsigned)(BURN_GBA_BANK_BYTES / (1024u * 1024u)),
        job->gba_force_multi ? 1 : 0,
        job->addr_begin,
        (uint32_t)(requested_top64 - 1u));

    burner_status_set_probe_info(
        BURNER_CART_MODE_GBA,
        id,
        8u,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok,
        burner_is_gba_multi_card(job),
        job->gba_force_multi,
        s_cart_ctx.d0d1_known,
        s_cart_ctx.d0d1_swapped,
        chip_name);

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_switch_bank(uint16_t bank)
{
    uint8_t b0 = (uint8_t)(bank & 0xFFu);
    uint8_t b1 = (uint8_t)((bank >> 8) & 0xFFu);
    esp_err_t err;

    if (s_gb_mapper_kind == BURNER_GB_MAPPER_MBC3) {
        b0 = burner_gb_mapper_normalize_rom_bank(s_gb_mapper_kind, bank);
        return burner_bacon_gbc_write(0x2000u, &b0, 1u);
    }

    /*
     * Match ChisFlashBurner mission_mbc5.cs:
     * write low 8 bits at 0x2000, then high bit at 0x3000.
     */
    err = burner_bacon_gbc_write(0x2000u, &b0, 1u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gbc_write(0x3000u, &b1, 1u);
}

static esp_err_t burner_bacon_mbc5_ram_switch_bank(uint8_t bank)
{
    if (s_gb_mapper_kind == BURNER_GB_MAPPER_MBC3) {
        bank &= 0x07u;
    }
    return burner_bacon_gbc_write(0x4000u, &bank, 1);
}

static esp_err_t burner_bacon_mbc5_ram_enable(bool enable)
{
    uint8_t cmd = enable ? 0x0Au : 0x00u;
    return burner_bacon_gbc_write(0x0000u, &cmd, 1);
}

static esp_err_t burner_bacon_wait_u8(uint16_t addr, uint8_t expected, uint32_t timeout_ms)
{
    int64_t deadline_us;
    uint8_t read_back = 0;
    esp_err_t err;

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(addr, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == expected) {
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    return ESP_ERR_TIMEOUT;
}

static uint32_t burner_erase_timeout_ms_for_bytes(uint32_t bytes)
{
    uint32_t mb;
    uint64_t timeout_ms;

    if (bytes == 0u) {
        return BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS;
    }

    mb = (bytes + ((1024u * 1024u) - 1u)) / (1024u * 1024u);
    if (mb == 0u) {
        mb = 1u;
    }
    timeout_ms = (uint64_t)mb * (uint64_t)BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS;
    if (timeout_ms > BURNER_ROM_ERASE_TIMEOUT_MAX_MS) {
        timeout_ms = BURNER_ROM_ERASE_TIMEOUT_MAX_MS;
    }
    return (uint32_t)timeout_ms;
}

static uint32_t burner_erase_remaining_timeout_ms(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();

    if (remaining_us <= 0) {
        return 0u;
    }
    return (uint32_t)((remaining_us + 999LL) / 1000LL);
}

static bool burner_buffer_all_equal(const uint8_t *left, const uint8_t *right, size_t len)
{
    return left != NULL && right != NULL && len > 0u && memcmp(left, right, len) == 0;
}

static esp_err_t burner_bacon_gb_detect_mapper(burner_gb_mapper_t *mapper_out)
{
    static const uint16_t sample_addrs[] = {
        0x4000u,
        0x4100u,
        0x4300u,
        0x47C0u,
    };
    uint8_t bank0_sample[sizeof(sample_addrs)] = {0};
    uint8_t bank1_sample[sizeof(sample_addrs)] = {0};
    uint8_t bank81_sample[sizeof(sample_addrs)] = {0};
    bool bank0_blank = false;
    bool bank1_blank = false;
    bool bank81_blank = false;
    bool bank0_eq_bank1;
    bool bank1_eq_bank81;
    esp_err_t err = ESP_OK;

    if (mapper_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *mapper_out = BURNER_GB_MAPPER_UNKNOWN;

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = 0u;
    for (size_t i = 0u; i < sizeof(sample_addrs) / sizeof(sample_addrs[0]); ++i) {
        err = burner_bacon_gbc_read_u8(sample_addrs[i], &bank0_sample[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = burner_bacon_mbc5_switch_bank(1u);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = 1u;
    for (size_t i = 0u; i < sizeof(sample_addrs) / sizeof(sample_addrs[0]); ++i) {
        err = burner_bacon_gbc_read_u8(sample_addrs[i], &bank1_sample[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = burner_bacon_mbc5_switch_bank(0x81u);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = 0x81u;
    for (size_t i = 0u; i < sizeof(sample_addrs) / sizeof(sample_addrs[0]); ++i) {
        err = burner_bacon_gbc_read_u8(sample_addrs[i], &bank81_sample[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = burner_buffer_all_ff(bank0_sample, sizeof(bank0_sample), &bank0_blank);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_buffer_all_ff(bank1_sample, sizeof(bank1_sample), &bank1_blank);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_buffer_all_ff(bank81_sample, sizeof(bank81_sample), &bank81_blank);
    if (err != ESP_OK) {
        return err;
    }

    bank0_eq_bank1 = burner_buffer_all_equal(bank0_sample, bank1_sample, sizeof(bank0_sample));
    bank1_eq_bank81 = burner_buffer_all_equal(bank1_sample, bank81_sample, sizeof(bank1_sample));
    if (!bank0_blank && !bank1_blank && bank0_eq_bank1) {
        *mapper_out = BURNER_GB_MAPPER_MBC3;
    } else if (!bank1_blank && !bank81_blank && bank1_eq_bank81) {
        *mapper_out = BURNER_GB_MAPPER_MBC3;
    } else {
        *mapper_out = BURNER_GB_MAPPER_MBC5;
    }

    ESP_LOGI(
        BURNER_TAG,
        "GB mapper detect: bank0=%02X-%02X-%02X-%02X bank1=%02X-%02X-%02X-%02X bank81=%02X-%02X-%02X-%02X blank=%d/%d/%d result=%s",
        bank0_sample[0],
        bank0_sample[1],
        bank0_sample[2],
        bank0_sample[3],
        bank1_sample[0],
        bank1_sample[1],
        bank1_sample[2],
        bank1_sample[3],
        bank81_sample[0],
        bank81_sample[1],
        bank81_sample[2],
        bank81_sample[3],
        bank0_blank ? 1 : 0,
        bank1_blank ? 1 : 0,
        bank81_blank ? 1 : 0,
        burner_gb_mapper_name(*mapper_out));
    return ESP_OK;
}

esp_err_t burner_bacon_mbc5_get_id(uint8_t id_out[4])
{
    uint8_t cmd;
    esp_err_t err;
    int64_t trace_start_us = 0;
    int64_t trace_last_us = 0;

    if (id_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_readid_trace_begin("MBC5 ReadID trace", &trace_start_us, &trace_last_us);

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0AAA data=0xAA failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0AAAu, 0xAAu, &trace_last_us);
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0555 data=0x55 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0555u, 0x55u, &trace_last_us);
    cmd = 0x90u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0AAA data=0x90 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0AAAu, 0x90u, &trace_last_us);

    err = burner_bacon_gbc_read_u8(0x0000u, &id_out[0]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x0000 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x0000u, id_out[0], &trace_last_us);
    err = burner_bacon_gbc_read_u8(0x0002u, &id_out[1]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x0002 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x0002u, id_out[1], &trace_last_us);
    err = burner_bacon_gbc_read_u8(0x001Cu, &id_out[2]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x001C failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x001Cu, id_out[2], &trace_last_us);
    err = burner_bacon_gbc_read_u8(0x001Eu, &id_out[3]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x001E failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x001Eu, id_out[3], &trace_last_us);

    cmd = 0xF0u;
    err = burner_bacon_gbc_write(0x0000u, &cmd, 1);
    if (err == ESP_OK) {
        burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0000u, 0xF0u, &trace_last_us);
    } else {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0000 data=0xF0 failed: %s", esp_err_to_name(err));
    }
    burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
    return err;
}

esp_err_t burner_bacon_mbc5_get_cfi(
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry,
    burner_nor_cmdset_t *cmdset_out)
{
    uint8_t cfi = 0;
    uint8_t hi = 0;
    uint8_t lo = 0;
    uint8_t cmd = 0x98u;
    uint8_t reset_cmd = 0xF0u;
    uint32_t tmp32;
    uint16_t tmp16;
    uint16_t primary_cmdset_id = 0u;
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    const uint16_t enter_addrs[] = {0x00AAu, 0x0000u};
    const uint8_t reset_cmds[] = {0xF0u, 0xFFu};
    uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    bool entered_cfi = false;
    bool cfi_matched = false;
    bool reverse_sector_region = false;
    uint32_t region_count = 0u;
    size_t enter_idx;
    esp_err_t err;

    if (device_size == NULL || sector_size == NULL || buffer_write_bytes == NULL || geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }
    burner_nor_geometry_clear(geometry);

    for (enter_idx = 0u; enter_idx < (sizeof(enter_addrs) / sizeof(enter_addrs[0])); ++enter_idx) {
        reset_cmd = reset_cmds[enter_idx];
        err = burner_bacon_gbc_write(enter_addrs[enter_idx], &cmd, 1);
        if (err != ESP_OK) {
            return err;
        }
        entered_cfi = true;

        err = burner_bacon_gbc_read_u8(0x0020u, &cfi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (cfi != 0x51u) {
            goto cfi_retry;
        }
        err = burner_bacon_gbc_read_u8(0x0022u, &cfi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (cfi != 0x52u) {
            goto cfi_retry;
        }
        err = burner_bacon_gbc_read_u8(0x0024u, &cfi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (cfi != 0x59u) {
            goto cfi_retry;
        }
        cfi_matched = true;
        break;

cfi_retry:
        if (entered_cfi) {
            esp_err_t reset_err = burner_bacon_gbc_write(0x0000u, &reset_cmd, 1);
            if (reset_err != ESP_OK) {
                return reset_err;
            }
            entered_cfi = false;
        }
    }

    if (!cfi_matched) {
        err = ESP_FAIL;
        goto cfi_out;
    }

    err = burner_bacon_gbc_read_u8(0x0026u, &lo);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    err = burner_bacon_gbc_read_u8(0x0028u, &hi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    primary_cmdset_id = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
    cfi_cmdset = burner_nor_cmdset_from_cfi_primary_id(primary_cmdset_id);
    if (cfi_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto cfi_out;
    }
    if (cmdset_out != NULL) {
        *cmdset_out = cfi_cmdset;
    }

    err = burner_bacon_gbc_read_u8(0x004Eu, &cfi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    if (cfi >= 31u) {
        err = ESP_FAIL;
        goto cfi_out;
    }
    *device_size = (1u << cfi);

    err = burner_bacon_gbc_read_u8(0x0056u, &hi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    err = burner_bacon_gbc_read_u8(0x0054u, &lo);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    tmp16 = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
    if (tmp16 == 0u) {
        *buffer_write_bytes = 0u;
    } else {
        /* Keep host-compatible behavior: exponent mainly comes from low byte. */
        if (hi != 0u) {
            ESP_LOGW(
                BURNER_TAG,
                "MBC5 CFI buffer size exponent high byte non-zero (0x%02X), using low byte 0x%02X",
                hi,
                lo);
        }
        if (lo >= 16u) {
            err = ESP_FAIL;
            goto cfi_out;
        }
        *buffer_write_bytes = (uint16_t)(1u << lo);
    }

    err = burner_bacon_gbc_read_u8(0x0058u, &cfi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    region_count = (uint32_t)cfi;
    if (region_count == 0u || region_count > BURNER_NOR_GEOMETRY_REGION_MAX) {
        err = ESP_ERR_INVALID_SIZE;
        goto cfi_out;
    }

    for (uint32_t i = 0u; i < region_count; ++i) {
        uint32_t base = 0x005Au + (i * 8u);

        err = burner_bacon_gbc_read_u8(base + 0u, &lo);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(base + 2u, &hi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        sector_counts[i] = (((uint32_t)hi << 8) | (uint32_t)lo) + 1u;

        err = burner_bacon_gbc_read_u8(base + 4u, &lo);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(base + 6u, &hi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        sector_sizes[i] = ((((uint32_t)hi << 8) | (uint32_t)lo) * 256u);
        if (sector_counts[i] == 0u || sector_sizes[i] == 0u) {
            err = ESP_ERR_INVALID_SIZE;
            goto cfi_out;
        }
    }

    err = burner_bacon_gbc_read_u8(0x002Au, &lo);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    err = burner_bacon_gbc_read_u8(0x002Cu, &hi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    tmp32 = ((((uint32_t)hi << 8) | (uint32_t)lo) * 2u);
    if (tmp32 + 0x3Cu >= 0x400u) {
        tmp32 = 0x80u;
    }
    if (tmp32 + 0x1Eu < 0x400u) {
        uint8_t pri_p = 0u;
        uint8_t pri_r = 0u;
        uint8_t pri_i = 0u;

        err = burner_bacon_gbc_read_u8(tmp32 + 0u, &pri_p);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(tmp32 + 2u, &pri_r);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(tmp32 + 4u, &pri_i);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (pri_p == 'P' && pri_r == 'R' && pri_i == 'I') {
            uint8_t tb_boot_sector_raw = 0u;

            err = burner_bacon_gbc_read_u8(tmp32 + 0x1Eu, &tb_boot_sector_raw);
            if (err != ESP_OK) {
                goto cfi_out;
            }
            reverse_sector_region = (tb_boot_sector_raw == 0x03u);
        }
    }

    err = burner_nor_geometry_build(
        geometry,
        *device_size,
        sector_counts,
        sector_sizes,
        region_count,
        reverse_sector_region);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    *sector_size = burner_nor_geometry_report_sector_size(geometry);

cfi_out:
    if (entered_cfi) {
        esp_err_t reset_err = burner_bacon_gbc_write(0x0000u, &reset_cmd, 1);
        if (err == ESP_OK && reset_err != ESP_OK) {
            err = reset_err;
        }
    }
    return err;
}

static esp_err_t burner_bacon_mbc5_probe_locked(
    uint8_t id_out[4],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out,
    burner_nor_cmdset_t *cmdset_out)
{
    uint32_t cfi_device_size = 0u;
    uint32_t cfi_sector_size = 0u;
    uint16_t cfi_buffer_write_bytes = 0u;
    burner_nor_geometry_t cfi_geometry = {0};
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    const burner_nor_entry_t *id_entry = NULL;
    burner_nor_geometry_t id_geometry = {0};
    uint32_t id_device_size = 0u;
    uint32_t id_sector_size = 0u;
    uint16_t id_buffer_write_bytes = 0u;
    burner_nor_cmdset_t id_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    bool id_geometry_ok = false;
    uint32_t cfi_try;
    esp_err_t err = ESP_FAIL;

    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(id_out, 0, 4u);
    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }
    burner_nor_geometry_clear(&s_cart_ctx.geometry);

    err = burner_bacon_mbc5_get_id(id_out);
    if (err == ESP_OK) {
        id_entry = burner_nor_db_lookup_mbc5(id_out);
        id_cmdset = burner_nor_entry_cmdset(id_entry);
        id_geometry_ok = burner_mbc5_probe_load_entry_geometry(
            id_out,
            id_entry,
            &id_device_size,
            &id_sector_size,
            &id_buffer_write_bytes,
            &id_geometry);
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 ID probe: id=%02X %02X %02X %02X chip=%s flash=%" PRIu32
            " sector=%" PRIu32 " geom=%s largest=%" PRIu32 " regions=%u buf=%u cmdset=%s",
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3],
            burner_nor_entry_name(id_entry),
            id_device_size,
            id_sector_size,
            burner_nor_geometry_is_uniform(&id_geometry) ? "uniform" : "mixed",
            burner_nor_geometry_largest_sector_size(&id_geometry),
            (unsigned)id_geometry.region_count,
            (unsigned)id_buffer_write_bytes,
            burner_nor_cmdset_name(id_cmdset));
    } else {
        ESP_LOGW(BURNER_TAG, "MBC5 ID probe failed before CFI: %s", esp_err_to_name(err));
        memset(id_out, 0, 4u);
    }

    err = ESP_FAIL;
    for (cfi_try = 0u; cfi_try < 3u; ++cfi_try) {
        burner_nor_geometry_clear(&cfi_geometry);
        cfi_device_size = 0u;
        cfi_sector_size = 0u;
        cfi_buffer_write_bytes = 0u;
        cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
        err = burner_bacon_mbc5_get_cfi(
            &cfi_device_size,
            &cfi_sector_size,
            &cfi_buffer_write_bytes,
            &cfi_geometry,
            &cfi_cmdset);
        if (err == ESP_OK) {
            if (!id_geometry_ok) {
                esp_err_t id_retry_err = burner_bacon_mbc5_get_id(id_out);

                if (id_retry_err == ESP_OK) {
                    id_entry = burner_nor_db_lookup_mbc5(id_out);
                    id_cmdset = burner_nor_entry_cmdset(id_entry);
                    id_geometry_ok = burner_mbc5_probe_load_entry_geometry(
                        id_out,
                        id_entry,
                        &id_device_size,
                        &id_sector_size,
                        &id_buffer_write_bytes,
                        &id_geometry);
                    ESP_LOGI(
                        BURNER_TAG,
                        "MBC5 ID retry after CFI: id=%02X %02X %02X %02X chip=%s flash=%" PRIu32
                        " sector=%" PRIu32 " geom=%s largest=%" PRIu32 " regions=%u buf=%u cmdset=%s",
                        id_out[0],
                        id_out[1],
                        id_out[2],
                        id_out[3],
                        burner_nor_entry_name(id_entry),
                        id_device_size,
                        id_sector_size,
                        burner_nor_geometry_is_uniform(&id_geometry) ? "uniform" : "mixed",
                        burner_nor_geometry_largest_sector_size(&id_geometry),
                        (unsigned)id_geometry.region_count,
                        (unsigned)id_buffer_write_bytes,
                        burner_nor_cmdset_name(id_cmdset));
                } else {
                    ESP_LOGW(BURNER_TAG, "MBC5 ID retry after CFI failed: %s", esp_err_to_name(id_retry_err));
                    memset(id_out, 0, 4u);
                }
            }
            if (id_geometry_ok &&
                burner_mbc5_geometry_should_prefer_id(
                    &cfi_geometry,
                    cfi_device_size,
                    cfi_sector_size,
                    &id_geometry,
                    id_device_size,
                    id_sector_size)) {
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 CFI geometry conflicts with ID, using ID geometry: cfi_flash=%" PRIu32
                    " cfi_sector=%" PRIu32 " cfi_geom=%s cfi_largest=%" PRIu32
                    " id_flash=%" PRIu32 " id_sector=%" PRIu32 " chip=%s",
                    cfi_device_size,
                    cfi_sector_size,
                    burner_nor_geometry_is_uniform(&cfi_geometry) ? "uniform" : "mixed",
                    burner_nor_geometry_largest_sector_size(&cfi_geometry),
                    id_device_size,
                    id_sector_size,
                    burner_nor_entry_name(id_entry));
                cfi_device_size = id_device_size;
                cfi_sector_size = id_sector_size;
                if (cfi_buffer_write_bytes == 0u || id_buffer_write_bytes < cfi_buffer_write_bytes) {
                    cfi_buffer_write_bytes = id_buffer_write_bytes;
                }
                cfi_geometry = id_geometry;
            } else if (id_geometry_ok) {
                if (cfi_device_size == 0u) {
                    cfi_device_size = id_device_size;
                }
                if (cfi_sector_size == 0u) {
                    cfi_sector_size = id_sector_size;
                }
                if (cfi_buffer_write_bytes == 0u) {
                    cfi_buffer_write_bytes = id_buffer_write_bytes;
                }
                if (!burner_nor_geometry_is_valid(&cfi_geometry)) {
                    cfi_geometry = id_geometry;
                }
            }
            if (!burner_nor_geometry_is_valid(&cfi_geometry) ||
                cfi_device_size == 0u || cfi_sector_size == 0u) {
                err = ESP_ERR_INVALID_SIZE;
                continue;
            }
            if (id_cmdset != BURNER_NOR_CMDSET_UNKNOWN &&
                cfi_cmdset != BURNER_NOR_CMDSET_UNKNOWN &&
                cfi_cmdset != id_cmdset) {
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 CFI cmdset conflicts with ID, using ID cmdset: cfi=%s id=%s chip=%s",
                    burner_nor_cmdset_name(cfi_cmdset),
                    burner_nor_cmdset_name(id_cmdset),
                    burner_nor_entry_name(id_entry));
            }
            if (id_buffer_write_bytes > 0u && cfi_buffer_write_bytes > id_buffer_write_bytes) {
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 CFI buffer larger than ID limit, clamping: cfi_buf=%u id_buf=%u chip=%s",
                    (unsigned)cfi_buffer_write_bytes,
                    (unsigned)id_buffer_write_bytes,
                    burner_nor_entry_name(id_entry));
                cfi_buffer_write_bytes = id_buffer_write_bytes;
            }
            *device_size = cfi_device_size;
            *sector_size = cfi_sector_size;
            *buffer_write_bytes = cfi_buffer_write_bytes;
            *cfi_ok_out = true;
            if (cmdset_out != NULL) {
                *cmdset_out = (id_cmdset != BURNER_NOR_CMDSET_UNKNOWN) ? id_cmdset : cfi_cmdset;
            }
            s_cart_ctx.geometry = cfi_geometry;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (id_geometry_ok) {
        *device_size = id_device_size;
        *sector_size = id_sector_size;
        *buffer_write_bytes = id_buffer_write_bytes;
        *cfi_ok_out = false;
        if (cmdset_out != NULL) {
            *cmdset_out = id_cmdset;
        }
        s_cart_ctx.geometry = id_geometry;
        ESP_LOGW(
            BURNER_TAG,
            "MBC5 CFI read failed after retries, fallback by ID geometry: flash=%" PRIu32
            " sector=%" PRIu32 " buf=%u chip=%s id=%02X %02X %02X %02X",
            *device_size,
            *sector_size,
            (unsigned)*buffer_write_bytes,
            burner_nor_entry_name(id_entry),
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3]);
        return ESP_OK;
    }

    burner_nor_geometry_clear(&s_cart_ctx.geometry);
    return err;
}

static esp_err_t burner_bacon_mbc5_erase_sector(uint32_t flash_addr, uint32_t timeout_ms)
{
    uint16_t bank = 0u;
    uint16_t cart_addr = 0u;
    uint8_t cmd;
    esp_err_t err;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    burner_mbc5_addr_to_program_window(flash_addr, &bank, &cart_addr, NULL);
    err = burner_bacon_mbc5_switch_bank(bank);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = bank;

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x80u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x30u;
    err = burner_bacon_gbc_write(cart_addr, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_wait_u8(cart_addr, 0xFFu, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        uint8_t read_back = 0u;
        (void)burner_bacon_gbc_read_u8(cart_addr, &read_back);
        ESP_LOGW(
            BURNER_TAG,
            "MBC5 erase timeout flash=0x%08" PRIX32 " bank=%u cart_addr=0x%04X sector=%" PRIu32
            " read=0x%02X timeout=%ums",
            flash_addr,
            (unsigned)bank,
            (unsigned)cart_addr,
            burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry),
            (unsigned)read_back,
            (unsigned)timeout_ms);
    }
    return err;
}

static esp_err_t burner_buffer_all_ff(const uint8_t *buf, size_t len, bool *all_ff_out)
{
    if (buf == NULL || all_ff_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != 0xFFu) {
            *all_ff_out = false;
            return ESP_OK;
        }
    }
    *all_ff_out = true;
    return ESP_OK;
}

static size_t burner_blank_head_check_len(uint32_t region_size)
{
    return (region_size < BURN_BLANK_HEAD_CHECK_BYTES) ? (size_t)region_size : (size_t)BURN_BLANK_HEAD_CHECK_BYTES;
}

static esp_err_t burner_mbc5_region_is_blank_head(
    uint32_t region_addr,
    uint32_t region_size,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_HEAD_CHECK_BYTES];
    size_t sample_len;
    esp_err_t err;

    if (blank_out == NULL || region_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_len = burner_blank_head_check_len(region_size);
    err = burner_bacon_mbc5_read_block_program_window(sample_buf, sample_len, region_addr);
    if (err != ESP_OK) {
        return err;
    }
    return burner_buffer_all_ff(sample_buf, sample_len, blank_out);
}

static esp_err_t burner_gba_region_is_blank_head(
    uint32_t region_addr,
    uint32_t region_size,
    bool is_multi_card,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_HEAD_CHECK_BYTES];
    size_t sample_len;
    esp_err_t err;

    if (blank_out == NULL || region_size == 0u || (region_size & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_len = burner_blank_head_check_len(region_size);
    if ((sample_len & 0x1u) != 0u) {
        sample_len -= 1u;
    }
    if (sample_len == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = burner_bacon_gba_read_block(sample_buf, sample_len, region_addr, is_multi_card);
    if (err != ESP_OK) {
        return err;
    }
    return burner_buffer_all_ff(sample_buf, sample_len, blank_out);
}

static bool burner_blank_sample_offset_seen(const uint32_t *offsets, size_t count, uint32_t offset)
{
    for (size_t i = 0u; i < count; ++i) {
        if (offsets[i] == offset) {
            return true;
        }
    }
    return false;
}

static size_t burner_build_blank_sample_offsets(
    uint32_t region_size,
    size_t sample_len,
    uint32_t align_mask,
    uint32_t offsets[BURN_BLANK_SAMPLE_POINTS])
{
    uint32_t candidates[BURN_BLANK_SAMPLE_POINTS];
    uint32_t max_offset;
    size_t count = 0u;

    if (offsets == NULL || region_size == 0u || sample_len == 0u) {
        return 0u;
    }

    max_offset = (region_size > (uint32_t)sample_len) ? (region_size - (uint32_t)sample_len) : 0u;
    candidates[0] = 0u;
    candidates[1] = (uint32_t)(((uint64_t)max_offset * 30u) / 100u);
    candidates[2] = (uint32_t)(((uint64_t)max_offset * 70u) / 100u);
    candidates[3] = max_offset;

    for (size_t i = 0u; i < BURN_BLANK_SAMPLE_POINTS; ++i) {
        uint32_t offset = candidates[i];

        if (align_mask != 0u) {
            offset &= ~align_mask;
        }
        if (!burner_blank_sample_offset_seen(offsets, count, offset)) {
            offsets[count++] = offset;
        }
    }

    return count;
}

static esp_err_t burner_mbc5_region_is_blank_sampled(
    uint32_t region_addr,
    uint32_t region_size,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_SAMPLE_BYTES];
    uint32_t sample_offsets[BURN_BLANK_SAMPLE_POINTS];
    size_t sample_len;
    size_t sample_count;
    esp_err_t err;

    if (blank_out == NULL || region_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_len = (region_size < BURN_BLANK_SAMPLE_BYTES) ? (size_t)region_size : (size_t)BURN_BLANK_SAMPLE_BYTES;
    sample_count = burner_build_blank_sample_offsets(region_size, sample_len, 0u, sample_offsets);
    if (sample_count == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *blank_out = true;
    for (size_t i = 0u; i < sample_count; ++i) {
        bool chunk_blank = false;

        err = burner_bacon_mbc5_read_block_program_window(
            sample_buf,
            sample_len,
            region_addr + sample_offsets[i]);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_buffer_all_ff(sample_buf, sample_len, &chunk_blank);
        if (err != ESP_OK) {
            return err;
        }
        if (!chunk_blank) {
            *blank_out = false;
            return ESP_OK;
        }
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static esp_err_t burner_mbc5_sector_is_blank(
    uint32_t sector_addr,
    uint32_t sector_size,
    bool *blank_out)
{
    return burner_mbc5_region_is_blank_sampled(sector_addr, sector_size, blank_out);
}

static esp_err_t burner_bacon_mbc5_erase_range(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool sample_blank_sectors,
    bool erase_always)
{
    const burner_nor_geometry_t *geometry = &s_cart_ctx.geometry;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr = 0u;
    uint32_t skipped_blank = 0u;
    uint32_t erased = 0u;
    uint32_t erase_bytes;
    uint32_t timeout_ms;
    int64_t erase_deadline_us;
    esp_err_t err = ESP_OK;

    (void)sector_size;
    if (!burner_nor_geometry_is_valid(geometry) || addr_end < addr_begin) {
        return ESP_ERR_INVALID_ARG;
    }
    if (burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    sector_addr = cursor.addr_begin + (((addr_begin - cursor.addr_begin) / cursor.sector_size) * cursor.sector_size);

    erase_bytes = burner_nor_geometry_erase_bytes_from_range(geometry, addr_begin, addr_end);
    if (erase_bytes == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    timeout_ms = burner_erase_timeout_ms_for_bytes(erase_bytes);
    erase_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    {
        uint16_t first_bank = 0u;
        uint16_t first_cart_addr = 0u;

        burner_mbc5_addr_to_program_window(sector_addr, &first_bank, &first_cart_addr, NULL);
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 erase plan: range=0x%08" PRIX32 "-0x%08" PRIX32
            " first_sector=0x%08" PRIX32 " first_bank=%u first_cart=0x%04X bytes=%" PRIu32
            " timeout=%" PRIu32 "ms geom=%s largest=%" PRIu32 " regions=%u",
            addr_begin,
            addr_end,
            sector_addr,
            (unsigned)first_bank,
            (unsigned)first_cart_addr,
            erase_bytes,
            timeout_ms,
            burner_nor_geometry_is_uniform(geometry) ? "uniform" : "mixed",
            burner_nor_geometry_largest_sector_size(geometry),
            (unsigned)geometry->region_count);
    }

    while (sector_addr <= addr_end) {
        uint32_t current_sector_size = cursor.sector_size;
        uint32_t region_end_addr = cursor.addr_end - 1u;
        uint32_t region_limit_addr = (addr_end < region_end_addr) ? addr_end : region_end_addr;

        while (sector_addr <= region_limit_addr) {
            uint16_t sector_bank = 0u;
            uint16_t sector_cart_addr = 0u;

            err = burner_cancel_poll();
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            burner_mbc5_addr_to_program_window(sector_addr, &sector_bank, &sector_cart_addr, NULL);
            ESP_LOGI(
                BURNER_TAG,
                "MBC5 erase sector: flash=0x%08" PRIX32 " size=%" PRIu32 " bank=%u cart=0x%04X",
                sector_addr,
                current_sector_size,
                (unsigned)sector_bank,
                (unsigned)sector_cart_addr);
            if (sample_blank_sectors && !erase_always) {
                bool blank = false;

                err = burner_mbc5_sector_is_blank(
                    sector_addr,
                    current_sector_size,
                    &blank);
                if (err != ESP_OK) {
                    goto erase_range_out;
                }
                if (blank) {
                    skipped_blank++;
                    burner_status_advance_erase_phase(1u, current_sector_size);
                    sector_addr += current_sector_size;
                    continue;
                }
            }
            err = burner_bacon_mbc5_erase_sector(
                sector_addr,
                burner_erase_remaining_timeout_ms(erase_deadline_us));
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            erased++;
            burner_status_advance_erase_phase(1u, current_sector_size);
            sector_addr += current_sector_size;
        }
        if (region_limit_addr >= addr_end) {
            break;
        }
        err = burner_nor_geometry_region_cursor_advance(geometry, &cursor);
        if (err != ESP_OK) {
            break;
        }
        sector_addr = cursor.addr_begin;
    }

erase_range_out:
    if (err == ESP_OK && sample_blank_sectors && !erase_always &&
        (erased > 0u || skipped_blank > 0u)) {
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 erase sector-sample: 4x2B erased=%" PRIu32 " skipped_blank=%" PRIu32,
            erased,
            skipped_blank);
    }
    return err;
}

static esp_err_t burner_bacon_mbc5_chip_erase(void)
{
    uint8_t cmd;
    uint8_t read_back = 0;
    int64_t deadline_us;
    esp_err_t err;

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = 0u;

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x80u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x10u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)BURNER_ROM_CHIP_ERASE_TIMEOUT_MS * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(0x0000u, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == 0xFFu) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gbc_rom_program(
    uint16_t cart_addr,
    const uint8_t *buf,
    size_t len,
    uint16_t buffer_write_bytes)
{
    size_t i = 0;
    esp_err_t err;

    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (i < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint16_t start_addr = (uint16_t)(cart_addr + (uint16_t)i);

        if (buffer_write_bytes == 0u) {
            uint8_t seq[24];

            seq[0] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[1] = 0xAAu;
            seq[2] = 0x0Au;
            seq[3] = 0xAAu;
            seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[5] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[6] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[7] = 0x55u;
            seq[8] = 0x05u;
            seq[9] = 0x55u;
            seq[10] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[11] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[12] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[13] = 0xAAu;
            seq[14] = 0x0Au;
            seq[15] = 0xA0u;
            seq[16] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[17] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[18] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[19] = (uint8_t)(start_addr & 0xFFu);
            seq[20] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[21] = buf[i];
            seq[22] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[23] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

            err = burner_spi_transfer(seq, NULL, sizeof(seq));
            if (err != ESP_OK) {
                return err;
            }
            err = burner_bacon_wait_u8(start_addr, buf[i], BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
            i += 1u;
        } else {
            size_t wr_buf_cnt;
            size_t write_len = len - i;
            size_t seq_len;
            uint8_t *seq;
            uint8_t last_expected;
            uint16_t last_addr;
            bool fallback_to_single = false;

            if (write_len > buffer_write_bytes) {
                write_len = buffer_write_bytes;
            }

            seq_len = 25u + 3u * write_len;
            if (seq_len > BURNER_SPI_MAX_XFER) {
                return ESP_ERR_INVALID_SIZE;
            }

            seq = (uint8_t *)malloc(seq_len);
            if (seq == NULL) {
                return ESP_ERR_NO_MEM;
            }

            seq[0] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[1] = 0xAAu;
            seq[2] = 0xAAu;
            seq[3] = 0x0Au;
            seq[4] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[5] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[6] = 0x55u;
            seq[7] = 0x55u;
            seq[8] = 0x05u;
            seq[9] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[10] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[11] = 0x25u;
            seq[12] = (uint8_t)(start_addr & 0xFFu);
            seq[13] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[14] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[15] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[16] = (uint8_t)(write_len - 1u);
            seq[17] = (uint8_t)(start_addr & 0xFFu);
            seq[18] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[19] = burner_bacon_option_byte2(0, true, true, false, true, true, true);

            for (wr_buf_cnt = 0; wr_buf_cnt < write_len; ++wr_buf_cnt) {
                size_t base = 20u + 3u * wr_buf_cnt;

                seq[base + 0u] = burner_bacon_option_byte2(1, true, true, false, false, true, false);
                seq[base + 1u] = buf[i + wr_buf_cnt];
                seq[base + 2u] = burner_bacon_option_byte2(0, true, true, true, false, true, true);
            }

            seq[20u + 3u * write_len + 0u] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[20u + 3u * write_len + 1u] = 0x29u;
            seq[20u + 3u * write_len + 2u] = (uint8_t)(start_addr & 0xFFu);
            seq[20u + 3u * write_len + 3u] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[20u + 3u * write_len + 4u] = burner_bacon_option_byte2(0, true, true, false, true, true, true);

            err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, seq, NULL, seq_len);
            free(seq);
            if (err != ESP_OK) {
                fallback_to_single = true;
            }

            if (!fallback_to_single) {
                last_addr = (uint16_t)(start_addr + (uint16_t)write_len - 1u);
                last_expected = buf[i + write_len - 1u];
                err = burner_bacon_wait_u8(last_addr, last_expected, BURNER_ROM_POLL_TIMEOUT_MS);
                if (err != ESP_OK) {
                    fallback_to_single = true;
                }
            }

            if (fallback_to_single) {
                size_t single_i;
                burner_status_record_mbc5_buffer_write(true);
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 buffer write fallback -> single-byte at 0x%04X len=%u",
                    (unsigned)start_addr,
                    (unsigned)write_len);
                for (single_i = 0u; single_i < write_len; ++single_i) {
                    uint16_t single_addr = (uint16_t)(start_addr + (uint16_t)single_i);
                    uint8_t seq_single[24];

                    seq_single[0] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[1] = 0xAAu;
                    seq_single[2] = 0x0Au;
                    seq_single[3] = 0xAAu;
                    seq_single[4] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[5] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[6] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[7] = 0x55u;
                    seq_single[8] = 0x05u;
                    seq_single[9] = 0x55u;
                    seq_single[10] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[11] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[12] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[13] = 0xAAu;
                    seq_single[14] = 0x0Au;
                    seq_single[15] = 0xA0u;
                    seq_single[16] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[17] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[18] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[19] = (uint8_t)(single_addr & 0xFFu);
                    seq_single[20] = (uint8_t)((single_addr >> 8) & 0xFFu);
                    seq_single[21] = buf[i + single_i];
                    seq_single[22] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[23] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

                    err = burner_spi_transfer(seq_single, NULL, sizeof(seq_single));
                    if (err != ESP_OK) {
                        return err;
                    }
                    err = burner_bacon_wait_u8(single_addr, buf[i + single_i], BURNER_ROM_POLL_TIMEOUT_MS);
                    if (err != ESP_OK) {
                        return err;
                    }
                    burner_task_yield_if_due();
                }
            } else {
                burner_status_record_mbc5_buffer_write(false);
            }

            i += write_len;
            burner_task_yield_if_due();
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_intel_program_words(uint32_t byte_addr, const uint8_t *buf, size_t len)
{
    size_t off = 0u;
    esp_err_t err;

    if (buf == NULL || len == 0u || (byte_addr & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (off < len) {
        uint32_t starting_address = byte_addr + (uint32_t)off;
        uint32_t starting_word_address = starting_address >> 1;
        uint16_t pd = (uint16_t)((uint16_t)buf[off] | ((uint16_t)buf[off + 1u] << 8));
        uint16_t status = 0u;

        err = burner_bacon_gba_command_write_u16(0x000u, 0x0070u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x0010u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_write_u16(starting_word_address, pd);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_intel_wait_ready(0x000u, 0x0070u, BURNER_ROM_POLL_TIMEOUT_MS, &status);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA intel single-word program timeout @0x%08" PRIX32 " status=0x%04X",
                starting_address,
                status);
            return err;
        }
        err = burner_bacon_gba_intel_read_array();
        if (err != ESP_OK) {
            return err;
        }

        off += 2u;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_rom_program(
    uint32_t byte_addr,
    const uint8_t *buf,
    size_t len,
    uint16_t buffer_write_bytes)
{
    size_t i = 0;
    esp_err_t err;
    bool intel_cmdset;

    if (buf == NULL || len == 0u || (byte_addr & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    intel_cmdset = burner_gba_nor_is_intel_active();

    while (i < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t starting_address = byte_addr + (uint32_t)i;
        uint32_t starting_word_address = starting_address >> 1;

        if (intel_cmdset) {
            if (buffer_write_bytes >= 2u) {
                uint16_t active_buffer_write_bytes = buffer_write_bytes;
                bool allow_runtime_fallback = burner_gba_intel_program_buffer_needs_runtime_fallback(
                    s_cart_ctx.gba_cmdset,
                    s_cart_ctx.buffer_write_bytes,
                    active_buffer_write_bytes);

                while (true) {
                    size_t write_len = 0u;
                    uint16_t next_buffer_write_bytes;

                    err = burner_bacon_gba_intel_buffered_program_once(
                        starting_address,
                        buf + i,
                        len - i,
                        active_buffer_write_bytes,
                        &write_len);
                    if (err == ESP_OK) {
                        i += write_len;
                        burner_task_yield_if_due();
                        break;
                    }
                    if (!allow_runtime_fallback ||
                        err == ESP_ERR_INVALID_ARG ||
                        err == ESP_ERR_INVALID_SIZE ||
                        err == ESP_ERR_NO_MEM ||
                        err == ESP_ERR_INVALID_STATE) {
                        return err;
                    }
                    next_buffer_write_bytes =
                        burner_gba_intel_next_program_buffer_write_bytes(active_buffer_write_bytes);
                    if (next_buffer_write_bytes == 0u || next_buffer_write_bytes >= active_buffer_write_bytes) {
                        return err;
                    }
                    ESP_LOGW(
                        BURNER_TAG,
                        "GBA intel runtime buffer fallback: probe_buf=0 active=%u next=%u addr=0x%08" PRIX32
                        " err=%s",
                        (unsigned)active_buffer_write_bytes,
                        (unsigned)next_buffer_write_bytes,
                        starting_address,
                        esp_err_to_name(err));
                    err = burner_bacon_gba_intel_reset();
                    if (err != ESP_OK) {
                        return err;
                    }
                    active_buffer_write_bytes = next_buffer_write_bytes;
                    buffer_write_bytes = next_buffer_write_bytes;
                    s_cart_ctx.program_buffer_write_bytes = next_buffer_write_bytes;
                }
                continue;
            }

            {
                err = burner_bacon_gba_intel_program_words(starting_address, buf + i, 2u);
                if (err != ESP_OK) {
                    return err;
                }
                i += 2u;
                continue;
            }
        } else if (buffer_write_bytes < 2u) {
            uint8_t seq[41];
            uint16_t pd = (uint16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1u] << 8));
            uint16_t cmd_aa = burner_apply_d0d1_swap_on_write(0x00AAu, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_55 = burner_apply_d0d1_swap_on_write(0x0055u, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_a0 = burner_apply_d0d1_swap_on_write(0x00A0u, s_cart_ctx.d0d1_swapped);
            uint8_t addr0 = (uint8_t)(starting_word_address & 0xFFu);
            uint8_t addr1 = (uint8_t)((starting_word_address >> 8) & 0xFFu);
            uint8_t addr2 = (uint8_t)((starting_word_address >> 16) & 0xFFu);

            seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[1] = 0x55u;
            seq[2] = 0x05u;
            seq[3] = 0x00u;
            seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[5] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[6] = (uint8_t)(cmd_aa & 0xFFu);
            seq[7] = (uint8_t)((cmd_aa >> 8) & 0xFFu);
            seq[8] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[9] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[10] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[11] = 0xAAu;
            seq[12] = 0x02u;
            seq[13] = 0x00u;
            seq[14] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[15] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[16] = (uint8_t)(cmd_55 & 0xFFu);
            seq[17] = (uint8_t)((cmd_55 >> 8) & 0xFFu);
            seq[18] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[19] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[20] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[21] = 0x55u;
            seq[22] = 0x05u;
            seq[23] = 0x00u;
            seq[24] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[25] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[26] = (uint8_t)(cmd_a0 & 0xFFu);
            seq[27] = (uint8_t)((cmd_a0 >> 8) & 0xFFu);
            seq[28] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[29] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[30] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[31] = addr0;
            seq[32] = addr1;
            seq[33] = addr2;
            seq[34] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[35] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[36] = buf[i + 0u];
            seq[37] = buf[i + 1u];
            seq[38] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[39] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[40] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

            err = burner_spi_transfer(seq, NULL, sizeof(seq));
            if (err != ESP_OK) {
                return err;
            }

            err = burner_bacon_wait_u16(starting_address, pd, BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }

            i += 2u;
        } else {
            size_t write_len = len - i;
            size_t wr;
            size_t write_words;
            size_t max_write_words_by_spi = 1u;
            size_t seq_len;
            uint8_t *seq;
            uint8_t addr0;
            uint8_t addr1;
            uint8_t addr2;
            uint8_t unlock0_addr0;
            uint8_t unlock0_addr1;
            uint8_t unlock0_addr2;
            uint8_t unlock1_addr0;
            uint8_t unlock1_addr1;
            uint8_t unlock1_addr2;
            uint16_t last_word;
            uint16_t cmd_aa = burner_apply_d0d1_swap_on_write(0x00AAu, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_55 = burner_apply_d0d1_swap_on_write(0x0055u, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_25 = burner_apply_d0d1_swap_on_write(0x0025u, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_29 = burner_apply_d0d1_swap_on_write(0x0029u, s_cart_ctx.d0d1_swapped);
            uint16_t write_count_word;
            uint32_t unlock0_addr = burner_gba_unlock_addr0();
            uint32_t unlock1_addr = burner_gba_unlock_addr1();

            if (write_len > buffer_write_bytes) {
                write_len = buffer_write_bytes;
            }
            write_len = burner_gba_program_safe_chunk_bytes(
                starting_address,
                write_len,
                (size_t)buffer_write_bytes);
            write_words = write_len / 2u;

            if (BURNER_SPI_MAX_XFER > 57u) {
                max_write_words_by_spi = (BURNER_SPI_MAX_XFER - 57u) / 5u;
            }
            if (max_write_words_by_spi == 0u) {
                max_write_words_by_spi = 1u;
            }
            if (write_words > max_write_words_by_spi) {
                write_words = max_write_words_by_spi;
                write_len = write_words * 2u;
            }

            seq_len = 57u + 5u * write_words;
            if (seq_len > BURNER_SPI_MAX_XFER) {
                return ESP_ERR_INVALID_SIZE;
            }
            seq = (uint8_t *)malloc(seq_len);
            if (seq == NULL) {
                return ESP_ERR_NO_MEM;
            }

            addr0 = (uint8_t)(starting_word_address & 0xFFu);
            addr1 = (uint8_t)((starting_word_address >> 8) & 0xFFu);
            addr2 = (uint8_t)((starting_word_address >> 16) & 0xFFu);
            unlock0_addr0 = (uint8_t)(unlock0_addr & 0xFFu);
            unlock0_addr1 = (uint8_t)((unlock0_addr >> 8) & 0xFFu);
            unlock0_addr2 = (uint8_t)((unlock0_addr >> 16) & 0xFFu);
            unlock1_addr0 = (uint8_t)(unlock1_addr & 0xFFu);
            unlock1_addr1 = (uint8_t)((unlock1_addr >> 8) & 0xFFu);
            unlock1_addr2 = (uint8_t)((unlock1_addr >> 16) & 0xFFu);
            write_count_word = burner_apply_d0d1_swap_on_write(
                (uint16_t)(write_words - 1u),
                s_cart_ctx.d0d1_swapped);

            seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[1] = unlock0_addr0;
            seq[2] = unlock0_addr1;
            seq[3] = unlock0_addr2;
            seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[5] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[6] = (uint8_t)(cmd_aa & 0xFFu);
            seq[7] = (uint8_t)((cmd_aa >> 8) & 0xFFu);
            seq[8] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[9] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[10] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[11] = unlock1_addr0;
            seq[12] = unlock1_addr1;
            seq[13] = unlock1_addr2;
            seq[14] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[15] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[16] = (uint8_t)(cmd_55 & 0xFFu);
            seq[17] = (uint8_t)((cmd_55 >> 8) & 0xFFu);
            seq[18] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[19] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[20] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[21] = addr0;
            seq[22] = addr1;
            seq[23] = addr2;
            seq[24] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[25] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[26] = (uint8_t)(cmd_25 & 0xFFu);
            seq[27] = (uint8_t)((cmd_25 >> 8) & 0xFFu);
            seq[28] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[29] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[30] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[31] = addr0;
            seq[32] = addr1;
            seq[33] = addr2;
            seq[34] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[35] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[36] = (uint8_t)(write_count_word & 0xFFu);
            seq[37] = (uint8_t)((write_count_word >> 8) & 0xFFu);
            seq[38] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[39] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[40] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[41] = addr0;
            seq[42] = addr1;
            seq[43] = addr2;
            seq[44] = burner_bacon_option_byte0(0, true, true, true, false, true, true);

            for (wr = 0; wr < write_words; ++wr) {
                size_t base = 45u + 5u * wr;
                seq[base + 0u] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
                seq[base + 1u] = buf[i + wr * 2u];
                seq[base + 2u] = buf[i + wr * 2u + 1u];
                seq[base + 3u] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                seq[base + 4u] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            }

            seq[45u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[46u + 5u * write_words] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[47u + 5u * write_words] = addr0;
            seq[48u + 5u * write_words] = addr1;
            seq[49u + 5u * write_words] = addr2;
            seq[50u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[51u + 5u * write_words] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[52u + 5u * write_words] = (uint8_t)(cmd_29 & 0xFFu);
            seq[53u + 5u * write_words] = (uint8_t)((cmd_29 >> 8) & 0xFFu);
            seq[54u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[55u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[56u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

            err = burner_spi_transfer(seq, NULL, seq_len);
            free(seq);
            if (err != ESP_OK) {
                return err;
            }

            last_word = (uint16_t)((uint16_t)buf[i + write_len - 2u] |
                                   ((uint16_t)buf[i + write_len - 1u] << 8));
            err = burner_bacon_wait_u16(
                starting_address + (uint32_t)write_len - 2u,
                last_word,
                BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }

            i += write_len;
            burner_task_yield_if_due();
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_program_block(
    const uint8_t *data,
    size_t len,
    uint32_t offset,
    bool is_multi_card,
    bool prepare_sectors)
{
    size_t programmed = 0;
    esp_err_t err;
    bool geometry_valid = burner_nor_geometry_is_valid(&s_cart_ctx.geometry);

    if (data == NULL || len == 0u || (offset & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_cart_ctx.prepared) {
        return ESP_ERR_INVALID_STATE;
    }

    while (programmed < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)programmed;
        uint32_t bank = 0u;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        uint32_t sector_end = 0u;
        size_t remain = len - programmed;
        size_t chunk;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        chunk = (remain < bank_remain) ? remain : bank_remain;
        if (geometry_valid) {
            err = burner_nor_geometry_sector_bounds(&s_cart_ctx.geometry, rom_addr, NULL, &sector_end, NULL);
            if (err != ESP_OK || sector_end <= rom_addr) {
                return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
            }
            if (chunk > (size_t)(sector_end - rom_addr)) {
                chunk = (size_t)(sector_end - rom_addr);
            }
        }

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (prepare_sectors) {
            err = burner_gba_sector_erase_prepare_current(rom_addr);
            if (err != ESP_OK) {
                return err;
            }
        }

        err = burner_bacon_gba_rom_program(
            rom_addr,
            data + programmed,
            chunk,
            s_cart_ctx.program_buffer_write_bytes);
        if (err != ESP_OK) {
            return err;
        }

        if (prepare_sectors) {
            err = burner_gba_sector_erase_prefetch_next(rom_addr, chunk);
            if (err != ESP_OK) {
                return err;
            }
        }

        programmed += chunk;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

esp_err_t burner_bacon_gba_read_block(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint32_t bank = 0u;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        size_t remain = len - copied;
        size_t chunk;
        size_t read_words;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        read_words = chunk / 2u;
        if (read_words > 0u) {
            err = burner_bacon_rom_read_u16_batched(rom_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((rom_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}

#if 0
/* Legacy GBA verify block reader kept only for reference. */
static esp_err_t burner_bacon_gba_verify_read_block(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint32_t bank = 0u;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        size_t remain = len - copied;
        size_t chunk;
        size_t read_words;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        read_words = chunk / 2u;
        if (read_words > 0u) {
            err = burner_bacon_rom_verify_read_u16_batched(rom_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((rom_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}
#endif

esp_err_t burner_bacon_gba_verify_read_block_hoststyle(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr;
        uint32_t bank;
        uint32_t bank_remain;
        size_t remain;
        size_t chunk;
        size_t read_words;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        rom_addr = offset + (uint32_t)copied;
        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        remain = len - copied;
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        read_words = chunk / 2u;
        if (read_words > 0u) {
            err = burner_bacon_rom_verify_read_u16_batched_hoststyle(rom_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((rom_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_erase_sector(uint32_t flash_addr, bool is_multi_card, uint32_t timeout_ms)
{
    uint32_t bank = 0u;
    uint32_t sa_word = flash_addr >> 1;
    uint16_t read_back = 0;
    int64_t deadline_us;
    esp_err_t err;
    bool intel_cmdset;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    intel_cmdset = burner_gba_nor_is_intel_active();
    burner_gba_resolve_write_addr(flash_addr, is_multi_card, &bank, NULL);

    if (is_multi_card) {
        err = burner_gba_switch_bank_if_needed(bank);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (intel_cmdset) {
        err = burner_bacon_gba_intel_reset();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x0060u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x0020u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_intel_wait_ready(flash_addr, 0x0070u, timeout_ms, &read_back);
        if (err == ESP_OK) {
            err = burner_bacon_gba_intel_reset();
            if (err != ESP_OK) {
                return err;
            }
            return ESP_OK;
        }
        ESP_LOGW(
            BURNER_TAG,
            "GBA intel erase timeout flash=0x%08" PRIX32 " bank=%" PRIu32 " sa_word=0x%06" PRIX32 " status=0x%04X multi=%u timeout=%ums",
            flash_addr,
            bank,
            sa_word,
            read_back,
            is_multi_card ? 1u : 0u,
            (unsigned)timeout_ms);
        (void)burner_bacon_gba_intel_reset();
        return err;
    }

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0080u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(sa_word, 0x0030u);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(sa_word, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == 0xFFFFu) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGW(
        BURNER_TAG,
        "GBA erase timeout flash=0x%08" PRIX32 " bank=%" PRIu32 " sa_word=0x%06" PRIX32 " read=0x%04X multi=%u timeout=%ums",
        flash_addr,
        bank,
        sa_word,
        read_back,
        is_multi_card ? 1u : 0u,
        (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_gba_region_is_blank_sampled(
    uint32_t region_addr,
    uint32_t region_size,
    bool is_multi_card,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_SAMPLE_BYTES];
    uint32_t sample_offsets[BURN_BLANK_SAMPLE_POINTS];
    size_t sample_count;
    esp_err_t err;

    if (blank_out == NULL || region_size < BURN_BLANK_SAMPLE_BYTES || (region_size & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_count = burner_build_blank_sample_offsets(
        region_size,
        BURN_BLANK_SAMPLE_BYTES,
        0x1u,
        sample_offsets);
    if (sample_count == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *blank_out = true;
    for (size_t i = 0u; i < sample_count; ++i) {
        bool chunk_blank = false;

        err = burner_bacon_gba_read_block(
            sample_buf,
            BURN_BLANK_SAMPLE_BYTES,
            region_addr + sample_offsets[i],
            is_multi_card);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_buffer_all_ff(sample_buf, BURN_BLANK_SAMPLE_BYTES, &chunk_blank);
        if (err != ESP_OK) {
            return err;
        }
        if (!chunk_blank) {
            *blank_out = false;
            return ESP_OK;
        }
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static esp_err_t burner_gba_sector_is_blank(
    uint32_t sector_addr,
    uint32_t sector_size,
    bool is_multi_card,
    bool *blank_out)
{
    return burner_gba_region_is_blank_sampled(sector_addr, sector_size, is_multi_card, blank_out);
}

static esp_err_t burner_bacon_gba_erase_range(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool is_multi_card,
    bool sample_blank_sectors,
    bool erase_always)
{
    const burner_nor_geometry_t *geometry = &s_cart_ctx.geometry;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr = 0u;
    uint32_t skipped_blank = 0u;
    uint32_t erased = 0u;
    uint32_t erase_bytes;
    uint32_t timeout_ms;
    int64_t erase_deadline_us;
    esp_err_t err = ESP_OK;

    (void)sector_size;
    if (!burner_nor_geometry_is_valid(geometry) || addr_end < addr_begin) {
        return ESP_ERR_INVALID_ARG;
    }
    if (burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    sector_addr = cursor.addr_begin + (((addr_begin - cursor.addr_begin) / cursor.sector_size) * cursor.sector_size);

    erase_bytes = burner_nor_geometry_erase_bytes_from_range(geometry, addr_begin, addr_end);
    if (erase_bytes == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    timeout_ms = burner_erase_timeout_ms_for_bytes(erase_bytes);
    erase_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    ESP_LOGI(
        BURNER_TAG,
        "GBA erase timeout budget: bytes=%" PRIu32 " timeout=%" PRIu32 "ms",
        erase_bytes,
        timeout_ms);

    while (sector_addr <= addr_end) {
        uint32_t current_sector_size = cursor.sector_size;
        uint32_t region_end_addr = cursor.addr_end - 1u;
        uint32_t region_limit_addr = (addr_end < region_end_addr) ? addr_end : region_end_addr;

        while (sector_addr <= region_limit_addr) {
            err = burner_cancel_poll();
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            if (sample_blank_sectors && !erase_always) {
                bool blank = false;

                err = burner_gba_sector_is_blank(
                    sector_addr,
                    current_sector_size,
                    is_multi_card,
                    &blank);
                if (err != ESP_OK) {
                    goto erase_range_out;
                }
                if (blank) {
                    skipped_blank++;
                    burner_status_advance_erase_phase(1u, current_sector_size);
                    sector_addr += current_sector_size;
                    continue;
                }
            }
            err = burner_bacon_gba_erase_sector(
                sector_addr,
                is_multi_card,
                burner_erase_remaining_timeout_ms(erase_deadline_us));
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            erased++;
            burner_status_advance_erase_phase(1u, current_sector_size);
            sector_addr += current_sector_size;
        }
        if (region_limit_addr >= addr_end) {
            break;
        }
        err = burner_nor_geometry_region_cursor_advance(geometry, &cursor);
        if (err != ESP_OK) {
            break;
        }
        sector_addr = cursor.addr_begin;
    }

erase_range_out:
    if (err == ESP_OK && sample_blank_sectors && !erase_always &&
        (erased > 0u || skipped_blank > 0u)) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA erase sector-sample: 4x2B erased=%" PRIu32 " skipped_blank=%" PRIu32,
            erased,
            skipped_blank);
    }
    return err;
}

static esp_err_t burner_bacon_gba_chip_erase_once(void)
{
    uint16_t read_back = 0;
    int64_t deadline_us;
    esp_err_t err;
    bool intel_cmdset;

    intel_cmdset = burner_gba_nor_is_intel_active();

    if (intel_cmdset) {
        err = burner_bacon_gba_intel_reset();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x0060u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x0020u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_intel_wait_ready(0x000u, 0x0070u, BURNER_ROM_CHIP_ERASE_TIMEOUT_MS, &read_back);
        (void)burner_bacon_gba_intel_reset();
        return err;
    }

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0080u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0010u);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)BURNER_ROM_CHIP_ERASE_TIMEOUT_MS * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(0u, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == 0xFFFFu) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gba_chip_erase(void)
{
    uint8_t id[8] = {0};
    esp_err_t err;

    err = burner_bacon_gba_read_id(id, s_cart_ctx.d0d1_swapped);
    if (err != ESP_OK) {
        return err;
    }

    if (burner_bacon_gba_is_s70gl02(id)) {
        err = burner_bacon_gba_rom_switch_bank(0u);
        if (err != ESP_OK) {
            return err;
        }
        s_cart_ctx.current_bank = 0u;
    }

    err = burner_bacon_gba_chip_erase_once();
    if (err != ESP_OK) {
        return err;
    }

    if (burner_bacon_gba_is_s70gl02(id)) {
        err = burner_bacon_gba_rom_switch_bank(5u);
        if (err != ESP_OK) {
            return err;
        }
        s_cart_ctx.current_bank = 5u;
        err = burner_bacon_gba_chip_erase_once();
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_prepare(uint32_t total_bytes)
{
    uint8_t id[4];
    char chip_name[48] = {0};
    uint32_t device_size = 0;
    uint32_t probed_device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    bool cfi_ok = false;
    burner_nor_cmdset_t cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    burner_gb_mapper_t mapper = BURNER_GB_MAPPER_MBC5;
    esp_err_t err;

    if (total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_mbc5_probe_locked(
        id,
        &device_size,
        &sector_size,
        &buffer_write_bytes,
        &cfi_ok,
        &cmdset);
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "MBC5 probe failed: %s", esp_err_to_name(err));
        return err;
    }
    if (cmdset != BURNER_NOR_CMDSET_AMD) {
        ESP_LOGE(
            BURNER_TAG,
            "MBC5 cmdset unsupported for write path: %s",
            burner_nor_cmdset_name(cmdset));
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!cfi_ok) {
        ESP_LOGW(BURNER_TAG, "MBC5 CFI unavailable; continuing with ID geometry");
    }
    if (burner_mbc5_nor_has_flag(id, BURNER_NOR_FLAG_LIMIT_BUFFER_TO_ID)) {
        uint32_t id_device_size = 0u;
        uint32_t id_sector_size = 0u;
        uint16_t id_buffer_write_bytes = 0u;
        if (burner_mbc5_geometry_from_id(id, &id_device_size, &id_sector_size, &id_buffer_write_bytes) &&
            id_buffer_write_bytes > 0u) {
            buffer_write_bytes = id_buffer_write_bytes;
        }
    }
    if (buffer_write_bytes > 512u) {
        /* Keep a conservative upper bound for GBC flash families currently supported. */
        buffer_write_bytes = 512u;
    }
    if (buffer_write_bytes > ((BURNER_SPI_MAX_XFER - 30u) / 6u)) {
        buffer_write_bytes = (uint16_t)((BURNER_SPI_MAX_XFER - 30u) / 6u);
    }

    probed_device_size = device_size;
    err = burner_bacon_gb_detect_mapper(&mapper);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GB mapper detect failed, defaulting to mbc5: %s", esp_err_to_name(err));
        mapper = BURNER_GB_MAPPER_MBC5;
        err = ESP_OK;
    }
    s_gb_mapper_kind = mapper;
    if (burner_gb_mapper_device_size_limit(mapper) > 0u &&
        device_size > burner_gb_mapper_device_size_limit(mapper)) {
        device_size = burner_gb_mapper_device_size_limit(mapper);
        if (burner_nor_geometry_limit_prefix(&s_cart_ctx.geometry, device_size) != ESP_OK) {
            ESP_LOGE(BURNER_TAG, "GB mapper geometry clamp failed");
            return ESP_ERR_INVALID_SIZE;
        }
        sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    }

    if (total_bytes > device_size) {
        ESP_LOGE(
            BURNER_TAG,
            "ROM larger than flash: rom=%" PRIu32 " flash=%" PRIu32,
            total_bytes,
            device_size);
        return ESP_ERR_INVALID_SIZE;
    }

    s_cart_ctx.prepared = true;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_cart_ctx.buffer_write_bytes = buffer_write_bytes;
    s_cart_ctx.sector_size = sector_size;
    s_cart_ctx.device_size = device_size;
    memcpy(s_cart_ctx.mbc5_id, id, sizeof(s_cart_ctx.mbc5_id));
    burner_nor_format_chip_name(
        chip_name,
        sizeof(chip_name),
        burner_mbc5_chip_name(id),
        cmdset,
        probed_device_size);

    ESP_LOGI(
        BURNER_TAG,
        "GB prepared: mapper=%s flash=%" PRIu32 " usable=%" PRIu32 " sector=%" PRIu32
        " geom=%s largest=%" PRIu32 " regions=%u buf=%u nor=%s cfi=%s id=%02X %02X %02X %02X",
        burner_gb_mapper_name(mapper),
        probed_device_size,
        device_size,
        sector_size,
        burner_nor_geometry_is_uniform(&s_cart_ctx.geometry) ? "uniform" : "mixed",
        burner_nor_geometry_largest_sector_size(&s_cart_ctx.geometry),
        (unsigned)s_cart_ctx.geometry.region_count,
        (unsigned)buffer_write_bytes,
        burner_nor_cmdset_name(cmdset),
        cfi_ok ? "ok" : "id-fallback",
        id[0],
        id[1],
        id[2],
        id[3]);

    burner_status_set_probe_info(
        BURNER_CART_MODE_MBC5,
        id,
        4u,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok,
        false,
        false,
        false,
        false,
        chip_name);

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_program_block(const uint8_t *data, size_t len, uint32_t offset)
{
    size_t programmed = 0;
    esp_err_t err;

    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_cart_ctx.prepared) {
        return ESP_ERR_INVALID_STATE;
    }

    while (programmed < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)programmed;
        uint16_t bank = 0u;
        uint32_t bank_off = 0u;
        uint16_t cart_addr = 0u;
        size_t remain = len - programmed;
        size_t bank_remain;
        size_t chunk;

        burner_mbc5_addr_to_program_window(rom_addr, &bank, &cart_addr, &bank_off);
        bank_remain = BURN_MBC5_ROM_BANK_BYTES - bank_off;
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        err = burner_bacon_gbc_rom_program(
            cart_addr,
            data + programmed,
            chunk,
            s_cart_ctx.buffer_write_bytes);
        if (err != ESP_OK) {
            return err;
        }

        programmed += chunk;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static bool burner_is_gba_multi_card(const burner_task_param_t *job)
{
    if (job == NULL || job->cart_mode != BURNER_CART_MODE_GBA) {
        return false;
    }
    if (job->gba_force_multi) {
        return true;
    }
    return s_cart_ctx.device_size > BURN_GBA_LINEAR_ADDR_BYTES;
}

esp_err_t burner_spi_prepare_burn_mbc5(const burner_task_param_t *job)
{
    esp_err_t err;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_mbc5_prepare_power();
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_mbc5_prepare(job->total_bytes);
}

esp_err_t burner_spi_prepare_burn_gba(const burner_task_param_t *job)
{
    esp_err_t err;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_gba_prepare_power();
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gba_prepare(job);
}

static esp_err_t burner_spi_prepare_ram(void)
{
    esp_err_t err;

    err = burner_bacon_mbc5_prepare_power();
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_mbc5_ram_enable(true);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

esp_err_t burner_probe_cart_capacity_bytes(burner_cart_mode_t cart_mode, uint32_t *device_size_out)
{
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    bool cfi_ok = false;
    esp_err_t err = ESP_OK;

    if (device_size_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_spi_init();
    if (err != ESP_OK) {
        return err;
    }

    burner_spi_lock_take();
    if (cart_mode == BURNER_CART_MODE_GBA) {
        uint8_t gba_id[8] = {0};

        err = burner_bacon_gba_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_gba_probe_locked(
                gba_id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok);
        }
        if (err == ESP_OK && !cfi_ok) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA capacity probe: CFI unavailable: flash=%" PRIu32
                " sector=%" PRIu32 " buf=%u cmdset=%s id=%02X %02X %02X %02X %02X %02X %02X %02X",
                device_size,
                sector_size,
                (unsigned)buffer_write_bytes,
                burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
                gba_id[0],
                gba_id[1],
                gba_id[2],
                gba_id[3],
                gba_id[4],
                gba_id[5],
                gba_id[6],
                gba_id[7]);
        }
    } else {
        uint8_t mbc5_id[4] = {0};
        burner_nor_cmdset_t mbc5_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

        err = burner_bacon_mbc5_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_probe_locked(
                mbc5_id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok,
                &mbc5_cmdset);
        }
        if (err == ESP_OK && !cfi_ok) {
            ESP_LOGW(
                BURNER_TAG,
                "GB capacity probe: mapper=%s CFI unavailable: flash=%" PRIu32
                " sector=%" PRIu32 " buf=%u cmdset=%s id=%02X %02X %02X %02X",
                burner_gb_mapper_name(s_gb_mapper_kind),
                device_size,
                sector_size,
                (unsigned)buffer_write_bytes,
                burner_nor_cmdset_name(mbc5_cmdset),
                mbc5_id[0],
                mbc5_id[1],
                mbc5_id[2],
                mbc5_id[3]);
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        return err;
    }

    *device_size_out = device_size;
    return ESP_OK;
}

static esp_err_t burner_bacon_gba_reset_aso_locked(void)
{
    esp_err_t err;

    err = burner_bacon_gba_command_write_u16(0x000u, 0x0090u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(0x000u, 0x0000u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gba_reset_to_read_mode();
}

static esp_err_t burner_bacon_mbc5_reset_aso_locked(void)
{
    static const uint8_t cmd_90 = 0x90u;
    static const uint8_t cmd_00 = 0x00u;
    static const uint8_t cmd_f0 = 0xF0u;
    esp_err_t err;

    err = burner_bacon_gbc_write(0x0000u, &cmd_90, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0000u, &cmd_00, 1u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gbc_write(0x0000u, &cmd_f0, 1u);
}

static esp_err_t burner_bacon_gba_get_ppb_lock_status_locked(uint16_t *lock_status_out)
{
    esp_err_t err;

    if (lock_status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0050u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_read_u16(0x000u, lock_status_out);
    if (err != ESP_OK) {
        return err;
    }
    *lock_status_out = burner_apply_d0d1_swap_on_read(*lock_status_out, s_cart_ctx.d0d1_swapped);
    return burner_bacon_gba_reset_aso_locked();
}

static esp_err_t burner_bacon_mbc5_get_ppb_lock_status_locked(uint8_t *lock_status_out)
{
    static const uint8_t cmd_aa = 0xAAu;
    static const uint8_t cmd_55 = 0x55u;
    static const uint8_t cmd_50 = 0x50u;
    esp_err_t err;

    if (lock_status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_aa, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0555u, &cmd_55, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_50, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_read_u8(0x0000u, lock_status_out);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_mbc5_reset_aso_locked();
}

static esp_err_t burner_bacon_gba_scan_ppb_locked(
    uint32_t device_size,
    uint32_t sector_size,
    uint32_t *needs_unlock_count_out)
{
    uint32_t sector_count;
    uint32_t sector_idx;
    uint16_t ppb = 0u;
    esp_err_t err;

    if (needs_unlock_count_out == NULL || sector_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    *needs_unlock_count_out = 0u;
    sector_count = burner_erase_sector_count_from_bytes(device_size, sector_size);
    for (sector_idx = 0u; sector_idx < sector_count; ++sector_idx) {
        uint32_t sector_addr = sector_idx * sector_size;
        uint32_t ppb_addr = sector_addr;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        if (device_size > BURN_GBA_LINEAR_ADDR_BYTES) {
            uint32_t bank = 0u;
            burner_gba_resolve_write_addr(sector_addr, true, &bank, NULL);
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00C0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(ppb_addr >> 1, &ppb);
        if (err != ESP_OK) {
            return err;
        }
        ppb = burner_apply_d0d1_swap_on_read(ppb, s_cart_ctx.d0d1_swapped);
        err = burner_bacon_gba_reset_aso_locked();
        if (err != ESP_OK) {
            return err;
        }

        if (ppb != 0x0001u) {
            (*needs_unlock_count_out)++;
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_scan_ppb_locked(
    uint32_t device_size,
    uint32_t sector_size,
    uint32_t *needs_unlock_count_out)
{
    uint32_t sector_count;
    uint32_t sector_idx;
    uint16_t current_bank = UINT16_MAX;
    esp_err_t err;

    if (needs_unlock_count_out == NULL || sector_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    *needs_unlock_count_out = 0u;
    sector_count = burner_erase_sector_count_from_bytes(device_size, sector_size);
    for (sector_idx = 0u; sector_idx < sector_count; ++sector_idx) {
        uint32_t sector_addr = sector_idx * sector_size;
        uint16_t bank = 0u;
        uint16_t cart_addr = 0u;
        uint8_t ppb = 0u;
        static const uint8_t cmd_aa = 0xAAu;
        static const uint8_t cmd_55 = 0x55u;
        static const uint8_t cmd_c0 = 0xC0u;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        burner_mbc5_addr_to_program_window(sector_addr, &bank, &cart_addr, NULL);
        if (bank != current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        err = burner_bacon_gbc_write(0x0AAAu, &cmd_aa, 1u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_write(0x0555u, &cmd_55, 1u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_write(0x0AAAu, &cmd_c0, 1u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(cart_addr, &ppb);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_mbc5_reset_aso_locked();
        if (err != ESP_OK) {
            return err;
        }

        if (ppb != 0x01u) {
            (*needs_unlock_count_out)++;
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_all_ppb_erase_locked(void)
{
    esp_err_t err;

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00C0u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(0x000u, 0x0080u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(0x000u, 0x0030u);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    return burner_bacon_gba_reset_aso_locked();
}

static esp_err_t burner_bacon_mbc5_all_ppb_erase_locked(void)
{
    static const uint8_t cmd_aa = 0xAAu;
    static const uint8_t cmd_55 = 0x55u;
    static const uint8_t cmd_c0 = 0xC0u;
    static const uint8_t cmd_80 = 0x80u;
    static const uint8_t cmd_30 = 0x30u;
    esp_err_t err;

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_aa, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0555u, &cmd_55, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_c0, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0000u, &cmd_80, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0000u, &cmd_30, 1u);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    return burner_bacon_mbc5_reset_aso_locked();
}

esp_err_t burner_cart_unlock_ppb_locked(
    burner_cart_mode_t cart_mode,
    burner_ppb_unlock_report_t *report)
{
    esp_err_t err = ESP_OK;

    if (report == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(report, 0, sizeof(*report));
    report->cart_mode = cart_mode;

    if (cart_mode == BURNER_CART_MODE_GBA) {
        err = burner_bacon_gba_prepare_power();
        if (err != ESP_OK) {
            return err;
        }

        err = burner_bacon_gba_probe_locked(
            report->gba_id,
            &report->device_size,
            &report->sector_size,
            &report->buffer_write_bytes,
            &report->cfi_ok);
        if (err != ESP_OK) {
            return err;
        }
        report->gba_d0d1_known = s_cart_ctx.d0d1_known;
        report->gba_d0d1_swapped = s_cart_ctx.d0d1_swapped;
        if (report->device_size == 0u || report->sector_size == 0u) {
            return ESP_ERR_INVALID_SIZE;
        }

        report->sector_count = burner_erase_sector_count_from_bytes(report->device_size, report->sector_size);

        err = burner_bacon_gba_get_ppb_lock_status_locked(&report->gba_lock_status);
        if (err != ESP_OK) {
            return err;
        }
        if (report->gba_lock_status != 0x0001u) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        err = burner_bacon_gba_scan_ppb_locked(
            report->device_size,
            report->sector_size,
            &report->ppb_needs_unlock_before);
        if (err != ESP_OK) {
            return err;
        }

        err = burner_bacon_gba_all_ppb_erase_locked();
        if (err != ESP_OK) {
            return err;
        }

        return burner_bacon_gba_scan_ppb_locked(
            report->device_size,
            report->sector_size,
            &report->ppb_needs_unlock_after);
    }

    err = burner_bacon_mbc5_prepare_power();
    if (err != ESP_OK) {
        return err;
    }

    {
        burner_nor_cmdset_t mbc5_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

        err = burner_bacon_mbc5_probe_locked(
        report->mbc5_id,
        &report->device_size,
        &report->sector_size,
        &report->buffer_write_bytes,
        &report->cfi_ok,
        &mbc5_cmdset);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (report->device_size == 0u || report->sector_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    report->sector_count = burner_erase_sector_count_from_bytes(report->device_size, report->sector_size);

    err = burner_bacon_mbc5_get_ppb_lock_status_locked(&report->mbc5_lock_status);
    if (err != ESP_OK) {
        return err;
    }
    if (report->mbc5_lock_status != 0x01u) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = burner_bacon_mbc5_scan_ppb_locked(
        report->device_size,
        report->sector_size,
        &report->ppb_needs_unlock_before);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_mbc5_all_ppb_erase_locked();
    if (err != ESP_OK) {
        return err;
    }

    return burner_bacon_mbc5_scan_ppb_locked(
        report->device_size,
        report->sector_size,
        &report->ppb_needs_unlock_after);
}

static void burner_emit_progress_cb(int progress, uint32_t processed)
{
    if (s_receive_cb != NULL) {
        uint8_t cb_payload[4];
        cb_payload[0] = (uint8_t)progress;
        cb_payload[1] = (uint8_t)((processed >> 8) & 0xFF);
        cb_payload[2] = (uint8_t)(processed & 0xFF);
        cb_payload[3] = 0;
        s_receive_cb(cb_payload, sizeof(cb_payload));
    }
}

esp_err_t burner_bacon_mbc5_read_block(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = (uint16_t)(rom_addr >> 14);
        uint16_t bank_off = (uint16_t)(rom_addr & 0x3FFFu);
        uint16_t cart_addr;
        size_t remain = len - copied;
        size_t bank_remain = 0x4000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        if (bank == 0u) {
            cart_addr = bank_off;
        } else {
            cart_addr = (uint16_t)(0x4000u + bank_off);
        }

        err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }
        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_read_block_program_window(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0u;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = 0u;
        uint16_t cart_addr = 0u;
        uint32_t bank_off = 0u;
        size_t remain = len - copied;
        size_t bank_remain;
        size_t chunk;

        burner_mbc5_addr_to_program_window(rom_addr, &bank, &cart_addr, &bank_off);
        bank_remain = BURN_MBC5_ROM_BANK_BYTES - bank_off;
        chunk = (remain < bank_remain) ? remain : bank_remain;
        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }
        copied += chunk;
    }

    return ESP_OK;
}

esp_err_t burner_bacon_mbc5_read_block_hoststyle(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0u;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = (uint16_t)(rom_addr >> 14);
        uint16_t bank_off = (uint16_t)(rom_addr & 0x3FFFu);
        uint16_t cart_addr;
        size_t remain = len - copied;
        size_t bank_remain = 0x4000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        if (bank == 0u) {
            cart_addr = bank_off;
        } else {
            cart_addr = (uint16_t)(0x4000u + bank_off);
        }

        err = burner_bacon_gbc_read_stream_hoststyle(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }

        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_ram_write_block(
    const uint8_t *data,
    size_t len,
    uint32_t offset,
    bool fram_mode,
    uint8_t fram_latency)
{
    size_t written = 0;
    uint8_t current_bank = 0xFFu;
    esp_err_t err;

    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (written < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t ram_addr = offset + (uint32_t)written;
        uint8_t bank = (uint8_t)(ram_addr >> 13);
        uint16_t bank_off = (uint16_t)(ram_addr & 0x1FFFu);
        uint16_t cart_addr = (uint16_t)(0xA000u + bank_off);
        size_t remain = len - written;
        size_t bank_remain = 0x2000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_WRITE_MAX_BYTES) {
            chunk = BURN_CART_WRITE_MAX_BYTES;
        }

        if (bank != current_bank) {
            err = burner_bacon_mbc5_ram_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        if (fram_mode) {
            err = burner_bacon_gbc_write_for_fram(cart_addr, data + written, chunk, fram_latency);
        } else {
            err = burner_bacon_gbc_write(cart_addr, data + written, chunk);
        }
        if (err != ESP_OK) {
            return err;
        }

        written += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_ram_read_block(
    uint8_t *out,
    size_t len,
    uint32_t offset,
    bool fram_mode,
    uint8_t fram_latency)
{
    size_t copied = 0;
    uint8_t current_bank = 0xFFu;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t ram_addr = offset + (uint32_t)copied;
        uint8_t bank = (uint8_t)(ram_addr >> 13);
        uint16_t bank_off = (uint16_t)(ram_addr & 0x1FFFu);
        uint16_t cart_addr = (uint16_t)(0xA000u + bank_off);
        size_t remain = len - copied;
        size_t bank_remain = 0x2000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        if (bank != current_bank) {
            err = burner_bacon_mbc5_ram_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        if (fram_mode) {
            err = burner_bacon_gbc_read_for_fram(cart_addr, out + copied, chunk, fram_latency);
        } else {
            err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        }
        if (err != ESP_OK) {
            return err;
        }

        copied += chunk;
    }

    return ESP_OK;
}

esp_err_t burner_ensure_dump_dir(void)
{
    struct stat st;

    if (stat(DUMP_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(DUMP_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t burner_ensure_rom_output_dir(void)
{
    struct stat st;

    if (stat(ROM_OUTPUT_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(ROM_OUTPUT_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static bool burner_dump_stage_build_dir_rel(
    const char *rom_name,
    char *stage_rel,
    size_t stage_rel_len)
{
    int n;

    if (rom_name == NULL || rom_name[0] == '\0' || stage_rel == NULL || stage_rel_len < 2u) {
        return false;
    }

    n = snprintf(stage_rel, stage_rel_len, ROM_OUTPUT_TEMP_ROOT_REL "/%s.parts", rom_name);
    return n > 0 && n < (int)stage_rel_len;
}

static esp_err_t burner_dump_stage_prepare(
    const char *rom_name,
    char *stage_rel,
    size_t stage_rel_len,
    char *stage_full,
    size_t stage_full_len)
{
    struct stat st;

    if (!burner_dump_stage_build_dir_rel(rom_name, stage_rel, stage_rel_len) ||
        !burner_build_full_path(stage_rel, stage_full, stage_full_len)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (stat(stage_full, &st) == 0) {
        if (burner_remove_recursive(stage_full) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    return burner_mkdirs_rel(stage_rel);
}

static bool burner_dump_stage_build_fragment_rel(
    const char *stage_rel,
    const char *rom_name,
    uint32_t fragment_index,
    char *fragment_rel,
    size_t fragment_rel_len)
{
    int n;

    if (stage_rel == NULL || stage_rel[0] == '\0' || rom_name == NULL || rom_name[0] == '\0' ||
        fragment_rel == NULL || fragment_rel_len < 2u) {
        return false;
    }

    n = snprintf(
        fragment_rel,
        fragment_rel_len,
        "%s/%s.part%03" PRIu32,
        stage_rel,
        rom_name,
        fragment_index);
    return n > 0 && n < (int)fragment_rel_len;
}

static esp_err_t burner_dump_stage_write_fragment(
    const char *stage_rel,
    const char *rom_name,
    uint32_t fragment_index,
    const uint8_t *data,
    size_t data_len)
{
    char fragment_rel[TF_PATH_LEN_MAX] = {0};
    char fragment_full[TF_PATH_LEN_MAX + 64] = {0};
    FILE *fp = NULL;

    if (!burner_dump_stage_build_fragment_rel(
            stage_rel,
            rom_name,
            fragment_index,
            fragment_rel,
            sizeof(fragment_rel)) ||
        !burner_build_full_path(fragment_rel, fragment_full, sizeof(fragment_full))) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (burner_cancel_poll() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    fp = fopen(fragment_full, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }
    if (fwrite(data, 1, data_len, fp) != data_len) {
        fclose(fp);
        unlink(fragment_full);
        return ESP_FAIL;
    }
    fclose(fp);
    return ESP_OK;
}

static esp_err_t burner_replace_file(const char *tmp_path, const char *target_path)
{
    if (tmp_path == NULL || target_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (rename(tmp_path, target_path) == 0) {
        (void)burner_apply_current_file_mtime(target_path, NULL);
        return ESP_OK;
    }
    if (errno == EEXIST) {
        if (unlink(target_path) == 0 && rename(tmp_path, target_path) == 0) {
            (void)burner_apply_current_file_mtime(target_path, NULL);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static uint8_t *burner_attach_stdio_buffer(FILE *fp, size_t preferred_size)
{
    uint8_t *buf;

    if (fp == NULL || preferred_size == 0u) {
        return NULL;
    }

    buf = (uint8_t *)heap_caps_malloc(preferred_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)malloc(preferred_size);
    }
    if (buf == NULL) {
        buf = (uint8_t *)heap_caps_malloc(preferred_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        return NULL;
    }

    if (setvbuf(fp, (char *)buf, _IOFBF, preferred_size) != 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

static esp_err_t burner_dump_stage_merge_fragments(
    const char *stage_rel,
    const char *rom_name,
    uint32_t fragment_count,
    const char *target_path)
{
    char fragment_rel[TF_PATH_LEN_MAX] = {0};
    char fragment_full[TF_PATH_LEN_MAX + 64] = {0};
    char tmp_target[TF_PATH_LEN_MAX + 96] = {0};
    uint8_t *copy_buf = NULL;
    FILE *out_fp = NULL;
    esp_err_t err = ESP_OK;
    uint32_t index;

    if (stage_rel == NULL || rom_name == NULL || fragment_count == 0u || target_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(tmp_target, sizeof(tmp_target), "%s.merge_tmp", target_path) >= (int)sizeof(tmp_target)) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_fp = fopen(tmp_target, "wb");
    if (out_fp == NULL) {
        return ESP_FAIL;
    }

    copy_buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (copy_buf == NULL) {
        fclose(out_fp);
        unlink(tmp_target);
        return ESP_ERR_NO_MEM;
    }

    for (index = 0u; index < fragment_count && err == ESP_OK; ++index) {
        FILE *in_fp;

        if (burner_cancel_poll() != ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (usb_msc_tf_in_use_by_host()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!burner_dump_stage_build_fragment_rel(
                stage_rel,
                rom_name,
                index,
                fragment_rel,
                sizeof(fragment_rel)) ||
            !burner_build_full_path(fragment_rel, fragment_full, sizeof(fragment_full))) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        in_fp = fopen(fragment_full, "rb");
        if (in_fp == NULL) {
            err = ESP_FAIL;
            break;
        }

        while (err == ESP_OK) {
            size_t read_len = fread(copy_buf, 1, TF_IO_CHUNK_SIZE, in_fp);

            if (burner_cancel_poll() != ESP_OK) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            if (usb_msc_tf_in_use_by_host()) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }

            if (read_len == 0u) {
                break;
            }
            if (fwrite(copy_buf, 1, read_len, out_fp) != read_len) {
                err = ESP_FAIL;
                break;
            }
        }
        if (err == ESP_OK && ferror(in_fp)) {
            err = ESP_FAIL;
        }
        fclose(in_fp);
    }

    free(copy_buf);

    if (fclose(out_fp) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        unlink(tmp_target);
        return err;
    }

    return burner_replace_file(tmp_target, target_path);
}

typedef esp_err_t (*burner_dump_read_block_fn_t)(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job);

static esp_err_t burner_dump_read_block_mbc5(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job)
{
    esp_err_t err;

    (void)job;

    burner_spi_lock_take();
    /* Use a dedicated Bacon-style streaming ROM read for MBC5 dump/export. */
    err = burner_bacon_mbc5_read_block_hoststyle(dst, len, addr);
    burner_spi_lock_give();

    return err;
}

static esp_err_t burner_dump_read_block_gba(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job)
{
    esp_err_t err;

    burner_spi_lock_take();
    /* Use Bacon hoststyle ROM reads for GBA dump/export. */
    err = burner_bacon_gba_verify_read_block_hoststyle(dst, len, addr, burner_is_gba_multi_card(job));
    burner_spi_lock_give();

    return err;
}

static int burner_dump_stage_progress(uint32_t processed, uint32_t total)
{
    int progress = burner_calc_progress_percent_u64(processed, total);

    if (processed >= total) {
        return 99;
    }
    if (progress > 99) {
        return 99;
    }
    return progress;
}

static esp_err_t __attribute__((unused)) burner_run_read_job_staged(
    const burner_task_param_t *job,
    uint32_t work_total,
    uint32_t chunk_bytes,
    burner_dump_read_block_fn_t read_block,
    const char *cache_msg,
    const char *flush_msg,
    const char *merge_msg,
    const char *alloc_fail_msg,
    const char *read_fail_msg)
{
    uint8_t *psram_stage_buf = NULL;
    uint32_t processed = 0;
    size_t stage_capacity = 0u;
    size_t staged_bytes = 0u;
    esp_err_t err = ESP_OK;
    char stage_rel[TF_PATH_LEN_MAX] = {0};
    char stage_full[TF_PATH_LEN_MAX + 64] = {0};
    uint32_t fragment_count = 0u;
    bool stage_ready = false;

    if (job == NULL || work_total == 0u || chunk_bytes == 0u || read_block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    stage_capacity = (work_total < burner_psram_window_mb_to_bytes(BURN_READ_PSRAM_FRAGMENT_MB))
                         ? (size_t)work_total
                         : (size_t)burner_psram_window_mb_to_bytes(BURN_READ_PSRAM_FRAGMENT_MB);
    if (stage_capacity == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        work_total,
        cache_msg,
        job->rom_name,
        job->rom_path);

    err = burner_dump_stage_prepare(job->rom_name, stage_rel, sizeof(stage_rel), stage_full, sizeof(stage_full));
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            "prepare temp dump dir failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    stage_ready = true;

    psram_stage_buf = (uint8_t *)heap_caps_malloc(stage_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_stage_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            alloc_fail_msg,
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto staged_dump_done;
    }

    while (processed < work_total) {
        size_t read_len = (size_t)(work_total - processed);
        int progress;

        if (read_len > chunk_bytes) {
            read_len = chunk_bytes;
        }

        if ((staged_bytes + read_len) > stage_capacity && staged_bytes > 0u) {
            burner_status_update(
                BURNER_STATE_BURNING,
                burner_dump_stage_progress(processed, work_total),
                processed,
                work_total,
                flush_msg,
                job->rom_name,
                job->rom_path);
            err = burner_dump_stage_write_fragment(
                stage_rel,
                job->rom_name,
                fragment_count,
                psram_stage_buf,
                staged_bytes);
            if (err != ESP_OK) {
                if (!burner_cancel_is_requested()) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_dump_stage_progress(processed, work_total),
                        processed,
                        work_total,
                        "write temp fragment failed",
                        job->rom_name,
                        job->rom_path);
                }
                break;
            }
            fragment_count++;
            staged_bytes = 0u;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_dump_stage_progress(processed, work_total),
                processed,
                work_total,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        err = read_block(psram_stage_buf + staged_bytes, read_len, job->addr_begin + processed, job);
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_dump_stage_progress(processed, work_total),
                    processed,
                    work_total,
                    read_fail_msg,
                    job->rom_name,
                    job->rom_path);
            }
            break;
        }

        staged_bytes += read_len;
        processed += (uint32_t)read_len;
        progress = burner_dump_stage_progress(processed, work_total);
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            work_total,
            cache_msg,
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

    if (err == ESP_OK && staged_bytes > 0u) {
        burner_status_update(
            BURNER_STATE_BURNING,
            burner_dump_stage_progress(processed, work_total),
            processed,
            work_total,
            flush_msg,
            job->rom_name,
            job->rom_path);
        err = burner_dump_stage_write_fragment(
            stage_rel,
            job->rom_name,
            fragment_count,
            psram_stage_buf,
            staged_bytes);
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_dump_stage_progress(processed, work_total),
                    processed,
                    work_total,
                    "write temp fragment failed",
                    job->rom_name,
                    job->rom_path);
            }
        } else {
            fragment_count++;
            staged_bytes = 0u;
        }
    }

    if (err == ESP_OK) {
        burner_status_update(
            BURNER_STATE_BURNING,
            99,
            processed,
            work_total,
            merge_msg,
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(99, processed);
        err = burner_dump_stage_merge_fragments(stage_rel, job->rom_name, fragment_count, job->rom_path);
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    99,
                    processed,
                    work_total,
                    "merge dump file failed",
                    job->rom_name,
                    job->rom_path);
            }
        }
    }

staged_dump_done:
    if (psram_stage_buf != NULL) {
        free(psram_stage_buf);
    }
    if (stage_ready) {
        (void)burner_remove_recursive(stage_full);
    }
    return err;
}

static esp_err_t burner_run_read_job_direct(
    const burner_task_param_t *job,
    uint32_t work_total,
    uint32_t chunk_bytes,
    burner_dump_read_block_fn_t read_block,
    const char *progress_msg,
    const char *alloc_fail_msg,
    const char *read_fail_msg,
    const char *write_fail_msg)
{
    uint8_t *dump_buf[2] = {NULL, NULL};
    burner_tf_writer_ctx_t tf_writer = {0};
    uint32_t read_offset = 0u;
    uint32_t processed = 0u;
    uint32_t pending_write_bytes = 0u;
    size_t buf_size = 0u;
    size_t buf_count = 0u;
    size_t slot_fill[2] = {0u, 0u};
    size_t fill_slot = 0u;
    esp_err_t err = ESP_OK;
    int out_fd = -1;
    char tmp_target[TF_PATH_LEN_MAX + 96] = {0};
    uint64_t op_start_us;
    uint64_t op_elapsed_us;
    bool writer_inflight = false;

    if (job == NULL || work_total == 0u || chunk_bytes == 0u || read_block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(tmp_target, sizeof(tmp_target), "%s.dump_tmp", job->rom_path) >= (int)sizeof(tmp_target)) {
        return ESP_ERR_INVALID_SIZE;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        work_total,
        progress_msg,
        job->rom_name,
        job->rom_path);

    out_fd = open(tmp_target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            write_fail_msg,
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
    }

    buf_size = (work_total < chunk_bytes) ? (size_t)work_total : (size_t)chunk_bytes;
    dump_buf[0] = (uint8_t *)malloc(buf_size);
    if (dump_buf[0] != NULL) {
        dump_buf[1] = (uint8_t *)malloc(buf_size);
        buf_count = (dump_buf[1] != NULL) ? 2u : 1u;
    }
    if (dump_buf[0] == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            alloc_fail_msg,
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto direct_dump_done;
    }
    if (buf_count == 0u) {
        buf_count = 1u;
    }

    err = burner_tf_writer_start(&tf_writer, out_fd);
    if (err != ESP_OK) {
        memset(&tf_writer, 0, sizeof(tf_writer));
        tf_writer.fd = out_fd;
        err = ESP_OK;
    }

    while (read_offset < work_total) {
        size_t read_len = (size_t)(work_total - read_offset);
        size_t free_space = buf_size - slot_fill[fill_slot];
        int progress;
        uint8_t *read_buf = dump_buf[fill_slot] + slot_fill[fill_slot];

        if (writer_inflight && buf_count < 2u) {
            err = burner_tf_writer_wait(&tf_writer);
            writer_inflight = false;
            if (err != ESP_OK) {
                if (!burner_cancel_is_requested()) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, work_total),
                        processed,
                        work_total,
                        write_fail_msg,
                        job->rom_name,
                        job->rom_path);
                }
                goto direct_dump_done;
            }
            processed += pending_write_bytes;
            pending_write_bytes = 0u;
        }

        if (free_space == 0u) {
            if (writer_inflight) {
                err = burner_tf_writer_wait(&tf_writer);
                writer_inflight = false;
                if (err != ESP_OK) {
                    if (!burner_cancel_is_requested()) {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            burner_calc_progress_percent_u64(processed, work_total),
                            processed,
                            work_total,
                            write_fail_msg,
                            job->rom_name,
                            job->rom_path);
                    }
                    goto direct_dump_done;
                }
                processed += pending_write_bytes;
                pending_write_bytes = 0u;
            }

            err = burner_tf_writer_submit(&tf_writer, dump_buf[fill_slot], slot_fill[fill_slot]);
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_calc_progress_percent_u64(processed, work_total),
                    processed,
                    work_total,
                    write_fail_msg,
                    job->rom_name,
                    job->rom_path);
                goto direct_dump_done;
            }

            if (tf_writer.running && tf_writer.request != NULL && tf_writer.done != NULL) {
                writer_inflight = true;
                pending_write_bytes = (uint32_t)slot_fill[fill_slot];
            } else {
                processed += (uint32_t)slot_fill[fill_slot];
            }

            slot_fill[fill_slot] = 0u;
            if (buf_count > 1u) {
                fill_slot = (fill_slot + 1u) % buf_count;
            }
            continue;
        }

        if (read_len > (size_t)chunk_bytes) {
            read_len = (size_t)chunk_bytes;
        }
        if (read_len > free_space) {
            read_len = free_space;
        }

        if (burner_cancel_poll() != ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
            goto direct_dump_done;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_calc_progress_percent_u64(processed, work_total),
                processed,
                work_total,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            goto direct_dump_done;
        }

        op_start_us = (uint64_t)esp_timer_get_time();
        err = read_block(read_buf, read_len, job->addr_begin + read_offset, job);
        op_elapsed_us = (uint64_t)esp_timer_get_time();
        if (op_elapsed_us > op_start_us) {
            burner_status_record_dump_read((uint32_t)read_len, op_elapsed_us - op_start_us);
        }
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_calc_progress_percent_u64(processed, work_total),
                    processed,
                    work_total,
                    read_fail_msg,
                    job->rom_name,
                    job->rom_path);
            }
            goto direct_dump_done;
        }

        read_offset += (uint32_t)read_len;
        slot_fill[fill_slot] += read_len;

        if (slot_fill[fill_slot] == buf_size || read_offset >= work_total) {
            if (writer_inflight) {
                err = burner_tf_writer_wait(&tf_writer);
                writer_inflight = false;
                if (err != ESP_OK) {
                    if (!burner_cancel_is_requested()) {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            burner_calc_progress_percent_u64(processed, work_total),
                            processed,
                            work_total,
                            write_fail_msg,
                            job->rom_name,
                            job->rom_path);
                    }
                    goto direct_dump_done;
                }
                processed += pending_write_bytes;
                pending_write_bytes = 0u;
            }

            err = burner_tf_writer_submit(&tf_writer, dump_buf[fill_slot], slot_fill[fill_slot]);
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_calc_progress_percent_u64(processed, work_total),
                    processed,
                    work_total,
                    write_fail_msg,
                    job->rom_name,
                    job->rom_path);
                goto direct_dump_done;
            }

            if (tf_writer.running && tf_writer.request != NULL && tf_writer.done != NULL) {
                writer_inflight = true;
                pending_write_bytes = (uint32_t)slot_fill[fill_slot];
            } else {
                processed += (uint32_t)slot_fill[fill_slot];
            }

            slot_fill[fill_slot] = 0u;
            if (buf_count > 1u) {
                fill_slot = (fill_slot + 1u) % buf_count;
            }
        }

        progress = burner_calc_progress_percent_u64(processed, work_total);
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            work_total,
            progress_msg,
            job->rom_name,
            job->rom_path);
    }

    if (slot_fill[fill_slot] > 0u) {
        if (writer_inflight) {
            err = burner_tf_writer_wait(&tf_writer);
            writer_inflight = false;
            if (err != ESP_OK) {
                if (!burner_cancel_is_requested()) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, work_total),
                        processed,
                        work_total,
                        write_fail_msg,
                        job->rom_name,
                        job->rom_path);
                }
                goto direct_dump_done;
            }
            processed += pending_write_bytes;
            pending_write_bytes = 0u;
        }

        err = burner_tf_writer_submit(&tf_writer, dump_buf[fill_slot], slot_fill[fill_slot]);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_calc_progress_percent_u64(processed, work_total),
                processed,
                work_total,
                write_fail_msg,
                job->rom_name,
                job->rom_path);
            goto direct_dump_done;
        }

        if (tf_writer.running && tf_writer.request != NULL && tf_writer.done != NULL) {
            writer_inflight = true;
            pending_write_bytes = (uint32_t)slot_fill[fill_slot];
        } else {
            processed += (uint32_t)slot_fill[fill_slot];
        }
    }

    if (writer_inflight) {
        err = burner_tf_writer_wait(&tf_writer);
        writer_inflight = false;
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_calc_progress_percent_u64(processed, work_total),
                processed,
                work_total,
                write_fail_msg,
                job->rom_name,
                job->rom_path);
            goto direct_dump_done;
        }
        processed += pending_write_bytes;
        pending_write_bytes = 0u;
        burner_status_update(
            BURNER_STATE_BURNING,
            burner_calc_progress_percent_u64(processed, work_total),
            processed,
            work_total,
            progress_msg,
            job->rom_name,
            job->rom_path);
    }

    op_start_us = (uint64_t)esp_timer_get_time();
    if (close(out_fd) != 0) {
        out_fd = -1;
        op_elapsed_us = (uint64_t)esp_timer_get_time();
        if (op_elapsed_us > op_start_us) {
            burner_status_record_dump_finalize(op_elapsed_us - op_start_us);
        }
        burner_status_update(
            BURNER_STATE_ERROR,
            burner_calc_progress_percent_u64(processed, work_total),
            processed,
            work_total,
            write_fail_msg,
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto direct_dump_done;
    }
    out_fd = -1;

    err = burner_replace_file(tmp_target, job->rom_path);
    op_elapsed_us = (uint64_t)esp_timer_get_time();
    if (op_elapsed_us > op_start_us) {
        burner_status_record_dump_finalize(op_elapsed_us - op_start_us);
    }
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            burner_calc_progress_percent_u64(processed, work_total),
            processed,
            work_total,
            write_fail_msg,
            job->rom_name,
            job->rom_path);
        goto direct_dump_done;
    }

direct_dump_done:
    if (writer_inflight) {
        (void)burner_tf_writer_wait(&tf_writer);
        writer_inflight = false;
    }
    burner_tf_writer_stop(&tf_writer);
    if (out_fd >= 0) {
        close(out_fd);
    }
    if (err != ESP_OK) {
        unlink(tmp_target);
    }
    if (dump_buf[0] != NULL) {
        free(dump_buf[0]);
    }
    if (dump_buf[1] != NULL) {
        free(dump_buf[1]);
    }

    return err;
}

typedef struct {
    FILE *fp;
    uint8_t *dst;
    size_t bytes;
    size_t read_len;
    esp_err_t err;
    SemaphoreHandle_t done;
} burner_tf_prefetch_ctx_t;

typedef enum {
    BURNER_ERASE_OP_MBC5_RANGE = 0,
    BURNER_ERASE_OP_GBA_RANGE,
    BURNER_ERASE_OP_MBC5_CHIP,
    BURNER_ERASE_OP_GBA_CHIP,
} burner_erase_op_t;

typedef struct {
    burner_erase_op_t op;
    uint32_t addr_begin;
    uint32_t addr_end;
    uint32_t sector_size;
    bool gba_multi;
    bool sample_blank_sectors;
    bool erase_always;
    esp_err_t err;
    SemaphoreHandle_t done;
} burner_erase_task_ctx_t;

typedef struct {
    FILE *fp;
    uint8_t *dst;
    size_t bytes;
    size_t read_len;
    esp_err_t err;
    bool stop;
    bool running;
    TaskHandle_t task;
    SemaphoreHandle_t request;
    SemaphoreHandle_t done;
} burner_tf_reader_ctx_t;

static esp_err_t burner_tf_read_exact(FILE *fp, uint8_t *dst, size_t bytes)
{
    size_t read_len;

    if (fp == NULL || dst == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    read_len = fread(dst, 1, bytes, fp);
    if (read_len != bytes) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t burner_tf_write_exact(int fd, const uint8_t *src, size_t bytes)
{
    size_t offset = 0u;
    uint64_t write_start_us;
    uint64_t write_elapsed_us;

    if (fd < 0 || src == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    write_start_us = (uint64_t)esp_timer_get_time();
    while (offset < bytes) {
        ssize_t written = write(fd, src + offset, bytes - offset);
        if (written <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)written;
        if (offset < bytes) {
            if (burner_cancel_is_requested()) {
                return ESP_ERR_INVALID_STATE;
            }
            if (usb_msc_tf_in_use_by_host()) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    write_elapsed_us = (uint64_t)esp_timer_get_time();
    if (offset == bytes && write_elapsed_us > write_start_us) {
        burner_status_record_dump_write((uint32_t)bytes, write_elapsed_us - write_start_us);
    }

    return ESP_OK;
}

static void burner_tf_prefetch_task(void *arg)
{
    burner_tf_prefetch_ctx_t *ctx = (burner_tf_prefetch_ctx_t *)arg;
    uint64_t read_start_us = 0u;
    uint64_t read_elapsed_us = 0u;

    if (ctx == NULL || ctx->fp == NULL || ctx->dst == NULL || ctx->bytes == 0u || ctx->done == NULL) {
        if (ctx != NULL) {
            ctx->err = ESP_ERR_INVALID_ARG;
        }
        vTaskDelete(NULL);
        return;
    }

    if (usb_msc_tf_in_use_by_host()) {
        ctx->err = ESP_ERR_INVALID_STATE;
    } else {
        read_start_us = (uint64_t)esp_timer_get_time();
        ctx->read_len = fread(ctx->dst, 1, ctx->bytes, ctx->fp);
        read_elapsed_us = (uint64_t)esp_timer_get_time() - read_start_us;
        ctx->err = (ctx->read_len == ctx->bytes) ? ESP_OK : ESP_FAIL;
        if (ctx->err == ESP_OK && ctx->read_len > 0u && read_elapsed_us > 0u) {
            burner_status_record_tf_to_psram_copy((uint32_t)ctx->read_len, read_elapsed_us);
        }
    }

    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

void burner_tf_writer_task(void *arg)
{
    burner_tf_writer_ctx_t *ctx = (burner_tf_writer_ctx_t *)arg;

    if (ctx == NULL || ctx->fd < 0 || ctx->request == NULL || ctx->done == NULL) {
        if (ctx != NULL) {
            ctx->err = ESP_ERR_INVALID_ARG;
        }
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        xSemaphoreTake(ctx->request, portMAX_DELAY);
        if (ctx->stop) {
            ctx->err = ESP_OK;
            xSemaphoreGive(ctx->done);
            break;
        }

        ctx->written = 0u;
        if (ctx->src == NULL || ctx->bytes == 0u) {
            ctx->err = ESP_ERR_INVALID_ARG;
        } else {
            ctx->err = burner_tf_write_exact(ctx->fd, ctx->src, ctx->bytes);
            if (ctx->err == ESP_OK) {
                ctx->written = ctx->bytes;
            }
        }
        xSemaphoreGive(ctx->done);
    }

    vTaskDelete(NULL);
}

static void burner_erase_task(void *arg)
{
    burner_erase_task_ctx_t *ctx = (burner_erase_task_ctx_t *)arg;

    if (ctx == NULL || ctx->done == NULL) {
        vTaskDelete(NULL);
        return;
    }

    burner_spi_lock_take();
    switch (ctx->op) {
    case BURNER_ERASE_OP_MBC5_RANGE:
        ctx->err = burner_bacon_mbc5_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_GBA_RANGE:
        ctx->err = burner_bacon_gba_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->gba_multi,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_MBC5_CHIP:
        ctx->err = burner_bacon_mbc5_chip_erase();
        break;
    case BURNER_ERASE_OP_GBA_CHIP:
        ctx->err = burner_bacon_gba_chip_erase();
        break;
    default:
        ctx->err = ESP_ERR_INVALID_ARG;
        break;
    }
    burner_spi_lock_give();

    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static esp_err_t burner_erase_exec_in_current_task(burner_erase_task_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    burner_spi_lock_take();
    switch (ctx->op) {
    case BURNER_ERASE_OP_MBC5_RANGE:
        ctx->err = burner_bacon_mbc5_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_GBA_RANGE:
        ctx->err = burner_bacon_gba_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->gba_multi,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_MBC5_CHIP:
        ctx->err = burner_bacon_mbc5_chip_erase();
        break;
    case BURNER_ERASE_OP_GBA_CHIP:
        ctx->err = burner_bacon_gba_chip_erase();
        break;
    default:
        ctx->err = ESP_ERR_INVALID_ARG;
        break;
    }
    burner_spi_lock_give();
    return ctx->err;
}

static esp_err_t burner_run_erase_task(burner_erase_task_ctx_t *ctx)
{
    BaseType_t create_ret;

    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_burn_core_cfg.erase_core == BURNER_CORE_AFFINITY_AUTO) {
        return burner_erase_exec_in_current_task(ctx);
    }

    ctx->done = xSemaphoreCreateBinary();
    if (ctx->done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    create_ret = burner_create_task_with_affinity(
        burner_erase_task,
        "burn_erase",
        4096,
        ctx,
        4,
        NULL,
        s_burn_core_cfg.erase_core);
    if (create_ret != pdPASS) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(ctx->done, portMAX_DELAY);
    vSemaphoreDelete(ctx->done);
    ctx->done = NULL;
    return ctx->err;
}

static esp_err_t burner_run_mbc5_range_erase(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool sample_blank_sectors,
    bool erase_always)
{
    uint32_t planned_sectors = burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t planned_bytes = burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_MBC5_RANGE,
        .addr_begin = addr_begin,
        .addr_end = addr_end,
        .sector_size = sector_size,
        .gba_multi = false,
        .sample_blank_sectors = sample_blank_sectors,
        .erase_always = erase_always,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_gba_range_erase(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool gba_multi,
    bool sample_blank_sectors,
    bool erase_always)
{
    uint32_t planned_sectors = burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t planned_bytes = burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_GBA_RANGE,
        .addr_begin = addr_begin,
        .addr_end = addr_end,
        .sector_size = sector_size,
        .gba_multi = gba_multi,
        .sample_blank_sectors = sample_blank_sectors,
        .erase_always = erase_always,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_mbc5_chip_erase(void)
{
    uint32_t planned_sectors =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : burner_erase_sector_count_from_bytes(s_cart_ctx.device_size, s_cart_ctx.sector_size);
    uint32_t planned_bytes =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : s_cart_ctx.device_size;
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_MBC5_CHIP,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_gba_chip_erase(void)
{
    uint32_t planned_sectors =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : burner_erase_sector_count_from_bytes(s_cart_ctx.device_size, s_cart_ctx.sector_size);
    uint32_t planned_bytes =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : s_cart_ctx.device_size;
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_GBA_CHIP,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static void burner_tf_reader_task(void *arg)
{
    burner_tf_reader_ctx_t *ctx = (burner_tf_reader_ctx_t *)arg;

    if (ctx == NULL || ctx->request == NULL || ctx->done == NULL || ctx->fp == NULL) {
        if (ctx != NULL) {
            ctx->running = false;
        }
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        xSemaphoreTake(ctx->request, portMAX_DELAY);
        if (ctx->stop) {
            ctx->running = false;
            xSemaphoreGive(ctx->done);
            break;
        }

        if (ctx->dst == NULL || ctx->bytes == 0u) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_ARG;
            xSemaphoreGive(ctx->done);
            continue;
        }

        if (burner_cancel_is_requested()) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(ctx->done);
            continue;
        }

        if (usb_msc_tf_in_use_by_host()) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(ctx->done);
            continue;
        }

        ctx->read_len = fread(ctx->dst, 1, ctx->bytes, ctx->fp);
        ctx->err = (ctx->read_len == ctx->bytes) ? ESP_OK : ESP_FAIL;
        xSemaphoreGive(ctx->done);
    }

    vTaskDelete(NULL);
}

static esp_err_t burner_tf_reader_start(burner_tf_reader_ctx_t *ctx, FILE *fp)
{
    BaseType_t create_ret;

    if (ctx == NULL || fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fp = fp;

    if (s_burn_core_cfg.tf_core == BURNER_CORE_AFFINITY_AUTO) {
        return ESP_OK;
    }

    ctx->request = xSemaphoreCreateBinary();
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->request == NULL || ctx->done == NULL) {
        if (ctx->request != NULL) {
            vSemaphoreDelete(ctx->request);
            ctx->request = NULL;
        }
        if (ctx->done != NULL) {
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ctx->running = true;
    create_ret = burner_create_task_with_affinity(
        burner_tf_reader_task,
        "tf_reader",
        4096,
        ctx,
        4,
        &ctx->task,
        s_burn_core_cfg.tf_core);
    if (create_ret != pdPASS) {
        ctx->running = false;
        vSemaphoreDelete(ctx->request);
        vSemaphoreDelete(ctx->done);
        ctx->request = NULL;
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t burner_tf_reader_read(burner_tf_reader_ctx_t *ctx, uint8_t *dst, size_t bytes)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_burn_core_cfg.tf_core == BURNER_CORE_AFFINITY_AUTO || !ctx->running || ctx->request == NULL ||
        ctx->done == NULL) {
        return burner_tf_read_exact(ctx->fp, dst, bytes);
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx->dst = dst;
    ctx->bytes = bytes;
    ctx->read_len = 0u;
    ctx->err = ESP_FAIL;
    xSemaphoreGive(ctx->request);
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    if (ctx->err != ESP_OK) {
        return ctx->err;
    }
    return (ctx->read_len == bytes) ? ESP_OK : ESP_FAIL;
}

static void burner_tf_reader_stop(burner_tf_reader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->running && ctx->request != NULL && ctx->done != NULL) {
        ctx->stop = true;
        xSemaphoreGive(ctx->request);
        xSemaphoreTake(ctx->done, portMAX_DELAY);
    }

    if (ctx->request != NULL) {
        vSemaphoreDelete(ctx->request);
        ctx->request = NULL;
    }
    if (ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    ctx->running = false;
}

esp_err_t burner_tf_writer_start(burner_tf_writer_ctx_t *ctx, int fd)
{
    BaseType_t create_ret;

    if (ctx == NULL || fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = fd;

    ctx->request = xSemaphoreCreateBinary();
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->request == NULL || ctx->done == NULL) {
        if (ctx->request != NULL) {
            vSemaphoreDelete(ctx->request);
            ctx->request = NULL;
        }
        if (ctx->done != NULL) {
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ctx->running = true;
    create_ret = burner_create_task_with_affinity(
        burner_tf_writer_task,
        "tf_writer",
        4096,
        ctx,
        4,
        &ctx->task,
        s_burn_core_cfg.tf_core);
    if (create_ret != pdPASS) {
        ctx->running = false;
        vSemaphoreDelete(ctx->request);
        vSemaphoreDelete(ctx->done);
        ctx->request = NULL;
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t burner_tf_writer_submit(burner_tf_writer_ctx_t *ctx, const uint8_t *src, size_t bytes)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->running || ctx->request == NULL || ctx->done == NULL) {
        return burner_tf_write_exact(ctx->fd, src, bytes);
    }

    if (src == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->src = src;
    ctx->bytes = bytes;
    ctx->written = 0u;
    ctx->err = ESP_FAIL;
    xSemaphoreGive(ctx->request);
    return ESP_OK;
}

esp_err_t burner_tf_writer_wait(burner_tf_writer_ctx_t *ctx)
{
    uint64_t wait_start_us;
    uint64_t wait_end_us;

    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->running || ctx->request == NULL || ctx->done == NULL) {
        return ESP_OK;
    }

    wait_start_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    wait_end_us = (uint64_t)esp_timer_get_time();
    if (wait_end_us > wait_start_us) {
        burner_status_record_dump_wait(wait_end_us - wait_start_us);
    }
    if (ctx->err != ESP_OK) {
        return ctx->err;
    }
    return (ctx->written == ctx->bytes) ? ESP_OK : ESP_FAIL;
}

void burner_tf_writer_stop(burner_tf_writer_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->running && ctx->request != NULL && ctx->done != NULL) {
        ctx->stop = true;
        xSemaphoreGive(ctx->request);
        xSemaphoreTake(ctx->done, portMAX_DELAY);
    }

    if (ctx->request != NULL) {
        vSemaphoreDelete(ctx->request);
        ctx->request = NULL;
    }
    if (ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    ctx->running = false;
}

static esp_err_t burner_run_write_job_mbc5(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint8_t *psram_stage_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool should_erase = false;
    bool use_psram_stage = false;
    bool use_pipeline_stage = false;
    bool range_blank = false;
    bool force_erase_sectors = false;
    size_t stage_capacity = 0;
    size_t program_chunk_bytes = BURN_MBC5_PROGRAM_CHUNK_BYTES;
    burner_nor_region_cursor_t pipeline_cursor = {0};
    burner_tf_prefetch_ctx_t prefetch = {0};
    burner_tf_reader_ctx_t tf_reader = {0};
    SemaphoreHandle_t prefetch_done = NULL;
    bool prefetch_inflight = false;
    bool prefetch_started = false;
    bool tf_reader_started = false;
    bool erase_timer_started = false;
    bool write_timer_started = false;
    uint32_t psram_window_mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    uint32_t psram_window_bytes = BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES;
    char direct_copy_msg[64] = {0};
    char direct_program_msg[64] = {0};
    char psram_program_msg[64] = {0};
    char psram_alloc_fail_msg[96] = {0};
    char psram_erase_prefetch_msg[96] = {0};
    char psram_copy_msg[64] = {0};

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    use_psram_stage = (job->write_path == BURNER_WRITE_PATH_PSRAM ||
                       job->write_path == BURNER_WRITE_PATH_PIPELINE);
    use_pipeline_stage = (job->write_path == BURNER_WRITE_PATH_PIPELINE);
    force_erase_sectors = use_pipeline_stage ? true : job->erase_always;
    program_chunk_bytes = (size_t)burner_clamp_mbc5_program_chunk_bytes(job->mbc5_program_chunk_bytes);
    if (use_pipeline_stage) {
        psram_window_mb = BURN_PSRAM_WINDOW_AUTO_MB;
        psram_window_bytes = 0u;
    } else {
        psram_window_mb = burner_psram_window_bytes_to_mb(job->psram_window_bytes);
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    }
    (void)snprintf(
        direct_copy_msg,
        sizeof(direct_copy_msg),
        "direct path: tf->ram (%uKB chunk)",
        (unsigned)(program_chunk_bytes / 1024u));
    (void)snprintf(
        direct_program_msg,
        sizeof(direct_program_msg),
        "direct path: ram->cart (%uKB chunk)",
        (unsigned)(program_chunk_bytes / 1024u));
    (void)snprintf(
        psram_program_msg,
        sizeof(psram_program_msg),
        "%s->cart (%uKB chunk)",
        use_pipeline_stage ? "pipeline psram" : "psram",
        (unsigned)(program_chunk_bytes / 1024u));
    (void)snprintf(
        psram_alloc_fail_msg,
        sizeof(psram_alloc_fail_msg),
        use_pipeline_stage ? "alloc pipeline psram staging failed" : "alloc %uMB psram staging failed",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_erase_prefetch_msg,
        sizeof(psram_erase_prefetch_msg),
        use_pipeline_stage ? "pipeline erase sector + prefetch tf->psram" : "erasing flash sectors (%uMB) + prefetch tf->psram",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_copy_msg,
        sizeof(psram_copy_msg),
        use_pipeline_stage ? "pipeline copy tf->psram (sector window)" : "copy tf->psram (%uMB window)",
        (unsigned)psram_window_mb);
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing cart and preparing flash",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)job->total_bytes) > (uint64_t)s_cart_ctx.device_size) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "selected range exceeds flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }
    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open rom failed",
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
    }

    err = burner_tf_reader_start(&tf_reader, fp);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "create tf reader failed",
            job->rom_name,
            job->rom_path);
        fclose(fp);
        return err;
    }
    tf_reader_started = true;

    if (use_psram_stage) {
        if (use_pipeline_stage) {
            uint32_t pipeline_stage_capacity = 0u;

            err = burner_nor_geometry_largest_sector_size_in_range(
                &s_cart_ctx.geometry,
                addr_begin,
                job->total_bytes,
                &pipeline_stage_capacity);
            if (err != ESP_OK || pipeline_stage_capacity == 0u) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    0,
                    job->total_bytes,
                    "pipeline sector geometry unavailable",
                    job->rom_name,
                    job->rom_path);
                goto write_done;
            }
            stage_capacity = (job->total_bytes < pipeline_stage_capacity)
                                 ? (size_t)job->total_bytes
                                 : (size_t)pipeline_stage_capacity;
        } else {
            stage_capacity = (job->total_bytes < psram_window_bytes)
                                 ? (size_t)job->total_bytes
                                 : (size_t)psram_window_bytes;
        }
        psram_stage_buf = (uint8_t *)heap_caps_malloc(
            stage_capacity,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (psram_stage_buf == NULL) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                psram_alloc_fail_msg,
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_NO_MEM;
            goto write_done;
        }
    } else {
        buf = (uint8_t *)malloc(program_chunk_bytes);
        if (buf == NULL) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "no memory for write chunk",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_NO_MEM;
            goto write_done;
        }
    }

    if (use_pipeline_stage) {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 burn erase policy: pipeline sector erase mode=%s",
            force_erase_sectors ? "force" : "smart-skip");
    } else {
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "checking flash blank state",
            job->rom_name,
            job->rom_path);

        burner_spi_lock_take();
        err = burner_mbc5_region_is_blank_head(addr_begin, job->total_bytes, &range_blank);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "read flash blank failed",
                job->rom_name,
                job->rom_path);
            goto write_done;
        }

        should_erase = !range_blank;
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 burn erase policy: head-512B erase=%s",
            should_erase ? "yes" : "no");
    }
    if (should_erase && !burner_nor_geometry_is_valid(&s_cart_ctx.geometry)) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "flash sector geometry unavailable",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_INVALID_SIZE;
        goto write_done;
    }
    if (should_erase) {
        uint32_t planned_erase_sectors =
            use_pipeline_stage
                ? burner_nor_geometry_sector_count_from_range(
                      &s_cart_ctx.geometry,
                      addr_begin,
                      addr_begin + job->total_bytes - 1u)
                : (use_psram_stage
                       ? burner_nor_geometry_planned_stage_erase_sector_count(
                             &s_cart_ctx.geometry,
                             addr_begin,
                             job->total_bytes,
                             (uint32_t)stage_capacity)
                       : burner_nor_geometry_sector_count_from_range(
                             &s_cart_ctx.geometry,
                             addr_begin,
                             addr_begin + job->total_bytes - 1u));
        uint32_t planned_erase_bytes = burner_nor_geometry_erase_bytes_from_range(
            &s_cart_ctx.geometry,
            addr_begin,
            addr_begin + job->total_bytes - 1u);
        burner_status_plan_erase_phase(
            planned_erase_sectors,
            planned_erase_bytes,
            burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry));
    }
    if (use_pipeline_stage) {
        err = burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry, addr_begin, &pipeline_cursor);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "pipeline sector cursor unavailable",
                job->rom_name,
                job->rom_path);
            goto write_done;
        }
    }

    if (should_erase && !use_psram_stage) {
        burner_status_mark_erase_begin();
        erase_timer_started = true;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "erasing flash sectors",
            job->rom_name,
            job->rom_path);

        err = burner_run_mbc5_range_erase(
            addr_begin,
            addr_begin + job->total_bytes - 1u,
            s_cart_ctx.sector_size,
            true,
            force_erase_sectors);
        burner_status_mark_erase_end();
        erase_timer_started = false;

        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "erase flash failed",
                job->rom_name,
                job->rom_path);
            goto write_done;
        }
    }

    burner_status_mark_write_begin();
    write_timer_started = true;
    if (use_psram_stage) {
        while (processed < job->total_bytes) {
            size_t stage_bytes = (size_t)(job->total_bytes - processed);
            size_t stage_off = 0;
            uint32_t stage_addr = addr_begin + processed;
            bool stage_prefetched = false;

            if (use_pipeline_stage) {
                uint32_t pipeline_stage_bytes = 0u;

                err = burner_nor_geometry_region_cursor_seek_forward(
                    &s_cart_ctx.geometry,
                    stage_addr,
                    &pipeline_cursor);
                if (err == ESP_OK) {
                    err = burner_nor_geometry_stage_bytes_in_cursor(
                        &pipeline_cursor,
                        stage_addr,
                        job->total_bytes - processed,
                        &pipeline_stage_bytes);
                }
                if (err != ESP_OK || pipeline_stage_bytes == 0u) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "pipeline sector window invalid",
                        job->rom_name,
                        job->rom_path);
                    goto write_done;
                }
                stage_bytes = (size_t)pipeline_stage_bytes;
            } else if (stage_bytes > stage_capacity) {
                stage_bytes = stage_capacity;
            }

            if (should_erase) {
                uint32_t stage_erase_begin = stage_addr;
                uint32_t stage_erase_end = stage_addr + (uint32_t)stage_bytes - 1u;

                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    psram_erase_prefetch_msg,
                    job->rom_name,
                    job->rom_path);

                prefetch_started = false;
                prefetch_inflight = false;
                if (prefetch_done != NULL) {
                    vSemaphoreDelete(prefetch_done);
                    prefetch_done = NULL;
                }

                prefetch_done = xSemaphoreCreateBinary();
                if (prefetch_done != NULL) {
                    prefetch.fp = fp;
                    prefetch.dst = psram_stage_buf;
                    prefetch.bytes = stage_bytes;
                    prefetch.read_len = 0u;
                    prefetch.err = ESP_FAIL;
                    prefetch.done = prefetch_done;
                    if (burner_create_task_with_affinity(
                            burner_tf_prefetch_task,
                            "tf_prefetch",
                            4096,
                            &prefetch,
                            4,
                            NULL,
                            s_burn_core_cfg.tf_core) == pdPASS) {
                        prefetch_inflight = true;
                        prefetch_started = true;
                    } else {
                        vSemaphoreDelete(prefetch_done);
                        prefetch_done = NULL;
                    }
                }

                burner_status_mark_erase_begin();
                erase_timer_started = true;
                if (processed > 0u && !use_pipeline_stage) {
                    err = burner_nor_geometry_sector_begin_ceil(&s_cart_ctx.geometry, stage_addr, &stage_erase_begin);
                    if (err != ESP_OK || stage_erase_begin > stage_erase_end) {
                        err = ESP_OK;
                        burner_status_mark_erase_end();
                        erase_timer_started = false;
                        goto mbc5_stage_erase_done;
                    }
                }
                err = burner_run_mbc5_range_erase(
                    stage_erase_begin,
                    stage_erase_end,
                    s_cart_ctx.sector_size,
                    true,
                    force_erase_sectors);
                burner_status_mark_erase_end();
                erase_timer_started = false;

mbc5_stage_erase_done:
                if (prefetch_inflight && prefetch_done != NULL) {
                    xSemaphoreTake(prefetch_done, portMAX_DELAY);
                    prefetch_inflight = false;
                }
                if (prefetch_done != NULL) {
                    vSemaphoreDelete(prefetch_done);
                    prefetch_done = NULL;
                }

                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "erase flash failed",
                        job->rom_name,
                        job->rom_path);
                    goto write_done;
                }

                if (prefetch_started) {
                    if (prefetch.err == ESP_OK && prefetch.read_len == stage_bytes) {
                        stage_prefetched = true;
                    } else {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            0,
                            processed,
                            job->total_bytes,
                            "prefetch tf->psram failed",
                            job->rom_name,
                            job->rom_path);
                        err = (prefetch.err != ESP_OK) ? prefetch.err : ESP_FAIL;
                        goto write_done;
                    }
                }
            }

            if (!stage_prefetched) {
                uint64_t tf_read_start_us;
                uint64_t tf_read_elapsed_us;
                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    psram_copy_msg,
                    job->rom_name,
                    job->rom_path);

                tf_read_start_us = (uint64_t)esp_timer_get_time();
                err = burner_tf_reader_read(&tf_reader, psram_stage_buf, stage_bytes);
                tf_read_elapsed_us = (uint64_t)esp_timer_get_time() - tf_read_start_us;
                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                        job->rom_name,
                        job->rom_path);
                    goto write_done;
                }
                if (stage_bytes > 0u && tf_read_elapsed_us > 0u) {
                    burner_status_record_tf_to_psram_copy((uint32_t)stage_bytes, tf_read_elapsed_us);
                }
            }

            while (stage_off < stage_bytes) {
                size_t chunk_bytes = stage_bytes - stage_off;
                uint32_t now_processed;
                int progress;

                if (chunk_bytes > program_chunk_bytes) {
                    chunk_bytes = program_chunk_bytes;
                }

                burner_spi_lock_take();
                err = burner_bacon_mbc5_program_block(
                    psram_stage_buf + stage_off,
                    chunk_bytes,
                    addr_begin + processed + (uint32_t)stage_off);
                burner_spi_lock_give();
                if (err != ESP_OK) {
                    char program_err_msg[96];
                    (void)snprintf(
                        program_err_msg,
                        sizeof(program_err_msg),
                        "program cart failed @0x%08" PRIX32 " (%s)",
                        addr_begin + processed + (uint32_t)stage_off,
                        esp_err_to_name(err));
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed + (uint32_t)stage_off,
                        job->total_bytes,
                        program_err_msg,
                        job->rom_name,
                        job->rom_path);
                    goto write_done;
                }

                stage_off += chunk_bytes;
                now_processed = processed + (uint32_t)stage_off;
                progress = burner_calc_progress_percent_u64(now_processed, job->total_bytes);
                if (progress > 100) {
                    progress = 100;
                }
                burner_status_update(
                    BURNER_STATE_BURNING,
                    progress,
                    now_processed,
                    job->total_bytes,
                    psram_program_msg,
                    job->rom_name,
                    job->rom_path);
                burner_emit_progress_cb(progress, now_processed);
            }

            processed += (uint32_t)stage_bytes;
        }
    } else {
        while (processed < job->total_bytes) {
            size_t chunk_bytes = (size_t)(job->total_bytes - processed);
            int progress;

            if (chunk_bytes > program_chunk_bytes) {
                chunk_bytes = program_chunk_bytes;
            }

            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed,
                job->total_bytes,
                direct_copy_msg,
                job->rom_name,
                job->rom_path);

            err = burner_tf_reader_read(&tf_reader, buf, chunk_bytes);
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                    job->rom_name,
                    job->rom_path);
                goto write_done;
            }

            burner_spi_lock_take();
            err = burner_bacon_mbc5_program_block(buf, chunk_bytes, addr_begin + processed);
            burner_spi_lock_give();
            if (err != ESP_OK) {
                char program_err_msg[96];
                (void)snprintf(
                    program_err_msg,
                    sizeof(program_err_msg),
                    "program cart failed @0x%08" PRIX32 " (%s)",
                    addr_begin + processed,
                    esp_err_to_name(err));
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    program_err_msg,
                    job->rom_name,
                    job->rom_path);
                goto write_done;
            }

            processed += (uint32_t)chunk_bytes;
            progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
            if (progress > 100) {
                progress = 100;
            }
            burner_status_update(
                BURNER_STATE_BURNING,
                progress,
                processed,
                job->total_bytes,
                direct_program_msg,
                job->rom_name,
                job->rom_path);
            burner_emit_progress_cb(progress, processed);
        }
    }

    if (err == ESP_OK && processed != job->total_bytes) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            processed,
            job->total_bytes,
            "tf file shorter than expected",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
    }

write_done:
    if (erase_timer_started) {
        burner_status_mark_erase_end();
    }
    if (write_timer_started) {
        burner_status_mark_write_end();
    }
    if (prefetch_inflight && prefetch_done != NULL) {
        xSemaphoreTake(prefetch_done, portMAX_DELAY);
        prefetch_inflight = false;
    }
    if (prefetch_done != NULL) {
        vSemaphoreDelete(prefetch_done);
    }
    if (tf_reader_started) {
        burner_tf_reader_stop(&tf_reader);
    }
    if (psram_stage_buf != NULL) {
        free(psram_stage_buf);
    }
    if (buf != NULL) {
        free(buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_write_job_gba(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint8_t *psram_stage_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool should_erase = false;
    bool use_psram_stage = false;
    bool use_pipeline_stage = false;
    bool range_blank = false;
    bool force_erase_sectors = false;
    size_t stage_capacity = 0;
    burner_tf_prefetch_ctx_t prefetch = {0};
    burner_tf_reader_ctx_t tf_reader = {0};
    burner_nor_region_cursor_t pipeline_cursor = {0};
    SemaphoreHandle_t prefetch_done = NULL;
    bool prefetch_inflight = false;
    bool prefetch_started = false;
    bool tf_reader_started = false;
    bool erase_timer_started = false;
    bool write_timer_started = false;
    bool sector_geometry_valid = false;
    uint32_t psram_window_mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    uint32_t psram_window_bytes = BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES;
    char psram_alloc_fail_msg[96] = {0};
    char psram_erase_prefetch_msg[96] = {0};
    char psram_copy_msg[64] = {0};

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    use_psram_stage = (job->write_path == BURNER_WRITE_PATH_PSRAM ||
                       job->write_path == BURNER_WRITE_PATH_PIPELINE);
    use_pipeline_stage = (job->write_path == BURNER_WRITE_PATH_PIPELINE);
    force_erase_sectors = use_pipeline_stage ? true : job->erase_always;
    if (use_pipeline_stage) {
        psram_window_mb = BURN_PSRAM_WINDOW_AUTO_MB;
        psram_window_bytes = 0u;
    } else if (s_gba_fixed_erase_window_enabled != 0u) {
        psram_window_mb = BURN_GBA_FIXED_ERASE_WINDOW_MB;
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    } else {
        psram_window_mb = burner_psram_window_bytes_to_mb(job->psram_window_bytes);
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    }
    (void)snprintf(
        psram_alloc_fail_msg,
        sizeof(psram_alloc_fail_msg),
        use_pipeline_stage ? "alloc pipeline psram staging failed" : "alloc %uMB psram staging failed",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_erase_prefetch_msg,
        sizeof(psram_erase_prefetch_msg),
        use_pipeline_stage ? "pipeline erase gba sector + prefetch tf->psram" : "erasing gba flash sectors (%uMB) + prefetch tf->psram",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_copy_msg,
        sizeof(psram_copy_msg),
        use_pipeline_stage ? "pipeline copy tf->psram (sector window)" : "copy tf->psram (%uMB window)",
        (unsigned)psram_window_mb);
    if (use_psram_stage && !use_pipeline_stage) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA psram erase window policy: %s window=%uMB",
            s_gba_fixed_erase_window_enabled != 0u ? "fixed" : "dynamic",
            (unsigned)psram_window_mb);
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addr_begin & 0x1u) != 0u || (job->total_bytes & 0x1u) != 0u) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba write range must be even aligned",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing gba cart and preparing flash",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)job->total_bytes) > (uint64_t)s_cart_ctx.device_size) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "selected range exceeds gba flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }
    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open rom failed",
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
    }

    err = burner_tf_reader_start(&tf_reader, fp);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "create tf reader failed",
            job->rom_name,
            job->rom_path);
        fclose(fp);
        return err;
    }
    tf_reader_started = true;

    if (use_psram_stage) {
        if (use_pipeline_stage) {
            uint32_t pipeline_stage_capacity = 0u;

            err = burner_nor_geometry_largest_sector_size_in_range(
                &s_cart_ctx.geometry,
                addr_begin,
                job->total_bytes,
                &pipeline_stage_capacity);
            if (err != ESP_OK || pipeline_stage_capacity == 0u) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    0,
                    job->total_bytes,
                    "gba pipeline sector geometry unavailable",
                    job->rom_name,
                    job->rom_path);
                goto write_gba_done;
            }
            stage_capacity = (job->total_bytes < pipeline_stage_capacity)
                                 ? (size_t)job->total_bytes
                                 : (size_t)pipeline_stage_capacity;
        } else {
            stage_capacity = (job->total_bytes < psram_window_bytes)
                                 ? (size_t)job->total_bytes
                                 : (size_t)psram_window_bytes;
        }
        psram_stage_buf = (uint8_t *)heap_caps_malloc(
            stage_capacity,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (psram_stage_buf == NULL) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                psram_alloc_fail_msg,
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_NO_MEM;
            goto write_gba_done;
        }
    } else {
        buf = (uint8_t *)malloc(BURN_GBA_PROGRAM_CHUNK_BYTES);
        if (buf == NULL) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "no memory for gba write chunk",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_NO_MEM;
            goto write_gba_done;
        }
    }

    if (use_pipeline_stage) {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBA burn erase policy: pipeline sector erase mode=%s",
            force_erase_sectors ? "force" : "smart-skip");
    } else {
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "checking gba flash blank state",
            job->rom_name,
            job->rom_path);

        burner_spi_lock_take();
        err = burner_gba_region_is_blank_head(
            addr_begin,
            job->total_bytes,
            burner_is_gba_multi_card(job),
            &range_blank);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "read gba flash blank failed",
                job->rom_name,
                job->rom_path);
            goto write_gba_done;
        }

        should_erase = !range_blank;
        ESP_LOGI(
            BURNER_TAG,
            "GBA burn erase policy: head-512B erase=%s",
            should_erase ? "yes" : "no");
    }
    sector_geometry_valid = burner_nor_geometry_is_valid(&s_cart_ctx.geometry);
    if (should_erase && !sector_geometry_valid) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba sector geometry unavailable",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_INVALID_SIZE;
        goto write_gba_done;
    }
    if (should_erase && sector_geometry_valid) {
        uint32_t planned_erase_sectors =
            use_pipeline_stage
                ? burner_nor_geometry_sector_count_from_range(
                      &s_cart_ctx.geometry,
                      addr_begin,
                      addr_begin + job->total_bytes - 1u)
                : (use_psram_stage
                       ? burner_nor_geometry_planned_stage_erase_sector_count(
                             &s_cart_ctx.geometry,
                             addr_begin,
                             job->total_bytes,
                             (uint32_t)stage_capacity)
                       : burner_nor_geometry_sector_count_from_range(
                             &s_cart_ctx.geometry,
                             addr_begin,
                             addr_begin + job->total_bytes - 1u));
        uint32_t planned_erase_bytes = burner_nor_geometry_erase_bytes_from_range(
            &s_cart_ctx.geometry,
            addr_begin,
            addr_begin + job->total_bytes - 1u);
        burner_status_plan_erase_phase(
            planned_erase_sectors,
            planned_erase_bytes,
            burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry));
    }
    if (use_pipeline_stage) {
        err = burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry, addr_begin, &pipeline_cursor);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "gba pipeline sector cursor unavailable",
                job->rom_name,
                job->rom_path);
            goto write_gba_done;
        }
    }
    burner_gba_sector_erase_ctx_reset();
    if (use_pipeline_stage && should_erase) {
        burner_gba_sector_erase_ctx_begin(
            addr_begin,
            addr_begin + job->total_bytes - 1u,
            s_cart_ctx.sector_size,
            burner_is_gba_multi_card(job),
            force_erase_sectors);
    }
    if (should_erase && !use_psram_stage) {
        burner_status_mark_erase_begin();
        erase_timer_started = true;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "erasing gba flash sectors",
            job->rom_name,
            job->rom_path);

        err = burner_run_gba_range_erase(
            addr_begin,
            addr_begin + job->total_bytes - 1u,
            s_cart_ctx.sector_size,
            burner_is_gba_multi_card(job),
            true,
            force_erase_sectors);
        burner_status_mark_erase_end();
        erase_timer_started = false;

        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "erase gba flash failed",
                job->rom_name,
                job->rom_path);
            goto write_gba_done;
        }
    }

    burner_status_mark_write_begin();
    write_timer_started = true;
    if (use_psram_stage) {
        while (processed < job->total_bytes) {
            size_t stage_bytes = (size_t)(job->total_bytes - processed);
            size_t stage_off = 0;
            uint32_t stage_addr = addr_begin + processed;
            bool stage_prefetched = false;

            if (use_pipeline_stage) {
                uint32_t pipeline_stage_bytes = 0u;

                err = burner_nor_geometry_region_cursor_seek_forward(
                    &s_cart_ctx.geometry,
                    stage_addr,
                    &pipeline_cursor);
                if (err == ESP_OK) {
                    err = burner_nor_geometry_stage_bytes_in_cursor(
                        &pipeline_cursor,
                        stage_addr,
                        job->total_bytes - processed,
                        &pipeline_stage_bytes);
                }
                if (err != ESP_OK || pipeline_stage_bytes == 0u) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "gba pipeline sector window invalid",
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
                stage_bytes = (size_t)pipeline_stage_bytes;
            } else if (stage_bytes > stage_capacity) {
                stage_bytes = stage_capacity;
            }
            if (burner_gba_should_log_program_boundary(stage_addr, stage_bytes, processed, job->total_bytes)) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA write stage: addr=0x%08" PRIX32 " bytes=%u processed=%" PRIu32 "/%" PRIu32 " path=%s erase=%s",
                    stage_addr,
                    (unsigned)stage_bytes,
                    processed,
                    job->total_bytes,
                    burner_write_path_to_str(job->write_path),
                    should_erase ? "yes" : "no");
            }

            if (should_erase) {
                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    psram_erase_prefetch_msg,
                    job->rom_name,
                    job->rom_path);
            }

            prefetch_started = false;
            prefetch_inflight = false;
            if (prefetch_done != NULL) {
                vSemaphoreDelete(prefetch_done);
                prefetch_done = NULL;
            }

            prefetch_done = xSemaphoreCreateBinary();
            if (prefetch_done != NULL) {
                prefetch.fp = fp;
                prefetch.dst = psram_stage_buf;
                prefetch.bytes = stage_bytes;
                prefetch.read_len = 0u;
                prefetch.err = ESP_FAIL;
                prefetch.done = prefetch_done;
                if (burner_create_task_with_affinity(
                        burner_tf_prefetch_task,
                        "tf_prefetch",
                        4096,
                        &prefetch,
                        4,
                        NULL,
                        s_burn_core_cfg.tf_core) == pdPASS) {
                    prefetch_inflight = true;
                    prefetch_started = true;
                } else {
                    vSemaphoreDelete(prefetch_done);
                    prefetch_done = NULL;
                }
            }

            if (should_erase) {
                burner_spi_lock_take();
                err = burner_gba_sector_erase_prepare_current(stage_addr);
                burner_spi_lock_give();
                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "erase gba flash failed",
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
            }

            if (prefetch_inflight && prefetch_done != NULL) {
                xSemaphoreTake(prefetch_done, portMAX_DELAY);
                prefetch_inflight = false;
            }
            if (prefetch_done != NULL) {
                vSemaphoreDelete(prefetch_done);
                prefetch_done = NULL;
            }
            if (prefetch_started) {
                if (prefetch.err == ESP_OK && prefetch.read_len == stage_bytes) {
                    stage_prefetched = true;
                } else {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "prefetch tf->psram failed",
                        job->rom_name,
                        job->rom_path);
                    err = (prefetch.err != ESP_OK) ? prefetch.err : ESP_FAIL;
                    goto write_gba_done;
                }
            }

            if (!stage_prefetched) {
                uint64_t tf_read_start_us;
                uint64_t tf_read_elapsed_us;
                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    psram_copy_msg,
                    job->rom_name,
                    job->rom_path);

                tf_read_start_us = (uint64_t)esp_timer_get_time();
                err = burner_tf_reader_read(&tf_reader, psram_stage_buf, stage_bytes);
                tf_read_elapsed_us = (uint64_t)esp_timer_get_time() - tf_read_start_us;
                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
                if (stage_bytes > 0u && tf_read_elapsed_us > 0u) {
                    burner_status_record_tf_to_psram_copy((uint32_t)stage_bytes, tf_read_elapsed_us);
                }
            }

            while (stage_off < stage_bytes) {
                size_t chunk_bytes = stage_bytes - stage_off;
                uint32_t write_addr = addr_begin + processed + (uint32_t)stage_off;
                uint32_t now_processed;
                int progress;

                if (chunk_bytes > BURN_GBA_PROGRAM_CHUNK_BYTES) {
                    chunk_bytes = BURN_GBA_PROGRAM_CHUNK_BYTES;
                }
                if (burner_gba_should_log_program_boundary(
                        write_addr,
                        chunk_bytes,
                        processed + (uint32_t)stage_off,
                        job->total_bytes)) {
                    ESP_LOGI(
                        BURNER_TAG,
                        "GBA program chunk: addr=0x%08" PRIX32 " bytes=%u processed=%" PRIu32 "/%" PRIu32 " path=%s",
                        write_addr,
                        (unsigned)chunk_bytes,
                        processed + (uint32_t)stage_off,
                        job->total_bytes,
                        burner_write_path_to_str(job->write_path));
                }

                burner_spi_lock_take();
                err = burner_bacon_gba_program_block(
                    psram_stage_buf + stage_off,
                    chunk_bytes,
                    write_addr,
                    burner_is_gba_multi_card(job),
                    use_pipeline_stage && should_erase && !burner_gba_nor_is_intel_active());
                burner_spi_lock_give();
                if (err != ESP_OK) {
                    char program_err_msg[96];
                    (void)snprintf(
                        program_err_msg,
                        sizeof(program_err_msg),
                        "program gba failed @0x%08" PRIX32 " (%s)",
                        write_addr,
                        esp_err_to_name(err));
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed + (uint32_t)stage_off,
                        job->total_bytes,
                        program_err_msg,
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }

                stage_off += chunk_bytes;
                now_processed = processed + (uint32_t)stage_off;
                progress = burner_calc_progress_percent_u64(now_processed, job->total_bytes);
                if (progress > 100) {
                    progress = 100;
                }
                burner_status_update(
                    BURNER_STATE_BURNING,
                    progress,
                    now_processed,
                    job->total_bytes,
                    "psram->gba cart programmed",
                    job->rom_name,
                    job->rom_path);
                burner_emit_progress_cb(progress, now_processed);
            }

            processed += (uint32_t)stage_bytes;
        }
    } else {
        while (processed < job->total_bytes) {
            size_t chunk_bytes = (size_t)(job->total_bytes - processed);
            uint32_t write_addr = addr_begin + processed;
            int progress;

            if (chunk_bytes > BURN_GBA_PROGRAM_CHUNK_BYTES) {
                chunk_bytes = BURN_GBA_PROGRAM_CHUNK_BYTES;
            }
            if (burner_gba_should_log_program_boundary(write_addr, chunk_bytes, processed, job->total_bytes)) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA program chunk: addr=0x%08" PRIX32 " bytes=%u processed=%" PRIu32 "/%" PRIu32 " path=%s",
                    write_addr,
                    (unsigned)chunk_bytes,
                    processed,
                    job->total_bytes,
                    burner_write_path_to_str(job->write_path));
            }

            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed,
                job->total_bytes,
                "copy tf->ram (64KB chunk)",
                job->rom_name,
                job->rom_path);

            err = burner_tf_reader_read(&tf_reader, buf, chunk_bytes);
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                    job->rom_name,
                    job->rom_path);
                goto write_gba_done;
            }

            burner_spi_lock_take();
            err = burner_bacon_gba_program_block(
                buf,
                chunk_bytes,
                write_addr,
                burner_is_gba_multi_card(job),
                false);
            burner_spi_lock_give();
            if (err != ESP_OK) {
                char program_err_msg[96];
                (void)snprintf(
                    program_err_msg,
                    sizeof(program_err_msg),
                    "program gba failed @0x%08" PRIX32 " (%s)",
                    write_addr,
                    esp_err_to_name(err));
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    program_err_msg,
                    job->rom_name,
                    job->rom_path);
                goto write_gba_done;
            }

            processed += (uint32_t)chunk_bytes;
            progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
            if (progress > 100) {
                progress = 100;
            }
            burner_status_update(
                BURNER_STATE_BURNING,
                progress,
                processed,
                job->total_bytes,
                "ram->gba cart programmed",
                job->rom_name,
                job->rom_path);
            burner_emit_progress_cb(progress, processed);
        }
    }

    if (err == ESP_OK && processed != job->total_bytes) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            processed,
            job->total_bytes,
            "tf file shorter than expected",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
    }

write_gba_done:
    burner_gba_sector_erase_ctx_reset();
    if (erase_timer_started) {
        burner_status_mark_erase_end();
    }
    if (write_timer_started) {
        burner_status_mark_write_end();
    }
    if (prefetch_inflight && prefetch_done != NULL) {
        xSemaphoreTake(prefetch_done, portMAX_DELAY);
        prefetch_inflight = false;
    }
    if (prefetch_done != NULL) {
        vSemaphoreDelete(prefetch_done);
    }
    if (tf_reader_started) {
        burner_tf_reader_stop(&tf_reader);
    }
    if (psram_stage_buf != NULL) {
        free(psram_stage_buf);
    }
    if (buf != NULL) {
        free(buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_write_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_write_job_gba(job);
    }
    return burner_run_write_job_mbc5(job);
}

static esp_err_t burner_run_read_job_mbc5(const burner_task_param_t *job)
{
    uint32_t addr_begin = 0;
    uint32_t dump_chunk_bytes = BURN_MBC5_DUMP_CHUNK_BYTES;
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing cart for read",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)job->total_bytes) > (uint64_t)s_cart_ctx.device_size) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "selected range exceeds flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }

    if (burner_is_supported_dump_chunk_bytes(job->read_chunk_bytes)) {
        dump_chunk_bytes = job->read_chunk_bytes;
    }

    return burner_run_read_job_direct(
        job,
        job->total_bytes,
        dump_chunk_bytes,
        burner_dump_read_block_mbc5,
        "cart->tf direct dumping",
        "alloc direct dump buffer failed",
        "read cart failed",
        "write dump file failed");
}

static esp_err_t burner_run_read_job_gba(const burner_task_param_t *job)
{
    uint32_t addr_begin = 0;
    uint32_t work_total = 0;
    uint32_t dump_chunk_bytes = BURN_GBA_DUMP_CHUNK_BYTES;
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    work_total = job->total_bytes;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addr_begin & 0x1u) != 0u || (work_total & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing gba cart for read",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)work_total) > (uint64_t)s_cart_ctx.device_size) {
        if (addr_begin >= s_cart_ctx.device_size) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                work_total,
                "selected range has no gba cart data",
                job->rom_name,
                job->rom_path);
            return ESP_ERR_INVALID_SIZE;
        }
        work_total = s_cart_ctx.device_size - addr_begin;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            work_total,
            "selected range clamped to gba flash size",
            job->rom_name,
            job->rom_path);
    }

    if (burner_is_supported_dump_chunk_bytes(job->read_chunk_bytes)) {
        dump_chunk_bytes = job->read_chunk_bytes;
    }

    return burner_run_read_job_direct(
        job,
        work_total,
        dump_chunk_bytes,
        burner_dump_read_block_gba,
        "gba cart->tf direct dumping",
        "alloc direct dump buffer failed",
        "read gba cart failed",
        "write dump file failed");
}

static esp_err_t burner_run_read_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_read_job_gba(job);
    }
    return burner_run_read_job_mbc5(job);
}

static esp_err_t burner_run_verify_rom_job_mbc5(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    FILE *verify_log_fp = NULL;
    uint8_t *psram_stage_buf = NULL;
    uint8_t *cart_buf = NULL;
    burner_tf_reader_ctx_t tf_reader = {0};
    bool tf_reader_started = false;
    bool verify_log_open_attempted = false;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    uint32_t psram_window_mb = BURN_VERIFY_PSRAM_WINDOW_MB;
    uint32_t psram_window_bytes = BURN_VERIFY_PSRAM_WINDOW_BYTES;
    size_t stage_capacity = 0u;
    char psram_copy_msg[64] = {0};
    char psram_verify_msg[64] = {0};
    char psram_alloc_fail_msg[96] = {0};
    char verify_log_rel[TF_PATH_LEN_MAX] = {0};
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    psram_window_mb = burner_psram_window_bytes_to_mb(job->psram_window_bytes);
    psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(
        psram_copy_msg,
        sizeof(psram_copy_msg),
        "copy tf->psram (%uMB verify window)",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_verify_msg,
        sizeof(psram_verify_msg),
        "psram->cart verify (%uKB chunk)",
        (unsigned)(BURN_MBC5_DUMP_CHUNK_BYTES / 1024u));
    (void)snprintf(
        psram_alloc_fail_msg,
        sizeof(psram_alloc_fail_msg),
        "alloc %uMB psram verify staging failed",
        (unsigned)psram_window_mb);

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing cart for verify",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)job->total_bytes) > (uint64_t)s_cart_ctx.device_size) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "selected range exceeds flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open verify file failed",
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
    }

    err = burner_tf_reader_start(&tf_reader, fp);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "create tf reader failed",
            job->rom_name,
            job->rom_path);
        fclose(fp);
        return err;
    }
    tf_reader_started = true;

    stage_capacity = (job->total_bytes < psram_window_bytes)
                         ? (size_t)job->total_bytes
                         : (size_t)psram_window_bytes;
    psram_stage_buf = (uint8_t *)heap_caps_malloc(
        stage_capacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cart_buf = (uint8_t *)malloc(BURN_MBC5_DUMP_CHUNK_BYTES);
    if (psram_stage_buf == NULL || cart_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            (psram_stage_buf == NULL) ? psram_alloc_fail_msg : "no memory for verify cart chunk",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto verify_done;
    }

    while (processed < job->total_bytes) {
        size_t stage_bytes = (size_t)(job->total_bytes - processed);
        size_t stage_off = 0u;
        uint64_t tf_read_start_us;
        uint64_t tf_read_elapsed_us;

        if (stage_bytes > stage_capacity) {
            stage_bytes = stage_capacity;
        }

        burner_status_update(
            BURNER_STATE_BURNING,
            burner_calc_progress_percent_u64(processed, job->total_bytes),
            processed,
            job->total_bytes,
            psram_copy_msg,
            job->rom_name,
            job->rom_path);

        tf_read_start_us = (uint64_t)esp_timer_get_time();
        err = burner_tf_reader_read(&tf_reader, psram_stage_buf, stage_bytes);
        tf_read_elapsed_us = (uint64_t)esp_timer_get_time() - tf_read_start_us;
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read verify file failed",
                job->rom_name,
                job->rom_path);
            break;
        }
        if (stage_bytes > 0u && tf_read_elapsed_us > 0u) {
            burner_status_record_tf_to_psram_copy((uint32_t)stage_bytes, tf_read_elapsed_us);
        }

        while (stage_off < stage_bytes) {
            size_t verify_bytes = stage_bytes - stage_off;
            uint32_t chunk_addr = addr_begin + processed + (uint32_t)stage_off;
            uint32_t now_processed;
            int progress;

            if (verify_bytes > BURN_MBC5_DUMP_CHUNK_BYTES) {
                verify_bytes = BURN_MBC5_DUMP_CHUNK_BYTES;
            }

            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed + (uint32_t)stage_off, job->total_bytes),
                processed + (uint32_t)stage_off,
                job->total_bytes,
                psram_verify_msg,
                job->rom_name,
                job->rom_path);

            burner_spi_lock_take();
            err = burner_bacon_mbc5_read_block_hoststyle(cart_buf, verify_bytes, chunk_addr);
            burner_spi_lock_give();
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed + (uint32_t)stage_off,
                    job->total_bytes,
                    "read cart failed",
                    job->rom_name,
                    job->rom_path);
                goto verify_done;
            }

            if (memcmp(psram_stage_buf + stage_off, cart_buf, verify_bytes) != 0) {
                size_t i;

                for (i = 0u; i < verify_bytes; ++i) {
                    uint8_t rom_byte = psram_stage_buf[stage_off + i];
                    uint8_t cart_byte = cart_buf[i];

                    if (rom_byte != cart_byte) {
                        uint32_t mismatch_addr = chunk_addr + (uint32_t)i;
                        uint32_t mismatch_processed = mismatch_addr - addr_begin;
                        int mismatch_progress = burner_calc_progress_percent_u64(
                            mismatch_processed,
                            job->total_bytes);
                        char final_msg[96] = {0};

                        if (!verify_log_open_attempted) {
                            verify_log_fp = burner_open_mbc5_verify_log(
                                job,
                                verify_log_rel,
                                sizeof(verify_log_rel));
                            verify_log_open_attempted = true;
                        }
                        if (verify_log_fp != NULL) {
                            (void)fprintf(
                                verify_log_fp,
                                "0x%08" PRIX32 " %02X->%02X\n",
                                mismatch_addr,
                                rom_byte,
                                cart_byte);
                        }
                        burner_status_set_verify_sample(
                            mismatch_addr,
                            rom_byte,
                            cart_byte,
                            false);
                        if (verify_log_fp != NULL) {
                            const char *log_name = strrchr(verify_log_rel, '/');
                            log_name = (log_name != NULL) ? (log_name + 1) : verify_log_rel;
                            fflush(verify_log_fp);
                            (void)snprintf(
                                final_msg,
                                sizeof(final_msg),
                                "verify mismatch @0x%08" PRIX32 " %02X->%02X log=%.32s",
                                mismatch_addr,
                                rom_byte,
                                cart_byte,
                                log_name);
                        } else {
                            (void)snprintf(
                                final_msg,
                                sizeof(final_msg),
                                "verify mismatch @0x%08" PRIX32 " %02X->%02X",
                                mismatch_addr,
                                rom_byte,
                                cart_byte);
                        }
                        if (mismatch_progress > 100) {
                            mismatch_progress = 100;
                        }
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            mismatch_progress,
                            mismatch_processed,
                            job->total_bytes,
                            final_msg,
                            job->rom_name,
                            job->rom_path);
                        err = ESP_FAIL;
                        goto verify_done;
                    }
                }
            }

            if (verify_bytes > 0u) {
                size_t sample_index = verify_bytes - 1u;
                burner_status_set_verify_sample(
                    chunk_addr + (uint32_t)sample_index,
                    psram_stage_buf[stage_off + sample_index],
                    cart_buf[sample_index],
                    true);
            }

            stage_off += verify_bytes;
            now_processed = processed + (uint32_t)stage_off;
            progress = burner_calc_progress_percent_u64(now_processed, job->total_bytes);
            if (progress > 100) {
                progress = 100;
            }
            burner_status_update(
                BURNER_STATE_BURNING,
                progress,
                now_processed,
                job->total_bytes,
                "psram->cart verify running",
                job->rom_name,
                job->rom_path);
            burner_emit_progress_cb(progress, now_processed);
        }

        processed += (uint32_t)stage_bytes;
    }

verify_done:
    if (tf_reader_started) {
        burner_tf_reader_stop(&tf_reader);
    }
    if (verify_log_fp != NULL) {
        fclose(verify_log_fp);
    }
    if (psram_stage_buf != NULL) {
        free(psram_stage_buf);
    }
    if (cart_buf != NULL) {
        free(cart_buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_verify_rom_job_gba(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *rom_buf = NULL;
    uint8_t *cart_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    uint32_t work_total = 0;
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    work_total = job->total_bytes;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addr_begin & 0x1u) != 0u || (work_total & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing gba cart for verify",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)work_total) > (uint64_t)s_cart_ctx.device_size) {
        if (addr_begin >= s_cart_ctx.device_size) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                work_total,
                "selected range has no gba cart data",
                job->rom_name,
                job->rom_path);
            return ESP_ERR_INVALID_SIZE;
        }
        work_total = s_cart_ctx.device_size - addr_begin;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            work_total,
            "selected range clamped to gba flash size",
            job->rom_name,
            job->rom_path);
    }

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            "open verify file failed",
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
    }

    rom_buf = (uint8_t *)malloc(BURN_GBA_DUMP_CHUNK_BYTES);
    cart_buf = (uint8_t *)malloc(BURN_GBA_DUMP_CHUNK_BYTES);
    if (rom_buf == NULL || cart_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            "no memory for gba verify",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto verify_gba_done;
    }

    while (processed < work_total) {
        size_t read_len = (size_t)(work_total - processed);
        int progress;

        if (read_len > BURN_GBA_DUMP_CHUNK_BYTES) {
            read_len = BURN_GBA_DUMP_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                work_total,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        {
            size_t got = fread(rom_buf, 1, read_len, fp);
            if (got != read_len) {
                if (got + 1u == read_len && feof(fp)) {
                    rom_buf[got] = 0x00u;
                } else {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        work_total,
                        "read verify file failed",
                        job->rom_name,
                        job->rom_path);
                    err = ESP_FAIL;
                    break;
                }
            }
        }

        burner_spi_lock_take();
        err = burner_bacon_gba_verify_read_block_hoststyle(
            cart_buf,
            read_len,
            addr_begin + processed,
            burner_is_gba_multi_card(job));
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                work_total,
                "read gba cart failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        if (memcmp(rom_buf, cart_buf, read_len) != 0) {
            size_t i;
            char msg[96];

            for (i = 0; i < read_len; ++i) {
                if (rom_buf[i] != cart_buf[i]) {
                    uint32_t mismatch_addr = addr_begin + processed + (uint32_t)i;
                    burner_status_set_verify_sample(
                        mismatch_addr,
                        rom_buf[i],
                        cart_buf[i],
                        false);
                    snprintf(
                        msg,
                        sizeof(msg),
                        "verify mismatch @0x%08" PRIX32 " %02X->%02X",
                        mismatch_addr,
                        rom_buf[i],
                        cart_buf[i]);
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, work_total),
                        processed,
                        work_total,
                        msg,
                        job->rom_name,
                        job->rom_path);
                    err = ESP_FAIL;
                    break;
                }
            }
            if (err != ESP_OK) {
                break;
            }
        }

        if (read_len > 0u) {
            size_t sample_index = read_len - 1u;
            burner_status_set_verify_sample(
                addr_begin + processed + (uint32_t)sample_index,
                rom_buf[sample_index],
                cart_buf[sample_index],
                true);
        }

        processed += (uint32_t)read_len;
        progress = burner_calc_progress_percent_u64(processed, work_total);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            work_total,
            "gba cart verify running",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

verify_gba_done:
    if (rom_buf != NULL) {
        free(rom_buf);
    }
    if (cart_buf != NULL) {
        free(cart_buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_verify_rom_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_verify_rom_job_gba(job);
    }
    return burner_run_verify_rom_job_mbc5(job);
}

static esp_err_t burner_run_erase_rom_job_mbc5(const burner_task_param_t *job)
{
    esp_err_t err;

    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        1u,
        "probing cart for chip erase",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }

    if (usb_msc_tf_in_use_by_host()) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "tf busy by usb host",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_STATE;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        1u,
        "chip erase running",
        job->rom_name,
        job->rom_path);
    burner_status_mark_erase_begin();

    err = burner_run_mbc5_chip_erase();
    burner_status_mark_erase_end();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "chip erase failed",
            job->rom_name,
            job->rom_path);
        return err;
    }

    burner_emit_progress_cb(100, 1u);
    return ESP_OK;
}

static esp_err_t burner_run_erase_rom_job_gba(const burner_task_param_t *job)
{
    esp_err_t err;

    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        1u,
        "probing gba cart for chip erase",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }

    if (usb_msc_tf_in_use_by_host()) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "tf busy by usb host",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_STATE;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        1u,
        "gba chip erase running",
        job->rom_name,
        job->rom_path);
    burner_status_mark_erase_begin();

    err = burner_run_gba_chip_erase();
    burner_status_mark_erase_end();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "gba chip erase failed",
            job->rom_name,
            job->rom_path);
        return err;
    }

    burner_emit_progress_cb(100, 1u);
    return ESP_OK;
}

static esp_err_t burner_run_erase_rom_job(const burner_task_param_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (job->cart_mode == BURNER_CART_MODE_GBA) {
        return burner_run_erase_rom_job_gba(job);
    }
    return burner_run_erase_rom_job_mbc5(job);
}

static esp_err_t burner_run_write_ram_job(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool ram_enabled = false;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "preparing cart ram write",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_ram();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart ram prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    ram_enabled = true;

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open sav file failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto write_ram_done;
    }

    buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    if (buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "no memory for ram write",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto write_ram_done;
    }

    while (processed < job->total_bytes) {
        size_t chunk = (size_t)(job->total_bytes - processed);
        int progress;

        if (chunk > BURN_MBC5_RAM_CHUNK_BYTES) {
            chunk = BURN_MBC5_RAM_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        if (fread(buf, 1, chunk, fp) != chunk) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read sav file failed",
                job->rom_name,
                job->rom_path);
            err = ESP_FAIL;
            break;
        }

        burner_spi_lock_take();
        err = burner_bacon_mbc5_ram_write_block(
            buf,
            chunk,
            addr_begin + processed,
            job->ram_fram,
            job->ram_latency);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "write cart ram failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        processed += (uint32_t)chunk;
        progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            job->total_bytes,
            "sav->cart ram writing",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

write_ram_done:
    if (ram_enabled) {
        burner_spi_lock_take();
        (void)burner_bacon_mbc5_ram_enable(false);
        burner_spi_lock_give();
    }
    if (buf != NULL) {
        free(buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_read_ram_job(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint8_t *file_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool ram_enabled = false;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "preparing cart ram dump",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_ram();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart ram prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    ram_enabled = true;

    fp = fopen(job->rom_path, "wb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open ram dump file failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto read_ram_done;
    }

    file_buf = burner_attach_stdio_buffer(fp, BURN_TF_STDIO_BUFFER_BYTES);

    buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    if (buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "no memory for ram dump",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto read_ram_done;
    }

    while (processed < job->total_bytes) {
        size_t chunk = (size_t)(job->total_bytes - processed);
        int progress;

        if (chunk > BURN_MBC5_RAM_CHUNK_BYTES) {
            chunk = BURN_MBC5_RAM_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        burner_spi_lock_take();
        err = burner_bacon_mbc5_ram_read_block(
            buf,
            chunk,
            addr_begin + processed,
            job->ram_fram,
            job->ram_latency);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read cart ram failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        if (fwrite(buf, 1, chunk, fp) != chunk) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "write ram dump failed",
                job->rom_name,
                job->rom_path);
            err = ESP_FAIL;
            break;
        }

        processed += (uint32_t)chunk;
        progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            job->total_bytes,
            "cart ram->tf dumping",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

read_ram_done:
    if (ram_enabled) {
        burner_spi_lock_take();
        (void)burner_bacon_mbc5_ram_enable(false);
        burner_spi_lock_give();
    }
    if (buf != NULL) {
        free(buf);
    }
    if (fp != NULL) {
        if (fclose(fp) != 0 && err == ESP_OK) {
            err = ESP_FAIL;
        }
    }
    if (file_buf != NULL) {
        free(file_buf);
    }
    if (err != ESP_OK) {
        unlink(job->rom_path);
    } else {
        (void)burner_apply_current_file_mtime(job->rom_path, NULL);
    }
    return err;
}

static esp_err_t burner_run_verify_ram_job(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *sav_buf = NULL;
    uint8_t *cart_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool ram_enabled = false;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "preparing cart ram verify",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_ram();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart ram prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    ram_enabled = true;

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open sav verify file failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto verify_ram_done;
    }

    sav_buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    cart_buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    if (sav_buf == NULL || cart_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "no memory for ram verify",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto verify_ram_done;
    }

    while (processed < job->total_bytes) {
        size_t chunk = (size_t)(job->total_bytes - processed);
        int progress;

        if (chunk > BURN_MBC5_RAM_CHUNK_BYTES) {
            chunk = BURN_MBC5_RAM_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        if (fread(sav_buf, 1, chunk, fp) != chunk) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read sav verify file failed",
                job->rom_name,
                job->rom_path);
            err = ESP_FAIL;
            break;
        }

        burner_spi_lock_take();
        err = burner_bacon_mbc5_ram_read_block(
            cart_buf,
            chunk,
            addr_begin + processed,
            job->ram_fram,
            job->ram_latency);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read cart ram failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        if (memcmp(sav_buf, cart_buf, chunk) != 0) {
            size_t i;
            char msg[96];

            for (i = 0; i < chunk; ++i) {
                if (sav_buf[i] != cart_buf[i]) {
                    uint32_t mismatch_addr = addr_begin + processed + (uint32_t)i;
                    burner_status_set_verify_sample(
                        mismatch_addr,
                        sav_buf[i],
                        cart_buf[i],
                        false);
                    snprintf(
                        msg,
                        sizeof(msg),
                        "ram mismatch @0x%08" PRIX32 " %02X->%02X",
                        mismatch_addr,
                        sav_buf[i],
                        cart_buf[i]);
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, job->total_bytes),
                        processed,
                        job->total_bytes,
                        msg,
                        job->rom_name,
                        job->rom_path);
                    err = ESP_FAIL;
                    break;
                }
            }
            if (err != ESP_OK) {
                break;
            }
        }

        if (chunk > 0u) {
            size_t sample_index = chunk - 1u;
            burner_status_set_verify_sample(
                addr_begin + processed + (uint32_t)sample_index,
                sav_buf[sample_index],
                cart_buf[sample_index],
                true);
        }

        processed += (uint32_t)chunk;
        progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            job->total_bytes,
            "cart ram verify running",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

verify_ram_done:
    if (ram_enabled) {
        burner_spi_lock_take();
        (void)burner_bacon_mbc5_ram_enable(false);
        burner_spi_lock_give();
    }
    if (sav_buf != NULL) {
        free(sav_buf);
    }
    if (cart_buf != NULL) {
        free(cart_buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_write_gba_save_job_new(const burner_task_param_t *job)
{
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t burner_run_read_gba_save_job_new(const burner_task_param_t *job)
{
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t burner_run_verify_gba_save_job_new(const burner_task_param_t *job)
{
    (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}

static void burner_task(void *param)
{
    burner_task_param_t *job = (burner_task_param_t *)param;
    esp_err_t err = ESP_OK;
    const char *done_msg = "task finished";
    const char *start_msg = "task started";
    bool restore_power = false;
    bool task_with_caps = false;

    if (job == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = job->task_with_caps;

    burner_cancel_reset();

    switch (job->mode) {
    case BURNER_JOB_WRITE_ROM:
        done_msg = "burn finished";
        if (job->write_path == BURNER_WRITE_PATH_PIPELINE) {
            start_msg = "burn task started (pipeline erase/write)";
        } else if (job->write_path == BURNER_WRITE_PATH_PSRAM) {
            start_msg = "burn task started (psram staging)";
        } else {
            start_msg = "burn task started";
        }
        break;
    case BURNER_JOB_READ_ROM:
        done_msg = "dump finished";
        start_msg = "dump task started (direct)";
        break;
    case BURNER_JOB_VERIFY_ROM:
        done_msg = "verify finished";
        start_msg = "verify task started";
        break;
    case BURNER_JOB_ERASE_ROM:
        done_msg = "chip erase finished";
        start_msg = "chip erase task started";
        break;
    case BURNER_JOB_WRITE_RAM:
        done_msg = "ram write finished";
        start_msg = "ram write task started";
        break;
    case BURNER_JOB_READ_RAM:
        done_msg = "ram dump finished";
        start_msg = "ram dump task started";
        break;
    case BURNER_JOB_VERIFY_RAM:
        done_msg = "ram verify finished";
        start_msg = "ram verify task started";
        break;
    case BURNER_JOB_WRITE_GBA_SAVE_NEW:
        done_msg = "gba save write finished";
        start_msg = "gba save write task started";
        break;
    case BURNER_JOB_READ_GBA_SAVE_NEW:
        done_msg = "gba save dump finished";
        start_msg = "gba save dump task started";
        break;
    case BURNER_JOB_VERIFY_GBA_SAVE_NEW:
        done_msg = "gba save verify finished";
        start_msg = "gba save verify task started";
        break;
    default:
        break;
    }

    burner_status_mark_task_begin();
    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        start_msg,
        job->rom_name,
        job->rom_path);

    err = burner_spi_init();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "spi init failed",
            job->rom_name,
            job->rom_path);
        goto task_done;
    }
    restore_power = true;

    if (job->mode == BURNER_JOB_READ_ROM) {
        err = burner_run_read_job(job);
    } else if (job->mode == BURNER_JOB_WRITE_ROM) {
        err = burner_run_write_job(job);
    } else if (job->mode == BURNER_JOB_VERIFY_ROM) {
        err = burner_run_verify_rom_job(job);
    } else if (job->mode == BURNER_JOB_ERASE_ROM) {
        err = burner_run_erase_rom_job(job);
    } else if (job->mode == BURNER_JOB_WRITE_RAM) {
        err = burner_run_write_ram_job(job);
    } else if (job->mode == BURNER_JOB_READ_RAM) {
        err = burner_run_read_ram_job(job);
    } else if (job->mode == BURNER_JOB_VERIFY_RAM) {
        err = burner_run_verify_ram_job(job);
    } else if (job->mode == BURNER_JOB_WRITE_GBA_SAVE_NEW) {
        err = burner_run_write_gba_save_job_new(job);
    } else if (job->mode == BURNER_JOB_READ_GBA_SAVE_NEW) {
        err = burner_run_read_gba_save_job_new(job);
    } else if (job->mode == BURNER_JOB_VERIFY_GBA_SAVE_NEW) {
        err = burner_run_verify_gba_save_job_new(job);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }

    if (restore_power) {
        burner_spi_lock_take();
        burner_bacon_restore_3v3_power();
        burner_spi_lock_give();
    }

    burner_status_mark_task_end();

    if (err == ESP_OK) {
        burner_status_t snap;
        uint32_t done_total = job->total_bytes;
        uint32_t done_processed = job->total_bytes;

        burner_status_snapshot(&snap);
        if (snap.total_bytes != 0u) {
            done_total = snap.total_bytes;
            done_processed = snap.processed_bytes;
        }
        burner_status_update(
            BURNER_STATE_DONE,
            100,
            done_processed,
            done_total,
            done_msg,
            job->rom_name,
            job->rom_path);
    } else {
        burner_status_t snap;
        burner_status_snapshot(&snap);
        if (snap.state != BURNER_STATE_ERROR && snap.state != BURNER_STATE_CANCELLED) {
            if (burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_CANCELLED,
                    snap.progress,
                    snap.processed_bytes,
                    snap.total_bytes,
                    "task cancelled",
                    job->rom_name,
                    job->rom_path);
            } else {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    snap.progress,
                    snap.processed_bytes,
                    snap.total_bytes,
                    "task failed",
                    job->rom_name,
                job->rom_path);
            }
        }
    }

    {
        burner_status_t final_snap;

        burner_status_snapshot(&final_snap);
        if (err == ESP_OK) {
            ESP_LOGI(
                BURNER_TAG,
                "burn_task result: err=%s state=%s processed=%" PRIu32 "/%" PRIu32 " msg=%s",
                esp_err_to_name(err),
                burner_state_to_str(final_snap.state),
                final_snap.processed_bytes,
                final_snap.total_bytes,
                final_snap.message);
        } else {
            ESP_LOGW(
                BURNER_TAG,
                "burn_task result: err=%s state=%s processed=%" PRIu32 "/%" PRIu32 " msg=%s",
                esp_err_to_name(err),
                burner_state_to_str(final_snap.state),
                final_snap.processed_bytes,
                final_snap.total_bytes,
                final_snap.message);
        }
    }

task_done:
    burner_cancel_reset();
    ESP_LOGI(
        BURNER_TAG,
        "burn_task stack free min=%u bytes",
        (unsigned)uxTaskGetStackHighWaterMark2(NULL));
    free(job);
    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        s_burn_task = NULL;
        xSemaphoreGive(s_status_lock);
    } else {
        s_burn_task = NULL;
    }
    if (task_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

esp_err_t burner_start_task_ex(
    burner_job_mode_t mode,
    burner_cart_mode_t cart_mode,
    burner_write_path_t write_path,
    bool erase_always,
    bool gba_force_multi,
    bool gba_force_no_cfi,
    uint32_t mbc5_program_chunk_bytes,
    uint32_t read_chunk_bytes,
    uint32_t psram_window_bytes,
    const char *rom_name,
    const char *rom_path,
    uint32_t addr_begin,
    uint32_t total_bytes,
    burner_gba_save_type_t gba_save_type,
    bool ram_fram,
    uint8_t ram_latency)
{
    BaseType_t ret;
    burner_task_param_t *job = NULL;
    burner_core_affinity_t burn_task_affinity = BURNER_CORE_AFFINITY_AUTO;
    bool is_busy = false;

    if (rom_name == NULL || rom_path == NULL || total_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode != BURNER_JOB_ERASE_ROM && usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        is_busy = (s_burn_task != NULL);
        xSemaphoreGive(s_status_lock);
    } else {
        is_busy = (s_burn_task != NULL);
    }

    if (is_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    burner_cancel_reset();

    job = (burner_task_param_t *)calloc(1, sizeof(*job));
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }

    job->mode = mode;
    job->cart_mode = cart_mode;
    job->write_path = write_path;
    job->erase_always = erase_always;
    job->mbc5_program_chunk_bytes = burner_clamp_mbc5_program_chunk_bytes(mbc5_program_chunk_bytes);
    job->read_chunk_bytes = burner_dump_chunk_kb_to_bytes(
        burner_dump_chunk_bytes_to_kb(read_chunk_bytes));
    if (write_path == BURNER_WRITE_PATH_PIPELINE) {
        job->psram_window_bytes = 0u;
    } else {
        job->psram_window_bytes = burner_psram_window_mb_to_bytes(
            burner_psram_window_bytes_to_mb(psram_window_bytes));
    }
    snprintf(job->rom_name, sizeof(job->rom_name), "%s", rom_name);
    snprintf(job->rom_path, sizeof(job->rom_path), "%s", rom_path);
    job->addr_begin = addr_begin;
    job->total_bytes = total_bytes;
    job->gba_save_type = gba_save_type;
    job->ram_fram = ram_fram;
    job->ram_latency = ram_latency;
    job->gba_force_multi = gba_force_multi;
    job->gba_force_no_cfi = gba_force_no_cfi;

    if (mode == BURNER_JOB_WRITE_ROM) {
        burn_task_affinity = s_burn_core_cfg.psram_core;
    } else if (mode == BURNER_JOB_ERASE_ROM) {
        burn_task_affinity = s_burn_core_cfg.erase_core;
    } else {
        burn_task_affinity = BURNER_CORE_AFFINITY_CPU1;
    }

    job->task_with_caps = true;
    ret = xTaskCreatePinnedToCoreWithCaps(
        burner_task,
        "burn_task",
        BURN_TASK_STACK_BYTES,
        job,
        5,
        &s_burn_task,
        burner_core_affinity_to_task_core_id(burn_task_affinity),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        job->task_with_caps = false;
        ret = burner_create_task_with_affinity(
            burner_task,
            "burn_task",
            BURN_TASK_STACK_BYTES,
            job,
            5,
            &s_burn_task,
            burn_task_affinity);
    }
    if (ret != pdPASS) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGW(
            BURNER_TAG,
            "burn_task create failed: ret=%d stack=%u affinity=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
            (int)ret,
            (unsigned)BURN_TASK_STACK_BYTES,
            burner_core_affinity_to_str(burn_task_affinity),
            (unsigned)internal_free,
            (unsigned)internal_largest,
            (unsigned)psram_free,
            (unsigned)psram_largest);
        free(job);
        s_burn_task = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t burner_start_task(
    burner_job_mode_t mode,
    const char *rom_name,
    const char *rom_path,
    uint32_t addr_begin,
    uint32_t total_bytes,
    burner_gba_save_type_t gba_save_type,
    bool ram_fram,
    uint8_t ram_latency)
{
    return burner_start_task_ex(
        mode,
        BURNER_CART_MODE_MBC5,
        BURNER_WRITE_PATH_DIRECT,
        false,
        false,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        rom_name,
        rom_path,
        addr_begin,
        total_bytes,
        gba_save_type,
        ram_fram,
        ram_latency);
}

esp_err_t burner_start_gba_save_write_from_tf_new(
    const char *raw_name,
    burner_gba_save_type_t save_type,
    uint32_t save_size,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    (void)raw_name;
    (void)save_type;
    (void)save_size;
    (void)result;
    if (error_msg != NULL && error_msg_len > 0u) {
        snprintf(error_msg, error_msg_len, "%s", "new GBA save write not implemented");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t burner_start_gba_save_verify_from_tf_new(
    const char *raw_name,
    burner_gba_save_type_t save_type,
    uint32_t save_size,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    (void)raw_name;
    (void)save_type;
    (void)save_size;
    (void)result;
    if (error_msg != NULL && error_msg_len > 0u) {
        snprintf(error_msg, error_msg_len, "%s", "new GBA save verify not implemented");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t burner_start_gba_save_dump_new(
    const char *safe_name,
    const char *full_path,
    burner_gba_save_type_t save_type,
    uint32_t save_size)
{
    return burner_start_task_ex(
        BURNER_JOB_READ_GBA_SAVE_NEW,
        BURNER_CART_MODE_GBA,
        BURNER_WRITE_PATH_DIRECT,
        false,
        false,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        safe_name,
        full_path,
        0u,
        save_size,
        save_type,
        false,
        0u);
}

esp_err_t burner_reject_if_tf_busy(httpd_req_t *req)
{
    if (usb_msc_tf_in_use_by_host()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(
            req,
            "TF storage is in USB pass-through mode. Open / and disable USB pass-through first.");
    }
    return ESP_OK;
}

static const char *burner_guess_content_type(const char *path)
{
    const char *ext = NULL;

    if (path == NULL) {
        return "application/octet-stream";
    }

    ext = strrchr(path, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(ext, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcasecmp(ext, ".json") == 0) {
        return "application/json; charset=utf-8";
    }
    if (strcasecmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcasecmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcasecmp(ext, ".txt") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(ext, ".woff") == 0) {
        return "font/woff";
    }
    if (strcasecmp(ext, ".woff2") == 0) {
        return "font/woff2";
    }

    return "application/octet-stream";
}

static bool burner_strip_uri_suffix(const char *uri, char *out, size_t out_len)
{
    size_t i = 0;

    if (uri == NULL || out == NULL || out_len < 2) {
        return false;
    }

    while (uri[i] != '\0' && uri[i] != '?' && uri[i] != '#') {
        if (i + 1 >= out_len) {
            return false;
        }
        out[i] = uri[i];
        i++;
    }

    out[i] = '\0';
    return true;
}

bool burner_uri_to_web_rel_path(const char *uri, char *rel_path, size_t rel_path_len)
{
    char uri_path[TF_QUERY_LEN_MAX] = {0};
    char decoded[WEB_FILE_PATH_LEN_MAX] = {0};
    char normalized[WEB_FILE_PATH_LEN_MAX] = {0};
    const char *raw_rel = NULL;

    if (uri == NULL || rel_path == NULL || rel_path_len < 2) {
        return false;
    }

    if (!burner_strip_uri_suffix(uri, uri_path, sizeof(uri_path))) {
        return false;
    }

    if (strcmp(uri_path, "/") == 0 || strcmp(uri_path, "/tf") == 0 || strcmp(uri_path, "/cart") == 0 ||
        strcmp(uri_path, "/burner") == 0 || strcmp(uri_path, "/settings") == 0) {
        return snprintf(rel_path, rel_path_len, "%s", WEB_MAIN_FILE_REL) < (int)rel_path_len;
    }

    if (strncmp(uri_path, "/api/", 5) == 0) {
        return false;
    }

    raw_rel = uri_path;
    while (*raw_rel == '/') {
        raw_rel++;
    }

    if (*raw_rel == '\0') {
        return snprintf(rel_path, rel_path_len, "%s", WEB_MAIN_FILE_REL) < (int)rel_path_len;
    }

    if (!burner_url_decode(raw_rel, decoded, sizeof(decoded))) {
        return false;
    }

    if (!burner_normalize_rel_path(decoded, normalized, sizeof(normalized), false)) {
        return false;
    }

    if (strcmp(normalized, WEB_ROOT_DIR_REL) == 0) {
        return snprintf(rel_path, rel_path_len, "%s", WEB_MAIN_FILE_REL) < (int)rel_path_len;
    }

    if (strncmp(normalized, WEB_ROOT_DIR_REL "/", strlen(WEB_ROOT_DIR_REL) + 1) == 0) {
        return snprintf(rel_path, rel_path_len, "%s", normalized) < (int)rel_path_len;
    }

    return snprintf(rel_path, rel_path_len, WEB_ROOT_DIR_REL "/%s", normalized) < (int)rel_path_len;
}

esp_err_t burner_send_static_file(httpd_req_t *req, const char *rel_path)
{
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    struct stat st;
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    const char *content_type = NULL;

    if (req == NULL || rel_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web file path too long");
    }

    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "web file not found");
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open web file failed");
    }

    buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(fp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    content_type = burner_guess_content_type(rel_path);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while (true) {
        size_t read_len = fread(buf, 1, TF_IO_CHUNK_SIZE, fp);
        if (read_len == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, read_len) != ESP_OK) {
            free(buf);
            fclose(fp);
            return ESP_FAIL;
        }
    }

    if (ferror(fp)) {
        free(buf);
        fclose(fp);
        return ESP_FAIL;
    }

    free(buf);
    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}












#include "ui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_app_desc.h"
#include "app_config.h"
#include "app_registry.h"
#include "burner/db/burner_nor_db.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ip5306.h"
#include "boot_partition.h"
#include "lcd_display.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "libs/qrcode/qrcodegen.h"
#include "music_player.h"
#include "mori_system_settings.h"
#include "smb_client.h"
#include "power_manager.h"
#include "epub_native.h"
#include "usb_msc_tf.h"
#include "wifi_manager.h"
#include "burner/core/ws_server_internal.h"
#include "ui_text.h"

#define UI_TAG "ui"
#define UI_SCREEN_W 320
#define UI_SCREEN_H 240
#define UI_TOP_H 0
#define UI_STATUS_H 0
#define UI_CONTENT_Y UI_TOP_H
#define UI_CONTENT_H (UI_SCREEN_H - UI_TOP_H - UI_STATUS_H)

#define UI_CANVAS_W UI_SCREEN_W
#define UI_CANVAS_H UI_CONTENT_H
#define UI_CANVAS_PIXELS (UI_CANVAS_W * UI_CANVAS_H)
#define UI_COLOR_BLACK 0x0000U
#define UI_COLOR_WHITE 0xFFFFU

#define UI_TILE_GRID_COLS 4
#define UI_TILE_GRID_ROWS 2
#define UI_TILE_PAGE_H 38
#define UI_TILE_PAGE_GAP_X 8
#define UI_TILE_PAGE_GAP_Y 12
#define UI_TILE_PAGE_TOP 38
#define UI_TILE_PAGE_BOTTOM_HINT_GAP 10
#define UI_TILE_CELL_W ((UI_CANVAS_W - UI_TILE_PAGE_GAP_X * (UI_TILE_GRID_COLS + 1)) / UI_TILE_GRID_COLS)
#define UI_TILE_CELL_H ((UI_CANVAS_H - UI_TILE_PAGE_TOP - UI_HINT_H - UI_TILE_PAGE_BOTTOM_HINT_GAP - UI_TILE_PAGE_GAP_Y * (UI_TILE_GRID_ROWS - 1)) / UI_TILE_GRID_ROWS)
#define UI_TILE_ICON_AUTO_SIZE ((UI_TILE_CELL_W < UI_TILE_CELL_H ? UI_TILE_CELL_W : UI_TILE_CELL_H) - 20)
#define UI_TILE_ICON_SIZE ((UI_TILE_ICON_AUTO_SIZE < 28) ? 28 : ((UI_TILE_ICON_AUTO_SIZE > 44) ? 44 : UI_TILE_ICON_AUTO_SIZE))
#define UI_TILE_ICON_Y 52
#define UI_TILE_ICON_SPACING (UI_TILE_ICON_SIZE + (UI_TILE_ICON_SIZE / 2))
#define UI_TILE_SELECTOR_MARGIN 5
#define UI_TILE_TITLE_Y (UI_TILE_ICON_Y + UI_TILE_ICON_SIZE + 30)
#define UI_TILE_HINT_Y (UI_TILE_TITLE_Y + 16)
#define UI_TILE_DOTTED_Y (UI_TILE_HINT_Y + 16)
#define UI_TILE_ARROW_Y 214
#define UI_TILE_BAR_H 2
#define UI_TILE_SELECT_LINE 5
#define UI_TILE_HEADER_Y0 0
#define UI_TILE_HEADER_H 38
#define UI_TILE_ICON_DYNAMIC_Y0 (UI_TILE_ICON_Y - UI_TILE_SELECTOR_MARGIN)
#define UI_TILE_ICON_DYNAMIC_H (UI_TILE_ICON_SIZE + UI_TILE_SELECTOR_MARGIN * 2)
#define UI_TEXT_GLYPH_TOP_OFFSET ((UI_CJK_Y_OFFSET < UI_ASCII_ZH_Y_OFFSET) ? UI_CJK_Y_OFFSET : UI_ASCII_ZH_Y_OFFSET)
#define UI_TEXT_GLYPH_H ((UI_HZK16_H > UI_ASCII_ZH_H) ? UI_HZK16_H : UI_ASCII_ZH_H)
#define UI_TILE_TEXT_DYNAMIC_Y0 (UI_TILE_TITLE_Y + UI_TEXT_GLYPH_TOP_OFFSET - 1)
#define UI_TILE_TEXT_DYNAMIC_H (UI_TILE_HINT_Y - UI_TILE_TITLE_Y + UI_TEXT_GLYPH_H + 2)
#define UI_TILE_DOT_DYNAMIC_Y0 (UI_TILE_DOTTED_Y - 1)
#define UI_TILE_DOT_DYNAMIC_H 4
#define UI_BURNER_ICON_W 86
#define UI_BURNER_ICON_H 72
#define UI_BURNER_ICON_GBA_X 56
#define UI_BURNER_ICON_GBC_X 178
#define UI_BURNER_ICON_Y 76
#define UI_BURNER_SELECT_MARGIN 5

#define UI_LIST_LINE_H 16
#define UI_LIST_TEXT_X 4
#define UI_LIST_BAR_W 5
#define UI_LIST_SELECTOR_MARGIN 4
#define UI_FILE_SIZE_COL_W 64
#define UI_FILE_NAME_SIZE_GAP 6
#define UI_FILE_MARQUEE_START_MS 650U
#define UI_FILE_MARQUEE_STEP_MS 18U
#define UI_FILE_MARQUEE_END_PAUSE_STEPS 8
#define UI_BURN_ROW_PROMPT_MS 2000U
#define UI_HINT_H 14
#define UI_LIST_HEADER_H 28
#define UI_LIST_HEADER_TEXT_Y 7
#define UI_LIST_VISIBLE_COUNT ((UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H) / UI_LIST_LINE_H)

#define UI_STATUS_TEXT_MAX_LEN 96
#define UI_IP_TEXT_MAX_LEN 32
#define UI_TIME_TEXT_MAX_LEN 8
#define UI_FPS_TEXT_MAX_LEN 12
#define UI_TITLE_TEXT_MAX_LEN 32

#define UI_HOME_FIXED_COUNT APP_REGISTRY_HOME_FIXED_COUNT
#define UI_HOME_APP_MAX APP_REGISTRY_HOME_APP_MAX
#define UI_HOME_ICON_MAX APP_REGISTRY_HOME_ICON_MAX
#define UI_TF_ITEM_COUNT 7
#define UI_WIFI_ITEM_COUNT 8
#define UI_SMB_BASE_ITEM_COUNT 12
#define UI_SMB_INPUT_ACTION_COUNT 3U
#define UI_POWER_ITEM_COUNT 14
#define UI_SYSTEM_ITEM_COUNT 5
#define UI_BURNER_MODE_COUNT 2
#define UI_BURN_ROM_LOCKED_ITEM_COUNT 3
#define UI_BURN_ROM_WRITE_PATH_ITEM_COUNT 4
#define UI_BURN_ROM_RECIPE_ITEM_COUNT 3
#define UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT 5
#define UI_BURN_ROM_DUMP_KEY_COUNT 13
#define UI_BURN_ROM_MAPPER_ITEM_COUNT 2
#define UI_BURN_ROM_GBA_SETTINGS_ITEM_COUNT 5
#define UI_BURN_ROM_MBC5_SETTINGS_ITEM_COUNT 6
#define UI_BURN_ROM_ERASE_CONFIRM_ITEM_COUNT 2
#define UI_BURN_RAM_ITEM_COUNT 8
#define UI_BURN_ROM_CUSTOM_SIZE_TEXT_MAX 16
#define UI_BURN_ROM_GBA_WITH_ROM_ITEM_COUNT 9
#define UI_BURN_ROM_MBC5_WITH_ROM_ITEM_COUNT 10
#define UI_BURN_ROM_ACTION_ROWS UI_LIST_VISIBLE_COUNT
#define UI_BURN_SPLIT_GAP 8
#define UI_BURN_SIDE_MARGIN 4
#define UI_BURN_INFO_W 146
#define UI_BURN_OPS_W (UI_CANVAS_W - (UI_BURN_SIDE_MARGIN * 2) - UI_BURN_SPLIT_GAP - UI_BURN_INFO_W)
#define UI_BURN_SAVE_ITEM_COUNT 8
#define UI_SETTINGS_ITEM_COUNT 10
#define UI_TASK_STATUS_ITEM_COUNT 12
#define UI_TASK_PATCH_BASE_ROW UI_TASK_STATUS_ITEM_COUNT
#define UI_TASK_RESULT_ITEM_COUNT 18
#define UI_TASK_CANCEL_CONFIRM_ITEM_COUNT 2U
#define UI_TASK_ERASE_PROGRESS_ROW 6U
#define UI_TASK_BURN_PROGRESS_ROW 7U
#define UI_ROW_COUNT UI_LIST_VISIBLE_COUNT
#define UI_ROW_TEXT_MAX_LEN 96
#define UI_FILE_NAME_MAX_LEN 128
#define UI_WIFI_WEB_URL_MAX_LEN 48
#define UI_WIFI_QR_VERSION 2
#define UI_WIFI_QR_QUIET_MODULES 4
#define UI_WIFI_QR_MODULE_PX 5
#define UI_FILE_WINDOW_COUNT (UI_ROW_COUNT * 3U)
#define UI_FILE_SCAN_LIMIT 512U
#define UI_FILE_START_TASK_STACK_SIZE (16U * 1024U)
#define UI_FILE_START_TASK_PRIORITY 4
#define UI_WIFI_TASK_STACK_SIZE 4096U
#define UI_WIFI_TASK_PRIORITY 3
#define UI_SMB_TASK_STACK_SIZE (12U * 1024U)
#define UI_SMB_SCAN_TIMEOUT_MS 15000U
#define UI_STORAGE_TASK_STACK_SIZE 4096U
#define UI_BOOT_RETRO_GO_TASK_STACK_SIZE (12U * 1024U)
#define UI_STORAGE_TASK_PRIORITY 3
#define UI_BURN_WORK_TASK_STACK_SIZE (16U * 1024U)
#define UI_BURN_PROBE_TASK_STACK_SIZE (24U * 1024U)
#define UI_SYSTEM_TASK_CORE_ID 0
#define UI_BURN_TASK_CORE_ID 1
#define UI_BUTTON_QUEUE_LEN 16
#define UI_BUTTONS_PER_FRAME 8
#define UI_BUTTON_REPEAT_START_FRAMES 16U
#define UI_BUTTON_REPEAT_INTERVAL_FRAMES 3U
#define UI_BUTTON_COUNT ((uint8_t)UI_BUTTON_VOL_DOWN + 1U)
#define UI_MUSIC_SEEK_HOLD_MS 100U
#define UI_MUSIC_TOGGLE_DEFER_MS 100U
#define UI_SETTING_HOLD_REPEAT_MS 100U
#define UI_SETTING_HOLD_FAST_MS 1000U
#define UI_SETTING_HOLD_REPEAT_INTERVAL_MS 70U
#define UI_CLOCK_REFRESH_MS 1000U
#define UI_FPS_REFRESH_MS 1000U
#define UI_BATTERY_REFRESH_MS 5000U
#define UI_LIVE_REFRESH_MS 250U
#define UI_FILE_PSRAM_WINDOW_MB 4U
#define UI_MUSIC_DIR "music"
#define UI_MUSIC_HISTORY_PATH mount_point "/.setting/music_history.ini"
#define UI_MUSIC_HISTORY_SAVE_MS 2000U
#define UI_MUSIC_HISTORY_SAVE_DELTA_BYTES 65536U
#define UI_READER_HISTORY_PATH mount_point "/.setting/reader_history.ini"
#define UI_MUSIC_DRAWER_W 160
#define UI_MUSIC_DRAWER_X_CLOSED (-UI_MUSIC_DRAWER_W)
#define UI_MUSIC_DRAWER_Y UI_LIST_HEADER_H
#define UI_MUSIC_DRAWER_H (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H)
#define UI_MUSIC_DRAWER_VISIBLE_ROWS ((UI_MUSIC_DRAWER_H - 18) / UI_LIST_LINE_H)
#define UI_MUSIC_INFO_Y 78
#define UI_MUSIC_VOLUME_Y 106
#define UI_MUSIC_TIME_Y 132
#define UI_MUSIC_SEEK_BAR_X 26
#define UI_MUSIC_SEEK_BAR_W (UI_CANVAS_W - 52)
#define UI_MUSIC_SEEK_BAR_Y 154
#define UI_MUSIC_META_Y 168
#define UI_MUSIC_CTRL_Y 180
#define UI_MUSIC_CTRL_SIDE_SIZE 28
#define UI_MUSIC_CTRL_MAIN_SIZE 36
#define UI_MUSIC_HINT2_Y 214
#define UI_MUSIC_SEEK_DELTA_BYTES 131072
#define UI_MUSIC_RESUME_TAIL_GUARD_BYTES 262144U
#define UI_MUSIC_RESUME_MAX_PERCENT 98U
#define UI_READER_MARGIN_X 6
#define UI_READER_TOP_Y (UI_LIST_HEADER_H + 6)
#define UI_READER_LINE_H 18
#define UI_READER_TEXT_W (UI_CANVAS_W - UI_READER_MARGIN_X * 2)
#define UI_READER_TEXT_H (UI_CANVAS_H - UI_LIST_HEADER_H - UI_HINT_H - 10)
#define UI_READER_MAX_LINES ((UI_READER_TEXT_H / UI_READER_LINE_H) > 0 ? (UI_READER_TEXT_H / UI_READER_LINE_H) : 1)
#define UI_READER_MAX_FILE_BYTES (16U * 1024U * 1024U)
#define UI_READER_MAX_PAGE_COUNT 262144U
#define UI_READER_SCAN_CHUNK_BYTES 4096U
#define UI_READER_WINDOW_PAGES 24U
#define UI_READER_PREFETCH_PERCENT 70U
#define UI_TILE_CAMERA_SPEED 92.0f
#define UI_TILE_BAR_SPEED 88.0f
#define UI_TILE_FORE_SPEED 90.0f
#define UI_LIST_SELECTOR_Y_SPEED 96.0f
#define UI_LIST_SELECTOR_W_SPEED 94.0f
#define UI_LIST_BAR_SPEED 92.0f
#define UI_ASCII_W 5
#define UI_ASCII_H 7
#define UI_ASCII_ADVANCE 6
#define UI_ASCII_ZH_W 7
#define UI_ASCII_ZH_H 10
#define UI_ASCII_ZH_ADVANCE 8
#define UI_ASCII_ZH_Y_OFFSET (-1)
#define UI_CJK_ADVANCE 17
#define UI_CJK_Y_OFFSET (-4)
#define UI_HZK16_PATH "/assets/hzk16.bin"
#define UI_HZK16_W 16
#define UI_HZK16_H 16
#define UI_HZK16_BYTES_PER_GLYPH 32U
#define UI_HZK16_QH_MIN 0xA1U
#define UI_HZK16_QH_MAX 0xF7U
#define UI_HZK16_WH_MIN 0xA1U
#define UI_HZK16_WH_MAX 0xFEU
#define UI_HZK16_COLS 94U
#define UI_HZK16_GLYPH_COUNT (((UI_HZK16_QH_MAX - UI_HZK16_QH_MIN) + 1U) * UI_HZK16_COLS)
#define UI_HZK16_FILE_SIZE (UI_HZK16_GLYPH_COUNT * UI_HZK16_BYTES_PER_GLYPH)

typedef enum {
    UI_PAGE_ROOT = 0,
    UI_PAGE_SYSTEM,
    UI_PAGE_APP_MANAGER,
    UI_PAGE_TF,
    UI_PAGE_MUSIC_FILES,
    UI_PAGE_MUSIC_PLAYER,
    UI_PAGE_FILES,
    UI_PAGE_READER,
    UI_PAGE_FILE_ACTIONS,
    UI_PAGE_WIFI,
    UI_PAGE_WIFI_QR,
    UI_PAGE_SMB,
    UI_PAGE_SMB_TEXT_INPUT,
    UI_PAGE_POWER,
    UI_PAGE_BURNER,
    UI_PAGE_BURN_ROM,
    UI_PAGE_BURN_SAVE,
    UI_PAGE_SETTINGS,
    UI_PAGE_TASK_STATUS,
    UI_PAGE_TASK_RESULT,
} ui_page_t;

typedef enum {
    UI_ACTION_OPEN_SYSTEM = 0,
    UI_ACTION_OPEN_APP_MANAGER,
    UI_ACTION_OPEN_TF,
    UI_ACTION_OPEN_READER,
    UI_ACTION_OPEN_MUSIC,
    UI_ACTION_OPEN_WIFI,
    UI_ACTION_OPEN_POWER,
    UI_ACTION_OPEN_BURNER,
    UI_ACTION_OPEN_SETTINGS,
    UI_ACTION_OPEN_RETRO_GO,
} ui_action_t;

typedef enum {
    UI_FILE_KIND_UNSUPPORTED = 0,
    UI_FILE_KIND_ROM_GBA,
    UI_FILE_KIND_ROM_MBC5,
    UI_FILE_KIND_SAVE,
    UI_FILE_KIND_AUDIO,
    UI_FILE_KIND_READER,
} ui_file_kind_t;

typedef enum {
    UI_FILE_FILTER_NONE = 0,
    UI_FILE_FILTER_ROM_GBA,
    UI_FILE_FILTER_ROM_MBC5,
    UI_FILE_FILTER_SAVE,
    UI_FILE_FILTER_AUDIO,
    UI_FILE_FILTER_READER,
} ui_file_filter_t;

#define UI_FILE_KIND_MP3 UI_FILE_KIND_AUDIO
#define UI_FILE_FILTER_MP3 UI_FILE_FILTER_AUDIO

typedef enum {
    UI_FILE_ACTION_BURN_PSRAM = 0,
    UI_FILE_ACTION_BURN_PIPELINE,
    UI_FILE_ACTION_BURN_DIRECT,
    UI_FILE_ACTION_VERIFY_ROM,
    UI_FILE_ACTION_WRITE_SAVE,
    UI_FILE_ACTION_VERIFY_SAVE,
    UI_FILE_ACTION_WRITE_GBA_SAVE_NEW,
    UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW,
} ui_file_action_t;

typedef enum {
    UI_BURN_ROM_OP_RECIPE_MODE = 0,
    UI_BURN_ROM_OP_ANALYZE,
    UI_BURN_ROM_OP_CHOOSE_ROM,
    UI_BURN_ROM_OP_ROM_MAPPER,
    UI_BURN_ROM_OP_WRITE_ROM,
    UI_BURN_ROM_OP_VERIFY_ROM,
    UI_BURN_ROM_OP_DUMP_ROM,
    UI_BURN_ROM_OP_ERASE_CHIP,
    UI_BURN_ROM_OP_UNLOCK_PPB,
    UI_BURN_ROM_OP_RAM_MENU,
    UI_BURN_ROM_OP_CHOOSE_SAVE,
    UI_BURN_ROM_OP_WRITE_SAVE,
    UI_BURN_ROM_OP_VERIFY_SAVE,
    UI_BURN_ROM_OP_DUMP_SAVE,
    UI_BURN_ROM_OP_SAVE_SIZE,
    UI_BURN_ROM_OP_GBA_SAVE_TYPE,
    UI_BURN_ROM_OP_RAM_TYPE,
    UI_BURN_ROM_OP_RAM_LATENCY,
    UI_BURN_ROM_OP_DUMP_BATTERYLESS,
    UI_BURN_ROM_OP_SETTINGS,
    UI_BURN_ROM_OP_INVALID,
} ui_burn_rom_op_t;

typedef enum {
    UI_BURN_ROM_SUBMENU_NONE = 0,
    UI_BURN_ROM_SUBMENU_WRITE,
    UI_BURN_ROM_SUBMENU_SAVE_PATCH,
    UI_BURN_ROM_SUBMENU_RECIPE_MODE,
    UI_BURN_ROM_SUBMENU_DUMP_SIZE,
    UI_BURN_ROM_SUBMENU_DUMP_CUSTOM,
    UI_BURN_ROM_SUBMENU_MAPPER,
    UI_BURN_ROM_SUBMENU_SETTINGS,
    UI_BURN_ROM_SUBMENU_ERASE_CONFIRM,
    UI_BURN_ROM_SUBMENU_RAM,
} ui_burn_rom_submenu_t;

typedef enum {
    UI_SMB_FIELD_HOST = 0,
    UI_SMB_FIELD_SHARE,
    UI_SMB_FIELD_USER,
    UI_SMB_FIELD_PASSWORD,
    UI_SMB_FIELD_DOMAIN,
} ui_smb_field_t;

typedef enum {
    UI_SETTING_ADJUST_NONE = 0,
    UI_SETTING_ADJUST_BRIGHTNESS,
    UI_SETTING_ADJUST_VOLUME,
} ui_setting_adjust_t;

typedef struct {
    char name[UI_FILE_NAME_MAX_LEN];
    char path[TF_PATH_LEN_MAX];
    uint32_t size;
    bool is_dir;
    uint32_t ordinal;
} ui_file_entry_t;

typedef struct {
    char name[UI_FILE_NAME_MAX_LEN];
    char path[TF_PATH_LEN_MAX];
    uint32_t size;
    ui_file_kind_t kind;
    ui_file_action_t action;
    burner_cart_mode_t cart_mode;
    burner_recipe_mode_t recipe_mode;
    uint32_t slot;
    burner_write_path_t write_path;
    bool erase_always;
    uint32_t psram_mb;
    uint32_t mbc5_chunk_kb;
    bool gba_force_no_cfi;
    bool gba_sram_patch;
    bool gba_waitcnt_patch;
    bool gba_batteryless_patch;
    burner_gba_save_type_t gba_save_type;
    uint32_t gba_save_size;
    char gbx_profile_file[BURNER_GBX_PROFILE_NAME_LEN];
    bool ram_fram;
    uint8_t ram_latency;
    bool task_with_caps;
    ui_page_t return_page;
} ui_file_start_request_t;

typedef struct {
    char title[UI_TITLE_TEXT_MAX_LEN];
    char hint[UI_ROW_TEXT_MAX_LEN];
    const char *symbol;
    ui_action_t action;
    uint32_t accent;
} ui_menu_item_t;

typedef struct {
    ui_page_t page;
    ui_page_t parent_page;
    uint16_t selected;
    uint16_t scroll;
    uint16_t file_selected;
    uint16_t file_scroll;
    uint16_t file_total;
    uint16_t file_window_start;
    uint16_t file_loaded_count;
    char file_path[TF_PATH_LEN_MAX];
    bool file_book_scope;
    uint32_t reader_page;
    ui_file_entry_t file_window[UI_FILE_WINDOW_COUNT];
    ui_file_entry_t action_file;
    ui_file_kind_t action_kind;
    ui_file_filter_t file_filter;
    ui_wifi_state_t wifi_state;
    char ip_text[UI_IP_TEXT_MAX_LEN];
    char status_text[UI_STATUS_TEXT_MAX_LEN];
    char time_text[UI_TIME_TEXT_MAX_LEN];
    char fps_text[UI_FPS_TEXT_MAX_LEN];
    int burn_progress;
    uint32_t burn_processed;
    uint32_t burn_total;
    int erase_progress;
    uint32_t erase_done_sectors;
    uint32_t erase_total_sectors;
    uint64_t burn_elapsed_us;
    bool gba_patch_sram;
    bool gba_patch_batteryless;
    bool gba_patch_waitcnt;
    int gba_patch_progress[3];
    char gba_patch_message[3][32];
    bool task_result_status_valid;
    uint8_t battery_percent;
    bool battery_valid;
    bool battery_charging;
    bool power_valid;
    power_manager_telemetry_t power_telemetry;
    bool dirty;
    bool motion_dirty;
    bool content_dirty;
    bool chrome_dirty;
    bool music_player_dirty;
    bool music_progress_dirty;
    ui_setting_adjust_t settings_adjust;
} ui_model_t;

typedef struct {
    bool valid;
    ui_file_entry_t current;
    bool drawer_open;
} ui_music_context_t;

typedef struct {
    uint8_t *text_data;
    size_t text_len;
    uint32_t *page_text_offsets;
    uint32_t page_start;
    uint32_t page_count;
} ui_reader_window_t;

typedef enum {
    UI_READER_SOURCE_TEXT = 0,
    UI_READER_SOURCE_EPUB,
} ui_reader_source_t;

typedef struct {
    bool active;
    bool utf8_text;
    ui_reader_source_t source;
    char path[TF_PATH_LEN_MAX];
    char name[UI_FILE_NAME_MAX_LEN];
    ui_reader_window_t window;
    ui_reader_window_t prefetch;
    uint32_t *page_offsets;
    uint32_t *section_page_starts;
    uint32_t section_count;
    ui_epub_book_t *epub_book;
    uint32_t page_count;
    uint32_t file_size;
} ui_reader_context_t;

typedef struct {
    smb_client_discovery_entry_t servers[SMB_CLIENT_DISCOVERY_MAX];
    uint16_t server_count;
} ui_smb_scan_result_t;

typedef struct {
    smb_client_favorite_t favorites[SMB_CLIENT_FAVORITE_MAX];
    uint16_t favorite_count;
} ui_smb_favorite_result_t;

typedef struct {
    smb_client_discovery_entry_t servers[SMB_CLIENT_DISCOVERY_MAX];
    smb_client_favorite_t favorites[SMB_CLIENT_FAVORITE_MAX];
    uint16_t server_count;
    uint16_t favorite_count;
    smb_client_config_t config;
    bool initialized;
    ui_smb_field_t editing_field;
    uint32_t selected_favorite_id;
} ui_smb_context_t;

typedef struct {
    ui_button_t button;
    ui_input_action_t action;
    bool pressed;
} ui_button_event_t;

typedef struct {
    bool pressed;
    uint16_t frames_until_repeat;
    uint16_t repeat_count;
    uint32_t pressed_since_ms;
    uint32_t last_repeat_ms;
    bool hold_action_started;
    ui_input_action_t action;
} ui_button_state_t;

typedef struct {
    ui_page_t page;
    ui_input_action_t mapping[UI_BUTTON_COUNT];
} ui_button_page_map_t;

typedef enum {
    UI_WORK_WIFI_CONNECT_SAVED = 0,
    UI_WORK_WIFI_START_AP,
    UI_WORK_WIFI_DISCONNECT,
    UI_WORK_WIFI_CLOSE_AP,
    UI_WORK_WIFI_CLEAR_SAVED,
    UI_WORK_STORAGE_USB_ENABLE,
    UI_WORK_STORAGE_USB_DISABLE,
    UI_WORK_BURN_READ_ID,
    UI_WORK_BURN_ERASE_CHIP,
    UI_WORK_BURN_DUMP_ROM,
    UI_WORK_BURN_DUMP_SAVE,
    UI_WORK_BURN_UNLOCK_PPB,
    UI_WORK_DEVICE_RESTART,
    UI_WORK_UPDATE_GBX_CACHE,
    UI_WORK_BOOT_RETRO_GO,
    UI_WORK_SMB_SCAN,
    UI_WORK_SMB_LOAD_FAVORITES,
    UI_WORK_SMB_SAVE_FAVORITE,
    UI_WORK_SMB_CONNECT,
    UI_WORK_SMB_DISCONNECT,
} ui_work_type_t;

typedef struct {
    ui_work_type_t type;
    burner_cart_mode_t cart_mode;
    bool task_with_caps;
} ui_work_request_t;

typedef struct {
    ui_page_t page;
    ui_page_t parent;
} ui_nav_entry_t;

typedef struct {
    int32_t info_x;
    int32_t info_y;
    int32_t info_w;
    int32_t info_h;
    int32_t ops_x;
    int32_t ops_y;
    int32_t ops_w;
    int32_t ops_h;
} ui_burn_layout_t;

typedef struct {
    const ui_menu_item_t *items;
    uint16_t count;
    uint8_t cols;
    uint8_t rows;
} ui_icon_page_config_t;

typedef struct {
    float tile_camera_x;
    float tile_camera_target_x;
    float tile_bar_w;
    float tile_bar_target_w;
    float tile_fore_y;
    float tile_fore_target_y;
    float list_selector_y;
    float list_selector_target_y;
    float list_selector_w;
    float list_selector_target_w;
    float list_bar_h;
    float list_bar_target_h;
    float list_bar_x;
    float list_bar_target_x;
    float list_camera_y;
    float list_camera_target_y;
    uint16_t marquee_selected;
    uint32_t marquee_selected_since_ms;
    uint16_t burner_prev_selected;
    uint16_t icon_prev_selected;
    ui_page_t page;
    bool page_changed;
    int32_t list_prev_selector_y;
    int32_t list_prev_selector_w;
    int32_t list_prev_bar_h;
    int32_t list_prev_bar_x;
    float music_drawer_x;
    float music_drawer_target_x;
} ui_anim_state_t;

static SemaphoreHandle_t s_model_lock = NULL;
static QueueHandle_t s_button_queue = NULL;
static ui_button_state_t s_button_states[UI_BUTTON_COUNT];
static volatile uint32_t s_last_activity_ms = 0;
static volatile uint32_t s_last_network_activity_ms = 0;
static void (*s_activity_cb)(void) = NULL;
static uint16_t s_root_selected = 0;
static bool s_ui_inited = false;
static uint8_t s_ui_language = UI_LANGUAGE_DEFAULT;
static bool s_button_map_inited = false;
static bool s_file_start_active = false;
static bool s_wifi_work_active = false;
static bool s_smb_work_active = false;
static bool s_storage_work_active = false;
static bool s_burn_probe_active = false;
static bool s_task_cancel_confirm = false;
static bool s_task_cancel_exit_pending = false;
static bool s_task_cancel_request_pending = false;
static bool s_cart_analyzed = false;
static burner_cart_mode_t s_analyzed_cart_mode = BURNER_CART_MODE_GBA;
static ui_page_t s_task_cancel_return_page = UI_PAGE_BURNER;
static char s_analyzed_cart_info[UI_STATUS_TEXT_MAX_LEN] = "";
static bool s_burn_rom_write_menu = false;
static uint32_t s_last_clock_refresh_ms = 0;
static uint32_t s_last_fps_refresh_ms = 0;
static uint32_t s_render_frames_this_second = 0;
static uint32_t s_last_battery_refresh_ms = 0;
static uint32_t s_last_live_refresh_ms = 0;
static uint32_t s_last_button_queue_full_log_ms = 0;
static DRAM_ATTR burner_status_t s_task_result_status = {0};
static bool s_task_result_status_valid = false;
static bool s_task_result_capture_armed = false;
static bool s_task_result_active_seen = false;
static bool s_model_defaults_inited = false;
static ui_music_context_t s_music_ctx = {0};
static ui_reader_context_t s_reader_ctx = {0};
static ui_smb_context_t s_smb_ctx = {0};
static music_player_snapshot_t s_music_snapshot_cache = {0};
static bool s_music_snapshot_cached = false;
static char s_music_history_saved_path[TF_PATH_LEN_MAX] = {0};
static uint32_t s_music_history_saved_position = UINT32_MAX;
static music_player_state_t s_music_history_saved_state = MUSIC_PLAYER_STATE_IDLE;
static uint32_t s_music_history_last_save_ms = 0;
static bool s_music_toggle_pending = false;
static uint32_t s_music_toggle_due_ms = 0;
static ui_button_page_map_t s_button_page_maps[] = {
    {.page = UI_PAGE_ROOT},
    {.page = UI_PAGE_SYSTEM},
    {.page = UI_PAGE_APP_MANAGER},
    {.page = UI_PAGE_TF},
    {.page = UI_PAGE_MUSIC_FILES},
    {.page = UI_PAGE_MUSIC_PLAYER},
    {.page = UI_PAGE_FILES},
    {.page = UI_PAGE_READER},
    {.page = UI_PAGE_FILE_ACTIONS},
    {.page = UI_PAGE_WIFI},
    {.page = UI_PAGE_WIFI_QR},
    {.page = UI_PAGE_SMB},
    {.page = UI_PAGE_SMB_TEXT_INPUT},
    {.page = UI_PAGE_POWER},
    {.page = UI_PAGE_BURNER},
    {.page = UI_PAGE_BURN_ROM},
    {.page = UI_PAGE_BURN_SAVE},
    {.page = UI_PAGE_SETTINGS},
    {.page = UI_PAGE_TASK_STATUS},
    {.page = UI_PAGE_TASK_RESULT},
};

static lv_obj_t *s_canvas = NULL;
static uint16_t *s_canvas_buf = NULL;

static void ui_task_cancel_confirm_reset_locked(void);
static void ui_task_cancel_confirm_open_locked(ui_model_t *model);
static void ui_task_cancel_confirm_close_locked(ui_model_t *model, const char *status_key);
static void ui_issue_pending_task_cancel(void);
static bool ui_task_status_operation_active(void);
static ui_page_t ui_task_return_parent_page(ui_page_t page);
static ui_page_t ui_burn_task_entry_return_page(ui_page_t page);
static void ui_open_page_locked(ui_model_t *model, ui_page_t page);
static void ui_set_status_locked(ui_model_t *model, const char *text);
static void ui_mark_content_dirty(ui_model_t *model);
static void ui_mark_chrome_dirty(ui_model_t *model);
static void ui_clear_task_result_runtime_locked(ui_model_t *model);
static void ui_calc_burn_snapshot_fields(
    const burner_status_t *status,
    int *progress_out,
    int *erase_progress_out,
    uint32_t *processed_out,
    uint32_t *total_out,
    uint32_t *erase_done_out,
    uint32_t *erase_total_out);
static void ui_present_active_burn_task_locked(ui_model_t *model, const burner_status_t *status, ui_page_t return_page);

static EXT_RAM_BSS_ATTR ui_model_t s_model;

static void ui_reset_model_defaults(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }

    memset(model, 0, sizeof(*model));
    model->page = UI_PAGE_ROOT;
    model->parent_page = UI_PAGE_ROOT;
    model->wifi_state = UI_WIFI_STATE_UNKNOWN;
    snprintf(model->ip_text, sizeof(model->ip_text), "%s", "--");
    snprintf(model->status_text, sizeof(model->status_text), "%s", "system initializing");
    snprintf(model->time_text, sizeof(model->time_text), "%s", "--:--");
    snprintf(model->fps_text, sizeof(model->fps_text), "%s", "FPS --");
    model->dirty = true;
}

static void ui_smb_ensure_context(void)
{
    smb_client_status_t status = {0};

    if (s_smb_ctx.initialized) {
        return;
    }
    smb_client_get_status(&status);
    s_smb_ctx.config = status.config;
    if (s_smb_ctx.config.port <= 0) {
        s_smb_ctx.config.port = 445;
    }
    s_smb_ctx.initialized = true;
}

static uint16_t ui_smb_item_count(void)
{
    return (uint16_t)(UI_SMB_BASE_ITEM_COUNT + s_smb_ctx.favorite_count + s_smb_ctx.server_count);
}

static const char *ui_smb_field_label(ui_smb_field_t field)
{
    switch (field) {
        case UI_SMB_FIELD_HOST:
            return "Host";
        case UI_SMB_FIELD_SHARE:
            return "Share";
        case UI_SMB_FIELD_USER:
            return "User";
        case UI_SMB_FIELD_PASSWORD:
            return "Password";
        case UI_SMB_FIELD_DOMAIN:
            return "Domain";
        default:
            return "Field";
    }
}

static char *ui_smb_field_buffer(ui_smb_field_t field, size_t *len_out)
{
    ui_smb_ensure_context();
    switch (field) {
        case UI_SMB_FIELD_HOST:
            if (len_out != NULL) {
                *len_out = sizeof(s_smb_ctx.config.host);
            }
            return s_smb_ctx.config.host;
        case UI_SMB_FIELD_SHARE:
            if (len_out != NULL) {
                *len_out = sizeof(s_smb_ctx.config.share);
            }
            return s_smb_ctx.config.share;
        case UI_SMB_FIELD_USER:
            if (len_out != NULL) {
                *len_out = sizeof(s_smb_ctx.config.user);
            }
            return s_smb_ctx.config.user;
        case UI_SMB_FIELD_PASSWORD:
            if (len_out != NULL) {
                *len_out = sizeof(s_smb_ctx.config.password);
            }
            return s_smb_ctx.config.password;
        case UI_SMB_FIELD_DOMAIN:
            if (len_out != NULL) {
                *len_out = sizeof(s_smb_ctx.config.domain);
            }
            return s_smb_ctx.config.domain;
        default:
            if (len_out != NULL) {
                *len_out = 0;
            }
            return NULL;
    }
}

static const char *ui_smb_input_charset(void)
{
    return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_@/ ";
}

static uint16_t ui_smb_text_input_count(void)
{
    return (uint16_t)(strlen(ui_smb_input_charset()) + UI_SMB_INPUT_ACTION_COUNT);
}

static char ui_smb_input_char_for_index(uint16_t index)
{
    const char *keys = ui_smb_input_charset();
    size_t key_count = strlen(keys);

    return (index < key_count) ? keys[index] : '\0';
}

static const char *ui_smb_input_action_label(uint16_t index)
{
    size_t key_count = strlen(ui_smb_input_charset());
    uint16_t action = (index >= key_count) ? (uint16_t)(index - key_count) : 0U;

    switch (action) {
        case 0:
            return "Backspace";
        case 1:
            return "Clear";
        default:
            return "OK";
    }
}

static void ui_smb_open_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    ui_smb_ensure_context();
    ui_open_page_locked(model, UI_PAGE_SMB);
    ui_set_status_locked(model, "SMB server");
}

static void ui_smb_open_text_input_locked(ui_model_t *model, ui_smb_field_t field)
{
    if (model == NULL) {
        return;
    }
    ui_smb_ensure_context();
    s_smb_ctx.editing_field = field;
    ui_open_page_locked(model, UI_PAGE_SMB_TEXT_INPUT);
    ui_set_status_locked(model, ui_smb_field_label(field));
}

static void ui_smb_select_server_locked(ui_model_t *model, uint16_t server_index)
{
    if (model == NULL || server_index >= s_smb_ctx.server_count) {
        return;
    }
    snprintf(
        s_smb_ctx.config.host,
        sizeof(s_smb_ctx.config.host),
        "%s",
        s_smb_ctx.servers[server_index].host);
    s_smb_ctx.config.port = s_smb_ctx.servers[server_index].port > 0 ? s_smb_ctx.servers[server_index].port : 445;
    ui_set_status_locked(model, s_smb_ctx.config.host);
}

static void ui_smb_toggle_signing_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    ui_smb_ensure_context();
    s_smb_ctx.config.signing = !s_smb_ctx.config.signing;
    ui_set_status_locked(model, s_smb_ctx.config.signing ? "SMB signing on" : "SMB signing off");
}

static void ui_smb_adjust_port_locked(ui_model_t *model, int delta)
{
    int port;

    if (model == NULL) {
        return;
    }
    ui_smb_ensure_context();
    port = s_smb_ctx.config.port > 0 ? s_smb_ctx.config.port : 445;
    port += delta;
    if (port < 1) {
        port = 1;
    } else if (port > 65535) {
        port = 65535;
    }
    s_smb_ctx.config.port = port;
    ui_set_status_locked(model, "SMB port changed");
}

static esp_err_t ui_smb_discover_collect_cb(const smb_client_discovery_entry_t *entry, void *user_ctx)
{
    ui_smb_scan_result_t *ctx = (ui_smb_scan_result_t *)user_ctx;

    if (ctx == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->server_count >= SMB_CLIENT_DISCOVERY_MAX) {
        return ESP_OK;
    }
    ctx->servers[ctx->server_count++] = *entry;
    return ESP_OK;
}

static esp_err_t ui_smb_favorite_collect_cb(const smb_client_favorite_t *entry, void *user_ctx)
{
    ui_smb_favorite_result_t *ctx = (ui_smb_favorite_result_t *)user_ctx;

    if (ctx == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->favorite_count >= SMB_CLIENT_FAVORITE_MAX) {
        return ESP_OK;
    }
    ctx->favorites[ctx->favorite_count++] = *entry;
    return ESP_OK;
}

static void ui_smb_select_favorite_locked(ui_model_t *model, uint16_t favorite_index)
{
    if (model == NULL || favorite_index >= s_smb_ctx.favorite_count) {
        return;
    }
    s_smb_ctx.selected_favorite_id = s_smb_ctx.favorites[favorite_index].id;
    ui_set_status_locked(model, s_smb_ctx.favorites[favorite_index].label);
}

static burner_status_t *ui_task_result_status_buffer(void)
{
    return &s_task_result_status;
}

static void ui_clear_task_result_locked(ui_model_t *model)
{
    if (model != NULL) {
        model->task_result_status_valid = false;
    }
    memset(&s_task_result_status, 0, sizeof(s_task_result_status));
    s_task_result_status_valid = false;
    s_task_result_capture_armed = false;
    s_task_result_active_seen = false;
}

static void ui_clear_task_result_runtime_locked(ui_model_t *model)
{
    ui_clear_task_result_locked(model);
    burner_status_clear_task_result();
    if (model != NULL) {
        model->burn_progress = 0;
        model->burn_processed = 0;
        model->burn_total = 0;
        model->erase_progress = 0;
        model->erase_done_sectors = 0;
        model->erase_total_sectors = 0;
        model->burn_elapsed_us = 0;
    }
}

static void ui_begin_task_result_capture_locked(ui_model_t *model)
{
    ui_clear_task_result_locked(model);
    s_task_result_capture_armed = true;
}

static void ui_capture_task_result_locked(ui_model_t *model, const burner_status_t *status)
{
    burner_status_t *cache = NULL;

    if (model == NULL || status == NULL) {
        return;
    }
    if (!s_task_result_capture_armed || !s_task_result_active_seen) {
        return;
    }
    cache = ui_task_result_status_buffer();
    if (cache == NULL) {
        return;
    }
    *cache = *status;
    if (cache->processed_bytes == 0U) {
        cache->processed_bytes = model->burn_processed;
    }
    if (cache->total_bytes == 0U) {
        cache->total_bytes = model->burn_total;
    }
    if (cache->task_elapsed_us == 0ULL) {
        cache->task_elapsed_us = model->burn_elapsed_us;
    }
    s_task_result_status_valid = true;
    model->task_result_status_valid = true;
    s_task_result_capture_armed = false;
    s_task_result_active_seen = false;
}

static const ui_menu_item_t s_home_system_item = {
    .title = "System",
    .hint = "web overview",
    .symbol = "SYS",
    .action = UI_ACTION_OPEN_SYSTEM,
    .accent = UI_COLOR_WHITE,
};

static const ui_menu_item_t s_home_burner_item = {
    .title = "Burner",
    .hint = "cart operations",
    .symbol = "BURN",
    .action = UI_ACTION_OPEN_BURNER,
    .accent = UI_COLOR_WHITE,
};

static const ui_menu_item_t s_home_retro_go_item = {
    .title = "Retro-Go",
    .hint = "reboot to games",
    .symbol = "RG",
    .action = UI_ACTION_OPEN_RETRO_GO,
    .accent = UI_COLOR_WHITE,
};

static app_config_t s_app_config = {0};
static ui_menu_item_t s_home_items[UI_HOME_ICON_MAX] = {0};
static uint16_t s_home_item_count = 0;
static bool s_home_items_ready = false;

static const ui_menu_item_t s_system_items[UI_SYSTEM_ITEM_COUNT] = {
    {.title = "Apps", .hint = "home shortcuts", .symbol = "APP", .action = UI_ACTION_OPEN_APP_MANAGER, .accent = UI_COLOR_WHITE},
    {.title = "TF", .hint = "files and USB", .symbol = "TF", .action = UI_ACTION_OPEN_TF, .accent = UI_COLOR_WHITE},
    {.title = "Settings", .hint = "device tools", .symbol = "CFG", .action = UI_ACTION_OPEN_SETTINGS, .accent = UI_COLOR_WHITE},
    {.title = "Power", .hint = "battery telemetry", .symbol = "PWR", .action = UI_ACTION_OPEN_POWER, .accent = UI_COLOR_WHITE},
    {.title = "Wi-Fi", .hint = "network setup", .symbol = "WIFI", .action = UI_ACTION_OPEN_WIFI, .accent = UI_COLOR_WHITE},
};

static bool ui_take_model_lock(void);
static void ui_button_map_init_defaults(void);
static ui_input_action_t ui_button_action_for_page(ui_page_t page, ui_button_t button);
static bool ui_action_is_repeatable(ui_input_action_t action);
static uint16_t ui_action_repeat_interval_frames(ui_input_action_t action, uint16_t repeat_count);
static bool ui_action_hold_started(ui_input_action_t action);
static bool ui_apply_volume_delta_locked(ui_model_t *model, int delta);
static bool ui_handle_global_button_action_locked(ui_model_t *model, ui_input_action_t action);
static void ui_handle_page_button_action_locked(
    ui_model_t *model,
    ui_input_action_t action,
    ui_file_start_request_t **start_request,
    ui_work_type_t *work_type,
    bool *start_work);
static void ui_handle_button_action(ui_input_action_t action);
static bool ui_file_entry_for_visible_row(const ui_model_t *model, uint16_t row, ui_file_entry_t *entry_out);
static int32_t ui_file_name_col_w(void);
static uint16_t ui_file_visible_rows_for_page(const ui_model_t *model);
static uint16_t ui_scroll_for_selected_rows(uint16_t selected, uint16_t scroll, uint16_t count, uint16_t visible_rows);
static uint16_t ui_burn_rom_visible_rows(void);
static ui_burn_rom_op_t ui_burn_rom_op_for_index(uint16_t index);
static bool ui_burn_cart_has_slots(void);
static const char *ui_mbc5_voltage_label(void);
static void ui_focus_burn_rom_op_locked(ui_model_t *model, ui_burn_rom_op_t op);
static void ui_burn_rom_open_mapper_menu_locked(ui_model_t *model);
static void ui_persist_burn_settings_locked(ui_model_t *model);
static const char *ui_selected_gb_mapper_label(void);
static void ui_push_current_page_locked(const ui_model_t *model);
static bool ui_page_is_icon_grid(ui_page_t page);
static bool ui_icon_page_config_for_page(ui_page_t page, ui_icon_page_config_t *config_out);
static bool ui_icon_page_item_at(ui_page_t page, uint16_t index, const ui_menu_item_t **item_out);
static uint16_t ui_icon_page_move_selection(uint16_t selected, uint16_t count, uint8_t cols, uint8_t rows, int dx, int dy);
static bool ui_file_page_is_book_scope(const ui_model_t *model);
static bool ui_reader_text_file_supported_name(const char *name);
static bool ui_reader_native_file_supported_name(const char *name);
static const char *ui_page_title(ui_page_t page);
static void ui_format_page_header(const ui_model_t *model, char *header, size_t header_len);
static void ui_open_book_library_locked(ui_model_t *model, bool reset_selection);
static void ui_reader_clear_locked(void);
static bool ui_open_reader_locked(ui_model_t *model, const ui_file_entry_t *entry);
static void ui_reader_move_page_locked(ui_model_t *model, int delta);
static void ui_reader_save_history_locked(const ui_model_t *model);
static bool ui_reader_load_history(char *path, size_t path_len, uint32_t *page_out);
static bool ui_music_play_selected_locked(ui_model_t *model);
static void ui_open_music_player_locked(ui_model_t *model, const ui_file_entry_t *entry);
static void ui_music_set_drawer_open_locked(ui_model_t *model, bool open);
static bool ui_music_restore_history_locked(ui_model_t *model);
static bool ui_music_select_path_locked(ui_model_t *model, const char *path);
static bool ui_music_seek_relative_locked(ui_model_t *model, int32_t delta_bytes);
static void ui_music_save_history_snapshot(const music_player_snapshot_t *snap);
static bool ui_music_load_history(char *path, size_t path_len, uint32_t *position_out);
static bool ui_music_toggle_or_play_current_locked(ui_model_t *model);
static bool ui_settings_adjust_active(const ui_model_t *model);
static uint8_t ui_settings_current_volume(void);
static void ui_settings_open_adjust_locked(ui_model_t *model, ui_setting_adjust_t target);
static void ui_settings_close_adjust_locked(ui_model_t *model);
static void ui_settings_adjust_value_locked(ui_model_t *model, int direction, uint8_t step);
static void ui_px_apply_settings_adjust(const ui_model_t *model);

static bool ui_lang_is_zh(void)
{
    return s_ui_language == UI_LANGUAGE_ZH;
}

static const char *ui_tr(const char *key)
{
    return ui_text_lookup(s_ui_language, key);
}

static ui_button_page_map_t *ui_button_map_for_page(ui_page_t page)
{
    for (size_t i = 0; i < sizeof(s_button_page_maps) / sizeof(s_button_page_maps[0]); ++i) {
        if (s_button_page_maps[i].page == page) {
            return &s_button_page_maps[i];
        }
    }
    return NULL;
}

static void ui_button_map_init_defaults(void)
{
    static const ui_input_action_t base_map[UI_BUTTON_COUNT] = {
        UI_INPUT_ACTION_LEFT,
        UI_INPUT_ACTION_RIGHT,
        UI_INPUT_ACTION_UP,
        UI_INPUT_ACTION_DOWN,
        UI_INPUT_ACTION_SELECT,
        UI_INPUT_ACTION_PANEL_TOGGLE,
        UI_INPUT_ACTION_BACK,
        UI_INPUT_ACTION_MENU,
        UI_INPUT_ACTION_VOLUME_UP,
        UI_INPUT_ACTION_VOLUME_DOWN,
    };

    for (size_t i = 0; i < sizeof(s_button_page_maps) / sizeof(s_button_page_maps[0]); ++i) {
        memcpy(s_button_page_maps[i].mapping, base_map, sizeof(base_map));
    }
}

static ui_input_action_t ui_button_action_for_page(ui_page_t page, ui_button_t button)
{
    ui_button_page_map_t *map = NULL;

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_VOL_DOWN) {
        return UI_INPUT_ACTION_NONE;
    }
    if (button == UI_BUTTON_VOL_UP) {
        return UI_INPUT_ACTION_VOLUME_UP;
    }
    if (button == UI_BUTTON_VOL_DOWN) {
        return UI_INPUT_ACTION_VOLUME_DOWN;
    }
    map = ui_button_map_for_page(page);
    if (map == NULL) {
        return UI_INPUT_ACTION_NONE;
    }
    return map->mapping[(uint8_t)button];
}

static bool ui_action_is_repeatable(ui_input_action_t action)
{
    return action == UI_INPUT_ACTION_LEFT || action == UI_INPUT_ACTION_RIGHT ||
           action == UI_INPUT_ACTION_UP || action == UI_INPUT_ACTION_DOWN ||
           action == UI_INPUT_ACTION_VOLUME_UP || action == UI_INPUT_ACTION_VOLUME_DOWN;
}

static uint16_t ui_action_repeat_interval_frames(ui_input_action_t action, uint16_t repeat_count)
{
    if ((action == UI_INPUT_ACTION_LEFT || action == UI_INPUT_ACTION_RIGHT) &&
        s_model.page == UI_PAGE_MUSIC_PLAYER) {
        (void)repeat_count;
        return 2U;
    }
    return UI_BUTTON_REPEAT_INTERVAL_FRAMES;
}

static bool ui_action_hold_started(ui_input_action_t action)
{
    for (uint8_t i = 0; i < UI_BUTTON_COUNT; ++i) {
        const ui_button_state_t *state = &s_button_states[i];

        if (state->action != action) {
            continue;
        }
        if (state->hold_action_started || state->repeat_count > 0U) {
            return true;
        }
    }
    return false;
}

static void ui_set_volume_status_locked(ui_model_t *model, uint8_t volume_percent)
{
    char status[UI_STATUS_TEXT_MAX_LEN];

    snprintf(status, sizeof(status), "%s %u%%", ui_tr("Volume"), (unsigned)volume_percent);
    ui_set_status_locked(model, status);
}

static bool ui_apply_volume_delta_locked(ui_model_t *model, int delta)
{
    uint8_t current = ui_settings_current_volume();
    int next = (int)current + delta;

    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    if ((uint8_t)next == current) {
        ui_set_volume_status_locked(model, current);
        return false;
    }
    if (music_player_set_volume((uint8_t)next) != ESP_OK) {
        ui_set_status_locked(model, ui_tr("save settings failed"));
        return false;
    }
    (void)mori_save_av_settings_to_system_ini(lcd_display_get_brightness(), (uint8_t)next);
    ui_set_volume_status_locked(model, (uint8_t)next);
    ui_mark_content_dirty(model);
    ui_mark_chrome_dirty(model);
    model->dirty = true;
    return true;
}

static bool ui_handle_global_button_action_locked(ui_model_t *model, ui_input_action_t action)
{
    if (model == NULL) {
        return false;
    }
    if (action == UI_INPUT_ACTION_VOLUME_UP) {
        return ui_apply_volume_delta_locked(model, 1);
    }
    if (action == UI_INPUT_ACTION_VOLUME_DOWN) {
        return ui_apply_volume_delta_locked(model, -1);
    }
    return false;
}

static bool ui_action_for_app_id(app_id_t id, ui_action_t *action_out)
{
    if (action_out == NULL) {
        return false;
    }
    switch (id) {
        case APP_ID_READER:
            *action_out = UI_ACTION_OPEN_READER;
            return true;
        case APP_ID_MUSIC:
            *action_out = UI_ACTION_OPEN_MUSIC;
            return true;
        default:
            return false;
    }
}

static void ui_home_append_item(const ui_menu_item_t *item)
{
    if (item == NULL || s_home_item_count >= UI_HOME_ICON_MAX) {
        return;
    }
    s_home_items[s_home_item_count++] = *item;
}

static void ui_home_append_app(app_id_t app_id)
{
    const app_descriptor_t *desc = app_registry_get(app_id);
    ui_menu_item_t item = {0};

    if (desc == NULL || s_home_item_count >= UI_HOME_ICON_MAX) {
        return;
    }
    if (!ui_action_for_app_id(desc->id, &item.action)) {
        ESP_LOGW(UI_TAG, "app '%s' has no UI action", desc->key);
        return;
    }
    snprintf(item.title, sizeof(item.title), "%s", desc->title);
    snprintf(item.hint, sizeof(item.hint), "%s", desc->hint);
    item.symbol = desc->symbol;
    item.accent = UI_COLOR_WHITE;
    ui_home_append_item(&item);
}

static void ui_home_rebuild_items(void)
{
    memset(s_home_items, 0, sizeof(s_home_items));
    s_home_item_count = 0;
    ui_home_append_item(&s_home_system_item);
    for (size_t i = 0; i < s_app_config.count && i < UI_HOME_APP_MAX; ++i) {
        ui_home_append_app(s_app_config.apps[i]);
    }
    ui_home_append_item(&s_home_burner_item);
    ui_home_append_item(&s_home_retro_go_item);
    if (s_root_selected >= s_home_item_count) {
        s_root_selected = 0;
    }
    s_home_items_ready = true;
}

static void ui_home_load_config(void)
{
    esp_err_t err = app_config_load(&s_app_config);

    if (err != ESP_OK) {
        ESP_LOGW(UI_TAG, "app launcher config loaded with issues: %s", esp_err_to_name(err));
    }
    ui_home_rebuild_items();
    ESP_LOGI(UI_TAG, "home launcher items=%u apps=%u", (unsigned)s_home_item_count, (unsigned)s_app_config.count);
}

static uint16_t ui_home_item_count(void)
{
    if (!s_home_items_ready) {
        ui_home_load_config();
    }
    return s_home_item_count;
}

static const ui_menu_item_t *ui_home_item_at(uint16_t index)
{
    if (!s_home_items_ready) {
        ui_home_load_config();
    }
    if (index >= s_home_item_count) {
        return NULL;
    }
    return &s_home_items[index];
}

static uint16_t ui_app_manager_item_count(void)
{
    uint16_t count = 0;

    for (size_t i = 0; i < app_registry_count(); ++i) {
        const app_descriptor_t *desc = app_registry_at(i);

        if (desc != NULL && desc->user_visible && count < UINT16_MAX) {
            count++;
        }
    }
    return count;
}

static const app_descriptor_t *ui_app_manager_desc_at(uint16_t visible_index)
{
    uint16_t visible = 0;

    for (size_t i = 0; i < app_registry_count(); ++i) {
        const app_descriptor_t *desc = app_registry_at(i);

        if (desc == NULL || !desc->user_visible) {
            continue;
        }
        if (visible == visible_index) {
            return desc;
        }
        visible++;
    }
    return NULL;
}

static bool ui_app_config_position(app_id_t id, size_t *position_out)
{
    for (size_t i = 0; i < s_app_config.count; ++i) {
        if (s_app_config.apps[i] == id) {
            if (position_out != NULL) {
                *position_out = i;
            }
            return true;
        }
    }
    return false;
}

static bool ui_app_manager_commit_locked(ui_model_t *model, const app_config_t *before, const char *ok_status)
{
    esp_err_t err = app_config_save(&s_app_config);

    if (err != ESP_OK) {
        if (before != NULL) {
            s_app_config = *before;
        }
        ui_home_rebuild_items();
        ui_set_status_locked(model, ui_tr("save apps failed"));
        ui_mark_content_dirty(model);
        ui_mark_chrome_dirty(model);
        return false;
    }
    ui_home_rebuild_items();
    ui_set_status_locked(model, ok_status);
    ui_mark_content_dirty(model);
    ui_mark_chrome_dirty(model);
    return true;
}

static void ui_app_manager_toggle_locked(ui_model_t *model)
{
    const app_descriptor_t *desc;
    app_config_t before;
    esp_err_t err;
    char status[24] = {0};
    bool enabled;

    if (model == NULL) {
        return;
    }
    desc = ui_app_manager_desc_at(model->selected);
    if (desc == NULL) {
        ui_set_status_locked(model, ui_tr("invalid app"));
        return;
    }
    before = s_app_config;
    enabled = app_config_contains(&s_app_config, desc->id);
    err = app_config_set_enabled(&s_app_config, desc->id, !enabled);
    if (err == ESP_ERR_NO_MEM) {
        ui_set_status_locked(model, ui_tr("max 5 apps"));
        return;
    }
    if (err != ESP_OK) {
        ui_set_status_locked(model, ui_tr("invalid app"));
        return;
    }
    if (!enabled) {
        snprintf(status, sizeof(status), "shown %u/%u", (unsigned)s_app_config.count, (unsigned)APP_REGISTRY_HOME_APP_MAX);
    } else {
        snprintf(status, sizeof(status), "%s", ui_tr("hidden"));
    }
    (void)ui_app_manager_commit_locked(model, &before, status);
}

static void ui_app_manager_move_locked(ui_model_t *model, int delta)
{
    const app_descriptor_t *desc;
    app_config_t before;

    if (model == NULL) {
        return;
    }
    desc = ui_app_manager_desc_at(model->selected);
    if (desc == NULL) {
        ui_set_status_locked(model, ui_tr("invalid app"));
        return;
    }
    if (!app_config_contains(&s_app_config, desc->id)) {
        ui_set_status_locked(model, ui_tr("enable app first"));
        return;
    }
    before = s_app_config;
    if (!app_config_move(&s_app_config, desc->id, delta)) {
        ui_set_status_locked(model, ui_tr("order unchanged"));
        return;
    }
    (void)ui_app_manager_commit_locked(model, &before, ui_tr("order saved"));
}

static const char *ui_root_item_title(const ui_menu_item_t *item)
{
    if (item == NULL) {
        return "";
    }
    switch (item->action) {
        case UI_ACTION_OPEN_SYSTEM:
            return ui_tr("System");
        case UI_ACTION_OPEN_APP_MANAGER:
            return ui_tr("Apps");
        case UI_ACTION_OPEN_TF:
            return "TF";
        case UI_ACTION_OPEN_READER:
            return ui_tr("Reader");
        case UI_ACTION_OPEN_MUSIC:
            return ui_tr("Music");
        case UI_ACTION_OPEN_WIFI:
            return "Wi-Fi";
        case UI_ACTION_OPEN_POWER:
            return ui_tr("Power");
        case UI_ACTION_OPEN_BURNER:
            return ui_tr("Burner");
        case UI_ACTION_OPEN_SETTINGS:
            return ui_tr("Settings");
        case UI_ACTION_OPEN_RETRO_GO:
            return "Retro-Go";
        default:
            return item->title;
    }
}

static const char *ui_root_item_hint(const ui_menu_item_t *item)
{
    if (item == NULL) {
        return "";
    }
    switch (item->action) {
        case UI_ACTION_OPEN_SYSTEM:
            return ui_tr("web overview");
        case UI_ACTION_OPEN_APP_MANAGER:
            return ui_tr("home shortcuts");
        case UI_ACTION_OPEN_TF:
            return ui_tr("files and USB");
        case UI_ACTION_OPEN_READER:
            return ui_tr("read TF files");
        case UI_ACTION_OPEN_MUSIC:
            return ui_tr("audio player");
        case UI_ACTION_OPEN_WIFI:
            return ui_tr("network setup");
        case UI_ACTION_OPEN_POWER:
            return ui_tr("battery telemetry");
        case UI_ACTION_OPEN_BURNER:
            return ui_tr("cart operations");
        case UI_ACTION_OPEN_SETTINGS:
            return ui_tr("device tools");
        case UI_ACTION_OPEN_RETRO_GO:
            return ui_tr("reboot to games");
        default:
            return item->hint;
    }
}

static bool ui_page_is_icon_grid(ui_page_t page)
{
    return page == UI_PAGE_ROOT || page == UI_PAGE_SYSTEM;
}

static bool ui_icon_page_config_for_page(ui_page_t page, ui_icon_page_config_t *config_out)
{
    ui_icon_page_config_t config = {0};

    switch (page) {
        case UI_PAGE_ROOT:
            config.items = s_home_items;
            config.count = ui_home_item_count();
            break;
        case UI_PAGE_SYSTEM:
            config.items = s_system_items;
            config.count = UI_SYSTEM_ITEM_COUNT;
            break;
        default:
            return false;
    }
    config.cols = UI_TILE_GRID_COLS;
    config.rows = UI_TILE_GRID_ROWS;
    if (config_out != NULL) {
        *config_out = config;
    }
    return true;
}

static bool ui_icon_page_item_at(ui_page_t page, uint16_t index, const ui_menu_item_t **item_out)
{
    ui_icon_page_config_t config = {0};

    if (item_out == NULL || !ui_icon_page_config_for_page(page, &config) || index >= config.count) {
        return false;
    }
    *item_out = &config.items[index];
    return true;
}

static uint16_t ui_icon_page_move_selection(uint16_t selected, uint16_t count, uint8_t cols, uint8_t rows, int dx, int dy)
{
    uint16_t page_slots;
    uint16_t page_index;
    uint16_t page_base;
    uint16_t page_count;
    uint16_t local_index;
    uint16_t local_rows;
    uint16_t row;
    uint16_t col;
    uint16_t target_index;

    if (count == 0U || cols == 0U || rows == 0U) {
        return 0U;
    }
    page_slots = (uint16_t)(cols * rows);
    if (page_slots == 0U) {
        return 0U;
    }
    if (selected >= count) {
        selected = 0U;
    }
    page_index = (uint16_t)(selected / page_slots);
    page_base = (uint16_t)(page_index * page_slots);
    page_count = (uint16_t)((count - page_base) > page_slots ? page_slots : (count - page_base));
    local_index = (uint16_t)(selected - page_base);
    local_rows = (uint16_t)((page_count + cols - 1U) / cols);
    row = (uint16_t)(local_index / cols);
    col = (uint16_t)(local_index % cols);

    if (dx != 0) {
        uint16_t row_count = (uint16_t)((row == local_rows - 1U && (page_count % cols) != 0U) ? (page_count % cols) : cols);
        if (row_count == 0U) {
            row_count = cols;
        }
        col = (uint16_t)((col + row_count + (dx > 0 ? 1 : -1)) % row_count);
    }
    if (dy != 0) {
        int target_row = (int)row + (dy > 0 ? 1 : -1);

        if (target_row < 0) {
            target_row = (int)local_rows - 1;
        } else if (target_row >= (int)local_rows) {
            target_row = 0;
        }
        row = (uint16_t)target_row;
        if (row == local_rows - 1U && (page_count % cols) != 0U) {
            uint16_t row_count = (uint16_t)(page_count % cols);

            if (row_count != 0U && col >= row_count) {
                col = (uint16_t)(row_count - 1U);
            }
        }
    }

    target_index = (uint16_t)(page_base + row * cols + col);
    if (target_index >= page_base + page_count) {
        target_index = (uint16_t)(page_base + page_count - 1U);
    }
    return target_index;
}

static const char *ui_system_item_title(const ui_menu_item_t *item)
{
    if (item == NULL) {
        return "";
    }
    switch (item->action) {
        case UI_ACTION_OPEN_TF:
            return "TF";
        case UI_ACTION_OPEN_SETTINGS:
            return ui_tr("Settings");
        case UI_ACTION_OPEN_POWER:
            return ui_tr("Power");
        case UI_ACTION_OPEN_WIFI:
            return "Wi-Fi";
        default:
            return item->title;
    }
}

static const char *ui_system_item_hint(const ui_menu_item_t *item)
{
    if (item == NULL) {
        return "";
    }
    switch (item->action) {
        case UI_ACTION_OPEN_TF:
            return ui_tr("files and USB");
        case UI_ACTION_OPEN_SETTINGS:
            return ui_tr("device tools");
        case UI_ACTION_OPEN_POWER:
            return ui_tr("battery telemetry");
        case UI_ACTION_OPEN_WIFI:
            return ui_tr("network setup");
        default:
            return item->hint;
    }
}

uint8_t ui_get_language(void)
{
    return s_ui_language;
}

void ui_set_language(uint8_t language)
{
    uint8_t normalized = (language == UI_LANGUAGE_EN) ? UI_LANGUAGE_EN : UI_LANGUAGE_ZH;

    if (s_ui_language == normalized) {
        return;
    }
    s_ui_language = normalized;
    if (ui_take_model_lock()) {
        s_model.dirty = true;
        xSemaphoreGive(s_model_lock);
    }
}

static const uint32_t s_save_size_kib_options[] = {32U, 64U, 128U, 256U, 512U};
static const uint32_t s_power_idle_min_options[] = {0U, 1U, 2U, 3U, 5U, 10U, 15U, 30U, 60U, 120U};
static const uint32_t s_psram_mb_options[] = {
    BURN_PSRAM_WINDOW_AUTO_MB,
    1U,
    2U,
    3U,
    4U,
    5U,
    6U,
    7U,
    8U,
};
static const uint32_t s_dump_chunk_kb_options[] = {32U, 64U, 128U, 256U};
static const uint32_t s_power_settle_ms_options[] = {100U, 200U, 400U, 800U, 1000U};

static ui_anim_state_t s_anim = {
    .tile_fore_y = (float)UI_CANVAS_H,
    .tile_fore_target_y = 0.0f,
    .list_bar_x = (float)UI_CANVAS_W,
    .list_bar_target_x = (float)(UI_CANVAS_W - UI_LIST_BAR_W),
    .music_drawer_x = (float)UI_MUSIC_DRAWER_X_CLOSED,
    .music_drawer_target_x = (float)UI_MUSIC_DRAWER_X_CLOSED,
    .marquee_selected = UINT16_MAX,
    .burner_prev_selected = UINT16_MAX,
    .icon_prev_selected = UINT16_MAX,
    .page = UI_PAGE_ROOT,
    .page_changed = true,
};
static ui_nav_entry_t s_nav_stack[8];
static uint8_t s_nav_depth = 0;
static burner_cart_mode_t s_cart_mode = BURNER_CART_MODE_GBA;
static bool s_burner_info_left = true;
static burner_write_path_t s_write_path = BURNER_WRITE_PATH_DIRECT;
static burner_recipe_mode_t s_recipe_mode = BURNER_RECIPE_MODE_CHIS;
static bool s_ram_fram = false;
static burner_gba_save_type_t s_gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
static uint32_t s_cart_slot = 0;
static uint32_t s_rom_size_mib = 32;
static uint32_t s_save_size_kib = 128;
static uint32_t s_psram_mb = BURN_PSRAM_WINDOW_AUTO_MB;
static uint32_t s_mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
static uint32_t s_dump_chunk_kb = BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
static uint8_t s_ram_latency = 10;
static bool s_gba_sram_patch = false;
static uint8_t s_gba_save_patch_choice = 0U; /* 0 none, 1 SRAM, 2 FLASH */
static bool s_gba_waitcnt_patch = false;
static bool s_gba_batteryless_patch = false;
static bool s_gba_sram_patch_available = false;
static bool s_gba_patch_analysis_active = false;
static bool s_gba_patch_analysis_done = false;
static bool s_dump_batteryless_requested = false;
static ui_file_entry_t s_last_rom_file_by_cart[2] = {0};
static ui_file_kind_t s_last_rom_kind_by_cart[2] = {UI_FILE_KIND_UNSUPPORTED, UI_FILE_KIND_UNSUPPORTED};
static ui_file_entry_t s_last_save_file = {0};
static ui_burn_rom_submenu_t s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
static char s_burn_rom_custom_size_text[UI_BURN_ROM_CUSTOM_SIZE_TEXT_MAX] = "";
static uint32_t s_burn_rom_write_prompt_until_ms = 0;
static uint32_t s_burn_rom_verify_prompt_until_ms = 0;

static void ui_scan_file_window_locked(ui_model_t *model);
static void ui_set_status_locked(ui_model_t *model, const char *text);
static void ui_drop_nav_target_locked(ui_page_t page);
static void ui_mark_content_dirty(ui_model_t *model);
static void ui_mark_chrome_dirty(ui_model_t *model);
static void ui_burn_probe_task(void *param);
static void ui_patch_analysis_task(void *param);
static bool ui_burner_operation_active(void);
static void ui_focus_burn_rom_row_locked(ui_model_t *model, uint16_t row);
static const char *ui_probe_chip_name(const burner_status_t *status);
static const char *ui_probe_type_label(const burner_status_t *status, burner_cart_mode_t mode);
static void ui_px_draw_music_file_row(
    const ui_model_t *model,
    const music_player_snapshot_t *snap,
    uint16_t row,
    int32_t x,
    int32_t y,
    int32_t w);
static void ui_music_player_view_state(
    const ui_model_t *model,
    music_player_snapshot_t *snap_out,
    char *header,
    size_t header_len,
    bool *pause_icon_out,
    uint32_t *percent_out);
static void ui_px_draw_music_button_prev(int32_t x, int32_t y, int32_t size);
static void ui_px_draw_music_button_next(int32_t x, int32_t y, int32_t size);
static void ui_px_draw_music_button_play_pause(int32_t x, int32_t y, int32_t size, bool pause_icon);
static void ui_px_apply_wifi_qr(const ui_model_t *model);
static void ui_smb_ensure_context(void);
static void ui_smb_open_locked(ui_model_t *model);
static void ui_smb_open_text_input_locked(ui_model_t *model, ui_smb_field_t field);
static esp_err_t ui_smb_discover_collect_cb(const smb_client_discovery_entry_t *entry, void *user_ctx);
static esp_err_t ui_smb_favorite_collect_cb(const smb_client_favorite_t *entry, void *user_ctx);
static char ui_smb_input_char_for_index(uint16_t index);
static uint16_t ui_smb_item_count(void);
static uint16_t ui_smb_text_input_count(void);
static char *ui_smb_field_buffer(ui_smb_field_t field, size_t *len_out);
static const char *ui_smb_field_label(ui_smb_field_t field);
static void ui_smb_select_server_locked(ui_model_t *model, uint16_t server_index);
static void ui_smb_select_favorite_locked(ui_model_t *model, uint16_t favorite_index);
static void ui_smb_toggle_signing_locked(ui_model_t *model);
static void ui_smb_adjust_port_locked(ui_model_t *model, int delta);
static bool ui_music_header_marquee_active(const ui_model_t *model);
static bool ui_music_drawer_marquee_active(const ui_model_t *model);

static bool ui_gba_sram_patch_selectable(void)
{
    return s_cart_mode == BURNER_CART_MODE_GBA &&
           (!s_gba_patch_analysis_done || s_gba_sram_patch_available);
}

static bool ui_take_model_lock(void)
{
    if (s_model_lock == NULL) {
        s_model_lock = xSemaphoreCreateMutex();
        if (s_model_lock == NULL) {
            ESP_LOGE(UI_TAG, "create UI model mutex failed");
            return false;
        }
    }

    if (xSemaphoreTake(s_model_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(UI_TAG, "take UI model mutex failed");
        return false;
    }
    if (!s_model_defaults_inited) {
        ui_reset_model_defaults(&s_model);
        s_model_defaults_inited = true;
    }
    return true;
}

static void ui_set_status_locked(ui_model_t *model, const char *text)
{
    const char *next = (text != NULL) ? text : "";

    if (model == NULL) {
        return;
    }
    if (strcmp(model->status_text, next) == 0) {
        return;
    }
    snprintf(model->status_text, sizeof(model->status_text), "%s", next);
    ui_mark_chrome_dirty(model);
    ui_mark_content_dirty(model);
}

static bool ui_burner_operation_active(void)
{
    burner_status_t status = {0};

    burner_status_snapshot(&status);
    return burner_status_is_operation_active_state(status.state);
}

static void ui_mark_motion_dirty(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->motion_dirty = true;
}

static void ui_mark_content_dirty(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->content_dirty = true;
}

static void ui_mark_chrome_dirty(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->chrome_dirty = true;
}

static bool ui_ensure_button_queue(void)
{
    if (s_button_queue != NULL) {
        return true;
    }

    s_button_queue = xQueueCreate(UI_BUTTON_QUEUE_LEN, sizeof(ui_button_event_t));
    if (s_button_queue == NULL) {
        ESP_LOGE(UI_TAG, "create UI button queue failed");
        return false;
    }
    return true;
}

static void ui_px_clear(void)
{
    if (s_canvas_buf == NULL) {
        return;
    }
    for (uint32_t i = 0; i < UI_CANVAS_PIXELS; ++i) {
        s_canvas_buf[i] = UI_COLOR_BLACK;
    }
}

static void ui_px_clear_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;

    if (s_canvas_buf == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > UI_CANVAS_W) {
        x1 = UI_CANVAS_W;
    }
    if (y1 > UI_CANVAS_H) {
        y1 = UI_CANVAS_H;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (int32_t yy = y0; yy < y1; ++yy) {
        uint16_t *row = &s_canvas_buf[(uint32_t)yy * UI_CANVAS_W + (uint32_t)x0];
        for (int32_t xx = x0; xx < x1; ++xx) {
            *row++ = UI_COLOR_BLACK;
        }
    }
}

static void ui_px_invalidate_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_area_t area;
    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = x + w - 1;
    int32_t y1 = y + h - 1;

    if (s_canvas == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= UI_CANVAS_W) {
        x1 = UI_CANVAS_W - 1;
    }
    if (y1 >= UI_CANVAS_H) {
        y1 = UI_CANVAS_H - 1;
    }
    if (x0 > x1 || y0 > y1) {
        return;
    }
    area.x1 = x0;
    area.y1 = y0;
    area.x2 = x1;
    area.y2 = y1;
    lv_obj_invalidate_area(s_canvas, &area);
}

static void ui_px_invalidate_full(void)
{
    if (s_canvas != NULL) {
        lv_obj_invalidate(s_canvas);
    }
}

static bool s_px_clip_active = false;
static int32_t s_px_clip_x0 = 0;
static int32_t s_px_clip_y0 = 0;
static int32_t s_px_clip_x1 = 0;
static int32_t s_px_clip_y1 = 0;

static void ui_px_clip_set(int32_t x, int32_t y, int32_t w, int32_t h)
{
    s_px_clip_active = false;
    if (w <= 0 || h <= 0) {
        return;
    }
    s_px_clip_x0 = x;
    s_px_clip_y0 = y;
    s_px_clip_x1 = x + w;
    s_px_clip_y1 = y + h;
    s_px_clip_active = true;
}

static void ui_px_clip_clear(void)
{
    s_px_clip_active = false;
}

static void ui_px_clip_left_covered_region(int32_t covered_w, int32_t *x, int32_t *w)
{
    int32_t x0;
    int32_t x1;

    if (x == NULL || w == NULL || *w <= 0 || covered_w <= 0) {
        return;
    }
    x0 = *x;
    x1 = *x + *w;
    if (x1 <= covered_w) {
        *w = 0;
        return;
    }
    if (x0 < covered_w) {
        *x = covered_w;
        *w = x1 - covered_w;
    }
}

static void ui_px_set(int32_t x, int32_t y, bool on)
{
    if (s_canvas_buf == NULL || x < 0 || y < 0 || x >= UI_CANVAS_W || y >= UI_CANVAS_H) {
        return;
    }
    if (s_px_clip_active &&
        (x < s_px_clip_x0 || y < s_px_clip_y0 || x >= s_px_clip_x1 || y >= s_px_clip_y1)) {
        return;
    }
    s_canvas_buf[(uint32_t)y * UI_CANVAS_W + (uint32_t)x] = on ? UI_COLOR_WHITE : UI_COLOR_BLACK;
}

static void ui_px_toggle(int32_t x, int32_t y)
{
    uint16_t *px = NULL;

    if (s_canvas_buf == NULL || x < 0 || y < 0 || x >= UI_CANVAS_W || y >= UI_CANVAS_H) {
        return;
    }
    if (s_px_clip_active &&
        (x < s_px_clip_x0 || y < s_px_clip_y0 || x >= s_px_clip_x1 || y >= s_px_clip_y1)) {
        return;
    }
    px = &s_canvas_buf[(uint32_t)y * UI_CANVAS_W + (uint32_t)x];
    *px = (*px == UI_COLOR_BLACK) ? UI_COLOR_WHITE : UI_COLOR_BLACK;
}

static void ui_px_hline(int32_t x, int32_t y, int32_t w, bool on)
{
    for (int32_t i = 0; i < w; ++i) {
        ui_px_set(x + i, y, on);
    }
}

static void ui_px_vline(int32_t x, int32_t y, int32_t h, bool on)
{
    for (int32_t i = 0; i < h; ++i) {
        ui_px_set(x, y + i, on);
    }
}

static void ui_px_box(int32_t x, int32_t y, int32_t w, int32_t h, bool on)
{
    for (int32_t yy = 0; yy < h; ++yy) {
        ui_px_hline(x, y + yy, w, on);
    }
}

static void ui_px_frame(int32_t x, int32_t y, int32_t w, int32_t h, bool on)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    ui_px_hline(x, y, w, on);
    ui_px_hline(x, y + h - 1, w, on);
    ui_px_vline(x, y, h, on);
    ui_px_vline(x + w - 1, y, h, on);
}

static void ui_px_invert_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    for (int32_t yy = 0; yy < h; ++yy) {
        for (int32_t xx = 0; xx < w; ++xx) {
            ui_px_toggle(x + xx, y + yy);
        }
    }
}

static void ui_px_corner_box_draw(int32_t x, int32_t y, int32_t w, int32_t h, bool on)
{
    const int32_t len = UI_TILE_SELECT_LINE;

    ui_px_hline(x, y, len + 1, on);
    ui_px_vline(x, y, len + 1, on);
    ui_px_hline(x, y + h - 1, len + 1, on);
    ui_px_vline(x, y + h - len - 1, len + 1, on);
    ui_px_hline(x + w - len - 1, y, len + 1, on);
    ui_px_vline(x + w - 1, y, len + 1, on);
    ui_px_hline(x + w - len - 1, y + h - 1, len + 1, on);
    ui_px_vline(x + w - 1, y + h - len - 1, len + 1, on);
}

static void ui_px_corner_box(int32_t x, int32_t y, int32_t w, int32_t h)
{
    ui_px_corner_box_draw(x, y, w, h, true);
}

static const uint8_t *ui_font5x7(char ch)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t table[][5] = {
        ['0'] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
        ['1'] = {0x00, 0x42, 0x7F, 0x40, 0x00},
        ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
        ['3'] = {0x21, 0x41, 0x45, 0x4B, 0x31},
        ['4'] = {0x18, 0x14, 0x12, 0x7F, 0x10},
        ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
        ['6'] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
        ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
        ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
        ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1E},
        ['A'] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
        ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
        ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
        ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
        ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
        ['F'] = {0x7F, 0x09, 0x09, 0x09, 0x01},
        ['G'] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
        ['H'] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
        ['I'] = {0x00, 0x41, 0x7F, 0x41, 0x00},
        ['J'] = {0x20, 0x40, 0x41, 0x3F, 0x01},
        ['K'] = {0x7F, 0x08, 0x14, 0x22, 0x41},
        ['L'] = {0x7F, 0x40, 0x40, 0x40, 0x40},
        ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
        ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
        ['O'] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
        ['P'] = {0x7F, 0x09, 0x09, 0x09, 0x06},
        ['Q'] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
        ['R'] = {0x7F, 0x09, 0x19, 0x29, 0x46},
        ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
        ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
        ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
        ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
        ['W'] = {0x3F, 0x40, 0x38, 0x40, 0x3F},
        ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
        ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
        ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
        ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
        ['_'] = {0x40, 0x40, 0x40, 0x40, 0x40},
        ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
        ['/'] = {0x20, 0x10, 0x08, 0x04, 0x02},
        ['%'] = {0x23, 0x13, 0x08, 0x64, 0x62},
        ['+'] = {0x08, 0x08, 0x3E, 0x08, 0x08},
        ['*'] = {0x14, 0x08, 0x3E, 0x08, 0x14},
        [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
        ['<'] = {0x08, 0x14, 0x22, 0x41, 0x00},
        ['>'] = {0x41, 0x22, 0x14, 0x08, 0x00},
        ['?'] = {0x02, 0x01, 0x51, 0x09, 0x06},
        ['|'] = {0x00, 0x00, 0x7F, 0x00, 0x00},
        [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    };

    unsigned char c = (unsigned char)ch;

    if (c >= 'a' && c <= 'z') {
        c = (unsigned char)toupper(c);
    }
    if (c >= sizeof(table) / sizeof(table[0])) {
        return blank;
    }
    return table[c];
}

static uint8_t *s_hzk16_data = NULL;
static bool s_hzk16_load_attempted = false;

static bool ui_utf8_next_codepoint(const char **cursor, uint32_t *codepoint)
{
    const uint8_t *p;

    if (cursor == NULL || *cursor == NULL || codepoint == NULL || **cursor == '\0') {
        return false;
    }

    p = (const uint8_t *)*cursor;
    if (p[0] < 0x80U) {
        *codepoint = p[0];
        *cursor += 1;
        return true;
    }

    if (p[0] >= 0xC2U && p[0] <= 0xDFU && (p[1] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(p[0] & 0x1FU) << 6) | (uint32_t)(p[1] & 0x3FU);
        *cursor += 2;
        return true;
    }

    if (p[0] >= 0xE0U && p[0] <= 0xEFU &&
        (p[1] & 0xC0U) == 0x80U &&
        (p[2] & 0xC0U) == 0x80U &&
        !(p[0] == 0xE0U && p[1] < 0xA0U) &&
        !(p[0] == 0xEDU && p[1] >= 0xA0U)) {
        *codepoint = ((uint32_t)(p[0] & 0x0FU) << 12) |
                     ((uint32_t)(p[1] & 0x3FU) << 6) |
                     (uint32_t)(p[2] & 0x3FU);
        *cursor += 3;
        return true;
    }

    if (p[0] >= 0xF0U && p[0] <= 0xF4U &&
        (p[1] & 0xC0U) == 0x80U &&
        (p[2] & 0xC0U) == 0x80U &&
        (p[3] & 0xC0U) == 0x80U &&
        !(p[0] == 0xF0U && p[1] < 0x90U) &&
        !(p[0] == 0xF4U && p[1] >= 0x90U)) {
        *codepoint = ((uint32_t)(p[0] & 0x07U) << 18) |
                     ((uint32_t)(p[1] & 0x3FU) << 12) |
                     ((uint32_t)(p[2] & 0x3FU) << 6) |
                     (uint32_t)(p[3] & 0x3FU);
        *cursor += 4;
        return true;
    }

    *codepoint = '?';
    *cursor += 1;
    return true;
}

static bool ui_hzk16_offset_for_codepoint(uint32_t codepoint, size_t *offset_out)
{
    WCHAR oem;
    uint8_t qh;
    uint8_t wh;
    size_t index;

    if (codepoint < 0x80U || codepoint > 0xFFFFU) {
        return false;
    }

    oem = ff_uni2oem((DWORD)codepoint, FF_CODE_PAGE);
    if (oem <= 0xFFU) {
        return false;
    }

    qh = (uint8_t)((oem >> 8) & 0xFFU);
    wh = (uint8_t)(oem & 0xFFU);
    if (qh < UI_HZK16_QH_MIN || qh > UI_HZK16_QH_MAX ||
        wh < UI_HZK16_WH_MIN || wh > UI_HZK16_WH_MAX) {
        return false;
    }

    index = ((size_t)(qh - UI_HZK16_QH_MIN) * UI_HZK16_COLS) + (size_t)(wh - UI_HZK16_WH_MIN);
    if (index >= UI_HZK16_GLYPH_COUNT) {
        return false;
    }

    if (offset_out != NULL) {
        *offset_out = index * UI_HZK16_BYTES_PER_GLYPH;
    }
    return true;
}

static const uint8_t *ui_hzk16_data(void)
{
    FILE *file;
    uint8_t *data;
    size_t read_len;

    if (s_hzk16_data != NULL) {
        return s_hzk16_data;
    }
    if (s_hzk16_load_attempted) {
        return NULL;
    }
    s_hzk16_load_attempted = true;

    file = fopen(UI_HZK16_PATH, "rb");
    if (file == NULL) {
        ESP_LOGW(UI_TAG, "Chinese font missing: %s", UI_HZK16_PATH);
        return NULL;
    }

    data = (uint8_t *)heap_caps_malloc(UI_HZK16_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = (uint8_t *)heap_caps_malloc(UI_HZK16_FILE_SIZE, MALLOC_CAP_8BIT);
    }
    if (data == NULL) {
        ESP_LOGW(UI_TAG, "no memory for Chinese font (%u bytes)", (unsigned)UI_HZK16_FILE_SIZE);
        fclose(file);
        return NULL;
    }

    read_len = fread(data, 1, UI_HZK16_FILE_SIZE, file);
    fclose(file);
    if (read_len != UI_HZK16_FILE_SIZE) {
        ESP_LOGW(UI_TAG, "Chinese font size mismatch: %u/%u", (unsigned)read_len, (unsigned)UI_HZK16_FILE_SIZE);
        heap_caps_free(data);
        return NULL;
    }

    s_hzk16_data = data;
    ESP_LOGI(UI_TAG, "Chinese font loaded: %s (%u bytes)", UI_HZK16_PATH, (unsigned)UI_HZK16_FILE_SIZE);
    return s_hzk16_data;
}

static const uint8_t *ui_hzk16_glyph(uint32_t codepoint)
{
    size_t offset;
    const uint8_t *data;

    if (!ui_hzk16_offset_for_codepoint(codepoint, &offset)) {
        return NULL;
    }

    data = ui_hzk16_data();
    if (data == NULL) {
        return NULL;
    }
    return &data[offset];
}

static int32_t ui_px_codepoint_advance(uint32_t codepoint)
{
    if (ui_hzk16_offset_for_codepoint(codepoint, NULL)) {
        return UI_CJK_ADVANCE;
    }
    return ui_lang_is_zh() ? UI_ASCII_ZH_ADVANCE : UI_ASCII_ADVANCE;
}

static int32_t ui_px_codepoint_draw_width(uint32_t codepoint)
{
    if (ui_hzk16_offset_for_codepoint(codepoint, NULL)) {
        return UI_HZK16_W;
    }
    return ui_lang_is_zh() ? UI_ASCII_ZH_W : UI_ASCII_W;
}

static int32_t ui_ascii_zh_x(int32_t x, int32_t col)
{
    return x + (col * (UI_ASCII_ZH_W - 1) + (UI_ASCII_W - 2) / 2) / (UI_ASCII_W - 1);
}

static int32_t ui_ascii_zh_y(int32_t y, int32_t row)
{
    return y + (row * (UI_ASCII_ZH_H - 1) + (UI_ASCII_H - 2) / 2) / (UI_ASCII_H - 1);
}

static void ui_px_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool on)
{
    int32_t dx = (x0 < x1) ? (x1 - x0) : (x0 - x1);
    int32_t dy = (y0 < y1) ? (y1 - y0) : (y0 - y1);
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;

    while (true) {
        int32_t e2;

        ui_px_set(x0, y0, on);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int32_t ui_px_text_width(const char *text)
{
    int32_t w = 0;
    const char *p = text;

    if (text == NULL) {
        return 0;
    }
    while (*p != '\0') {
        uint32_t codepoint = 0;
        if (!ui_utf8_next_codepoint(&p, &codepoint)) {
            break;
        }
        w += ui_px_codepoint_advance(codepoint);
    }
    return (w > 0) ? (w - 1) : 0;
}

static void ui_px_draw_char(int32_t x, int32_t y, char ch, bool on)
{
    const uint8_t *glyph = ui_font5x7(ch);

    if ((unsigned char)ch >= 0x80U) {
        glyph = ui_font5x7('?');
    }
    if (ui_lang_is_zh() && ch == ':') {
        y += UI_ASCII_ZH_Y_OFFSET;
        ui_px_box(x + 3, y + 3, 2, 2, on);
        ui_px_box(x + 3, y + 7, 2, 2, on);
        return;
    }
    if (ui_lang_is_zh()) {
        y += UI_ASCII_ZH_Y_OFFSET;
        for (int32_t col = 0; col < UI_ASCII_W; ++col) {
            uint8_t bits = glyph[col];
            for (int32_t row = 0; row < UI_ASCII_H; ++row) {
                int32_t px;
                int32_t py;

                if ((bits & (1U << row)) == 0U) {
                    continue;
                }
                px = ui_ascii_zh_x(x, col);
                py = ui_ascii_zh_y(y, row);
                ui_px_set(px, py, on);
                if (col > 0 && (glyph[col - 1] & (1U << row)) != 0U) {
                    int32_t prev_x = ui_ascii_zh_x(x, col - 1);
                    ui_px_hline(prev_x, py, px - prev_x + 1, on);
                }
                if (row > 0 && (bits & (1U << (row - 1))) != 0U) {
                    int32_t prev_y = ui_ascii_zh_y(y, row - 1);
                    ui_px_vline(px, prev_y, py - prev_y + 1, on);
                }
                if (col > 0 && row > 0 && (glyph[col - 1] & (1U << (row - 1))) != 0U) {
                    ui_px_line(ui_ascii_zh_x(x, col - 1), ui_ascii_zh_y(y, row - 1), px, py, on);
                }
                if (col > 0 && row + 1 < UI_ASCII_H && (glyph[col - 1] & (1U << (row + 1))) != 0U) {
                    ui_px_line(ui_ascii_zh_x(x, col - 1), ui_ascii_zh_y(y, row + 1), px, py, on);
                }
            }
        }
        return;
    }

    for (int32_t col = 0; col < UI_ASCII_W; ++col) {
        uint8_t bits = glyph[col];
        for (int32_t row = 0; row < UI_ASCII_H; ++row) {
            if ((bits & (1U << row)) != 0U) {
                ui_px_set(x + col, y + row, on);
            }
        }
    }
}

static void ui_px_draw_hzk16(int32_t x, int32_t y, const uint8_t *glyph, bool on)
{
    if (glyph == NULL) {
        return;
    }
    for (int32_t row = 0; row < UI_HZK16_H; ++row) {
        uint8_t left = glyph[row * 2];
        uint8_t right = glyph[row * 2 + 1];
        for (int32_t bit = 0; bit < 8; ++bit) {
            if ((left & (uint8_t)(0x80U >> bit)) != 0U) {
                ui_px_set(x + bit, y + row, on);
            }
            if ((right & (uint8_t)(0x80U >> bit)) != 0U) {
                ui_px_set(x + 8 + bit, y + row, on);
            }
        }
    }
}

static void ui_px_draw_codepoint(int32_t x, int32_t y, uint32_t codepoint, bool on)
{
    const uint8_t *glyph;

    if (codepoint < 0x80U) {
        ui_px_draw_char(x, y, (char)codepoint, on);
        return;
    }

    glyph = ui_hzk16_glyph(codepoint);
    if (glyph != NULL) {
        ui_px_draw_hzk16(x, y + UI_CJK_Y_OFFSET, glyph, on);
        return;
    }

    ui_px_draw_char(x, y, '?', on);
}

static void ui_px_text(int32_t x, int32_t y, const char *text, bool on)
{
    const char *p = text;

    if (text == NULL) {
        return;
    }
    while (*p != '\0') {
        uint32_t codepoint = 0;
        int32_t advance;
        if (!ui_utf8_next_codepoint(&p, &codepoint)) {
            break;
        }
        advance = ui_px_codepoint_advance(codepoint);
        ui_px_draw_codepoint(x, y, codepoint, on);
        x += advance;
    }
}

static void ui_px_text_clipped(int32_t x, int32_t y, int32_t max_w, const char *text, bool on)
{
    const char *p = text;
    int32_t remaining = max_w;

    if (text == NULL || max_w <= 0) {
        return;
    }
    while (*p != '\0') {
        uint32_t codepoint = 0;
        int32_t advance;
        int32_t draw_w;

        if (!ui_utf8_next_codepoint(&p, &codepoint)) {
            break;
        }
        advance = ui_px_codepoint_advance(codepoint);
        draw_w = ui_px_codepoint_draw_width(codepoint);
        if (remaining < draw_w) {
            break;
        }
        ui_px_draw_codepoint(x, y, codepoint, on);
        x += advance;
        remaining -= advance;
    }
}

static void ui_px_text_clipped_disabled(int32_t x, int32_t y, int32_t max_w, const char *text)
{
    int32_t yy;
    int32_t xx;

    ui_px_text_clipped(x, y, max_w, text, true);
    for (yy = 0; yy < UI_TEXT_GLYPH_H; ++yy) {
        for (xx = 0; xx < max_w; ++xx) {
            if (((xx + yy) & 1) == 0) {
                ui_px_set(x + xx, y + yy, false);
            }
        }
    }
}

static void ui_px_text_clipped_offset(int32_t x, int32_t y, int32_t max_w, const char *text, int32_t offset_x, bool on)
{
    const char *p = text;
    int32_t cursor_x = x - offset_x;
    int32_t clip_right = x + max_w;

    if (text == NULL || max_w <= 0) {
        return;
    }
    while (*p != '\0') {
        uint32_t codepoint = 0;
        int32_t advance;
        int32_t draw_w;

        if (!ui_utf8_next_codepoint(&p, &codepoint)) {
            break;
        }
        advance = ui_px_codepoint_advance(codepoint);
        draw_w = ui_px_codepoint_draw_width(codepoint);
        if (cursor_x >= clip_right) {
            break;
        }
        if (cursor_x + draw_w > x && cursor_x >= x) {
            ui_px_draw_codepoint(cursor_x, y, codepoint, on);
        }
        cursor_x += advance;
    }
}

static void ui_px_draw_charge_bolt(int32_t x, int32_t y)
{
    ui_px_set(x + 1, y + 0, true);
    ui_px_set(x + 2, y + 0, true);
    ui_px_set(x + 1, y + 1, true);
    ui_px_set(x + 0, y + 2, true);
    ui_px_set(x + 1, y + 2, true);
    ui_px_set(x + 2, y + 3, true);
    ui_px_set(x + 1, y + 4, true);
    ui_px_set(x + 2, y + 4, true);
}

static void ui_px_draw_battery_overlay(const ui_model_t *model, int32_t y)
{
    const int32_t body_w = 12;
    const int32_t body_h = 7;
    const int32_t tip_w = 2;
    const int32_t tip_h = 3;
    const int32_t inner_w = body_w - 2;
    const int32_t text_gap = 4;
    const int32_t charge_gap = 6;
    char percent_text[8] = "--%";
    bool valid = model != NULL && model->battery_valid;
    bool charging = model != NULL && model->battery_charging;
    uint8_t percent = valid ? model->battery_percent : 0U;
    int32_t icon_x = UI_CANVAS_W - (body_w + tip_w) - 4;
    int32_t icon_y = y + 1;
    int32_t text_w;
    int32_t text_x;

    if (valid) {
        snprintf(percent_text, sizeof(percent_text), "%u%%", (unsigned)percent);
    }

    text_w = ui_px_text_width(percent_text);
    text_x = icon_x - text_gap - text_w - (charging ? charge_gap : 0);
    if (text_x < 0) {
        text_x = 0;
    }

    ui_px_text(text_x, y, percent_text, true);
    if (charging) {
        ui_px_draw_charge_bolt(icon_x - 5, y + 1);
    }

    ui_px_frame(icon_x, icon_y, body_w, body_h, true);
    ui_px_box(icon_x + body_w, icon_y + 2, tip_w, tip_h, true);

    if (!valid) {
        ui_px_hline(icon_x + 3, icon_y + 3, body_w - 6, true);
        return;
    }
    if (percent > 0U) {
        int32_t fill_w = (int32_t)(((uint32_t)inner_w * (uint32_t)percent + 99U) / 100U);

        if (fill_w < 1) {
            fill_w = 1;
        } else if (fill_w > inner_w) {
            fill_w = inner_w;
        }
        ui_px_box(icon_x + 1, icon_y + 1, fill_w, body_h - 2, true);
    }
}

static void ui_format_file_size(uint32_t size, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (size >= 1024U * 1024U) {
        snprintf(out, out_len, "%.1f MB", (double)size / (1024.0 * 1024.0));
    } else if (size >= 1024U) {
        snprintf(out, out_len, "%.1f KB", (double)size / 1024.0);
    } else {
        snprintf(out, out_len, "%u B", (unsigned)size);
    }
}

static void ui_format_speed_text(uint32_t bps, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (bps >= 1024U * 1024U) {
        snprintf(out, out_len, "%.2f MB/s", (double)bps / (1024.0 * 1024.0));
    } else if (bps >= 1024U) {
        snprintf(out, out_len, "%.1f KB/s", (double)bps / 1024.0);
    } else if (bps > 0U) {
        snprintf(out, out_len, "%u B/s", (unsigned)bps);
    } else {
        snprintf(out, out_len, "--");
    }
}

static const char *ui_psram_window_label(uint32_t mb, char *out, size_t out_len)
{
    if (mb == BURN_PSRAM_WINDOW_AUTO_MB) {
        return "Auto";
    }
    if (out == NULL || out_len == 0U) {
        return "";
    }
    snprintf(out, out_len, "%" PRIu32 " MB", mb);
    return out;
}

#include "burner/ui/ui_burner_settings.inc"

static void ui_format_idle_minutes(uint16_t minutes, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (minutes == 0U) {
        snprintf(out, out_len, "%s", ui_tr("Off"));
    } else {
        snprintf(out, out_len, "%u %s", (unsigned)minutes, ui_tr("min"));
    }
}

static void ui_power_save_idle_settings_status_locked(ui_model_t *model)
{
    if (mori_save_power_idle_settings_to_system_ini(
            mori_screen_idle_off_minutes(),
            mori_wifi_idle_off_minutes()) == ESP_OK) {
        ui_set_status_locked(model, ui_tr("power settings saved"));
    } else {
        ui_set_status_locked(model, ui_tr("save power settings failed"));
    }
}

static void ui_power_adjust_idle_minutes_locked(ui_model_t *model, int delta)
{
    uint32_t next = 0U;

    if (model == NULL || model->selected > 1U) {
        return;
    }

    if (delta == 0) {
        delta = 1;
    }

    if (model->selected == 0U) {
        next = ui_next_option_u32(
            s_power_idle_min_options,
            sizeof(s_power_idle_min_options) / sizeof(s_power_idle_min_options[0]),
            mori_screen_idle_off_minutes(),
            delta);
        mori_set_screen_idle_off_minutes((uint16_t)next);
    } else {
        next = ui_next_option_u32(
            s_power_idle_min_options,
            sizeof(s_power_idle_min_options) / sizeof(s_power_idle_min_options[0]),
            mori_wifi_idle_off_minutes(),
            delta);
        mori_set_wifi_idle_off_minutes((uint16_t)next);
    }

    ui_power_save_idle_settings_status_locked(model);
    ui_mark_content_dirty(model);
}

static bool ui_settings_adjust_active(const ui_model_t *model)
{
    return model != NULL &&
           model->page == UI_PAGE_SETTINGS &&
           model->settings_adjust != UI_SETTING_ADJUST_NONE;
}

static uint8_t ui_settings_current_volume(void)
{
    music_player_snapshot_t snap = {0};

    music_player_get_snapshot(&snap);
    return snap.volume_percent;
}

static void ui_settings_save_av_status_locked(ui_model_t *model)
{
    if (mori_save_av_settings_to_system_ini(lcd_display_get_brightness(), ui_settings_current_volume()) == ESP_OK) {
        ui_set_status_locked(model, ui_tr("settings saved"));
    } else {
        ui_set_status_locked(model, ui_tr("save settings failed"));
    }
}

static void ui_settings_open_adjust_locked(ui_model_t *model, ui_setting_adjust_t target)
{
    if (model == NULL || target == UI_SETTING_ADJUST_NONE) {
        return;
    }
    model->settings_adjust = target;
    ui_set_status_locked(model, ui_tr("L/R adjust  B back"));
    ui_mark_chrome_dirty(model);
    ui_mark_content_dirty(model);
    model->dirty = true;
}

static void ui_settings_close_adjust_locked(ui_model_t *model)
{
    if (model == NULL || model->settings_adjust == UI_SETTING_ADJUST_NONE) {
        return;
    }
    model->settings_adjust = UI_SETTING_ADJUST_NONE;
    ui_set_status_locked(model, ui_tr("settings saved"));
    ui_mark_chrome_dirty(model);
    ui_mark_content_dirty(model);
    model->dirty = true;
}

static void ui_settings_adjust_value_locked(ui_model_t *model, int direction, uint8_t step)
{
    int value;
    int max_value;

    if (!ui_settings_adjust_active(model) || direction == 0 || step == 0U) {
        return;
    }

    if (model->settings_adjust == UI_SETTING_ADJUST_BRIGHTNESS) {
        value = (int)lcd_display_get_brightness();
        max_value = 255;
    } else {
        value = (int)ui_settings_current_volume();
        max_value = 100;
    }

    value += (direction > 0) ? (int)step : -(int)step;
    if (value < 0) {
        value = 0;
    } else if (value > max_value) {
        value = max_value;
    }

    if (model->settings_adjust == UI_SETTING_ADJUST_BRIGHTNESS) {
        if ((uint8_t)value == lcd_display_get_brightness()) {
            return;
        }
        if (lcd_display_set_brightness((uint8_t)value) != ESP_OK) {
            ui_set_status_locked(model, ui_tr("save settings failed"));
            return;
        }
    } else {
        if ((uint8_t)value == ui_settings_current_volume()) {
            return;
        }
        if (music_player_set_volume((uint8_t)value) != ESP_OK) {
            ui_set_status_locked(model, ui_tr("save settings failed"));
            return;
        }
    }

    ui_settings_save_av_status_locked(model);
    ui_mark_content_dirty(model);
}

static void ui_anim_move(float *value, float target, float speed)
{
    float diff;
    float divisor;

    if (value == NULL || *value == target) {
        return;
    }
    diff = target - *value;
    if (diff > -1.0f && diff < 1.0f) {
        *value = target;
        return;
    }
    divisor = (100.0f - speed);
    if (divisor < 1.0f) {
        divisor = 1.0f;
    }
    *value += diff / divisor;
}

static bool ui_chip_erase_busy_active(const ui_model_t *model, const burner_status_t *status)
{
    if (model == NULL || status == NULL) {
        return false;
    }
    if (model->page != UI_PAGE_TASK_STATUS || s_task_cancel_confirm) {
        return false;
    }
    if (status->state != BURNER_STATE_BURNING) {
        return false;
    }
    return status->chip_erase_ui_active;
}

static bool ui_anim_value_active(float value, float target)
{
    float diff = target - value;

    return diff > 0.5f || diff < -0.5f;
}

static bool ui_anim_active_for_page(const ui_model_t *model)
{
    ui_file_entry_t entry = {0};
    int32_t name_w;
    uint16_t visible_rows;

    if (model == NULL) {
        return false;
    }
    if (ui_page_is_icon_grid(model->page)) {
        return ui_anim_value_active(s_anim.tile_bar_w, s_anim.tile_bar_target_w);
    }
    if (model->page == UI_PAGE_BURNER) {
        return ui_anim_value_active(s_anim.tile_bar_w, s_anim.tile_bar_target_w) ||
               ui_anim_value_active(s_anim.tile_fore_y, s_anim.tile_fore_target_y);
    }
    if (model->page == UI_PAGE_MUSIC_PLAYER) {
        return ui_anim_value_active(s_anim.music_drawer_x, s_anim.music_drawer_target_x) ||
               ui_music_header_marquee_active(model) ||
               ui_music_drawer_marquee_active(model);
    }
    if (model->page == UI_PAGE_READER || model->page == UI_PAGE_WIFI_QR || model->page == UI_PAGE_SMB_TEXT_INPUT) {
        return false;
    }
    visible_rows = ui_file_visible_rows_for_page(model);
    if ((model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES) &&
        model->file_selected >= model->file_scroll &&
        model->file_selected < model->file_scroll + visible_rows &&
        ui_file_entry_for_visible_row(model, (uint16_t)(model->file_selected - model->file_scroll), &entry)) {
        name_w = ui_px_text_width(entry.name);
        if (name_w > ui_file_name_col_w()) {
            return true;
        }
    }
    return ui_anim_value_active(s_anim.list_selector_y, s_anim.list_selector_target_y) ||
           ui_anim_value_active(s_anim.list_selector_w, s_anim.list_selector_target_w) ||
           ui_anim_value_active(s_anim.list_bar_h, s_anim.list_bar_target_h) ||
           ui_anim_value_active(s_anim.list_bar_x, s_anim.list_bar_target_x);
}

static uint16_t ui_current_selected(const ui_model_t *model)
{
    if (model == NULL) {
        return 0;
    }
    return (model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES || model->page == UI_PAGE_MUSIC_PLAYER) ?
               model->file_selected :
               model->selected;
}

static uint16_t ui_current_scroll(const ui_model_t *model)
{
    if (model == NULL) {
        return 0;
    }
    return (model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES || model->page == UI_PAGE_MUSIC_PLAYER) ?
               model->file_scroll :
               model->scroll;
}

#include "burner/ui/ui_burner_selection.inc"

static uint16_t ui_scroll_for_selected(uint16_t selected, uint16_t scroll, uint16_t count)
{
    return ui_scroll_for_selected_rows(selected, scroll, count, UI_ROW_COUNT);
}

static uint16_t ui_file_visible_rows_for_page(const ui_model_t *model)
{
    if (model != NULL && model->page == UI_PAGE_MUSIC_PLAYER) {
        return (UI_MUSIC_DRAWER_VISIBLE_ROWS > 0) ? (uint16_t)UI_MUSIC_DRAWER_VISIBLE_ROWS : 1U;
    }
    return UI_ROW_COUNT;
}

static uint16_t ui_file_scroll_for_selected(
    const ui_model_t *model,
    uint16_t selected,
    uint16_t scroll,
    uint16_t count)
{
    return ui_scroll_for_selected_rows(selected, scroll, count, ui_file_visible_rows_for_page(model));
}

static uint16_t ui_scroll_for_selected_rows(uint16_t selected, uint16_t scroll, uint16_t count, uint16_t visible_rows)
{
    if (count == 0U) {
        return 0;
    }

    if (visible_rows == 0U) {
        visible_rows = UI_ROW_COUNT;
    }
    if (count <= visible_rows) {
        return 0;
    }

    if (selected < scroll) {
        return (uint16_t)((selected / visible_rows) * visible_rows);
    }

    if (selected >= scroll + visible_rows) {
        return (uint16_t)((selected / visible_rows) * visible_rows);
    }
    return scroll;
}

static size_t ui_utf8_sequence_len(const uint8_t *src, size_t remaining)
{
    uint8_t c;

    if (src == NULL || remaining == 0U) {
        return 0U;
    }
    c = src[0];
    if (c < 0x80U) {
        return 1U;
    }
    if (c >= 0xC2U && c <= 0xDFU) {
        return (remaining >= 2U && (src[1] & 0xC0U) == 0x80U) ? 2U : 0U;
    }
    if (c >= 0xE0U && c <= 0xEFU) {
        if (remaining < 3U || (src[1] & 0xC0U) != 0x80U || (src[2] & 0xC0U) != 0x80U) {
            return 0U;
        }
        if (c == 0xE0U && src[1] < 0xA0U) {
            return 0U;
        }
        if (c == 0xEDU && src[1] >= 0xA0U) {
            return 0U;
        }
        return 3U;
    }
    if (c >= 0xF0U && c <= 0xF4U) {
        if (remaining < 4U || (src[1] & 0xC0U) != 0x80U || (src[2] & 0xC0U) != 0x80U ||
            (src[3] & 0xC0U) != 0x80U) {
            return 0U;
        }
        if (c == 0xF0U && src[1] < 0x90U) {
            return 0U;
        }
        if (c == 0xF4U && src[1] >= 0x90U) {
            return 0U;
        }
        return 4U;
    }
    return 0U;
}

static void ui_utf8_safe_copy(char *dst, size_t dst_len, const char *src)
{
    size_t src_len;
    size_t src_offset = 0U;
    size_t dst_offset = 0U;

    if (dst == NULL || dst_len == 0U) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    src_len = strlen(src);
    while (src_offset < src_len && dst_offset + 1U < dst_len) {
        size_t seq_len = ui_utf8_sequence_len((const uint8_t *)&src[src_offset], src_len - src_offset);
        if (seq_len == 0U) {
            dst[dst_offset++] = '?';
            src_offset++;
            continue;
        }
        if (dst_offset + seq_len >= dst_len) {
            break;
        }
        memcpy(&dst[dst_offset], &src[src_offset], seq_len);
        dst_offset += seq_len;
        src_offset += seq_len;
    }
    dst[dst_offset] = '\0';
}

static bool ui_utf8_is_valid(const char *src)
{
    size_t len;
    size_t offset = 0U;

    if (src == NULL) {
        return false;
    }
    len = strlen(src);
    while (offset < len) {
        size_t seq_len = ui_utf8_sequence_len((const uint8_t *)&src[offset], len - offset);
        if (seq_len == 0U) {
            return false;
        }
        offset += seq_len;
    }
    return true;
}

static size_t ui_utf8_encode(uint32_t codepoint, char *dst, size_t dst_len)
{
    if (dst == NULL) {
        return 0U;
    }
    if (codepoint < 0x80U) {
        if (dst_len < 1U) {
            return 0U;
        }
        dst[0] = (char)codepoint;
        return 1U;
    }
    if (codepoint < 0x800U) {
        if (dst_len < 2U) {
            return 0U;
        }
        dst[0] = (char)(0xC0U | (codepoint >> 6));
        dst[1] = (char)(0x80U | (codepoint & 0x3FU));
        return 2U;
    }
    if (codepoint < 0x10000U) {
        if (dst_len < 3U) {
            return 0U;
        }
        dst[0] = (char)(0xE0U | (codepoint >> 12));
        dst[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        dst[2] = (char)(0x80U | (codepoint & 0x3FU));
        return 3U;
    }
    if (codepoint <= 0x10FFFFU) {
        if (dst_len < 4U) {
            return 0U;
        }
        dst[0] = (char)(0xF0U | (codepoint >> 18));
        dst[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        dst[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        dst[3] = (char)(0x80U | (codepoint & 0x3FU));
        return 4U;
    }
    return 0U;
}

static bool ui_cp936_to_utf8_copy(char *dst, size_t dst_len, const char *src)
{
    const uint8_t *bytes = (const uint8_t *)src;
    size_t src_offset = 0U;
    size_t dst_offset = 0U;
    bool converted_any = false;

    if (dst == NULL || dst_len == 0U) {
        return false;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return false;
    }

    while (bytes[src_offset] != 0U && dst_offset + 1U < dst_len) {
        uint32_t codepoint;
        uint8_t c = bytes[src_offset];

        if (c < 0x80U) {
            codepoint = c;
            src_offset++;
        } else if (bytes[src_offset + 1U] != 0U) {
            WCHAR uni = ff_oem2uni((WCHAR)(((uint16_t)c << 8) | bytes[src_offset + 1U]), FF_CODE_PAGE);
            codepoint = (uni != 0U) ? (uint32_t)uni : (uint32_t)'?';
            src_offset += 2U;
            converted_any = converted_any || (uni != 0U);
        } else {
            codepoint = '?';
            src_offset++;
        }

        {
            size_t written = ui_utf8_encode(codepoint, &dst[dst_offset], dst_len - dst_offset - 1U);
            if (written == 0U) {
                break;
            }
            dst_offset += written;
        }
    }

    dst[dst_offset] = '\0';
    return converted_any || dst_offset > 0U;
}

static void ui_fs_name_to_display_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (ui_utf8_is_valid(src)) {
        ui_utf8_safe_copy(dst, dst_len, src);
        return;
    }
    if (ui_cp936_to_utf8_copy(dst, dst_len, src)) {
        return;
    }
    ui_utf8_safe_copy(dst, dst_len, src);
}

static const char *ui_file_ext(const char *name)
{
    const char *dot = NULL;

    if (name == NULL) {
        return "";
    }
    dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0') {
        return "";
    }
    return dot + 1;
}

static bool ui_file_dir_is_system_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return true;
    }
    return name[0] == '.' ||
           strcasecmp(name, "System Volume Information") == 0 ||
           strcasecmp(name, "$RECYCLE.BIN") == 0 ||
           strncasecmp(name, "FOUND.", strlen("FOUND.")) == 0 ||
           strcasecmp(name, "LOST.DIR") == 0 ||
           strcasecmp(name, "RECYCLED") == 0 ||
           strcasecmp(name, "RECYCLER") == 0 ||
           strcasecmp(name, "__MACOSX") == 0;
}

static ui_file_kind_t ui_file_kind_from_name(const char *name)
{
    const char *ext = ui_file_ext(name);

    if (strcasecmp(ext, "gba") == 0) {
        return UI_FILE_KIND_ROM_GBA;
    }
    if (strcasecmp(ext, "gb") == 0 || strcasecmp(ext, "gbc") == 0) {
        return UI_FILE_KIND_ROM_MBC5;
    }
    if (strcasecmp(ext, "sav") == 0 || strcasecmp(ext, "srm") == 0) {
        return UI_FILE_KIND_SAVE;
    }
    if (strcasecmp(ext, "mp3") == 0 ||
        strcasecmp(ext, "aac") == 0 ||
        strcasecmp(ext, "flac") == 0 ||
        strcasecmp(ext, "wav") == 0) {
        return UI_FILE_KIND_AUDIO;
    }
    if (ui_reader_native_file_supported_name(name)) {
        return UI_FILE_KIND_READER;
    }
    return UI_FILE_KIND_UNSUPPORTED;
}

static bool ui_file_kind_matches_filter(ui_file_kind_t kind, ui_file_filter_t filter)
{
    switch (filter) {
        case UI_FILE_FILTER_NONE:
            return true;
        case UI_FILE_FILTER_ROM_GBA:
            return kind == UI_FILE_KIND_ROM_GBA;
        case UI_FILE_FILTER_ROM_MBC5:
            return kind == UI_FILE_KIND_ROM_MBC5;
        case UI_FILE_FILTER_SAVE:
            return kind == UI_FILE_KIND_SAVE;
        case UI_FILE_FILTER_AUDIO:
            return kind == UI_FILE_KIND_AUDIO;
        case UI_FILE_FILTER_READER:
            return kind == UI_FILE_KIND_READER;
        default:
            return false;
    }
}

static uint8_t ui_file_sort_rank(const ui_file_entry_t *entry)
{
    if (entry == NULL) {
        return 0xFFU;
    }
    if (entry->is_dir) {
        return 0U;
    }
    switch (ui_file_kind_from_name(entry->name)) {
        case UI_FILE_KIND_ROM_GBA:
            return 1U;
        case UI_FILE_KIND_ROM_MBC5:
            return 2U;
        case UI_FILE_KIND_SAVE:
            return 3U;
        case UI_FILE_KIND_AUDIO:
            return 4U;
        case UI_FILE_KIND_READER:
            return 5U;
        case UI_FILE_KIND_UNSUPPORTED:
        default:
            return 6U;
    }
}

static int ui_file_entry_compare(const void *lhs, const void *rhs)
{
    const ui_file_entry_t *a = (const ui_file_entry_t *)lhs;
    const ui_file_entry_t *b = (const ui_file_entry_t *)rhs;
    uint8_t rank_a = ui_file_sort_rank(a);
    uint8_t rank_b = ui_file_sort_rank(b);
    int cmp;

    if (rank_a != rank_b) {
        return (rank_a < rank_b) ? -1 : 1;
    }
    cmp = strcasecmp(a->name, b->name);
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(a->name, b->name);
    if (cmp != 0) {
        return cmp;
    }
    return strcmp(a->path, b->path);
}

static uint8_t ui_file_action_count_for_kind(ui_file_kind_t kind)
{
    if (kind == UI_FILE_KIND_ROM_GBA || kind == UI_FILE_KIND_ROM_MBC5) {
        return 4U;
    }
    if (kind == UI_FILE_KIND_SAVE) {
        return 2U;
    }
    if (kind == UI_FILE_KIND_AUDIO) {
        return 1U;
    }
    return 0U;
}

static ui_file_action_t ui_file_action_for_kind(ui_file_kind_t kind, uint8_t index)
{
    if (kind == UI_FILE_KIND_SAVE) {
        if (s_cart_mode == BURNER_CART_MODE_GBA) {
            return (index == 0U) ? UI_FILE_ACTION_WRITE_GBA_SAVE_NEW : UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW;
        }
        return (index == 0U) ? UI_FILE_ACTION_WRITE_SAVE : UI_FILE_ACTION_VERIFY_SAVE;
    }
    switch (index) {
        case 0:
            return UI_FILE_ACTION_BURN_PSRAM;
        case 1:
            return UI_FILE_ACTION_BURN_PIPELINE;
        case 2:
            return UI_FILE_ACTION_BURN_DIRECT;
        case 3:
        default:
            return UI_FILE_ACTION_VERIFY_ROM;
    }
}

static const char *ui_file_action_label(ui_file_action_t action)
{
    switch (action) {
        case UI_FILE_ACTION_BURN_PSRAM:
            return ui_tr("Burn via PSRAM");
        case UI_FILE_ACTION_BURN_PIPELINE:
            return ui_tr("Burn pipeline");
        case UI_FILE_ACTION_BURN_DIRECT:
            return ui_tr("Burn direct");
        case UI_FILE_ACTION_VERIFY_ROM:
            return ui_tr("Verify ROM");
        case UI_FILE_ACTION_WRITE_SAVE:
            return ui_tr("Write save");
        case UI_FILE_ACTION_VERIFY_SAVE:
            return ui_tr("Verify save");
        case UI_FILE_ACTION_WRITE_GBA_SAVE_NEW:
            return ui_tr("Write GBA save");
        case UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW:
            return ui_tr("Verify GBA save");
        default:
            return "--";
    }
}

static const char *ui_file_kind_label(ui_file_kind_t kind)
{
    switch (kind) {
        case UI_FILE_KIND_ROM_GBA:
            return "GBA ROM";
        case UI_FILE_KIND_ROM_MBC5:
            return "GB/GBC ROM";
        case UI_FILE_KIND_SAVE:
            return ui_tr("Save file");
        case UI_FILE_KIND_AUDIO:
            return "Audio";
        case UI_FILE_KIND_READER:
            return ui_tr("Reader");
        case UI_FILE_KIND_UNSUPPORTED:
        default:
            return ui_tr("File");
    }
}

static burner_cart_mode_t ui_file_cart_mode_for_kind(ui_file_kind_t kind)
{
    return (kind == UI_FILE_KIND_ROM_GBA) ? BURNER_CART_MODE_GBA : BURNER_CART_MODE_MBC5;
}

static bool ui_file_parent_path(const char *path, char *out, size_t out_len)
{
    const char *slash = NULL;
    size_t len;

    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        return true;
    }
    len = (size_t)(slash - path);
    if (len >= out_len) {
        return false;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

static bool ui_file_page_is_book_scope(const ui_model_t *model)
{
    return model != NULL && model->page == UI_PAGE_FILES && model->file_book_scope;
}

#include "reader/ui_reader_logic.inc"

static void ui_format_page_header(const ui_model_t *model, char *header, size_t header_len)
{
    if (header == NULL || header_len == 0U) {
        return;
    }
    header[0] = '\0';
    if (model == NULL) {
        return;
    }

    snprintf(header, header_len, "%s", ui_page_title(model->page));
    if (ui_page_is_icon_grid(model->page)) {
        if (model->page == UI_PAGE_ROOT) {
            snprintf(header, header_len, "MORI");
        }
        return;
    }
    if (model->page == UI_PAGE_BURNER) {
        snprintf(header, header_len, "%s", ui_tr("Burner"));
        return;
    }
    if (model->page == UI_PAGE_READER && s_reader_ctx.active && s_reader_ctx.path[0] != '\0') {
        snprintf(header, header_len, "%s/", ui_tr("Reader"));
        strncat(header, s_reader_ctx.path, header_len - strlen(header) - 1U);
        return;
    }
    if ((model->page != UI_PAGE_FILES && model->page != UI_PAGE_MUSIC_FILES) || model->file_path[0] == '\0') {
        return;
    }
    if (ui_file_page_is_book_scope(model)) {
        if (model->file_path[0] == '\0') {
            snprintf(header, header_len, "%s", ui_tr("Reader"));
            return;
        }
        snprintf(header, header_len, "%s/", ui_tr("Reader"));
        strncat(header, model->file_path, header_len - strlen(header) - 1U);
        return;
    }
    snprintf(header, header_len, "TF/");
    strncat(header, model->file_path, header_len - strlen(header) - 1U);
}

static const char *ui_page_title(ui_page_t page)
{
    switch (page) {
        case UI_PAGE_ROOT:
            return "MORI";
        case UI_PAGE_SYSTEM:
            return ui_tr("System");
        case UI_PAGE_APP_MANAGER:
            return ui_tr("Apps");
        case UI_PAGE_TF:
            return "TF";
        case UI_PAGE_MUSIC_FILES:
            return ui_tr("Music");
        case UI_PAGE_MUSIC_PLAYER:
            return ui_tr("Now Playing");
        case UI_PAGE_FILES:
            return ui_tr("Files");
        case UI_PAGE_READER:
            return ui_tr("Reader");
        case UI_PAGE_FILE_ACTIONS:
            return ui_tr("Actions");
        case UI_PAGE_WIFI:
            return "Wi-Fi";
        case UI_PAGE_WIFI_QR:
            return ui_tr("View QR");
        case UI_PAGE_SMB:
            return "SMB";
        case UI_PAGE_SMB_TEXT_INPUT:
            return ui_smb_field_label(s_smb_ctx.editing_field);
        case UI_PAGE_POWER:
            return ui_tr("Power");
        case UI_PAGE_BURNER:
            return ui_tr("Burner");
        case UI_PAGE_BURN_ROM:
            return ui_cart_mode_label(s_cart_mode);
        case UI_PAGE_BURN_SAVE:
            return ui_tr("Save");
        case UI_PAGE_SETTINGS:
            return ui_tr("Settings");
        case UI_PAGE_TASK_STATUS:
            return ui_tr("Task");
        case UI_PAGE_TASK_RESULT:
            return ui_tr("Result");
        default:
            return "MORI";
    }
}

static const char *ui_wifi_state_name(ui_wifi_state_t state)
{
    switch (state) {
        case UI_WIFI_STATE_CONNECTED:
            return ui_tr("connected");
        case UI_WIFI_STATE_PROVISIONING:
            return ui_tr("provisioning");
        case UI_WIFI_STATE_DISCONNECTED:
            return ui_tr("disconnected");
        case UI_WIFI_STATE_UNKNOWN:
        default:
            return ui_tr("unknown");
    }
}

static const char *ui_burn_state_text(burner_state_t state)
{
    switch (state) {
        case BURNER_STATE_IDLE:
            return ui_tr("idle");
        case BURNER_STATE_RECEIVING:
            return ui_tr("receiving");
        case BURNER_STATE_BURNING:
            return ui_tr("running");
        case BURNER_STATE_DONE:
            return ui_tr("done");
        case BURNER_STATE_ERROR:
            return ui_tr("error");
        case BURNER_STATE_CANCELLED:
            return ui_tr("cancelled");
        default:
            return ui_tr("unknown");
    }
}

static const char *ui_strip_burner_status_prefix(const char *text)
{
    const char *colon = NULL;

    if (text == NULL) {
        return "";
    }
    if (strncmp(text, "burner ", strlen("burner ")) != 0) {
        return text;
    }
    colon = strstr(text, ": ");
    return (colon != NULL) ? (colon + 2) : text;
}

static const char *ui_status_text_to_display(const char *text)
{
    static char connected_ip_text[48];

    if (text == NULL || text[0] == '\0') {
        return "";
    }
    if (strcmp(text, "system initializing") == 0 || strcmp(text, "system booting") == 0) {
        return ui_tr("initializing");
    }
    if (strcmp(text, "system initialized") == 0 || strcmp(text, "ready") == 0) {
        return ui_tr("ready");
    }
    if (strcmp(text, "connecting wifi.txt") == 0 || strcmp(text, "connecting saved wifi") == 0) {
        return ui_tr("connecting Wi-Fi");
    }
    if (strcmp(text, "wifi connected") == 0 || strcmp(text, "Wi-Fi connected") == 0) {
        return ui_tr("Wi-Fi connected");
    }
    if (strncmp(text, "wifi connected ", strlen("wifi connected ")) == 0) {
        snprintf(connected_ip_text, sizeof(connected_ip_text), ui_tr("Wi-Fi connected %s"), text + strlen("wifi connected "));
        return connected_ip_text;
    }
    if (strcmp(text, "wifi disconnected") == 0 || strcmp(text, "Wi-Fi disconnected") == 0) {
        return ui_tr("Wi-Fi disconnected");
    }
    if (strcmp(text, "wifi provisioning mode") == 0 || strcmp(text, "no saved wifi, provisioning") == 0 ||
        strcmp(text, "provisioning AP") == 0) {
        return ui_tr("provisioning AP");
    }
    if (strncmp(text, "burner ", strlen("burner ")) == 0) {
        return ui_strip_burner_status_prefix(text);
    }
    return text;
}

static bool ui_burn_status_text_is_terminal(const char *text)
{
    if (text == NULL) {
        return false;
    }
    return strncmp(text, "burner done:", strlen("burner done:")) == 0 ||
           strncmp(text, "burner error:", strlen("burner error:")) == 0 ||
           strncmp(text, "burner cancelled:", strlen("burner cancelled:")) == 0;
}

static bool ui_build_file_entry_for_dirent(
    const char *dir_rel,
    const struct dirent *dirent,
    ui_file_entry_t *entry_out)
{
    char child_rel[TF_PATH_LEN_MAX] = {0};
    char child_full[TF_PATH_LEN_MAX + 64] = {0};
    struct stat st;
    int n;

    if (dirent == NULL || entry_out == NULL) {
        return false;
    }
    if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) {
        return false;
    }

    if (dir_rel == NULL || dir_rel[0] == '\0') {
        n = snprintf(child_rel, sizeof(child_rel), "%s", dirent->d_name);
    } else {
        n = snprintf(child_rel, sizeof(child_rel), "%s/%s", dir_rel, dirent->d_name);
    }
    if (n <= 0 || n >= (int)sizeof(child_rel)) {
        return false;
    }
    if (!burner_build_full_path(child_rel, child_full, sizeof(child_full))) {
        return false;
    }
    if (stat(child_full, &st) != 0) {
        return false;
    }

    memset(entry_out, 0, sizeof(*entry_out));
    ui_fs_name_to_display_text(entry_out->name, sizeof(entry_out->name), dirent->d_name);
    snprintf(entry_out->path, sizeof(entry_out->path), "%s", child_rel);
    entry_out->is_dir = S_ISDIR(st.st_mode);
    if (!entry_out->is_dir && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= UINT32_MAX) {
        entry_out->size = (uint32_t)st.st_size;
    }
    return true;
}

static void ui_scan_file_window_locked(ui_model_t *model)
{
    char normalized[TF_PATH_LEN_MAX] = {0};
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    ui_file_entry_t *entries = NULL;
    DIR *dir = NULL;
    struct dirent *dirent = NULL;
    uint16_t visible_begin;
    uint16_t total = 0;
    uint16_t loaded = 0;

    if (model == NULL) {
        return;
    }

    memset(model->file_window, 0, sizeof(model->file_window));
    model->file_window_start = model->file_scroll;
    model->file_loaded_count = 0;
    model->file_total = 0;

    if (card == NULL) {
        ui_set_status_locked(model, ui_tr("TF card not ready"));
        return;
    }
    if (usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, ui_tr("TF busy by USB host"));
        return;
    }
    if (!burner_normalize_rel_path(model->file_path, normalized, sizeof(normalized), true)) {
        ui_set_status_locked(model, ui_tr("invalid path"));
        model->file_path[0] = '\0';
        normalized[0] = '\0';
    }
    snprintf(model->file_path, sizeof(model->file_path), "%s", normalized);
    if (!burner_build_full_path(normalized, full_path, sizeof(full_path))) {
        ui_set_status_locked(model, ui_tr("path too long"));
        return;
    }

    dir = opendir(full_path);
    if (dir == NULL) {
        ui_set_status_locked(model, ui_tr("open directory failed"));
        return;
    }

    entries = (ui_file_entry_t *)calloc(UI_FILE_SCAN_LIMIT, sizeof(*entries));
    if (entries == NULL) {
        closedir(dir);
        ui_set_status_locked(model, ui_tr("no memory"));
        return;
    }

    while ((dirent = readdir(dir)) != NULL && total < UI_FILE_SCAN_LIMIT) {
        if (!ui_build_file_entry_for_dirent(normalized, dirent, &entries[total])) {
            continue;
        }
        if (model->file_filter != UI_FILE_FILTER_NONE) {
            if (entries[total].is_dir) {
                if (ui_file_dir_is_system_name(dirent->d_name)) {
                    continue;
                }
            } else if (!ui_file_kind_matches_filter(ui_file_kind_from_name(entries[total].name), model->file_filter)) {
                continue;
            }
        }
        total++;
    }
    closedir(dir);

    if (total > 1U) {
        qsort(entries, total, sizeof(entries[0]), ui_file_entry_compare);
    }

    if (model->file_selected >= total && total > 0U) {
        model->file_selected = total - 1U;
    }
    if (total == 0U) {
        model->file_selected = 0;
        model->file_scroll = 0;
    } else {
        model->file_scroll = ui_file_scroll_for_selected(model, model->file_selected, model->file_scroll, total);
    }
    visible_begin = model->file_scroll;
    if (total > UI_FILE_WINDOW_COUNT && visible_begin + UI_FILE_WINDOW_COUNT > total) {
        visible_begin = total - UI_FILE_WINDOW_COUNT;
    }
    model->file_window_start = visible_begin;

    for (uint16_t i = visible_begin; i < total && loaded < UI_FILE_WINDOW_COUNT; ++i) {
        entries[i].ordinal = i;
        model->file_window[loaded++] = entries[i];
    }

    model->file_total = total;
    model->file_loaded_count = loaded;
    if (total >= UI_FILE_SCAN_LIMIT) {
        ui_set_status_locked(model, ui_tr("directory clipped"));
    } else if (total == 0U && model->file_filter != UI_FILE_FILTER_NONE) {
        ui_set_status_locked(model, ui_tr("no matching files"));
    } else if (total == 0U) {
        ui_set_status_locked(model, ui_tr("empty directory"));
    } else if (model->file_book_scope && normalized[0] == '\0') {
        ui_set_status_locked(model, ui_tr("Reader"));
    } else {
        ui_set_status_locked(model, normalized[0] == '\0' ? ui_tr("TF root") : normalized);
    }
    free(entries);
}

static bool ui_current_file_locked(const ui_model_t *model, ui_file_entry_t *entry_out)
{
    if (model == NULL || entry_out == NULL) {
        return false;
    }
    for (uint16_t i = 0; i < model->file_loaded_count && i < UI_FILE_WINDOW_COUNT; ++i) {
        if (model->file_window[i].ordinal == model->file_selected) {
            *entry_out = model->file_window[i];
            return true;
        }
    }
    return false;
}

static bool ui_file_entry_for_visible_row(const ui_model_t *model, uint16_t row, ui_file_entry_t *entry_out)
{
    uint16_t ordinal;
    uint16_t window_index;

    if (model == NULL || entry_out == NULL) {
        return false;
    }
    ordinal = model->file_scroll + row;
    if (ordinal >= model->file_total ||
        ordinal < model->file_window_start ||
        ordinal >= model->file_window_start + model->file_loaded_count) {
        return false;
    }
    window_index = ordinal - model->file_window_start;
    *entry_out = model->file_window[window_index];
    return true;
}

static bool ui_file_visible_window_loaded_locked(const ui_model_t *model)
{
    uint16_t visible_end;
    uint16_t visible_rows;

    if (model == NULL) {
        return false;
    }
    if (model->file_total == 0U) {
        return true;
    }
    visible_rows = ui_file_visible_rows_for_page(model);
    visible_end = model->file_scroll + visible_rows;
    if (visible_end > model->file_total) {
        visible_end = model->file_total;
    }
    return model->file_scroll >= model->file_window_start &&
           visible_end <= model->file_window_start + model->file_loaded_count;
}

static void ui_file_ensure_window_locked(ui_model_t *model, bool force_scan)
{
    if (model == NULL) {
        return;
    }
    if (force_scan || !ui_file_visible_window_loaded_locked(model)) {
        uint16_t cached_selected = model->file_selected;

        ui_scan_file_window_locked(model);
        if (model->file_total > 0U && cached_selected < model->file_total) {
            model->file_selected = cached_selected;
            model->file_scroll =
                ui_file_scroll_for_selected(model, model->file_selected, model->file_scroll, model->file_total);
        }
        model->dirty = true;
    }
}

static void ui_file_move_locked(ui_model_t *model, int delta)
{
    int selected;
    int total;
    uint16_t old_scroll;
    uint16_t old_window_start;

    if (model == NULL || model->file_total == 0U || delta == 0) {
        return;
    }
    total = (int)model->file_total;
    selected = ((int)model->file_selected + delta) % total;
    if (selected < 0) {
        selected += total;
    }
    if ((uint16_t)selected != model->file_selected) {
        model->file_selected = (uint16_t)selected;
        old_scroll = model->file_scroll;
        old_window_start = model->file_window_start;
        model->file_scroll =
            ui_file_scroll_for_selected(model, model->file_selected, model->file_scroll, model->file_total);
        ui_file_ensure_window_locked(model, false);
        if (model->file_scroll == old_scroll && model->file_window_start == old_window_start &&
            ui_file_visible_window_loaded_locked(model)) {
            ui_mark_motion_dirty(model);
        } else {
            ui_mark_content_dirty(model);
        }
    }
}

static void ui_open_files_locked(ui_model_t *model, const char *path, bool reset_selection)
{
    if (model == NULL) {
        return;
    }
    if (path != NULL) {
        snprintf(model->file_path, sizeof(model->file_path), "%s", path);
    }
    if (reset_selection) {
        model->file_selected = 0;
        model->file_scroll = 0;
    }
    ui_scan_file_window_locked(model);
    model->dirty = true;
}

static void ui_open_files_with_filter_locked(
    ui_model_t *model,
    const char *path,
    bool reset_selection,
    ui_file_filter_t filter)
{
    if (model == NULL) {
        return;
    }
    model->file_filter = filter;
    model->page = (filter == UI_FILE_FILTER_MP3) ? UI_PAGE_MUSIC_FILES : UI_PAGE_FILES;
    ui_open_files_locked(model, path, reset_selection);
}

static void ui_open_book_library_locked(ui_model_t *model, bool reset_selection)
{
    if (model == NULL) {
        return;
    }
    if (model->page != UI_PAGE_FILES) {
        ui_push_current_page_locked(model);
    }
    model->page = UI_PAGE_FILES;
    model->parent_page = UI_PAGE_ROOT;
    model->selected = 0;
    model->scroll = 0;
    model->file_filter = UI_FILE_FILTER_READER;
    model->file_book_scope = true;
    ui_music_set_drawer_open_locked(model, false);
    ui_open_files_locked(model, "", reset_selection);
    ui_mark_chrome_dirty(model);
    ui_mark_content_dirty(model);
}

static void ui_open_music_library_locked(ui_model_t *model, bool reset_selection)
{
    bool restored = false;

    if (model == NULL) {
        return;
    }
    if (model->page != UI_PAGE_MUSIC_PLAYER) {
        ui_push_current_page_locked(model);
    }
    model->page = UI_PAGE_MUSIC_PLAYER;
    model->parent_page = UI_PAGE_ROOT;
    model->selected = 0;
    model->scroll = 0;
    model->file_filter = UI_FILE_FILTER_AUDIO;
    model->file_book_scope = false;
    ui_music_set_drawer_open_locked(model, false);
    ui_open_files_locked(model, UI_MUSIC_DIR, reset_selection);
    restored = ui_music_restore_history_locked(model);
    if (restored) {
        model->page = UI_PAGE_MUSIC_PLAYER;
        model->parent_page = UI_PAGE_ROOT;
        model->selected = 0;
        model->scroll = 0;
        ui_set_status_locked(model, s_music_ctx.current.name[0] != '\0' ? s_music_ctx.current.name : ui_tr("Music"));
    }
    ui_mark_chrome_dirty(model);
    ui_mark_content_dirty(model);
}

static ui_file_filter_t ui_burner_rom_file_filter(void)
{
    return (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_FILE_FILTER_ROM_GBA : UI_FILE_FILTER_ROM_MBC5;
}

static void ui_open_file_action_page_locked(ui_model_t *model, const ui_file_entry_t *entry)
{
    if (model == NULL || entry == NULL) {
        return;
    }
    model->action_kind = ui_file_kind_from_name(entry->name);
    if (ui_file_action_count_for_kind(model->action_kind) == 0U) {
        ui_set_status_locked(model, ui_tr("unsupported file"));
        return;
    }
    model->action_file = *entry;
    model->page = UI_PAGE_FILE_ACTIONS;
    model->parent_page = UI_PAGE_FILES;
    model->selected = 0;
    model->scroll = 0;
    ui_set_status_locked(model, ui_file_kind_label(model->action_kind));
}

#include "music/ui_music_logic.inc"

#include "burner/ui/ui_burner_tasks.inc"

static void ui_smb_work_task(void *param)
{
    ui_work_request_t *request = (ui_work_request_t *)param;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *ok_text = ui_tr("done");
    char ok_buf[UI_STATUS_TEXT_MAX_LEN] = {0};
    bool task_with_caps = false;

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = request->task_with_caps;

    switch (request->type) {
        case UI_WORK_SMB_SCAN: {
            ui_smb_scan_result_t scan_ctx = {0};

            ok_text = "SMB scan done";
            err = smb_client_discover(UI_SMB_SCAN_TIMEOUT_MS, ui_smb_discover_collect_cb, &scan_ctx);
            if (err == ESP_OK && ui_take_model_lock()) {
                ui_smb_ensure_context();
                memcpy(s_smb_ctx.servers, scan_ctx.servers, sizeof(s_smb_ctx.servers));
                s_smb_ctx.server_count = scan_ctx.server_count;
                if (s_smb_ctx.server_count > 0U && s_smb_ctx.config.host[0] == '\0') {
                    snprintf(
                        s_smb_ctx.config.host,
                        sizeof(s_smb_ctx.config.host),
                        "%s",
                        s_smb_ctx.servers[0].host);
                    s_smb_ctx.config.port = s_smb_ctx.servers[0].port > 0 ? s_smb_ctx.servers[0].port : 445;
                }
                snprintf(ok_buf, sizeof(ok_buf), "SMB found %u", (unsigned)s_smb_ctx.server_count);
                ok_text = ok_buf;
                ui_mark_content_dirty(&s_model);
                xSemaphoreGive(s_model_lock);
            }
            break;
        }
        case UI_WORK_SMB_LOAD_FAVORITES: {
            ui_smb_favorite_result_t favorite_ctx = {0};

            ok_text = "SMB saved loaded";
            err = smb_client_list_favorites(ui_smb_favorite_collect_cb, &favorite_ctx);
            if (err == ESP_OK && ui_take_model_lock()) {
                ui_smb_ensure_context();
                memcpy(s_smb_ctx.favorites, favorite_ctx.favorites, sizeof(s_smb_ctx.favorites));
                s_smb_ctx.favorite_count = favorite_ctx.favorite_count;
                s_smb_ctx.selected_favorite_id = 0;
                snprintf(ok_buf, sizeof(ok_buf), "SMB saved %u", (unsigned)s_smb_ctx.favorite_count);
                ok_text = ok_buf;
                ui_mark_content_dirty(&s_model);
                xSemaphoreGive(s_model_lock);
            }
            break;
        }
        case UI_WORK_SMB_SAVE_FAVORITE: {
            smb_client_config_t config = {0};
            smb_client_favorite_t favorite = {0};

            if (ui_take_model_lock()) {
                ui_smb_ensure_context();
                config = s_smb_ctx.config;
                xSemaphoreGive(s_model_lock);
            }
            ok_text = "SMB position saved";
            if (config.user[0] == '\0' && config.password[0] == '\0') {
                (void)smb_client_apply_saved_auth(&config);
            }
            (void)smb_client_save_server_auth(&config);
            err = smb_client_save_favorite(&config, &favorite);
            if (err == ESP_OK && ui_take_model_lock()) {
                ui_smb_favorite_result_t favorite_ctx = {0};

                ui_smb_ensure_context();
                (void)smb_client_list_favorites(ui_smb_favorite_collect_cb, &favorite_ctx);
                memcpy(s_smb_ctx.favorites, favorite_ctx.favorites, sizeof(s_smb_ctx.favorites));
                s_smb_ctx.favorite_count = favorite_ctx.favorite_count;
                s_smb_ctx.selected_favorite_id = favorite.id;
                snprintf(ok_buf, sizeof(ok_buf), "Saved %.*s", (int)sizeof(ok_buf) - 8, favorite.label);
                ok_text = ok_buf;
                ui_mark_content_dirty(&s_model);
                xSemaphoreGive(s_model_lock);
            } else if (err != ESP_OK && ui_take_model_lock()) {
                smb_client_status_t status = {0};

                smb_client_get_status(&status);
                if (status.last_error[0] != '\0') {
                    snprintf(ok_buf, sizeof(ok_buf), "%.*s", (int)sizeof(ok_buf) - 1, status.last_error);
                    ok_text = ok_buf;
                }
                xSemaphoreGive(s_model_lock);
            }
            break;
        }
        case UI_WORK_SMB_CONNECT: {
            smb_client_config_t config = {0};
            uint32_t favorite_id = 0;

            if (ui_take_model_lock()) {
                ui_smb_ensure_context();
                config = s_smb_ctx.config;
                favorite_id = s_smb_ctx.selected_favorite_id;
                xSemaphoreGive(s_model_lock);
            }
            ok_text = "SMB connected";
            err = (favorite_id != 0U) ? smb_client_connect_favorite(favorite_id) : smb_client_connect(&config);
            if (err == ESP_OK && ui_take_model_lock()) {
                smb_client_status_t status = {0};

                smb_client_get_status(&status);
                s_smb_ctx.config = status.config;
                ui_mark_content_dirty(&s_model);
                xSemaphoreGive(s_model_lock);
            } else if (err != ESP_OK && ui_take_model_lock()) {
                smb_client_status_t status = {0};

                smb_client_get_status(&status);
                if (status.last_error[0] != '\0') {
                    snprintf(ok_buf, sizeof(ok_buf), "%.*s", (int)sizeof(ok_buf) - 1, status.last_error);
                    ok_text = ok_buf;
                }
                xSemaphoreGive(s_model_lock);
            }
            break;
        }
        case UI_WORK_SMB_DISCONNECT:
            smb_client_disconnect();
            if (ui_take_model_lock()) {
                s_smb_ctx.selected_favorite_id = 0;
                ui_mark_content_dirty(&s_model);
                xSemaphoreGive(s_model_lock);
            }
            ok_text = "SMB disconnected";
            err = ESP_OK;
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    ui_finish_work_task(request->type, err, ok_text, request->cart_mode);
    free(request);
    ui_delete_async_task(task_with_caps);
}

#include "burner/ui/ui_burner_probe.inc"

static void ui_start_work_async(ui_work_type_t type)
{
    ui_work_request_t *request = NULL;
    bool *active = NULL;
    const char *status = ui_tr("working");
    const char *task_name = "ui_work";
    uint32_t stack_size = UI_STORAGE_TASK_STACK_SIZE;
    UBaseType_t priority = UI_STORAGE_TASK_PRIORITY;
    BaseType_t core_id = UI_SYSTEM_TASK_CORE_ID;
    TaskFunction_t task_fn = ui_work_task;
    bool is_burn_probe = (type == UI_WORK_BURN_READ_ID || type == UI_WORK_BURN_UNLOCK_PPB);
    bool is_smb_work = (type == UI_WORK_SMB_SCAN || type == UI_WORK_SMB_LOAD_FAVORITES ||
                        type == UI_WORK_SMB_SAVE_FAVORITE || type == UI_WORK_SMB_CONNECT ||
                        type == UI_WORK_SMB_DISCONNECT);
    bool is_gbx_cache = (type == UI_WORK_UPDATE_GBX_CACHE);
    bool is_boot_rg = (type == UI_WORK_BOOT_RETRO_GO);
    BaseType_t ret = pdFAIL;

    if (!ui_take_model_lock()) {
        return;
    }
    if (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
        type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLOSE_AP ||
        type == UI_WORK_WIFI_CLEAR_SAVED) {
        active = &s_wifi_work_active;
        status = ui_tr("Wi-Fi working");
        stack_size = UI_WIFI_TASK_STACK_SIZE;
        priority = UI_WIFI_TASK_PRIORITY;
    } else if (is_smb_work) {
        active = &s_smb_work_active;
        task_fn = ui_smb_work_task;
        task_name = "ui_smb";
        if (type == UI_WORK_SMB_SCAN) {
            status = "SMB scanning";
        } else if (type == UI_WORK_SMB_LOAD_FAVORITES) {
            status = "SMB loading saved";
        } else if (type == UI_WORK_SMB_SAVE_FAVORITE) {
            status = "SMB saving";
        } else {
            status = "SMB working";
        }
        stack_size = UI_SMB_TASK_STACK_SIZE;
        priority = UI_WIFI_TASK_PRIORITY;
    } else if (type == UI_WORK_STORAGE_USB_ENABLE || type == UI_WORK_STORAGE_USB_DISABLE ||
               is_boot_rg || is_gbx_cache) {
        active = &s_storage_work_active;
        status = is_gbx_cache ? ui_tr("Update GBX cache") : ui_tr("storage working");
        if (is_boot_rg) {
            status = ui_tr("booting Retro-Go");
        }
        task_name = is_gbx_cache ? "ui_gbx_cache" : (is_boot_rg ? "ui_boot_rg" : "ui_work");
        stack_size = is_gbx_cache ? UI_BURN_PROBE_TASK_STACK_SIZE :
                                    (is_boot_rg ? UI_BOOT_RETRO_GO_TASK_STACK_SIZE : UI_STORAGE_TASK_STACK_SIZE);
        priority = UI_STORAGE_TASK_PRIORITY;
        if (is_boot_rg) {
            task_fn = ui_boot_rg_task;
        }
    } else if (is_burn_probe) {
        active = &s_burn_probe_active;
        status = (type == UI_WORK_BURN_UNLOCK_PPB) ? ui_tr("PPB unlocking") : ui_tr("analyzing cart");
        task_fn = ui_burn_probe_task;
        task_name = "ui_probe";
        stack_size = UI_BURN_PROBE_TASK_STACK_SIZE;
        priority = UI_STORAGE_TASK_PRIORITY;
        core_id = UI_BURN_TASK_CORE_ID;
    } else {
        active = NULL;
        status = ui_tr("working");
        if (type == UI_WORK_BURN_ERASE_CHIP ||
            type == UI_WORK_BURN_DUMP_ROM || type == UI_WORK_BURN_DUMP_SAVE) {
            stack_size = UI_BURN_WORK_TASK_STACK_SIZE;
            priority = UI_STORAGE_TASK_PRIORITY;
            core_id = UI_BURN_TASK_CORE_ID;
        }
    }
    if (is_burn_probe && ui_burner_operation_active()) {
        ui_set_status_locked(&s_model, ui_tr("action already running"));
        xSemaphoreGive(s_model_lock);
        return;
    }
    if (active != NULL && *active) {
        ui_set_status_locked(&s_model, ui_tr("action already running"));
        xSemaphoreGive(s_model_lock);
        return;
    }
    if (active != NULL) {
        *active = true;
    }
    if (type == UI_WORK_BURN_READ_ID) {
        ui_reset_cart_analysis_locked();
        ui_mark_content_dirty(&s_model);
        s_model.dirty = true;
    }
    ui_set_status_locked(&s_model, status);
    xSemaphoreGive(s_model_lock);

    request = (ui_work_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        ui_finish_work_task(type, ESP_ERR_NO_MEM, NULL, s_cart_mode);
        return;
    }
    request->type = type;
    request->cart_mode = s_cart_mode;
    request->task_with_caps = false;

    /*
     * Keep Retro-Go off the WithCaps/SPIRAM stack path.
     * esp_ota_set_boot_partition() must execute on an internal-DRAM task stack.
     */
    if (is_burn_probe || is_gbx_cache) {
        request->task_with_caps = true;
        ret = xTaskCreatePinnedToCoreWithCaps(
            task_fn,
            task_name,
            stack_size,
            request,
            priority,
            NULL,
            core_id,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ret == pdPASS) {
            return;
        }
        request->task_with_caps = false;
    }

    ret = xTaskCreatePinnedToCore(
        task_fn,
        task_name,
        stack_size,
        request,
        priority,
        NULL,
        core_id);
    if (ret != pdPASS) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        ESP_LOGW(
            UI_TAG,
            "UI work task create failed: type=%d ret=%d stack=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
            (int)type,
            (int)ret,
            (unsigned)stack_size,
            (unsigned)internal_free,
            (unsigned)internal_largest,
            (unsigned)psram_free,
            (unsigned)psram_largest);
        free(request);
        ui_finish_work_task(type, ESP_ERR_NO_MEM, NULL, s_cart_mode);
    }
}

static esp_err_t ui_create_canvas(lv_obj_t *scr)
{
    s_canvas_buf = heap_caps_malloc(UI_CANVAS_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_canvas_buf == NULL) {
        s_canvas_buf = heap_caps_malloc(UI_CANVAS_PIXELS * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_canvas_buf == NULL) {
        ESP_LOGE(UI_TAG, "alloc UI canvas failed (%u bytes)", (unsigned)(UI_CANVAS_PIXELS * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    s_canvas = lv_canvas_create(scr);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, UI_CONTENT_Y);
    lv_obj_set_size(s_canvas, UI_CANVAS_W, UI_CANVAS_H);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, UI_CANVAS_W, UI_CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    ui_px_clear();
    return ESP_OK;
}

static uint16_t ui_page_item_count(const ui_model_t *model)
{
    if (model == NULL) {
        return 0;
    }
    switch (model->page) {
        case UI_PAGE_ROOT:
            return ui_home_item_count();
        case UI_PAGE_SYSTEM:
            return UI_SYSTEM_ITEM_COUNT;
        case UI_PAGE_APP_MANAGER:
            return ui_app_manager_item_count();
        case UI_PAGE_TF:
            return UI_TF_ITEM_COUNT;
        case UI_PAGE_MUSIC_FILES:
            return model->file_total;
        case UI_PAGE_MUSIC_PLAYER:
            return 0;
        case UI_PAGE_FILES:
            return model->file_total;
        case UI_PAGE_READER:
            return (s_reader_ctx.page_count > UINT16_MAX) ? UINT16_MAX : (uint16_t)s_reader_ctx.page_count;
        case UI_PAGE_FILE_ACTIONS:
            return ui_file_action_count_for_kind(model->action_kind);
        case UI_PAGE_WIFI:
            return UI_WIFI_ITEM_COUNT;
        case UI_PAGE_WIFI_QR:
            return 0;
        case UI_PAGE_SMB:
            ui_smb_ensure_context();
            return ui_smb_item_count();
        case UI_PAGE_SMB_TEXT_INPUT:
            return ui_smb_text_input_count();
        case UI_PAGE_POWER:
            return UI_POWER_ITEM_COUNT;
        case UI_PAGE_BURNER:
            return UI_BURNER_MODE_COUNT;
        case UI_PAGE_BURN_ROM:
            return ui_burn_rom_item_count();
        case UI_PAGE_BURN_SAVE:
            return UI_BURN_SAVE_ITEM_COUNT;
        case UI_PAGE_SETTINGS:
            return UI_SETTINGS_ITEM_COUNT;
        case UI_PAGE_TASK_STATUS:
            if (s_task_cancel_confirm) {
                return UI_TASK_CANCEL_CONFIRM_ITEM_COUNT;
            }
            return (uint16_t)(UI_TASK_STATUS_ITEM_COUNT +
                              (model->gba_patch_sram ? 1U : 0U) +
                              (model->gba_patch_batteryless ? 1U : 0U) +
                              (model->gba_patch_waitcnt ? 1U : 0U));
        case UI_PAGE_TASK_RESULT:
            return UI_TASK_RESULT_ITEM_COUNT;
        default:
            return 0;
    }
}

static void ui_push_current_page_locked(const ui_model_t *model)
{
    if (model == NULL || s_nav_depth >= (sizeof(s_nav_stack) / sizeof(s_nav_stack[0]))) {
        return;
    }
    if (model->page == UI_PAGE_ROOT) {
        s_root_selected = model->selected;
    }
    s_nav_stack[s_nav_depth].page = model->page;
    s_nav_stack[s_nav_depth].parent = model->parent_page;
    s_nav_depth++;
}

static void ui_drop_nav_target_locked(ui_page_t page)
{
    if (s_nav_depth > 0U && s_nav_stack[s_nav_depth - 1U].page == page) {
        s_nav_depth--;
    }
}

static void ui_open_page_locked(ui_model_t *model, ui_page_t page)
{
    ui_page_t parent;

    if (model == NULL) {
        return;
    }
    if (model->page == UI_PAGE_READER && page != UI_PAGE_READER) {
        ui_reader_save_history_locked(model);
        ui_reader_clear_locked();
        model->reader_page = 0U;
    }
    parent = model->page;
    ui_push_current_page_locked(model);
    model->page = page;
    model->parent_page = parent;
    model->selected = (page == UI_PAGE_ROOT) ? s_root_selected : 0;
    model->scroll = 0;
    if (page != UI_PAGE_BURN_ROM) {
        s_burn_rom_write_menu = false;
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        s_burn_rom_write_prompt_until_ms = 0;
        s_burn_rom_verify_prompt_until_ms = 0;
    }
    if (page != UI_PAGE_FILES && page != UI_PAGE_FILE_ACTIONS) {
        model->file_book_scope = false;
    }
    if (page == UI_PAGE_FILES && parent != UI_PAGE_BURN_ROM && parent != UI_PAGE_BURN_SAVE &&
        parent != UI_PAGE_BURNER) {
        model->file_filter = UI_FILE_FILTER_NONE;
    }
    if (page == UI_PAGE_MUSIC_FILES) {
        model->file_filter = UI_FILE_FILTER_AUDIO;
        model->file_book_scope = false;
    }
    if (page == UI_PAGE_FILES || page == UI_PAGE_MUSIC_FILES) {
        ui_open_files_locked(model, model->file_path, false);
        model->parent_page = parent;
    } else {
        if (page == UI_PAGE_SMB) {
            ui_smb_ensure_context();
        }
        model->dirty = true;
    }
}

static void ui_open_root_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    if (model->page == UI_PAGE_READER) {
        ui_reader_save_history_locked(model);
    }
    ui_reader_clear_locked();
    model->page = UI_PAGE_ROOT;
    model->parent_page = UI_PAGE_ROOT;
    model->selected = (s_root_selected < ui_home_item_count()) ? s_root_selected : 0;
    model->scroll = 0;
    model->file_book_scope = false;
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
    s_burn_rom_write_prompt_until_ms = 0;
    s_burn_rom_verify_prompt_until_ms = 0;
    ui_music_set_drawer_open_locked(model, false);
    ui_clear_task_result_runtime_locked(model);
    s_nav_depth = 0;
    ui_set_status_locked(model, ui_tr("ready"));
}

static bool ui_task_status_operation_active(void)
{
    burner_status_t status = {0};

    burner_status_snapshot(&status);
    return s_file_start_active || status.state == BURNER_STATE_RECEIVING || status.state == BURNER_STATE_BURNING;
}

static ui_page_t ui_burn_task_entry_return_page(ui_page_t page)
{
    switch (page) {
        case UI_PAGE_BURN_ROM:
        case UI_PAGE_BURN_SAVE:
            return page;
        default:
            return UI_PAGE_BURNER;
    }
}

static ui_page_t ui_task_return_parent_page(ui_page_t page)
{
    switch (page) {
        case UI_PAGE_ROOT:
            return UI_PAGE_ROOT;
        case UI_PAGE_MUSIC_FILES:
            return UI_PAGE_ROOT;
        case UI_PAGE_MUSIC_PLAYER:
            return UI_PAGE_ROOT;
        case UI_PAGE_BURNER:
            return UI_PAGE_ROOT;
        case UI_PAGE_BURN_ROM:
        case UI_PAGE_BURN_SAVE:
            return UI_PAGE_BURNER;
        default:
            return UI_PAGE_ROOT;
    }
}

static void ui_calc_burn_snapshot_fields(
    const burner_status_t *status,
    int *progress_out,
    int *erase_progress_out,
    uint32_t *processed_out,
    uint32_t *total_out,
    uint32_t *erase_done_out,
    uint32_t *erase_total_out)
{
    int progress = 0;
    int erase_progress = 0;
    uint32_t processed = 0;
    uint32_t total = 0;
    uint32_t erase_done_bytes = 0;
    uint32_t erase_total_bytes = 0;
    uint32_t erase_done = 0;
    uint32_t erase_total = 0;

    if (status != NULL) {
        progress = status->progress;
        processed = status->processed_bytes;
        total = status->total_bytes;
        erase_total_bytes = status->erase_phase_total_bytes;
        erase_done_bytes = status->erase_phase_done_bytes;
        erase_total = status->erase_phase_total_sectors;
        erase_done = status->erase_phase_done_sectors;
    }
    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }
    if (total > 0U && processed > total) {
        processed = total;
    }
    if (erase_total > 0U) {
        if (erase_done > erase_total) {
            erase_done = erase_total;
        }
        erase_progress = burner_calc_progress_percent_u64(erase_done, erase_total);
        if (erase_progress > 100) {
            erase_progress = 100;
        }
    } else if (erase_total_bytes > 0U) {
        if (erase_done_bytes > erase_total_bytes) {
            erase_done_bytes = erase_total_bytes;
        }
        erase_progress = burner_calc_progress_percent_u64(erase_done_bytes, erase_total_bytes);
        if (erase_progress > 100) {
            erase_progress = 100;
        }
    } else if (status != NULL) {
        erase_total = status->erase_sector_count;
        erase_done = status->erase_sector_count;
        if (erase_total > 0U) {
            erase_progress = 100;
        }
    }

    if (progress_out != NULL) {
        *progress_out = progress;
    }
    if (erase_progress_out != NULL) {
        *erase_progress_out = erase_progress;
    }
    if (processed_out != NULL) {
        *processed_out = processed;
    }
    if (total_out != NULL) {
        *total_out = total;
    }
    if (erase_done_out != NULL) {
        *erase_done_out = erase_done;
    }
    if (erase_total_out != NULL) {
        *erase_total_out = erase_total;
    }
}

static void ui_present_active_burn_task_locked(ui_model_t *model, const burner_status_t *status, ui_page_t return_page)
{
    int progress = 0;
    int erase_progress = 0;
    uint32_t processed = 0;
    uint32_t total = 0;
    uint32_t erase_done = 0;
    uint32_t erase_total = 0;

    if (model == NULL || status == NULL) {
        return;
    }

    ui_calc_burn_snapshot_fields(
        status,
        &progress,
        &erase_progress,
        &processed,
        &total,
        &erase_done,
        &erase_total);

    ui_task_cancel_confirm_reset_locked();
    ui_begin_task_result_capture_locked(model);
    if (burner_status_is_operation_active_state(status->state)) {
        s_task_result_active_seen = true;
    }
    model->task_result_status_valid = false;
    model->page = UI_PAGE_TASK_STATUS;
    model->parent_page = return_page;
    model->selected = 0;
    model->scroll = 0;
    model->burn_progress = progress;
    model->burn_processed = processed;
    model->burn_total = total;
    model->erase_progress = erase_progress;
    model->erase_done_sectors = erase_done;
    model->erase_total_sectors = erase_total;
    model->burn_elapsed_us = status->task_elapsed_us;
    ui_set_status_locked(model, status->message[0] != '\0' ? status->message : ui_tr("task status"));
    ui_mark_chrome_dirty(model);
    ui_mark_content_dirty(model);
    model->dirty = true;
}

static void ui_task_cancel_confirm_reset_locked(void)
{
    s_task_cancel_confirm = false;
    s_task_cancel_exit_pending = false;
    s_task_cancel_request_pending = false;
    s_task_cancel_return_page = UI_PAGE_BURNER;
}

static void ui_task_cancel_confirm_open_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_task_cancel_confirm = true;
    s_task_cancel_return_page = model->parent_page;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("confirm stop task"));
}

static void ui_task_cancel_confirm_close_locked(ui_model_t *model, const char *status_key)
{
    s_task_cancel_confirm = false;
    if (model == NULL) {
        return;
    }
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    if (status_key != NULL && status_key[0] != '\0') {
        ui_set_status_locked(model, ui_tr(status_key));
    }
}

static void ui_menu_move_locked(ui_model_t *model, int delta)
{
    uint16_t count;
    int selected;

    if (model == NULL) {
        return;
    }
    if (model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES) {
        ui_file_move_locked(model, delta);
        return;
    }
    if (model->page == UI_PAGE_BURNER) {
        delta = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
    } else if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        uint16_t selected = model->selected;
        uint16_t row_start;
        uint16_t row_len;

        count = ui_page_item_count(model);
        if (count == 0U) {
            return;
        }
        row_start = (uint16_t)((selected / 3U) * 3U);
        row_len = (uint16_t)(((count - row_start) >= 3U) ? 3U : (count - row_start));
        if (delta >= UI_ROW_COUNT) {
            selected = (uint16_t)((selected - row_start + 1U < row_len) ? (selected + 1U) : row_start);
        } else if (delta <= -UI_ROW_COUNT) {
            selected = (uint16_t)((selected == row_start) ? (row_start + row_len - 1U) : (selected - 1U));
        } else if (delta > 0) {
            uint16_t col = (uint16_t)(selected % 3U);
            uint16_t next = (uint16_t)(selected + 3U);

            if (next >= count) {
                next = col;
            }
            if (next >= count) {
                next = (uint16_t)(count - 1U);
            }
            selected = next;
        } else if (delta < 0) {
            uint16_t col = (uint16_t)(selected % 3U);
            uint16_t last_row_start = (uint16_t)(((count - 1U) / 3U) * 3U);
            uint16_t prev = (selected >= 3U) ? (uint16_t)(selected - 3U) : (uint16_t)(last_row_start + col);

            if (prev >= count) {
                prev = (uint16_t)(count - 1U);
            }
            selected = prev;
        } else {
            return;
        }
        if (selected != model->selected) {
            model->selected = selected;
            ui_mark_motion_dirty(model);
        }
        return;
    }

    count = ui_page_item_count(model);
    if (count == 0U || delta == 0) {
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE &&
        !ui_gba_sram_patch_selectable() && count == UI_BURN_ROM_WRITE_PATH_ITEM_COUNT) {
        /* SRAM is not a valid target for this ROM; keep it visible but skip it. */
        if (model->selected == 1U) {
            model->selected = (delta > 0) ? 2U : 0U;
            ui_mark_motion_dirty(model);
            return;
        }
        if (delta > 0) {
            model->selected = (model->selected == 0U) ? 2U :
                              (model->selected == 2U) ? 3U : 0U;
        } else if (delta < 0) {
            model->selected = (model->selected == 0U) ? 3U :
                              (model->selected == 3U) ? 2U : 0U;
        }
        ui_mark_motion_dirty(model);
        return;
    }
    if (ui_page_is_icon_grid(model->page)) {
        ui_icon_page_config_t config = {0};
        uint16_t next_selected = model->selected;

        (void)ui_icon_page_config_for_page(model->page, &config);
        if (delta == -1) {
            next_selected = ui_icon_page_move_selection(model->selected, count, config.cols, config.rows, -1, 0);
        } else if (delta == 1) {
            next_selected = ui_icon_page_move_selection(model->selected, count, config.cols, config.rows, 1, 0);
        } else if (delta == -(int)UI_ROW_COUNT) {
            next_selected = ui_icon_page_move_selection(model->selected, count, config.cols, config.rows, 0, -1);
        } else if (delta == (int)UI_ROW_COUNT) {
            next_selected = ui_icon_page_move_selection(model->selected, count, config.cols, config.rows, 0, 1);
        }
        if (next_selected != model->selected) {
            model->selected = next_selected;
            if (model->page == UI_PAGE_ROOT) {
                s_root_selected = model->selected;
            }
            ui_mark_motion_dirty(model);
        }
        return;
    }
    selected = ((int)model->selected + delta) % (int)count;
    if (selected < 0) {
        selected += (int)count;
    }
    if ((uint16_t)selected != model->selected) {
        uint16_t old_scroll = model->scroll;

        model->selected = (uint16_t)selected;
        if (model->page == UI_PAGE_ROOT) {
            s_root_selected = model->selected;
        }
        if (model->page == UI_PAGE_BURN_ROM) {
            model->scroll = ui_scroll_for_selected_rows(model->selected, model->scroll, count, ui_burn_rom_visible_rows());
        } else if (model->page == UI_PAGE_SMB_TEXT_INPUT) {
            uint16_t visible_rows = (uint16_t)((UI_CANVAS_H - UI_HINT_H - (UI_LIST_HEADER_H + 34)) / UI_LIST_LINE_H);

            if (visible_rows == 0U) {
                visible_rows = 1U;
            }
            model->scroll = ui_scroll_for_selected_rows(model->selected, model->scroll, count, visible_rows);
        } else {
            model->scroll = ui_scroll_for_selected(model->selected, model->scroll, count);
        }
        if (model->scroll == old_scroll) {
            ui_mark_motion_dirty(model);
        } else {
            ui_mark_content_dirty(model);
        }
    }
}

static void ui_back_locked(ui_model_t *model)
{
    char parent[TF_PATH_LEN_MAX] = {0};

    if (model == NULL) {
        return;
    }
    if (ui_settings_adjust_active(model)) {
        ui_settings_close_adjust_locked(model);
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS && s_task_cancel_confirm) {
        ui_task_cancel_confirm_close_locked(model, "task status");
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS && ui_task_status_operation_active()) {
        ui_task_cancel_confirm_open_locked(model);
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS) {
        ui_page_t return_page = model->parent_page;

        model->page = return_page;
        model->parent_page = ui_task_return_parent_page(return_page);
        model->selected = 0;
        model->scroll = 0;
        ui_drop_nav_target_locked(model->page);
        ui_mark_chrome_dirty(model);
        ui_mark_content_dirty(model);
        model->dirty = true;
        return;
    }
    if (model->page == UI_PAGE_TASK_RESULT) {
        ui_page_t return_page = model->parent_page;

        ui_clear_task_result_runtime_locked(model);
        model->page = return_page;
        model->parent_page = ui_task_return_parent_page(return_page);
        model->selected = 0;
        model->scroll = 0;
        ui_drop_nav_target_locked(model->page);
        ui_set_status_locked(model, ui_tr("ready"));
        ui_mark_chrome_dirty(model);
        ui_mark_content_dirty(model);
        model->dirty = true;
        return;
    }
    if (model->page == UI_PAGE_MUSIC_PLAYER) {
        if (s_music_ctx.drawer_open &&
            model->file_path[0] != '\0' &&
            strcmp(model->file_path, UI_MUSIC_DIR) != 0 &&
            ui_file_parent_path(model->file_path, parent, sizeof(parent))) {
            ui_open_files_locked(model, parent, true);
            ui_music_set_drawer_open_locked(model, true);
            return;
        }
        ui_open_root_locked(model);
        return;
    }
    if (model->page == UI_PAGE_READER) {
        ui_reader_save_history_locked(model);
        ui_reader_clear_locked();
        model->page = UI_PAGE_FILES;
        model->parent_page = UI_PAGE_ROOT;
        model->reader_page = 0U;
        ui_file_ensure_window_locked(model, true);
        ui_mark_chrome_dirty(model);
        ui_mark_content_dirty(model);
        model->dirty = true;
        return;
    }
    if (model->page == UI_PAGE_SMB_TEXT_INPUT) {
        model->page = UI_PAGE_SMB;
        model->parent_page = UI_PAGE_WIFI;
        model->selected = 0;
        model->scroll = 0;
        ui_drop_nav_target_locked(model->page);
        ui_mark_chrome_dirty(model);
        ui_mark_content_dirty(model);
        model->dirty = true;
        return;
    }
    if (ui_file_page_is_book_scope(model)) {
        if (model->file_path[0] == '\0') {
            ui_open_root_locked(model);
            return;
        }
        if (!ui_file_parent_path(model->file_path, parent, sizeof(parent))) {
            ui_open_root_locked(model);
            return;
        }
        ui_open_files_locked(model, parent, true);
        return;
    }
    if (model->page == UI_PAGE_FILES && model->file_path[0] != '\0') {
        if (ui_file_parent_path(model->file_path, parent, sizeof(parent))) {
            ui_open_files_locked(model, parent, true);
            return;
        }
    }
    if (model->page == UI_PAGE_MUSIC_FILES && model->file_path[0] != '\0') {
        if (strcmp(model->file_path, UI_MUSIC_DIR) == 0) {
            ui_open_root_locked(model);
            return;
        }
        if (ui_file_parent_path(model->file_path, parent, sizeof(parent))) {
            model->page = UI_PAGE_MUSIC_FILES;
            ui_open_files_with_filter_locked(model, parent, true, UI_FILE_FILTER_AUDIO);
            model->parent_page = UI_PAGE_MUSIC_FILES;
            return;
        }
    }
    if (model->page == UI_PAGE_FILES &&
        (model->parent_page == UI_PAGE_BURN_ROM || model->parent_page == UI_PAGE_BURN_SAVE ||
         model->parent_page == UI_PAGE_BURNER)) {
        model->file_filter = UI_FILE_FILTER_NONE;
        model->page = model->parent_page;
        model->parent_page = UI_PAGE_BURNER;
        model->selected = 0;
        model->scroll = 0;
        s_burn_rom_write_menu = false;
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        s_burn_rom_write_prompt_until_ms = 0;
        s_burn_rom_verify_prompt_until_ms = 0;
        ui_drop_nav_target_locked(model->page);
        model->dirty = true;
        return;
    }
    if (model->page == UI_PAGE_MUSIC_FILES && model->parent_page == UI_PAGE_MUSIC_FILES) {
        ui_open_root_locked(model);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        size_t len = strlen(s_burn_rom_custom_size_text);

        if (len > 0U) {
            s_burn_rom_custom_size_text[len - 1U] = '\0';
            ui_mark_content_dirty(model);
            return;
        }
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_DUMP_SIZE;
        model->selected = 2;
        model->scroll = 0;
        ui_mark_content_dirty(model);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_SIZE) {
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        for (uint16_t i = 0; i < ui_burn_rom_item_count(); ++i) {
            if (ui_burn_rom_op_for_index(i) == UI_BURN_ROM_OP_DUMP_ROM) {
                model->selected = i;
                break;
            }
        }
        model->scroll = ui_scroll_for_selected_rows(model->selected, 0, ui_burn_rom_item_count(), ui_burn_rom_visible_rows());
        ui_mark_content_dirty(model);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RECIPE_MODE) {
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        ui_focus_burn_rom_op_locked(model, UI_BURN_ROM_OP_RECIPE_MODE);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_MAPPER) {
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        ui_focus_burn_rom_op_locked(model, UI_BURN_ROM_OP_ROM_MAPPER);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        for (uint16_t i = 0; i < ui_burn_rom_item_count(); ++i) {
            if (ui_burn_rom_op_for_index(i) == UI_BURN_ROM_OP_SETTINGS) {
                model->selected = i;
                break;
            }
        }
        model->scroll = ui_scroll_for_selected_rows(model->selected, 0, ui_burn_rom_item_count(), ui_burn_rom_visible_rows());
        ui_mark_content_dirty(model);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_ERASE_CONFIRM) {
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        for (uint16_t i = 0; i < ui_burn_rom_item_count(); ++i) {
            if (ui_burn_rom_op_for_index(i) == UI_BURN_ROM_OP_ERASE_CHIP) {
                model->selected = i;
                break;
            }
        }
        model->scroll = ui_scroll_for_selected_rows(model->selected, 0, ui_burn_rom_item_count(), ui_burn_rom_visible_rows());
        ui_mark_content_dirty(model);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RAM) {
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        for (uint16_t i = 0; i < ui_burn_rom_item_count(); ++i) {
            if (ui_burn_rom_op_for_index(i) == UI_BURN_ROM_OP_RAM_MENU) {
                model->selected = i;
                break;
            }
        }
        model->scroll = ui_scroll_for_selected_rows(model->selected, 0, ui_burn_rom_item_count(), ui_burn_rom_visible_rows());
        ui_mark_content_dirty(model);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SAVE_PATCH) {
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_WRITE;
        model->selected = 1U;
        model->scroll = 0U;
        ui_mark_content_dirty(model);
        return;
    }
    if (model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
        s_burn_rom_write_menu = false;
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        for (uint16_t i = 0; i < ui_burn_rom_item_count(); ++i) {
            if (ui_burn_rom_op_for_index(i) == UI_BURN_ROM_OP_WRITE_ROM) {
                model->selected = i;
                break;
            }
        }
        model->scroll = ui_scroll_for_selected_rows(model->selected, 0, ui_burn_rom_item_count(), ui_burn_rom_visible_rows());
        ui_mark_content_dirty(model);
        return;
    }
    if (model->page == UI_PAGE_FILE_ACTIONS) {
        if (model->file_book_scope) {
            model->page = UI_PAGE_FILES;
            model->parent_page = UI_PAGE_ROOT;
            ui_scan_file_window_locked(model);
            model->dirty = true;
            return;
        }
        model->file_filter = UI_FILE_FILTER_NONE;
        model->page = UI_PAGE_FILES;
        model->parent_page = UI_PAGE_TF;
        ui_scan_file_window_locked(model);
        model->dirty = true;
        return;
    }
    if (s_nav_depth > 0U) {
        s_nav_depth--;
        model->page = s_nav_stack[s_nav_depth].page;
        model->parent_page = s_nav_stack[s_nav_depth].parent;
        model->selected = (model->page == UI_PAGE_ROOT) ? s_root_selected : 0;
        model->scroll = 0;
        if (model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES) {
            ui_scan_file_window_locked(model);
        }
        model->dirty = true;
        return;
    }
    ui_open_root_locked(model);
}

static esp_err_t ui_prepare_last_file_action_locked(
    ui_model_t *model,
    ui_file_action_t action,
    ui_file_start_request_t **request_out)
{
    ui_file_start_request_t *request = NULL;
    bool want_save = (action == UI_FILE_ACTION_WRITE_SAVE || action == UI_FILE_ACTION_VERIFY_SAVE ||
                      action == UI_FILE_ACTION_WRITE_GBA_SAVE_NEW || action == UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW);
    ui_file_entry_t selected_file = {0};
    ui_file_kind_t selected_kind = UI_FILE_KIND_UNSUPPORTED;
    bool is_rom;

    if (request_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *request_out = NULL;
    if (model != NULL && model->page == UI_PAGE_BURN_ROM) {
        if (want_save) {
            selected_file = s_last_save_file;
            selected_kind = UI_FILE_KIND_SAVE;
        } else {
            selected_file = *ui_last_rom_file_for_mode(s_cart_mode);
            selected_kind = ui_last_rom_kind_for_mode(s_cart_mode);
        }
    } else if (model != NULL) {
        selected_file = model->action_file;
        selected_kind = model->action_kind;
    }
    is_rom = (selected_kind == UI_FILE_KIND_ROM_GBA || selected_kind == UI_FILE_KIND_ROM_MBC5);
    if (model == NULL || selected_file.path[0] == '\0') {
        ui_set_status_locked(
            model,
            want_save ? ui_tr("select TF save first") : ui_tr("select TF ROM first"));
        return ESP_ERR_INVALID_STATE;
    }
    if ((want_save && selected_kind != UI_FILE_KIND_SAVE) || (!want_save && !is_rom)) {
        ui_set_status_locked(
            model,
            want_save ? ui_tr("selected file is not save") : ui_tr("selected file is not ROM"));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_file_start_active) {
        ui_set_status_locked(model, ui_tr("task starting"));
        return ESP_ERR_INVALID_STATE;
    }
    if (!want_save &&
        model != NULL &&
        model->page == UI_PAGE_BURN_ROM &&
        s_cart_mode == BURNER_CART_MODE_MBC5 &&
        s_gb_mapper_override_kind == BURNER_GB_MAPPER_UNKNOWN) {
        ui_burn_rom_open_mapper_menu_locked(model);
        return ESP_ERR_INVALID_STATE;
    }
    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, ui_tr("TF not available"));
        return ESP_ERR_INVALID_STATE;
    }

    request = (ui_file_start_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        ui_set_status_locked(model, ui_tr("no memory"));
        return ESP_ERR_NO_MEM;
    }

    ui_utf8_safe_copy(request->name, sizeof(request->name), selected_file.name);
    snprintf(request->path, sizeof(request->path), "%s", selected_file.path);
    request->size = selected_file.size;
    request->kind = selected_kind;
    request->action = action;
    request->cart_mode = (model->page == UI_PAGE_BURN_ROM) ? s_cart_mode : ui_file_cart_mode_for_kind(selected_kind);
    request->recipe_mode = s_recipe_mode;
    request->slot = s_cart_slot;
    request->write_path = ui_burn_action_for_write_path(s_write_path) == action ? s_write_path :
                           ((action == UI_FILE_ACTION_BURN_PSRAM) ?
                                BURNER_WRITE_PATH_PSRAM :
                                ((action == UI_FILE_ACTION_BURN_PIPELINE) ?
                                     BURNER_WRITE_PATH_PIPELINE :
                                     BURNER_WRITE_PATH_DIRECT));
    request->erase_always = (s_burn_erase_always != 0u);
    request->psram_mb = s_psram_mb;
    request->mbc5_chunk_kb = s_mbc5_chunk_kb;
    request->gba_force_no_cfi = false;
    request->gba_sram_patch = (request->cart_mode == BURNER_CART_MODE_GBA) ? s_gba_sram_patch : false;
    request->gba_waitcnt_patch = (request->cart_mode == BURNER_CART_MODE_GBA) ? s_gba_waitcnt_patch : false;
    request->gba_batteryless_patch = (request->cart_mode == BURNER_CART_MODE_GBA) ? s_gba_batteryless_patch : false;
    request->gba_save_type = s_gba_save_type;
    request->gba_save_size = selected_file.size;
    request->gbx_profile_file[0] = '\0';
    request->ram_fram = s_ram_fram;
    request->ram_latency = s_ram_latency;
    request->return_page =
        (model != NULL && (model->page == UI_PAGE_BURN_ROM || model->page == UI_PAGE_BURN_SAVE)) ?
            model->page :
            UI_PAGE_ROOT;

    s_file_start_active = true;
    ui_task_cancel_confirm_reset_locked();
    ui_begin_task_result_capture_locked(model);
    model->page = UI_PAGE_TASK_STATUS;
    model->parent_page = request->return_page;
    model->burn_progress = 0;
    model->burn_processed = 0;
    model->burn_total = request->size;
    model->erase_progress = 0;
    model->erase_done_sectors = 0;
    model->erase_total_sectors = 0;
    model->burn_elapsed_us = 0;
    ui_set_status_locked(model, ui_tr("starting burn task"));
    *request_out = request;
    return ESP_OK;
}

#include "burner/ui/ui_burner_menus.inc"

static void ui_select_locked(
    ui_model_t *model,
    ui_file_start_request_t **start_request,
    ui_work_type_t *work_type,
    bool *start_work)
{
    ui_file_entry_t entry;

    if (model == NULL) {
        return;
    }

    switch (model->page) {
        case UI_PAGE_ROOT:
            {
                const ui_menu_item_t *item = ui_home_item_at(model->selected);

                if (item == NULL) {
                    break;
                }
                switch (item->action) {
                    case UI_ACTION_OPEN_SYSTEM:
                        ui_open_page_locked(model, UI_PAGE_SYSTEM);
                        break;
                    case UI_ACTION_OPEN_TF:
                        ui_open_page_locked(model, UI_PAGE_TF);
                        break;
                    case UI_ACTION_OPEN_READER:
                        ui_open_book_library_locked(model, true);
                        break;
                    case UI_ACTION_OPEN_MUSIC:
                        ui_open_music_library_locked(model, true);
                        break;
                    case UI_ACTION_OPEN_WIFI:
                        ui_open_page_locked(model, UI_PAGE_WIFI);
                        break;
                    case UI_ACTION_OPEN_POWER:
                        ui_open_page_locked(model, UI_PAGE_POWER);
                        break;
                    case UI_ACTION_OPEN_BURNER:
                        if (ui_task_status_operation_active()) {
                            burner_status_t status = {0};

                            burner_status_snapshot(&status);
                            ui_present_active_burn_task_locked(model, &status, UI_PAGE_BURNER);
                        } else {
                            ui_open_page_locked(model, UI_PAGE_BURNER);
                        }
                        break;
                    case UI_ACTION_OPEN_SETTINGS:
                        ui_open_page_locked(model, UI_PAGE_SETTINGS);
                        break;
                    case UI_ACTION_OPEN_RETRO_GO:
                        ui_set_status_locked(model, ui_tr("booting Retro-Go"));
                        *work_type = UI_WORK_BOOT_RETRO_GO;
                        *start_work = true;
                        break;
                    default:
                        break;
                }
            }
            break;
        case UI_PAGE_SYSTEM:
            if (model->selected < UI_SYSTEM_ITEM_COUNT) {
                switch (s_system_items[model->selected].action) {
                    case UI_ACTION_OPEN_TF:
                        ui_open_page_locked(model, UI_PAGE_TF);
                        break;
                    case UI_ACTION_OPEN_APP_MANAGER:
                        ui_open_page_locked(model, UI_PAGE_APP_MANAGER);
                        break;
                    case UI_ACTION_OPEN_SETTINGS:
                        ui_open_page_locked(model, UI_PAGE_SETTINGS);
                        break;
                    case UI_ACTION_OPEN_POWER:
                        ui_open_page_locked(model, UI_PAGE_POWER);
                        break;
                    case UI_ACTION_OPEN_WIFI:
                        ui_open_page_locked(model, UI_PAGE_WIFI);
                        break;
                    default:
                        break;
                }
            }
            break;
        case UI_PAGE_APP_MANAGER:
            ui_app_manager_toggle_locked(model);
            break;
        case UI_PAGE_TF:
            if (model->selected == 0U) {
                ui_open_files_with_filter_locked(model, "", true, UI_FILE_FILTER_NONE);
                model->parent_page = UI_PAGE_TF;
            } else if (model->selected == 4U) {
                ui_scan_file_window_locked(model);
            } else if (model->selected == 5U) {
                *work_type = UI_WORK_STORAGE_USB_ENABLE;
                *start_work = true;
            } else if (model->selected == 6U) {
                *work_type = UI_WORK_STORAGE_USB_DISABLE;
                *start_work = true;
            } else {
                ui_set_status_locked(model, ui_tr("use web page for this TF action"));
            }
            break;
        case UI_PAGE_MUSIC_FILES:
            if (!ui_current_file_locked(model, &entry)) {
                ui_set_status_locked(model, ui_tr("no file selected"));
                break;
            }
            if (entry.is_dir) {
                ui_open_files_with_filter_locked(model, entry.path, true, UI_FILE_FILTER_AUDIO);
                break;
            }
            ui_open_music_player_locked(model, &entry);
            break;
        case UI_PAGE_MUSIC_PLAYER:
            (void)ui_music_play_selected_locked(model);
            break;
        case UI_PAGE_FILES:
            if (!ui_current_file_locked(model, &entry)) {
                ui_set_status_locked(model, ui_tr("no file selected"));
                break;
            }
            if (entry.is_dir) {
                ui_open_files_locked(model, entry.path, true);
            } else if (ui_file_page_is_book_scope(model)) {
                if (ui_reader_native_file_supported_name(entry.name)) {
                    (void)ui_open_reader_locked(model, &entry);
                } else {
                    ui_set_status_locked(model, ui_tr("unsupported file"));
                }
            } else {
                if (model->parent_page == UI_PAGE_BURN_ROM || model->parent_page == UI_PAGE_BURN_SAVE ||
                    model->parent_page == UI_PAGE_BURNER) {
                    ui_select_file_for_burner_locked(model, &entry);
                } else {
                    ui_open_file_action_page_locked(model, &entry);
                }
            }
            break;
        case UI_PAGE_READER:
            ui_reader_move_page_locked(model, 1);
            break;
        case UI_PAGE_FILE_ACTIONS:
            (void)ui_prepare_file_action_locked(model, start_request);
            break;
        case UI_PAGE_WIFI:
            if (model->selected == 1U) {
                ui_open_page_locked(model, UI_PAGE_WIFI_QR);
            } else if (model->selected == 2U) {
                ui_smb_open_locked(model);
                *work_type = UI_WORK_SMB_LOAD_FAVORITES;
                *start_work = true;
            } else if (model->selected == 3U) {
                *work_type = UI_WORK_WIFI_CONNECT_SAVED;
                *start_work = true;
            } else if (model->selected == 4U) {
                *work_type = UI_WORK_WIFI_START_AP;
                *start_work = true;
            } else if (model->selected == 5U) {
                *work_type = UI_WORK_WIFI_CLOSE_AP;
                *start_work = true;
            } else if (model->selected == 6U) {
                *work_type = UI_WORK_WIFI_DISCONNECT;
                *start_work = true;
            } else if (model->selected == 7U) {
                *work_type = UI_WORK_WIFI_CLEAR_SAVED;
                *start_work = true;
            } else {
                ui_set_status_locked(model, ui_wifi_state_name(model->wifi_state));
            }
            break;
        case UI_PAGE_SMB:
            ui_smb_ensure_context();
            if (model->selected == 0U) {
                *work_type = UI_WORK_SMB_SCAN;
                *start_work = true;
            } else if (model->selected == 1U) {
                *work_type = UI_WORK_SMB_LOAD_FAVORITES;
                *start_work = true;
            } else if (model->selected == 2U) {
                ui_smb_open_text_input_locked(model, UI_SMB_FIELD_HOST);
            } else if (model->selected == 3U) {
                ui_smb_open_text_input_locked(model, UI_SMB_FIELD_SHARE);
            } else if (model->selected == 4U) {
                ui_smb_open_text_input_locked(model, UI_SMB_FIELD_USER);
            } else if (model->selected == 5U) {
                ui_smb_open_text_input_locked(model, UI_SMB_FIELD_PASSWORD);
            } else if (model->selected == 6U) {
                ui_smb_open_text_input_locked(model, UI_SMB_FIELD_DOMAIN);
            } else if (model->selected == 7U) {
                ui_smb_adjust_port_locked(model, 1);
            } else if (model->selected == 8U) {
                ui_smb_toggle_signing_locked(model);
            } else if (model->selected == 9U) {
                *work_type = UI_WORK_SMB_SAVE_FAVORITE;
                *start_work = true;
            } else if (model->selected == 10U) {
                *work_type = UI_WORK_SMB_DISCONNECT;
                *start_work = true;
            } else if (model->selected >= UI_SMB_BASE_ITEM_COUNT &&
                       model->selected < UI_SMB_BASE_ITEM_COUNT + s_smb_ctx.favorite_count) {
                ui_smb_select_favorite_locked(model, (uint16_t)(model->selected - UI_SMB_BASE_ITEM_COUNT));
                *work_type = UI_WORK_SMB_CONNECT;
                *start_work = true;
            } else if (model->selected >= UI_SMB_BASE_ITEM_COUNT) {
                ui_smb_select_server_locked(
                    model,
                    (uint16_t)(model->selected - UI_SMB_BASE_ITEM_COUNT - s_smb_ctx.favorite_count));
            } else {
                ui_set_status_locked(model, "use web for music folder");
            }
            break;
        case UI_PAGE_SMB_TEXT_INPUT: {
            size_t field_len = 0;
            char *field = ui_smb_field_buffer(s_smb_ctx.editing_field, &field_len);
            char key = ui_smb_input_char_for_index(model->selected);

            if (field == NULL || field_len == 0U) {
                ui_set_status_locked(model, "invalid field");
            } else if (key != '\0') {
                size_t len = strlen(field);

                if (len + 1U < field_len) {
                    field[len] = key;
                    field[len + 1U] = '\0';
                    ui_mark_content_dirty(model);
                } else {
                    ui_set_status_locked(model, "field full");
                }
            } else {
                size_t key_count = strlen(ui_smb_input_charset());
                uint16_t action = (model->selected >= key_count) ? (uint16_t)(model->selected - key_count) : 0U;

                if (action == 0U) {
                    size_t len = strlen(field);

                    if (len > 0U) {
                        field[len - 1U] = '\0';
                        ui_mark_content_dirty(model);
                    }
                } else if (action == 1U) {
                    field[0] = '\0';
                    ui_mark_content_dirty(model);
                } else {
                    ui_back_locked(model);
                }
            }
            break;
        }
        case UI_PAGE_POWER:
            if (model->selected <= 1U) {
                ui_power_adjust_idle_minutes_locked(model, 1);
            } else {
                ui_set_status_locked(model, ui_tr("Power"));
            }
            break;
        case UI_PAGE_BURNER:
            s_cart_mode = (model->selected == 0U) ? BURNER_CART_MODE_GBA : BURNER_CART_MODE_MBC5;
            ui_open_page_locked(model, UI_PAGE_BURN_ROM);
            ui_set_status_locked(model, ui_cart_is_unlocked() ? ui_tr("cart analyzed") : ui_tr("analyze cart first"));
            break;
        case UI_PAGE_BURN_ROM:
            if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SAVE_PATCH) {
                if (model->selected < 3U) {
                    s_gba_save_patch_choice = (model->selected == 0U) ? 1U :
                                              (model->selected == 1U) ? 2U : 0U;
                    s_gba_sram_patch = s_gba_save_patch_choice == 1U;
                    if (s_gba_save_patch_choice != 1U) s_gba_batteryless_patch = false;
                    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_WRITE;
                    model->selected = 1U;
                    model->scroll = 0U;
                    ui_set_status_locked(model, ui_tr("save patch selected"));
                    ui_mark_content_dirty(model);
                }
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
                if (model->selected == 0U) {
                    ui_file_action_t action = ui_burn_action_for_write_path(s_write_path);
                    if (ui_prepare_last_file_action_locked(model, action, start_request) == ESP_OK) {
                        s_burn_rom_write_menu = false;
                        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
                    }
                } else if (model->selected == 1U) {
                    if (!ui_gba_sram_patch_selectable()) {
                        ui_set_status_locked(model, ui_tr("Save patch not needed"));
                        return;
                    }
                    ui_burn_rom_open_save_patch_menu_locked(model);
                } else if (model->selected == 2U) {
                    s_gba_waitcnt_patch = !s_gba_waitcnt_patch;
                    ui_set_status_locked(model, s_gba_waitcnt_patch ? ui_tr("Latency patch: yes") : ui_tr("Latency patch: no"));
                    ui_mark_content_dirty(model);
                } else {
                    if (s_gba_save_patch_choice != 1U) {
                        ui_set_status_locked(model, ui_tr("Batteryless patch disabled"));
                        return;
                    }
                    s_gba_batteryless_patch = !s_gba_batteryless_patch;
                    ui_set_status_locked(model, s_gba_batteryless_patch ? ui_tr("Batteryless patch: yes") : ui_tr("Batteryless patch: no"));
                    ui_mark_content_dirty(model);
                }
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_SIZE) {
                uint32_t dump_size_mib = ui_burn_dump_size_for_index(model->selected);

                if (model->selected == 2U) {
                    ui_burn_rom_open_dump_custom_locked(model);
                } else if (model->selected == UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT && ui_burn_cart_has_slots()) {
                    s_cart_slot = (s_cart_slot >= BURNER_MBC5_SLOT_MAX) ? 0U : (s_cart_slot + 1U);
                    ui_set_status_locked(model, ui_tr("slot changed"));
                } else if (dump_size_mib > 0U) {
                    s_rom_size_mib = dump_size_mib;
                    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
                    *work_type = UI_WORK_BURN_DUMP_ROM;
                    *start_work = true;
                }
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
                char key = ui_burn_dump_key_for_index(model->selected);

                if (key == '\n') {
                    uint32_t custom_mib = 0;

                    if (ui_parse_first_u32_text(s_burn_rom_custom_size_text, &custom_mib)) {
                        s_rom_size_mib = custom_mib;
                        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
                        *work_type = UI_WORK_BURN_DUMP_ROM;
                        *start_work = true;
                    } else {
                        ui_set_status_locked(model, ui_tr("input size first"));
                    }
                } else {
                    size_t len = strlen(s_burn_rom_custom_size_text);

                    if (len + 1U < sizeof(s_burn_rom_custom_size_text)) {
                        s_burn_rom_custom_size_text[len] = key;
                        s_burn_rom_custom_size_text[len + 1U] = '\0';
                        ui_mark_content_dirty(model);
                    }
                }
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RECIPE_MODE) {
                ui_apply_recipe_mode_locked(
                    model,
                    ui_recipe_mode_for_index(model->selected));
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_MAPPER) {
                s_gb_mapper_override_kind =
                    (model->selected == 1U) ? BURNER_GB_MAPPER_MBC3 : BURNER_GB_MAPPER_MBC5;
                s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
                ui_set_status_locked(model, ui_selected_gb_mapper_label());
                ui_focus_burn_rom_op_locked(model, UI_BURN_ROM_OP_WRITE_ROM);
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
                if (model->selected == 0U) {
                    s_write_path = (s_write_path == BURNER_WRITE_PATH_DIRECT) ? BURNER_WRITE_PATH_PSRAM :
                                   ((s_write_path == BURNER_WRITE_PATH_PSRAM) ? BURNER_WRITE_PATH_PIPELINE : BURNER_WRITE_PATH_DIRECT);
                    ui_persist_burn_settings_locked(model);
                    ui_set_status_locked(model, ui_write_path_label());
                } else if (model->selected == 1U) {
                    s_burn_erase_always = (s_burn_erase_always == 0u) ? 1u : 0u;
                    ui_persist_burn_settings_locked(model);
                    ui_set_status_locked(model, s_burn_erase_always != 0u ?
                                                   ui_tr("Erase mode: force erase") :
                                                   ui_tr("Erase mode: smart skip"));
                } else if (model->selected == 2U) {
                    s_bacon_power_settle_ms = ui_next_option_u32(
                        s_power_settle_ms_options,
                        sizeof(s_power_settle_ms_options) / sizeof(s_power_settle_ms_options[0]),
                        s_bacon_power_settle_ms,
                        1);
                    ui_persist_burn_settings_locked(model);
                    ui_set_status_locked(model, ui_tr("Voltage settle changed"));
                } else if (model->selected == 3U) {
                    s_psram_mb = ui_next_option_u32(
                        s_psram_mb_options,
                        sizeof(s_psram_mb_options) / sizeof(s_psram_mb_options[0]),
                        s_psram_mb,
                        1);
                    ui_persist_burn_settings_locked(model);
                    ui_set_status_locked(model, ui_tr("PSRAM window changed"));
                } else if (model->selected == 4U) {
                    s_dump_chunk_kb = ui_next_option_u32(
                        s_dump_chunk_kb_options,
                        sizeof(s_dump_chunk_kb_options) / sizeof(s_dump_chunk_kb_options[0]),
                        s_dump_chunk_kb,
                        1);
                    ui_persist_burn_settings_locked(model);
                    ui_set_status_locked(model, ui_tr("dump chunk changed"));
                } else if (s_cart_mode == BURNER_CART_MODE_MBC5) {
                    s_mbc5_power_5v_enabled = (s_mbc5_power_5v_enabled == 0u) ? 1u : 0u;
                    ui_persist_burn_settings_locked(model);
                    ui_set_status_locked(model, s_mbc5_power_5v_enabled != 0u ?
                                                   ui_tr("GBC voltage: 5V") :
                                                   ui_tr("GBC voltage: 3V3"));
                }
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_ERASE_CONFIRM) {
                s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
                if (model->selected == 1U) {
                    *work_type = UI_WORK_BURN_ERASE_CHIP;
                    *start_work = true;
                } else {
                    ui_set_status_locked(model, ui_tr("chip erase cancelled"));
                }
                for (uint16_t i = 0; i < ui_burn_rom_item_count(); ++i) {
                    if (ui_burn_rom_op_for_index(i) == UI_BURN_ROM_OP_ERASE_CHIP) {
                        model->selected = i;
                        break;
                    }
                }
                model->scroll = ui_scroll_for_selected_rows(model->selected, 0, ui_burn_rom_item_count(), ui_burn_rom_visible_rows());
                ui_mark_content_dirty(model);
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RAM) {
                switch (ui_burn_ram_op_for_index(model->selected)) {
                    case UI_BURN_ROM_OP_CHOOSE_SAVE:
                        ui_open_files_with_filter_locked(model, "", true, UI_FILE_FILTER_SAVE);
                        model->parent_page = UI_PAGE_BURN_ROM;
                        break;
                    case UI_BURN_ROM_OP_WRITE_SAVE:
                        (void)ui_prepare_last_file_action_locked(
                            model,
                            (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_FILE_ACTION_WRITE_GBA_SAVE_NEW : UI_FILE_ACTION_WRITE_SAVE,
                            start_request);
                        break;
                    case UI_BURN_ROM_OP_VERIFY_SAVE:
                        (void)ui_prepare_last_file_action_locked(
                            model,
                            (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW : UI_FILE_ACTION_VERIFY_SAVE,
                            start_request);
                        break;
                    case UI_BURN_ROM_OP_DUMP_SAVE:
                        s_dump_batteryless_requested = false;
                        *work_type = UI_WORK_BURN_DUMP_SAVE;
                        *start_work = true;
                        break;
                    case UI_BURN_ROM_OP_SAVE_SIZE:
                        s_save_size_kib = ui_next_option_u32(
                            s_save_size_kib_options,
                            sizeof(s_save_size_kib_options) / sizeof(s_save_size_kib_options[0]),
                            s_save_size_kib,
                            1);
                        ui_set_status_locked(model, ui_tr("save size changed"));
                        break;
                    case UI_BURN_ROM_OP_RAM_TYPE:
                        s_ram_fram = !s_ram_fram;
                        ui_set_status_locked(model, s_ram_fram ? ui_tr("FRAM") : ui_tr("SRAM"));
                        break;
                    case UI_BURN_ROM_OP_GBA_SAVE_TYPE:
                        s_gba_save_type = (burner_gba_save_type_t)(((uint32_t)s_gba_save_type + 1U) % 4U);
                        ui_set_status_locked(model, ui_gba_save_type_label(s_gba_save_type));
                        break;
                    case UI_BURN_ROM_OP_RAM_LATENCY:
                        if (s_cart_mode == BURNER_CART_MODE_MBC5) {
                            s_ram_latency = (uint8_t)((s_ram_latency >= 255U) ? 0U : (s_ram_latency + 1U));
                        } else {
                            ui_set_status_locked(model, ui_tr("GBA save path does not use FRAM latency"));
                            break;
                        }
                        ui_set_status_locked(model, ui_tr("latency changed"));
                        break;
                    case UI_BURN_ROM_OP_DUMP_BATTERYLESS:
                        if (s_cart_mode == BURNER_CART_MODE_GBA) {
                            s_dump_batteryless_requested = true;
                            *work_type = UI_WORK_BURN_DUMP_SAVE;
                            *start_work = true;
                        } else {
                            ui_set_status_locked(model, ui_tr("GBA only"));
                        }
                        break;
                    case UI_BURN_ROM_OP_ANALYZE:
                    case UI_BURN_ROM_OP_CHOOSE_ROM:
                    case UI_BURN_ROM_OP_ROM_MAPPER:
                    case UI_BURN_ROM_OP_WRITE_ROM:
                    case UI_BURN_ROM_OP_VERIFY_ROM:
                    case UI_BURN_ROM_OP_DUMP_ROM:
                    case UI_BURN_ROM_OP_ERASE_CHIP:
                    case UI_BURN_ROM_OP_RAM_MENU:
                    case UI_BURN_ROM_OP_UNLOCK_PPB:
                    case UI_BURN_ROM_OP_SETTINGS:
                    case UI_BURN_ROM_OP_INVALID:
                    default:
                        break;
                }
            } else {
                switch (ui_burn_rom_op_for_index(model->selected)) {
                    case UI_BURN_ROM_OP_RECIPE_MODE:
                        ui_burn_rom_open_recipe_menu_locked(model);
                        break;
                    case UI_BURN_ROM_OP_ANALYZE:
                        *work_type = UI_WORK_BURN_READ_ID;
                        *start_work = true;
                        break;
                    case UI_BURN_ROM_OP_CHOOSE_ROM:
                    case UI_BURN_ROM_OP_CHOOSE_SAVE:
                        ui_open_files_with_filter_locked(
                            model,
                            "",
                            true,
                            (ui_burn_rom_op_for_index(model->selected) == UI_BURN_ROM_OP_CHOOSE_SAVE) ?
                                UI_FILE_FILTER_SAVE :
                                ui_burner_rom_file_filter());
                        model->parent_page = UI_PAGE_BURN_ROM;
                        break;
                    case UI_BURN_ROM_OP_ROM_MAPPER:
                        ui_burn_rom_open_mapper_menu_locked(model);
                        break;
                    case UI_BURN_ROM_OP_WRITE_ROM:
                        if (ui_last_rom_file_for_mode(s_cart_mode)->path[0] == '\0') {
                            s_burn_rom_write_prompt_until_ms = esp_log_timestamp() + UI_BURN_ROW_PROMPT_MS;
                            ui_mark_content_dirty(model);
                        } else {
                            ui_burn_rom_open_write_menu_locked(model);
                        }
                        break;
                    case UI_BURN_ROM_OP_VERIFY_ROM:
                        if (ui_last_rom_file_for_mode(s_cart_mode)->path[0] == '\0') {
                            s_burn_rom_verify_prompt_until_ms = esp_log_timestamp() + UI_BURN_ROW_PROMPT_MS;
                            ui_mark_content_dirty(model);
                        } else {
                            (void)ui_prepare_last_file_action_locked(model, UI_FILE_ACTION_VERIFY_ROM, start_request);
                        }
                        break;
                    case UI_BURN_ROM_OP_DUMP_ROM:
                        ui_burn_rom_open_dump_size_menu_locked(model);
                        break;
                    case UI_BURN_ROM_OP_ERASE_CHIP:
                        ui_burn_rom_open_erase_confirm_locked(model);
                        break;
                    case UI_BURN_ROM_OP_UNLOCK_PPB:
                        *work_type = UI_WORK_BURN_UNLOCK_PPB;
                        *start_work = true;
                        break;
                    case UI_BURN_ROM_OP_RAM_MENU:
                        ui_burn_rom_open_ram_menu_locked(model);
                        break;
                    case UI_BURN_ROM_OP_WRITE_SAVE:
                        (void)ui_prepare_last_file_action_locked(
                            model,
                            (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_FILE_ACTION_WRITE_GBA_SAVE_NEW : UI_FILE_ACTION_WRITE_SAVE,
                            start_request);
                        break;
                    case UI_BURN_ROM_OP_VERIFY_SAVE:
                        (void)ui_prepare_last_file_action_locked(
                            model,
                            (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW : UI_FILE_ACTION_VERIFY_SAVE,
                            start_request);
                        break;
                    case UI_BURN_ROM_OP_DUMP_SAVE:
                        s_dump_batteryless_requested = false;
                        *work_type = UI_WORK_BURN_DUMP_SAVE;
                        *start_work = true;
                        break;
                    case UI_BURN_ROM_OP_SAVE_SIZE:
                        s_save_size_kib = ui_next_option_u32(
                            s_save_size_kib_options,
                            sizeof(s_save_size_kib_options) / sizeof(s_save_size_kib_options[0]),
                            s_save_size_kib,
                            1);
                        ui_set_status_locked(model, ui_tr("save size changed"));
                        break;
                    case UI_BURN_ROM_OP_SETTINGS:
                        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_SETTINGS;
                        model->selected = 0;
                        model->scroll = 0;
                        ui_mark_content_dirty(model);
                        break;
                    case UI_BURN_ROM_OP_INVALID:
                    default:
                        ui_set_status_locked(model, ui_tr("analyze cart first"));
                        break;
                }
            }
            break;
        case UI_PAGE_BURN_SAVE:
            if (model->selected == 0U) {
                ui_open_files_with_filter_locked(model, "", true, UI_FILE_FILTER_SAVE);
                model->parent_page = UI_PAGE_BURN_SAVE;
            } else if (model->selected == 1U) {
                (void)ui_prepare_last_file_action_locked(
                    model,
                    (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_FILE_ACTION_WRITE_GBA_SAVE_NEW : UI_FILE_ACTION_WRITE_SAVE,
                    start_request);
            } else if (model->selected == 2U) {
                (void)ui_prepare_last_file_action_locked(
                    model,
                    (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW : UI_FILE_ACTION_VERIFY_SAVE,
                    start_request);
            } else if (model->selected == 3U) {
                *work_type = UI_WORK_BURN_DUMP_SAVE;
                *start_work = true;
            } else if (model->selected == 4U) {
                s_save_size_kib = ui_next_option_u32(s_save_size_kib_options, sizeof(s_save_size_kib_options) / sizeof(s_save_size_kib_options[0]), s_save_size_kib, 1);
                ui_set_status_locked(model, ui_tr("save size changed"));
            } else if (model->selected == 5U) {
                if (s_cart_mode == BURNER_CART_MODE_GBA) {
                    s_gba_save_type = (burner_gba_save_type_t)(((uint32_t)s_gba_save_type + 1U) % 4U);
                    ui_set_status_locked(model, ui_gba_save_type_label(s_gba_save_type));
                } else {
                    s_ram_fram = !s_ram_fram;
                    ui_set_status_locked(model, s_ram_fram ? ui_tr("FRAM") : ui_tr("SRAM"));
                }
            } else if (model->selected == 6U) {
                if (s_cart_mode == BURNER_CART_MODE_GBA) {
                    ui_set_status_locked(model, ui_tr("GBA save path does not use FRAM latency"));
                } else {
                    s_ram_latency = (uint8_t)((s_ram_latency >= 255U) ? 0U : (s_ram_latency + 1U));
                }
                ui_set_status_locked(model, ui_tr("latency changed"));
            } else if (model->selected == 7U) {
                s_cart_slot = (s_cart_slot >= BURNER_MBC5_SLOT_MAX) ? 0U : (s_cart_slot + 1U);
                ui_set_status_locked(model, ui_tr("slot changed"));
            }
            break;
        case UI_PAGE_SETTINGS:
            if (model->selected == 0U) {
                ui_settings_open_adjust_locked(model, UI_SETTING_ADJUST_BRIGHTNESS);
            } else if (model->selected == 1U) {
                ui_settings_open_adjust_locked(model, UI_SETTING_ADJUST_VOLUME);
            } else if (model->selected == 2U) {
                *work_type = UI_WORK_DEVICE_RESTART;
                *start_work = true;
            } else if (model->selected == 3U) {
                ui_open_page_locked(model, UI_PAGE_TF);
            } else if (model->selected == 4U) {
                ui_open_page_locked(model, UI_PAGE_WIFI);
            } else if (model->selected == 5U) {
                *work_type = UI_WORK_UPDATE_GBX_CACHE;
                *start_work = true;
            } else if (model->selected == 6U) {
                uint8_t language = (s_ui_language == UI_LANGUAGE_ZH) ? UI_LANGUAGE_EN : UI_LANGUAGE_ZH;

                ui_set_language(language);
                if (mori_save_language_settings_to_system_ini(
                        language == UI_LANGUAGE_EN ? MORI_SYSTEM_LANGUAGE_EN_INI : MORI_SYSTEM_LANGUAGE_ZH_INI,
                        language) == ESP_OK) {
                    ui_set_status_locked(model, ui_tr("settings saved"));
                } else {
                    ui_set_status_locked(model, ui_tr("save settings failed"));
                }
                ui_mark_content_dirty(model);
            } else if (model->selected == 9U) {
                ui_open_page_locked(model, UI_PAGE_TASK_STATUS);
            } else {
                ui_set_status_locked(model, ui_tr("use web page for this setting"));
            }
            break;
        case UI_PAGE_TASK_STATUS:
            if (s_task_cancel_confirm) {
                if (model->selected == 1U) {
                    ui_page_t return_page = s_task_cancel_return_page;

                    s_task_cancel_confirm = false;
                    s_task_cancel_exit_pending = true;
                    s_task_cancel_request_pending = true;
                    model->selected = 0;
                    model->scroll = 0;
                    ui_mark_content_dirty(model);
                    ui_set_status_locked(model, ui_tr("cancelling task"));
                    model->page = return_page;
                    model->parent_page = ui_task_return_parent_page(return_page);
                    model->selected = 0;
                    model->scroll = 0;
                    ui_mark_chrome_dirty(model);
                    ui_mark_content_dirty(model);
                    model->dirty = true;
                } else {
                    ui_task_cancel_confirm_close_locked(model, "task status");
                }
            } else {
                ui_set_status_locked(model, ui_tr("task status"));
            }
            break;
        case UI_PAGE_TASK_RESULT:
            ui_set_status_locked(model, ui_tr("result summary"));
            break;
        default:
            ui_set_status_locked(model, ui_tr("read only"));
            break;
    }
}

static void ui_refresh_current_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    if (model->page == UI_PAGE_READER && s_reader_ctx.active) {
        ui_file_entry_t entry = {0};
        uint32_t page_before = model->reader_page;

        ui_reader_save_history_locked(model);
        snprintf(entry.name, sizeof(entry.name), "%s", s_reader_ctx.name);
        snprintf(entry.path, sizeof(entry.path), "%s", s_reader_ctx.path);
        entry.size = s_reader_ctx.file_size;
        if (ui_open_reader_locked(model, &entry) && s_reader_ctx.page_count > 0U) {
            if (page_before >= s_reader_ctx.page_count) {
                page_before = s_reader_ctx.page_count - 1U;
            }
            if (ui_reader_ensure_page_loaded_locked(page_before)) {
                model->reader_page = page_before;
            } else {
                ui_set_status_locked(model, ui_tr("no memory"));
            }
            ui_reader_sync_status_locked(model);
        }
    } else if (model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES || model->page == UI_PAGE_MUSIC_PLAYER) {
        ui_scan_file_window_locked(model);
    } else if (model->page == UI_PAGE_APP_MANAGER) {
        esp_err_t err = app_config_load(&s_app_config);

        ui_home_rebuild_items();
        ui_set_status_locked(model, err == ESP_OK ? ui_tr("apps reloaded") : ui_tr("apps config error"));
        ui_mark_content_dirty(model);
        ui_mark_chrome_dirty(model);
    } else if (model->page == UI_PAGE_ROOT) {
        model->dirty = true;
    } else {
        ui_set_status_locked(model, ui_tr("refreshed"));
    }
    model->dirty = true;
}

#include "burner/ui/ui_burner_live.inc"

static void ui_handle_page_button_action_locked(
    ui_model_t *model,
    ui_input_action_t action,
    ui_file_start_request_t **start_request,
    ui_work_type_t *work_type,
    bool *start_work)
{
    if (model == NULL) {
        return;
    }

    if (model->page == UI_PAGE_READER) {
        switch (action) {
            case UI_INPUT_ACTION_LEFT:
            case UI_INPUT_ACTION_UP:
                ui_reader_move_page_locked(model, -1);
                break;
            case UI_INPUT_ACTION_RIGHT:
            case UI_INPUT_ACTION_DOWN:
            case UI_INPUT_ACTION_SELECT:
            case UI_INPUT_ACTION_PANEL_TOGGLE:
                ui_reader_move_page_locked(model, 1);
                break;
            case UI_INPUT_ACTION_BACK:
                ui_back_locked(model);
                break;
            case UI_INPUT_ACTION_MENU:
                ui_refresh_current_locked(model);
                break;
            default:
                break;
        }
        return;
    }

    if (model->page == UI_PAGE_SMB_TEXT_INPUT) {
        switch (action) {
            case UI_INPUT_ACTION_UP:
                ui_menu_move_locked(model, -1);
                break;
            case UI_INPUT_ACTION_DOWN:
                ui_menu_move_locked(model, 1);
                break;
            case UI_INPUT_ACTION_LEFT:
                ui_menu_move_locked(model, -UI_ROW_COUNT);
                break;
            case UI_INPUT_ACTION_RIGHT:
                ui_menu_move_locked(model, UI_ROW_COUNT);
                break;
            case UI_INPUT_ACTION_SELECT:
            case UI_INPUT_ACTION_PANEL_TOGGLE:
                ui_select_locked(model, start_request, work_type, start_work);
                break;
            case UI_INPUT_ACTION_BACK: {
                size_t field_len = 0;
                char *field = ui_smb_field_buffer(s_smb_ctx.editing_field, &field_len);
                size_t len = field != NULL ? strlen(field) : 0U;

                (void)field_len;
                if (field != NULL && len > 0U) {
                    field[len - 1U] = '\0';
                    ui_mark_content_dirty(model);
                } else {
                    ui_back_locked(model);
                }
                break;
            }
            case UI_INPUT_ACTION_MENU:
                ui_back_locked(model);
                break;
            default:
                break;
        }
        return;
    }

    if (model->page == UI_PAGE_APP_MANAGER) {
        switch (action) {
            case UI_INPUT_ACTION_UP:
                ui_menu_move_locked(model, -1);
                break;
            case UI_INPUT_ACTION_DOWN:
                ui_menu_move_locked(model, 1);
                break;
            case UI_INPUT_ACTION_LEFT:
                ui_app_manager_move_locked(model, -1);
                break;
            case UI_INPUT_ACTION_RIGHT:
                ui_app_manager_move_locked(model, 1);
                break;
            case UI_INPUT_ACTION_SELECT:
            case UI_INPUT_ACTION_PANEL_TOGGLE:
                ui_app_manager_toggle_locked(model);
                break;
            case UI_INPUT_ACTION_BACK:
                ui_back_locked(model);
                break;
            case UI_INPUT_ACTION_MENU:
                ui_refresh_current_locked(model);
                break;
            default:
                break;
        }
        return;
    }

    if (model->page == UI_PAGE_TASK_RESULT) {
        switch (action) {
            case UI_INPUT_ACTION_UP:
                ui_menu_move_locked(model, -1);
                break;
            case UI_INPUT_ACTION_DOWN:
                ui_menu_move_locked(model, 1);
                break;
            case UI_INPUT_ACTION_LEFT:
                ui_menu_move_locked(model, -UI_ROW_COUNT);
                break;
            case UI_INPUT_ACTION_RIGHT:
                ui_menu_move_locked(model, UI_ROW_COUNT);
                break;
            case UI_INPUT_ACTION_BACK:
                ui_back_locked(model);
                break;
            default:
                break;
        }
        return;
    }

    if (model->page == UI_PAGE_MUSIC_PLAYER) {
        switch (action) {
            case UI_INPUT_ACTION_UP:
                if (s_music_ctx.drawer_open) {
                    ui_file_move_locked(model, -1);
                }
                break;
            case UI_INPUT_ACTION_DOWN:
                if (s_music_ctx.drawer_open) {
                    ui_file_move_locked(model, 1);
                }
                break;
            case UI_INPUT_ACTION_LEFT:
                if (ui_action_hold_started(UI_INPUT_ACTION_LEFT)) {
                    (void)ui_music_seek_relative_locked(model, -UI_MUSIC_SEEK_DELTA_BYTES);
                } else {
                    ui_music_play_adjacent_locked(model, -1);
                }
                break;
            case UI_INPUT_ACTION_RIGHT:
                if (ui_action_hold_started(UI_INPUT_ACTION_RIGHT)) {
                    (void)ui_music_seek_relative_locked(model, UI_MUSIC_SEEK_DELTA_BYTES);
                } else {
                    ui_music_play_adjacent_locked(model, 1);
                }
                break;
            case UI_INPUT_ACTION_SELECT:
            case UI_INPUT_ACTION_PANEL_TOGGLE:
                ui_music_request_toggle_locked(model);
                break;
            case UI_INPUT_ACTION_MENU:
                ui_music_set_drawer_open_locked(model, !s_music_ctx.drawer_open);
                break;
            case UI_INPUT_ACTION_BACK:
                ui_back_locked(model);
                break;
            default:
                break;
        }
        return;
    }

    if (ui_settings_adjust_active(model)) {
        switch (action) {
            case UI_INPUT_ACTION_LEFT:
                ui_settings_adjust_value_locked(model, -1, 1U);
                break;
            case UI_INPUT_ACTION_RIGHT:
                ui_settings_adjust_value_locked(model, 1, 1U);
                break;
            case UI_INPUT_ACTION_SELECT:
            case UI_INPUT_ACTION_PANEL_TOGGLE:
            case UI_INPUT_ACTION_BACK:
                ui_settings_close_adjust_locked(model);
                break;
            case UI_INPUT_ACTION_MENU:
                ui_settings_save_av_status_locked(model);
                ui_mark_content_dirty(model);
                break;
            default:
                break;
        }
        return;
    }

    switch (action) {
        case UI_INPUT_ACTION_UP:
            ui_menu_move_locked(model, ui_page_is_icon_grid(model->page) ? -UI_ROW_COUNT : -1);
            break;
        case UI_INPUT_ACTION_DOWN:
            ui_menu_move_locked(model, ui_page_is_icon_grid(model->page) ? UI_ROW_COUNT : 1);
            break;
        case UI_INPUT_ACTION_LEFT:
            if (model->page == UI_PAGE_SMB && model->selected == 7U) {
                ui_smb_adjust_port_locked(model, -1);
            } else if (model->page == UI_PAGE_POWER && model->selected <= 1U) {
                ui_power_adjust_idle_minutes_locked(model, -1);
            } else {
                ui_menu_move_locked(model, ui_page_is_icon_grid(model->page) ? -1 : -UI_ROW_COUNT);
            }
            break;
        case UI_INPUT_ACTION_RIGHT:
            if (model->page == UI_PAGE_SMB && model->selected == 7U) {
                ui_smb_adjust_port_locked(model, 1);
            } else if (model->page == UI_PAGE_POWER && model->selected <= 1U) {
                ui_power_adjust_idle_minutes_locked(model, 1);
            } else {
                ui_menu_move_locked(model, ui_page_is_icon_grid(model->page) ? 1 : UI_ROW_COUNT);
            }
            break;
        case UI_INPUT_ACTION_SELECT:
            ui_select_locked(model, start_request, work_type, start_work);
            break;
        case UI_INPUT_ACTION_PANEL_TOGGLE:
            if (model->page == UI_PAGE_BURN_ROM) {
                s_burner_info_left = !s_burner_info_left;
                model->dirty = true;
                ui_set_status_locked(
                    model,
                    s_burner_info_left ? ui_tr("info panel left") : ui_tr("info panel right"));
            } else {
                ui_select_locked(model, start_request, work_type, start_work);
            }
            break;
        case UI_INPUT_ACTION_BACK:
            ui_back_locked(model);
            break;
        case UI_INPUT_ACTION_MENU:
            if (model->page == UI_PAGE_SMB) {
                *work_type = UI_WORK_SMB_SCAN;
                *start_work = true;
            } else {
                ui_refresh_current_locked(model);
            }
            break;
        default:
            break;
    }
}

static void ui_handle_button_action(ui_input_action_t action)
{
    ui_file_start_request_t *start_request = NULL;
    ui_work_type_t work_type = UI_WORK_WIFI_CONNECT_SAVED;
    bool start_work = false;

    if (action <= UI_INPUT_ACTION_NONE || action > UI_INPUT_ACTION_VOLUME_DOWN) {
        return;
    }
    if (!ui_take_model_lock()) {
        return;
    }
    if (!ui_handle_global_button_action_locked(&s_model, action)) {
        ui_handle_page_button_action_locked(&s_model, action, &start_request, &work_type, &start_work);
    }

    xSemaphoreGive(s_model_lock);

    if (start_request != NULL) {
        ui_start_file_action_async(start_request);
    }
    if (start_work) {
        ui_start_work_async(work_type);
    }
}

static void ui_handle_button_event(ui_button_t button, ui_input_action_t action, bool pressed)
{
    ui_button_state_t *state = NULL;
    bool defer_music_lr_press = false;
    ui_input_action_t effective_action = action;

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_VOL_DOWN) {
        return;
    }
    state = &s_button_states[(uint8_t)button];
    if (!pressed && state->action != UI_INPUT_ACTION_NONE) {
        effective_action = state->action;
    }
    if (state->pressed == pressed) {
        return;
    }

    state->pressed = pressed;
    state->action = effective_action;
    if (!pressed) {
        if ((state->action == UI_INPUT_ACTION_LEFT || state->action == UI_INPUT_ACTION_RIGHT) &&
            ui_take_model_lock()) {
            bool music_page = (s_model.page == UI_PAGE_MUSIC_PLAYER);
            xSemaphoreGive(s_model_lock);
            if (music_page && !state->hold_action_started) {
                ui_handle_button_action(state->action);
            }
        }
        state->frames_until_repeat = 0;
        state->repeat_count = 0;
        state->pressed_since_ms = 0;
        state->last_repeat_ms = 0;
        state->hold_action_started = false;
        return;
    }

    state->frames_until_repeat = UI_BUTTON_REPEAT_START_FRAMES;
    state->repeat_count = 0;
    state->pressed_since_ms = esp_log_timestamp();
    state->last_repeat_ms = state->pressed_since_ms;
    state->hold_action_started = false;

    if ((state->action == UI_INPUT_ACTION_LEFT || state->action == UI_INPUT_ACTION_RIGHT) &&
        ui_take_model_lock()) {
        defer_music_lr_press = (s_model.page == UI_PAGE_MUSIC_PLAYER);
        xSemaphoreGive(s_model_lock);
    }
    if (defer_music_lr_press) {
        return;
    }
    ui_handle_button_action(state->action);
}

static void ui_clear_stuck_button_state(ui_button_t button)
{
    ui_button_state_t *state = NULL;

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_VOL_DOWN) {
        return;
    }
    state = &s_button_states[(uint8_t)button];
    state->pressed = false;
    state->frames_until_repeat = 0;
    state->repeat_count = 0;
    state->pressed_since_ms = 0;
    state->last_repeat_ms = 0;
    state->hold_action_started = false;
    state->action = UI_INPUT_ACTION_NONE;
}

static void ui_process_button_repeats(void)
{
    bool settings_adjusting = false;

    if (ui_take_model_lock()) {
        settings_adjusting = ui_settings_adjust_active(&s_model);
        xSemaphoreGive(s_model_lock);
    }

    for (uint8_t i = 0; i < UI_BUTTON_COUNT; ++i) {
        ui_button_state_t *state = &s_button_states[i];
        ui_input_action_t action = state->action;

        if (!state->pressed || !ui_action_is_repeatable(action)) {
            continue;
        }
        if ((action == UI_INPUT_ACTION_LEFT || action == UI_INPUT_ACTION_RIGHT) &&
            ui_take_model_lock()) {
            bool music_page = (s_model.page == UI_PAGE_MUSIC_PLAYER);
            xSemaphoreGive(s_model_lock);
            if (music_page) {
                uint32_t now_ms = esp_log_timestamp();
                if (!state->hold_action_started) {
                    if ((now_ms - state->pressed_since_ms) < UI_MUSIC_SEEK_HOLD_MS) {
                        continue;
                    }
                    state->hold_action_started = true;
                    state->repeat_count = 1U;
                    state->last_repeat_ms = now_ms;
                    ui_handle_button_action(action);
                    continue;
                }
                if ((now_ms - state->last_repeat_ms) < UI_SETTING_HOLD_REPEAT_INTERVAL_MS) {
                    continue;
                }
                state->last_repeat_ms = now_ms;
                state->repeat_count++;
                ui_handle_button_action(action);
                continue;
            }
        }
        if (settings_adjusting && (action == UI_INPUT_ACTION_LEFT || action == UI_INPUT_ACTION_RIGHT)) {
            uint32_t now_ms = esp_log_timestamp();
            uint32_t held_ms = now_ms - state->pressed_since_ms;
            uint8_t step = (held_ms >= UI_SETTING_HOLD_FAST_MS) ? 10U : 1U;

            if (held_ms < UI_SETTING_HOLD_REPEAT_MS ||
                (now_ms - state->last_repeat_ms) < UI_SETTING_HOLD_REPEAT_INTERVAL_MS) {
                continue;
            }
            state->last_repeat_ms = now_ms;
            state->repeat_count++;
            if (ui_take_model_lock()) {
                ui_settings_adjust_value_locked(&s_model, action == UI_INPUT_ACTION_RIGHT ? 1 : -1, step);
                xSemaphoreGive(s_model_lock);
            }
            continue;
        }
        if (state->frames_until_repeat > 0U) {
            state->frames_until_repeat--;
            continue;
        }

        ui_handle_button_action(action);
        state->repeat_count++;
        state->frames_until_repeat = ui_action_repeat_interval_frames(action, state->repeat_count);
    }
}

static void ui_process_button_queue(void)
{
    ui_button_event_t event;
    int processed = 0;

    if (s_button_queue == NULL) {
        return;
    }

    while (processed < UI_BUTTONS_PER_FRAME && xQueueReceive(s_button_queue, &event, 0) == pdTRUE) {
        ui_handle_button_event(event.button, event.action, event.pressed);
        processed++;
    }
}

static void ui_update_clock_if_needed(uint32_t now_ms)
{
    time_t now = 0;
    struct tm local_tm = {0};
    char time_text[UI_TIME_TEXT_MAX_LEN] = "--:--";

    if (s_last_clock_refresh_ms != 0U && (now_ms - s_last_clock_refresh_ms) < UI_CLOCK_REFRESH_MS) {
        return;
    }
    s_last_clock_refresh_ms = now_ms;

    now = time(NULL);
    if (now > 1704067200 && localtime_r(&now, &local_tm) != NULL) {
        snprintf(time_text, sizeof(time_text), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
    }

    if (!ui_take_model_lock()) {
        return;
    }
    if (strcmp(s_model.time_text, time_text) != 0) {
        snprintf(s_model.time_text, sizeof(s_model.time_text), "%s", time_text);
        ui_mark_chrome_dirty(&s_model);
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_update_fps_if_needed(uint32_t now_ms)
{
    char fps_text[UI_FPS_TEXT_MAX_LEN] = {0};
    uint32_t elapsed_ms;
    uint32_t fps;

    if (s_last_fps_refresh_ms == 0U) {
        s_last_fps_refresh_ms = now_ms;
        return;
    }
    elapsed_ms = now_ms - s_last_fps_refresh_ms;
    if (elapsed_ms < UI_FPS_REFRESH_MS) {
        return;
    }

    fps = (s_render_frames_this_second * 1000U + (elapsed_ms / 2U)) / elapsed_ms;
    s_render_frames_this_second = 0;
    s_last_fps_refresh_ms = now_ms;
    snprintf(fps_text, sizeof(fps_text), "FPS %02u", (unsigned)fps);

    if (!ui_take_model_lock()) {
        return;
    }
    if (strcmp(s_model.fps_text, fps_text) != 0) {
        snprintf(s_model.fps_text, sizeof(s_model.fps_text), "%s", fps_text);
        ui_mark_chrome_dirty(&s_model);
    }
    xSemaphoreGive(s_model_lock);
}

static bool ui_power_adc_supported(const ui_model_t *model)
{
    return model != NULL && model->power_valid && model->power_telemetry.chip_type == POWER_CHIP_AXP209;
}

static void ui_format_power_voltage(uint32_t millivolts, bool supported, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!supported) {
        snprintf(out, out_len, "--");
        return;
    }
    snprintf(out, out_len, "%" PRIu32 "mV", millivolts);
}

static void ui_format_power_current(uint32_t milliamps_x10, bool supported, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!supported) {
        snprintf(out, out_len, "--");
        return;
    }
    if ((milliamps_x10 % 10U) == 0U) {
        snprintf(out, out_len, "%" PRIu32 "mA", milliamps_x10 / 10U);
        return;
    }
    snprintf(out, out_len, "%" PRIu32 ".%" PRIu32 "mA", milliamps_x10 / 10U, milliamps_x10 % 10U);
}

static void ui_format_power_temp(int32_t deci_celsius, bool supported, char *out, size_t out_len)
{
    uint32_t abs_value;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!supported) {
        snprintf(out, out_len, "--");
        return;
    }

    abs_value = (deci_celsius < 0) ? (uint32_t)(-deci_celsius) : (uint32_t)deci_celsius;
    snprintf(
        out,
        out_len,
        "%s%" PRIu32 ".%" PRIu32 "C",
        (deci_celsius < 0) ? "-" : "",
        abs_value / 10U,
        abs_value % 10U);
}

static const char *ui_power_state_label(const char *state)
{
    if (state == NULL || state[0] == '\0' || strcmp(state, "unknown") == 0) {
        return "--";
    }
    if (strcmp(state, "charging") == 0) {
        return ui_lang_is_zh() ? "充电中" : "charging";
    }
    if (strcmp(state, "discharging") == 0) {
        return ui_lang_is_zh() ? "放电中" : "dischg";
    }
    if (strcmp(state, "charge_full") == 0) {
        return ui_lang_is_zh() ? "已满" : "full";
    }
    if (strcmp(state, "discharging_light_load") == 0) {
        return ui_lang_is_zh() ? "轻载" : "light";
    }
    if (strcmp(state, "no_battery_external_power") == 0) {
        return ui_lang_is_zh() ? "外部供电" : "ext only";
    }
    return state;
}

static const char *ui_power_direction_label(const char *direction)
{
    if (direction == NULL || direction[0] == '\0' || strcmp(direction, "unknown") == 0) {
        return "--";
    }
    if (strcmp(direction, "charge") == 0) {
        return ui_lang_is_zh() ? "充电" : "chg";
    }
    if (strcmp(direction, "discharge") == 0) {
        return ui_lang_is_zh() ? "放电" : "dis";
    }
    if (strcmp(direction, "external") == 0) {
        return ui_lang_is_zh() ? "外供" : "ext";
    }
    return direction;
}

static const char *ui_power_mode_label(const char *mode)
{
    if (mode == NULL || mode[0] == '\0' || strcmp(mode, "unknown") == 0) {
        return "--";
    }
    if (strcmp(mode, "charging") == 0) {
        return ui_lang_is_zh() ? "充电" : "charging";
    }
    if (strcmp(mode, "charging_limited") == 0) {
        return ui_lang_is_zh() ? "限流充电" : "chg lim";
    }
    if (strcmp(mode, "charge_disabled") == 0) {
        return ui_lang_is_zh() ? "已禁用" : "disabled";
    }
    if (strcmp(mode, "charge_ready") == 0) {
        return ui_lang_is_zh() ? "待充电" : "ready";
    }
    if (strcmp(mode, "battery_only") == 0) {
        return ui_lang_is_zh() ? "电池供电" : "battery";
    }
    if (strcmp(mode, "external_only") == 0) {
        return ui_lang_is_zh() ? "外部供电" : "ext only";
    }
    if (strcmp(mode, "no_battery") == 0) {
        return ui_lang_is_zh() ? "无电池" : "no batt";
    }
    return mode;
}

static void ui_update_power_if_needed(uint32_t now_ms)
{
    uint8_t percent = 0;
    bool valid = false;
    bool charging = false;
    bool power_valid = false;
    power_manager_telemetry_t telemetry = {0};
    bool battery_changed;
    bool power_changed;

    if (s_last_battery_refresh_ms != 0U && (now_ms - s_last_battery_refresh_ms) < UI_BATTERY_REFRESH_MS) {
        return;
    }
    s_last_battery_refresh_ms = now_ms;

    if (power_manager_get_telemetry(&telemetry) == ESP_OK) {
        power_valid = true;
        percent = telemetry.battery_percent;
        valid = telemetry.battery_percent_valid;
        charging = telemetry.charging;
    }

    if (!ui_take_model_lock()) {
        return;
    }
    battery_changed = (s_model.battery_valid != valid || s_model.battery_percent != percent ||
        s_model.battery_charging != charging);
    power_changed = (s_model.power_valid != power_valid ||
        memcmp(&s_model.power_telemetry, &telemetry, sizeof(telemetry)) != 0);
    if (battery_changed || power_changed) {
        s_model.battery_valid = valid;
        s_model.battery_percent = percent;
        s_model.battery_charging = charging;
        s_model.power_valid = power_valid;
        s_model.power_telemetry = telemetry;
        if (battery_changed) {
            ui_mark_chrome_dirty(&s_model);
        }
        if (s_model.page == UI_PAGE_POWER) {
            ui_mark_content_dirty(&s_model);
        }
    }
    xSemaphoreGive(s_model_lock);
}

#include "burner/ui/ui_burner_snapshot.inc"

static void ui_issue_pending_task_cancel(void)
{
    bool pending = false;

    if (!ui_take_model_lock()) {
        return;
    }
    pending = s_task_cancel_request_pending;
    xSemaphoreGive(s_model_lock);

    if (!pending) {
        return;
    }

    (void)burner_cancel_request();

    if (!ui_take_model_lock()) {
        return;
    }
    s_task_cancel_request_pending = false;
    xSemaphoreGive(s_model_lock);
}

static void ui_refresh_sources(void)
{
    uint32_t now_ms = esp_log_timestamp();
    music_player_snapshot_t snapshot = {0};
    bool music_player_changed = false;
    bool music_progress_changed = false;

    ui_sync_burn_settings_from_runtime();
    ui_update_clock_if_needed(now_ms);
    ui_update_fps_if_needed(now_ms);
    ui_update_power_if_needed(now_ms);
    ui_update_burn_snapshot_if_needed(now_ms);
    music_player_get_snapshot(&snapshot);
    ui_music_save_history_snapshot(&snapshot);
    if (!s_music_snapshot_cached) {
        music_player_changed = true;
        music_progress_changed = true;
    } else {
        music_player_changed = ui_music_snapshot_player_changed(&s_music_snapshot_cache, &snapshot);
        music_progress_changed = ui_music_snapshot_progress_changed(&s_music_snapshot_cache, &snapshot);
    }
    if (!s_music_snapshot_cached || music_player_changed || music_progress_changed) {
        s_music_snapshot_cache = snapshot;
        s_music_snapshot_cached = true;
    }
    if ((music_player_changed || music_progress_changed) && ui_take_model_lock()) {
        if (s_model.page == UI_PAGE_MUSIC_PLAYER) {
            if (music_player_changed) {
                s_model.music_player_dirty = true;
            }
            if (music_progress_changed) {
                s_model.music_progress_dirty = true;
            }
        }
        xSemaphoreGive(s_model_lock);
    }
}

static void ui_fill_action_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    ui_file_action_t action = ui_file_action_for_kind(model->action_kind, (uint8_t)index);

    snprintf(title, title_len, "%s", ui_file_action_label(action));
    snprintf(hint, hint_len, "%s", ui_file_kind_label(model->action_kind));
    *symbol = (action == UI_FILE_ACTION_VERIFY_ROM || action == UI_FILE_ACTION_VERIFY_SAVE) ? LV_SYMBOL_OK : LV_SYMBOL_UPLOAD;
    *accent = 0x4CC9F0;
}

#include "music/ui_music_row.inc"

static void ui_fill_action_detail_row(const ui_model_t *model, uint16_t detail_index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    char size_text[24] = {0};

    if (model == NULL) {
        return;
    }
    switch (detail_index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("File"));
            snprintf(hint, hint_len, "%s", model->action_file.name);
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Size"));
            ui_format_file_size(model->action_file.size, size_text, sizeof(size_text));
            snprintf(hint, hint_len, "%s", size_text);
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Type"));
            snprintf(hint, hint_len, "%s", ui_file_kind_label(model->action_kind));
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Path"));
            snprintf(hint, hint_len, "%s", model->action_file.path[0] != '\0' ? model->action_file.path : "--");
            break;
    }
}

static void ui_fill_wifi_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Current"));
            snprintf(hint, hint_len, "%s %s", ui_wifi_state_name(model->wifi_state), model->ip_text);
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("View QR"));
            snprintf(hint, hint_len, "%s", ui_tr("Scan web address"));
            break;
        case 2:
            snprintf(title, title_len, "%s", "SMB Server");
            snprintf(hint, hint_len, "%s", "scan / saved NAS");
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Connect saved"));
            snprintf(hint, hint_len, "%s", ui_tr("STA profile"));
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Provision AP"));
            snprintf(hint, hint_len, "%s", ui_tr("Setup portal"));
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "关闭热点" : "Close hotspot");
            snprintf(
                hint,
                hint_len,
                "%s",
                wifi_maneger_provisioning_waiting_confirm() ?
                    (ui_lang_is_zh() ? "结束配网热点" : "end setup hotspot") :
                    (ui_lang_is_zh() ? "切回仅路由Wi-Fi" : "switch to STA only"));
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("Disconnect"));
            snprintf(hint, hint_len, "%s", ui_tr("Stop STA"));
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Clear saved"));
            snprintf(hint, hint_len, "%s", ui_tr("Forget profile"));
            break;
    }
    *symbol = (index == 5U || index == 6U) ? LV_SYMBOL_CLOSE : LV_SYMBOL_WIFI;
    *accent = (model->wifi_state == UI_WIFI_STATE_CONNECTED) ? 0x72EFDD : 0xF7B32B;
}

static void ui_fill_smb_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    (void)model;
    ui_smb_ensure_context();
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", "Scan LAN SMB");
            snprintf(hint, hint_len, "%u found", (unsigned)s_smb_ctx.server_count);
            *symbol = LV_SYMBOL_REFRESH;
            break;
        case 1:
            snprintf(title, title_len, "%s", "Saved positions");
            snprintf(hint, hint_len, "%u saved", (unsigned)s_smb_ctx.favorite_count);
            *symbol = LV_SYMBOL_REFRESH;
            break;
        case 2:
            snprintf(title, title_len, "%s", "Host");
            snprintf(hint, hint_len, "%s", s_smb_ctx.config.host[0] != '\0' ? s_smb_ctx.config.host : "--");
            *symbol = LV_SYMBOL_HOME;
            break;
        case 3:
            snprintf(title, title_len, "%s", "Share");
            snprintf(hint, hint_len, "%s", s_smb_ctx.config.share[0] != '\0' ? s_smb_ctx.config.share : "--");
            *symbol = LV_SYMBOL_DIRECTORY;
            break;
        case 4:
            snprintf(title, title_len, "%s", "User");
            snprintf(hint, hint_len, "%s", s_smb_ctx.config.user[0] != '\0' ? s_smb_ctx.config.user : "guest");
            *symbol = LV_SYMBOL_EDIT;
            break;
        case 5:
            snprintf(title, title_len, "%s", "Password");
            snprintf(hint, hint_len, "%s", s_smb_ctx.config.password[0] != '\0' ? "********" : "(empty)");
            *symbol = LV_SYMBOL_EDIT;
            break;
        case 6:
            snprintf(title, title_len, "%s", "Domain");
            snprintf(hint, hint_len, "%s", s_smb_ctx.config.domain[0] != '\0' ? s_smb_ctx.config.domain : "(empty)");
            *symbol = LV_SYMBOL_EDIT;
            break;
        case 7:
            snprintf(title, title_len, "%s", "Port");
            snprintf(hint, hint_len, "%d", s_smb_ctx.config.port > 0 ? s_smb_ctx.config.port : 445);
            *symbol = LV_SYMBOL_SETTINGS;
            break;
        case 8:
            snprintf(title, title_len, "%s", "Signing");
            snprintf(hint, hint_len, "%s", s_smb_ctx.config.signing ? "on" : "off");
            *symbol = LV_SYMBOL_SETTINGS;
            break;
        case 9:
            snprintf(title, title_len, "%s", "Add position");
            snprintf(hint, hint_len, "%s", s_smb_ctx.config.share[0] != '\0' ? "save host/share" : "set share first");
            *symbol = LV_SYMBOL_OK;
            break;
        case 10:
            snprintf(title, title_len, "%s", "Disconnect current");
            snprintf(hint, hint_len, "%s", "saved stays");
            *symbol = LV_SYMBOL_CLOSE;
            break;
        case 11:
            snprintf(title, title_len, "%s", "Music folder");
            snprintf(hint, hint_len, "%s", "connect saved first");
            *symbol = LV_SYMBOL_AUDIO;
            break;
        default: {
            uint16_t favorite_index = (uint16_t)(index - UI_SMB_BASE_ITEM_COUNT);

            if (favorite_index < s_smb_ctx.favorite_count) {
                snprintf(title, title_len, "%s", s_smb_ctx.favorites[favorite_index].label);
                snprintf(
                    hint,
                    hint_len,
                    "saved as %s",
                    s_smb_ctx.favorites[favorite_index].config.user[0] != '\0'
                        ? s_smb_ctx.favorites[favorite_index].config.user
                        : "guest");
                *symbol = LV_SYMBOL_DIRECTORY;
            } else {
                uint16_t server_index = (uint16_t)(favorite_index - s_smb_ctx.favorite_count);

                if (server_index < s_smb_ctx.server_count) {
                    snprintf(title, title_len, "%s", s_smb_ctx.servers[server_index].host);
                    snprintf(hint, hint_len, "%s:%d", s_smb_ctx.servers[server_index].name, s_smb_ctx.servers[server_index].port);
                    *symbol = LV_SYMBOL_WIFI;
                } else {
                    title[0] = '\0';
                    hint[0] = '\0';
                }
            }
            break;
        }
    }
    *accent = 0x4CC9F0;
}

static void ui_fill_smb_text_input_row(uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    char key = ui_smb_input_char_for_index(index);

    if (key != '\0') {
        if (key == ' ') {
            snprintf(title, title_len, "%s", "Space");
            snprintf(hint, hint_len, "%s", "append space");
        } else {
            snprintf(title, title_len, "%c", key);
            snprintf(hint, hint_len, "%s", "append");
        }
        return;
    }
    snprintf(title, title_len, "%s", ui_smb_input_action_label(index));
    snprintf(hint, hint_len, "%s", "edit");
}

static void ui_fill_file_row(const ui_model_t *model, uint16_t visible_index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    const ui_file_entry_t *entry = NULL;
    uint16_t ordinal;
    uint16_t window_index;
    char size_text[24] = {0};
    ui_file_kind_t kind = UI_FILE_KIND_UNSUPPORTED;
    bool reader_supported = false;

    ordinal = model->file_scroll + visible_index;
    if (ordinal >= model->file_total ||
        ordinal < model->file_window_start ||
        ordinal >= model->file_window_start + model->file_loaded_count) {
        if (title_len > 0U) {
            title[0] = '\0';
        }
        if (hint_len > 0U) {
            hint[0] = '\0';
        }
        *symbol = LV_SYMBOL_DUMMY;
        *accent = 0x4CC9F0;
        return;
    }

    window_index = ordinal - model->file_window_start;
    entry = &model->file_window[window_index];
    snprintf(title, title_len, "%s", entry->name);
    if (entry->is_dir) {
        snprintf(hint, hint_len, "%s", ui_tr("dir"));
        *symbol = LV_SYMBOL_DIRECTORY;
    } else {
        kind = ui_file_kind_from_name(entry->name);
        reader_supported = ui_file_page_is_book_scope(model) && ui_reader_native_file_supported_name(entry->name);
        ui_format_file_size(entry->size, size_text, sizeof(size_text));
        snprintf(hint, hint_len, "%s", size_text);
        *symbol = (ui_file_action_count_for_kind(kind) > 0U || reader_supported) ? LV_SYMBOL_FILE : LV_SYMBOL_WARNING;
    }
    *accent = 0x4CC9F0;
}
static void ui_fill_system_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    const ui_menu_item_t *item = NULL;

    (void)model;
    if (index >= UI_SYSTEM_ITEM_COUNT) {
        if (title_len > 0U) {
            title[0] = '\0';
        }
        if (hint_len > 0U) {
            hint[0] = '\0';
        }
        return;
    }
    item = &s_system_items[index];
    snprintf(title, title_len, "%s", ui_system_item_title(item));
    snprintf(hint, hint_len, "%s", ui_system_item_hint(item));
}

static void ui_fill_app_manager_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    const app_descriptor_t *desc = ui_app_manager_desc_at(index);
    size_t position = 0;

    (void)model;
    if (desc == NULL) {
        if (title_len > 0U) {
            title[0] = '\0';
        }
        if (hint_len > 0U) {
            hint[0] = '\0';
        }
        return;
    }
    snprintf(title, title_len, "%s", desc->title);
    if (ui_app_config_position(desc->id, &position)) {
        snprintf(
            hint,
            hint_len,
            "shown %u/%u",
            (unsigned)(position + 1U),
            (unsigned)APP_REGISTRY_HOME_APP_MAX);
    } else {
        snprintf(hint, hint_len, "%s", ui_tr("hidden"));
    }
}

static void ui_fill_tf_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    (void)model;
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Browse TF"));
            snprintf(hint, hint_len, "%s", card != NULL ? ui_tr("open files") : ui_tr("card missing"));
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Upload files"));
            snprintf(hint, hint_len, "%s", ui_tr("web action"));
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("New folder"));
            snprintf(hint, hint_len, "%s", ui_tr("web action"));
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Delete/Rename"));
            snprintf(hint, hint_len, "%s", ui_tr("web action"));
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Refresh"));
            snprintf(hint, hint_len, "%s", usb_msc_tf_in_use_by_host() ? ui_tr("USB owns TF") : ui_tr("ESP owns TF"));
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("Enable USB"));
            snprintf(hint, hint_len, "%s", ui_tr("PC owns TF"));
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Disable USB"));
            snprintf(hint, hint_len, "%s", ui_tr("ESP owns TF"));
            break;
    }
}

static void ui_fill_power_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    if (index == 0U) {
        snprintf(title, title_len, "%s", ui_tr("Screen auto off"));
        ui_format_idle_minutes(mori_screen_idle_off_minutes(), hint, hint_len);
        return;
    }
    if (index == 1U) {
        snprintf(title, title_len, "%s", ui_tr("Wi-Fi auto off"));
        ui_format_idle_minutes(mori_wifi_idle_off_minutes(), hint, hint_len);
        return;
    }
    index -= 2U;

    bool power_valid = (model != NULL && model->power_valid);
    bool adc_supported = ui_power_adc_supported(model);
    const power_manager_telemetry_t *power = power_valid ? &model->power_telemetry : NULL;

    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "电池电量" : "Battery level");
            if (model != NULL && model->battery_valid) {
                snprintf(hint, hint_len, "%u%%", (unsigned)model->battery_percent);
            } else if (power_valid && power->battery_present) {
                snprintf(hint, hint_len, "--");
            } else if (power_valid) {
                snprintf(hint, hint_len, "%s", ui_lang_is_zh() ? "无电池" : "no batt");
            } else {
                snprintf(hint, hint_len, "--");
            }
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "电池电压" : "Battery voltage");
            ui_format_power_voltage(power_valid ? power->battery_voltage_mv : 0U, adc_supported, hint, hint_len);
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "外部输入电压" : "ACIN voltage");
            ui_format_power_voltage(power_valid ? power->acin_voltage_mv : 0U, adc_supported, hint, hint_len);
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "USB输入电压" : "VBUS voltage");
            ui_format_power_voltage(power_valid ? power->vbus_voltage_mv : 0U, adc_supported, hint, hint_len);
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "系统输出电压" : "IPSOUT voltage");
            ui_format_power_voltage(power_valid ? power->ipsout_voltage_mv : 0U, adc_supported, hint, hint_len);
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "充电电流" : "Charge current");
            ui_format_power_current(power_valid ? power->battery_charge_current_ma_x10 : 0U, adc_supported, hint, hint_len);
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "放电电流" : "Discharge current");
            ui_format_power_current(power_valid ? power->battery_discharge_current_ma_x10 : 0U, adc_supported, hint, hint_len);
            break;
        case 7:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "电流方向" : "Current direction");
            snprintf(hint, hint_len, "%s", power_valid ? ui_power_direction_label(power->current_direction) : "--");
            break;
        case 8:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "充电模式" : "Charge mode");
            snprintf(hint, hint_len, "%s", power_valid ? ui_power_mode_label(power->charge_mode) : "--");
            break;
        case 9:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "当前状态" : "Current state");
            snprintf(hint, hint_len, "%s", power_valid ? ui_power_state_label(power->charge_state) : "--");
            break;
        case 10:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "芯片温度" : "Chip temperature");
            ui_format_power_temp(power_valid ? power->internal_temp_deci_c : 0, adc_supported, hint, hint_len);
            break;
        default:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "电源芯片" : "Power chip");
            snprintf(hint, hint_len, "%s", power_valid ? power->chip_name : (power_manager_ready() ? power_manager_chip_name() : ui_tr("missing")));
            break;
    }
}

#include "burner/ui/ui_burner_rows.inc"

static void ui_fill_settings_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    (void)model;
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Brightness"));
            snprintf(hint, hint_len, "%u/255", (unsigned)lcd_display_get_brightness());
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Volume"));
            snprintf(hint, hint_len, "%u%%", (unsigned)ui_settings_current_volume());
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Restart"));
            snprintf(hint, hint_len, "%s", ui_tr("reboot device"));
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Storage"));
            snprintf(hint, hint_len, "TF/USB");
            break;
        case 4:
            snprintf(title, title_len, "Wi-Fi");
            snprintf(hint, hint_len, "network");
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("Update GBX cache"));
            snprintf(hint, hint_len, "%s", ".gbx/gbx_cache.bin");
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("Language"));
            snprintf(hint, hint_len, "%s", ui_tr("current language"));
            break;
        case 7:
            snprintf(title, title_len, "%s", ui_tr("Firmware Full Upgrade"));
            snprintf(hint, hint_len, "%s", ui_tr("web action"));
            break;
        case 8:
            snprintf(title, title_len, "%s", ui_tr("Web deploy"));
            snprintf(hint, hint_len, "%s", ui_tr("web action"));
            break;
        case 9:
            snprintf(title, title_len, "%s", ui_tr("Task status"));
            snprintf(hint, hint_len, "%s", ui_tr("burn progress"));
            break;
        default:
            if (title_len > 0U) {
                title[0] = '\0';
            }
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
            break;
    }
}

static void ui_px_icon(const ui_menu_item_t *item, int32_t x, int32_t y, bool selected)
{
    const int32_t s = UI_TILE_ICON_SIZE;
    ui_action_t action = item != NULL ? item->action : UI_ACTION_OPEN_SETTINGS;

#define UI_ICON_X(v) (x + ((int32_t)(v) * s) / 30)
#define UI_ICON_Y(v) (y + ((int32_t)(v) * s) / 30)
#define UI_ICON_W(v) ((((int32_t)(v) * s) / 30) > 0 ? (((int32_t)(v) * s) / 30) : 1)

    ui_px_frame(x + 1, y + 1, UI_TILE_ICON_SIZE - 2, UI_TILE_ICON_SIZE - 2, true);
    switch (action) {
        case UI_ACTION_OPEN_SYSTEM:
            ui_px_frame(UI_ICON_X(6), UI_ICON_Y(7), UI_ICON_W(18), UI_ICON_W(13), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(23), UI_ICON_W(10), true);
            ui_px_vline(UI_ICON_X(15), UI_ICON_Y(20), UI_ICON_W(3), true);
            break;
        case UI_ACTION_OPEN_APP_MANAGER:
            ui_px_frame(UI_ICON_X(7), UI_ICON_Y(7), UI_ICON_W(6), UI_ICON_W(6), true);
            ui_px_frame(UI_ICON_X(17), UI_ICON_Y(7), UI_ICON_W(6), UI_ICON_W(6), true);
            ui_px_frame(UI_ICON_X(7), UI_ICON_Y(17), UI_ICON_W(6), UI_ICON_W(6), true);
            ui_px_frame(UI_ICON_X(17), UI_ICON_Y(17), UI_ICON_W(6), UI_ICON_W(6), true);
            ui_px_hline(UI_ICON_X(9), UI_ICON_Y(25), UI_ICON_W(12), true);
            break;
        case UI_ACTION_OPEN_TF:
            ui_px_frame(UI_ICON_X(6), UI_ICON_Y(8), UI_ICON_W(18), UI_ICON_W(14), true);
            ui_px_hline(UI_ICON_X(8), UI_ICON_Y(6), UI_ICON_W(8), true);
            ui_px_vline(UI_ICON_X(6), UI_ICON_Y(7), UI_ICON_W(3), true);
            break;
        case UI_ACTION_OPEN_READER:
            ui_px_frame(UI_ICON_X(7), UI_ICON_Y(8), UI_ICON_W(16), UI_ICON_W(14), true);
            ui_px_vline(UI_ICON_X(15), UI_ICON_Y(8), UI_ICON_W(14), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(12), UI_ICON_W(3), true);
            ui_px_hline(UI_ICON_X(17), UI_ICON_Y(12), UI_ICON_W(3), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(15), UI_ICON_W(3), true);
            ui_px_hline(UI_ICON_X(17), UI_ICON_Y(15), UI_ICON_W(3), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(18), UI_ICON_W(3), true);
            ui_px_hline(UI_ICON_X(17), UI_ICON_Y(18), UI_ICON_W(3), true);
            break;
        case UI_ACTION_OPEN_MUSIC:
            ui_px_vline(UI_ICON_X(11), UI_ICON_Y(7), UI_ICON_W(13), true);
            ui_px_vline(UI_ICON_X(19), UI_ICON_Y(5), UI_ICON_W(11), true);
            ui_px_hline(UI_ICON_X(11), UI_ICON_Y(7), UI_ICON_W(8), true);
            ui_px_hline(UI_ICON_X(11), UI_ICON_Y(10), UI_ICON_W(8), true);
            ui_px_box(UI_ICON_X(7), UI_ICON_Y(20), UI_ICON_W(5), UI_ICON_W(5), true);
            ui_px_box(UI_ICON_X(15), UI_ICON_Y(18), UI_ICON_W(5), UI_ICON_W(5), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(23), UI_ICON_W(2), false);
            ui_px_hline(UI_ICON_X(18), UI_ICON_Y(21), UI_ICON_W(2), false);
            break;
        case UI_ACTION_OPEN_BURNER:
            ui_px_hline(UI_ICON_X(14), UI_ICON_Y(5), UI_ICON_W(1), true);
            ui_px_hline(UI_ICON_X(12), UI_ICON_Y(6), UI_ICON_W(5), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(8), UI_ICON_W(9), true);
            ui_px_hline(UI_ICON_X(9), UI_ICON_Y(11), UI_ICON_W(12), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(16), UI_ICON_W(10), true);
            ui_px_hline(UI_ICON_X(12), UI_ICON_Y(20), UI_ICON_W(6), true);
            break;
        case UI_ACTION_OPEN_WIFI:
            ui_px_hline(UI_ICON_X(7), UI_ICON_Y(9), UI_ICON_W(16), true);
            ui_px_hline(UI_ICON_X(9), UI_ICON_Y(12), UI_ICON_W(12), true);
            ui_px_hline(UI_ICON_X(11), UI_ICON_Y(15), UI_ICON_W(8), true);
            ui_px_hline(UI_ICON_X(13), UI_ICON_Y(18), UI_ICON_W(4), true);
            ui_px_box(UI_ICON_X(14), UI_ICON_Y(22), UI_ICON_W(2), UI_ICON_W(2), true);
            break;
        case UI_ACTION_OPEN_POWER:
            ui_px_frame(UI_ICON_X(8), UI_ICON_Y(9), UI_ICON_W(14), UI_ICON_W(12), true);
            ui_px_box(UI_ICON_X(22), UI_ICON_Y(13), UI_ICON_W(2), UI_ICON_W(4), true);
            ui_px_box(UI_ICON_X(11), UI_ICON_Y(12), UI_ICON_W(8), UI_ICON_W(6), true);
            break;
        case UI_ACTION_OPEN_RETRO_GO:
            ui_px_frame(UI_ICON_X(6), UI_ICON_Y(8), UI_ICON_W(18), UI_ICON_W(14), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(11), UI_ICON_W(10), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(15), UI_ICON_W(7), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(19), UI_ICON_W(9), true);
            ui_px_vline(UI_ICON_X(18), UI_ICON_Y(11), UI_ICON_W(8), true);
            ui_px_hline(UI_ICON_X(19), UI_ICON_Y(15), UI_ICON_W(3), true);
            ui_px_vline(UI_ICON_X(22), UI_ICON_Y(14), UI_ICON_W(3), true);
            break;
        case UI_ACTION_OPEN_SETTINGS:
        default:
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(14), UI_ICON_W(10), true);
            ui_px_vline(UI_ICON_X(15), UI_ICON_Y(9), UI_ICON_W(10), true);
            ui_px_frame(UI_ICON_X(12), UI_ICON_Y(11), UI_ICON_W(6), UI_ICON_W(6), true);
            ui_px_set(UI_ICON_X(8), UI_ICON_Y(8), true);
            ui_px_set(UI_ICON_X(22), UI_ICON_Y(8), true);
            ui_px_set(UI_ICON_X(8), UI_ICON_Y(22), true);
            ui_px_set(UI_ICON_X(22), UI_ICON_Y(22), true);
            break;
    }
    if (selected) {
        ui_px_corner_box(
            x - UI_TILE_SELECTOR_MARGIN,
            y - UI_TILE_SELECTOR_MARGIN,
            UI_TILE_ICON_SIZE + UI_TILE_SELECTOR_MARGIN * 2,
            UI_TILE_ICON_SIZE + UI_TILE_SELECTOR_MARGIN * 2);
    }

#undef UI_ICON_X
#undef UI_ICON_Y
#undef UI_ICON_W
}

static void ui_px_draw_icon_page(const ui_model_t *model)
{
    ui_icon_page_config_t config = {0};
    const ui_menu_item_t *selected_item = NULL;
    uint16_t selected = 0;
    int32_t progress_w = 0;

    if (model == NULL || !ui_icon_page_config_for_page(model->page, &config) || config.count == 0U) {
        return;
    }
    selected = (model->selected < config.count) ? model->selected : 0U;
    selected_item = &config.items[selected];
    s_anim.tile_bar_target_w = (float)(((uint32_t)(selected + 1U) * (uint32_t)UI_CANVAS_W) / config.count);
    ui_anim_move(&s_anim.tile_bar_w, s_anim.tile_bar_target_w, UI_TILE_BAR_SPEED);
    progress_w = (int32_t)s_anim.tile_bar_w;

    ui_px_text(8, 10, model->page == UI_PAGE_ROOT ? "MORI" : ui_tr("System"), true);
    ui_px_text_clipped(UI_CANVAS_W - 118, 10, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, 10);
    ui_px_box(0, 0, progress_w, UI_TILE_BAR_H, true);

    for (uint16_t i = 0; i < config.count; ++i) {
        int32_t col = (int32_t)(i % config.cols);
        int32_t row = (int32_t)(i / config.cols);
        int32_t cell_x = UI_TILE_PAGE_GAP_X + col * (UI_TILE_CELL_W + UI_TILE_PAGE_GAP_X);
        int32_t cell_y = UI_TILE_PAGE_TOP + row * (UI_TILE_CELL_H + UI_TILE_PAGE_GAP_Y);
        int32_t icon_x = cell_x + (UI_TILE_CELL_W - UI_TILE_ICON_SIZE) / 2;
        int32_t icon_y = cell_y + 4;
        const ui_menu_item_t *item = &config.items[i];
        const char *title = (model->page == UI_PAGE_ROOT) ? ui_root_item_title(item) : ui_system_item_title(item);
        int32_t title_w = ui_px_text_width(title);
        int32_t title_x = cell_x + (UI_TILE_CELL_W - title_w) / 2;

        ui_px_icon(item, icon_x, icon_y, i == selected);
        ui_px_text_clipped(title_x, icon_y + UI_TILE_ICON_SIZE + 10, UI_TILE_CELL_W, title, true);
    }

    if (selected_item != NULL) {
        const char *hint = (model->page == UI_PAGE_ROOT) ? ui_root_item_hint(selected_item) : ui_system_item_hint(selected_item);

        ui_px_text((UI_CANVAS_W - ui_px_text_width(hint)) / 2, UI_CANVAS_H - UI_HINT_H - UI_TILE_PAGE_BOTTOM_HINT_GAP, hint, true);
    }
    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
}

static bool ui_px_icon_page_item_rect(
    const ui_icon_page_config_t *config,
    uint16_t index,
    int32_t *x_out,
    int32_t *y_out,
    int32_t *w_out,
    int32_t *h_out)
{
    int32_t col;
    int32_t row;
    int32_t cell_x;
    int32_t cell_y;

    if (config == NULL || index >= config->count) {
        return false;
    }
    col = (int32_t)(index % config->cols);
    row = (int32_t)(index / config->cols);
    cell_x = UI_TILE_PAGE_GAP_X + col * (UI_TILE_CELL_W + UI_TILE_PAGE_GAP_X);
    cell_y = UI_TILE_PAGE_TOP + row * (UI_TILE_CELL_H + UI_TILE_PAGE_GAP_Y);
    if (x_out != NULL) {
        *x_out = cell_x - UI_TILE_SELECTOR_MARGIN;
    }
    if (y_out != NULL) {
        *y_out = cell_y - UI_TILE_SELECTOR_MARGIN;
    }
    if (w_out != NULL) {
        *w_out = UI_TILE_CELL_W + UI_TILE_SELECTOR_MARGIN * 2;
    }
    if (h_out != NULL) {
        *h_out = UI_TILE_CELL_H + UI_TILE_SELECTOR_MARGIN * 2;
    }
    return true;
}

static void ui_px_draw_icon_page_item(
    const ui_model_t *model,
    const ui_icon_page_config_t *config,
    uint16_t index,
    bool selected)
{
    int32_t col;
    int32_t row;
    int32_t cell_x;
    int32_t cell_y;
    int32_t icon_x;
    int32_t icon_y;
    int32_t title_w;
    int32_t title_x;
    const ui_menu_item_t *item;
    const char *title;

    if (model == NULL || config == NULL || index >= config->count) {
        return;
    }
    col = (int32_t)(index % config->cols);
    row = (int32_t)(index / config->cols);
    cell_x = UI_TILE_PAGE_GAP_X + col * (UI_TILE_CELL_W + UI_TILE_PAGE_GAP_X);
    cell_y = UI_TILE_PAGE_TOP + row * (UI_TILE_CELL_H + UI_TILE_PAGE_GAP_Y);
    icon_x = cell_x + (UI_TILE_CELL_W - UI_TILE_ICON_SIZE) / 2;
    icon_y = cell_y + 4;
    item = &config->items[index];
    title = (model->page == UI_PAGE_ROOT) ? ui_root_item_title(item) : ui_system_item_title(item);
    title_w = ui_px_text_width(title);
    title_x = cell_x + (UI_TILE_CELL_W - title_w) / 2;

    ui_px_icon(item, icon_x, icon_y, selected);
    ui_px_text_clipped(title_x, icon_y + UI_TILE_ICON_SIZE + 10, UI_TILE_CELL_W, title, true);
}

static void ui_px_draw_icon_page_footer(const ui_model_t *model, const ui_menu_item_t *selected_item)
{
    const char *hint = "";

    if (model == NULL) {
        return;
    }
    if (selected_item != NULL) {
        hint = (model->page == UI_PAGE_ROOT) ? ui_root_item_hint(selected_item) : ui_system_item_hint(selected_item);
    }
    ui_px_clear_rect(0, UI_CANVAS_H - UI_HINT_H - 3, UI_CANVAS_W, UI_HINT_H + 3);
    if (hint[0] != '\0') {
        ui_px_text((UI_CANVAS_W - ui_px_text_width(hint)) / 2, UI_CANVAS_H - UI_HINT_H - UI_TILE_PAGE_BOTTOM_HINT_GAP, hint, true);
    }
    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
}

static void ui_px_apply_icon_grid_chrome_dynamic(const ui_model_t *model)
{
    char header[UI_ROW_TEXT_MAX_LEN] = {0};
    ui_icon_page_config_t config = {0};
    const ui_menu_item_t *selected_item = NULL;

    if (model == NULL) {
        return;
    }
    (void)ui_icon_page_config_for_page(model->page, &config);
    if (config.count > 0U) {
        uint16_t selected = (model->selected < config.count) ? model->selected : 0U;
        selected_item = &config.items[selected];
    }

    ui_px_clear_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
    ui_format_page_header(model, header, sizeof(header));
    ui_px_text(4, 10, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, 10, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, 10);
    ui_px_invalidate_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);

    ui_px_draw_icon_page_footer(model, selected_item);
    ui_px_invalidate_rect(0, UI_CANVAS_H - UI_HINT_H - 3, UI_CANVAS_W, UI_HINT_H + 3);
}

static void ui_px_apply_tile(const ui_model_t *model)
{
    ui_icon_page_config_t config = {0};

    if (model != NULL && ui_icon_page_config_for_page(model->page, &config) && config.count > 0U) {
        s_anim.icon_prev_selected = (model->selected < config.count) ? model->selected : 0U;
    } else {
        s_anim.icon_prev_selected = UINT16_MAX;
    }
    ui_px_draw_icon_page(model);
}

static void ui_px_apply_tile_dynamic(const ui_model_t *model)
{
    ui_icon_page_config_t config = {0};
    uint16_t selected;
    uint16_t prev_selected;
    int32_t prev_bar_w;
    int32_t new_bar_w;
    int32_t bar_x;
    int32_t bar_w;
    const ui_menu_item_t *selected_item = NULL;

    if (model == NULL || !ui_icon_page_config_for_page(model->page, &config) || config.count == 0U) {
        return;
    }

    selected = (model->selected < config.count) ? model->selected : 0U;
    selected_item = &config.items[selected];
    prev_selected = s_anim.icon_prev_selected;
    if (prev_selected >= config.count) {
        prev_selected = selected;
    }

    prev_bar_w = (int32_t)s_anim.tile_bar_w;
    s_anim.tile_bar_target_w = (float)(((uint32_t)(selected + 1U) * (uint32_t)UI_CANVAS_W) / config.count);
    ui_anim_move(&s_anim.tile_bar_w, s_anim.tile_bar_target_w, UI_TILE_BAR_SPEED);
    new_bar_w = (int32_t)s_anim.tile_bar_w;
    bar_x = (prev_bar_w < new_bar_w) ? prev_bar_w : new_bar_w;
    bar_w = ((prev_bar_w > new_bar_w) ? prev_bar_w : new_bar_w) - bar_x + 1;
    ui_px_clear_rect(bar_x, 0, bar_w, UI_TILE_BAR_H);
    ui_px_box(0, 0, new_bar_w, UI_TILE_BAR_H, true);
    ui_px_invalidate_rect(bar_x, 0, bar_w, UI_TILE_BAR_H);

    if (model->motion_dirty || prev_selected != selected) {
        int32_t x;
        int32_t y;
        int32_t w;
        int32_t h;

        if (ui_px_icon_page_item_rect(&config, prev_selected, &x, &y, &w, &h)) {
            ui_px_clear_rect(x, y, w, h);
            ui_px_draw_icon_page_item(model, &config, prev_selected, false);
            ui_px_invalidate_rect(x, y, w, h);
        }
        if (selected != prev_selected &&
            ui_px_icon_page_item_rect(&config, selected, &x, &y, &w, &h)) {
            ui_px_clear_rect(x, y, w, h);
            ui_px_draw_icon_page_item(model, &config, selected, true);
            ui_px_invalidate_rect(x, y, w, h);
        }
        s_anim.icon_prev_selected = selected;
    }

    if (model->chrome_dirty || model->content_dirty || prev_selected != selected) {
        ui_px_draw_icon_page_footer(model, selected_item);
        ui_px_invalidate_rect(0, UI_CANVAS_H - UI_HINT_H - 3, UI_CANVAS_W, UI_HINT_H + 3);
    }
}

#include "burner/ui/ui_burner_modes_draw.inc"

static void ui_wifi_web_url(const ui_model_t *model, char *url, size_t url_len)
{
    const char *ip = "192.168.4.1";

    if (url == NULL || url_len == 0U) {
        return;
    }
    if (model != NULL &&
        model->ip_text[0] != '\0' &&
        strcmp(model->ip_text, "--") != 0 &&
        strcmp(model->ip_text, "192.168.4.1") != 0) {
        ip = model->ip_text;
    }
    snprintf(url, url_len, "http://%s/", ip);
}

static void ui_px_draw_wifi_qr_matrix(const uint8_t *qrcode, int32_t x, int32_t y, int32_t module_px)
{
    int32_t qr_size;
    int32_t total_modules;

    if (qrcode == NULL || module_px <= 0) {
        return;
    }
    qr_size = qrcodegen_getSize(qrcode);
    if (qr_size <= 0) {
        return;
    }
    total_modules = qr_size + UI_WIFI_QR_QUIET_MODULES * 2;
    ui_px_box(x, y, total_modules * module_px, total_modules * module_px, true);
    for (int32_t row = 0; row < qr_size; ++row) {
        for (int32_t col = 0; col < qr_size; ++col) {
            if (qrcodegen_getModule(qrcode, col, row)) {
                ui_px_box(
                    x + (col + UI_WIFI_QR_QUIET_MODULES) * module_px,
                    y + (row + UI_WIFI_QR_QUIET_MODULES) * module_px,
                    module_px,
                    module_px,
                    false);
            }
        }
    }
}

static void ui_px_apply_wifi_qr(const ui_model_t *model)
{
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(UI_WIFI_QR_VERSION)] = {0};
    uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(UI_WIFI_QR_VERSION)] = {0};
    char url[UI_WIFI_WEB_URL_MAX_LEN] = {0};
    char header[UI_ROW_TEXT_MAX_LEN] = {0};
    bool ok;
    int32_t qr_size_px;
    int32_t qr_x;
    int32_t qr_y = 38;
    const char *hint;

    if (model == NULL) {
        return;
    }

    ui_wifi_web_url(model, url, sizeof(url));
    ok = qrcodegen_encodeText(
        url,
        temp,
        qrcode,
        qrcodegen_Ecc_LOW,
        UI_WIFI_QR_VERSION,
        UI_WIFI_QR_VERSION,
        qrcodegen_Mask_AUTO,
        true);

    snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
    ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - 2, true);

    if (!ok) {
        ui_px_text_clipped(24, 104, UI_CANVAS_W - 48, ui_tr("QR unavailable"), true);
        ui_px_text_clipped(24, 124, UI_CANVAS_W - 48, url, true);
        return;
    }

    qr_size_px = (qrcodegen_getSize(qrcode) + UI_WIFI_QR_QUIET_MODULES * 2) * UI_WIFI_QR_MODULE_PX;
    qr_x = (UI_CANVAS_W - qr_size_px) / 2;
    ui_px_draw_wifi_qr_matrix(qrcode, qr_x, qr_y, UI_WIFI_QR_MODULE_PX);

    ui_px_text_clipped(4, 210, UI_CANVAS_W - 8, url, true);
    if (model->ip_text[0] != '\0' &&
        strcmp(model->ip_text, "--") != 0 &&
        strcmp(model->ip_text, "192.168.4.1") != 0) {
        hint = ui_tr("Scan to open web");
    } else if (model->wifi_state == UI_WIFI_STATE_CONNECTED) {
        hint = ui_tr("Scan to open web");
    } else if (model->wifi_state == UI_WIFI_STATE_PROVISIONING) {
        hint = ui_tr("Connect phone to MORI AP first");
    } else {
        hint = ui_tr("Start Provision AP first");
    }
    ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, hint, true);
}

static void ui_px_apply_smb_text_input(const ui_model_t *model)
{
    char header[UI_ROW_TEXT_MAX_LEN] = {0};
    char value[UI_ROW_TEXT_MAX_LEN] = {0};
    char *field = NULL;
    size_t field_len = 0;
    uint16_t count;
    uint16_t selected;
    uint16_t scroll;
    int32_t list_y = UI_LIST_HEADER_H + 34;
    int32_t list_h = UI_CANVAS_H - UI_HINT_H - list_y;
    uint16_t visible_rows = (list_h > 0) ? (uint16_t)(list_h / UI_LIST_LINE_H) : 1U;
    int32_t selected_row;
    int32_t selector_y;
    int32_t selector_w = UI_CANVAS_W - UI_LIST_BAR_W - 2;
    int32_t bar_h = 1;

    if (model == NULL) {
        return;
    }
    field = ui_smb_field_buffer(s_smb_ctx.editing_field, &field_len);
    (void)field_len;
    count = ui_page_item_count(model);
    selected = model->selected;
    scroll = model->scroll;
    if (visible_rows == 0U) {
        visible_rows = 1U;
    }

    snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
    ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - 2, true);

    ui_px_text_clipped(4, UI_LIST_HEADER_H + 2, UI_CANVAS_W - 8, ui_smb_field_label(s_smb_ctx.editing_field), true);
    if (s_smb_ctx.editing_field == UI_SMB_FIELD_PASSWORD && field != NULL && field[0] != '\0') {
        snprintf(value, sizeof(value), "%s", "********");
    } else {
        snprintf(value, sizeof(value), "%s", (field != NULL && field[0] != '\0') ? field : "--");
    }
    ui_px_frame(4, UI_LIST_HEADER_H + 16, UI_CANVAS_W - 12, 14, true);
    ui_px_text_clipped(8, UI_LIST_HEADER_H + 19, UI_CANVAS_W - 20, value, true);

    selected_row = (int32_t)selected - (int32_t)scroll;
    selector_y = list_y + selected_row * UI_LIST_LINE_H;
    if (selected_row < 0 || selected_row >= (int32_t)visible_rows) {
        selector_y = list_y;
    }
    for (uint16_t row = 0; row < visible_rows; ++row) {
        uint16_t index = scroll + row;
        int32_t y = list_y + (int32_t)row * UI_LIST_LINE_H;
        char title[UI_ROW_TEXT_MAX_LEN] = {0};
        char hint[UI_ROW_TEXT_MAX_LEN] = {0};

        if (index >= count) {
            continue;
        }
        ui_fill_smb_text_input_row(index, title, sizeof(title), hint, sizeof(hint));
        ui_px_text_clipped(UI_LIST_TEXT_X, y + 3, 84, title, true);
        ui_px_text_clipped(96, y + 3, UI_CANVAS_W - UI_LIST_BAR_W - 100, hint, true);
    }
    if (count > 0U) {
        bar_h = (int32_t)(((uint32_t)(selected + 1U) * (uint32_t)list_h) / count);
        if (bar_h < 1) {
            bar_h = 1;
        } else if (bar_h > list_h) {
            bar_h = list_h;
        }
    }
    ui_px_vline(UI_CANVAS_W - 3, list_y, list_h, true);
    ui_px_box(UI_CANVAS_W - UI_LIST_BAR_W, list_y, UI_LIST_BAR_W, bar_h, true);
    ui_px_invert_rect(0, selector_y, selector_w, UI_LIST_LINE_H - 1);

    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }
    ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, "A append  B delete  Menu OK", true);
}

static void ui_px_draw_task_progress_row(const ui_model_t *model, uint16_t row)
{
    int32_t y;
    int32_t x = UI_LIST_TEXT_X;
    int32_t w = UI_CANVAS_W - UI_LIST_BAR_W - 12;
    const char *label = ui_tr("Burn");
    int32_t label_w = ui_px_text_width(label) + 6;
    int32_t percent_w = ui_px_text_width("100%");
    int32_t percent_x;
    int32_t bar_x;
    int32_t bar_y;
    int32_t bar_w;
    int32_t fill_w;
    int progress;
    char percent[12] = {0};

    if (model == NULL) {
        return;
    }

    progress = model->burn_progress;
    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }

    y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
    bar_y = y + 7;
    percent_x = x + w - percent_w;
    bar_x = x + label_w;
    bar_w = percent_x - bar_x - 4;
    if (bar_w < 24) {
        bar_w = 24;
    }
    fill_w = (bar_w * progress) / 100;
    if (progress > 0 && fill_w < 1) {
        fill_w = 1;
    }

    ui_px_text(x, y + 4, label, true);
    ui_px_frame(bar_x, bar_y - 3, bar_w, 7, true);
    if (fill_w > 0) {
        ui_px_box(bar_x + 1, bar_y - 2, fill_w > bar_w - 2 ? bar_w - 2 : fill_w, 5, true);
    }
    snprintf(percent, sizeof(percent), "%d%%", progress);
    ui_px_text_clipped(percent_x, y + 4, percent_w, percent, true);
}

static int ui_task_patch_kind_for_index(const ui_model_t *model, uint16_t index)
{
    uint16_t patch_index;

    if (model == NULL || index < UI_TASK_PATCH_BASE_ROW) {
        return -1;
    }
    patch_index = (uint16_t)(index - UI_TASK_PATCH_BASE_ROW);
    if (model->gba_patch_sram) {
        if (patch_index == 0U) return 0;
        patch_index--;
    }
    if (model->gba_patch_batteryless) {
        if (patch_index == 0U) return 1;
        patch_index--;
    }
    if (model->gba_patch_waitcnt && patch_index == 0U) return 2;
    return -1;
}

static void ui_px_draw_task_patch_row(const ui_model_t *model, uint16_t row, int kind)
{
    static const char *labels[] = {"SRAM", "Batteryless", "Latency"};
    int32_t y;
    int32_t x = UI_LIST_TEXT_X;
    int32_t w = UI_CANVAS_W - UI_LIST_BAR_W - 12;
    int32_t label_w;
    int32_t percent_w = ui_px_text_width("100%");
    int32_t percent_x;
    int32_t bar_x;
    int32_t bar_w;
    int progress;
    int fill_w;
    char percent[12] = {0};

    if (model == NULL || kind < 0 || kind >= 3) return;
    progress = model->gba_patch_progress[kind];
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
    label_w = ui_px_text_width(labels[kind]) + 6;
    percent_x = x + w - percent_w;
    bar_x = x + label_w;
    bar_w = percent_x - bar_x - 4;
    if (bar_w < 24) bar_w = 24;
    fill_w = (bar_w * progress) / 100;
    if (progress > 0 && fill_w < 1) fill_w = 1;
    ui_px_text(x, y + 4, labels[kind], true);
    ui_px_frame(bar_x, y + 4, bar_w, 7, true);
    if (fill_w > 0) ui_px_box(bar_x + 1, y + 5, fill_w > bar_w - 2 ? bar_w - 2 : fill_w, 5, true);
    snprintf(percent, sizeof(percent), "%d%%", progress);
    ui_px_text_clipped(percent_x, y + 4, percent_w, percent, true);
}

static void ui_px_draw_chip_erase_busy_row(const ui_model_t *model, uint16_t row)
{
    burner_status_t status = {0};
    char text[32] = {0};
    uint32_t dots;
    int32_t y;
    int32_t x;

    if (model == NULL) {
        return;
    }

    burner_status_snapshot(&status);
    if (!ui_chip_erase_busy_active(model, &status)) {
        return;
    }

    dots = (uint32_t)((esp_log_timestamp() / UI_LIVE_REFRESH_MS) % 4U);
    snprintf(
        text,
        sizeof(text),
        "%s%.*s",
        ui_tr("chip erase busy"),
        (int)dots,
        "...");
    y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H + 4;
    x = (UI_CANVAS_W - UI_LIST_BAR_W - ui_px_text_width(text)) / 2;
    if (x < 0) {
        x = 0;
    }
    ui_px_text_clipped(x, y, UI_CANVAS_W - UI_LIST_BAR_W - 4, text, true);
}

static void ui_px_draw_task_erase_progress_row(const ui_model_t *model, uint16_t row)
{
    int32_t y;
    int32_t x = UI_LIST_TEXT_X;
    int32_t w = UI_CANVAS_W - UI_LIST_BAR_W - 12;
    const char *label = ui_tr("Erase");
    int32_t label_w = ui_px_text_width(label) + 6;
    int32_t percent_w = ui_px_text_width("100%");
    int32_t percent_x;
    int32_t bar_x;
    int32_t bar_y;
    int32_t bar_w;
    int32_t fill_w;
    int progress;
    char percent[12] = {0};

    if (model == NULL) {
        return;
    }

    progress = model->erase_progress;
    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }

    y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
    bar_y = y + 7;
    percent_x = x + w - percent_w;
    bar_x = x + label_w;
    bar_w = percent_x - bar_x - 4;
    if (bar_w < 24) {
        bar_w = 24;
    }
    fill_w = (bar_w * progress) / 100;
    if (progress > 0 && fill_w < 1) {
        fill_w = 1;
    }

    ui_px_text(x, y + 4, label, true);
    ui_px_frame(bar_x, bar_y - 3, bar_w, 7, true);
    if (fill_w > 0) {
        ui_px_box(bar_x + 1, bar_y - 2, fill_w > bar_w - 2 ? bar_w - 2 : fill_w, 5, true);
    }
    snprintf(percent, sizeof(percent), "%d%%", progress);
    ui_px_text_clipped(percent_x, y + 4, percent_w, percent, true);
}

static int32_t ui_file_name_col_w(void)
{
    int32_t w = UI_CANVAS_W - UI_LIST_BAR_W - UI_LIST_TEXT_X - UI_FILE_SIZE_COL_W - UI_FILE_NAME_SIZE_GAP - 4;

    return (w > 24) ? w : 24;
}

static int32_t ui_file_size_col_x(void)
{
    return UI_CANVAS_W - UI_LIST_BAR_W - UI_FILE_SIZE_COL_W - 4;
}

static int32_t ui_file_marquee_offset(uint16_t selected, int32_t text_w, int32_t view_w)
{
    uint32_t now_ms;
    uint32_t elapsed_ms;
    uint32_t step;
    uint32_t cycle_steps;
    int32_t overflow;

    if (text_w <= view_w) {
        return 0;
    }
    now_ms = esp_log_timestamp();
    if (s_anim.marquee_selected != selected) {
        s_anim.marquee_selected = selected;
        s_anim.marquee_selected_since_ms = now_ms;
        return 0;
    }
    elapsed_ms = now_ms - s_anim.marquee_selected_since_ms;
    if (elapsed_ms < UI_FILE_MARQUEE_START_MS) {
        return 0;
    }

    overflow = text_w - view_w;
    step = (elapsed_ms - UI_FILE_MARQUEE_START_MS) / UI_FILE_MARQUEE_STEP_MS;
    cycle_steps = (uint32_t)overflow + (uint32_t)(UI_FILE_MARQUEE_END_PAUSE_STEPS * 2);
    if (cycle_steps == 0U) {
        return 0;
    }
    step %= cycle_steps;
    if (step >= (uint32_t)overflow + UI_FILE_MARQUEE_END_PAUSE_STEPS) {
        return overflow;
    }
    if (step >= (uint32_t)overflow) {
        return overflow;
    }
    return (int32_t)step;
}

static void ui_px_draw_file_row(const ui_model_t *model, uint16_t row)
{
    ui_file_entry_t entry = {0};
    uint16_t selected = ui_current_selected(model);
    uint16_t ordinal;
    int32_t y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
    int32_t name_w = ui_file_name_col_w();
    int32_t size_x = ui_file_size_col_x();
    char size_text[24] = {0};
    int32_t size_w;
    int32_t name_text_w;
    int32_t offset = 0;

    if (!ui_file_entry_for_visible_row(model, row, &entry)) {
        return;
    }

    ordinal = model->file_scroll + row;
    name_text_w = ui_px_text_width(entry.name);
    if (ordinal == selected) {
        offset = ui_file_marquee_offset(selected, name_text_w, name_w);
    }
    ui_px_text_clipped_offset(UI_LIST_TEXT_X, y + 4, name_w, entry.name, offset, true);

    if (entry.is_dir) {
        snprintf(size_text, sizeof(size_text), "%s", ui_tr("dir"));
    } else {
        ui_format_file_size(entry.size, size_text, sizeof(size_text));
    }
    size_w = ui_px_text_width(size_text);
    if (size_w > UI_FILE_SIZE_COL_W) {
        size_w = UI_FILE_SIZE_COL_W;
    }
    ui_px_text_clipped(size_x + UI_FILE_SIZE_COL_W - size_w, y + 4, size_w, size_text, true);
}

static void ui_px_draw_music_file_row(
    const ui_model_t *model,
    const music_player_snapshot_t *snap,
    uint16_t row,
    int32_t x,
    int32_t y,
    int32_t w)
{
    ui_file_entry_t entry = {0};
    uint16_t selected = ui_current_selected(model);
    uint16_t ordinal;
    int32_t row_y;
    int32_t text_x;
    int32_t text_w;
    int32_t name_text_w;
    int32_t offset = 0;
    bool is_current = false;

    if (!ui_file_entry_for_visible_row(model, row, &entry)) {
        return;
    }

    ordinal = model->file_scroll + row;
    row_y = y + (int32_t)row * UI_LIST_LINE_H;
    text_x = x + 10;
    text_w = w - 16;
    if (text_w < 12) {
        text_w = 12;
    }
    if (snap != NULL && snap->path[0] != '\0' && strcmp(entry.path, snap->path) == 0) {
        is_current = true;
    }
    if (entry.is_dir) {
        ui_px_text(x + 3, row_y + 4, ">", true);
    } else if (is_current) {
        if (snap != NULL && snap->state == MUSIC_PLAYER_STATE_PAUSED) {
            ui_px_frame(x + 3, row_y + 4, 6, 8, true);
        } else {
            ui_px_box(x + 4, row_y + 5, 4, 6, true);
        }
    }
    name_text_w = ui_px_text_width(entry.name);
    if (ordinal == selected) {
        offset = ui_music_loop_marquee_offset(name_text_w, text_w, (uint32_t)row * 5U);
    }
    ui_px_text_clipped_offset(text_x, row_y + 4, text_w, entry.name, offset, true);
}

static void ui_px_draw_music_button_prev(int32_t x, int32_t y, int32_t size)
{
    ui_px_frame(x, y, size, size, true);
    ui_px_box(x + 7, y + 7, 3, size - 14, true);
    for (int32_t i = 0; i < 5; ++i) {
        ui_px_box(x + 12 + i * 2, y + (size / 2) - 2 - i, 2, 4 + i * 2, true);
    }
}

static void ui_px_draw_music_button_next(int32_t x, int32_t y, int32_t size)
{
    ui_px_frame(x, y, size, size, true);
    ui_px_box(x + size - 10, y + 7, 3, size - 14, true);
    for (int32_t i = 0; i < 5; ++i) {
        ui_px_box(x + size - 14 - i * 2, y + (size / 2) - 2 - i, 2, 4 + i * 2, true);
    }
}

static void ui_px_draw_music_button_play_pause(int32_t x, int32_t y, int32_t size, bool pause_icon)
{
    ui_px_frame(x, y, size, size, true);
    if (pause_icon) {
        ui_px_box(x + 10, y + 8, 4, size - 16, true);
        ui_px_box(x + size - 14, y + 8, 4, size - 16, true);
        return;
    }
    for (int32_t i = 0; i < 8; ++i) {
        ui_px_box(x + 11 + i * 2, y + (size / 2) - 2 - i, 2, 4 + i * 2, true);
    }
}

static void ui_px_draw_action_details(const ui_model_t *model)
{
    uint16_t action_count;
    uint16_t start_row;
    char title[UI_ROW_TEXT_MAX_LEN] = {0};
    char hint[UI_ROW_TEXT_MAX_LEN] = {0};

    if (model == NULL || model->page != UI_PAGE_FILE_ACTIONS) {
        return;
    }

    action_count = ui_file_action_count_for_kind(model->action_kind);
    start_row = action_count + 1U;
    if (start_row < (UI_ROW_COUNT / 2U)) {
        start_row = UI_ROW_COUNT / 2U;
    }
    if (start_row >= UI_ROW_COUNT) {
        return;
    }

    ui_px_hline(4, UI_LIST_HEADER_H + (int32_t)start_row * UI_LIST_LINE_H - 3, UI_CANVAS_W - UI_LIST_BAR_W - 12, true);
    for (uint16_t row = start_row; row < UI_ROW_COUNT; ++row) {
        uint16_t detail_index = row - start_row;
        int32_t y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
        int32_t title_w;

        title[0] = '\0';
        hint[0] = '\0';
        ui_fill_action_detail_row(model, detail_index, title, sizeof(title), hint, sizeof(hint));
        title_w = ui_px_text_width(title);
        ui_px_text_clipped(UI_LIST_TEXT_X, y + 4, 54, title, true);
        if (hint[0] != '\0') {
            int32_t hint_x = UI_LIST_TEXT_X + 58;
            int32_t hint_w = UI_CANVAS_W - UI_LIST_BAR_W - hint_x - 8;
            if (title_w > 54) {
                hint_x = UI_LIST_TEXT_X + title_w + 8;
                hint_w = UI_CANVAS_W - UI_LIST_BAR_W - hint_x - 8;
            }
            ui_px_text_clipped(hint_x, y + 4, hint_w, hint, true);
        }
    }
}

#include "burner/ui/ui_burner_info_draw.inc"

#include "music/ui_music_player_draw.inc"

#include "burner/ui/ui_burner_split_draw.inc"

#include "reader/ui_reader_draw.inc"

static void ui_px_apply_chrome_dynamic(const ui_model_t *model)
{
    char header[UI_ROW_TEXT_MAX_LEN] = {0};

    if (model == NULL) {
        return;
    }
    ui_px_clear_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
    ui_format_page_header(model, header, sizeof(header));
    ui_px_text(4, (model->page == UI_PAGE_ROOT || model->page == UI_PAGE_BURNER) ? 10 : UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, (model->page == UI_PAGE_ROOT || model->page == UI_PAGE_BURNER) ? 10 : UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, (model->page == UI_PAGE_ROOT || model->page == UI_PAGE_BURNER) ? 10 : UI_LIST_HEADER_TEXT_Y);
    ui_px_clear_rect(0, UI_CANVAS_H - UI_HINT_H, UI_CANVAS_W, UI_HINT_H);
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    } else if (model->page == UI_PAGE_APP_MANAGER) {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, "A show/hide  L/R order", true);
    }
    ui_px_invalidate_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
    ui_px_invalidate_rect(0, UI_CANVAS_H - UI_HINT_H, UI_CANVAS_W, UI_HINT_H);
}

static void ui_px_apply_settings_adjust(const ui_model_t *model)
{
    const bool volume = model != NULL && model->settings_adjust == UI_SETTING_ADJUST_VOLUME;
    const char *title = volume ? ui_tr("Volume") : ui_tr("Brightness");
    uint32_t value = volume ? (uint32_t)ui_settings_current_volume() : (uint32_t)lcd_display_get_brightness();
    uint32_t max_value = volume ? 100U : 255U;
    char header[UI_ROW_TEXT_MAX_LEN] = {0};
    char value_text[24] = {0};
    int32_t panel_x = 20;
    int32_t panel_y = 58;
    int32_t panel_w = UI_CANVAS_W - 40;
    int32_t bar_x = panel_x + 12;
    int32_t bar_y = panel_y + 74;
    int32_t bar_w = panel_w - 24;
    int32_t fill_w = (int32_t)(((uint64_t)value * (uint64_t)(bar_w - 2)) / max_value);

    if (fill_w < 0) {
        fill_w = 0;
    } else if (fill_w > bar_w - 2) {
        fill_w = bar_w - 2;
    }

    ui_format_page_header(model, header, sizeof(header));
    ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - 2, true);
    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }

    ui_px_frame(panel_x, panel_y, panel_w, 116, true);
    ui_px_text_clipped(panel_x + 12, panel_y + 14, panel_w - 24, title, true);
    if (volume) {
        snprintf(value_text, sizeof(value_text), "%u%%", (unsigned)value);
    } else {
        snprintf(value_text, sizeof(value_text), "%u/255", (unsigned)value);
    }
    ui_px_text_clipped(panel_x + 12, panel_y + 38, panel_w - 24, value_text, true);
    ui_px_frame(bar_x, bar_y, bar_w, 14, true);
    if (fill_w > 0) {
        ui_px_box(bar_x + 1, bar_y + 1, fill_w, 12, true);
    }
    ui_px_text_clipped(panel_x + 12, panel_y + 96, panel_w - 24, ui_tr("hold 1s for x10"), true);
    ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_tr("L/R adjust  B back"), true);
}

static void ui_px_apply_list(const ui_model_t *model)
{
    uint16_t count = ui_page_item_count(model);
    uint16_t selected = ui_current_selected(model);
    uint16_t scroll = ui_current_scroll(model);
    burner_status_t task_status = {0};
    bool chip_erase_busy = false;
    int32_t bar_h = 1;
    int32_t list_h = (model->page == UI_PAGE_BURN_ROM) ?
                         (int32_t)(UI_BURN_ROM_ACTION_ROWS * UI_LIST_LINE_H) :
                         (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H);
    int32_t selected_row = (int32_t)selected - (int32_t)scroll;
    int32_t selector_y;
    int32_t selector_w;
    char header[UI_ROW_TEXT_MAX_LEN] = {0};

    ui_format_page_header(model, header, sizeof(header));
    if (model->page == UI_PAGE_TASK_STATUS) {
        burner_status_snapshot(&task_status);
        chip_erase_busy = ui_chip_erase_busy_active(model, &task_status);
    }

    ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - UI_LIST_BAR_W - 2, true);
    for (int32_t x = 0; x < UI_CANVAS_W - UI_LIST_BAR_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }

    ui_px_hline(UI_CANVAS_W - UI_LIST_BAR_W, UI_LIST_HEADER_H, UI_LIST_BAR_W, true);
    ui_px_hline(UI_CANVAS_W - UI_LIST_BAR_W, UI_CANVAS_H - UI_HINT_H - 1, UI_LIST_BAR_W, true);
    ui_px_vline(UI_CANVAS_W - 3, UI_LIST_HEADER_H, list_h, true);
    if (count > 0U) {
        bar_h = (int32_t)(((uint32_t)(selected + 1U) * (uint32_t)list_h) / count);
        if (bar_h < 1) {
            bar_h = 1;
        } else if (bar_h > list_h) {
            bar_h = list_h;
        }
        s_anim.list_bar_target_h = (float)bar_h;
        s_anim.list_bar_target_x = (float)(UI_CANVAS_W - UI_LIST_BAR_W);
    }
    ui_anim_move(&s_anim.list_bar_h, s_anim.list_bar_target_h, UI_LIST_BAR_SPEED);
    ui_anim_move(&s_anim.list_bar_x, s_anim.list_bar_target_x, UI_LIST_BAR_SPEED);
    ui_px_box((int32_t)s_anim.list_bar_x, UI_LIST_HEADER_H, UI_LIST_BAR_W, (int32_t)s_anim.list_bar_h, true);

    for (uint16_t row = 0; row < UI_ROW_COUNT; ++row) {
        uint16_t index = scroll + row;
        int32_t y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
        char title[UI_ROW_TEXT_MAX_LEN] = {0};
        char hint[UI_ROW_TEXT_MAX_LEN] = {0};
        int32_t title_w;

        if (model->page == UI_PAGE_BURN_ROM && row >= UI_BURN_ROM_ACTION_ROWS) {
            continue;
        }
        if (index >= count) {
            continue;
        }
        if (model->page == UI_PAGE_TASK_STATUS && index == UI_TASK_ERASE_PROGRESS_ROW) {
            if (chip_erase_busy) {
                ui_px_draw_chip_erase_busy_row(model, row);
                continue;
            }
            ui_px_draw_task_erase_progress_row(model, row);
            continue;
        }
        if (model->page == UI_PAGE_TASK_STATUS && index == UI_TASK_BURN_PROGRESS_ROW) {
            if (chip_erase_busy) {
                continue;
            }
            ui_px_draw_task_progress_row(model, row);
            continue;
        }
        if (model->page == UI_PAGE_TASK_STATUS) {
            int patch_kind = ui_task_patch_kind_for_index(model, index);
            if (patch_kind >= 0) {
                ui_px_draw_task_patch_row(model, row, patch_kind);
                continue;
            }
        }
        if (model->page == UI_PAGE_FILE_ACTIONS) {
            uint16_t action_count = ui_file_action_count_for_kind(model->action_kind);
            uint16_t detail_start = action_count + 1U;
            if (detail_start < (UI_ROW_COUNT / 2U)) {
                detail_start = UI_ROW_COUNT / 2U;
            }
            if (row >= detail_start) {
                continue;
            }
        }
        if (model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES) {
            ui_px_draw_file_row(model, row);
            continue;
        }
        ui_px_fill_row(model, index, row, title, sizeof(title), hint, sizeof(hint));
        title_w = ui_px_text_width(title) + UI_LIST_SELECTOR_MARGIN * 2;
        if (title_w < 18) {
            title_w = 18;
        }
        if (title_w > UI_CANVAS_W - UI_LIST_BAR_W - 2) {
            title_w = UI_CANVAS_W - UI_LIST_BAR_W - 2;
        }
        ui_px_text_clipped(UI_LIST_TEXT_X, y + 4, UI_CANVAS_W - UI_LIST_BAR_W - 12, title, true);
        if (hint[0] != '\0') {
            int32_t hint_w = ui_px_text_width(hint);
            int32_t hint_x = UI_CANVAS_W - UI_LIST_BAR_W - 8 - hint_w;
            if (hint_x > title_w + 4) {
                ui_px_text_clipped(hint_x, y + 4, hint_w, hint, true);
            }
        }
    }

    if (model->page == UI_PAGE_MUSIC_PLAYER) {
        ui_px_draw_music_player(model);
    }

    ui_px_draw_action_details(model);
    if (model->page == UI_PAGE_BURN_ROM) {
        ui_px_draw_burner_cart_info(model);
    }

    selector_y = UI_LIST_HEADER_H + selected_row * UI_LIST_LINE_H;
    if (selected_row < 0 || selected_row >= UI_ROW_COUNT) {
        selector_y = UI_LIST_HEADER_H;
    }
    {
        char title[UI_ROW_TEXT_MAX_LEN] = {0};
        char hint[UI_ROW_TEXT_MAX_LEN] = {0};
        ui_px_fill_row(model, selected, (uint16_t)((selected >= scroll) ? (selected - scroll) : 0U), title, sizeof(title), hint, sizeof(hint));
        selector_w = ui_px_text_width(title) + UI_LIST_SELECTOR_MARGIN * 2;
        if (model->page == UI_PAGE_TASK_STATUS && !s_task_cancel_confirm) {
            burner_status_t status = {0};

            burner_status_snapshot(&status);
            if (ui_chip_erase_busy_active(model, &status) &&
                (selected == UI_TASK_ERASE_PROGRESS_ROW || selected == UI_TASK_BURN_PROGRESS_ROW)) {
                selector_w = UI_CANVAS_W - UI_LIST_BAR_W - 2;
            }
        }
        if (selector_w < 22) {
            selector_w = 22;
        }
        if (selector_w > UI_CANVAS_W - UI_LIST_BAR_W - 2) {
            selector_w = UI_CANVAS_W - UI_LIST_BAR_W - 2;
        }
    }
    s_anim.list_selector_target_y = (float)selector_y;
    s_anim.list_selector_target_w = (float)selector_w;
    ui_anim_move(&s_anim.list_selector_y, s_anim.list_selector_target_y, UI_LIST_SELECTOR_Y_SPEED);
    ui_anim_move(&s_anim.list_selector_w, s_anim.list_selector_target_w, UI_LIST_SELECTOR_W_SPEED);
    ui_px_invert_rect(0, (int32_t)s_anim.list_selector_y, (int32_t)s_anim.list_selector_w, UI_LIST_LINE_H - 1);
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    } else if (model->page == UI_PAGE_APP_MANAGER) {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, "A show/hide  L/R order", true);
    }
}

static void ui_px_draw_visible_row(const ui_model_t *model, uint16_t row)
{
    uint16_t scroll = ui_current_scroll(model);
    uint16_t count = ui_page_item_count(model);
    uint16_t index = scroll + row;
    burner_status_t task_status = {0};
    bool chip_erase_busy = false;
    int32_t y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
    char title[UI_ROW_TEXT_MAX_LEN] = {0};
    char hint[UI_ROW_TEXT_MAX_LEN] = {0};
    int32_t title_w;

    ui_px_clear_rect(0, y, UI_CANVAS_W - UI_LIST_BAR_W - 1, UI_LIST_LINE_H);
    if (model->page == UI_PAGE_BURN_ROM && row >= UI_BURN_ROM_ACTION_ROWS) {
        return;
    }
    if (index >= count) {
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS) {
        burner_status_snapshot(&task_status);
        chip_erase_busy = ui_chip_erase_busy_active(model, &task_status);
        if (chip_erase_busy &&
            (index == UI_TASK_ERASE_PROGRESS_ROW || index == UI_TASK_BURN_PROGRESS_ROW - 1U)) {
            ui_px_clear_rect(
                0,
                y,
                UI_CANVAS_W - UI_LIST_BAR_W - 1,
                UI_LIST_LINE_H * 2);
        }
    }
    if (model->page == UI_PAGE_TASK_STATUS && index == UI_TASK_ERASE_PROGRESS_ROW) {
        if (chip_erase_busy) {
            ui_px_draw_chip_erase_busy_row(model, row);
            return;
        }
        ui_px_draw_task_erase_progress_row(model, row);
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS && index == UI_TASK_BURN_PROGRESS_ROW) {
        if (chip_erase_busy) {
            return;
        }
        ui_px_draw_task_progress_row(model, row);
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS) {
        int patch_kind = ui_task_patch_kind_for_index(model, index);
        if (patch_kind >= 0) {
            ui_px_draw_task_patch_row(model, row, patch_kind);
            return;
        }
    }
    if (model->page == UI_PAGE_FILE_ACTIONS) {
        uint16_t action_count = ui_file_action_count_for_kind(model->action_kind);
        uint16_t detail_start = action_count + 1U;
        if (detail_start < (UI_ROW_COUNT / 2U)) {
            detail_start = UI_ROW_COUNT / 2U;
        }
        if (row >= detail_start) {
            return;
        }
    }
    if (model->page == UI_PAGE_FILES || model->page == UI_PAGE_MUSIC_FILES) {
        ui_px_draw_file_row(model, row);
        return;
    }
    ui_px_fill_row(model, index, row, title, sizeof(title), hint, sizeof(hint));
    title_w = ui_px_text_width(title) + UI_LIST_SELECTOR_MARGIN * 2;
    if (title_w < 18) {
        title_w = 18;
    }
    if (title_w > UI_CANVAS_W - UI_LIST_BAR_W - 2) {
        title_w = UI_CANVAS_W - UI_LIST_BAR_W - 2;
    }
    ui_px_text_clipped(UI_LIST_TEXT_X, y + 4, UI_CANVAS_W - UI_LIST_BAR_W - 12, title, true);
    if (hint[0] != '\0') {
        int32_t hint_w = ui_px_text_width(hint);
        int32_t hint_x = UI_CANVAS_W - UI_LIST_BAR_W - 8 - hint_w;
        if (hint_x > title_w + 4) {
            ui_px_text_clipped(hint_x, y + 4, hint_w, hint, true);
        }
    }
}

static void ui_px_apply_list_dynamic(const ui_model_t *model)
{
    uint16_t count = ui_page_item_count(model);
    uint16_t selected = ui_current_selected(model);
    uint16_t scroll = ui_current_scroll(model);
    int32_t list_h = (model->page == UI_PAGE_BURN_ROM) ?
                         (int32_t)(UI_BURN_ROM_ACTION_ROWS * UI_LIST_LINE_H) :
                         (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H);
    int32_t selected_row = (int32_t)selected - (int32_t)scroll;
    int32_t selector_y = UI_LIST_HEADER_H + selected_row * UI_LIST_LINE_H;
    int32_t selector_w;
    int32_t prev_selector_y = s_anim.list_prev_selector_y;
    int32_t prev_selector_w = s_anim.list_prev_selector_w;
    int32_t redraw_y0;
    int32_t redraw_y1;
    int32_t redraw_row0;
    int32_t redraw_row1;
    int32_t redraw_w;
    int32_t bar_h = 1;
    burner_status_t task_status = {0};
    bool chip_erase_busy = false;

    if (selected_row < 0 || selected_row >= UI_ROW_COUNT) {
        selector_y = UI_LIST_HEADER_H;
    }
    {
        char title[UI_ROW_TEXT_MAX_LEN] = {0};
        char hint[UI_ROW_TEXT_MAX_LEN] = {0};
        ui_px_fill_row(
            model,
            selected,
            (uint16_t)((selected >= scroll) ? (selected - scroll) : 0U),
            title,
            sizeof(title),
            hint,
            sizeof(hint));
        selector_w = ui_px_text_width(title) + UI_LIST_SELECTOR_MARGIN * 2;
        if (model->page == UI_PAGE_TASK_STATUS && !s_task_cancel_confirm) {
            burner_status_snapshot(&task_status);
            chip_erase_busy = ui_chip_erase_busy_active(model, &task_status);
            if (chip_erase_busy &&
                (selected == UI_TASK_ERASE_PROGRESS_ROW || selected == UI_TASK_BURN_PROGRESS_ROW)) {
                selector_w = UI_CANVAS_W - UI_LIST_BAR_W - 2;
            }
        }
        if (selector_w < 22) {
            selector_w = 22;
        }
        if (selector_w > UI_CANVAS_W - UI_LIST_BAR_W - 2) {
            selector_w = UI_CANVAS_W - UI_LIST_BAR_W - 2;
        }
    }
    if (count > 0U) {
        bar_h = (int32_t)(((uint32_t)(selected + 1U) * (uint32_t)list_h) / count);
        if (bar_h < 1) {
            bar_h = 1;
        } else if (bar_h > list_h) {
            bar_h = list_h;
        }
    }

    s_anim.list_selector_target_y = (float)selector_y;
    s_anim.list_selector_target_w = (float)selector_w;
    s_anim.list_bar_target_h = (float)bar_h;
    s_anim.list_bar_target_x = (float)(UI_CANVAS_W - UI_LIST_BAR_W);

    ui_anim_move(&s_anim.list_selector_y, s_anim.list_selector_target_y, UI_LIST_SELECTOR_Y_SPEED);
    ui_anim_move(&s_anim.list_selector_w, s_anim.list_selector_target_w, UI_LIST_SELECTOR_W_SPEED);
    ui_anim_move(&s_anim.list_bar_h, s_anim.list_bar_target_h, UI_LIST_BAR_SPEED);
    ui_anim_move(&s_anim.list_bar_x, s_anim.list_bar_target_x, UI_LIST_BAR_SPEED);

    redraw_y0 = prev_selector_y < (int32_t)s_anim.list_selector_y ? prev_selector_y : (int32_t)s_anim.list_selector_y;
    redraw_y1 = (prev_selector_y + UI_LIST_LINE_H) > ((int32_t)s_anim.list_selector_y + UI_LIST_LINE_H) ?
                    (prev_selector_y + UI_LIST_LINE_H) :
                    ((int32_t)s_anim.list_selector_y + UI_LIST_LINE_H);
    if (redraw_y0 < UI_LIST_HEADER_H) {
        redraw_y0 = UI_LIST_HEADER_H;
    }
    if (redraw_y1 > UI_CANVAS_H - UI_HINT_H) {
        redraw_y1 = UI_CANVAS_H - UI_HINT_H;
    }
    redraw_row0 = (redraw_y0 - UI_LIST_HEADER_H) / UI_LIST_LINE_H;
    redraw_row1 = (redraw_y1 - UI_LIST_HEADER_H) / UI_LIST_LINE_H;
    if (redraw_row0 < 0) {
        redraw_row0 = 0;
    }
    if (redraw_row1 >= UI_ROW_COUNT) {
        redraw_row1 = UI_ROW_COUNT - 1;
    }
    for (int32_t row = redraw_row0; row <= redraw_row1; ++row) {
        ui_px_draw_visible_row(model, (uint16_t)row);
    }

    ui_px_clear_rect(UI_CANVAS_W - UI_LIST_BAR_W, UI_LIST_HEADER_H, UI_LIST_BAR_W, list_h);
    ui_px_hline(UI_CANVAS_W - UI_LIST_BAR_W, UI_LIST_HEADER_H, UI_LIST_BAR_W, true);
    ui_px_hline(UI_CANVAS_W - UI_LIST_BAR_W, UI_CANVAS_H - UI_HINT_H - 1, UI_LIST_BAR_W, true);
    ui_px_vline(UI_CANVAS_W - 3, UI_LIST_HEADER_H, list_h, true);
    ui_px_box((int32_t)s_anim.list_bar_x, UI_LIST_HEADER_H, UI_LIST_BAR_W, (int32_t)s_anim.list_bar_h, true);

    ui_px_invert_rect(0, (int32_t)s_anim.list_selector_y, (int32_t)s_anim.list_selector_w, UI_LIST_LINE_H - 1);
    redraw_w = prev_selector_w > (int32_t)s_anim.list_selector_w ? prev_selector_w : (int32_t)s_anim.list_selector_w;
    ui_px_invalidate_rect(0, redraw_y0, redraw_w + 2, redraw_y1 - redraw_y0);
    ui_px_invalidate_rect(UI_CANVAS_W - UI_LIST_BAR_W, UI_LIST_HEADER_H, UI_LIST_BAR_W, list_h);

    s_anim.list_prev_selector_y = (int32_t)s_anim.list_selector_y;
    s_anim.list_prev_selector_w = (int32_t)s_anim.list_selector_w;
    s_anim.list_prev_bar_h = (int32_t)s_anim.list_bar_h;
    s_anim.list_prev_bar_x = (int32_t)s_anim.list_bar_x;
}

static void ui_px_apply_list_content_dynamic(const ui_model_t *model)
{
    int32_t list_y = UI_LIST_HEADER_H;
    int32_t list_h = (model->page == UI_PAGE_BURN_ROM) ?
                         (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H) :
                         (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H);

    ui_px_clear_rect(0, list_y, UI_CANVAS_W, list_h);
    for (uint16_t row = 0; row < UI_ROW_COUNT; ++row) {
        ui_px_draw_visible_row(model, row);
    }
    ui_px_draw_action_details(model);
    if (model->page == UI_PAGE_BURN_ROM) {
        ui_px_draw_burner_cart_info(model);
    }
    ui_px_hline(UI_CANVAS_W - UI_LIST_BAR_W, UI_LIST_HEADER_H, UI_LIST_BAR_W, true);
    ui_px_hline(UI_CANVAS_W - UI_LIST_BAR_W, UI_CANVAS_H - UI_HINT_H - 1, UI_LIST_BAR_W, true);
    ui_px_vline(UI_CANVAS_W - 3, UI_LIST_HEADER_H, list_h, true);

    ui_px_apply_list_dynamic(model);
    ui_px_invalidate_rect(0, list_y, UI_CANVAS_W, list_h);
}

static void ui_px_render(const ui_model_t *model)
{
    bool full_redraw = false;

    if (s_canvas == NULL || s_canvas_buf == NULL) {
        return;
    }
    if (s_anim.page != model->page) {
        s_anim.page = model->page;
        s_anim.page_changed = true;
        full_redraw = true;
        if (ui_page_is_icon_grid(model->page)) {
            s_anim.tile_fore_y = (float)UI_CANVAS_H;
        } else if (model->page == UI_PAGE_BURNER) {
            s_anim.tile_fore_y = (float)UI_CANVAS_H;
        } else {
            s_anim.list_bar_x = (float)UI_CANVAS_W;
            s_anim.list_selector_y = (float)UI_LIST_HEADER_H;
            s_anim.list_selector_w = 18.0f;
        }
    }
    full_redraw = full_redraw || model->dirty || s_anim.page_changed;
    if (!full_redraw && !model->dirty && !model->motion_dirty && !model->content_dirty &&
        !model->chrome_dirty && !model->music_player_dirty && !model->music_progress_dirty &&
        !ui_anim_active_for_page(model)) {
        return;
    }
    if (!full_redraw) {
        if (ui_settings_adjust_active(model)) {
            if (model->chrome_dirty) {
                ui_px_clear();
            } else {
                ui_px_clear_rect(0, UI_LIST_HEADER_H, UI_CANVAS_W, UI_CANVAS_H - UI_LIST_HEADER_H);
            }
            ui_px_apply_settings_adjust(model);
            if (model->chrome_dirty) {
                ui_px_invalidate_full();
            } else {
                ui_px_invalidate_rect(0, UI_LIST_HEADER_H, UI_CANVAS_W, UI_CANVAS_H - UI_LIST_HEADER_H);
            }
        } else if (model->page == UI_PAGE_WIFI_QR) {
            ui_px_clear();
            ui_px_apply_wifi_qr(model);
            ui_px_invalidate_full();
        } else if (model->page == UI_PAGE_SMB_TEXT_INPUT) {
            ui_px_clear();
            ui_px_apply_smb_text_input(model);
            ui_px_invalidate_full();
        } else if (model->page == UI_PAGE_READER) {
            ui_px_clear();
            ui_px_apply_reader(model);
            ui_px_invalidate_full();
        } else if (model->page == UI_PAGE_MUSIC_PLAYER) {
            ui_px_apply_music_player_dynamic(model);
        } else if (ui_page_is_icon_grid(model->page) && !model->dirty && !model->motion_dirty &&
                   !model->content_dirty && model->chrome_dirty) {
            ui_px_apply_icon_grid_chrome_dynamic(model);
        } else if (!model->dirty && !model->motion_dirty && !model->content_dirty && model->chrome_dirty) {
            ui_px_apply_chrome_dynamic(model);
        } else if (ui_page_is_icon_grid(model->page)) {
            ui_px_apply_tile_dynamic(model);
        } else if (model->page == UI_PAGE_BURNER) {
            ui_px_apply_burner_modes_dynamic(model);
        } else if (model->page == UI_PAGE_BURN_ROM) {
            ui_px_apply_burn_rom_split_dynamic(model);
        } else if (model->content_dirty) {
            ui_px_apply_list_content_dynamic(model);
        } else {
            ui_px_apply_list_dynamic(model);
        }
        s_anim.page_changed = false;
        return;
    }
    ui_px_clear();
    if (ui_settings_adjust_active(model)) {
        ui_px_apply_settings_adjust(model);
    } else if (ui_page_is_icon_grid(model->page)) {
        ui_px_apply_tile(model);
    } else if (model->page == UI_PAGE_BURNER) {
        ui_px_apply_burner_modes(model);
    } else if (model->page == UI_PAGE_BURN_ROM) {
        ui_px_apply_burn_rom_split(model);
    } else if (model->page == UI_PAGE_MUSIC_PLAYER) {
        ui_px_apply_music_player(model);
    } else if (model->page == UI_PAGE_READER) {
        ui_px_apply_reader(model);
    } else if (model->page == UI_PAGE_WIFI_QR) {
        ui_px_apply_wifi_qr(model);
    } else if (model->page == UI_PAGE_SMB_TEXT_INPUT) {
        ui_px_apply_smb_text_input(model);
    } else {
        ui_px_apply_list(model);
    }
    if (!ui_page_is_icon_grid(model->page) && model->page != UI_PAGE_BURNER && model->page != UI_PAGE_MUSIC_PLAYER) {
        s_anim.list_prev_selector_y = (int32_t)s_anim.list_selector_y;
        s_anim.list_prev_selector_w = (int32_t)s_anim.list_selector_w;
        s_anim.list_prev_bar_h = (int32_t)s_anim.list_bar_h;
        s_anim.list_prev_bar_x = (int32_t)s_anim.list_bar_x;
    }
    ui_px_invalidate_full();
    s_anim.page_changed = false;
}

static void ui_apply_snapshot(const ui_model_t *model)
{
    if (model == NULL) {
        return;
    }

    ui_px_render(model);
}

esp_err_t ui_init(void)
{
    lv_obj_t *scr = NULL;
    esp_err_t err;

    if (s_ui_inited) {
        return ESP_OK;
    }

    if (!ui_take_model_lock()) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_model_lock);
    if (!s_home_items_ready) {
        ui_home_load_config();
    }
    ui_sync_burn_settings_from_runtime();
    if (!s_button_map_inited) {
        ui_button_map_init_defaults();
        s_button_map_inited = true;
    }
    if (!ui_ensure_button_queue()) {
        return ESP_ERR_NO_MEM;
    }

    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    err = ui_create_canvas(scr);
    if (err != ESP_OK) {
        return err;
    }

    s_ui_inited = true;
    ui_process();
    ESP_LOGI(UI_TAG, "Astra pixel canvas UI initialized");
    return ESP_OK;
}

void ui_process(void)
{
    static EXT_RAM_BSS_ATTR ui_model_t snapshot;
    bool should_render;

    if (!s_ui_inited) {
        return;
    }

    ui_process_button_queue();
    ui_process_button_repeats();
    ui_refresh_sources();

    if (!ui_take_model_lock()) {
        return;
    }
    ui_music_process_pending_toggle_locked(&s_model);
    ui_update_burn_rom_prompt_locked(&s_model);
    snapshot = s_model;
    should_render = s_model.dirty || s_model.motion_dirty || s_model.content_dirty || s_model.chrome_dirty ||
                    s_model.music_player_dirty || s_model.music_progress_dirty ||
                    ui_anim_active_for_page(&s_model) || s_anim.page != s_model.page ||
                    s_anim.page_changed;
    snapshot.motion_dirty = s_model.motion_dirty;
    snapshot.content_dirty = s_model.content_dirty;
    snapshot.chrome_dirty = s_model.chrome_dirty;
    snapshot.music_player_dirty = s_model.music_player_dirty;
    snapshot.music_progress_dirty = s_model.music_progress_dirty;
    s_model.dirty = false;
    s_model.motion_dirty = false;
    s_model.content_dirty = false;
    s_model.chrome_dirty = false;
    s_model.music_player_dirty = false;
    s_model.music_progress_dirty = false;
    xSemaphoreGive(s_model_lock);

    if (should_render) {
        ui_apply_snapshot(&snapshot);
        s_render_frames_this_second++;
    }

    ui_issue_pending_task_cancel();
}

void ui_mark_activity(void)
{
    uint32_t now_ms = esp_log_timestamp();
    s_last_activity_ms = now_ms;
    s_last_network_activity_ms = now_ms;
    lvgl_port_mark_activity();
    if (s_activity_cb != NULL) {
        s_activity_cb();
    }
}

void ui_mark_network_activity(void)
{
    s_last_network_activity_ms = esp_log_timestamp();
    if (s_activity_cb != NULL) {
        s_activity_cb();
    }
}

uint32_t ui_get_last_activity_ms(void)
{
    return s_last_activity_ms;
}

uint32_t ui_get_last_network_activity_ms(void)
{
    return s_last_network_activity_ms;
}

void ui_set_activity_callback(void (*cb)(void))
{
    s_activity_cb = cb;
}

void ui_post_button(ui_button_t button, bool pressed)
{
    ui_button_event_t event = {
        .button = button,
        .action = UI_INPUT_ACTION_NONE,
        .pressed = pressed,
    };

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_VOL_DOWN) {
        return;
    }
    if (!pressed && s_button_states[(uint8_t)button].action != UI_INPUT_ACTION_NONE) {
        event.action = s_button_states[(uint8_t)button].action;
    } else if (ui_take_model_lock()) {
        event.action = ui_button_action_for_page(s_model.page, button);
        xSemaphoreGive(s_model_lock);
    }
    if (pressed) {
        bool was_dimmed = lvgl_port_is_idle_dimmed();
        ui_mark_activity();
        if (was_dimmed) {
            return;
        }
    }
    if (!ui_ensure_button_queue()) {
        return;
    }
    if (xQueueSend(s_button_queue, &event, 0) != pdTRUE) {
        ui_button_event_t dropped;
        uint32_t now_ms = esp_log_timestamp();

        (void)xQueueReceive(s_button_queue, &dropped, 0);
        if (xQueueSend(s_button_queue, &event, 0) != pdTRUE) {
            if (!pressed) {
                ui_clear_stuck_button_state(button);
            }
            return;
        }
        if (s_last_button_queue_full_log_ms == 0U ||
            (now_ms - s_last_button_queue_full_log_ms) >= 1000U) {
            s_last_button_queue_full_log_ms = now_ms;
            ESP_LOGW(UI_TAG, "UI button queue full, drop oldest button=%d, keep button=%d", (int)dropped.button, (int)button);
        }
    }
}

void ui_handle_button(ui_button_t button, bool pressed)
{
    ui_post_button(button, pressed);
}

void ui_set_wifi_state(ui_wifi_state_t state)
{
    if (state < UI_WIFI_STATE_UNKNOWN || state > UI_WIFI_STATE_CONNECTED) {
        state = UI_WIFI_STATE_UNKNOWN;
    }
    if (!ui_take_model_lock()) {
        return;
    }
    if (s_model.wifi_state != state) {
        s_model.wifi_state = state;
        ui_mark_content_dirty(&s_model);
    }
    xSemaphoreGive(s_model_lock);
}

void ui_set_ip_text(const char *ip)
{
    const char *safe_ip = (ip == NULL || ip[0] == '\0') ? "--" : ip;

    if (!ui_take_model_lock()) {
        return;
    }
    if (strcmp(s_model.ip_text, safe_ip) != 0) {
        snprintf(s_model.ip_text, sizeof(s_model.ip_text), "%s", safe_ip);
        ui_mark_chrome_dirty(&s_model);
        ui_mark_content_dirty(&s_model);
    }
    xSemaphoreGive(s_model_lock);
}

void ui_set_burn_progress(int progress, uint32_t processed, uint32_t total)
{
    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }
    if (total > 0U && processed > total) {
        processed = total;
    }

    if (!ui_take_model_lock()) {
        return;
    }
    s_model.burn_progress = progress;
    s_model.burn_processed = processed;
    s_model.burn_total = total;
    if (processed == 0U) {
        s_model.burn_elapsed_us = 0;
        s_model.erase_progress = 0;
        s_model.erase_done_sectors = 0;
        s_model.erase_total_sectors = 0;
    }
    ui_mark_content_dirty(&s_model);
    xSemaphoreGive(s_model_lock);
}

void ui_show_burn_task_status(uint32_t total_hint)
{
    ui_show_burn_task_status_with_patches(total_hint, false, false, false);
}

void ui_show_burn_task_status_with_patches(
    uint32_t total_hint,
    bool sram_patch,
    bool batteryless_patch,
    bool waitcnt_patch)
{
    if (!ui_take_model_lock()) {
        return;
    }

    ui_task_cancel_confirm_reset_locked();
    ui_begin_task_result_capture_locked(&s_model);
    s_model.page = UI_PAGE_TASK_STATUS;
    s_model.parent_page = UI_PAGE_BURNER;
    s_model.selected = 0;
    s_model.scroll = 0;
    s_model.burn_progress = 0;
    s_model.burn_processed = 0;
    s_model.burn_total = total_hint;
    s_model.erase_progress = 0;
    s_model.erase_done_sectors = 0;
    s_model.erase_total_sectors = 0;
    s_model.burn_elapsed_us = 0;
    s_model.gba_patch_sram = sram_patch;
    s_model.gba_patch_batteryless = batteryless_patch;
    s_model.gba_patch_waitcnt = waitcnt_patch;
    memset(s_model.gba_patch_progress, 0, sizeof(s_model.gba_patch_progress));
    memset(s_model.gba_patch_message, 0, sizeof(s_model.gba_patch_message));
    ui_set_status_locked(&s_model, ui_tr("task status"));
    ui_mark_chrome_dirty(&s_model);
    ui_mark_content_dirty(&s_model);
    xSemaphoreGive(s_model_lock);
}

void ui_set_gba_patch_progress(int kind, int progress, const char *message)
{
    if (kind < 0 || kind >= 3) return;
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    if (!ui_take_model_lock()) return;
    s_model.gba_patch_progress[kind] = progress;
    snprintf(s_model.gba_patch_message[kind], sizeof(s_model.gba_patch_message[kind]), "%s", message != NULL ? message : "");
    ui_mark_content_dirty(&s_model);
    xSemaphoreGive(s_model_lock);
}

void ui_set_status_text(const char *text)
{
    const char *safe_text = (text == NULL) ? "" : text;

    if (ui_burn_status_text_is_terminal(safe_text)) {
        s_file_start_active = false;
    }
    if (!ui_take_model_lock()) {
        return;
    }
    snprintf(s_model.status_text, sizeof(s_model.status_text), "%s", safe_text);
    ui_mark_chrome_dirty(&s_model);
    xSemaphoreGive(s_model_lock);
}

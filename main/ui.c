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
#include "burner_nor_db.h"
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
#include "lcd_display.h"
#include "lvgl.h"
#include "power_manager.h"
#include "usb_msc_tf.h"
#include "wifi_manager.h"
#include "ws_server_internal.h"
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

#define UI_TILE_ICON_AUTO_SIZE (UI_CANVAS_H / 4)
#define UI_TILE_ICON_SIZE ((UI_TILE_ICON_AUTO_SIZE < 44) ? 44 : ((UI_TILE_ICON_AUTO_SIZE > 62) ? 62 : UI_TILE_ICON_AUTO_SIZE))
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

#define UI_ROOT_ITEM_COUNT 6
#define UI_TF_ITEM_COUNT 7
#define UI_WIFI_ITEM_COUNT 5
#define UI_POWER_ITEM_COUNT 6
#define UI_SYSTEM_ITEM_COUNT 7
#define UI_BURNER_MODE_COUNT 2
#define UI_BURN_ROM_LOCKED_ITEM_COUNT 2
#define UI_BURN_ROM_WRITE_PATH_ITEM_COUNT 3
#define UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT 5
#define UI_BURN_ROM_DUMP_KEY_COUNT 13
#define UI_BURN_ROM_GBA_SETTINGS_ITEM_COUNT 3
#define UI_BURN_ROM_MBC5_SETTINGS_ITEM_COUNT 4
#define UI_BURN_ROM_ERASE_CONFIRM_ITEM_COUNT 2
#define UI_BURN_RAM_ITEM_COUNT 7
#define UI_BURN_ROM_CUSTOM_SIZE_TEXT_MAX 16
#define UI_BURN_ROM_GBA_WITH_ROM_ITEM_COUNT 8
#define UI_BURN_ROM_MBC5_WITH_ROM_ITEM_COUNT 9
#define UI_BURN_ROM_ACTION_ROWS UI_LIST_VISIBLE_COUNT
#define UI_BURN_SPLIT_GAP 8
#define UI_BURN_SIDE_MARGIN 4
#define UI_BURN_INFO_W 146
#define UI_BURN_OPS_W (UI_CANVAS_W - (UI_BURN_SIDE_MARGIN * 2) - UI_BURN_SPLIT_GAP - UI_BURN_INFO_W)
#define UI_BURN_SAVE_ITEM_COUNT 8
#define UI_SETTINGS_ITEM_COUNT 10
#define UI_TASK_STATUS_ITEM_COUNT 12
#define UI_TASK_CANCEL_CONFIRM_ITEM_COUNT 2U
#define UI_TASK_ERASE_PROGRESS_ROW 6U
#define UI_TASK_BURN_PROGRESS_ROW 7U
#define UI_ROW_COUNT UI_LIST_VISIBLE_COUNT
#define UI_ROW_TEXT_MAX_LEN 96
#define UI_FILE_NAME_MAX_LEN 128
#define UI_FILE_WINDOW_COUNT (UI_ROW_COUNT * 3U)
#define UI_FILE_SCAN_LIMIT 512U
#define UI_FILE_START_TASK_STACK_SIZE (16U * 1024U)
#define UI_FILE_START_TASK_PRIORITY 4
#define UI_WIFI_TASK_STACK_SIZE 4096U
#define UI_WIFI_TASK_PRIORITY 3
#define UI_STORAGE_TASK_STACK_SIZE 4096U
#define UI_STORAGE_TASK_PRIORITY 3
#define UI_BURN_WORK_TASK_STACK_SIZE (16U * 1024U)
#define UI_BURN_PROBE_TASK_STACK_SIZE (24U * 1024U)
#define UI_SYSTEM_TASK_CORE_ID 0
#define UI_BURN_TASK_CORE_ID 1
#define UI_BUTTON_QUEUE_LEN 16
#define UI_BUTTONS_PER_FRAME 8
#define UI_BUTTON_REPEAT_START_FRAMES 16U
#define UI_BUTTON_REPEAT_INTERVAL_FRAMES 3U
#define UI_BUTTON_COUNT ((uint8_t)UI_BUTTON_MENU + 1U)
#define UI_CLOCK_REFRESH_MS 1000U
#define UI_FPS_REFRESH_MS 1000U
#define UI_BATTERY_REFRESH_MS 5000U
#define UI_LIVE_REFRESH_MS 250U
#define UI_FILE_PSRAM_WINDOW_MB 4U
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
    UI_PAGE_TF,
    UI_PAGE_FILES,
    UI_PAGE_FILE_ACTIONS,
    UI_PAGE_WIFI,
    UI_PAGE_POWER,
    UI_PAGE_BURNER,
    UI_PAGE_BURN_ROM,
    UI_PAGE_BURN_SAVE,
    UI_PAGE_SETTINGS,
    UI_PAGE_TASK_STATUS,
} ui_page_t;

typedef enum {
    UI_ACTION_OPEN_SYSTEM = 0,
    UI_ACTION_OPEN_TF,
    UI_ACTION_OPEN_WIFI,
    UI_ACTION_OPEN_POWER,
    UI_ACTION_OPEN_BURNER,
    UI_ACTION_OPEN_SETTINGS,
} ui_action_t;

typedef enum {
    UI_FILE_KIND_UNSUPPORTED = 0,
    UI_FILE_KIND_ROM_GBA,
    UI_FILE_KIND_ROM_MBC5,
    UI_FILE_KIND_SAVE,
} ui_file_kind_t;

typedef enum {
    UI_FILE_FILTER_NONE = 0,
    UI_FILE_FILTER_ROM_GBA,
    UI_FILE_FILTER_ROM_MBC5,
    UI_FILE_FILTER_SAVE,
} ui_file_filter_t;

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
    UI_BURN_ROM_OP_ANALYZE = 0,
    UI_BURN_ROM_OP_CHOOSE_ROM,
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
    UI_BURN_ROM_OP_SETTINGS,
    UI_BURN_ROM_OP_INVALID,
} ui_burn_rom_op_t;

typedef enum {
    UI_BURN_ROM_SUBMENU_NONE = 0,
    UI_BURN_ROM_SUBMENU_WRITE,
    UI_BURN_ROM_SUBMENU_DUMP_SIZE,
    UI_BURN_ROM_SUBMENU_DUMP_CUSTOM,
    UI_BURN_ROM_SUBMENU_SETTINGS,
    UI_BURN_ROM_SUBMENU_ERASE_CONFIRM,
    UI_BURN_ROM_SUBMENU_RAM,
} ui_burn_rom_submenu_t;

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
    uint32_t slot;
    burner_write_path_t write_path;
    bool erase_always;
    uint32_t psram_mb;
    uint32_t mbc5_chunk_kb;
    bool gba_force_no_cfi;
    burner_gba_save_type_t gba_save_type;
    uint32_t gba_save_size;
    bool ram_fram;
    uint8_t ram_latency;
    bool task_with_caps;
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
    uint8_t battery_percent;
    bool battery_valid;
    bool battery_charging;
    bool dirty;
    bool motion_dirty;
    bool content_dirty;
    bool chrome_dirty;
} ui_model_t;

typedef struct {
    ui_button_t button;
    bool pressed;
} ui_button_event_t;

typedef struct {
    bool pressed;
    uint16_t frames_until_repeat;
} ui_button_state_t;

typedef enum {
    UI_WORK_WIFI_CONNECT_SAVED = 0,
    UI_WORK_WIFI_START_AP,
    UI_WORK_WIFI_DISCONNECT,
    UI_WORK_WIFI_CLEAR_SAVED,
    UI_WORK_STORAGE_USB_ENABLE,
    UI_WORK_STORAGE_USB_DISABLE,
    UI_WORK_BURN_READ_ID,
    UI_WORK_BURN_ERASE_CHIP,
    UI_WORK_BURN_DUMP_ROM,
    UI_WORK_BURN_DUMP_SAVE,
    UI_WORK_BURN_UNLOCK_PPB,
    UI_WORK_DEVICE_RESTART,
    UI_WORK_BRIGHTNESS_UP,
    UI_WORK_BRIGHTNESS_DOWN,
} ui_work_type_t;

typedef struct {
    ui_work_type_t type;
    burner_cart_mode_t cart_mode;
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
    ui_page_t page;
    bool page_changed;
    int32_t list_prev_selector_y;
    int32_t list_prev_selector_w;
    int32_t list_prev_bar_h;
    int32_t list_prev_bar_x;
} ui_anim_state_t;

static SemaphoreHandle_t s_model_lock = NULL;
static QueueHandle_t s_button_queue = NULL;
static ui_button_state_t s_button_states[UI_BUTTON_COUNT];
static uint16_t s_root_selected = 0;
static bool s_ui_inited = false;
static uint8_t s_ui_language = UI_LANGUAGE_DEFAULT;
static bool s_file_start_active = false;
static bool s_wifi_work_active = false;
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

static lv_obj_t *s_canvas = NULL;
static uint16_t *s_canvas_buf = NULL;

static void ui_task_cancel_confirm_reset_locked(void);
static void ui_task_cancel_confirm_open_locked(ui_model_t *model);
static void ui_task_cancel_confirm_close_locked(ui_model_t *model, const char *status_key);
static void ui_issue_pending_task_cancel(void);
static bool ui_task_status_operation_active(void);
static ui_page_t ui_task_return_parent_page(ui_page_t page);

static ui_model_t s_model = {
    .page = UI_PAGE_ROOT,
    .parent_page = UI_PAGE_ROOT,
    .selected = 0,
    .scroll = 0,
    .file_path = "",
    .wifi_state = UI_WIFI_STATE_UNKNOWN,
    .ip_text = "--",
    .status_text = "system initializing",
    .time_text = "--:--",
    .fps_text = "FPS --",
    .dirty = true,
};

static const ui_menu_item_t s_root_items[UI_ROOT_ITEM_COUNT] = {
    {.title = "System", .hint = "web overview", .symbol = "SYS", .action = UI_ACTION_OPEN_SYSTEM, .accent = UI_COLOR_WHITE},
    {.title = "TF", .hint = "files and USB", .symbol = "TF", .action = UI_ACTION_OPEN_TF, .accent = UI_COLOR_WHITE},
    {.title = "Wi-Fi", .hint = "network setup", .symbol = "WIFI", .action = UI_ACTION_OPEN_WIFI, .accent = UI_COLOR_WHITE},
    {.title = "Power", .hint = "battery telemetry", .symbol = "PWR", .action = UI_ACTION_OPEN_POWER, .accent = UI_COLOR_WHITE},
    {.title = "Burner", .hint = "cart operations", .symbol = "BURN", .action = UI_ACTION_OPEN_BURNER, .accent = UI_COLOR_WHITE},
    {.title = "Settings", .hint = "device tools", .symbol = "CFG", .action = UI_ACTION_OPEN_SETTINGS, .accent = UI_COLOR_WHITE},
};

static bool ui_take_model_lock(void);
static bool ui_file_entry_for_visible_row(const ui_model_t *model, uint16_t row, ui_file_entry_t *entry_out);
static int32_t ui_file_name_col_w(void);
static uint16_t ui_scroll_for_selected_rows(uint16_t selected, uint16_t scroll, uint16_t count, uint16_t visible_rows);
static uint16_t ui_burn_rom_visible_rows(void);
static ui_burn_rom_op_t ui_burn_rom_op_for_index(uint16_t index);
static bool ui_burn_cart_has_slots(void);
static const char *ui_mbc5_voltage_label(void);

static bool ui_lang_is_zh(void)
{
    return s_ui_language == UI_LANGUAGE_ZH;
}

static const char *ui_tr(const char *key)
{
    return ui_text_lookup(s_ui_language, key);
}

static const char *ui_root_item_title(const ui_menu_item_t *item)
{
    if (item == NULL) {
        return "";
    }
    switch (item->action) {
        case UI_ACTION_OPEN_SYSTEM:
            return ui_tr("System");
        case UI_ACTION_OPEN_TF:
            return "TF";
        case UI_ACTION_OPEN_WIFI:
            return "Wi-Fi";
        case UI_ACTION_OPEN_POWER:
            return ui_tr("Power");
        case UI_ACTION_OPEN_BURNER:
            return ui_tr("Burner");
        case UI_ACTION_OPEN_SETTINGS:
            return ui_tr("Settings");
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
        case UI_ACTION_OPEN_TF:
            return ui_tr("files and USB");
        case UI_ACTION_OPEN_WIFI:
            return ui_tr("network setup");
        case UI_ACTION_OPEN_POWER:
            return ui_tr("battery telemetry");
        case UI_ACTION_OPEN_BURNER:
            return ui_tr("cart operations");
        case UI_ACTION_OPEN_SETTINGS:
            return ui_tr("device tools");
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

static ui_anim_state_t s_anim = {
    .tile_fore_y = (float)UI_CANVAS_H,
    .tile_fore_target_y = 0.0f,
    .list_bar_x = (float)UI_CANVAS_W,
    .list_bar_target_x = (float)(UI_CANVAS_W - UI_LIST_BAR_W),
    .marquee_selected = UINT16_MAX,
    .burner_prev_selected = UINT16_MAX,
    .page = UI_PAGE_ROOT,
    .page_changed = true,
};
static ui_nav_entry_t s_nav_stack[8];
static uint8_t s_nav_depth = 0;
static burner_cart_mode_t s_cart_mode = BURNER_CART_MODE_GBA;
static bool s_burner_info_left = true;
static burner_write_path_t s_write_path = BURNER_WRITE_PATH_PIPELINE;
static bool s_ram_fram = false;
static burner_gba_save_type_t s_gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
static uint32_t s_cart_slot = 0;
static uint32_t s_rom_size_mib = 32;
static uint32_t s_save_size_kib = 128;
static uint32_t s_psram_mb = BURN_PSRAM_WINDOW_AUTO_MB;
static uint32_t s_mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
static uint32_t s_dump_chunk_kb = BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
static uint8_t s_ram_latency = 10;
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
static bool ui_burner_operation_active(void);
static void ui_focus_burn_rom_row_locked(ui_model_t *model, uint16_t row);
static const char *ui_probe_chip_name(const burner_status_t *status);

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
    return true;
}

static void ui_set_status_locked(ui_model_t *model, const char *text)
{
    if (model == NULL) {
        return;
    }
    snprintf(model->status_text, sizeof(model->status_text), "%s", (text != NULL) ? text : "");
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

static void ui_px_set(int32_t x, int32_t y, bool on)
{
    if (s_canvas_buf == NULL || x < 0 || y < 0 || x >= UI_CANVAS_W || y >= UI_CANVAS_H) {
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

static uint16_t ui_burn_rom_settings_item_count(void)
{
    return (s_cart_mode == BURNER_CART_MODE_MBC5) ?
               UI_BURN_ROM_MBC5_SETTINGS_ITEM_COUNT :
               UI_BURN_ROM_GBA_SETTINGS_ITEM_COUNT;
}

static const char *ui_mbc5_voltage_label(void)
{
    return (s_mbc5_power_5v_enabled != 0u) ? "5V" : "3V3";
}

static const char *ui_erase_mode_label(void)
{
    return (s_burn_erase_always != 0u) ? "Force erase" : "Smart skip";
}

static const char *ui_gba_save_type_label(burner_gba_save_type_t save_type)
{
    switch (save_type) {
        case BURNER_GBA_SAVE_TYPE_SRAM:
            return "SRAM";
        case BURNER_GBA_SAVE_TYPE_EEPROM:
            return "EEPROM";
        case BURNER_GBA_SAVE_TYPE_FLASH:
            return "FLASH";
        case BURNER_GBA_SAVE_TYPE_BATTERYLESS:
            return "BATTERYLESS";
        default:
            return "UNKNOWN";
    }
}

static const char *ui_gba_save_type_probe_label(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid || status->probe_cart_mode != BURNER_CART_MODE_GBA ||
        !status->probe_gba_save_detected) {
        return ui_tr("unknown");
    }
    return ui_gba_save_type_label(status->probe_gba_save_type);
}

static const char *ui_gba_sram_patch_label(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid || status->probe_cart_mode != BURNER_CART_MODE_GBA ||
        !status->probe_gba_sram_patch_scanned) {
        return ui_tr("Not scanned");
    }
    if (!status->probe_gba_sram_patch_detected) {
        return ui_tr("Not patched");
    }
    switch (status->probe_gba_sram_patch_kind) {
        case BURNER_GBA_SRAM_PATCH_GBATA:
            return ui_tr("GBATA SRAM patched");
        case BURNER_GBA_SRAM_PATCH_FLASH1M_REPRO:
            return ui_tr("Flash1M Repro SRAM patched");
        case BURNER_GBA_SRAM_PATCH_NONE:
        default:
            return ui_tr("Patched");
    }
}

static const char *ui_cart_mode_label(burner_cart_mode_t mode)
{
    return (mode == BURNER_CART_MODE_GBA) ? "GBA" : "GBC";
}

static bool ui_cart_is_unlocked(void)
{
    return s_cart_analyzed && s_analyzed_cart_mode == s_cart_mode;
}

static ui_file_action_t ui_burn_action_for_write_path(burner_write_path_t path)
{
    switch (path) {
        case BURNER_WRITE_PATH_PSRAM:
            return UI_FILE_ACTION_BURN_PSRAM;
        case BURNER_WRITE_PATH_PIPELINE:
            return UI_FILE_ACTION_BURN_PIPELINE;
        case BURNER_WRITE_PATH_DIRECT:
        default:
            return UI_FILE_ACTION_BURN_DIRECT;
    }
}

static void ui_format_bytes_text(uint32_t bytes, char *out, size_t out_len)
{
    ui_format_file_size(bytes, out, out_len);
}

static void ui_format_probe_id(const burner_status_t *status, char *out, size_t out_len)
{
    size_t id_len;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (status == NULL || !status->probe_valid) {
        snprintf(out, out_len, "--");
        return;
    }

    id_len = (status->probe_cart_mode == BURNER_CART_MODE_GBA) ? 8U : 4U;
    if (id_len > sizeof(status->probe_id)) {
        id_len = sizeof(status->probe_id);
    }
    out[0] = '\0';
    for (size_t i = 0; i < id_len; ++i) {
        char part[4];
        snprintf(part, sizeof(part), "%02X", status->probe_id[i]);
        strncat(out, part, out_len - strlen(out) - 1U);
        if (i + 1U < id_len) {
            strncat(out, " ", out_len - strlen(out) - 1U);
        }
    }
}

static void ui_format_gba_d0d1_text(const burner_status_t *status, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (status == NULL || !status->probe_valid || status->probe_cart_mode != BURNER_CART_MODE_GBA) {
        snprintf(out, out_len, "--");
    } else if (!status->probe_gba_d0d1_known) {
        snprintf(out, out_len, "%s", ui_tr("unknown"));
    } else {
        snprintf(out, out_len, "%s", status->probe_gba_d0d1_swapped ? ui_tr("swapped") : ui_tr("normal"));
    }
}

static void ui_format_u64_size(uint64_t bytes, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%" PRIu64 " B", bytes);
    }
}

static void ui_format_elapsed(uint64_t elapsed_us, char *out, size_t out_len)
{
    uint64_t seconds = elapsed_us / 1000000ULL;
    uint64_t minutes = seconds / 60ULL;
    uint64_t hours = minutes / 60ULL;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (hours > 0ULL) {
        snprintf(out, out_len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, minutes % 60ULL, seconds % 60ULL);
    } else {
        snprintf(out, out_len, "%02" PRIu64 ":%02" PRIu64, minutes, seconds % 60ULL);
    }
}

static void ui_format_uptime(uint64_t uptime_ms, char *out, size_t out_len)
{
    uint64_t seconds = uptime_ms / 1000ULL;
    uint64_t hours = seconds / 3600ULL;
    uint64_t minutes = (seconds / 60ULL) % 60ULL;
    uint64_t secs = seconds % 60ULL;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (hours > 99ULL) {
        snprintf(out, out_len, "%" PRIu64 "d %02" PRIu64 "h", hours / 24ULL, hours % 24ULL);
    } else {
        snprintf(out, out_len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, minutes, secs);
    }
}

static uint32_t ui_next_option_u32(const uint32_t *options, size_t count, uint32_t current, int delta)
{
    size_t index = 0;

    if (options == NULL || count == 0U) {
        return current;
    }
    for (size_t i = 0; i < count; ++i) {
        if (options[i] == current) {
            index = i;
            break;
        }
    }
    if (delta >= 0) {
        index = (index + 1U) % count;
    } else {
        index = (index == 0U) ? (count - 1U) : (index - 1U);
    }
    return options[index];
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

static bool ui_anim_value_active(float value, float target)
{
    float diff = target - value;

    return diff > 0.5f || diff < -0.5f;
}

static bool ui_anim_active_for_page(const ui_model_t *model)
{
    ui_file_entry_t entry = {0};
    int32_t name_w;

    if (model == NULL) {
        return false;
    }
    if (model->page == UI_PAGE_ROOT) {
        return ui_anim_value_active(s_anim.tile_camera_x, s_anim.tile_camera_target_x) ||
               ui_anim_value_active(s_anim.tile_bar_w, s_anim.tile_bar_target_w) ||
               ui_anim_value_active(s_anim.tile_fore_y, s_anim.tile_fore_target_y);
    }
    if (model->page == UI_PAGE_BURNER) {
        return ui_anim_value_active(s_anim.tile_bar_w, s_anim.tile_bar_target_w) ||
               ui_anim_value_active(s_anim.tile_fore_y, s_anim.tile_fore_target_y);
    }
    if (model->page == UI_PAGE_FILES &&
        model->file_selected >= model->file_scroll &&
        model->file_selected < model->file_scroll + UI_ROW_COUNT &&
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
    return (model->page == UI_PAGE_FILES) ? model->file_selected : model->selected;
}

static uint16_t ui_current_scroll(const ui_model_t *model)
{
    if (model == NULL) {
        return 0;
    }
    return (model->page == UI_PAGE_FILES) ? model->file_scroll : model->scroll;
}

static uint16_t ui_burn_rom_item_count(void)
{
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
        return UI_BURN_ROM_WRITE_PATH_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_SIZE) {
        return (uint16_t)(UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT + (ui_burn_cart_has_slots() ? 1U : 0U));
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        return UI_BURN_ROM_DUMP_KEY_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
        return ui_burn_rom_settings_item_count();
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_ERASE_CONFIRM) {
        return UI_BURN_ROM_ERASE_CONFIRM_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RAM) {
        return UI_BURN_RAM_ITEM_COUNT;
    }
    if (!ui_cart_is_unlocked()) {
        return UI_BURN_ROM_LOCKED_ITEM_COUNT;
    }
    if (s_cart_mode == BURNER_CART_MODE_GBA) {
        return UI_BURN_ROM_GBA_WITH_ROM_ITEM_COUNT;
    }
    return UI_BURN_ROM_MBC5_WITH_ROM_ITEM_COUNT;
}

static size_t ui_cart_mode_index(burner_cart_mode_t mode)
{
    return (mode == BURNER_CART_MODE_GBA) ? 1U : 0U;
}

static const ui_file_entry_t *ui_last_rom_file_for_mode(burner_cart_mode_t mode)
{
    return &s_last_rom_file_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_entry_t *ui_last_rom_file_for_mode_mut(burner_cart_mode_t mode)
{
    return &s_last_rom_file_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_kind_t ui_last_rom_kind_for_mode(burner_cart_mode_t mode)
{
    return s_last_rom_kind_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_kind_t *ui_last_rom_kind_for_mode_mut(burner_cart_mode_t mode)
{
    return &s_last_rom_kind_by_cart[ui_cart_mode_index(mode)];
}

static void ui_focus_burn_rom_row_locked(ui_model_t *model, uint16_t row)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }

    count = ui_burn_rom_item_count();
    if (count == 0U) {
        model->selected = 0;
        model->scroll = 0;
        ui_mark_content_dirty(model);
        return;
    }
    if (row >= count) {
        row = (uint16_t)(count - 1U);
    }

    model->selected = row;
    model->scroll = ui_scroll_for_selected_rows(row, 0, count, ui_burn_rom_visible_rows());
    ui_mark_content_dirty(model);
}

static bool ui_burn_cart_has_slots(void)
{
    burner_status_t status = {0};

    burner_status_snapshot(&status);
    return status.probe_valid &&
           status.probe_cart_mode == s_cart_mode &&
           status.probe_device_size > (128U * 1024U * 1024U);
}

static void ui_clamp_burn_rom_selection_locked(ui_model_t *model)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }
    count = ui_burn_rom_item_count();
    if (count == 0U) {
        model->selected = 0;
        model->scroll = 0;
        return;
    }
    if (model->selected >= count) {
        model->selected = (uint16_t)(count - 1U);
    }
    model->scroll = ui_scroll_for_selected_rows(model->selected, model->scroll, count, ui_burn_rom_visible_rows());
}

static uint16_t ui_scroll_for_selected(uint16_t selected, uint16_t scroll, uint16_t count)
{
    return ui_scroll_for_selected_rows(selected, scroll, count, UI_ROW_COUNT);
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
        case UI_FILE_KIND_UNSUPPORTED:
        default:
            return 4U;
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
            return UI_FILE_ACTION_BURN_PIPELINE;
        case 1:
            return UI_FILE_ACTION_BURN_PSRAM;
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

static const char *ui_page_title(ui_page_t page)
{
    switch (page) {
        case UI_PAGE_ROOT:
            return "MORI";
        case UI_PAGE_SYSTEM:
            return ui_tr("System");
        case UI_PAGE_TF:
            return "TF";
        case UI_PAGE_FILES:
            return ui_tr("Files");
        case UI_PAGE_FILE_ACTIONS:
            return ui_tr("Actions");
        case UI_PAGE_WIFI:
            return "Wi-Fi";
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
        model->file_scroll = ui_scroll_for_selected(model->file_selected, model->file_scroll, total);
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

    if (model == NULL) {
        return false;
    }
    if (model->file_total == 0U) {
        return true;
    }
    visible_end = model->file_scroll + UI_ROW_COUNT;
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
            model->file_scroll = ui_scroll_for_selected(model->file_selected, model->file_scroll, model->file_total);
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
        model->file_scroll = ui_scroll_for_selected(model->file_selected, model->file_scroll, model->file_total);
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
    model->page = UI_PAGE_FILES;
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
    ui_open_files_locked(model, path, reset_selection);
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

static void ui_select_file_for_burner_locked(ui_model_t *model, const ui_file_entry_t *entry)
{
    ui_file_kind_t kind;

    if (model == NULL || entry == NULL) {
        return;
    }
    kind = ui_file_kind_from_name(entry->name);
    if (kind == UI_FILE_KIND_UNSUPPORTED) {
        ui_set_status_locked(model, ui_tr("unsupported file"));
        return;
    }
    if (!ui_file_kind_matches_filter(kind, model->file_filter)) {
        ui_set_status_locked(model, ui_tr("file type mismatch"));
        return;
    }
    model->action_kind = kind;
    model->action_file = *entry;
    model->file_filter = UI_FILE_FILTER_NONE;
    if (kind == UI_FILE_KIND_SAVE) {
        s_last_save_file = *entry;
        s_burn_rom_write_menu = false;
        model->page = UI_PAGE_BURN_ROM;
        model->parent_page = UI_PAGE_BURNER;
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_RAM;
        ui_set_status_locked(model, ui_tr("save selected"));
        ui_focus_burn_rom_row_locked(model, 1U);
    } else {
        *ui_last_rom_file_for_mode_mut(s_cart_mode) = *entry;
        *ui_last_rom_kind_for_mode_mut(s_cart_mode) = kind;
        s_burn_rom_write_menu = false;
        model->page = UI_PAGE_BURN_ROM;
        model->parent_page = UI_PAGE_BURNER;
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
        ui_set_status_locked(model, ui_tr("ROM selected"));
        ui_focus_burn_rom_row_locked(model, 2U);
    }
    ui_clamp_burn_rom_selection_locked(model);
    ui_drop_nav_target_locked(model->page);
    model->dirty = true;
}

static esp_err_t ui_prepare_file_action_locked(ui_model_t *model, ui_file_start_request_t **request_out)
{
    ui_file_start_request_t *request = NULL;
    ui_file_action_t action;

    if (request_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *request_out = NULL;
    if (model == NULL || model->page != UI_PAGE_FILE_ACTIONS) {
        return ESP_ERR_INVALID_STATE;
    }
    if (model->selected >= ui_file_action_count_for_kind(model->action_kind)) {
        ui_set_status_locked(model, ui_tr("file details"));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_file_start_active) {
        ui_set_status_locked(model, ui_tr("task starting"));
        return ESP_ERR_INVALID_STATE;
    }
    if (card == NULL) {
        ui_set_status_locked(model, ui_tr("TF card not ready"));
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, ui_tr("TF busy by USB host"));
        return ESP_ERR_INVALID_STATE;
    }

    action = ui_file_action_for_kind(model->action_kind, (uint8_t)model->selected);
    request = (ui_file_start_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        ui_set_status_locked(model, ui_tr("no memory"));
        return ESP_ERR_NO_MEM;
    }

    ui_utf8_safe_copy(request->name, sizeof(request->name), model->action_file.name);
    snprintf(request->path, sizeof(request->path), "%s", model->action_file.path);
    request->size = model->action_file.size;
    request->kind = model->action_kind;
    request->action = action;
    if (model->page == UI_PAGE_BURN_ROM) {
        request->cart_mode = s_cart_mode;
    } else {
        request->cart_mode = ui_file_cart_mode_for_kind(model->action_kind);
    }
    request->slot = 0;
    request->write_path = (action == UI_FILE_ACTION_BURN_PSRAM) ?
                              BURNER_WRITE_PATH_PSRAM :
                              ((action == UI_FILE_ACTION_BURN_PIPELINE) ?
                                   BURNER_WRITE_PATH_PIPELINE :
                                   BURNER_WRITE_PATH_DIRECT);
    request->erase_always = (s_burn_erase_always != 0u);
    request->psram_mb = BURN_PSRAM_WINDOW_AUTO_MB;
    request->mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
    request->gba_force_no_cfi = false;
    request->gba_save_type = s_gba_save_type;
    request->gba_save_size = model->action_file.size;
    request->ram_fram = false;
    request->ram_latency = 10U;

    s_file_start_active = true;
    ui_task_cancel_confirm_reset_locked();
    model->page = UI_PAGE_TASK_STATUS;
    model->parent_page = UI_PAGE_ROOT;
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

static void ui_finish_file_start_task(
    const ui_file_start_request_t *request,
    esp_err_t err,
    const burner_task_start_result_t *result,
    const char *error_msg)
{
    const char *label = (request != NULL) ? ui_file_action_label(request->action) : "burn";
    const char *msg = (error_msg != NULL && error_msg[0] != '\0') ? error_msg : esp_err_to_name(err);

    if (!ui_take_model_lock()) {
        return;
    }

    s_file_start_active = false;
    ui_task_cancel_confirm_reset_locked();
    s_model.page = UI_PAGE_TASK_STATUS;
    s_model.parent_page = UI_PAGE_ROOT;
    if (err == ESP_OK) {
        s_model.burn_progress = 0;
        s_model.burn_processed = 0;
        s_model.burn_total = (result != NULL) ? result->effective_size : 0U;
        s_model.erase_progress = 0;
        s_model.erase_done_sectors = 0;
        s_model.erase_total_sectors = 0;
        s_model.burn_elapsed_us = 0;
        snprintf(s_model.status_text, sizeof(s_model.status_text), "%s%s", label, ui_tr(" started"));
    } else {
        s_model.burn_progress = 0;
        s_model.burn_processed = 0;
        s_model.burn_total = 0;
        s_model.erase_progress = 0;
        s_model.erase_done_sectors = 0;
        s_model.erase_total_sectors = 0;
        s_model.burn_elapsed_us = 0;
        snprintf(
            s_model.status_text,
            sizeof(s_model.status_text),
            "%s%.72s",
            ui_tr("start failed: "),
            msg);
        ESP_LOGW(UI_TAG, "LCD file action start failed (%s): %s", label, msg);
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_start_file_action_task(void *param)
{
    ui_file_start_request_t *request = (ui_file_start_request_t *)param;
    burner_task_start_result_t result = {0};
    char error_msg[160] = {0};
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool task_with_caps = false;

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = request->task_with_caps;

    err = burner_backend_init();
    if (err != ESP_OK) {
        snprintf(error_msg, sizeof(error_msg), "burn backend init failed: %s", esp_err_to_name(err));
        ui_finish_file_start_task(request, err, &result, error_msg);
        free(request);
        if (task_with_caps) {
            vTaskDeleteWithCaps(NULL);
        } else {
            vTaskDelete(NULL);
        }
        return;
    }

    ESP_LOGI(
        UI_TAG,
        "LCD start file action: action=%s path=%s mode=%s write_path=%s",
        ui_file_action_label(request->action),
        request->path,
        (request->cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
        burner_write_path_to_str(request->write_path));

    switch (request->action) {
        case UI_FILE_ACTION_BURN_PSRAM:
        case UI_FILE_ACTION_BURN_PIPELINE:
        case UI_FILE_ACTION_BURN_DIRECT:
            err = burner_start_write_from_tf(
                request->path,
                request->cart_mode,
                request->slot,
                request->write_path,
                request->erase_always,
                request->psram_mb,
                request->mbc5_chunk_kb,
                request->gba_force_no_cfi,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_VERIFY_ROM:
            err = burner_start_verify_from_tf(
                request->path,
                request->cart_mode,
                request->slot,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_WRITE_SAVE:
            err = burner_start_ram_write_from_tf(
                request->path,
                request->slot,
                request->ram_fram,
                request->ram_latency,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_VERIFY_SAVE:
            err = burner_start_ram_verify_from_tf(
                request->path,
                request->slot,
                request->ram_fram,
                request->ram_latency,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_WRITE_GBA_SAVE_NEW:
            err = burner_start_gba_save_write_from_tf_new(
                request->path,
                request->gba_save_type,
                request->gba_save_size,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW:
            err = burner_start_gba_save_verify_from_tf_new(
                request->path,
                request->gba_save_type,
                request->gba_save_size,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        default:
            err = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    ESP_LOGI(UI_TAG, "LCD file action task stack free min=%u bytes", (unsigned)uxTaskGetStackHighWaterMark2(NULL));
    ui_finish_file_start_task(request, err, &result, error_msg);
    free(request);
    if (task_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static void ui_start_file_action_async(ui_file_start_request_t *request)
{
    BaseType_t ret;

    if (request == NULL) {
        return;
    }

    request->task_with_caps = true;
    ret = xTaskCreatePinnedToCoreWithCaps(
        ui_start_file_action_task,
        "ui_file_start",
        UI_FILE_START_TASK_STACK_SIZE,
        request,
        UI_FILE_START_TASK_PRIORITY,
        NULL,
        UI_BURN_TASK_CORE_ID,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        return;
    }

    request->task_with_caps = false;
    ret = xTaskCreatePinnedToCore(
        ui_start_file_action_task,
        "ui_file_start",
        UI_FILE_START_TASK_STACK_SIZE,
        request,
        UI_FILE_START_TASK_PRIORITY,
        NULL,
        UI_BURN_TASK_CORE_ID);
    if (ret != pdPASS) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGW(
            UI_TAG,
            "LCD file action task create failed: ret=%d stack=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
            (int)ret,
            (unsigned)UI_FILE_START_TASK_STACK_SIZE,
            (unsigned)internal_free,
            (unsigned)internal_largest,
            (unsigned)psram_free,
            (unsigned)psram_largest);
        ui_finish_file_start_task(request, ESP_ERR_NO_MEM, NULL, "create start task failed");
        free(request);
    }
}

static void ui_set_task_page_starting(const char *text, uint32_t total)
{
    if (!ui_take_model_lock()) {
        return;
    }
    ui_task_cancel_confirm_reset_locked();
    s_model.page = UI_PAGE_TASK_STATUS;
    s_model.parent_page = UI_PAGE_BURNER;
    s_model.burn_progress = 0;
    s_model.burn_processed = 0;
    s_model.burn_total = total;
    s_model.erase_progress = 0;
    s_model.erase_done_sectors = 0;
    s_model.erase_total_sectors = 0;
    s_model.burn_elapsed_us = 0;
    ui_set_status_locked(&s_model, text);
    xSemaphoreGive(s_model_lock);
}

static esp_err_t ui_start_dump_rom_task(void)
{
    char timestamp[32] = {0};
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    uint32_t read_size = s_rom_size_mib * 1024U * 1024U;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    uint32_t dump_chunk_bytes = burner_dump_chunk_kb_to_bytes(s_dump_chunk_kb);
    bool gba_force_multi = false;
    esp_err_t err;

    if (card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_mode == BURNER_CART_MODE_GBA && (read_size & 0x1U) != 0U) {
        read_size++;
    }
    if (s_cart_mode == BURNER_CART_MODE_GBA) {
        err = burner_apply_gba_slot_limit(s_cart_slot, read_size, &addr_begin, &effective_size, &gba_force_multi);
    } else {
        err = burner_apply_mbc5_slot_limit(false, s_cart_slot, read_size, &addr_begin, &effective_size);
    }
    if (err != ESP_OK) {
        return err;
    }
    err = burner_ensure_rom_output_dir();
    if (err != ESP_OK) {
        return err;
    }
    burner_build_output_timestamp(timestamp, sizeof(timestamp));
    snprintf(safe_name, sizeof(safe_name), "cart_%s%s", timestamp, burner_rom_dump_ext_for_mode(s_cart_mode));
    err = burner_resolve_unique_output_path(
        ROM_OUTPUT_DIR_PATH,
        safe_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }
    err = burner_start_task_ex(
        BURNER_JOB_READ_ROM,
        s_cart_mode,
        BURNER_WRITE_PATH_DIRECT,
        false,
        gba_force_multi,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        dump_chunk_bytes,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        false,
        0U);
    if (err == ESP_OK) {
        ui_set_task_page_starting(ui_tr("ROM dump started"), effective_size);
    }
    return err;
}

static esp_err_t ui_start_dump_save_task(void)
{
    char timestamp[32] = {0};
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    uint32_t read_size = s_save_size_kib * 1024U;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    esp_err_t err;

    if (s_cart_mode == BURNER_CART_MODE_GBA) {
        if (card == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        if (usb_msc_tf_in_use_by_host()) {
            return ESP_ERR_INVALID_STATE;
        }
        err = burner_ensure_dump_dir();
        if (err != ESP_OK) {
            return err;
        }
        burner_build_output_timestamp(timestamp, sizeof(timestamp));
        snprintf(safe_name, sizeof(safe_name), "cart_gba_save_%s.sav", timestamp);
        err = burner_resolve_unique_output_path(
            DUMP_DIR_PATH,
            safe_name,
            safe_name,
            sizeof(safe_name),
            full_path,
            sizeof(full_path));
        if (err != ESP_OK) {
            return err;
        }
        err = burner_start_gba_save_dump_new(
            safe_name,
            full_path,
            s_gba_save_type,
            s_save_size_kib * 1024U);
        if (err == ESP_OK) {
            ui_set_task_page_starting("GBA save dump started", s_save_size_kib * 1024U);
        }
        return err;
    }
    if (s_cart_mode != BURNER_CART_MODE_MBC5) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = burner_apply_mbc5_slot_limit(true, s_cart_slot, read_size, &addr_begin, &effective_size);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_ensure_dump_dir();
    if (err != ESP_OK) {
        return err;
    }
    burner_build_output_timestamp(timestamp, sizeof(timestamp));
    snprintf(safe_name, sizeof(safe_name), "cart_save_%s.sav", timestamp);
    err = burner_resolve_unique_output_path(
        DUMP_DIR_PATH,
        safe_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }
    err = burner_start_task(
        BURNER_JOB_READ_RAM,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        s_ram_fram,
        s_ram_latency);
    if (err == ESP_OK) {
        ui_set_task_page_starting(ui_tr("save dump started"), effective_size);
    }
    return err;
}

static esp_err_t ui_start_chip_erase_task(void)
{
    esp_err_t err = burner_start_task_ex(
        BURNER_JOB_ERASE_ROM,
        s_cart_mode,
        BURNER_WRITE_PATH_DIRECT,
        false,
        false,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        "chip_erase",
        "/cart",
        0U,
        1U,
        BURNER_GBA_SAVE_TYPE_SRAM,
        false,
        0U);

    if (err == ESP_OK) {
        ui_set_task_page_starting(ui_tr("chip erase started"), 1U);
    }
    return err;
}

static esp_err_t ui_read_cart_id_once(char *out, size_t out_len, burner_cart_mode_t cart_mode)
{
    uint8_t gba_id[8] = {0};
    uint8_t mbc5_id[4] = {0};
    char chip_name[48] = {0};
    burner_nor_geometry_t cfi_geometry = {0};
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    burner_gba_save_type_t gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t gba_save_size = 0;
    bool gba_d0d1_known = false;
    bool gba_d0d1_swapped = false;
    bool cfi_ok = false;
    burner_nor_cmdset_t mbc5_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    esp_err_t err;

    if (out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    err = burner_spi_init();
    if (err != ESP_OK) {
        return err;
    }

    burner_spi_lock_take();
    if (cart_mode == BURNER_CART_MODE_MBC5) {
        err = burner_bacon_mbc5_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_get_cfi(
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_geometry,
                &mbc5_cmdset);
            if (err == ESP_OK) {
                cfi_ok = true;
                if (mbc5_cmdset == BURNER_NOR_CMDSET_AMD) {
                    err = burner_bacon_mbc5_get_id(mbc5_id);
                    if (err != ESP_OK) {
                        memset(mbc5_id, 0, sizeof(mbc5_id));
                        err = ESP_OK;
                    }
                }
            }
        }
    } else {
        err = burner_bacon_gba_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_gba_probe_locked(gba_id, &device_size, &sector_size, &buffer_write_bytes, &cfi_ok);
            burner_bacon_gba_d0d1_status(&gba_d0d1_known, &gba_d0d1_swapped);
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        return err;
    }
    if (cart_mode == BURNER_CART_MODE_MBC5) {
        burner_nor_format_chip_name(
            chip_name,
            sizeof(chip_name),
            burner_mbc5_chip_name(mbc5_id),
            mbc5_cmdset,
            device_size);
        burner_status_set_probe_info(
            BURNER_CART_MODE_MBC5,
            mbc5_id,
            sizeof(mbc5_id),
            device_size,
            sector_size,
            buffer_write_bytes,
            cfi_ok,
            false,
            false,
            false,
            false,
            chip_name);
        snprintf(
            out,
            out_len,
            "%s ID %02X %02X %02X %02X %s",
            ui_cart_mode_label(cart_mode),
            mbc5_id[0],
            mbc5_id[1],
            mbc5_id[2],
            mbc5_id[3],
            cfi_ok ? "CFI" : "no CFI");
    } else {
        burner_nor_format_chip_name(
            chip_name,
            sizeof(chip_name),
            burner_gba_chip_name(gba_id),
            s_cart_ctx.gba_cmdset,
            device_size);
        burner_status_set_probe_info(
            BURNER_CART_MODE_GBA,
            gba_id,
            sizeof(gba_id),
            device_size,
            sector_size,
            buffer_write_bytes,
            cfi_ok,
            false,
            false,
            gba_d0d1_known,
            gba_d0d1_swapped,
            chip_name);
        {
            bool detected = false;
            burner_spi_lock_take();
            err = burner_probe_gba_save_type_head_locked(
                device_size,
                &gba_save_type,
                &gba_save_size,
                &detected);
            burner_bacon_restore_3v3_power();
            burner_spi_lock_give();
            if (err == ESP_OK) {
                burner_status_set_gba_save_probe(gba_save_type, gba_save_size, detected);
                burner_status_set_gba_sram_patch_probe(BURNER_GBA_SRAM_PATCH_NONE, false, false);
            } else {
                burner_status_set_gba_save_probe(BURNER_GBA_SAVE_TYPE_SRAM, 0u, false);
                burner_status_set_gba_sram_patch_probe(BURNER_GBA_SRAM_PATCH_NONE, false, false);
            }
        }
        snprintf(
            out,
            out_len,
            "%s ID %02X %02X %02X %02X %s",
            ui_cart_mode_label(cart_mode),
            gba_id[0],
            gba_id[1],
            gba_id[2],
            gba_id[3],
            cfi_ok ? "CFI" : "no CFI");
    }
    return ESP_OK;
}

static esp_err_t ui_unlock_ppb_once(char *out, size_t out_len, burner_cart_mode_t cart_mode)
{
    burner_ppb_unlock_report_t report;
    esp_err_t err;

    if (out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    err = burner_spi_init();
    if (err != ESP_OK) {
        return err;
    }

    memset(&report, 0, sizeof(report));
    burner_spi_lock_take();
    err = burner_cart_unlock_ppb_locked(cart_mode, &report);
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        return err;
    }

    if (cart_mode == BURNER_CART_MODE_MBC5) {
        burner_status_set_probe_info(
            BURNER_CART_MODE_MBC5,
            report.mbc5_id,
            sizeof(report.mbc5_id),
            report.device_size,
            report.sector_size,
            report.buffer_write_bytes,
            report.cfi_ok,
            false,
            false,
            false,
            false,
            burner_mbc5_chip_name(report.mbc5_id));
    } else {
        burner_status_set_probe_info(
            BURNER_CART_MODE_GBA,
            report.gba_id,
            sizeof(report.gba_id),
            report.device_size,
            report.sector_size,
            report.buffer_write_bytes,
            report.cfi_ok,
            false,
            false,
            report.gba_d0d1_known,
            report.gba_d0d1_swapped,
            burner_gba_chip_name(report.gba_id));
    }

    if (report.ppb_needs_unlock_before == 0U && report.ppb_needs_unlock_after == 0U) {
        snprintf(
            out,
            out_len,
            "%s",
            ui_tr("PPB already unlocked"));
    } else {
        snprintf(
            out,
            out_len,
            "%s %" PRIu32 "->%" PRIu32,
            ui_tr("PPB unlocked"),
            report.ppb_needs_unlock_before,
            report.ppb_needs_unlock_after);
    }
    return ESP_OK;
}

static void ui_finish_work_task(ui_work_type_t type, esp_err_t err, const char *ok_text, burner_cart_mode_t cart_mode)
{
    if (!ui_take_model_lock()) {
        return;
    }
    if (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
        type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLEAR_SAVED) {
        s_wifi_work_active = false;
    } else if (type == UI_WORK_STORAGE_USB_ENABLE || type == UI_WORK_STORAGE_USB_DISABLE) {
        s_storage_work_active = false;
    } else if (type == UI_WORK_BURN_READ_ID || type == UI_WORK_BURN_UNLOCK_PPB) {
        s_burn_probe_active = false;
    }
    if (type == UI_WORK_BURN_READ_ID) {
        if (err == ESP_OK) {
            s_cart_analyzed = true;
            s_analyzed_cart_mode = cart_mode;
            snprintf(s_analyzed_cart_info, sizeof(s_analyzed_cart_info), "%s", ok_text != NULL ? ok_text : "");
            s_burn_rom_write_menu = false;
            s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
            ui_focus_burn_rom_row_locked(&s_model, 1U);
        } else {
            s_cart_analyzed = false;
            s_analyzed_cart_info[0] = '\0';
        }
    }

    if (err == ESP_OK) {
        ui_set_status_locked(&s_model, ok_text);
    } else {
        char msg[UI_STATUS_TEXT_MAX_LEN] = {0};
        snprintf(
            msg,
            sizeof(msg),
            "%s%s",
            ui_tr("action failed: "),
            esp_err_to_name(err));
        ui_set_status_locked(&s_model, msg);
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_work_task(void *param)
{
    ui_work_request_t *request = (ui_work_request_t *)param;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *ok_text = ui_tr("done");

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }

    switch (request->type) {
        case UI_WORK_WIFI_CONNECT_SAVED:
            ok_text = ui_tr("Wi-Fi connected");
            err = wifi_maneger_ready() ? wifi_maneger_connect_saved(15000U) : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_WIFI_START_AP:
            ok_text = ui_tr("provisioning AP");
            err = wifi_maneger_ready() ? wifi_maneger_ap() : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_WIFI_DISCONNECT:
            wifi_maneger_disconnect();
            ok_text = ui_tr("Wi-Fi disconnected");
            err = ESP_OK;
            break;
        case UI_WORK_WIFI_CLEAR_SAVED:
            ok_text = ui_tr("saved Wi-Fi cleared");
            err = wifi_maneger_ready() ? wifi_maneger_clear_sta_config() : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_STORAGE_USB_ENABLE:
            ok_text = ui_tr("USB pass-through enabled");
            err = usb_msc_tf_set_enabled(true);
            break;
        case UI_WORK_STORAGE_USB_DISABLE:
            ok_text = ui_tr("USB pass-through disabled");
            err = usb_msc_tf_set_enabled(false);
            break;
        case UI_WORK_BURN_ERASE_CHIP:
            ok_text = ui_tr("chip erase started");
            err = ui_start_chip_erase_task();
            break;
        case UI_WORK_BURN_DUMP_ROM:
            ok_text = ui_tr("ROM dump started");
            err = ui_start_dump_rom_task();
            break;
        case UI_WORK_BURN_DUMP_SAVE:
            ok_text = ui_tr("save dump started");
            err = ui_start_dump_save_task();
            break;
        case UI_WORK_BRIGHTNESS_UP:
            ok_text = ui_tr("brightness up");
            err = lcd_display_set_brightness((uint8_t)((lcd_display_get_brightness() > 223U) ? 255U : (lcd_display_get_brightness() + 32U)));
            break;
        case UI_WORK_BRIGHTNESS_DOWN:
            ok_text = ui_tr("brightness down");
            err = lcd_display_set_brightness((uint8_t)((lcd_display_get_brightness() < 32U) ? 0U : (lcd_display_get_brightness() - 32U)));
            break;
        case UI_WORK_DEVICE_RESTART:
            ok_text = ui_tr("restarting");
            burner_schedule_restart();
            err = ESP_OK;
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    ui_finish_work_task(request->type, err, ok_text, request->cart_mode);
    free(request);
    vTaskDelete(NULL);
}

static void ui_burn_probe_task(void *param)
{
    ui_work_request_t *request = (ui_work_request_t *)param;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *ok_text = ui_tr("done");
    char id_text[UI_STATUS_TEXT_MAX_LEN] = {0};

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }

    switch (request->type) {
        case UI_WORK_BURN_READ_ID:
            err = ui_read_cart_id_once(id_text, sizeof(id_text), request->cart_mode);
            ok_text = (err == ESP_OK) ? id_text : ui_tr("read id failed");
            break;
        case UI_WORK_BURN_UNLOCK_PPB:
            err = ui_unlock_ppb_once(id_text, sizeof(id_text), request->cart_mode);
            ok_text = (err == ESP_OK) ? id_text : ui_tr("PPB unlock failed");
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    ui_finish_work_task(request->type, err, ok_text, request->cart_mode);
    ESP_LOGI(UI_TAG, "UI probe task stack free min=%u bytes", (unsigned)uxTaskGetStackHighWaterMark2(NULL));
    free(request);
    vTaskDelete(NULL);
}

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

    if (!ui_take_model_lock()) {
        return;
    }
    if (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
        type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLEAR_SAVED) {
        active = &s_wifi_work_active;
        status = ui_tr("Wi-Fi working");
        stack_size = UI_WIFI_TASK_STACK_SIZE;
        priority = UI_WIFI_TASK_PRIORITY;
    } else if (type == UI_WORK_STORAGE_USB_ENABLE || type == UI_WORK_STORAGE_USB_DISABLE) {
        active = &s_storage_work_active;
        status = ui_tr("storage working");
        stack_size = UI_STORAGE_TASK_STACK_SIZE;
        priority = UI_STORAGE_TASK_PRIORITY;
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
    ui_set_status_locked(&s_model, status);
    xSemaphoreGive(s_model_lock);

    request = (ui_work_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        ui_finish_work_task(type, ESP_ERR_NO_MEM, NULL, s_cart_mode);
        return;
    }
    request->type = type;
    request->cart_mode = s_cart_mode;

    if (xTaskCreatePinnedToCore(
            task_fn,
            task_name,
            stack_size,
            request,
            priority,
            NULL,
            core_id) != pdPASS) {
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
            return UI_ROOT_ITEM_COUNT;
        case UI_PAGE_SYSTEM:
            return UI_SYSTEM_ITEM_COUNT;
        case UI_PAGE_TF:
            return UI_TF_ITEM_COUNT;
        case UI_PAGE_FILES:
            return model->file_total;
        case UI_PAGE_FILE_ACTIONS:
            return ui_file_action_count_for_kind(model->action_kind);
        case UI_PAGE_WIFI:
            return UI_WIFI_ITEM_COUNT;
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
            return s_task_cancel_confirm ? UI_TASK_CANCEL_CONFIRM_ITEM_COUNT : UI_TASK_STATUS_ITEM_COUNT;
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
    if (page == UI_PAGE_FILES && parent != UI_PAGE_BURN_ROM && parent != UI_PAGE_BURN_SAVE &&
        parent != UI_PAGE_BURNER) {
        model->file_filter = UI_FILE_FILTER_NONE;
    }
    if (page == UI_PAGE_FILES) {
        ui_open_files_locked(model, model->file_path, false);
        model->parent_page = parent;
    } else {
        model->dirty = true;
    }
}

static void ui_open_root_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->page = UI_PAGE_ROOT;
    model->parent_page = UI_PAGE_ROOT;
    model->selected = (s_root_selected < UI_ROOT_ITEM_COUNT) ? s_root_selected : 0;
    model->scroll = 0;
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
    s_burn_rom_write_prompt_until_ms = 0;
    s_burn_rom_verify_prompt_until_ms = 0;
    s_nav_depth = 0;
    ui_set_status_locked(model, ui_tr("ready"));
}

static bool ui_task_status_operation_active(void)
{
    burner_status_t status = {0};

    burner_status_snapshot(&status);
    return s_file_start_active || status.state == BURNER_STATE_RECEIVING || status.state == BURNER_STATE_BURNING;
}

static ui_page_t ui_task_return_parent_page(ui_page_t page)
{
    switch (page) {
        case UI_PAGE_ROOT:
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
    if (model->page == UI_PAGE_FILES) {
        ui_file_move_locked(model, delta);
        return;
    }
    if (model->page == UI_PAGE_ROOT) {
        delta = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
    } else if (model->page == UI_PAGE_BURNER) {
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
        model->scroll = (model->page == UI_PAGE_BURN_ROM) ?
                            ui_scroll_for_selected_rows(model->selected, model->scroll, count, ui_burn_rom_visible_rows()) :
                            ui_scroll_for_selected(model->selected, model->scroll, count);
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
    if (model->page == UI_PAGE_TASK_STATUS && s_task_cancel_confirm) {
        ui_task_cancel_confirm_close_locked(model, "task status");
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS && ui_task_status_operation_active()) {
        ui_task_cancel_confirm_open_locked(model);
        return;
    }
    if (model->page == UI_PAGE_FILES && model->file_path[0] != '\0') {
        if (ui_file_parent_path(model->file_path, parent, sizeof(parent))) {
            ui_open_files_locked(model, parent, true);
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
        if (model->page == UI_PAGE_FILES) {
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
    request->gba_save_type = s_gba_save_type;
    request->gba_save_size = selected_file.size;
    request->ram_fram = s_ram_fram;
    request->ram_latency = s_ram_latency;

    s_file_start_active = true;
    ui_task_cancel_confirm_reset_locked();
    model->page = UI_PAGE_TASK_STATUS;
    model->parent_page = UI_PAGE_BURNER;
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

static ui_file_action_t ui_burn_write_action_for_index(uint16_t index)
{
    switch (index) {
        case 0:
            return UI_FILE_ACTION_BURN_PIPELINE;
        case 1:
            return UI_FILE_ACTION_BURN_PSRAM;
        case 2:
        default:
            return UI_FILE_ACTION_BURN_DIRECT;
    }
}

static uint32_t ui_burn_dump_size_for_index(uint16_t index)
{
    switch (index) {
        case 0:
            return 4U;
        case 1:
            return 8U;
        case 3:
            return 16U;
        case 4:
            return 32U;
        default:
            return 0U;
    }
}

static char ui_burn_dump_key_for_index(uint16_t index)
{
    static const char keys[UI_BURN_ROM_DUMP_KEY_COUNT] = {
        '1', '2', '3',
        '4', '5', '6',
        '7', '8', '9',
        ',', '0', '.',
        '\n',
    };

    return (index < UI_BURN_ROM_DUMP_KEY_COUNT) ? keys[index] : '\0';
}

static bool ui_parse_first_u32_text(const char *text, uint32_t *value_out)
{
    uint32_t value = 0;
    bool fractional_nonzero = false;
    bool any = false;

    if (text == NULL || value_out == NULL) {
        return false;
    }
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text - '0');

        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        any = true;
        text++;
    }
    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9') {
            if (*text != '0') {
                fractional_nonzero = true;
            }
            text++;
        }
    }
    if (!any || value == 0U) {
        return false;
    }
    if (fractional_nonzero) {
        if (value == UINT32_MAX) {
            return false;
        }
        value++;
    }
    *value_out = value;
    return true;
}

static ui_burn_rom_op_t ui_burn_rom_op_for_index(uint16_t index)
{
    uint16_t cursor = 0;

    if (index == cursor++) {
        return UI_BURN_ROM_OP_ANALYZE;
    }
    if (!ui_cart_is_unlocked()) {
        return UI_BURN_ROM_OP_INVALID;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_CHOOSE_ROM;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_WRITE_ROM;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_VERIFY_ROM;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_DUMP_ROM;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_ERASE_CHIP;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_UNLOCK_PPB;
    }
    if (s_cart_mode == BURNER_CART_MODE_MBC5) {
        if (index == cursor++) {
            return UI_BURN_ROM_OP_RAM_MENU;
        }
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_SETTINGS;
    }
    return UI_BURN_ROM_OP_INVALID;
}

static ui_burn_rom_op_t ui_burn_ram_op_for_index(uint16_t index)
{
    switch (index) {
        case 0:
            return UI_BURN_ROM_OP_CHOOSE_SAVE;
        case 1:
            return UI_BURN_ROM_OP_WRITE_SAVE;
        case 2:
            return UI_BURN_ROM_OP_VERIFY_SAVE;
        case 3:
            return UI_BURN_ROM_OP_DUMP_SAVE;
        case 4:
            return UI_BURN_ROM_OP_SAVE_SIZE;
        case 5:
            return (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_BURN_ROM_OP_GBA_SAVE_TYPE : UI_BURN_ROM_OP_RAM_TYPE;
        case 6:
            return UI_BURN_ROM_OP_RAM_LATENCY;
        default:
            return UI_BURN_ROM_OP_INVALID;
    }
}

static void ui_burn_rom_open_write_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = true;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_WRITE;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("select write method"));
}

static void ui_burn_rom_open_dump_size_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_DUMP_SIZE;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("select dump size"));
}

static void ui_burn_rom_open_dump_custom_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_DUMP_CUSTOM;
    s_burn_rom_custom_size_text[0] = '\0';
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("input MiB size"));
}

static void ui_burn_rom_open_erase_confirm_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_ERASE_CONFIRM;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("confirm chip erase"));
}

static void ui_burn_rom_open_ram_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_RAM;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("RAM operations"));
}

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
            if (model->selected < UI_ROOT_ITEM_COUNT) {
                switch (s_root_items[model->selected].action) {
                    case UI_ACTION_OPEN_SYSTEM:
                        ui_open_page_locked(model, UI_PAGE_SYSTEM);
                        break;
                    case UI_ACTION_OPEN_TF:
                        ui_open_page_locked(model, UI_PAGE_TF);
                        break;
                    case UI_ACTION_OPEN_WIFI:
                        ui_open_page_locked(model, UI_PAGE_WIFI);
                        break;
                    case UI_ACTION_OPEN_POWER:
                        ui_open_page_locked(model, UI_PAGE_POWER);
                        break;
                    case UI_ACTION_OPEN_BURNER:
                        ui_open_page_locked(model, UI_PAGE_BURNER);
                        break;
                    case UI_ACTION_OPEN_SETTINGS:
                        ui_open_page_locked(model, UI_PAGE_SETTINGS);
                        break;
                    default:
                        break;
                }
            }
            break;
        case UI_PAGE_SYSTEM:
            if (model->selected == 4U) {
                ui_open_page_locked(model, UI_PAGE_TASK_STATUS);
            } else if (model->selected == 5U) {
                ui_open_page_locked(model, UI_PAGE_TF);
            } else if (model->selected == 6U) {
                ui_open_page_locked(model, UI_PAGE_SETTINGS);
            } else {
                ui_set_status_locked(model, ui_tr("system overview"));
            }
            break;
        case UI_PAGE_TF:
            if (model->selected == 0U) {
                model->file_filter = UI_FILE_FILTER_NONE;
                ui_open_page_locked(model, UI_PAGE_FILES);
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
        case UI_PAGE_FILES:
            if (!ui_current_file_locked(model, &entry)) {
                ui_set_status_locked(model, ui_tr("no file selected"));
                break;
            }
            if (entry.is_dir) {
                ui_open_files_locked(model, entry.path, true);
            } else {
                if (model->parent_page == UI_PAGE_BURN_ROM || model->parent_page == UI_PAGE_BURN_SAVE ||
                    model->parent_page == UI_PAGE_BURNER) {
                    ui_select_file_for_burner_locked(model, &entry);
                } else {
                    ui_open_file_action_page_locked(model, &entry);
                }
            }
            break;
        case UI_PAGE_FILE_ACTIONS:
            (void)ui_prepare_file_action_locked(model, start_request);
            break;
        case UI_PAGE_WIFI:
            if (model->selected == 1U) {
                *work_type = UI_WORK_WIFI_CONNECT_SAVED;
                *start_work = true;
            } else if (model->selected == 2U) {
                *work_type = UI_WORK_WIFI_START_AP;
                *start_work = true;
            } else if (model->selected == 3U) {
                *work_type = UI_WORK_WIFI_DISCONNECT;
                *start_work = true;
            } else if (model->selected == 4U) {
                *work_type = UI_WORK_WIFI_CLEAR_SAVED;
                *start_work = true;
            } else {
                ui_set_status_locked(model, ui_wifi_state_name(model->wifi_state));
            }
            break;
        case UI_PAGE_BURNER:
            s_cart_mode = (model->selected == 0U) ? BURNER_CART_MODE_GBA : BURNER_CART_MODE_MBC5;
            ui_open_page_locked(model, UI_PAGE_BURN_ROM);
            ui_set_status_locked(model, ui_cart_is_unlocked() ? ui_tr("cart analyzed") : ui_tr("analyze cart first"));
            break;
        case UI_PAGE_BURN_ROM:
            if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
                ui_file_action_t action = ui_burn_write_action_for_index(model->selected);

                s_write_path = (action == UI_FILE_ACTION_BURN_PSRAM) ?
                                   BURNER_WRITE_PATH_PSRAM :
                                   ((action == UI_FILE_ACTION_BURN_PIPELINE) ?
                                        BURNER_WRITE_PATH_PIPELINE :
                                        BURNER_WRITE_PATH_DIRECT);
                if (ui_prepare_last_file_action_locked(model, action, start_request) == ESP_OK) {
                    s_burn_rom_write_menu = false;
                    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
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
            } else if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
                if (model->selected == 0U) {
                    s_burn_erase_always = (s_burn_erase_always == 0u) ? 1u : 0u;
                    ui_set_status_locked(model, s_burn_erase_always != 0u ?
                                                   ui_tr("Erase mode: force erase") :
                                                   ui_tr("Erase mode: smart skip"));
                } else if (model->selected == 1U) {
                    s_psram_mb = ui_next_option_u32(
                        s_psram_mb_options,
                        sizeof(s_psram_mb_options) / sizeof(s_psram_mb_options[0]),
                        s_psram_mb,
                        1);
                    ui_set_status_locked(model, ui_tr("PSRAM window changed"));
                } else if (model->selected == 2U) {
                    s_dump_chunk_kb = ui_next_option_u32(
                        s_dump_chunk_kb_options,
                        sizeof(s_dump_chunk_kb_options) / sizeof(s_dump_chunk_kb_options[0]),
                        s_dump_chunk_kb,
                        1);
                    ui_set_status_locked(model, ui_tr("dump chunk changed"));
                } else if (s_cart_mode == BURNER_CART_MODE_MBC5) {
                    s_mbc5_power_5v_enabled = (s_mbc5_power_5v_enabled == 0u) ? 1u : 0u;
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
                        ui_open_files_with_filter_locked(model, model->file_path, false, UI_FILE_FILTER_SAVE);
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
                    case UI_BURN_ROM_OP_ANALYZE:
                    case UI_BURN_ROM_OP_CHOOSE_ROM:
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
                    case UI_BURN_ROM_OP_ANALYZE:
                        *work_type = UI_WORK_BURN_READ_ID;
                        *start_work = true;
                        break;
                    case UI_BURN_ROM_OP_CHOOSE_ROM:
                    case UI_BURN_ROM_OP_CHOOSE_SAVE:
                        ui_open_files_with_filter_locked(
                            model,
                            model->file_path,
                            false,
                            (ui_burn_rom_op_for_index(model->selected) == UI_BURN_ROM_OP_CHOOSE_SAVE) ?
                                UI_FILE_FILTER_SAVE :
                                ui_burner_rom_file_filter());
                        model->parent_page = UI_PAGE_BURN_ROM;
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
                ui_open_files_with_filter_locked(model, model->file_path, false, UI_FILE_FILTER_SAVE);
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
                *work_type = UI_WORK_BRIGHTNESS_UP;
                *start_work = true;
            } else if (model->selected == 1U) {
                *work_type = UI_WORK_BRIGHTNESS_DOWN;
                *start_work = true;
            } else if (model->selected == 2U) {
                *work_type = UI_WORK_DEVICE_RESTART;
                *start_work = true;
            } else if (model->selected == 3U) {
                ui_open_page_locked(model, UI_PAGE_TF);
            } else if (model->selected == 4U) {
                ui_open_page_locked(model, UI_PAGE_WIFI);
            } else if (model->selected == 5U) {
                ui_open_page_locked(model, UI_PAGE_POWER);
            } else if (model->selected == 6U) {
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
    if (model->page == UI_PAGE_FILES) {
        ui_scan_file_window_locked(model);
    } else if (model->page == UI_PAGE_ROOT) {
        model->dirty = true;
    } else {
        ui_set_status_locked(model, ui_tr("refreshed"));
    }
    model->dirty = true;
}

static void ui_update_burn_rom_prompt_locked(ui_model_t *model)
{
    bool changed = false;
    uint32_t now_ms = esp_log_timestamp();

    if (model == NULL) {
        return;
    }
    if (s_burn_rom_write_prompt_until_ms != 0U &&
        (int32_t)(now_ms - s_burn_rom_write_prompt_until_ms) >= 0) {
        s_burn_rom_write_prompt_until_ms = 0;
        changed = true;
    }
    if (s_burn_rom_verify_prompt_until_ms != 0U &&
        (int32_t)(now_ms - s_burn_rom_verify_prompt_until_ms) >= 0) {
        s_burn_rom_verify_prompt_until_ms = 0;
        changed = true;
    }
    if (changed && model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_NONE) {
        ui_mark_content_dirty(model);
    }
}

static bool ui_button_is_repeatable(ui_button_t button)
{
    return button == UI_BUTTON_LEFT || button == UI_BUTTON_RIGHT ||
           button == UI_BUTTON_UP || button == UI_BUTTON_DOWN;
}

static void ui_handle_button_action(ui_button_t button)
{
    ui_file_start_request_t *start_request = NULL;
    ui_work_type_t work_type = UI_WORK_WIFI_CONNECT_SAVED;
    bool start_work = false;

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_MENU) {
        return;
    }
    if (!ui_take_model_lock()) {
        return;
    }

    switch (button) {
        case UI_BUTTON_UP:
            ui_menu_move_locked(&s_model, -1);
            break;
        case UI_BUTTON_DOWN:
            ui_menu_move_locked(&s_model, 1);
            break;
        case UI_BUTTON_LEFT:
            ui_menu_move_locked(&s_model, s_model.page == UI_PAGE_ROOT ? -1 : -UI_ROW_COUNT);
            break;
        case UI_BUTTON_RIGHT:
            ui_menu_move_locked(&s_model, s_model.page == UI_PAGE_ROOT ? 1 : UI_ROW_COUNT);
            break;
        case UI_BUTTON_SELECT:
            ui_select_locked(&s_model, &start_request, &work_type, &start_work);
            break;
        case UI_BUTTON_PANEL_TOGGLE:
            if (s_model.page == UI_PAGE_BURN_ROM) {
                s_burner_info_left = !s_burner_info_left;
                s_model.dirty = true;
                ui_set_status_locked(
                    &s_model,
                    s_burner_info_left ? ui_tr("info panel left") : ui_tr("info panel right"));
            } else {
                ui_select_locked(&s_model, &start_request, &work_type, &start_work);
            }
            break;
        case UI_BUTTON_BACK:
            ui_back_locked(&s_model);
            break;
        case UI_BUTTON_MENU:
            if (s_model.page == UI_PAGE_ROOT) {
                ui_open_page_locked(&s_model, UI_PAGE_SETTINGS);
            } else {
                ui_refresh_current_locked(&s_model);
            }
            break;
        default:
            break;
    }

    xSemaphoreGive(s_model_lock);

    if (start_request != NULL) {
        ui_start_file_action_async(start_request);
    }
    if (start_work) {
        ui_start_work_async(work_type);
    }
}

static void ui_handle_button_event(ui_button_t button, bool pressed)
{
    ui_button_state_t *state = NULL;

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_MENU) {
        return;
    }
    state = &s_button_states[(uint8_t)button];
    if (state->pressed == pressed) {
        return;
    }

    state->pressed = pressed;
    if (!pressed) {
        state->frames_until_repeat = 0;
        return;
    }

    state->frames_until_repeat = UI_BUTTON_REPEAT_START_FRAMES;
    ui_handle_button_action(button);
}

static void ui_clear_stuck_button_state(ui_button_t button)
{
    ui_button_state_t *state = NULL;

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_MENU) {
        return;
    }
    state = &s_button_states[(uint8_t)button];
    state->pressed = false;
    state->frames_until_repeat = 0;
}

static void ui_process_button_repeats(void)
{
    for (uint8_t i = 0; i < UI_BUTTON_COUNT; ++i) {
        ui_button_state_t *state = &s_button_states[i];
        ui_button_t button = (ui_button_t)i;

        if (!state->pressed || !ui_button_is_repeatable(button)) {
            continue;
        }
        if (state->frames_until_repeat > 0U) {
            state->frames_until_repeat--;
            continue;
        }

        ui_handle_button_action(button);
        state->frames_until_repeat = UI_BUTTON_REPEAT_INTERVAL_FRAMES;
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
        ui_handle_button_event(event.button, event.pressed);
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

static void ui_update_battery_if_needed(uint32_t now_ms)
{
    uint8_t percent = 0;
    bool valid = false;
    bool charging = false;
    power_manager_telemetry_t telemetry = {0};

    if (s_last_battery_refresh_ms != 0U && (now_ms - s_last_battery_refresh_ms) < UI_BATTERY_REFRESH_MS) {
        return;
    }
    s_last_battery_refresh_ms = now_ms;

    if (power_manager_get_telemetry(&telemetry) == ESP_OK) {
        percent = telemetry.battery_percent;
        valid = telemetry.battery_percent_valid;
        charging = telemetry.charging;
    }

    if (!ui_take_model_lock()) {
        return;
    }
    if (s_model.battery_valid != valid || s_model.battery_percent != percent ||
        s_model.battery_charging != charging) {
        s_model.battery_valid = valid;
        s_model.battery_percent = percent;
        s_model.battery_charging = charging;
        ui_mark_chrome_dirty(&s_model);
        ui_mark_content_dirty(&s_model);
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_update_burn_snapshot_if_needed(uint32_t now_ms)
{
    burner_status_t status = {0};
    int progress;
    int erase_progress = 0;
    uint32_t erase_done_bytes = 0;
    uint32_t erase_total_bytes = 0;
    uint32_t erase_done = 0;
    uint32_t erase_total = 0;
    bool release_start = false;

    if (s_last_live_refresh_ms != 0U && (now_ms - s_last_live_refresh_ms) < UI_LIVE_REFRESH_MS) {
        return;
    }
    s_last_live_refresh_ms = now_ms;

    burner_status_snapshot(&status);
    progress = status.progress;
    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }
    if (status.total_bytes > 0U && status.processed_bytes > status.total_bytes) {
        status.processed_bytes = status.total_bytes;
    }
    erase_total_bytes = status.erase_phase_total_bytes;
    erase_done_bytes = status.erase_phase_done_bytes;
    erase_total = status.erase_phase_total_sectors;
    erase_done = status.erase_phase_done_sectors;
    if (erase_total_bytes > 0U) {
        if (erase_done_bytes > erase_total_bytes) {
            erase_done_bytes = erase_total_bytes;
        }
        erase_progress = burner_calc_progress_percent_u64(erase_done_bytes, erase_total_bytes);
        if (erase_progress > 100) {
            erase_progress = 100;
        }
    } else if (erase_total == 0U) {
        erase_total = status.erase_sector_count;
        erase_done = status.erase_sector_count;
    }
    if (erase_total_bytes == 0U && erase_total > 0U) {
        if (erase_done > erase_total) {
            erase_done = erase_total;
        }
        erase_progress = burner_calc_progress_percent_u64(erase_done, erase_total);
        if (erase_progress > 100) {
            erase_progress = 100;
        }
    }
    if (status.state == BURNER_STATE_DONE || status.state == BURNER_STATE_ERROR ||
        status.state == BURNER_STATE_CANCELLED) {
        release_start = true;
    }

    if (!ui_take_model_lock()) {
        return;
    }
    if (s_model.burn_progress != progress ||
        s_model.burn_processed != status.processed_bytes ||
        s_model.burn_total != status.total_bytes ||
        s_model.erase_progress != erase_progress ||
        s_model.erase_done_sectors != erase_done ||
        s_model.erase_total_sectors != erase_total ||
        s_model.burn_elapsed_us != status.task_elapsed_us) {
        s_model.burn_progress = progress;
        s_model.burn_processed = status.processed_bytes;
        s_model.burn_total = status.total_bytes;
        s_model.erase_progress = erase_progress;
        s_model.erase_done_sectors = erase_done;
        s_model.erase_total_sectors = erase_total;
        s_model.burn_elapsed_us = status.task_elapsed_us;
        ui_mark_content_dirty(&s_model);
    }
    if (release_start) {
        s_task_cancel_confirm = false;
        s_task_cancel_request_pending = false;
        if (s_task_cancel_exit_pending &&
            s_model.page == UI_PAGE_TASK_STATUS &&
            (status.state == BURNER_STATE_CANCELLED || status.state == BURNER_STATE_ERROR)) {
            s_model.page = s_task_cancel_return_page;
            s_model.parent_page = ui_task_return_parent_page(s_task_cancel_return_page);
            s_model.selected = 0;
            s_model.scroll = 0;
            s_task_cancel_exit_pending = false;
            ui_mark_chrome_dirty(&s_model);
            ui_mark_content_dirty(&s_model);
            s_model.dirty = true;
        } else if (status.state == BURNER_STATE_DONE || status.state == BURNER_STATE_CANCELLED ||
                   status.state == BURNER_STATE_ERROR) {
            s_task_cancel_exit_pending = false;
        }
    }
    if (s_model.page == UI_PAGE_TASK_STATUS) {
        ui_mark_content_dirty(&s_model);
    }
    xSemaphoreGive(s_model_lock);

    if (release_start) {
        s_file_start_active = false;
    }
}

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

    ui_update_clock_if_needed(now_ms);
    ui_update_fps_if_needed(now_ms);
    ui_update_battery_if_needed(now_ms);
    ui_update_burn_snapshot_if_needed(now_ms);
}

static void ui_fill_action_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    ui_file_action_t action = ui_file_action_for_kind(model->action_kind, (uint8_t)index);

    snprintf(title, title_len, "%s", ui_file_action_label(action));
    snprintf(hint, hint_len, "%s", ui_file_kind_label(model->action_kind));
    *symbol = (action == UI_FILE_ACTION_VERIFY_ROM || action == UI_FILE_ACTION_VERIFY_SAVE) ? LV_SYMBOL_OK : LV_SYMBOL_UPLOAD;
    *accent = 0x4CC9F0;
}

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
            snprintf(title, title_len, "%s", ui_tr("Connect saved"));
            snprintf(hint, hint_len, "%s", ui_tr("STA profile"));
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Provision AP"));
            snprintf(hint, hint_len, "%s", ui_tr("Setup portal"));
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Disconnect"));
            snprintf(hint, hint_len, "%s", ui_tr("Stop STA"));
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Clear saved"));
            snprintf(hint, hint_len, "%s", ui_tr("Forget profile"));
            break;
    }
    *symbol = (index == 3U) ? LV_SYMBOL_CLOSE : LV_SYMBOL_WIFI;
    *accent = (model->wifi_state == UI_WIFI_STATE_CONNECTED) ? 0x72EFDD : 0xF7B32B;
}

static void ui_fill_file_row(const ui_model_t *model, uint16_t visible_index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    const ui_file_entry_t *entry = NULL;
    uint16_t ordinal;
    uint16_t window_index;
    char size_text[24] = {0};

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
        ui_format_file_size(entry->size, size_text, sizeof(size_text));
        snprintf(hint, hint_len, "%s", size_text);
        *symbol = (ui_file_action_count_for_kind(ui_file_kind_from_name(entry->name)) > 0U) ? LV_SYMBOL_FILE : LV_SYMBOL_WARNING;
    }
    *accent = 0x4CC9F0;
}

static void ui_fill_system_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char size_text[32] = {0};
    char uptime_text[24] = {0};

    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Device"));
            snprintf(hint, hint_len, "%s", app != NULL ? app->project_name : "MORI Burner");
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Version"));
            snprintf(hint, hint_len, "%s", app != NULL ? app->version : ui_tr("unknown"));
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Heap"));
            ui_format_u64_size((uint64_t)esp_get_free_heap_size(), size_text, sizeof(size_text));
            if (ui_lang_is_zh()) {
                snprintf(hint, hint_len, "%s%s", ui_tr("free prefix"), size_text);
            } else {
                snprintf(hint, hint_len, "%s %s", size_text, ui_tr("free suffix"));
            }
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Uptime"));
            ui_format_uptime((uint64_t)(esp_timer_get_time() / 1000LL), uptime_text, sizeof(uptime_text));
            snprintf(hint, hint_len, "%s", uptime_text);
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Task status"));
            snprintf(hint, hint_len, "%d%%", model->burn_progress);
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("TF manager"));
            snprintf(hint, hint_len, "%s", card != NULL ? ui_tr("ready") : ui_tr("missing"));
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Settings"));
            snprintf(hint, hint_len, "%s", ui_tr("device tools"));
            break;
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
    char size_text[32] = {0};
    char uptime_text[24] = {0};

    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Battery"));
            if (model->battery_valid) {
                snprintf(hint, hint_len, "%u%%", (unsigned)model->battery_percent);
            } else {
                snprintf(hint, hint_len, "--");
            }
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Charging"));
            snprintf(hint, hint_len, "%s", model->battery_charging ? ui_tr("yes") : ui_tr("no"));
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Uptime"));
            ui_format_uptime((uint64_t)(esp_timer_get_time() / 1000LL), uptime_text, sizeof(uptime_text));
            snprintf(hint, hint_len, "%s", uptime_text);
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Heap"));
            ui_format_u64_size((uint64_t)esp_get_free_heap_size(), size_text, sizeof(size_text));
            snprintf(hint, hint_len, "%s", size_text);
            break;
        case 4:
            snprintf(title, title_len, "Power");
            snprintf(hint, hint_len, "%s", power_manager_ready() ? power_manager_chip_name() : ui_tr("missing"));
            break;
        default:
            snprintf(title, title_len, "TCA9555");
            snprintf(hint, hint_len, "%s", tca9555_ready() ? ui_tr("ready") : ui_tr("missing"));
            break;
    }
}

static void ui_fill_burner_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    (void)model;
    if (index == 0U) {
        snprintf(title, title_len, "GBA");
        snprintf(hint, hint_len, "%s", ui_tr("Game Boy Advance"));
        return;
    }
    snprintf(title, title_len, "GBC");
    snprintf(hint, hint_len, "%s", ui_tr("Game Boy Color"));
}

static void ui_fill_burn_rom_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    char psram_label[16] = {0};
    (void)model;
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
        ui_file_action_t action = ui_burn_write_action_for_index(index);

        snprintf(title, title_len, "%s", ui_file_action_label(action));
        if (action == UI_FILE_ACTION_BURN_PSRAM) {
            snprintf(hint, hint_len, "%s", ui_psram_window_label(s_psram_mb, psram_label, sizeof(psram_label)));
        } else if (action == UI_FILE_ACTION_BURN_PIPELINE && s_cart_mode == BURNER_CART_MODE_MBC5) {
            snprintf(hint, hint_len, "%" PRIu32 " KB", s_mbc5_chunk_kb);
        } else if (hint_len > 0U) {
            hint[0] = '\0';
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_SIZE) {
        uint32_t dump_size_mib = ui_burn_dump_size_for_index(index);

        if (index == UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT && ui_burn_cart_has_slots()) {
            snprintf(title, title_len, "%s", ui_tr("Slot"));
            snprintf(hint, hint_len, "%" PRIu32, s_cart_slot);
        } else if (index == 2U) {
            snprintf(title, title_len, "%s", ui_tr("Custom"));
            snprintf(hint, hint_len, "%s", s_burn_rom_custom_size_text[0] != '\0' ? s_burn_rom_custom_size_text : ui_tr("keypad"));
        } else {
            snprintf(title, title_len, "%" PRIu32 " MiB", dump_size_mib);
            snprintf(hint, hint_len, "%s", ui_tr("dump ROM"));
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        char key = ui_burn_dump_key_for_index(index);

        if (key == '\n') {
            snprintf(title, title_len, "%s", ui_tr("OK"));
        } else {
            snprintf(title, title_len, "%c", key);
        }
        snprintf(hint, hint_len, "%s", s_burn_rom_custom_size_text[0] != '\0' ? s_burn_rom_custom_size_text : ui_tr("MiB"));
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
        if (index == 0U) {
            snprintf(title, title_len, "%s", ui_tr("Erase mode"));
            snprintf(hint, hint_len, "%s", ui_erase_mode_label());
        } else if (index == 1U) {
            snprintf(title, title_len, "%s", ui_tr("PSRAM window"));
            snprintf(hint, hint_len, "%s", ui_psram_window_label(s_psram_mb, psram_label, sizeof(psram_label)));
        } else if (index == 2U) {
            snprintf(title, title_len, "%s", ui_tr("Dump chunk"));
            snprintf(hint, hint_len, "%" PRIu32 " KB", s_dump_chunk_kb);
        } else if (s_cart_mode == BURNER_CART_MODE_MBC5) {
            snprintf(title, title_len, "%s", ui_tr("Voltage"));
            snprintf(hint, hint_len, "%s", ui_mbc5_voltage_label());
        } else {
            snprintf(title, title_len, "--");
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_ERASE_CONFIRM) {
        if (index == 0U) {
            snprintf(title, title_len, "%s", ui_tr("No"));
            snprintf(hint, hint_len, "%s", ui_tr("cancel"));
        } else {
            snprintf(title, title_len, "%s", ui_tr("Yes, erase chip"));
            snprintf(hint, hint_len, "%s", ui_tr("danger"));
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RAM) {
        switch (index) {
            case 0:
                snprintf(title, title_len, "%s", ui_tr("RAM: Choose save"));
                snprintf(hint, hint_len, "%s", s_last_save_file.path[0] != '\0' ? s_last_save_file.name : ui_tr("not selected"));
                break;
            case 1:
                snprintf(title, title_len, "%s", ui_tr("RAM: Write"));
                snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
                break;
            case 2:
                snprintf(title, title_len, "%s", ui_tr("RAM: Verify"));
                snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
                break;
            case 3:
                snprintf(title, title_len, "%s", ui_tr("RAM: Dump"));
                snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
                break;
            case 4:
                snprintf(title, title_len, "%s", ui_tr("RAM size"));
                snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
                break;
            case 5:
                snprintf(title, title_len, "%s", ui_tr("RAM type"));
                snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
                break;
            default:
                snprintf(title, title_len, "%s", ui_tr("FRAM latency"));
                snprintf(hint, hint_len, "%u", (unsigned)s_ram_latency);
                break;
        }
        return;
    }
    if (index == 0U) {
        snprintf(title, title_len, "%s", (s_cart_analyzed && s_analyzed_cart_mode == s_cart_mode) ? ui_tr("Re-analyze") : ui_tr("Analyze cart"));
        snprintf(hint, hint_len, "%s", ui_cart_is_unlocked() ? ui_tr("done") : "");
        return;
    }
    if (!ui_cart_is_unlocked()) {
        snprintf(title, title_len, "%s", ui_tr("Locked"));
        snprintf(hint, hint_len, "%s", ui_tr("analyze first"));
        return;
    }
    switch (ui_burn_rom_op_for_index(index)) {
        case UI_BURN_ROM_OP_CHOOSE_ROM:
            {
                const ui_file_entry_t *rom_file = ui_last_rom_file_for_mode(s_cart_mode);

                if (rom_file->path[0] != '\0') {
                    const char *rom_name = (rom_file->name[0] != '\0') ? rom_file->name : rom_file->path;

                    snprintf(title, title_len, "ROM: %s", rom_name);
                    snprintf(hint, hint_len, "%s", rom_file->path);
                } else {
                    snprintf(title, title_len, "%s", ui_tr("ROM: Choose file"));
                    snprintf(hint, hint_len, "%s", ui_tr("not selected"));
                }
            }
            return;
        case UI_BURN_ROM_OP_WRITE_ROM:
            if (s_burn_rom_write_prompt_until_ms != 0U &&
                (int32_t)(s_burn_rom_write_prompt_until_ms - esp_log_timestamp()) > 0) {
                snprintf(title, title_len, "%s", ui_tr("Select file first!!!"));
                if (hint_len > 0U) {
                    hint[0] = '\0';
                }
                return;
            }
            snprintf(title, title_len, "%s", ui_tr("ROM: Write"));
            snprintf(hint, hint_len, "%s", ui_tr("choose method"));
            return;
        case UI_BURN_ROM_OP_VERIFY_ROM:
            if (s_burn_rom_verify_prompt_until_ms != 0U &&
                (int32_t)(s_burn_rom_verify_prompt_until_ms - esp_log_timestamp()) > 0) {
                snprintf(title, title_len, "%s", ui_tr("Select file first!!!"));
                if (hint_len > 0U) {
                    hint[0] = '\0';
                }
                return;
            }
            snprintf(title, title_len, "%s", ui_tr("ROM: Verify"));
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
            return;
        case UI_BURN_ROM_OP_DUMP_ROM:
            snprintf(title, title_len, "%s", ui_tr("ROM: Dump"));
            snprintf(hint, hint_len, "%s", ui_tr("select size"));
            return;
        case UI_BURN_ROM_OP_ERASE_CHIP:
            snprintf(title, title_len, "%s", ui_tr("ROM: Erase chip"));
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
            return;
        case UI_BURN_ROM_OP_UNLOCK_PPB:
            snprintf(title, title_len, "%s", ui_tr("ROM: Unlock PPB"));
            snprintf(hint, hint_len, "%s", ui_tr("clear sector protection"));
            return;
        case UI_BURN_ROM_OP_RAM_MENU:
            snprintf(title, title_len, "%s", ui_tr("RAM: Operations"));
            snprintf(hint, hint_len, "%s", ui_tr("save write/dump"));
            return;
        case UI_BURN_ROM_OP_CHOOSE_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Choose save"));
            snprintf(hint, hint_len, "%s", s_last_save_file.path[0] != '\0' ? s_last_save_file.name : ui_tr("not selected"));
            return;
        case UI_BURN_ROM_OP_WRITE_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Write"));
            snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
            return;
        case UI_BURN_ROM_OP_VERIFY_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Verify"));
            snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
            return;
        case UI_BURN_ROM_OP_DUMP_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Dump"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            return;
        case UI_BURN_ROM_OP_SAVE_SIZE:
            snprintf(title, title_len, "%s", ui_tr("RAM size"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            return;
        case UI_BURN_ROM_OP_SETTINGS:
            snprintf(title, title_len, "%s", ui_tr("Settings"));
            snprintf(
                hint,
                hint_len,
                "%s",
                (s_cart_mode == BURNER_CART_MODE_MBC5) ?
                    ui_tr("erase/PSRAM/chunk/voltage") :
                    ui_tr("erase/PSRAM/dump"));
            return;
        case UI_BURN_ROM_OP_ANALYZE:
        case UI_BURN_ROM_OP_INVALID:
        default:
            snprintf(title, title_len, "--");
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
            return;
    }
}

static void ui_fill_burn_save_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Choose TF save"));
            snprintf(hint, hint_len, "%s", model->action_file.path[0] != '\0' ? model->action_file.name : ui_tr("not selected"));
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Write save"));
            snprintf(hint, hint_len, "%s", ui_tr("MBC5 only"));
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Verify save"));
            snprintf(hint, hint_len, "%s", ui_tr("MBC5 only"));
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Dump save"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Save size"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("RAM type"));
            snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("FRAM latency"));
            snprintf(hint, hint_len, "%u", (unsigned)s_ram_latency);
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Slot"));
            snprintf(hint, hint_len, "%" PRIu32, s_cart_slot);
            break;
    }
}

static void ui_fill_task_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    burner_status_t status = {0};
    char size_a[24] = {0};
    char size_b[24] = {0};
    char speed[24] = {0};
    char elapsed[24] = {0};
    char id_text[32] = {0};

    burner_status_snapshot(&status);
    ui_format_elapsed(status.task_elapsed_us > 0ULL ? status.task_elapsed_us : model->burn_elapsed_us, elapsed, sizeof(elapsed));
    if (s_task_cancel_confirm) {
        if (index == 0U) {
            snprintf(title, title_len, "%s", ui_tr("No"));
            snprintf(hint, hint_len, "%s", ui_tr("continue current task"));
        } else {
            snprintf(title, title_len, "%s", ui_tr("Yes, stop task"));
            snprintf(hint, hint_len, "%s", ui_tr("return to previous page"));
        }
        return;
    }
    if (index == 1U) {
        snprintf(title, title_len, "%s", ui_tr("ROM/File"));
        snprintf(hint, hint_len, "%s", status.rom_name[0] != '\0' ? status.rom_name : "--");
        return;
    }
    if (index == UI_TASK_ERASE_PROGRESS_ROW) {
        snprintf(title, title_len, "%s", ui_tr("Erase"));
        if (model->erase_total_sectors > 0U) {
            snprintf(
                hint,
                hint_len,
                "%" PRIu32 "/%" PRIu32 " %d%%",
                model->erase_done_sectors,
                model->erase_total_sectors,
                model->erase_progress);
        } else {
            snprintf(hint, hint_len, "--");
        }
        return;
    }
    if (index == UI_TASK_BURN_PROGRESS_ROW) {
        snprintf(title, title_len, "%s", ui_tr("Burn"));
        snprintf(hint, hint_len, "%d%%", model->burn_progress);
        return;
    }
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("State"));
            snprintf(hint, hint_len, "%s", ui_burn_state_text(status.state));
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Progress"));
            snprintf(hint, hint_len, "%d%%", model->burn_progress);
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Bytes"));
            ui_format_bytes_text(model->burn_processed, size_a, sizeof(size_a));
            ui_format_bytes_text(model->burn_total, size_b, sizeof(size_b));
            snprintf(hint, hint_len, "%s/%s", size_a, size_b);
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Elapsed"));
            snprintf(hint, hint_len, "%s", elapsed);
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Speed"));
            ui_format_speed_text(status.speed_current_bps, speed, sizeof(speed));
            snprintf(hint, hint_len, "%s", speed);
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("Average"));
            ui_format_speed_text(status.speed_avg_bps, speed, sizeof(speed));
            snprintf(hint, hint_len, "%s", speed);
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("ROM/File"));
            snprintf(hint, hint_len, "%s", status.rom_name[0] != '\0' ? status.rom_name : "--");
            break;
        case UI_TASK_BURN_PROGRESS_ROW:
            snprintf(title, title_len, "%s", ui_tr("Burn"));
            snprintf(hint, hint_len, "%d%%", model->burn_progress);
            break;
        case 8:
            snprintf(title, title_len, "%s", ui_tr("Cart"));
            if (status.probe_valid) {
                ui_format_bytes_text(status.probe_device_size, size_a, sizeof(size_a));
                snprintf(
                    hint,
                    hint_len,
                    "%s %s",
                    (status.probe_cart_mode == BURNER_CART_MODE_GBA) ? "GBA" : "MBC5",
                    size_a);
            } else {
                snprintf(hint, hint_len, "%s", ui_tr("not probed"));
            }
            break;
        case 9:
            snprintf(title, title_len, "%s", ui_tr("NOR geom"));
            if (status.probe_valid) {
                ui_format_bytes_text(status.probe_sector_size, size_a, sizeof(size_a));
                ui_format_bytes_text(status.probe_buffer_write_bytes, size_b, sizeof(size_b));
                snprintf(
                    hint,
                    hint_len,
                    "sec %s buf %s",
                    status.probe_sector_size > 0U ? size_a : "--",
                    status.probe_buffer_write_bytes > 0U ? size_b : "--");
            } else {
                snprintf(hint, hint_len, "--");
            }
            break;
        case 10:
            snprintf(title, title_len, "%s", ui_tr("Mapping"));
            if (status.probe_valid && status.probe_cart_mode == BURNER_CART_MODE_GBA) {
                snprintf(
                    hint,
                    hint_len,
                    "%s CFI %s%s",
                    status.probe_gba_multi ? "4MB window" : "linear",
                    status.probe_cfi_ok ? "ok" : "fallback",
                    status.probe_gba_force_multi ? " forced" : "");
            } else if (status.probe_valid) {
                snprintf(hint, hint_len, "MBC5 CFI %s", status.probe_cfi_ok ? "ok" : "fallback");
            } else {
                snprintf(hint, hint_len, "--");
            }
            break;
        default:
            snprintf(title, title_len, "NOR ID");
            ui_format_probe_id(&status, id_text, sizeof(id_text));
            snprintf(hint, hint_len, "%s", id_text);
            break;
    }
}

static void ui_fill_settings_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    (void)model;
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Brightness +"));
            snprintf(hint, hint_len, "%u", (unsigned)lcd_display_get_brightness());
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Brightness -"));
            snprintf(hint, hint_len, "%u", (unsigned)lcd_display_get_brightness());
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
            snprintf(title, title_len, "%s", ui_tr("Power"));
            snprintf(hint, hint_len, "%s", ui_tr("telemetry"));
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("Task status"));
            snprintf(hint, hint_len, "%s", ui_tr("burn progress"));
            break;
        case 7:
            snprintf(title, title_len, "%s", ui_tr("Language"));
            snprintf(hint, hint_len, "%s", ui_tr("current language"));
            break;
        case 8:
            snprintf(title, title_len, "%s", ui_tr("Firmware OTA"));
            snprintf(hint, hint_len, "%s", ui_tr("web action"));
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Web deploy"));
            snprintf(hint, hint_len, "%s", ui_tr("web action"));
            break;
    }
}

static void ui_px_icon(uint16_t index, int32_t x, int32_t y, bool selected)
{
    const int32_t s = UI_TILE_ICON_SIZE;
#define UI_ICON_X(v) (x + ((int32_t)(v) * s) / 30)
#define UI_ICON_Y(v) (y + ((int32_t)(v) * s) / 30)
#define UI_ICON_W(v) ((((int32_t)(v) * s) / 30) > 0 ? (((int32_t)(v) * s) / 30) : 1)

    ui_px_frame(x + 1, y + 1, UI_TILE_ICON_SIZE - 2, UI_TILE_ICON_SIZE - 2, true);
    switch (s_root_items[index].action) {
        case UI_ACTION_OPEN_SYSTEM:
            ui_px_frame(UI_ICON_X(6), UI_ICON_Y(7), UI_ICON_W(18), UI_ICON_W(13), true);
            ui_px_hline(UI_ICON_X(10), UI_ICON_Y(23), UI_ICON_W(10), true);
            ui_px_vline(UI_ICON_X(15), UI_ICON_Y(20), UI_ICON_W(3), true);
            break;
        case UI_ACTION_OPEN_TF:
            ui_px_frame(UI_ICON_X(6), UI_ICON_Y(8), UI_ICON_W(18), UI_ICON_W(14), true);
            ui_px_hline(UI_ICON_X(8), UI_ICON_Y(6), UI_ICON_W(8), true);
            ui_px_vline(UI_ICON_X(6), UI_ICON_Y(7), UI_ICON_W(3), true);
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

static void ui_px_apply_tile(const ui_model_t *model)
{
    const uint16_t selected = (model->selected < UI_ROOT_ITEM_COUNT) ? model->selected : 0;
    const ui_menu_item_t *item = &s_root_items[selected];
    const int32_t center_x = (UI_CANVAS_W - UI_TILE_ICON_SIZE) / 2;
    int32_t progress_w = (int32_t)s_anim.tile_bar_w;

    s_anim.tile_camera_target_x = (float)selected * (float)(UI_TILE_ICON_SIZE + UI_TILE_ICON_SPACING - UI_TILE_ICON_SIZE);
    s_anim.tile_bar_target_w = (float)(((uint32_t)(selected + 1U) * (uint32_t)UI_CANVAS_W) / UI_ROOT_ITEM_COUNT);
    s_anim.tile_fore_target_y = 0.0f;
    ui_anim_move(&s_anim.tile_camera_x, s_anim.tile_camera_target_x, UI_TILE_CAMERA_SPEED);
    ui_anim_move(&s_anim.tile_bar_w, s_anim.tile_bar_target_w, UI_TILE_BAR_SPEED);
    ui_anim_move(&s_anim.tile_fore_y, s_anim.tile_fore_target_y, UI_TILE_FORE_SPEED);

    ui_px_text(8, 10, "MORI", true);
    ui_px_text_clipped(UI_CANVAS_W - 118, 10, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, 10);
    ui_px_box(0, 0, progress_w, UI_TILE_BAR_H, true);
    for (uint16_t i = 0; i < UI_ROOT_ITEM_COUNT; ++i) {
        int32_t x = center_x + (int32_t)((float)i * (float)UI_TILE_ICON_SPACING - s_anim.tile_camera_x);
        if (x <= -UI_TILE_ICON_SIZE || x >= UI_CANVAS_W) {
            continue;
        }
        ui_px_icon(i, x, UI_TILE_ICON_Y, i == selected);
    }

    ui_px_text((UI_CANVAS_W - ui_px_text_width(ui_root_item_title(item))) / 2, UI_TILE_TITLE_Y, ui_root_item_title(item), true);
    ui_px_text((UI_CANVAS_W - ui_px_text_width(ui_root_item_hint(item))) / 2, UI_TILE_HINT_Y, ui_root_item_hint(item), true);
    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_TILE_DOTTED_Y, 2, true);
    }

    ui_px_hline(4, UI_TILE_ARROW_Y, 6, true);
    ui_px_set(5, UI_TILE_ARROW_Y - 1, true);
    ui_px_set(6, UI_TILE_ARROW_Y - 2, true);
    ui_px_set(5, UI_TILE_ARROW_Y + 1, true);
    ui_px_set(6, UI_TILE_ARROW_Y + 2, true);
    ui_px_text(16, UI_TILE_ARROW_Y - 4, "|", true);

    ui_px_hline(UI_CANVAS_W - 10, UI_TILE_ARROW_Y, 6, true);
    ui_px_set(UI_CANVAS_W - 5, UI_TILE_ARROW_Y - 1, true);
    ui_px_set(UI_CANVAS_W - 6, UI_TILE_ARROW_Y - 2, true);
    ui_px_set(UI_CANVAS_W - 5, UI_TILE_ARROW_Y + 1, true);
    ui_px_set(UI_CANVAS_W - 6, UI_TILE_ARROW_Y + 2, true);
    ui_px_text(UI_CANVAS_W - 22, UI_TILE_ARROW_Y - 4, "|", true);
}

static void ui_px_apply_tile_dynamic(const ui_model_t *model)
{
    ui_px_clear_rect(0, UI_TILE_HEADER_Y0, UI_CANVAS_W, UI_TILE_HEADER_H);
    ui_px_clear_rect(0, UI_TILE_ICON_DYNAMIC_Y0, UI_CANVAS_W, UI_TILE_ICON_DYNAMIC_H);
    ui_px_clear_rect(0, UI_TILE_TEXT_DYNAMIC_Y0, UI_CANVAS_W, UI_TILE_TEXT_DYNAMIC_H);
    ui_px_clear_rect(0, UI_TILE_DOT_DYNAMIC_Y0, UI_CANVAS_W, UI_TILE_DOT_DYNAMIC_H);
    ui_px_apply_tile(model);
    ui_px_invalidate_rect(0, UI_TILE_HEADER_Y0, UI_CANVAS_W, UI_TILE_HEADER_H);
    ui_px_invalidate_rect(0, UI_TILE_ICON_DYNAMIC_Y0, UI_CANVAS_W, UI_TILE_ICON_DYNAMIC_H);
    ui_px_invalidate_rect(0, UI_TILE_TEXT_DYNAMIC_Y0, UI_CANVAS_W, UI_TILE_TEXT_DYNAMIC_H);
    ui_px_invalidate_rect(0, UI_TILE_DOT_DYNAMIC_Y0, UI_CANVAS_W, UI_TILE_DOT_DYNAMIC_H);
}

static void ui_px_cart_icon(int32_t x, int32_t y, const char *label, bool selected)
{
    int32_t w = UI_BURNER_ICON_W;
    int32_t h = UI_BURNER_ICON_H;
    int32_t label_w = ui_px_text_width(label);

    ui_px_frame(x, y, w, h, true);
    ui_px_frame(x + 16, y + 10, w - 32, h - 30, true);
    ui_px_hline(x + 26, y + 20, w - 52, true);
    ui_px_hline(x + 26, y + 28, w - 52, true);
    ui_px_hline(x + 26, y + 36, w - 52, true);
    ui_px_text(x + (w - label_w) / 2, y + h - 14, label, true);
    if (selected) {
        ui_px_corner_box(x - UI_BURNER_SELECT_MARGIN, y - UI_BURNER_SELECT_MARGIN, w + UI_BURNER_SELECT_MARGIN * 2, h + UI_BURNER_SELECT_MARGIN * 2);
    }
}

static void ui_px_clear_corner_box(int32_t x, int32_t y, int32_t w, int32_t h)
{
    ui_px_corner_box_draw(x, y, w, h, false);
}

static void ui_px_draw_burner_select_corners(int32_t icon_x, bool selected)
{
    int32_t x = icon_x - UI_BURNER_SELECT_MARGIN;
    int32_t y = UI_BURNER_ICON_Y - UI_BURNER_SELECT_MARGIN;
    int32_t w = UI_BURNER_ICON_W + UI_BURNER_SELECT_MARGIN * 2;
    int32_t h = UI_BURNER_ICON_H + UI_BURNER_SELECT_MARGIN * 2;

    ui_px_clear_corner_box(x, y, w, h);
    if (selected) {
        ui_px_corner_box(x, y, w, h);
    }
}

static void ui_px_invalidate_burner_select_corners(int32_t icon_x)
{
    int32_t x = icon_x - UI_BURNER_SELECT_MARGIN;
    int32_t y = UI_BURNER_ICON_Y - UI_BURNER_SELECT_MARGIN;
    int32_t w = UI_BURNER_ICON_W + UI_BURNER_SELECT_MARGIN * 2;
    int32_t h = UI_BURNER_ICON_H + UI_BURNER_SELECT_MARGIN * 2;
    int32_t len = UI_TILE_SELECT_LINE + 1;

    ui_px_invalidate_rect(x, y, len, len);
    ui_px_invalidate_rect(x, y + h - len, len, len);
    ui_px_invalidate_rect(x + w - len, y, len, len);
    ui_px_invalidate_rect(x + w - len, y + h - len, len, len);
}

static void ui_px_apply_burner_modes(const ui_model_t *model)
{
    uint16_t selected = (model != NULL && model->selected < UI_BURNER_MODE_COUNT) ? model->selected : 0;
    int32_t progress_w = (int32_t)s_anim.tile_bar_w;

    s_anim.tile_bar_target_w = (float)(((uint32_t)(selected + 1U) * (uint32_t)UI_CANVAS_W) / UI_BURNER_MODE_COUNT);
    s_anim.tile_fore_target_y = 0.0f;
    ui_anim_move(&s_anim.tile_bar_w, s_anim.tile_bar_target_w, UI_TILE_BAR_SPEED);
    ui_anim_move(&s_anim.tile_fore_y, s_anim.tile_fore_target_y, UI_TILE_FORE_SPEED);

    ui_px_text(8, 10, ui_tr("Burner"), true);
    ui_px_text_clipped(UI_CANVAS_W - 118, 10, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, 10);
    ui_px_box(0, 0, progress_w, UI_TILE_BAR_H, true);
    ui_px_text((UI_CANVAS_W - ui_px_text_width(ui_tr("Select cart type"))) / 2, 34, ui_tr("Select cart type"), true);
    ui_px_cart_icon(UI_BURNER_ICON_GBA_X, UI_BURNER_ICON_Y, "GBA", selected == 0U);
    ui_px_cart_icon(UI_BURNER_ICON_GBC_X, UI_BURNER_ICON_Y, "GBC", selected == 1U);
    ui_px_text((UI_CANVAS_W - ui_px_text_width(ui_tr("A: enter  B: back"))) / 2, 178, ui_tr("A: enter  B: back"), true);
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
    s_anim.burner_prev_selected = selected;
}

static void ui_px_apply_burner_modes_dynamic(const ui_model_t *model)
{
    uint16_t selected = (model != NULL && model->selected < UI_BURNER_MODE_COUNT) ? model->selected : 0;
    bool selection_changed = s_anim.burner_prev_selected != selected;
    int32_t prev_bar_w = (int32_t)s_anim.tile_bar_w;
    int32_t new_bar_w;
    int32_t bar_x;
    int32_t bar_w;

    s_anim.tile_bar_target_w = (float)(((uint32_t)(selected + 1U) * (uint32_t)UI_CANVAS_W) / UI_BURNER_MODE_COUNT);
    ui_anim_move(&s_anim.tile_bar_w, s_anim.tile_bar_target_w, UI_TILE_BAR_SPEED);
    new_bar_w = (int32_t)s_anim.tile_bar_w;
    bar_x = (prev_bar_w < new_bar_w) ? prev_bar_w : new_bar_w;
    bar_w = ((prev_bar_w > new_bar_w) ? prev_bar_w : new_bar_w) - bar_x + 1;
    ui_px_clear_rect(bar_x, 0, bar_w, UI_TILE_BAR_H);
    ui_px_box(0, 0, new_bar_w, UI_TILE_BAR_H, true);
    ui_px_invalidate_rect(bar_x, 0, bar_w, UI_TILE_BAR_H);

    if (selection_changed) {
        ui_px_draw_burner_select_corners(UI_BURNER_ICON_GBA_X, selected == 0U);
        ui_px_draw_burner_select_corners(UI_BURNER_ICON_GBC_X, selected == 1U);
        ui_px_invalidate_burner_select_corners(UI_BURNER_ICON_GBA_X);
        ui_px_invalidate_burner_select_corners(UI_BURNER_ICON_GBC_X);
        s_anim.burner_prev_selected = selected;
    }
}

static void ui_px_fill_row(
    const ui_model_t *model,
    uint16_t index,
    uint16_t row,
    char *title,
    size_t title_len,
    char *hint,
    size_t hint_len)
{
    const char *symbol = " ";
    uint32_t accent = UI_COLOR_WHITE;

    (void)row;
    switch (model->page) {
        case UI_PAGE_SYSTEM:
            ui_fill_system_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_TF:
            ui_fill_tf_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_FILES:
            ui_fill_file_row(model, row, title, title_len, hint, hint_len, &symbol, &accent);
            break;
        case UI_PAGE_FILE_ACTIONS:
            ui_fill_action_row(model, index, title, title_len, hint, hint_len, &symbol, &accent);
            break;
        case UI_PAGE_WIFI:
            ui_fill_wifi_row(model, index, title, title_len, hint, hint_len, &symbol, &accent);
            break;
        case UI_PAGE_POWER:
            ui_fill_power_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_BURNER:
            ui_fill_burner_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_BURN_ROM:
            ui_fill_burn_rom_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_BURN_SAVE:
            ui_fill_burn_save_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_TASK_STATUS:
            ui_fill_task_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_SETTINGS:
            ui_fill_settings_row(model, index, title, title_len, hint, hint_len);
            break;
        default:
            break;
    }
    (void)symbol;
    (void)accent;
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

static void ui_px_draw_burner_cart_info(const ui_model_t *model)
{
    burner_status_t status = {0};
    char size_text[24] = {0};
    char sector_text[24] = {0};
    char sector_count_text[32] = {0};
    char d0d1_text[24] = {0};
    char id_text[32] = {0};
    int32_t y0 = UI_LIST_HEADER_H + (int32_t)(UI_BURN_ROM_ACTION_ROWS + 1U) * UI_LIST_LINE_H;
    int32_t w = UI_CANVAS_W - UI_LIST_BAR_W - 12;

    (void)model;
    burner_status_snapshot(&status);
    ui_px_hline(4, y0 - 4, w, true);
    ui_px_text(UI_LIST_TEXT_X, y0 + 2, ui_tr("Cart info"), true);
    if (!ui_cart_is_unlocked()) {
        ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 20, w, ui_tr("Only Analyze cart is available"), true);
        return;
    }

    if (status.probe_valid && status.probe_cart_mode == s_cart_mode) {
        ui_format_bytes_text(status.probe_device_size, size_text, sizeof(size_text));
        ui_format_bytes_text(status.probe_sector_size, sector_text, sizeof(sector_text));
        if (status.probe_sector_size > 0U) {
            snprintf(sector_count_text, sizeof(sector_count_text), "%" PRIu32, status.probe_device_size / status.probe_sector_size);
        } else {
            snprintf(sector_count_text, sizeof(sector_count_text), "--");
        }
        ui_format_probe_id(&status, id_text, sizeof(id_text));
        ui_format_gba_d0d1_text(&status, d0d1_text, sizeof(d0d1_text));
        ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 20, w, ui_probe_chip_name(&status), true);
        if (status.probe_cart_mode == BURNER_CART_MODE_GBA) {
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 36, w, "D1/D0", true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 36, w - 72, d0d1_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 52, w, ui_tr("Capacity"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 52, w - 72, size_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 68, w, ui_tr("Sector"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 68, w - 72, sector_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 148, y0 + 68, w - 148, ui_tr("Count"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 216, y0 + 68, w - 216, sector_count_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 84, w, "NOR ID", true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 84, w - 72, id_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 100, w, ui_tr("SRAM patch"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 100, w - 72, ui_gba_sram_patch_label(&status), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 148, y0 + 36, w - 148, ui_tr("Save"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 216, y0 + 36, w - 216, ui_gba_save_type_probe_label(&status), true);
        } else {
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 36, w, ui_tr("Capacity"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 36, w - 72, size_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 52, w, ui_tr("Sector"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 52, w - 72, sector_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 148, y0 + 52, w - 148, ui_tr("Count"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 216, y0 + 52, w - 216, sector_count_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 68, w, "NOR ID", true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 68, w - 72, id_text, true);
        }
    } else {
        ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 20, w, s_analyzed_cart_info[0] != '\0' ? s_analyzed_cart_info : ui_tr("analyzed"), true);
    }
}

static uint16_t ui_burn_rom_visible_rows(void)
{
    int32_t rows = (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H) / UI_LIST_LINE_H;

    return (rows > 0) ? (uint16_t)rows : 1U;
}

static void ui_burn_layout(ui_burn_layout_t *layout)
{
    int32_t content_y = UI_LIST_HEADER_H;
    int32_t content_h = UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H;
    int32_t left_x = UI_BURN_SIDE_MARGIN;

    if (layout == NULL) {
        return;
    }
    if (s_burner_info_left) {
        layout->info_x = left_x;
        layout->ops_x = left_x + UI_BURN_INFO_W + UI_BURN_SPLIT_GAP;
    } else {
        layout->ops_x = left_x;
        layout->info_x = left_x + UI_BURN_OPS_W + UI_BURN_SPLIT_GAP;
    }
    layout->info_y = content_y;
    layout->info_w = UI_BURN_INFO_W;
    layout->info_h = content_h;
    layout->ops_y = content_y;
    layout->ops_w = UI_BURN_OPS_W;
    layout->ops_h = content_h;
}

static void ui_px_info_line(int32_t x, int32_t y, int32_t w, const char *label, const char *value)
{
    int32_t label_w = ui_px_text_width(label) + 4;

    if (label_w > 62) {
        label_w = 62;
    }
    ui_px_text_clipped(x, y, label_w, label, true);
    ui_px_text_clipped(x + label_w, y, w - label_w, value, true);
}

static void ui_px_draw_burner_cart_info_panel(const ui_model_t *model, int32_t x, int32_t y, int32_t w, int32_t h)
{
    burner_status_t status = {0};
    char size_text[24] = {0};
    char sector_text[24] = {0};
    char sector_count_text[32] = {0};
    char d0d1_text[24] = {0};
    char save_size_text[24] = {0};
    char id_text[32] = {0};
    int32_t text_x = x + 5;
    int32_t text_w = w - 10;
    int32_t yy = y + 8;

    (void)model;
    burner_status_snapshot(&status);
    ui_px_frame(x, y, w, h, true);
    ui_px_text_clipped(text_x, yy, text_w, ui_tr("Cart info"), true);
    yy += UI_LIST_LINE_H + 2;
    ui_px_hline(x + 4, yy - 4, w - 8, true);

    if (!ui_cart_is_unlocked()) {
        ui_px_text_clipped(text_x, yy, text_w, ui_tr("Analyze first"), true);
        yy += UI_LIST_LINE_H;
        ui_px_text_clipped(text_x, yy, text_w, ui_tr("Then operations unlock"), true);
        return;
    }

    ui_px_info_line(text_x, yy, text_w, ui_tr("Type:"), ui_cart_mode_label(s_cart_mode));
    yy += UI_LIST_LINE_H;
    if (status.probe_valid && status.probe_cart_mode == s_cart_mode) {
        ui_px_text_clipped(text_x, yy, text_w, ui_probe_chip_name(&status), true);
        yy += UI_LIST_LINE_H;
    } else if (s_analyzed_cart_info[0] != '\0') {
        ui_px_text_clipped(text_x, yy, text_w, s_analyzed_cart_info, true);
        yy += UI_LIST_LINE_H;
    }

    if (status.probe_valid && status.probe_cart_mode == s_cart_mode) {
        ui_format_bytes_text(status.probe_device_size, size_text, sizeof(size_text));
        ui_format_bytes_text(status.probe_sector_size, sector_text, sizeof(sector_text));
        if (status.probe_sector_size > 0U) {
            snprintf(sector_count_text, sizeof(sector_count_text), "%" PRIu32, status.probe_device_size / status.probe_sector_size);
        } else {
            snprintf(sector_count_text, sizeof(sector_count_text), "--");
        }
        ui_format_probe_id(&status, id_text, sizeof(id_text));
        ui_format_gba_d0d1_text(&status, d0d1_text, sizeof(d0d1_text));

        if (status.probe_cart_mode == BURNER_CART_MODE_GBA) {
            ui_px_info_line(text_x, yy, text_w, ui_tr("D1/D0:"), d0d1_text);
            yy += UI_LIST_LINE_H;
            if (status.probe_gba_save_detected && status.probe_gba_save_size > 0u) {
                ui_format_bytes_text(status.probe_gba_save_size, save_size_text, sizeof(save_size_text));
            } else {
                snprintf(save_size_text, sizeof(save_size_text), "--");
            }
            ui_px_info_line(text_x, yy, text_w, ui_tr("Save:"), ui_gba_save_type_probe_label(&status));
            yy += UI_LIST_LINE_H;
            ui_px_info_line(text_x, yy, text_w, ui_tr("Size:"), save_size_text);
            yy += UI_LIST_LINE_H;
            ui_px_info_line(text_x, yy, text_w, ui_tr("SRAM patch:"), ui_gba_sram_patch_label(&status));
            yy += UI_LIST_LINE_H;
        }
        ui_px_info_line(text_x, yy, text_w, ui_tr("Capacity:"), size_text);
        yy += UI_LIST_LINE_H;
        ui_px_info_line(text_x, yy, text_w, ui_tr("Sector:"), sector_text);
        yy += UI_LIST_LINE_H;
        ui_px_info_line(text_x, yy, text_w, ui_tr("Count:"), sector_count_text);
        yy += UI_LIST_LINE_H;
        ui_px_text_clipped(text_x, yy, text_w, "NOR ID", true);
        yy += UI_LIST_LINE_H;
        ui_px_text_clipped(text_x, yy, text_w, id_text, true);
        yy += UI_LIST_LINE_H;
    } else {
        ui_px_text_clipped(text_x, yy, text_w, ui_tr("No probe detail"), true);
        yy += UI_LIST_LINE_H;
    }
    {
        const ui_file_entry_t *rom_file = ui_last_rom_file_for_mode(s_cart_mode);

        if (rom_file->path[0] != '\0' && yy + (UI_LIST_LINE_H * 2) <= y + h - 2) {
            ui_px_info_line(text_x, yy, text_w, ui_tr("File:"), "");
            yy += UI_LIST_LINE_H;
            ui_px_text_clipped(text_x, yy, text_w, rom_file->name, true);
        }
    }
}

static const char *ui_probe_chip_name(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid) {
        return "--";
    }
    if (status->probe_chip_name[0] != '\0') {
        return status->probe_chip_name;
    }
    return "unknown";
}

static void ui_px_draw_burn_rom_ops_panel(const ui_model_t *model, int32_t x, int32_t y, int32_t w, int32_t h)
{
    uint16_t count = ui_page_item_count(model);
    uint16_t selected = ui_current_selected(model);
    uint16_t scroll = ui_current_scroll(model);
    uint16_t visible_rows = ui_burn_rom_visible_rows();
    int32_t list_h = visible_rows * UI_LIST_LINE_H;
    int32_t bar_x = x + w - UI_LIST_BAR_W;
    int32_t text_x = x + 4;
    int32_t text_w = w - UI_LIST_BAR_W - 10;
    int32_t selected_row = (int32_t)selected - (int32_t)scroll;
    int32_t selector_y = y + selected_row * UI_LIST_LINE_H;
    int32_t selector_w = w - UI_LIST_BAR_W - 1;
    int32_t bar_h = 1;

    if (list_h > h) {
        list_h = h;
    }
    ui_px_frame(x, y, w, h, true);
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        const int32_t key_w = (w - UI_LIST_BAR_W - 12) / 3;
        const int32_t key_h = 22;
        const int32_t key_y0 = y + 34;
        const int32_t key_x0 = x + 5;

        ui_px_text_clipped(x + 5, y + 6, w - 10, ui_tr("Custom MiB"), true);
        ui_px_frame(x + 5, y + 20, w - UI_LIST_BAR_W - 12, 12, true);
        ui_px_text_clipped(x + 9, y + 22, w - UI_LIST_BAR_W - 20, s_burn_rom_custom_size_text[0] != '\0' ? s_burn_rom_custom_size_text : "--", true);
        for (uint16_t i = 0; i < UI_BURN_ROM_DUMP_KEY_COUNT; ++i) {
            int32_t col = (int32_t)(i % 3U);
            int32_t row = (int32_t)(i / 3U);
            int32_t key_x = key_x0 + col * key_w;
            int32_t key_y = key_y0 + row * key_h;
            char label[8] = {0};
            char key = ui_burn_dump_key_for_index(i);

            if (key == '\n') {
                snprintf(label, sizeof(label), "%s", ui_tr("OK"));
            } else {
                snprintf(label, sizeof(label), "%c", key);
            }
            ui_px_frame(key_x, key_y, key_w - 3, key_h - 3, true);
            ui_px_text_clipped(key_x + 8, key_y + 6, key_w - 16, label, true);
            if (i == selected) {
                ui_px_invert_rect(key_x + 1, key_y + 1, key_w - 5, key_h - 5);
            }
        }
        return;
    }
    ui_px_vline(bar_x + 2, y + 1, h - 2, true);
    if (count > 0U) {
        bar_h = (int32_t)(((uint32_t)(selected + 1U) * (uint32_t)list_h) / count);
        if (bar_h < 1) {
            bar_h = 1;
        } else if (bar_h > list_h - 2) {
            bar_h = list_h - 2;
        }
        ui_px_box(bar_x, y + 1, UI_LIST_BAR_W, bar_h, true);
    }

    for (uint16_t row = 0; row < visible_rows; ++row) {
        uint16_t index = scroll + row;
        int32_t row_y = y + (int32_t)row * UI_LIST_LINE_H + 1;
        char title[UI_ROW_TEXT_MAX_LEN] = {0};
        char hint[UI_ROW_TEXT_MAX_LEN] = {0};

        if (index >= count || row_y + UI_LIST_LINE_H > y + h) {
            continue;
        }
        ui_fill_burn_rom_row(model, index, title, sizeof(title), hint, sizeof(hint));
        ui_px_text_clipped(text_x, row_y + 3, text_w, title, true);
        if (hint[0] != '\0') {
            int32_t hint_w = ui_px_text_width(hint);
            int32_t hint_x;

            if (hint_w > text_w / 2) {
                hint_w = text_w / 2;
            }
            hint_x = text_x + text_w - hint_w;
            if (hint_x > text_x + ui_px_text_width(title) + 6) {
                ui_px_text_clipped(hint_x, row_y + 3, hint_w, hint, true);
            }
        }
    }

    if (selected_row < 0 || selected_row >= (int32_t)visible_rows) {
        selector_y = y;
    }
    s_anim.list_selector_target_y = (float)selector_y;
    s_anim.list_selector_target_w = (float)selector_w;
    ui_anim_move(&s_anim.list_selector_y, s_anim.list_selector_target_y, UI_LIST_SELECTOR_Y_SPEED);
    ui_anim_move(&s_anim.list_selector_w, s_anim.list_selector_target_w, UI_LIST_SELECTOR_W_SPEED);
    ui_px_invert_rect(x, (int32_t)s_anim.list_selector_y, (int32_t)s_anim.list_selector_w, UI_LIST_LINE_H - 1);
}

static void ui_px_draw_burn_rom_split_chrome(const ui_model_t *model)
{
    char header[UI_ROW_TEXT_MAX_LEN] = {0};

    snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
    ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - 2, true);
    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
}

static void ui_px_apply_burn_rom_split(const ui_model_t *model)
{
    ui_burn_layout_t layout = {0};

    ui_burn_layout(&layout);
    ui_px_draw_burn_rom_split_chrome(model);
    ui_px_draw_burner_cart_info_panel(model, layout.info_x, layout.info_y, layout.info_w, layout.info_h);
    ui_px_draw_burn_rom_ops_panel(model, layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
}

static void ui_px_apply_burn_rom_split_dynamic(const ui_model_t *model)
{
    ui_burn_layout_t layout = {0};
    bool redraw_content = model->content_dirty;
    bool redraw_chrome = model->chrome_dirty;

    ui_burn_layout(&layout);
    if (redraw_chrome) {
        ui_px_clear_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
        ui_px_clear_rect(0, UI_CANVAS_H - UI_HINT_H - 3, UI_CANVAS_W, UI_HINT_H + 3);
        ui_px_draw_burn_rom_split_chrome(model);
        ui_px_invalidate_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
        ui_px_invalidate_rect(0, UI_CANVAS_H - UI_HINT_H - 3, UI_CANVAS_W, UI_HINT_H + 3);
    }
    if (redraw_content) {
        ui_px_clear_rect(layout.info_x, layout.info_y, layout.info_w, layout.info_h);
        ui_px_clear_rect(layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
        ui_px_draw_burner_cart_info_panel(model, layout.info_x, layout.info_y, layout.info_w, layout.info_h);
        ui_px_draw_burn_rom_ops_panel(model, layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
        ui_px_invalidate_rect(layout.info_x, layout.info_y, layout.info_w, layout.info_h);
        ui_px_invalidate_rect(layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
        return;
    }
    ui_px_clear_rect(layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
    ui_px_draw_burn_rom_ops_panel(model, layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
    ui_px_invalidate_rect(layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
}

static void ui_px_apply_chrome_dynamic(const ui_model_t *model)
{
    char header[UI_ROW_TEXT_MAX_LEN] = {0};

    if (model == NULL) {
        return;
    }
    ui_px_clear_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
    snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
    if (model->page == UI_PAGE_FILES && model->file_path[0] != '\0') {
        snprintf(header, sizeof(header), "TF/");
        strncat(header, model->file_path, sizeof(header) - strlen(header) - 1U);
    } else if (model->page == UI_PAGE_ROOT) {
        snprintf(header, sizeof(header), "MORI");
    } else if (model->page == UI_PAGE_BURNER) {
        snprintf(header, sizeof(header), "%s", ui_tr("Burner"));
    }
    ui_px_text(4, (model->page == UI_PAGE_ROOT || model->page == UI_PAGE_BURNER) ? 10 : UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, (model->page == UI_PAGE_ROOT || model->page == UI_PAGE_BURNER) ? 10 : UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, (model->page == UI_PAGE_ROOT || model->page == UI_PAGE_BURNER) ? 10 : UI_LIST_HEADER_TEXT_Y);
    ui_px_clear_rect(0, UI_CANVAS_H - UI_HINT_H, UI_CANVAS_W, UI_HINT_H);
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
    ui_px_invalidate_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
    ui_px_invalidate_rect(0, UI_CANVAS_H - UI_HINT_H, UI_CANVAS_W, UI_HINT_H);
}

static void ui_px_apply_list(const ui_model_t *model)
{
    uint16_t count = ui_page_item_count(model);
    uint16_t selected = ui_current_selected(model);
    uint16_t scroll = ui_current_scroll(model);
    int32_t bar_h = 1;
    int32_t list_h = (model->page == UI_PAGE_BURN_ROM) ?
                         (int32_t)(UI_BURN_ROM_ACTION_ROWS * UI_LIST_LINE_H) :
                         (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H);
    int32_t selected_row = (int32_t)selected - (int32_t)scroll;
    int32_t selector_y;
    int32_t selector_w;
    char header[UI_ROW_TEXT_MAX_LEN] = {0};

    snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
    if (model->page == UI_PAGE_FILES && model->file_path[0] != '\0') {
        snprintf(header, sizeof(header), "TF/");
        strncat(header, model->file_path, sizeof(header) - strlen(header) - 1U);
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
            ui_px_draw_task_erase_progress_row(model, row);
            continue;
        }
        if (model->page == UI_PAGE_TASK_STATUS && index == UI_TASK_BURN_PROGRESS_ROW) {
            ui_px_draw_task_progress_row(model, row);
            continue;
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
        if (model->page == UI_PAGE_FILES) {
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
    }
}

static void ui_px_draw_visible_row(const ui_model_t *model, uint16_t row)
{
    uint16_t scroll = ui_current_scroll(model);
    uint16_t count = ui_page_item_count(model);
    uint16_t index = scroll + row;
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
    if (model->page == UI_PAGE_TASK_STATUS && index == UI_TASK_ERASE_PROGRESS_ROW) {
        ui_px_draw_task_erase_progress_row(model, row);
        return;
    }
    if (model->page == UI_PAGE_TASK_STATUS && index == UI_TASK_BURN_PROGRESS_ROW) {
        ui_px_draw_task_progress_row(model, row);
        return;
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
    if (model->page == UI_PAGE_FILES) {
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
        if (model->page == UI_PAGE_ROOT) {
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
        !model->chrome_dirty && !ui_anim_active_for_page(model)) {
        return;
    }
    if (!full_redraw) {
        if (!model->dirty && !model->motion_dirty && !model->content_dirty && model->chrome_dirty) {
            ui_px_apply_chrome_dynamic(model);
        } else if (model->page == UI_PAGE_ROOT) {
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
    if (model->page == UI_PAGE_ROOT) {
        ui_px_apply_tile(model);
    } else if (model->page == UI_PAGE_BURNER) {
        ui_px_apply_burner_modes(model);
    } else if (model->page == UI_PAGE_BURN_ROM) {
        ui_px_apply_burn_rom_split(model);
    } else {
        ui_px_apply_list(model);
    }
    if (model->page != UI_PAGE_ROOT && model->page != UI_PAGE_BURNER) {
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
    static ui_model_t snapshot;
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
    ui_update_burn_rom_prompt_locked(&s_model);
    snapshot = s_model;
    should_render = s_model.dirty || s_model.motion_dirty || s_model.content_dirty || s_model.chrome_dirty ||
                    ui_anim_active_for_page(&s_model) || s_anim.page != s_model.page ||
                    s_anim.page_changed;
    snapshot.motion_dirty = s_model.motion_dirty;
    snapshot.content_dirty = s_model.content_dirty;
    snapshot.chrome_dirty = s_model.chrome_dirty;
    s_model.dirty = false;
    s_model.motion_dirty = false;
    s_model.content_dirty = false;
    s_model.chrome_dirty = false;
    xSemaphoreGive(s_model_lock);

    if (should_render) {
        ui_apply_snapshot(&snapshot);
        s_render_frames_this_second++;
    }

    ui_issue_pending_task_cancel();
}

void ui_post_button(ui_button_t button, bool pressed)
{
    ui_button_event_t event = {
        .button = button,
        .pressed = pressed,
    };

    if (button < UI_BUTTON_LEFT || button > UI_BUTTON_MENU) {
        return;
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

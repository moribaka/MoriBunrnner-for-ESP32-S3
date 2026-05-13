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
#include "usb_msc_tf.h"
#include "wifi_manager.h"
#include "ws_server_internal.h"

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

#define UI_TILE_ICON_SIZE 30
#define UI_TILE_ICON_Y 52
#define UI_TILE_ICON_SPACING 38
#define UI_TILE_SELECTOR_MARGIN 3
#define UI_TILE_TITLE_Y 158
#define UI_TILE_HINT_Y 174
#define UI_TILE_DOTTED_Y 190
#define UI_TILE_ARROW_Y 214
#define UI_TILE_BAR_H 2
#define UI_TILE_SELECT_LINE 5

#define UI_LIST_LINE_H 16
#define UI_LIST_TEXT_X 4
#define UI_LIST_BAR_W 5
#define UI_LIST_SELECTOR_MARGIN 4
#define UI_HINT_H 14
#define UI_LIST_HEADER_H 28
#define UI_LIST_VISIBLE_COUNT ((UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H) / UI_LIST_LINE_H)

#define UI_STATUS_TEXT_MAX_LEN 96
#define UI_IP_TEXT_MAX_LEN 32
#define UI_TIME_TEXT_MAX_LEN 8
#define UI_TITLE_TEXT_MAX_LEN 32

#define UI_ROOT_ITEM_COUNT 6
#define UI_TF_ITEM_COUNT 7
#define UI_WIFI_ITEM_COUNT 5
#define UI_POWER_ITEM_COUNT 6
#define UI_SYSTEM_ITEM_COUNT 7
#define UI_BURNER_ITEM_COUNT 8
#define UI_BURN_ROM_ITEM_COUNT 12
#define UI_BURN_SAVE_ITEM_COUNT 8
#define UI_SETTINGS_ITEM_COUNT 10
#define UI_ROW_COUNT UI_LIST_VISIBLE_COUNT
#define UI_ROW_TEXT_MAX_LEN 96
#define UI_FILE_NAME_MAX_LEN 128
#define UI_FILE_WINDOW_COUNT UI_ROW_COUNT
#define UI_FILE_SCAN_LIMIT 512U
#define UI_FILE_START_TASK_STACK_SIZE (16U * 1024U)
#define UI_FILE_START_TASK_PRIORITY 4
#define UI_WIFI_TASK_STACK_SIZE 4096U
#define UI_WIFI_TASK_PRIORITY 3
#define UI_STORAGE_TASK_STACK_SIZE 4096U
#define UI_STORAGE_TASK_PRIORITY 3
#define UI_BUTTON_QUEUE_LEN 16
#define UI_BUTTONS_PER_FRAME 8
#define UI_CLOCK_REFRESH_MS 1000U
#define UI_BATTERY_REFRESH_MS 5000U
#define UI_LIVE_REFRESH_MS 250U
#define UI_FILE_PSRAM_WINDOW_MB 4U

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
    UI_FILE_ACTION_BURN_PSRAM = 0,
    UI_FILE_ACTION_BURN_DIRECT,
    UI_FILE_ACTION_VERIFY_ROM,
    UI_FILE_ACTION_WRITE_SAVE,
    UI_FILE_ACTION_VERIFY_SAVE,
} ui_file_action_t;

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
    uint32_t psram_mb;
    uint32_t mbc5_chunk_kb;
    bool gba_force_no_cfi;
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
    uint16_t file_loaded_count;
    char file_path[TF_PATH_LEN_MAX];
    ui_file_entry_t file_window[UI_FILE_WINDOW_COUNT];
    ui_file_entry_t action_file;
    ui_file_kind_t action_kind;
    ui_wifi_state_t wifi_state;
    char ip_text[UI_IP_TEXT_MAX_LEN];
    char status_text[UI_STATUS_TEXT_MAX_LEN];
    char time_text[UI_TIME_TEXT_MAX_LEN];
    int burn_progress;
    uint32_t burn_processed;
    uint32_t burn_total;
    uint8_t battery_percent;
    bool battery_valid;
    bool battery_charging;
    bool dirty;
} ui_model_t;

typedef struct {
    ui_button_t button;
} ui_button_event_t;

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
} ui_work_request_t;

typedef struct {
    ui_page_t page;
    ui_page_t parent;
} ui_nav_entry_t;

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
    ui_page_t page;
    bool page_changed;
} ui_anim_state_t;

static SemaphoreHandle_t s_model_lock = NULL;
static QueueHandle_t s_button_queue = NULL;
static bool s_ui_inited = false;
static bool s_file_start_active = false;
static bool s_wifi_work_active = false;
static bool s_storage_work_active = false;
static uint32_t s_last_clock_refresh_ms = 0;
static uint32_t s_last_battery_refresh_ms = 0;
static uint32_t s_last_live_refresh_ms = 0;
static uint32_t s_last_button_queue_full_log_ms = 0;

static lv_obj_t *s_canvas = NULL;
static uint16_t *s_canvas_buf = NULL;

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

static const uint32_t s_rom_size_mib_options[] = {1U, 2U, 4U, 8U, 16U, 32U};
static const uint32_t s_save_size_kib_options[] = {32U, 64U, 128U, 256U, 512U};
static const uint32_t s_psram_mb_options[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
static const uint32_t s_mbc5_chunk_kb_options[] = {4U, 8U, 16U, 32U, 64U, 128U};
static const uint32_t s_dump_chunk_kb_options[] = {32U, 64U, 128U, 256U};

static ui_anim_state_t s_anim = {
    .tile_fore_y = (float)UI_CANVAS_H,
    .tile_fore_target_y = 0.0f,
    .list_bar_x = (float)UI_CANVAS_W,
    .list_bar_target_x = (float)(UI_CANVAS_W - UI_LIST_BAR_W),
    .page = UI_PAGE_ROOT,
    .page_changed = true,
};
static ui_nav_entry_t s_nav_stack[8];
static uint8_t s_nav_depth = 0;
static burner_cart_mode_t s_cart_mode = BURNER_CART_MODE_GBA;
static burner_write_path_t s_write_path = BURNER_WRITE_PATH_DIRECT;
static bool s_ram_fram = false;
static uint32_t s_cart_slot = 0;
static uint32_t s_rom_size_mib = 32;
static uint32_t s_save_size_kib = 128;
static uint32_t s_psram_mb = UI_FILE_PSRAM_WINDOW_MB;
static uint32_t s_mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
static uint32_t s_dump_chunk_kb = BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
static uint8_t s_ram_latency = 10;

static bool ui_take_model_lock(void);
static void ui_scan_file_window_locked(ui_model_t *model);
static void ui_set_status_locked(ui_model_t *model, const char *text);
static void ui_drop_nav_target_locked(ui_page_t page);

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
    model->dirty = true;
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

static void ui_px_corner_box(int32_t x, int32_t y, int32_t w, int32_t h)
{
    const int32_t len = UI_TILE_SELECT_LINE;

    ui_px_hline(x, y, len + 1, true);
    ui_px_vline(x, y, len + 1, true);
    ui_px_hline(x, y + h - 1, len + 1, true);
    ui_px_vline(x, y + h - len - 1, len + 1, true);
    ui_px_hline(x + w - len - 1, y, len + 1, true);
    ui_px_vline(x + w - 1, y, len + 1, true);
    ui_px_hline(x + w - len - 1, y + h - 1, len + 1, true);
    ui_px_vline(x + w - 1, y + h - len - 1, len + 1, true);
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

static int32_t ui_px_text_width(const char *text)
{
    int32_t w = 0;

    if (text == NULL) {
        return 0;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x80U) {
            c = '?';
        }
        (void)c;
        w += 6;
    }
    return (w > 0) ? (w - 1) : 0;
}

static void ui_px_draw_char(int32_t x, int32_t y, char ch, bool on)
{
    const uint8_t *glyph = ui_font5x7(ch);

    if ((unsigned char)ch >= 0x80U) {
        glyph = ui_font5x7('?');
    }
    for (int32_t col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int32_t row = 0; row < 7; ++row) {
            if ((bits & (1U << row)) != 0U) {
                ui_px_set(x + col, y + row, on);
            }
        }
    }
}

static void ui_px_text(int32_t x, int32_t y, const char *text, bool on)
{
    if (text == NULL) {
        return;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        char ch = *p;
        if ((unsigned char)ch >= 0x80U) {
            ch = '?';
        }
        ui_px_draw_char(x, y, ch, on);
        x += 6;
    }
}

static void ui_px_text_clipped(int32_t x, int32_t y, int32_t max_w, const char *text, bool on)
{
    char clipped[UI_ROW_TEXT_MAX_LEN] = {0};
    size_t out = 0;
    int32_t remaining = max_w;

    if (text == NULL || max_w <= 0) {
        return;
    }
    while (*text != '\0' && remaining >= 5 && out + 1U < sizeof(clipped)) {
        char ch = *text++;
        if ((unsigned char)ch >= 0x80U) {
            ch = '?';
        }
        clipped[out++] = ch;
        remaining -= 6;
    }
    clipped[out] = '\0';
    ui_px_text(x, y, clipped, on);
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

static void ui_format_bytes_text(uint32_t bytes, char *out, size_t out_len)
{
    ui_format_file_size(bytes, out, out_len);
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

static void ui_format_cart_mode(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    snprintf(out, out_len, "%s", s_cart_mode == BURNER_CART_MODE_GBA ? "GBA" : "MBC5");
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

static uint8_t ui_file_action_count_for_kind(ui_file_kind_t kind)
{
    if (kind == UI_FILE_KIND_ROM_GBA || kind == UI_FILE_KIND_ROM_MBC5) {
        return 3U;
    }
    if (kind == UI_FILE_KIND_SAVE) {
        return 2U;
    }
    return 0U;
}

static ui_file_action_t ui_file_action_for_kind(ui_file_kind_t kind, uint8_t index)
{
    if (kind == UI_FILE_KIND_SAVE) {
        return (index == 0U) ? UI_FILE_ACTION_WRITE_SAVE : UI_FILE_ACTION_VERIFY_SAVE;
    }
    switch (index) {
        case 0:
            return UI_FILE_ACTION_BURN_PSRAM;
        case 1:
            return UI_FILE_ACTION_BURN_DIRECT;
        case 2:
        default:
            return UI_FILE_ACTION_VERIFY_ROM;
    }
}

static const char *ui_file_action_label(ui_file_action_t action)
{
    switch (action) {
        case UI_FILE_ACTION_BURN_PSRAM:
            return "Burn via PSRAM";
        case UI_FILE_ACTION_BURN_DIRECT:
            return "Burn direct";
        case UI_FILE_ACTION_VERIFY_ROM:
            return "Verify ROM";
        case UI_FILE_ACTION_WRITE_SAVE:
            return "Write save";
        case UI_FILE_ACTION_VERIFY_SAVE:
            return "Verify save";
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
            return "Save file";
        case UI_FILE_KIND_UNSUPPORTED:
        default:
            return "File";
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
            return "System";
        case UI_PAGE_TF:
            return "TF";
        case UI_PAGE_FILES:
            return "Files";
        case UI_PAGE_FILE_ACTIONS:
            return "Actions";
        case UI_PAGE_WIFI:
            return "Wi-Fi";
        case UI_PAGE_POWER:
            return "Power";
        case UI_PAGE_BURNER:
            return "Burner";
        case UI_PAGE_BURN_ROM:
            return "ROM";
        case UI_PAGE_BURN_SAVE:
            return "Save";
        case UI_PAGE_SETTINGS:
            return "Settings";
        case UI_PAGE_TASK_STATUS:
            return "Task";
        default:
            return "MORI";
    }
}

static const char *ui_wifi_state_name(ui_wifi_state_t state)
{
    switch (state) {
        case UI_WIFI_STATE_CONNECTED:
            return "connected";
        case UI_WIFI_STATE_PROVISIONING:
            return "provisioning";
        case UI_WIFI_STATE_DISCONNECTED:
            return "disconnected";
        case UI_WIFI_STATE_UNKNOWN:
        default:
            return "unknown";
    }
}

static const char *ui_burn_state_text(burner_state_t state)
{
    switch (state) {
        case BURNER_STATE_IDLE:
            return "idle";
        case BURNER_STATE_RECEIVING:
            return "receiving";
        case BURNER_STATE_BURNING:
            return "running";
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
    if (text == NULL || text[0] == '\0') {
        return "";
    }
    if (strcmp(text, "system initializing") == 0 || strcmp(text, "system booting") == 0) {
        return "initializing";
    }
    if (strcmp(text, "system initialized") == 0 || strcmp(text, "ready") == 0) {
        return "ready";
    }
    if (strcmp(text, "connecting wifi.txt") == 0 || strcmp(text, "connecting saved wifi") == 0) {
        return "connecting Wi-Fi";
    }
    if (strcmp(text, "wifi connected") == 0) {
        return "Wi-Fi connected";
    }
    if (strcmp(text, "wifi disconnected") == 0) {
        return "Wi-Fi disconnected";
    }
    if (strcmp(text, "wifi provisioning mode") == 0 || strcmp(text, "no saved wifi, provisioning") == 0) {
        return "provisioning AP";
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
    DIR *dir = NULL;
    struct dirent *dirent = NULL;
    uint16_t visible_begin;
    uint16_t total = 0;
    uint16_t loaded = 0;

    if (model == NULL) {
        return;
    }

    memset(model->file_window, 0, sizeof(model->file_window));
    model->file_loaded_count = 0;
    model->file_total = 0;

    if (card == NULL) {
        ui_set_status_locked(model, "TF card not ready");
        return;
    }
    if (usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, "TF busy by USB host");
        return;
    }
    if (!burner_normalize_rel_path(model->file_path, normalized, sizeof(normalized), true)) {
        ui_set_status_locked(model, "invalid path");
        model->file_path[0] = '\0';
        normalized[0] = '\0';
    }
    snprintf(model->file_path, sizeof(model->file_path), "%s", normalized);
    if (!burner_build_full_path(normalized, full_path, sizeof(full_path))) {
        ui_set_status_locked(model, "path too long");
        return;
    }

    dir = opendir(full_path);
    if (dir == NULL) {
        ui_set_status_locked(model, "open directory failed");
        return;
    }

    while ((dirent = readdir(dir)) != NULL && total < UI_FILE_SCAN_LIMIT) {
        ui_file_entry_t temp;

        if (!ui_build_file_entry_for_dirent(normalized, dirent, &temp)) {
            continue;
        }
        total++;
    }

    if (model->file_selected >= total && total > 0U) {
        model->file_selected = total - 1U;
    }
    if (total == 0U) {
        model->file_selected = 0;
        model->file_scroll = 0;
    } else {
        if (model->file_selected < model->file_scroll) {
            model->file_scroll = model->file_selected;
        }
        if (model->file_selected >= model->file_scroll + UI_FILE_WINDOW_COUNT) {
            model->file_scroll = model->file_selected - UI_FILE_WINDOW_COUNT + 1U;
        }
    }
    visible_begin = model->file_scroll;

    rewinddir(dir);
    total = 0;
    while ((dirent = readdir(dir)) != NULL && total < UI_FILE_SCAN_LIMIT) {
        ui_file_entry_t temp;

        if (!ui_build_file_entry_for_dirent(normalized, dirent, &temp)) {
            continue;
        }
        if (total >= visible_begin && loaded < UI_FILE_WINDOW_COUNT) {
            temp.ordinal = total;
            model->file_window[loaded++] = temp;
        }
        total++;
    }
    closedir(dir);

    model->file_total = total;
    model->file_loaded_count = loaded;
    if (total >= UI_FILE_SCAN_LIMIT) {
        ui_set_status_locked(model, "directory clipped");
    } else if (total == 0U) {
        ui_set_status_locked(model, "empty directory");
    } else {
        ui_set_status_locked(model, normalized[0] == '\0' ? "TF root" : normalized);
    }
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

static void ui_file_move_locked(ui_model_t *model, int delta)
{
    int selected;

    if (model == NULL || model->file_total == 0U) {
        return;
    }
    selected = (int)model->file_selected + delta;
    if (selected < 0) {
        selected = 0;
    }
    if (selected >= (int)model->file_total) {
        selected = (int)model->file_total - 1;
    }
    if ((uint16_t)selected != model->file_selected) {
        model->file_selected = (uint16_t)selected;
        ui_scan_file_window_locked(model);
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

static void ui_open_file_action_page_locked(ui_model_t *model, const ui_file_entry_t *entry)
{
    if (model == NULL || entry == NULL) {
        return;
    }
    model->action_kind = ui_file_kind_from_name(entry->name);
    if (ui_file_action_count_for_kind(model->action_kind) == 0U) {
        ui_set_status_locked(model, "unsupported file");
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
        ui_set_status_locked(model, "unsupported file");
        return;
    }
    model->action_kind = kind;
    model->action_file = *entry;
    if (kind == UI_FILE_KIND_SAVE) {
        model->page = UI_PAGE_BURN_SAVE;
        model->parent_page = UI_PAGE_BURNER;
        ui_set_status_locked(model, "save selected");
    } else {
        s_cart_mode = ui_file_cart_mode_for_kind(kind);
        model->page = UI_PAGE_BURN_ROM;
        model->parent_page = UI_PAGE_BURNER;
        ui_set_status_locked(model, "ROM selected");
    }
    model->selected = 0;
    model->scroll = 0;
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
    if (s_file_start_active) {
        ui_set_status_locked(model, "task starting");
        return ESP_ERR_INVALID_STATE;
    }
    if (card == NULL) {
        ui_set_status_locked(model, "TF card not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, "TF busy by USB host");
        return ESP_ERR_INVALID_STATE;
    }

    action = ui_file_action_for_kind(model->action_kind, (uint8_t)model->selected);
    request = (ui_file_start_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        ui_set_status_locked(model, "no memory");
        return ESP_ERR_NO_MEM;
    }

    ui_utf8_safe_copy(request->name, sizeof(request->name), model->action_file.name);
    snprintf(request->path, sizeof(request->path), "%s", model->action_file.path);
    request->size = model->action_file.size;
    request->kind = model->action_kind;
    request->action = action;
    request->cart_mode = ui_file_cart_mode_for_kind(model->action_kind);
    request->slot = 0;
    request->write_path = (action == UI_FILE_ACTION_BURN_PSRAM) ? BURNER_WRITE_PATH_PSRAM : BURNER_WRITE_PATH_DIRECT;
    request->psram_mb = UI_FILE_PSRAM_WINDOW_MB;
    request->mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
    request->gba_force_no_cfi = false;
    request->ram_fram = false;
    request->ram_latency = 10U;

    s_file_start_active = true;
    model->page = UI_PAGE_TASK_STATUS;
    model->parent_page = UI_PAGE_ROOT;
    model->burn_progress = 0;
    model->burn_processed = 0;
    model->burn_total = request->size;
    ui_set_status_locked(model, "starting burn task");
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
    s_model.page = UI_PAGE_TASK_STATUS;
    s_model.parent_page = UI_PAGE_ROOT;
    if (err == ESP_OK) {
        s_model.burn_progress = 0;
        s_model.burn_processed = 0;
        s_model.burn_total = (result != NULL) ? result->effective_size : 0U;
        snprintf(s_model.status_text, sizeof(s_model.status_text), "%s started", label);
    } else {
        s_model.burn_progress = 0;
        s_model.burn_processed = 0;
        s_model.burn_total = 0;
        snprintf(s_model.status_text, sizeof(s_model.status_text), "start failed: %.72s", msg);
        ESP_LOGW(UI_TAG, "LCD file action start failed (%s): %s", label, msg);
    }
    s_model.dirty = true;
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
        case UI_FILE_ACTION_BURN_DIRECT:
            err = burner_start_write_from_tf(
                request->path,
                request->cart_mode,
                request->slot,
                request->write_path,
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
    ret = xTaskCreateWithCaps(
        ui_start_file_action_task,
        "ui_file_start",
        UI_FILE_START_TASK_STACK_SIZE,
        request,
        UI_FILE_START_TASK_PRIORITY,
        NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        return;
    }

    request->task_with_caps = false;
    ret = xTaskCreate(
        ui_start_file_action_task,
        "ui_file_start",
        UI_FILE_START_TASK_STACK_SIZE,
        request,
        UI_FILE_START_TASK_PRIORITY,
        NULL);
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
    s_model.page = UI_PAGE_TASK_STATUS;
    s_model.parent_page = UI_PAGE_BURNER;
    s_model.burn_progress = 0;
    s_model.burn_processed = 0;
    s_model.burn_total = total;
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
        gba_force_multi,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        dump_chunk_bytes,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        false,
        0U);
    if (err == ESP_OK) {
        ui_set_task_page_starting("ROM dump started", effective_size);
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
        s_ram_fram,
        s_ram_latency);
    if (err == ESP_OK) {
        ui_set_task_page_starting("save dump started", effective_size);
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
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        "chip_erase",
        "/cart",
        0U,
        1U,
        false,
        0U);

    if (err == ESP_OK) {
        ui_set_task_page_starting("chip erase started", 1U);
    }
    return err;
}

static esp_err_t ui_read_cart_id_once(char *out, size_t out_len)
{
    uint8_t gba_id[8] = {0};
    uint8_t mbc5_id[4] = {0};
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    bool cfi_ok = false;
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
    if (s_cart_mode == BURNER_CART_MODE_MBC5) {
        err = burner_bacon_mbc5_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_get_id(mbc5_id);
        }
        if (err == ESP_OK) {
            if (burner_bacon_mbc5_get_cfi(&device_size, &sector_size, &buffer_write_bytes) == ESP_OK) {
                cfi_ok = true;
            } else {
                (void)burner_mbc5_geometry_from_id(mbc5_id, &device_size, &sector_size, &buffer_write_bytes);
            }
        }
    } else {
        err = burner_bacon_gba_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_gba_probe_locked(gba_id, &device_size, &sector_size, &buffer_write_bytes, &cfi_ok);
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        return err;
    }
    if (s_cart_mode == BURNER_CART_MODE_MBC5) {
        snprintf(
            out,
            out_len,
            "ID %02X %02X %02X %02X %s",
            mbc5_id[0],
            mbc5_id[1],
            mbc5_id[2],
            mbc5_id[3],
            cfi_ok ? "CFI" : "no CFI");
    } else {
        snprintf(
            out,
            out_len,
            "ID %02X %02X %02X %02X %s",
            gba_id[0],
            gba_id[1],
            gba_id[2],
            gba_id[3],
            cfi_ok ? "CFI" : "no CFI");
    }
    return ESP_OK;
}

static void ui_finish_work_task(ui_work_type_t type, esp_err_t err, const char *ok_text)
{
    if (!ui_take_model_lock()) {
        return;
    }
    if (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
        type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLEAR_SAVED) {
        s_wifi_work_active = false;
    } else if (type == UI_WORK_STORAGE_USB_ENABLE || type == UI_WORK_STORAGE_USB_DISABLE) {
        s_storage_work_active = false;
    }

    if (err == ESP_OK) {
        ui_set_status_locked(&s_model, ok_text);
    } else {
        char msg[UI_STATUS_TEXT_MAX_LEN] = {0};
        snprintf(msg, sizeof(msg), "action failed: %s", esp_err_to_name(err));
        ui_set_status_locked(&s_model, msg);
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_work_task(void *param)
{
    ui_work_request_t *request = (ui_work_request_t *)param;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *ok_text = "done";
    char id_text[UI_STATUS_TEXT_MAX_LEN] = {0};

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }

    switch (request->type) {
        case UI_WORK_WIFI_CONNECT_SAVED:
            ok_text = "Wi-Fi connected";
            err = wifi_maneger_ready() ? wifi_maneger_connect_saved(15000U) : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_WIFI_START_AP:
            ok_text = "provisioning AP";
            err = wifi_maneger_ready() ? wifi_maneger_ap() : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_WIFI_DISCONNECT:
            wifi_maneger_disconnect();
            ok_text = "Wi-Fi disconnected";
            err = ESP_OK;
            break;
        case UI_WORK_WIFI_CLEAR_SAVED:
            ok_text = "saved Wi-Fi cleared";
            err = wifi_maneger_ready() ? wifi_maneger_clear_sta_config() : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_STORAGE_USB_ENABLE:
            ok_text = "USB pass-through enabled";
            err = usb_msc_tf_set_enabled(true);
            break;
        case UI_WORK_STORAGE_USB_DISABLE:
            ok_text = "USB pass-through disabled";
            err = usb_msc_tf_set_enabled(false);
            break;
        case UI_WORK_BURN_READ_ID:
            err = ui_read_cart_id_once(id_text, sizeof(id_text));
            ok_text = (err == ESP_OK) ? id_text : "read id failed";
            break;
        case UI_WORK_BURN_ERASE_CHIP:
            ok_text = "chip erase started";
            err = ui_start_chip_erase_task();
            break;
        case UI_WORK_BURN_DUMP_ROM:
            ok_text = "ROM dump started";
            err = ui_start_dump_rom_task();
            break;
        case UI_WORK_BURN_DUMP_SAVE:
            ok_text = "save dump started";
            err = ui_start_dump_save_task();
            break;
        case UI_WORK_BURN_UNLOCK_PPB:
            ok_text = "PPB unlock unavailable on LCD";
            err = ESP_ERR_NOT_SUPPORTED;
            break;
        case UI_WORK_BRIGHTNESS_UP:
            ok_text = "brightness up";
            err = lcd_display_set_brightness((uint8_t)((lcd_display_get_brightness() > 223U) ? 255U : (lcd_display_get_brightness() + 32U)));
            break;
        case UI_WORK_BRIGHTNESS_DOWN:
            ok_text = "brightness down";
            err = lcd_display_set_brightness((uint8_t)((lcd_display_get_brightness() < 32U) ? 0U : (lcd_display_get_brightness() - 32U)));
            break;
        case UI_WORK_DEVICE_RESTART:
            ok_text = "restarting";
            burner_schedule_restart();
            err = ESP_OK;
            break;
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    ui_finish_work_task(request->type, err, ok_text);
    free(request);
    vTaskDelete(NULL);
}

static void ui_start_work_async(ui_work_type_t type)
{
    ui_work_request_t *request = NULL;
    bool *active = NULL;
    const char *status = "working";

    if (!ui_take_model_lock()) {
        return;
    }
    if (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
        type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLEAR_SAVED) {
        active = &s_wifi_work_active;
        status = "Wi-Fi working";
    } else if (type == UI_WORK_STORAGE_USB_ENABLE || type == UI_WORK_STORAGE_USB_DISABLE) {
        active = &s_storage_work_active;
        status = "storage working";
    } else {
        active = NULL;
        status = "working";
    }
    if (active != NULL && *active) {
        ui_set_status_locked(&s_model, "action already running");
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
        ui_finish_work_task(type, ESP_ERR_NO_MEM, NULL);
        return;
    }
    request->type = type;

    if (xTaskCreate(
            ui_work_task,
            "ui_work",
            (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
             type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLEAR_SAVED) ?
                UI_WIFI_TASK_STACK_SIZE :
                UI_STORAGE_TASK_STACK_SIZE,
            request,
            (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
             type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLEAR_SAVED) ?
                UI_WIFI_TASK_PRIORITY :
                UI_STORAGE_TASK_PRIORITY,
            NULL) != pdPASS) {
        free(request);
        ui_finish_work_task(type, ESP_ERR_NO_MEM, NULL);
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
            return UI_BURNER_ITEM_COUNT;
        case UI_PAGE_BURN_ROM:
            return UI_BURN_ROM_ITEM_COUNT;
        case UI_PAGE_BURN_SAVE:
            return UI_BURN_SAVE_ITEM_COUNT;
        case UI_PAGE_SETTINGS:
            return UI_SETTINGS_ITEM_COUNT;
        case UI_PAGE_TASK_STATUS:
            return 7;
        default:
            return 0;
    }
}

static void ui_push_current_page_locked(const ui_model_t *model)
{
    if (model == NULL || s_nav_depth >= (sizeof(s_nav_stack) / sizeof(s_nav_stack[0]))) {
        return;
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
    model->selected = 0;
    model->scroll = 0;
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
    model->selected = 0;
    model->scroll = 0;
    s_nav_depth = 0;
    ui_set_status_locked(model, "ready");
}

static void ui_ensure_visible_locked(ui_model_t *model)
{
    uint16_t count;
    uint16_t *selected = NULL;
    uint16_t *scroll = NULL;

    if (model == NULL) {
        return;
    }
    if (model->page == UI_PAGE_FILES) {
        selected = &model->file_selected;
        scroll = &model->file_scroll;
    } else {
        selected = &model->selected;
        scroll = &model->scroll;
    }
    count = ui_page_item_count(model);
    if (count == 0U) {
        *selected = 0;
        *scroll = 0;
        return;
    }
    if (*selected >= count) {
        *selected = count - 1U;
    }
    if (*selected < *scroll) {
        *scroll = *selected;
    }
    if (*selected >= *scroll + UI_ROW_COUNT) {
        *scroll = *selected - UI_ROW_COUNT + 1U;
    }
    if (model->page == UI_PAGE_FILES) {
        ui_scan_file_window_locked(model);
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
    }

    count = ui_page_item_count(model);
    if (count == 0U || delta == 0) {
        return;
    }
    selected = (int)model->selected + delta;
    if (model->page == UI_PAGE_ROOT) {
        if (selected < 0) {
            selected = (int)count - 1;
        }
        if (selected >= (int)count) {
            selected = 0;
        }
    } else {
        if (selected < 0) {
            selected = 0;
        }
        if (selected >= (int)count) {
            selected = (int)count - 1;
        }
    }
    model->selected = (uint16_t)selected;
    ui_ensure_visible_locked(model);
    model->dirty = true;
}

static void ui_back_locked(ui_model_t *model)
{
    char parent[TF_PATH_LEN_MAX] = {0};

    if (model == NULL) {
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
        model->page = model->parent_page;
        model->parent_page = UI_PAGE_BURNER;
        model->selected = 0;
        model->scroll = 0;
        ui_drop_nav_target_locked(model->page);
        model->dirty = true;
        return;
    }
    if (model->page == UI_PAGE_FILE_ACTIONS) {
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
        model->selected = 0;
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
    bool want_save = (action == UI_FILE_ACTION_WRITE_SAVE || action == UI_FILE_ACTION_VERIFY_SAVE);
    bool is_rom = (model != NULL &&
                   (model->action_kind == UI_FILE_KIND_ROM_GBA || model->action_kind == UI_FILE_KIND_ROM_MBC5));

    if (request_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *request_out = NULL;
    if (model == NULL || model->action_file.path[0] == '\0') {
        ui_set_status_locked(model, want_save ? "select TF save first" : "select TF ROM first");
        return ESP_ERR_INVALID_STATE;
    }
    if ((want_save && model->action_kind != UI_FILE_KIND_SAVE) || (!want_save && !is_rom)) {
        ui_set_status_locked(model, want_save ? "selected file is not save" : "selected file is not ROM");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_file_start_active) {
        ui_set_status_locked(model, "task starting");
        return ESP_ERR_INVALID_STATE;
    }
    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, "TF not available");
        return ESP_ERR_INVALID_STATE;
    }

    request = (ui_file_start_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        ui_set_status_locked(model, "no memory");
        return ESP_ERR_NO_MEM;
    }

    ui_utf8_safe_copy(request->name, sizeof(request->name), model->action_file.name);
    snprintf(request->path, sizeof(request->path), "%s", model->action_file.path);
    request->size = model->action_file.size;
    request->kind = model->action_kind;
    request->action = action;
    request->cart_mode = ui_file_cart_mode_for_kind(model->action_kind);
    request->slot = s_cart_slot;
    request->write_path = (action == UI_FILE_ACTION_BURN_PSRAM) ? BURNER_WRITE_PATH_PSRAM : s_write_path;
    request->psram_mb = s_psram_mb;
    request->mbc5_chunk_kb = s_mbc5_chunk_kb;
    request->gba_force_no_cfi = false;
    request->ram_fram = s_ram_fram;
    request->ram_latency = s_ram_latency;

    s_file_start_active = true;
    model->page = UI_PAGE_TASK_STATUS;
    model->parent_page = UI_PAGE_BURNER;
    model->burn_progress = 0;
    model->burn_processed = 0;
    model->burn_total = request->size;
    ui_set_status_locked(model, "starting burn task");
    *request_out = request;
    return ESP_OK;
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
                ui_set_status_locked(model, "system overview");
            }
            break;
        case UI_PAGE_TF:
            if (model->selected == 0U) {
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
                ui_set_status_locked(model, "use web page for this TF action");
            }
            break;
        case UI_PAGE_FILES:
            if (!ui_current_file_locked(model, &entry)) {
                ui_set_status_locked(model, "no file selected");
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
            if (model->selected == 0U) {
                s_cart_mode = (s_cart_mode == BURNER_CART_MODE_GBA) ? BURNER_CART_MODE_MBC5 : BURNER_CART_MODE_GBA;
                ui_set_status_locked(model, s_cart_mode == BURNER_CART_MODE_GBA ? "mode GBA" : "mode MBC5");
            } else if (model->selected == 1U) {
                ui_open_page_locked(model, UI_PAGE_BURN_ROM);
            } else if (model->selected == 2U) {
                ui_open_page_locked(model, UI_PAGE_BURN_SAVE);
            } else if (model->selected == 3U) {
                ui_open_page_locked(model, UI_PAGE_TASK_STATUS);
            } else if (model->selected == 4U) {
                s_write_path = (s_write_path == BURNER_WRITE_PATH_DIRECT) ? BURNER_WRITE_PATH_PSRAM : BURNER_WRITE_PATH_DIRECT;
                ui_set_status_locked(model, burner_write_path_to_str(s_write_path));
            } else if (model->selected == 5U) {
                s_cart_slot = (s_cart_slot >= BURNER_MBC5_SLOT_MAX) ? 0U : (s_cart_slot + 1U);
                ui_set_status_locked(model, "slot changed");
            } else if (model->selected == 6U) {
                *work_type = UI_WORK_BURN_READ_ID;
                *start_work = true;
            } else if (model->selected == 7U) {
                *work_type = UI_WORK_BURN_ERASE_CHIP;
                *start_work = true;
            }
            break;
        case UI_PAGE_BURN_ROM:
            if (model->selected == 0U) {
                ui_open_page_locked(model, UI_PAGE_FILES);
            } else if (model->selected == 1U) {
                (void)ui_prepare_last_file_action_locked(
                    model,
                    (s_write_path == BURNER_WRITE_PATH_PSRAM) ? UI_FILE_ACTION_BURN_PSRAM : UI_FILE_ACTION_BURN_DIRECT,
                    start_request);
            } else if (model->selected == 2U) {
                (void)ui_prepare_last_file_action_locked(model, UI_FILE_ACTION_VERIFY_ROM, start_request);
            } else if (model->selected == 3U) {
                *work_type = UI_WORK_BURN_DUMP_ROM;
                *start_work = true;
            } else if (model->selected == 4U) {
                s_rom_size_mib = ui_next_option_u32(s_rom_size_mib_options, sizeof(s_rom_size_mib_options) / sizeof(s_rom_size_mib_options[0]), s_rom_size_mib, 1);
                ui_set_status_locked(model, "ROM size changed");
            } else if (model->selected == 5U) {
                s_cart_slot = (s_cart_slot >= BURNER_MBC5_SLOT_MAX) ? 0U : (s_cart_slot + 1U);
                ui_set_status_locked(model, "slot changed");
            } else if (model->selected == 6U) {
                s_write_path = (s_write_path == BURNER_WRITE_PATH_DIRECT) ? BURNER_WRITE_PATH_PSRAM : BURNER_WRITE_PATH_DIRECT;
                ui_set_status_locked(model, burner_write_path_to_str(s_write_path));
            } else if (model->selected == 7U) {
                s_psram_mb = ui_next_option_u32(s_psram_mb_options, sizeof(s_psram_mb_options) / sizeof(s_psram_mb_options[0]), s_psram_mb, 1);
                ui_set_status_locked(model, "PSRAM window changed");
            } else if (model->selected == 8U) {
                s_mbc5_chunk_kb = ui_next_option_u32(s_mbc5_chunk_kb_options, sizeof(s_mbc5_chunk_kb_options) / sizeof(s_mbc5_chunk_kb_options[0]), s_mbc5_chunk_kb, 1);
                ui_set_status_locked(model, "MBC5 chunk changed");
            } else if (model->selected == 9U) {
                s_dump_chunk_kb = ui_next_option_u32(s_dump_chunk_kb_options, sizeof(s_dump_chunk_kb_options) / sizeof(s_dump_chunk_kb_options[0]), s_dump_chunk_kb, 1);
                ui_set_status_locked(model, "dump chunk changed");
            } else if (model->selected == 10U) {
                *work_type = UI_WORK_BURN_READ_ID;
                *start_work = true;
            } else if (model->selected == 11U) {
                *work_type = UI_WORK_BURN_ERASE_CHIP;
                *start_work = true;
            }
            break;
        case UI_PAGE_BURN_SAVE:
            if (model->selected == 0U) {
                ui_open_page_locked(model, UI_PAGE_FILES);
            } else if (model->selected == 1U) {
                (void)ui_prepare_last_file_action_locked(model, UI_FILE_ACTION_WRITE_SAVE, start_request);
            } else if (model->selected == 2U) {
                (void)ui_prepare_last_file_action_locked(model, UI_FILE_ACTION_VERIFY_SAVE, start_request);
            } else if (model->selected == 3U) {
                *work_type = UI_WORK_BURN_DUMP_SAVE;
                *start_work = true;
            } else if (model->selected == 4U) {
                s_save_size_kib = ui_next_option_u32(s_save_size_kib_options, sizeof(s_save_size_kib_options) / sizeof(s_save_size_kib_options[0]), s_save_size_kib, 1);
                ui_set_status_locked(model, "save size changed");
            } else if (model->selected == 5U) {
                s_ram_fram = !s_ram_fram;
                ui_set_status_locked(model, s_ram_fram ? "FRAM" : "SRAM");
            } else if (model->selected == 6U) {
                s_ram_latency = (uint8_t)((s_ram_latency >= 255U) ? 0U : (s_ram_latency + 1U));
                ui_set_status_locked(model, "latency changed");
            } else if (model->selected == 7U) {
                s_cart_slot = (s_cart_slot >= BURNER_MBC5_SLOT_MAX) ? 0U : (s_cart_slot + 1U);
                ui_set_status_locked(model, "slot changed");
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
                ui_set_status_locked(model, "use web page for this setting");
            }
            break;
        case UI_PAGE_TASK_STATUS:
            if (burner_cancel_request()) {
                ui_set_status_locked(model, "cancel requested");
            } else {
                ui_set_status_locked(model, "nothing to cancel");
            }
            break;
        default:
            ui_set_status_locked(model, "read only");
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
        ui_set_status_locked(model, "refreshed");
    }
    model->dirty = true;
}

static void ui_handle_button_now(ui_button_t button, bool pressed)
{
    ui_file_start_request_t *start_request = NULL;
    ui_work_type_t work_type = UI_WORK_WIFI_CONNECT_SAVED;
    bool start_work = false;

    if (!pressed || button < UI_BUTTON_LEFT || button > UI_BUTTON_MENU) {
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

    s_model.dirty = true;
    xSemaphoreGive(s_model_lock);

    if (start_request != NULL) {
        ui_start_file_action_async(start_request);
    }
    if (start_work) {
        ui_start_work_async(work_type);
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
        ui_handle_button_now(event.button, true);
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
        s_model.dirty = true;
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_update_battery_if_needed(uint32_t now_ms)
{
    uint8_t percent = 0;
    bool valid = false;
    bool charging = false;
    ip5306_status_t status = {0};

    if (s_last_battery_refresh_ms != 0U && (now_ms - s_last_battery_refresh_ms) < UI_BATTERY_REFRESH_MS) {
        return;
    }
    s_last_battery_refresh_ms = now_ms;

    if (ip5306_ready() && ip5306_get_battery_level_percent(&percent) == ESP_OK) {
        valid = true;
        if (ip5306_get_status(&status) == ESP_OK) {
            charging = status.charge_enable && !status.charge_full;
        }
    }

    if (!ui_take_model_lock()) {
        return;
    }
    if (s_model.battery_valid != valid || s_model.battery_percent != percent ||
        s_model.battery_charging != charging) {
        s_model.battery_valid = valid;
        s_model.battery_percent = percent;
        s_model.battery_charging = charging;
        s_model.dirty = true;
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_update_burn_snapshot_if_needed(uint32_t now_ms)
{
    burner_status_t status = {0};
    int progress;
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
    if (status.state == BURNER_STATE_DONE || status.state == BURNER_STATE_ERROR ||
        status.state == BURNER_STATE_CANCELLED) {
        release_start = true;
    }

    if (!ui_take_model_lock()) {
        return;
    }
    if (s_model.burn_progress != progress ||
        s_model.burn_processed != status.processed_bytes ||
        s_model.burn_total != status.total_bytes) {
        s_model.burn_progress = progress;
        s_model.burn_processed = status.processed_bytes;
        s_model.burn_total = status.total_bytes;
        s_model.dirty = true;
    }
    if (s_model.page == UI_PAGE_TASK_STATUS) {
        s_model.dirty = true;
    }
    xSemaphoreGive(s_model_lock);

    if (release_start) {
        s_file_start_active = false;
    }
}

static void ui_refresh_sources(void)
{
    uint32_t now_ms = esp_log_timestamp();

    ui_update_clock_if_needed(now_ms);
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

static void ui_fill_wifi_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    switch (index) {
        case 0:
            snprintf(title, title_len, "Current");
            snprintf(hint, hint_len, "%s %s", ui_wifi_state_name(model->wifi_state), model->ip_text);
            break;
        case 1:
            snprintf(title, title_len, "Connect saved");
            snprintf(hint, hint_len, "STA profile");
            break;
        case 2:
            snprintf(title, title_len, "Provision AP");
            snprintf(hint, hint_len, "Setup portal");
            break;
        case 3:
            snprintf(title, title_len, "Disconnect");
            snprintf(hint, hint_len, "Stop STA");
            break;
        default:
            snprintf(title, title_len, "Clear saved");
            snprintf(hint, hint_len, "Forget profile");
            break;
    }
    *symbol = (index == 3U) ? LV_SYMBOL_CLOSE : LV_SYMBOL_WIFI;
    *accent = (model->wifi_state == UI_WIFI_STATE_CONNECTED) ? 0x72EFDD : 0xF7B32B;
}

static void ui_fill_file_row(const ui_model_t *model, uint16_t visible_index, char *title, size_t title_len, char *hint, size_t hint_len, const char **symbol, uint32_t *accent)
{
    const ui_file_entry_t *entry = NULL;
    char size_text[24] = {0};

    if (visible_index >= model->file_loaded_count) {
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

    entry = &model->file_window[visible_index];
    snprintf(title, title_len, "%s", entry->name);
    if (entry->is_dir) {
        snprintf(hint, hint_len, "dir");
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
            snprintf(title, title_len, "Device");
            snprintf(hint, hint_len, "%s", app != NULL ? app->project_name : "MORI Burner");
            break;
        case 1:
            snprintf(title, title_len, "Version");
            snprintf(hint, hint_len, "%s", app != NULL ? app->version : "unknown");
            break;
        case 2:
            snprintf(title, title_len, "Heap");
            ui_format_u64_size((uint64_t)esp_get_free_heap_size(), size_text, sizeof(size_text));
            snprintf(hint, hint_len, "%s free", size_text);
            break;
        case 3:
            snprintf(title, title_len, "Uptime");
            ui_format_uptime((uint64_t)(esp_timer_get_time() / 1000LL), uptime_text, sizeof(uptime_text));
            snprintf(hint, hint_len, "%s", uptime_text);
            break;
        case 4:
            snprintf(title, title_len, "Task status");
            snprintf(hint, hint_len, "%d%%", model->burn_progress);
            break;
        case 5:
            snprintf(title, title_len, "TF manager");
            snprintf(hint, hint_len, "%s", card != NULL ? "ready" : "missing");
            break;
        default:
            snprintf(title, title_len, "Settings");
            snprintf(hint, hint_len, "device tools");
            break;
    }
}

static void ui_fill_tf_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    (void)model;
    switch (index) {
        case 0:
            snprintf(title, title_len, "Browse TF");
            snprintf(hint, hint_len, "%s", card != NULL ? "open files" : "card missing");
            break;
        case 1:
            snprintf(title, title_len, "Upload files");
            snprintf(hint, hint_len, "web action");
            break;
        case 2:
            snprintf(title, title_len, "New folder");
            snprintf(hint, hint_len, "web action");
            break;
        case 3:
            snprintf(title, title_len, "Delete/Rename");
            snprintf(hint, hint_len, "web action");
            break;
        case 4:
            snprintf(title, title_len, "Refresh");
            snprintf(hint, hint_len, "%s", usb_msc_tf_in_use_by_host() ? "USB owns TF" : "ESP owns TF");
            break;
        case 5:
            snprintf(title, title_len, "Enable USB");
            snprintf(hint, hint_len, "PC owns TF");
            break;
        default:
            snprintf(title, title_len, "Disable USB");
            snprintf(hint, hint_len, "ESP owns TF");
            break;
    }
}

static void ui_fill_power_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    char size_text[32] = {0};
    char uptime_text[24] = {0};

    switch (index) {
        case 0:
            snprintf(title, title_len, "Battery");
            if (model->battery_valid) {
                snprintf(hint, hint_len, "%u%%", (unsigned)model->battery_percent);
            } else {
                snprintf(hint, hint_len, "--");
            }
            break;
        case 1:
            snprintf(title, title_len, "Charging");
            snprintf(hint, hint_len, "%s", model->battery_charging ? "yes" : "no");
            break;
        case 2:
            snprintf(title, title_len, "Uptime");
            ui_format_uptime((uint64_t)(esp_timer_get_time() / 1000LL), uptime_text, sizeof(uptime_text));
            snprintf(hint, hint_len, "%s", uptime_text);
            break;
        case 3:
            snprintf(title, title_len, "Heap");
            ui_format_u64_size((uint64_t)esp_get_free_heap_size(), size_text, sizeof(size_text));
            snprintf(hint, hint_len, "%s", size_text);
            break;
        case 4:
            snprintf(title, title_len, "IP5306");
            snprintf(hint, hint_len, "%s", ip5306_ready() ? "ready" : "missing");
            break;
        default:
            snprintf(title, title_len, "TCA9555");
            snprintf(hint, hint_len, "%s", tca9555_ready() ? "ready" : "missing");
            break;
    }
}

static void ui_fill_burner_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    char mode[12] = {0};
    (void)model;
    ui_format_cart_mode(mode, sizeof(mode));
    switch (index) {
        case 0:
            snprintf(title, title_len, "Cart mode");
            snprintf(hint, hint_len, "%s", mode);
            break;
        case 1:
            snprintf(title, title_len, "ROM operations");
            snprintf(hint, hint_len, "write/dump/verify");
            break;
        case 2:
            snprintf(title, title_len, "Save operations");
            snprintf(hint, hint_len, "write/dump/verify");
            break;
        case 3:
            snprintf(title, title_len, "Task status");
            snprintf(hint, hint_len, "live progress");
            break;
        case 4:
            snprintf(title, title_len, "Write path");
            snprintf(hint, hint_len, "%s", burner_write_path_to_str(s_write_path));
            break;
        case 5:
            snprintf(title, title_len, "Slot");
            snprintf(hint, hint_len, "%" PRIu32, s_cart_slot);
            break;
        case 6:
            snprintf(title, title_len, "Read ID");
            snprintf(hint, hint_len, "%s", mode);
            break;
        default:
            snprintf(title, title_len, "Erase chip");
            snprintf(hint, hint_len, "%s", mode);
            break;
    }
}

static void ui_fill_burn_rom_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    char mode[12] = {0};
    (void)model;
    ui_format_cart_mode(mode, sizeof(mode));
    switch (index) {
        case 0:
            snprintf(title, title_len, "Choose TF ROM");
            snprintf(hint, hint_len, "%s", model->action_file.path[0] != '\0' ? model->action_file.name : "not selected");
            break;
        case 1:
            snprintf(title, title_len, "Write ROM");
            snprintf(hint, hint_len, "%s %s", mode, burner_write_path_to_str(s_write_path));
            break;
        case 2:
            snprintf(title, title_len, "Verify ROM");
            snprintf(hint, hint_len, "%s", mode);
            break;
        case 3:
            snprintf(title, title_len, "Dump ROM");
            snprintf(hint, hint_len, "%" PRIu32 " MiB", s_rom_size_mib);
            break;
        case 4:
            snprintf(title, title_len, "ROM size");
            snprintf(hint, hint_len, "%" PRIu32 " MiB", s_rom_size_mib);
            break;
        case 5:
            snprintf(title, title_len, "Slot");
            snprintf(hint, hint_len, "%" PRIu32, s_cart_slot);
            break;
        case 6:
            snprintf(title, title_len, "Write path");
            snprintf(hint, hint_len, "%s", burner_write_path_to_str(s_write_path));
            break;
        case 7:
            snprintf(title, title_len, "PSRAM window");
            snprintf(hint, hint_len, "%" PRIu32 " MB", s_psram_mb);
            break;
        case 8:
            snprintf(title, title_len, "MBC5 chunk");
            snprintf(hint, hint_len, "%" PRIu32 " KB", s_mbc5_chunk_kb);
            break;
        case 9:
            snprintf(title, title_len, "Dump chunk");
            snprintf(hint, hint_len, "%" PRIu32 " KB", s_dump_chunk_kb);
            break;
        case 10:
            snprintf(title, title_len, "Read ID");
            snprintf(hint, hint_len, "%s", mode);
            break;
        default:
            snprintf(title, title_len, "Erase chip");
            snprintf(hint, hint_len, "%s", mode);
            break;
    }
}

static void ui_fill_burn_save_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    switch (index) {
        case 0:
            snprintf(title, title_len, "Choose TF save");
            snprintf(hint, hint_len, "%s", model->action_file.path[0] != '\0' ? model->action_file.name : "not selected");
            break;
        case 1:
            snprintf(title, title_len, "Write save");
            snprintf(hint, hint_len, "MBC5 only");
            break;
        case 2:
            snprintf(title, title_len, "Verify save");
            snprintf(hint, hint_len, "MBC5 only");
            break;
        case 3:
            snprintf(title, title_len, "Dump save");
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            break;
        case 4:
            snprintf(title, title_len, "Save size");
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            break;
        case 5:
            snprintf(title, title_len, "RAM type");
            snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
            break;
        case 6:
            snprintf(title, title_len, "FRAM latency");
            snprintf(hint, hint_len, "%u", (unsigned)s_ram_latency);
            break;
        default:
            snprintf(title, title_len, "Slot");
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

    burner_status_snapshot(&status);
    switch (index) {
        case 0:
            snprintf(title, title_len, "State");
            snprintf(hint, hint_len, "%s", ui_burn_state_text(status.state));
            break;
        case 1:
            snprintf(title, title_len, "Progress");
            snprintf(hint, hint_len, "%d%%", model->burn_progress);
            break;
        case 2:
            snprintf(title, title_len, "Bytes");
            ui_format_bytes_text(model->burn_processed, size_a, sizeof(size_a));
            ui_format_bytes_text(model->burn_total, size_b, sizeof(size_b));
            snprintf(hint, hint_len, "%s/%s", size_a, size_b);
            break;
        case 3:
            snprintf(title, title_len, "Speed");
            ui_format_speed_text(status.speed_current_bps, speed, sizeof(speed));
            snprintf(hint, hint_len, "%s", speed);
            break;
        case 4:
            snprintf(title, title_len, "Average");
            ui_format_speed_text(status.speed_avg_bps, speed, sizeof(speed));
            snprintf(hint, hint_len, "%s", speed);
            break;
        case 5:
            snprintf(title, title_len, "ROM/File");
            snprintf(hint, hint_len, "%s", status.rom_name[0] != '\0' ? status.rom_name : "--");
            break;
        default:
            snprintf(title, title_len, "Cancel task");
            snprintf(hint, hint_len, "%s", burner_status_is_operation_active_state(status.state) ? "select to cancel" : "idle");
            break;
    }
}

static void ui_fill_settings_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    (void)model;
    switch (index) {
        case 0:
            snprintf(title, title_len, "Brightness +");
            snprintf(hint, hint_len, "%u", (unsigned)lcd_display_get_brightness());
            break;
        case 1:
            snprintf(title, title_len, "Brightness -");
            snprintf(hint, hint_len, "%u", (unsigned)lcd_display_get_brightness());
            break;
        case 2:
            snprintf(title, title_len, "Restart");
            snprintf(hint, hint_len, "reboot device");
            break;
        case 3:
            snprintf(title, title_len, "Storage");
            snprintf(hint, hint_len, "TF/USB");
            break;
        case 4:
            snprintf(title, title_len, "Wi-Fi");
            snprintf(hint, hint_len, "network");
            break;
        case 5:
            snprintf(title, title_len, "Power");
            snprintf(hint, hint_len, "telemetry");
            break;
        case 6:
            snprintf(title, title_len, "Task status");
            snprintf(hint, hint_len, "burn progress");
            break;
        case 7:
            snprintf(title, title_len, "Language");
            snprintf(hint, hint_len, "web action");
            break;
        case 8:
            snprintf(title, title_len, "Firmware OTA");
            snprintf(hint, hint_len, "web action");
            break;
        default:
            snprintf(title, title_len, "Web deploy");
            snprintf(hint, hint_len, "web action");
            break;
    }
}

static void ui_px_icon(uint16_t index, int32_t x, int32_t y, bool selected)
{
    ui_px_frame(x + 1, y + 1, UI_TILE_ICON_SIZE - 2, UI_TILE_ICON_SIZE - 2, true);
    switch (s_root_items[index].action) {
        case UI_ACTION_OPEN_SYSTEM:
            ui_px_frame(x + 6, y + 7, 18, 13, true);
            ui_px_hline(x + 10, y + 23, 10, true);
            ui_px_vline(x + 15, y + 20, 3, true);
            break;
        case UI_ACTION_OPEN_TF:
            ui_px_frame(x + 6, y + 8, 18, 14, true);
            ui_px_hline(x + 8, y + 6, 8, true);
            ui_px_vline(x + 6, y + 7, 3, true);
            break;
        case UI_ACTION_OPEN_BURNER:
            ui_px_hline(x + 14, y + 5, 1, true);
            ui_px_hline(x + 12, y + 6, 5, true);
            ui_px_hline(x + 10, y + 8, 9, true);
            ui_px_hline(x + 9, y + 11, 12, true);
            ui_px_hline(x + 10, y + 16, 10, true);
            ui_px_hline(x + 12, y + 20, 6, true);
            break;
        case UI_ACTION_OPEN_WIFI:
            ui_px_hline(x + 7, y + 9, 16, true);
            ui_px_hline(x + 9, y + 12, 12, true);
            ui_px_hline(x + 11, y + 15, 8, true);
            ui_px_hline(x + 13, y + 18, 4, true);
            ui_px_box(x + 14, y + 22, 2, 2, true);
            break;
        case UI_ACTION_OPEN_POWER:
            ui_px_frame(x + 8, y + 9, 14, 12, true);
            ui_px_box(x + 22, y + 13, 2, 4, true);
            ui_px_box(x + 11, y + 12, 8, 6, true);
            break;
        case UI_ACTION_OPEN_SETTINGS:
        default:
            ui_px_hline(x + 10, y + 14, 10, true);
            ui_px_vline(x + 15, y + 9, 10, true);
            ui_px_frame(x + 12, y + 11, 6, 6, true);
            ui_px_set(x + 8, y + 8, true);
            ui_px_set(x + 22, y + 8, true);
            ui_px_set(x + 8, y + 22, true);
            ui_px_set(x + 22, y + 22, true);
            break;
    }
    if (selected) {
        ui_px_corner_box(
            x - UI_TILE_SELECTOR_MARGIN,
            y - UI_TILE_SELECTOR_MARGIN,
            UI_TILE_ICON_SIZE + UI_TILE_SELECTOR_MARGIN * 2,
            UI_TILE_ICON_SIZE + UI_TILE_SELECTOR_MARGIN * 2);
    }
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
    ui_anim_move(&s_anim.tile_camera_x, s_anim.tile_camera_target_x, 80.0f);
    ui_anim_move(&s_anim.tile_bar_w, s_anim.tile_bar_target_w, 70.0f);
    ui_anim_move(&s_anim.tile_fore_y, s_anim.tile_fore_target_y, 70.0f);

    ui_px_text(8, 10, "MORI", true);
    ui_px_text_clipped(UI_CANVAS_W - 78, 10, 70, model->time_text, true);
    ui_px_text((UI_CANVAS_W - ui_px_text_width("ad astra per aspera")) / 2, 28, "ad astra per aspera", true);
    ui_px_box(0, 0, progress_w, UI_TILE_BAR_H, true);
    for (uint16_t i = 0; i < UI_ROOT_ITEM_COUNT; ++i) {
        int32_t x = center_x + (int32_t)((float)i * (float)UI_TILE_ICON_SPACING - s_anim.tile_camera_x);
        if (x <= -UI_TILE_ICON_SIZE || x >= UI_CANVAS_W) {
            continue;
        }
        ui_px_icon(i, x, UI_TILE_ICON_Y, i == selected);
    }

    ui_px_text((UI_CANVAS_W - ui_px_text_width(item->title)) / 2, UI_TILE_TITLE_Y, item->title, true);
    ui_px_text((UI_CANVAS_W - ui_px_text_width(item->hint)) / 2, UI_TILE_HINT_Y, item->hint, true);
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

static void ui_px_apply_list(const ui_model_t *model)
{
    uint16_t count = ui_page_item_count(model);
    uint16_t selected = ui_current_selected(model);
    uint16_t scroll = ui_current_scroll(model);
    int32_t bar_h = 1;
    int32_t list_h = UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H;
    int32_t selected_row = (int32_t)selected - (int32_t)scroll;
    int32_t selector_y;
    int32_t selector_w;
    char header[UI_ROW_TEXT_MAX_LEN] = {0};

    snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
    if (model->page == UI_PAGE_FILES && model->file_path[0] != '\0') {
        snprintf(header, sizeof(header), "TF/");
        strncat(header, model->file_path, sizeof(header) - strlen(header) - 1U);
    }

    ui_px_text(4, 5, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 92, 5, 84, model->time_text, true);
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
    ui_anim_move(&s_anim.list_bar_h, s_anim.list_bar_target_h, 70.0f);
    ui_anim_move(&s_anim.list_bar_x, s_anim.list_bar_target_x, 70.0f);
    ui_px_box((int32_t)s_anim.list_bar_x, UI_LIST_HEADER_H, UI_LIST_BAR_W, (int32_t)s_anim.list_bar_h, true);

    for (uint16_t row = 0; row < UI_ROW_COUNT; ++row) {
        uint16_t index = scroll + row;
        int32_t y = UI_LIST_HEADER_H + (int32_t)row * UI_LIST_LINE_H;
        char title[UI_ROW_TEXT_MAX_LEN] = {0};
        char hint[UI_ROW_TEXT_MAX_LEN] = {0};
        int32_t title_w;

        if (index >= count) {
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
    ui_anim_move(&s_anim.list_selector_y, s_anim.list_selector_target_y, 60.0f);
    ui_anim_move(&s_anim.list_selector_w, s_anim.list_selector_target_w, 70.0f);
    ui_px_invert_rect(0, (int32_t)s_anim.list_selector_y, (int32_t)s_anim.list_selector_w, UI_LIST_LINE_H - 1);
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
}

static void ui_px_render(const ui_model_t *model)
{
    if (s_canvas == NULL || s_canvas_buf == NULL) {
        return;
    }
    if (s_anim.page != model->page) {
        s_anim.page = model->page;
        s_anim.page_changed = true;
        if (model->page == UI_PAGE_ROOT) {
            s_anim.tile_fore_y = (float)UI_CANVAS_H;
        } else {
            s_anim.list_bar_x = (float)UI_CANVAS_W;
            s_anim.list_selector_y = (float)UI_LIST_HEADER_H;
            s_anim.list_selector_w = 18.0f;
        }
    }
    ui_px_clear();
    if (model->page == UI_PAGE_ROOT) {
        ui_px_apply_tile(model);
    } else {
        ui_px_apply_list(model);
    }
    lv_obj_invalidate(s_canvas);
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

    if (!s_ui_inited) {
        return;
    }

    ui_process_button_queue();
    ui_refresh_sources();

    if (!ui_take_model_lock()) {
        return;
    }
    snapshot = s_model;
    s_model.dirty = false;
    xSemaphoreGive(s_model_lock);

    ui_apply_snapshot(&snapshot);
}

void ui_post_button(ui_button_t button, bool pressed)
{
    ui_button_event_t event = {
        .button = button,
    };

    if (!pressed) {
        return;
    }
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
    s_model.wifi_state = state;
    s_model.dirty = true;
    xSemaphoreGive(s_model_lock);
}

void ui_set_ip_text(const char *ip)
{
    const char *safe_ip = (ip == NULL || ip[0] == '\0') ? "--" : ip;

    if (!ui_take_model_lock()) {
        return;
    }
    snprintf(s_model.ip_text, sizeof(s_model.ip_text), "%s", safe_ip);
    s_model.dirty = true;
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
    s_model.dirty = true;
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
    s_model.dirty = true;
    xSemaphoreGive(s_model_lock);
}

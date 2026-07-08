#include "lvgl_port.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_display.h"
#include "lvgl.h"
#include "ui.h"

#define LVGL_TAG "lvgl_port"

#define LVGL_TASK_STACK_SIZE 8192
#define LVGL_TASK_PRIORITY 4
#define LVGL_DRAW_BUF_LINES 20
#define LVGL_TASK_FRAME_MS 16
#define LVGL_IDLE_DIM_TIMEOUT_DEFAULT_MIN 1U
#define LVGL_TASK_CORE_ID 0
#define LVGL_PSRAM_POOL_COUNT 4
#define LVGL_PSRAM_POOL_CHUNK_BYTES (48U * 1024U)
#define LVGL_IDLE_BRIGHTNESS 0U

static lv_display_t *s_display = NULL;
static TaskHandle_t s_lvgl_task = NULL;
static void *s_draw_buf1 = NULL;
static void *s_draw_buf2 = NULL;
static void *s_lvgl_psram_pools[LVGL_PSRAM_POOL_COUNT] = {0};
static lv_mem_pool_t s_lvgl_psram_mem_pools[LVGL_PSRAM_POOL_COUNT] = {0};
static bool s_inited = false;
static int64_t s_last_activity_us = 0;
static bool s_idle_dimmed = false;
static uint8_t s_active_brightness = 0;
static volatile uint32_t s_idle_dim_suspend_count = 0;
static volatile uint16_t s_idle_dim_timeout_minutes = LVGL_IDLE_DIM_TIMEOUT_DEFAULT_MIN;

static void *lvgl_alloc_draw_buffer(size_t size, const char *name)
{
    /* Keep LVGL draw buffers in internal RAM; PSRAM is reserved for burner path. */
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (ptr != NULL) {
        ESP_LOGI(LVGL_TAG, "%s allocated in internal DMA (%u bytes)", name, (unsigned int)size);
        return ptr;
    }

    ESP_LOGE(LVGL_TAG, "%s internal DMA alloc failed (%u bytes)", name, (unsigned int)size);
    return NULL;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_err_t err;
    uint32_t px_cnt;

    if (area == NULL || px_map == NULL) {
        lv_display_flush_ready(disp);
        return;
    }

    /*
     * ST7789 over SPI expects RGB565 byte order opposite to LVGL's in-memory
     * layout on little-endian MCUs. Swap bytes before pushing to panel.
     */
    px_cnt = (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, px_cnt);

    err = lcd_display_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    if (err != ESP_OK) {
        ESP_LOGE(LVGL_TAG, "flush failed: %s", esp_err_to_name(err));
    }

    lv_display_flush_ready(disp);
}

static void lvgl_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();

    (void)arg;

    while (1) {
        (void)lvgl_port_is_idle_dimmed();
        lv_tick_inc(LVGL_TASK_FRAME_MS);
        ui_process();
        (void)lv_timer_handler();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LVGL_TASK_FRAME_MS));
    }
}

void lvgl_port_mark_activity(void)
{
    int64_t now_us = esp_timer_get_time();

    s_last_activity_us = now_us;
    if (s_idle_dimmed) {
        s_idle_dimmed = false;
        (void)lcd_display_set_brightness(s_active_brightness);
    }
}

void lvgl_port_set_idle_dim_suspended(bool suspended)
{
    if (suspended) {
        if (s_idle_dim_suspend_count < UINT32_MAX) {
            s_idle_dim_suspend_count++;
        }
        if (s_idle_dimmed) {
            s_idle_dimmed = false;
            (void)lcd_display_set_brightness(s_active_brightness);
        }
        return;
    }

    if (s_idle_dim_suspend_count > 0u) {
        s_idle_dim_suspend_count--;
    }
    if (s_idle_dim_suspend_count == 0u) {
        (void)lvgl_port_is_idle_dimmed();
    }
}

void lvgl_port_set_idle_dim_timeout_minutes(uint16_t minutes)
{
    s_idle_dim_timeout_minutes = minutes;
    if (minutes == 0U) {
        lvgl_port_mark_activity();
    }
}

bool lvgl_port_is_idle_dimmed(void)
{
    int64_t now_us;
    uint16_t timeout_minutes = s_idle_dim_timeout_minutes;

    if (!s_inited) {
        return false;
    }

    now_us = esp_timer_get_time();
    if (s_last_activity_us == 0) {
        s_last_activity_us = now_us;
    }
    if (s_idle_dim_suspend_count > 0u) {
        return false;
    }
    if (timeout_minutes == 0U) {
        if (s_idle_dimmed) {
            s_idle_dimmed = false;
            (void)lcd_display_set_brightness(s_active_brightness);
        }
        return false;
    }
    if (!s_idle_dimmed &&
        (uint64_t)(now_us - s_last_activity_us) >= ((uint64_t)timeout_minutes * 60ULL * 1000ULL * 1000ULL)) {
        s_active_brightness = lcd_display_get_brightness();
        s_idle_dimmed = true;
        (void)lcd_display_set_brightness(LVGL_IDLE_BRIGHTNESS);
    }

    return s_idle_dimmed;
}

esp_err_t lvgl_port_init(void)
{
    esp_err_t err;
    int width;
    int height;
    uint32_t draw_buf_size;

    if (s_inited) {
        return ESP_OK;
    }

    err = lcd_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(LVGL_TAG, "lcd init failed: %s", esp_err_to_name(err));
        return err;
    }

    width = lcd_display_width();
    height = lcd_display_height();
    draw_buf_size = (uint32_t)width * LVGL_DRAW_BUF_LINES * sizeof(uint16_t);

    s_draw_buf1 = lvgl_alloc_draw_buffer(draw_buf_size, "draw_buf1");
    if (s_draw_buf1 == NULL) {
        ESP_LOGE(LVGL_TAG, "alloc draw buf1 failed");
        return ESP_ERR_NO_MEM;
    }

    s_draw_buf2 = NULL;
    ESP_LOGI(LVGL_TAG, "using single LVGL draw buffer (%u lines)", (unsigned int)LVGL_DRAW_BUF_LINES);

    lv_init();

    for (uint32_t i = 0; i < LVGL_PSRAM_POOL_COUNT; ++i) {
        s_lvgl_psram_pools[i] = heap_caps_malloc(LVGL_PSRAM_POOL_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_lvgl_psram_pools[i] != NULL) {
            s_lvgl_psram_mem_pools[i] = lv_mem_add_pool(s_lvgl_psram_pools[i], LVGL_PSRAM_POOL_CHUNK_BYTES);
            if (s_lvgl_psram_mem_pools[i] != NULL) {
                ESP_LOGI(
                    LVGL_TAG,
                    "LVGL PSRAM heap pool added (%u/%u, %u bytes)",
                    (unsigned)(i + 1U),
                    (unsigned)LVGL_PSRAM_POOL_COUNT,
                    (unsigned)LVGL_PSRAM_POOL_CHUNK_BYTES);
            } else {
                ESP_LOGW(
                    LVGL_TAG,
                    "LVGL PSRAM heap pool add failed (%u/%u, %u bytes)",
                    (unsigned)(i + 1U),
                    (unsigned)LVGL_PSRAM_POOL_COUNT,
                    (unsigned)LVGL_PSRAM_POOL_CHUNK_BYTES);
                heap_caps_free(s_lvgl_psram_pools[i]);
                s_lvgl_psram_pools[i] = NULL;
            }
        } else {
            ESP_LOGW(
                LVGL_TAG,
                "LVGL PSRAM heap pool alloc failed (%u/%u, %u bytes)",
                (unsigned)(i + 1U),
                (unsigned)LVGL_PSRAM_POOL_COUNT,
                (unsigned)LVGL_PSRAM_POOL_CHUNK_BYTES);
        }
    }

    s_display = lv_display_create(width, height);
    if (s_display == NULL) {
        ESP_LOGE(LVGL_TAG, "lv_display_create failed");
        return ESP_FAIL;
    }

    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);
    lv_display_set_buffers(
        s_display,
        s_draw_buf1,
        s_draw_buf2,
        draw_buf_size,
        LV_DISPLAY_RENDER_MODE_PARTIAL);

    err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(LVGL_TAG, "ui init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_active_brightness = lcd_display_get_brightness();
    s_last_activity_us = esp_timer_get_time();
    s_idle_dimmed = false;

    if (xTaskCreatePinnedToCore(
            lvgl_task,
            "lvgl",
            LVGL_TASK_STACK_SIZE,
            NULL,
            LVGL_TASK_PRIORITY,
            &s_lvgl_task,
            LVGL_TASK_CORE_ID) != pdPASS) {
        ESP_LOGE(LVGL_TAG, "create lvgl task failed");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(LVGL_TAG, "LVGL started on %dx%d core=%d", width, height, LVGL_TASK_CORE_ID);
    return ESP_OK;
}


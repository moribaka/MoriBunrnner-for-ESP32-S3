#include "lvgl_port.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_display.h"
#include "lvgl.h"
#include "ui.h"

#define LVGL_TAG "lvgl_port"

#define LVGL_TASK_STACK_SIZE 8192
#define LVGL_TASK_PRIORITY 4
#define LVGL_DRAW_BUF_LINES 40
#define LVGL_TASK_MIN_DELAY_MS 5
#define LVGL_TASK_MAX_DELAY_MS 30
#define LVGL_TASK_TICK_MS 5

static lv_display_t *s_display = NULL;
static TaskHandle_t s_lvgl_task = NULL;
static void *s_draw_buf1 = NULL;
static void *s_draw_buf2 = NULL;
static bool s_inited = false;

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
    (void)arg;

    while (1) {
        lv_tick_inc(LVGL_TASK_TICK_MS);
        ui_process();
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
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

    s_draw_buf2 = lvgl_alloc_draw_buffer(draw_buf_size, "draw_buf2");
    if (s_draw_buf2 == NULL) {
        ESP_LOGW(LVGL_TAG, "alloc draw buf2 failed, fallback to single buffer");
    }

    lv_init();

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

    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, &s_lvgl_task) != pdPASS) {
        ESP_LOGE(LVGL_TAG, "create lvgl task failed");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(LVGL_TAG, "LVGL started on %dx%d", width, height);
    return ESP_OK;
}


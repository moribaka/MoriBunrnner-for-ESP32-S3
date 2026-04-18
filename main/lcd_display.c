#include "lcd_display.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pin_map.h"

#define LCD_TAG "lcd_display"

/*
 * Panel reference (from your docs): ST7789, native 240x320.
 * Current target orientation:
 * - FPC at left side
 * - Logical canvas 320x240
 * - Equivalent to rotate right 90 deg from portrait view
 */
#define MORI_LCD_SPI_HOST SPI3_HOST
#define MORI_LCD_ROTATION_DEG 270
#define MORI_LCD_H_RES 320
#define MORI_LCD_V_RES 240
#define MORI_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define MORI_LCD_DRAW_TIMEOUT_MS 200
#define MORI_LCD_SPI_MODE 3
#define MORI_LCD_BK_PWM_FREQ_HZ 20000
#define MORI_LCD_BK_PWM_MODE LEDC_LOW_SPEED_MODE
#define MORI_LCD_BK_PWM_TIMER LEDC_TIMER_0
#define MORI_LCD_BK_PWM_CHANNEL LEDC_CHANNEL_0
#define MORI_LCD_BK_PWM_RESOLUTION LEDC_TIMER_8_BIT
#define MORI_LCD_BK_BRIGHTNESS_DEFAULT 128U
#define MORI_LCD_BK_BRIGHTNESS_MAX ((1U << MORI_LCD_BK_PWM_RESOLUTION) - 1U)

/*
 * Backlight polarity:
 * 1 => high level turns backlight on
 * 0 => low level turns backlight on
 */
#define MORI_LCD_BK_ON_LEVEL 0

#if MORI_LCD_ROTATION_DEG == 0
#define MORI_LCD_SWAP_XY 0
#define MORI_LCD_MIRROR_X 0
#define MORI_LCD_MIRROR_Y 0
#elif MORI_LCD_ROTATION_DEG == 90
#define MORI_LCD_SWAP_XY 1
#define MORI_LCD_MIRROR_X 1
#define MORI_LCD_MIRROR_Y 0
#elif MORI_LCD_ROTATION_DEG == 180
#define MORI_LCD_SWAP_XY 0
#define MORI_LCD_MIRROR_X 1
#define MORI_LCD_MIRROR_Y 1
#elif MORI_LCD_ROTATION_DEG == 270
#define MORI_LCD_SWAP_XY 1
#define MORI_LCD_MIRROR_X 0
#define MORI_LCD_MIRROR_Y 1
#else
#error "Unsupported MORI_LCD_ROTATION_DEG"
#endif

static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_lcd_ready = false;
static SemaphoreHandle_t s_flush_done_sem = NULL;
static bool s_backlight_pwm_ready = false;
static uint8_t s_backlight_brightness = MORI_LCD_BK_BRIGHTNESS_DEFAULT;

static void *lcd_alloc_dma_buffer(size_t size, const char *tag)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (ptr != NULL) {
        ESP_LOGI(LCD_TAG, "%s: alloc internal DMA buffer ok (%u bytes)", tag, (unsigned int)size);
        return ptr;
    }
    ESP_LOGE(LCD_TAG, "%s: alloc internal DMA buffer failed (%u bytes)", tag, (unsigned int)size);
    return NULL;
}

static esp_err_t lcd_write_cmd(const uint8_t cmd, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_panel_io, cmd, data, len);
}

static esp_err_t lcd_apply_vendor_init(void)
{
    esp_err_t err;

    static const uint8_t b2[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t b7[] = {0x71};
    static const uint8_t bb[] = {0x3B};
    static const uint8_t c0[] = {0x2C};
    static const uint8_t c2[] = {0x01};
    static const uint8_t c3[] = {0x13};
    static const uint8_t c4[] = {0x20};
    static const uint8_t c6[] = {0x0F};
    static const uint8_t d0[] = {0xA4, 0xA1};
    static const uint8_t d6[] = {0xA1};
    static const uint8_t e0[] = {0xD0, 0x08, 0x0A, 0x0D, 0x0B, 0x07, 0x21, 0x33, 0x39, 0x39, 0x16, 0x16, 0x1F, 0x3C};
    static const uint8_t e1[] = {0xD0, 0x00, 0x03, 0x01, 0x00, 0x10, 0x21, 0x32, 0x38, 0x16, 0x14, 0x14, 0x20, 0x3D};
    static const struct {
        uint8_t cmd;
        const uint8_t *data;
        uint8_t len;
    } seq[] = {
        {0xB2, b2, sizeof(b2)},
        {0xB7, b7, sizeof(b7)},
        {0xBB, bb, sizeof(bb)},
        {0xC0, c0, sizeof(c0)},
        {0xC2, c2, sizeof(c2)},
        {0xC3, c3, sizeof(c3)},
        {0xC4, c4, sizeof(c4)},
        {0xC6, c6, sizeof(c6)},
        {0xD0, d0, sizeof(d0)},
        {0xD6, d6, sizeof(d6)},
        {0xE0, e0, sizeof(e0)},
        {0xE1, e1, sizeof(e1)},
    };

    err = lcd_write_cmd(0x11, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "vendor init cmd 0x11 failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        err = lcd_write_cmd(seq[i].cmd, seq[i].data, seq[i].len);
        if (err != ESP_OK) {
            ESP_LOGE(LCD_TAG, "vendor init cmd 0x%02X failed: %s", seq[i].cmd, esp_err_to_name(err));
            return err;
        }
    }

    err = lcd_write_cmd(0x21, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "vendor init cmd 0x21 failed: %s", esp_err_to_name(err));
        return err;
    }

    err = lcd_write_cmd(0x29, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "vendor init cmd 0x29 failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static bool lcd_panel_io_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;

    (void)panel_io;
    (void)edata;

    if (sem != NULL) {
        xSemaphoreGiveFromISR(sem, &need_yield);
    }
    return need_yield == pdTRUE;
}

static void lcd_backlight_set(bool on)
{
    uint8_t target = on ? s_backlight_brightness : 0U;
    uint32_t duty = 0;
    esp_err_t err;

    if (s_backlight_pwm_ready) {
        duty = (MORI_LCD_BK_ON_LEVEL != 0) ? target : (MORI_LCD_BK_BRIGHTNESS_MAX - target);
        err = ledc_set_duty(MORI_LCD_BK_PWM_MODE, MORI_LCD_BK_PWM_CHANNEL, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty(MORI_LCD_BK_PWM_MODE, MORI_LCD_BK_PWM_CHANNEL);
        }
        if (err != ESP_OK) {
            ESP_LOGW(LCD_TAG, "set backlight duty failed: %s", esp_err_to_name(err));
        }
        return;
    }

    {
        int level = on ? MORI_LCD_BK_ON_LEVEL : !MORI_LCD_BK_ON_LEVEL;
        gpio_config_t bk_cfg = {
            .pin_bit_mask = (1ULL << MORI_PIN_LCD_BK),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&bk_cfg);
        gpio_set_level(MORI_PIN_LCD_BK, level);
    }
}

static esp_err_t lcd_backlight_pwm_init(void)
{
    esp_err_t err;
    ledc_timer_config_t timer_cfg = {
        .speed_mode = MORI_LCD_BK_PWM_MODE,
        .duty_resolution = MORI_LCD_BK_PWM_RESOLUTION,
        .timer_num = MORI_LCD_BK_PWM_TIMER,
        .freq_hz = MORI_LCD_BK_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel_cfg = {
        .gpio_num = MORI_PIN_LCD_BK,
        .speed_mode = MORI_LCD_BK_PWM_MODE,
        .channel = MORI_LCD_BK_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = MORI_LCD_BK_PWM_TIMER,
        .duty = (MORI_LCD_BK_ON_LEVEL != 0) ? 0 : MORI_LCD_BK_BRIGHTNESS_MAX,
        .hpoint = 0,
    };

    if (s_backlight_pwm_ready) {
        return ESP_OK;
    }

    err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(LCD_TAG, "backlight timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ledc_channel_config(&channel_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(LCD_TAG, "backlight channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_backlight_pwm_ready = true;
    return ESP_OK;
}

esp_err_t lcd_display_init(void)
{
    esp_err_t err;
    spi_bus_config_t bus_config = {
        .sclk_io_num = MORI_PIN_LCD_CLK,
        .mosi_io_num = MORI_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MORI_LCD_H_RES * MORI_LCD_V_RES * sizeof(uint16_t),
    };
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = MORI_PIN_LCD_DC,
        .cs_gpio_num = MORI_PIN_LCD_CS,
        .pclk_hz = MORI_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = MORI_LCD_SPI_MODE,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1, /* LCD_RST# is not wired to ESP32 in current netlist */
        .bits_per_pixel = 16,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };

    if (s_lcd_ready) {
        return ESP_OK;
    }

    if (s_flush_done_sem == NULL) {
        s_flush_done_sem = xSemaphoreCreateBinary();
        if (s_flush_done_sem == NULL) {
            ESP_LOGE(LCD_TAG, "create LCD flush semaphore failed");
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(s_flush_done_sem, 0) == pdTRUE) {
    }

    (void)lcd_backlight_pwm_init();
    lcd_backlight_set(false);

    err = spi_bus_initialize(MORI_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(LCD_TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)MORI_LCD_SPI_HOST, &io_config, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "new panel io failed: %s", esp_err_to_name(err));
        return err;
    }

    {
        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = lcd_panel_io_color_trans_done,
        };
        err = esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, s_flush_done_sem);
        if (err != ESP_OK) {
            ESP_LOGE(LCD_TAG, "register panel io callbacks failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "new ST7789 panel failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "panel reset failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lcd_apply_vendor_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_invert_color(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "panel invert color failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_swap_xy(s_panel, MORI_LCD_SWAP_XY != 0);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "panel swap_xy failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_mirror(s_panel, MORI_LCD_MIRROR_X != 0, MORI_LCD_MIRROR_Y != 0);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "panel mirror failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_set_gap(s_panel, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "panel set gap failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(LCD_TAG, "panel display on failed: %s", esp_err_to_name(err));
        return err;
    }

    lcd_backlight_set(true);
    s_lcd_ready = true;

    ESP_LOGI(
        LCD_TAG,
        "LCD init done (ST7789, %dx%d, rot=%d), pins CLK=%d MOSI=%d CS=%d DC=%d BK=%d, bk_pwm=%d",
        MORI_LCD_H_RES,
        MORI_LCD_V_RES,
        MORI_LCD_ROTATION_DEG,
        MORI_PIN_LCD_CLK,
        MORI_PIN_LCD_MOSI,
        MORI_PIN_LCD_CS,
        MORI_PIN_LCD_DC,
        MORI_PIN_LCD_BK,
        s_backlight_pwm_ready ? 1 : 0);
    return ESP_OK;
}

esp_err_t lcd_display_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    esp_err_t err;

    if (!s_lcd_ready || s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (color_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_flush_done_sem != NULL) {
        while (xSemaphoreTake(s_flush_done_sem, 0) == pdTRUE) {
        }
    }

    err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data);
    if (err != ESP_OK) {
        return err;
    }

    if (s_flush_done_sem != NULL) {
        if (xSemaphoreTake(s_flush_done_sem, pdMS_TO_TICKS(MORI_LCD_DRAW_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(LCD_TAG, "wait LCD flush done timeout (%d ms)", MORI_LCD_DRAW_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}

esp_err_t lcd_display_set_brightness(uint8_t brightness)
{
    esp_err_t err;

    s_backlight_brightness = brightness;

    err = lcd_backlight_pwm_init();
    if (err != ESP_OK) {
        return err;
    }

    lcd_backlight_set(true);
    return ESP_OK;
}

uint8_t lcd_display_get_brightness(void)
{
    return s_backlight_brightness;
}

int lcd_display_width(void)
{
    return MORI_LCD_H_RES;
}

int lcd_display_height(void)
{
    return MORI_LCD_V_RES;
}


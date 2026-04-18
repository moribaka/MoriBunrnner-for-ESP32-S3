#include "ui.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#define UI_TAG "ui"
#define UI_STATUS_TEXT_MAX_LEN 96
#define UI_IP_TEXT_MAX_LEN 32

typedef struct {
    ui_wifi_state_t wifi_state;
    char ip_text[UI_IP_TEXT_MAX_LEN];
    int burn_progress;
    uint32_t burn_processed;
    uint32_t burn_total;
    char status_text[UI_STATUS_TEXT_MAX_LEN];
    bool dirty;
} ui_model_t;

static SemaphoreHandle_t s_model_lock = NULL;
static bool s_ui_inited = false;

static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_ip_label = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_progress_label = NULL;
static lv_obj_t *s_status_label = NULL;

static ui_model_t s_model = {
    .wifi_state = UI_WIFI_STATE_UNKNOWN,
    .ip_text = "--",
    .burn_progress = 0,
    .burn_processed = 0,
    .burn_total = 0,
    .status_text = "system booting",
    .dirty = true,
};

static const lv_font_t *ui_default_font(void)
{
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#else
    return &lv_font_montserrat_14;
#endif
}

static const char *ui_wifi_state_str(ui_wifi_state_t state)
{
    switch (state) {
        case UI_WIFI_STATE_CONNECTED:
            return "connected";
        case UI_WIFI_STATE_DISCONNECTED:
            return "disconnected";
        case UI_WIFI_STATE_PROVISIONING:
            return "provisioning";
        case UI_WIFI_STATE_UNKNOWN:
        default:
            return "unknown";
    }
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

    return true;
}

static void ui_apply_snapshot(const ui_model_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    if (s_wifi_label == NULL || s_ip_label == NULL || s_progress_bar == NULL || s_progress_label == NULL ||
        s_status_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(s_wifi_label, "Wi-Fi: %s", ui_wifi_state_str(snapshot->wifi_state));
    lv_label_set_text_fmt(s_ip_label, "IP: %s", snapshot->ip_text);
    lv_bar_set_value(s_progress_bar, snapshot->burn_progress, LV_ANIM_OFF);
    lv_label_set_text_fmt(
        s_progress_label,
        "Burn: %d%% (%" PRIu32 "/%" PRIu32 " bytes)",
        snapshot->burn_progress,
        snapshot->burn_processed,
        snapshot->burn_total);
    lv_label_set_text_fmt(s_status_label, "Status: %s", snapshot->status_text);
}

esp_err_t ui_init(void)
{
    lv_obj_t *scr;
    lv_obj_t *title;
    lv_obj_t *progress_title;
    lv_obj_t *status_title;

    if (s_ui_inited) {
        return ESP_OK;
    }

    if (!ui_take_model_lock()) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_model_lock);

    scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0C1220), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ui_default_font(), 0);

    title = lv_label_create(scr);
    lv_label_set_text(title, "MORI BURNER");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF3F6FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_wifi_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x8CC8FF), 0);
    lv_obj_align(s_wifi_label, LV_ALIGN_TOP_LEFT, 12, 38);

    s_ip_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_ip_label, lv_color_hex(0x8CC8FF), 0);
    lv_obj_align(s_ip_label, LV_ALIGN_TOP_RIGHT, -12, 38);

    progress_title = lv_label_create(scr);
    lv_label_set_text(progress_title, "Burn Progress");
    lv_obj_set_style_text_color(progress_title, lv_color_hex(0xC9D3EE), 0);
    lv_obj_align(progress_title, LV_ALIGN_TOP_LEFT, 12, 62);

    s_progress_bar = lv_bar_create(scr);
    lv_obj_set_size(s_progress_bar, 296, 16);
    lv_obj_align(s_progress_bar, LV_ALIGN_TOP_LEFT, 12, 82);
    lv_bar_set_range(s_progress_bar, 0, 100);

    s_progress_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_progress_label, lv_color_hex(0xE3E9FF), 0);
    lv_obj_align(s_progress_label, LV_ALIGN_TOP_LEFT, 12, 104);

    status_title = lv_label_create(scr);
    lv_label_set_text(status_title, "Runtime Status");
    lv_obj_set_style_text_color(status_title, lv_color_hex(0xC9D3EE), 0);
    lv_obj_align(status_title, LV_ALIGN_TOP_LEFT, 12, 132);

    s_status_label = lv_label_create(scr);
    lv_obj_set_width(s_status_label, 296);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xF3F6FF), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 12, 152);

    s_ui_inited = true;
    ui_process();
    ESP_LOGI(UI_TAG, "UI initialized");
    return ESP_OK;
}

void ui_process(void)
{
    ui_model_t snapshot;

    if (!s_ui_inited) {
        return;
    }
    if (!ui_take_model_lock()) {
        return;
    }

    if (!s_model.dirty) {
        xSemaphoreGive(s_model_lock);
        return;
    }

    snapshot = s_model;
    s_model.dirty = false;
    xSemaphoreGive(s_model_lock);

    ui_apply_snapshot(&snapshot);
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

void ui_set_burn_progress(int progress, uint32_t processed, uint32_t total)
{
    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }
    if (total > 0 && processed > total) {
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

void ui_set_status_text(const char *text)
{
    const char *safe_text = (text == NULL) ? "" : text;

    if (!ui_take_model_lock()) {
        return;
    }

    snprintf(s_model.status_text, sizeof(s_model.status_text), "%s", safe_text);
    s_model.dirty = true;

    xSemaphoreGive(s_model_lock);
}

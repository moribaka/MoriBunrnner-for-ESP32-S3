#include "ui_character_pixels.inc"

#define UI_COLOR_HEADER UI_RGB565(50, 37, 33)
#define UI_COLOR_HEADER_TEXT UI_RGB565(255, 246, 230)
#define UI_COLOR_HEADER_STATUS UI_RGB565(255, 219, 156)

static void ui_px_character(const ui_model_t *model, int32_t center_x, int32_t center_y, bool compact, bool cheerful)
{
    const bool baicheng = model->theme_baicheng;
    const uint16_t *pixels = baicheng
        ? (model->page == UI_PAGE_ROOT ? ui_sprite_baicheng_idle : ui_sprite_baicheng_awake)
        : (cheerful ? ui_sprite_chis_smile : ui_sprite_chis_calm);
    const int32_t width = baicheng ? 64 : 48;
    const int32_t height = baicheng ? 40 : 48;
    const int32_t step = compact ? 2 : 1;
    const int32_t x = center_x - width / step / 2;
    const int32_t y = center_y - height / step / 2;

    for (int32_t row = 0; row < height; row += step) {
        for (int32_t col = 0; col < width; col += step) {
            uint16_t color = pixels[row * width + col];
            if (color != 0x0020U) {
                s_canvas_buf[(y + row / step) * UI_CANVAS_W + x + col / step] = color;
            }
        }
    }
}

static void ui_px_theme_header(const ui_model_t *model)
{
    char title[UI_ROW_TEXT_MAX_LEN] = {0};
    const bool portrait = model->page == UI_PAGE_FILES || model->page == UI_PAGE_TF ||
        model->page == UI_PAGE_TASK_STATUS || model->page == UI_PAGE_TASK_RESULT ||
        model->page == UI_PAGE_SYSTEM || model->page == UI_PAGE_APPS || model->page == UI_PAGE_BURNER;
    const bool result = model->page == UI_PAGE_TASK_RESULT && model->task_result_status_valid;
    const burner_status_t *status = ui_task_result_status_buffer();
    const bool success = result && status->state == BURNER_STATE_DONE;

    ui_format_page_header(model, title, sizeof(title));
    if (model->page == UI_PAGE_ROOT) {
        snprintf(title, sizeof(title), "MoriBurner");
    } else if (result) {
        snprintf(title, sizeof(title), "%s", ui_burn_state_text(status->state));
    }
    ui_px_color_box(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H, UI_COLOR_HEADER);
    s_px_foreground = UI_COLOR_HEADER_TEXT;
    if (success) {
        s_px_foreground = UI_RGB565(168, 217, 160);
    } else if (result && status->state == BURNER_STATE_ERROR) {
        s_px_foreground = UI_RGB565(255, 169, 150);
    }
    ui_px_text_clipped(10, UI_LIST_HEADER_TEXT_Y, 185, title, true);
    s_px_foreground = UI_COLOR_HEADER_STATUS;
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    if (portrait) {
        ui_px_character(model, 227, UI_LIST_HEADER_H / 2, true, success);
    } else if (model->page == UI_PAGE_ROOT) {
        ui_px_text(226, UI_LIST_HEADER_TEXT_Y, "TF", true);
        ui_px_color_box(248, 10, 4, 4, usb_msc_tf_ready() ? UI_RGB565(168, 217, 160) : UI_COLOR_SAND);
    } else {
        ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    }
    s_px_foreground = UI_COLOR_INK;
    ui_px_invalidate_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
}

static void ui_px_theme_footer(void)
{
    const int32_t y = UI_CANVAS_H - UI_HINT_H;
    uint16_t *pixel = s_canvas_buf + y * UI_CANVAS_W;
    const uint16_t *end = s_canvas_buf + UI_CANVAS_PIXELS;

    /* 保留各页面已绘制的状态和按键提示。 */
    for (; pixel < end; ++pixel) {
        if (*pixel == UI_COLOR_PAPER) {
            *pixel = UI_COLOR_SAND;
        }
    }
    ui_px_invalidate_rect(0, y, UI_CANVAS_W, UI_HINT_H);
}

static void ui_px_home_border(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color, int32_t thickness)
{
    ui_px_color_box(x, y + 3, thickness, h - 6, color);
    ui_px_color_box(x + 3, y, w - 6, thickness, color);
    ui_px_color_box(x + 3, y + h - thickness, w - 6, thickness, color);
    ui_px_color_box(x + w - thickness, y + 3, thickness, h - 6, color);
}

static void ui_px_home_gradient(int32_t x, int32_t y, int32_t w, int32_t h)
{
    // 预览的 110° 渐变，左右比重 100:36，色值从 #FFE091 到 #FFD17C。
    const int32_t extent = (w - 1) * 100 + (h - 1) * 36;
    for (int32_t row = 0; row < h; ++row) {
        const int32_t inset = (row < 3 || row >= h - 3) ? 3 : 0;
        uint16_t *pixels = &s_canvas_buf[(y + row) * UI_CANVAS_W + x];
        for (int32_t col = inset; col < w - inset; ++col) {
            const int32_t position = col * 100 + row * 36;
            const int32_t green = 224 - (15 * position + extent / 2) / extent;
            const int32_t blue = 145 - (21 * position + extent / 2) / extent;
            pixels[col] = UI_RGB565(255, green, blue);
        }
    }
}

static void ui_px_home_workbench(const ui_model_t *model)
{
    const char *name = ui_tr(model->theme_baicheng ? "Baicheng" : "Chis");
    uint16_t selected = model->selected;

    ui_px_home_gradient(12, 39, 296, 112);
    if (selected == 0U) {
        ui_px_home_border(12, 39, 296, 112, UI_COLOR_FOCUS_BORDER, 2);
    }
    s_px_foreground = UI_RGB565(113, 59, 38);
    ui_px_text(27, 56, ui_tr("Offline burner"), true);
    s_px_foreground = UI_COLOR_INK;
    ui_px_text(27, 80, ui_tr("Cart workbench"), true);
    ui_px_text(27, 101, "GBA / GB-GBC", true);
    s_px_foreground = UI_RGB565(114, 59, 32);
    ui_px_text(27, 128, ui_tr("A: enter"), true);
    ui_px_color_box(204, 53, 82, 68, UI_RGB565(255, 241, 191));
    ui_px_character(model, 245, 87, false, true);
    s_px_foreground = UI_RGB565(98, 59, 38);
    ui_px_text(245 - ui_px_text_width(name) / 2, 135, name, true);
    s_px_foreground = UI_COLOR_INK;
}

static void ui_px_home_dock_item(uint16_t entry, bool focused)
{
    const char *label = ui_root_item_title(&s_home_items[entry]);
    const int32_t x = 12 + (entry - 1) * 101;
    const uint16_t fill = focused ? UI_COLOR_AMBER : UI_COLOR_HOME_TILE;
    ui_px_color_box(x + 3, 163, 88, 47, fill);
    ui_px_color_box(x, 166, 94, 41, fill);
    ui_px_home_border(x, 163, 94, 47, focused ? UI_COLOR_FOCUS_BORDER : UI_COLOR_TILE_BORDER,
                      focused ? 2 : 1);
    int32_t label_w = ui_px_text_width(label);
    ui_px_text_clipped(x + (94 - label_w) / 2, 185, 88, label, true);
}

static void ui_px_apply_home(const ui_model_t *model, bool full)
{
    static uint16_t prev;
    uint16_t selected = model->selected;
    if (full) ui_px_color_box(0, 0, UI_CANVAS_W, UI_CANVAS_H, UI_COLOR_HOME_PAPER);
    if (full || model->chrome_dirty) ui_px_theme_header(model);
    if (full || (selected != prev && (selected == 0 || prev == 0))) {
        ui_px_home_workbench(model);
        if (!full) ui_px_invalidate_rect(12, 39, 296, 112);
    }
    for (uint16_t i = 1; i < ui_home_item_count(); ++i) {
        if (full || (selected != prev && (i == prev || i == selected))) {
            ui_px_home_dock_item(i, i == selected);
            if (!full) ui_px_invalidate_rect(12 + (i - 1) * 101, 163, 94, 47);
        }
    }
    prev = selected;
    if (full || model->chrome_dirty) {
        ui_px_color_box(0, 220, UI_CANVAS_W, 20, UI_COLOR_SAND);
        ui_px_text_clipped(10, 230, UI_CANVAS_W - 20,
            model->status_text[0] != '\0' ? ui_status_text_to_display(model->status_text)
                                         : ui_tr("D-pad move  A enter"), true);
        if (!full) ui_px_invalidate_rect(0, 220, UI_CANVAS_W, 20);
    }
    if (full) ui_px_invalidate_full();
}

static const uint16_t s_sleeve_gold = UI_RGB565(255, 218, 146);
static const uint16_t s_sleeve_label = UI_RGB565(255, 197, 109);

static void ui_px_studio_number(int32_t x, int32_t y, const char *value)
{
    const int32_t scale = 4;
    for (; *value != '\0'; ++value, x += 6 * scale) {
        const uint8_t *glyph = ui_font5x7(*value);
        for (int32_t col = 0; col < 5; ++col) {
            for (int32_t row = 0; row < 7; ++row) {
                if (glyph[col] & (1u << row)) {
                    ui_px_box(x + col * scale, y + row * scale, scale, scale, true);
                }
            }
        }
    }
}

static void ui_px_sleeve_art(const ui_model_t *model)
{
    const uint16_t shell = UI_RGB565(206, 129, 61);
    const uint16_t edge = UI_RGB565(92, 60, 40);
    ui_px_color_box(14, 19, 126, 65, edge);
    ui_px_color_box(24, 13, 104, 7, edge);
    ui_px_color_box(20, 84, 114, 7, edge);
    ui_px_color_box(17, 22, 120, 59, shell);
    ui_px_color_box(27, 16, 98, 7, shell);
    ui_px_color_box(23, 81, 108, 7, shell);
    ui_px_color_box(23, 27, 108, 50, edge);
    ui_px_color_box(25, 29, 104, 46, s_sleeve_label);
    for (int32_t row = 0; row < 46; ++row) {
        for (int32_t col = -(row % 14); col < 104; col += 14) {
            const int32_t start = MAX(col, 0);
            const int32_t end = MIN(col + 6, 104);
            if (end > start) {
                ui_px_color_box(25 + start, 29 + row, end - start, 1, UI_RGB565(246, 175, 90));
            }
        }
    }
    ui_px_character(model, 99, 52, false, true);
    ui_px_color_box(29, 34, 34, 15, UI_COLOR_HEADER);
    s_px_foreground = UI_COLOR_HEADER_TEXT;
    ui_px_text_clipped(33, 36, 27, ui_cart_mode_label(s_cart_mode), true);
    s_px_foreground = UI_COLOR_INK;
    for (int32_t y = 80; y <= 86; y += 3) {
        ui_px_color_box(58, y, 38, 1, edge);
    }
}

enum {
    UI_SLEEVE_ACTION_TOP = 128,
    UI_SLEEVE_ACTION_ROW_H = 22,
    UI_SLEEVE_ACTION_ROWS = 5,
    UI_SLEEVE_ACTION_H = UI_CANVAS_H - UI_SLEEVE_ACTION_TOP,
};

static struct {
    uint16_t selected;
    uint16_t count;
    int offset;
} s_sleeve_scroll;

static bool ui_sleeve_scroll_active(void)
{
    return s_sleeve_scroll.offset != 0;
}

static void ui_px_sleeve_action_row(const ui_model_t *model, uint16_t index, int y)
{
    char title[UI_ROW_TEXT_MAX_LEN] = {0};
    char hint[UI_ROW_TEXT_MAX_LEN] = {0};
    const bool selected = index == model->selected;
    ui_fill_burn_rom_row(model, index, title, sizeof(title), hint, sizeof(hint));
    ui_px_color_box(12, y, 296, UI_SLEEVE_ACTION_ROW_H - 2,
                    selected ? UI_COLOR_HEADER : UI_COLOR_TILE);
    if (selected) {
        ui_px_color_box(12, y, 5, UI_SLEEVE_ACTION_ROW_H - 2, UI_COLOR_ORANGE);
    }

    const int hint_width = MIN(ui_px_text_width(hint), 99);
    s_px_foreground = selected ? UI_COLOR_HEADER_TEXT : UI_COLOR_INK;
    ui_px_text_clipped(25, y + 5, hint_width > 0 ? 266 - hint_width : 274, title, true);
    s_px_foreground = selected ? UI_COLOR_HEADER_STATUS : UI_RGB565(113, 59, 38);
    ui_px_text_clipped(299 - hint_width, y + 5, hint_width, hint, true);
    s_px_foreground = UI_COLOR_INK;
}

static void ui_px_sleeve_actions(const ui_model_t *model, bool full)
{
    const uint16_t count = ui_burn_rom_item_count();
    const uint16_t selected = model->selected;
    if (full || count != s_sleeve_scroll.count) {
        s_sleeve_scroll.offset = 0;
    } else if (selected != s_sleeve_scroll.selected) {
        const int delta = ((int)selected - s_sleeve_scroll.selected + count) % count;
        if (delta == 1) {
            s_sleeve_scroll.offset += UI_SLEEVE_ACTION_ROW_H;
        } else if (delta == count - 1) {
            s_sleeve_scroll.offset -= UI_SLEEVE_ACTION_ROW_H;
        } else {
            s_sleeve_scroll.offset = 0;
        }
        s_sleeve_scroll.offset = MAX(-UI_SLEEVE_ACTION_ROW_H,
                                    MIN(UI_SLEEVE_ACTION_ROW_H, s_sleeve_scroll.offset));
    }
    s_sleeve_scroll.selected = selected;
    s_sleeve_scroll.count = count;
    s_sleeve_scroll.offset /= 2;

    ui_px_color_box(0, UI_SLEEVE_ACTION_TOP, UI_CANVAS_W, UI_SLEEVE_ACTION_H, UI_COLOR_PAPER);
    const int rows = MIN(count, UI_SLEEVE_ACTION_ROWS);
    const int height = rows * UI_SLEEVE_ACTION_ROW_H;
    const int top = UI_SLEEVE_ACTION_TOP + (UI_SLEEVE_ACTION_H - height) / 2;
    ui_px_clip_set(0, top, UI_CANVAS_W, height);
    for (int slot = -1; count > 0 && slot <= rows; ++slot) {
        const int y = top + slot * UI_SLEEVE_ACTION_ROW_H + s_sleeve_scroll.offset;
        if (y + UI_SLEEVE_ACTION_ROW_H <= top || y >= top + height) {
            continue;
        }
        const uint16_t index = (uint16_t)((selected + slot - rows / 2 + 2 * count) % count);
        ui_px_sleeve_action_row(model, index, y);
    }
    ui_px_clip_clear();
    if (count > 1) {
        const int bar_x = UI_CANVAS_W - 8;
        const int thumb_h = MAX(8, height / count);
        const int thumb_y = top + (height - thumb_h) * selected / (count - 1);
        ui_px_color_box(bar_x, top, 4, height, UI_COLOR_TILE_BORDER);
        ui_px_color_box(bar_x, thumb_y, 4, thumb_h, UI_COLOR_ORANGE);
    }
    ui_px_invalidate_rect(0, UI_SLEEVE_ACTION_TOP, UI_CANVAS_W, UI_SLEEVE_ACTION_H);
}

static void ui_px_studio_footer(const char *keys, const char *status)
{
    ui_px_color_box(0, 210, UI_CANVAS_W, 30, UI_COLOR_SAND);
    ui_px_color_box(0, 210, UI_CANVAS_W, 1, UI_COLOR_TILE_BORDER);
    ui_px_text_clipped(12, 219, 108, keys, true);
    ui_px_text_clipped(123, 219, 185, status, true);
    ui_px_invalidate_rect(0, 210, UI_CANVAS_W, 30);
}

static void ui_px_apply_sleeve(const ui_model_t *model, bool full)
{
    if (full || model->content_dirty) {
        burner_status_t status = {0};
        burner_status_snapshot(&status);
        const bool analyzed = ui_cart_is_unlocked() && status.probe_valid &&
                              status.probe_cart_mode == s_cart_mode;
        ui_px_color_box(0, 0, UI_CANVAS_W, UI_SLEEVE_ACTION_TOP, s_sleeve_gold);
        ui_px_sleeve_art(model);
        ui_px_text(157, 13, analyzed ? (ui_lang_is_zh() ? "卡带已识别" : "Cart identified")
                                     : ui_tr("Analyze first"), true);
        if (analyzed && status.probe_device_size > 0) {
            const uint32_t mib = 1024u * 1024u;
            const unsigned capacity = (unsigned)(status.probe_device_size / mib);
            if (status.probe_device_size % mib == 0 && capacity < 10000u) {
                char number[5];
                snprintf(number, sizeof(number), "%u", capacity);
                ui_px_studio_number(157, 32, number);
                ui_px_text(157 + (int32_t)strlen(number) * 24 + 3, 47, "MiB", true);
            } else {
                char size[24];
                ui_format_bytes_text(status.probe_device_size, size, sizeof(size));
                ui_px_text_clipped(157, 48, 151, size, true);
            }
            const char *chip = ui_probe_chip_name(&status);
            const char *model_name = strstr(chip, " with ");
            if (model_name != NULL) {
                char board[24];
                const int len = MIN((int)(model_name - chip), (int)sizeof(board) - 1);
                snprintf(board, sizeof(board), "%.*s", len, chip);
                ui_px_text_clipped(157, 64, 155, board, true);
                chip = model_name + 6;
            }
            ui_px_text_clipped(157, 81, 155, chip, true);
            if (status.probe_sector_size > 0) {
                char sector[24], sector_label[36];
                ui_format_bytes_text(status.probe_sector_size, sector, sizeof(sector));
                snprintf(sector_label, sizeof(sector_label), ui_lang_is_zh() ? "扇区 %s" : "Sector %s", sector);
                ui_px_text_clipped(157, 97, 155, sector_label, true);
            }
        } else {
            ui_px_text(157, 48, ui_cart_mode_label(s_cart_mode), true);
            if (analyzed) ui_px_text_clipped(157, 69, 155, ui_probe_chip_name(&status), true);
        }
        if (analyzed && status.probe_gba_d0d1_known && status.probe_gba_d0d1_swapped) {
            ui_px_text_clipped(14, 98, 130, ui_lang_is_zh() ? "D0/D1 对调" : "D0/D1 swapped", true);
        }
        ui_px_invalidate_rect(0, 0, UI_CANVAS_W, UI_SLEEVE_ACTION_TOP);
    }
    if (full || model->motion_dirty || model->content_dirty || ui_sleeve_scroll_active()) {
        ui_px_sleeve_actions(model, full);
    }
    if (full || model->content_dirty || model->chrome_dirty) {
        ui_px_color_box(0, 109, UI_CANVAS_W, UI_SLEEVE_ACTION_TOP - 109, s_sleeve_gold);
        const char *status = ui_status_text_to_display(model->status_text);
        if (!status[0] || strcmp(status, ui_tr("analyze cart first")) == 0) {
            status = ui_lang_is_zh() ? "上下切换操作  左右查看资料" : "Up/Down: actions  Left/Right: info";
        }
        ui_px_text_clipped(14, 114, 292, status, true);
        ui_px_invalidate_rect(0, 109, UI_CANVAS_W, UI_SLEEVE_ACTION_TOP - 109);
    }
}

static const uint8_t s_studio_meter_ticks[][4] = {
    {44, 161, 32, 161},
    {40, 155, 32, 154},
    {41, 148, 33, 147},
    {43, 142, 35, 140},
    {49, 138, 38, 133},
    {48, 130, 41, 127},
    {51, 125, 44, 120},
    {55, 120, 49, 115},
    {62, 118, 53, 109},
    {64, 111, 59, 105},
    {69, 107, 64, 100},
    {74, 104, 71, 97},
    {82, 105, 77, 94},
    {86, 99, 84, 91},
    {92, 97, 91, 89},
    {99, 96, 98, 88},
    {105, 100, 105, 88},
    {111, 96, 112, 88},
    {118, 97, 119, 89},
    {124, 99, 126, 91},
    {128, 105, 133, 94},
    {136, 104, 139, 97},
    {141, 107, 146, 100},
    {146, 111, 151, 105},
    {148, 118, 157, 109},
    {155, 120, 161, 115},
    {159, 125, 166, 120},
    {162, 130, 169, 127},
    {161, 138, 172, 133},
    {167, 142, 175, 140},
    {169, 148, 177, 147},
    {170, 155, 178, 154},
    {166, 161, 178, 161},
};

static void ui_px_apply_studio_meter(const ui_model_t *model, const burner_status_t *status, bool full)
{
    static int previous_percent;
    static uint32_t previous_bytes;
    static uint32_t previous_total;
    const int percent = MAX(0, MIN(model->burn_progress, 100));
    const int tick_count = sizeof(s_studio_meter_ticks) / sizeof(s_studio_meter_ticks[0]);
    const int active = percent * tick_count / 100;
    const int previous_active = previous_percent * tick_count / 100;
    if (full) {
        ui_px_color_box(0, UI_LIST_HEADER_H, UI_CANVAS_W, 210 - UI_LIST_HEADER_H, UI_COLOR_PAPER);
        ui_px_text(12, 41, ui_lang_is_zh() ? "ROM 写入" : "Writing ROM", true);
        ui_px_text_clipped(12, 63, 226, status->rom_name, true);
        ui_px_character(model, 279, 56, true, false);
        ui_px_panel(200, 91, 106, 104, false);
        ui_px_color_box(200, 91, 106, 3, UI_COLOR_ORANGE);
        ui_px_text(210, 105, ui_lang_is_zh() ? "文件大小" : "File size", true);
        ui_px_color_box(210, 149, 86, 1, UI_COLOR_TILE_BORDER);
        ui_px_text_clipped(210, 168, 86, ui_cart_mode_label(status->probe_valid ? status->probe_cart_mode : s_cart_mode), true);
        ui_px_text(35, 188, ui_lang_is_zh() ? "已写入" : "Written", true);
        ui_px_color_box(35, 174, 142, 1, UI_COLOR_FOCUS_BORDER);
    }
    if (full || active != previous_active) {
        const int first = full ? 0 : MIN(active, previous_active);
        const int end = full ? tick_count : MAX(active, previous_active);
        for (int i = first; i < end; ++i) {
            const uint8_t *tick = s_studio_meter_ticks[i];
            s_px_foreground = i < active ? UI_COLOR_ORANGE : UI_COLOR_TILE_BORDER;
            ui_px_line(tick[0], tick[1], tick[2], tick[3], true);
            ui_px_line(tick[0] + 1, tick[1], tick[2] + 1, tick[3], true);
            s_px_foreground = UI_COLOR_INK;
            if (!full) {
                const int x = MIN(tick[0], tick[2]);
                const int y = MIN(tick[1], tick[3]);
                ui_px_invalidate_rect(x, y, MAX(tick[0], tick[2]) - x + 2, MAX(tick[1], tick[3]) - y + 1);
            }
        }
    }
    if (full || percent != previous_percent) {
        char text[6];
        snprintf(text, sizeof(text), "%d%%", percent);
        ui_px_color_box(55, 128, 100, 36, UI_COLOR_PAPER);
        ui_px_studio_number(105 - (int32_t)strlen(text) * 12, 130, text);
        if (!full) ui_px_invalidate_rect(55, 128, 100, 36);
    }
    if (full || previous_total != model->burn_total) {
        char text[24];
        ui_format_bytes_text(model->burn_total, text, sizeof(text));
        ui_px_color_box(207, 123, 92, 20, UI_COLOR_TILE);
        ui_px_text_clipped(210, 125, 86, text, true);
        if (!full) ui_px_invalidate_rect(207, 123, 92, 20);
    }
    if (full || previous_bytes != model->burn_processed) {
        char text[24];
        ui_format_bytes_text(model->burn_processed, text, sizeof(text));
        ui_px_color_box(93, 184, 102, 20, UI_COLOR_PAPER);
        ui_px_text_clipped(95, 188, 99, text, true);
        if (!full) ui_px_invalidate_rect(93, 184, 102, 20);
    }
    if (full || model->chrome_dirty) {
        ui_px_theme_header(model);
        ui_px_studio_footer(ui_lang_is_zh() ? "B 任务选项" : "B Task options",
                            ui_lang_is_zh() ? "下键查看详情" : "Down: details");
    }
    if (full) ui_px_invalidate_rect(0, UI_LIST_HEADER_H, UI_CANVAS_W, 210 - UI_LIST_HEADER_H);
    previous_percent = percent;
    previous_bytes = model->burn_processed;
    previous_total = model->burn_total;
}

enum { UI_BURN_OP_TOP = 24, UI_BURN_OP_LINE = 21 };
static uint16_t s_burn_drawn_selection;

static uint16_t ui_burn_rom_visible_rows(void)
{
    int32_t rows = (UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H - UI_BURN_OP_TOP) / UI_BURN_OP_LINE;

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

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
} ui_burn_info_draw_ctx_t;

static void ui_burn_info_draw_emit(
    void *ctx_ptr,
    uint16_t index,
    const char *label,
    const char *value,
    ui_burn_info_kind_t kind)
{
    ui_burn_info_draw_ctx_t *ctx = (ui_burn_info_draw_ctx_t *)ctx_ptr;
    int32_t row;
    int32_t row_y;
    int32_t text_x;
    int32_t text_w;

    if (ctx == NULL || index < s_burner_info_scroll) {
        return;
    }
    row = (int32_t)(index - s_burner_info_scroll);
    if (row >= (int32_t)ui_burn_info_visible_rows()) {
        return;
    }
    row_y = ctx->y + UI_BURN_INFO_ROW_Y0 + row * UI_LIST_LINE_H;
    text_x = ctx->x + 5;
    text_w = ctx->w - 10;

    if (kind == UI_BURN_INFO_ROW_GAP) {
        return;
    }
    if (kind == UI_BURN_INFO_ROW_HEADER) {
        /* Section divider above a non-selectable header row. */
        ui_px_hline(ctx->x + 4, row_y - 3, ctx->w - 8, true);
    }
    if (label != NULL && label[0] != '\0' && value != NULL && value[0] != '\0') {
        ui_px_info_line(text_x, row_y, text_w, label, value);
    } else if (label != NULL && label[0] != '\0') {
        ui_px_text_clipped(text_x, row_y, text_w, label, true);
    } else if (value != NULL) {
        ui_px_text_clipped(text_x, row_y, text_w, value, true);
    }
    if (ui_burn_info_kind_selectable(kind) && s_burner_focus_info && index == s_burner_info_selected) {
        ui_px_highlight_rect(ctx->x + 1, row_y - 2, ctx->w - 2, UI_LIST_LINE_H - 1);
    }
}

static void ui_px_draw_burner_cart_info_panel(const ui_model_t *model, int32_t x, int32_t y, int32_t w, int32_t h)
{
    ui_burn_info_draw_ctx_t ctx = {.x = x, .y = y, .w = w};
    int32_t text_x = x + 5;
    int32_t text_w = w - 10;

    (void)model;
    ui_px_panel(x, y, w, h, false);
    ui_px_color_box(x + 1, y + 1, w - 2, 21, s_burner_focus_info ? UI_COLOR_AMBER : UI_COLOR_SAND);
    const char *heading = s_burner_info_nor_detail ? "NOR Flash" :
        (ui_lang_is_zh() ? "卡带资料" : "Cartridge info");
    ui_px_text_clipped(text_x, y + 7, text_w, heading, true);

    ui_burn_info_ensure_visible();
    (void)ui_burn_info_enumerate(ui_burn_info_draw_emit, &ctx);
}

static const char *ui_probe_chip_name(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid) {
        return "--";
    }
    if (status->probe_cart_mode == BURNER_CART_MODE_GBA &&
        s_cart_ctx.gba_likely_read_only &&
        !status->probe_cfi_ok) {
        return ui_tr("Read-only retail ROM");
    }
    if (status->probe_chip_name[0] != '\0') {
        return status->probe_chip_name;
    }
    return "unknown";
}

static void ui_px_burn_action_row(const ui_model_t *model, uint16_t row, uint16_t count,
                                  int32_t x, int32_t y, int32_t w)
{
    const int32_t text_x = x + 6;
    const int32_t text_w = w - 12;
    uint16_t index = model->scroll + row;
    int32_t row_y = y + UI_BURN_OP_TOP + (int32_t)row * UI_BURN_OP_LINE;
    char title[UI_ROW_TEXT_MAX_LEN] = {0};
    char hint[UI_ROW_TEXT_MAX_LEN] = {0};

    if (index >= count) {
        return;
    }
    bool focus = !s_burner_focus_info && index == model->selected;
    ui_px_color_box(x + 1, row_y, w - 2, UI_BURN_OP_LINE - 1, focus ? UI_COLOR_AMBER : UI_COLOR_TILE);
    if (focus) ui_px_color_box(x + 1, row_y, 3, UI_BURN_OP_LINE - 1, UI_COLOR_ORANGE);
    ui_fill_burn_rom_row(model, index, title, sizeof(title), hint, sizeof(hint));
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE &&
        ((index == 1U && !ui_gba_sram_patch_selectable()) ||
         (index == 3U && s_gba_save_patch_choice != 1U))) {
        ui_px_text_clipped_disabled(text_x, row_y + 5, text_w, title);
    } else {
        ui_px_text_clipped(text_x, row_y + 5, text_w, title, true);
    }
    if (hint[0] != '\0') {
        int32_t hint_w = ui_px_text_width(hint);
        int32_t hint_x;

        if (hint_w > text_w / 2) {
            hint_w = text_w / 2;
        }
        hint_x = text_x + text_w - hint_w;
        if (hint_x > text_x + ui_px_text_width(title) + 6) {
            if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE &&
                ((index == 1U && !ui_gba_sram_patch_selectable()) ||
                 (index == 3U && s_gba_save_patch_choice != 1U))) {
                ui_px_text_clipped_disabled(hint_x, row_y + 5, hint_w, hint);
            } else {
                ui_px_text_clipped(hint_x, row_y + 5, hint_w, hint, true);
            }
        }
    }
}

static void ui_px_burn_action_header(const ui_model_t *model, int32_t x, int32_t y, int32_t w, uint16_t count)
{
    char position[20];
    snprintf(position, sizeof(position), "%u/%u", (unsigned)model->selected + 1U, (unsigned)count);
    ui_px_color_box(x + 1, y + 1, w - 2, 21, s_burner_focus_info ? UI_COLOR_SAND : UI_COLOR_AMBER);
    const char *heading = ui_lang_is_zh() ? "卡带操作" : "Actions";
    ui_px_text(x + 6, y + 7, heading, true);
    ui_px_text(x + w - 6 - ui_px_text_width(position), y + 7, position, true);
}

static void ui_px_draw_burn_rom_ops_panel(const ui_model_t *model, int32_t x, int32_t y, int32_t w, int32_t h)
{
    uint16_t count = ui_page_item_count(model);
    uint16_t selected = model->selected;
    s_burn_drawn_selection = selected;
    ui_px_panel(x, y, w, h, false);
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
                ui_px_highlight_rect(key_x + 1, key_y + 1, key_w - 5, key_h - 5);
            }
        }
        return;
    }
    ui_px_burn_action_header(model, x, y, w, count);
    for (uint16_t row = 0; row < ui_burn_rom_visible_rows(); ++row) {
        ui_px_burn_action_row(model, row, count, x, y, w);
    }
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
        ui_px_draw_burner_cart_info_panel(model, layout.info_x, layout.info_y, layout.info_w, layout.info_h);
        ui_px_invalidate_rect(layout.info_x, layout.info_y, layout.info_w, layout.info_h);
    }
    if (redraw_content || s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        ui_px_draw_burn_rom_ops_panel(model, layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
        ui_px_invalidate_rect(layout.ops_x, layout.ops_y, layout.ops_w, layout.ops_h);
        return;
    }
    if (model->motion_dirty && model->selected != s_burn_drawn_selection) {
        uint16_t count = ui_page_item_count(model);
        uint16_t indices[] = {s_burn_drawn_selection, model->selected};
        for (size_t i = 0; i < 2; ++i) {
            uint16_t row = indices[i] - model->scroll;
            if (row >= ui_burn_rom_visible_rows()) continue;
            ui_px_burn_action_row(model, row, count, layout.ops_x, layout.ops_y, layout.ops_w);
            ui_px_invalidate_rect(layout.ops_x, layout.ops_y + UI_BURN_OP_TOP + row * UI_BURN_OP_LINE,
                                  layout.ops_w, UI_BURN_OP_LINE);
        }
        ui_px_burn_action_header(model, layout.ops_x, layout.ops_y, layout.ops_w, count);
        ui_px_invalidate_rect(layout.ops_x, layout.ops_y, layout.ops_w, UI_BURN_OP_TOP);
        s_burn_drawn_selection = model->selected;
    }
}

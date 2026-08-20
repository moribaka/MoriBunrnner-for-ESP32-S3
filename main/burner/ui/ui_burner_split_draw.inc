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

    ui_px_info_line(text_x, yy, text_w, ui_tr("Type:"), ui_probe_type_label(&status, s_cart_mode));
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
        ui_px_text_clipped(
            text_x,
            row_y + 3,
            text_w,
            title,
            !(s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE && index == 1U &&
              s_gba_patch_analysis_done && !ui_gba_sram_patch_selectable()));
        if (hint[0] != '\0') {
            int32_t hint_w = ui_px_text_width(hint);
            int32_t hint_x;

            if (hint_w > text_w / 2) {
                hint_w = text_w / 2;
            }
            hint_x = text_x + text_w - hint_w;
            if (hint_x > text_x + ui_px_text_width(title) + 6) {
                ui_px_text_clipped(
                    hint_x,
                    row_y + 3,
                    hint_w,
                    hint,
                    !(s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE && index == 1U &&
                      s_gba_patch_analysis_done && !ui_gba_sram_patch_selectable()));
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


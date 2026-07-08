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
        case UI_PAGE_APP_MANAGER:
            ui_fill_app_manager_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_TF:
            ui_fill_tf_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_MUSIC_PLAYER:
            ui_fill_music_player_row(model, index, title, title_len, hint, hint_len);
            break;
        case UI_PAGE_FILES:
        case UI_PAGE_MUSIC_FILES:
            ui_fill_file_row(model, row, title, title_len, hint, hint_len, &symbol, &accent);
            break;
        case UI_PAGE_FILE_ACTIONS:
            ui_fill_action_row(model, index, title, title_len, hint, hint_len, &symbol, &accent);
            break;
        case UI_PAGE_WIFI:
            ui_fill_wifi_row(model, index, title, title_len, hint, hint_len, &symbol, &accent);
            break;
        case UI_PAGE_SMB:
            ui_fill_smb_row(model, index, title, title_len, hint, hint_len, &symbol, &accent);
            break;
        case UI_PAGE_SMB_TEXT_INPUT:
            ui_fill_smb_text_input_row(index, title, title_len, hint, hint_len);
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
        case UI_PAGE_TASK_RESULT:
            ui_fill_task_result_row(model, index, title, title_len, hint, hint_len);
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

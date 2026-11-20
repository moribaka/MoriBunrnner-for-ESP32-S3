static void ui_px_cart_icon(int32_t x, int32_t y, const char *label, bool selected)
{
    const int32_t w = UI_BURNER_ICON_W;
    const int32_t h = UI_BURNER_ICON_H;
    const int32_t label_w = ui_px_text_width(label);

    ui_px_panel(x, y, w, h, selected);
    ui_px_frame(x + 16, y + 10, w - 32, h - 30, true);
    ui_px_color_box(x + 26, y + 20, w - 52, 2, UI_COLOR_ORANGE);
    ui_px_hline(x + 26, y + 28, w - 52, true);
    ui_px_hline(x + 26, y + 36, w - 52, true);
    ui_px_text(x + (w - label_w) / 2, y + h - 14, label, true);
}

static void ui_px_draw_burner_mode_cards(const ui_model_t *model)
{
    ui_px_cart_icon(UI_BURNER_ICON_GBA_X, UI_BURNER_ICON_Y, "GBA", model->selected == 0U);
    ui_px_cart_icon(UI_BURNER_ICON_GBC_X, UI_BURNER_ICON_Y, "GB/GBC", model->selected == 1U);
}

static void ui_px_apply_burner_modes(const ui_model_t *model)
{
    const char *title = ui_tr("Select cart type");
    const char *hint = ui_tr("A: enter  B: back");

    ui_px_text((UI_CANVAS_W - ui_px_text_width(title)) / 2, 48, title, true);
    ui_px_draw_burner_mode_cards(model);
    ui_px_text((UI_CANVAS_W - ui_px_text_width(hint)) / 2, 178, hint, true);
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8,
                           ui_status_text_to_display(model->status_text), true);
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
        case UI_PAGE_APPS:
            if (index < s_app_item_count) {
                const ui_menu_item_t *item = &s_app_items[index];
                snprintf(title, title_len, "%s", ui_root_item_title(item));
                snprintf(hint, hint_len, "%s", ui_root_item_hint(item));
            }
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
        case UI_PAGE_AG32_UPDATE:
            ui_fill_ag32_row(model, index, title, title_len, hint, hint_len);
            break;
        default:
            break;
    }
    (void)symbol;
    (void)accent;
}

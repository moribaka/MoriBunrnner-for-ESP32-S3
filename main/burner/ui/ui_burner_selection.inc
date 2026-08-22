static uint16_t ui_burn_rom_item_count(void)
{
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
        return UI_BURN_ROM_WRITE_PATH_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RECIPE_MODE) {
        return UI_BURN_ROM_RECIPE_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_SIZE) {
        return (uint16_t)(UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT + (ui_burn_cart_has_slots() ? 1U : 0U));
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        return UI_BURN_ROM_DUMP_KEY_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_MAPPER) {
        return UI_BURN_ROM_MAPPER_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
        return ui_burn_rom_settings_item_count();
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_ERASE_CONFIRM) {
        return UI_BURN_ROM_ERASE_CONFIRM_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RAM) {
        return UI_BURN_RAM_ITEM_COUNT;
    }
    if (!ui_cart_is_unlocked()) {
        return (uint16_t)(UI_BURN_ROM_LOCKED_ITEM_COUNT + (ui_burn_rom_has_recipe_mode_row() ? 1U : 0U));
    }
    if (s_cart_mode == BURNER_CART_MODE_GBA) {
        return UI_BURN_ROM_GBA_WITH_ROM_ITEM_COUNT;
    }
    return UI_BURN_ROM_MBC5_WITH_ROM_ITEM_COUNT;
}

static size_t ui_cart_mode_index(burner_cart_mode_t mode)
{
    return (mode == BURNER_CART_MODE_GBA) ? 1U : 0U;
}

static const ui_file_entry_t *ui_last_rom_file_for_mode(burner_cart_mode_t mode)
{
    return &s_last_rom_file_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_entry_t *ui_last_rom_file_for_mode_mut(burner_cart_mode_t mode)
{
    return &s_last_rom_file_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_kind_t ui_last_rom_kind_for_mode(burner_cart_mode_t mode)
{
    return s_last_rom_kind_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_kind_t *ui_last_rom_kind_for_mode_mut(burner_cart_mode_t mode)
{
    return &s_last_rom_kind_by_cart[ui_cart_mode_index(mode)];
}

static void ui_focus_burn_rom_row_locked(ui_model_t *model, uint16_t row)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }

    count = ui_burn_rom_item_count();
    if (count == 0U) {
        model->selected = 0;
        model->scroll = 0;
        ui_mark_content_dirty(model);
        return;
    }
    if (row >= count) {
        row = (uint16_t)(count - 1U);
    }

    model->selected = row;
    model->scroll = ui_scroll_for_selected_rows(row, 0, count, ui_burn_rom_visible_rows());
    ui_mark_content_dirty(model);
}

static void ui_focus_burn_rom_op_locked(ui_model_t *model, ui_burn_rom_op_t op)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }

    count = ui_burn_rom_item_count();
    for (uint16_t i = 0U; i < count; ++i) {
        if (ui_burn_rom_op_for_index(i) == op) {
            ui_focus_burn_rom_row_locked(model, i);
            return;
        }
    }
}

static bool ui_burn_cart_has_slots(void)
{
    burner_status_t status = {0};

    burner_status_snapshot(&status);
    return status.probe_valid &&
           status.probe_cart_mode == s_cart_mode &&
           status.probe_device_size > (128U * 1024U * 1024U);
}

static void ui_clamp_burn_rom_selection_locked(ui_model_t *model)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }
    count = ui_burn_rom_item_count();
    if (count == 0U) {
        model->selected = 0;
        model->scroll = 0;
        return;
    }
    if (model->selected >= count) {
        model->selected = (uint16_t)(count - 1U);
    }
    model->scroll = ui_scroll_for_selected_rows(model->selected, model->scroll, count, ui_burn_rom_visible_rows());
}


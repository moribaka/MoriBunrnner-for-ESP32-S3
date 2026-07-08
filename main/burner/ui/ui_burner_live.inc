static void ui_update_burn_rom_prompt_locked(ui_model_t *model)
{
    bool changed = false;
    uint32_t now_ms = esp_log_timestamp();

    if (model == NULL) {
        return;
    }
    if (s_burn_rom_write_prompt_until_ms != 0U &&
        (int32_t)(now_ms - s_burn_rom_write_prompt_until_ms) >= 0) {
        s_burn_rom_write_prompt_until_ms = 0;
        changed = true;
    }
    if (s_burn_rom_verify_prompt_until_ms != 0U &&
        (int32_t)(now_ms - s_burn_rom_verify_prompt_until_ms) >= 0) {
        s_burn_rom_verify_prompt_until_ms = 0;
        changed = true;
    }
    if (changed && model->page == UI_PAGE_BURN_ROM && s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_NONE) {
        ui_mark_content_dirty(model);
    }
}


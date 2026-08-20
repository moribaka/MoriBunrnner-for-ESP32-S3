static uint32_t ui_burn_dump_size_for_index(uint16_t index)
{
    switch (index) {
        case 0:
            return 4U;
        case 1:
            return 8U;
        case 3:
            return 16U;
        case 4:
            return 32U;
        default:
            return 0U;
    }
}

static char ui_burn_dump_key_for_index(uint16_t index)
{
    static const char keys[UI_BURN_ROM_DUMP_KEY_COUNT] = {
        '1', '2', '3',
        '4', '5', '6',
        '7', '8', '9',
        ',', '0', '.',
        '\n',
    };

    return (index < UI_BURN_ROM_DUMP_KEY_COUNT) ? keys[index] : '\0';
}

static bool ui_parse_first_u32_text(const char *text, uint32_t *value_out)
{
    uint32_t value = 0;
    bool fractional_nonzero = false;
    bool any = false;

    if (text == NULL || value_out == NULL) {
        return false;
    }
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text - '0');

        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        any = true;
        text++;
    }
    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9') {
            if (*text != '0') {
                fractional_nonzero = true;
            }
            text++;
        }
    }
    if (!any || value == 0U) {
        return false;
    }
    if (fractional_nonzero) {
        if (value == UINT32_MAX) {
            return false;
        }
        value++;
    }
    *value_out = value;
    return true;
}

static ui_burn_rom_op_t ui_burn_rom_op_for_index(uint16_t index)
{
    uint16_t cursor = 0;

    if (index == cursor++) {
        return UI_BURN_ROM_OP_ANALYZE;
    }
    if (!ui_cart_is_unlocked()) {
        if (ui_burn_rom_has_recipe_mode_row() && index == cursor++) {
            return UI_BURN_ROM_OP_RECIPE_MODE;
        }
        if (index == cursor++) {
            return UI_BURN_ROM_OP_SETTINGS;
        }
        return UI_BURN_ROM_OP_INVALID;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_CHOOSE_ROM;
    }
    if (s_cart_mode == BURNER_CART_MODE_MBC5 && index == cursor++) {
        return UI_BURN_ROM_OP_ROM_MAPPER;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_WRITE_ROM;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_VERIFY_ROM;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_DUMP_ROM;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_ERASE_CHIP;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_UNLOCK_PPB;
    }
    if (s_cart_mode == BURNER_CART_MODE_MBC5) {
        if (index == cursor++) {
            return UI_BURN_ROM_OP_RAM_MENU;
        }
    }
    if (ui_burn_rom_has_recipe_mode_row() && index == cursor++) {
        return UI_BURN_ROM_OP_RECIPE_MODE;
    }
    if (index == cursor++) {
        return UI_BURN_ROM_OP_SETTINGS;
    }
    return UI_BURN_ROM_OP_INVALID;
}

static ui_burn_rom_op_t ui_burn_ram_op_for_index(uint16_t index)
{
    switch (index) {
        case 0:
            return UI_BURN_ROM_OP_CHOOSE_SAVE;
        case 1:
            return UI_BURN_ROM_OP_WRITE_SAVE;
        case 2:
            return UI_BURN_ROM_OP_VERIFY_SAVE;
        case 3:
            return UI_BURN_ROM_OP_DUMP_SAVE;
        case 4:
            return UI_BURN_ROM_OP_SAVE_SIZE;
        case 5:
            return (s_cart_mode == BURNER_CART_MODE_GBA) ? UI_BURN_ROM_OP_GBA_SAVE_TYPE : UI_BURN_ROM_OP_RAM_TYPE;
        case 6:
            return UI_BURN_ROM_OP_RAM_LATENCY;
        default:
            return UI_BURN_ROM_OP_INVALID;
    }
}

static void ui_burn_rom_open_write_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = true;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_WRITE;
    model->selected = 0U;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("select burn patch"));
}

static void ui_start_patch_analysis_locked(ui_model_t *model)
{
    const ui_file_entry_t *rom;
    char *path;

    if (model == NULL || s_gba_patch_analysis_active) {
        return;
    }
    rom = ui_last_rom_file_for_mode(s_cart_mode);
    if (rom == NULL || rom->path[0] == '\0') {
        ui_set_status_locked(model, ui_tr("select ROM first"));
        return;
    }
    path = (char *)malloc(strlen(rom->path) + 1U);
    if (path == NULL) {
        ui_set_status_locked(model, ui_tr("no memory"));
        return;
    }
    strcpy(path, rom->path);
    s_gba_patch_analysis_active = true;
    s_gba_patch_analysis_done = false;
    s_gba_sram_patch_available = false;
    s_gba_sram_patch = false;
    ui_set_status_locked(model, ui_tr("analyzing ROM..."));
    ui_mark_content_dirty(model);
    if (xTaskCreatePinnedToCore(
            ui_patch_analysis_task,
            "ui_patch_probe",
            UI_BURN_PROBE_TASK_STACK_SIZE,
            path,
            UI_STORAGE_TASK_PRIORITY,
            NULL,
            UI_BURN_TASK_CORE_ID) != pdPASS) {
        free(path);
        s_gba_patch_analysis_active = false;
        ui_set_status_locked(model, ui_tr("analysis start failed"));
    }
}

static void ui_burn_rom_open_recipe_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_RECIPE_MODE;
    model->selected = ui_recipe_mode_index(s_recipe_mode);
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_lang_is_zh() ? "选择烧录方式" : "Select burn method");
}

static void ui_burn_rom_open_dump_size_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_DUMP_SIZE;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("select dump size"));
}

static void ui_burn_rom_open_dump_custom_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_DUMP_CUSTOM;
    s_burn_rom_custom_size_text[0] = '\0';
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("input MiB size"));
}

static void ui_burn_rom_open_mapper_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_MAPPER;
    model->selected = (s_gb_mapper_override_kind == BURNER_GB_MAPPER_MBC3) ? 1U : 0U;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("select type"));
}

static void ui_burn_rom_open_erase_confirm_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_ERASE_CONFIRM;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("confirm chip erase"));
}

static void ui_burn_rom_open_ram_menu_locked(ui_model_t *model)
{
    if (model == NULL) {
        return;
    }
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_RAM;
    model->selected = 0;
    model->scroll = 0;
    ui_mark_content_dirty(model);
    ui_set_status_locked(model, ui_tr("RAM operations"));
}


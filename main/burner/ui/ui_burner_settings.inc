static uint16_t ui_burn_rom_settings_item_count(void)
{
    return (s_cart_mode == BURNER_CART_MODE_MBC5) ?
               UI_BURN_ROM_MBC5_SETTINGS_ITEM_COUNT :
               UI_BURN_ROM_GBA_SETTINGS_ITEM_COUNT;
}

static const char *ui_mbc5_voltage_label(void)
{
    return (s_mbc5_power_5v_enabled != 0u) ? "5V" : "3V3";
}

static const char *ui_power_settle_label(uint32_t ms, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return "";
    }
    snprintf(out, out_len, "%" PRIu32 " ms", ms);
    return out;
}

static const char *ui_erase_mode_label(void)
{
    return (s_burn_erase_always != 0u) ? "Force erase" : "Smart skip";
}

static const char *ui_recipe_mode_label(burner_recipe_mode_t mode)
{
    if (mode == BURNER_RECIPE_MODE_CHISLINK) {
        return "CHISLINK";
    }
    return (mode == BURNER_RECIPE_MODE_GBX) ? "GBX" : "CHIS";
}

static const char *ui_recipe_mode_hint(burner_recipe_mode_t mode)
{
    if (mode == s_recipe_mode) {
        return ui_lang_is_zh() ? "当前" : "Current";
    }
    if (mode == BURNER_RECIPE_MODE_CHISLINK) {
        return "CHISLink ID";
    }
    return (mode == BURNER_RECIPE_MODE_GBX) ?
               (ui_lang_is_zh() ? ".gbx配置" : ".gbx profile") :
               (ui_lang_is_zh() ? "自动探测" : "Auto probe");
}

static burner_recipe_mode_t ui_recipe_mode_for_index(uint16_t index)
{
    switch (index) {
        case 1:
            return BURNER_RECIPE_MODE_CHISLINK;
        case 2:
            return BURNER_RECIPE_MODE_GBX;
        case 0:
        default:
            return BURNER_RECIPE_MODE_CHIS;
    }
}

static uint16_t ui_recipe_mode_index(burner_recipe_mode_t mode)
{
    switch (mode) {
        case BURNER_RECIPE_MODE_CHISLINK:
            return 1U;
        case BURNER_RECIPE_MODE_GBX:
            return 2U;
        case BURNER_RECIPE_MODE_CHIS:
        default:
            return 0U;
    }
}

static bool ui_burn_rom_has_recipe_mode_row(void)
{
    return s_cart_mode == BURNER_CART_MODE_GBA || s_cart_mode == BURNER_CART_MODE_MBC5;
}

static void ui_reset_cart_analysis_locked(void)
{
    s_cart_analyzed = false;
    s_analyzed_cart_info[0] = '\0';
    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        burner_status_probe_reset_locked();
        xSemaphoreGive(s_status_lock);
    }
}

static void ui_apply_recipe_mode_locked(ui_model_t *model, burner_recipe_mode_t mode)
{
    bool changed = (s_recipe_mode != mode);

    s_recipe_mode = mode;
    s_burn_rom_write_menu = false;
    s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
    if (changed) {
        ui_persist_burn_settings_locked(model);
        ui_reset_cart_analysis_locked();
    }
    if (mode == BURNER_RECIPE_MODE_CHISLINK) {
        ui_set_status_locked(model, "Burn method: CHISLINK");
        ui_focus_burn_rom_op_locked(model, UI_BURN_ROM_OP_RECIPE_MODE);
        return;
    }
    ui_set_status_locked(
        model,
        (mode == BURNER_RECIPE_MODE_GBX) ?
            (ui_lang_is_zh() ? "烧录方式: GBX" : "Burn method: GBX") :
            (ui_lang_is_zh() ? "烧录方式: CHIS" : "Burn method: CHIS"));
    ui_focus_burn_rom_op_locked(model, UI_BURN_ROM_OP_RECIPE_MODE);
}

static void ui_sync_burn_settings_from_runtime(void)
{
    s_write_path = s_burn_write_path_default;
    s_recipe_mode = s_burn_recipe_mode_default;
    s_psram_mb = s_burn_psram_window_mb;
    s_mbc5_chunk_kb = s_burn_mbc5_chunk_kb;
    s_dump_chunk_kb = s_burn_dump_chunk_kb;
}

static void ui_sync_runtime_burn_settings_from_local(void)
{
    s_burn_write_path_default = s_write_path;
    s_burn_recipe_mode_default = s_recipe_mode;
    s_burn_psram_window_mb = s_psram_mb;
    s_burn_mbc5_chunk_kb = s_mbc5_chunk_kb;
    s_burn_dump_chunk_kb = s_dump_chunk_kb;
}

static void ui_persist_burn_settings_locked(ui_model_t *model)
{
    ui_sync_runtime_burn_settings_from_local();
    if (burner_save_burn_config() != ESP_OK) {
        ui_set_status_locked(model, ui_tr("save failed"));
    }
}

static const char *ui_gba_save_type_label(burner_gba_save_type_t save_type)
{
    switch (save_type) {
        case BURNER_GBA_SAVE_TYPE_SRAM:
            return "SRAM";
        case BURNER_GBA_SAVE_TYPE_EEPROM:
            return "EEPROM";
        case BURNER_GBA_SAVE_TYPE_FLASH:
            return "FLASH";
        case BURNER_GBA_SAVE_TYPE_BATTERYLESS:
            return "BATTERYLESS";
        default:
            return "UNKNOWN";
    }
}

static const char *ui_gba_save_type_probe_label(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid || status->probe_cart_mode != BURNER_CART_MODE_GBA ||
        !status->probe_gba_save_detected) {
        return ui_tr("unknown");
    }
    return ui_gba_save_type_label(status->probe_gba_save_type);
}

static const char *ui_gba_sram_patch_label(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid || status->probe_cart_mode != BURNER_CART_MODE_GBA ||
        !status->probe_gba_sram_patch_scanned) {
        return ui_tr("Not scanned");
    }
    if (!status->probe_gba_sram_patch_detected) {
        return ui_tr("Not patched");
    }
    switch (status->probe_gba_sram_patch_kind) {
        case BURNER_GBA_SRAM_PATCH_GBATA:
            return ui_tr("GBATA SRAM patched");
        case BURNER_GBA_SRAM_PATCH_FLASH1M_REPRO:
            return ui_tr("Flash1M Repro SRAM patched");
        case BURNER_GBA_SRAM_PATCH_NONE:
        default:
            return ui_tr("Patched");
    }
}

static const char *ui_cart_mode_label(burner_cart_mode_t mode)
{
    return (mode == BURNER_CART_MODE_GBA) ? "GBA" : "GBC";
}

static const char *ui_probe_type_label(const burner_status_t *status, burner_cart_mode_t mode)
{
    if (mode == BURNER_CART_MODE_MBC5 &&
        status != NULL &&
        status->probe_valid &&
        status->probe_cart_mode == BURNER_CART_MODE_MBC5 &&
        status->probe_mapper_name[0] != '\0') {
        return status->probe_mapper_name;
    }
    return ui_cart_mode_label(mode);
}

static const char *ui_selected_gb_mapper_label(void)
{
    switch (s_gb_mapper_override_kind) {
        case BURNER_GB_MAPPER_MBC3:
            return "MBC3";
        case BURNER_GB_MAPPER_MBC5:
            return "MBC5";
        case BURNER_GB_MAPPER_UNKNOWN:
        default:
            return ui_tr("Type");
    }
}

static bool ui_cart_is_unlocked(void)
{
    return s_cart_analyzed && s_analyzed_cart_mode == s_cart_mode;
}

static ui_file_action_t ui_burn_action_for_write_path(burner_write_path_t path)
{
    switch (path) {
        case BURNER_WRITE_PATH_PSRAM:
            return UI_FILE_ACTION_BURN_PSRAM;
        case BURNER_WRITE_PATH_PIPELINE:
            return UI_FILE_ACTION_BURN_PIPELINE;
        case BURNER_WRITE_PATH_DIRECT:
        default:
            return UI_FILE_ACTION_BURN_DIRECT;
    }
}

static void ui_format_bytes_text(uint32_t bytes, char *out, size_t out_len)
{
    ui_format_file_size(bytes, out, out_len);
}

static void ui_format_probe_id(const burner_status_t *status, char *out, size_t out_len)
{
    size_t id_len;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (status == NULL || !status->probe_valid) {
        snprintf(out, out_len, "--");
        return;
    }

    id_len = (status->probe_cart_mode == BURNER_CART_MODE_GBA) ? 8U : 4U;
    if (id_len > sizeof(status->probe_id)) {
        id_len = sizeof(status->probe_id);
    }
    out[0] = '\0';
    for (size_t i = 0; i < id_len; ++i) {
        char part[4];
        snprintf(part, sizeof(part), "%02X", status->probe_id[i]);
        strncat(out, part, out_len - strlen(out) - 1U);
        if (i + 1U < id_len) {
            strncat(out, " ", out_len - strlen(out) - 1U);
        }
    }
}

static const char *ui_nor_cmdset_label(burner_nor_cmdset_t cmdset)
{
    switch (cmdset) {
        case BURNER_NOR_CMDSET_AMD:
            return "AMD";
        case BURNER_NOR_CMDSET_INTEL:
            return "INTEL";
        case BURNER_NOR_CMDSET_UNKNOWN:
        default:
            return ui_tr("unknown");
    }
}

static burner_nor_cmdset_t ui_probe_cmdset(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid) {
        return BURNER_NOR_CMDSET_UNKNOWN;
    }

    if (status->probe_cart_mode == BURNER_CART_MODE_GBA) {
        return burner_gba_cmdset_from_id(status->probe_id);
    }
    return burner_mbc5_cmdset_from_id(status->probe_id);
}

static const burner_nor_family_t *ui_probe_family(const burner_status_t *status)
{
    if (status == NULL || !status->probe_valid) {
        return NULL;
    }

    if (status->probe_cart_mode == BURNER_CART_MODE_GBA) {
        return burner_gba_family_from_id(status->probe_id);
    }
    return burner_mbc5_family_from_id(status->probe_id);
}

static void ui_format_probe_family_text(const burner_status_t *status, char *out, size_t out_len)
{
    const burner_nor_family_t *family = NULL;
    burner_nor_cmdset_t cmdset = BURNER_NOR_CMDSET_UNKNOWN;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (status == NULL || !status->probe_valid) {
        snprintf(out, out_len, "--");
        return;
    }

    family = ui_probe_family(status);
    cmdset = ui_probe_cmdset(status);
    if (family != NULL && family->name != NULL && family->name[0] != '\0') {
        if (cmdset != BURNER_NOR_CMDSET_UNKNOWN) {
            snprintf(out, out_len, "%s / %s", family->name, ui_nor_cmdset_label(cmdset));
        } else {
            snprintf(out, out_len, "%s", family->name);
        }
        return;
    }
    if (cmdset != BURNER_NOR_CMDSET_UNKNOWN) {
        snprintf(out, out_len, "%s", ui_nor_cmdset_label(cmdset));
        return;
    }

    snprintf(out, out_len, "--");
}

static void ui_format_gba_d0d1_text(const burner_status_t *status, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (status == NULL || !status->probe_valid || status->probe_cart_mode != BURNER_CART_MODE_GBA) {
        snprintf(out, out_len, "--");
    } else if (!status->probe_gba_d0d1_known) {
        snprintf(out, out_len, "%s", ui_tr("unknown"));
    } else {
        snprintf(out, out_len, "%s", status->probe_gba_d0d1_swapped ? ui_tr("swapped") : ui_tr("normal"));
    }
}

static void ui_format_u64_size(uint64_t bytes, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_len, "%" PRIu64 " B", bytes);
    }
}

static void ui_format_elapsed(uint64_t elapsed_us, char *out, size_t out_len)
{
    uint64_t seconds = elapsed_us / 1000000ULL;
    uint64_t minutes = seconds / 60ULL;
    uint64_t hours = minutes / 60ULL;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (hours > 0ULL) {
        snprintf(out, out_len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, minutes % 60ULL, seconds % 60ULL);
    } else {
        snprintf(out, out_len, "%02" PRIu64 ":%02" PRIu64, minutes, seconds % 60ULL);
    }
}

static void ui_format_uptime(uint64_t uptime_ms, char *out, size_t out_len)
{
    uint64_t seconds = uptime_ms / 1000ULL;
    uint64_t hours = seconds / 3600ULL;
    uint64_t minutes = (seconds / 60ULL) % 60ULL;
    uint64_t secs = seconds % 60ULL;

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (hours > 99ULL) {
        snprintf(out, out_len, "%" PRIu64 "d %02" PRIu64 "h", hours / 24ULL, hours % 24ULL);
    } else {
        snprintf(out, out_len, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, minutes, secs);
    }
}

static uint32_t ui_next_option_u32(const uint32_t *options, size_t count, uint32_t current, int delta)
{
    size_t index = 0;

    if (options == NULL || count == 0U) {
        return current;
    }
    for (size_t i = 0; i < count; ++i) {
        if (options[i] == current) {
            index = i;
            break;
        }
    }
    if (delta >= 0) {
        index = (index + 1U) % count;
    } else {
        index = (index == 0U) ? (count - 1U) : (index - 1U);
    }
    return options[index];
}


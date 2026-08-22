static void ui_fill_burner_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    (void)model;
    if (index == 0U) {
        snprintf(title, title_len, "GBA");
        snprintf(hint, hint_len, "%s", ui_tr("Game Boy Advance"));
        return;
    }
    snprintf(title, title_len, "GBC");
    snprintf(hint, hint_len, "%s", ui_tr("Game Boy Color"));
}

static void ui_fill_burn_rom_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    char psram_label[16] = {0};
    char settle_label[16] = {0};
    (void)model;
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
        if (index == 0U) {
            snprintf(title, title_len, "%s", ui_tr("Burn direct"));
            snprintf(hint, hint_len, "%s", ui_write_path_label());
        } else if (index == 1U) {
            snprintf(title, title_len, "%s", ui_tr("Save patch SRAM/FLASH"));
            snprintf(hint, hint_len, "%s", ui_gba_sram_patch_selectable() ?
                                                    (s_gba_sram_patch ? ui_tr("Yes") : ui_tr("No")) :
                                                    ui_tr("Not needed"));
        } else if (index == 2U) {
            snprintf(title, title_len, "%s", ui_tr("Latency patch"));
            snprintf(hint, hint_len, "%s", s_gba_waitcnt_patch ? ui_tr("Yes") : ui_tr("No"));
        } else {
            snprintf(title, title_len, "%s", ui_tr("Batteryless patch"));
            snprintf(hint, hint_len, "%s", !s_gba_sram_patch ? ui_tr("Disabled") :
                                                    (s_gba_batteryless_patch ? ui_tr("Yes") : ui_tr("No")));
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_SIZE) {
        uint32_t dump_size_mib = ui_burn_dump_size_for_index(index);

        if (index == UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT && ui_burn_cart_has_slots()) {
            snprintf(title, title_len, "%s", ui_tr("Slot"));
            snprintf(hint, hint_len, "%" PRIu32, s_cart_slot);
        } else if (index == 2U) {
            snprintf(title, title_len, "%s", ui_tr("Custom"));
            snprintf(hint, hint_len, "%s", s_burn_rom_custom_size_text[0] != '\0' ? s_burn_rom_custom_size_text : ui_tr("keypad"));
        } else {
            snprintf(title, title_len, "%" PRIu32 " MiB", dump_size_mib);
            snprintf(hint, hint_len, "%s", ui_tr("dump ROM"));
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        char key = ui_burn_dump_key_for_index(index);

        if (key == '\n') {
            snprintf(title, title_len, "%s", ui_tr("OK"));
        } else {
            snprintf(title, title_len, "%c", key);
        }
        snprintf(hint, hint_len, "%s", s_burn_rom_custom_size_text[0] != '\0' ? s_burn_rom_custom_size_text : ui_tr("MiB"));
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RECIPE_MODE) {
        burner_recipe_mode_t mode = ui_recipe_mode_for_index(index);

        snprintf(title, title_len, "%s", ui_recipe_mode_label(mode));
        snprintf(hint, hint_len, "%s", ui_recipe_mode_hint(mode));
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_MAPPER) {
        snprintf(title, title_len, "%s", (index == 1U) ? "MBC3" : "MBC5");
        if ((index == 0U && s_gb_mapper_override_kind == BURNER_GB_MAPPER_MBC5) ||
            (index == 1U && s_gb_mapper_override_kind == BURNER_GB_MAPPER_MBC3)) {
            snprintf(hint, hint_len, "%s", ui_tr("done"));
        } else if (hint_len > 0U) {
            hint[0] = '\0';
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
        if (index == 0U) {
            snprintf(title, title_len, "%s", ui_tr("Write method"));
            snprintf(hint, hint_len, "%s", ui_write_path_label());
        } else if (index == 1U) {
            snprintf(title, title_len, "%s", ui_tr("Erase mode"));
            snprintf(hint, hint_len, "%s", ui_erase_mode_label());
        } else if (index == 2U) {
            snprintf(title, title_len, "%s", ui_tr("Voltage settle"));
            snprintf(hint, hint_len, "%s", ui_power_settle_label(s_bacon_power_settle_ms, settle_label, sizeof(settle_label)));
        } else if (index == 3U) {
            snprintf(title, title_len, "%s", ui_tr("PSRAM window"));
            snprintf(hint, hint_len, "%s", ui_psram_window_label(s_psram_mb, psram_label, sizeof(psram_label)));
        } else if (index == 4U) {
            snprintf(title, title_len, "%s", ui_tr("Dump chunk"));
            snprintf(hint, hint_len, "%" PRIu32 " KB", s_dump_chunk_kb);
        } else if (s_cart_mode == BURNER_CART_MODE_MBC5) {
            snprintf(title, title_len, "%s", ui_tr("Voltage"));
            snprintf(hint, hint_len, "%s", ui_mbc5_voltage_label());
        } else {
            snprintf(title, title_len, "--");
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_ERASE_CONFIRM) {
        if (index == 0U) {
            snprintf(title, title_len, "%s", ui_tr("No"));
            snprintf(hint, hint_len, "%s", ui_tr("cancel"));
        } else {
            snprintf(title, title_len, "%s", ui_tr("Yes, erase chip"));
            snprintf(hint, hint_len, "%s", ui_tr("danger"));
        }
        return;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RAM) {
        switch (index) {
            case 0:
                snprintf(title, title_len, "%s", ui_tr("RAM: Choose save"));
                snprintf(hint, hint_len, "%s", s_last_save_file.path[0] != '\0' ? s_last_save_file.name : ui_tr("not selected"));
                break;
            case 1:
                snprintf(title, title_len, "%s", ui_tr("RAM: Write"));
                snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
                break;
            case 2:
                snprintf(title, title_len, "%s", ui_tr("RAM: Verify"));
                snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
                break;
            case 3:
                snprintf(title, title_len, "%s", ui_tr("RAM: Dump"));
                snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
                break;
            case 4:
                snprintf(title, title_len, "%s", ui_tr("RAM size"));
                snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
                break;
            case 5:
                snprintf(title, title_len, "%s", ui_tr("RAM type"));
                snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
                break;
            default:
                snprintf(title, title_len, "%s", ui_tr("FRAM latency"));
                snprintf(hint, hint_len, "%u", (unsigned)s_ram_latency);
                break;
        }
        return;
    }
    if (ui_burn_rom_op_for_index(index) == UI_BURN_ROM_OP_RECIPE_MODE) {
        snprintf(title, title_len, "%s", ui_lang_is_zh() ? "烧录方式" : ui_tr("Burn method"));
        snprintf(hint, hint_len, "%s", ui_recipe_mode_label(s_recipe_mode));
        return;
    }
    if (ui_burn_rom_op_for_index(index) == UI_BURN_ROM_OP_ANALYZE) {
        snprintf(title, title_len, "%s", (s_cart_analyzed && s_analyzed_cart_mode == s_cart_mode) ? ui_tr("Re-analyze") : ui_tr("Analyze cart"));
        snprintf(hint, hint_len, "%s", ui_cart_is_unlocked() ? ui_tr("done") : "");
        return;
    }
    if (!ui_cart_is_unlocked()) {
        switch (ui_burn_rom_op_for_index(index)) {
            case UI_BURN_ROM_OP_SETTINGS:
                snprintf(title, title_len, "%s", ui_tr("Settings"));
                snprintf(
                    hint,
                    hint_len,
                    "%s",
                    (s_cart_mode == BURNER_CART_MODE_MBC5) ?
                        ui_tr("erase/PSRAM/chunk/voltage") :
                        ui_tr("erase/PSRAM/dump"));
                return;
            case UI_BURN_ROM_OP_INVALID:
            default:
                snprintf(title, title_len, "%s", ui_tr("Locked"));
                snprintf(hint, hint_len, "%s", ui_tr("analyze first"));
                return;
        }
    }
    switch (ui_burn_rom_op_for_index(index)) {
        case UI_BURN_ROM_OP_CHOOSE_ROM:
            {
                const ui_file_entry_t *rom_file = ui_last_rom_file_for_mode(s_cart_mode);

                if (rom_file->path[0] != '\0') {
                    const char *rom_name = (rom_file->name[0] != '\0') ? rom_file->name : rom_file->path;

                    snprintf(title, title_len, "ROM: %s", rom_name);
                    snprintf(hint, hint_len, "%s", rom_file->path);
                } else {
                    snprintf(title, title_len, "%s", ui_tr("ROM: Choose file"));
                    snprintf(hint, hint_len, "%s", ui_tr("not selected"));
                }
            }
            return;
        case UI_BURN_ROM_OP_ROM_MAPPER:
            if (s_gb_mapper_override_kind == BURNER_GB_MAPPER_UNKNOWN) {
                snprintf(title, title_len, "%s", ui_tr("ROM: Choose MBC"));
                if (hint_len > 0U) {
                    hint[0] = '\0';
                }
            } else {
                snprintf(title, title_len, "ROM: %s", ui_selected_gb_mapper_label());
                snprintf(hint, hint_len, "%s", ui_tr("done"));
            }
            return;
        case UI_BURN_ROM_OP_WRITE_ROM:
            if (s_burn_rom_write_prompt_until_ms != 0U &&
                (int32_t)(s_burn_rom_write_prompt_until_ms - esp_log_timestamp()) > 0) {
                snprintf(title, title_len, "%s", ui_tr("Select file first!!!"));
                if (hint_len > 0U) {
                    hint[0] = '\0';
                }
                return;
            }
            snprintf(title, title_len, "%s", ui_tr("ROM: Write"));
            snprintf(hint, hint_len, "%s", ui_tr("choose method"));
            return;
        case UI_BURN_ROM_OP_VERIFY_ROM:
            if (s_burn_rom_verify_prompt_until_ms != 0U &&
                (int32_t)(s_burn_rom_verify_prompt_until_ms - esp_log_timestamp()) > 0) {
                snprintf(title, title_len, "%s", ui_tr("Select file first!!!"));
                if (hint_len > 0U) {
                    hint[0] = '\0';
                }
                return;
            }
            snprintf(title, title_len, "%s", ui_tr("ROM: Verify"));
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
            return;
        case UI_BURN_ROM_OP_DUMP_ROM:
            snprintf(title, title_len, "%s", ui_tr("ROM: Dump"));
            snprintf(hint, hint_len, "%s", ui_tr("select size"));
            return;
        case UI_BURN_ROM_OP_ERASE_CHIP:
            snprintf(title, title_len, "%s", ui_tr("ROM: Erase chip"));
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
            return;
        case UI_BURN_ROM_OP_UNLOCK_PPB:
            snprintf(title, title_len, "%s", ui_tr("ROM: Unlock PPB"));
            snprintf(hint, hint_len, "%s", ui_tr("clear sector protection"));
            return;
        case UI_BURN_ROM_OP_RAM_MENU:
            snprintf(title, title_len, "%s", ui_tr("RAM: Operations"));
            snprintf(hint, hint_len, "%s", ui_tr("save write/dump"));
            return;
        case UI_BURN_ROM_OP_CHOOSE_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Choose save"));
            snprintf(hint, hint_len, "%s", s_last_save_file.path[0] != '\0' ? s_last_save_file.name : ui_tr("not selected"));
            return;
        case UI_BURN_ROM_OP_WRITE_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Write"));
            snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
            return;
        case UI_BURN_ROM_OP_VERIFY_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Verify"));
            snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
            return;
        case UI_BURN_ROM_OP_DUMP_SAVE:
            snprintf(title, title_len, "%s", ui_tr("RAM: Dump"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            return;
        case UI_BURN_ROM_OP_SAVE_SIZE:
            snprintf(title, title_len, "%s", ui_tr("RAM size"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            return;
        case UI_BURN_ROM_OP_RECIPE_MODE:
            snprintf(title, title_len, "%s", ui_lang_is_zh() ? "烧录方式" : ui_tr("Burn method"));
            snprintf(hint, hint_len, "%s", ui_recipe_mode_label(s_recipe_mode));
            return;
        case UI_BURN_ROM_OP_SETTINGS:
            snprintf(title, title_len, "%s", ui_tr("Settings"));
            snprintf(
                hint,
                hint_len,
                "%s",
                (s_cart_mode == BURNER_CART_MODE_MBC5) ?
                    ui_tr("erase/PSRAM/chunk/voltage") :
                    ui_tr("erase/PSRAM/dump"));
            return;
        case UI_BURN_ROM_OP_ANALYZE:
        case UI_BURN_ROM_OP_INVALID:
        default:
            snprintf(title, title_len, "--");
            if (hint_len > 0U) {
                hint[0] = '\0';
            }
            return;
    }
}

static void ui_fill_burn_save_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("Choose TF save"));
            snprintf(hint, hint_len, "%s", model->action_file.path[0] != '\0' ? model->action_file.name : ui_tr("not selected"));
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Write save"));
            snprintf(hint, hint_len, "%s", ui_tr("MBC5 only"));
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Verify save"));
            snprintf(hint, hint_len, "%s", ui_tr("MBC5 only"));
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Dump save"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Save size"));
            snprintf(hint, hint_len, "%" PRIu32 " KiB", s_save_size_kib);
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("RAM type"));
            snprintf(hint, hint_len, "%s", s_ram_fram ? "FRAM" : "SRAM");
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("FRAM latency"));
            snprintf(hint, hint_len, "%u", (unsigned)s_ram_latency);
            break;
        case 7:
            snprintf(title, title_len, "%s", ui_tr("Export batteryless save"));
            snprintf(hint, hint_len, "%s", s_cart_mode == BURNER_CART_MODE_GBA ? ui_tr("ROM tail") : ui_tr("GBA only"));
            break;
        default:
            snprintf(title, title_len, "%s", ui_tr("Slot"));
            snprintf(hint, hint_len, "%" PRIu32, s_cart_slot);
            break;
    }
}

static void ui_fill_task_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    burner_status_t status = {0};
    char size_a[24] = {0};
    char size_b[24] = {0};
    char speed[24] = {0};
    char elapsed[24] = {0};
    char id_text[32] = {0};

    burner_status_snapshot(&status);
    ui_format_elapsed(status.task_elapsed_us > 0ULL ? status.task_elapsed_us : model->burn_elapsed_us, elapsed, sizeof(elapsed));
    if (s_task_cancel_confirm) {
        if (index == 0U) {
            snprintf(title, title_len, "%s", ui_tr("No"));
            snprintf(hint, hint_len, "%s", ui_tr("continue current task"));
        } else {
            snprintf(title, title_len, "%s", ui_tr("Yes, stop task"));
            snprintf(hint, hint_len, "%s", ui_tr("return to previous page"));
        }
        return;
    }
    if (index >= UI_TASK_PATCH_BASE_ROW) {
        uint16_t patch_index = (uint16_t)(index - UI_TASK_PATCH_BASE_ROW);
        int kind = -1;
        if (model->gba_patch_sram) {
            if (patch_index == 0U) kind = 0;
            if (kind < 0) patch_index--;
        }
        if (kind < 0 && model->gba_patch_batteryless) {
            if (patch_index == 0U) kind = 1;
            if (kind < 0) patch_index--;
        }
        if (kind < 0 && model->gba_patch_waitcnt && patch_index == 0U) kind = 2;
        if (kind >= 0) {
            const char *labels[] = {"SRAM patch", "Batteryless patch", "Latency patch"};
            snprintf(title, title_len, "%s", ui_tr(labels[kind]));
            snprintf(hint, hint_len, "%d%% %s", model->gba_patch_progress[kind], model->gba_patch_message[kind]);
        }
        return;
    }
    if (index == 1U) {
        snprintf(title, title_len, "%s", ui_tr("ROM/File"));
        snprintf(hint, hint_len, "%s", status.rom_name[0] != '\0' ? status.rom_name : "--");
        return;
    }
    if (ui_chip_erase_busy_active(model, &status) &&
        (index == UI_TASK_ERASE_PROGRESS_ROW || index == UI_TASK_BURN_PROGRESS_ROW)) {
        title[0] = '\0';
        hint[0] = '\0';
        return;
    }
    if (index == UI_TASK_ERASE_PROGRESS_ROW) {
        snprintf(title, title_len, "%s", ui_tr("Erase"));
        if (model->erase_total_sectors > 0U) {
            snprintf(
                hint,
                hint_len,
                "%" PRIu32 "/%" PRIu32 " %d%%",
                model->erase_done_sectors,
                model->erase_total_sectors,
                model->erase_progress);
        } else {
            snprintf(hint, hint_len, "--");
        }
        return;
    }
    if (index == UI_TASK_BURN_PROGRESS_ROW) {
        snprintf(title, title_len, "%s", ui_tr("Burn"));
        snprintf(hint, hint_len, "%d%%", model->burn_progress);
        return;
    }
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("State"));
            snprintf(hint, hint_len, "%s", ui_burn_state_text(status.state));
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("Progress"));
            snprintf(hint, hint_len, "%d%%", model->burn_progress);
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Bytes"));
            ui_format_bytes_text(model->burn_processed, size_a, sizeof(size_a));
            ui_format_bytes_text(model->burn_total, size_b, sizeof(size_b));
            snprintf(hint, hint_len, "%s/%s", size_a, size_b);
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Elapsed"));
            snprintf(hint, hint_len, "%s", elapsed);
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Speed"));
            ui_format_speed_text(status.speed_current_bps, speed, sizeof(speed));
            snprintf(hint, hint_len, "%s", speed);
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("Average"));
            ui_format_speed_text(status.speed_avg_bps, speed, sizeof(speed));
            snprintf(hint, hint_len, "%s", speed);
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("ROM/File"));
            snprintf(hint, hint_len, "%s", status.rom_name[0] != '\0' ? status.rom_name : "--");
            break;
        case UI_TASK_BURN_PROGRESS_ROW:
            snprintf(title, title_len, "%s", ui_tr("Burn"));
            snprintf(hint, hint_len, "%d%%", model->burn_progress);
            break;
        case 8:
            snprintf(title, title_len, "%s", ui_tr("Cart"));
            if (status.probe_valid) {
                ui_format_bytes_text(status.probe_device_size, size_a, sizeof(size_a));
                snprintf(
                    hint,
                    hint_len,
                    "%s %s",
                    (status.probe_cart_mode == BURNER_CART_MODE_GBA) ? "GBA" : "MBC5",
                    size_a);
            } else {
                snprintf(hint, hint_len, "%s", ui_tr("not probed"));
            }
            break;
        case 9:
            snprintf(title, title_len, "%s", ui_tr("NOR geom"));
            if (status.probe_valid) {
                ui_format_bytes_text(status.probe_sector_size, size_a, sizeof(size_a));
                ui_format_bytes_text(status.probe_buffer_write_bytes, size_b, sizeof(size_b));
                snprintf(
                    hint,
                    hint_len,
                    "sec %s buf %s",
                    status.probe_sector_size > 0U ? size_a : "--",
                    status.probe_buffer_write_bytes > 0U ? size_b : "--");
            } else {
                snprintf(hint, hint_len, "--");
            }
            break;
        case 10:
            snprintf(title, title_len, "%s", ui_tr("Mapping"));
            if (status.probe_valid && status.probe_cart_mode == BURNER_CART_MODE_GBA) {
                if (s_cart_ctx.gba_likely_read_only && !status.probe_cfi_ok) {
                    snprintf(hint, hint_len, "%s", ui_tr("Read-only retail ROM"));
                } else {
                    snprintf(
                        hint,
                        hint_len,
                        "%s CFI %s%s",
                        status.probe_gba_multi ? "4MB window" : "linear",
                        status.probe_cfi_ok ? "ok" : "fallback",
                        status.probe_gba_force_multi ? " forced" : "");
                }
            } else if (status.probe_valid) {
                snprintf(hint, hint_len, "MBC5 CFI %s", status.probe_cfi_ok ? "ok" : "fallback");
            } else {
                snprintf(hint, hint_len, "--");
            }
            break;
        default:
            snprintf(title, title_len, "NOR ID");
            ui_format_probe_id(&status, id_text, sizeof(id_text));
            snprintf(hint, hint_len, "%s", id_text);
            break;
    }
}

static void ui_fill_task_result_row(const ui_model_t *model, uint16_t index, char *title, size_t title_len, char *hint, size_t hint_len)
{
    burner_status_t status = {0};
    const burner_status_t *cached_status = ui_task_result_status_buffer();
    char size_a[24] = {0};
    char size_b[24] = {0};
    char text[24] = {0};
    char probe_text[96] = {0};
    uint64_t elapsed_us;

    if (cached_status != NULL && (s_task_result_status_valid || (model != NULL && model->task_result_status_valid))) {
        status = *cached_status;
    } else {
        burner_status_snapshot(&status);
    }
    elapsed_us = status.task_elapsed_us > 0ULL ? status.task_elapsed_us : (model != NULL ? model->burn_elapsed_us : 0ULL);

    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_tr("State"));
            snprintf(hint, hint_len, "%s", ui_burn_state_text(status.state));
            break;
        case 1:
            snprintf(title, title_len, "%s", ui_tr("ROM/File"));
            snprintf(hint, hint_len, "%s", status.rom_name[0] != '\0' ? status.rom_name : "--");
            break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Bytes"));
            ui_format_bytes_text(
                status.processed_bytes > 0U ? status.processed_bytes : (model != NULL ? model->burn_processed : 0U),
                size_a,
                sizeof(size_a));
            ui_format_bytes_text(
                status.total_bytes > 0U ? status.total_bytes : (model != NULL ? model->burn_total : 0U),
                size_b,
                sizeof(size_b));
            snprintf(hint, hint_len, "%s/%s", size_a, size_b);
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Total time"));
            ui_format_elapsed(elapsed_us, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 4:
            snprintf(title, title_len, "%s", ui_tr("Erase time"));
            ui_format_elapsed(status.erase_elapsed_us, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("Write time"));
            ui_format_elapsed(status.write_elapsed_us, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("Write max"));
            ui_format_speed_text(status.speed_max_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 7:
            snprintf(title, title_len, "%s", ui_tr("Write avg"));
            ui_format_speed_text(status.speed_avg_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 8:
            snprintf(title, title_len, "%s", ui_tr("Write min"));
            ui_format_speed_text(status.speed_min_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 9:
            snprintf(title, title_len, "%s", ui_tr("TF->PSRAM max"));
            ui_format_speed_text(status.tf_to_psram_speed_max_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 10:
            snprintf(title, title_len, "%s", ui_tr("TF->PSRAM avg"));
            ui_format_speed_text(status.tf_to_psram_speed_avg_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 11:
            snprintf(title, title_len, "%s", ui_tr("Dump read max"));
            ui_format_speed_text(status.dump_read_speed_max_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 12:
            snprintf(title, title_len, "%s", ui_tr("Dump read avg"));
            ui_format_speed_text(status.dump_read_speed_avg_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 13:
            snprintf(title, title_len, "%s", ui_tr("Dump write max"));
            ui_format_speed_text(status.dump_write_speed_max_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 14:
            snprintf(title, title_len, "%s", ui_tr("Dump write avg"));
            ui_format_speed_text(status.dump_write_speed_avg_bps, text, sizeof(text));
            snprintf(hint, hint_len, "%s", text);
            break;
        case 15:
            snprintf(title, title_len, "NOR family");
            ui_format_probe_family_text(&status, probe_text, sizeof(probe_text));
            snprintf(hint, hint_len, "%s", probe_text);
            break;
        case 16:
            snprintf(title, title_len, "NOR chip");
            snprintf(hint, hint_len, "%s", ui_probe_chip_name(&status));
            break;
        default:
            snprintf(title, title_len, "NOR ID");
            ui_format_probe_id(&status, probe_text, sizeof(probe_text));
            snprintf(hint, hint_len, "%s", probe_text);
            break;
    }
}


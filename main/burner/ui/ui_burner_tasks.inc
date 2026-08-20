static void ui_patch_analysis_task(void *param)
{
    char *path = (char *)param;
    bool available = burner_gba_rom_has_sram_patch_target(path);

    free(path);
    if (ui_take_model_lock()) {
        s_gba_sram_patch_available = available;
        s_gba_patch_analysis_active = false;
        s_gba_patch_analysis_done = true;
        if (!available) {
            s_gba_sram_patch = false;
        }
        ui_set_status_locked(&s_model, available ? ui_tr("patch options ready") : ui_tr("SRAM patch not needed"));
        ui_mark_content_dirty(&s_model);
        xSemaphoreGive(s_model_lock);
    }
    vTaskDelete(NULL);
}

static void ui_select_file_for_burner_locked(ui_model_t *model, const ui_file_entry_t *entry)
{
    ui_file_kind_t kind;

    if (model == NULL || entry == NULL) {
        return;
    }
    kind = ui_file_kind_from_name(entry->name);
    if (kind == UI_FILE_KIND_UNSUPPORTED) {
        ui_set_status_locked(model, ui_tr("unsupported file"));
        return;
    }
    if (!ui_file_kind_matches_filter(kind, model->file_filter)) {
        ui_set_status_locked(model, ui_tr("file type mismatch"));
        return;
    }
    model->action_kind = kind;
    model->action_file = *entry;
    model->file_filter = UI_FILE_FILTER_NONE;
    if (kind == UI_FILE_KIND_SAVE) {
        s_last_save_file = *entry;
        s_burn_rom_write_menu = false;
        model->page = UI_PAGE_BURN_ROM;
        model->parent_page = UI_PAGE_BURNER;
        s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_RAM;
        ui_set_status_locked(model, ui_tr("save selected"));
        ui_focus_burn_rom_row_locked(model, 1U);
    } else {
        *ui_last_rom_file_for_mode_mut(s_cart_mode) = *entry;
        *ui_last_rom_kind_for_mode_mut(s_cart_mode) = kind;
        s_gba_sram_patch_available = false;
        s_gba_patch_analysis_active = false;
        s_gba_patch_analysis_done = false;
        s_gba_sram_patch = false;
        if (s_cart_mode == BURNER_CART_MODE_MBC5) {
            s_gb_mapper_override_kind = BURNER_GB_MAPPER_UNKNOWN;
        }
        s_burn_rom_write_menu = false;
        model->page = UI_PAGE_BURN_ROM;
        model->parent_page = UI_PAGE_BURNER;
        ui_set_status_locked(model, ui_tr("ROM selected"));
        if (s_cart_mode == BURNER_CART_MODE_MBC5) {
            ui_burn_rom_open_mapper_menu_locked(model);
        } else {
            s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
            ui_focus_burn_rom_op_locked(
                model,
                UI_BURN_ROM_OP_WRITE_ROM);
        }
    }
    ui_clamp_burn_rom_selection_locked(model);
    ui_drop_nav_target_locked(model->page);
    model->dirty = true;
}

static esp_err_t ui_prepare_file_action_locked(ui_model_t *model, ui_file_start_request_t **request_out)
{
    ui_file_start_request_t *request = NULL;
    ui_file_action_t action;

    if (request_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *request_out = NULL;
    if (model == NULL || model->page != UI_PAGE_FILE_ACTIONS) {
        return ESP_ERR_INVALID_STATE;
    }
    if (model->selected >= ui_file_action_count_for_kind(model->action_kind)) {
        ui_set_status_locked(model, ui_tr("file details"));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_file_start_active) {
        ui_set_status_locked(model, ui_tr("task starting"));
        return ESP_ERR_INVALID_STATE;
    }
    if (card == NULL) {
        ui_set_status_locked(model, ui_tr("TF card not ready"));
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, ui_tr("TF busy by USB host"));
        return ESP_ERR_INVALID_STATE;
    }

    action = ui_file_action_for_kind(model->action_kind, (uint8_t)model->selected);
    request = (ui_file_start_request_t *)calloc(1, sizeof(*request));
    if (request == NULL) {
        ui_set_status_locked(model, ui_tr("no memory"));
        return ESP_ERR_NO_MEM;
    }

    ui_utf8_safe_copy(request->name, sizeof(request->name), model->action_file.name);
    snprintf(request->path, sizeof(request->path), "%s", model->action_file.path);
    request->size = model->action_file.size;
    request->kind = model->action_kind;
    request->action = action;
    if (model->page == UI_PAGE_BURN_ROM) {
        request->cart_mode = s_cart_mode;
    } else {
        request->cart_mode = ui_file_cart_mode_for_kind(model->action_kind);
    }
    request->recipe_mode = s_recipe_mode;
    request->slot = 0;
    request->write_path = (action == UI_FILE_ACTION_BURN_PSRAM) ?
                              BURNER_WRITE_PATH_PSRAM :
                              ((action == UI_FILE_ACTION_BURN_PIPELINE) ?
                                   BURNER_WRITE_PATH_PIPELINE :
                                   BURNER_WRITE_PATH_DIRECT);
    request->erase_always = (s_burn_erase_always != 0u);
    request->psram_mb = s_psram_mb;
    request->mbc5_chunk_kb = s_mbc5_chunk_kb;
    request->gba_force_no_cfi = false;
    request->gba_sram_patch = false;
    request->gba_waitcnt_patch = false;
    request->gba_save_type = s_gba_save_type;
    request->gba_save_size = model->action_file.size;
    request->gbx_profile_file[0] = '\0';
    request->ram_fram = false;
    request->ram_latency = 10U;
    request->return_page = UI_PAGE_ROOT;

    s_file_start_active = true;
    ui_task_cancel_confirm_reset_locked();
    ui_begin_task_result_capture_locked(model);
    model->page = UI_PAGE_TASK_STATUS;
    model->parent_page = request->return_page;
    model->burn_progress = 0;
    model->burn_processed = 0;
    model->burn_total = request->size;
    model->erase_progress = 0;
    model->erase_done_sectors = 0;
    model->erase_total_sectors = 0;
    model->burn_elapsed_us = 0;
    ui_set_status_locked(model, ui_tr("starting burn task"));
    *request_out = request;
    return ESP_OK;
}

static void ui_finish_file_start_task(
    const ui_file_start_request_t *request,
    esp_err_t err,
    const burner_task_start_result_t *result,
    const char *error_msg)
{
    const char *label = (request != NULL) ? ui_file_action_label(request->action) : "burn";
    const char *msg = (error_msg != NULL && error_msg[0] != '\0') ? error_msg : esp_err_to_name(err);

    if (!ui_take_model_lock()) {
        return;
    }

    s_file_start_active = false;
    ui_task_cancel_confirm_reset_locked();
    s_model.page = UI_PAGE_TASK_STATUS;
    s_model.parent_page = (request != NULL) ? request->return_page : UI_PAGE_ROOT;
    if (err == ESP_OK) {
        s_model.burn_progress = 0;
        s_model.burn_processed = 0;
        s_model.burn_total = (result != NULL) ? result->effective_size : 0U;
        s_model.erase_progress = 0;
        s_model.erase_done_sectors = 0;
        s_model.erase_total_sectors = 0;
        s_model.burn_elapsed_us = 0;
        snprintf(s_model.status_text, sizeof(s_model.status_text), "%s%s", label, ui_tr(" started"));
    } else {
        s_model.burn_progress = 0;
        s_model.burn_processed = 0;
        s_model.burn_total = 0;
        s_model.erase_progress = 0;
        s_model.erase_done_sectors = 0;
        s_model.erase_total_sectors = 0;
        s_model.burn_elapsed_us = 0;
        snprintf(
            s_model.status_text,
            sizeof(s_model.status_text),
            "%s%.72s",
            ui_tr("start failed: "),
            msg);
        ESP_LOGW(UI_TAG, "LCD file action start failed (%s): %s", label, msg);
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_start_file_action_task(void *param)
{
    ui_file_start_request_t *request = (ui_file_start_request_t *)param;
    burner_task_start_result_t result = {0};
    char error_msg[160] = {0};
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool task_with_caps = false;
    bool perf_lock_acquired = false;

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = request->task_with_caps;
    if (power_manager_perf_lock_acquire("ui_file_start") == ESP_OK) {
        perf_lock_acquired = true;
    }

    err = burner_backend_init();
    if (err != ESP_OK) {
        snprintf(error_msg, sizeof(error_msg), "burn backend init failed: %s", esp_err_to_name(err));
        ui_finish_file_start_task(request, err, &result, error_msg);
        free(request);
        if (perf_lock_acquired) {
            power_manager_perf_lock_release("ui_file_start");
            perf_lock_acquired = false;
        }
        if (task_with_caps) {
            vTaskDeleteWithCaps(NULL);
        } else {
            vTaskDelete(NULL);
        }
        return;
    }

    ESP_LOGI(
        UI_TAG,
        "LCD start file action: action=%s path=%s mode=%s write_path=%s",
        ui_file_action_label(request->action),
        request->path,
        (request->cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
        burner_write_path_to_str(request->write_path));

    switch (request->action) {
        case UI_FILE_ACTION_BURN_PSRAM:
        case UI_FILE_ACTION_BURN_PIPELINE:
        case UI_FILE_ACTION_BURN_DIRECT:
            err = burner_start_write_from_tf(
                request->path,
                request->cart_mode,
                request->recipe_mode,
                request->slot,
                request->write_path,
                request->erase_always,
                request->psram_mb,
                request->mbc5_chunk_kb,
                request->gba_force_no_cfi,
                request->gba_sram_patch,
                request->gba_waitcnt_patch,
                request->gbx_profile_file,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_VERIFY_ROM:
            err = burner_start_verify_from_tf(
                request->path,
                request->cart_mode,
                request->recipe_mode,
                request->slot,
                request->gbx_profile_file,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_WRITE_SAVE:
            err = burner_start_ram_write_from_tf(
                request->path,
                request->slot,
                request->ram_fram,
                request->ram_latency,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_VERIFY_SAVE:
            err = burner_start_ram_verify_from_tf(
                request->path,
                request->slot,
                request->ram_fram,
                request->ram_latency,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_WRITE_GBA_SAVE_NEW:
            err = burner_start_gba_save_write_from_tf_new(
                request->path,
                request->gba_save_type,
                request->gba_save_size,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        case UI_FILE_ACTION_VERIFY_GBA_SAVE_NEW:
            err = burner_start_gba_save_verify_from_tf_new(
                request->path,
                request->gba_save_type,
                request->gba_save_size,
                &result,
                error_msg,
                sizeof(error_msg));
            break;
        default:
            err = ESP_ERR_NOT_SUPPORTED;
            break;
    }

    ESP_LOGI(UI_TAG, "LCD file action task stack free min=%u bytes", (unsigned)uxTaskGetStackHighWaterMark2(NULL));
    ui_finish_file_start_task(request, err, &result, error_msg);
    free(request);
    if (perf_lock_acquired) {
        power_manager_perf_lock_release("ui_file_start");
        perf_lock_acquired = false;
    }
    if (task_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static void ui_start_file_action_async(ui_file_start_request_t *request)
{
    BaseType_t ret;

    if (request == NULL) {
        return;
    }

    request->task_with_caps = true;
    ret = xTaskCreatePinnedToCoreWithCaps(
        ui_start_file_action_task,
        "ui_file_start",
        UI_FILE_START_TASK_STACK_SIZE,
        request,
        UI_FILE_START_TASK_PRIORITY,
        NULL,
        UI_BURN_TASK_CORE_ID,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        return;
    }

    request->task_with_caps = false;
    ret = xTaskCreatePinnedToCore(
        ui_start_file_action_task,
        "ui_file_start",
        UI_FILE_START_TASK_STACK_SIZE,
        request,
        UI_FILE_START_TASK_PRIORITY,
        NULL,
        UI_BURN_TASK_CORE_ID);
    if (ret != pdPASS) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGW(
            UI_TAG,
            "LCD file action task create failed: ret=%d stack=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
            (int)ret,
            (unsigned)UI_FILE_START_TASK_STACK_SIZE,
            (unsigned)internal_free,
            (unsigned)internal_largest,
            (unsigned)psram_free,
            (unsigned)psram_largest);
        ui_finish_file_start_task(request, ESP_ERR_NO_MEM, NULL, "create start task failed");
        free(request);
    }
}

static void ui_set_task_page_starting(const char *text, uint32_t total, ui_page_t return_page)
{
    if (!ui_take_model_lock()) {
        return;
    }
    ui_task_cancel_confirm_reset_locked();
    s_model.page = UI_PAGE_TASK_STATUS;
    s_model.parent_page = return_page;
    s_model.burn_progress = 0;
    s_model.burn_processed = 0;
    s_model.burn_total = total;
    s_model.erase_progress = 0;
    s_model.erase_done_sectors = 0;
    s_model.erase_total_sectors = 0;
    s_model.burn_elapsed_us = 0;
    ui_set_status_locked(&s_model, text);
    xSemaphoreGive(s_model_lock);
}

static esp_err_t ui_start_dump_rom_task(void)
{
    char timestamp[32] = {0};
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    uint32_t read_size = s_rom_size_mib * 1024U * 1024U;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    uint32_t dump_chunk_bytes = burner_dump_chunk_kb_to_bytes(s_dump_chunk_kb);
    bool gba_force_multi = false;
    esp_err_t err;

    if (card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_mode == BURNER_CART_MODE_GBA && (read_size & 0x1U) != 0U) {
        read_size++;
    }
    if (s_cart_mode == BURNER_CART_MODE_GBA) {
        err = burner_apply_gba_slot_limit(s_cart_slot, read_size, &addr_begin, &effective_size, &gba_force_multi);
    } else {
        err = burner_apply_mbc5_slot_limit(false, s_cart_slot, read_size, &addr_begin, &effective_size);
    }
    if (err != ESP_OK) {
        return err;
    }
    err = burner_ensure_rom_output_dir();
    if (err != ESP_OK) {
        return err;
    }
    burner_build_output_timestamp(timestamp, sizeof(timestamp));
    snprintf(safe_name, sizeof(safe_name), "cart_%s%s", timestamp, burner_rom_dump_ext_for_mode(s_cart_mode));
    err = burner_resolve_unique_output_path(
        ROM_OUTPUT_DIR_PATH,
        safe_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }
    err = burner_start_task_ex(
        BURNER_JOB_READ_ROM,
        s_cart_mode,
        BURNER_WRITE_PATH_DIRECT,
        s_recipe_mode,
        false,
        gba_force_multi,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        dump_chunk_bytes,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        "",
        false,
        0U);
    if (err == ESP_OK) {
        ui_set_task_page_starting(ui_tr("ROM dump started"), effective_size, UI_PAGE_BURN_ROM);
    }
    return err;
}

static esp_err_t ui_start_dump_save_task(void)
{
    char timestamp[32] = {0};
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    uint32_t read_size = s_save_size_kib * 1024U;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    esp_err_t err;

    if (s_cart_mode == BURNER_CART_MODE_GBA) {
        if (card == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        if (usb_msc_tf_in_use_by_host()) {
            return ESP_ERR_INVALID_STATE;
        }
        err = burner_ensure_dump_dir();
        if (err != ESP_OK) {
            return err;
        }
        burner_build_output_timestamp(timestamp, sizeof(timestamp));
        snprintf(safe_name, sizeof(safe_name), "cart_gba_save_%s.sav", timestamp);
        err = burner_resolve_unique_output_path(
            DUMP_DIR_PATH,
            safe_name,
            safe_name,
            sizeof(safe_name),
            full_path,
            sizeof(full_path));
        if (err != ESP_OK) {
            return err;
        }
        err = burner_start_gba_save_dump_new(
            safe_name,
            full_path,
            s_gba_save_type,
            s_save_size_kib * 1024U);
        if (err == ESP_OK) {
            ui_set_task_page_starting("GBA save dump started", s_save_size_kib * 1024U, UI_PAGE_BURN_ROM);
        }
        return err;
    }
    if (s_cart_mode != BURNER_CART_MODE_MBC5) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = burner_apply_mbc5_slot_limit(true, s_cart_slot, read_size, &addr_begin, &effective_size);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_ensure_dump_dir();
    if (err != ESP_OK) {
        return err;
    }
    burner_build_output_timestamp(timestamp, sizeof(timestamp));
    snprintf(safe_name, sizeof(safe_name), "cart_save_%s.sav", timestamp);
    err = burner_resolve_unique_output_path(
        DUMP_DIR_PATH,
        safe_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path));
    if (err != ESP_OK) {
        return err;
    }
    err = burner_start_task(
        BURNER_JOB_READ_RAM,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        s_ram_fram,
        s_ram_latency);
    if (err == ESP_OK) {
        ui_set_task_page_starting(ui_tr("save dump started"), effective_size, UI_PAGE_BURN_ROM);
    }
    return err;
}

static esp_err_t ui_start_chip_erase_task(void)
{
    esp_err_t err = ESP_OK;

    if (s_cart_mode == BURNER_CART_MODE_GBA) {
        uint32_t device_size = 0u;

        err = burner_probe_cart_capacity_bytes(BURNER_CART_MODE_GBA, &device_size);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = burner_start_task_ex(
        BURNER_JOB_ERASE_ROM,
        s_cart_mode,
        BURNER_WRITE_PATH_DIRECT,
        s_recipe_mode,
        false,
        false,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        "chip_erase",
        "/cart",
        0U,
        1U,
        BURNER_GBA_SAVE_TYPE_SRAM,
        "",
        false,
        0U);

    if (err == ESP_OK) {
        ui_set_task_page_starting(ui_tr("chip erase started"), 1U, UI_PAGE_BURN_ROM);
    }
    return err;
}

static esp_err_t ui_read_cart_id_once(char *out, size_t out_len, burner_cart_mode_t cart_mode)
{
    uint8_t gba_id[8] = {0};
    uint8_t mbc5_id[4] = {0};
    char chip_name[48] = {0};
    burner_nor_geometry_t cfi_geometry = {0};
    uint32_t device_size = 0;
    uint32_t save_probe_device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    burner_gba_save_type_t gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t gba_save_size = 0;
    uint32_t batteryless_save_address = 0u;
    uint32_t batteryless_save_size = 0u;
    bool batteryless_region_found = false;
    bool batteryless_data_present = false;
    const burner_gbx_profile_t *gbx_profile = &s_cart_ctx.gbx;
    bool gbx_profile_matched = false;
    bool gba_d0d1_known = false;
    bool gba_d0d1_swapped = false;
    bool cfi_ok = false;
    burner_nor_cmdset_t mbc5_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    esp_err_t err;

    if (out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    err = burner_spi_init();
    if (err != ESP_OK) {
        return err;
    }

    burner_spi_lock_take();
    if (cart_mode == BURNER_CART_MODE_MBC5) {
        err = burner_bacon_mbc5_prepare_power();
        if (err == ESP_OK) {
            if (s_recipe_mode == BURNER_RECIPE_MODE_GBX) {
                err = burner_gbc_gbx_probe_locked(
                    mbc5_id,
                    NULL,
                    &device_size,
                    &sector_size,
                    &buffer_write_bytes,
                    &cfi_ok,
                    &mbc5_cmdset);
                gbx_profile_matched = (err == ESP_OK);
            } else {
                err = burner_bacon_mbc5_get_cfi(
                    &device_size,
                    &sector_size,
                    &buffer_write_bytes,
                    &cfi_geometry,
                    &mbc5_cmdset);
                if (err == ESP_OK) {
                    cfi_ok = true;
                    if (mbc5_cmdset == BURNER_NOR_CMDSET_AMD) {
                        err = burner_bacon_mbc5_get_id(mbc5_id);
                        if (err != ESP_OK) {
                            memset(mbc5_id, 0, sizeof(mbc5_id));
                            err = ESP_OK;
                        }
                    }
                }
            }
        }
    } else {
        err = burner_bacon_gba_prepare_power();
        if (err == ESP_OK) {
            if (s_recipe_mode == BURNER_RECIPE_MODE_GBX) {
                err = burner_gba_gbx_probe_locked(
                    gba_id,
                    NULL,
                    &device_size,
                    &sector_size,
                    &buffer_write_bytes,
                    &cfi_ok);
                gbx_profile_matched = (err == ESP_OK);
            } else if (s_recipe_mode == BURNER_RECIPE_MODE_CHISLINK) {
                err = burner_chislink_gba_probe_locked(
                    gba_id,
                    &device_size,
                    &sector_size,
                    &buffer_write_bytes,
                    &cfi_ok);
            } else {
                err = burner_bacon_gba_probe_locked(
                    gba_id,
                    &device_size,
                    &sector_size,
                    &buffer_write_bytes,
                    &cfi_ok);
            }
            burner_bacon_gba_d0d1_status(&gba_d0d1_known, &gba_d0d1_swapped);
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        return err;
    }
    if (cart_mode == BURNER_CART_MODE_GBA && s_recipe_mode == BURNER_RECIPE_MODE_GBX && gbx_profile_matched) {
        burner_gba_gbx_cache_probe_result(
            gba_id,
            device_size,
            sector_size,
            buffer_write_bytes,
            cfi_ok,
            true);
    } else {
        burner_gba_gbx_clear_cached_probe();
    }
    if (cart_mode == BURNER_CART_MODE_MBC5) {
        burner_nor_format_chip_name(
            chip_name,
            sizeof(chip_name),
            (gbx_profile_matched && gbx_profile->display_name[0] != '\0') ?
                gbx_profile->display_name :
                burner_mbc5_chip_name(mbc5_id),
            mbc5_cmdset,
            device_size);
        burner_status_set_probe_info(
            BURNER_CART_MODE_MBC5,
            mbc5_id,
            sizeof(mbc5_id),
            device_size,
            sector_size,
            buffer_write_bytes,
            cfi_ok,
            false,
            false,
            false,
            false,
            chip_name,
            gbx_profile_matched ? "GBX" : "MBC5");
        if (gbx_profile_matched) {
            snprintf(out, out_len, "%s GBX %s", ui_cart_mode_label(cart_mode), chip_name);
        } else {
            snprintf(
                out,
                out_len,
                "%s ID %02X %02X %02X %02X %s",
                ui_cart_mode_label(cart_mode),
                mbc5_id[0],
                mbc5_id[1],
                mbc5_id[2],
                mbc5_id[3],
                cfi_ok ? "CFI" : "no CFI");
        }
    } else {
        save_probe_device_size = device_size;
        if (s_cart_ctx.gba_likely_read_only) {
            snprintf(chip_name, sizeof(chip_name), "%s", ui_tr("Read-only retail ROM"));
            device_size = 0u;
            sector_size = 0u;
            buffer_write_bytes = 0u;
        } else if (gbx_profile_matched) {
            if (gbx_profile->d0d1_known) {
                gba_d0d1_known = true;
                gba_d0d1_swapped = gbx_profile->d0d1_swapped;
            }

            burner_nor_format_chip_name(
                chip_name,
                sizeof(chip_name),
                gbx_profile->display_name[0] != '\0' ? gbx_profile->display_name : burner_gba_chip_name(gba_id),
                (gbx_profile->base_cmdset != BURNER_NOR_CMDSET_UNKNOWN) ? gbx_profile->base_cmdset : s_cart_ctx.gba_cmdset,
                device_size);
            if (device_size != 0u) {
                save_probe_device_size = device_size;
            }
        } else {
            burner_nor_format_chip_name(
                chip_name,
                sizeof(chip_name),
                burner_gba_chip_name(gba_id),
                s_cart_ctx.gba_cmdset,
                device_size);
        }
        burner_status_set_probe_info(
            BURNER_CART_MODE_GBA,
            gba_id,
            sizeof(gba_id),
            device_size,
            sector_size,
            buffer_write_bytes,
            cfi_ok,
            false,
            false,
            gba_d0d1_known,
            gba_d0d1_swapped,
            chip_name,
            gbx_profile_matched ? "GBX" :
                                  ((s_recipe_mode == BURNER_RECIPE_MODE_CHISLINK) ? "CHISLINK" : ""));
        {
            bool detected = false;
            burner_spi_lock_take();
            err = burner_probe_gba_save_type_head_locked(
                save_probe_device_size,
                &gba_save_type,
                &gba_save_size,
                &detected);
            esp_err_t batteryless_err = ESP_OK;
            if (err == ESP_OK) {
                batteryless_err = burner_probe_gba_batteryless_save_locked(
                    save_probe_device_size,
                    &batteryless_save_address,
                    &batteryless_save_size,
                    &batteryless_region_found,
                    &batteryless_data_present);
            }
            burner_bacon_restore_3v3_power();
            burner_spi_lock_give();
            if (err == ESP_OK) {
                if (batteryless_err == ESP_OK && batteryless_region_found) {
                    gba_save_type = BURNER_GBA_SAVE_TYPE_BATTERYLESS;
                    gba_save_size = batteryless_save_size;
                    detected = true;
                }
                burner_status_set_gba_save_probe(gba_save_type, gba_save_size, detected);
                burner_status_set_gba_sram_patch_probe(BURNER_GBA_SRAM_PATCH_NONE, false, false);
                burner_status_set_gba_batteryless_probe(
                    batteryless_save_address,
                    batteryless_save_size,
                    batteryless_region_found,
                    batteryless_data_present);
            } else {
                burner_status_set_gba_save_probe(BURNER_GBA_SAVE_TYPE_SRAM, 0u, false);
                burner_status_set_gba_sram_patch_probe(BURNER_GBA_SRAM_PATCH_NONE, false, false);
                burner_status_set_gba_batteryless_probe(0u, 0u, false, false);
            }
        }
        if (gbx_profile_matched) {
            snprintf(out, out_len, "%s GBX %s", ui_cart_mode_label(cart_mode), chip_name);
        } else {
            snprintf(
                out,
                out_len,
                "%s ID %02X %02X %02X %02X %s",
                ui_cart_mode_label(cart_mode),
                gba_id[0],
                gba_id[1],
                gba_id[2],
                gba_id[3],
                s_cart_ctx.gba_likely_read_only ? ui_tr("Read-only retail ROM") : (cfi_ok ? "CFI" : "no CFI"));
        }
    }
    return ESP_OK;
}

static esp_err_t ui_unlock_ppb_once(char *out, size_t out_len, burner_cart_mode_t cart_mode)
{
    burner_ppb_unlock_report_t report;
    esp_err_t err;

    if (out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    err = burner_spi_init();
    if (err != ESP_OK) {
        return err;
    }

    memset(&report, 0, sizeof(report));
    burner_spi_lock_take();
    err = burner_cart_unlock_ppb_locked(cart_mode, &report);
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        return err;
    }

    if (cart_mode == BURNER_CART_MODE_MBC5) {
        burner_status_set_probe_info(
            BURNER_CART_MODE_MBC5,
            report.mbc5_id,
            sizeof(report.mbc5_id),
            report.device_size,
            report.sector_size,
            report.buffer_write_bytes,
            report.cfi_ok,
            false,
            false,
            false,
            false,
            burner_mbc5_chip_name(report.mbc5_id),
            "MBC5");
    } else {
        burner_status_set_probe_info(
            BURNER_CART_MODE_GBA,
            report.gba_id,
            sizeof(report.gba_id),
            report.device_size,
            report.sector_size,
            report.buffer_write_bytes,
            report.cfi_ok,
            false,
            false,
            report.gba_d0d1_known,
            report.gba_d0d1_swapped,
            burner_gba_chip_name(report.gba_id),
            "");
    }

    if (report.ppb_needs_unlock_before == 0U && report.ppb_needs_unlock_after == 0U) {
        snprintf(
            out,
            out_len,
            "%s",
            ui_tr("PPB already unlocked"));
    } else {
        snprintf(
            out,
            out_len,
            "%s %" PRIu32 "->%" PRIu32,
            ui_tr("PPB unlocked"),
            report.ppb_needs_unlock_before,
            report.ppb_needs_unlock_after);
    }
    return ESP_OK;
}

static void ui_finish_work_task(ui_work_type_t type, esp_err_t err, const char *ok_text, burner_cart_mode_t cart_mode)
{
    if (!ui_take_model_lock()) {
        return;
    }
    if (type == UI_WORK_WIFI_CONNECT_SAVED || type == UI_WORK_WIFI_START_AP ||
        type == UI_WORK_WIFI_DISCONNECT || type == UI_WORK_WIFI_CLOSE_AP ||
        type == UI_WORK_WIFI_CLEAR_SAVED) {
        s_wifi_work_active = false;
    } else if (type == UI_WORK_SMB_SCAN || type == UI_WORK_SMB_LOAD_FAVORITES ||
               type == UI_WORK_SMB_SAVE_FAVORITE || type == UI_WORK_SMB_CONNECT ||
               type == UI_WORK_SMB_DISCONNECT) {
        s_smb_work_active = false;
    } else if (type == UI_WORK_STORAGE_USB_ENABLE || type == UI_WORK_STORAGE_USB_DISABLE ||
               type == UI_WORK_BOOT_RETRO_GO || type == UI_WORK_UPDATE_GBX_CACHE) {
        s_storage_work_active = false;
    } else if (type == UI_WORK_BURN_READ_ID || type == UI_WORK_BURN_UNLOCK_PPB) {
        s_burn_probe_active = false;
    }
    if (type == UI_WORK_BURN_READ_ID) {
        if (err == ESP_OK) {
            s_cart_analyzed = true;
            s_analyzed_cart_mode = cart_mode;
            snprintf(s_analyzed_cart_info, sizeof(s_analyzed_cart_info), "%s", ok_text != NULL ? ok_text : "");
            s_burn_rom_write_menu = false;
            s_burn_rom_submenu = UI_BURN_ROM_SUBMENU_NONE;
            ui_focus_burn_rom_op_locked(&s_model, UI_BURN_ROM_OP_CHOOSE_ROM);
        } else {
            s_cart_analyzed = false;
            s_analyzed_cart_info[0] = '\0';
        }
    }

    if (err == ESP_OK) {
        ui_set_status_locked(&s_model, ok_text);
    } else {
        char msg[UI_STATUS_TEXT_MAX_LEN] = {0};
        snprintf(
            msg,
            sizeof(msg),
            "%s%s",
            ui_tr("action failed: "),
            esp_err_to_name(err));
        ui_set_status_locked(&s_model, msg);
    }
    xSemaphoreGive(s_model_lock);
}

static void ui_delete_async_task(bool task_with_caps)
{
    if (task_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static void ui_work_task(void *param)
{
    ui_work_request_t *request = (ui_work_request_t *)param;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *ok_text = ui_tr("done");
    char ok_buf[UI_STATUS_TEXT_MAX_LEN] = {0};
    bool task_with_caps = false;

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = request->task_with_caps;

    switch (request->type) {
        case UI_WORK_WIFI_CONNECT_SAVED:
            ok_text = ui_tr("Wi-Fi connected");
            err = wifi_maneger_ready() ? wifi_maneger_connect_saved(15000U) : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_WIFI_START_AP:
            ok_text = ui_tr("provisioning AP");
            err = wifi_maneger_ready() ? wifi_maneger_ap() : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_WIFI_DISCONNECT:
            wifi_maneger_disconnect();
            ok_text = ui_tr("Wi-Fi disconnected");
            err = ESP_OK;
            break;
        case UI_WORK_WIFI_CLOSE_AP:
            ok_text = "setup hotspot closed";
            err = wifi_maneger_provisioning_confirm();
            break;
        case UI_WORK_WIFI_CLEAR_SAVED:
            ok_text = ui_tr("saved Wi-Fi cleared");
            err = wifi_maneger_ready() ? wifi_maneger_clear_sta_config() : ESP_ERR_INVALID_STATE;
            break;
        case UI_WORK_STORAGE_USB_ENABLE:
            ok_text = ui_tr("USB pass-through enabled");
            err = usb_msc_tf_set_enabled(true);
            break;
        case UI_WORK_STORAGE_USB_DISABLE:
            ok_text = ui_tr("USB pass-through disabled");
            err = usb_msc_tf_set_enabled(false);
            break;
        case UI_WORK_BURN_ERASE_CHIP:
            ok_text = ui_tr("chip erase started");
            err = ui_start_chip_erase_task();
            break;
        case UI_WORK_BURN_DUMP_ROM:
            ok_text = ui_tr("ROM dump started");
            err = ui_start_dump_rom_task();
            break;
        case UI_WORK_BURN_DUMP_SAVE:
            ok_text = ui_tr("save dump started");
            err = ui_start_dump_save_task();
            break;
        case UI_WORK_DEVICE_RESTART:
            ok_text = ui_tr("restarting");
            burner_schedule_restart();
            err = ESP_OK;
            break;
        case UI_WORK_UPDATE_GBX_CACHE: {
            uint32_t profile_count = 0u;
            uint32_t entry_count = 0u;

            err = burner_gbx_rebuild_cache(&profile_count, &entry_count);
            if (err == ESP_OK) {
                snprintf(
                    ok_buf,
                    sizeof(ok_buf),
                    "GBX缓存 %" PRIu32 "/%" PRIu32,
                    profile_count,
                    entry_count);
                ok_text = ok_buf;
            } else {
                ok_text = "GBX缓存失败";
            }
            break;
        }
        default:
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    ui_finish_work_task(request->type, err, ok_text, request->cart_mode);
    free(request);
    ui_delete_async_task(task_with_caps);
}

static void ui_boot_rg_task(void *param)
{
    ui_work_request_t *request = (ui_work_request_t *)param;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    const char *ok_text = ui_tr("booting Retro-Go");
    bool task_with_caps = false;

    if (request == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = request->task_with_caps;

    /*
     * esp_ota_set_boot_partition() validates the target image through flash/cache
     * paths that require the current task stack to remain in internal DRAM.
     */
    err = boot_partition_switch_to("launcher");

    ui_finish_work_task(request->type, err, ok_text, request->cart_mode);
    free(request);
    ui_delete_async_task(task_with_caps);
}


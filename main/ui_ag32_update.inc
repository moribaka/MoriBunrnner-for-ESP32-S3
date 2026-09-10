/* Native AG32 update page. Validation runs off the UI thread; programming and
 * progress use the same backend as the web/serial interfaces. */
typedef struct {
    char path[sizeof(((ag32_batch_job_status_t *)0)->path)];
    bool start;
} ui_ag32_request_t;
static ui_ag32_request_t s_ui_ag32_request;
static bool s_ui_ag32_pending;

static bool ui_ag32_busy(const ui_model_t *model)
{
    return model->ag32.phase == UI_AG32_CHECKING ||
           model->ag32.phase == UI_AG32_STARTING ||
           model->ag32.phase == UI_AG32_RUNNING;
}

static void ui_ag32_worker(void *arg)
{
    ui_ag32_request_t *request = arg;
    uint32_t records = 0, bytes = 0;
    char error[160] = {0};
    esp_err_t err;
    if (request->start) {
        err = ag32_batch_program_start(request->path);
    } else {
        err = ag32_batch_validate_path(request->path, &records, &bytes, error, sizeof(error));
    }
    if (ui_take_model_lock()) {
        if (err == ESP_OK) {
            /* Polling may already have observed this job's terminal state. */
            if (!request->start) s_model.ag32.phase = UI_AG32_READY;
            else if (s_model.ag32.phase == UI_AG32_STARTING) s_model.ag32.phase = UI_AG32_RUNNING;
            if (!request->start) {
                s_model.ag32.status.report.record_count = records;
                s_model.ag32.status.total = bytes;
                s_model.selected = 4;
            }
            if (!request->start || s_model.ag32.phase == UI_AG32_RUNNING)
                ui_set_status_locked(&s_model, ui_tr(request->start ?
                    "AG32 busy - keep power on" : "AG32 batch valid"));
        } else {
            s_model.ag32.phase = UI_AG32_ERROR;
            snprintf(s_model.ag32.status.message, sizeof(s_model.ag32.status.message), "%s",
                     error[0] ? error : esp_err_to_name(err));
            ui_set_status_locked(&s_model, s_model.ag32.status.message);
        }
        ui_mark_content_dirty(&s_model);
        xSemaphoreGive(s_model_lock);
    }
}

static void ui_ag32_launch_locked(ui_model_t *model, bool start)
{
    music_player_snapshot_t music;
    music_player_get_snapshot(&music);
    if (card == NULL || usb_msc_tf_in_use_by_host() || burner_task_is_running_snapshot() ||
        s_file_start_active || s_storage_work_active ||
        s_burn_probe_active || music.state == MUSIC_PLAYER_STATE_LOADING ||
        music.state == MUSIC_PLAYER_STATE_PLAYING) {
        ui_set_status_locked(model, ui_tr("AG32 requires idle TF"));
        return;
    }
    ui_ag32_request_t *request = &s_ui_ag32_request;
    s_file_scan_generation++;
    s_file_scan_pending = false;
    s_gba_patch_analysis_active = false;
    snprintf(request->path, sizeof(request->path), "%s", model->ag32.status.path);
    request->start = start;
    model->ag32.phase = start ? UI_AG32_STARTING : UI_AG32_CHECKING;
    ui_set_status_locked(model, ui_tr(start ? "AG32 busy - keep power on" : "checking AG32 batch"));
    s_ui_ag32_pending = true;
    xTaskNotifyGive(s_file_worker);
    ui_mark_content_dirty(model);
}

static void ui_ag32_select_file_locked(ui_model_t *model, const ui_file_entry_t *entry)
{
    memset(&model->ag32, 0, sizeof(model->ag32));
    model->page = UI_PAGE_AG32_UPDATE;
    model->parent_page = UI_PAGE_SETTINGS;
    model->selected = 0;
    model->scroll = 0;
    model->file_filter = UI_FILE_FILTER_NONE;
    ui_drop_nav_target_locked(UI_PAGE_AG32_UPDATE);
    if (!burner_build_full_path(entry->path, model->ag32.status.path, sizeof(model->ag32.status.path))) {
        model->ag32.phase = UI_AG32_ERROR;
        snprintf(model->ag32.status.message, sizeof(model->ag32.status.message), "%s", ui_tr("invalid batch path"));
        ui_set_status_locked(model, model->ag32.status.message);
    } else {
        ui_ag32_launch_locked(model, false);
    }
    model->dirty = true;
}

static void ui_ag32_select_locked(ui_model_t *model)
{
    if (model->ag32.phase == UI_AG32_CONFIRM) {
        if (model->selected == 0) {
            model->ag32.phase = UI_AG32_READY;
            model->selected = 4;
            ui_set_status_locked(model, ui_tr("AG32 update cancelled"));
        } else if (model->selected == 1) {
            model->selected = 0;
            ui_ag32_launch_locked(model, true);
        }
    } else if (model->selected == 0) {
        if (card == NULL || usb_msc_tf_in_use_by_host()) {
            ui_set_status_locked(model, ui_tr("AG32 requires idle TF"));
            return;
        }
        ui_push_current_page_locked(model);
        model->parent_page = UI_PAGE_AG32_UPDATE;
        ui_open_files_with_filter_locked(model, "", true, UI_FILE_FILTER_BIN);
        ui_set_status_locked(model, ui_tr("choose AG32 batch from TF"));
    } else if (model->selected == 4 && model->ag32.phase == UI_AG32_READY) {
        model->ag32.phase = UI_AG32_CONFIRM;
        model->selected = 0; // Explicit confirmation, initially focused on cancel.
        model->scroll = 0;
        ui_set_status_locked(model, ui_tr("AG32 erases MCU and CPLD"));
    }
    ui_mark_content_dirty(model);
}

static void ui_ag32_poll(void)
{
    static uint32_t last_ms;
    uint32_t now = esp_log_timestamp();
    if (now - last_ms < 250U) return;
    last_ms = now;
    ag32_batch_job_status_t status;
    ag32_batch_program_status(&status);
    if (status.state == AG32_BATCH_JOB_RUNNING) ui_mark_activity();
    if (!ui_take_model_lock()) return;
    if (status.state == AG32_BATCH_JOB_RUNNING || s_model.ag32.phase == UI_AG32_RUNNING) {
        if (status.state == AG32_BATCH_JOB_RUNNING && s_model.page != UI_PAGE_AG32_UPDATE) {
            ui_open_page_locked(&s_model, UI_PAGE_AG32_UPDATE);
        }
        if (memcmp(&s_model.ag32.status, &status, sizeof(status)) != 0) {
            s_model.ag32.status = status;
            if (status.state == AG32_BATCH_JOB_SUCCESS) {
                s_model.ag32.phase = UI_AG32_SUCCESS;
                ui_set_status_locked(&s_model, ui_tr("AG32 update complete"));
            } else if (status.state == AG32_BATCH_JOB_FAILED) {
                s_model.ag32.phase = UI_AG32_ERROR;
                ui_set_status_locked(&s_model, status.message);
            } else {
                s_model.ag32.phase = UI_AG32_RUNNING;
                ui_set_status_locked(&s_model, ui_tr("AG32 busy - keep power on"));
            }
            ui_mark_content_dirty(&s_model);
        }
    }
    xSemaphoreGive(s_model_lock);
}

static const char *ui_ag32_phase_text(const ui_model_t *model)
{
    switch (model->ag32.phase) {
        case UI_AG32_EMPTY: return ui_tr("choose AG32 batch from TF");
        case UI_AG32_CHECKING: return ui_tr("checking AG32 batch");
        case UI_AG32_READY: return ui_tr("AG32 batch valid");
        case UI_AG32_STARTING: return ui_tr("starting AG32 update");
        case UI_AG32_SUCCESS: return ui_tr("AG32 update complete");
        case UI_AG32_ERROR: return ui_tr("AG32 update failed");
        default: break;
    }
    const char *phase = model->ag32.status.phase;
    if (!strcmp(phase, "erase")) return ui_tr("erasing AG32");
    if (!strcmp(phase, "program")) return ui_tr("programming AG32");
    if (!strcmp(phase, "verify")) return ui_tr("verifying AG32");
    if (!strcmp(phase, "connect")) return ui_tr("connecting AG32");
    return ui_tr("checking AG32 batch");
}

static void ui_fill_ag32_row(const ui_model_t *model, uint16_t index,
                             char *title, size_t title_len, char *hint, size_t hint_len)
{
    if (title_len) title[0] = '\0';
    if (hint_len) hint[0] = '\0';
    const ag32_batch_job_status_t *s = &model->ag32.status;
    const char *name = strrchr(s->path, '/');
    name = name ? name + 1 : s->path;
    if (model->ag32.phase == UI_AG32_CONFIRM) {
        if (index == 0) snprintf(title, title_len, "%s", ui_tr("Cancel"));
        else if (index == 1) snprintf(title, title_len, "%s", ui_tr("Confirm AG32 update"));
        else if (index == 2) ui_fs_name_to_display_text(title, title_len, name);
        else snprintf(title, title_len, "%s", ui_tr("AG32 erases MCU and CPLD"));
        return;
    }
    switch (index) {
        case 0:
            snprintf(title, title_len, "%s", ui_ag32_busy(model) ? ui_ag32_phase_text(model) : ui_tr("Choose AG32 batch"));
            if (ui_ag32_busy(model) && s->total) snprintf(hint, hint_len, "%u%%",
                (unsigned)((uint64_t)s->processed * 100 / s->total));
            break;
        case 1: ui_fs_name_to_display_text(title, title_len, name[0] ? name : "--"); break;
        case 2:
            snprintf(title, title_len, "%s", ui_tr("Batch records"));
            snprintf(hint, hint_len, "%" PRIu32, s->report.record_count);
            break;
        case 3:
            snprintf(title, title_len, "%s", ui_tr("Payload bytes"));
            snprintf(hint, hint_len, "%" PRIu32 " / %" PRIu32, s->processed, s->total);
            break;
        case 4:
            snprintf(title, title_len, "%s", model->ag32.phase == UI_AG32_READY ?
                ui_tr("Start AG32 update") : ui_ag32_phase_text(model));
            break;
        case 5:
            snprintf(title, title_len, "%s", ui_tr("Programmed bytes"));
            snprintf(hint, hint_len, "%" PRIu32, s->report.programmed_bytes);
            break;
        case 6:
            snprintf(title, title_len, "%s", ui_tr("Verified bytes"));
            snprintf(hint, hint_len, "%" PRIu32, s->report.verified_bytes);
            break;
    }
}

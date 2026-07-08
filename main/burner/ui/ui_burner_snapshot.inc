static void ui_update_burn_snapshot_if_needed(uint32_t now_ms)
{
    burner_status_t status = {0};
    int progress;
    int erase_progress = 0;
    uint32_t erase_done = 0;
    uint32_t erase_total = 0;
    bool release_start = false;

    if (s_last_live_refresh_ms != 0U && (now_ms - s_last_live_refresh_ms) < UI_LIVE_REFRESH_MS) {
        return;
    }
    s_last_live_refresh_ms = now_ms;

    burner_status_snapshot(&status);
    ui_calc_burn_snapshot_fields(
        &status,
        &progress,
        &erase_progress,
        &status.processed_bytes,
        &status.total_bytes,
        &erase_done,
        &erase_total);
    if (status.state == BURNER_STATE_DONE || status.state == BURNER_STATE_ERROR ||
        status.state == BURNER_STATE_CANCELLED) {
        release_start = true;
    }

    if (!ui_take_model_lock()) {
        return;
    }
    if (s_task_result_capture_armed && burner_status_is_operation_active_state(status.state)) {
        s_task_result_active_seen = true;
    }
    if (!s_file_start_active &&
        burner_status_is_operation_active_state(status.state) &&
        (s_model.page == UI_PAGE_BURNER || s_model.page == UI_PAGE_BURN_ROM || s_model.page == UI_PAGE_BURN_SAVE)) {
        ui_present_active_burn_task_locked(
            &s_model,
            &status,
            ui_burn_task_entry_return_page(s_model.page));
    }
    if (s_model.burn_progress != progress ||
        s_model.burn_processed != status.processed_bytes ||
        s_model.burn_total != status.total_bytes ||
        s_model.erase_progress != erase_progress ||
        s_model.erase_done_sectors != erase_done ||
        s_model.erase_total_sectors != erase_total ||
        s_model.burn_elapsed_us != status.task_elapsed_us) {
        s_model.burn_progress = progress;
        s_model.burn_processed = status.processed_bytes;
        s_model.burn_total = status.total_bytes;
        s_model.erase_progress = erase_progress;
        s_model.erase_done_sectors = erase_done;
        s_model.erase_total_sectors = erase_total;
        s_model.burn_elapsed_us = status.task_elapsed_us;
        ui_mark_content_dirty(&s_model);
    }
    if (release_start) {
        s_task_cancel_confirm = false;
        s_task_cancel_request_pending = false;
        ui_capture_task_result_locked(&s_model, &status);
        if (s_task_cancel_exit_pending &&
            s_model.page == UI_PAGE_TASK_STATUS &&
            (status.state == BURNER_STATE_CANCELLED || status.state == BURNER_STATE_ERROR)) {
            s_model.page = s_task_cancel_return_page;
            s_model.parent_page = ui_task_return_parent_page(s_task_cancel_return_page);
            s_model.selected = 0;
            s_model.scroll = 0;
            s_task_cancel_exit_pending = false;
            ui_mark_chrome_dirty(&s_model);
            ui_mark_content_dirty(&s_model);
            s_model.dirty = true;
        } else if (status.state == BURNER_STATE_DONE || status.state == BURNER_STATE_CANCELLED ||
                   status.state == BURNER_STATE_ERROR) {
            if (s_model.page == UI_PAGE_TASK_STATUS) {
                s_model.page = UI_PAGE_TASK_RESULT;
                s_model.selected = 0;
                s_model.scroll = 0;
                ui_mark_chrome_dirty(&s_model);
                ui_mark_content_dirty(&s_model);
                s_model.dirty = true;
            }
            s_task_cancel_exit_pending = false;
        }
    }
    if (s_model.page == UI_PAGE_TASK_STATUS) {
        ui_mark_content_dirty(&s_model);
    }
    xSemaphoreGive(s_model_lock);

    if (release_start) {
        s_file_start_active = false;
    }
}


static void ui_music_set_current_locked(const ui_file_entry_t *entry)
{
    if (entry == NULL) {
        s_music_ctx.valid = false;
        memset(&s_music_ctx.current, 0, sizeof(s_music_ctx.current));
        return;
    }
    s_music_ctx.valid = true;
    s_music_ctx.current = *entry;
}

static void ui_music_save_history_snapshot(const music_player_snapshot_t *snap)
{
    FILE *fp = NULL;
    uint32_t now_ms;
    uint32_t delta;
    bool same_path;

    if (snap == NULL || snap->source != MUSIC_PLAYER_SOURCE_TF || snap->path[0] == '\0') {
        return;
    }
    if (snap->state != MUSIC_PLAYER_STATE_PLAYING &&
        snap->state != MUSIC_PLAYER_STATE_PAUSED &&
        snap->state != MUSIC_PLAYER_STATE_FINISHED) {
        return;
    }

    now_ms = esp_log_timestamp();
    same_path = strcmp(s_music_history_saved_path, snap->path) == 0;
    delta = (snap->position > s_music_history_saved_position) ?
                (snap->position - s_music_history_saved_position) :
                (s_music_history_saved_position - snap->position);
    if (same_path &&
        snap->state == s_music_history_saved_state &&
        delta < UI_MUSIC_HISTORY_SAVE_DELTA_BYTES &&
        (now_ms - s_music_history_last_save_ms) < UI_MUSIC_HISTORY_SAVE_MS) {
        return;
    }
    if (same_path &&
        snap->state == s_music_history_saved_state &&
        snap->position == s_music_history_saved_position) {
        return;
    }

    fp = fopen(UI_MUSIC_HISTORY_PATH, "wb");
    if (fp == NULL) {
        return;
    }

    (void)fprintf(fp, "path=%s\nposition=%" PRIu32 "\n", snap->path, snap->position);
    fclose(fp);
    snprintf(s_music_history_saved_path, sizeof(s_music_history_saved_path), "%s", snap->path);
    s_music_history_saved_position = snap->position;
    s_music_history_saved_state = snap->state;
    s_music_history_last_save_ms = now_ms;
}

static bool ui_music_load_history(char *path, size_t path_len, uint32_t *position_out)
{
    FILE *fp = NULL;
    char line[320];
    bool found_path = false;
    uint32_t position = 0U;

    if (path == NULL || path_len == 0U) {
        return false;
    }

    path[0] = '\0';
    if (position_out != NULL) {
        *position_out = 0U;
    }

    fp = fopen(UI_MUSIC_HISTORY_PATH, "rb");
    if (fp == NULL) {
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq = strchr(line, '=');
        char *value = NULL;

        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';

        if (strcmp(line, "path") == 0) {
            snprintf(path, path_len, "%s", value);
            found_path = (path[0] != '\0');
        } else if (strcmp(line, "position") == 0) {
            char *endptr = NULL;
            unsigned long parsed = strtoul(value, &endptr, 10);

            if (endptr != value && endptr != NULL && *endptr == '\0' && parsed <= UINT32_MAX) {
                position = (uint32_t)parsed;
            }
        }
    }

    fclose(fp);
    if (position_out != NULL) {
        *position_out = position;
    }
    return found_path;
}

static uint32_t ui_music_resume_position_for_entry(const ui_file_entry_t *entry)
{
    char saved_path[TF_PATH_LEN_MAX] = {0};
    uint32_t saved_position = 0U;

    if (entry == NULL || entry->path[0] == '\0' || entry->size == 0U) {
        return 0U;
    }
    if (!ui_music_load_history(saved_path, sizeof(saved_path), &saved_position) ||
        strcmp(saved_path, entry->path) != 0) {
        return 0U;
    }
    if (saved_position >= entry->size) {
        return 0U;
    }
    if (entry->size > UI_MUSIC_RESUME_TAIL_GUARD_BYTES &&
        saved_position + UI_MUSIC_RESUME_TAIL_GUARD_BYTES >= entry->size) {
        return 0U;
    }
    if (((uint64_t)saved_position * 100ULL) / (uint64_t)entry->size >= UI_MUSIC_RESUME_MAX_PERCENT) {
        return 0U;
    }
    return saved_position;
}

static bool ui_music_select_path_locked(ui_model_t *model, const char *path)
{
    ui_file_entry_t entry = {0};

    if (model == NULL || path == NULL || path[0] == '\0') {
        return false;
    }
    ui_scan_file_window_locked(model);
    for (uint16_t ordinal = 0; ordinal < model->file_total; ++ordinal) {
        model->file_selected = ordinal;
        model->file_scroll =
            ui_file_scroll_for_selected(model, model->file_selected, model->file_scroll, model->file_total);
        ui_file_ensure_window_locked(model, false);
        if (ui_current_file_locked(model, &entry) && strcmp(entry.path, path) == 0) {
            ui_mark_content_dirty(model);
            return true;
        }
    }
    return false;
}

static bool ui_music_restore_history_locked(ui_model_t *model)
{
    char saved_path[TF_PATH_LEN_MAX] = {0};
    char parent[TF_PATH_LEN_MAX] = {0};
    uint32_t saved_position = 0U;

    if (model == NULL) {
        return false;
    }
    if (!s_music_ctx.valid || s_music_ctx.current.path[0] == '\0') {
        if (!ui_music_load_history(saved_path, sizeof(saved_path), &saved_position)) {
            return false;
        }
        s_music_ctx.valid = true;
        s_music_ctx.current.is_dir = false;
        s_music_ctx.current.ordinal = 0U;
        s_music_ctx.current.size = 0U;
        snprintf(s_music_ctx.current.path, sizeof(s_music_ctx.current.path), "%s", saved_path);
        {
            const char *base = strrchr(s_music_ctx.current.path, '/');
            size_t base_len;
            if (base != NULL && base[1] != '\0') {
                base++;
            } else {
                base = s_music_ctx.current.path;
            }
            base_len = strnlen(base, sizeof(s_music_ctx.current.name) - 1U);
            memcpy(s_music_ctx.current.name, base, base_len);
            s_music_ctx.current.name[base_len] = '\0';
        }
    }
    if (!ui_file_parent_path(s_music_ctx.current.path, parent, sizeof(parent))) {
        parent[0] = '\0';
    }
    if (parent[0] == '\0') {
        snprintf(parent, sizeof(parent), "%s", UI_MUSIC_DIR);
    }
    ui_open_files_locked(model, parent, true);
    return ui_music_select_path_locked(model, s_music_ctx.current.path);
}

static void ui_music_set_drawer_open_locked(ui_model_t *model, bool open)
{
    float target_x = open ? 0.0f : (float)UI_MUSIC_DRAWER_X_CLOSED;

    s_music_ctx.drawer_open = open;
    s_anim.music_drawer_target_x = target_x;
    if (model != NULL) {
        ui_mark_motion_dirty(model);
        ui_mark_content_dirty(model);
    }
}

static bool ui_music_seek_relative_locked(ui_model_t *model, int32_t delta_bytes)
{
    esp_err_t err;
    music_player_snapshot_t snap = {0};

    if (model == NULL) {
        return false;
    }
    err = music_player_seek_relative(delta_bytes);
    if (err != ESP_OK) {
        return false;
    }
    music_player_get_snapshot(&snap);
    if (delta_bytes > 0) {
        ui_set_status_locked(model, ui_tr("fast forward"));
    } else {
        ui_set_status_locked(model, ui_tr("rewind"));
    }
    ui_mark_content_dirty(model);
    return true;
}

static bool ui_music_toggle_or_play_current_locked(ui_model_t *model)
{
    music_player_snapshot_t snap = {0};
    ui_file_entry_t entry = {0};
    bool have_entry = false;
    bool use_cached_entry = false;

    if (model == NULL) {
        return false;
    }

    music_player_get_snapshot(&snap);
    if (snap.path[0] != '\0' &&
        (snap.state == MUSIC_PLAYER_STATE_PLAYING || snap.state == MUSIC_PLAYER_STATE_PAUSED)) {
        if (music_player_toggle_pause() != ESP_OK) {
            ui_set_status_locked(model, ui_tr("playback start failed"));
            return false;
        }
        ui_set_status_locked(
            model,
            (snap.state == MUSIC_PLAYER_STATE_PLAYING) ? ui_tr("paused") : ui_tr("playing"));
        ui_mark_content_dirty(model);
        return true;
    }

    have_entry = ui_current_file_locked(model, &entry);
    if (!have_entry && s_music_ctx.current.path[0] != '\0') {
        entry = s_music_ctx.current;
        have_entry = true;
        use_cached_entry = true;
    }
    if (!have_entry) {
        ui_set_status_locked(model, ui_tr("no file selected"));
        return false;
    }
    if (use_cached_entry) {
        uint32_t start_position = 0U;
        esp_err_t err;

        if (entry.is_dir || ui_file_kind_from_name(entry.name) != UI_FILE_KIND_AUDIO) {
            ui_set_status_locked(model, ui_tr("unsupported file"));
            return false;
        }
        start_position = ui_music_resume_position_for_entry(&entry);
        ESP_LOGI(
            UI_TAG,
            "music play cached: path=%s size=%" PRIu32 " pos=%" PRIu32,
            entry.path,
            entry.size,
            start_position);
        if (start_position > 0U) {
            err = music_player_play_from_position(entry.path, entry.size, start_position);
        } else {
            err = music_player_play(entry.path, entry.size);
        }
        if (err != ESP_OK) {
            ESP_LOGW(UI_TAG, "music play cached failed: path=%s err=%s", entry.path, esp_err_to_name(err));
            ui_set_status_locked(model, ui_tr("playback start failed"));
            return false;
        }
        ui_music_set_current_locked(&entry);
        ui_music_set_drawer_open_locked(model, false);
        model->page = UI_PAGE_MUSIC_PLAYER;
        model->parent_page = UI_PAGE_ROOT;
        ui_set_status_locked(model, entry.name);
        ui_mark_chrome_dirty(model);
        ui_mark_content_dirty(model);
        return true;
    }

    return ui_music_play_selected_locked(model);
}

static void ui_music_request_toggle_locked(ui_model_t *model)
{
    uint32_t now_ms = esp_log_timestamp();

    if (model == NULL) {
        return;
    }
    if (s_music_toggle_pending && (int32_t)(s_music_toggle_due_ms - now_ms) > 0) {
        s_music_toggle_pending = false;
        s_music_toggle_due_ms = 0U;
        ui_mark_content_dirty(model);
        return;
    }
    s_music_toggle_pending = true;
    s_music_toggle_due_ms = now_ms + UI_MUSIC_TOGGLE_DEFER_MS;
    ui_mark_content_dirty(model);
}

static void ui_music_process_pending_toggle_locked(ui_model_t *model)
{
    uint32_t now_ms;

    if (model == NULL || !s_music_toggle_pending) {
        return;
    }
    now_ms = esp_log_timestamp();
    if ((int32_t)(now_ms - s_music_toggle_due_ms) < 0) {
        return;
    }
    s_music_toggle_pending = false;
    s_music_toggle_due_ms = 0U;
    if (model->page == UI_PAGE_MUSIC_PLAYER) {
        (void)ui_music_toggle_or_play_current_locked(model);
    }
}

static int32_t ui_music_loop_marquee_offset(int32_t text_w, int32_t view_w, uint32_t phase)
{
    uint32_t tick;
    uint32_t cycle_steps;
    uint32_t step;
    int32_t overflow;

    if (text_w <= view_w) {
        return 0;
    }
    overflow = text_w - view_w;
    cycle_steps = (uint32_t)overflow + (uint32_t)(UI_FILE_MARQUEE_END_PAUSE_STEPS * 2);
    if (cycle_steps == 0U) {
        return 0;
    }
    tick = (esp_log_timestamp() / UI_FILE_MARQUEE_STEP_MS) + phase;
    step = tick % cycle_steps;
    if (step < UI_FILE_MARQUEE_END_PAUSE_STEPS) {
        return 0;
    }
    step -= UI_FILE_MARQUEE_END_PAUSE_STEPS;
    if (step >= (uint32_t)overflow) {
        return overflow;
    }
    return (int32_t)step;
}

static int32_t ui_music_drawer_visible_w(int32_t drawer_x)
{
    if (drawer_x <= UI_MUSIC_DRAWER_X_CLOSED) {
        return 0;
    }
    if (drawer_x >= 0) {
        return UI_MUSIC_DRAWER_W;
    }
    return drawer_x + UI_MUSIC_DRAWER_W;
}

static bool ui_music_header_marquee_active(const ui_model_t *model)
{
    music_player_snapshot_t snap = {0};
    char header[UI_ROW_TEXT_MAX_LEN] = {0};
    bool pause_icon = false;
    uint32_t percent = 0U;

    if (model == NULL || model->page != UI_PAGE_MUSIC_PLAYER) {
        return false;
    }

    ui_music_player_view_state(model, &snap, header, sizeof(header), &pause_icon, &percent);
    return ui_px_text_width(header) > (UI_CANVAS_W - 40);
}

static bool ui_music_drawer_marquee_active(const ui_model_t *model)
{
    ui_file_entry_t entry = {0};
    int32_t text_w = UI_MUSIC_DRAWER_W - 28;

    if (model == NULL || model->page != UI_PAGE_MUSIC_PLAYER || !s_music_ctx.drawer_open) {
        return false;
    }
    if (ui_music_drawer_visible_w((int32_t)s_anim.music_drawer_x) <= 0) {
        return false;
    }
    if (model->file_total == 0U ||
        model->file_selected < model->file_scroll ||
        model->file_selected >= model->file_scroll + UI_MUSIC_DRAWER_VISIBLE_ROWS) {
        return false;
    }
    if (!ui_current_file_locked(model, &entry)) {
        return false;
    }
    return ui_px_text_width(entry.name) > text_w;
}

static bool ui_music_snapshot_player_changed(
    const music_player_snapshot_t *prev,
    const music_player_snapshot_t *next)
{
    if (prev == NULL || next == NULL) {
        return true;
    }
    return prev->state != next->state ||
           prev->file_size != next->file_size ||
           prev->bitrate != next->bitrate ||
           prev->volume_percent != next->volume_percent ||
           strcmp(prev->message, next->message) != 0 ||
           strcmp(prev->path, next->path) != 0 ||
           strcmp(prev->name, next->name) != 0;
}

static bool ui_music_snapshot_progress_changed(
    const music_player_snapshot_t *prev,
    const music_player_snapshot_t *next)
{
    if (prev == NULL || next == NULL) {
        return true;
    }
    return prev->position != next->position ||
           prev->elapsed_ms != next->elapsed_ms ||
           prev->duration_ms != next->duration_ms ||
           prev->file_size != next->file_size ||
           prev->bitrate != next->bitrate;
}

static void ui_music_player_view_state(
    const ui_model_t *model,
    music_player_snapshot_t *snap_out,
    char *header,
    size_t header_len,
    bool *pause_icon_out,
    uint32_t *percent_out)
{
    music_player_snapshot_t snap = {0};
    ui_file_entry_t selected_entry = {0};
    uint32_t percent = 0U;
    bool pause_icon = false;

    if (header != NULL && header_len > 0U) {
        header[0] = '\0';
    }
    if (snap_out == NULL || header == NULL || header_len == 0U || pause_icon_out == NULL || percent_out == NULL) {
        return;
    }

    music_player_get_snapshot(&snap);
    if (snap.file_size != 0U) {
        percent = (uint32_t)(((uint64_t)snap.position * 100ULL) / (uint64_t)snap.file_size);
        if (percent > 100U) {
            percent = 100U;
        }
    }

    if (snap.name[0] != '\0') {
        snprintf(header, header_len, "%.*s", (int)(header_len - 1U), snap.name);
    } else if (model != NULL && ui_current_file_locked(model, &selected_entry)) {
        snprintf(header, header_len, "%.*s", (int)(header_len - 1U), selected_entry.name);
    } else {
        snprintf(header, header_len, "%s", ui_tr("no file selected"));
    }

    switch (snap.state) {
        case MUSIC_PLAYER_STATE_PLAYING:
            pause_icon = true;
            break;
        case MUSIC_PLAYER_STATE_LOADING:
        case MUSIC_PLAYER_STATE_PAUSED:
        case MUSIC_PLAYER_STATE_ERROR:
        case MUSIC_PLAYER_STATE_FINISHED:
        case MUSIC_PLAYER_STATE_IDLE:
        default:
            pause_icon = false;
            break;
    }

    *snap_out = snap;
    *pause_icon_out = pause_icon;
    *percent_out = percent;
}

static void ui_px_draw_music_header_line(const char *header)
{
    int32_t header_view_w = UI_CANVAS_W - 40;
    int32_t header_w;
    int32_t header_offset;

    if (header == NULL) {
        return;
    }
    header_w = ui_px_text_width(header);
    if (header_w <= header_view_w) {
        ui_px_text((UI_CANVAS_W - header_w) / 2, UI_MUSIC_INFO_Y, header, true);
        return;
    }
    header_offset = ui_music_loop_marquee_offset(header_w, header_view_w, 0U);
    ui_px_text_clipped_offset(20, UI_MUSIC_INFO_Y, header_view_w, header, header_offset, true);
}

static void ui_music_format_seconds(uint32_t seconds, char *out, size_t out_len)
{
    uint32_t hours;
    uint32_t minutes;

    if (out == NULL || out_len == 0U) {
        return;
    }
    hours = seconds / 3600U;
    minutes = (seconds / 60U) % 60U;
    if (hours > 0U) {
        snprintf(out, out_len, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, hours, minutes, seconds % 60U);
    } else {
        snprintf(out, out_len, "%02" PRIu32 ":%02" PRIu32, minutes, seconds % 60U);
    }
}

static bool ui_music_snapshot_times(
    const music_player_snapshot_t *snap,
    uint32_t *current_seconds_out,
    uint32_t *total_seconds_out)
{
    if (current_seconds_out != NULL) {
        *current_seconds_out = 0U;
    }
    if (total_seconds_out != NULL) {
        *total_seconds_out = 0U;
    }
    if (snap == NULL) {
        return false;
    }
    if (current_seconds_out != NULL) {
        *current_seconds_out = (snap->elapsed_ms + 500U) / 1000U;
    }
    if (total_seconds_out != NULL) {
        *total_seconds_out = (snap->duration_ms + 500U) / 1000U;
    }
    return snap->duration_ms > 0U;
}

static void ui_px_draw_music_time_line(const music_player_snapshot_t *snap)
{
    char current_text[16] = "--:--";
    char total_text[16] = "--:--";
    uint32_t current_seconds = 0U;
    uint32_t total_seconds = 0U;
    int32_t total_w;

    if (snap != NULL) {
        bool have_total = ui_music_snapshot_times(snap, &current_seconds, &total_seconds);

        ui_music_format_seconds(current_seconds, current_text, sizeof(current_text));
        if (have_total) {
            ui_music_format_seconds(total_seconds, total_text, sizeof(total_text));
        }
    }
    total_w = ui_px_text_width(total_text);
    ui_px_text(UI_MUSIC_SEEK_BAR_X, UI_MUSIC_TIME_Y, current_text, true);
    ui_px_text(UI_MUSIC_SEEK_BAR_X + UI_MUSIC_SEEK_BAR_W - total_w, UI_MUSIC_TIME_Y, total_text, true);
}

static void ui_px_draw_music_volume_line(const music_player_snapshot_t *snap)
{
    char volume_text[32] = {0};
    int32_t volume_w;

    snprintf(
        volume_text,
        sizeof(volume_text),
        "%s %u%%",
        ui_tr("Volume"),
        (unsigned)((snap != NULL) ? snap->volume_percent : 0U));
    volume_w = ui_px_text_width(volume_text);
    ui_px_text((UI_CANVAS_W - volume_w) / 2, UI_MUSIC_VOLUME_Y, volume_text, true);
}

static void ui_px_draw_music_seek_bar(uint32_t percent)
{
    int32_t fill_w = (int32_t)(((uint32_t)UI_MUSIC_SEEK_BAR_W * percent) / 100U);

    ui_px_frame(UI_MUSIC_SEEK_BAR_X, UI_MUSIC_SEEK_BAR_Y, UI_MUSIC_SEEK_BAR_W, 10, true);
    if (fill_w > 0) {
        ui_px_box(UI_MUSIC_SEEK_BAR_X + 1, UI_MUSIC_SEEK_BAR_Y + 1, fill_w - (fill_w > 1 ? 1 : 0), 8, true);
    }
}

static void ui_px_draw_music_controls(bool pause_icon)
{
    int32_t main_btn_x = (UI_CANVAS_W - UI_MUSIC_CTRL_MAIN_SIZE) / 2;
    int32_t main_btn_y = UI_MUSIC_CTRL_Y;
    int32_t side_btn_y = UI_MUSIC_CTRL_Y + ((UI_MUSIC_CTRL_MAIN_SIZE - UI_MUSIC_CTRL_SIDE_SIZE) / 2);
    int32_t left_btn_x = main_btn_x - 18 - UI_MUSIC_CTRL_SIDE_SIZE;
    int32_t right_btn_x = main_btn_x + UI_MUSIC_CTRL_MAIN_SIZE + 18;

    ui_px_draw_music_button_prev(left_btn_x, side_btn_y, UI_MUSIC_CTRL_SIDE_SIZE);
    ui_px_draw_music_button_play_pause(main_btn_x, main_btn_y, UI_MUSIC_CTRL_MAIN_SIZE, pause_icon);
    ui_px_draw_music_button_next(right_btn_x, side_btn_y, UI_MUSIC_CTRL_SIDE_SIZE);
}

static void ui_px_draw_music_drawer(const ui_model_t *model, const music_player_snapshot_t *snap)
{
    if (model == NULL || snap == NULL) {
        return;
    }

    if ((int32_t)s_anim.music_drawer_x > UI_MUSIC_DRAWER_X_CLOSED) {
        int32_t drawer_x = (int32_t)s_anim.music_drawer_x;
        int32_t drawer_y = UI_MUSIC_DRAWER_Y;
        int32_t drawer_w = UI_MUSIC_DRAWER_W;
        int32_t drawer_h = UI_MUSIC_DRAWER_H;
        int32_t list_y = drawer_y + 18;
        int32_t list_h = drawer_h - 22;

        ui_px_box(drawer_x, drawer_y, drawer_w, drawer_h, false);
        ui_px_frame(drawer_x, drawer_y, drawer_w, drawer_h, true);
        ui_px_text_clipped(drawer_x + 8, drawer_y + 4, drawer_w - 16, ui_tr("Music"), true);
        ui_px_hline(drawer_x + 1, drawer_y + 16, drawer_w - 2, true);
        if (model->file_total == 0U) {
            ui_px_text_clipped(drawer_x + 8, list_y + 8, drawer_w - 16, ui_tr("no matching files"), true);
        } else {
            int32_t selector_y = list_y + (int32_t)(model->file_selected - model->file_scroll) * UI_LIST_LINE_H;
            int32_t list_bar_h = (int32_t)(((uint32_t)(model->file_selected + 1U) * (uint32_t)list_h) / model->file_total);

            if (list_bar_h < 1) {
                list_bar_h = 1;
            } else if (list_bar_h > list_h - 2) {
                list_bar_h = list_h - 2;
            }
            for (uint16_t row = 0; row < (uint16_t)UI_MUSIC_DRAWER_VISIBLE_ROWS; ++row) {
                ui_px_draw_music_file_row(model, snap, row, drawer_x + 4, list_y, drawer_w - 12);
            }
            if (model->file_selected >= model->file_scroll &&
                model->file_selected < model->file_scroll + UI_MUSIC_DRAWER_VISIBLE_ROWS) {
                ui_px_invert_rect(drawer_x + 2, selector_y, drawer_w - 8, UI_LIST_LINE_H - 1);
            }
            ui_px_vline(drawer_x + drawer_w - 4, list_y, list_h, true);
            ui_px_box(drawer_x + drawer_w - 6, list_y, 4, list_bar_h, true);
        }
    }
}

static bool ui_music_selected_matches_snapshot_locked(
    const ui_model_t *model,
    const music_player_snapshot_t *snap,
    ui_file_entry_t *entry_out)
{
    ui_file_entry_t entry = {0};

    if (model == NULL || snap == NULL) {
        return false;
    }
    if (!ui_current_file_locked(model, &entry)) {
        return false;
    }
    if (entry_out != NULL) {
        *entry_out = entry;
    }
    return entry.path[0] != '\0' &&
           snap->path[0] != '\0' &&
           strcmp(entry.path, snap->path) == 0;
}

static bool ui_music_play_selected_locked(ui_model_t *model)
{
    ui_file_entry_t entry = {0};
    music_player_snapshot_t snap = {0};
    char saved_path[TF_PATH_LEN_MAX] = {0};
    bool same_track = false;
    uint32_t start_position = 0U;
    esp_err_t err;

    if (model == NULL) {
        return false;
    }
    music_player_get_snapshot(&snap);
    same_track = ui_music_selected_matches_snapshot_locked(model, &snap, &entry);
    if (!same_track && !ui_current_file_locked(model, &entry)) {
        ui_set_status_locked(model, ui_tr("no file selected"));
        return false;
    }
    if (entry.is_dir) {
        ui_open_files_locked(model, entry.path, true);
        ui_music_set_drawer_open_locked(model, true);
        return true;
    }
    if (entry.is_dir || ui_file_kind_from_name(entry.name) != UI_FILE_KIND_AUDIO) {
        ui_set_status_locked(model, ui_tr("unsupported file"));
        return false;
    }
    if (same_track &&
        (snap.state == MUSIC_PLAYER_STATE_PLAYING || snap.state == MUSIC_PLAYER_STATE_PAUSED)) {
        return ui_music_toggle_or_play_current_locked(model);
    }

    (void)saved_path;
    start_position = ui_music_resume_position_for_entry(&entry);
    ESP_LOGI(
        UI_TAG,
        "music play selected: path=%s size=%" PRIu32 " pos=%" PRIu32 " same=%d",
        entry.path,
        entry.size,
        start_position,
        same_track ? 1 : 0);
    if (start_position > 0U) {
        err = music_player_play_from_position(entry.path, entry.size, start_position);
    } else {
        err = music_player_play(entry.path, entry.size);
    }
    if (err != ESP_OK) {
        ESP_LOGW(UI_TAG, "music play selected failed: path=%s err=%s", entry.path, esp_err_to_name(err));
        ui_set_status_locked(model, ui_tr("playback start failed"));
        return false;
    }

    ui_music_set_current_locked(&entry);
    ui_music_set_drawer_open_locked(model, false);
    model->page = UI_PAGE_MUSIC_PLAYER;
    model->parent_page = UI_PAGE_ROOT;
    model->selected = 0;
    model->scroll = 0;
    ui_set_status_locked(model, entry.name);
    ui_mark_chrome_dirty(model);
    ui_mark_content_dirty(model);
    return true;
}

static bool ui_music_find_track_locked(const ui_model_t *model, int delta, ui_file_entry_t *entry_out)
{
    ui_model_t *mutable_model = (ui_model_t *)model;
    uint16_t saved_selected;
    uint16_t saved_scroll;
    int total;
    int selected;

    if (mutable_model == NULL || entry_out == NULL || mutable_model->file_total == 0U || delta == 0) {
        return false;
    }
    saved_selected = mutable_model->file_selected;
    saved_scroll = mutable_model->file_scroll;
    total = (int)mutable_model->file_total;
    selected = (int)mutable_model->file_selected;

    for (int tries = 0; tries < total; ++tries) {
        ui_file_entry_t entry = {0};

        selected = (selected + delta) % total;
        if (selected < 0) {
            selected += total;
        }
        mutable_model->file_selected = (uint16_t)selected;
        mutable_model->file_scroll = ui_file_scroll_for_selected(
            mutable_model,
            mutable_model->file_selected,
            mutable_model->file_scroll,
            mutable_model->file_total);
        ui_file_ensure_window_locked(mutable_model, false);
        if (ui_current_file_locked(mutable_model, &entry) &&
            !entry.is_dir &&
            ui_file_kind_from_name(entry.name) == UI_FILE_KIND_AUDIO) {
            *entry_out = entry;
            return true;
        }
    }

    mutable_model->file_selected = saved_selected;
    mutable_model->file_scroll = saved_scroll;
    ui_file_ensure_window_locked(mutable_model, false);
    return false;
}

static void ui_open_music_player_locked(ui_model_t *model, const ui_file_entry_t *entry)
{
    if (model == NULL || entry == NULL) {
        return;
    }
    if (card == NULL) {
        ui_set_status_locked(model, ui_tr("TF card not ready"));
        return;
    }
    if (usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, ui_tr("TF busy by USB host"));
        return;
    }
    ui_music_set_current_locked(entry);
    (void)ui_music_play_selected_locked(model);
}

static void ui_music_play_adjacent_locked(ui_model_t *model, int delta)
{
    ui_file_entry_t next = {0};

    if (model == NULL || model->file_total == 0U) {
        return;
    }
    if (!ui_music_find_track_locked(model, delta, &next)) {
        return;
    }
    model->file_selected = next.ordinal;
    model->file_scroll =
        ui_file_scroll_for_selected(model, model->file_selected, model->file_scroll, model->file_total);
    ui_file_ensure_window_locked(model, false);
    ui_open_music_player_locked(model, &next);
}

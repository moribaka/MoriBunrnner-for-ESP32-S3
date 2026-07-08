static void ui_px_draw_music_player(const ui_model_t *model)
{
    music_player_snapshot_t snap = {0};
    char header[UI_ROW_TEXT_MAX_LEN] = {0};
    bool pause_icon = false;
    uint32_t percent = 0U;

    if (model == NULL) {
        return;
    }
    ui_music_player_view_state(model, &snap, header, sizeof(header), &pause_icon, &percent);
    ui_px_draw_music_header_line(header);
    ui_px_draw_music_volume_line(&snap);
    ui_px_draw_music_time_line(&snap);
    ui_px_draw_music_seek_bar(percent);
    ui_px_draw_music_controls(pause_icon);
    ui_px_draw_music_drawer(model, &snap);
}

static void ui_px_apply_music_player(const ui_model_t *model)
{
    char header[UI_ROW_TEXT_MAX_LEN] = {0};

    if (model == NULL) {
        return;
    }
    snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
    ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - 2, true);
    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }
    ui_px_draw_music_player(model);
    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
}

static void ui_px_apply_music_player_dynamic(const ui_model_t *model)
{
    bool redraw_chrome;
    bool redraw_player;
    bool redraw_header;
    bool redraw_controls;
    bool redraw_progress;
    bool redraw_drawer;
    bool redraw_header_marquee;
    bool redraw_drawer_marquee;
    int32_t seek_x = UI_MUSIC_SEEK_BAR_X - 1;
    int32_t seek_y = UI_MUSIC_TIME_Y - 2;
    int32_t seek_w = UI_MUSIC_SEEK_BAR_W + 2;
    int32_t seek_h = (UI_MUSIC_SEEK_BAR_Y + 12) - (UI_MUSIC_TIME_Y - 2);
    int32_t header_x = 18;
    int32_t header_y = UI_MUSIC_INFO_Y - 2;
    int32_t header_w = UI_CANVAS_W - 36;
    int32_t header_h = (UI_MUSIC_VOLUME_Y + UI_TEXT_GLYPH_H + 4) - (UI_MUSIC_INFO_Y - 2);
    int32_t controls_y = UI_MUSIC_CTRL_Y - 1;
    int32_t controls_h = UI_MUSIC_CTRL_MAIN_SIZE + 2;
    int32_t prev_drawer_x = (int32_t)s_anim.music_drawer_x;
    int32_t prev_drawer_visible_w = ui_music_drawer_visible_w(prev_drawer_x);
    int32_t drawer_clear_w = 0;
    int32_t drawer_visible_w = 0;
    bool drawer_static_cover = false;
    music_player_snapshot_t snap = {0};
    char track_header[UI_ROW_TEXT_MAX_LEN] = {0};
    bool pause_icon = false;
    uint32_t percent = 0U;

    if (model == NULL) {
        return;
    }
    redraw_chrome = model->chrome_dirty;
    redraw_player = model->music_player_dirty;
    redraw_header_marquee = ui_music_header_marquee_active(model);
    redraw_drawer_marquee = ui_music_drawer_marquee_active(model);
    redraw_header = redraw_player || redraw_header_marquee;
    redraw_controls = redraw_player;
    redraw_progress = model->music_progress_dirty || redraw_player;
    redraw_drawer = model->content_dirty ||
                    model->motion_dirty ||
                    redraw_drawer_marquee ||
                    ui_anim_value_active(s_anim.music_drawer_x, s_anim.music_drawer_target_x) ||
                    (ui_music_drawer_visible_w((int32_t)s_anim.music_drawer_x) > 0 &&
                     redraw_player);

    if (redraw_drawer) {
        ui_anim_move(&s_anim.music_drawer_x, s_anim.music_drawer_target_x, UI_LIST_BAR_SPEED);
        drawer_clear_w = ui_music_drawer_visible_w((int32_t)s_anim.music_drawer_x);
        if (prev_drawer_visible_w > drawer_clear_w) {
            drawer_clear_w = prev_drawer_visible_w;
        }
        if (drawer_clear_w > 0) {
            redraw_header = true;
            redraw_controls = true;
            redraw_progress = true;
            ui_px_clear_rect(0, UI_MUSIC_DRAWER_Y, drawer_clear_w, UI_MUSIC_DRAWER_H);
        }
    }
    drawer_visible_w = ui_music_drawer_visible_w((int32_t)s_anim.music_drawer_x);
    drawer_static_cover = !redraw_drawer && drawer_visible_w > 0;

    if (redraw_chrome) {
        ui_px_clear_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
        ui_px_clear_rect(0, UI_CANVAS_H - UI_HINT_H - 3, UI_CANVAS_W, UI_HINT_H + 3);
        {
            char header[UI_ROW_TEXT_MAX_LEN] = {0};

            snprintf(header, sizeof(header), "%s", ui_page_title(model->page));
            ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
            ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
            ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
            ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - 2, true);
            for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
                ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
            }
            if (model->status_text[0] != '\0') {
                ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
            }
        }
        ui_px_invalidate_rect(0, 0, UI_CANVAS_W, UI_LIST_HEADER_H);
        ui_px_invalidate_rect(0, UI_CANVAS_H - UI_HINT_H - 3, UI_CANVAS_W, UI_HINT_H + 3);
    }

    if (!redraw_header && !redraw_controls && !redraw_progress && !redraw_drawer) {
        return;
    }
    if (redraw_header || redraw_controls || redraw_progress || redraw_drawer) {
        ui_music_player_view_state(model, &snap, track_header, sizeof(track_header), &pause_icon, &percent);
    }
    if (redraw_header) {
        int32_t clear_x = header_x;
        int32_t clear_w = header_w;

        if (drawer_static_cover) {
            ui_px_clip_left_covered_region(drawer_visible_w, &clear_x, &clear_w);
        }
        ui_px_clear_rect(clear_x, header_y, clear_w, header_h);
        if (drawer_static_cover) {
            ui_px_clip_set(clear_x, header_y, clear_w, header_h);
        }
        ui_px_draw_music_header_line(track_header);
        ui_px_draw_music_volume_line(&snap);
        ui_px_clip_clear();
        ui_px_invalidate_rect(clear_x, header_y, clear_w, header_h);
    }
    if (redraw_controls) {
        int32_t clear_x = 0;
        int32_t clear_w = UI_CANVAS_W;

        if (drawer_static_cover) {
            ui_px_clip_left_covered_region(drawer_visible_w, &clear_x, &clear_w);
        }
        ui_px_clear_rect(clear_x, controls_y, clear_w, controls_h);
        if (drawer_static_cover) {
            ui_px_clip_set(clear_x, controls_y, clear_w, controls_h);
        }
        ui_px_draw_music_controls(pause_icon);
        ui_px_clip_clear();
        ui_px_invalidate_rect(clear_x, controls_y, clear_w, controls_h);
    }
    if (redraw_progress) {
        int32_t clear_x = seek_x;
        int32_t clear_w = seek_w;

        if (drawer_static_cover) {
            ui_px_clip_left_covered_region(drawer_visible_w, &clear_x, &clear_w);
        }
        ui_px_clear_rect(clear_x, seek_y, clear_w, seek_h);
        if (drawer_static_cover) {
            ui_px_clip_set(clear_x, seek_y, clear_w, seek_h);
        }
        ui_px_draw_music_time_line(&snap);
        ui_px_draw_music_seek_bar(percent);
        ui_px_clip_clear();
        ui_px_invalidate_rect(clear_x, seek_y, clear_w, seek_h);
    }
    if (redraw_drawer && drawer_clear_w > 0) {
        ui_px_draw_music_drawer(model, &snap);
        ui_px_invalidate_rect(0, UI_MUSIC_DRAWER_Y, drawer_clear_w, UI_MUSIC_DRAWER_H);
    }
}


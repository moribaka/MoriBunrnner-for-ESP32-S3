static void ui_px_apply_reader(const ui_model_t *model)
{
    char header[UI_ROW_TEXT_MAX_LEN] = {0};
    const ui_reader_window_t *window = NULL;
    uint32_t page_index = 0U;
    uint32_t page_count = 0U;
    uint32_t local_page_index = 0U;
    size_t start = 0U;
    size_t end = 0U;
    size_t offset = 0U;
    uint32_t line_index = 0U;
    int32_t line_w = 0;

    if (model == NULL) {
        return;
    }

    ui_format_page_header(model, header, sizeof(header));
    ui_px_text(4, UI_LIST_HEADER_TEXT_Y, header, true);
    ui_px_text_clipped(UI_CANVAS_W - 118, UI_LIST_HEADER_TEXT_Y, 58, model->time_text, true);
    ui_px_draw_battery_overlay(model, UI_LIST_HEADER_TEXT_Y);
    ui_px_hline(0, UI_LIST_HEADER_H - 5, UI_CANVAS_W - 2, true);
    for (int32_t x = 0; x < UI_CANVAS_W; x += 6) {
        ui_px_hline(x, UI_CANVAS_H - UI_HINT_H - 2, 2, true);
    }

    if (!s_reader_ctx.active || s_reader_ctx.page_count == 0U) {
        ui_px_text_clipped(
            UI_READER_MARGIN_X,
            UI_READER_TOP_Y + 4,
            UI_READER_TEXT_W,
            ui_tr("empty file"),
            true);
        if (model->status_text[0] != '\0') {
            ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
        }
        return;
    }

    page_count = s_reader_ctx.page_count;
    page_index = (model->reader_page < page_count) ? model->reader_page : (page_count - 1U);
    window = &s_reader_ctx.window;
    if (!ui_reader_window_contains_page(window, page_index) || window->page_text_offsets == NULL ||
        window->text_data == NULL || window->page_count == 0U) {
        ui_px_text_clipped(
            UI_READER_MARGIN_X,
            UI_READER_TOP_Y + 4,
            UI_READER_TEXT_W,
            ui_tr("empty file"),
            true);
        if (model->status_text[0] != '\0') {
            ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
        }
        return;
    }

    local_page_index = page_index - window->page_start;
    start = window->page_text_offsets[local_page_index];
    end = (local_page_index + 1U < window->page_count) ? window->page_text_offsets[local_page_index + 1U] : window->text_len;
    offset = start;

    while (offset < end && line_index < UI_READER_MAX_LINES) {
        if (window->text_data[offset] == '\n') {
            offset++;
            line_index++;
            line_w = 0;
            continue;
        }

        {
            const char *cursor = (const char *)&window->text_data[offset];
            const char *next = cursor;
            uint32_t codepoint = 0;
            size_t seq_len;
            int32_t draw_w;
            int32_t advance;

            if (!ui_utf8_next_codepoint(&next, &codepoint) || next <= cursor) {
                offset++;
                continue;
            }

            seq_len = (size_t)(next - cursor);
            draw_w = ui_px_codepoint_draw_width(codepoint);
            advance = ui_px_codepoint_advance(codepoint);

            if (line_w > 0 && line_w + draw_w > UI_READER_TEXT_W) {
                line_index++;
                line_w = 0;
                continue;
            }
            if (line_index >= UI_READER_MAX_LINES) {
                break;
            }

            ui_px_draw_codepoint(
                UI_READER_MARGIN_X + line_w,
                UI_READER_TOP_Y + (int32_t)line_index * UI_READER_LINE_H,
                codepoint,
                true);
            offset += seq_len;
            line_w += advance;
            if (line_w >= UI_READER_TEXT_W && offset < end) {
                line_index++;
                line_w = 0;
            }
        }
    }

    if (page_count > 1U) {
        int32_t bar_x = UI_READER_MARGIN_X;
        int32_t bar_y = UI_CANVAS_H - UI_HINT_H - 9;
        int32_t bar_w = UI_CANVAS_W - UI_READER_MARGIN_X * 2;
        int32_t fill_w = (int32_t)(((uint64_t)(page_index + 1U) * (uint64_t)(bar_w - 2)) / page_count);

        if (fill_w < 1) {
            fill_w = 1;
        }
        if (fill_w > bar_w - 2) {
            fill_w = bar_w - 2;
        }
        ui_px_frame(bar_x, bar_y, bar_w, 5, true);
        ui_px_box(bar_x + 1, bar_y + 1, fill_w, 3, true);
    }

    if (model->status_text[0] != '\0') {
        ui_px_text_clipped(4, UI_CANVAS_H - UI_HINT_H + 3, UI_CANVAS_W - 8, ui_status_text_to_display(model->status_text), true);
    }
}


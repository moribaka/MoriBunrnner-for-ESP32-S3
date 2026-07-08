static bool ui_reader_text_file_supported_name(const char *name)
{
    const char *ext = ui_file_ext(name);

    return strcasecmp(ext, "txt") == 0 ||
           strcasecmp(ext, "log") == 0 ||
           strcasecmp(ext, "ini") == 0 ||
           strcasecmp(ext, "cfg") == 0 ||
           strcasecmp(ext, "conf") == 0 ||
           strcasecmp(ext, "md") == 0 ||
           strcasecmp(ext, "json") == 0 ||
           strcasecmp(ext, "xml") == 0 ||
           strcasecmp(ext, "csv") == 0;
}

static bool ui_reader_native_file_supported_name(const char *name)
{
    return ui_reader_text_file_supported_name(name) || strcasecmp(ui_file_ext(name), "epub") == 0;
}

static void *ui_alloc_8bit_buffer(size_t size)
{
    void *ptr;

    if (size == 0U) {
        size = 1U;
    }

    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *ui_realloc_8bit_buffer(void *ptr, size_t size)
{
    void *next;

    if (ptr == NULL) {
        return ui_alloc_8bit_buffer(size);
    }
    if (size == 0U) {
        size = 1U;
    }
    next = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next == NULL) {
        next = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return next;
}

typedef struct {
    bool newline;
    uint32_t codepoint;
    size_t raw_len;
} ui_reader_token_t;

typedef struct {
    uint32_t line_index;
    int32_t line_w;
} ui_reader_page_scan_t;

static size_t ui_reader_utf8_expected_len(uint8_t first)
{
    if (first < 0x80U) {
        return 1U;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        return 2U;
    }
    if (first >= 0xE0U && first <= 0xEFU) {
        return 3U;
    }
    if (first >= 0xF0U && first <= 0xF4U) {
        return 4U;
    }
    return 0U;
}

static bool ui_reader_utf8_prefix_valid(const uint8_t *src, size_t len, size_t expected_len)
{
    if (src == NULL || len == 0U || expected_len == 0U || len > expected_len) {
        return false;
    }
    if (len >= 2U) {
        if ((src[1] & 0xC0U) != 0x80U) {
            return false;
        }
        if (expected_len == 3U) {
            if (src[0] == 0xE0U && src[1] < 0xA0U) {
                return false;
            }
            if (src[0] == 0xEDU && src[1] >= 0xA0U) {
                return false;
            }
        } else if (expected_len == 4U) {
            if (src[0] == 0xF0U && src[1] < 0x90U) {
                return false;
            }
            if (src[0] == 0xF4U && src[1] >= 0x90U) {
                return false;
            }
        }
    }
    if (len >= 3U && (src[2] & 0xC0U) != 0x80U) {
        return false;
    }
    if (len >= 4U && (src[3] & 0xC0U) != 0x80U) {
        return false;
    }
    return true;
}

static bool ui_reader_utf8_decode(const uint8_t *src, size_t len, uint32_t *codepoint_out, size_t *seq_len_out)
{
    const char *cursor = (const char *)src;
    uint32_t codepoint = 0U;
    size_t expected_len;
    size_t seq_len;

    if (codepoint_out != NULL) {
        *codepoint_out = 0U;
    }
    if (seq_len_out != NULL) {
        *seq_len_out = 0U;
    }
    if (src == NULL || len == 0U || codepoint_out == NULL) {
        return false;
    }

    expected_len = ui_reader_utf8_expected_len(src[0]);
    if (expected_len == 0U || len < expected_len) {
        return false;
    }

    seq_len = ui_utf8_sequence_len(src, len);
    if (seq_len == 0U) {
        return false;
    }
    if (!ui_utf8_next_codepoint(&cursor, &codepoint) || (size_t)(cursor - (const char *)src) != seq_len) {
        return false;
    }

    *codepoint_out = codepoint;
    if (seq_len_out != NULL) {
        *seq_len_out = seq_len;
    }
    return true;
}

static bool ui_reader_file_is_valid_utf8(FILE *file)
{
    uint8_t buffer[UI_READER_SCAN_CHUNK_BYTES + 4U];
    size_t buffered = 0U;
    bool valid = true;

    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        return false;
    }

    for (;;) {
        size_t read_len = fread(&buffer[buffered], 1, UI_READER_SCAN_CHUNK_BYTES, file);
        bool eof = (read_len < UI_READER_SCAN_CHUNK_BYTES) && feof(file);
        size_t total = buffered + read_len;
        size_t offset = 0U;

        if (read_len == 0U && !eof && ferror(file)) {
            valid = false;
            break;
        }
        while (offset < total) {
            size_t expected_len;
            size_t seq_len;

            if (buffer[offset] == 0U) {
                valid = false;
                break;
            }
            expected_len = ui_reader_utf8_expected_len(buffer[offset]);
            if (expected_len == 0U) {
                valid = false;
                break;
            }
            if (total - offset < expected_len) {
                if (!eof && ui_reader_utf8_prefix_valid(&buffer[offset], total - offset, expected_len)) {
                    memmove(buffer, &buffer[offset], total - offset);
                    buffered = total - offset;
                    break;
                }
                valid = false;
                break;
            }
            seq_len = ui_utf8_sequence_len(&buffer[offset], total - offset);
            if (seq_len == 0U) {
                valid = false;
                break;
            }
            offset += seq_len;
        }
        if (!valid) {
            break;
        }
        if (offset == total) {
            buffered = 0U;
        }
        if (eof) {
            if (buffered != 0U) {
                valid = false;
            }
            break;
        }
    }

    clearerr(file);
    (void)fseek(file, 0L, SEEK_SET);
    return valid;
}

static bool ui_reader_decode_token(
    const uint8_t *src,
    size_t len,
    bool utf8_text,
    bool eof,
    ui_reader_token_t *token_out)
{
    ui_reader_token_t token = {0};
    uint8_t c;

    if (token_out != NULL) {
        memset(token_out, 0, sizeof(*token_out));
    }
    if (src == NULL || len == 0U || token_out == NULL) {
        return false;
    }

    c = src[0];
    if (c == '\r') {
        if (len >= 2U && src[1] == '\n') {
            token.newline = true;
            token.raw_len = 2U;
            *token_out = token;
            return true;
        }
        if (!eof && len == 1U) {
            return false;
        }
        token.newline = true;
        token.raw_len = 1U;
        *token_out = token;
        return true;
    }
    if (c == '\n') {
        token.newline = true;
        token.raw_len = 1U;
        *token_out = token;
        return true;
    }
    if (c == '\t') {
        token.codepoint = ' ';
        token.raw_len = 1U;
        *token_out = token;
        return true;
    }
    if (c < 0x20U) {
        token.codepoint = ' ';
        token.raw_len = 1U;
        *token_out = token;
        return true;
    }
    if (c < 0x80U) {
        token.codepoint = c;
        token.raw_len = 1U;
        *token_out = token;
        return true;
    }

    if (utf8_text) {
        size_t expected_len = ui_reader_utf8_expected_len(c);

        if (expected_len == 0U) {
            token.codepoint = '?';
            token.raw_len = 1U;
            *token_out = token;
            return true;
        }
        if (len < expected_len) {
            if (!eof && ui_reader_utf8_prefix_valid(src, len, expected_len)) {
                return false;
            }
            token.codepoint = '?';
            token.raw_len = 1U;
            *token_out = token;
            return true;
        }
        if (!ui_reader_utf8_decode(src, len, &token.codepoint, &token.raw_len)) {
            token.codepoint = '?';
            token.raw_len = 1U;
        }
        *token_out = token;
        return true;
    }

    if (len < 2U && !eof) {
        return false;
    }

    token.codepoint = '?';
    if (len >= 2U && src[1] != 0U) {
        WCHAR uni = ff_oem2uni((WCHAR)(((uint16_t)c << 8) | src[1]), FF_CODE_PAGE);

        if (uni != 0U) {
            token.codepoint = (uint32_t)uni;
        }
        token.raw_len = 2U;
    } else {
        token.raw_len = 1U;
    }
    *token_out = token;
    return true;
}

static bool ui_reader_append_token_utf8(
    const ui_reader_token_t *token,
    uint8_t *dst,
    size_t dst_cap,
    size_t *dst_len)
{
    if (token == NULL || dst == NULL || dst_len == NULL || *dst_len + 1U >= dst_cap) {
        return false;
    }

    if (token->newline) {
        dst[(*dst_len)++] = '\n';
        return true;
    }
    if (token->codepoint < 0x80U) {
        dst[(*dst_len)++] = (uint8_t)token->codepoint;
        return true;
    }

    {
        size_t written = ui_utf8_encode(token->codepoint, (char *)&dst[*dst_len], dst_cap - *dst_len - 1U);

        if (written == 0U) {
            return false;
        }
        *dst_len += written;
    }
    return true;
}

static void ui_reader_page_scan_reset_page(ui_reader_page_scan_t *scan)
{
    if (scan == NULL) {
        return;
    }
    scan->line_index = 0U;
    scan->line_w = 0;
}

static bool ui_reader_page_scan_advance(
    ui_reader_page_scan_t *scan,
    const ui_reader_token_t *token,
    bool has_more,
    size_t raw_start,
    size_t raw_end,
    size_t *page_break_raw_offset_out,
    bool *consumed_out)
{
    int32_t draw_w;
    int32_t advance;

    if (page_break_raw_offset_out != NULL) {
        *page_break_raw_offset_out = SIZE_MAX;
    }
    if (consumed_out != NULL) {
        *consumed_out = false;
    }
    if (scan == NULL || token == NULL || page_break_raw_offset_out == NULL || consumed_out == NULL) {
        return false;
    }

    if (token->newline) {
        *consumed_out = true;
        if (scan->line_index + 1U >= UI_READER_MAX_LINES) {
            *page_break_raw_offset_out = raw_end;
            ui_reader_page_scan_reset_page(scan);
            return true;
        }
        scan->line_index++;
        scan->line_w = 0;
        return true;
    }

    draw_w = ui_px_codepoint_draw_width(token->codepoint);
    advance = ui_px_codepoint_advance(token->codepoint);
    if (scan->line_w > 0 && scan->line_w + draw_w > UI_READER_TEXT_W) {
        if (scan->line_index + 1U >= UI_READER_MAX_LINES) {
            *page_break_raw_offset_out = raw_start;
            ui_reader_page_scan_reset_page(scan);
            return true;
        }
        scan->line_index++;
        scan->line_w = 0;
    }

    *consumed_out = true;
    scan->line_w += advance;
    if (scan->line_w >= UI_READER_TEXT_W && has_more) {
        if (scan->line_index + 1U >= UI_READER_MAX_LINES) {
            *page_break_raw_offset_out = raw_end;
            ui_reader_page_scan_reset_page(scan);
            return true;
        }
        scan->line_index++;
        scan->line_w = 0;
    }
    return true;
}

static bool ui_reader_page_offsets_push(
    uint32_t **page_offsets,
    uint32_t *page_count,
    uint32_t *page_capacity,
    uint32_t value)
{
    uint32_t *next_offsets;
    uint32_t next_capacity;

    if (page_offsets == NULL || page_count == NULL || page_capacity == NULL) {
        return false;
    }
    if (*page_count >= UI_READER_MAX_PAGE_COUNT) {
        return false;
    }
    if (*page_count < *page_capacity) {
        (*page_offsets)[(*page_count)++] = value;
        return true;
    }

    next_capacity = (*page_capacity == 0U) ? 128U : (*page_capacity * 2U);
    if (next_capacity < *page_count + 1U) {
        next_capacity = *page_count + 1U;
    }
    if (next_capacity > UI_READER_MAX_PAGE_COUNT) {
        next_capacity = UI_READER_MAX_PAGE_COUNT;
    }

    next_offsets = (uint32_t *)ui_realloc_8bit_buffer(*page_offsets, sizeof(uint32_t) * next_capacity);
    if (next_offsets == NULL) {
        return false;
    }
    *page_offsets = next_offsets;
    *page_capacity = next_capacity;
    (*page_offsets)[(*page_count)++] = value;
    return true;
}

static bool ui_reader_build_page_offsets_from_file(
    FILE *file,
    bool utf8_text,
    uint32_t file_size,
    uint32_t **page_offsets_out,
    uint32_t *page_count_out)
{
    uint8_t buffer[UI_READER_SCAN_CHUNK_BYTES + 4U];
    ui_reader_page_scan_t scan = {0};
    uint32_t *page_offsets = NULL;
    uint32_t page_count = 0U;
    uint32_t page_capacity = 0U;
    size_t buffer_start = 0U;
    size_t buffered = 0U;
    bool ok = false;

    if (page_offsets_out == NULL || page_count_out == NULL || file == NULL) {
        return false;
    }

    *page_offsets_out = NULL;
    *page_count_out = 0U;
    if (fseek(file, 0L, SEEK_SET) != 0) {
        return false;
    }
    if (!ui_reader_page_offsets_push(&page_offsets, &page_count, &page_capacity, 0U)) {
        goto cleanup;
    }
    ui_reader_page_scan_reset_page(&scan);

    for (;;) {
        size_t read_len = fread(&buffer[buffered], 1, UI_READER_SCAN_CHUNK_BYTES, file);
        bool eof = (read_len < UI_READER_SCAN_CHUNK_BYTES) && feof(file);
        size_t total = buffered + read_len;
        size_t offset = 0U;

        if (read_len == 0U && !eof && ferror(file)) {
            goto cleanup;
        }
        if (total == 0U) {
            ok = true;
            break;
        }

        while (offset < total) {
            ui_reader_token_t token = {0};
            size_t raw_start;
            size_t raw_end;

            if (!ui_reader_decode_token(&buffer[offset], total - offset, utf8_text, eof, &token)) {
                break;
            }

            raw_start = buffer_start + offset;
            raw_end = raw_start + token.raw_len;
            for (;;) {
                size_t page_break_raw_offset = SIZE_MAX;
                bool consumed = false;

                if (!ui_reader_page_scan_advance(
                        &scan,
                        &token,
                        raw_end < file_size,
                        raw_start,
                        raw_end,
                        &page_break_raw_offset,
                        &consumed)) {
                    goto cleanup;
                }
                if (page_break_raw_offset != SIZE_MAX) {
                    if (page_break_raw_offset > UINT32_MAX) {
                        goto cleanup;
                    }
                    if (!ui_reader_page_offsets_push(
                            &page_offsets,
                            &page_count,
                            &page_capacity,
                            (uint32_t)page_break_raw_offset)) {
                        goto cleanup;
                    }
                }
                if (consumed) {
                    break;
                }
            }

            offset += token.raw_len;
        }

        if (offset < total) {
            buffered = total - offset;
            memmove(buffer, &buffer[offset], buffered);
            buffer_start += offset;
        } else {
            buffered = 0U;
            buffer_start += total;
        }

        if (eof) {
            if (buffered != 0U) {
                goto cleanup;
            }
            ok = true;
            break;
        }
    }

cleanup:
    clearerr(file);
    (void)fseek(file, 0L, SEEK_SET);
    if (!ok) {
        if (page_offsets != NULL) {
            heap_caps_free(page_offsets);
        }
        return false;
    }

    *page_offsets_out = page_offsets;
    *page_count_out = page_count;
    return true;
}

static bool ui_reader_build_page_offsets_from_raw(
    const uint8_t *raw,
    size_t raw_len,
    bool utf8_text,
    uint32_t **page_offsets_out,
    uint32_t *page_count_out)
{
    ui_reader_page_scan_t scan = {0};
    uint32_t *page_offsets = NULL;
    uint32_t page_count = 0U;
    uint32_t page_capacity = 0U;
    size_t offset = 0U;
    bool ok = false;

    if (page_offsets_out == NULL || page_count_out == NULL || (raw == NULL && raw_len > 0U)) {
        return false;
    }
    *page_offsets_out = NULL;
    *page_count_out = 0U;

    if (!ui_reader_page_offsets_push(&page_offsets, &page_count, &page_capacity, 0U)) {
        goto cleanup;
    }
    ui_reader_page_scan_reset_page(&scan);

    while (offset < raw_len) {
        ui_reader_token_t token = {0};
        size_t raw_start = offset;
        size_t raw_end;

        if (!ui_reader_decode_token(&raw[offset], raw_len - offset, utf8_text, true, &token)) {
            goto cleanup;
        }
        raw_end = raw_start + token.raw_len;

        for (;;) {
            size_t page_break_raw_offset = SIZE_MAX;
            bool consumed = false;

            if (!ui_reader_page_scan_advance(
                    &scan,
                    &token,
                    raw_end < raw_len,
                    raw_start,
                    raw_end,
                    &page_break_raw_offset,
                    &consumed)) {
                goto cleanup;
            }
            if (page_break_raw_offset != SIZE_MAX) {
                if (page_break_raw_offset > UINT32_MAX) {
                    goto cleanup;
                }
                if (!ui_reader_page_offsets_push(
                        &page_offsets,
                        &page_count,
                        &page_capacity,
                        (uint32_t)page_break_raw_offset)) {
                    goto cleanup;
                }
            }
            if (consumed) {
                break;
            }
        }

        offset += token.raw_len;
    }

    ok = true;

cleanup:
    if (!ok) {
        if (page_offsets != NULL) {
            heap_caps_free(page_offsets);
        }
        return false;
    }
    *page_offsets_out = page_offsets;
    *page_count_out = page_count;
    return true;
}

static void ui_reader_window_clear(ui_reader_window_t *window)
{
    if (window == NULL) {
        return;
    }
    if (window->text_data != NULL) {
        heap_caps_free(window->text_data);
    }
    if (window->page_text_offsets != NULL) {
        heap_caps_free(window->page_text_offsets);
    }
    memset(window, 0, sizeof(*window));
}

static bool ui_reader_window_contains_page(const ui_reader_window_t *window, uint32_t page_index)
{
    if (window == NULL || window->page_count == 0U) {
        return false;
    }
    return page_index >= window->page_start &&
           page_index < window->page_start + window->page_count;
}

static bool ui_reader_build_window_from_raw(
    const uint8_t *raw,
    size_t raw_len,
    bool utf8_text,
    uint32_t page_start,
    uint32_t page_count,
    ui_reader_window_t *window_out)
{
    ui_reader_window_t window = {0};
    ui_reader_page_scan_t scan = {0};
    size_t dst_cap;
    size_t raw_offset = 0U;
    size_t dst_len = 0U;
    uint32_t local_page_index = 0U;
    bool ok = false;

    if (window_out == NULL || (raw == NULL && raw_len > 0U) || page_count == 0U) {
        return false;
    }

    if (utf8_text) {
        if (raw_len > SIZE_MAX - 1U) {
            return false;
        }
        dst_cap = raw_len + 1U;
    } else {
        if (raw_len > ((SIZE_MAX - 16U) / 3U) * 2U) {
            return false;
        }
        dst_cap = raw_len + (raw_len / 2U) + 16U;
    }

    window.text_data = (uint8_t *)ui_alloc_8bit_buffer(dst_cap);
    if (window.text_data == NULL) {
        goto cleanup;
    }
    window.page_text_offsets = (uint32_t *)ui_alloc_8bit_buffer(sizeof(uint32_t) * page_count);
    if (window.page_text_offsets == NULL) {
        goto cleanup;
    }
    window.page_start = page_start;
    window.page_count = page_count;
    window.page_text_offsets[0] = 0U;
    ui_reader_page_scan_reset_page(&scan);

    while (raw_offset < raw_len) {
        ui_reader_token_t token = {0};

        if (!ui_reader_decode_token(&raw[raw_offset], raw_len - raw_offset, utf8_text, true, &token)) {
            goto cleanup;
        }

        for (;;) {
            size_t page_break_raw_offset = SIZE_MAX;
            bool consumed = false;

            if (!ui_reader_page_scan_advance(
                    &scan,
                    &token,
                    (raw_offset + token.raw_len) < raw_len,
                    raw_offset,
                    raw_offset + token.raw_len,
                    &page_break_raw_offset,
                    &consumed)) {
                goto cleanup;
            }
            if (!consumed) {
                if (page_break_raw_offset == SIZE_MAX || local_page_index + 1U >= page_count) {
                    goto cleanup;
                }
                local_page_index++;
                window.page_text_offsets[local_page_index] = (uint32_t)dst_len;
                continue;
            }

            if (!ui_reader_append_token_utf8(&token, window.text_data, dst_cap, &dst_len)) {
                goto cleanup;
            }
            if (page_break_raw_offset != SIZE_MAX) {
                if (local_page_index + 1U < page_count) {
                    local_page_index++;
                    window.page_text_offsets[local_page_index] = (uint32_t)dst_len;
                } else if (raw_offset + token.raw_len < raw_len) {
                    goto cleanup;
                }
            }
            break;
        }

        raw_offset += token.raw_len;
    }

    if (local_page_index + 1U != page_count) {
        goto cleanup;
    }

    window.text_data[dst_len] = '\0';
    window.text_len = dst_len;
    *window_out = window;
    memset(&window, 0, sizeof(window));
    ok = true;

cleanup:
    if (!ok) {
        ui_reader_window_clear(&window);
    }
    return ok;
}

static bool ui_reader_find_epub_section_for_page(uint32_t page_index, uint32_t *section_index_out)
{
    uint32_t left = 0U;
    uint32_t right;

    if (section_index_out != NULL) {
        *section_index_out = 0U;
    }
    if (s_reader_ctx.section_page_starts == NULL || s_reader_ctx.section_count == 0U) {
        return false;
    }
    if (page_index >= s_reader_ctx.page_count) {
        page_index = s_reader_ctx.page_count - 1U;
    }

    right = s_reader_ctx.section_count;
    while (left + 1U < right) {
        uint32_t mid = left + (right - left) / 2U;

        if (s_reader_ctx.section_page_starts[mid] <= page_index) {
            left = mid;
        } else {
            right = mid;
        }
    }

    if (section_index_out != NULL) {
        *section_index_out = left;
    }
    return true;
}

static bool ui_reader_load_window_locked(uint32_t page_start, ui_reader_window_t *window_out)
{
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    ui_reader_window_t window = {0};
    FILE *file = NULL;
    uint8_t *raw = NULL;
    size_t raw_start;
    size_t raw_end;
    size_t raw_len;
    uint32_t window_page_end;
    bool ok = false;

    if (window_out != NULL) {
        memset(window_out, 0, sizeof(*window_out));
    }
    if (window_out == NULL || !s_reader_ctx.active || page_start >= s_reader_ctx.page_count) {
        return false;
    }
    if (s_reader_ctx.source == UI_READER_SOURCE_EPUB) {
        uint32_t section_index = 0U;
        uint32_t remaining_pages;
        uint32_t loaded_pages = 0U;
        uint32_t current_page = page_start;
        size_t text_cap = 0U;
        size_t text_len = 0U;

        if (s_reader_ctx.epub_book == NULL || !ui_reader_find_epub_section_for_page(page_start, &section_index)) {
            return false;
        }
        remaining_pages = s_reader_ctx.page_count - page_start;
        if (remaining_pages > UI_READER_WINDOW_PAGES) {
            remaining_pages = UI_READER_WINDOW_PAGES;
        }

        window.page_text_offsets = (uint32_t *)ui_alloc_8bit_buffer(sizeof(uint32_t) * remaining_pages);
        if (window.page_text_offsets == NULL) {
            return false;
        }
        window.page_start = page_start;
        window.page_count = remaining_pages;

        while (section_index < s_reader_ctx.section_count && loaded_pages < remaining_pages) {
            uint8_t *section_text = NULL;
            size_t section_text_len = 0U;
            uint32_t *section_offsets = NULL;
            uint32_t section_pages = 0U;
            uint32_t section_page_start = s_reader_ctx.section_page_starts[section_index];
            uint32_t local_start_page;
            uint32_t pages_to_take;
            size_t raw_start;
            size_t raw_end;
            size_t needed_cap;

            if (!ui_epub_book_load_section_text(s_reader_ctx.epub_book, section_index, &section_text, &section_text_len)) {
                goto cleanup;
            }
            if (!ui_reader_build_page_offsets_from_raw(section_text, section_text_len, true, &section_offsets, &section_pages)) {
                ui_epub_book_free_buffer(section_text);
                goto cleanup;
            }

            local_start_page = (current_page > section_page_start) ? (current_page - section_page_start) : 0U;
            if (local_start_page >= section_pages) {
                heap_caps_free(section_offsets);
                ui_epub_book_free_buffer(section_text);
                section_index++;
                continue;
            }

            pages_to_take = section_pages - local_start_page;
            if (pages_to_take > remaining_pages - loaded_pages) {
                pages_to_take = remaining_pages - loaded_pages;
            }
            raw_start = section_offsets[local_start_page];
            raw_end = (local_start_page + pages_to_take < section_pages) ?
                          section_offsets[local_start_page + pages_to_take] :
                          section_text_len;
            if (raw_end < raw_start) {
                heap_caps_free(section_offsets);
                ui_epub_book_free_buffer(section_text);
                goto cleanup;
            }

            needed_cap = text_len + (raw_end - raw_start) + 1U;
            if (needed_cap > text_cap) {
                uint8_t *next_text = (uint8_t *)ui_realloc_8bit_buffer(window.text_data, needed_cap);

                if (next_text == NULL) {
                    heap_caps_free(section_offsets);
                    ui_epub_book_free_buffer(section_text);
                    goto cleanup;
                }
                window.text_data = next_text;
                text_cap = needed_cap;
            }

            memcpy(&window.text_data[text_len], &section_text[raw_start], raw_end - raw_start);
            for (uint32_t i = 0U; i < pages_to_take; ++i) {
                window.page_text_offsets[loaded_pages + i] =
                    (uint32_t)(text_len + (size_t)(section_offsets[local_start_page + i] - raw_start));
            }
            text_len += raw_end - raw_start;
            loaded_pages += pages_to_take;
            current_page += pages_to_take;

            heap_caps_free(section_offsets);
            ui_epub_book_free_buffer(section_text);
            section_index++;
        }

        if (window.text_data == NULL) {
            window.text_data = (uint8_t *)ui_alloc_8bit_buffer(1U);
            if (window.text_data == NULL) {
                goto cleanup;
            }
            text_cap = 1U;
        }
        window.text_data[text_len] = '\0';
        window.text_len = text_len;
        if (loaded_pages == 0U) {
            goto cleanup;
        }
        window.page_count = loaded_pages;
        *window_out = window;
        memset(&window, 0, sizeof(window));
        return true;
    }
    if (s_reader_ctx.page_offsets == NULL) {
        return false;
    }
    if (!burner_build_full_path(s_reader_ctx.path, full_path, sizeof(full_path))) {
        return false;
    }

    window_page_end = page_start + UI_READER_WINDOW_PAGES;
    if (window_page_end > s_reader_ctx.page_count) {
        window_page_end = s_reader_ctx.page_count;
    }
    raw_start = s_reader_ctx.page_offsets[page_start];
    raw_end = (window_page_end < s_reader_ctx.page_count) ?
                  s_reader_ctx.page_offsets[window_page_end] :
                  s_reader_ctx.file_size;
    if (raw_end < raw_start) {
        return false;
    }
    raw_len = raw_end - raw_start;

    file = fopen(full_path, "rb");
    if (file == NULL) {
        return false;
    }
    if (raw_len > 0U) {
        raw = (uint8_t *)ui_alloc_8bit_buffer(raw_len);
        if (raw == NULL) {
            goto cleanup;
        }
        if (fseek(file, (long)raw_start, SEEK_SET) != 0) {
            goto cleanup;
        }
        if (fread(raw, 1, raw_len, file) != raw_len) {
            goto cleanup;
        }
    }

    if (!ui_reader_build_window_from_raw(
            raw,
            raw_len,
            s_reader_ctx.utf8_text,
            page_start,
            window_page_end - page_start,
            &window)) {
        goto cleanup;
    }

    *window_out = window;
    memset(&window, 0, sizeof(window));
    ok = true;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    if (raw != NULL) {
        heap_caps_free(raw);
    }
    if (!ok) {
        ui_reader_window_clear(&window);
    }
    return ok;
}

static void ui_reader_replace_window_locked(ui_reader_window_t *dst, ui_reader_window_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    ui_reader_window_clear(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

static void ui_reader_prefetch_if_needed_locked(uint32_t current_page)
{
    ui_reader_window_t prefetch = {0};
    uint32_t local_page_index;
    uint32_t next_page_start;

    if (!s_reader_ctx.active || !ui_reader_window_contains_page(&s_reader_ctx.window, current_page)) {
        return;
    }

    local_page_index = current_page - s_reader_ctx.window.page_start;
    if (((uint64_t)(local_page_index + 1U) * 100U) <
        ((uint64_t)s_reader_ctx.window.page_count * UI_READER_PREFETCH_PERCENT)) {
        return;
    }

    next_page_start = s_reader_ctx.window.page_start + s_reader_ctx.window.page_count;
    if (next_page_start >= s_reader_ctx.page_count) {
        ui_reader_window_clear(&s_reader_ctx.prefetch);
        return;
    }
    if (s_reader_ctx.prefetch.page_count > 0U && s_reader_ctx.prefetch.page_start == next_page_start) {
        return;
    }
    if (!ui_reader_load_window_locked(next_page_start, &prefetch)) {
        return;
    }

    ui_reader_window_clear(&s_reader_ctx.prefetch);
    s_reader_ctx.prefetch = prefetch;
}

static bool ui_reader_ensure_page_loaded_locked(uint32_t page_index)
{
    ui_reader_window_t next_window = {0};
    uint32_t window_page_start;

    if (!s_reader_ctx.active || s_reader_ctx.page_count == 0U) {
        return false;
    }
    if (s_reader_ctx.source == UI_READER_SOURCE_TEXT && s_reader_ctx.page_offsets == NULL) {
        return false;
    }
    if (s_reader_ctx.source == UI_READER_SOURCE_EPUB &&
        (s_reader_ctx.epub_book == NULL || s_reader_ctx.section_page_starts == NULL || s_reader_ctx.section_count == 0U)) {
        return false;
    }
    if (page_index >= s_reader_ctx.page_count) {
        page_index = s_reader_ctx.page_count - 1U;
    }

    if (ui_reader_window_contains_page(&s_reader_ctx.window, page_index)) {
        ui_reader_prefetch_if_needed_locked(page_index);
        return true;
    }
    if (ui_reader_window_contains_page(&s_reader_ctx.prefetch, page_index)) {
        ui_reader_window_t old_window = s_reader_ctx.window;

        s_reader_ctx.window = s_reader_ctx.prefetch;
        memset(&s_reader_ctx.prefetch, 0, sizeof(s_reader_ctx.prefetch));
        ui_reader_window_clear(&old_window);
        ui_reader_prefetch_if_needed_locked(page_index);
        return true;
    }

    window_page_start = (page_index / UI_READER_WINDOW_PAGES) * UI_READER_WINDOW_PAGES;
    if (!ui_reader_load_window_locked(window_page_start, &next_window)) {
        return false;
    }

    ui_reader_replace_window_locked(&s_reader_ctx.window, &next_window);
    ui_reader_window_clear(&s_reader_ctx.prefetch);
    ui_reader_prefetch_if_needed_locked(page_index);
    return true;
}

static void ui_reader_sync_status_locked(ui_model_t *model)
{
    char status[UI_STATUS_TEXT_MAX_LEN] = {0};
    uint32_t page_index;
    uint32_t page_count;

    if (model == NULL) {
        return;
    }
    if (!s_reader_ctx.active || s_reader_ctx.page_count == 0U) {
        ui_set_status_locked(model, ui_tr("read only"));
        return;
    }

    page_count = s_reader_ctx.page_count;
    page_index = (model->reader_page < page_count) ? model->reader_page : (page_count - 1U);
    snprintf(
        status,
        sizeof(status),
        ui_lang_is_zh() ? "%" PRIu32 "/%" PRIu32 "  L/R翻页  B返回" : "%" PRIu32 "/%" PRIu32 "  L/R page  B back",
        page_index + 1U,
        page_count);
    ui_set_status_locked(model, status);
}

static void ui_reader_save_history_path_page(const char *path, uint32_t page)
{
    FILE *fp = NULL;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    fp = fopen(UI_READER_HISTORY_PATH, "wb");
    if (fp == NULL) {
        return;
    }

    (void)fprintf(fp, "path=%s\npage=%" PRIu32 "\n", path, page);
    fclose(fp);
}

static void ui_reader_save_history_locked(const ui_model_t *model)
{
    uint32_t page = 0U;

    if (model == NULL || !s_reader_ctx.active || s_reader_ctx.path[0] == '\0') {
        return;
    }
    page = model->reader_page;
    if (s_reader_ctx.page_count > 0U && page >= s_reader_ctx.page_count) {
        page = s_reader_ctx.page_count - 1U;
    }
    ui_reader_save_history_path_page(s_reader_ctx.path, page);
}

static bool ui_reader_load_history(char *path, size_t path_len, uint32_t *page_out)
{
    FILE *fp = NULL;
    char line[320];
    bool found_path = false;
    uint32_t page = 0U;

    if (path == NULL || path_len == 0U) {
        return false;
    }

    path[0] = '\0';
    if (page_out != NULL) {
        *page_out = 0U;
    }

    fp = fopen(UI_READER_HISTORY_PATH, "rb");
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
        } else if (strcmp(line, "page") == 0) {
            char *endptr = NULL;
            unsigned long parsed = strtoul(value, &endptr, 10);

            if (endptr != value && endptr != NULL && *endptr == '\0' && parsed <= UINT32_MAX) {
                page = (uint32_t)parsed;
            }
        }
    }

    fclose(fp);
    if (page_out != NULL) {
        *page_out = page;
    }
    return found_path;
}

static void ui_reader_clear_locked(void)
{
    ui_reader_window_clear(&s_reader_ctx.window);
    ui_reader_window_clear(&s_reader_ctx.prefetch);
    if (s_reader_ctx.page_offsets != NULL) {
        heap_caps_free(s_reader_ctx.page_offsets);
    }
    if (s_reader_ctx.section_page_starts != NULL) {
        heap_caps_free(s_reader_ctx.section_page_starts);
    }
    if (s_reader_ctx.epub_book != NULL) {
        ui_epub_book_close(s_reader_ctx.epub_book);
    }
    memset(&s_reader_ctx, 0, sizeof(s_reader_ctx));
}

static bool ui_open_reader_locked(ui_model_t *model, const ui_file_entry_t *entry)
{
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    char saved_path[TF_PATH_LEN_MAX] = {0};
    FILE *file = NULL;
    struct stat st;
    ui_epub_book_t *epub_book = NULL;
    uint32_t *page_offsets = NULL;
    uint32_t *section_page_starts = NULL;
    uint32_t page_count = 0U;
    uint32_t section_count = 0U;
    uint32_t saved_page = 0U;
    uint32_t initial_page = 0U;
    uint64_t file_size = 0U;
    bool utf8_text;
    bool is_epub;
    bool restore_page = false;
    bool ok = false;

    if (model == NULL || entry == NULL) {
        return false;
    }
    if (entry->is_dir || !ui_reader_native_file_supported_name(entry->name)) {
        ui_set_status_locked(model, ui_tr("unsupported file"));
        return false;
    }
    if (card == NULL) {
        ui_set_status_locked(model, ui_tr("TF card not ready"));
        return false;
    }
    if (usb_msc_tf_in_use_by_host()) {
        ui_set_status_locked(model, ui_tr("TF busy by USB host"));
        return false;
    }
    if (!burner_build_full_path(entry->path, full_path, sizeof(full_path))) {
        ui_set_status_locked(model, ui_tr("path too long"));
        return false;
    }
    if (stat(full_path, &st) != 0) {
        ui_set_status_locked(model, ui_tr("open file failed"));
        return false;
    }

    restore_page = ui_reader_load_history(saved_path, sizeof(saved_path), &saved_page) &&
                   strcmp(saved_path, entry->path) == 0;
    file_size = (st.st_size > 0) ? (uint64_t)st.st_size : 0U;
    if (file_size > UI_READER_MAX_FILE_BYTES) {
        ui_set_status_locked(model, ui_tr("file too large"));
        return false;
    }

    is_epub = strcasecmp(ui_file_ext(entry->name), "epub") == 0;
    if (is_epub) {
        uint32_t running_page = 0U;

        if (!ui_epub_book_open(full_path, &epub_book)) {
            ui_set_status_locked(model, ui_tr("open file failed"));
            goto cleanup;
        }

        section_count = ui_epub_book_section_count(epub_book);
        if (section_count == 0U) {
            ui_set_status_locked(model, ui_tr("empty file"));
            goto cleanup;
        }

        section_page_starts = (uint32_t *)ui_alloc_8bit_buffer(sizeof(uint32_t) * section_count);
        if (section_page_starts == NULL) {
            ui_set_status_locked(model, ui_tr("no memory"));
            goto cleanup;
        }

        for (uint32_t i = 0U; i < section_count; ++i) {
            uint8_t *section_text = NULL;
            size_t section_text_len = 0U;
            uint32_t *section_offsets = NULL;
            uint32_t section_pages = 0U;

            section_page_starts[i] = running_page;
            if (!ui_epub_book_load_section_text(epub_book, i, &section_text, &section_text_len)) {
                ui_set_status_locked(model, ui_tr("open file failed"));
                goto cleanup;
            }
            if (!ui_reader_build_page_offsets_from_raw(section_text, section_text_len, true, &section_offsets, &section_pages)) {
                ui_epub_book_free_buffer(section_text);
                ui_set_status_locked(model, ui_tr("no memory"));
                goto cleanup;
            }
            if (section_offsets != NULL) {
                heap_caps_free(section_offsets);
            }
            ui_epub_book_free_buffer(section_text);
            if (running_page > UI_READER_MAX_PAGE_COUNT - section_pages) {
                ui_set_status_locked(model, ui_tr("file too large"));
                goto cleanup;
            }
            running_page += section_pages;
        }

        page_count = running_page;
        if (page_count == 0U) {
            page_count = 1U;
        }

        ui_reader_clear_locked();
        s_reader_ctx.active = true;
        s_reader_ctx.utf8_text = true;
        s_reader_ctx.source = UI_READER_SOURCE_EPUB;
        snprintf(s_reader_ctx.path, sizeof(s_reader_ctx.path), "%s", entry->path);
        snprintf(s_reader_ctx.name, sizeof(s_reader_ctx.name), "%s", entry->name);
        s_reader_ctx.page_count = page_count;
        s_reader_ctx.file_size = (uint32_t)file_size;
        s_reader_ctx.section_page_starts = section_page_starts;
        s_reader_ctx.section_count = section_count;
        s_reader_ctx.epub_book = epub_book;
        section_page_starts = NULL;
        epub_book = NULL;

        initial_page = restore_page ? saved_page : 0U;
        if (initial_page >= page_count) {
            initial_page = page_count - 1U;
        }
        if (!ui_reader_ensure_page_loaded_locked(initial_page)) {
            ui_reader_clear_locked();
            ui_set_status_locked(model, ui_tr("no memory"));
            goto cleanup;
        }

        model->page = UI_PAGE_READER;
        model->parent_page = UI_PAGE_FILES;
        model->reader_page = initial_page;
        ui_music_set_drawer_open_locked(model, false);
        ui_reader_sync_status_locked(model);
        ok = true;
        goto cleanup;
    }

    file = fopen(full_path, "rb");
    if (file == NULL) {
        ui_set_status_locked(model, ui_tr("open file failed"));
        return false;
    }

    utf8_text = ui_reader_file_is_valid_utf8(file);
    if (!ui_reader_build_page_offsets_from_file(file, utf8_text, (uint32_t)file_size, &page_offsets, &page_count)) {
        ui_set_status_locked(model, ui_tr("no memory"));
        goto cleanup;
    }

    ui_reader_clear_locked();
    s_reader_ctx.active = true;
    s_reader_ctx.utf8_text = utf8_text;
    s_reader_ctx.source = UI_READER_SOURCE_TEXT;
    snprintf(s_reader_ctx.path, sizeof(s_reader_ctx.path), "%s", entry->path);
    snprintf(s_reader_ctx.name, sizeof(s_reader_ctx.name), "%s", entry->name);
    s_reader_ctx.page_offsets = page_offsets;
    page_offsets = NULL;
    s_reader_ctx.page_count = page_count;
    s_reader_ctx.file_size = (uint32_t)file_size;
    initial_page = restore_page ? saved_page : 0U;
    if (initial_page >= page_count) {
        initial_page = page_count - 1U;
    }
    if (!ui_reader_ensure_page_loaded_locked(initial_page)) {
        ui_reader_clear_locked();
        ui_set_status_locked(model, ui_tr("no memory"));
        goto cleanup;
    }

    model->page = UI_PAGE_READER;
    model->parent_page = UI_PAGE_FILES;
    model->reader_page = initial_page;
    ui_music_set_drawer_open_locked(model, false);
    ui_reader_sync_status_locked(model);
    ok = true;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    if (!ok) {
        if (epub_book != NULL) {
            ui_epub_book_close(epub_book);
        }
        if (section_page_starts != NULL) {
            heap_caps_free(section_page_starts);
        }
        if (page_offsets != NULL) {
            heap_caps_free(page_offsets);
        }
    }
    return ok;
}

static void ui_reader_move_page_locked(ui_model_t *model, int delta)
{
    int64_t next_page;

    if (model == NULL || !s_reader_ctx.active || s_reader_ctx.page_count == 0U || delta == 0) {
        return;
    }

    next_page = (int64_t)model->reader_page + (int64_t)delta;
    if (next_page < 0) {
        next_page = 0;
    }
    if ((uint64_t)next_page >= s_reader_ctx.page_count) {
        next_page = (int64_t)s_reader_ctx.page_count - 1LL;
    }
    if ((uint32_t)next_page == model->reader_page) {
        return;
    }

    model->reader_page = (uint32_t)next_page;
    if (!ui_reader_ensure_page_loaded_locked(model->reader_page)) {
        ui_set_status_locked(model, ui_tr("no memory"));
        return;
    }
    ui_reader_save_history_locked(model);
    ui_reader_sync_status_locked(model);
}


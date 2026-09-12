static uint16_t ui_burn_rom_item_count(void)
{
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_WRITE) {
        return s_cart_mode == BURNER_CART_MODE_GBA ? UI_BURN_ROM_WRITE_PATH_ITEM_COUNT : 1U;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SAVE_PATCH) {
        return 3U;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RECIPE_MODE) {
        return UI_BURN_ROM_RECIPE_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_SIZE) {
        return (uint16_t)(UI_BURN_ROM_DUMP_SIZE_ITEM_COUNT + (ui_burn_cart_has_slots() ? 1U : 0U));
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_DUMP_CUSTOM) {
        return UI_BURN_ROM_DUMP_KEY_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_MAPPER) {
        return UI_BURN_ROM_MAPPER_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_SETTINGS) {
        return ui_burn_rom_settings_item_count();
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_ERASE_CONFIRM) {
        return UI_BURN_ROM_ERASE_CONFIRM_ITEM_COUNT;
    }
    if (s_burn_rom_submenu == UI_BURN_ROM_SUBMENU_RAM) {
        return UI_BURN_RAM_ITEM_COUNT;
    }
    if (s_cart_mode == BURNER_CART_MODE_MBC5) {
        uint16_t count = 0;
        while (ui_burn_rom_op_for_index(count) != UI_BURN_ROM_OP_INVALID) ++count;
        return count;
    }
    if (!ui_cart_is_unlocked()) {
        return (uint16_t)(UI_BURN_ROM_LOCKED_ITEM_COUNT + (ui_burn_rom_has_recipe_mode_row() ? 1U : 0U));
    }
    return UI_BURN_ROM_GBA_WITH_ROM_ITEM_COUNT;
}

static size_t ui_cart_mode_index(burner_cart_mode_t mode)
{
    return (mode == BURNER_CART_MODE_GBA) ? 1U : 0U;
}

static const ui_file_entry_t *ui_last_rom_file_for_mode(burner_cart_mode_t mode)
{
    return &s_last_rom_file_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_entry_t *ui_last_rom_file_for_mode_mut(burner_cart_mode_t mode)
{
    return &s_last_rom_file_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_kind_t ui_last_rom_kind_for_mode(burner_cart_mode_t mode)
{
    return s_last_rom_kind_by_cart[ui_cart_mode_index(mode)];
}

static ui_file_kind_t *ui_last_rom_kind_for_mode_mut(burner_cart_mode_t mode)
{
    return &s_last_rom_kind_by_cart[ui_cart_mode_index(mode)];
}

static void ui_focus_burn_rom_row_locked(ui_model_t *model, uint16_t row)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }

    count = ui_burn_rom_item_count();
    if (count == 0U) {
        model->selected = 0;
        model->scroll = 0;
        ui_mark_content_dirty(model);
        return;
    }
    if (row >= count) {
        row = (uint16_t)(count - 1U);
    }

    model->selected = row;
    model->scroll = ui_scroll_for_selected_rows(row, 0, count, ui_burn_rom_visible_rows());
    ui_mark_content_dirty(model);
}

static void ui_focus_burn_rom_op_locked(ui_model_t *model, ui_burn_rom_op_t op)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }

    count = ui_burn_rom_item_count();
    for (uint16_t i = 0U; i < count; ++i) {
        if (ui_burn_rom_op_for_index(i) == op) {
            ui_focus_burn_rom_row_locked(model, i);
            return;
        }
    }
}

static bool ui_burn_cart_has_slots(void)
{
    burner_status_t status = {0};

    burner_status_snapshot(&status);
    return status.probe_valid &&
           status.probe_cart_mode == s_cart_mode &&
           status.probe_device_size > (128U * 1024U * 1024U);
}

/* Number of cart-info rows that fit inside the left/right info panel. */
static uint16_t ui_burn_info_visible_rows(void)
{
    int32_t panel_h = UI_CANVAS_H - UI_HINT_H - UI_LIST_HEADER_H;
    int32_t rows = (panel_h - UI_BURN_INFO_ROW_Y0) / UI_LIST_LINE_H;

    return (rows > 0) ? (uint16_t)rows : 1U;
}

typedef enum {
    UI_BURN_INFO_ROW_TEXT = 0,
    UI_BURN_INFO_ROW_GAP,
    UI_BURN_INFO_ROW_HEADER,
    UI_BURN_INFO_ROW_STATIC,
    UI_BURN_INFO_ROW_NOR,
} ui_burn_info_kind_t;

/* GAP/HEADER/STATIC rows are separators or read-only text, so the cursor skips
   them and only TEXT/NOR rows can be selected. */
static bool ui_burn_info_kind_selectable(ui_burn_info_kind_t kind)
{
    return kind == UI_BURN_INFO_ROW_TEXT || kind == UI_BURN_INFO_ROW_NOR;
}

typedef void (*ui_burn_info_emit_fn_t)(
    void *ctx,
    uint16_t index,
    const char *label,
    const char *value,
    ui_burn_info_kind_t kind);

/* Walks every cart-info summary line in display order. A NULL/empty label or
   value means the other field is drawn full width; this keeps the drawing code
   and the cursor index in sync for any probe/analyze state. NOR geometry is not
   expanded here: it lives behind the selectable UI_BURN_INFO_ROW_NOR row and
   its own in-panel detail list. */
static uint16_t ui_burn_info_summary_enumerate(ui_burn_info_emit_fn_t emit, void *ctx)
{
    burner_status_t status = {0};
    uint16_t index = 0U;

#define UI_BURN_INFO_EMIT(_label, _value, _kind)             \
    do {                                                     \
        if (emit != NULL) {                                  \
            emit(ctx, index, (_label), (_value), (_kind));   \
        }                                                    \
        ++index;                                             \
    } while (0)

    burner_status_snapshot(&status);

    if (!ui_cart_is_unlocked()) {
        UI_BURN_INFO_EMIT(ui_tr("Analyze first"), "", UI_BURN_INFO_ROW_TEXT);
        UI_BURN_INFO_EMIT(ui_tr("Then operations unlock"), "", UI_BURN_INFO_ROW_TEXT);
        return index;
    }

    UI_BURN_INFO_EMIT(
        ui_tr("Type:"),
        ui_probe_type_label(&status, s_cart_mode),
        UI_BURN_INFO_ROW_TEXT);

    {
        const bool probe_ok = (status.probe_valid && status.probe_cart_mode == s_cart_mode);

        if (probe_ok) {
            if (status.probe_cart_mode == BURNER_CART_MODE_GBA) {
                char d0d1_text[24] = {0};
                char save_size_text[24] = {0};

                ui_format_gba_d0d1_text(&status, d0d1_text, sizeof(d0d1_text));
                if (status.probe_gba_save_detected && status.probe_gba_save_size > 0u) {
                    ui_format_bytes_text(status.probe_gba_save_size, save_size_text, sizeof(save_size_text));
                } else {
                    snprintf(save_size_text, sizeof(save_size_text), "--");
                }
                UI_BURN_INFO_EMIT(ui_tr("D1/D0:"), d0d1_text, UI_BURN_INFO_ROW_TEXT);
                UI_BURN_INFO_EMIT(ui_tr("Save:"), ui_gba_save_type_probe_label(&status), UI_BURN_INFO_ROW_TEXT);
                UI_BURN_INFO_EMIT(ui_tr("Size:"), save_size_text, UI_BURN_INFO_ROW_TEXT);
                if (status.probe_gba_batteryless_region_found) {
                    char batteryless_address_text[24] = {0};

                    snprintf(
                        batteryless_address_text,
                        sizeof(batteryless_address_text),
                        "0x%06" PRIX32,
                        status.probe_gba_batteryless_save_address);
                    UI_BURN_INFO_EMIT("Save area:", batteryless_address_text, UI_BURN_INFO_ROW_TEXT);
                    UI_BURN_INFO_EMIT(
                        "Save data:",
                        status.probe_gba_batteryless_data_present ? "present" : "empty",
                        UI_BURN_INFO_ROW_TEXT);
                }
                UI_BURN_INFO_EMIT(ui_tr("SRAM patch:"), ui_gba_sram_patch_label(&status), UI_BURN_INFO_ROW_TEXT);
            }
            /* CFI is independent of the NOR block below. */
            UI_BURN_INFO_EMIT(
                ui_tr("CFI:"),
                status.probe_cfi_ok ? "ok" : "fallback",
                UI_BURN_INFO_ROW_TEXT);
        } else if (s_analyzed_cart_info[0] != '\0') {
            UI_BURN_INFO_EMIT(s_analyzed_cart_info, "", UI_BURN_INFO_ROW_TEXT);
        }

        if (!probe_ok) {
            UI_BURN_INFO_EMIT(ui_tr("No probe detail"), "", UI_BURN_INFO_ROW_TEXT);
        }

        {
            const ui_file_entry_t *rom_file = ui_last_rom_file_for_mode(s_cart_mode);

            if (rom_file->path[0] != '\0') {
                UI_BURN_INFO_EMIT(ui_tr("File:"), "", UI_BURN_INFO_ROW_TEXT);
                UI_BURN_INFO_EMIT(rom_file->name, "", UI_BURN_INFO_ROW_TEXT);
            }
        }

        if (probe_ok) {
            /* NOR block at the bottom: blank spacer, divider/header, model, capacity. */
            char capacity_text[24] = {0};

            if (status.probe_device_size == 0U) {
                snprintf(capacity_text, sizeof(capacity_text), "--");
            } else if ((status.probe_device_size % (1024U * 1024U)) == 0U) {
                snprintf(
                    capacity_text,
                    sizeof(capacity_text),
                    "%" PRIu32 "MB",
                    status.probe_device_size / (1024U * 1024U));
            } else {
                ui_format_bytes_text(status.probe_device_size, capacity_text, sizeof(capacity_text));
            }
            UI_BURN_INFO_EMIT("", "", UI_BURN_INFO_ROW_GAP);
            UI_BURN_INFO_EMIT("", "NOR INFO", UI_BURN_INFO_ROW_HEADER);
            UI_BURN_INFO_EMIT("", ui_nor_model_label(&status), UI_BURN_INFO_ROW_NOR);
            UI_BURN_INFO_EMIT("", capacity_text, UI_BURN_INFO_ROW_STATIC);
        }
    }

#undef UI_BURN_INFO_EMIT
    return index;
}

/* NOR detail lines. They are drawn in the same info panel as the summary so the
   detail never takes over the operation list. */
static uint16_t ui_burn_info_nor_detail_enumerate(ui_burn_info_emit_fn_t emit, void *ctx)
{
    burner_status_t status = {0};
    uint16_t index = 0U;

#define UI_BURN_INFO_EMIT(_label, _value, _kind)             \
    do {                                                     \
        if (emit != NULL) {                                  \
            emit(ctx, index, (_label), (_value), (_kind));   \
        }                                                    \
        ++index;                                             \
    } while (0)

    burner_status_snapshot(&status);

    UI_BURN_INFO_EMIT(
        "NOR chip",
        status.probe_valid ? ui_probe_chip_name(&status) : "--",
        UI_BURN_INFO_ROW_TEXT);
    {
        char family_text[96] = {0};

        ui_format_probe_family_text(&status, family_text, sizeof(family_text));
        UI_BURN_INFO_EMIT("NOR family", family_text, UI_BURN_INFO_ROW_TEXT);
    }
    UI_BURN_INFO_EMIT("Cmdset", ui_nor_cmdset_label(ui_probe_cmdset(&status)), UI_BURN_INFO_ROW_TEXT);
    {
        char id_text[32] = {0};

        ui_format_probe_id(&status, id_text, sizeof(id_text));
        UI_BURN_INFO_EMIT("NOR ID", id_text, UI_BURN_INFO_ROW_TEXT);
    }
    {
        const bool gba = (status.probe_cart_mode == BURNER_CART_MODE_GBA);
        char manuf_text[24] = {0};
        char device_text[24] = {0};

        ui_format_probe_id_part(&status, 0U, gba ? 2U : 1U, manuf_text, sizeof(manuf_text));
        ui_format_probe_id_part(&status, gba ? 2U : 1U, 2U, device_text, sizeof(device_text));
        UI_BURN_INFO_EMIT("Manuf ID", manuf_text, UI_BURN_INFO_ROW_TEXT);
        UI_BURN_INFO_EMIT("Device ID", device_text, UI_BURN_INFO_ROW_TEXT);
    }
    {
        char size_text[24] = {0};
        char sector_text[24] = {0};
        char count_text[32] = {0};

        if (status.probe_device_size > 0U) {
            ui_format_bytes_text(status.probe_device_size, size_text, sizeof(size_text));
        } else {
            snprintf(size_text, sizeof(size_text), "--");
        }
        if (status.probe_sector_size > 0U) {
            ui_format_bytes_text(status.probe_sector_size, sector_text, sizeof(sector_text));
            snprintf(
                count_text,
                sizeof(count_text),
                "%" PRIu32,
                status.probe_device_size / status.probe_sector_size);
        } else {
            snprintf(sector_text, sizeof(sector_text), "--");
            snprintf(count_text, sizeof(count_text), "--");
        }
        UI_BURN_INFO_EMIT(ui_tr("Capacity"), size_text, UI_BURN_INFO_ROW_TEXT);
        UI_BURN_INFO_EMIT(ui_tr("Sector"), sector_text, UI_BURN_INFO_ROW_TEXT);
        UI_BURN_INFO_EMIT(ui_tr("Count"), count_text, UI_BURN_INFO_ROW_TEXT);
    }
    {
        char buffer_text[24] = {0};

        if (status.probe_buffer_write_bytes > 0U) {
            snprintf(buffer_text, sizeof(buffer_text), "%u B", (unsigned)status.probe_buffer_write_bytes);
        } else {
            snprintf(buffer_text, sizeof(buffer_text), "--");
        }
        UI_BURN_INFO_EMIT("Buffer", buffer_text, UI_BURN_INFO_ROW_TEXT);
    }
    UI_BURN_INFO_EMIT(
        "CFI",
        !status.probe_valid ? "--" : (status.probe_cfi_ok ? "ok" : "fallback"),
        UI_BURN_INFO_ROW_TEXT);

#undef UI_BURN_INFO_EMIT
    return index;
}

/* Active info-panel list: NOR detail while open, otherwise the summary. */
static uint16_t ui_burn_info_enumerate(ui_burn_info_emit_fn_t emit, void *ctx)
{
    if (s_burner_info_nor_detail) {
        return ui_burn_info_nor_detail_enumerate(emit, ctx);
    }
    return ui_burn_info_summary_enumerate(emit, ctx);
}

static uint16_t ui_burn_info_item_count(void)
{
    return ui_burn_info_enumerate(NULL, NULL);
}

static void ui_burn_info_find_nor(
    void *ctx,
    uint16_t index,
    const char *label,
    const char *value,
    ui_burn_info_kind_t kind)
{
    uint16_t *out = (uint16_t *)ctx;

    (void)label;
    (void)value;
    if (out != NULL && *out == UINT16_MAX && kind == UI_BURN_INFO_ROW_NOR) {
        *out = index;
    }
}

typedef struct {
    uint16_t index;
    ui_burn_info_kind_t kind;
    bool found;
} ui_burn_info_kind_query_t;

static void ui_burn_info_kind_at_cb(
    void *ctx,
    uint16_t index,
    const char *label,
    const char *value,
    ui_burn_info_kind_t kind)
{
    ui_burn_info_kind_query_t *query = (ui_burn_info_kind_query_t *)ctx;

    (void)label;
    (void)value;
    if (query != NULL && !query->found && index == query->index) {
        query->kind = kind;
        query->found = true;
    }
}

static ui_burn_info_kind_t ui_burn_info_kind_at(uint16_t index)
{
    ui_burn_info_kind_query_t query = {.index = index};

    (void)ui_burn_info_enumerate(ui_burn_info_kind_at_cb, &query);
    return query.found ? query.kind : UI_BURN_INFO_ROW_TEXT;
}

/* Moves the cursor one entry, skipping separator (gap/header) rows. */
static void ui_burn_info_move(int delta)
{
    uint16_t count = ui_burn_info_item_count();
    uint16_t next;

    if (count == 0U || delta == 0) {
        return;
    }
    next = (s_burner_info_selected < count) ? s_burner_info_selected : 0U;
    if (delta > 0) {
        while (next + 1U < count) {
            ++next;
            if (ui_burn_info_kind_selectable(ui_burn_info_kind_at(next))) {
                s_burner_info_selected = next;
                return;
            }
        }
    } else {
        while (next > 0U) {
            --next;
            if (ui_burn_info_kind_selectable(ui_burn_info_kind_at(next))) {
                s_burner_info_selected = next;
                return;
            }
        }
    }
}

/* Index of the selectable NOR model row in the summary list, or UINT16_MAX
   when absent. */
static uint16_t ui_burn_info_nor_index(void)
{
    uint16_t index = UINT16_MAX;

    if (s_burner_info_nor_detail) {
        return UINT16_MAX;
    }
    (void)ui_burn_info_summary_enumerate(ui_burn_info_find_nor, &index);
    return index;
}

static void ui_burn_rom_open_nor_info_locked(ui_model_t *model)
{
    burner_status_t status = {0};

    if (model == NULL) {
        return;
    }
    burner_status_snapshot(&status);
    s_burner_info_nor_detail = true;
    s_burner_info_selected = 0U;
    s_burner_info_scroll = 0U;
    ui_set_status_locked(model, ui_probe_chip_name(&status));
    ui_mark_content_dirty(model);
}

static void ui_burn_info_ensure_visible(void)
{
    uint16_t count = ui_burn_info_item_count();

    if (count == 0U) {
        s_burner_info_selected = 0U;
        s_burner_info_scroll = 0U;
        return;
    }
    if (s_burner_info_selected >= count) {
        s_burner_info_selected = (uint16_t)(count - 1U);
    }
    while (s_burner_info_selected > 0U &&
           !ui_burn_info_kind_selectable(ui_burn_info_kind_at(s_burner_info_selected))) {
        --s_burner_info_selected;
    }
    s_burner_info_scroll = ui_scroll_for_selected_rows(
        s_burner_info_selected,
        s_burner_info_scroll,
        count,
        ui_burn_info_visible_rows());
}

static void ui_burn_info_nor_close_locked(ui_model_t *model)
{
    uint16_t nor_index;

    if (model == NULL || !s_burner_info_nor_detail) {
        return;
    }
    s_burner_info_nor_detail = false;
    s_burner_info_scroll = 0U;
    nor_index = ui_burn_info_nor_index();
    if (nor_index != UINT16_MAX) {
        s_burner_info_selected = nor_index;
    }
    ui_burn_info_ensure_visible();
    ui_mark_content_dirty(model);
}

static void ui_clamp_burn_rom_selection_locked(ui_model_t *model)
{
    uint16_t count;

    if (model == NULL || model->page != UI_PAGE_BURN_ROM) {
        return;
    }
    count = ui_burn_rom_item_count();
    if (count == 0U) {
        model->selected = 0;
        model->scroll = 0;
        return;
    }
    if (model->selected >= count) {
        model->selected = (uint16_t)(count - 1U);
    }
    model->scroll = ui_scroll_for_selected_rows(model->selected, model->scroll, count, ui_burn_rom_visible_rows());
}


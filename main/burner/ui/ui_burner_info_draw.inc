static void ui_px_draw_burner_cart_info(const ui_model_t *model)
{
    burner_status_t status = {0};
    char size_text[24] = {0};
    char sector_text[24] = {0};
    char sector_count_text[32] = {0};
    char d0d1_text[24] = {0};
    char id_text[32] = {0};
    char batteryless_address_text[24] = {0};
    char batteryless_data_text[16] = {0};
    int32_t y0 = UI_LIST_HEADER_H + (int32_t)(UI_BURN_ROM_ACTION_ROWS + 1U) * UI_LIST_LINE_H;
    int32_t w = UI_CANVAS_W - UI_LIST_BAR_W - 12;

    (void)model;
    burner_status_snapshot(&status);
    ui_px_hline(4, y0 - 4, w, true);
    ui_px_text(UI_LIST_TEXT_X, y0 + 2, ui_tr("Cart info"), true);
    if (!ui_cart_is_unlocked()) {
        ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 20, w, ui_tr("Only Analyze cart is available"), true);
        return;
    }

    if (status.probe_valid && status.probe_cart_mode == s_cart_mode) {
        ui_format_bytes_text(status.probe_device_size, size_text, sizeof(size_text));
        ui_format_bytes_text(status.probe_sector_size, sector_text, sizeof(sector_text));
        if (status.probe_sector_size > 0U) {
            snprintf(sector_count_text, sizeof(sector_count_text), "%" PRIu32, status.probe_device_size / status.probe_sector_size);
        } else {
            snprintf(sector_count_text, sizeof(sector_count_text), "--");
        }
        ui_format_probe_id(&status, id_text, sizeof(id_text));
        ui_format_gba_d0d1_text(&status, d0d1_text, sizeof(d0d1_text));
        ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 20, w, ui_probe_chip_name(&status), true);
        if (status.probe_cart_mode == BURNER_CART_MODE_GBA) {
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 36, w, "D1/D0", true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 36, w - 72, d0d1_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 52, w, ui_tr("Capacity"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 52, w - 72, size_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 68, w, ui_tr("Sector"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 68, w - 72, sector_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 148, y0 + 68, w - 148, ui_tr("Count"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 216, y0 + 68, w - 216, sector_count_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 84, w, "NOR ID", true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 84, w - 72, id_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 100, w, ui_tr("SRAM patch"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 100, w - 72, ui_gba_sram_patch_label(&status), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 148, y0 + 36, w - 148, ui_tr("Save"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 216, y0 + 36, w - 216, ui_gba_save_type_probe_label(&status), true);
            if (status.probe_gba_batteryless_region_found) {
                snprintf(
                    batteryless_address_text,
                    sizeof(batteryless_address_text),
                    "0x%06" PRIX32,
                    status.probe_gba_batteryless_save_address);
                snprintf(
                    batteryless_data_text,
                    sizeof(batteryless_data_text),
                    "%s",
                    status.probe_gba_batteryless_data_present ? "present" : "empty");
                ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 116, w, "Save area", true);
                ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 116, w - 72, batteryless_address_text, true);
                ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 132, w, "Data", true);
                ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 132, w - 72, batteryless_data_text, true);
            }
        } else {
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 36, w, ui_tr("Capacity"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 36, w - 72, size_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 52, w, ui_tr("Sector"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 52, w - 72, sector_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 148, y0 + 52, w - 148, ui_tr("Count"), true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 216, y0 + 52, w - 216, sector_count_text, true);
            ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 68, w, "NOR ID", true);
            ui_px_text_clipped(UI_LIST_TEXT_X + 72, y0 + 68, w - 72, id_text, true);
        }
    } else {
        ui_px_text_clipped(UI_LIST_TEXT_X, y0 + 20, w, s_analyzed_cart_info[0] != '\0' ? s_analyzed_cart_info : ui_tr("analyzed"), true);
    }
}


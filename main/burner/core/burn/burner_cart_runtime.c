/* Cartridge prepare, capacity probe, and unlock control helpers. */

static esp_err_t burner_bacon_mbc5_prepare(uint32_t total_bytes)
{
    uint8_t id[4];
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    bool cfi_ok = false;
    burner_nor_cmdset_t cmdset = BURNER_NOR_CMDSET_UNKNOWN;

    if (total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    return burner_bacon_mbc5_prepare_probe_info_locked(
        id,
        total_bytes,
        &device_size,
        &sector_size,
        &buffer_write_bytes,
        &cfi_ok,
        &cmdset,
        NULL);
}

static esp_err_t burner_bacon_mbc5_program_block(const uint8_t *data, size_t len, uint32_t offset)
{
    size_t programmed = 0;
    esp_err_t err;

    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_cart_ctx.prepared) {
        return ESP_ERR_INVALID_STATE;
    }

    while (programmed < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)programmed;
        uint16_t bank = 0u;
        uint32_t bank_off = 0u;
        uint16_t cart_addr = 0u;
        size_t remain = len - programmed;
        size_t bank_remain;
        size_t chunk;

        burner_mbc5_addr_to_program_window(rom_addr, &bank, &cart_addr, &bank_off);
        bank_remain = BURN_MBC5_ROM_BANK_BYTES - bank_off;
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        err = burner_bacon_gbc_rom_program(
            cart_addr,
            data + programmed,
            chunk,
            s_cart_ctx.buffer_write_bytes);
        if (err != ESP_OK) {
            return err;
        }

        programmed += chunk;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static bool burner_is_gba_multi_card(const burner_task_param_t *job)
{
    if (job == NULL || job->cart_mode != BURNER_CART_MODE_GBA) {
        return false;
    }
    if (job->gba_force_multi) {
        return true;
    }
    return s_cart_ctx.device_size > BURN_GBA_LINEAR_ADDR_BYTES;
}

esp_err_t burner_spi_prepare_burn_mbc5(const burner_task_param_t *job)
{
    esp_err_t err;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_mbc5_prepare_power();
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_mbc5_prepare(job->total_bytes);
}

esp_err_t burner_spi_prepare_burn_gba(const burner_task_param_t *job)
{
    esp_err_t err;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_gba_prepare_power();
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gba_prepare(job);
}

static esp_err_t burner_spi_prepare_ram(void)
{
    esp_err_t err;

    err = burner_bacon_mbc5_prepare_power();
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_mbc5_ram_enable(true);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

esp_err_t burner_probe_cart_capacity_bytes(burner_cart_mode_t cart_mode, uint32_t *device_size_out)
{
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    bool cfi_ok = false;
    esp_err_t err = ESP_OK;

    if (device_size_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_spi_init();
    if (err != ESP_OK) {
        return err;
    }

    burner_spi_lock_take();
    if (cart_mode == BURNER_CART_MODE_GBA) {
        uint8_t gba_id[8] = {0};

        err = burner_bacon_gba_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_gba_probe_locked(
                gba_id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok);
        }
        if (err == ESP_OK && !cfi_ok) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA capacity probe: CFI unavailable: flash=%" PRIu32
                " sector=%" PRIu32 " buf=%u cmdset=%s id=%02X %02X %02X %02X %02X %02X %02X %02X",
                device_size,
                sector_size,
                (unsigned)buffer_write_bytes,
                burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
                gba_id[0],
                gba_id[1],
                gba_id[2],
                gba_id[3],
                gba_id[4],
                gba_id[5],
                gba_id[6],
                gba_id[7]);
        }
    } else {
        uint8_t mbc5_id[4] = {0};
        burner_nor_cmdset_t mbc5_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

        err = burner_bacon_mbc5_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_probe_locked(
                mbc5_id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok,
                &mbc5_cmdset);
        }
        if (err == ESP_OK && !cfi_ok) {
            ESP_LOGW(
                BURNER_TAG,
                "GB capacity probe: mapper=%s CFI unavailable: flash=%" PRIu32
                " sector=%" PRIu32 " buf=%u cmdset=%s id=%02X %02X %02X %02X",
                burner_gb_mapper_name(s_gb_mapper_kind),
                device_size,
                sector_size,
                (unsigned)buffer_write_bytes,
                burner_nor_cmdset_name(mbc5_cmdset),
                mbc5_id[0],
                mbc5_id[1],
                mbc5_id[2],
                mbc5_id[3]);
        }
        if (err == ESP_OK && s_gb_mapper_override_kind != BURNER_GB_MAPPER_UNKNOWN) {
            s_gb_mapper_kind = s_gb_mapper_override_kind;
            if (s_gb_mapper_override_kind == BURNER_GB_MAPPER_MBC3 &&
                device_size > (2u * 1024u * 1024u)) {
                device_size = (2u * 1024u * 1024u);
            }
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        return err;
    }

    *device_size_out = device_size;
    return ESP_OK;
}

static esp_err_t burner_bacon_gba_reset_aso_locked(void)
{
    esp_err_t err;

    err = burner_bacon_gba_command_write_u16(0x000u, 0x0090u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(0x000u, 0x0000u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gba_reset_to_read_mode();
}

static esp_err_t burner_bacon_mbc5_reset_aso_locked(void)
{
    static const uint8_t cmd_90 = 0x90u;
    static const uint8_t cmd_00 = 0x00u;
    static const uint8_t cmd_f0 = 0xF0u;
    esp_err_t err;

    err = burner_bacon_gbc_write(0x0000u, &cmd_90, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0000u, &cmd_00, 1u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gbc_write(0x0000u, &cmd_f0, 1u);
}

static esp_err_t burner_bacon_gba_get_ppb_lock_status_locked(uint16_t *lock_status_out)
{
    esp_err_t err;

    if (lock_status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0050u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_read_u16(0x000u, lock_status_out);
    if (err != ESP_OK) {
        return err;
    }
    *lock_status_out = burner_apply_d0d1_swap_on_read(*lock_status_out, s_cart_ctx.d0d1_swapped);
    return burner_bacon_gba_reset_aso_locked();
}

static esp_err_t burner_bacon_mbc5_get_ppb_lock_status_locked(uint8_t *lock_status_out)
{
    static const uint8_t cmd_aa = 0xAAu;
    static const uint8_t cmd_55 = 0x55u;
    static const uint8_t cmd_50 = 0x50u;
    esp_err_t err;

    if (lock_status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_aa, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0555u, &cmd_55, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_50, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_read_u8(0x0000u, lock_status_out);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_mbc5_reset_aso_locked();
}

static esp_err_t burner_bacon_gba_scan_ppb_locked(
    uint32_t device_size,
    uint32_t sector_size,
    uint32_t *needs_unlock_count_out)
{
    uint32_t sector_count;
    uint32_t sector_idx;
    uint16_t ppb = 0u;
    esp_err_t err;

    if (needs_unlock_count_out == NULL || sector_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    *needs_unlock_count_out = 0u;
    sector_count = burner_erase_sector_count_from_bytes(device_size, sector_size);
    for (sector_idx = 0u; sector_idx < sector_count; ++sector_idx) {
        uint32_t sector_addr = sector_idx * sector_size;
        uint32_t ppb_addr = sector_addr;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        if (device_size > BURN_GBA_LINEAR_ADDR_BYTES) {
            uint32_t bank = 0u;
            burner_gba_resolve_write_addr(sector_addr, true, &bank, NULL);
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00C0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(ppb_addr >> 1, &ppb);
        if (err != ESP_OK) {
            return err;
        }
        ppb = burner_apply_d0d1_swap_on_read(ppb, s_cart_ctx.d0d1_swapped);
        err = burner_bacon_gba_reset_aso_locked();
        if (err != ESP_OK) {
            return err;
        }

        if (ppb != 0x0001u) {
            (*needs_unlock_count_out)++;
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_scan_ppb_locked(
    uint32_t device_size,
    uint32_t sector_size,
    uint32_t *needs_unlock_count_out)
{
    uint32_t sector_count;
    uint32_t sector_idx;
    uint16_t current_bank = UINT16_MAX;
    esp_err_t err;

    if (needs_unlock_count_out == NULL || sector_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    *needs_unlock_count_out = 0u;
    sector_count = burner_erase_sector_count_from_bytes(device_size, sector_size);
    for (sector_idx = 0u; sector_idx < sector_count; ++sector_idx) {
        uint32_t sector_addr = sector_idx * sector_size;
        uint16_t bank = 0u;
        uint16_t cart_addr = 0u;
        uint8_t ppb = 0u;
        static const uint8_t cmd_aa = 0xAAu;
        static const uint8_t cmd_55 = 0x55u;
        static const uint8_t cmd_c0 = 0xC0u;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        burner_mbc5_addr_to_program_window(sector_addr, &bank, &cart_addr, NULL);
        if (bank != current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        err = burner_bacon_gbc_write(0x0AAAu, &cmd_aa, 1u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_write(0x0555u, &cmd_55, 1u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_write(0x0AAAu, &cmd_c0, 1u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(cart_addr, &ppb);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_mbc5_reset_aso_locked();
        if (err != ESP_OK) {
            return err;
        }

        if (ppb != 0x01u) {
            (*needs_unlock_count_out)++;
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_all_ppb_erase_locked(void)
{
    esp_err_t err;

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00C0u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(0x000u, 0x0080u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(0x000u, 0x0030u);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    return burner_bacon_gba_reset_aso_locked();
}

static esp_err_t burner_bacon_mbc5_all_ppb_erase_locked(void)
{
    static const uint8_t cmd_aa = 0xAAu;
    static const uint8_t cmd_55 = 0x55u;
    static const uint8_t cmd_c0 = 0xC0u;
    static const uint8_t cmd_80 = 0x80u;
    static const uint8_t cmd_30 = 0x30u;
    esp_err_t err;

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_aa, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0555u, &cmd_55, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0AAAu, &cmd_c0, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0000u, &cmd_80, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x0000u, &cmd_30, 1u);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    return burner_bacon_mbc5_reset_aso_locked();
}

esp_err_t burner_cart_unlock_ppb_locked(
    burner_cart_mode_t cart_mode,
    burner_ppb_unlock_report_t *report)
{
    esp_err_t err = ESP_OK;

    if (report == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(report, 0, sizeof(*report));
    report->cart_mode = cart_mode;

    if (cart_mode == BURNER_CART_MODE_GBA) {
        err = burner_bacon_gba_prepare_power();
        if (err != ESP_OK) {
            return err;
        }

        err = burner_bacon_gba_probe_locked(
            report->gba_id,
            &report->device_size,
            &report->sector_size,
            &report->buffer_write_bytes,
            &report->cfi_ok);
        if (err != ESP_OK) {
            return err;
        }
        report->gba_d0d1_known = s_cart_ctx.d0d1_known;
        report->gba_d0d1_swapped = s_cart_ctx.d0d1_swapped;
        if (report->device_size == 0u || report->sector_size == 0u) {
            return ESP_ERR_INVALID_SIZE;
        }

        report->sector_count = burner_erase_sector_count_from_bytes(report->device_size, report->sector_size);

        err = burner_bacon_gba_get_ppb_lock_status_locked(&report->gba_lock_status);
        if (err != ESP_OK) {
            return err;
        }
        if (report->gba_lock_status != 0x0001u) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        err = burner_bacon_gba_scan_ppb_locked(
            report->device_size,
            report->sector_size,
            &report->ppb_needs_unlock_before);
        if (err != ESP_OK) {
            return err;
        }

        err = burner_bacon_gba_all_ppb_erase_locked();
        if (err != ESP_OK) {
            return err;
        }

        return burner_bacon_gba_scan_ppb_locked(
            report->device_size,
            report->sector_size,
            &report->ppb_needs_unlock_after);
    }

    err = burner_bacon_mbc5_prepare_power();
    if (err != ESP_OK) {
        return err;
    }

    {
        burner_nor_cmdset_t mbc5_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

        err = burner_bacon_mbc5_probe_locked(
        report->mbc5_id,
        &report->device_size,
        &report->sector_size,
        &report->buffer_write_bytes,
        &report->cfi_ok,
        &mbc5_cmdset);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (report->device_size == 0u || report->sector_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    report->sector_count = burner_erase_sector_count_from_bytes(report->device_size, report->sector_size);

    err = burner_bacon_mbc5_get_ppb_lock_status_locked(&report->mbc5_lock_status);
    if (err != ESP_OK) {
        return err;
    }
    if (report->mbc5_lock_status != 0x01u) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = burner_bacon_mbc5_scan_ppb_locked(
        report->device_size,
        report->sector_size,
        &report->ppb_needs_unlock_before);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_mbc5_all_ppb_erase_locked();
    if (err != ESP_OK) {
        return err;
    }

    return burner_bacon_mbc5_scan_ppb_locked(
        report->device_size,
        report->sector_size,
        &report->ppb_needs_unlock_after);
}


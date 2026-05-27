/* Low-level GBC/MBC5 flash bank, erase, and program helpers. */

static esp_err_t burner_bacon_mbc5_switch_bank(uint16_t bank)
{
    uint8_t b0 = (uint8_t)(bank & 0xFFu);
    uint8_t b1 = (uint8_t)((bank >> 8) & 0xFFu);
    esp_err_t err;

    if (s_gb_mapper_kind == BURNER_GB_MAPPER_MBC3) {
        b0 = burner_gb_mapper_normalize_rom_bank(s_gb_mapper_kind, bank);
        return burner_bacon_gbc_write(0x2000u, &b0, 1u);
    }

    /*
     * Match ChisFlashBurner mission_mbc5.cs:
     * write low 8 bits at 0x2000, then high bit at 0x3000.
     */
    err = burner_bacon_gbc_write(0x2000u, &b0, 1u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gbc_write(0x3000u, &b1, 1u);
}

static esp_err_t burner_bacon_mbc5_ram_switch_bank(uint8_t bank)
{
    if (s_gb_mapper_kind == BURNER_GB_MAPPER_MBC3) {
        bank &= 0x07u;
    }
    return burner_bacon_gbc_write(0x4000u, &bank, 1);
}

static esp_err_t burner_bacon_mbc5_ram_enable(bool enable)
{
    uint8_t cmd = enable ? 0x0Au : 0x00u;
    return burner_bacon_gbc_write(0x0000u, &cmd, 1);
}

static esp_err_t burner_bacon_wait_u8(uint16_t addr, uint8_t expected, uint32_t timeout_ms)
{
    int64_t deadline_us;
    uint8_t read_back = 0;
    esp_err_t err;

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(addr, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == expected) {
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    return ESP_ERR_TIMEOUT;
}

static uint32_t burner_erase_timeout_ms_for_bytes(uint32_t bytes)
{
    uint32_t mb;
    uint64_t timeout_ms;

    if (bytes == 0u) {
        return BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS;
    }

    mb = (bytes + ((1024u * 1024u) - 1u)) / (1024u * 1024u);
    if (mb == 0u) {
        mb = 1u;
    }
    timeout_ms = (uint64_t)mb * (uint64_t)BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS;
    if (timeout_ms > BURNER_ROM_ERASE_TIMEOUT_MAX_MS) {
        timeout_ms = BURNER_ROM_ERASE_TIMEOUT_MAX_MS;
    }
    return (uint32_t)timeout_ms;
}

static uint32_t burner_erase_remaining_timeout_ms(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();

    if (remaining_us <= 0) {
        return 0u;
    }
    return (uint32_t)((remaining_us + 999LL) / 1000LL);
}

static bool burner_buffer_all_equal(const uint8_t *left, const uint8_t *right, size_t len)
{
    return left != NULL && right != NULL && len > 0u && memcmp(left, right, len) == 0;
}

static esp_err_t burner_bacon_mbc5_erase_sector(uint32_t flash_addr, uint32_t timeout_ms)
{
    uint16_t bank = 0u;
    uint16_t cart_addr = 0u;
    uint8_t cmd;
    esp_err_t err;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    burner_mbc5_addr_to_program_window(flash_addr, &bank, &cart_addr, NULL);
    err = burner_bacon_mbc5_switch_bank(bank);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = bank;

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x80u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x30u;
    err = burner_bacon_gbc_write(cart_addr, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_wait_u8(cart_addr, 0xFFu, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        uint8_t read_back = 0u;
        (void)burner_bacon_gbc_read_u8(cart_addr, &read_back);
        ESP_LOGW(
            BURNER_TAG,
            "MBC5 erase timeout flash=0x%08" PRIX32 " bank=%u cart_addr=0x%04X sector=%" PRIu32
            " read=0x%02X timeout=%ums",
            flash_addr,
            (unsigned)bank,
            (unsigned)cart_addr,
            burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry),
            (unsigned)read_back,
            (unsigned)timeout_ms);
    }
    return err;
}

static esp_err_t burner_buffer_all_ff(const uint8_t *buf, size_t len, bool *all_ff_out)
{
    if (buf == NULL || all_ff_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != 0xFFu) {
            *all_ff_out = false;
            return ESP_OK;
        }
    }
    *all_ff_out = true;
    return ESP_OK;
}

static size_t burner_blank_head_check_len(uint32_t region_size)
{
    return (region_size < BURN_BLANK_HEAD_CHECK_BYTES) ? (size_t)region_size : (size_t)BURN_BLANK_HEAD_CHECK_BYTES;
}

static esp_err_t burner_mbc5_region_is_blank_head(
    uint32_t region_addr,
    uint32_t region_size,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_HEAD_CHECK_BYTES];
    size_t sample_len;
    esp_err_t err;

    if (blank_out == NULL || region_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_len = burner_blank_head_check_len(region_size);
    err = burner_bacon_mbc5_read_block_program_window(sample_buf, sample_len, region_addr);
    if (err != ESP_OK) {
        return err;
    }
    return burner_buffer_all_ff(sample_buf, sample_len, blank_out);
}

static esp_err_t burner_gba_region_is_blank_head(
    uint32_t region_addr,
    uint32_t region_size,
    bool is_multi_card,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_HEAD_CHECK_BYTES];
    size_t sample_len;
    esp_err_t err;

    if (blank_out == NULL || region_size == 0u || (region_size & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_len = burner_blank_head_check_len(region_size);
    if ((sample_len & 0x1u) != 0u) {
        sample_len -= 1u;
    }
    if (sample_len == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = burner_bacon_gba_read_block(sample_buf, sample_len, region_addr, is_multi_card);
    if (err != ESP_OK) {
        return err;
    }
    return burner_buffer_all_ff(sample_buf, sample_len, blank_out);
}

static bool burner_blank_sample_offset_seen(const uint32_t *offsets, size_t count, uint32_t offset)
{
    for (size_t i = 0u; i < count; ++i) {
        if (offsets[i] == offset) {
            return true;
        }
    }
    return false;
}

static size_t burner_build_blank_sample_offsets(
    uint32_t region_size,
    size_t sample_len,
    uint32_t align_mask,
    uint32_t offsets[BURN_BLANK_SAMPLE_POINTS])
{
    uint32_t candidates[BURN_BLANK_SAMPLE_POINTS];
    uint32_t max_offset;
    size_t count = 0u;

    if (offsets == NULL || region_size == 0u || sample_len == 0u) {
        return 0u;
    }

    max_offset = (region_size > (uint32_t)sample_len) ? (region_size - (uint32_t)sample_len) : 0u;
    candidates[0] = 0u;
    candidates[1] = (uint32_t)(((uint64_t)max_offset * 30u) / 100u);
    candidates[2] = (uint32_t)(((uint64_t)max_offset * 70u) / 100u);
    candidates[3] = max_offset;

    for (size_t i = 0u; i < BURN_BLANK_SAMPLE_POINTS; ++i) {
        uint32_t offset = candidates[i];

        if (align_mask != 0u) {
            offset &= ~align_mask;
        }
        if (!burner_blank_sample_offset_seen(offsets, count, offset)) {
            offsets[count++] = offset;
        }
    }

    return count;
}

static esp_err_t burner_mbc5_region_is_blank_sampled(
    uint32_t region_addr,
    uint32_t region_size,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_SAMPLE_BYTES];
    uint32_t sample_offsets[BURN_BLANK_SAMPLE_POINTS];
    size_t sample_len;
    size_t sample_count;
    esp_err_t err;

    if (blank_out == NULL || region_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_len = (region_size < BURN_BLANK_SAMPLE_BYTES) ? (size_t)region_size : (size_t)BURN_BLANK_SAMPLE_BYTES;
    sample_count = burner_build_blank_sample_offsets(region_size, sample_len, 0u, sample_offsets);
    if (sample_count == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *blank_out = true;
    for (size_t i = 0u; i < sample_count; ++i) {
        bool chunk_blank = false;

        err = burner_bacon_mbc5_read_block_program_window(
            sample_buf,
            sample_len,
            region_addr + sample_offsets[i]);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_buffer_all_ff(sample_buf, sample_len, &chunk_blank);
        if (err != ESP_OK) {
            return err;
        }
        if (!chunk_blank) {
            *blank_out = false;
            return ESP_OK;
        }
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static esp_err_t burner_mbc5_sector_is_blank(
    uint32_t sector_addr,
    uint32_t sector_size,
    bool *blank_out)
{
    return burner_mbc5_region_is_blank_sampled(sector_addr, sector_size, blank_out);
}

static esp_err_t burner_bacon_mbc5_erase_range(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool sample_blank_sectors,
    bool erase_always)
{
    const burner_nor_geometry_t *geometry = &s_cart_ctx.geometry;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr = 0u;
    uint32_t skipped_blank = 0u;
    uint32_t erased = 0u;
    uint32_t erase_bytes;
    uint32_t timeout_ms;
    int64_t erase_deadline_us;
    esp_err_t err = ESP_OK;

    (void)sector_size;
    if (!burner_nor_geometry_is_valid(geometry) || addr_end < addr_begin) {
        return ESP_ERR_INVALID_ARG;
    }
    if (burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    sector_addr = cursor.addr_begin + (((addr_begin - cursor.addr_begin) / cursor.sector_size) * cursor.sector_size);

    erase_bytes = burner_nor_geometry_erase_bytes_from_range(geometry, addr_begin, addr_end);
    if (erase_bytes == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    timeout_ms = burner_erase_timeout_ms_for_bytes(erase_bytes);
    erase_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    {
        uint16_t first_bank = 0u;
        uint16_t first_cart_addr = 0u;

        burner_mbc5_addr_to_program_window(sector_addr, &first_bank, &first_cart_addr, NULL);
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 erase plan: range=0x%08" PRIX32 "-0x%08" PRIX32
            " first_sector=0x%08" PRIX32 " first_bank=%u first_cart=0x%04X bytes=%" PRIu32
            " timeout=%" PRIu32 "ms geom=%s largest=%" PRIu32 " regions=%u",
            addr_begin,
            addr_end,
            sector_addr,
            (unsigned)first_bank,
            (unsigned)first_cart_addr,
            erase_bytes,
            timeout_ms,
            burner_nor_geometry_is_uniform(geometry) ? "uniform" : "mixed",
            burner_nor_geometry_largest_sector_size(geometry),
            (unsigned)geometry->region_count);
    }

    while (sector_addr <= addr_end) {
        uint32_t current_sector_size = cursor.sector_size;
        uint32_t region_end_addr = cursor.addr_end - 1u;
        uint32_t region_limit_addr = (addr_end < region_end_addr) ? addr_end : region_end_addr;

        while (sector_addr <= region_limit_addr) {
            uint16_t sector_bank = 0u;
            uint16_t sector_cart_addr = 0u;

            err = burner_cancel_poll();
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            burner_mbc5_addr_to_program_window(sector_addr, &sector_bank, &sector_cart_addr, NULL);
            ESP_LOGI(
                BURNER_TAG,
                "MBC5 erase sector: flash=0x%08" PRIX32 " size=%" PRIu32 " bank=%u cart=0x%04X",
                sector_addr,
                current_sector_size,
                (unsigned)sector_bank,
                (unsigned)sector_cart_addr);
            if (sample_blank_sectors && !erase_always) {
                bool blank = false;

                err = burner_mbc5_sector_is_blank(
                    sector_addr,
                    current_sector_size,
                    &blank);
                if (err != ESP_OK) {
                    goto erase_range_out;
                }
                if (blank) {
                    skipped_blank++;
                    burner_status_advance_erase_phase(1u, current_sector_size);
                    sector_addr += current_sector_size;
                    continue;
                }
            }
            err = burner_bacon_mbc5_erase_sector(
                sector_addr,
                burner_erase_remaining_timeout_ms(erase_deadline_us));
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            erased++;
            burner_status_advance_erase_phase(1u, current_sector_size);
            sector_addr += current_sector_size;
        }
        if (region_limit_addr >= addr_end) {
            break;
        }
        err = burner_nor_geometry_region_cursor_advance(geometry, &cursor);
        if (err != ESP_OK) {
            break;
        }
        sector_addr = cursor.addr_begin;
    }

erase_range_out:
    if (err == ESP_OK && sample_blank_sectors && !erase_always &&
        (erased > 0u || skipped_blank > 0u)) {
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 erase sector-sample: 4x2B erased=%" PRIu32 " skipped_blank=%" PRIu32,
            erased,
            skipped_blank);
    }
    return err;
}

static esp_err_t burner_bacon_mbc5_chip_erase(void)
{
    uint8_t cmd;
    uint8_t read_back = 0;
    int64_t deadline_us;
    esp_err_t err;

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = 0u;

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x80u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x10u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)BURNER_ROM_CHIP_ERASE_TIMEOUT_MS * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(0x0000u, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == 0xFFu) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gbc_rom_program(
    uint16_t cart_addr,
    const uint8_t *buf,
    size_t len,
    uint16_t buffer_write_bytes)
{
    size_t i = 0;
    esp_err_t err;

    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (i < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint16_t start_addr = (uint16_t)(cart_addr + (uint16_t)i);

        if (buffer_write_bytes == 0u) {
            uint8_t seq[24];

            seq[0] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[1] = 0xAAu;
            seq[2] = 0x0Au;
            seq[3] = 0xAAu;
            seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[5] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[6] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[7] = 0x55u;
            seq[8] = 0x05u;
            seq[9] = 0x55u;
            seq[10] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[11] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[12] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[13] = 0xAAu;
            seq[14] = 0x0Au;
            seq[15] = 0xA0u;
            seq[16] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[17] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[18] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[19] = (uint8_t)(start_addr & 0xFFu);
            seq[20] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[21] = buf[i];
            seq[22] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[23] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

            err = burner_spi_transfer(seq, NULL, sizeof(seq));
            if (err != ESP_OK) {
                return err;
            }
            err = burner_bacon_wait_u8(start_addr, buf[i], BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
            i += 1u;
        } else {
            size_t wr_buf_cnt;
            size_t write_len = len - i;
            size_t seq_len;
            uint8_t *seq;
            uint8_t last_expected;
            uint16_t last_addr;
            bool fallback_to_single = false;

            if (write_len > buffer_write_bytes) {
                write_len = buffer_write_bytes;
            }

            seq_len = 25u + 3u * write_len;
            if (seq_len > BURNER_SPI_MAX_XFER) {
                return ESP_ERR_INVALID_SIZE;
            }

            seq = (uint8_t *)malloc(seq_len);
            if (seq == NULL) {
                return ESP_ERR_NO_MEM;
            }

            seq[0] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[1] = 0xAAu;
            seq[2] = 0xAAu;
            seq[3] = 0x0Au;
            seq[4] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[5] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[6] = 0x55u;
            seq[7] = 0x55u;
            seq[8] = 0x05u;
            seq[9] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[10] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[11] = 0x25u;
            seq[12] = (uint8_t)(start_addr & 0xFFu);
            seq[13] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[14] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[15] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[16] = (uint8_t)(write_len - 1u);
            seq[17] = (uint8_t)(start_addr & 0xFFu);
            seq[18] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[19] = burner_bacon_option_byte2(0, true, true, false, true, true, true);

            for (wr_buf_cnt = 0; wr_buf_cnt < write_len; ++wr_buf_cnt) {
                size_t base = 20u + 3u * wr_buf_cnt;

                seq[base + 0u] = burner_bacon_option_byte2(1, true, true, false, false, true, false);
                seq[base + 1u] = buf[i + wr_buf_cnt];
                seq[base + 2u] = burner_bacon_option_byte2(0, true, true, true, false, true, true);
            }

            seq[20u + 3u * write_len + 0u] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[20u + 3u * write_len + 1u] = 0x29u;
            seq[20u + 3u * write_len + 2u] = (uint8_t)(start_addr & 0xFFu);
            seq[20u + 3u * write_len + 3u] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[20u + 3u * write_len + 4u] = burner_bacon_option_byte2(0, true, true, false, true, true, true);

            err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, seq, NULL, seq_len);
            free(seq);
            if (err != ESP_OK) {
                fallback_to_single = true;
            }

            if (!fallback_to_single) {
                last_addr = (uint16_t)(start_addr + (uint16_t)write_len - 1u);
                last_expected = buf[i + write_len - 1u];
                err = burner_bacon_wait_u8(last_addr, last_expected, BURNER_ROM_POLL_TIMEOUT_MS);
                if (err != ESP_OK) {
                    fallback_to_single = true;
                }
            }

            if (fallback_to_single) {
                size_t single_i;
                burner_status_record_mbc5_buffer_write(true);
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 buffer write fallback -> single-byte at 0x%04X len=%u",
                    (unsigned)start_addr,
                    (unsigned)write_len);
                for (single_i = 0u; single_i < write_len; ++single_i) {
                    uint16_t single_addr = (uint16_t)(start_addr + (uint16_t)single_i);
                    uint8_t seq_single[24];

                    seq_single[0] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[1] = 0xAAu;
                    seq_single[2] = 0x0Au;
                    seq_single[3] = 0xAAu;
                    seq_single[4] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[5] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[6] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[7] = 0x55u;
                    seq_single[8] = 0x05u;
                    seq_single[9] = 0x55u;
                    seq_single[10] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[11] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[12] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[13] = 0xAAu;
                    seq_single[14] = 0x0Au;
                    seq_single[15] = 0xA0u;
                    seq_single[16] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[17] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[18] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[19] = (uint8_t)(single_addr & 0xFFu);
                    seq_single[20] = (uint8_t)((single_addr >> 8) & 0xFFu);
                    seq_single[21] = buf[i + single_i];
                    seq_single[22] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[23] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

                    err = burner_spi_transfer(seq_single, NULL, sizeof(seq_single));
                    if (err != ESP_OK) {
                        return err;
                    }
                    err = burner_bacon_wait_u8(single_addr, buf[i + single_i], BURNER_ROM_POLL_TIMEOUT_MS);
                    if (err != ESP_OK) {
                        return err;
                    }
                    burner_task_yield_if_due();
                }
            } else {
                burner_status_record_mbc5_buffer_write(false);
            }

            i += write_len;
            burner_task_yield_if_due();
        }
    }

    return ESP_OK;
}


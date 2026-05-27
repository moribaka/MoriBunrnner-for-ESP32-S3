/* Low-level GBA flash program, read, and erase helpers. */

static esp_err_t burner_bacon_gba_intel_program_words(uint32_t byte_addr, const uint8_t *buf, size_t len)
{
    size_t off = 0u;
    esp_err_t err;

    if (buf == NULL || len == 0u || (byte_addr & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (off < len) {
        uint32_t starting_address = byte_addr + (uint32_t)off;
        uint32_t starting_word_address = starting_address >> 1;
        uint16_t pd = (uint16_t)((uint16_t)buf[off] | ((uint16_t)buf[off + 1u] << 8));
        uint16_t status = 0u;

        err = burner_bacon_gba_command_write_u16(0x000u, 0x0070u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x0010u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_write_u16(starting_word_address, pd);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_intel_wait_ready(0x000u, 0x0070u, BURNER_ROM_POLL_TIMEOUT_MS, &status);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA intel single-word program timeout @0x%08" PRIX32 " status=0x%04X",
                starting_address,
                status);
            return err;
        }
        err = burner_bacon_gba_intel_read_array();
        if (err != ESP_OK) {
            return err;
        }

        off += 2u;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_rom_program(
    uint32_t byte_addr,
    const uint8_t *buf,
    size_t len,
    uint16_t buffer_write_bytes)
{
    size_t i = 0;
    esp_err_t err;
    bool intel_cmdset;

    if (buf == NULL || len == 0u || (byte_addr & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    intel_cmdset = burner_gba_nor_is_intel_active();

    while (i < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t starting_address = byte_addr + (uint32_t)i;
        uint32_t starting_word_address = starting_address >> 1;

        if (intel_cmdset) {
            if (buffer_write_bytes >= 2u) {
                uint16_t active_buffer_write_bytes = buffer_write_bytes;
                bool allow_runtime_fallback = burner_gba_intel_program_buffer_needs_runtime_fallback(
                    s_cart_ctx.gba_cmdset,
                    s_cart_ctx.buffer_write_bytes,
                    active_buffer_write_bytes);

                while (true) {
                    size_t write_len = 0u;
                    uint16_t next_buffer_write_bytes;

                    err = burner_bacon_gba_intel_buffered_program_once(
                        starting_address,
                        buf + i,
                        len - i,
                        active_buffer_write_bytes,
                        &write_len);
                    if (err == ESP_OK) {
                        i += write_len;
                        burner_task_yield_if_due();
                        break;
                    }
                    if (!allow_runtime_fallback ||
                        err == ESP_ERR_INVALID_ARG ||
                        err == ESP_ERR_INVALID_SIZE ||
                        err == ESP_ERR_NO_MEM ||
                        err == ESP_ERR_INVALID_STATE) {
                        return err;
                    }
                    next_buffer_write_bytes =
                        burner_gba_intel_next_program_buffer_write_bytes(active_buffer_write_bytes);
                    if (next_buffer_write_bytes == 0u || next_buffer_write_bytes >= active_buffer_write_bytes) {
                        return err;
                    }
                    ESP_LOGW(
                        BURNER_TAG,
                        "GBA intel runtime buffer fallback: probe_buf=0 active=%u next=%u addr=0x%08" PRIX32
                        " err=%s",
                        (unsigned)active_buffer_write_bytes,
                        (unsigned)next_buffer_write_bytes,
                        starting_address,
                        esp_err_to_name(err));
                    err = burner_bacon_gba_intel_reset();
                    if (err != ESP_OK) {
                        return err;
                    }
                    active_buffer_write_bytes = next_buffer_write_bytes;
                    buffer_write_bytes = next_buffer_write_bytes;
                    s_cart_ctx.program_buffer_write_bytes = next_buffer_write_bytes;
                }
                continue;
            }

            {
                err = burner_bacon_gba_intel_program_words(starting_address, buf + i, 2u);
                if (err != ESP_OK) {
                    return err;
                }
                i += 2u;
                continue;
            }
        } else if (buffer_write_bytes < 2u) {
            uint8_t seq[41];
            uint16_t pd = (uint16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1u] << 8));
            uint16_t cmd_aa = burner_apply_d0d1_swap_on_write(0x00AAu, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_55 = burner_apply_d0d1_swap_on_write(0x0055u, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_a0 = burner_apply_d0d1_swap_on_write(0x00A0u, s_cart_ctx.d0d1_swapped);
            uint8_t addr0 = (uint8_t)(starting_word_address & 0xFFu);
            uint8_t addr1 = (uint8_t)((starting_word_address >> 8) & 0xFFu);
            uint8_t addr2 = (uint8_t)((starting_word_address >> 16) & 0xFFu);

            seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[1] = 0x55u;
            seq[2] = 0x05u;
            seq[3] = 0x00u;
            seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[5] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[6] = (uint8_t)(cmd_aa & 0xFFu);
            seq[7] = (uint8_t)((cmd_aa >> 8) & 0xFFu);
            seq[8] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[9] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[10] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[11] = 0xAAu;
            seq[12] = 0x02u;
            seq[13] = 0x00u;
            seq[14] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[15] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[16] = (uint8_t)(cmd_55 & 0xFFu);
            seq[17] = (uint8_t)((cmd_55 >> 8) & 0xFFu);
            seq[18] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[19] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[20] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[21] = 0x55u;
            seq[22] = 0x05u;
            seq[23] = 0x00u;
            seq[24] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[25] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[26] = (uint8_t)(cmd_a0 & 0xFFu);
            seq[27] = (uint8_t)((cmd_a0 >> 8) & 0xFFu);
            seq[28] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[29] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[30] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[31] = addr0;
            seq[32] = addr1;
            seq[33] = addr2;
            seq[34] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[35] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[36] = buf[i + 0u];
            seq[37] = buf[i + 1u];
            seq[38] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[39] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[40] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

            err = burner_spi_transfer(seq, NULL, sizeof(seq));
            if (err != ESP_OK) {
                return err;
            }

            err = burner_bacon_wait_u16(starting_address, pd, BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }

            i += 2u;
        } else {
            size_t write_len = len - i;
            size_t wr;
            size_t write_words;
            size_t max_write_words_by_spi = 1u;
            size_t seq_len;
            uint8_t *seq;
            uint8_t addr0;
            uint8_t addr1;
            uint8_t addr2;
            uint8_t unlock0_addr0;
            uint8_t unlock0_addr1;
            uint8_t unlock0_addr2;
            uint8_t unlock1_addr0;
            uint8_t unlock1_addr1;
            uint8_t unlock1_addr2;
            uint16_t last_word;
            uint16_t cmd_aa = burner_apply_d0d1_swap_on_write(0x00AAu, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_55 = burner_apply_d0d1_swap_on_write(0x0055u, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_25 = burner_apply_d0d1_swap_on_write(0x0025u, s_cart_ctx.d0d1_swapped);
            uint16_t cmd_29 = burner_apply_d0d1_swap_on_write(0x0029u, s_cart_ctx.d0d1_swapped);
            uint16_t write_count_word;
            uint32_t unlock0_addr = burner_gba_unlock_addr0();
            uint32_t unlock1_addr = burner_gba_unlock_addr1();

            if (write_len > buffer_write_bytes) {
                write_len = buffer_write_bytes;
            }
            write_len = burner_gba_program_safe_chunk_bytes(
                starting_address,
                write_len,
                (size_t)buffer_write_bytes);
            write_words = write_len / 2u;

            if (BURNER_SPI_MAX_XFER > 57u) {
                max_write_words_by_spi = (BURNER_SPI_MAX_XFER - 57u) / 5u;
            }
            if (max_write_words_by_spi == 0u) {
                max_write_words_by_spi = 1u;
            }
            if (write_words > max_write_words_by_spi) {
                write_words = max_write_words_by_spi;
                write_len = write_words * 2u;
            }

            seq_len = 57u + 5u * write_words;
            if (seq_len > BURNER_SPI_MAX_XFER) {
                return ESP_ERR_INVALID_SIZE;
            }
            seq = (uint8_t *)malloc(seq_len);
            if (seq == NULL) {
                return ESP_ERR_NO_MEM;
            }

            addr0 = (uint8_t)(starting_word_address & 0xFFu);
            addr1 = (uint8_t)((starting_word_address >> 8) & 0xFFu);
            addr2 = (uint8_t)((starting_word_address >> 16) & 0xFFu);
            unlock0_addr0 = (uint8_t)(unlock0_addr & 0xFFu);
            unlock0_addr1 = (uint8_t)((unlock0_addr >> 8) & 0xFFu);
            unlock0_addr2 = (uint8_t)((unlock0_addr >> 16) & 0xFFu);
            unlock1_addr0 = (uint8_t)(unlock1_addr & 0xFFu);
            unlock1_addr1 = (uint8_t)((unlock1_addr >> 8) & 0xFFu);
            unlock1_addr2 = (uint8_t)((unlock1_addr >> 16) & 0xFFu);
            write_count_word = burner_apply_d0d1_swap_on_write(
                (uint16_t)(write_words - 1u),
                s_cart_ctx.d0d1_swapped);

            seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[1] = unlock0_addr0;
            seq[2] = unlock0_addr1;
            seq[3] = unlock0_addr2;
            seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[5] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[6] = (uint8_t)(cmd_aa & 0xFFu);
            seq[7] = (uint8_t)((cmd_aa >> 8) & 0xFFu);
            seq[8] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[9] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[10] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[11] = unlock1_addr0;
            seq[12] = unlock1_addr1;
            seq[13] = unlock1_addr2;
            seq[14] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[15] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[16] = (uint8_t)(cmd_55 & 0xFFu);
            seq[17] = (uint8_t)((cmd_55 >> 8) & 0xFFu);
            seq[18] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[19] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[20] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[21] = addr0;
            seq[22] = addr1;
            seq[23] = addr2;
            seq[24] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[25] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[26] = (uint8_t)(cmd_25 & 0xFFu);
            seq[27] = (uint8_t)((cmd_25 >> 8) & 0xFFu);
            seq[28] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[29] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[30] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[31] = addr0;
            seq[32] = addr1;
            seq[33] = addr2;
            seq[34] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[35] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[36] = (uint8_t)(write_count_word & 0xFFu);
            seq[37] = (uint8_t)((write_count_word >> 8) & 0xFFu);
            seq[38] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[39] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[40] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[41] = addr0;
            seq[42] = addr1;
            seq[43] = addr2;
            seq[44] = burner_bacon_option_byte0(0, true, true, true, false, true, true);

            for (wr = 0; wr < write_words; ++wr) {
                size_t base = 45u + 5u * wr;
                seq[base + 0u] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
                seq[base + 1u] = buf[i + wr * 2u];
                seq[base + 2u] = buf[i + wr * 2u + 1u];
                seq[base + 3u] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                seq[base + 4u] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            }

            seq[45u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[46u + 5u * write_words] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            seq[47u + 5u * write_words] = addr0;
            seq[48u + 5u * write_words] = addr1;
            seq[49u + 5u * write_words] = addr2;
            seq[50u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[51u + 5u * write_words] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
            seq[52u + 5u * write_words] = (uint8_t)(cmd_29 & 0xFFu);
            seq[53u + 5u * write_words] = (uint8_t)((cmd_29 >> 8) & 0xFFu);
            seq[54u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[55u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            seq[56u + 5u * write_words] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

            err = burner_spi_transfer(seq, NULL, seq_len);
            free(seq);
            if (err != ESP_OK) {
                return err;
            }

            last_word = (uint16_t)((uint16_t)buf[i + write_len - 2u] |
                                   ((uint16_t)buf[i + write_len - 1u] << 8));
            err = burner_bacon_wait_u16(
                starting_address + (uint32_t)write_len - 2u,
                last_word,
                BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }

            i += write_len;
            burner_task_yield_if_due();
        }
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_program_block(
    const uint8_t *data,
    size_t len,
    uint32_t offset,
    bool is_multi_card,
    bool prepare_sectors)
{
    size_t programmed = 0;
    esp_err_t err;
    bool geometry_valid = burner_nor_geometry_is_valid(&s_cart_ctx.geometry);

    if (data == NULL || len == 0u || (offset & 0x1u) != 0u || (len & 0x1u) != 0u) {
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
        uint32_t bank = 0u;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        uint32_t sector_end = 0u;
        size_t remain = len - programmed;
        size_t chunk;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        chunk = (remain < bank_remain) ? remain : bank_remain;
        if (geometry_valid) {
            err = burner_nor_geometry_sector_bounds(&s_cart_ctx.geometry, rom_addr, NULL, &sector_end, NULL);
            if (err != ESP_OK || sector_end <= rom_addr) {
                return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
            }
            if (chunk > (size_t)(sector_end - rom_addr)) {
                chunk = (size_t)(sector_end - rom_addr);
            }
        }

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (prepare_sectors) {
            err = burner_gba_sector_erase_prepare_current(rom_addr);
            if (err != ESP_OK) {
                return err;
            }
        }

        err = burner_bacon_gba_rom_program(
            rom_addr,
            data + programmed,
            chunk,
            s_cart_ctx.program_buffer_write_bytes);
        if (err != ESP_OK) {
            return err;
        }

        if (prepare_sectors) {
            err = burner_gba_sector_erase_prefetch_next(rom_addr, chunk);
            if (err != ESP_OK) {
                return err;
            }
        }

        programmed += chunk;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

esp_err_t burner_bacon_gba_read_block(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint32_t bank = 0u;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        size_t remain = len - copied;
        size_t chunk;
        size_t read_words;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        read_words = chunk / 2u;
        if (read_words > 0u) {
            err = burner_bacon_rom_read_u16_batched(rom_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((rom_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}

#if 0
/* Legacy GBA verify block reader kept only for reference. */
static esp_err_t burner_bacon_gba_verify_read_block(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint32_t bank = 0u;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        size_t remain = len - copied;
        size_t chunk;
        size_t read_words;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        read_words = chunk / 2u;
        if (read_words > 0u) {
            err = burner_bacon_rom_verify_read_u16_batched(rom_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((rom_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}
#endif

esp_err_t burner_bacon_gba_verify_read_block_hoststyle(uint8_t *out, size_t len, uint32_t offset, bool is_multi_card)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr;
        uint32_t bank;
        uint32_t bank_remain;
        size_t remain;
        size_t chunk;
        size_t read_words;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        rom_addr = offset + (uint32_t)copied;
        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &bank_remain);
        remain = len - copied;
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        read_words = chunk / 2u;
        if (read_words > 0u) {
            err = burner_bacon_rom_verify_read_u16_batched_hoststyle(rom_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((rom_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_erase_sector(uint32_t flash_addr, bool is_multi_card, uint32_t timeout_ms)
{
    uint32_t bank = 0u;
    uint32_t sa_word = flash_addr >> 1;
    uint16_t read_back = 0;
    int64_t deadline_us;
    esp_err_t err;
    bool intel_cmdset;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    intel_cmdset = burner_gba_nor_is_intel_active();
    burner_gba_resolve_write_addr(flash_addr, is_multi_card, &bank, NULL);

    if (is_multi_card) {
        err = burner_gba_switch_bank_if_needed(bank);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (intel_cmdset) {
        err = burner_bacon_gba_intel_reset();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x0060u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x0020u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(sa_word, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_intel_wait_ready(flash_addr, 0x0070u, timeout_ms, &read_back);
        if (err == ESP_OK) {
            err = burner_bacon_gba_intel_reset();
            if (err != ESP_OK) {
                return err;
            }
            return ESP_OK;
        }
        ESP_LOGW(
            BURNER_TAG,
            "GBA intel erase timeout flash=0x%08" PRIX32 " bank=%" PRIu32 " sa_word=0x%06" PRIX32 " status=0x%04X multi=%u timeout=%ums",
            flash_addr,
            bank,
            sa_word,
            read_back,
            is_multi_card ? 1u : 0u,
            (unsigned)timeout_ms);
        (void)burner_bacon_gba_intel_reset();
        return err;
    }

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0080u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(sa_word, 0x0030u);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(sa_word, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == 0xFFFFu) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGW(
        BURNER_TAG,
        "GBA erase timeout flash=0x%08" PRIX32 " bank=%" PRIu32 " sa_word=0x%06" PRIX32 " read=0x%04X multi=%u timeout=%ums",
        flash_addr,
        bank,
        sa_word,
        read_back,
        is_multi_card ? 1u : 0u,
        (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_gba_region_is_blank_sampled(
    uint32_t region_addr,
    uint32_t region_size,
    bool is_multi_card,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_SAMPLE_BYTES];
    uint32_t sample_offsets[BURN_BLANK_SAMPLE_POINTS];
    size_t sample_count;
    esp_err_t err;

    if (blank_out == NULL || region_size < BURN_BLANK_SAMPLE_BYTES || (region_size & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_count = burner_build_blank_sample_offsets(
        region_size,
        BURN_BLANK_SAMPLE_BYTES,
        0x1u,
        sample_offsets);
    if (sample_count == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *blank_out = true;
    for (size_t i = 0u; i < sample_count; ++i) {
        bool chunk_blank = false;

        err = burner_bacon_gba_read_block(
            sample_buf,
            BURN_BLANK_SAMPLE_BYTES,
            region_addr + sample_offsets[i],
            is_multi_card);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_buffer_all_ff(sample_buf, BURN_BLANK_SAMPLE_BYTES, &chunk_blank);
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

static esp_err_t burner_gba_sector_is_blank(
    uint32_t sector_addr,
    uint32_t sector_size,
    bool is_multi_card,
    bool *blank_out)
{
    return burner_gba_region_is_blank_sampled(sector_addr, sector_size, is_multi_card, blank_out);
}

static esp_err_t burner_bacon_gba_erase_range(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool is_multi_card,
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
    ESP_LOGI(
        BURNER_TAG,
        "GBA erase timeout budget: bytes=%" PRIu32 " timeout=%" PRIu32 "ms",
        erase_bytes,
        timeout_ms);

    while (sector_addr <= addr_end) {
        uint32_t current_sector_size = cursor.sector_size;
        uint32_t region_end_addr = cursor.addr_end - 1u;
        uint32_t region_limit_addr = (addr_end < region_end_addr) ? addr_end : region_end_addr;

        while (sector_addr <= region_limit_addr) {
            err = burner_cancel_poll();
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            if (sample_blank_sectors && !erase_always) {
                bool blank = false;

                err = burner_gba_sector_is_blank(
                    sector_addr,
                    current_sector_size,
                    is_multi_card,
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
            err = burner_bacon_gba_erase_sector(
                sector_addr,
                is_multi_card,
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
            "GBA erase sector-sample: 4x2B erased=%" PRIu32 " skipped_blank=%" PRIu32,
            erased,
            skipped_blank);
    }
    return err;
}

static esp_err_t burner_bacon_gba_chip_erase_once(void)
{
    uint16_t read_back = 0;
    int64_t deadline_us;
    esp_err_t err;
    bool intel_cmdset;

    intel_cmdset = burner_gba_nor_is_intel_active();

    if (intel_cmdset) {
        err = burner_bacon_gba_intel_reset();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x0060u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x0020u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(0x000u, 0x00D0u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_intel_wait_ready(0x000u, 0x0070u, BURNER_ROM_CHIP_ERASE_TIMEOUT_MS, &read_back);
        (void)burner_bacon_gba_intel_reset();
        return err;
    }

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0080u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0010u);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)BURNER_ROM_CHIP_ERASE_TIMEOUT_MS * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(0u, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == 0xFFFFu) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gba_chip_erase(void)
{
    uint8_t id[8] = {0};
    esp_err_t err;

    err = burner_bacon_gba_read_id(id, s_cart_ctx.d0d1_swapped);
    if (err != ESP_OK) {
        return err;
    }

    if (burner_bacon_gba_is_s70gl02(id)) {
        err = burner_bacon_gba_rom_switch_bank(0u);
        if (err != ESP_OK) {
            return err;
        }
        s_cart_ctx.current_bank = 0u;
    }

    err = burner_bacon_gba_chip_erase_once();
    if (err != ESP_OK) {
        return err;
    }

    if (burner_bacon_gba_is_s70gl02(id)) {
        err = burner_bacon_gba_rom_switch_bank(5u);
        if (err != ESP_OK) {
            return err;
        }
        s_cart_ctx.current_bank = 5u;
        err = burner_bacon_gba_chip_erase_once();
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

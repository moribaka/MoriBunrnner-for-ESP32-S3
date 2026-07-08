/* Low-level GBA flash program, read, and erase helpers. */

#define BURNER_GBA_AMD_BUFFER_TEMPLATE_MAX_WORDS 512u
#define BURNER_GBA_AMD_BUFFER_TEMPLATE_MAX_BYTES (57u + 5u * BURNER_GBA_AMD_BUFFER_TEMPLATE_MAX_WORDS)

static uint8_t s_gba_amd_buffer_template[BURNER_GBA_AMD_BUFFER_TEMPLATE_MAX_BYTES];
static size_t s_gba_amd_buffer_template_words = 0u;
static bool s_gba_amd_buffer_template_valid = false;
static bool s_gba_amd_buffer_template_d0d1_swapped = false;
static burner_gba_amd_runtime_profile_t s_gba_amd_buffer_template_profile = BURNER_GBA_AMD_RUNTIME_STANDARD;
static uint32_t s_gba_amd_buffer_template_unlock0 = 0u;
static uint32_t s_gba_amd_buffer_template_unlock1 = 0u;

static bool burner_gba_amd_status_matches_dq7(uint16_t status, uint16_t expected_data)
{
    return (status & 0x0080u) == (expected_data & 0x0080u);
}

static esp_err_t burner_bacon_gba_amd_wait_program_complete(
    uint32_t last_word_addr,
    uint16_t expected_data,
    uint32_t timeout_ms,
    uint16_t *status_out)
{
    int64_t deadline_us;
    uint16_t status1 = 0u;
    uint16_t status2 = 0u;
    esp_err_t err;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(last_word_addr, &status1);
        if (err != ESP_OK) {
            return err;
        }
        if (burner_gba_amd_status_matches_dq7(status1, expected_data)) {
            err = burner_bacon_rom_read_u16(last_word_addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if (burner_gba_amd_status_matches_dq7(status2, expected_data)) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
        }
        if ((status1 & 0x0020u) != 0u) {
            err = burner_bacon_rom_read_u16(last_word_addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if (burner_gba_amd_status_matches_dq7(status2, expected_data)) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
            if (status_out != NULL) {
                *status_out = status2;
            }
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    if (status_out != NULL) {
        *status_out = status1;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gba_amd_wait_erase_complete(
    uint32_t sector_word_addr,
    uint32_t timeout_ms,
    uint16_t *status_out)
{
    int64_t deadline_us;
    uint16_t status1 = 0u;
    uint16_t status2 = 0u;
    esp_err_t err;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(sector_word_addr, &status1);
        if (err != ESP_OK) {
            return err;
        }
        if ((status1 & 0x0080u) != 0u) {
            err = burner_bacon_rom_read_u16(sector_word_addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if ((status2 & 0x0080u) != 0u) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
        }
        if ((status1 & 0x0020u) != 0u) {
            err = burner_bacon_rom_read_u16(sector_word_addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if ((status2 & 0x0080u) != 0u) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
            if (status_out != NULL) {
                *status_out = status2;
            }
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    if (status_out != NULL) {
        *status_out = status1;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_gba_amd_buffer_template_get(
    const burner_gba_program_cmd_cache_t *cmd,
    size_t write_words,
    const uint8_t **template_out,
    size_t *seq_len_out)
{
    uint8_t *seq = s_gba_amd_buffer_template;
    size_t seq_len;
    size_t wr;
    uint16_t write_count_word;

    if (cmd == NULL || template_out == NULL || seq_len_out == NULL ||
        write_words == 0u || write_words > BURNER_GBA_AMD_BUFFER_TEMPLATE_MAX_WORDS) {
        return ESP_ERR_INVALID_ARG;
    }
    seq_len = 57u + 5u * write_words;
    if (seq_len > sizeof(s_gba_amd_buffer_template) || seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_gba_amd_buffer_template_valid &&
        s_gba_amd_buffer_template_words == write_words &&
        s_gba_amd_buffer_template_d0d1_swapped == s_cart_ctx.d0d1_swapped &&
        s_gba_amd_buffer_template_profile == cmd->amd_profile &&
        s_gba_amd_buffer_template_unlock0 == cmd->unlock0_addr &&
        s_gba_amd_buffer_template_unlock1 == cmd->unlock1_addr) {
        *template_out = s_gba_amd_buffer_template;
        *seq_len_out = seq_len;
        return ESP_OK;
    }

    memset(seq, 0, seq_len);
    write_count_word = (uint16_t)(write_words - 1u);
    if (!cmd->amd_count_uses_raw) {
        write_count_word = burner_apply_d0d1_swap_on_write(write_count_word, s_cart_ctx.d0d1_swapped);
    }

    seq[0] = cmd->opt_addr;
    memcpy(&seq[1], cmd->unlock0_addr_bytes, 3u);
    seq[4] = cmd->opt_wr_setup;
    seq[5] = cmd->opt_wr_data;
    seq[6] = (uint8_t)(cmd->amd_aa & 0xFFu);
    seq[7] = (uint8_t)((cmd->amd_aa >> 8) & 0xFFu);
    seq[8] = cmd->opt_wr_low;
    seq[9] = cmd->opt_wr_setup;
    seq[10] = cmd->opt_addr;
    memcpy(&seq[11], cmd->unlock1_addr_bytes, 3u);
    seq[14] = cmd->opt_wr_setup;
    seq[15] = cmd->opt_wr_data;
    seq[16] = (uint8_t)(cmd->amd_55 & 0xFFu);
    seq[17] = (uint8_t)((cmd->amd_55 >> 8) & 0xFFu);
    seq[18] = cmd->opt_wr_low;
    seq[19] = cmd->opt_wr_setup;
    seq[20] = cmd->opt_addr;
    seq[24] = cmd->opt_wr_setup;
    seq[25] = cmd->opt_wr_data;
    seq[26] = (uint8_t)(cmd->amd_25 & 0xFFu);
    seq[27] = (uint8_t)((cmd->amd_25 >> 8) & 0xFFu);
    seq[28] = cmd->opt_wr_low;
    seq[29] = cmd->opt_wr_setup;
    seq[30] = cmd->opt_addr;
    seq[34] = cmd->opt_wr_setup;
    seq[35] = cmd->opt_wr_data;
    seq[36] = (uint8_t)(write_count_word & 0xFFu);
    seq[37] = (uint8_t)((write_count_word >> 8) & 0xFFu);
    seq[38] = cmd->opt_wr_low;
    seq[39] = cmd->opt_wr_setup;
    seq[40] = cmd->opt_addr;
    seq[44] = cmd->opt_wr_setup;

    for (wr = 0; wr < write_words; ++wr) {
        size_t base = 45u + 5u * wr;
        seq[base + 0u] = cmd->opt_wr_data;
        seq[base + 3u] = cmd->opt_wr_low;
        seq[base + 4u] = cmd->opt_wr_setup;
    }

    seq[45u + 5u * write_words] = cmd->opt_release;
    seq[46u + 5u * write_words] = cmd->opt_addr;
    seq[50u + 5u * write_words] = cmd->opt_wr_setup;
    seq[51u + 5u * write_words] = cmd->opt_wr_data;
    seq[52u + 5u * write_words] = (uint8_t)(cmd->amd_29 & 0xFFu);
    seq[53u + 5u * write_words] = (uint8_t)((cmd->amd_29 >> 8) & 0xFFu);
    seq[54u + 5u * write_words] = cmd->opt_wr_low;
    seq[55u + 5u * write_words] = cmd->opt_wr_setup;
    seq[56u + 5u * write_words] = cmd->opt_release;

    s_gba_amd_buffer_template_valid = true;
    s_gba_amd_buffer_template_words = write_words;
    s_gba_amd_buffer_template_d0d1_swapped = s_cart_ctx.d0d1_swapped;
    s_gba_amd_buffer_template_profile = cmd->amd_profile;
    s_gba_amd_buffer_template_unlock0 = cmd->unlock0_addr;
    s_gba_amd_buffer_template_unlock1 = cmd->unlock1_addr;
    *template_out = s_gba_amd_buffer_template;
    *seq_len_out = seq_len;
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
    uint64_t program_start_us;

    if (buf == NULL || len == 0u || (byte_addr & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    intel_cmdset = burner_gba_nor_is_intel_active();
    if (intel_cmdset && !s_cart_ctx.probe_cfi_ok) {
        return ESP_ERR_INVALID_STATE;
    }

    program_start_us = burner_gba_diag_now_us();
    while (i < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t starting_address = byte_addr + (uint32_t)i;
        uint32_t starting_word_address = starting_address >> 1;

        if (intel_cmdset) {
            size_t write_len = 0u;

            if (buffer_write_bytes < 2u) {
                ESP_LOGE(
                    BURNER_TAG,
                    "GBA intel buffered program unavailable @0x%08" PRIX32
                    ": cfi_buf=%u, single-word fallback disabled",
                    starting_address,
                    (unsigned)buffer_write_bytes);
                return ESP_ERR_NOT_SUPPORTED;
            }

            err = burner_bacon_gba_intel_buffered_program_once(
                starting_address,
                buf + i,
                len - i,
                buffer_write_bytes,
                &write_len);
            if (err != ESP_OK) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA intel buffered program failed @0x%08" PRIX32
                    ": cfi_buf=%u err=%s",
                    starting_address,
                    (unsigned)buffer_write_bytes,
                    esp_err_to_name(err));
                (void)burner_bacon_gba_intel_reset();
                return err;
            }
            if (write_len == 0u) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA intel buffered program made no progress @0x%08" PRIX32
                    ": cfi_buf=%u",
                    starting_address,
                    (unsigned)buffer_write_bytes);
                (void)burner_bacon_gba_intel_reset();
                return ESP_ERR_INVALID_RESPONSE;
            }
            i += write_len;
            burner_task_yield_if_due();
            continue;
        } else if (buffer_write_bytes < 2u) {
            uint8_t seq[41];
            uint16_t pd = (uint16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1u] << 8));
            const burner_gba_program_cmd_cache_t *cmd = burner_gba_program_cmd_cache_get();
            uint8_t addr0 = (uint8_t)(starting_word_address & 0xFFu);
            uint8_t addr1 = (uint8_t)((starting_word_address >> 8) & 0xFFu);
            uint8_t addr2 = (uint8_t)((starting_word_address >> 16) & 0xFFu);

            seq[0] = cmd->opt_addr;
            memcpy(&seq[1], cmd->unlock0_addr_bytes, 3u);
            seq[4] = cmd->opt_wr_setup;
            seq[5] = cmd->opt_wr_data;
            seq[6] = (uint8_t)(cmd->amd_aa & 0xFFu);
            seq[7] = (uint8_t)((cmd->amd_aa >> 8) & 0xFFu);
            seq[8] = cmd->opt_wr_low;
            seq[9] = cmd->opt_wr_setup;
            seq[10] = cmd->opt_addr;
            memcpy(&seq[11], cmd->unlock1_addr_bytes, 3u);
            seq[14] = cmd->opt_wr_setup;
            seq[15] = cmd->opt_wr_data;
            seq[16] = (uint8_t)(cmd->amd_55 & 0xFFu);
            seq[17] = (uint8_t)((cmd->amd_55 >> 8) & 0xFFu);
            seq[18] = cmd->opt_wr_low;
            seq[19] = cmd->opt_wr_setup;
            seq[20] = cmd->opt_addr;
            memcpy(&seq[21], cmd->unlock0_addr_bytes, 3u);
            seq[24] = cmd->opt_wr_setup;
            seq[25] = cmd->opt_wr_data;
            seq[26] = (uint8_t)(cmd->amd_a0 & 0xFFu);
            seq[27] = (uint8_t)((cmd->amd_a0 >> 8) & 0xFFu);
            seq[28] = cmd->opt_wr_low;
            seq[29] = cmd->opt_wr_setup;
            seq[30] = cmd->opt_addr;
            seq[31] = addr0;
            seq[32] = addr1;
            seq[33] = addr2;
            seq[34] = cmd->opt_wr_setup;
            seq[35] = cmd->opt_wr_data;
            seq[36] = buf[i + 0u];
            seq[37] = buf[i + 1u];
            seq[38] = cmd->opt_wr_low;
            seq[39] = cmd->opt_wr_setup;
            seq[40] = cmd->opt_release;

            err = burner_spi_transfer(seq, NULL, sizeof(seq));
            if (err != ESP_OK) {
                return err;
            }

            err = burner_bacon_wait_u16(starting_address, pd, BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_bacon_gba_reset_to_read_mode();
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
            bool free_seq = false;
            uint8_t addr0;
            uint8_t addr1;
            uint8_t addr2;
            uint16_t last_word;
            uint16_t write_count_word;
            const burner_gba_program_cmd_cache_t *cmd = burner_gba_program_cmd_cache_get();
            const uint8_t *template_seq = NULL;
            bool use_template;
            uint64_t once_start_us = burner_gba_diag_now_us();
            uint64_t build_us = 0u;
            uint64_t spi_us = 0u;
            uint64_t done_wait_us = 0u;
            uint64_t reset_us = 0u;
            uint64_t t0;

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

            use_template = (write_len == (size_t)buffer_write_bytes &&
                            write_words <= BURNER_GBA_AMD_BUFFER_TEMPLATE_MAX_WORDS);
            t0 = burner_gba_diag_now_us();
            seq = burner_spi_alloc_tx_buffer(seq_len, &free_seq);
            if (seq == NULL) {
                return ESP_ERR_NO_MEM;
            }

            addr0 = (uint8_t)(starting_word_address & 0xFFu);
            addr1 = (uint8_t)((starting_word_address >> 8) & 0xFFu);
            addr2 = (uint8_t)((starting_word_address >> 16) & 0xFFu);

            if (use_template) {
                err = burner_gba_amd_buffer_template_get(cmd, write_words, &template_seq, &seq_len);
                if (err != ESP_OK) {
                    if (free_seq) {
                        free(seq);
                    }
                    return err;
                }
                memcpy(seq, template_seq, seq_len);
                burner_gba_patch_u24(&seq[21], starting_word_address);
                burner_gba_patch_u24(&seq[31], starting_word_address);
                burner_gba_patch_u24(&seq[41], starting_word_address);
                burner_gba_patch_u24(&seq[47u + 5u * write_words], starting_word_address);
                for (wr = 0; wr < write_words; ++wr) {
                    size_t base = 45u + 5u * wr;
                    seq[base + 1u] = buf[i + wr * 2u];
                    seq[base + 2u] = buf[i + wr * 2u + 1u];
                }
            } else {
                write_count_word = (uint16_t)(write_words - 1u);
                if (!cmd->amd_count_uses_raw) {
                    write_count_word = burner_apply_d0d1_swap_on_write(write_count_word, s_cart_ctx.d0d1_swapped);
                }

                seq[0] = cmd->opt_addr;
                memcpy(&seq[1], cmd->unlock0_addr_bytes, 3u);
                seq[4] = cmd->opt_wr_setup;
                seq[5] = cmd->opt_wr_data;
                seq[6] = (uint8_t)(cmd->amd_aa & 0xFFu);
                seq[7] = (uint8_t)((cmd->amd_aa >> 8) & 0xFFu);
                seq[8] = cmd->opt_wr_low;
                seq[9] = cmd->opt_wr_setup;
                seq[10] = cmd->opt_addr;
                memcpy(&seq[11], cmd->unlock1_addr_bytes, 3u);
                seq[14] = cmd->opt_wr_setup;
                seq[15] = cmd->opt_wr_data;
                seq[16] = (uint8_t)(cmd->amd_55 & 0xFFu);
                seq[17] = (uint8_t)((cmd->amd_55 >> 8) & 0xFFu);
                seq[18] = cmd->opt_wr_low;
                seq[19] = cmd->opt_wr_setup;
                seq[20] = cmd->opt_addr;
                seq[21] = addr0;
                seq[22] = addr1;
                seq[23] = addr2;
                seq[24] = cmd->opt_wr_setup;
                seq[25] = cmd->opt_wr_data;
                seq[26] = (uint8_t)(cmd->amd_25 & 0xFFu);
                seq[27] = (uint8_t)((cmd->amd_25 >> 8) & 0xFFu);
                seq[28] = cmd->opt_wr_low;
                seq[29] = cmd->opt_wr_setup;
                seq[30] = cmd->opt_addr;
                seq[31] = addr0;
                seq[32] = addr1;
                seq[33] = addr2;
                seq[34] = cmd->opt_wr_setup;
                seq[35] = cmd->opt_wr_data;
                seq[36] = (uint8_t)(write_count_word & 0xFFu);
                seq[37] = (uint8_t)((write_count_word >> 8) & 0xFFu);
                seq[38] = cmd->opt_wr_low;
                seq[39] = cmd->opt_wr_setup;
                seq[40] = cmd->opt_addr;
                seq[41] = addr0;
                seq[42] = addr1;
                seq[43] = addr2;
                seq[44] = cmd->opt_wr_setup;

                for (wr = 0; wr < write_words; ++wr) {
                    size_t base = 45u + 5u * wr;
                    seq[base + 0u] = cmd->opt_wr_data;
                    seq[base + 1u] = buf[i + wr * 2u];
                    seq[base + 2u] = buf[i + wr * 2u + 1u];
                    seq[base + 3u] = cmd->opt_wr_low;
                    seq[base + 4u] = cmd->opt_wr_setup;
                }

                seq[45u + 5u * write_words] = cmd->opt_release;
                seq[46u + 5u * write_words] = cmd->opt_addr;
                seq[47u + 5u * write_words] = addr0;
                seq[48u + 5u * write_words] = addr1;
                seq[49u + 5u * write_words] = addr2;
                seq[50u + 5u * write_words] = cmd->opt_wr_setup;
                seq[51u + 5u * write_words] = cmd->opt_wr_data;
                seq[52u + 5u * write_words] = (uint8_t)(cmd->amd_29 & 0xFFu);
                seq[53u + 5u * write_words] = (uint8_t)((cmd->amd_29 >> 8) & 0xFFu);
                seq[54u + 5u * write_words] = cmd->opt_wr_low;
                seq[55u + 5u * write_words] = cmd->opt_wr_setup;
                seq[56u + 5u * write_words] = cmd->opt_release;
            }
            build_us = burner_gba_diag_now_us() - t0;

            t0 = burner_gba_diag_now_us();
            err = burner_spi_transfer(seq, NULL, seq_len);
            spi_us = burner_gba_diag_now_us() - t0;
            if (free_seq) {
                free(seq);
            }
            if (err != ESP_OK) {
                return err;
            }

            last_word = (uint16_t)((uint16_t)buf[i + write_len - 2u] |
                                   ((uint16_t)buf[i + write_len - 1u] << 8));
            t0 = burner_gba_diag_now_us();
            err = burner_bacon_gba_amd_wait_program_complete(
                starting_word_address + (uint32_t)write_words - 1u,
                last_word,
                BURNER_ROM_POLL_TIMEOUT_MS,
                NULL);
            done_wait_us = burner_gba_diag_now_us() - t0;
            if (err != ESP_OK) {
                (void)burner_bacon_gba_reset_to_read_mode();
                return err;
            }
            t0 = burner_gba_diag_now_us();
            err = burner_bacon_gba_reset_to_read_mode();
            reset_us = burner_gba_diag_now_us() - t0;
            if (err != ESP_OK) {
                return err;
            }
            burner_gba_chis_diag_add_program_once(
                write_len,
                burner_gba_diag_now_us() - once_start_us,
                build_us,
                spi_us,
                0u,
                done_wait_us,
                reset_us);

            i += write_len;
            burner_task_yield_if_due();
        }
    }

    burner_gba_chis_diag_add_program_lowlevel(burner_gba_diag_now_us() - program_start_us);
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
    if (burner_gba_gbx_is_active()) {
        return burner_gba_gbx_program_block(data, len, offset, is_multi_card, prepare_sectors);
    }

    while (programmed < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)programmed;
        uint32_t bank = 0u;
        uint32_t local_addr = rom_addr;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        uint32_t sector_end = 0u;
        size_t remain = len - programmed;
        size_t chunk;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &local_addr, &bank_remain);
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

        uint64_t program_call_start_us = burner_gba_diag_now_us();
        err = burner_bacon_gba_rom_program(
            local_addr,
            data + programmed,
            chunk,
            s_cart_ctx.program_buffer_write_bytes);
        if (err != ESP_OK) {
            return err;
        }
        burner_gba_chis_diag_add_program_call(chunk, burner_gba_diag_now_us() - program_call_start_us);

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
        uint32_t local_addr = rom_addr;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        size_t remain = len - copied;
        size_t chunk;
        size_t read_words;

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &local_addr, &bank_remain);
        chunk = (remain < bank_remain) ? remain : bank_remain;

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        read_words = chunk / 2u;
        if (read_words > 0u) {
            err = burner_bacon_rom_read_u16_batched(local_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((local_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}

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
        uint32_t local_addr;
        uint32_t bank_remain;
        size_t remain;
        size_t chunk;
        size_t read_words;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        rom_addr = offset + (uint32_t)copied;
        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, &local_addr, &bank_remain);
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
            err = burner_bacon_rom_verify_read_u16_batched_hoststyle(local_addr >> 1, out + copied, read_words);
            if (err != ESP_OK) {
                return err;
            }
        }

        if ((chunk & 0x1u) != 0u) {
            uint16_t word = 0;
            err = burner_bacon_rom_read_u16((local_addr + (uint32_t)(read_words * 2u)) >> 1, &word);
            if (err != ESP_OK) {
                return err;
            }
            out[copied + read_words * 2u] = (uint8_t)(word & 0xFFu);
        }

        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_gba_reset_aso_diag(void)
{
    esp_err_t err;
    const burner_gba_program_cmd_cache_t *cmd = burner_gba_program_cmd_cache_get();

    err = burner_bacon_rom_write_u16(0x000u, cmd->amd_90);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(0x000u, 0x0000u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gba_reset_to_read_mode();
}

static esp_err_t burner_bacon_gba_ppb_enter_aso(const burner_gba_program_cmd_cache_t *cmd, uint16_t ppb_command)
{
    esp_err_t err;

    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_aa);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock1_addr, cmd->amd_55);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_rom_write_u16(cmd->unlock0_addr, ppb_command);
}

static esp_err_t burner_bacon_gba_diag_read_ppb_lock_status(uint16_t *lock_status_out)
{
    esp_err_t err;
    const burner_gba_program_cmd_cache_t *cmd = burner_gba_program_cmd_cache_get();

    if (lock_status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_gba_reset_aso_diag();
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_ppb_enter_aso(cmd, cmd->amd_50);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_read_u16(0x000u, lock_status_out);
    if (err == ESP_OK) {
        *lock_status_out = burner_apply_d0d1_swap_on_read(*lock_status_out, s_cart_ctx.d0d1_swapped);
    }
    (void)burner_bacon_gba_reset_aso_diag();
    return err;
}

static esp_err_t burner_bacon_gba_diag_read_sector_ppb(uint32_t sector_addr, uint16_t *ppb_out)
{
    esp_err_t err;
    const burner_gba_program_cmd_cache_t *cmd = burner_gba_program_cmd_cache_get();

    if (ppb_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_gba_reset_aso_diag();
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gba_ppb_enter_aso(cmd, cmd->amd_c0);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_read_u16(sector_addr >> 1, ppb_out);
    if (err == ESP_OK) {
        *ppb_out = burner_apply_d0d1_swap_on_read(*ppb_out, s_cart_ctx.d0d1_swapped);
    }
    (void)burner_bacon_gba_reset_aso_diag();
    return err;
}

static esp_err_t burner_bacon_gba_erase_sector(uint32_t flash_addr, bool is_multi_card, uint32_t timeout_ms)
{
    uint32_t bank = 0u;
    uint32_t local_addr = flash_addr;
    uint32_t sa_word;
    uint16_t read_back = 0;
    uint16_t ppb_lock_status = 0u;
    uint16_t sector_ppb = 0u;
    esp_err_t ppb_lock_err = ESP_FAIL;
    esp_err_t sector_ppb_err = ESP_FAIL;
    int64_t deadline_us;
    esp_err_t err;
    bool intel_cmdset;
    const burner_gba_program_cmd_cache_t *cmd = NULL;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }
    if (burner_gba_gbx_is_active()) {
        return burner_gba_gbx_erase_sector(flash_addr, is_multi_card, timeout_ms);
    }
    burner_gba_chis_diag_add_erase_call();

    intel_cmdset = burner_gba_nor_is_intel_active();
    burner_gba_resolve_write_addr(flash_addr, is_multi_card, &bank, &local_addr, NULL);
    sa_word = local_addr >> 1;

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
        err = burner_bacon_gba_intel_wait_ready(local_addr, 0x0000u, timeout_ms, &read_back);
        if (err == ESP_OK) {
            if (burner_gba_intel_status_has_error(read_back)) {
                ppb_lock_err = burner_bacon_gba_diag_read_ppb_lock_status(&ppb_lock_status);
                sector_ppb_err = burner_bacon_gba_diag_read_sector_ppb(local_addr, &sector_ppb);
                burner_gba_intel_log_status_error("erase", flash_addr, read_back);
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA intel erase diag flash=0x%08" PRIX32 " ppb_lock=%s(0x%04X) sector_ppb=%s(0x%04X)",
                    flash_addr,
                    (ppb_lock_err == ESP_OK) ? "ok" : esp_err_to_name(ppb_lock_err),
                    ppb_lock_status,
                    (sector_ppb_err == ESP_OK) ? "ok" : esp_err_to_name(sector_ppb_err),
                    sector_ppb);
                if (ppb_lock_err == ESP_OK && sector_ppb_err == ESP_OK &&
                    ppb_lock_status == 0x0001u && sector_ppb != 0x0001u) {
                    ESP_LOGW(BURNER_TAG, "GBA intel erase hint: sector appears PPB-protected, unlock PPB before burn");
                }
                (void)burner_bacon_gba_intel_reset();
                return ESP_ERR_INVALID_RESPONSE;
            }
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

    cmd = burner_gba_program_cmd_cache_get();

    err = burner_bacon_gba_reset_to_read_mode();
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_aa);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock1_addr, cmd->amd_55);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_80);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_aa);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock1_addr, cmd->amd_55);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(sa_word, cmd->amd_30);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    err = burner_bacon_gba_amd_wait_erase_complete(sa_word, burner_erase_remaining_timeout_ms(deadline_us), &read_back);
    if (err == ESP_OK) {
        err = burner_bacon_gba_reset_to_read_mode();
        if (err != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    ppb_lock_err = burner_bacon_gba_diag_read_ppb_lock_status(&ppb_lock_status);
    sector_ppb_err = burner_bacon_gba_diag_read_sector_ppb(local_addr, &sector_ppb);
    ESP_LOGW(
        BURNER_TAG,
        "GBA erase timeout flash=0x%08" PRIX32 " bank=%" PRIu32 " sa_word=0x%06" PRIX32
        " read=0x%04X multi=%u timeout=%ums ppb_lock=%s(0x%04X) sector_ppb=%s(0x%04X)",
        flash_addr,
        bank,
        sa_word,
        read_back,
        is_multi_card ? 1u : 0u,
        (unsigned)timeout_ms,
        (ppb_lock_err == ESP_OK) ? "ok" : esp_err_to_name(ppb_lock_err),
        ppb_lock_status,
        (sector_ppb_err == ESP_OK) ? "ok" : esp_err_to_name(sector_ppb_err),
        sector_ppb);
    if (ppb_lock_err == ESP_OK && sector_ppb_err == ESP_OK &&
        ppb_lock_status == 0x0001u && sector_ppb != 0x0001u) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA erase timeout hint: sector appears PPB-protected, unlock PPB before burn");
    }
    (void)burner_bacon_gba_reset_to_read_mode();
    return err;
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
    uint32_t stop_sector_addr = 0u;
    uint32_t skipped_blank = 0u;
    uint32_t erased = 0u;
    uint32_t erase_bytes;
    uint32_t timeout_ms;
    int64_t erase_deadline_us;
    bool sample_blank;
    esp_err_t err = ESP_OK;

    (void)sector_size;
    if (!burner_nor_geometry_is_valid(geometry) || addr_end < addr_begin) {
        return ESP_ERR_INVALID_ARG;
    }
    if (burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    err = burner_nor_geometry_sector_bounds_in_cursor(&cursor, addr_begin, &stop_sector_addr, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_nor_geometry_region_cursor_begin(geometry, addr_end, &cursor);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    err = burner_nor_geometry_sector_bounds_in_cursor(&cursor, addr_end, &sector_addr, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    erase_bytes = burner_nor_geometry_erase_bytes_from_range(geometry, addr_begin, addr_end);
    if (erase_bytes == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    timeout_ms = burner_erase_timeout_ms_for_bytes(erase_bytes);
    erase_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    sample_blank = sample_blank_sectors && !erase_always;
    ESP_LOGI(
        BURNER_TAG,
        "GBA erase timeout budget: bytes=%" PRIu32 " timeout=%" PRIu32 "ms",
        erase_bytes,
        timeout_ms);

    while (true) {
        uint32_t current_sector_size = 0u;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            goto erase_range_out;
        }
        err = burner_nor_geometry_region_cursor_seek_forward(geometry, sector_addr, &cursor);
        if (err != ESP_OK) {
            goto erase_range_out;
        }
        err = burner_nor_geometry_sector_bounds_in_cursor(
            &cursor,
            sector_addr,
            &sector_addr,
            NULL,
            &current_sector_size);
        if (err != ESP_OK || current_sector_size == 0u) {
            err = (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
            goto erase_range_out;
        }
        if (sample_blank) {
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
            } else {
                err = burner_bacon_gba_erase_sector(
                    sector_addr,
                    is_multi_card,
                    burner_erase_remaining_timeout_ms(erase_deadline_us));
                if (err != ESP_OK) {
                    goto erase_range_out;
                }
                erased++;
                burner_status_advance_erase_phase(1u, current_sector_size);
            }
        } else {
            err = burner_bacon_gba_erase_sector(
                sector_addr,
                is_multi_card,
                burner_erase_remaining_timeout_ms(erase_deadline_us));
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            erased++;
            burner_status_advance_erase_phase(1u, current_sector_size);
        }
        if (sector_addr == stop_sector_addr) {
            break;
        }
        if (sector_addr == 0u) {
            err = ESP_ERR_INVALID_SIZE;
            goto erase_range_out;
        }
        err = burner_nor_geometry_region_cursor_begin(geometry, sector_addr - 1u, &cursor);
        if (err != ESP_OK) {
            goto erase_range_out;
        }
        err = burner_nor_geometry_sector_bounds_in_cursor(&cursor, sector_addr - 1u, &sector_addr, NULL, NULL);
        if (err != ESP_OK) {
            goto erase_range_out;
        }
    }

erase_range_out:
    if (err == ESP_OK && sample_blank &&
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
    const burner_gba_program_cmd_cache_t *cmd = NULL;

    if (burner_gba_gbx_is_active()) {
        return burner_gba_gbx_chip_erase_once();
    }

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
        err = burner_bacon_gba_intel_wait_ready(0x000u, 0x0000u, BURNER_ROM_CHIP_ERASE_TIMEOUT_MS, &read_back);
        if (err == ESP_OK && burner_gba_intel_status_has_error(read_back)) {
            burner_gba_intel_log_status_error("chip-erase", 0x00000000u, read_back);
            (void)burner_bacon_gba_intel_reset();
            return ESP_ERR_INVALID_RESPONSE;
        }
        (void)burner_bacon_gba_intel_reset();
        return err;
    }

    cmd = burner_gba_program_cmd_cache_get();
    if (!cmd->amd_chip_erase_supported) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA AMD chip erase unsupported for runtime profile=%s; use sector erase during write",
            burner_gba_amd_runtime_profile_name(cmd->amd_profile));
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_aa);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock1_addr, cmd->amd_55);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_80);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_aa);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock1_addr, cmd->amd_55);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_rom_write_u16(cmd->unlock0_addr, cmd->amd_10);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)BURNER_ROM_CHIP_ERASE_TIMEOUT_MS * 1000);
    err = burner_bacon_gba_amd_wait_erase_complete(0u, burner_erase_remaining_timeout_ms(deadline_us), &read_back);
    if (err != ESP_OK) {
        (void)burner_bacon_gba_reset_to_read_mode();
        return err;
    }
    return burner_bacon_gba_reset_to_read_mode();
}

static esp_err_t burner_bacon_gba_chip_erase(void)
{
    uint8_t id[8] = {0};
    esp_err_t err;

    if (burner_gba_gbx_is_active()) {
        return burner_gba_gbx_chip_erase_once();
    }

    if (s_gba_amd_runtime_profile != BURNER_GBA_AMD_RUNTIME_STANDARD) {
        return burner_bacon_gba_chip_erase_once();
    }

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

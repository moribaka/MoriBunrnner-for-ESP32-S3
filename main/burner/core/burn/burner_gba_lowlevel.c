/* Low-level GBA ROM bus, probe, and prepare helpers. */

static esp_err_t burner_bacon_rom_write_u16(uint32_t word_addr, uint16_t value)
{
    uint8_t seq[11];

    seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    seq[1] = (uint8_t)(word_addr & 0xFFu);
    seq[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    seq[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[5] = burner_bacon_option_byte0(2, true, true, true, false, true, true);
    seq[6] = (uint8_t)(value & 0xFFu);
    seq[7] = (uint8_t)((value >> 8) & 0xFFu);
    seq[8] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
    seq[9] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    seq[10] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
    return burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0, seq, NULL, sizeof(seq));
}

static esp_err_t burner_bacon_rom_read_u16(uint32_t word_addr, uint16_t *out_value);
static esp_err_t burner_bacon_gba_command_write_u16(uint32_t word_addr, uint16_t value);
static esp_err_t burner_bacon_gba_reset_to_read_mode(void);
static bool burner_gba_nor_is_intel_active(void);
static bool burner_gba_chis_intel_uses_strict_chislink_flow(void);
static esp_err_t burner_gba_switch_bank_if_needed(uint32_t bank);
static uint32_t burner_erase_timeout_ms_for_bytes(uint32_t bytes);
static esp_err_t burner_bacon_gba_erase_sector(uint32_t flash_addr, bool is_multi_card, uint32_t timeout_ms);
static uint32_t burner_gba_unlock_addr0(void);
static uint32_t burner_gba_unlock_addr1(void);
static uint32_t s_gba_active_nor_flags = 0u;
static bool s_gba_active_intel_generic_cfi = false;
static bool s_gba_active_intel_e9_entry = false;

typedef struct {
    bool active;
    bool intel_active;
    uint64_t job_start_us;
    uint32_t total_bytes;
    uint32_t stage_count;
    uint32_t program_calls;
    uint32_t program_once_calls;
    uint32_t wait_ready_calls;
    uint32_t reset_calls;
    uint32_t bank_switch_calls;
    uint32_t erase_calls;
    uint32_t programmed_bytes;
    uint32_t program_once_bytes;
    uint64_t erase_us;
    uint64_t prefetch_wait_us;
    uint64_t tf_read_us;
    uint64_t program_total_us;
    uint64_t program_lowlevel_us;
    uint64_t program_once_total_us;
    uint64_t program_once_build_us;
    uint64_t program_once_spi_us;
    uint64_t program_once_wait_entry_us;
    uint64_t program_once_wait_done_us;
    uint64_t program_once_reset_us;
    uint64_t wait_ready_us;
    uint64_t reset_us;
    uint64_t bank_switch_us;
    uint64_t finalize_us;
    uint64_t post_verify_us;
    uint64_t stage_erase_us;
    uint64_t stage_prefetch_wait_us;
    uint64_t stage_tf_read_us;
    uint64_t stage_program_total_us;
    uint64_t stage_program_once_reset_us;
} burner_gba_chis_diag_t;

static burner_gba_chis_diag_t s_gba_chis_diag = {0};

static uint64_t burner_gba_diag_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void burner_gba_chis_diag_add_u64(uint64_t *field, uint64_t value)
{
    if (field == NULL || value == 0u) {
        return;
    }
    if (*field > UINT64_MAX - value) {
        *field = UINT64_MAX;
    } else {
        *field += value;
    }
}

static void burner_gba_chis_diag_begin(const burner_task_param_t *job)
{
    memset(&s_gba_chis_diag, 0, sizeof(s_gba_chis_diag));
    s_gba_chis_diag.active = (job != NULL && job->recipe_mode == BURNER_RECIPE_MODE_CHIS);
    s_gba_chis_diag.job_start_us = burner_gba_diag_now_us();
    s_gba_chis_diag.total_bytes = (job != NULL) ? job->total_bytes : 0u;
}

static void burner_gba_chis_diag_set_intel(bool intel_active)
{
    s_gba_chis_diag.intel_active = intel_active;
    if (intel_active) {
        s_gba_chis_diag.active = true;
    }
}

static void burner_gba_chis_diag_stage_begin(void)
{
    if (s_gba_chis_diag.active) {
        s_gba_chis_diag.stage_count++;
        s_gba_chis_diag.stage_erase_us = 0u;
        s_gba_chis_diag.stage_prefetch_wait_us = 0u;
        s_gba_chis_diag.stage_tf_read_us = 0u;
        s_gba_chis_diag.stage_program_total_us = 0u;
        s_gba_chis_diag.stage_program_once_reset_us = 0u;
    }
}

static void burner_gba_chis_diag_add_erase(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.erase_us, elapsed_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.stage_erase_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_prefetch_wait(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.prefetch_wait_us, elapsed_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.stage_prefetch_wait_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_tf_read(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.tf_read_us, elapsed_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.stage_tf_read_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_program_call(size_t bytes, uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        s_gba_chis_diag.program_calls++;
        if ((uint64_t)s_gba_chis_diag.programmed_bytes + (uint64_t)bytes > (uint64_t)UINT32_MAX) {
            s_gba_chis_diag.programmed_bytes = UINT32_MAX;
        } else {
            s_gba_chis_diag.programmed_bytes += (uint32_t)bytes;
        }
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_total_us, elapsed_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.stage_program_total_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_program_lowlevel(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_lowlevel_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_program_once(
    size_t bytes,
    uint64_t total_us,
    uint64_t build_us,
    uint64_t spi_us,
    uint64_t entry_wait_us,
    uint64_t done_wait_us,
    uint64_t reset_us)
{
    if (s_gba_chis_diag.active) {
        s_gba_chis_diag.program_once_calls++;
        if ((uint64_t)s_gba_chis_diag.program_once_bytes + (uint64_t)bytes > (uint64_t)UINT32_MAX) {
            s_gba_chis_diag.program_once_bytes = UINT32_MAX;
        } else {
            s_gba_chis_diag.program_once_bytes += (uint32_t)bytes;
        }
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_once_total_us, total_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_once_build_us, build_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_once_spi_us, spi_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_once_wait_entry_us, entry_wait_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_once_wait_done_us, done_wait_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.program_once_reset_us, reset_us);
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.stage_program_once_reset_us, reset_us);
    }
}

static void burner_gba_chis_diag_add_wait_ready(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        s_gba_chis_diag.wait_ready_calls++;
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.wait_ready_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_reset(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        s_gba_chis_diag.reset_calls++;
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.reset_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_bank_switch(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        s_gba_chis_diag.bank_switch_calls++;
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.bank_switch_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_erase_call(void)
{
    if (s_gba_chis_diag.active) {
        s_gba_chis_diag.erase_calls++;
    }
}

static void burner_gba_chis_diag_add_finalize(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.finalize_us, elapsed_us);
    }
}

static void burner_gba_chis_diag_add_post_verify(uint64_t elapsed_us)
{
    if (s_gba_chis_diag.active) {
        burner_gba_chis_diag_add_u64(&s_gba_chis_diag.post_verify_us, elapsed_us);
    }
}

static uint32_t burner_gba_chis_diag_us_to_ms(uint64_t us)
{
    return burner_us_to_ms_clamped(us);
}

static void burner_gba_chis_diag_log_stage(uint32_t addr, size_t bytes, uint32_t processed_before, uint32_t processed_after)
{
    (void)addr;
    (void)bytes;
    (void)processed_before;
    (void)processed_after;
}

static void burner_gba_chis_diag_log_summary(esp_err_t err)
{
    uint64_t elapsed_us;
    uint32_t avg_once_bytes = 0u;
    const char *label;

    if (!s_gba_chis_diag.active) {
        return;
    }
    elapsed_us = burner_gba_diag_now_us() - s_gba_chis_diag.job_start_us;
    label = s_gba_chis_diag.intel_active ? "Intel" : "CHIS";
    if (s_gba_chis_diag.program_once_calls != 0u) {
        avg_once_bytes = s_gba_chis_diag.program_once_bytes / s_gba_chis_diag.program_once_calls;
    }
    ESP_LOGI(
        BURNER_TAG,
        "GBA %s summary: err=%s total=%" PRIu32 "ms bytes=%" PRIu32 "/%" PRIu32
        " erase=%" PRIu32 "ms tf=%" PRIu32 "ms program=%" PRIu32 "ms avg_once=%" PRIu32 "B",
        label,
        esp_err_to_name(err),
        burner_gba_chis_diag_us_to_ms(elapsed_us),
        s_gba_chis_diag.programmed_bytes,
        s_gba_chis_diag.total_bytes,
        burner_gba_chis_diag_us_to_ms(s_gba_chis_diag.erase_us),
        burner_gba_chis_diag_us_to_ms(s_gba_chis_diag.tf_read_us),
        burner_gba_chis_diag_us_to_ms(s_gba_chis_diag.program_total_us),
        avg_once_bytes);
    s_gba_chis_diag.active = false;
}

static esp_err_t burner_bacon_rom_read_packed(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    size_t read_len_word;
    size_t chunk_len_limit;
    size_t chunk_words_limit;
    size_t words_done = 0u;
    uint8_t setup[5];
    uint8_t rd_low;
    uint8_t release;
    uint8_t *tx_chunk = NULL;
    uint8_t *rx_chunk = NULL;
    bool free_tx_chunk = false;
    bool free_rx_chunk = false;
    bool session_open = false;
    bool release_sent = false;
    esp_err_t err;

    if (buf == NULL || len == 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    read_len_word = len / 2u;
    chunk_len_limit = BURNER_SPI_STREAM_CHUNK_BYTES;
    if (chunk_len_limit == 0u || chunk_len_limit > BURNER_SPI_MAX_XFER) {
        chunk_len_limit = BURNER_SPI_MAX_XFER;
    }
    chunk_len_limit -= (chunk_len_limit % 3u);
    if (chunk_len_limit == 0u) {
        chunk_len_limit = 3u;
    }
    chunk_words_limit = chunk_len_limit / 3u;
    if (chunk_words_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_chunk = burner_spi_alloc_tx_buffer(chunk_len_limit, &free_tx_chunk);
    rx_chunk = burner_spi_alloc_rw_buffer(chunk_len_limit, &free_rx_chunk);
    if (tx_chunk == NULL || rx_chunk == NULL) {
        if (free_tx_chunk && tx_chunk != NULL) {
            free(tx_chunk);
        }
        if (free_rx_chunk && rx_chunk != NULL) {
            free(rx_chunk);
        }
        return ESP_ERR_NO_MEM;
    }

    setup[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    setup[1] = (uint8_t)((addr_byte >> 1) & 0xFFu);
    setup[2] = (uint8_t)(((addr_byte >> 1) >> 8) & 0xFFu);
    setup[3] = (uint8_t)(((addr_byte >> 1) >> 16) & 0xFFu);
    setup[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    rd_low = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    release = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_0);
    if (err != ESP_OK) {
        goto rom_read_out;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto rom_read_out;
    }

    err = burner_spi_transfer_active(&rd_low, NULL, 1u);
    if (err != ESP_OK) {
        goto rom_read_out;
    }
    if (BURNER_GBA_ROM_PHASE_GAP_US > 0u) {
        esp_rom_delay_us(BURNER_GBA_ROM_PHASE_GAP_US);
    }

    while (words_done < read_len_word) {
        size_t chunk_words = read_len_word - words_done;
        size_t chunk_len;

        if (chunk_words > chunk_words_limit) {
            chunk_words = chunk_words_limit;
        }
        chunk_len = chunk_words * 3u;

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 3u;
            tx_chunk[base + 0u] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
            tx_chunk[base + 1u] = 0x00u;
            tx_chunk[base + 2u] = 0x00u;
        }

        err = burner_spi_transfer_active(tx_chunk, rx_chunk, chunk_len);
        if (err != ESP_OK) {
            goto rom_read_out;
        }

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 3u;
            buf[(words_done + i) * 2u + 0u] = rx_chunk[base + 1u];
            buf[(words_done + i) * 2u + 1u] = rx_chunk[base + 2u];
        }

        words_done += chunk_words;
    }

    if (BURNER_GBA_ROM_PHASE_GAP_US > 0u) {
        esp_rom_delay_us(BURNER_GBA_ROM_PHASE_GAP_US);
    }
    err = burner_spi_transfer_active(&release, NULL, 1u);
    if (err == ESP_OK) {
        release_sent = true;
    }

rom_read_out:
    if (session_open) {
        if (!release_sent) {
            (void)burner_spi_transfer_active(&release, NULL, 1u);
        }
        burner_spi_end_cs(BURNER_SPI_CS_MODE_0);
    }
    if (free_tx_chunk && tx_chunk != NULL) {
        free(tx_chunk);
    }
    if (free_rx_chunk && rx_chunk != NULL) {
        free(rx_chunk);
    }
    return err;
}

#if 0
/* Legacy GBA verify path kept only for reference. */
static esp_err_t burner_bacon_rom_verify_read_packed(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    const size_t bytes_per_word = 10u;
    size_t words_done = 0u;
    size_t total_words;
    size_t chunk_words_limit;
    esp_err_t err = ESP_OK;

    if (buf == NULL || len == 0u || (len & 0x1u) != 0u || (addr_byte & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    total_words = len / 2u;
    chunk_words_limit = BURNER_SPI_MAX_XFER / bytes_per_word;
    if (chunk_words_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (words_done < total_words) {
        size_t chunk_words = total_words - words_done;
        size_t seq_len;
        uint8_t *tx_sequence = NULL;
        uint8_t *rx_sequence = NULL;
        bool free_tx_sequence = false;
        bool free_rx_sequence = false;

        if (chunk_words > chunk_words_limit) {
            chunk_words = chunk_words_limit;
        }
        seq_len = chunk_words * bytes_per_word;

        tx_sequence = burner_spi_alloc_tx_buffer(seq_len, &free_tx_sequence);
        rx_sequence = burner_spi_alloc_rw_buffer(seq_len, &free_rx_sequence);
        if (tx_sequence == NULL || rx_sequence == NULL) {
            if (free_tx_sequence && tx_sequence != NULL) {
                free(tx_sequence);
            }
            if (free_rx_sequence && rx_sequence != NULL) {
                free(rx_sequence);
            }
            return ESP_ERR_NO_MEM;
        }

        for (size_t i = 0u; i < chunk_words; ++i) {
            uint32_t word_addr = (addr_byte >> 1) + (uint32_t)words_done + (uint32_t)i;
            size_t base = i * bytes_per_word;

            tx_sequence[base + 0u] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
            tx_sequence[base + 1u] = (uint8_t)(word_addr & 0xFFu);
            tx_sequence[base + 2u] = (uint8_t)((word_addr >> 8) & 0xFFu);
            tx_sequence[base + 3u] = (uint8_t)((word_addr >> 16) & 0xFFu);
            tx_sequence[base + 4u] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
            tx_sequence[base + 5u] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
            tx_sequence[base + 6u] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
            tx_sequence[base + 7u] = 0x00u;
            tx_sequence[base + 8u] = 0x00u;
            tx_sequence[base + 9u] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
        }

        err = burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0, tx_sequence, rx_sequence, seq_len);
        if (err == ESP_OK) {
            for (size_t i = 0u; i < chunk_words; ++i) {
                size_t base = i * bytes_per_word;
                buf[(words_done + i) * 2u + 0u] = rx_sequence[base + 7u];
                buf[(words_done + i) * 2u + 1u] = rx_sequence[base + 8u];
            }
        }

        if (free_tx_sequence) {
            free(tx_sequence);
        }
        if (free_rx_sequence) {
            free(rx_sequence);
        }
        if (err != ESP_OK) {
            return err;
        }

        words_done += chunk_words;
    }

    return ESP_OK;
}
#endif

static esp_err_t burner_bacon_rom_verify_read_packed_hoststyle(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    size_t read_len_word;
    size_t chunk_len_limit;
    size_t chunk_words_limit;
    size_t words_done = 0u;
    uint8_t setup[5];
    uint8_t release;
    uint8_t *tx_chunk = NULL;
    uint8_t *rx_chunk = NULL;
    bool free_tx_chunk = false;
    bool free_rx_chunk = false;
    bool session_open = false;
    bool release_sent = false;
    esp_err_t err;

    if (buf == NULL || len == 0u || (len & 0x1u) != 0u || (addr_byte & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    read_len_word = len / 2u;
    chunk_len_limit = BURNER_SPI_STREAM_CHUNK_BYTES;
    if (chunk_len_limit == 0u || chunk_len_limit > BURNER_SPI_MAX_XFER) {
        chunk_len_limit = BURNER_SPI_MAX_XFER;
    }
    chunk_len_limit -= (chunk_len_limit % 4u);
    if (chunk_len_limit == 0u) {
        chunk_len_limit = 4u;
    }
    chunk_words_limit = chunk_len_limit / 4u;
    if (chunk_words_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_chunk = burner_spi_alloc_tx_buffer(chunk_len_limit, &free_tx_chunk);
    rx_chunk = burner_spi_alloc_rw_buffer(chunk_len_limit, &free_rx_chunk);
    if (tx_chunk == NULL || rx_chunk == NULL) {
        if (free_tx_chunk && tx_chunk != NULL) {
            free(tx_chunk);
        }
        if (free_rx_chunk && rx_chunk != NULL) {
            free(rx_chunk);
        }
        return ESP_ERR_NO_MEM;
    }

    /* Match Bacon host bacon_romRead(): setup once, then per-word RD low + 16-bit sample. */
    setup[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    setup[1] = (uint8_t)((addr_byte >> 1) & 0xFFu);
    setup[2] = (uint8_t)(((addr_byte >> 1) >> 8) & 0xFFu);
    setup[3] = (uint8_t)(((addr_byte >> 1) >> 16) & 0xFFu);
    setup[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    release = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_0);
    if (err != ESP_OK) {
        goto rom_verify_hoststyle_out;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto rom_verify_hoststyle_out;
    }

    while (words_done < read_len_word) {
        size_t chunk_words = read_len_word - words_done;
        size_t chunk_len;

        if (chunk_words > chunk_words_limit) {
            chunk_words = chunk_words_limit;
        }
        chunk_len = chunk_words * 4u;

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 4u;
            tx_chunk[base + 0u] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
            tx_chunk[base + 1u] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
            tx_chunk[base + 2u] = 0x00u;
            tx_chunk[base + 3u] = 0x00u;
        }

        err = burner_spi_transfer_active(tx_chunk, rx_chunk, chunk_len);
        if (err != ESP_OK) {
            goto rom_verify_hoststyle_out;
        }

        for (size_t i = 0u; i < chunk_words; ++i) {
            size_t base = i * 4u;
            buf[(words_done + i) * 2u + 0u] = rx_chunk[base + 2u];
            buf[(words_done + i) * 2u + 1u] = rx_chunk[base + 3u];
        }

        words_done += chunk_words;
    }

    err = burner_spi_transfer_active(&release, NULL, 1u);
    if (err == ESP_OK) {
        release_sent = true;
    }

rom_verify_hoststyle_out:
    if (session_open) {
        if (!release_sent) {
            (void)burner_spi_transfer_active(&release, NULL, 1u);
        }
        burner_spi_end_cs(BURNER_SPI_CS_MODE_0);
    }
    if (free_tx_chunk && tx_chunk != NULL) {
        free(tx_chunk);
    }
    if (free_rx_chunk && rx_chunk != NULL) {
        free(rx_chunk);
    }
    return err;
}

static esp_err_t burner_bacon_rom_read(uint32_t addr_byte, uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_read_packed(addr_byte, buf, len);
}

static esp_err_t burner_bacon_rom_read_u16(uint32_t word_addr, uint16_t *out_value)
{
    uint8_t tx_seq[10];
    uint8_t rx_seq[10] = {0};
    esp_err_t err;

    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tx_seq[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    tx_seq[1] = (uint8_t)(word_addr & 0xFFu);
    tx_seq[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    tx_seq[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    tx_seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    tx_seq[5] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    tx_seq[6] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
    tx_seq[7] = 0x00u;
    tx_seq[8] = 0x00u;
    tx_seq[9] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0, tx_seq, rx_seq, sizeof(tx_seq));
    if (err != ESP_OK) {
        return err;
    }
    *out_value = (uint16_t)((uint16_t)rx_seq[7] | ((uint16_t)rx_seq[8] << 8));
    return ESP_OK;
}

static esp_err_t burner_bacon_rom_read_u16_batched(uint32_t start_word_addr, uint8_t *out, size_t word_count)
{
    if (out == NULL || word_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_read(start_word_addr << 1, out, word_count * 2u);
}

#if 0
/* Legacy GBA verify wrapper kept only for reference. */
static esp_err_t burner_bacon_rom_verify_read_u16_batched(uint32_t start_word_addr, uint8_t *out, size_t word_count)
{
    if (out == NULL || word_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_verify_read_packed(start_word_addr << 1, out, word_count * 2u);
}
#endif

static esp_err_t burner_bacon_rom_verify_read_u16_batched_hoststyle(uint32_t start_word_addr, uint8_t *out, size_t word_count)
{
    if (out == NULL || word_count == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_rom_verify_read_packed_hoststyle(start_word_addr << 1, out, word_count * 2u);
}

static esp_err_t burner_bacon_wait_u16(uint32_t byte_addr, uint16_t expected, uint32_t timeout_ms)
{
    int64_t deadline_us;
    uint16_t read_back = 0;
    esp_err_t err = ESP_OK;

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(byte_addr >> 1, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if (read_back == expected) {
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    ESP_LOGW(
        BURNER_TAG,
        "GBA program wait timeout @0x%08" PRIX32 ": expected=0x%04X read=0x%04X timeout=%" PRIu32 "ms",
        byte_addr,
        expected,
        read_back,
        timeout_ms);
    (void)burner_bacon_gba_reset_to_read_mode();
    return ESP_ERR_TIMEOUT;
}

static inline uint16_t burner_apply_d0d1_swap_on_read(uint16_t data, bool is_swapped);
static inline uint16_t burner_apply_d0d1_swap_on_write(uint16_t data, bool is_swapped);
static esp_err_t burner_bacon_gba_read_id(uint8_t id_out[8], bool is_swapped);
static esp_err_t burner_bacon_gba_read_id_with_cmdset(
    uint8_t id_out[8],
    bool is_swapped,
    burner_nor_cmdset_t cmdset,
    const char *trace_name);

static esp_err_t burner_bacon_wait_u16_mask(
    uint32_t byte_addr,
    uint16_t mask,
    uint16_t expected,
    uint32_t timeout_ms,
    uint16_t *read_back_out)
{
    int64_t deadline_us;
    uint16_t read_back = 0;
    esp_err_t err = ESP_OK;

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(byte_addr >> 1, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        read_back = burner_apply_d0d1_swap_on_read(read_back, s_cart_ctx.d0d1_swapped);
        if ((read_back & mask) == expected) {
            if (read_back_out != NULL) {
                *read_back_out = read_back;
            }
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    if (read_back_out != NULL) {
        *read_back_out = read_back;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gba_intel_reset(void)
{
    esp_err_t err;
    uint64_t reset_start_us = burner_gba_diag_now_us();

    err = burner_bacon_gba_command_write_u16(0x000u, 0x0050u);
    if (err != ESP_OK) {
        burner_gba_chis_diag_add_reset(burner_gba_diag_now_us() - reset_start_us);
        return err;
    }
    err = burner_bacon_gba_command_write_u16(0x000u, 0x00FFu);
    burner_gba_chis_diag_add_reset(burner_gba_diag_now_us() - reset_start_us);
    return err;
}

static esp_err_t burner_bacon_gba_reset_to_read_mode_for_cmdset(burner_nor_cmdset_t cmdset)
{
    if (cmdset == BURNER_NOR_CMDSET_INTEL) {
        return burner_bacon_gba_intel_reset();
    }
    return burner_bacon_gba_command_write_u16(0x000u, 0x00F0u);
}

static esp_err_t burner_bacon_gba_reset_to_read_mode(void)
{
    if (burner_gba_gbx_is_active()) {
        return burner_gba_gbx_reset_to_read_mode(false, false, 0u);
    }
    return burner_bacon_gba_reset_to_read_mode_for_cmdset(s_cart_ctx.gba_cmdset);
}

esp_err_t burner_bacon_gba_finalize_write(bool is_multi_card)
{
    static const uint32_t reset_stride_bytes = 0x00100000u;
    uint32_t addr;
    esp_err_t err;

    if (burner_gba_gbx_is_active()) {
        return burner_gba_gbx_finalize_write(is_multi_card);
    }
    if (burner_gba_chis_intel_uses_strict_chislink_flow()) {
        ESP_LOGI(BURNER_TAG, "GBA Intel final reset: ChisLink single reset");
        return burner_bacon_gba_reset_to_read_mode();
    }
    if (!burner_gba_nor_is_intel_active() ||
        (s_gba_active_nor_flags & BURNER_NOR_FLAG_RESET_EVERY_1MB) == 0u) {
        return burner_bacon_gba_reset_to_read_mode();
    }
    if (s_cart_ctx.device_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA Intel final reset sweep: stride=0x%06" PRIX32 " flash=%" PRIu32 "MB multi=%u",
        reset_stride_bytes,
        s_cart_ctx.device_size / (1024u * 1024u),
        is_multi_card ? 1u : 0u);

    for (addr = 0u; addr < s_cart_ctx.device_size; addr += reset_stride_bytes) {
        uint32_t bank = 0u;
        uint32_t bank_word_addr = addr >> 1;

        if (is_multi_card) {
            bank = addr / BURN_GBA_BANK_BYTES;
            bank_word_addr = (addr % BURN_GBA_BANK_BYTES) >> 1;
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        err = burner_bacon_gba_command_write_u16(bank_word_addr, 0x0050u);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(bank_word_addr, 0x00FFu);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (is_multi_card) {
        err = burner_gba_switch_bank_if_needed(0u);
        if (err != ESP_OK) {
            return err;
        }
    }
    return burner_bacon_gba_reset_to_read_mode();
}

static esp_err_t burner_bacon_gba_intel_wait_ready(uint32_t flash_addr, uint16_t command, uint32_t timeout_ms, uint16_t *status_out)
{
    esp_err_t err;
    uint64_t wait_start_us = burner_gba_diag_now_us();

    if (command != 0u) {
        err = burner_bacon_gba_command_write_u16(flash_addr >> 1, command);
        if (err != ESP_OK) {
            burner_gba_chis_diag_add_wait_ready(burner_gba_diag_now_us() - wait_start_us);
            return err;
        }
    }
    err = burner_bacon_wait_u16_mask(flash_addr, 0x0080u, 0x0080u, timeout_ms, status_out);
    burner_gba_chis_diag_add_wait_ready(burner_gba_diag_now_us() - wait_start_us);
    return err;
}

#define BURNER_GBA_INTEL_STATUS_DEVICE_PROTECT 0x0002u
#define BURNER_GBA_INTEL_STATUS_VPP_LOW 0x0008u
#define BURNER_GBA_INTEL_STATUS_PROGRAM_FAIL 0x0010u
#define BURNER_GBA_INTEL_STATUS_ERASE_FAIL 0x0020u
#define BURNER_GBA_INTEL_STATUS_ERROR_MASK                                                   \
    (BURNER_GBA_INTEL_STATUS_DEVICE_PROTECT | BURNER_GBA_INTEL_STATUS_VPP_LOW |             \
     BURNER_GBA_INTEL_STATUS_PROGRAM_FAIL | BURNER_GBA_INTEL_STATUS_ERASE_FAIL)

static bool burner_gba_intel_status_has_error(uint16_t status)
{
    return (status & BURNER_GBA_INTEL_STATUS_ERROR_MASK) != 0u;
}

static void burner_gba_intel_status_format_flags(char *out, size_t out_len, uint16_t status)
{
    int written = 0;

    if (out == NULL || out_len == 0u) {
        return;
    }

    out[0] = '\0';
    if ((status & BURNER_GBA_INTEL_STATUS_DEVICE_PROTECT) != 0u) {
        written += snprintf(out + written, out_len - (size_t)written, "%sprotect", written > 0 ? "|" : "");
    }
    if ((status & BURNER_GBA_INTEL_STATUS_VPP_LOW) != 0u && (size_t)written < out_len) {
        written += snprintf(out + written, out_len - (size_t)written, "%svpp", written > 0 ? "|" : "");
    }
    if ((status & BURNER_GBA_INTEL_STATUS_PROGRAM_FAIL) != 0u && (size_t)written < out_len) {
        written += snprintf(out + written, out_len - (size_t)written, "%sprogram", written > 0 ? "|" : "");
    }
    if ((status & BURNER_GBA_INTEL_STATUS_ERASE_FAIL) != 0u && (size_t)written < out_len) {
        written += snprintf(out + written, out_len - (size_t)written, "%serase", written > 0 ? "|" : "");
    }
    if (written == 0) {
        snprintf(out, out_len, "none");
    }
}

static void burner_gba_intel_log_status_error(const char *op, uint32_t byte_addr, uint16_t status)
{
    char flags[32];

    burner_gba_intel_status_format_flags(flags, sizeof(flags), status);
    ESP_LOGW(
        BURNER_TAG,
        "GBA intel %s status error @0x%08" PRIX32 " status=0x%04X flags=%s",
        op != NULL ? op : "op",
        byte_addr,
        status,
        flags);
    if ((status & BURNER_GBA_INTEL_STATUS_DEVICE_PROTECT) != 0u) {
        ESP_LOGW(BURNER_TAG, "GBA intel %s hint: chip reports protect; try Unlock PPB before burn", op != NULL ? op : "op");
    }
}

static uint16_t burner_gba_intel_buffer_entry_command(void)
{
    return (((s_gba_active_nor_flags & BURNER_NOR_FLAG_INTEL_88B0) != 0u) ||
            s_gba_active_intel_e9_entry)
               ? 0x00E9u
               : 0x00E8u;
}

static uint16_t burner_gba_program_buffer_write_bytes(uint16_t reported_bytes, burner_nor_cmdset_t cmdset)
{
    (void)cmdset;
    return reported_bytes;
}

typedef struct {
    bool valid;
    bool d0d1_swapped;
    uint16_t intel_entry_command;
    uint32_t unlock0_addr;
    uint32_t unlock1_addr;
    uint8_t opt_addr;
    uint8_t opt_wr_setup;
    uint8_t opt_wr_data;
    uint8_t opt_wr_low;
    uint8_t opt_release;
    uint8_t unlock0_addr_bytes[3];
    uint8_t unlock1_addr_bytes[3];
    uint16_t amd_aa;
    uint16_t amd_55;
    uint16_t amd_a0;
    uint16_t amd_25;
    uint16_t amd_29;
    uint16_t intel_d0;
} burner_gba_program_cmd_cache_t;

static burner_gba_program_cmd_cache_t s_gba_program_cmd_cache = {0};

static void burner_gba_patch_u24(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
}

static const burner_gba_program_cmd_cache_t *burner_gba_program_cmd_cache_get(void)
{
    uint32_t unlock0_addr = burner_gba_unlock_addr0();
    uint32_t unlock1_addr = burner_gba_unlock_addr1();
    uint16_t intel_entry_command = burner_gba_intel_buffer_entry_command();

    if (s_gba_program_cmd_cache.valid &&
        s_gba_program_cmd_cache.d0d1_swapped == s_cart_ctx.d0d1_swapped &&
        s_gba_program_cmd_cache.intel_entry_command == intel_entry_command &&
        s_gba_program_cmd_cache.unlock0_addr == unlock0_addr &&
        s_gba_program_cmd_cache.unlock1_addr == unlock1_addr) {
        return &s_gba_program_cmd_cache;
    }

    memset(&s_gba_program_cmd_cache, 0, sizeof(s_gba_program_cmd_cache));
    s_gba_program_cmd_cache.valid = true;
    s_gba_program_cmd_cache.d0d1_swapped = s_cart_ctx.d0d1_swapped;
    s_gba_program_cmd_cache.intel_entry_command = intel_entry_command;
    s_gba_program_cmd_cache.unlock0_addr = unlock0_addr;
    s_gba_program_cmd_cache.unlock1_addr = unlock1_addr;
    s_gba_program_cmd_cache.opt_addr = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    s_gba_program_cmd_cache.opt_wr_setup = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    s_gba_program_cmd_cache.opt_wr_data = burner_bacon_option_byte0(2, true, true, true, false, true, true);
    s_gba_program_cmd_cache.opt_wr_low = burner_bacon_option_byte0(0, true, true, true, false, true, false);
    s_gba_program_cmd_cache.opt_release = burner_bacon_option_byte0(0, true, true, true, true, true, true);
    burner_gba_patch_u24(s_gba_program_cmd_cache.unlock0_addr_bytes, unlock0_addr);
    burner_gba_patch_u24(s_gba_program_cmd_cache.unlock1_addr_bytes, unlock1_addr);
    s_gba_program_cmd_cache.amd_aa = burner_apply_d0d1_swap_on_write(0x00AAu, s_cart_ctx.d0d1_swapped);
    s_gba_program_cmd_cache.amd_55 = burner_apply_d0d1_swap_on_write(0x0055u, s_cart_ctx.d0d1_swapped);
    s_gba_program_cmd_cache.amd_a0 = burner_apply_d0d1_swap_on_write(0x00A0u, s_cart_ctx.d0d1_swapped);
    s_gba_program_cmd_cache.amd_25 = burner_apply_d0d1_swap_on_write(0x0025u, s_cart_ctx.d0d1_swapped);
    s_gba_program_cmd_cache.amd_29 = burner_apply_d0d1_swap_on_write(0x0029u, s_cart_ctx.d0d1_swapped);
    s_gba_program_cmd_cache.intel_d0 = burner_apply_d0d1_swap_on_write(0x00D0u, s_cart_ctx.d0d1_swapped);
    return &s_gba_program_cmd_cache;
}

static bool burner_gba_chis_intel_uses_strict_chislink_flow(void)
{
    return !burner_gba_gbx_is_active() &&
           s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL &&
           s_cart_ctx.probe_cfi_ok;
}

static void burner_gba_sector_erase_ctx_reset(void)
{
    memset(&s_gba_sector_erase_ctx, 0, sizeof(s_gba_sector_erase_ctx));
}

static esp_err_t burner_gba_sector_is_blank(
    uint32_t sector_addr,
    uint32_t sector_size,
    bool is_multi_card,
    bool *blank_out);

static void burner_gba_sector_erase_ctx_begin(
    uint32_t range_begin,
    uint32_t range_end,
    uint32_t sector_size,
    bool multi_card,
    bool erase_always)
{
    burner_gba_sector_erase_ctx_reset();
    (void)sector_size;
    if (!burner_nor_geometry_is_valid(&s_cart_ctx.geometry) || range_end < range_begin) {
        return;
    }
    if (burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry, range_begin, &s_gba_sector_erase_ctx.cursor) !=
        ESP_OK) {
        burner_gba_sector_erase_ctx_reset();
        return;
    }

    s_gba_sector_erase_ctx.active = true;
    s_gba_sector_erase_ctx.multi_card = multi_card;
    s_gba_sector_erase_ctx.erase_always = erase_always;
    s_gba_sector_erase_ctx.range_end = range_end;
    s_gba_sector_erase_ctx.erased_sector_addr = UINT32_MAX;
    s_gba_sector_erase_ctx.pre_erased_sector_addr = UINT32_MAX;
    s_gba_sector_erase_ctx.pre_erased_valid = false;
}

static bool burner_gba_sector_erase_ctx_should_handle(void)
{
    return s_gba_sector_erase_ctx.active && burner_nor_geometry_is_valid(&s_cart_ctx.geometry);
}

static esp_err_t burner_gba_sector_erase_ctx_sync_cursor(
    uint32_t byte_addr,
    uint32_t *sector_addr_out,
    uint32_t *sector_end_out,
    uint32_t *sector_size_out)
{
    esp_err_t err;

    err = burner_nor_geometry_region_cursor_seek_forward(&s_cart_ctx.geometry, byte_addr, &s_gba_sector_erase_ctx.cursor);
    if (err != ESP_OK) {
        return err;
    }
    return burner_nor_geometry_sector_bounds_in_cursor(
        &s_gba_sector_erase_ctx.cursor,
        byte_addr,
        sector_addr_out,
        sector_end_out,
        sector_size_out);
}

static esp_err_t burner_gba_sector_erase_now(uint32_t sector_addr, uint32_t sector_size)
{
    esp_err_t err;

    if (sector_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_gba_sector_erase_ctx.erase_always) {
        bool blank = false;

        err = burner_gba_sector_is_blank(
            sector_addr,
            sector_size,
            s_gba_sector_erase_ctx.multi_card,
            &blank);
        if (err != ESP_OK) {
            return err;
        }
        if (blank) {
            burner_status_advance_erase_phase(1u, sector_size);
            return ESP_OK;
        }
    }

    burner_status_mark_erase_begin();
    err = burner_bacon_gba_erase_sector(
        sector_addr,
        s_gba_sector_erase_ctx.multi_card,
        burner_erase_timeout_ms_for_bytes(sector_size));
    burner_status_mark_erase_end();
    if (err == ESP_OK) {
        burner_status_advance_erase_phase(1u, sector_size);
    }
    return err;
}

static esp_err_t burner_gba_sector_erase_prepare_current(uint32_t byte_addr)
{
    uint32_t sector_addr;
    uint32_t sector_size = 0u;
    esp_err_t err;

    if (!burner_gba_sector_erase_ctx_should_handle()) {
        return ESP_OK;
    }

    err = burner_gba_sector_erase_ctx_sync_cursor(byte_addr, &sector_addr, NULL, &sector_size);
    if (err != ESP_OK || sector_size == 0u) {
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }
    if (s_gba_sector_erase_ctx.erased_sector_addr == sector_addr) {
        return ESP_OK;
    }

    if (s_gba_sector_erase_ctx.pre_erased_valid &&
        s_gba_sector_erase_ctx.pre_erased_sector_addr == sector_addr) {
        s_gba_sector_erase_ctx.erased_sector_addr = sector_addr;
        s_gba_sector_erase_ctx.pre_erased_valid = false;
        s_gba_sector_erase_ctx.pre_erased_sector_addr = UINT32_MAX;
        return ESP_OK;
    }

    err = burner_gba_sector_erase_now(sector_addr, sector_size);
    if (err != ESP_OK) {
        return err;
    }
    s_gba_sector_erase_ctx.erased_sector_addr = sector_addr;
    return ESP_OK;
}

static esp_err_t burner_gba_sector_erase_prefetch_next(uint32_t byte_addr, size_t len)
{
    uint32_t current_sector_addr;
    uint32_t current_sector_end;
    uint32_t next_sector_addr;
    uint32_t current_sector_size = 0u;
    uint32_t next_sector_size = 0u;
    uint32_t chunk_end;
    burner_nor_region_cursor_t next_cursor;
    esp_err_t err;

    if (!burner_gba_sector_erase_ctx_should_handle() || len == 0u) {
        return ESP_OK;
    }

    err = burner_gba_sector_erase_ctx_sync_cursor(
        byte_addr,
        &current_sector_addr,
        &current_sector_end,
        &current_sector_size);
    if (err != ESP_OK || current_sector_size == 0u) {
        return err;
    }
    current_sector_end -= 1u;
    chunk_end = byte_addr + (uint32_t)len - 1u;
    if (chunk_end < current_sector_end) {
        return ESP_OK;
    }

    next_cursor = s_gba_sector_erase_ctx.cursor;
    next_sector_addr = current_sector_addr + current_sector_size;
    next_sector_size = current_sector_size;
    if (next_sector_addr >= next_cursor.addr_end) {
        err = burner_nor_geometry_region_cursor_advance(&s_cart_ctx.geometry, &next_cursor);
        if (err != ESP_OK) {
            return ESP_OK;
        }
        next_sector_addr = next_cursor.addr_begin;
        next_sector_size = next_cursor.sector_size;
    }
    if (next_sector_addr > s_gba_sector_erase_ctx.range_end || next_sector_size == 0u) {
        return ESP_OK;
    }
    if (s_gba_sector_erase_ctx.pre_erased_valid &&
        s_gba_sector_erase_ctx.pre_erased_sector_addr == next_sector_addr) {
        return ESP_OK;
    }
    if (s_gba_sector_erase_ctx.erased_sector_addr == next_sector_addr) {
        return ESP_OK;
    }

    err = burner_gba_sector_erase_now(next_sector_addr, next_sector_size);
    if (err != ESP_OK) {
        return err;
    }
    s_gba_sector_erase_ctx.pre_erased_valid = true;
    s_gba_sector_erase_ctx.pre_erased_sector_addr = next_sector_addr;
    s_gba_sector_erase_ctx.erased_sector_addr = current_sector_addr;
    return ESP_OK;
}

static bool burner_gba_nor_is_intel_active(void)
{
    return s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL;
}

static bool burner_gba_uses_flashgbx_bank_select_type1(void)
{
    if ((s_gba_active_nor_flags & BURNER_NOR_FLAG_INTEL_88B0) != 0u) {
        return true;
    }
    if (!burner_gba_gbx_is_active() &&
        s_gba_active_intel_generic_cfi &&
        s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL &&
        s_cart_ctx.device_size > BURN_GBA_LINEAR_ADDR_BYTES) {
        return true;
    }
    return s_cart_ctx.gbx.active &&
           s_cart_ctx.gbx.has_flash_bank_select_type &&
           s_cart_ctx.gbx.flash_bank_select_type == 1u;
}

static const char *burner_gba_multicard_mapping_name(void)
{
    if ((s_gba_active_nor_flags & BURNER_NOR_FLAG_INTEL_88B0) != 0u) {
        return "FlashGBX Intel 32MB-bank";
    }
    if (!burner_gba_gbx_is_active() &&
        s_gba_active_intel_generic_cfi &&
        s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL &&
        s_cart_ctx.device_size > BURN_GBA_LINEAR_ADDR_BYTES) {
        return "FlashGBX Intel generic 32MB-bank";
    }
    if (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_AMD) {
        return "Bacon AMD 32MB-bank";
    }
    return "32MB-bank multicart";
}

static esp_err_t burner_bacon_gba_rom_switch_bank(uint8_t bank)
{
    uint8_t high = (uint8_t)((bank & 0x0Fu) << 4);
    uint8_t low = 0x40u;
    uint8_t mode = 0x00u;
    esp_err_t err = ESP_OK;

    err = burner_bacon_ram_write(0x0002u, &high, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_ram_write(0x0003u, &low, 1u);
    if (err != ESP_OK) {
        return err;
    }
    if (!burner_gba_uses_flashgbx_bank_select_type1()) {
        return ESP_OK;
    }
    return burner_bacon_ram_write(0x0004u, &mode, 1u);
}

static size_t burner_gba_program_safe_chunk_bytes(uint32_t byte_addr, size_t requested_len, size_t write_unit_bytes)
{
    size_t chunk = requested_len;
    uint32_t boundary_remain;

    if (chunk == 0u) {
        return 0u;
    }

    if (write_unit_bytes >= 2u && (write_unit_bytes & (write_unit_bytes - 1u)) == 0u) {
        boundary_remain = (uint32_t)(write_unit_bytes - (byte_addr & (uint32_t)(write_unit_bytes - 1u)));
        if (boundary_remain > 0u && chunk > boundary_remain) {
            chunk = boundary_remain;
        }
    }

    boundary_remain = BURN_GBA_BANK_BYTES - (byte_addr % BURN_GBA_BANK_BYTES);
    if (boundary_remain > 0u && chunk > boundary_remain) {
        chunk = boundary_remain;
    }

    chunk &= ~((size_t)0x1u);
    return (chunk == 0u) ? 2u : chunk;
}

static uint32_t burner_gba_sector_begin_for_addr(uint32_t byte_addr)
{
    uint32_t sector_begin = byte_addr;

    if (burner_nor_geometry_sector_bounds(&s_cart_ctx.geometry, byte_addr, &sector_begin, NULL, NULL) == ESP_OK) {
        return sector_begin;
    }
    if (s_cart_ctx.sector_size > 0u && (s_cart_ctx.sector_size & (s_cart_ctx.sector_size - 1u)) == 0u) {
        return byte_addr & ~(s_cart_ctx.sector_size - 1u);
    }
    return byte_addr;
}

#define BURNER_GBA_INTEL_64B_TEMPLATE_WORDS 32u
#define BURNER_GBA_INTEL_64B_TEMPLATE_SEQ_LEN (23u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS)

typedef struct {
    bool valid;
    bool d0d1_swapped;
    uint16_t write_count_word;
    uint16_t intel_d0;
    uint8_t opt_addr;
    uint8_t opt_wr_setup;
    uint8_t opt_wr_data;
    uint8_t opt_wr_low;
    uint8_t opt_release;
    uint8_t seq[BURNER_GBA_INTEL_64B_TEMPLATE_SEQ_LEN];
} burner_gba_intel_64b_template_t;

static burner_gba_intel_64b_template_t s_gba_intel_64b_template = {0};

static bool burner_gba_intel_64b_template_matches(
    const burner_gba_program_cmd_cache_t *cmd,
    uint16_t write_count_word)
{
    return cmd != NULL &&
           s_gba_intel_64b_template.valid &&
           s_gba_intel_64b_template.d0d1_swapped == s_cart_ctx.d0d1_swapped &&
           s_gba_intel_64b_template.write_count_word == write_count_word &&
           s_gba_intel_64b_template.intel_d0 == cmd->intel_d0 &&
           s_gba_intel_64b_template.opt_addr == cmd->opt_addr &&
           s_gba_intel_64b_template.opt_wr_setup == cmd->opt_wr_setup &&
           s_gba_intel_64b_template.opt_wr_data == cmd->opt_wr_data &&
           s_gba_intel_64b_template.opt_wr_low == cmd->opt_wr_low &&
           s_gba_intel_64b_template.opt_release == cmd->opt_release;
}

static void burner_gba_intel_64b_template_build(
    const burner_gba_program_cmd_cache_t *cmd,
    uint16_t write_count_word)
{
    uint8_t *seq;

    if (cmd == NULL) {
        memset(&s_gba_intel_64b_template, 0, sizeof(s_gba_intel_64b_template));
        return;
    }

    memset(&s_gba_intel_64b_template, 0, sizeof(s_gba_intel_64b_template));
    s_gba_intel_64b_template.valid = true;
    s_gba_intel_64b_template.d0d1_swapped = s_cart_ctx.d0d1_swapped;
    s_gba_intel_64b_template.write_count_word = write_count_word;
    s_gba_intel_64b_template.intel_d0 = cmd->intel_d0;
    s_gba_intel_64b_template.opt_addr = cmd->opt_addr;
    s_gba_intel_64b_template.opt_wr_setup = cmd->opt_wr_setup;
    s_gba_intel_64b_template.opt_wr_data = cmd->opt_wr_data;
    s_gba_intel_64b_template.opt_wr_low = cmd->opt_wr_low;
    s_gba_intel_64b_template.opt_release = cmd->opt_release;

    seq = s_gba_intel_64b_template.seq;
    seq[0] = cmd->opt_addr;
    seq[4] = cmd->opt_wr_setup;
    seq[5] = cmd->opt_wr_data;
    seq[6] = (uint8_t)(write_count_word & 0xFFu);
    seq[7] = (uint8_t)((write_count_word >> 8) & 0xFFu);
    seq[8] = cmd->opt_wr_low;
    seq[9] = cmd->opt_wr_setup;
    seq[10] = cmd->opt_release;

    for (size_t wr = 0u; wr < BURNER_GBA_INTEL_64B_TEMPLATE_WORDS; ++wr) {
        size_t base = 11u + 10u * wr;

        seq[base + 0u] = cmd->opt_addr;
        seq[base + 4u] = cmd->opt_wr_setup;
        seq[base + 5u] = cmd->opt_wr_data;
        seq[base + 8u] = cmd->opt_wr_low;
        seq[base + 9u] = cmd->opt_wr_setup;
    }

    seq[11u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = cmd->opt_release;
    seq[12u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = cmd->opt_addr;
    seq[16u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = cmd->opt_wr_setup;
    seq[17u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = cmd->opt_wr_data;
    seq[18u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = (uint8_t)(cmd->intel_d0 & 0xFFu);
    seq[19u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = (uint8_t)((cmd->intel_d0 >> 8) & 0xFFu);
    seq[20u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = cmd->opt_wr_low;
    seq[21u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = cmd->opt_wr_setup;
    seq[22u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS] = cmd->opt_release;
}

static esp_err_t burner_bacon_gba_intel_buffered_program_once(
    uint32_t starting_address,
    const uint8_t *buf,
    size_t remain_len,
    uint16_t buffer_write_bytes,
    size_t *written_out)
{
    uint32_t starting_word_address;
    size_t write_len;
    size_t write_words;
    size_t max_write_words_by_spi = 1u;
    size_t seq_len;
    uint8_t *seq;
    bool free_seq = false;
    uint8_t addr0;
    uint8_t addr1;
    uint8_t addr2;
    uint32_t command_address;
    uint32_t command_word_address;
    uint16_t status = 0u;
    uint16_t write_count_word;
    uint16_t entry_cmd;
    size_t wr;
    esp_err_t err;
    const burner_gba_program_cmd_cache_t *cmd;
    uint64_t once_start_us = burner_gba_diag_now_us();
    uint64_t build_us = 0u;
    uint64_t entry_wait_us = 0u;
    uint64_t spi_us = 0u;
    uint64_t done_wait_us = 0u;
    uint64_t reset_us = 0u;
    uint64_t t0;

    if (written_out != NULL) {
        *written_out = 0u;
    }
    if (buf == NULL || written_out == NULL || remain_len < 2u || buffer_write_bytes < 2u ||
        (starting_address & 0x1u) != 0u || (remain_len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    starting_word_address = starting_address >> 1;
    write_len = remain_len;
    if (write_len > buffer_write_bytes) {
        write_len = buffer_write_bytes;
    }
    write_len = burner_gba_program_safe_chunk_bytes(
        starting_address,
        write_len,
        (size_t)buffer_write_bytes);
    if (write_len < 2u) {
        write_len = 2u;
    }
    write_words = write_len / 2u;
    if (BURNER_SPI_MAX_XFER > 23u) {
        max_write_words_by_spi = (BURNER_SPI_MAX_XFER - 23u) / 10u;
    }
    if (max_write_words_by_spi == 0u) {
        max_write_words_by_spi = 1u;
    }
    if (write_words > max_write_words_by_spi) {
        write_words = max_write_words_by_spi;
        write_len = write_words * 2u;
    }

    command_address = starting_address;
    command_word_address = command_address >> 1;
    cmd = burner_gba_program_cmd_cache_get();
    entry_cmd = cmd->intel_entry_command;
    t0 = burner_gba_diag_now_us();
    err = burner_bacon_gba_intel_wait_ready(command_address, entry_cmd, BURNER_ROM_POLL_TIMEOUT_MS, &status);
    entry_wait_us = burner_gba_diag_now_us() - t0;
    if (err != ESP_OK) {
        return err;
    }

    seq_len = 23u + 10u * write_words;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }
    t0 = burner_gba_diag_now_us();
    seq = burner_spi_alloc_tx_buffer(seq_len, &free_seq);
    if (seq == NULL) {
        return ESP_ERR_NO_MEM;
    }

    write_count_word = burner_apply_d0d1_swap_on_write(
        (uint16_t)(write_words - 1u),
        s_cart_ctx.d0d1_swapped);

    if (write_words == BURNER_GBA_INTEL_64B_TEMPLATE_WORDS) {
        if (!burner_gba_intel_64b_template_matches(cmd, write_count_word)) {
            burner_gba_intel_64b_template_build(cmd, write_count_word);
        }
        memcpy(seq, s_gba_intel_64b_template.seq, BURNER_GBA_INTEL_64B_TEMPLATE_SEQ_LEN);
        burner_gba_patch_u24(&seq[1], command_word_address);

        for (wr = 0u; wr < BURNER_GBA_INTEL_64B_TEMPLATE_WORDS; ++wr) {
            uint32_t word_address = starting_word_address + (uint32_t)wr;
            size_t base = 11u + 10u * wr;

            burner_gba_patch_u24(&seq[base + 1u], word_address);
            seq[base + 6u] = buf[wr * 2u];
            seq[base + 7u] = buf[wr * 2u + 1u];
        }
        burner_gba_patch_u24(&seq[13u + 10u * BURNER_GBA_INTEL_64B_TEMPLATE_WORDS], command_word_address);
    } else {
        addr0 = (uint8_t)(command_word_address & 0xFFu);
        addr1 = (uint8_t)((command_word_address >> 8) & 0xFFu);
        addr2 = (uint8_t)((command_word_address >> 16) & 0xFFu);

        seq[0] = cmd->opt_addr;
        seq[1] = addr0;
        seq[2] = addr1;
        seq[3] = addr2;
        seq[4] = cmd->opt_wr_setup;
        seq[5] = cmd->opt_wr_data;
        seq[6] = (uint8_t)(write_count_word & 0xFFu);
        seq[7] = (uint8_t)((write_count_word >> 8) & 0xFFu);
        seq[8] = cmd->opt_wr_low;
        seq[9] = cmd->opt_wr_setup;
        seq[10] = cmd->opt_release;
        /* Bacon CS0 has no address auto-increment; match ChisLink's per-halfword AGB_FLASH_WRITE(pa+i). */
        for (wr = 0u; wr < write_words; ++wr) {
            uint32_t word_address = starting_word_address + (uint32_t)wr;
            size_t base = 11u + 10u * wr;

            seq[base + 0u] = cmd->opt_addr;
            seq[base + 1u] = (uint8_t)(word_address & 0xFFu);
            seq[base + 2u] = (uint8_t)((word_address >> 8) & 0xFFu);
            seq[base + 3u] = (uint8_t)((word_address >> 16) & 0xFFu);
            seq[base + 4u] = cmd->opt_wr_setup;
            seq[base + 5u] = cmd->opt_wr_data;
            seq[base + 6u] = buf[wr * 2u];
            seq[base + 7u] = buf[wr * 2u + 1u];
            seq[base + 8u] = cmd->opt_wr_low;
            seq[base + 9u] = cmd->opt_wr_setup;
        }

        seq[11u + 10u * write_words] = cmd->opt_release;
        seq[12u + 10u * write_words] = cmd->opt_addr;
        seq[13u + 10u * write_words] = addr0;
        seq[14u + 10u * write_words] = addr1;
        seq[15u + 10u * write_words] = addr2;
        seq[16u + 10u * write_words] = cmd->opt_wr_setup;
        seq[17u + 10u * write_words] = cmd->opt_wr_data;
        seq[18u + 10u * write_words] = (uint8_t)(cmd->intel_d0 & 0xFFu);
        seq[19u + 10u * write_words] = (uint8_t)((cmd->intel_d0 >> 8) & 0xFFu);
        seq[20u + 10u * write_words] = cmd->opt_wr_low;
        seq[21u + 10u * write_words] = cmd->opt_wr_setup;
        seq[22u + 10u * write_words] = cmd->opt_release;
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

    t0 = burner_gba_diag_now_us();
    err = burner_bacon_gba_intel_wait_ready(command_address, 0x0000u, BURNER_ROM_POLL_TIMEOUT_MS, &status);
    done_wait_us = burner_gba_diag_now_us() - t0;
    if (err != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA intel buffered program timeout @0x%08" PRIX32 " status=0x%04X",
            starting_address,
            status);
        return err;
    }
    if (burner_gba_intel_status_has_error(status)) {
        burner_gba_intel_log_status_error("buffered-program", starting_address, status);
        (void)burner_bacon_gba_intel_reset();
        return ESP_ERR_INVALID_RESPONSE;
    }
    t0 = burner_gba_diag_now_us();
    err = burner_bacon_gba_command_write_u16(command_word_address, 0x00FFu);
    reset_us = burner_gba_diag_now_us() - t0;
    burner_gba_chis_diag_add_program_once(
        write_len,
        burner_gba_diag_now_us() - once_start_us,
        build_us,
        spi_us,
        entry_wait_us,
        done_wait_us,
        reset_us);
    if (err != ESP_OK) {
        return err;
    }

    *written_out = write_len;
    return ESP_OK;
}

static void burner_gba_resolve_write_addr(
    uint32_t rom_addr,
    bool is_multi_card,
    uint32_t *bank_out,
    uint32_t *local_addr_out,
    uint32_t *bank_remain_out)
{
    uint32_t bank = 0u;
    uint32_t local_addr = rom_addr;
    uint32_t bank_remain = UINT32_MAX - rom_addr;
    uint32_t bank_off;

    if (is_multi_card) {
        bank = rom_addr / BURN_GBA_BANK_BYTES;
        bank_off = rom_addr % BURN_GBA_BANK_BYTES;
        local_addr = bank_off;
        bank_remain = BURN_GBA_BANK_BYTES - bank_off;
    }

    if (bank_out != NULL) {
        *bank_out = bank;
    }
    if (local_addr_out != NULL) {
        *local_addr_out = local_addr;
    }
    if (bank_remain_out != NULL) {
        *bank_remain_out = bank_remain;
    }
}

static esp_err_t burner_gba_switch_bank_if_needed(uint32_t bank)
{
    esp_err_t err;
    uint64_t bank_start_us;

    if (bank > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (bank == (uint32_t)s_cart_ctx.current_bank) {
        return ESP_OK;
    }

    bank_start_us = burner_gba_diag_now_us();
    err = burner_bacon_gba_rom_switch_bank((uint8_t)bank);
    if (err != ESP_OK) {
        burner_gba_chis_diag_add_bank_switch(burner_gba_diag_now_us() - bank_start_us);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BURNER_GBA_BANK_SWITCH_SETTLE_MS));
    (void)burner_bacon_gba_reset_to_read_mode();
    vTaskDelay(pdMS_TO_TICKS(BURNER_GBA_BANK_SWITCH_SETTLE_MS));
    s_cart_ctx.current_bank = (uint16_t)bank;
    burner_gba_chis_diag_add_bank_switch(burner_gba_diag_now_us() - bank_start_us);
    return ESP_OK;
}

static bool burner_gba_should_log_program_boundary(uint32_t byte_addr, size_t bytes, uint32_t processed, uint32_t total)
{
    uint32_t chunk_end;

    if (bytes == 0u) {
        return false;
    }
    if (processed == 0u || processed + (uint32_t)bytes >= total) {
        return true;
    }
    if ((byte_addr % BURN_GBA_BANK_BYTES) == 0u) {
        return true;
    }
    chunk_end = byte_addr + (uint32_t)bytes;
    return (chunk_end % BURN_GBA_BANK_BYTES) == 0u;
}

const char *burner_gba_cmd_addr_mode_name(burner_gba_cmd_addr_mode_t mode)
{
    switch (mode) {
    case BURNER_GBA_CMD_ADDR_BYTE_X16:
        return "byte-x16";
    case BURNER_GBA_CMD_ADDR_BYTE:
        return "byte";
    case BURNER_GBA_CMD_ADDR_WORD:
    default:
        return "word";
    }
}

const char *burner_gba_cmd_data_lane_name(burner_gba_cmd_data_lane_t lane)
{
    switch (lane) {
    case BURNER_GBA_CMD_DATA_HIGH:
        return "high";
    case BURNER_GBA_CMD_DATA_LOW:
    default:
        return "low";
    }
}

static uint32_t burner_gba_unlock_addr0(void)
{
    return BURNER_GBA_HOST_UNLOCK_ADDR0;
}

static uint32_t burner_gba_unlock_addr1(void)
{
    return BURNER_GBA_HOST_UNLOCK_ADDR1;
}

static uint32_t burner_gba_cfi_enter_addr(void)
{
    return BURNER_GBA_HOST_CFI_ENTER_ADDR;
}

static esp_err_t burner_gba_diag_read_word_raw(
    burner_spi_cs_mode_t mode,
    uint32_t word_addr,
    uint16_t *value_out,
    uint8_t *raw_out,
    size_t raw_len)
{
    size_t turnaround_hold_bytes;
    size_t seq_len;
    size_t data_base;
    uint8_t tx_sequence[16] = {0};
    uint8_t rx_sequence[16] = {0};
    esp_err_t err;

    if (value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    turnaround_hold_bytes = BURNER_GBA_READ_TURNAROUND_HOLD_BYTES;
    seq_len = 4u + 1u + (4u + turnaround_hold_bytes) + 1u;
    data_base = 5u + 1u + turnaround_hold_bytes;
    if (seq_len > sizeof(tx_sequence) || seq_len > sizeof(rx_sequence)) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_sequence[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    tx_sequence[1] = (uint8_t)(word_addr & 0xFFu);
    tx_sequence[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    tx_sequence[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    tx_sequence[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    tx_sequence[5] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    for (size_t hold = 0u; hold < turnaround_hold_bytes; ++hold) {
        tx_sequence[6u + hold] = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    }
    tx_sequence[6u + turnaround_hold_bytes] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
    tx_sequence[7u + turnaround_hold_bytes] = 0x00u;
    tx_sequence[8u + turnaround_hold_bytes] = 0x00u;
    tx_sequence[seq_len - 1u] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_transfer_cs(mode, tx_sequence, rx_sequence, seq_len);
    if (err != ESP_OK) {
        return err;
    }

    *value_out = (uint16_t)((uint16_t)rx_sequence[data_base + 1u] |
                            ((uint16_t)rx_sequence[data_base + 2u] << 8));
    if (raw_out != NULL && raw_len > 0u) {
        if (raw_len > seq_len) {
            raw_len = seq_len;
        }
        memcpy(raw_out, rx_sequence, raw_len);
    }
    return ESP_OK;
}

static size_t burner_gba_diag_raw_seq_len(void)
{
    return 4u + 1u + (4u + BURNER_GBA_READ_TURNAROUND_HOLD_BYTES) + 1u;
}

static esp_err_t burner_gba_diag_read_word_segmented(
    burner_spi_cs_mode_t mode,
    uint32_t word_addr,
    uint16_t *value_out,
    uint8_t raw_read[3])
{
    uint8_t setup[5];
    uint8_t rd_low;
    uint8_t tx_read[3];
    uint8_t rx_read[3] = {0};
    uint8_t release;
    bool session_open = false;
    bool release_sent = false;
    esp_err_t err;

    if (value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    setup[0] = burner_bacon_option_byte0(3, true, true, true, true, true, true);
    setup[1] = (uint8_t)(word_addr & 0xFFu);
    setup[2] = (uint8_t)((word_addr >> 8) & 0xFFu);
    setup[3] = (uint8_t)((word_addr >> 16) & 0xFFu);
    setup[4] = burner_bacon_option_byte0(0, true, true, true, false, true, true);
    rd_low = burner_bacon_option_byte0(0, true, false, true, false, false, true);
    tx_read[0] = burner_bacon_option_byte0(2, true, false, true, false, true, true);
    tx_read[1] = 0x00u;
    tx_read[2] = 0x00u;
    release = burner_bacon_option_byte0(0, true, true, true, true, true, true);

    err = burner_spi_begin_cs(mode);
    if (err != ESP_OK) {
        return err;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto segmented_out;
    }

    err = burner_spi_transfer_active(&rd_low, NULL, 1u);
    if (err != ESP_OK) {
        goto segmented_out;
    }
    if (BURNER_GBA_ROM_PHASE_GAP_US > 0u) {
        esp_rom_delay_us(BURNER_GBA_ROM_PHASE_GAP_US);
    }

    err = burner_spi_transfer_active(tx_read, rx_read, sizeof(tx_read));
    if (err != ESP_OK) {
        goto segmented_out;
    }

    err = burner_spi_transfer_active(&release, NULL, 1u);
    if (err == ESP_OK) {
        release_sent = true;
    }

segmented_out:
    if (session_open) {
        if (!release_sent) {
            (void)burner_spi_transfer_active(&release, NULL, 1u);
        }
        burner_spi_end_cs(mode);
    }
    if (raw_read != NULL) {
        memcpy(raw_read, rx_read, sizeof(rx_read));
    }
    *value_out = (uint16_t)((uint16_t)rx_read[1] | ((uint16_t)rx_read[2] << 8));
    return err;
}

void burner_format_hex_bytes(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t i;
    size_t pos = 0u;

    if (out == NULL || out_len == 0u) {
        return;
    }

    out[0] = '\0';
    if (data == NULL) {
        return;
    }

    for (i = 0u; i < len && pos + 1u < out_len; ++i) {
        int written = snprintf(out + pos, out_len - pos, (i + 1u < len) ? "%02X " : "%02X", data[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= (out_len - pos)) {
            pos = out_len - 1u;
            break;
        }
        pos += (size_t)written;
    }

    out[out_len - 1u] = '\0';
}

static void burner_gba_diag_capture_cs_levels(burner_spi_cs_mode_t mode, int *cs0_level, int *cs1_level)
{
#if BURNER_SPI_ENABLE
    uint32_t cs_setup_delay_us = burner_spi_cs_setup_delay_us(mode);

    if (cs0_level != NULL) {
        *cs0_level = -1;
    }
    if (cs1_level != NULL) {
        *cs1_level = -1;
    }

    burner_spi_apply_cs_mode(mode);
    if (cs_setup_delay_us > 0u) {
        esp_rom_delay_us(cs_setup_delay_us);
    }
    if (cs0_level != NULL) {
        *cs0_level = gpio_get_level(MORI_PIN_MCU_SPI_CS);
    }
    if (cs1_level != NULL) {
        *cs1_level = gpio_get_level(MORI_PIN_MCU_SPI_CS1);
    }
    burner_spi_release_cs();
#else
    (void)mode;
    if (cs0_level != NULL) {
        *cs0_level = -1;
    }
    if (cs1_level != NULL) {
        *cs1_level = -1;
    }
#endif
}

static void burner_log_gba_diag_compare_word(const char *phase, uint32_t word_addr)
{
    uint8_t raw_mode0[16] = {0};
    uint8_t raw_mode2[16] = {0};
    uint8_t seg_mode0[3] = {0};
    uint8_t seg_mode2[3] = {0};
    char raw_mode0_hex[sizeof(raw_mode0) * 3u] = {0};
    char raw_mode2_hex[sizeof(raw_mode2) * 3u] = {0};
    char seg_mode0_hex[sizeof(seg_mode0) * 3u] = {0};
    char seg_mode2_hex[sizeof(seg_mode2) * 3u] = {0};
    size_t raw_len = burner_gba_diag_raw_seq_len();
    uint16_t value_mode0 = 0;
    uint16_t value_mode2 = 0;
    uint16_t seg_value_mode0 = 0;
    uint16_t seg_value_mode2 = 0;
    uint16_t seg_alt01_mode0 = 0;
    uint16_t seg_alt01_mode2 = 0;
    int cs0_mode0 = -1;
    int cs1_mode0 = -1;
    int cs0_mode2 = -1;
    int cs1_mode2 = -1;
    esp_err_t err_mode0;
    esp_err_t err_mode2;
    esp_err_t seg_err_mode0;
    esp_err_t seg_err_mode2;

    if (phase == NULL) {
        phase = "raw";
    }
    if (raw_len > sizeof(raw_mode0) || raw_len > sizeof(raw_mode2)) {
        ESP_LOGW(BURNER_TAG, "GBA raw %s diag buffer too small", phase);
        return;
    }

    burner_gba_diag_capture_cs_levels(BURNER_SPI_CS_MODE_0, &cs0_mode0, &cs1_mode0);
    burner_gba_diag_capture_cs_levels(BURNER_SPI_CS_MODE_2, &cs0_mode2, &cs1_mode2);

    err_mode0 = burner_gba_diag_read_word_raw(
        BURNER_SPI_CS_MODE_0, word_addr, &value_mode0, raw_mode0, raw_len);
    err_mode2 = burner_gba_diag_read_word_raw(
        BURNER_SPI_CS_MODE_2, word_addr, &value_mode2, raw_mode2, raw_len);
    seg_err_mode0 = burner_gba_diag_read_word_segmented(
        BURNER_SPI_CS_MODE_0, word_addr, &seg_value_mode0, seg_mode0);
    seg_err_mode2 = burner_gba_diag_read_word_segmented(
        BURNER_SPI_CS_MODE_2, word_addr, &seg_value_mode2, seg_mode2);
    if (err_mode0 != ESP_OK || err_mode2 != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA raw %s @%03" PRIX32 " failed: mode0=%s mode2=%s",
            phase,
            word_addr,
            esp_err_to_name(err_mode0),
            esp_err_to_name(err_mode2));
        return;
    }

    burner_format_hex_bytes(raw_mode0, raw_len, raw_mode0_hex, sizeof(raw_mode0_hex));
    burner_format_hex_bytes(raw_mode2, raw_len, raw_mode2_hex, sizeof(raw_mode2_hex));
    burner_format_hex_bytes(seg_mode0, sizeof(seg_mode0), seg_mode0_hex, sizeof(seg_mode0_hex));
    burner_format_hex_bytes(seg_mode2, sizeof(seg_mode2), seg_mode2_hex, sizeof(seg_mode2_hex));
    seg_alt01_mode0 = (uint16_t)((uint16_t)seg_mode0[0] | ((uint16_t)seg_mode0[1] << 8));
    seg_alt01_mode2 = (uint16_t)((uint16_t)seg_mode2[0] | ((uint16_t)seg_mode2[1] << 8));
    ESP_LOGI(
        BURNER_TAG,
        "GBA raw %s @%03" PRIX32 ": mode0 cs0=%d cs1=%d val=%04X rx=%s | mode2 cs0=%d cs1=%d val=%04X rx=%s",
        phase,
        word_addr,
        cs0_mode0,
        cs1_mode0,
        value_mode0,
        raw_mode0_hex,
        cs0_mode2,
        cs1_mode2,
        value_mode2,
        raw_mode2_hex);
    if (seg_err_mode0 == ESP_OK && seg_err_mode2 == ESP_OK) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA seg %s @%03" PRIX32 ": mode0 val12=%04X val01=%04X rx=%s | mode2 val12=%04X val01=%04X rx=%s",
            phase,
            word_addr,
            seg_value_mode0,
            seg_alt01_mode0,
            seg_mode0_hex,
            seg_value_mode2,
            seg_alt01_mode2,
            seg_mode2_hex);
    } else {
        ESP_LOGW(
            BURNER_TAG,
            "GBA seg %s @%03" PRIX32 " failed: mode0=%s mode2=%s",
            phase,
            word_addr,
            esp_err_to_name(seg_err_mode0),
            esp_err_to_name(seg_err_mode2));
    }

    if (cs0_mode0 != 0 || cs1_mode0 != 1) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA raw %s @%03" PRIX32 " mode0 CS readback unexpected: cs0=%d cs1=%d",
            phase,
            word_addr,
            cs0_mode0,
            cs1_mode0);
    }
    if (memcmp(raw_mode0, raw_mode2, raw_len) == 0) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA raw %s @%03" PRIX32 " mode0 and mode2 returned identical SPI bytes",
            phase,
            word_addr);
    }
}

/*
 * Some repro GBA flash carts swap D0/D1 between the cart edge and the flash
 * chip. Commands and chip-generated data (ID/CFI/PPB) must be translated, but
 * normal ROM payload bytes are left as the cart-edge logical data.
 */
#define SWAP_D0D1_U8(data) (((data) & 0xFCU) | (((data) & 0x01U) << 1) | (((data) & 0x02U) >> 1))
#define SWAP_D0D1_U16(data) \
    ((uint16_t)(((data) & 0xFFFCU) | (((data) & 0x0001U) << 1) | (((data) & 0x0002U) >> 1)))

static inline uint16_t burner_apply_d0d1_swap_on_read(uint16_t data, bool is_swapped)
{
    return is_swapped ? SWAP_D0D1_U16(data) : data;
}

static inline uint16_t burner_apply_d0d1_swap_on_write(uint16_t data, bool is_swapped)
{
    return is_swapped ? SWAP_D0D1_U16(data) : data;
}

static esp_err_t burner_bacon_gba_command_write_u16(uint32_t word_addr, uint16_t value)
{
    return burner_bacon_rom_write_u16(
        word_addr,
        burner_apply_d0d1_swap_on_write(value, s_cart_ctx.d0d1_swapped));
}

static void burner_readid_trace_begin(const char *name, int64_t *start_us, int64_t *last_us)
{
    int64_t now_us = esp_timer_get_time();

    if (start_us != NULL) {
        *start_us = now_us;
    }
    if (last_us != NULL) {
        *last_us = now_us;
    }
    ESP_LOGI(BURNER_TAG, "%s begin t=%" PRId64 "us", name, now_us);
}

static void burner_readid_trace_log_u16(
    const char *name,
    const char *op,
    uint32_t addr,
    uint16_t data,
    int64_t *last_us)
{
    int64_t now_us = esp_timer_get_time();
    int64_t dt_us = 0;
    int64_t dt_sec = 0;
    int64_t dt_sub_us = 0;

    if (last_us != NULL) {
        dt_us = now_us - *last_us;
        *last_us = now_us;
    }
    dt_sec = dt_us / 1000000;
    dt_sub_us = dt_us % 1000000;
    if (dt_sub_us < 0) {
        dt_sub_us = -dt_sub_us;
    }

    ESP_LOGI(
        BURNER_TAG,
        "%s %s addr=0x%03" PRIX32 " data=0x%04X dt=%" PRId64 "us (%" PRId64 ".%06" PRId64 "s)",
        name,
        op,
        addr,
        data,
        dt_us,
        dt_sec,
        dt_sub_us);
}

static void burner_readid_trace_log_u8(
    const char *name,
    const char *op,
    uint32_t addr,
    uint8_t data,
    int64_t *last_us)
{
    int64_t now_us = esp_timer_get_time();
    int64_t dt_us = 0;
    int64_t dt_sec = 0;
    int64_t dt_sub_us = 0;

    if (last_us != NULL) {
        dt_us = now_us - *last_us;
        *last_us = now_us;
    }
    dt_sec = dt_us / 1000000;
    dt_sub_us = dt_us % 1000000;
    if (dt_sub_us < 0) {
        dt_sub_us = -dt_sub_us;
    }

    ESP_LOGI(
        BURNER_TAG,
        "%s %s addr=0x%04" PRIX32 " data=0x%02X dt=%" PRId64 "us (%" PRId64 ".%06" PRId64 "s)",
        name,
        op,
        addr,
        data,
        dt_us,
        dt_sec,
        dt_sub_us);
}

static void burner_readid_trace_end(const char *name, int64_t start_us, esp_err_t err)
{
    int64_t total_us = esp_timer_get_time() - start_us;
    int64_t total_sec = total_us / 1000000;
    int64_t total_sub_us = total_us % 1000000;

    if (total_sub_us < 0) {
        total_sub_us = -total_sub_us;
    }

    ESP_LOGI(
        BURNER_TAG,
        "%s end status=%s total=%" PRId64 "us (%" PRId64 ".%06" PRId64 "s)",
        name,
        esp_err_to_name(err),
        total_us,
        total_sec,
        total_sub_us);
}

static void burner_log_gba_id_window(void)
{
    static const uint32_t probe_words[] = {0x000u, 0x001u, 0x002u, 0x003u, 0x00Eu, 0x00Fu, 0x010u, 0x011u};
    uint16_t values[sizeof(probe_words) / sizeof(probe_words[0])] = {0};
    esp_err_t err = ESP_OK;
    bool entered_id = false;
    size_t i;

    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug ID window enter step1 failed: %s", esp_err_to_name(err));
        return;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug ID window enter step2 failed: %s", esp_err_to_name(err));
        goto id_window_out;
    }
    err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0090u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug ID window enter step3 failed: %s", esp_err_to_name(err));
        goto id_window_out;
    }
    entered_id = true;

    for (i = 0u; i < (sizeof(probe_words) / sizeof(probe_words[0])); ++i) {
        err = burner_bacon_rom_read_u16(probe_words[i], &values[i]);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA debug ID window read failed at 0x%03" PRIX32 ": %s",
                probe_words[i],
                esp_err_to_name(err));
            goto id_window_out;
        }
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA ID window: [000]=%04X [001]=%04X [002]=%04X [003]=%04X [00E]=%04X [00F]=%04X [010]=%04X [011]=%04X",
        values[0],
        values[1],
        values[2],
        values[3],
        values[4],
        values[5],
        values[6],
        values[7]);

id_window_out:
    if (entered_id) {
        burner_log_gba_diag_compare_word("id", 0x000u);
    }
    (void)burner_bacon_gba_reset_to_read_mode();
}

static void burner_log_gba_cfi_window(void)
{
    static const uint32_t probe_words[] = {0x010u, 0x011u, 0x012u, 0x027u, 0x02Au, 0x02Du, 0x02Eu, 0x02Fu, 0x030u};
    uint16_t values[sizeof(probe_words) / sizeof(probe_words[0])] = {0};
    esp_err_t err = ESP_OK;
    bool entered_cfi = false;
    size_t i;

    err = burner_bacon_gba_command_write_u16(burner_gba_cfi_enter_addr(), 0x0098u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "GBA debug CFI window enter failed: %s", esp_err_to_name(err));
        return;
    }
    entered_cfi = true;

    for (i = 0u; i < (sizeof(probe_words) / sizeof(probe_words[0])); ++i) {
        err = burner_bacon_rom_read_u16(probe_words[i], &values[i]);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA debug CFI window read failed at 0x%03" PRIX32 ": %s",
                probe_words[i],
                esp_err_to_name(err));
            goto cfi_window_out;
        }
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA CFI window: [010]=%04X [011]=%04X [012]=%04X [027]=%04X [02A]=%04X [02D]=%04X [02E]=%04X [02F]=%04X [030]=%04X",
        values[0],
        values[1],
        values[2],
        values[3],
        values[4],
        values[5],
        values[6],
        values[7],
        values[8]);

cfi_window_out:
    if (entered_cfi) {
        burner_log_gba_diag_compare_word("cfi", 0x010u);
    }
    (void)burner_bacon_gba_reset_to_read_mode();
}

static bool burner_gba_detect_qry_words(uint16_t q, uint16_t r, uint16_t y, bool *is_swapped_out, bool *high_byte_lane_out)
{
    for (uint8_t lane = 0u; lane < 2u; ++lane) {
        bool high_byte_lane = (lane != 0u);
        uint8_t q_byte = high_byte_lane ? (uint8_t)((q >> 8) & 0xFFu) : (uint8_t)(q & 0xFFu);
        uint8_t r_byte = high_byte_lane ? (uint8_t)((r >> 8) & 0xFFu) : (uint8_t)(r & 0xFFu);
        uint8_t y_byte = high_byte_lane ? (uint8_t)((y >> 8) & 0xFFu) : (uint8_t)(y & 0xFFu);
        uint8_t q_swap = SWAP_D0D1_U8(q_byte);
        uint8_t r_swap = SWAP_D0D1_U8(r_byte);
        uint8_t y_swap = SWAP_D0D1_U8(y_byte);

        if (q_byte == 0x51u && r_byte == 0x52u && y_byte == 0x59u) {
            *is_swapped_out = false;
            *high_byte_lane_out = high_byte_lane;
            return true;
        }
        if (q_swap == 0x51u && r_swap == 0x52u && y_swap == 0x59u) {
            *is_swapped_out = true;
            *high_byte_lane_out = high_byte_lane;
            return true;
        }
    }
    return false;
}

/*
 * Detect D0/D1 swap by reading the CFI "QRY" signature. Keep this aligned with
 * burner_bacon_gba_get_cfi(): GBA flash is probed in word-address mode, so the
 * canonical signature words are 0x010/0x011/0x012 after entering CFI at 0x055.
 */
static esp_err_t burner_gba_detect_d0d1_swap(
    bool *is_swapped_out,
    burner_gba_cmd_data_lane_t *lane_out)
{
    esp_err_t err;
    uint16_t q = 0;
    uint16_t r = 0;
    uint16_t y = 0;
    bool high_byte_lane = false;

    if (is_swapped_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(BURNER_TAG, "D0/D1 swap detection: entering CFI mode (word addr 0x%03" PRIX32 ")", burner_gba_cfi_enter_addr());

    err = burner_bacon_gba_command_write_u16(burner_gba_cfi_enter_addr(), 0x0098u);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "D0/D1 swap detection: CFI entry write failed: %s", esp_err_to_name(err));
        goto reset_out;
    }

    err = burner_bacon_rom_read_u16(0x010u, &q);
    if (err != ESP_OK) {
        goto reset_out;
    }
    err = burner_bacon_rom_read_u16(0x011u, &r);
    if (err != ESP_OK) {
        goto reset_out;
    }
    err = burner_bacon_rom_read_u16(0x012u, &y);
    if (err != ESP_OK) {
        goto reset_out;
    }

    if (burner_gba_detect_qry_words(q, r, y, is_swapped_out, &high_byte_lane)) {
        if (lane_out != NULL) {
            *lane_out = high_byte_lane ? BURNER_GBA_CMD_DATA_HIGH : BURNER_GBA_CMD_DATA_LOW;
        }
        ESP_LOGI(
            BURNER_TAG,
            "D0/D1 swap detection: CFI 'QRY' detected (%s, %s-byte lane) [010]=%04X [011]=%04X [012]=%04X",
            *is_swapped_out ? "SWAPPED" : "normal",
            high_byte_lane ? "high" : "low",
            q,
            r,
            y);
        err = ESP_OK;
        goto reset_out;
    }

    ESP_LOGW(BURNER_TAG, "D0/D1 swap detection: CFI 'QRY' not detected [010]=%04X [011]=%04X [012]=%04X", q, r, y);
    err = ESP_ERR_NOT_FOUND;

reset_out:
    (void)burner_bacon_gba_reset_to_read_mode();
    return err;
}

static esp_err_t burner_bacon_gba_read_id_with_cmdset(
    uint8_t id_out[8],
    bool is_swapped,
    burner_nor_cmdset_t cmdset,
    const char *trace_name)
{
    esp_err_t err;
    uint16_t w0 = 0;
    uint16_t w1 = 0;
    uint16_t w2 = 0;
    uint16_t w3 = 0;
    int64_t trace_start_us = 0;
    int64_t trace_last_us = 0;
    const char *name = (trace_name != NULL) ? trace_name : "GBA ReadID trace";
    uint16_t reset_word = 0x00F0u;
    bool entered_id_mode = false;

    if (id_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_readid_trace_begin(name, &trace_start_us, &trace_last_us);

    if (cmdset == BURNER_NOR_CMDSET_INTEL) {
        reset_word = 0x00FFu;

        err = burner_bacon_gba_command_write_u16(0x000u, 0x0050u);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s write addr=0x000 data=0x0050 failed: %s", name, esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", 0x000u, 0x0050u, &trace_last_us);

        err = burner_bacon_gba_command_write_u16(0x000u, 0x00FFu);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s write addr=0x000 data=0x00FF failed: %s", name, esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", 0x000u, 0x00FFu, &trace_last_us);

        err = burner_bacon_gba_command_write_u16(0x000u, 0x0090u);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s write addr=0x000 data=0x0090 failed: %s", name, esp_err_to_name(err));
            goto reset_out;
        }
        burner_readid_trace_log_u16(name, "write", 0x000u, 0x0090u, &trace_last_us);
        entered_id_mode = true;
    } else {
        /*
         * Match Bacon host exactly:
         * 1. write  0x555 <- 0x00AA
         * 2. write  0x2AA <- 0x0055
         * 3. write  0x555 <- 0x0090
         * 4. read   word 0x000
         * 5. read   word 0x001
         * 6. read   word 0x00E
         * 7. read   word 0x00F
         * 8. write  0x000 <- 0x00F0
         */
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x00AAu);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "%s write addr=0x%03" PRIX32 " data=0x00AA failed: %s",
                name,
                burner_gba_unlock_addr0(),
                esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", burner_gba_unlock_addr0(), 0x00AAu, &trace_last_us);
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr1(), 0x0055u);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "%s write addr=0x%03" PRIX32 " data=0x0055 failed: %s",
                name,
                burner_gba_unlock_addr1(),
                esp_err_to_name(err));
            burner_readid_trace_end(name, trace_start_us, err);
            return err;
        }
        burner_readid_trace_log_u16(name, "write", burner_gba_unlock_addr1(), 0x0055u, &trace_last_us);
        err = burner_bacon_gba_command_write_u16(burner_gba_unlock_addr0(), 0x0090u);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "%s write addr=0x%03" PRIX32 " data=0x0090 failed: %s",
                name,
                burner_gba_unlock_addr0(),
                esp_err_to_name(err));
            goto reset_out;
        }
        burner_readid_trace_log_u16(name, "write", burner_gba_unlock_addr0(), 0x0090u, &trace_last_us);
        entered_id_mode = true;
    }

    err = burner_bacon_rom_read_u16(0x000u, &w0);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x000 failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x000u, w0, &trace_last_us);
    err = burner_bacon_rom_read_u16(0x001u, &w1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x001 failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x001u, w1, &trace_last_us);
    err = burner_bacon_rom_read_u16(0x00Eu, &w2);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x00E failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x00Eu, w2, &trace_last_us);
    err = burner_bacon_rom_read_u16(0x00Fu, &w3);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "%s read addr=0x00F failed: %s", name, esp_err_to_name(err));
        goto reset_out;
    }
    burner_readid_trace_log_u16(name, "read", 0x00Fu, w3, &trace_last_us);

    /* Apply D0/D1 swap to read data */
    w0 = burner_apply_d0d1_swap_on_read(w0, is_swapped);
    w1 = burner_apply_d0d1_swap_on_read(w1, is_swapped);
    w2 = burner_apply_d0d1_swap_on_read(w2, is_swapped);
    w3 = burner_apply_d0d1_swap_on_read(w3, is_swapped);

    id_out[0] = (uint8_t)(w0 & 0xFFu);
    id_out[1] = (uint8_t)((w0 >> 8) & 0xFFu);
    id_out[2] = (uint8_t)(w1 & 0xFFu);
    id_out[3] = (uint8_t)((w1 >> 8) & 0xFFu);
    id_out[4] = (uint8_t)(w2 & 0xFFu);
    id_out[5] = (uint8_t)((w2 >> 8) & 0xFFu);
    id_out[6] = (uint8_t)(w3 & 0xFFu);
    id_out[7] = (uint8_t)((w3 >> 8) & 0xFFu);

reset_out:
    {
        esp_err_t reset_err = ESP_OK;

        if (entered_id_mode) {
            reset_err = burner_bacon_gba_reset_to_read_mode_for_cmdset(cmdset);
        }
        if (entered_id_mode && reset_err == ESP_OK) {
            if (cmdset == BURNER_NOR_CMDSET_INTEL) {
                burner_readid_trace_log_u16(name, "write", 0x000u, 0x0050u, &trace_last_us);
            }
            burner_readid_trace_log_u16(name, "write", 0x000u, reset_word, &trace_last_us);
        } else if (entered_id_mode && reset_err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "%s reset failed: %s", name, esp_err_to_name(reset_err));
        }
    }
    burner_readid_trace_end(name, trace_start_us, err);
    return err;
}

static esp_err_t burner_bacon_gba_read_id(uint8_t id_out[8], bool is_swapped)
{
    burner_nor_cmdset_t cmdset = s_cart_ctx.gba_cmdset;

    if (cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
        cmdset = BURNER_NOR_CMDSET_AMD;
    }
    return burner_bacon_gba_read_id_with_cmdset(id_out, is_swapped, cmdset, "GBA ReadID trace");
}

static esp_err_t burner_bacon_gba_cfi_read_u8(
    uint32_t word_addr,
    bool high_byte_lane,
    uint8_t *out)
{
    uint16_t word = 0u;
    esp_err_t err;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_rom_read_u16(word_addr, &word);
    if (err != ESP_OK) {
        return err;
    }
    word = burner_apply_d0d1_swap_on_read(word, s_cart_ctx.d0d1_swapped);
    *out = high_byte_lane ? (uint8_t)((word >> 8) & 0xFFu) : (uint8_t)(word & 0xFFu);
    return ESP_OK;
}

static esp_err_t burner_bacon_gba_get_cfi(
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry,
    burner_nor_cmdset_t *cmdset_out,
    uint16_t *primary_cmdset_id_out)
{
    esp_err_t err;
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    uint16_t primary_cmdset_id = 0u;
    uint16_t w10 = 0;
    uint16_t w11 = 0;
    uint16_t w12 = 0;
    uint8_t cmdset_lo = 0u;
    uint8_t cmdset_hi = 0u;
    uint8_t cfi27 = 0;
    uint8_t cfi2a = 0;
    uint8_t cfi2c = 0;
    bool high_byte_lane = false;
    uint32_t enter_addrs[2];
    uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    size_t enter_idx = 0u;
    bool reverse_sector_region = false;
    uint32_t region_count = 0u;

    if (device_size == NULL || sector_size == NULL || buffer_write_bytes == NULL || geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }
    if (primary_cmdset_id_out != NULL) {
        *primary_cmdset_id_out = 0u;
    }
    burner_nor_geometry_clear(geometry);

    enter_addrs[0] = burner_gba_cfi_enter_addr();
    enter_addrs[1] = 0x000u;

    for (enter_idx = 0u; enter_idx < (sizeof(enter_addrs) / sizeof(enter_addrs[0])); ++enter_idx) {
        err = burner_bacon_gba_intel_reset();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_reset_to_read_mode();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gba_command_write_u16(enter_addrs[enter_idx], 0x0098u);
        if (err != ESP_OK) {
            return err;
        }

        err = burner_bacon_rom_read_u16(0x010u, &w10);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_rom_read_u16(0x011u, &w11);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_rom_read_u16(0x012u, &w12);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        if (burner_gba_detect_qry_words(
                burner_apply_d0d1_swap_on_read(w10, s_cart_ctx.d0d1_swapped),
                burner_apply_d0d1_swap_on_read(w11, s_cart_ctx.d0d1_swapped),
                burner_apply_d0d1_swap_on_read(w12, s_cart_ctx.d0d1_swapped),
                &(bool){false},
                &high_byte_lane)) {
            break;
        }
    }

    if (enter_idx >= (sizeof(enter_addrs) / sizeof(enter_addrs[0]))) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA CFI signature mismatch: [010]=%04X [011]=%04X [012]=%04X",
            w10,
            w11,
            w12);
        err = ESP_FAIL;
        goto cfi_reset;
    }

    err = burner_bacon_gba_cfi_read_u8(0x013u, high_byte_lane, &cmdset_lo);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x014u, high_byte_lane, &cmdset_hi);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    primary_cmdset_id = (uint16_t)(((uint16_t)cmdset_hi << 8) | (uint16_t)cmdset_lo);
    cfi_cmdset = burner_nor_cmdset_from_cfi_primary_id(primary_cmdset_id);
    if (cmdset_out != NULL) {
        *cmdset_out = cfi_cmdset;
    }
    if (primary_cmdset_id_out != NULL) {
        *primary_cmdset_id_out = primary_cmdset_id;
    }

    err = burner_bacon_gba_cfi_read_u8(0x027u, high_byte_lane, &cfi27);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x02Au, high_byte_lane, &cfi2a);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x02Cu, high_byte_lane, &cfi2c);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    s_cart_ctx.gba_cmd_data_lane = high_byte_lane ? BURNER_GBA_CMD_DATA_HIGH : BURNER_GBA_CMD_DATA_LOW;

    if (cfi27 >= 31u) {
        err = ESP_FAIL;
        goto cfi_reset;
    }
    *device_size = (1u << cfi27);

    if (cfi2a == 0u) {
        *buffer_write_bytes = 0u;
    } else {
        if (cfi2a >= 16u) {
            err = ESP_FAIL;
            goto cfi_reset;
        }
        *buffer_write_bytes = (uint16_t)(1u << cfi2a);
    }

    region_count = (uint32_t)cfi2c;
    if (region_count == 0u || region_count > BURNER_NOR_GEOMETRY_REGION_MAX) {
        err = ESP_ERR_INVALID_SIZE;
        goto cfi_reset;
    }

    if (cfi_cmdset == BURNER_NOR_CMDSET_INTEL) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA Intel CFI raw: size_exp=0x%02X buf_exp=0x%02X regions=%u lane=%s",
            cfi27,
            cfi2a,
            (unsigned)region_count,
            high_byte_lane ? "high" : "low");
    }

    for (uint32_t i = 0u; i < region_count; ++i) {
        uint8_t count_lo = 0u;
        uint8_t count_hi = 0u;
        uint8_t size_lo = 0u;
        uint8_t size_hi = 0u;
        uint32_t base = 0x02Du + (i * 4u);

        err = burner_bacon_gba_cfi_read_u8(base + 0u, high_byte_lane, &count_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 1u, high_byte_lane, &count_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 2u, high_byte_lane, &size_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 3u, high_byte_lane, &size_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }

        sector_counts[i] = (((uint32_t)count_hi << 8) | (uint32_t)count_lo) + 1u;
        sector_sizes[i] = ((((uint32_t)size_hi << 8) | (uint32_t)size_lo) * 256u);
        if (sector_counts[i] == 0u || sector_sizes[i] == 0u) {
            err = ESP_ERR_INVALID_SIZE;
            goto cfi_reset;
        }
    }

    {
        uint8_t pri_lo = 0u;
        uint8_t pri_hi = 0u;
        uint32_t pri_word_addr = 0u;

        err = burner_bacon_gba_cfi_read_u8(0x015u, high_byte_lane, &pri_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(0x016u, high_byte_lane, &pri_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }

        pri_word_addr = ((uint32_t)pri_hi << 8) | (uint32_t)pri_lo;
        if (pri_word_addr + 0x1Eu >= 0x200u) {
            pri_word_addr = 0x040u;
        }

        if (pri_word_addr + 0x1Eu < 0x200u) {
            uint8_t pri_p = 0u;
            uint8_t pri_r = 0u;
            uint8_t pri_i = 0u;

            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 0u, high_byte_lane, &pri_p);
            if (err != ESP_OK) {
                goto cfi_reset;
            }
            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 1u, high_byte_lane, &pri_r);
            if (err != ESP_OK) {
                goto cfi_reset;
            }
            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 2u, high_byte_lane, &pri_i);
            if (err != ESP_OK) {
                goto cfi_reset;
            }

            if (pri_p == 'P' && pri_r == 'R' && pri_i == 'I') {
                uint8_t tb_boot_sector_raw = 0u;

                err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 0x1Eu, high_byte_lane, &tb_boot_sector_raw);
                if (err != ESP_OK) {
                    goto cfi_reset;
                }
                reverse_sector_region = (tb_boot_sector_raw == 0x03u);
            }
        }
    }

    err = burner_nor_geometry_build(
        geometry,
        *device_size,
        sector_counts,
        sector_sizes,
        region_count,
        reverse_sector_region);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    *sector_size = burner_nor_geometry_report_sector_size(geometry);
    err = ESP_OK;

cfi_reset:
    (void)burner_bacon_gba_reset_to_read_mode();
    return err;
}

static bool burner_bacon_gba_is_s70gl02(const uint8_t id[8])
{
    return burner_gba_nor_has_flag(id, BURNER_NOR_FLAG_DUAL_DIE);
}

static void burner_gba_select_probe_id(
    uint8_t id_out[8],
    const uint8_t amd_id[8],
    bool amd_id_valid,
    const uint8_t intel_id[8],
    bool intel_id_valid,
    burner_nor_cmdset_t cmdset)
{
    if (id_out == NULL) {
        return;
    }
    if (cmdset == BURNER_NOR_CMDSET_INTEL && intel_id_valid) {
        memcpy(id_out, intel_id, 8u);
        return;
    }
    if (amd_id_valid) {
        memcpy(id_out, amd_id, 8u);
        return;
    }
    if (intel_id_valid) {
        memcpy(id_out, intel_id, 8u);
        return;
    }
    memset(id_out, 0, 8u);
}

static bool burner_gba_id_looks_like_intel_style(const uint8_t id[8])
{
    if (id == NULL || id[1] != 0x00u) {
        return false;
    }
    if (id[3] != 0x00u && id[3] != 0x88u && id[3] != 0x89u) {
        return false;
    }

    switch (id[0]) {
        case 0x1Cu: /* Mitsubishi */
        case 0x20u: /* ST/Micron */
        case 0x89u: /* Intel */
        case 0x8Au: /* Intel/Numonyx */
        case 0xB0u: /* Sharp */
            return true;
        default:
            return false;
    }
}

static bool burner_gba_intel_id_uses_e9_entry(const uint8_t id[8])
{
    if (!burner_gba_id_looks_like_intel_style(id)) {
        return false;
    }
    return ((uint16_t)id[2] | ((uint16_t)id[3] << 8)) == 0x88B0u;
}

static bool burner_gba_cfi_primary_is_intel_compat(
    uint16_t primary_id,
    const uint8_t intel_id[8],
    bool intel_id_valid)
{
    if (!intel_id_valid || !burner_gba_id_looks_like_intel_style(intel_id)) {
        return false;
    }

    switch (primary_id) {
        case 0x0100u:
        case 0x0200u:
        case 0x0300u:
            return true;
        default:
            return false;
    }
}

static bool burner_gba_probe_load_entry_geometry(
    const burner_nor_entry_t *entry,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry)
{
    uint32_t entry_device_size;
    uint32_t entry_sector_size;
    uint16_t entry_buffer_write_bytes;

    if (entry == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || geometry == NULL) {
        return false;
    }

    entry_device_size = burner_nor_entry_device_size(entry);
    entry_sector_size = burner_nor_entry_sector_size(entry);
    entry_buffer_write_bytes = burner_nor_entry_buffer_write_bytes(entry);
    if (entry_device_size == 0u || entry_sector_size == 0u) {
        return false;
    }

    *device_size = entry_device_size;
    *sector_size = entry_sector_size;
    *buffer_write_bytes = entry_buffer_write_bytes;
    if (burner_nor_geometry_set_uniform(geometry, entry_device_size, entry_sector_size) != ESP_OK) {
        burner_nor_geometry_clear(geometry);
        return false;
    }
    return true;
}

static bool burner_mbc5_id_is_mx29lv640eb(const uint8_t id[4])
{
    return id != NULL && id[0] == 0xC2u && id[1] == 0xCBu;
}

static bool burner_mbc5_id_is_mx29lv640et(const uint8_t id[4])
{
    return id != NULL && id[0] == 0xC2u && id[1] == 0xC9u;
}

static bool burner_mbc5_probe_load_entry_geometry(
    const uint8_t id[4],
    const burner_nor_entry_t *entry,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry)
{
    uint32_t entry_device_size = 0u;
    uint32_t entry_sector_size = 0u;
    uint16_t entry_buffer_write_bytes = 0u;
    bool entry_ok;

    entry_ok = burner_gba_probe_load_entry_geometry(
        entry,
        &entry_device_size,
        &entry_sector_size,
        &entry_buffer_write_bytes,
        geometry);
    if (!entry_ok || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || geometry == NULL) {
        return false;
    }

    if ((burner_mbc5_id_is_mx29lv640eb(id) || burner_mbc5_id_is_mx29lv640et(id)) &&
        entry_device_size == (8u * 1024u * 1024u) &&
        entry_sector_size == (64u * 1024u)) {
        uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
        uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};

        if (burner_mbc5_id_is_mx29lv640eb(id)) {
            sector_counts[0] = 8u;
            sector_sizes[0] = 8u * 1024u;
            sector_counts[1] = 127u;
            sector_sizes[1] = 64u * 1024u;
        } else {
            sector_counts[0] = 127u;
            sector_sizes[0] = 64u * 1024u;
            sector_counts[1] = 8u;
            sector_sizes[1] = 8u * 1024u;
        }
        if (burner_nor_geometry_build(
                geometry,
                entry_device_size,
                sector_counts,
                sector_sizes,
                2u,
                false) != ESP_OK) {
            return false;
        }
    }

    *device_size = entry_device_size;
    *sector_size = burner_nor_geometry_report_sector_size(geometry);
    *buffer_write_bytes = entry_buffer_write_bytes;
    return true;
}

static bool burner_mbc5_geometry_should_prefer_id(
    const burner_nor_geometry_t *cfi_geometry,
    uint32_t cfi_device_size,
    uint32_t cfi_sector_size,
    const burner_nor_geometry_t *id_geometry,
    uint32_t id_device_size,
    uint32_t id_sector_size)
{
    uint32_t cfi_largest;
    uint32_t id_largest;

    if (!burner_nor_geometry_is_valid(id_geometry) ||
        id_device_size == 0u || id_sector_size == 0u) {
        return false;
    }
    if (!burner_nor_geometry_is_valid(cfi_geometry) ||
        cfi_device_size == 0u || cfi_sector_size == 0u) {
        return true;
    }
    if (cfi_device_size != id_device_size) {
        return false;
    }
    cfi_largest = burner_nor_geometry_largest_sector_size(cfi_geometry);
    id_largest = burner_nor_geometry_largest_sector_size(id_geometry);
    if (cfi_largest > 0u && id_largest > cfi_largest &&
        cfi_largest <= BURN_MBC5_ROM_BANK_BYTES) {
        return true;
    }
    if (!burner_nor_geometry_is_uniform(id_geometry) &&
        !burner_nor_geometry_equal(cfi_geometry, id_geometry)) {
        return true;
    }
    return false;
}

static void burner_gba_build_auto_profile_name(
    char *buf,
    size_t buf_len,
    const uint8_t id[8],
    burner_nor_cmdset_t cmdset)
{
    const char *known_profile = burner_gba_profile_name(id);

    if (buf == NULL || buf_len == 0u) {
        return;
    }
    if (known_profile != NULL && strcmp(known_profile, "unknown") != 0) {
        (void)snprintf(buf, buf_len, "%s", known_profile);
        return;
    }
    (void)snprintf(buf, buf_len, "AGB:%s:auto-cfi", burner_nor_cmdset_name(cmdset));
}

bool burner_gba_id_looks_like_rom_header(const uint8_t id[8])
{
    if (id == NULL) {
        return false;
    }
    /* 01 00 00 EA 08 00 .. .. commonly matches normal ROM vector data. */
    return id[0] == 0x01u &&
           id[1] == 0x00u &&
           id[2] == 0x00u &&
           id[3] == 0xEAu &&
           id[4] == 0x08u &&
           id[5] == 0x00u;
}

static bool burner_gba_id_matches_plain_rom_data(const uint8_t id[8])
{
    uint16_t w0 = 0;
    uint16_t w1 = 0;
    uint16_t w2 = 0;
    uint16_t w3 = 0;
    uint8_t plain[8];
    bool first4_match;

    if (id == NULL) {
        return false;
    }
    if (burner_bacon_rom_read_u16(0x000u, &w0) != ESP_OK ||
        burner_bacon_rom_read_u16(0x001u, &w1) != ESP_OK ||
        burner_bacon_rom_read_u16(0x002u, &w2) != ESP_OK ||
        burner_bacon_rom_read_u16(0x003u, &w3) != ESP_OK) {
        return false;
    }

    plain[0] = (uint8_t)(w0 & 0xFFu);
    plain[1] = (uint8_t)((w0 >> 8) & 0xFFu);
    plain[2] = (uint8_t)(w1 & 0xFFu);
    plain[3] = (uint8_t)((w1 >> 8) & 0xFFu);
    plain[4] = (uint8_t)(w2 & 0xFFu);
    plain[5] = (uint8_t)((w2 >> 8) & 0xFFu);
    plain[6] = (uint8_t)(w3 & 0xFFu);
    plain[7] = (uint8_t)((w3 >> 8) & 0xFFu);
    first4_match = (memcmp(id, plain, 4u) == 0);
    if (memcmp(id, plain, sizeof(plain)) == 0) {
        return true;
    }
    return first4_match;
}

static esp_err_t burner_bacon_gba_probe_after_power_locked(
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out)
{
    esp_err_t err;
    esp_err_t id_err;
    esp_err_t intel_err;
    bool id_looks_like_header = false;
    bool id_matches_plain_rom = false;
    char chip_name[48] = {0};
    char profile_name[48] = {0};
    uint32_t probe_hz = (s_mcu_spi_actual_hz > 0u) ? s_mcu_spi_actual_hz : s_mcu_spi_clock_hz;
    uint32_t attempt = 0u;
    burner_nor_geometry_t cfi_geometry = {0};
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    uint16_t cfi_primary_cmdset_id = 0u;
    uint8_t amd_id[8] = {0};
    uint8_t intel_id[8] = {0};
    uint8_t last_amd_id[8] = {0};
    uint8_t last_intel_id[8] = {0};
    bool amd_id_valid = false;
    bool intel_id_valid = false;
    bool last_amd_id_valid = false;
    bool last_intel_id_valid = false;
    const burner_nor_entry_t *amd_entry = NULL;
    const burner_nor_entry_t *intel_entry = NULL;
    const burner_nor_entry_t *known_entry = NULL;
    burner_nor_cmdset_t resolved_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Lock GBA probe to the legacy command lane/address mapping. */
    s_cart_ctx.gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD;
    s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_gba_active_nor_flags = 0u;
    s_gba_active_intel_generic_cfi = false;
    s_gba_active_intel_e9_entry = false;
    s_cart_ctx.d0d1_known = false;
    s_cart_ctx.d0d1_swapped = false; /* Default: no swap */
    s_cart_ctx.gba_likely_read_only = false;
    burner_nor_geometry_clear(&s_cart_ctx.geometry);

    memset(id_out, 0, 8u);
    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;

    /* Detect D0/D1 swap before reading ID */
    ESP_LOGI(BURNER_TAG, "GBA D0/D1 swap detection starting...");
    err = burner_gba_detect_d0d1_swap(&s_cart_ctx.d0d1_swapped, &s_cart_ctx.gba_cmd_data_lane);
    if (err == ESP_OK) {
        s_cart_ctx.d0d1_known = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBA D0/D1 swap detection: %s, lane=%s",
            s_cart_ctx.d0d1_swapped ? "SWAPPED (D0<->D1)" : "NORMAL (no swap)",
            burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane));
    } else {
        ESP_LOGW(BURNER_TAG, "GBA D0/D1 swap detection failed, assuming normal (no swap)");
        s_cart_ctx.d0d1_known = false;
        s_cart_ctx.d0d1_swapped = false;
        s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    }

    for (attempt = 0u; attempt < BURNER_GBA_CFI_RETRY_COUNT; ++attempt) {
        uint32_t cfi_device_size = 0u;
        uint32_t cfi_sector_size = 0u;
        uint16_t cfi_buffer_write_bytes = 0u;

        memset(amd_id, 0, sizeof(amd_id));
        memset(intel_id, 0, sizeof(intel_id));
        amd_id_valid = false;
        intel_id_valid = false;
        amd_entry = NULL;
        intel_entry = NULL;
        known_entry = NULL;
        resolved_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
        cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
        cfi_primary_cmdset_id = 0u;
        burner_nor_geometry_clear(&cfi_geometry);

        if (attempt > 0u) {
            err = burner_bacon_gba_power_cycle_3v3_locked();
            if (err != ESP_OK) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA word-address probe retry %" PRIu32 " power-cycle failed @%" PRIu32 "Hz: %s",
                    attempt + 1u,
                    probe_hz,
                    esp_err_to_name(err));
                return err;
            }
            err = burner_bacon_gba_release_bus_idle();
            if (err != ESP_OK) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA word-address probe retry %" PRIu32 " release failed @%" PRIu32 "Hz: %s",
                    attempt + 1u,
                    probe_hz,
                    esp_err_to_name(err));
                return err;
            }
        }

        id_err = burner_bacon_gba_read_id_with_cmdset(
            amd_id,
            s_cart_ctx.d0d1_swapped,
            BURNER_NOR_CMDSET_AMD,
            "GBA ReadID trace");
        if (id_err == ESP_OK) {
            amd_id_valid = true;
            amd_entry = burner_nor_db_lookup_gba(amd_id);
            memcpy(last_amd_id, amd_id, sizeof(last_amd_id));
            last_amd_id_valid = true;
            memcpy(id_out, amd_id, sizeof(amd_id));
            id_looks_like_header = burner_gba_id_looks_like_rom_header(id_out);
            id_matches_plain_rom = burner_gba_id_matches_plain_rom_data(id_out);
            ESP_LOGI(
                BURNER_TAG,
                "GBA word-address probe @%" PRIu32 "Hz try=%" PRIu32
                " id=%02X %02X %02X %02X %02X %02X %02X %02X (%s)",
                probe_hz,
                attempt + 1u,
                amd_id[0],
                amd_id[1],
                amd_id[2],
                amd_id[3],
                amd_id[4],
                amd_id[5],
                amd_id[6],
                amd_id[7],
                id_looks_like_header ? "looks-like-rom-header" :
                (id_matches_plain_rom ? "looks-like-plain-rom-data" : "candidate-id"));
            if (amd_entry != NULL && burner_nor_entry_cmdset(amd_entry) == BURNER_NOR_CMDSET_AMD) {
                known_entry = amd_entry;
            }
        } else {
            ESP_LOGW(
                BURNER_TAG,
                "GBA word-address ID read failed (%s) @%" PRIu32 "Hz try=%" PRIu32 ": %s",
                burner_nor_cmdset_name(BURNER_NOR_CMDSET_AMD),
                probe_hz,
                attempt + 1u,
                esp_err_to_name(id_err));
        }

        intel_err = burner_bacon_gba_read_id_with_cmdset(
            intel_id,
            s_cart_ctx.d0d1_swapped,
            BURNER_NOR_CMDSET_INTEL,
            "GBA Intel ReadID trace");
        if (intel_err == ESP_OK) {
            intel_id_valid = true;
            intel_entry = burner_nor_db_lookup_gba(intel_id);
            memcpy(last_intel_id, intel_id, sizeof(last_intel_id));
            last_intel_id_valid = true;
            if (intel_entry != NULL && burner_nor_entry_cmdset(intel_entry) == BURNER_NOR_CMDSET_INTEL) {
                known_entry = intel_entry;
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA intel-id probe @%" PRIu32 "Hz try=%" PRIu32
                    " id=%02X %02X %02X %02X %02X %02X %02X %02X (candidate-id)",
                    probe_hz,
                    attempt + 1u,
                    intel_id[0],
                    intel_id[1],
                    intel_id[2],
                    intel_id[3],
                    intel_id[4],
                    intel_id[5],
                    intel_id[6],
                    intel_id[7]);
            }
        } else if (known_entry == NULL) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA intel-id read failed @%" PRIu32 "Hz try=%" PRIu32 ": %s",
                probe_hz,
                attempt + 1u,
                esp_err_to_name(intel_err));
        }

        err = burner_bacon_gba_get_cfi(
            &cfi_device_size,
            &cfi_sector_size,
            &cfi_buffer_write_bytes,
            &cfi_geometry,
            &cfi_cmdset,
            &cfi_primary_cmdset_id);
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA CFI probe failed in word-address mode @%" PRIu32 "Hz try=%" PRIu32 ": %s",
                probe_hz,
                attempt + 1u,
                esp_err_to_name(err));
            if (known_entry != NULL &&
                burner_nor_entry_cmdset(known_entry) != BURNER_NOR_CMDSET_INTEL &&
                burner_gba_probe_load_entry_geometry(
                    known_entry,
                    device_size,
                    sector_size,
                    buffer_write_bytes,
                    &s_cart_ctx.geometry)) {
                resolved_cmdset = burner_nor_entry_cmdset(known_entry);
                burner_gba_select_probe_id(
                    id_out,
                    amd_id,
                    amd_id_valid,
                    intel_id,
                    intel_id_valid,
                    resolved_cmdset);
                *cfi_ok_out = false;
                s_cart_ctx.gba_cmdset = resolved_cmdset;
                s_cart_ctx.gba_likely_read_only = false;
                burner_nor_format_chip_name(
                    chip_name,
                    sizeof(chip_name),
                    burner_gba_chip_name(id_out),
                    resolved_cmdset,
                    *device_size);
                burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, resolved_cmdset);
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA probe fallback @%" PRIu32 "Hz try=%" PRIu32
                    ": chip=%s profile=%s flash=%" PRIu32 " sector=%" PRIu32
                    " buf=%u cmdset=%s source=library-no-cfi",
                    probe_hz,
                    attempt + 1u,
                    chip_name,
                    profile_name,
                    *device_size,
                    *sector_size,
                    (unsigned)*buffer_write_bytes,
                    burner_nor_cmdset_name(resolved_cmdset));
                return ESP_OK;
            }
            continue;
        }

        if (known_entry == NULL &&
            cfi_cmdset == BURNER_NOR_CMDSET_UNKNOWN &&
            burner_gba_cfi_primary_is_intel_compat(cfi_primary_cmdset_id, intel_id, intel_id_valid)) {
            cfi_cmdset = BURNER_NOR_CMDSET_INTEL;
            ESP_LOGI(
                BURNER_TAG,
                "GBA Intel generic CFI primary compatibility @%" PRIu32 "Hz try=%" PRIu32
                ": primary=0x%04X id=%02X %02X %02X %02X %02X %02X %02X %02X",
                probe_hz,
                attempt + 1u,
                cfi_primary_cmdset_id,
                intel_id[0],
                intel_id[1],
                intel_id[2],
                intel_id[3],
                intel_id[4],
                intel_id[5],
                intel_id[6],
                intel_id[7]);
        }

        if (known_entry != NULL) {
            resolved_cmdset = burner_nor_entry_cmdset(known_entry);
            if (cfi_cmdset != BURNER_NOR_CMDSET_UNKNOWN && cfi_cmdset != resolved_cmdset) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA CFI cmdset mismatch @%" PRIu32 "Hz try=%" PRIu32
                    ": primary=0x%04X cfi=%s library=%s, using library",
                    probe_hz,
                    attempt + 1u,
                    cfi_primary_cmdset_id,
                    burner_nor_cmdset_name(cfi_cmdset),
                    burner_nor_cmdset_name(resolved_cmdset));
            }

            {
                uint32_t entry_device_size = 0u;
                uint32_t entry_sector_size = 0u;
                uint16_t entry_buffer_write_bytes = 0u;
                burner_nor_geometry_t entry_geometry = {0};

                if (resolved_cmdset != BURNER_NOR_CMDSET_INTEL &&
                    burner_gba_probe_load_entry_geometry(
                        known_entry,
                        &entry_device_size,
                        &entry_sector_size,
                        &entry_buffer_write_bytes,
                        &entry_geometry)) {
                    if (cfi_device_size == 0u) {
                        cfi_device_size = entry_device_size;
                    }
                    if (cfi_sector_size == 0u) {
                        cfi_sector_size = entry_sector_size;
                    }
                    if (cfi_buffer_write_bytes == 0u) {
                        cfi_buffer_write_bytes = entry_buffer_write_bytes;
                    }
                    if (!burner_nor_geometry_is_valid(&cfi_geometry)) {
                        cfi_geometry = entry_geometry;
                    }
                }
            }
            burner_gba_select_probe_id(
                id_out,
                amd_id,
                amd_id_valid,
                intel_id,
                intel_id_valid,
                resolved_cmdset);

            burner_nor_format_chip_name(
                chip_name,
                sizeof(chip_name),
                burner_gba_chip_name(id_out),
                resolved_cmdset,
                cfi_device_size);
            burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, resolved_cmdset);
            ESP_LOGI(
                BURNER_TAG,
                "GBA profile match @%" PRIu32 "Hz try=%" PRIu32 ": chip=%s profile=%s cmdset=%s",
                probe_hz,
                attempt + 1u,
                chip_name,
                profile_name,
                burner_nor_cmdset_name(resolved_cmdset));
        } else if (cfi_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA CFI primary cmdset unsupported @%" PRIu32 "Hz try=%" PRIu32
                ": primary=0x%04X (no library match, defaulting to amd)",
                probe_hz,
                attempt + 1u,
                cfi_primary_cmdset_id);
            resolved_cmdset = BURNER_NOR_CMDSET_AMD;
            burner_gba_select_probe_id(
                id_out,
                amd_id,
                amd_id_valid,
                intel_id,
                intel_id_valid,
                resolved_cmdset);
        } else {
            resolved_cmdset = cfi_cmdset;
            burner_gba_select_probe_id(
                id_out,
                amd_id,
                amd_id_valid,
                intel_id,
                intel_id_valid,
                resolved_cmdset);
        }

        if (cfi_device_size == 0u || cfi_sector_size == 0u) {
            if (resolved_cmdset == BURNER_NOR_CMDSET_INTEL) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA Intel probe rejected incomplete CFI geometry @%" PRIu32 "Hz try=%" PRIu32,
                    probe_hz,
                    attempt + 1u);
                continue;
            }
            if (known_entry != NULL &&
                burner_nor_entry_cmdset(known_entry) != BURNER_NOR_CMDSET_INTEL &&
                burner_gba_probe_load_entry_geometry(
                    known_entry,
                    &cfi_device_size,
                    &cfi_sector_size,
                    &cfi_buffer_write_bytes,
                    &cfi_geometry)) {
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA probe geometry repaired from library @%" PRIu32 "Hz try=%" PRIu32,
                    probe_hz,
                    attempt + 1u);
            } else {
                cfi_device_size = BURNER_GBA_FALLBACK_DEVICE_SIZE;
                cfi_sector_size = BURNER_GBA_FALLBACK_SECTOR_SIZE;
                cfi_buffer_write_bytes = BURNER_GBA_FALLBACK_BUFFER_WRITE_BYTES;
                (void)burner_nor_geometry_set_uniform(&cfi_geometry, cfi_device_size, cfi_sector_size);
                ESP_LOGW(
                    BURNER_TAG,
                    "GBA probe geometry fallback @%" PRIu32 "Hz try=%" PRIu32
                    ": flash=%" PRIu32 " sector=%" PRIu32 " buf=%u",
                    probe_hz,
                    attempt + 1u,
                    cfi_device_size,
                    cfi_sector_size,
                    (unsigned)cfi_buffer_write_bytes);
            }
        }

        *device_size = cfi_device_size;
        *sector_size = cfi_sector_size;
        *buffer_write_bytes = cfi_buffer_write_bytes;
        *cfi_ok_out = true;
        s_cart_ctx.geometry = cfi_geometry;
        s_cart_ctx.gba_cmdset = resolved_cmdset;
        s_cart_ctx.gba_likely_read_only = false;
        s_gba_active_intel_generic_cfi =
            (known_entry == NULL) &&
            resolved_cmdset == BURNER_NOR_CMDSET_INTEL;
        s_gba_active_intel_e9_entry =
            resolved_cmdset == BURNER_NOR_CMDSET_INTEL &&
            burner_gba_intel_id_uses_e9_entry(id_out);
        id_looks_like_header = burner_gba_id_looks_like_rom_header(id_out);
        id_matches_plain_rom = burner_gba_id_matches_plain_rom_data(id_out);

        if (s_gba_active_intel_generic_cfi) {
            ESP_LOGI(
                BURNER_TAG,
                "GBA Intel generic CFI compatibility: no ID-library match, using CFI flash=%" PRIu32
                " sector=%" PRIu32 " buf=%u primary=0x%04X entry=0x%02X",
                *device_size,
                *sector_size,
                (unsigned)*buffer_write_bytes,
                cfi_primary_cmdset_id,
                s_gba_active_intel_e9_entry ? 0xE9u : 0xE8u);
        }

        ESP_LOGI(
            BURNER_TAG,
            "GBA auto probe @%" PRIu32 "Hz try=%" PRIu32
            " id=%02X %02X %02X %02X %02X %02X %02X %02X (%s)",
            probe_hz,
            attempt + 1u,
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3],
            id_out[4],
            id_out[5],
            id_out[6],
            id_out[7],
            id_looks_like_header ? "looks-like-rom-header" :
            (id_matches_plain_rom ? "looks-like-plain-rom-data" : "candidate-id"));
        burner_nor_format_chip_name(
            chip_name,
            sizeof(chip_name),
            burner_gba_chip_name(id_out),
            resolved_cmdset,
            *device_size);
        burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, resolved_cmdset);
        ESP_LOGI(
            BURNER_TAG,
            "GBA CFI ok in word-address mode @%" PRIu32 "Hz try=%" PRIu32
            ": chip=%s profile=%s flash=%" PRIu32 " sector=%" PRIu32 " geom=%s largest=%" PRIu32
            " regions=%u buf=%u cmdset=%s lane=%s primary=0x%04X",
            probe_hz,
            attempt + 1u,
            chip_name,
            profile_name,
            *device_size,
            *sector_size,
            burner_nor_geometry_is_uniform(&s_cart_ctx.geometry) ? "uniform" : "mixed",
            burner_nor_geometry_largest_sector_size(&s_cart_ctx.geometry),
            (unsigned)s_cart_ctx.geometry.region_count,
            (unsigned)*buffer_write_bytes,
            burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
            burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane),
            cfi_primary_cmdset_id);
        return ESP_OK;
    }

    burner_log_gba_id_window();
    burner_log_gba_cfi_window();
    burner_gba_select_probe_id(
        id_out,
        last_amd_id,
        last_amd_id_valid,
        last_intel_id,
        last_intel_id_valid,
        last_intel_id_valid ? BURNER_NOR_CMDSET_INTEL : BURNER_NOR_CMDSET_AMD);
    id_looks_like_header = burner_gba_id_looks_like_rom_header(id_out);
    id_matches_plain_rom = burner_gba_id_matches_plain_rom_data(id_out);
    if (id_looks_like_header || id_matches_plain_rom) {
        *device_size = BURNER_GBA_FALLBACK_DEVICE_SIZE;
        *sector_size = BURNER_GBA_FALLBACK_SECTOR_SIZE;
        *buffer_write_bytes = BURNER_GBA_FALLBACK_BUFFER_WRITE_BYTES;
        *cfi_ok_out = false;
        (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
        s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
        s_cart_ctx.gba_likely_read_only = true;
        ESP_LOGW(
            BURNER_TAG,
            "GBA probe classified as likely read-only retail ROM after retries:"
            " id=%02X %02X %02X %02X %02X %02X %02X %02X source=%s",
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3],
            id_out[4],
            id_out[5],
            id_out[6],
            id_out[7],
            id_looks_like_header ? "rom-header" : "plain-rom-data");
        return ESP_OK;
    }
    *device_size = BURNER_GBA_FALLBACK_DEVICE_SIZE;
    *sector_size = BURNER_GBA_FALLBACK_SECTOR_SIZE;
    *buffer_write_bytes = BURNER_GBA_FALLBACK_BUFFER_WRITE_BYTES;
    *cfi_ok_out = false;
    (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
    s_cart_ctx.gba_cmdset = last_intel_id_valid ? BURNER_NOR_CMDSET_INTEL : BURNER_NOR_CMDSET_AMD;
    s_cart_ctx.gba_likely_read_only = false;
    burner_nor_format_chip_name(
        chip_name,
        sizeof(chip_name),
        burner_gba_chip_name(id_out),
        s_cart_ctx.gba_cmdset,
        *device_size);
    burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id_out, s_cart_ctx.gba_cmdset);
    ESP_LOGW(
        BURNER_TAG,
        "GBA probe fallback after retries: chip=%s profile=%s flash=%" PRIu32
        " sector=%" PRIu32 " buf=%u cmdset=%s source=%s",
        chip_name,
        profile_name,
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
        last_intel_id_valid ? "default-intel-no-cfi" : "default-amd");
    return ESP_OK;
}

esp_err_t burner_bacon_gba_probe_locked(
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out)
{
    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return burner_bacon_gba_probe_after_power_locked(
        id_out,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok_out);
}

void burner_bacon_gba_d0d1_status(bool *known_out, bool *swapped_out)
{
    if (known_out != NULL) {
        *known_out = s_cart_ctx.d0d1_known;
    }
    if (swapped_out != NULL) {
        *swapped_out = s_cart_ctx.d0d1_swapped;
    }
}

static esp_err_t burner_bacon_gba_prepare(const burner_task_param_t *job)
{
    uint8_t id[8];
    char chip_name[48] = {0};
    char profile_name[48] = {0};
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    uint16_t program_buffer_write_bytes = 0;
    uint32_t nor_flags = 0u;
    uint64_t requested_top64 = 0;
    bool cfi_ok = false;
    bool chislink_intel_compat = false;
    bool gbx_profile_matched = false;
    bool cached_probe_used = false;
    esp_err_t err;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    requested_top64 = (uint64_t)job->addr_begin + (uint64_t)job->total_bytes;
    if (requested_top64 == 0u || requested_top64 > ((uint64_t)UINT32_MAX + 1u)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (job->recipe_mode == BURNER_RECIPE_MODE_GBX) {
        if (burner_gba_gbx_take_cached_probe(
                id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok,
                &gbx_profile_matched)) {
            err = ESP_OK;
            cached_probe_used = true;
        } else {
            err = burner_gba_gbx_probe_locked(
                id,
                NULL,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok);
        }
        if (err == ESP_OK && !cached_probe_used) {
            gbx_profile_matched = s_cart_ctx.gbx.active;
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA GBX strict profile match failed; falling back to CHIS/CFI probe");
            burner_gbx_profile_clear(&s_cart_ctx.gbx);
            err = burner_bacon_gba_probe_locked(
                id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok);
        }
    } else {
        err = burner_bacon_gba_probe_locked(
            id,
            &device_size,
            &sector_size,
            &buffer_write_bytes,
            &cfi_ok);
    }
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "GBA probe failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!gbx_profile_matched &&
        s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
        s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_AMD;
    }
    if (job->recipe_mode != BURNER_RECIPE_MODE_GBX || gbx_profile_matched) {
        err = burner_gba_gbx_prepare_profile(
            job,
            id,
            &device_size,
            &sector_size,
            &buffer_write_bytes,
            &program_buffer_write_bytes,
            cfi_ok);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (burner_gba_gbx_is_active()) {
        s_gba_active_intel_generic_cfi = false;
        s_gba_active_intel_e9_entry = false;
    }
    if (device_size == 0u || sector_size == 0u) {
        ESP_LOGE(BURNER_TAG, "GBA probe returned incomplete geometry");
        return ESP_ERR_INVALID_SIZE;
    }
    if (!cfi_ok) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA prepare without CFI: flash=%" PRIu32 " sector=%" PRIu32
            " buf=%u cmdset=%s mode=%d",
            device_size,
            sector_size,
            (unsigned)buffer_write_bytes,
            burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
            (int)job->mode);
        if (!burner_gba_gbx_is_active() &&
            s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL &&
            (job->mode == BURNER_JOB_WRITE_ROM || job->mode == BURNER_JOB_ERASE_ROM)) {
            ESP_LOGE(BURNER_TAG, "GBA prepare blocked: Intel write/erase requires valid CFI");
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (!burner_gba_gbx_is_active() &&
        s_cart_ctx.gba_likely_read_only &&
        (job->mode == BURNER_JOB_WRITE_ROM || job->mode == BURNER_JOB_ERASE_ROM)) {
        ESP_LOGE(BURNER_TAG, "GBA prepare blocked: likely read-only retail ROM");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (job->total_bytes > device_size) {
        ESP_LOGE(
            BURNER_TAG,
            "GBA ROM larger than flash: rom=%" PRIu32 " flash=%" PRIu32,
            job->total_bytes,
            device_size);
        return ESP_ERR_INVALID_SIZE;
    }

    nor_flags = burner_gba_nor_flags_from_id(id);
    if (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL) {
        s_gba_active_intel_e9_entry = burner_gba_intel_id_uses_e9_entry(id);
        if (s_gba_active_intel_e9_entry) {
            nor_flags |= BURNER_NOR_FLAG_INTEL_88B0;
        }
        if (job->recipe_mode == BURNER_RECIPE_MODE_GBX && cfi_ok && burner_gba_chip_name(id) == NULL) {
            s_gba_active_intel_generic_cfi = true;
        }
    }
    if (!burner_gba_gbx_is_active()) {
        chislink_intel_compat =
            (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL) && cfi_ok;
        program_buffer_write_bytes = burner_gba_program_buffer_write_bytes(buffer_write_bytes, s_cart_ctx.gba_cmdset);
        if ((nor_flags & BURNER_NOR_FLAG_DISABLE_BUFFER_PROGRAM) != 0u) {
            const char *special_chip_name = burner_gba_chip_name(id);

            if (chislink_intel_compat) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA Intel ChisLink compatibility: keep buffered program chip=%s cfi_buf=%u",
                    special_chip_name != NULL ? special_chip_name : "unknown",
                    (unsigned)program_buffer_write_bytes);
            } else if (program_buffer_write_bytes != 0u) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA Intel special profile: disable buffered program chip=%s probe_buf=%u actual_buf=0",
                    special_chip_name != NULL ? special_chip_name : "unknown",
                    (unsigned)program_buffer_write_bytes);
            }
            if (!chislink_intel_compat) {
                program_buffer_write_bytes = 0u;
            }
        }
        if ((nor_flags & BURNER_NOR_FLAG_INTEL_88B0) != 0u) {
            if (device_size > BURN_GBA_LINEAR_ADDR_BYTES) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA Intel 88B0 FlashGBX compatibility: keep flash size %" PRIu32
                    ", use %uMB banked writes",
                    device_size,
                    (unsigned)(BURN_GBA_BANK_BYTES / (1024u * 1024u)));
            }
            if (chislink_intel_compat && program_buffer_write_bytes != 0u) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA Intel 88B0 ChisLink compatibility: keep buffered program cfi_buf=%u",
                    (unsigned)program_buffer_write_bytes);
            } else if (program_buffer_write_bytes != 0u) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA Intel 88B0 special profile: disable buffered program probe_buf=%u actual_buf=0",
                    (unsigned)program_buffer_write_bytes);
            }
            if (!chislink_intel_compat) {
                program_buffer_write_bytes = 0u;
            }
        }
        if (s_gba_active_intel_generic_cfi &&
            s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL) {
            ESP_LOGI(
                BURNER_TAG,
                "GBA Intel generic CFI prepare: flash=%" PRIu32 " sector=%" PRIu32
                " buf=%u entry=0x%02X banked=%u",
                device_size,
                sector_size,
                (unsigned)program_buffer_write_bytes,
                s_gba_active_intel_e9_entry ? 0xE9u : 0xE8u,
                device_size > BURN_GBA_LINEAR_ADDR_BYTES ? 1u : 0u);
        }
        if (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_AMD &&
            device_size > BURN_GBA_LINEAR_ADDR_BYTES) {
            ESP_LOGI(
                BURNER_TAG,
                "GBA AMD Bacon compatibility: keep flash size %" PRIu32
                ", use %uMB banked writes",
                device_size,
                (unsigned)(BURN_GBA_BANK_BYTES / (1024u * 1024u)));
        }
    }

    s_cart_ctx.prepared = true;
    s_cart_ctx.probe_cfi_ok = cfi_ok;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_cart_ctx.buffer_write_bytes = buffer_write_bytes;
    s_cart_ctx.program_buffer_write_bytes = program_buffer_write_bytes;
    s_cart_ctx.sector_size = sector_size;
    s_cart_ctx.device_size = device_size;
    s_gba_active_nor_flags = nor_flags;
    burner_nor_format_chip_name(
        chip_name,
        sizeof(chip_name),
        burner_gba_chip_name(id),
        s_cart_ctx.gba_cmdset,
        device_size);
    burner_gba_build_auto_profile_name(profile_name, sizeof(profile_name), id, s_cart_ctx.gba_cmdset);

    if (program_buffer_write_bytes != buffer_write_bytes) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA program buffer cap: cmdset=%s probe_buf=%u actual_buf=%u",
            burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
            (unsigned)buffer_write_bytes,
            (unsigned)program_buffer_write_bytes);
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA prepared: chip=%s profile=%s flash=%" PRIu32 " sector=%" PRIu32
        " geom=%s largest=%" PRIu32 " regions=%u buf=%u prog_buf=%u cfi=%s nor=%s cmd=%s-address %s-lane id=%02X %02X %02X %02X %02X %02X %02X %02X",
        chip_name,
        profile_name,
        device_size,
        sector_size,
        burner_nor_geometry_is_uniform(&s_cart_ctx.geometry) ? "uniform" : "mixed",
        burner_nor_geometry_largest_sector_size(&s_cart_ctx.geometry),
        (unsigned)s_cart_ctx.geometry.region_count,
        (unsigned)buffer_write_bytes,
        (unsigned)program_buffer_write_bytes,
        cfi_ok ? "ok" : "unavailable",
        burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
        burner_gba_cmd_addr_mode_name(s_cart_ctx.gba_cmd_addr_mode),
        burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane),
        id[0],
        id[1],
        id[2],
        id[3],
        id[4],
        id[5],
        id[6],
        id[7]);

    ESP_LOGI(
        BURNER_TAG,
        "GBA mapping: %s bank=%uMB force_multi=%d range=0x%08" PRIX32 "-0x%08" PRIX32,
        burner_is_gba_multi_card(job) ? burner_gba_multicard_mapping_name() : "linear single-card",
        (unsigned)(BURN_GBA_BANK_BYTES / (1024u * 1024u)),
        job->gba_force_multi ? 1 : 0,
        job->addr_begin,
        (uint32_t)(requested_top64 - 1u));

    burner_status_set_probe_info(
        BURNER_CART_MODE_GBA,
        id,
        8u,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok,
        burner_is_gba_multi_card(job),
        job->gba_force_multi,
        s_cart_ctx.d0d1_known,
        s_cart_ctx.d0d1_swapped,
        chip_name,
        "");

    return ESP_OK;
}

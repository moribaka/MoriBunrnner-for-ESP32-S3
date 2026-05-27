/* Low-level GBC/MBC5 bus read/write helpers. */

void burner_reset_cart_probe_state(void)
{
    s_cart_ctx.prepared = false;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_cart_ctx.buffer_write_bytes = 0;
    s_cart_ctx.program_buffer_write_bytes = 0;
    s_cart_ctx.sector_size = 0;
    s_cart_ctx.device_size = 0;
    memset(&s_cart_ctx.geometry, 0, sizeof(s_cart_ctx.geometry));
    memset(s_cart_ctx.mbc5_id, 0, sizeof(s_cart_ctx.mbc5_id));
    s_gb_mapper_kind = BURNER_GB_MAPPER_UNKNOWN;
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_cart_ctx.gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD;
    s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
}

static esp_err_t burner_bacon_gbc_write(uint16_t addr, const uint8_t *buf, size_t len)
{
    size_t i;
    size_t seq_len;
    esp_err_t err;
    uint8_t *sequence;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Host CartAdapter_bacon.cs: spi_cs=2 + optionByte2 stream. */
    seq_len = 4u + 4u * len;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

    sequence = (uint8_t *)malloc(seq_len);
    if (sequence == NULL) {
        return ESP_ERR_NO_MEM;
    }

    sequence[0] = burner_bacon_option_byte2(3, true, true, false, false, true, true);
    sequence[1] = 0x00u; /* a[7:0] */
    sequence[2] = (uint8_t)(addr & 0xFFu);
    sequence[3] = (uint8_t)((addr >> 8) & 0xFFu);

    for (i = 0; i < len; ++i) {
        size_t base = 4u + 4u * i;
        sequence[base + 0u] = burner_bacon_option_byte2(1, true, true, false, false, true, true);
        sequence[base + 1u] = buf[i];
        sequence[base + 2u] = burner_bacon_option_byte2(0, true, true, false, false, true, false);
        sequence[base + 3u] = burner_bacon_option_byte2(0, true, true, true, true, true, true);
    }

    err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, sequence, NULL, seq_len);
    free(sequence);
    return err;
}

static esp_err_t burner_bacon_gbc_read(uint16_t addr, uint8_t *buf, size_t len)
{
    size_t i;
    size_t seq_len;
    esp_err_t err;
    uint8_t *tx_sequence;
    uint8_t *rx_sequence;
    bool free_tx_sequence = false;
    bool free_rx_sequence = false;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Host CartAdapter_bacon.cs: spi_cs=2 + optionByte2 stream. */
    seq_len = 4u + 3u * len;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

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

    tx_sequence[0] = burner_bacon_option_byte2(3, false, true, false, false, true, true);
    tx_sequence[1] = 0x00u; /* a[7:0] */
    tx_sequence[2] = (uint8_t)(addr & 0xFFu);
    tx_sequence[3] = (uint8_t)((addr >> 8) & 0xFFu);

    for (i = 0; i < len; ++i) {
        size_t base = 4u + 3u * i;
        tx_sequence[base + 0u] = burner_bacon_option_byte2(0, false, true, false, false, false, true);
        tx_sequence[base + 1u] = burner_bacon_option_byte2(1, false, true, true, true, true, true);
        tx_sequence[base + 2u] = 0x00u;
    }

    err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, tx_sequence, rx_sequence, seq_len);
    if (err == ESP_OK) {
        for (i = 0; i < len; ++i) {
            buf[i] = rx_sequence[4u + i * 3u + 2u];
        }
    }
    if (free_tx_sequence) {
        free(tx_sequence);
    }
    if (free_rx_sequence) {
        free(rx_sequence);
    }
    return err;
}

static esp_err_t burner_bacon_gbc_read_stream_hoststyle(uint16_t addr, uint8_t *buf, size_t len)
{
    size_t bytes_done = 0u;
    size_t chunk_len_limit;
    size_t chunk_bytes_limit;
    uint8_t setup[4];
    uint8_t *tx_chunk = NULL;
    uint8_t *rx_chunk = NULL;
    bool free_tx_chunk = false;
    bool free_rx_chunk = false;
    bool session_open = false;
    esp_err_t err = ESP_OK;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    chunk_len_limit = BURNER_SPI_STREAM_CHUNK_BYTES;
    if (chunk_len_limit == 0u || chunk_len_limit > BURNER_SPI_MAX_XFER) {
        chunk_len_limit = BURNER_SPI_MAX_XFER;
    }
    chunk_len_limit -= (chunk_len_limit % 3u);
    if (chunk_len_limit == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    chunk_bytes_limit = chunk_len_limit / 3u;
    if (chunk_bytes_limit == 0u) {
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

    /* Match host bacon_gbc_read(): set base address once, then stream byte reads. */
    setup[0] = burner_bacon_option_byte2(3, false, true, false, false, true, true);
    setup[1] = 0x00u;
    setup[2] = (uint8_t)(addr & 0xFFu);
    setup[3] = (uint8_t)((addr >> 8) & 0xFFu);

    err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_2);
    if (err != ESP_OK) {
        goto gbc_read_stream_out;
    }
    session_open = true;

    err = burner_spi_transfer_active(setup, NULL, sizeof(setup));
    if (err != ESP_OK) {
        goto gbc_read_stream_out;
    }

    while (bytes_done < len) {
        size_t chunk_bytes = len - bytes_done;
        size_t chunk_len;

        if (chunk_bytes > chunk_bytes_limit) {
            chunk_bytes = chunk_bytes_limit;
        }
        chunk_len = chunk_bytes * 3u;

        for (size_t i = 0u; i < chunk_bytes; ++i) {
            size_t base = i * 3u;
            tx_chunk[base + 0u] = burner_bacon_option_byte2(0, false, true, false, false, false, true);
            tx_chunk[base + 1u] = burner_bacon_option_byte2(1, false, true, true, true, true, true);
            tx_chunk[base + 2u] = 0x00u;
        }

        err = burner_spi_transfer_active(tx_chunk, rx_chunk, chunk_len);
        if (err != ESP_OK) {
            goto gbc_read_stream_out;
        }

        for (size_t i = 0u; i < chunk_bytes; ++i) {
            buf[bytes_done + i] = rx_chunk[i * 3u + 2u];
        }

        bytes_done += chunk_bytes;
    }

gbc_read_stream_out:
    if (session_open) {
        burner_spi_end_cs(BURNER_SPI_CS_MODE_2);
    }
    if (free_tx_chunk && tx_chunk != NULL) {
        free(tx_chunk);
    }
    if (free_rx_chunk && rx_chunk != NULL) {
        free(rx_chunk);
    }
    return err;
}

static esp_err_t burner_bacon_ram_write(uint16_t addr, const uint8_t *buf, size_t len)
{
    size_t i;
    size_t seq_len;
    esp_err_t err;
    uint8_t *sequence;

    if (buf == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    seq_len = len * 6u;
    if (seq_len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_SIZE;
    }

    sequence = (uint8_t *)malloc(seq_len);
    if (sequence == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < len; ++i) {
        uint16_t ram_addr = (uint16_t)(addr + (uint16_t)i);
        size_t base = i * 6u;

        sequence[base + 0u] = burner_bacon_option_byte0(3, true, true, false, true, true, true);
        sequence[base + 1u] = (uint8_t)(ram_addr & 0xFFu);
        sequence[base + 2u] = (uint8_t)((ram_addr >> 8) & 0xFFu);
        sequence[base + 3u] = buf[i];
        sequence[base + 4u] = burner_bacon_option_byte0(0, true, true, false, true, true, false);
        sequence[base + 5u] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
    }

    err = burner_spi_transfer(sequence, NULL, seq_len);
    free(sequence);
    return err;
}

static esp_err_t burner_bacon_gbc_read_u8(uint16_t addr, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_gbc_read(addr, value, 1);
}

static esp_err_t burner_bacon_gbc_write_for_fram(uint16_t addr, const uint8_t *buf, size_t len, uint8_t latency)
{
    /* Host bacon adapter currently aliases FRAM path to normal GBC write. */
    (void)latency;
    return burner_bacon_gbc_write(addr, buf, len);
}

static esp_err_t burner_bacon_gbc_read_for_fram(uint16_t addr, uint8_t *buf, size_t len, uint8_t latency)
{
    /* Host bacon adapter currently aliases FRAM path to normal GBC read. */
    (void)latency;
    return burner_bacon_gbc_read(addr, buf, len);
}

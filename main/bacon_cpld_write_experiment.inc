void bacon_cpld_write_experiment_profile_locked(bacon_cpld_write_profile_t *out)
{
    if (out) *out = s_write_profile;
}

esp_err_t bacon_cpld_write_experiment_configure_locked(unsigned mode)
{
    if (mode > 2) return ESP_ERR_INVALID_ARG;
    if (mode && ag32_mcu_link_get_preference() != AG32_LINK_PREFERENCE_CPLD)
        return ESP_ERR_INVALID_STATE;
    unsigned previous = s_write_profile.mode;
    s_write_profile.mode = mode;
    esp_err_t err = mode ? burner_spi_init() : ESP_OK;
    if (err == ESP_OK && mode) err = bacon_cpld_probe_locked();
    if (err != ESP_OK) { s_write_profile.mode = previous; return err; }
    memset(&s_write_profile, 0, sizeof(s_write_profile));
    s_write_profile.mode = mode;
    return ESP_OK;
}

static size_t bsc2_prepare(uint8_t *wire, const uint8_t *data, size_t remaining,
    uint32_t address, uint16_t page_bytes)
{
    int64_t start = esp_timer_get_time();
    size_t count = page_bytes - (address & (page_bytes - 1u));
    if (count > remaining) count = remaining;
    size_t padded = (count + 3u) & ~(size_t)3u;
    memcpy(wire, data, count);
    memset(wire + count, 0, padded - count);
    ag32_mcu_write_le32(wire + padded, esp_rom_crc32_le(0, wire, count));
    s_write_profile.prepare_us += esp_timer_get_time() - start;
    return count;
}

static esp_err_t bsc2_program_locked(uint32_t address, const void *data,
    size_t size, uint16_t page_bytes, uint32_t timeout_ms)
{
    // The normal GBA program-block wrapper already owns the IDF bus.
    // Do not acquire it again; retain its existing lock/PM/cleanup contract.
    const size_t stride = BACON_CPLD_BLOCK_BYTES + 4u;
    uint8_t *storage = heap_caps_malloc(3u * stride,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage) return ESP_ERR_NO_MEM;
    uint8_t *rx = storage + 2u * stride;
    uint64_t started = esp_timer_get_time();
    uint32_t polls_before = s_stats.status_polls;
    int64_t timeout = (int64_t)(timeout_ms ? timeout_ms : 2000) * 1000;
    esp_err_t err = enter();
    if (err == ESP_OK && !(s_enter_flags & BACON_CPLD_V2_CAP_OR_CREDIT))
        err = ESP_ERR_NOT_SUPPORTED;
    uint8_t descriptor[BACON_CPLD_DESCRIPTOR_BYTES];
    bacon_cpld_descriptor(descriptor, BACON_CPLD_GBA_PROGRAM, address, size, page_bytes);
    ag32_mcu_write_le32(descriptor, BACON_CPLD_V2_DESCRIPTOR_MAGIC);
    ag32_mcu_write_le32(descriptor + 16, esp_rom_crc32_le(0, descriptor, 16));
    if (err == ESP_OK)
        err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, descriptor, NULL, sizeof(descriptor));
    if (err == ESP_OK)
        err = wait_flag(BACON_CPLD_CONFIGURED, size, esp_timer_get_time() + timeout);
    if (err == ESP_OK) ag32_link_mark_cpld_active();
    size_t offset = 0, count = 0;
    unsigned current = 0;
    bool credit = false;
    if (err == ESP_OK) count = bsc2_prepare(storage, data, size, address, page_bytes);
    while (err == ESP_OK && offset < size) {
        int64_t stage = esp_timer_get_time();
        if (!credit) err = wait_flag(BACON_CPLD_RX_READY, size, stage + timeout);
        else ++s_write_profile.credits;
        s_write_profile.ready_us += esp_timer_get_time() - stage;
        if (err != ESP_OK) break;
        size_t wire_bytes = ((count + 3u) & ~(size_t)3u) + 4u;
        spi_transaction_t transaction = {
            .length = wire_bytes * 8u,
            .tx_buffer = storage + current * stride,
            .rx_buffer = rx,
        };
        stage = esp_timer_get_time();
        err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_0);
        if (err != ESP_OK) break;
        err = spi_device_polling_start(s_mcu_spi, &transaction, portMAX_DELAY);
        if (err != ESP_OK) { burner_spi_end_cs(BURNER_SPI_CS_MODE_0); break; }
        size_t next_count = 0;
        if (offset + count < size)
            next_count = bsc2_prepare(storage + (current ^ 1u) * stride,
                (const uint8_t *)data + offset + count, size - offset - count,
                address + offset + count, page_bytes);
        // Always finish DMA before status/error handling, yielding or freeing.
        err = spi_device_polling_end(s_mcu_spi, portMAX_DELAY);
        burner_spi_end_cs(BURNER_SPI_CS_MODE_0);
        s_write_profile.tx_us += esp_timer_get_time() - stage;
        if (err != ESP_OK) break;
        if (rx[4] != 0xba || rx[5] != 0xce || rx[6] != 1 ||
            (rx[7] & (BACON_CPLD_MODE | BACON_CPLD_CONFIGURED)) !=
                (BACON_CPLD_MODE | BACON_CPLD_CONFIGURED)) {
            err = ESP_ERR_INVALID_RESPONSE; break;
        }
        if (rx[7] & BACON_CPLD_ERROR) { err = ESP_ERR_INVALID_RESPONSE; break; }
        credit = (rx[7] & BACON_CPLD_V2_CAP_OR_CREDIT) != 0;
        offset += count; count = next_count; current ^= 1u;
        ++s_write_profile.packets;
        err = burner_cancel_poll();
        burner_task_yield_if_due();
    }
    int64_t stage = esp_timer_get_time();
    if (err == ESP_OK) err = wait_flag(BACON_CPLD_DONE, size, stage + timeout);
    s_write_profile.done_us += esp_timer_get_time() - stage;
    esp_err_t exit_err = mode_key(BACON_CPLD_EXIT_MAGIC);
    if (err == ESP_OK) err = exit_err;
    ++s_write_profile.streams;
    s_write_profile.elapsed_us += esp_timer_get_time() - started;
    s_write_profile.status_polls += s_stats.status_polls - polls_before;
    if (err == ESP_OK) s_write_profile.bytes += size;
    free(storage);
    return err;
}

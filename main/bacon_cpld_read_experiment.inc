/* Opt-in diagnostic reader: the production transfer loop is unchanged.
 * Reuses BSC1 framing, READY, CS delays, CRC and completion checks verbatim.
 * SPI polling_start/end lets CPU work overlap DMA without ISR/CS changes.
 */
static esp_err_t experiment_consume(const uint8_t *wire, size_t count,
    uint8_t *destination, bacon_cpld_read_profile_t *profile)
{
    int64_t start = esp_timer_get_time();
    size_t padded = (count + 3u) & ~(size_t)3u;
    uint32_t computed = esp_rom_crc32_le(0, wire + 4, count);
    uint32_t received = ag32_mcu_read_le32(wire + 4 + padded);
    profile->crc_us += esp_timer_get_time() - start;
    if (computed != received) return ESP_ERR_INVALID_CRC;
    start = esp_timer_get_time();
    memcpy(destination, wire + 4, count);
    profile->copy_us += esp_timer_get_time() - start;
    return ESP_OK;
}

esp_err_t bacon_cpld_read_experiment_locked(uint32_t address, void *data,
    size_t size, bool pipeline, bacon_cpld_read_profile_t *profile)
{
    if (!profile || !data || !size || ((address | size) & 1u) ||
        address >= 33554432u || size > 33554432u - address)
        return ESP_ERR_INVALID_ARG;
    memset(profile, 0, sizeof(*profile));
    // The stride is word aligned, including padding, dummy and CRC storage.
    const size_t wire_stride = BACON_CPLD_BLOCK_BYTES + 8u;
    uint8_t *buffers = heap_caps_malloc(2u * wire_stride,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffers) return ESP_ERR_NO_MEM;
    int64_t started = esp_timer_get_time();
    uint32_t polls_before = s_stats.status_polls;
    esp_err_t err = burner_spi_init();
    bool acquired = false, entered = false;
    if (err == ESP_OK) {
        err = spi_device_acquire_bus(s_mcu_spi, portMAX_DELAY);
        acquired = err == ESP_OK;
    }
    if (err == ESP_OK) {
        entered = true; // Even a partial mode entry must be exited.
        err = enter();
    }
    uint8_t descriptor[BACON_CPLD_DESCRIPTOR_BYTES];
    bacon_cpld_descriptor(descriptor, BACON_CPLD_GBA_READ, address, size, 0);
    if (err == ESP_OK)
        err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, descriptor, NULL, sizeof(descriptor));
    if (err == ESP_OK)
        err = wait_flag(BACON_CPLD_CONFIGURED, size, esp_timer_get_time() + 2000000);

    size_t transferred = 0, pending_count = 0, pending_offset = 0;
    unsigned current = 0;
    while (err == ESP_OK && transferred < size) {
        size_t count = size - transferred;
        if (count > BACON_CPLD_READ_BYTES) count = BACON_CPLD_READ_BYTES;
        size_t padded = (count + 3u) & ~(size_t)3u;
        int64_t start = esp_timer_get_time();
        err = wait_flag(BACON_CPLD_TX_READY, size, start + 2000000);
        profile->ready_us += esp_timer_get_time() - start;
        if (err != ESP_OK) break;

        spi_transaction_t transaction = {
            .length = (padded + 8u) * 8u,
            .tx_buffer = s_read_clocks,
            .rx_buffer = buffers + current * wire_stride,
        };
        start = esp_timer_get_time();
        err = burner_spi_begin_cs(BURNER_SPI_CS_MODE_1);
        if (err != ESP_OK) break;
        err = spi_device_polling_start(s_mcu_spi, &transaction, portMAX_DELAY);
        profile->start_us += esp_timer_get_time() - start;
        if (err != ESP_OK) {
            burner_spi_end_cs(BURNER_SPI_CS_MODE_1);
            break;
        }
        // Never return, poll status, yield, or free storage with DMA in flight.
        if (pipeline && pending_count)
            err = experiment_consume(buffers + (current ^ 1u) * wire_stride,
                pending_count, (uint8_t *)data + pending_offset, profile);
        start = esp_timer_get_time();
        esp_err_t end_err = spi_device_polling_end(s_mcu_spi, portMAX_DELAY);
        burner_spi_end_cs(BURNER_SPI_CS_MODE_1);
        profile->finish_us += esp_timer_get_time() - start;
        if (err == ESP_OK) err = end_err;
        if (err != ESP_OK) break;
        ++profile->packets;
        if (pipeline) {
            pending_count = count;
            pending_offset = transferred;
            current ^= 1u;
        } else {
            err = experiment_consume(buffers, count, (uint8_t *)data + transferred, profile);
        }
        transferred += count;
        if (err == ESP_OK) err = burner_cancel_poll();
        // Same fairness policy as READY waiting, only after polling_end.
        burner_task_yield_if_due();
    }
    if (err == ESP_OK && pipeline && pending_count)
        err = experiment_consume(buffers + (current ^ 1u) * wire_stride,
            pending_count, (uint8_t *)data + pending_offset, profile);
    if (err == ESP_OK)
        err = wait_flag(BACON_CPLD_DONE, size, esp_timer_get_time() + 2000000);
    if (entered) {
        esp_err_t exit_err = mode_key(BACON_CPLD_EXIT_MAGIC);
        if (err == ESP_OK) err = exit_err;
    }
    if (acquired) spi_device_release_bus(s_mcu_spi);
    free(buffers);
    profile->status_polls = s_stats.status_polls - polls_before;
    profile->elapsed_us = esp_timer_get_time() - started;
    return err;
}

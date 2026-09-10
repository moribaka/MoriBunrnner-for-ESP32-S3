/* Exact file-to-cartridge comparison shared by GB/GBC and GBA.
 * TF uses internal DMA memory; bulk cartridge reads finish in PSRAM before comparison.
 * Verification never repairs the input header or synthesizes missing bytes. */
static esp_err_t burner_verify_stream(const burner_task_param_t *job,
                                      burner_dump_read_block_fn_t read_block)
{
    const size_t cart_capacity = 64u * 1024u;
    /* One small DMA buffer fits alongside the native worker. Cartridge
     * transport already DMA-receives into its own packet scratch buffer. */
    const size_t capacity = 8u * 1024u;
    uint8_t *expected = NULL, *actual = NULL;
    uint32_t processed = 0;
    uint64_t tf_us = 0, cart_us = 0;
    esp_err_t err = ESP_OK;
    const char *failure = "open verify file failed";
    int fd = open(job->rom_path, O_RDONLY);
    if (fd < 0) { err = ESP_FAIL; goto done; }
    expected = heap_caps_malloc(capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    actual = heap_caps_malloc(cart_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!expected || !actual) { err = ESP_ERR_NO_MEM; failure = "no memory for verify buffers"; goto done; }

    while (processed < job->total_bytes) {
        if (burner_cancel_poll() != ESP_OK || usb_msc_tf_in_use_by_host()) {
            err = ESP_ERR_INVALID_STATE; failure = "verify cancelled or TF unavailable"; goto done;
        }
        size_t cart_count = job->total_bytes - processed;
        if (cart_count > cart_capacity) cart_count = cart_capacity;
        uint64_t started = esp_timer_get_time();
        err = read_block(actual, cart_count, job->addr_begin + processed, job);
        cart_us += esp_timer_get_time() - started;
        if (err != ESP_OK) { failure = "read cart failed"; goto done; }
        size_t compared = 0;
        while (compared < cart_count) {
            size_t count = cart_count - compared;
            if (count > capacity) count = capacity;
            started = esp_timer_get_time();
            size_t got = 0;
            while (got < count) {
                ssize_t n = read(fd, expected + got, count - got);
                if (n <= 0) { err = ESP_FAIL; failure = "read verify file failed"; goto done; }
                got += n;
            }
            tf_us += esp_timer_get_time() - started;
            const uint8_t *cart_part = actual + compared;
            if (memcmp(expected, cart_part, count) != 0) {
                size_t mismatch = 0;
                while (expected[mismatch] == cart_part[mismatch]) ++mismatch;
                uint32_t address = job->addr_begin + processed + mismatch;
                burner_status_set_verify_sample(address, expected[mismatch], cart_part[mismatch], false);
                char message[96];
                snprintf(message, sizeof(message), "verify mismatch @0x%08" PRIX32 " %02X->%02X",
                         address, expected[mismatch], cart_part[mismatch]);
                if (job->cart_mode == BURNER_CART_MODE_MBC5) {
                    char log_path[TF_PATH_LEN_MAX];
                    FILE *log = burner_open_mbc5_verify_log(job, log_path, sizeof(log_path));
                    if (log) {
                        fprintf(log, "0x%08" PRIX32 " %02X->%02X\n",
                                address, expected[mismatch], cart_part[mismatch]);
                        fclose(log);
                    }
                }
                processed += mismatch;
                burner_status_update(BURNER_STATE_ERROR,
                    burner_calc_progress_percent_u64(processed, job->total_bytes), processed,
                    job->total_bytes, message, job->rom_name, job->rom_path);
                err = ESP_FAIL;
                failure = NULL; // Preserve the exact mismatch report.
                goto done;
            }
            burner_status_set_verify_sample(job->addr_begin + processed + count - 1,
                                           expected[count - 1], cart_part[count - 1], true);
            processed += count;
            compared += count;
        }
        int progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        burner_status_update(BURNER_STATE_BURNING, progress, processed, job->total_bytes,
                             "file->cart verify running", job->rom_name, job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }
done:
    if (fd >= 0) close(fd);
    free(expected);
    free(actual);
    if (err != ESP_OK && failure) {
        burner_status_update(BURNER_STATE_ERROR,
            burner_calc_progress_percent_u64(processed, job->total_bytes), processed,
            job->total_bytes, failure, job->rom_name, job->rom_path);
    }
    ESP_LOGI(BURNER_TAG, "ROM verify: err=%s bytes=%" PRIu32 "/%" PRIu32
             " tf=%" PRIu64 "ms cart=%" PRIu64 "ms", esp_err_to_name(err),
             processed, job->total_bytes, tf_us / 1000, cart_us / 1000);
    return err;
}

/* Two PSRAM slots; the writer owns a submitted slot until wait completes.
 * The other slot can receive SPI data concurrently. No fragment files. */
static esp_err_t burner_run_read_job_direct(
    const burner_task_param_t *job, uint32_t work_total, uint32_t chunk_bytes,
    burner_dump_read_block_fn_t read_block, const char *progress_msg,
    const char *alloc_fail_msg, const char *read_fail_msg, const char *write_fail_msg)
{
    uint8_t *slots[2] = {NULL, NULL};
    burner_tf_writer_ctx_t writer = {0};
    uint32_t read_offset = 0, processed = 0, pending = 0;
    unsigned slot = 0;
    int fd = -1;
    esp_err_t err = ESP_OK;
    const char *failure = write_fail_msg;
    char temporary[TF_PATH_LEN_MAX + 96];
    if (!job || !work_total || !chunk_bytes || !read_block) return ESP_ERR_INVALID_ARG;
    if (snprintf(temporary, sizeof(temporary), "%s.dump_tmp", job->rom_path) >= (int)sizeof(temporary))
        return ESP_ERR_INVALID_SIZE;

    size_t capacity = work_total < chunk_bytes ? work_total : chunk_bytes;
    slots[0] = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    slots[1] = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!slots[0] || !slots[1]) {
        err = ESP_ERR_NO_MEM; failure = alloc_fail_msg; goto done;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { err = ESP_FAIL; goto done; }
    err = burner_tf_writer_start(&writer, fd);
    if (err != ESP_OK) { failure = alloc_fail_msg; goto done; }

    while (read_offset < work_total) {
        size_t count = work_total - read_offset;
        if (count > capacity) count = capacity;
        if (burner_cancel_poll() != ESP_OK || usb_msc_tf_in_use_by_host()) {
            err = ESP_ERR_INVALID_STATE; failure = "dump cancelled or TF unavailable"; goto done;
        }
        uint64_t started = esp_timer_get_time();
        err = read_block(slots[slot], count, job->addr_begin + read_offset, job);
        uint64_t elapsed = esp_timer_get_time() - started;
        if (err != ESP_OK) { failure = read_fail_msg; goto done; }
        burner_status_record_dump_read((uint32_t)count, elapsed);
        read_offset += count;

        if (pending) {
            err = burner_tf_writer_wait(&writer);
            if (err != ESP_OK) goto done;
            processed += pending;
            pending = 0;
        }
        err = burner_tf_writer_submit(&writer, slots[slot], count);
        if (err != ESP_OK) goto done;
        pending = count;
        slot ^= 1u;
        int progress = burner_calc_progress_percent_u64(processed, work_total);
        burner_status_update(BURNER_STATE_BURNING, progress, processed, work_total,
                             progress_msg, job->rom_name, job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }
    err = burner_tf_writer_wait(&writer);
    if (err != ESP_OK) goto done;
    processed += pending;
    pending = 0;
    burner_tf_writer_stop(&writer);

    uint64_t finalize_start = esp_timer_get_time();
    if (close(fd) != 0) err = ESP_FAIL;
    fd = -1;
    if (err == ESP_OK) err = burner_replace_file(temporary, job->rom_path);
    burner_status_record_dump_finalize(esp_timer_get_time() - finalize_start);
    if (err == ESP_OK) {
        burner_status_update(BURNER_STATE_BURNING, 100, processed, work_total,
                             progress_msg, job->rom_name, job->rom_path);
    }
done:
    /* stop joins any in-flight write before the buffers or fd are released. */
    burner_tf_writer_stop(&writer);
    if (fd >= 0) close(fd);
    if (err != ESP_OK) {
        unlink(temporary);
        burner_status_update(BURNER_STATE_ERROR,
            burner_calc_progress_percent_u64(processed, work_total), processed, work_total,
            failure, job->rom_name, job->rom_path);
    }
    free(slots[0]);
    free(slots[1]);
    return err;
}

/* Burn pipeline helpers shared by MBC5/GBC and GBA job paths. */

static void burner_emit_progress_cb(int progress, uint32_t processed)
{
    if (s_receive_cb != NULL) {
        uint8_t cb_payload[4];
        cb_payload[0] = (uint8_t)progress;
        cb_payload[1] = (uint8_t)((processed >> 8) & 0xFF);
        cb_payload[2] = (uint8_t)(processed & 0xFF);
        cb_payload[3] = 0;
        s_receive_cb(cb_payload, sizeof(cb_payload));
    }
}

esp_err_t burner_bacon_mbc5_read_block(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = (uint16_t)(rom_addr >> 14);
        uint16_t bank_off = (uint16_t)(rom_addr & 0x3FFFu);
        uint16_t cart_addr;
        size_t remain = len - copied;
        size_t bank_remain = 0x4000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        if (bank == 0u) {
            cart_addr = bank_off;
        } else {
            cart_addr = (uint16_t)(0x4000u + bank_off);
        }

        err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }
        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_read_block_program_window(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0u;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = 0u;
        uint16_t cart_addr = 0u;
        uint32_t bank_off = 0u;
        size_t remain = len - copied;
        size_t bank_remain;
        size_t chunk;

        burner_mbc5_addr_to_program_window(rom_addr, &bank, &cart_addr, &bank_off);
        bank_remain = BURN_MBC5_ROM_BANK_BYTES - bank_off;
        chunk = (remain < bank_remain) ? remain : bank_remain;
        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }
        copied += chunk;
    }

    return ESP_OK;
}

esp_err_t burner_bacon_mbc5_read_block_hoststyle(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0u;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = (uint16_t)(rom_addr >> 14);
        uint16_t bank_off = (uint16_t)(rom_addr & 0x3FFFu);
        uint16_t cart_addr;
        size_t remain = len - copied;
        size_t bank_remain = 0x4000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        if (bank == 0u) {
            cart_addr = bank_off;
        } else {
            cart_addr = (uint16_t)(0x4000u + bank_off);
        }

        err = burner_bacon_gbc_read_stream_hoststyle(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }

        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_ram_write_block(
    const uint8_t *data,
    size_t len,
    uint32_t offset,
    bool fram_mode,
    uint8_t fram_latency)
{
    size_t written = 0;
    uint8_t current_bank = 0xFFu;
    esp_err_t err;

    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (written < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t ram_addr = offset + (uint32_t)written;
        uint8_t bank = (uint8_t)(ram_addr >> 13);
        uint16_t bank_off = (uint16_t)(ram_addr & 0x1FFFu);
        uint16_t cart_addr = (uint16_t)(0xA000u + bank_off);
        size_t remain = len - written;
        size_t bank_remain = 0x2000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_WRITE_MAX_BYTES) {
            chunk = BURN_CART_WRITE_MAX_BYTES;
        }

        if (bank != current_bank) {
            err = burner_bacon_mbc5_ram_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        if (fram_mode) {
            err = burner_bacon_gbc_write_for_fram(cart_addr, data + written, chunk, fram_latency);
        } else {
            err = burner_bacon_gbc_write(cart_addr, data + written, chunk);
        }
        if (err != ESP_OK) {
            return err;
        }

        written += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_ram_read_block(
    uint8_t *out,
    size_t len,
    uint32_t offset,
    bool fram_mode,
    uint8_t fram_latency)
{
    size_t copied = 0;
    uint8_t current_bank = 0xFFu;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t ram_addr = offset + (uint32_t)copied;
        uint8_t bank = (uint8_t)(ram_addr >> 13);
        uint16_t bank_off = (uint16_t)(ram_addr & 0x1FFFu);
        uint16_t cart_addr = (uint16_t)(0xA000u + bank_off);
        size_t remain = len - copied;
        size_t bank_remain = 0x2000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        if (bank != current_bank) {
            err = burner_bacon_mbc5_ram_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        if (fram_mode) {
            err = burner_bacon_gbc_read_for_fram(cart_addr, out + copied, chunk, fram_latency);
        } else {
            err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        }
        if (err != ESP_OK) {
            return err;
        }

        copied += chunk;
    }

    return ESP_OK;
}

esp_err_t burner_ensure_dump_dir(void)
{
    struct stat st;

    if (stat(DUMP_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(DUMP_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t burner_ensure_rom_output_dir(void)
{
    struct stat st;

    if (stat(ROM_OUTPUT_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(ROM_OUTPUT_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static bool burner_dump_stage_build_dir_rel(
    const char *rom_name,
    char *stage_rel,
    size_t stage_rel_len)
{
    int n;

    if (rom_name == NULL || rom_name[0] == '\0' || stage_rel == NULL || stage_rel_len < 2u) {
        return false;
    }

    n = snprintf(stage_rel, stage_rel_len, ROM_OUTPUT_TEMP_ROOT_REL "/%s.parts", rom_name);
    return n > 0 && n < (int)stage_rel_len;
}

static esp_err_t burner_dump_stage_prepare(
    const char *rom_name,
    char *stage_rel,
    size_t stage_rel_len,
    char *stage_full,
    size_t stage_full_len)
{
    struct stat st;

    if (!burner_dump_stage_build_dir_rel(rom_name, stage_rel, stage_rel_len) ||
        !burner_build_full_path(stage_rel, stage_full, stage_full_len)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (stat(stage_full, &st) == 0) {
        if (burner_remove_recursive(stage_full) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    return burner_mkdirs_rel(stage_rel);
}

static bool burner_dump_stage_build_fragment_rel(
    const char *stage_rel,
    const char *rom_name,
    uint32_t fragment_index,
    char *fragment_rel,
    size_t fragment_rel_len)
{
    int n;

    if (stage_rel == NULL || stage_rel[0] == '\0' || rom_name == NULL || rom_name[0] == '\0' ||
        fragment_rel == NULL || fragment_rel_len < 2u) {
        return false;
    }

    n = snprintf(
        fragment_rel,
        fragment_rel_len,
        "%s/%s.part%03" PRIu32,
        stage_rel,
        rom_name,
        fragment_index);
    return n > 0 && n < (int)fragment_rel_len;
}

static esp_err_t burner_dump_stage_write_fragment(
    const char *stage_rel,
    const char *rom_name,
    uint32_t fragment_index,
    const uint8_t *data,
    size_t data_len)
{
    char fragment_rel[TF_PATH_LEN_MAX] = {0};
    char fragment_full[TF_PATH_LEN_MAX + 64] = {0};
    FILE *fp = NULL;

    if (!burner_dump_stage_build_fragment_rel(
            stage_rel,
            rom_name,
            fragment_index,
            fragment_rel,
            sizeof(fragment_rel)) ||
        !burner_build_full_path(fragment_rel, fragment_full, sizeof(fragment_full))) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (burner_cancel_poll() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    fp = fopen(fragment_full, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }
    if (fwrite(data, 1, data_len, fp) != data_len) {
        fclose(fp);
        unlink(fragment_full);
        return ESP_FAIL;
    }
    fclose(fp);
    return ESP_OK;
}

static esp_err_t burner_replace_file(const char *tmp_path, const char *target_path)
{
    if (tmp_path == NULL || target_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (rename(tmp_path, target_path) == 0) {
        (void)burner_apply_current_file_mtime(target_path, NULL);
        return ESP_OK;
    }
    if (errno == EEXIST) {
        if (unlink(target_path) == 0 && rename(tmp_path, target_path) == 0) {
            (void)burner_apply_current_file_mtime(target_path, NULL);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static uint8_t *burner_attach_stdio_buffer(FILE *fp, size_t preferred_size)
{
    uint8_t *buf;

    if (fp == NULL || preferred_size == 0u) {
        return NULL;
    }

    buf = (uint8_t *)heap_caps_malloc(preferred_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)malloc(preferred_size);
    }
    if (buf == NULL) {
        buf = (uint8_t *)heap_caps_malloc(preferred_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        return NULL;
    }

    if (setvbuf(fp, (char *)buf, _IOFBF, preferred_size) != 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

static esp_err_t burner_dump_stage_merge_fragments(
    const char *stage_rel,
    const char *rom_name,
    uint32_t fragment_count,
    const char *target_path)
{
    char fragment_rel[TF_PATH_LEN_MAX] = {0};
    char fragment_full[TF_PATH_LEN_MAX + 64] = {0};
    char tmp_target[TF_PATH_LEN_MAX + 96] = {0};
    uint8_t *copy_buf = NULL;
    FILE *out_fp = NULL;
    esp_err_t err = ESP_OK;
    uint32_t index;

    if (stage_rel == NULL || rom_name == NULL || fragment_count == 0u || target_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(tmp_target, sizeof(tmp_target), "%s.merge_tmp", target_path) >= (int)sizeof(tmp_target)) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_fp = fopen(tmp_target, "wb");
    if (out_fp == NULL) {
        return ESP_FAIL;
    }

    copy_buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (copy_buf == NULL) {
        fclose(out_fp);
        unlink(tmp_target);
        return ESP_ERR_NO_MEM;
    }

    for (index = 0u; index < fragment_count && err == ESP_OK; ++index) {
        FILE *in_fp;

        if (burner_cancel_poll() != ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (usb_msc_tf_in_use_by_host()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!burner_dump_stage_build_fragment_rel(
                stage_rel,
                rom_name,
                index,
                fragment_rel,
                sizeof(fragment_rel)) ||
            !burner_build_full_path(fragment_rel, fragment_full, sizeof(fragment_full))) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        in_fp = fopen(fragment_full, "rb");
        if (in_fp == NULL) {
            err = ESP_FAIL;
            break;
        }

        while (err == ESP_OK) {
            size_t read_len = fread(copy_buf, 1, TF_IO_CHUNK_SIZE, in_fp);

            if (burner_cancel_poll() != ESP_OK) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            if (usb_msc_tf_in_use_by_host()) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }

            if (read_len == 0u) {
                break;
            }
            if (fwrite(copy_buf, 1, read_len, out_fp) != read_len) {
                err = ESP_FAIL;
                break;
            }
        }
        if (err == ESP_OK && ferror(in_fp)) {
            err = ESP_FAIL;
        }
        fclose(in_fp);
    }

    free(copy_buf);

    if (fclose(out_fp) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        unlink(tmp_target);
        return err;
    }

    return burner_replace_file(tmp_target, target_path);
}

typedef esp_err_t (*burner_dump_read_block_fn_t)(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job);

static esp_err_t burner_dump_read_block_mbc5(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job)
{
    esp_err_t err;

    (void)job;

    burner_spi_lock_take();
    /* Use a dedicated Bacon-style streaming ROM read for MBC5 dump/export. */
    err = burner_bacon_mbc5_read_block_hoststyle(dst, len, addr);
    burner_spi_lock_give();

    return err;
}

static esp_err_t burner_dump_read_block_gba(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job)
{
    esp_err_t err;

    burner_spi_lock_take();
    /* Use Bacon hoststyle ROM reads for GBA dump/export. */
    err = burner_bacon_gba_verify_read_block_hoststyle(dst, len, addr, burner_is_gba_multi_card(job));
    burner_spi_lock_give();

    return err;
}

static int burner_dump_stage_progress(uint32_t processed, uint32_t total)
{
    int progress = burner_calc_progress_percent_u64(processed, total);

    if (processed >= total) {
        return 99;
    }
    if (progress > 99) {
        return 99;
    }
    return progress;
}

static esp_err_t __attribute__((unused)) burner_run_read_job_staged(
    const burner_task_param_t *job,
    uint32_t work_total,
    uint32_t chunk_bytes,
    burner_dump_read_block_fn_t read_block,
    const char *cache_msg,
    const char *flush_msg,
    const char *merge_msg,
    const char *alloc_fail_msg,
    const char *read_fail_msg)
{
    uint8_t *psram_stage_buf = NULL;
    uint32_t processed = 0;
    size_t stage_capacity = 0u;
    size_t staged_bytes = 0u;
    esp_err_t err = ESP_OK;
    char stage_rel[TF_PATH_LEN_MAX] = {0};
    char stage_full[TF_PATH_LEN_MAX + 64] = {0};
    uint32_t fragment_count = 0u;
    bool stage_ready = false;

    if (job == NULL || work_total == 0u || chunk_bytes == 0u || read_block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    stage_capacity = (work_total < burner_psram_window_mb_to_bytes(BURN_READ_PSRAM_FRAGMENT_MB))
                         ? (size_t)work_total
                         : (size_t)burner_psram_window_mb_to_bytes(BURN_READ_PSRAM_FRAGMENT_MB);
    if (stage_capacity == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        work_total,
        cache_msg,
        job->rom_name,
        job->rom_path);

    err = burner_dump_stage_prepare(job->rom_name, stage_rel, sizeof(stage_rel), stage_full, sizeof(stage_full));
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            "prepare temp dump dir failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    stage_ready = true;

    psram_stage_buf = (uint8_t *)heap_caps_malloc(stage_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_stage_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            alloc_fail_msg,
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto staged_dump_done;
    }

    while (processed < work_total) {
        size_t read_len = (size_t)(work_total - processed);
        int progress;

        if (read_len > chunk_bytes) {
            read_len = chunk_bytes;
        }

        if ((staged_bytes + read_len) > stage_capacity && staged_bytes > 0u) {
            burner_status_update(
                BURNER_STATE_BURNING,
                burner_dump_stage_progress(processed, work_total),
                processed,
                work_total,
                flush_msg,
                job->rom_name,
                job->rom_path);
            err = burner_dump_stage_write_fragment(
                stage_rel,
                job->rom_name,
                fragment_count,
                psram_stage_buf,
                staged_bytes);
            if (err != ESP_OK) {
                if (!burner_cancel_is_requested()) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_dump_stage_progress(processed, work_total),
                        processed,
                        work_total,
                        "write temp fragment failed",
                        job->rom_name,
                        job->rom_path);
                }
                break;
            }
            fragment_count++;
            staged_bytes = 0u;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_dump_stage_progress(processed, work_total),
                processed,
                work_total,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        err = read_block(psram_stage_buf + staged_bytes, read_len, job->addr_begin + processed, job);
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_dump_stage_progress(processed, work_total),
                    processed,
                    work_total,
                    read_fail_msg,
                    job->rom_name,
                    job->rom_path);
            }
            break;
        }

        staged_bytes += read_len;
        processed += (uint32_t)read_len;
        progress = burner_dump_stage_progress(processed, work_total);
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            work_total,
            cache_msg,
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

    if (err == ESP_OK && staged_bytes > 0u) {
        burner_status_update(
            BURNER_STATE_BURNING,
            burner_dump_stage_progress(processed, work_total),
            processed,
            work_total,
            flush_msg,
            job->rom_name,
            job->rom_path);
        err = burner_dump_stage_write_fragment(
            stage_rel,
            job->rom_name,
            fragment_count,
            psram_stage_buf,
            staged_bytes);
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_dump_stage_progress(processed, work_total),
                    processed,
                    work_total,
                    "write temp fragment failed",
                    job->rom_name,
                    job->rom_path);
            }
        } else {
            fragment_count++;
            staged_bytes = 0u;
        }
    }

    if (err == ESP_OK) {
        burner_status_update(
            BURNER_STATE_BURNING,
            99,
            processed,
            work_total,
            merge_msg,
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(99, processed);
        err = burner_dump_stage_merge_fragments(stage_rel, job->rom_name, fragment_count, job->rom_path);
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    99,
                    processed,
                    work_total,
                    "merge dump file failed",
                    job->rom_name,
                    job->rom_path);
            }
        }
    }

staged_dump_done:
    if (psram_stage_buf != NULL) {
        free(psram_stage_buf);
    }
    if (stage_ready) {
        (void)burner_remove_recursive(stage_full);
    }
    return err;
}

static esp_err_t burner_run_read_job_direct(
    const burner_task_param_t *job,
    uint32_t work_total,
    uint32_t chunk_bytes,
    burner_dump_read_block_fn_t read_block,
    const char *progress_msg,
    const char *alloc_fail_msg,
    const char *read_fail_msg,
    const char *write_fail_msg)
{
    uint8_t *dump_buf[2] = {NULL, NULL};
    burner_tf_writer_ctx_t tf_writer = {0};
    uint32_t read_offset = 0u;
    uint32_t processed = 0u;
    uint32_t pending_write_bytes = 0u;
    size_t buf_size = 0u;
    size_t buf_count = 0u;
    size_t slot_fill[2] = {0u, 0u};
    size_t fill_slot = 0u;
    esp_err_t err = ESP_OK;
    int out_fd = -1;
    char tmp_target[TF_PATH_LEN_MAX + 96] = {0};
    uint64_t op_start_us;
    uint64_t op_elapsed_us;
    bool writer_inflight = false;

    if (job == NULL || work_total == 0u || chunk_bytes == 0u || read_block == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(tmp_target, sizeof(tmp_target), "%s.dump_tmp", job->rom_path) >= (int)sizeof(tmp_target)) {
        return ESP_ERR_INVALID_SIZE;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        work_total,
        progress_msg,
        job->rom_name,
        job->rom_path);

    out_fd = open(tmp_target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            write_fail_msg,
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
    }

    buf_size = (work_total < chunk_bytes) ? (size_t)work_total : (size_t)chunk_bytes;
    dump_buf[0] = (uint8_t *)malloc(buf_size);
    if (dump_buf[0] != NULL) {
        dump_buf[1] = (uint8_t *)malloc(buf_size);
        buf_count = (dump_buf[1] != NULL) ? 2u : 1u;
    }
    if (dump_buf[0] == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            alloc_fail_msg,
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto direct_dump_done;
    }
    if (buf_count == 0u) {
        buf_count = 1u;
    }

    err = burner_tf_writer_start(&tf_writer, out_fd);
    if (err != ESP_OK) {
        memset(&tf_writer, 0, sizeof(tf_writer));
        tf_writer.fd = out_fd;
        err = ESP_OK;
    }

    while (read_offset < work_total) {
        size_t read_len = (size_t)(work_total - read_offset);
        size_t free_space = buf_size - slot_fill[fill_slot];
        int progress;
        uint8_t *read_buf = dump_buf[fill_slot] + slot_fill[fill_slot];

        if (writer_inflight && buf_count < 2u) {
            err = burner_tf_writer_wait(&tf_writer);
            writer_inflight = false;
            if (err != ESP_OK) {
                if (!burner_cancel_is_requested()) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, work_total),
                        processed,
                        work_total,
                        write_fail_msg,
                        job->rom_name,
                        job->rom_path);
                }
                goto direct_dump_done;
            }
            processed += pending_write_bytes;
            pending_write_bytes = 0u;
        }

        if (free_space == 0u) {
            if (writer_inflight) {
                err = burner_tf_writer_wait(&tf_writer);
                writer_inflight = false;
                if (err != ESP_OK) {
                    if (!burner_cancel_is_requested()) {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            burner_calc_progress_percent_u64(processed, work_total),
                            processed,
                            work_total,
                            write_fail_msg,
                            job->rom_name,
                            job->rom_path);
                    }
                    goto direct_dump_done;
                }
                processed += pending_write_bytes;
                pending_write_bytes = 0u;
            }

            err = burner_tf_writer_submit(&tf_writer, dump_buf[fill_slot], slot_fill[fill_slot]);
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_calc_progress_percent_u64(processed, work_total),
                    processed,
                    work_total,
                    write_fail_msg,
                    job->rom_name,
                    job->rom_path);
                goto direct_dump_done;
            }

            if (tf_writer.running && tf_writer.request != NULL && tf_writer.done != NULL) {
                writer_inflight = true;
                pending_write_bytes = (uint32_t)slot_fill[fill_slot];
            } else {
                processed += (uint32_t)slot_fill[fill_slot];
            }

            slot_fill[fill_slot] = 0u;
            if (buf_count > 1u) {
                fill_slot = (fill_slot + 1u) % buf_count;
            }
            continue;
        }

        if (read_len > (size_t)chunk_bytes) {
            read_len = (size_t)chunk_bytes;
        }
        if (read_len > free_space) {
            read_len = free_space;
        }

        if (burner_cancel_poll() != ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
            goto direct_dump_done;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_calc_progress_percent_u64(processed, work_total),
                processed,
                work_total,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            goto direct_dump_done;
        }

        op_start_us = (uint64_t)esp_timer_get_time();
        err = read_block(read_buf, read_len, job->addr_begin + read_offset, job);
        op_elapsed_us = (uint64_t)esp_timer_get_time();
        if (op_elapsed_us > op_start_us) {
            burner_status_record_dump_read((uint32_t)read_len, op_elapsed_us - op_start_us);
        }
        if (err != ESP_OK) {
            if (!burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_calc_progress_percent_u64(processed, work_total),
                    processed,
                    work_total,
                    read_fail_msg,
                    job->rom_name,
                    job->rom_path);
            }
            goto direct_dump_done;
        }

        read_offset += (uint32_t)read_len;
        slot_fill[fill_slot] += read_len;

        if (slot_fill[fill_slot] == buf_size || read_offset >= work_total) {
            if (writer_inflight) {
                err = burner_tf_writer_wait(&tf_writer);
                writer_inflight = false;
                if (err != ESP_OK) {
                    if (!burner_cancel_is_requested()) {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            burner_calc_progress_percent_u64(processed, work_total),
                            processed,
                            work_total,
                            write_fail_msg,
                            job->rom_name,
                            job->rom_path);
                    }
                    goto direct_dump_done;
                }
                processed += pending_write_bytes;
                pending_write_bytes = 0u;
            }

            err = burner_tf_writer_submit(&tf_writer, dump_buf[fill_slot], slot_fill[fill_slot]);
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    burner_calc_progress_percent_u64(processed, work_total),
                    processed,
                    work_total,
                    write_fail_msg,
                    job->rom_name,
                    job->rom_path);
                goto direct_dump_done;
            }

            if (tf_writer.running && tf_writer.request != NULL && tf_writer.done != NULL) {
                writer_inflight = true;
                pending_write_bytes = (uint32_t)slot_fill[fill_slot];
            } else {
                processed += (uint32_t)slot_fill[fill_slot];
            }

            slot_fill[fill_slot] = 0u;
            if (buf_count > 1u) {
                fill_slot = (fill_slot + 1u) % buf_count;
            }
        }

        progress = burner_calc_progress_percent_u64(processed, work_total);
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            work_total,
            progress_msg,
            job->rom_name,
            job->rom_path);
    }

    if (slot_fill[fill_slot] > 0u) {
        if (writer_inflight) {
            err = burner_tf_writer_wait(&tf_writer);
            writer_inflight = false;
            if (err != ESP_OK) {
                if (!burner_cancel_is_requested()) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, work_total),
                        processed,
                        work_total,
                        write_fail_msg,
                        job->rom_name,
                        job->rom_path);
                }
                goto direct_dump_done;
            }
            processed += pending_write_bytes;
            pending_write_bytes = 0u;
        }

        err = burner_tf_writer_submit(&tf_writer, dump_buf[fill_slot], slot_fill[fill_slot]);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_calc_progress_percent_u64(processed, work_total),
                processed,
                work_total,
                write_fail_msg,
                job->rom_name,
                job->rom_path);
            goto direct_dump_done;
        }

        if (tf_writer.running && tf_writer.request != NULL && tf_writer.done != NULL) {
            writer_inflight = true;
            pending_write_bytes = (uint32_t)slot_fill[fill_slot];
        } else {
            processed += (uint32_t)slot_fill[fill_slot];
        }
    }

    if (writer_inflight) {
        err = burner_tf_writer_wait(&tf_writer);
        writer_inflight = false;
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                burner_calc_progress_percent_u64(processed, work_total),
                processed,
                work_total,
                write_fail_msg,
                job->rom_name,
                job->rom_path);
            goto direct_dump_done;
        }
        processed += pending_write_bytes;
        pending_write_bytes = 0u;
        burner_status_update(
            BURNER_STATE_BURNING,
            burner_calc_progress_percent_u64(processed, work_total),
            processed,
            work_total,
            progress_msg,
            job->rom_name,
            job->rom_path);
    }

    op_start_us = (uint64_t)esp_timer_get_time();
    if (close(out_fd) != 0) {
        out_fd = -1;
        op_elapsed_us = (uint64_t)esp_timer_get_time();
        if (op_elapsed_us > op_start_us) {
            burner_status_record_dump_finalize(op_elapsed_us - op_start_us);
        }
        burner_status_update(
            BURNER_STATE_ERROR,
            burner_calc_progress_percent_u64(processed, work_total),
            processed,
            work_total,
            write_fail_msg,
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto direct_dump_done;
    }
    out_fd = -1;

    err = burner_replace_file(tmp_target, job->rom_path);
    op_elapsed_us = (uint64_t)esp_timer_get_time();
    if (op_elapsed_us > op_start_us) {
        burner_status_record_dump_finalize(op_elapsed_us - op_start_us);
    }
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            burner_calc_progress_percent_u64(processed, work_total),
            processed,
            work_total,
            write_fail_msg,
            job->rom_name,
            job->rom_path);
        goto direct_dump_done;
    }

direct_dump_done:
    if (writer_inflight) {
        (void)burner_tf_writer_wait(&tf_writer);
        writer_inflight = false;
    }
    burner_tf_writer_stop(&tf_writer);
    if (out_fd >= 0) {
        close(out_fd);
    }
    if (err != ESP_OK) {
        unlink(tmp_target);
    }
    if (dump_buf[0] != NULL) {
        free(dump_buf[0]);
    }
    if (dump_buf[1] != NULL) {
        free(dump_buf[1]);
    }

    return err;
}

typedef struct {
    FILE *fp;
    uint8_t *dst;
    size_t bytes;
    size_t read_len;
    esp_err_t err;
    SemaphoreHandle_t done;
} burner_tf_prefetch_ctx_t;

typedef enum {
    BURNER_ERASE_OP_MBC5_RANGE = 0,
    BURNER_ERASE_OP_GBA_RANGE,
    BURNER_ERASE_OP_MBC5_CHIP,
    BURNER_ERASE_OP_GBA_CHIP,
} burner_erase_op_t;

typedef struct {
    burner_erase_op_t op;
    uint32_t addr_begin;
    uint32_t addr_end;
    uint32_t sector_size;
    bool gba_multi;
    bool sample_blank_sectors;
    bool erase_always;
    esp_err_t err;
    SemaphoreHandle_t done;
} burner_erase_task_ctx_t;

typedef struct {
    FILE *fp;
    uint8_t *dst;
    size_t bytes;
    size_t read_len;
    esp_err_t err;
    bool stop;
    bool running;
    TaskHandle_t task;
    SemaphoreHandle_t request;
    SemaphoreHandle_t done;
} burner_tf_reader_ctx_t;

static esp_err_t burner_tf_read_exact(FILE *fp, uint8_t *dst, size_t bytes)
{
    size_t read_len;

    if (fp == NULL || dst == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    read_len = fread(dst, 1, bytes, fp);
    if (read_len != bytes) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t burner_tf_write_exact(int fd, const uint8_t *src, size_t bytes)
{
    size_t offset = 0u;
    uint64_t write_start_us;
    uint64_t write_elapsed_us;

    if (fd < 0 || src == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    write_start_us = (uint64_t)esp_timer_get_time();
    while (offset < bytes) {
        ssize_t written = write(fd, src + offset, bytes - offset);
        if (written <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)written;
        if (offset < bytes) {
            if (burner_cancel_is_requested()) {
                return ESP_ERR_INVALID_STATE;
            }
            if (usb_msc_tf_in_use_by_host()) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    write_elapsed_us = (uint64_t)esp_timer_get_time();
    if (offset == bytes && write_elapsed_us > write_start_us) {
        burner_status_record_dump_write((uint32_t)bytes, write_elapsed_us - write_start_us);
    }

    return ESP_OK;
}

static void burner_tf_prefetch_task(void *arg)
{
    burner_tf_prefetch_ctx_t *ctx = (burner_tf_prefetch_ctx_t *)arg;
    uint64_t read_start_us = 0u;
    uint64_t read_elapsed_us = 0u;

    if (ctx == NULL || ctx->fp == NULL || ctx->dst == NULL || ctx->bytes == 0u || ctx->done == NULL) {
        if (ctx != NULL) {
            ctx->err = ESP_ERR_INVALID_ARG;
        }
        vTaskDelete(NULL);
        return;
    }

    if (usb_msc_tf_in_use_by_host()) {
        ctx->err = ESP_ERR_INVALID_STATE;
    } else {
        read_start_us = (uint64_t)esp_timer_get_time();
        ctx->read_len = fread(ctx->dst, 1, ctx->bytes, ctx->fp);
        read_elapsed_us = (uint64_t)esp_timer_get_time() - read_start_us;
        ctx->err = (ctx->read_len == ctx->bytes) ? ESP_OK : ESP_FAIL;
        if (ctx->err == ESP_OK && ctx->read_len > 0u && read_elapsed_us > 0u) {
            burner_status_record_tf_to_psram_copy((uint32_t)ctx->read_len, read_elapsed_us);
        }
    }

    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

void burner_tf_writer_task(void *arg)
{
    burner_tf_writer_ctx_t *ctx = (burner_tf_writer_ctx_t *)arg;

    if (ctx == NULL || ctx->fd < 0 || ctx->request == NULL || ctx->done == NULL) {
        if (ctx != NULL) {
            ctx->err = ESP_ERR_INVALID_ARG;
        }
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        xSemaphoreTake(ctx->request, portMAX_DELAY);
        if (ctx->stop) {
            ctx->err = ESP_OK;
            xSemaphoreGive(ctx->done);
            break;
        }

        ctx->written = 0u;
        if (ctx->src == NULL || ctx->bytes == 0u) {
            ctx->err = ESP_ERR_INVALID_ARG;
        } else {
            ctx->err = burner_tf_write_exact(ctx->fd, ctx->src, ctx->bytes);
            if (ctx->err == ESP_OK) {
                ctx->written = ctx->bytes;
            }
        }
        xSemaphoreGive(ctx->done);
    }

    vTaskDelete(NULL);
}

static void burner_erase_task(void *arg)
{
    burner_erase_task_ctx_t *ctx = (burner_erase_task_ctx_t *)arg;

    if (ctx == NULL || ctx->done == NULL) {
        vTaskDelete(NULL);
        return;
    }

    burner_spi_lock_take();
    switch (ctx->op) {
    case BURNER_ERASE_OP_MBC5_RANGE:
        ctx->err = burner_bacon_mbc5_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_GBA_RANGE:
        ctx->err = burner_bacon_gba_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->gba_multi,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_MBC5_CHIP:
        ctx->err = burner_bacon_mbc5_chip_erase();
        break;
    case BURNER_ERASE_OP_GBA_CHIP:
        ctx->err = burner_bacon_gba_chip_erase();
        break;
    default:
        ctx->err = ESP_ERR_INVALID_ARG;
        break;
    }
    burner_spi_lock_give();

    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static esp_err_t burner_erase_exec_in_current_task(burner_erase_task_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    burner_spi_lock_take();
    switch (ctx->op) {
    case BURNER_ERASE_OP_MBC5_RANGE:
        ctx->err = burner_bacon_mbc5_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_GBA_RANGE:
        ctx->err = burner_bacon_gba_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->gba_multi,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_MBC5_CHIP:
        ctx->err = burner_bacon_mbc5_chip_erase();
        break;
    case BURNER_ERASE_OP_GBA_CHIP:
        ctx->err = burner_bacon_gba_chip_erase();
        break;
    default:
        ctx->err = ESP_ERR_INVALID_ARG;
        break;
    }
    burner_spi_lock_give();
    return ctx->err;
}

static esp_err_t burner_run_erase_task(burner_erase_task_ctx_t *ctx)
{
    BaseType_t create_ret;

    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_burn_core_cfg.erase_core == BURNER_CORE_AFFINITY_AUTO) {
        return burner_erase_exec_in_current_task(ctx);
    }

    ctx->done = xSemaphoreCreateBinary();
    if (ctx->done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    create_ret = burner_create_task_with_affinity(
        burner_erase_task,
        "burn_erase",
        4096,
        ctx,
        4,
        NULL,
        s_burn_core_cfg.erase_core);
    if (create_ret != pdPASS) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(ctx->done, portMAX_DELAY);
    vSemaphoreDelete(ctx->done);
    ctx->done = NULL;
    return ctx->err;
}

static esp_err_t burner_run_mbc5_range_erase(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool sample_blank_sectors,
    bool erase_always)
{
    uint32_t planned_sectors = burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t planned_bytes = burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_MBC5_RANGE,
        .addr_begin = addr_begin,
        .addr_end = addr_end,
        .sector_size = sector_size,
        .gba_multi = false,
        .sample_blank_sectors = sample_blank_sectors,
        .erase_always = erase_always,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_gba_range_erase(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool gba_multi,
    bool sample_blank_sectors,
    bool erase_always)
{
    uint32_t planned_sectors = burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t planned_bytes = burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_GBA_RANGE,
        .addr_begin = addr_begin,
        .addr_end = addr_end,
        .sector_size = sector_size,
        .gba_multi = gba_multi,
        .sample_blank_sectors = sample_blank_sectors,
        .erase_always = erase_always,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_mbc5_chip_erase(void)
{
    uint32_t planned_sectors =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : burner_erase_sector_count_from_bytes(s_cart_ctx.device_size, s_cart_ctx.sector_size);
    uint32_t planned_bytes =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : s_cart_ctx.device_size;
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_MBC5_CHIP,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_gba_chip_erase(void)
{
    uint32_t planned_sectors =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : burner_erase_sector_count_from_bytes(s_cart_ctx.device_size, s_cart_ctx.sector_size);
    uint32_t planned_bytes =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : s_cart_ctx.device_size;
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_GBA_CHIP,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static void burner_tf_reader_task(void *arg)
{
    burner_tf_reader_ctx_t *ctx = (burner_tf_reader_ctx_t *)arg;

    if (ctx == NULL || ctx->request == NULL || ctx->done == NULL || ctx->fp == NULL) {
        if (ctx != NULL) {
            ctx->running = false;
        }
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        xSemaphoreTake(ctx->request, portMAX_DELAY);
        if (ctx->stop) {
            ctx->running = false;
            xSemaphoreGive(ctx->done);
            break;
        }

        if (ctx->dst == NULL || ctx->bytes == 0u) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_ARG;
            xSemaphoreGive(ctx->done);
            continue;
        }

        if (burner_cancel_is_requested()) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(ctx->done);
            continue;
        }

        if (usb_msc_tf_in_use_by_host()) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(ctx->done);
            continue;
        }

        ctx->read_len = fread(ctx->dst, 1, ctx->bytes, ctx->fp);
        ctx->err = (ctx->read_len == ctx->bytes) ? ESP_OK : ESP_FAIL;
        xSemaphoreGive(ctx->done);
    }

    vTaskDelete(NULL);
}

static esp_err_t burner_tf_reader_start(burner_tf_reader_ctx_t *ctx, FILE *fp)
{
    BaseType_t create_ret;

    if (ctx == NULL || fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fp = fp;

    if (s_burn_core_cfg.tf_core == BURNER_CORE_AFFINITY_AUTO) {
        return ESP_OK;
    }

    ctx->request = xSemaphoreCreateBinary();
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->request == NULL || ctx->done == NULL) {
        if (ctx->request != NULL) {
            vSemaphoreDelete(ctx->request);
            ctx->request = NULL;
        }
        if (ctx->done != NULL) {
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ctx->running = true;
    create_ret = burner_create_task_with_affinity(
        burner_tf_reader_task,
        "tf_reader",
        4096,
        ctx,
        4,
        &ctx->task,
        s_burn_core_cfg.tf_core);
    if (create_ret != pdPASS) {
        ctx->running = false;
        vSemaphoreDelete(ctx->request);
        vSemaphoreDelete(ctx->done);
        ctx->request = NULL;
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t burner_tf_reader_read(burner_tf_reader_ctx_t *ctx, uint8_t *dst, size_t bytes)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_burn_core_cfg.tf_core == BURNER_CORE_AFFINITY_AUTO || !ctx->running || ctx->request == NULL ||
        ctx->done == NULL) {
        return burner_tf_read_exact(ctx->fp, dst, bytes);
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx->dst = dst;
    ctx->bytes = bytes;
    ctx->read_len = 0u;
    ctx->err = ESP_FAIL;
    xSemaphoreGive(ctx->request);
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    if (ctx->err != ESP_OK) {
        return ctx->err;
    }
    return (ctx->read_len == bytes) ? ESP_OK : ESP_FAIL;
}

static void burner_tf_reader_stop(burner_tf_reader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->running && ctx->request != NULL && ctx->done != NULL) {
        ctx->stop = true;
        xSemaphoreGive(ctx->request);
        xSemaphoreTake(ctx->done, portMAX_DELAY);
    }

    if (ctx->request != NULL) {
        vSemaphoreDelete(ctx->request);
        ctx->request = NULL;
    }
    if (ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    ctx->running = false;
}

esp_err_t burner_tf_writer_start(burner_tf_writer_ctx_t *ctx, int fd)
{
    BaseType_t create_ret;

    if (ctx == NULL || fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = fd;

    ctx->request = xSemaphoreCreateBinary();
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->request == NULL || ctx->done == NULL) {
        if (ctx->request != NULL) {
            vSemaphoreDelete(ctx->request);
            ctx->request = NULL;
        }
        if (ctx->done != NULL) {
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ctx->running = true;
    create_ret = burner_create_task_with_affinity(
        burner_tf_writer_task,
        "tf_writer",
        4096,
        ctx,
        4,
        &ctx->task,
        s_burn_core_cfg.tf_core);
    if (create_ret != pdPASS) {
        ctx->running = false;
        vSemaphoreDelete(ctx->request);
        vSemaphoreDelete(ctx->done);
        ctx->request = NULL;
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t burner_tf_writer_submit(burner_tf_writer_ctx_t *ctx, const uint8_t *src, size_t bytes)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->running || ctx->request == NULL || ctx->done == NULL) {
        return burner_tf_write_exact(ctx->fd, src, bytes);
    }

    if (src == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->src = src;
    ctx->bytes = bytes;
    ctx->written = 0u;
    ctx->err = ESP_FAIL;
    xSemaphoreGive(ctx->request);
    return ESP_OK;
}

esp_err_t burner_tf_writer_wait(burner_tf_writer_ctx_t *ctx)
{
    uint64_t wait_start_us;
    uint64_t wait_end_us;

    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->running || ctx->request == NULL || ctx->done == NULL) {
        return ESP_OK;
    }

    wait_start_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    wait_end_us = (uint64_t)esp_timer_get_time();
    if (wait_end_us > wait_start_us) {
        burner_status_record_dump_wait(wait_end_us - wait_start_us);
    }
    if (ctx->err != ESP_OK) {
        return ctx->err;
    }
    return (ctx->written == ctx->bytes) ? ESP_OK : ESP_FAIL;
}

void burner_tf_writer_stop(burner_tf_writer_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->running && ctx->request != NULL && ctx->done != NULL) {
        ctx->stop = true;
        xSemaphoreGive(ctx->request);
        xSemaphoreTake(ctx->done, portMAX_DELAY);
    }

    if (ctx->request != NULL) {
        vSemaphoreDelete(ctx->request);
        ctx->request = NULL;
    }
    if (ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    ctx->running = false;
}

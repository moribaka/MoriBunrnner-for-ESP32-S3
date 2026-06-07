/* GBA ROM burn job implementations. */

static void burner_gba_post_write_diag_log_first_mismatch(
    const char *label,
    uint32_t base_addr,
    const uint8_t *expected,
    const uint8_t *actual,
    size_t len)
{
    size_t i;

    if (label == NULL || expected == NULL || actual == NULL || len == 0u) {
        return;
    }
    for (i = 0u; i < len; ++i) {
        if (expected[i] != actual[i]) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA post-write %s mismatch @0x%08" PRIX32 " file=%02X cart=%02X",
                label,
                base_addr + (uint32_t)i,
                expected[i],
                actual[i]);
            return;
        }
    }
}

static void burner_gba_post_write_diag_log_prefix(
    const char *label,
    const uint8_t *buf,
    size_t len)
{
    enum {
        BURNER_GBA_POST_WRITE_PREFIX_BYTES = 16
    };
    char line[BURNER_GBA_POST_WRITE_PREFIX_BYTES * 3 + 1];
    size_t prefix_len;
    size_t pos = 0u;

    if (label == NULL || buf == NULL || len == 0u) {
        return;
    }

    prefix_len = (len < BURNER_GBA_POST_WRITE_PREFIX_BYTES) ? len : (size_t)BURNER_GBA_POST_WRITE_PREFIX_BYTES;
    for (size_t i = 0u; i < prefix_len && (pos + 3u) < sizeof(line); ++i) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%02X", buf[i]);
        if (i + 1u < prefix_len && (pos + 1u) < sizeof(line)) {
            line[pos++] = ' ';
            line[pos] = '\0';
        }
    }
    line[sizeof(line) - 1u] = '\0';
    ESP_LOGW(BURNER_TAG, "GBA post-write %s prefix[%u]: %s", label, (unsigned)prefix_len, line);
}

static void burner_gba_post_write_diag_log_swapped_prefix(
    const char *label,
    const uint8_t *buf,
    size_t len)
{
    enum {
        BURNER_GBA_POST_WRITE_PREFIX_BYTES = 16
    };
    uint8_t swapped[BURNER_GBA_POST_WRITE_PREFIX_BYTES];
    size_t prefix_len;

    if (label == NULL || buf == NULL || len == 0u) {
        return;
    }

    prefix_len = (len < BURNER_GBA_POST_WRITE_PREFIX_BYTES) ? len : (size_t)BURNER_GBA_POST_WRITE_PREFIX_BYTES;
    for (size_t i = 0u; i < prefix_len; ++i) {
        swapped[i] = (uint8_t)SWAP_D0D1_U8(buf[i]);
    }
    burner_gba_post_write_diag_log_prefix(label, swapped, prefix_len);
}

static void burner_gba_post_write_diag_log_swap_match(
    const char *label,
    const uint8_t *expected,
    const uint8_t *actual,
    size_t len)
{
    enum {
        BURNER_GBA_POST_WRITE_COMPARE_BYTES = 32
    };
    size_t compare_len;
    size_t match_count = 0u;

    if (label == NULL || expected == NULL || actual == NULL || len == 0u) {
        return;
    }

    compare_len = (len < BURNER_GBA_POST_WRITE_COMPARE_BYTES) ? len : (size_t)BURNER_GBA_POST_WRITE_COMPARE_BYTES;
    for (size_t i = 0u; i < compare_len; ++i) {
        if ((uint8_t)SWAP_D0D1_U8(expected[i]) == actual[i]) {
            match_count++;
        }
    }
    ESP_LOGW(
        BURNER_TAG,
        "GBA post-write %s swapped-byte matches: %u/%u",
        label,
        (unsigned)match_count,
        (unsigned)compare_len);
}

static bool burner_gba_apply_header_checksum_fix(
    uint8_t *buf,
    size_t len,
    uint32_t file_offset,
    bool log_change)
{
    enum {
        BURNER_GBA_HEADER_CHECKSUM_START = 0xA0u,
        BURNER_GBA_HEADER_CHECKSUM_OFFSET = 0xBDu,
    };
    uint8_t checksum = 0u;
    uint8_t old_checksum;

    if (buf == NULL || file_offset != 0u || len <= BURNER_GBA_HEADER_CHECKSUM_OFFSET) {
        return false;
    }

    for (size_t i = BURNER_GBA_HEADER_CHECKSUM_START; i < BURNER_GBA_HEADER_CHECKSUM_OFFSET; ++i) {
        checksum = (uint8_t)(checksum - buf[i]);
    }
    checksum = (uint8_t)((checksum - 0x19u) & 0xFFu);
    old_checksum = buf[BURNER_GBA_HEADER_CHECKSUM_OFFSET];
    if (old_checksum == checksum) {
        return false;
    }

    buf[BURNER_GBA_HEADER_CHECKSUM_OFFSET] = checksum;
    if (log_change) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA ROM header checksum auto-fixed: old=%02X new=%02X",
            old_checksum,
            checksum);
    }
    return true;
}

static esp_err_t burner_gba_post_write_header_diag(FILE *fp, const burner_task_param_t *job)
{
    enum {
        BURNER_GBA_POST_WRITE_DIAG_BYTES = 0x200
    };
    uint8_t rom_buf[BURNER_GBA_POST_WRITE_DIAG_BYTES];
    uint8_t host_buf[BURNER_GBA_POST_WRITE_DIAG_BYTES];
    uint8_t std_buf[BURNER_GBA_POST_WRITE_DIAG_BYTES];
    uint32_t diag_addr;
    size_t diag_len;
    size_t got;
    bool host_match;
    bool std_match;
    esp_err_t err;

    if (fp == NULL || job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    diag_addr = job->addr_begin;
    diag_len = (job->total_bytes < BURNER_GBA_POST_WRITE_DIAG_BYTES)
                   ? (size_t)job->total_bytes
                   : (size_t)BURNER_GBA_POST_WRITE_DIAG_BYTES;
    diag_len &= ~((size_t)0x1u);
    if (diag_len < 2u) {
        return ESP_OK;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA post-write header diag skipped: seek failed file_offset=0 addr=0x%08" PRIX32,
            diag_addr);
        return ESP_FAIL;
    }
    got = fread(rom_buf, 1, diag_len, fp);
    if (got != diag_len) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA post-write header diag skipped: file read short got=%u need=%u addr=0x%08" PRIX32,
            (unsigned)got,
            (unsigned)diag_len,
            diag_addr);
        return ESP_FAIL;
    }
    (void)burner_gba_apply_header_checksum_fix(rom_buf, diag_len, 0u, false);

    burner_spi_lock_take();
    err = burner_bacon_gba_verify_read_block_hoststyle(
        host_buf,
        diag_len,
        diag_addr,
        burner_is_gba_multi_card(job));
    if (err == ESP_OK) {
        err = burner_bacon_gba_read_block(
            std_buf,
            diag_len,
            diag_addr,
            burner_is_gba_multi_card(job));
    }
    burner_spi_lock_give();
    if (err != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA post-write header diag read failed @0x%08" PRIX32 " err=%s",
            diag_addr,
            esp_err_to_name(err));
        return err;
    }

    host_match = (memcmp(rom_buf, host_buf, diag_len) == 0);
    std_match = (memcmp(rom_buf, std_buf, diag_len) == 0);

    if ((!host_match || !std_match) &&
        !burner_gba_gbx_is_active() &&
        burner_gba_nor_is_intel_active() &&
        s_cart_ctx.probe_cfi_ok) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA post-write header diag mismatch after first read; retrying after final Intel reset");
        burner_spi_lock_take();
        err = burner_bacon_gba_finalize_write(burner_is_gba_multi_card(job));
        if (err == ESP_OK) {
            err = burner_bacon_gba_verify_read_block_hoststyle(
                host_buf,
                diag_len,
                diag_addr,
                burner_is_gba_multi_card(job));
        }
        if (err == ESP_OK) {
            err = burner_bacon_gba_read_block(
                std_buf,
                diag_len,
                diag_addr,
                burner_is_gba_multi_card(job));
        }
        burner_spi_lock_give();
        if (err != ESP_OK) {
            ESP_LOGW(
                BURNER_TAG,
                "GBA post-write header diag retry failed @0x%08" PRIX32 " err=%s",
                diag_addr,
                esp_err_to_name(err));
            return err;
        }
        host_match = (memcmp(rom_buf, host_buf, diag_len) == 0);
        std_match = (memcmp(rom_buf, std_buf, diag_len) == 0);
        ESP_LOGI(
            BURNER_TAG,
            "GBA post-write header diag retry: hoststyle=%s standard=%s addr=0x%08" PRIX32 " len=%u",
            host_match ? "match" : "mismatch",
            std_match ? "match" : "mismatch",
            diag_addr,
            (unsigned)diag_len);
    }

    if (host_match && std_match) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA post-write header diag: hoststyle=match standard=match addr=0x%08" PRIX32 " len=%u",
            diag_addr,
            (unsigned)diag_len);
        return ESP_OK;
    }
    if (host_match) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA post-write header diag: hoststyle=match standard=mismatch addr=0x%08" PRIX32 " len=%u swapped=%u",
            diag_addr,
            (unsigned)diag_len,
            s_cart_ctx.d0d1_swapped ? 1u : 0u);
        burner_gba_post_write_diag_log_first_mismatch("standard", diag_addr, rom_buf, std_buf, diag_len);
        return ESP_OK;
    }
    if (std_match) {
        ESP_LOGW(
            BURNER_TAG,
            "GBA post-write header diag: hoststyle=mismatch standard=match addr=0x%08" PRIX32 " len=%u swapped=%u",
            diag_addr,
            (unsigned)diag_len,
            s_cart_ctx.d0d1_swapped ? 1u : 0u);
        burner_gba_post_write_diag_log_first_mismatch("hoststyle", diag_addr, rom_buf, host_buf, diag_len);
        return ESP_OK;
    }

    ESP_LOGW(
        BURNER_TAG,
        "GBA post-write header diag: hoststyle=mismatch standard=mismatch addr=0x%08" PRIX32 " len=%u swapped=%u",
        diag_addr,
        (unsigned)diag_len,
        s_cart_ctx.d0d1_swapped ? 1u : 0u);
    burner_gba_post_write_diag_log_prefix("file", rom_buf, diag_len);
    burner_gba_post_write_diag_log_prefix("hoststyle", host_buf, diag_len);
    burner_gba_post_write_diag_log_prefix("standard", std_buf, diag_len);
    if (s_cart_ctx.d0d1_swapped) {
        burner_gba_post_write_diag_log_swapped_prefix("file-swapped", rom_buf, diag_len);
        burner_gba_post_write_diag_log_swap_match("hoststyle", rom_buf, host_buf, diag_len);
        burner_gba_post_write_diag_log_swap_match("standard", rom_buf, std_buf, diag_len);
    }
    burner_gba_post_write_diag_log_first_mismatch("hoststyle", diag_addr, rom_buf, host_buf, diag_len);
    burner_gba_post_write_diag_log_first_mismatch("standard", diag_addr, rom_buf, std_buf, diag_len);
    return ESP_FAIL;
}

static size_t burner_gba_program_chunk_limit_bytes(void)
{
    if (burner_gba_chislink_is_active()) {
        return 32768u;
    }
    if (!burner_gba_gbx_is_active() && burner_gba_nor_is_intel_active()) {
        return 32768u;
    }
    return BURN_GBA_PROGRAM_CHUNK_BYTES;
}

static esp_err_t burner_gba_gbx_compare_sector_data(
    const uint8_t *expected,
    uint32_t addr,
    size_t len,
    bool is_multi_card,
    bool *match_out)
{
    enum {
        BURNER_GBA_GBX_COMPARE_CHUNK_BYTES = 2048u
    };
    uint8_t actual[BURNER_GBA_GBX_COMPARE_CHUNK_BYTES];
    size_t compared = 0u;

    if (expected == NULL || len == 0u || match_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *match_out = false;
    while (compared < len) {
        size_t chunk = len - compared;
        esp_err_t err;

        if (chunk > sizeof(actual)) {
            chunk = sizeof(actual);
        }
        err = burner_gba_gbx_read_chip_bytes(addr + (uint32_t)compared, actual, chunk, is_multi_card);
        if (err != ESP_OK) {
            return err;
        }
        if (memcmp(expected + compared, actual, chunk) != 0) {
            return ESP_OK;
        }
        compared += chunk;
        burner_task_yield_if_due();
    }

    *match_out = true;
    return ESP_OK;
}

static esp_err_t burner_run_write_job_gba_gbx(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    burner_tf_reader_ctx_t tf_reader = {0};
    burner_nor_region_cursor_t cursor = {0};
    uint8_t *sector_buf = NULL;
    uint32_t processed = 0u;
    uint32_t largest_sector_size = 0u;
    bool tf_reader_started = false;
    bool write_timer_started = false;
    bool erase_timer_started = false;
    bool sector_buf_in_psram = false;
    bool use_chip_erase = false;
    bool is_multi_card;
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_nor_geometry_is_valid(&s_cart_ctx.geometry)) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gbx sector geometry unavailable",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }

    err = burner_nor_geometry_largest_sector_size_in_range(
        &s_cart_ctx.geometry,
        job->addr_begin,
        job->total_bytes,
        &largest_sector_size);
    if (err != ESP_OK || largest_sector_size == 0u) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gbx sector size unavailable",
            job->rom_name,
            job->rom_path);
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open rom failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto gbx_write_done;
    }

    err = burner_tf_reader_start(&tf_reader, fp);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "create tf reader failed",
            job->rom_name,
            job->rom_path);
        goto gbx_write_done;
    }
    tf_reader_started = true;

    if (job->write_path != BURNER_WRITE_PATH_DIRECT) {
        sector_buf = (uint8_t *)heap_caps_malloc(largest_sector_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        sector_buf_in_psram = (sector_buf != NULL);
    }
    if (sector_buf == NULL) {
        sector_buf = (uint8_t *)malloc(largest_sector_size);
        sector_buf_in_psram = false;
    }
    if (sector_buf == NULL) {
        sector_buf = (uint8_t *)heap_caps_malloc(largest_sector_size, MALLOC_CAP_8BIT);
        sector_buf_in_psram = false;
    }
    if (sector_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "no memory for gbx sector buffer",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto gbx_write_done;
    }

    burner_status_plan_erase_phase(
        burner_nor_geometry_sector_count_from_range(
            &s_cart_ctx.geometry,
            job->addr_begin,
            job->addr_begin + job->total_bytes - 1u),
        burner_nor_geometry_erase_bytes_from_range(
            &s_cart_ctx.geometry,
            job->addr_begin,
            job->addr_begin + job->total_bytes - 1u),
        burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry));

    err = burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry, job->addr_begin, &cursor);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gbx sector cursor unavailable",
            job->rom_name,
            job->rom_path);
        goto gbx_write_done;
    }

    burner_status_mark_write_manual_begin();
    write_timer_started = true;
    is_multi_card = burner_is_gba_multi_card(job);
    use_chip_erase =
        (s_cart_ctx.gbx.sector_erase.count == 0u) &&
        (s_cart_ctx.gbx.chip_erase.count > 0u);

    if (use_chip_erase) {
        uint64_t erase_start_us = burner_gba_diag_now_us();

        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "gbx chip erase before program",
            job->rom_name,
            job->rom_path);
        burner_status_mark_erase_begin();
        erase_timer_started = true;
        err = burner_run_gba_chip_erase();
        burner_status_mark_erase_end();
        erase_timer_started = false;
        burner_gba_chis_diag_add_erase(burner_gba_diag_now_us() - erase_start_us);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "gbx chip erase failed",
                job->rom_name,
                job->rom_path);
            goto gbx_write_done;
        }
    }

    while (processed < job->total_bytes) {
        uint32_t sector_addr = job->addr_begin + processed;
        uint32_t sector_begin = 0u;
        uint32_t sector_end = 0u;
        uint32_t sector_size = 0u;
        uint32_t sector_processed_before = processed;
        size_t sector_bytes;
        uint64_t tf_read_start_us;
        uint64_t tf_read_elapsed_us;
        int progress;
        bool sector_match = false;
        bool erase_counted = false;

        burner_gba_chis_diag_stage_begin();
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            goto gbx_write_done;
        }
        err = burner_nor_geometry_region_cursor_seek_forward(&s_cart_ctx.geometry, sector_addr, &cursor);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "gbx sector cursor seek failed",
                job->rom_name,
                job->rom_path);
            goto gbx_write_done;
        }
        err = burner_nor_geometry_sector_bounds_in_cursor(
            &cursor,
            sector_addr,
            &sector_begin,
            &sector_end,
            &sector_size);
        if (err != ESP_OK || sector_end <= sector_addr || sector_size == 0u) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "gbx sector bounds invalid",
                job->rom_name,
                job->rom_path);
            err = (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
            goto gbx_write_done;
        }

        sector_bytes = (size_t)(job->total_bytes - processed);
        if (sector_bytes > (size_t)(sector_end - sector_addr)) {
            sector_bytes = (size_t)(sector_end - sector_addr);
        }

        burner_status_update(
            BURNER_STATE_BURNING,
            burner_calc_progress_percent_u64(processed, job->total_bytes),
            processed,
            job->total_bytes,
            sector_buf_in_psram ? "gbx copy tf->psram sector" : "gbx copy tf->ram sector",
            job->rom_name,
            job->rom_path);

        tf_read_start_us = burner_gba_diag_now_us();
        err = burner_tf_reader_read(&tf_reader, sector_buf, sector_bytes);
        tf_read_elapsed_us = burner_gba_diag_now_us() - tf_read_start_us;
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                job->rom_name,
                job->rom_path);
            goto gbx_write_done;
        }
        if (sector_buf_in_psram && sector_bytes > 0u && tf_read_elapsed_us > 0u) {
            burner_status_record_tf_to_psram_copy((uint32_t)sector_bytes, tf_read_elapsed_us);
        }
        burner_gba_chis_diag_add_tf_read(tf_read_elapsed_us);
        (void)burner_gba_apply_header_checksum_fix(
            sector_buf,
            sector_bytes,
            processed,
            processed == 0u);

        if (!use_chip_erase && !job->erase_always && sector_bytes > 0u) {
            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed,
                job->total_bytes,
                "gbx compare sector",
                job->rom_name,
                job->rom_path);

            burner_spi_lock_take();
            err = burner_gba_gbx_reset_to_read_mode(false, is_multi_card, 0u);
            if (err == ESP_OK) {
                err = burner_gba_gbx_compare_sector_data(
                    sector_buf,
                    sector_addr,
                    sector_bytes,
                    is_multi_card,
                    &sector_match);
            }
            burner_spi_lock_give();
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    "gbx compare sector failed",
                    job->rom_name,
                    job->rom_path);
                goto gbx_write_done;
            }

            if (sector_match) {
                burner_status_advance_erase_phase(1u, sector_size);
                processed += (uint32_t)sector_bytes;
                progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
                if (progress > 100) {
                    progress = 100;
                }
                burner_status_update(
                    BURNER_STATE_BURNING,
                    progress,
                    processed,
                    job->total_bytes,
                    "gbx sector matched",
                    job->rom_name,
                    job->rom_path);
                burner_emit_progress_cb(progress, processed);
                burner_gba_chis_diag_log_stage(sector_addr, sector_bytes, sector_processed_before, processed);
                burner_task_yield_if_due();
                continue;
            }
        }

        for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
            uint64_t erase_start_us;
            uint64_t program_start_us;
            uint64_t program_elapsed_us;

            if (attempt > 0u) {
                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    "gbx retry sector",
                    job->rom_name,
                    job->rom_path);
                burner_spi_lock_take();
                err = burner_gba_gbx_reset_to_read_mode(true, is_multi_card, s_cart_ctx.device_size);
                burner_spi_lock_give();
                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "gbx reset before retry failed",
                        job->rom_name,
                        job->rom_path);
                    goto gbx_write_done;
                }
            }

            if (!use_chip_erase) {
                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    "gbx erase current sector",
                    job->rom_name,
                    job->rom_path);

                erase_start_us = burner_gba_diag_now_us();
                burner_status_mark_erase_begin();
                erase_timer_started = true;
                burner_spi_lock_take();
                err = burner_bacon_gba_erase_sector(
                    sector_addr,
                    is_multi_card,
                    burner_erase_timeout_ms_for_bytes(sector_size));
                burner_spi_lock_give();
                burner_status_mark_erase_end();
                erase_timer_started = false;
                burner_gba_chis_diag_add_erase(burner_gba_diag_now_us() - erase_start_us);
                if (err != ESP_OK) {
                    if (attempt + 1u >= 2u) {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            0,
                            processed,
                            job->total_bytes,
                            "gbx erase sector failed",
                            job->rom_name,
                            job->rom_path);
                        goto gbx_write_done;
                    }
                    continue;
                }
                if (!erase_counted) {
                    burner_status_advance_erase_phase(1u, sector_size);
                    erase_counted = true;
                }
            }

            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed,
                job->total_bytes,
                "gbx program current sector",
                job->rom_name,
                job->rom_path);

            program_start_us = burner_gba_diag_now_us();
            burner_spi_lock_take();
            err = burner_gba_gbx_program_block(sector_buf, sector_bytes, sector_addr, is_multi_card, false);
            burner_spi_lock_give();
            program_elapsed_us = burner_gba_diag_now_us() - program_start_us;
            if (err == ESP_OK) {
                burner_status_record_write_sample((uint32_t)sector_bytes, program_elapsed_us);
                break;
            }

            if (attempt + 1u >= 2u) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    "gbx program sector failed",
                    job->rom_name,
                    job->rom_path);
                goto gbx_write_done;
            }
        }

        processed += (uint32_t)sector_bytes;
        progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            job->total_bytes,
            "gbx sector programmed",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
        burner_gba_chis_diag_log_stage(sector_addr, sector_bytes, sector_processed_before, processed);
        burner_task_yield_if_due();
    }

    if (err == ESP_OK && processed != job->total_bytes) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            processed,
            job->total_bytes,
            "tf file shorter than expected",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        uint64_t finalize_start_us = burner_gba_diag_now_us();

        burner_status_update(
            BURNER_STATE_BURNING,
            100,
            processed,
            job->total_bytes,
            "finalizing gbx flash state",
            job->rom_name,
            job->rom_path);
        burner_spi_lock_take();
        err = burner_bacon_gba_finalize_write(is_multi_card);
        burner_spi_lock_give();
        burner_gba_chis_diag_add_finalize(burner_gba_diag_now_us() - finalize_start_us);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "final gbx flash reset failed",
                job->rom_name,
                job->rom_path);
            goto gbx_write_done;
        }
    }
    if (err == ESP_OK) {
        uint64_t post_verify_start_us = burner_gba_diag_now_us();

        burner_status_update(
            BURNER_STATE_BURNING,
            100,
            processed,
            job->total_bytes,
            "post-write gbx header check",
            job->rom_name,
            job->rom_path);
        err = burner_gba_post_write_header_diag(fp, job);
        burner_gba_chis_diag_add_post_verify(burner_gba_diag_now_us() - post_verify_start_us);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "post-write gbx header verify failed",
                job->rom_name,
                job->rom_path);
        }
    }

gbx_write_done:
    burner_gba_sector_erase_ctx_reset();
    if (erase_timer_started) {
        burner_status_mark_erase_end();
    }
    if (write_timer_started) {
        burner_status_mark_write_end();
    }
    if (tf_reader_started) {
        burner_tf_reader_stop(&tf_reader);
    }
    if (sector_buf != NULL) {
        free(sector_buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    burner_gba_chis_diag_log_summary(err);
    return err;
}

static esp_err_t burner_run_write_job_gba(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint8_t *psram_stage_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool should_erase = false;
    bool use_psram_stage = false;
    bool use_pipeline_stage = false;
    bool intel_active = false;
    bool force_erase_sectors = true;
    bool erase_every_sector = false;
    size_t stage_capacity = 0;
    burner_tf_prefetch_ctx_t prefetch = {0};
    burner_tf_reader_ctx_t tf_reader = {0};
    burner_nor_region_cursor_t pipeline_cursor = {0};
    SemaphoreHandle_t prefetch_done = NULL;
    bool prefetch_inflight = false;
    bool prefetch_started = false;
    bool tf_reader_started = false;
    bool erase_timer_started = false;
    bool write_timer_started = false;
    bool sector_geometry_valid = false;
    uint32_t psram_window_mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    uint32_t psram_window_bytes = BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES;
    char psram_alloc_fail_msg[96] = {0};
    char psram_erase_prefetch_msg[96] = {0};
    char psram_copy_msg[64] = {0};

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    use_psram_stage = (job->write_path == BURNER_WRITE_PATH_PSRAM ||
                       job->write_path == BURNER_WRITE_PATH_PIPELINE);
    use_pipeline_stage = (job->write_path == BURNER_WRITE_PATH_PIPELINE);
    if (use_pipeline_stage) {
        psram_window_mb = BURN_PSRAM_WINDOW_AUTO_MB;
        psram_window_bytes = 0u;
    } else if (s_gba_fixed_erase_window_enabled != 0u) {
        psram_window_mb = BURN_GBA_FIXED_ERASE_WINDOW_MB;
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    } else {
        psram_window_mb = burner_psram_window_bytes_to_mb(job->psram_window_bytes);
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    }
    (void)snprintf(
        psram_alloc_fail_msg,
        sizeof(psram_alloc_fail_msg),
        use_pipeline_stage ? "alloc pipeline psram staging failed" : "alloc %uMB psram staging failed",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_erase_prefetch_msg,
        sizeof(psram_erase_prefetch_msg),
        use_pipeline_stage ? "pipeline erase gba sector + prefetch tf->psram" : "erasing gba flash sectors (%uMB) + prefetch tf->psram",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_copy_msg,
        sizeof(psram_copy_msg),
        use_pipeline_stage ? "pipeline copy tf->psram (sector window)" : "copy tf->psram (%uMB window)",
        (unsigned)psram_window_mb);
    if (use_psram_stage && !use_pipeline_stage) {
        ESP_LOGI(
            BURNER_TAG,
            "GBA psram erase window policy: %s window=%uMB",
            s_gba_fixed_erase_window_enabled != 0u ? "fixed" : "dynamic",
            (unsigned)psram_window_mb);
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addr_begin & 0x1u) != 0u || (job->total_bytes & 0x1u) != 0u) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba write range must be even aligned",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing gba cart and preparing flash",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)job->total_bytes) > (uint64_t)s_cart_ctx.device_size) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "selected range exceeds gba flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!burner_gba_gbx_is_active() &&
        s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL && !s_cart_ctx.probe_cfi_ok) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "intel gba write requires valid cfi",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    burner_gba_chis_diag_begin(job);
    intel_active = burner_gba_nor_is_intel_active();
    burner_gba_chis_diag_set_intel(intel_active);
    if (burner_gba_gbx_is_active()) {
        return burner_run_write_job_gba_gbx(job);
    }
    if (intel_active && job->write_path == BURNER_WRITE_PATH_PSRAM) {
        psram_window_mb = BURN_GBA_FIXED_ERASE_WINDOW_MB;
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
        (void)snprintf(
            psram_alloc_fail_msg,
            sizeof(psram_alloc_fail_msg),
            "alloc %uMB psram staging failed",
            (unsigned)psram_window_mb);
        (void)snprintf(
            psram_erase_prefetch_msg,
            sizeof(psram_erase_prefetch_msg),
            "intel erase gba flash sectors (%uMB) + prefetch tf->psram",
            (unsigned)psram_window_mb);
        (void)snprintf(
            psram_copy_msg,
            sizeof(psram_copy_msg),
            "copy tf->psram (%uMB window)",
            (unsigned)psram_window_mb);
        ESP_LOGI(
            BURNER_TAG,
            "GBA Intel PSRAM policy: fixed %uMB window, erase+prefetch then program",
            (unsigned)psram_window_mb);
    }
    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open rom failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto write_gba_done;
    }

    err = burner_tf_reader_start(&tf_reader, fp);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "create tf reader failed",
            job->rom_name,
            job->rom_path);
        goto write_gba_done;
    }
    tf_reader_started = true;

    if (use_psram_stage) {
        if (use_pipeline_stage) {
            uint32_t pipeline_stage_capacity = 0u;

            err = burner_nor_geometry_largest_sector_size_in_range(
                &s_cart_ctx.geometry,
                addr_begin,
                job->total_bytes,
                &pipeline_stage_capacity);
            if (err != ESP_OK || pipeline_stage_capacity == 0u) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    0,
                    job->total_bytes,
                    "gba pipeline sector geometry unavailable",
                    job->rom_name,
                    job->rom_path);
                goto write_gba_done;
            }
            stage_capacity = (job->total_bytes < pipeline_stage_capacity)
                                 ? (size_t)job->total_bytes
                                 : (size_t)pipeline_stage_capacity;
        } else {
            stage_capacity = (job->total_bytes < psram_window_bytes)
                                 ? (size_t)job->total_bytes
                                 : (size_t)psram_window_bytes;
        }
        psram_stage_buf = (uint8_t *)heap_caps_malloc(
            stage_capacity,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (psram_stage_buf == NULL) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                psram_alloc_fail_msg,
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_NO_MEM;
            goto write_gba_done;
        }
    } else {
        buf = (uint8_t *)malloc(BURN_GBA_PROGRAM_CHUNK_BYTES);
        if (buf == NULL) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "no memory for gba write chunk",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_NO_MEM;
            goto write_gba_done;
        }
    }

    force_erase_sectors = job->erase_always;
    erase_every_sector = force_erase_sectors || intel_active;
    if (force_erase_sectors) {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBA burn erase policy: force erase for all write paths");
    } else if (intel_active) {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBA burn erase policy: per-sector erase for Intel (blank-skip disabled)");
    } else {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBA burn erase policy: smart sector-sampled erase");
    }
    sector_geometry_valid = burner_nor_geometry_is_valid(&s_cart_ctx.geometry);
    if (should_erase && !sector_geometry_valid) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba sector geometry unavailable",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_INVALID_SIZE;
        goto write_gba_done;
    }
    if (should_erase && sector_geometry_valid) {
        uint32_t planned_erase_sectors =
            use_pipeline_stage
                ? burner_nor_geometry_sector_count_from_range(
                      &s_cart_ctx.geometry,
                      addr_begin,
                      addr_begin + job->total_bytes - 1u)
                : (use_psram_stage
                       ? burner_nor_geometry_planned_stage_erase_sector_count(
                             &s_cart_ctx.geometry,
                             addr_begin,
                             job->total_bytes,
                             (uint32_t)stage_capacity)
                       : burner_nor_geometry_sector_count_from_range(
                             &s_cart_ctx.geometry,
                             addr_begin,
                             addr_begin + job->total_bytes - 1u));
        uint32_t planned_erase_bytes = burner_nor_geometry_erase_bytes_from_range(
            &s_cart_ctx.geometry,
            addr_begin,
            addr_begin + job->total_bytes - 1u);
        burner_status_plan_erase_phase(
            planned_erase_sectors,
            planned_erase_bytes,
            burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry));
    }
    if (use_pipeline_stage) {
        err = burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry, addr_begin, &pipeline_cursor);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "gba pipeline sector cursor unavailable",
                job->rom_name,
                job->rom_path);
            goto write_gba_done;
        }
    }
    burner_gba_sector_erase_ctx_reset();
    if (use_pipeline_stage && should_erase) {
        burner_gba_sector_erase_ctx_begin(
            addr_begin,
            addr_begin + job->total_bytes - 1u,
            s_cart_ctx.sector_size,
            burner_is_gba_multi_card(job),
            erase_every_sector);
    }
    if (should_erase && !use_psram_stage) {
        uint64_t erase_start_us;

        burner_status_mark_erase_begin();
        erase_timer_started = true;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "erasing gba flash sectors",
            job->rom_name,
            job->rom_path);

        erase_start_us = burner_gba_diag_now_us();
        err = burner_run_gba_range_erase(
            addr_begin,
            addr_begin + job->total_bytes - 1u,
            s_cart_ctx.sector_size,
            burner_is_gba_multi_card(job),
            !erase_every_sector,
            erase_every_sector);
        burner_status_mark_erase_end();
        erase_timer_started = false;
        burner_gba_chis_diag_add_erase(burner_gba_diag_now_us() - erase_start_us);

        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "erase gba flash failed",
                job->rom_name,
                job->rom_path);
            goto write_gba_done;
        }
    }

    burner_status_mark_write_manual_begin();
    write_timer_started = true;
    if (use_psram_stage) {
        while (processed < job->total_bytes) {
            size_t stage_bytes = (size_t)(job->total_bytes - processed);
            size_t stage_off = 0;
            uint32_t stage_addr = addr_begin + processed;
            bool stage_prefetched = false;
            uint32_t stage_processed_before = processed;

            if (use_pipeline_stage) {
                uint32_t pipeline_stage_bytes = 0u;

                err = burner_nor_geometry_region_cursor_seek_forward(
                    &s_cart_ctx.geometry,
                    stage_addr,
                    &pipeline_cursor);
                if (err == ESP_OK) {
                    err = burner_nor_geometry_stage_bytes_in_cursor(
                        &pipeline_cursor,
                        stage_addr,
                        job->total_bytes - processed,
                        &pipeline_stage_bytes);
                }
                if (err != ESP_OK || pipeline_stage_bytes == 0u) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "gba pipeline sector window invalid",
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
                stage_bytes = (size_t)pipeline_stage_bytes;
            } else if (stage_bytes > stage_capacity) {
                stage_bytes = stage_capacity;
            }
            burner_gba_chis_diag_stage_begin();
            if (burner_gba_should_log_program_boundary(stage_addr, stage_bytes, processed, job->total_bytes)) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA write stage: addr=0x%08" PRIX32 " bytes=%u processed=%" PRIu32 "/%" PRIu32 " path=%s erase=%s",
                    stage_addr,
                    (unsigned)stage_bytes,
                    processed,
                    job->total_bytes,
                    burner_write_path_to_str(job->write_path),
                    should_erase ? "yes" : "no");
            }

            if (should_erase) {
                uint32_t stage_erase_begin = stage_addr;
                uint32_t stage_erase_end = stage_addr + (uint32_t)stage_bytes - 1u;
                uint64_t stage_erase_start_us = 0u;

                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    psram_erase_prefetch_msg,
                    job->rom_name,
                    job->rom_path);

                prefetch_started = false;
                prefetch_inflight = false;
                if (prefetch_done != NULL) {
                    vSemaphoreDelete(prefetch_done);
                    prefetch_done = NULL;
                }

                prefetch_done = xSemaphoreCreateBinary();
                if (prefetch_done != NULL) {
                    prefetch.fp = fp;
                    prefetch.dst = psram_stage_buf;
                    prefetch.bytes = stage_bytes;
                    prefetch.read_len = 0u;
                    prefetch.err = ESP_FAIL;
                    prefetch.done = prefetch_done;
                    if (burner_create_task_with_affinity(
                            burner_tf_prefetch_task,
                            "tf_prefetch",
                            4096,
                            &prefetch,
                            4,
                            NULL,
                            s_burn_core_cfg.tf_core) == pdPASS) {
                        prefetch_inflight = true;
                        prefetch_started = true;
                    } else {
                        vSemaphoreDelete(prefetch_done);
                        prefetch_done = NULL;
                    }
                }

                stage_erase_start_us = burner_gba_diag_now_us();
                burner_status_mark_erase_begin();
                erase_timer_started = true;
                if (use_pipeline_stage) {
                    burner_spi_lock_take();
                    err = burner_gba_sector_erase_prepare_current(stage_addr);
                    burner_spi_lock_give();
                } else {
                    if (processed > 0u) {
                        err = burner_nor_geometry_sector_begin_ceil(
                            &s_cart_ctx.geometry,
                            stage_addr,
                            &stage_erase_begin);
                        if (err != ESP_OK || stage_erase_begin > stage_erase_end) {
                            err = ESP_OK;
                            burner_status_mark_erase_end();
                            erase_timer_started = false;
                            burner_gba_chis_diag_add_erase(burner_gba_diag_now_us() - stage_erase_start_us);
                            goto gba_stage_erase_done;
                        }
                    }
                    err = burner_run_gba_range_erase(
                        stage_erase_begin,
                        stage_erase_end,
                        s_cart_ctx.sector_size,
                        burner_is_gba_multi_card(job),
                        !erase_every_sector,
                        erase_every_sector);
                }
                burner_status_mark_erase_end();
                erase_timer_started = false;
                burner_gba_chis_diag_add_erase(burner_gba_diag_now_us() - stage_erase_start_us);

gba_stage_erase_done:
                if (prefetch_inflight && prefetch_done != NULL) {
                    uint64_t prefetch_wait_start_us = burner_gba_diag_now_us();
                    xSemaphoreTake(prefetch_done, portMAX_DELAY);
                    burner_gba_chis_diag_add_prefetch_wait(burner_gba_diag_now_us() - prefetch_wait_start_us);
                    prefetch_inflight = false;
                }
                if (prefetch_done != NULL) {
                    vSemaphoreDelete(prefetch_done);
                    prefetch_done = NULL;
                }

                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        "erase gba flash failed",
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
                if (prefetch_started) {
                    if (prefetch.err == ESP_OK && prefetch.read_len == stage_bytes) {
                        stage_prefetched = true;
                    } else {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            0,
                            processed,
                            job->total_bytes,
                            "prefetch tf->psram failed",
                            job->rom_name,
                            job->rom_path);
                        err = (prefetch.err != ESP_OK) ? prefetch.err : ESP_FAIL;
                        goto write_gba_done;
                    }
                }
            } else {
                prefetch_started = false;
                prefetch_inflight = false;
                if (prefetch_done != NULL) {
                    vSemaphoreDelete(prefetch_done);
                    prefetch_done = NULL;
                }

                prefetch_done = xSemaphoreCreateBinary();
                if (prefetch_done != NULL) {
                    prefetch.fp = fp;
                    prefetch.dst = psram_stage_buf;
                    prefetch.bytes = stage_bytes;
                    prefetch.read_len = 0u;
                    prefetch.err = ESP_FAIL;
                    prefetch.done = prefetch_done;
                    if (burner_create_task_with_affinity(
                            burner_tf_prefetch_task,
                            "tf_prefetch",
                            4096,
                            &prefetch,
                            4,
                            NULL,
                            s_burn_core_cfg.tf_core) == pdPASS) {
                        prefetch_inflight = true;
                        prefetch_started = true;
                    } else {
                        vSemaphoreDelete(prefetch_done);
                        prefetch_done = NULL;
                    }
                }

                if (prefetch_inflight && prefetch_done != NULL) {
                    uint64_t prefetch_wait_start_us = burner_gba_diag_now_us();
                    xSemaphoreTake(prefetch_done, portMAX_DELAY);
                    burner_gba_chis_diag_add_prefetch_wait(burner_gba_diag_now_us() - prefetch_wait_start_us);
                    prefetch_inflight = false;
                }
                if (prefetch_done != NULL) {
                    vSemaphoreDelete(prefetch_done);
                    prefetch_done = NULL;
                }
                if (prefetch_started) {
                    if (prefetch.err == ESP_OK && prefetch.read_len == stage_bytes) {
                        stage_prefetched = true;
                    } else {
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            0,
                            processed,
                            job->total_bytes,
                            "prefetch tf->psram failed",
                            job->rom_name,
                            job->rom_path);
                        err = (prefetch.err != ESP_OK) ? prefetch.err : ESP_FAIL;
                        goto write_gba_done;
                    }
                }
            }

            if (!stage_prefetched) {
                uint64_t tf_read_start_us;
                uint64_t tf_read_elapsed_us;
                burner_status_update(
                    BURNER_STATE_BURNING,
                    burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed,
                    job->total_bytes,
                    psram_copy_msg,
                    job->rom_name,
                    job->rom_path);

                tf_read_start_us = (uint64_t)esp_timer_get_time();
                err = burner_tf_reader_read(&tf_reader, psram_stage_buf, stage_bytes);
                tf_read_elapsed_us = (uint64_t)esp_timer_get_time() - tf_read_start_us;
                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        job->total_bytes,
                        (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
                if (stage_bytes > 0u && tf_read_elapsed_us > 0u) {
                    burner_status_record_tf_to_psram_copy((uint32_t)stage_bytes, tf_read_elapsed_us);
                    burner_gba_chis_diag_add_tf_read(tf_read_elapsed_us);
                }
            }
            (void)burner_gba_apply_header_checksum_fix(psram_stage_buf, stage_bytes, processed, processed == 0u);

            while (stage_off < stage_bytes) {
                size_t chunk_bytes = stage_bytes - stage_off;
                size_t chunk_limit = burner_gba_program_chunk_limit_bytes();
                uint32_t write_addr = addr_begin + processed + (uint32_t)stage_off;
                uint32_t now_processed;
                uint64_t program_sample_start_us;
                uint64_t program_sample_elapsed_us;
                int progress;

                if (chunk_bytes > chunk_limit) {
                    chunk_bytes = chunk_limit;
                }
                if (burner_gba_should_log_program_boundary(
                        write_addr,
                        chunk_bytes,
                        processed + (uint32_t)stage_off,
                        job->total_bytes)) {
                    ESP_LOGI(
                        BURNER_TAG,
                        "GBA program chunk: addr=0x%08" PRIX32 " bytes=%u processed=%" PRIu32 "/%" PRIu32 " path=%s",
                        write_addr,
                        (unsigned)chunk_bytes,
                        processed + (uint32_t)stage_off,
                        job->total_bytes,
                        burner_write_path_to_str(job->write_path));
                }

                program_sample_start_us = burner_gba_diag_now_us();
                burner_spi_lock_take();
                err = burner_bacon_gba_program_block(
                    psram_stage_buf + stage_off,
                    chunk_bytes,
                    write_addr,
                    burner_is_gba_multi_card(job),
                    should_erase && use_pipeline_stage && !intel_active);
                burner_spi_lock_give();
                program_sample_elapsed_us = burner_gba_diag_now_us() - program_sample_start_us;
                if (err != ESP_OK) {
                    char program_err_msg[96];
                    (void)snprintf(
                        program_err_msg,
                        sizeof(program_err_msg),
                        "program gba failed @0x%08" PRIX32 " (%s)",
                        write_addr,
                        esp_err_to_name(err));
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed + (uint32_t)stage_off,
                        job->total_bytes,
                        program_err_msg,
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
                burner_status_record_write_sample((uint32_t)chunk_bytes, program_sample_elapsed_us);

                stage_off += chunk_bytes;
                now_processed = processed + (uint32_t)stage_off;
                progress = burner_calc_progress_percent_u64(now_processed, job->total_bytes);
                if (progress > 100) {
                    progress = 100;
                }
                burner_status_update(
                    BURNER_STATE_BURNING,
                    progress,
                    now_processed,
                    job->total_bytes,
                    "psram->gba cart programmed",
                    job->rom_name,
                    job->rom_path);
                burner_emit_progress_cb(progress, now_processed);
            }

            processed += (uint32_t)stage_bytes;
            burner_gba_chis_diag_log_stage(stage_addr, stage_bytes, stage_processed_before, processed);
        }
    } else {
        while (processed < job->total_bytes) {
            size_t chunk_bytes = (size_t)(job->total_bytes - processed);
            size_t chunk_limit = burner_gba_program_chunk_limit_bytes();
            uint32_t write_addr = addr_begin + processed;
            uint32_t processed_before = processed;
            uint64_t tf_read_start_us;
            uint64_t tf_read_elapsed_us;
            uint64_t program_sample_start_us;
            uint64_t program_sample_elapsed_us;
            int progress;

            if (chunk_bytes > chunk_limit) {
                chunk_bytes = chunk_limit;
            }
            burner_gba_chis_diag_stage_begin();
            if (burner_gba_should_log_program_boundary(write_addr, chunk_bytes, processed, job->total_bytes)) {
                ESP_LOGI(
                    BURNER_TAG,
                    "GBA program chunk: addr=0x%08" PRIX32 " bytes=%u processed=%" PRIu32 "/%" PRIu32 " path=%s",
                    write_addr,
                    (unsigned)chunk_bytes,
                    processed,
                    job->total_bytes,
                    burner_write_path_to_str(job->write_path));
            }

            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed,
                job->total_bytes,
                "copy tf->ram (64KB chunk)",
                job->rom_name,
                job->rom_path);

            tf_read_start_us = burner_gba_diag_now_us();
            err = burner_tf_reader_read(&tf_reader, buf, chunk_bytes);
            tf_read_elapsed_us = burner_gba_diag_now_us() - tf_read_start_us;
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                    job->rom_name,
                    job->rom_path);
                goto write_gba_done;
            }
            burner_gba_chis_diag_add_tf_read(tf_read_elapsed_us);
            (void)burner_gba_apply_header_checksum_fix(buf, chunk_bytes, processed, processed == 0u);

            program_sample_start_us = burner_gba_diag_now_us();
            burner_spi_lock_take();
            err = burner_bacon_gba_program_block(
                buf,
                chunk_bytes,
                write_addr,
                burner_is_gba_multi_card(job),
                false);
            burner_spi_lock_give();
            program_sample_elapsed_us = burner_gba_diag_now_us() - program_sample_start_us;
            if (err != ESP_OK) {
                char program_err_msg[96];
                (void)snprintf(
                    program_err_msg,
                    sizeof(program_err_msg),
                    "program gba failed @0x%08" PRIX32 " (%s)",
                    write_addr,
                    esp_err_to_name(err));
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    program_err_msg,
                    job->rom_name,
                    job->rom_path);
                goto write_gba_done;
            }
            burner_status_record_write_sample((uint32_t)chunk_bytes, program_sample_elapsed_us);

            processed += (uint32_t)chunk_bytes;
            progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
            if (progress > 100) {
                progress = 100;
            }
            burner_status_update(
                BURNER_STATE_BURNING,
                progress,
                processed,
                job->total_bytes,
                "ram->gba cart programmed",
                job->rom_name,
                job->rom_path);
            burner_emit_progress_cb(progress, processed);
            burner_gba_chis_diag_log_stage(write_addr, chunk_bytes, processed_before, processed);
        }
    }

    if (err == ESP_OK && processed != job->total_bytes) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            processed,
            job->total_bytes,
            "tf file shorter than expected",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        burner_status_update(
            BURNER_STATE_BURNING,
            100,
            processed,
            job->total_bytes,
            "finalizing gba flash state",
            job->rom_name,
            job->rom_path);
        uint64_t finalize_start_us = burner_gba_diag_now_us();
        burner_spi_lock_take();
        err = burner_bacon_gba_finalize_write(burner_is_gba_multi_card(job));
        burner_spi_lock_give();
        burner_gba_chis_diag_add_finalize(burner_gba_diag_now_us() - finalize_start_us);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "final gba flash reset failed",
                job->rom_name,
                job->rom_path);
        }
    }
    if (err == ESP_OK) {
        burner_status_update(
            BURNER_STATE_BURNING,
            100,
            processed,
            job->total_bytes,
            "post-write gba header check",
            job->rom_name,
            job->rom_path);
        uint64_t post_verify_start_us = burner_gba_diag_now_us();
        err = burner_gba_post_write_header_diag(fp, job);
        burner_gba_chis_diag_add_post_verify(burner_gba_diag_now_us() - post_verify_start_us);
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "post-write gba header verify failed",
                job->rom_name,
                job->rom_path);
        }
    }

write_gba_done:
    burner_gba_sector_erase_ctx_reset();
    if (erase_timer_started) {
        burner_status_mark_erase_end();
    }
    if (write_timer_started) {
        burner_status_mark_write_end();
    }
    if (prefetch_inflight && prefetch_done != NULL) {
        uint64_t prefetch_wait_start_us = burner_gba_diag_now_us();
        xSemaphoreTake(prefetch_done, portMAX_DELAY);
        burner_gba_chis_diag_add_prefetch_wait(burner_gba_diag_now_us() - prefetch_wait_start_us);
        prefetch_inflight = false;
    }
    if (prefetch_done != NULL) {
        vSemaphoreDelete(prefetch_done);
    }
    burner_gba_chis_diag_log_summary(err);
    if (tf_reader_started) {
        burner_tf_reader_stop(&tf_reader);
    }
    if (psram_stage_buf != NULL) {
        free(psram_stage_buf);
    }
    if (buf != NULL) {
        free(buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_read_job_gba(const burner_task_param_t *job)
{
    uint32_t addr_begin = 0;
    uint32_t work_total = 0;
    uint32_t dump_chunk_bytes = BURN_GBA_DUMP_CHUNK_BYTES;
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    work_total = job->total_bytes;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addr_begin & 0x1u) != 0u || (work_total & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing gba cart for read",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)work_total) > (uint64_t)s_cart_ctx.device_size) {
        if (addr_begin >= s_cart_ctx.device_size) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                work_total,
                "selected range has no gba cart data",
                job->rom_name,
                job->rom_path);
            return ESP_ERR_INVALID_SIZE;
        }
        work_total = s_cart_ctx.device_size - addr_begin;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            work_total,
            "selected range clamped to gba flash size",
            job->rom_name,
            job->rom_path);
    }

    if (burner_is_supported_dump_chunk_bytes(job->read_chunk_bytes)) {
        dump_chunk_bytes = job->read_chunk_bytes;
    }

    return burner_run_read_job_direct(
        job,
        work_total,
        dump_chunk_bytes,
        burner_dump_read_block_gba,
        "gba cart->tf direct dumping",
        "alloc direct dump buffer failed",
        "read gba cart failed",
        "write dump file failed");
}

static esp_err_t burner_run_verify_rom_job_gba(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *rom_buf = NULL;
    uint8_t *cart_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    uint32_t work_total = 0;
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    work_total = job->total_bytes;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((addr_begin & 0x1u) != 0u || (work_total & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing gba cart for verify",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    if (((uint64_t)addr_begin + (uint64_t)work_total) > (uint64_t)s_cart_ctx.device_size) {
        if (addr_begin >= s_cart_ctx.device_size) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                work_total,
                "selected range has no gba cart data",
                job->rom_name,
                job->rom_path);
            return ESP_ERR_INVALID_SIZE;
        }
        work_total = s_cart_ctx.device_size - addr_begin;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            work_total,
            "selected range clamped to gba flash size",
            job->rom_name,
            job->rom_path);
    }

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            "open verify file failed",
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
    }

    rom_buf = (uint8_t *)malloc(BURN_GBA_DUMP_CHUNK_BYTES);
    cart_buf = (uint8_t *)malloc(BURN_GBA_DUMP_CHUNK_BYTES);
    if (rom_buf == NULL || cart_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            work_total,
            "no memory for gba verify",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto verify_gba_done;
    }

    while (processed < work_total) {
        size_t read_len = (size_t)(work_total - processed);
        int progress;

        if (read_len > BURN_GBA_DUMP_CHUNK_BYTES) {
            read_len = BURN_GBA_DUMP_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                work_total,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        {
            size_t got = fread(rom_buf, 1, read_len, fp);
            if (got != read_len) {
                if (got + 1u == read_len && feof(fp)) {
                    rom_buf[got] = 0x00u;
                } else {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        work_total,
                        "read verify file failed",
                        job->rom_name,
                        job->rom_path);
                    err = ESP_FAIL;
                    break;
                }
            }
        }
        (void)burner_gba_apply_header_checksum_fix(rom_buf, read_len, processed, false);

        burner_spi_lock_take();
        err = burner_bacon_gba_verify_read_block_hoststyle(
            cart_buf,
            read_len,
            addr_begin + processed,
            burner_is_gba_multi_card(job));
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                work_total,
                "read gba cart failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        if (memcmp(rom_buf, cart_buf, read_len) != 0) {
            size_t i;
            char msg[96];

            if (s_cart_ctx.d0d1_swapped) {
                burner_spi_lock_take();
                err = burner_bacon_gba_read_block(
                    cart_buf,
                    read_len,
                    addr_begin + processed,
                    burner_is_gba_multi_card(job));
                burner_spi_lock_give();
                if (err != ESP_OK) {
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed,
                        work_total,
                        "read gba cart failed",
                        job->rom_name,
                        job->rom_path);
                    break;
                }
                if (memcmp(rom_buf, cart_buf, read_len) == 0) {
                    ESP_LOGW(
                        BURNER_TAG,
                        "GBA verify hoststyle mismatch recovered by standard read @0x%08" PRIX32
                        " len=%u swapped=%u",
                        addr_begin + processed,
                        (unsigned)read_len,
                        s_cart_ctx.d0d1_swapped ? 1u : 0u);
                    goto verify_gba_chunk_ok;
                }
            }

            for (i = 0; i < read_len; ++i) {
                if (rom_buf[i] != cart_buf[i]) {
                    uint32_t mismatch_addr = addr_begin + processed + (uint32_t)i;
                    burner_status_set_verify_sample(
                        mismatch_addr,
                        rom_buf[i],
                        cart_buf[i],
                        false);
                    snprintf(
                        msg,
                        sizeof(msg),
                        "verify mismatch @0x%08" PRIX32 " %02X->%02X",
                        mismatch_addr,
                        rom_buf[i],
                        cart_buf[i]);
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, work_total),
                        processed,
                        work_total,
                        msg,
                        job->rom_name,
                        job->rom_path);
                    err = ESP_FAIL;
                    break;
                }
            }
            if (err != ESP_OK) {
                break;
            }
        }

verify_gba_chunk_ok:
        if (read_len > 0u) {
            size_t sample_index = read_len - 1u;
            burner_status_set_verify_sample(
                addr_begin + processed + (uint32_t)sample_index,
                rom_buf[sample_index],
                cart_buf[sample_index],
                true);
        }

        processed += (uint32_t)read_len;
        progress = burner_calc_progress_percent_u64(processed, work_total);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            work_total,
            "gba cart verify running",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

verify_gba_done:
    if (rom_buf != NULL) {
        free(rom_buf);
    }
    if (cart_buf != NULL) {
        free(cart_buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_erase_rom_job_gba(const burner_task_param_t *job)
{
    esp_err_t err;

    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        1u,
        "probing gba cart for chip erase",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "gba cart prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }

    if (usb_msc_tf_in_use_by_host()) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "tf busy by usb host",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_STATE;
    }
    if (!burner_gba_gbx_is_active() &&
        s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL && !s_cart_ctx.probe_cfi_ok) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "intel gba chip erase requires valid cfi",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        1u,
        "gba chip erase running",
        job->rom_name,
        job->rom_path);
    burner_status_mark_erase_begin();

    err = burner_run_gba_chip_erase();
    burner_status_mark_erase_end();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "gba chip erase failed",
            job->rom_name,
            job->rom_path);
        return err;
    }

    burner_emit_progress_cb(100, 1u);
    return ESP_OK;
}

/* GBA ROM burn job implementations. */


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

    fp = burner_file_open_read(job->rom_path);
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
    burner_tf_reader_set_source_size(
        job->gba_patch_plan ? job->gba_patch_plan->source_size : job->total_bytes);

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
        burner_status_set_chip_erase_ui_active(true);
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
        burner_apply_write_transform(job, sector_buf, sector_bytes, processed);

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

            burner_spi_lock_take();
            burner_status_mark_write_begin();
            err = burner_gba_gbx_program_block(sector_buf, sector_bytes, sector_addr, is_multi_card, false);
            program_elapsed_us = burner_status_mark_write_end();
            burner_spi_lock_give();
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

gbx_write_done:
    burner_tf_reader_set_source_size(0u);
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
    if (!job || !job->total_bytes || (uint64_t)job->addr_begin + job->total_bytes > UINT32_MAX)
        return ESP_ERR_INVALID_ARG;
    burner_spi_lock_take();
    esp_err_t err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) return err;
    if ((uint64_t)job->addr_begin + job->total_bytes > s_cart_ctx.device_size) return ESP_ERR_INVALID_SIZE;
    burner_gba_chis_diag_begin(job);
    burner_gba_chis_diag_set_intel(burner_gba_nor_is_intel_active());
    if (burner_gba_gbx_is_active()) return burner_run_write_job_gba_gbx(job);
    if (burner_gba_nor_is_intel_active() && !s_cart_ctx.probe_cfi_ok) return ESP_ERR_NOT_SUPPORTED;
    burner_gba_sector_erase_ctx_reset();
    burner_spi_lock_take();
    burner_gba_check_poll_pair();
    burner_spi_lock_give();
    err = burner_write_sector_pipeline(job);
    burner_gba_chis_diag_log_summary(err);
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
    if (!job || !job->total_bytes ||
        (uint64_t)job->addr_begin + job->total_bytes > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    if (job->addr_begin & 1u) return ESP_ERR_INVALID_ARG;
    burner_status_update(BURNER_STATE_BURNING, 0, 0, job->total_bytes,
                         "probing cart for verify", job->rom_name, job->rom_path);
    burner_spi_lock_take();
    esp_err_t err = burner_spi_prepare_burn_gba(job);
    burner_spi_lock_give();
    if (err != ESP_OK) return err;
    if ((uint64_t)job->addr_begin + job->total_bytes > s_cart_ctx.device_size) {
        burner_status_update(BURNER_STATE_ERROR, 0, 0, job->total_bytes,
                             "verify file exceeds flash size", job->rom_name, job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }
    return burner_verify_stream(job, burner_dump_read_block_gba);
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
    burner_status_set_chip_erase_ui_active(true);
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

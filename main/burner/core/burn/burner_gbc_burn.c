/* MBC5/GBC ROM and RAM burn job implementations. */

static esp_err_t burner_run_write_job_mbc5(const burner_task_param_t *job)
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
    bool force_erase_sectors = true;
    size_t stage_capacity = 0;
    size_t program_chunk_bytes = BURN_MBC5_PROGRAM_CHUNK_BYTES;
    burner_nor_region_cursor_t pipeline_cursor = {0};
    burner_tf_prefetch_ctx_t prefetch = {0};
    burner_tf_reader_ctx_t tf_reader = {0};
    SemaphoreHandle_t prefetch_done = NULL;
    bool prefetch_inflight = false;
    bool prefetch_started = false;
    bool tf_reader_started = false;
    bool erase_timer_started = false;
    bool write_timer_started = false;
    uint32_t psram_window_mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    uint32_t psram_window_bytes = BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES;
    char direct_copy_msg[64] = {0};
    char direct_program_msg[64] = {0};
    char psram_program_msg[64] = {0};
    char psram_alloc_fail_msg[96] = {0};
    char psram_erase_prefetch_msg[96] = {0};
    char psram_copy_msg[64] = {0};

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    use_psram_stage = (job->write_path == BURNER_WRITE_PATH_PSRAM ||
                       job->write_path == BURNER_WRITE_PATH_PIPELINE);
    use_pipeline_stage = (job->write_path == BURNER_WRITE_PATH_PIPELINE);
    program_chunk_bytes = (size_t)burner_clamp_mbc5_program_chunk_bytes(job->mbc5_program_chunk_bytes);
    if (use_pipeline_stage) {
        psram_window_mb = BURN_PSRAM_WINDOW_AUTO_MB;
        psram_window_bytes = 0u;
    } else {
        psram_window_mb = burner_psram_window_bytes_to_mb(job->psram_window_bytes);
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    }
    (void)snprintf(
        direct_copy_msg,
        sizeof(direct_copy_msg),
        "direct path: tf->ram (%uKB chunk)",
        (unsigned)(program_chunk_bytes / 1024u));
    (void)snprintf(
        direct_program_msg,
        sizeof(direct_program_msg),
        "direct path: ram->cart (%uKB chunk)",
        (unsigned)(program_chunk_bytes / 1024u));
    (void)snprintf(
        psram_program_msg,
        sizeof(psram_program_msg),
        "%s->cart (%uKB chunk)",
        use_pipeline_stage ? "pipeline psram" : "psram",
        (unsigned)(program_chunk_bytes / 1024u));
    (void)snprintf(
        psram_alloc_fail_msg,
        sizeof(psram_alloc_fail_msg),
        use_pipeline_stage ? "alloc pipeline psram staging failed" : "alloc %uMB psram staging failed",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_erase_prefetch_msg,
        sizeof(psram_erase_prefetch_msg),
        use_pipeline_stage ? "pipeline erase sector + prefetch tf->psram" : "erasing flash sectors (%uMB) + prefetch tf->psram",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_copy_msg,
        sizeof(psram_copy_msg),
        use_pipeline_stage ? "pipeline copy tf->psram (sector window)" : "copy tf->psram (%uMB window)",
        (unsigned)psram_window_mb);
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing cart and preparing flash",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart prepare failed",
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
            "selected range exceeds flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
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
        return ESP_FAIL;
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
        fclose(fp);
        return err;
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
                    "pipeline sector geometry unavailable",
                    job->rom_name,
                    job->rom_path);
                goto write_done;
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
            goto write_done;
        }
    } else {
        buf = (uint8_t *)malloc(program_chunk_bytes);
        if (buf == NULL) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "no memory for write chunk",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_NO_MEM;
            goto write_done;
        }
    }

    force_erase_sectors = job->erase_always;
    if (force_erase_sectors) {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 burn erase policy: force erase for all write paths");
    } else {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 burn erase policy: smart sector-sampled erase");
    }
    if (should_erase && !burner_nor_geometry_is_valid(&s_cart_ctx.geometry)) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "flash sector geometry unavailable",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_INVALID_SIZE;
        goto write_done;
    }
    if (should_erase) {
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
                "pipeline sector cursor unavailable",
                job->rom_name,
                job->rom_path);
            goto write_done;
        }
    }

    if (should_erase && !use_psram_stage) {
        burner_status_mark_erase_begin();
        erase_timer_started = true;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "erasing flash sectors",
            job->rom_name,
            job->rom_path);

        err = burner_run_mbc5_range_erase(
            addr_begin,
            addr_begin + job->total_bytes - 1u,
            s_cart_ctx.sector_size,
            true,
            force_erase_sectors);
        burner_status_mark_erase_end();
        erase_timer_started = false;

        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "erase flash failed",
                job->rom_name,
                job->rom_path);
            goto write_done;
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
                        "pipeline sector window invalid",
                        job->rom_name,
                        job->rom_path);
                    goto write_done;
                }
                stage_bytes = (size_t)pipeline_stage_bytes;
            } else if (stage_bytes > stage_capacity) {
                stage_bytes = stage_capacity;
            }

            if (should_erase) {
                uint32_t stage_erase_begin = stage_addr;
                uint32_t stage_erase_end = stage_addr + (uint32_t)stage_bytes - 1u;

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

                burner_status_mark_erase_begin();
                erase_timer_started = true;
                if (processed > 0u && !use_pipeline_stage) {
                    err = burner_nor_geometry_sector_begin_ceil(&s_cart_ctx.geometry, stage_addr, &stage_erase_begin);
                    if (err != ESP_OK || stage_erase_begin > stage_erase_end) {
                        err = ESP_OK;
                        burner_status_mark_erase_end();
                        erase_timer_started = false;
                        goto mbc5_stage_erase_done;
                    }
                }
                err = burner_run_mbc5_range_erase(
                    stage_erase_begin,
                    stage_erase_end,
                    s_cart_ctx.sector_size,
                    true,
                    force_erase_sectors);
                burner_status_mark_erase_end();
                erase_timer_started = false;

mbc5_stage_erase_done:
                if (prefetch_inflight && prefetch_done != NULL) {
                    xSemaphoreTake(prefetch_done, portMAX_DELAY);
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
                        "erase flash failed",
                        job->rom_name,
                        job->rom_path);
                    goto write_done;
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
                        goto write_done;
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
                    goto write_done;
                }
                if (stage_bytes > 0u && tf_read_elapsed_us > 0u) {
                    burner_status_record_tf_to_psram_copy((uint32_t)stage_bytes, tf_read_elapsed_us);
                }
            }

            while (stage_off < stage_bytes) {
                size_t chunk_bytes = stage_bytes - stage_off;
                uint32_t now_processed;
                uint64_t program_sample_start_us;
                uint64_t program_sample_elapsed_us;
                int progress;

                if (chunk_bytes > program_chunk_bytes) {
                    chunk_bytes = program_chunk_bytes;
                }

                program_sample_start_us = (uint64_t)esp_timer_get_time();
                burner_spi_lock_take();
                err = burner_bacon_mbc5_program_block(
                    psram_stage_buf + stage_off,
                    chunk_bytes,
                    addr_begin + processed + (uint32_t)stage_off);
                burner_spi_lock_give();
                program_sample_elapsed_us = (uint64_t)esp_timer_get_time() - program_sample_start_us;
                if (err != ESP_OK) {
                    char program_err_msg[96];
                    (void)snprintf(
                        program_err_msg,
                        sizeof(program_err_msg),
                        "program cart failed @0x%08" PRIX32 " (%s)",
                        addr_begin + processed + (uint32_t)stage_off,
                        esp_err_to_name(err));
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        0,
                        processed + (uint32_t)stage_off,
                        job->total_bytes,
                        program_err_msg,
                        job->rom_name,
                        job->rom_path);
                    goto write_done;
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
                    psram_program_msg,
                    job->rom_name,
                    job->rom_path);
                burner_emit_progress_cb(progress, now_processed);
            }

            processed += (uint32_t)stage_bytes;
        }
    } else {
        while (processed < job->total_bytes) {
            size_t chunk_bytes = (size_t)(job->total_bytes - processed);
            uint64_t program_sample_start_us;
            uint64_t program_sample_elapsed_us;
            int progress;

            if (chunk_bytes > program_chunk_bytes) {
                chunk_bytes = program_chunk_bytes;
            }

            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed,
                job->total_bytes,
                direct_copy_msg,
                job->rom_name,
                job->rom_path);

            err = burner_tf_reader_read(&tf_reader, buf, chunk_bytes);
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read tf failed",
                    job->rom_name,
                    job->rom_path);
                goto write_done;
            }

            program_sample_start_us = (uint64_t)esp_timer_get_time();
            burner_spi_lock_take();
            err = burner_bacon_mbc5_program_block(buf, chunk_bytes, addr_begin + processed);
            burner_spi_lock_give();
            program_sample_elapsed_us = (uint64_t)esp_timer_get_time() - program_sample_start_us;
            if (err != ESP_OK) {
                char program_err_msg[96];
                (void)snprintf(
                    program_err_msg,
                    sizeof(program_err_msg),
                    "program cart failed @0x%08" PRIX32 " (%s)",
                    addr_begin + processed,
                    esp_err_to_name(err));
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed,
                    job->total_bytes,
                    program_err_msg,
                    job->rom_name,
                    job->rom_path);
                goto write_done;
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
                direct_program_msg,
                job->rom_name,
                job->rom_path);
            burner_emit_progress_cb(progress, processed);
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

write_done:
    if (erase_timer_started) {
        burner_status_mark_erase_end();
    }
    if (write_timer_started) {
        burner_status_mark_write_end();
    }
    if (prefetch_inflight && prefetch_done != NULL) {
        xSemaphoreTake(prefetch_done, portMAX_DELAY);
        prefetch_inflight = false;
    }
    if (prefetch_done != NULL) {
        vSemaphoreDelete(prefetch_done);
    }
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

static esp_err_t burner_run_read_job_mbc5(const burner_task_param_t *job)
{
    uint32_t addr_begin = 0;
    uint32_t dump_chunk_bytes = BURN_MBC5_DUMP_CHUNK_BYTES;
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing cart for read",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart prepare failed",
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
            "selected range exceeds flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }

    if (burner_is_supported_dump_chunk_bytes(job->read_chunk_bytes)) {
        dump_chunk_bytes = job->read_chunk_bytes;
    }

    return burner_run_read_job_direct(
        job,
        job->total_bytes,
        dump_chunk_bytes,
        burner_dump_read_block_mbc5,
        "cart->tf direct dumping",
        "alloc direct dump buffer failed",
        "read cart failed",
        "write dump file failed");
}

static esp_err_t burner_run_verify_rom_job_mbc5(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    FILE *verify_log_fp = NULL;
    uint8_t *psram_stage_buf = NULL;
    uint8_t *cart_buf = NULL;
    burner_tf_reader_ctx_t tf_reader = {0};
    bool tf_reader_started = false;
    bool verify_log_open_attempted = false;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    uint32_t psram_window_mb = BURN_VERIFY_PSRAM_WINDOW_MB;
    uint32_t psram_window_bytes = BURN_VERIFY_PSRAM_WINDOW_BYTES;
    size_t stage_capacity = 0u;
    char psram_copy_msg[64] = {0};
    char psram_verify_msg[64] = {0};
    char psram_alloc_fail_msg[96] = {0};
    char verify_log_rel[TF_PATH_LEN_MAX] = {0};
    esp_err_t err = ESP_OK;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    psram_window_mb = burner_psram_window_bytes_to_mb(job->psram_window_bytes);
    psram_window_bytes = burner_psram_window_mb_to_bytes(psram_window_mb);
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(
        psram_copy_msg,
        sizeof(psram_copy_msg),
        "copy tf->psram (%uMB verify window)",
        (unsigned)psram_window_mb);
    (void)snprintf(
        psram_verify_msg,
        sizeof(psram_verify_msg),
        "psram->cart verify (%uKB chunk)",
        (unsigned)(BURN_MBC5_DUMP_CHUNK_BYTES / 1024u));
    (void)snprintf(
        psram_alloc_fail_msg,
        sizeof(psram_alloc_fail_msg),
        "alloc %uMB psram verify staging failed",
        (unsigned)psram_window_mb);

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "probing cart for verify",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart prepare failed",
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
            "selected range exceeds flash size",
            job->rom_name,
            job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open verify file failed",
            job->rom_name,
            job->rom_path);
        return ESP_FAIL;
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
        fclose(fp);
        return err;
    }
    tf_reader_started = true;

    stage_capacity = (job->total_bytes < psram_window_bytes)
                         ? (size_t)job->total_bytes
                         : (size_t)psram_window_bytes;
    psram_stage_buf = (uint8_t *)heap_caps_malloc(
        stage_capacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cart_buf = (uint8_t *)malloc(BURN_MBC5_DUMP_CHUNK_BYTES);
    if (psram_stage_buf == NULL || cart_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            (psram_stage_buf == NULL) ? psram_alloc_fail_msg : "no memory for verify cart chunk",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto verify_done;
    }

    while (processed < job->total_bytes) {
        size_t stage_bytes = (size_t)(job->total_bytes - processed);
        size_t stage_off = 0u;
        uint64_t tf_read_start_us;
        uint64_t tf_read_elapsed_us;

        if (stage_bytes > stage_capacity) {
            stage_bytes = stage_capacity;
        }

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
                (err == ESP_ERR_INVALID_STATE) ? "tf busy by usb host" : "read verify file failed",
                job->rom_name,
                job->rom_path);
            break;
        }
        if (stage_bytes > 0u && tf_read_elapsed_us > 0u) {
            burner_status_record_tf_to_psram_copy((uint32_t)stage_bytes, tf_read_elapsed_us);
        }

        while (stage_off < stage_bytes) {
            size_t verify_bytes = stage_bytes - stage_off;
            uint32_t chunk_addr = addr_begin + processed + (uint32_t)stage_off;
            uint32_t now_processed;
            int progress;

            if (verify_bytes > BURN_MBC5_DUMP_CHUNK_BYTES) {
                verify_bytes = BURN_MBC5_DUMP_CHUNK_BYTES;
            }

            burner_status_update(
                BURNER_STATE_BURNING,
                burner_calc_progress_percent_u64(processed + (uint32_t)stage_off, job->total_bytes),
                processed + (uint32_t)stage_off,
                job->total_bytes,
                psram_verify_msg,
                job->rom_name,
                job->rom_path);

            burner_spi_lock_take();
            err = burner_bacon_mbc5_read_block_hoststyle(cart_buf, verify_bytes, chunk_addr);
            burner_spi_lock_give();
            if (err != ESP_OK) {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    0,
                    processed + (uint32_t)stage_off,
                    job->total_bytes,
                    "read cart failed",
                    job->rom_name,
                    job->rom_path);
                goto verify_done;
            }

            if (memcmp(psram_stage_buf + stage_off, cart_buf, verify_bytes) != 0) {
                size_t i;

                for (i = 0u; i < verify_bytes; ++i) {
                    uint8_t rom_byte = psram_stage_buf[stage_off + i];
                    uint8_t cart_byte = cart_buf[i];

                    if (rom_byte != cart_byte) {
                        uint32_t mismatch_addr = chunk_addr + (uint32_t)i;
                        uint32_t mismatch_processed = mismatch_addr - addr_begin;
                        int mismatch_progress = burner_calc_progress_percent_u64(
                            mismatch_processed,
                            job->total_bytes);
                        char final_msg[96] = {0};

                        if (!verify_log_open_attempted) {
                            verify_log_fp = burner_open_mbc5_verify_log(
                                job,
                                verify_log_rel,
                                sizeof(verify_log_rel));
                            verify_log_open_attempted = true;
                        }
                        if (verify_log_fp != NULL) {
                            (void)fprintf(
                                verify_log_fp,
                                "0x%08" PRIX32 " %02X->%02X\n",
                                mismatch_addr,
                                rom_byte,
                                cart_byte);
                        }
                        burner_status_set_verify_sample(
                            mismatch_addr,
                            rom_byte,
                            cart_byte,
                            false);
                        if (verify_log_fp != NULL) {
                            const char *log_name = strrchr(verify_log_rel, '/');
                            log_name = (log_name != NULL) ? (log_name + 1) : verify_log_rel;
                            fflush(verify_log_fp);
                            (void)snprintf(
                                final_msg,
                                sizeof(final_msg),
                                "verify mismatch @0x%08" PRIX32 " %02X->%02X log=%.32s",
                                mismatch_addr,
                                rom_byte,
                                cart_byte,
                                log_name);
                        } else {
                            (void)snprintf(
                                final_msg,
                                sizeof(final_msg),
                                "verify mismatch @0x%08" PRIX32 " %02X->%02X",
                                mismatch_addr,
                                rom_byte,
                                cart_byte);
                        }
                        if (mismatch_progress > 100) {
                            mismatch_progress = 100;
                        }
                        burner_status_update(
                            BURNER_STATE_ERROR,
                            mismatch_progress,
                            mismatch_processed,
                            job->total_bytes,
                            final_msg,
                            job->rom_name,
                            job->rom_path);
                        err = ESP_FAIL;
                        goto verify_done;
                    }
                }
            }

            if (verify_bytes > 0u) {
                size_t sample_index = verify_bytes - 1u;
                burner_status_set_verify_sample(
                    chunk_addr + (uint32_t)sample_index,
                    psram_stage_buf[stage_off + sample_index],
                    cart_buf[sample_index],
                    true);
            }

            stage_off += verify_bytes;
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
                "psram->cart verify running",
                job->rom_name,
                job->rom_path);
            burner_emit_progress_cb(progress, now_processed);
        }

        processed += (uint32_t)stage_bytes;
    }

verify_done:
    if (tf_reader_started) {
        burner_tf_reader_stop(&tf_reader);
    }
    if (verify_log_fp != NULL) {
        fclose(verify_log_fp);
    }
    if (psram_stage_buf != NULL) {
        free(psram_stage_buf);
    }
    if (cart_buf != NULL) {
        free(cart_buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

static esp_err_t burner_run_erase_rom_job_mbc5(const burner_task_param_t *job)
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
        "probing cart for chip erase",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "cart prepare failed",
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

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        1u,
        "chip erase running",
        job->rom_name,
        job->rom_path);
    burner_status_set_chip_erase_ui_active(true);
    burner_status_mark_erase_begin();

    err = burner_run_mbc5_chip_erase();
    burner_status_mark_erase_end();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "chip erase failed",
            job->rom_name,
            job->rom_path);
        return err;
    }

    burner_emit_progress_cb(100, 1u);
    return ESP_OK;
}

esp_err_t burner_run_write_ram_job(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool ram_enabled = false;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "preparing cart ram write",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_ram();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart ram prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    ram_enabled = true;

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open sav file failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto write_ram_done;
    }

    buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    if (buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "no memory for ram write",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto write_ram_done;
    }

    while (processed < job->total_bytes) {
        size_t chunk = (size_t)(job->total_bytes - processed);
        int progress;

        if (chunk > BURN_MBC5_RAM_CHUNK_BYTES) {
            chunk = BURN_MBC5_RAM_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        if (fread(buf, 1, chunk, fp) != chunk) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read sav file failed",
                job->rom_name,
                job->rom_path);
            err = ESP_FAIL;
            break;
        }

        burner_spi_lock_take();
        err = burner_bacon_mbc5_ram_write_block(
            buf,
            chunk,
            addr_begin + processed,
            job->ram_fram,
            job->ram_latency);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "write cart ram failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        processed += (uint32_t)chunk;
        progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            job->total_bytes,
            "sav->cart ram writing",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

write_ram_done:
    if (ram_enabled) {
        burner_spi_lock_take();
        (void)burner_bacon_mbc5_ram_enable(false);
        burner_spi_lock_give();
    }
    if (buf != NULL) {
        free(buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

esp_err_t burner_run_read_ram_job(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    uint8_t *file_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool ram_enabled = false;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "preparing cart ram dump",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_ram();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart ram prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    ram_enabled = true;

    fp = fopen(job->rom_path, "wb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open ram dump file failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto read_ram_done;
    }

    file_buf = burner_attach_stdio_buffer(fp, BURN_TF_STDIO_BUFFER_BYTES);

    buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    if (buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "no memory for ram dump",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto read_ram_done;
    }

    while (processed < job->total_bytes) {
        size_t chunk = (size_t)(job->total_bytes - processed);
        int progress;

        if (chunk > BURN_MBC5_RAM_CHUNK_BYTES) {
            chunk = BURN_MBC5_RAM_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        burner_spi_lock_take();
        err = burner_bacon_mbc5_ram_read_block(
            buf,
            chunk,
            addr_begin + processed,
            job->ram_fram,
            job->ram_latency);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read cart ram failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        if (fwrite(buf, 1, chunk, fp) != chunk) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "write ram dump failed",
                job->rom_name,
                job->rom_path);
            err = ESP_FAIL;
            break;
        }

        processed += (uint32_t)chunk;
        progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            job->total_bytes,
            "cart ram->tf dumping",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

read_ram_done:
    if (ram_enabled) {
        burner_spi_lock_take();
        (void)burner_bacon_mbc5_ram_enable(false);
        burner_spi_lock_give();
    }
    if (buf != NULL) {
        free(buf);
    }
    if (fp != NULL) {
        if (fclose(fp) != 0 && err == ESP_OK) {
            err = ESP_FAIL;
        }
    }
    if (file_buf != NULL) {
        free(file_buf);
    }
    if (err != ESP_OK) {
        unlink(job->rom_path);
    } else {
        (void)burner_apply_current_file_mtime(job->rom_path, NULL);
    }
    return err;
}

esp_err_t burner_run_verify_ram_job(const burner_task_param_t *job)
{
    FILE *fp = NULL;
    uint8_t *sav_buf = NULL;
    uint8_t *cart_buf = NULL;
    uint32_t processed = 0;
    uint32_t addr_begin = 0;
    esp_err_t err = ESP_OK;
    bool ram_enabled = false;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    addr_begin = job->addr_begin;
    if (addr_begin > (UINT32_MAX - (job->total_bytes - 1u))) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        "preparing cart ram verify",
        job->rom_name,
        job->rom_path);

    burner_spi_lock_take();
    err = burner_spi_prepare_ram();
    burner_spi_lock_give();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "cart ram prepare failed",
            job->rom_name,
            job->rom_path);
        return err;
    }
    ram_enabled = true;

    fp = fopen(job->rom_path, "rb");
    if (fp == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "open sav verify file failed",
            job->rom_name,
            job->rom_path);
        err = ESP_FAIL;
        goto verify_ram_done;
    }

    sav_buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    cart_buf = (uint8_t *)malloc(BURN_MBC5_RAM_CHUNK_BYTES);
    if (sav_buf == NULL || cart_buf == NULL) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "no memory for ram verify",
            job->rom_name,
            job->rom_path);
        err = ESP_ERR_NO_MEM;
        goto verify_ram_done;
    }

    while (processed < job->total_bytes) {
        size_t chunk = (size_t)(job->total_bytes - processed);
        int progress;

        if (chunk > BURN_MBC5_RAM_CHUNK_BYTES) {
            chunk = BURN_MBC5_RAM_CHUNK_BYTES;
        }

        if (usb_msc_tf_in_use_by_host()) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "tf busy by usb host",
                job->rom_name,
                job->rom_path);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        if (fread(sav_buf, 1, chunk, fp) != chunk) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read sav verify file failed",
                job->rom_name,
                job->rom_path);
            err = ESP_FAIL;
            break;
        }

        burner_spi_lock_take();
        err = burner_bacon_mbc5_ram_read_block(
            cart_buf,
            chunk,
            addr_begin + processed,
            job->ram_fram,
            job->ram_latency);
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                processed,
                job->total_bytes,
                "read cart ram failed",
                job->rom_name,
                job->rom_path);
            break;
        }

        if (memcmp(sav_buf, cart_buf, chunk) != 0) {
            size_t i;
            char msg[96];

            for (i = 0; i < chunk; ++i) {
                if (sav_buf[i] != cart_buf[i]) {
                    uint32_t mismatch_addr = addr_begin + processed + (uint32_t)i;
                    burner_status_set_verify_sample(
                        mismatch_addr,
                        sav_buf[i],
                        cart_buf[i],
                        false);
                    snprintf(
                        msg,
                        sizeof(msg),
                        "ram mismatch @0x%08" PRIX32 " %02X->%02X",
                        mismatch_addr,
                        sav_buf[i],
                        cart_buf[i]);
                    burner_status_update(
                        BURNER_STATE_ERROR,
                        burner_calc_progress_percent_u64(processed, job->total_bytes),
                        processed,
                        job->total_bytes,
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

        if (chunk > 0u) {
            size_t sample_index = chunk - 1u;
            burner_status_set_verify_sample(
                addr_begin + processed + (uint32_t)sample_index,
                sav_buf[sample_index],
                cart_buf[sample_index],
                true);
        }

        processed += (uint32_t)chunk;
        progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_BURNING,
            progress,
            processed,
            job->total_bytes,
            "cart ram verify running",
            job->rom_name,
            job->rom_path);
        burner_emit_progress_cb(progress, processed);
    }

verify_ram_done:
    if (ram_enabled) {
        burner_spi_lock_take();
        (void)burner_bacon_mbc5_ram_enable(false);
        burner_spi_lock_give();
    }
    if (sav_buf != NULL) {
        free(sav_buf);
    }
    if (cart_buf != NULL) {
        free(cart_buf);
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return err;
}

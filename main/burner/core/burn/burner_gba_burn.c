/* GBA ROM burn job implementations. */

static bool burner_gba_probe_buffer_all_ff(const uint8_t *buf, size_t len)
{
    if (buf == NULL) {
        return false;
    }

    for (size_t i = 0u; i < len; ++i) {
        if (buf[i] != 0xFFu) {
            return false;
        }
    }
    return true;
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
    bool force_erase_sectors = true;
    size_t stage_capacity = 0;
    size_t probe_len = 0u;
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
    uint8_t probe_buf[BURN_ERASE_PROBE_BYTES];
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
    if (force_erase_sectors) {
        should_erase = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBA burn erase policy: force erase for all write paths");
    } else {
        probe_len = (job->total_bytes < BURN_ERASE_PROBE_BYTES) ? (size_t)job->total_bytes : BURN_ERASE_PROBE_BYTES;
        burner_status_update(
            BURNER_STATE_BURNING,
            0,
            0,
            job->total_bytes,
            "checking gba flash blank state",
            job->rom_name,
            job->rom_path);

        burner_spi_lock_take();
        err = burner_bacon_gba_read_block(probe_buf, probe_len, addr_begin, burner_is_gba_multi_card(job));
        burner_spi_lock_give();
        if (err != ESP_OK) {
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                0,
                job->total_bytes,
                "read gba flash probe failed",
                job->rom_name,
                job->rom_path);
            goto write_gba_done;
        }

        should_erase = !burner_gba_probe_buffer_all_ff(probe_buf, probe_len);
        ESP_LOGI(
            BURNER_TAG,
            "GBA burn erase policy: smart head-%uB erase=%s",
            (unsigned)probe_len,
            should_erase ? "yes" : "skip");
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
            force_erase_sectors);
    }
    if (should_erase && !use_psram_stage) {
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

        err = burner_run_gba_range_erase(
            addr_begin,
            addr_begin + job->total_bytes - 1u,
            s_cart_ctx.sector_size,
            burner_is_gba_multi_card(job),
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
                "erase gba flash failed",
                job->rom_name,
                job->rom_path);
            goto write_gba_done;
        }
    }

    burner_status_mark_write_begin();
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
                        "gba pipeline sector window invalid",
                        job->rom_name,
                        job->rom_path);
                    goto write_gba_done;
                }
                stage_bytes = (size_t)pipeline_stage_bytes;
            } else if (stage_bytes > stage_capacity) {
                stage_bytes = stage_capacity;
            }
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
                            goto gba_stage_erase_done;
                        }
                    }
                    err = burner_run_gba_range_erase(
                        stage_erase_begin,
                        stage_erase_end,
                        s_cart_ctx.sector_size,
                        burner_is_gba_multi_card(job),
                        true,
                        force_erase_sectors);
                }
                burner_status_mark_erase_end();
                erase_timer_started = false;

gba_stage_erase_done:
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
                    xSemaphoreTake(prefetch_done, portMAX_DELAY);
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
                }
            }

            while (stage_off < stage_bytes) {
                size_t chunk_bytes = stage_bytes - stage_off;
                uint32_t write_addr = addr_begin + processed + (uint32_t)stage_off;
                uint32_t now_processed;
                int progress;

                if (chunk_bytes > BURN_GBA_PROGRAM_CHUNK_BYTES) {
                    chunk_bytes = BURN_GBA_PROGRAM_CHUNK_BYTES;
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

                burner_spi_lock_take();
                err = burner_bacon_gba_program_block(
                    psram_stage_buf + stage_off,
                    chunk_bytes,
                    write_addr,
                    burner_is_gba_multi_card(job),
                    use_pipeline_stage && should_erase && !burner_gba_nor_is_intel_active());
                burner_spi_lock_give();
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
        }
    } else {
        while (processed < job->total_bytes) {
            size_t chunk_bytes = (size_t)(job->total_bytes - processed);
            uint32_t write_addr = addr_begin + processed;
            int progress;

            if (chunk_bytes > BURN_GBA_PROGRAM_CHUNK_BYTES) {
                chunk_bytes = BURN_GBA_PROGRAM_CHUNK_BYTES;
            }
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
                goto write_gba_done;
            }

            burner_spi_lock_take();
            err = burner_bacon_gba_program_block(
                buf,
                chunk_bytes,
                write_addr,
                burner_is_gba_multi_card(job),
                false);
            burner_spi_lock_give();
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

write_gba_done:
    burner_gba_sector_erase_ctx_reset();
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

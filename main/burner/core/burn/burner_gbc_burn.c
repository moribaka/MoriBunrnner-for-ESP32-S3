/* MBC5/GBC ROM and RAM burn job implementations. */

static esp_err_t burner_run_write_job_mbc5(const burner_task_param_t *job)
{
    if (!job || !job->total_bytes || (uint64_t)job->addr_begin + job->total_bytes > UINT32_MAX)
        return ESP_ERR_INVALID_ARG;
    burner_spi_lock_take();
    esp_err_t err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) return err;
    if ((uint64_t)job->addr_begin + job->total_bytes > s_cart_ctx.device_size) return ESP_ERR_INVALID_SIZE;
    return burner_write_sector_pipeline(job);
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
    if (!job || !job->total_bytes ||
        (uint64_t)job->addr_begin + job->total_bytes > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    burner_status_update(BURNER_STATE_BURNING, 0, 0, job->total_bytes,
                         "probing cart for verify", job->rom_name, job->rom_path);
    burner_spi_lock_take();
    esp_err_t err = burner_spi_prepare_burn_mbc5(job);
    burner_spi_lock_give();
    if (err != ESP_OK) return err;
    if ((uint64_t)job->addr_begin + job->total_bytes > s_cart_ctx.device_size) {
        burner_status_update(BURNER_STATE_ERROR, 0, 0, job->total_bytes,
                             "verify file exceeds flash size", job->rom_name, job->rom_path);
        return ESP_ERR_INVALID_SIZE;
    }
    return burner_verify_stream(job, burner_dump_read_block_mbc5);
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

    fp = burner_file_open_read(job->rom_path);
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

    fp = burner_file_open_read(job->rom_path);
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

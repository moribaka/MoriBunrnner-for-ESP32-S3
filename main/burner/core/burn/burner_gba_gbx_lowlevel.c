typedef struct {
    uint32_t sector_addr;
    uint32_t program_addr;
    uint16_t program_data;
    uint16_t last_program_data;
    uint16_t buffer_size_value;
} burner_gba_gbx_exec_ctx_t;

bool burner_gba_gbx_is_active(void)
{
    return s_cart_ctx.gbx.active && s_cart_ctx.gbx.runtime_commands_enabled;
}

static bool burner_gba_gbx_profile_runtime_supported(
    const burner_gbx_profile_t *profile,
    const burner_task_param_t *job,
    uint16_t buffer_write_bytes)
{
    bool can_program;

    if (profile == NULL || job == NULL || !profile->active) {
        return false;
    }
    if (profile->type[0] != '\0' && strcasecmp(profile->type, "AGB") != 0) {
        return false;
    }

    can_program = profile->single_write.count > 0u ||
                  (profile->buffer_write.count > 0u && buffer_write_bytes >= 2u);

    switch (job->mode) {
    case BURNER_JOB_WRITE_ROM:
        return can_program && profile->sector_erase.count > 0u;
    case BURNER_JOB_ERASE_ROM:
        return profile->chip_erase.count > 0u;
    default:
        return false;
    }
}

static uint16_t burner_gba_gbx_program_buffer_bytes(
    const burner_gbx_profile_t *profile,
    uint16_t buffer_write_bytes)
{
    if (profile == NULL || profile->buffer_write.count == 0u || buffer_write_bytes < 2u) {
        return 0u;
    }
    return (uint16_t)(buffer_write_bytes & (uint16_t)~0x1u);
}

static esp_err_t burner_gba_gbx_prepare_bank_addr(
    uint32_t abs_byte_addr,
    bool is_multi_card,
    uint32_t *local_byte_addr_out)
{
    uint32_t local_byte_addr = abs_byte_addr;

    if (local_byte_addr_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (is_multi_card) {
        uint32_t bank = abs_byte_addr / BURN_GBA_BANK_BYTES;
        esp_err_t err = burner_gba_switch_bank_if_needed(bank);
        if (err != ESP_OK) {
            return err;
        }
        local_byte_addr = abs_byte_addr % BURN_GBA_BANK_BYTES;
    }

    *local_byte_addr_out = local_byte_addr;
    return ESP_OK;
}

static esp_err_t burner_gba_gbx_resolve_byte_addr(
    const burner_gba_gbx_exec_ctx_t *ctx,
    burner_gbx_addr_kind_t addr_kind,
    uint32_t addr_value,
    uint32_t base_offset,
    uint32_t *abs_byte_addr_out)
{
    if (ctx == NULL || abs_byte_addr_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (addr_kind) {
    case BURNER_GBX_ADDR_NONE:
        *abs_byte_addr_out = 0u;
        return ESP_OK;
    case BURNER_GBX_ADDR_ABS:
        *abs_byte_addr_out = base_offset + addr_value;
        return ESP_OK;
    case BURNER_GBX_ADDR_SA:
        *abs_byte_addr_out = ctx->sector_addr;
        return ESP_OK;
    case BURNER_GBX_ADDR_SA_PLUS_1:
        *abs_byte_addr_out = ctx->sector_addr + 1u;
        return ESP_OK;
    case BURNER_GBX_ADDR_SA_PLUS_2:
        *abs_byte_addr_out = ctx->sector_addr + 2u;
        return ESP_OK;
    case BURNER_GBX_ADDR_SA_PLUS_66:
        *abs_byte_addr_out = ctx->sector_addr + 66u;
        return ESP_OK;
    case BURNER_GBX_ADDR_SA_PLUS_132:
        *abs_byte_addr_out = ctx->sector_addr + 132u;
        return ESP_OK;
    case BURNER_GBX_ADDR_SA_PLUS_16384:
        *abs_byte_addr_out = ctx->sector_addr + 16384u;
        return ESP_OK;
    case BURNER_GBX_ADDR_SA_PLUS_28672:
        *abs_byte_addr_out = ctx->sector_addr + 28672u;
        return ESP_OK;
    case BURNER_GBX_ADDR_PA:
        *abs_byte_addr_out = ctx->program_addr;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t burner_gba_gbx_expected_value(
    const burner_gba_gbx_exec_ctx_t *ctx,
    burner_gbx_data_kind_t data_kind,
    uint16_t data_value,
    uint16_t *expected_out)
{
    if (ctx == NULL || expected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (data_kind) {
    case BURNER_GBX_DATA_VALUE:
        *expected_out = data_value;
        return ESP_OK;
    case BURNER_GBX_DATA_PD:
        *expected_out = ctx->last_program_data;
        return ESP_OK;
    case BURNER_GBX_DATA_BS:
        *expected_out = ctx->buffer_size_value;
        return ESP_OK;
    case BURNER_GBX_DATA_NONE:
        *expected_out = 0u;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t burner_gba_gbx_execute_status_reads(
    const burner_gba_gbx_exec_ctx_t *ctx,
    bool is_multi_card)
{
    const burner_gbx_profile_t *profile = &s_cart_ctx.gbx;

    if (!profile->wait_read_status_register || profile->read_status_register.count == 0u) {
        return ESP_OK;
    }

    for (uint32_t i = 0u; i < profile->read_status_register.count; ++i) {
        const burner_gbx_cmd_step_t *step = &profile->read_status_register.steps[i];
        uint32_t abs_byte_addr = 0u;
        uint32_t local_byte_addr = 0u;
        esp_err_t err;

        if (step->addr_kind == BURNER_GBX_ADDR_NONE || step->data_kind == BURNER_GBX_DATA_NONE) {
            continue;
        }
        if (step->data_kind != BURNER_GBX_DATA_VALUE) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        err = burner_gba_gbx_resolve_byte_addr(ctx, step->addr_kind, step->addr_value, 0u, &abs_byte_addr);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gba_gbx_prepare_bank_addr(abs_byte_addr, is_multi_card, &local_byte_addr);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_write_u16(local_byte_addr >> 1, step->data_value);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t burner_gba_gbx_wait_step(
    const burner_gba_gbx_exec_ctx_t *ctx,
    const burner_gbx_wait_step_t *wait_step,
    bool is_multi_card,
    uint32_t timeout_ms)
{
    int64_t deadline_us;
    uint16_t expected = 0u;
    uint16_t read_back = 0u;
    uint32_t abs_byte_addr = 0u;
    uint32_t local_byte_addr = 0u;
    esp_err_t err;

    if (ctx == NULL || wait_step == NULL || !wait_step->enabled) {
        return ESP_OK;
    }

    err = burner_gba_gbx_resolve_byte_addr(ctx, wait_step->addr_kind, wait_step->addr_value, 0u, &abs_byte_addr);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gba_gbx_expected_value(ctx, wait_step->expect_kind, wait_step->expect_value, &expected);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000ll);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gba_gbx_execute_status_reads(ctx, is_multi_card);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gba_gbx_prepare_bank_addr(abs_byte_addr, is_multi_card, &local_byte_addr);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(local_byte_addr >> 1, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if ((read_back & wait_step->mask) == expected) {
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    ESP_LOGW(
        BURNER_TAG,
        "GBX wait timeout: addr=0x%08" PRIX32 " expected=0x%04X mask=0x%04X last=0x%04X profile=%s",
        abs_byte_addr,
        expected,
        wait_step->mask,
        read_back,
        s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_gba_gbx_execute_plain_sequence(
    const burner_gbx_cmd_list_t *cmds,
    const burner_gbx_wait_list_t *waits,
    const burner_gba_gbx_exec_ctx_t *ctx,
    bool is_multi_card,
    uint32_t base_offset,
    uint32_t timeout_ms)
{
    if (cmds == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0u; i < cmds->count; ++i) {
        const burner_gbx_cmd_step_t *step = &cmds->steps[i];

        if (step->addr_kind != BURNER_GBX_ADDR_NONE && step->data_kind != BURNER_GBX_DATA_NONE) {
            uint32_t abs_byte_addr = 0u;
            uint32_t local_byte_addr = 0u;
            uint16_t value = 0u;
            esp_err_t err;

            err = burner_gba_gbx_resolve_byte_addr(ctx, step->addr_kind, step->addr_value, base_offset, &abs_byte_addr);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_gba_gbx_expected_value(ctx, step->data_kind, step->data_value, &value);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_gba_gbx_prepare_bank_addr(abs_byte_addr, is_multi_card, &local_byte_addr);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_bacon_rom_write_u16(local_byte_addr >> 1, value);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (waits != NULL && i < waits->count) {
            esp_err_t err = burner_gba_gbx_wait_step(ctx, &waits->steps[i], is_multi_card, timeout_ms);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t burner_gba_gbx_run_unlock_read_for_profile(
    const burner_gbx_profile_t *profile,
    bool is_multi_card)
{
    uint16_t discard = 0u;

    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0u; i < profile->unlock_read_count; ++i) {
        const burner_gbx_unlock_read_step_t *step = &profile->unlock_read[i];

        for (uint32_t repeat = 0u; repeat < step->repeat_count; ++repeat) {
            for (uint32_t off = 0u; off < step->len; off += 2u) {
                uint32_t abs_byte_addr = step->addr + off;
                uint32_t local_byte_addr = 0u;
                esp_err_t err = burner_gba_gbx_prepare_bank_addr(abs_byte_addr, is_multi_card, &local_byte_addr);
                if (err != ESP_OK) {
                    return err;
                }
                err = burner_bacon_rom_read_u16(local_byte_addr >> 1, &discard);
                if (err != ESP_OK) {
                    return err;
                }
            }
        }
    }

    return ESP_OK;
}

static esp_err_t burner_gba_gbx_run_unlock_read(bool is_multi_card)
{
    return burner_gba_gbx_run_unlock_read_for_profile(&s_cart_ctx.gbx, is_multi_card);
}

static esp_err_t burner_gba_gbx_apply_profile_geometry(
    const burner_gbx_profile_t *profile,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool cfi_ok)
{
    if (profile == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (profile->base_cmdset != BURNER_NOR_CMDSET_UNKNOWN) {
        s_cart_ctx.gba_cmdset = profile->base_cmdset;
    }

    if (profile->has_flash_size) {
        *device_size = profile->flash_size;
        if (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) &&
            s_cart_ctx.geometry.regions[s_cart_ctx.geometry.region_count - 1u].addr_end > *device_size) {
            (void)burner_nor_geometry_limit_prefix(&s_cart_ctx.geometry, *device_size);
        }
    }
    if (profile->has_sector_geometry && (!profile->sector_size_from_cfi || !cfi_ok)) {
        s_cart_ctx.geometry = profile->sector_geometry;
        *sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    } else if (profile->has_sector_size && (!profile->sector_size_from_cfi || !cfi_ok || *sector_size == 0u)) {
        *sector_size = profile->sector_size;
        if (*device_size != 0u && *sector_size != 0u) {
            (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
        }
    }
    if (profile->has_buffer_size || *buffer_write_bytes == 0u) {
        *buffer_write_bytes = profile->has_buffer_size ? profile->buffer_size : *buffer_write_bytes;
    }
    if (!burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && *device_size != 0u && *sector_size != 0u) {
        (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
    }

    return ESP_OK;
}

esp_err_t burner_gba_gbx_probe_locked(
    uint8_t id_out[8],
    burner_gbx_profile_t *profile_out,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out)
{
    burner_gbx_profile_t matched_profile = {0};
    size_t cache_match_len = 0u;
    esp_err_t err;

    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(id_out, 0, 8u);
    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;
    if (profile_out != NULL) {
        burner_gbx_profile_clear(profile_out);
    }

    burner_gbx_profile_clear(&s_cart_ctx.gbx);

    ESP_LOGI(BURNER_TAG, "GBX probe: CHIS ID/CFI first, then FlashGBX cache profile match");
    err = burner_bacon_gba_probe_locked(
        id_out,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok_out);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_gbx_find_agb_profile_for_probe(
        id_out,
        8u,
        s_cart_ctx.gba_cmdset,
        s_cart_ctx.d0d1_known,
        s_cart_ctx.d0d1_swapped,
        *device_size,
        *sector_size,
        &s_cart_ctx.geometry,
        *cfi_ok_out,
        &matched_profile,
        &cache_match_len);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(
            BURNER_TAG,
            "GBX probe failed: no FlashGBX AGB profile matched CHIS id=%02X %02X %02X %02X %02X %02X %02X %02X flash=%" PRIu32 " sector=%" PRIu32 " cmdset=%s cfi=%s",
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3],
            id_out[4],
            id_out[5],
            id_out[6],
            id_out[7],
            *device_size,
            *sector_size,
            burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
            *cfi_ok_out ? "ok" : "unavailable");
        return err;
    } else if (err != ESP_OK) {
        return err;
    }

    err = burner_gba_gbx_apply_profile_geometry(
        &matched_profile,
        device_size,
        sector_size,
        buffer_write_bytes,
        *cfi_ok_out);
    if (err != ESP_OK) {
        return err;
    }
    if (*device_size == 0u || *sector_size == 0u) {
        ESP_LOGE(BURNER_TAG, "GBX probe incomplete geometry: profile=%s", matched_profile.file_name);
        return ESP_ERR_INVALID_SIZE;
    }

    s_cart_ctx.gbx = matched_profile;
    if (profile_out != NULL) {
        *profile_out = matched_profile;
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBX probe ok: file=%s chip=%s flash=%" PRIu32 " sector=%" PRIu32
        " buf=%u cfi=%s cmdset=%s match_len=%u id=%02X %02X %02X %02X %02X %02X %02X %02X",
        matched_profile.file_name,
        matched_profile.display_name[0] != '\0' ? matched_profile.display_name : "unknown",
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        *cfi_ok_out ? "ok" : "unavailable",
        burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
        (unsigned)cache_match_len,
        id_out[0],
        id_out[1],
        id_out[2],
        id_out[3],
        id_out[4],
        id_out[5],
        id_out[6],
        id_out[7]);
    return ESP_OK;
}

esp_err_t burner_gba_gbx_reset_to_read_mode(bool full_reset, bool is_multi_card, uint32_t max_address)
{
    burner_gba_gbx_exec_ctx_t ctx = {0};
    esp_err_t err;

    if (!burner_gba_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (full_reset && s_cart_ctx.gbx.power_cycle) {
        err = burner_bacon_gba_power_cycle_3v3_locked();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gba_gbx_run_unlock_read(is_multi_card);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gba_gbx_execute_plain_sequence(
            &s_cart_ctx.gbx.unlock,
            NULL,
            &ctx,
            is_multi_card,
            0u,
            BURNER_ROM_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (full_reset &&
        s_cart_ctx.gbx.has_reset_every &&
        s_cart_ctx.gbx.reset_every != 0u &&
        s_cart_ctx.device_size != 0u) {
        uint32_t limit = max_address;

        if (limit == 0u || limit > s_cart_ctx.device_size) {
            limit = s_cart_ctx.device_size;
        }
        for (uint32_t base = 0u; base < limit; base += s_cart_ctx.gbx.reset_every) {
            err = burner_gba_gbx_execute_plain_sequence(
                &s_cart_ctx.gbx.reset,
                NULL,
                &ctx,
                is_multi_card,
                base,
                BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
        }
    } else {
        err = burner_gba_gbx_execute_plain_sequence(
            &s_cart_ctx.gbx.reset,
            NULL,
            &ctx,
            is_multi_card,
            0u,
            BURNER_ROM_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (is_multi_card) {
        err = burner_gba_switch_bank_if_needed(0u);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t burner_gba_gbx_finalize_write(bool is_multi_card)
{
    return burner_gba_gbx_reset_to_read_mode(true, is_multi_card, s_cart_ctx.device_size);
}

static esp_err_t burner_gba_gbx_single_write_word(
    uint32_t byte_addr,
    uint16_t word,
    bool is_multi_card,
    uint32_t timeout_ms)
{
    burner_gba_gbx_exec_ctx_t ctx = {
        .sector_addr = burner_gba_sector_begin_for_addr(byte_addr),
        .program_addr = byte_addr,
        .program_data = word,
        .last_program_data = word,
    };

    for (uint32_t i = 0u; i < s_cart_ctx.gbx.single_write.count; ++i) {
        const burner_gbx_cmd_step_t *step = &s_cart_ctx.gbx.single_write.steps[i];

        if (step->addr_kind != BURNER_GBX_ADDR_NONE && step->data_kind != BURNER_GBX_DATA_NONE) {
            uint32_t abs_byte_addr = 0u;
            uint32_t local_byte_addr = 0u;
            esp_err_t err =
                burner_gba_gbx_resolve_byte_addr(&ctx, step->addr_kind, step->addr_value, 0u, &abs_byte_addr);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_gba_gbx_prepare_bank_addr(abs_byte_addr, is_multi_card, &local_byte_addr);
            if (err != ESP_OK) {
                return err;
            }

            if (step->data_kind == BURNER_GBX_DATA_PD) {
                err = burner_bacon_rom_write_u16(local_byte_addr >> 1, word);
            } else if (step->data_kind == BURNER_GBX_DATA_VALUE) {
                err = burner_bacon_rom_write_u16(local_byte_addr >> 1, step->data_value);
            } else {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (err != ESP_OK) {
                return err;
            }
        }

        if (i < s_cart_ctx.gbx.single_write_wait_for.count) {
            esp_err_t err = burner_gba_gbx_wait_step(
                &ctx,
                &s_cart_ctx.gbx.single_write_wait_for.steps[i],
                is_multi_card,
                timeout_ms);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t burner_gba_gbx_buffer_write_chunk(
    uint32_t starting_address,
    const uint8_t *buf,
    size_t write_len,
    bool is_multi_card,
    uint32_t timeout_ms)
{
    burner_gba_gbx_exec_ctx_t ctx;
    size_t write_words;

    if (buf == NULL || write_len < 2u || (write_len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    write_words = write_len / 2u;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sector_addr = burner_gba_sector_begin_for_addr(starting_address);
    ctx.program_addr = starting_address;
    ctx.buffer_size_value = (uint16_t)(write_words - 1u);
    ctx.last_program_data = (uint16_t)((uint16_t)buf[write_len - 2u] | ((uint16_t)buf[write_len - 1u] << 8));

    for (uint32_t i = 0u; i < s_cart_ctx.gbx.buffer_write.count; ++i) {
        const burner_gbx_cmd_step_t *step = &s_cart_ctx.gbx.buffer_write.steps[i];

        if (step->addr_kind != BURNER_GBX_ADDR_NONE && step->data_kind != BURNER_GBX_DATA_NONE) {
            uint32_t abs_byte_addr = 0u;
            uint32_t local_byte_addr = 0u;
            esp_err_t err =
                burner_gba_gbx_resolve_byte_addr(&ctx, step->addr_kind, step->addr_value, 0u, &abs_byte_addr);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_gba_gbx_prepare_bank_addr(abs_byte_addr, is_multi_card, &local_byte_addr);
            if (err != ESP_OK) {
                return err;
            }

            if (step->data_kind == BURNER_GBX_DATA_VALUE) {
                err = burner_bacon_rom_write_u16(local_byte_addr >> 1, step->data_value);
            } else if (step->data_kind == BURNER_GBX_DATA_BS) {
                err = burner_bacon_rom_write_u16(local_byte_addr >> 1, ctx.buffer_size_value);
            } else if (step->data_kind == BURNER_GBX_DATA_PD) {
                for (size_t wr = 0u; wr < write_words; ++wr) {
                    uint16_t word = (uint16_t)((uint16_t)buf[wr * 2u] | ((uint16_t)buf[wr * 2u + 1u] << 8));

                    err = burner_bacon_rom_write_u16((local_byte_addr >> 1) + (uint32_t)wr, word);
                    if (err != ESP_OK) {
                        return err;
                    }
                }
            } else {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (err != ESP_OK) {
                return err;
            }
        }

        if (i < s_cart_ctx.gbx.buffer_write_wait_for.count) {
            esp_err_t err = burner_gba_gbx_wait_step(
                &ctx,
                &s_cart_ctx.gbx.buffer_write_wait_for.steps[i],
                is_multi_card,
                timeout_ms);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

esp_err_t burner_gba_gbx_program_block(
    const uint8_t *data,
    size_t len,
    uint32_t offset,
    bool is_multi_card,
    bool prepare_sectors)
{
    size_t programmed = 0u;
    uint16_t buffer_bytes = s_cart_ctx.program_buffer_write_bytes;

    if (data == NULL || len == 0u || (offset & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (programmed < len) {
        uint32_t rom_addr = offset + (uint32_t)programmed;
        uint32_t bank = 0u;
        uint32_t bank_remain = UINT32_MAX - rom_addr;
        uint32_t sector_end = 0u;
        size_t remain = len - programmed;
        size_t chunk = remain;
        esp_err_t err;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        burner_gba_resolve_write_addr(rom_addr, is_multi_card, &bank, NULL, &bank_remain);
        if (chunk > (size_t)bank_remain) {
            chunk = (size_t)bank_remain;
        }
        if (burner_nor_geometry_sector_bounds(&s_cart_ctx.geometry, rom_addr, NULL, &sector_end, NULL) == ESP_OK &&
            sector_end > rom_addr &&
            chunk > (size_t)(sector_end - rom_addr)) {
            chunk = (size_t)(sector_end - rom_addr);
        }

        if (is_multi_card) {
            err = burner_gba_switch_bank_if_needed(bank);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (prepare_sectors) {
            err = burner_gba_sector_erase_prepare_current(rom_addr);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (buffer_bytes >= 2u && s_cart_ctx.gbx.buffer_write.count > 0u) {
            if (chunk > (size_t)buffer_bytes) {
                chunk = (size_t)buffer_bytes;
            }
            chunk = burner_gba_program_safe_chunk_bytes(rom_addr, chunk, (size_t)buffer_bytes);
            err = burner_gba_gbx_buffer_write_chunk(
                rom_addr,
                data + programmed,
                chunk,
                is_multi_card,
                BURNER_ROM_POLL_TIMEOUT_MS);
        } else {
            chunk &= ~((size_t)0x1u);
            if (chunk == 0u) {
                chunk = 2u;
            }
            for (size_t off = 0u; off < chunk; off += 2u) {
                uint16_t word =
                    (uint16_t)((uint16_t)data[programmed + off] | ((uint16_t)data[programmed + off + 1u] << 8));
                err = burner_gba_gbx_single_write_word(
                    rom_addr + (uint32_t)off,
                    word,
                    is_multi_card,
                    BURNER_ROM_POLL_TIMEOUT_MS);
                if (err != ESP_OK) {
                    return err;
                }
            }
        }
        if (err != ESP_OK) {
            return err;
        }

        if (prepare_sectors) {
            err = burner_gba_sector_erase_prefetch_next(rom_addr, chunk);
            if (err != ESP_OK) {
                return err;
            }
        }

        programmed += chunk;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

esp_err_t burner_gba_gbx_erase_sector(uint32_t flash_addr, bool is_multi_card, uint32_t timeout_ms)
{
    burner_gba_gbx_exec_ctx_t ctx = {
        .sector_addr = flash_addr,
        .program_addr = flash_addr,
    };

    if (!burner_gba_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    return burner_gba_gbx_execute_plain_sequence(
        &s_cart_ctx.gbx.sector_erase,
        &s_cart_ctx.gbx.sector_erase_wait_for,
        &ctx,
        is_multi_card,
        0u,
        timeout_ms);
}

esp_err_t burner_gba_gbx_chip_erase_once(void)
{
    burner_gba_gbx_exec_ctx_t ctx = {0};
    uint32_t timeout_ms = s_cart_ctx.gbx.has_chip_erase_timeout ? s_cart_ctx.gbx.chip_erase_timeout_ms
                                                                : BURNER_ROM_CHIP_ERASE_TIMEOUT_MS;

    if (!burner_gba_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    return burner_gba_gbx_execute_plain_sequence(
        &s_cart_ctx.gbx.chip_erase,
        &s_cart_ctx.gbx.chip_erase_wait_for,
        &ctx,
        false,
        0u,
        timeout_ms);
}

esp_err_t burner_gba_gbx_prepare_profile(
    const burner_task_param_t *job,
    const uint8_t id[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    uint16_t *program_buffer_write_bytes,
    bool cfi_ok)
{
    burner_gbx_profile_t *profile = &s_cart_ctx.gbx;
    bool runtime_supported;

    if (job == NULL || id == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || program_buffer_write_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *program_buffer_write_bytes = 0u;

    if (job->recipe_mode != BURNER_RECIPE_MODE_GBX) {
        burner_gbx_profile_clear(&s_cart_ctx.gbx);
        return ESP_OK;
    }

    if (!s_cart_ctx.gbx.active) {
        ESP_LOGE(
            BURNER_TAG,
            "GBX profile not active after FlashGBX probe for id=%02X %02X %02X %02X %02X %02X %02X %02X",
            id[0],
            id[1],
            id[2],
            id[3],
            id[4],
            id[5],
            id[6],
            id[7]);
        return ESP_ERR_INVALID_STATE;
    }

    profile->runtime_commands_enabled = false;
    if (profile->d0d1_known) {
        s_cart_ctx.d0d1_known = true;
        s_cart_ctx.d0d1_swapped = profile->d0d1_swapped;
    }
    if (profile->base_cmdset != BURNER_NOR_CMDSET_UNKNOWN) {
        s_cart_ctx.gba_cmdset = profile->base_cmdset;
    }
    if (profile->has_flash_size) {
        *device_size = profile->flash_size;
    }
    if (profile->has_sector_geometry && (!profile->sector_size_from_cfi || !cfi_ok)) {
        s_cart_ctx.geometry = profile->sector_geometry;
        *sector_size = burner_nor_geometry_report_sector_size(&profile->sector_geometry);
    } else if (profile->has_sector_size && (!profile->sector_size_from_cfi || !cfi_ok || *sector_size == 0u)) {
        *sector_size = profile->sector_size;
        if (*device_size != 0u && *sector_size != 0u) {
            (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
        }
    }
    if (profile->has_buffer_size) {
        *buffer_write_bytes = profile->buffer_size;
    }
    if (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_UNKNOWN && profile->command_set_name[0] != '\0') {
        ESP_LOGW(
            BURNER_TAG,
            "GBX profile uses custom command_set=%s, CHIS runtime may not support it",
            profile->command_set_name);
    }
    runtime_supported = burner_gba_gbx_profile_runtime_supported(profile, job, *buffer_write_bytes);
    if ((job->mode == BURNER_JOB_WRITE_ROM || job->mode == BURNER_JOB_ERASE_ROM) && !runtime_supported) {
        ESP_LOGE(
            BURNER_TAG,
            "GBX profile runtime template unsupported: file=%s mode=%d single=%u buffer=%u sector_erase=%u chip_erase=%u",
            profile->file_name,
            (int)job->mode,
            (unsigned)profile->single_write.count,
            (unsigned)profile->buffer_write.count,
            (unsigned)profile->sector_erase.count,
            (unsigned)profile->chip_erase.count);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (runtime_supported) {
        profile->runtime_commands_enabled = true;
    }
    *program_buffer_write_bytes = burner_gba_gbx_program_buffer_bytes(profile, *buffer_write_bytes);

    ESP_LOGI(
        BURNER_TAG,
        "GBX profile prepared: file=%s chip=%s flash=%" PRIu32 " sector=%" PRIu32
        " buf=%u prog_buf=%u cmdset=%s cfi=%s swapped=%u runtime=%u",
        s_cart_ctx.gbx.file_name,
        s_cart_ctx.gbx.display_name[0] != '\0' ? s_cart_ctx.gbx.display_name : "unknown",
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        (unsigned)*program_buffer_write_bytes,
        s_cart_ctx.gbx.command_set_name[0] != '\0' ? s_cart_ctx.gbx.command_set_name
                                                   : burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
        cfi_ok ? "ok" : "unavailable",
        s_cart_ctx.d0d1_swapped ? 1u : 0u,
        profile->runtime_commands_enabled ? 1u : 0u);
    return ESP_OK;
}

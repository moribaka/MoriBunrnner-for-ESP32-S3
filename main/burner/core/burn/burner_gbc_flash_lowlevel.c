/* Low-level GBC/MBC5 flash bank, erase, and program helpers. */

static esp_err_t burner_bacon_mbc5_switch_bank(uint16_t bank)
{
    uint8_t b0 = (uint8_t)(bank & 0xFFu);
    uint8_t b1 = (uint8_t)((bank >> 8) & 0xFFu);
    esp_err_t err;

    if (s_gb_mapper_kind == BURNER_GB_MAPPER_MBC3) {
        b0 = burner_gb_mapper_normalize_rom_bank(s_gb_mapper_kind, bank);
        return burner_bacon_gbc_write(0x2000u, &b0, 1u);
    }

    /* MBC5 uses low 8 bits at 0x2000 and the high bit at 0x3000. */
    err = burner_bacon_gbc_write(0x2000u, &b0, 1u);
    if (err != ESP_OK) {
        return err;
    }
    return burner_bacon_gbc_write(0x3000u, &b1, 1u);
}

static esp_err_t burner_bacon_mbc5_ram_switch_bank(uint8_t bank)
{
    if (s_gb_mapper_kind == BURNER_GB_MAPPER_MBC3) {
        bank &= 0x07u;
    }
    return burner_bacon_gbc_write(0x4000u, &bank, 1);
}

static esp_err_t burner_bacon_mbc5_ram_enable(bool enable)
{
    uint8_t cmd = enable ? 0x0Au : 0x00u;
    return burner_bacon_gbc_write(0x0000u, &cmd, 1);
}

static bool burner_mbc5_amd_status_matches_dq7(uint8_t status, uint8_t expected_data)
{
    return (status & 0x80u) == (expected_data & 0x80u);
}

static esp_err_t burner_bacon_mbc5_amd_wait_program_complete(
    uint16_t addr,
    uint8_t expected_data,
    uint32_t timeout_ms,
    uint8_t *status_out)
{
    int64_t deadline_us;
    uint8_t status1 = 0u;
    uint8_t status2 = 0u;
    esp_err_t err;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(addr, &status1);
        if (err != ESP_OK) {
            return err;
        }
        if (burner_mbc5_amd_status_matches_dq7(status1, expected_data)) {
            err = burner_bacon_gbc_read_u8(addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if (burner_mbc5_amd_status_matches_dq7(status2, expected_data)) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
        }
        if ((status1 & 0x20u) != 0u) {
            err = burner_bacon_gbc_read_u8(addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if (burner_mbc5_amd_status_matches_dq7(status2, expected_data)) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
            if (status_out != NULL) {
                *status_out = status2;
            }
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    if (status_out != NULL) {
        *status_out = status1;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_mbc5_amd_wait_erase_complete(
    uint16_t addr,
    uint32_t timeout_ms,
    uint8_t *status_out)
{
    int64_t deadline_us;
    uint8_t status1 = 0u;
    uint8_t status2 = 0u;
    esp_err_t err;

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_gbc_read_u8(addr, &status1);
        if (err != ESP_OK) {
            return err;
        }
        if ((status1 & 0x80u) != 0u) {
            err = burner_bacon_gbc_read_u8(addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if ((status2 & 0x80u) != 0u) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
        }
        if ((status1 & 0x20u) != 0u) {
            err = burner_bacon_gbc_read_u8(addr, &status2);
            if (err != ESP_OK) {
                return err;
            }
            if ((status2 & 0x80u) != 0u) {
                if (status_out != NULL) {
                    *status_out = status2;
                }
                return ESP_OK;
            }
            if (status_out != NULL) {
                *status_out = status2;
            }
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    if (status_out != NULL) {
        *status_out = status1;
    }
    return ESP_ERR_TIMEOUT;
}

static uint32_t burner_erase_timeout_ms_for_bytes(uint32_t bytes)
{
    uint32_t mb;
    uint64_t timeout_ms;

    if (bytes == 0u) {
        return BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS;
    }

    mb = (bytes + ((1024u * 1024u) - 1u)) / (1024u * 1024u);
    if (mb == 0u) {
        mb = 1u;
    }
    timeout_ms = (uint64_t)mb * (uint64_t)BURNER_ROM_ERASE_TIMEOUT_PER_MB_MS;
    if (timeout_ms > BURNER_ROM_ERASE_TIMEOUT_MAX_MS) {
        timeout_ms = BURNER_ROM_ERASE_TIMEOUT_MAX_MS;
    }
    return (uint32_t)timeout_ms;
}

static uint32_t burner_erase_remaining_timeout_ms(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();

    if (remaining_us <= 0) {
        return 0u;
    }
    return (uint32_t)((remaining_us + 999LL) / 1000LL);
}

static bool burner_buffer_all_equal(const uint8_t *left, const uint8_t *right, size_t len)
{
    return left != NULL && right != NULL && len > 0u && memcmp(left, right, len) == 0;
}

typedef struct {
    uint32_t sector_addr;
    uint32_t program_addr;
    uint8_t program_data;
    uint8_t last_program_data;
    uint16_t buffer_size_value;
} burner_gbc_gbx_exec_ctx_t;

typedef struct {
    bool found;
    uint8_t id[BURNER_GBX_FLASH_ID_LEN_MAX];
    burner_gbx_profile_t *profile;
} burner_gbc_gbx_probe_ctx_t;

bool burner_gbc_gbx_is_active(void)
{
    return s_cart_ctx.gbx.active && s_cart_ctx.gbx.runtime_commands_enabled;
}

static bool burner_gbc_gbx_profile_write_supported(const burner_gbx_profile_t *profile)
{
    if (profile == NULL) {
        return false;
    }
    if (profile->type[0] != '\0' && strcasecmp(profile->type, "DMG") != 0) {
        return false;
    }
    if (profile->write_pin[0] != '\0' && strcasecmp(profile->write_pin, "WR") != 0) {
        return false;
    }
    if (profile->command_set_name[0] != '\0' && strcasecmp(profile->command_set_name, "AMD") != 0) {
        return false;
    }
    if (profile->has_flash_bank_select_type && profile->flash_bank_select_type != 0u) {
        return false;
    }
    return true;
}

static bool burner_gbc_gbx_runtime_supported(const burner_gbx_profile_t *profile)
{
    return burner_gbc_gbx_profile_write_supported(profile) &&
           (profile->single_write.count > 0u || profile->buffer_write.count > 0u);
}

static uint32_t burner_gbc_gbx_first_bank(void)
{
    return s_cart_ctx.gbx.has_first_bank ? s_cart_ctx.gbx.first_bank : 0u;
}

static uint32_t burner_gbc_gbx_start_addr(void)
{
    return s_cart_ctx.gbx.has_start_addr ? s_cart_ctx.gbx.start_addr : 0u;
}

static uint16_t burner_gbc_gbx_bank_base_addr(uint32_t logical_bank)
{
    if (logical_bank == 0u && s_cart_ctx.gbx.has_start_addr) {
        return (uint16_t)(s_cart_ctx.gbx.start_addr & 0xFFFFu);
    }
    return (logical_bank == 0u) ? 0x0000u : 0x4000u;
}

static esp_err_t burner_gbc_gbx_select_bank(uint32_t logical_bank)
{
    uint32_t physical_bank = logical_bank + burner_gbc_gbx_first_bank();
    uint8_t high = (uint8_t)((physical_bank >> 8) & 0x01u);
    uint8_t low = (uint8_t)(physical_bank & 0xFFu);
    esp_err_t err;

    if (physical_bank > 0x1FFu) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_cart_ctx.current_bank == (uint16_t)physical_bank) {
        return ESP_OK;
    }

    err = burner_bacon_gbc_write(0x3000u, &high, 1u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_write(0x2100u, &low, 1u);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = (uint16_t)physical_bank;
    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_logical_to_cart_addr(
    uint32_t logical_addr,
    uint16_t *cart_addr_out,
    uint32_t *bank_remain_out)
{
    uint32_t bank = logical_addr / BURN_MBC5_ROM_BANK_BYTES;
    uint32_t bank_off = logical_addr % BURN_MBC5_ROM_BANK_BYTES;
    uint32_t base = burner_gbc_gbx_bank_base_addr(bank);
    uint32_t cart_addr = base + bank_off;
    uint32_t bank_remain = BURN_MBC5_ROM_BANK_BYTES - bank_off;
    esp_err_t err;

    if (cart_addr > 0xFFFFu) {
        return ESP_ERR_INVALID_SIZE;
    }
    err = burner_gbc_gbx_select_bank(bank);
    if (err != ESP_OK) {
        return err;
    }
    if (base < 0x4000u && cart_addr >= 0x4000u) {
        bank_remain = 0x4000u - bank_off;
    }

    if (cart_addr_out != NULL) {
        *cart_addr_out = (uint16_t)cart_addr;
    }
    if (bank_remain_out != NULL) {
        *bank_remain_out = bank_remain;
    }
    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_resolve_byte_addr(
    const burner_gbc_gbx_exec_ctx_t *ctx,
    burner_gbx_addr_kind_t addr_kind,
    uint32_t addr_value,
    uint16_t *cart_addr_out)
{
    uint32_t addr = 0u;

    if (ctx == NULL || cart_addr_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (addr_kind) {
    case BURNER_GBX_ADDR_NONE:
        addr = 0u;
        break;
    case BURNER_GBX_ADDR_ABS:
        addr = addr_value;
        break;
    case BURNER_GBX_ADDR_SA:
        addr = ctx->sector_addr;
        break;
    case BURNER_GBX_ADDR_SA_PLUS_1:
        addr = ctx->sector_addr + 1u;
        break;
    case BURNER_GBX_ADDR_SA_PLUS_2:
        addr = ctx->sector_addr + 2u;
        break;
    case BURNER_GBX_ADDR_SA_PLUS_66:
        addr = ctx->sector_addr + 66u;
        break;
    case BURNER_GBX_ADDR_SA_PLUS_132:
        addr = ctx->sector_addr + 132u;
        break;
    case BURNER_GBX_ADDR_SA_PLUS_16384:
        addr = ctx->sector_addr + 16384u;
        break;
    case BURNER_GBX_ADDR_SA_PLUS_28672:
        addr = ctx->sector_addr + 28672u;
        break;
    case BURNER_GBX_ADDR_PA:
        addr = ctx->program_addr;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (addr > 0xFFFFu) {
        return ESP_ERR_INVALID_SIZE;
    }
    *cart_addr_out = (uint16_t)addr;
    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_expected_value(
    const burner_gbc_gbx_exec_ctx_t *ctx,
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
        *expected_out = (uint16_t)ctx->last_program_data | ((uint16_t)ctx->last_program_data << 8);
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

static esp_err_t burner_gbc_gbx_read_word_le(uint16_t addr, uint16_t *word_out)
{
    uint8_t bytes[2] = {0};
    esp_err_t err;

    if (word_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = burner_bacon_gbc_read(addr, bytes, sizeof(bytes));
    if (err != ESP_OK) {
        return err;
    }
    *word_out = (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_execute_status_reads(const burner_gbc_gbx_exec_ctx_t *ctx)
{
    const burner_gbx_profile_t *profile = &s_cart_ctx.gbx;

    if (!profile->wait_read_status_register || profile->read_status_register.count == 0u) {
        return ESP_OK;
    }

    for (uint32_t i = 0u; i < profile->read_status_register.count; ++i) {
        const burner_gbx_cmd_step_t *step = &profile->read_status_register.steps[i];
        uint16_t addr = 0u;
        uint16_t value = 0u;
        uint8_t byte_value;
        esp_err_t err;

        if (step->addr_kind == BURNER_GBX_ADDR_NONE || step->data_kind == BURNER_GBX_DATA_NONE) {
            continue;
        }
        err = burner_gbc_gbx_resolve_byte_addr(ctx, step->addr_kind, step->addr_value, &addr);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gbc_gbx_expected_value(ctx, step->data_kind, step->data_value, &value);
        if (err != ESP_OK) {
            return err;
        }
        byte_value = (uint8_t)(value & 0xFFu);
        err = burner_bacon_gbc_write(addr, &byte_value, 1u);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_wait_step(
    const burner_gbc_gbx_exec_ctx_t *ctx,
    const burner_gbx_wait_step_t *wait_step,
    uint32_t timeout_ms)
{
    int64_t deadline_us;
    uint16_t addr = 0u;
    uint16_t expected = 0u;
    uint16_t read_back = 0u;
    esp_err_t err;

    if (ctx == NULL || wait_step == NULL || !wait_step->enabled) {
        return ESP_OK;
    }
    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    err = burner_gbc_gbx_resolve_byte_addr(ctx, wait_step->addr_kind, wait_step->addr_value, &addr);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbc_gbx_expected_value(ctx, wait_step->expect_kind, wait_step->expect_value, &expected);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gbc_gbx_execute_status_reads(ctx);
        if (err != ESP_OK) {
            return err;
        }
        (void)burner_gbc_gbx_read_word_le(addr, &read_back);
        err = burner_gbc_gbx_read_word_le(addr, &read_back);
        if (err != ESP_OK) {
            return err;
        }
        if ((read_back & wait_step->mask) == (expected & wait_step->mask)) {
            return ESP_OK;
        }
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }

    ESP_LOGW(
        BURNER_TAG,
        "GBC GBX wait timeout: addr=0x%04X expected=0x%04X mask=0x%04X last=0x%04X profile=%s",
        (unsigned)addr,
        expected,
        wait_step->mask,
        read_back,
        s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_gbc_gbx_execute_plain_sequence(
    const burner_gbx_cmd_list_t *cmds,
    const burner_gbx_wait_list_t *waits,
    const burner_gbc_gbx_exec_ctx_t *ctx,
    uint32_t timeout_ms)
{
    if (cmds == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0u; i < cmds->count; ++i) {
        const burner_gbx_cmd_step_t *step = &cmds->steps[i];

        if (step->addr_kind != BURNER_GBX_ADDR_NONE && step->data_kind != BURNER_GBX_DATA_NONE) {
            uint16_t addr = 0u;
            uint16_t value = 0u;
            uint8_t byte_value;
            esp_err_t err;

            err = burner_gbc_gbx_resolve_byte_addr(ctx, step->addr_kind, step->addr_value, &addr);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_gbc_gbx_expected_value(ctx, step->data_kind, step->data_value, &value);
            if (err != ESP_OK) {
                return err;
            }
            byte_value = (uint8_t)(value & 0xFFu);
            err = burner_bacon_gbc_write(addr, &byte_value, 1u);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (waits != NULL && i < waits->count) {
            esp_err_t err = burner_gbc_gbx_wait_step(ctx, &waits->steps[i], timeout_ms);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_run_unlock_read_for_profile(const burner_gbx_profile_t *profile)
{
    uint8_t discard = 0u;

    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0u; i < profile->unlock_read_count; ++i) {
        const burner_gbx_unlock_read_step_t *step = &profile->unlock_read[i];

        for (uint32_t repeat = 0u; repeat < step->repeat_count; ++repeat) {
            for (uint32_t off = 0u; off < step->len; ++off) {
                uint32_t addr = step->addr + off;

                if (addr > 0xFFFFu) {
                    return ESP_ERR_INVALID_SIZE;
                }
                esp_err_t err = burner_bacon_gbc_read_u8((uint16_t)addr, &discard);
                if (err != ESP_OK) {
                    return err;
                }
            }
        }
    }

    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_read_chip_bytes(uint16_t addr, uint8_t *out, size_t len)
{
    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_gbc_read(addr, out, len);
}

static size_t burner_gbc_gbx_first_flash_id_len(const burner_gbx_profile_t *profile)
{
    if (profile == NULL) {
        return 0u;
    }
    if (profile->flash_id_count > 0u && profile->flash_id_len[0] > 0u) {
        return profile->flash_id_len[0];
    }
    if (profile->flash_id_bank_count > 0u && profile->flash_id_bank_len[0] > 0u) {
        return profile->flash_id_bank_len[0];
    }
    return 0u;
}

static esp_err_t burner_gbc_gbx_read_id_with_profile(
    const burner_gbx_profile_t *profile,
    uint8_t id_out[BURNER_GBX_FLASH_ID_LEN_MAX],
    size_t *id_len_out,
    bool *changed_out)
{
    burner_gbc_gbx_exec_ctx_t ctx = {0};
    uint8_t baseline[BURNER_GBX_FLASH_ID_LEN_MAX] = {0};
    uint32_t read_addr = 0u;
    size_t id_len;
    esp_err_t err;

    if (profile == NULL || id_out == NULL || id_len_out == NULL || changed_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (profile->read_identifier.count == 0u || !burner_gbc_gbx_profile_write_supported(profile)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    id_len = burner_gbc_gbx_first_flash_id_len(profile);
    if (id_len == 0u || id_len > BURNER_GBX_FLASH_ID_LEN_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    read_addr = profile->has_read_identifier_at ? profile->read_identifier_at : 0u;
    if (read_addr > 0xFFFFu) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(id_out, 0, BURNER_GBX_FLASH_ID_LEN_MAX);
    *id_len_out = id_len;
    *changed_out = false;

    err = burner_gbc_gbx_execute_plain_sequence(
        &profile->reset,
        NULL,
        &ctx,
        BURNER_ROM_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbc_gbx_read_chip_bytes((uint16_t)read_addr, baseline, id_len);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbc_gbx_run_unlock_read_for_profile(profile);
    if (err != ESP_OK) {
        return err;
    }
    if (profile->unlock.count > 0u) {
        err = burner_gbc_gbx_execute_plain_sequence(
            &profile->unlock,
            NULL,
            &ctx,
            BURNER_ROM_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        esp_rom_delay_us(1000u);
    }
    err = burner_gbc_gbx_execute_plain_sequence(
        &profile->read_identifier,
        NULL,
        &ctx,
        BURNER_ROM_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(1000u);
    err = burner_gbc_gbx_read_chip_bytes((uint16_t)read_addr, id_out, id_len);
    (void)burner_gbc_gbx_execute_plain_sequence(
        &profile->reset,
        NULL,
        &ctx,
        BURNER_ROM_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    *changed_out = (memcmp(baseline, id_out, id_len) != 0);
    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_probe_method_visitor(
    const burner_gbx_profile_t *profile,
    void *user,
    bool *stop_out)
{
    burner_gbc_gbx_probe_ctx_t *ctx = (burner_gbc_gbx_probe_ctx_t *)user;
    size_t cache_match_len = 0u;
    uint8_t id[BURNER_GBX_FLASH_ID_LEN_MAX] = {0};
    size_t id_len = 0u;
    bool changed = false;
    esp_err_t err;

    if (profile == NULL || ctx == NULL || ctx->profile == NULL || stop_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *stop_out = false;
    if (profile->read_identifier.count == 0u || !burner_gbc_gbx_profile_write_supported(profile)) {
        return ESP_OK;
    }

    err = burner_cancel_poll();
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbc_gbx_read_id_with_profile(profile, id, &id_len, &changed);
    if (err != ESP_OK || !changed) {
        return ESP_OK;
    }

    err = burner_gbx_find_cached_profile(
        "DMG",
        &profile->read_identifier,
        id,
        id_len,
        ctx->profile,
        &cache_match_len);
    if (err == ESP_OK && cache_match_len != 0u) {
        memcpy(ctx->id, id, sizeof(ctx->id));
        ctx->found = true;
        *stop_out = true;
        ESP_LOGI(
            BURNER_TAG,
            "GBC GBX profile matched by cache: method=%s profile=%s id=%02X %02X %02X %02X",
            profile->file_name,
            ctx->profile->file_name,
            id[0],
            id[1],
            id[2],
            id[3]);
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(BURNER_TAG, "GBC GBX cache match unavailable: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_apply_profile_geometry(
    const burner_gbx_profile_t *profile,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out,
    burner_nor_cmdset_t *cmdset_out)
{
    if (profile == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;
    if (cmdset_out != NULL) {
        *cmdset_out = profile->base_cmdset;
    }
    burner_nor_geometry_clear(&s_cart_ctx.geometry);

    if (profile->has_flash_size) {
        *device_size = profile->flash_size;
    }
    if (profile->has_sector_geometry) {
        s_cart_ctx.geometry = profile->sector_geometry;
        *sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    } else if (profile->has_sector_size) {
        *sector_size = profile->sector_size;
        if (*device_size != 0u && *sector_size != 0u) {
            (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
        }
    }
    if (profile->has_buffer_size) {
        *buffer_write_bytes = profile->buffer_size;
    }
    if (*buffer_write_bytes > 512u) {
        *buffer_write_bytes = 512u;
    }
    if (*buffer_write_bytes > ((BURNER_SPI_MAX_XFER - 30u) / 6u)) {
        *buffer_write_bytes = (uint16_t)((BURNER_SPI_MAX_XFER - 30u) / 6u);
    }
    if (*device_size == 0u || *sector_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!burner_nor_geometry_is_valid(&s_cart_ctx.geometry)) {
        (void)burner_nor_geometry_set_uniform(&s_cart_ctx.geometry, *device_size, *sector_size);
    }
    return burner_nor_geometry_is_valid(&s_cart_ctx.geometry) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static burner_gb_mapper_t burner_gbc_gbx_mapper_from_profile(const burner_gbx_profile_t *profile)
{
    if (profile != NULL &&
        profile->mbc_name[0] != '\0' &&
        strncasecmp(profile->mbc_name, "MBC3", 4u) == 0) {
        return BURNER_GB_MAPPER_MBC3;
    }
    return BURNER_GB_MAPPER_MBC5;
}

static esp_err_t burner_gbc_gbx_apply_mapper_limit(
    burner_gb_mapper_t mapper,
    uint32_t *device_size,
    uint32_t *sector_size)
{
    uint32_t device_limit = 0u;

    if (device_size == NULL || sector_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gb_mapper_kind = mapper;
    device_limit = burner_gb_mapper_device_size_limit(mapper);
    if (device_limit != 0u && *device_size > device_limit) {
        *device_size = device_limit;
        if (burner_nor_geometry_limit_prefix(&s_cart_ctx.geometry, *device_size) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        *sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    }

    return (*device_size != 0u && *sector_size != 0u) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t burner_gbc_gbx_prepare_manual_profile(
    const char *file_name,
    uint8_t id_out[4],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out,
    burner_nor_cmdset_t *cmdset_out)
{
    burner_gbx_profile_t manual_profile = {0};
    uint8_t raw_id[BURNER_GBX_FLASH_ID_LEN_MAX] = {0};
    size_t raw_id_len = 0u;
    size_t match_len = 0u;
    uint8_t match_index = 0u;
    bool bank_match = false;
    bool changed = false;
    burner_gb_mapper_t mapper = BURNER_GB_MAPPER_MBC5;
    esp_err_t err;

    if (file_name == NULL || file_name[0] == '\0' || id_out == NULL || device_size == NULL ||
        sector_size == NULL || buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(id_out, 0, 4u);
    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }

    burner_gbx_profile_clear(&s_cart_ctx.gbx);
    burner_nor_geometry_clear(&s_cart_ctx.geometry);
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_gb_mapper_kind = BURNER_GB_MAPPER_MBC5;

    err = burner_gbx_load_profile_by_file_name("DMG", file_name, &manual_profile);
    if (err != ESP_OK) {
        return err;
    }
    if (manual_profile.read_identifier.count == 0u ||
        !burner_gbc_gbx_runtime_supported(&manual_profile)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = burner_gbc_gbx_read_id_with_profile(&manual_profile, raw_id, &raw_id_len, &changed);
    if (err != ESP_OK) {
        return err;
    }

    match_len = burner_gbx_profile_match_id_ex(
        &manual_profile,
        raw_id,
        raw_id_len,
        &match_index,
        &bank_match);
    if (!changed || match_len == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBC GBX manual profile ID mismatch: file=%s changed=%u match_len=%u",
            manual_profile.file_name,
            changed ? 1u : 0u,
            (unsigned)match_len);
        return ESP_ERR_NOT_FOUND;
    }

    burner_gbx_profile_apply_match_name(&manual_profile, match_index, bank_match);
    memcpy(id_out, raw_id, (raw_id_len < 4u) ? raw_id_len : 4u);

    err = burner_gbc_gbx_apply_profile_geometry(
        &manual_profile,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok_out,
        cmdset_out);
    if (err != ESP_OK) {
        return err;
    }

    mapper = burner_gbc_gbx_mapper_from_profile(&manual_profile);
    err = burner_gbc_gbx_apply_mapper_limit(mapper, device_size, sector_size);
    if (err != ESP_OK) {
        return err;
    }

    s_cart_ctx.gbx = manual_profile;
    s_cart_ctx.gba_cmdset = manual_profile.base_cmdset;

    ESP_LOGI(
        BURNER_TAG,
        "GBC GBX manual profile prepared: file=%s chip=%s mapper=%s flash=%" PRIu32
        " sector=%" PRIu32 " buf=%u id=%02X %02X %02X %02X",
        s_cart_ctx.gbx.file_name,
        s_cart_ctx.gbx.display_name[0] != '\0' ? s_cart_ctx.gbx.display_name : "unknown",
        burner_gb_mapper_name(s_gb_mapper_kind),
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        id_out[0],
        id_out[1],
        id_out[2],
        id_out[3]);
    return ESP_OK;
}

esp_err_t burner_gbc_gbx_probe_locked(
    uint8_t id_out[4],
    burner_gbx_profile_t *profile_out,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out,
    burner_nor_cmdset_t *cmdset_out)
{
    burner_gbc_gbx_probe_ctx_t probe_ctx = {0};
    esp_err_t err;

    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(id_out, 0, 4u);
    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }
    if (profile_out != NULL) {
        burner_gbx_profile_clear(profile_out);
    }
    burner_gbx_profile_clear(&s_cart_ctx.gbx);
    burner_nor_geometry_clear(&s_cart_ctx.geometry);
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_gb_mapper_kind = BURNER_GB_MAPPER_MBC5;

    probe_ctx.profile = (burner_gbx_profile_t *)calloc(1u, sizeof(*probe_ctx.profile));
    if (probe_ctx.profile == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(BURNER_TAG, "GBC GBX probe: FlashGBX DMG profile scan starting");
    err = burner_gbx_visit_cached_methods_by_type("DMG", burner_gbc_gbx_probe_method_visitor, &probe_ctx);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        if (err == ESP_ERR_INVALID_VERSION) {
            ESP_LOGE(BURNER_TAG, "GBC GBX cache version mismatch; update GBX cache from system menu");
        }
        goto cleanup;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(BURNER_TAG, "GBC GBX cache unavailable or no cached DMG methods; update GBX cache from system menu");
    }
    if (!probe_ctx.found) {
        ESP_LOGE(BURNER_TAG, "GBC GBX probe failed: no FlashGBX DMG profile matched");
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    memcpy(id_out, probe_ctx.id, 4u);
    s_cart_ctx.gbx = *probe_ctx.profile;
    if (profile_out != NULL) {
        *profile_out = *probe_ctx.profile;
    }
    err = burner_gbc_gbx_apply_profile_geometry(
        probe_ctx.profile,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok_out,
        cmdset_out);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = burner_gbc_gbx_apply_mapper_limit(
        burner_gbc_gbx_mapper_from_profile(probe_ctx.profile),
        device_size,
        sector_size);
    if (err != ESP_OK) {
        goto cleanup;
    }
    s_cart_ctx.gba_cmdset = probe_ctx.profile->base_cmdset;

    ESP_LOGI(
        BURNER_TAG,
        "GBC GBX probe ok: file=%s chip=%s mapper=%s flash=%" PRIu32 " sector=%" PRIu32
        " buf=%u cmdset=%s id=%02X %02X %02X %02X",
        probe_ctx.profile->file_name,
        probe_ctx.profile->display_name[0] != '\0' ? probe_ctx.profile->display_name : "unknown",
        burner_gb_mapper_name(s_gb_mapper_kind),
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        probe_ctx.profile->command_set_name[0] != '\0' ? probe_ctx.profile->command_set_name : "unknown",
        id_out[0],
        id_out[1],
        id_out[2],
        id_out[3]);
    err = ESP_OK;

cleanup:
    free(probe_ctx.profile);
    return err;
}

esp_err_t burner_gbc_gbx_reset_to_read_mode(bool full_reset, uint32_t max_address)
{
    burner_gbc_gbx_exec_ctx_t ctx = {0};
    esp_err_t err;

    if (!burner_gbc_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
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
            (void)base;
            err = burner_gbc_gbx_execute_plain_sequence(
                &s_cart_ctx.gbx.reset,
                NULL,
                &ctx,
                BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
        }
    } else {
        err = burner_gbc_gbx_execute_plain_sequence(
            &s_cart_ctx.gbx.reset,
            NULL,
            &ctx,
            BURNER_ROM_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return burner_gbc_gbx_select_bank(0u);
}

esp_err_t burner_gbc_gbx_prepare(const burner_task_param_t *job)
{
    uint8_t id[4] = {0};
    char chip_name[48] = {0};
    uint32_t device_size = 0u;
    uint32_t sector_size = 0u;
    uint16_t buffer_write_bytes = 0u;
    bool cfi_ok = false;
    burner_nor_cmdset_t cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    esp_err_t err;

    if (job == NULL || job->total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (job->gbx_profile_file[0] != '\0') {
        err = burner_gbc_gbx_prepare_manual_profile(
            job->gbx_profile_file,
            id,
            &device_size,
            &sector_size,
            &buffer_write_bytes,
            &cfi_ok,
            &cmdset);
    } else {
        err = burner_gbc_gbx_probe_locked(
            id,
            NULL,
            &device_size,
            &sector_size,
            &buffer_write_bytes,
            &cfi_ok,
            &cmdset);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (job->total_bytes > device_size) {
        ESP_LOGE(
            BURNER_TAG,
            "GBC GBX ROM larger than flash: rom=%" PRIu32 " flash=%" PRIu32,
            job->total_bytes,
            device_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!burner_gbc_gbx_runtime_supported(&s_cart_ctx.gbx)) {
        ESP_LOGE(
            BURNER_TAG,
            "GBC GBX profile matched but runtime write commands are unavailable: file=%s",
            s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_cart_ctx.gbx.runtime_commands_enabled = true;
    s_cart_ctx.prepared = true;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_cart_ctx.buffer_write_bytes = buffer_write_bytes;
    s_cart_ctx.program_buffer_write_bytes = buffer_write_bytes;
    s_cart_ctx.sector_size = sector_size;
    s_cart_ctx.device_size = device_size;
    s_cart_ctx.probe_cfi_ok = cfi_ok;
    memcpy(s_cart_ctx.mbc5_id, id, sizeof(s_cart_ctx.mbc5_id));

    burner_nor_format_chip_name(
        chip_name,
        sizeof(chip_name),
        s_cart_ctx.gbx.display_name[0] != '\0' ? s_cart_ctx.gbx.display_name : "GBC GBX flash",
        cmdset,
        device_size);
    ESP_LOGI(
        BURNER_TAG,
        "GBC GBX identify-only prepared: chip=%s flash=%" PRIu32 " sector=%" PRIu32
        " geom=%s largest=%" PRIu32 " regions=%u buf=%u profile=%s start=0x%08" PRIX32 " first_bank=%" PRIu32,
        chip_name,
        device_size,
        sector_size,
        burner_nor_geometry_is_uniform(&s_cart_ctx.geometry) ? "uniform" : "mixed",
        burner_nor_geometry_largest_sector_size(&s_cart_ctx.geometry),
        (unsigned)s_cart_ctx.geometry.region_count,
        (unsigned)buffer_write_bytes,
        s_cart_ctx.gbx.file_name,
        burner_gbc_gbx_start_addr(),
        burner_gbc_gbx_first_bank());

    burner_status_set_probe_info(
        BURNER_CART_MODE_MBC5,
        id,
        sizeof(id),
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok,
        false,
        false,
        false,
        false,
        chip_name,
        burner_gb_mapper_name(s_gb_mapper_kind));
    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_single_write_byte(uint16_t cart_addr, uint8_t value, uint32_t timeout_ms)
{
    burner_gbc_gbx_exec_ctx_t ctx = {
        .sector_addr = cart_addr,
        .program_addr = cart_addr,
        .program_data = value,
        .last_program_data = value,
    };

    if (s_cart_ctx.gbx.single_write.count == 0u) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    for (uint32_t i = 0u; i < s_cart_ctx.gbx.single_write.count; ++i) {
        const burner_gbx_cmd_step_t *step = &s_cart_ctx.gbx.single_write.steps[i];

        if (step->addr_kind != BURNER_GBX_ADDR_NONE && step->data_kind != BURNER_GBX_DATA_NONE) {
            uint16_t addr = 0u;
            uint16_t resolved = 0u;
            uint8_t byte_value;
            esp_err_t err;

            err = burner_gbc_gbx_resolve_byte_addr(&ctx, step->addr_kind, step->addr_value, &addr);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_gbc_gbx_expected_value(&ctx, step->data_kind, step->data_value, &resolved);
            if (err != ESP_OK) {
                return err;
            }
            byte_value = (uint8_t)(resolved & 0xFFu);
            err = burner_bacon_gbc_write(addr, &byte_value, 1u);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (i < s_cart_ctx.gbx.single_write_wait_for.count) {
            esp_err_t err = burner_gbc_gbx_wait_step(
                &ctx,
                &s_cart_ctx.gbx.single_write_wait_for.steps[i],
                timeout_ms);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t burner_gbc_gbx_buffer_write_chunk(uint16_t cart_addr, const uint8_t *buf, size_t write_len)
{
    burner_gbc_gbx_exec_ctx_t ctx;

    if (buf == NULL || write_len == 0u || write_len > 65536u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cart_ctx.gbx.buffer_write.count == 0u) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.sector_addr = cart_addr;
    ctx.program_addr = cart_addr;
    ctx.buffer_size_value = (uint16_t)(write_len - 1u);
    ctx.last_program_data = buf[write_len - 1u];

    for (uint32_t i = 0u; i < s_cart_ctx.gbx.buffer_write.count; ++i) {
        const burner_gbx_cmd_step_t *step = &s_cart_ctx.gbx.buffer_write.steps[i];

        if (step->addr_kind != BURNER_GBX_ADDR_NONE && step->data_kind != BURNER_GBX_DATA_NONE) {
            uint16_t addr = 0u;
            uint16_t resolved = 0u;
            uint8_t byte_value;
            esp_err_t err;

            err = burner_gbc_gbx_resolve_byte_addr(&ctx, step->addr_kind, step->addr_value, &addr);
            if (err != ESP_OK) {
                return err;
            }
            err = burner_gbc_gbx_expected_value(&ctx, step->data_kind, step->data_value, &resolved);
            if (err != ESP_OK) {
                return err;
            }
            if (step->data_kind == BURNER_GBX_DATA_PD) {
                err = burner_bacon_gbc_write(addr, buf, write_len);
            } else {
                byte_value = (uint8_t)(resolved & 0xFFu);
                err = burner_bacon_gbc_write(addr, &byte_value, 1u);
            }
            if (err != ESP_OK) {
                return err;
            }
        }

        if (i < s_cart_ctx.gbx.buffer_write_wait_for.count) {
            esp_err_t err = burner_gbc_gbx_wait_step(
                &ctx,
                &s_cart_ctx.gbx.buffer_write_wait_for.steps[i],
                BURNER_ROM_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

esp_err_t burner_gbc_gbx_program_block(const uint8_t *data, size_t len, uint32_t offset)
{
    size_t programmed = 0u;
    uint16_t buffer_bytes = s_cart_ctx.program_buffer_write_bytes;

    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_gbc_gbx_is_active() || !s_cart_ctx.prepared) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_ctx.gbx.single_write.count == 0u && s_cart_ctx.gbx.buffer_write.count == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBC GBX profile missing write commands: file=%s",
            s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }

    while (programmed < len) {
        uint32_t logical_addr = offset + (uint32_t)programmed;
        uint32_t bank_remain = 0u;
        uint32_t sector_end = 0u;
        uint16_t cart_addr = 0u;
        size_t remain = len - programmed;
        size_t chunk = remain;
        esp_err_t err;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_gbc_gbx_logical_to_cart_addr(logical_addr, &cart_addr, &bank_remain);
        if (err != ESP_OK) {
            return err;
        }
        if (chunk > (size_t)bank_remain) {
            chunk = (size_t)bank_remain;
        }
        if (burner_nor_geometry_sector_bounds(&s_cart_ctx.geometry, logical_addr, NULL, &sector_end, NULL) == ESP_OK &&
            sector_end > logical_addr &&
            chunk > (size_t)(sector_end - logical_addr)) {
            chunk = (size_t)(sector_end - logical_addr);
        }

        if (buffer_bytes > 0u && s_cart_ctx.gbx.buffer_write.count > 0u) {
            if (chunk > (size_t)buffer_bytes) {
                chunk = (size_t)buffer_bytes;
            }
            err = burner_gbc_gbx_buffer_write_chunk(cart_addr, data + programmed, chunk);
            if (err == ESP_OK) {
                burner_status_record_mbc5_buffer_write(false);
            }
        } else {
            chunk = (chunk == 0u) ? 1u : chunk;
            for (size_t off = 0u; off < chunk; ++off) {
                err = burner_gbc_gbx_single_write_byte(
                    (uint16_t)(cart_addr + (uint16_t)off),
                    data[programmed + off],
                    BURNER_ROM_POLL_TIMEOUT_MS);
                if (err != ESP_OK) {
                    return err;
                }
            }
        }
        if (err != ESP_OK) {
            return err;
        }

        programmed += chunk;
        burner_task_yield_if_due();
    }
    return ESP_OK;
}

esp_err_t burner_gbc_gbx_erase_sector(uint32_t flash_addr, uint32_t timeout_ms)
{
    uint16_t cart_addr = 0u;
    burner_gbc_gbx_exec_ctx_t ctx = {0};
    esp_err_t err;

    if (!burner_gbc_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_ctx.gbx.sector_erase.count == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBC GBX profile missing sector erase commands: file=%s",
            s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = burner_gbc_gbx_logical_to_cart_addr(flash_addr, &cart_addr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    ctx.sector_addr = cart_addr;
    ctx.program_addr = cart_addr;
    return burner_gbc_gbx_execute_plain_sequence(
        &s_cart_ctx.gbx.sector_erase,
        &s_cart_ctx.gbx.sector_erase_wait_for,
        &ctx,
        timeout_ms);
}

esp_err_t burner_gbc_gbx_chip_erase_once(void)
{
    burner_gbc_gbx_exec_ctx_t ctx = {0};
    uint32_t timeout_ms = s_cart_ctx.gbx.has_chip_erase_timeout ? s_cart_ctx.gbx.chip_erase_timeout_ms
                                                                : BURNER_ROM_CHIP_ERASE_TIMEOUT_MS;

    if (!burner_gbc_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_ctx.gbx.chip_erase.count == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBC GBX profile missing chip erase commands: file=%s",
            s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return burner_gbc_gbx_execute_plain_sequence(
        &s_cart_ctx.gbx.chip_erase,
        &s_cart_ctx.gbx.chip_erase_wait_for,
        &ctx,
        timeout_ms);
}

static esp_err_t burner_bacon_mbc5_erase_sector(uint32_t flash_addr, uint32_t timeout_ms)
{
    uint16_t bank = 0u;
    uint16_t cart_addr = 0u;
    uint8_t cmd;
    esp_err_t err;

    if (burner_gbc_gbx_is_active()) {
        return burner_gbc_gbx_erase_sector(flash_addr, timeout_ms);
    }

    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
    }

    burner_mbc5_addr_to_program_window(flash_addr, &bank, &cart_addr, NULL);
    err = burner_bacon_mbc5_switch_bank(bank);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = bank;

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x80u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x30u;
    err = burner_bacon_gbc_write(cart_addr, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_bacon_mbc5_amd_wait_erase_complete(cart_addr, timeout_ms, NULL);
    if (err == ESP_ERR_TIMEOUT) {
        uint8_t read_back = 0u;
        (void)burner_bacon_gbc_read_u8(cart_addr, &read_back);
        ESP_LOGW(
            BURNER_TAG,
            "MBC5 erase timeout flash=0x%08" PRIX32 " bank=%u cart_addr=0x%04X sector=%" PRIu32
            " status=0x%02X timeout=%ums",
            flash_addr,
            (unsigned)bank,
            (unsigned)cart_addr,
            burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry),
            (unsigned)read_back,
            (unsigned)timeout_ms);
    }
    return err;
}

static esp_err_t burner_buffer_all_ff(const uint8_t *buf, size_t len, bool *all_ff_out)
{
    if (buf == NULL || all_ff_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != 0xFFu) {
            *all_ff_out = false;
            return ESP_OK;
        }
    }
    *all_ff_out = true;
    return ESP_OK;
}

static bool burner_blank_sample_offset_seen(const uint32_t *offsets, size_t count, uint32_t offset)
{
    for (size_t i = 0u; i < count; ++i) {
        if (offsets[i] == offset) {
            return true;
        }
    }
    return false;
}

static size_t burner_build_blank_sample_offsets(
    uint32_t region_size,
    size_t sample_len,
    uint32_t align_mask,
    uint32_t offsets[BURN_BLANK_SAMPLE_POINTS])
{
    uint32_t candidates[BURN_BLANK_SAMPLE_POINTS];
    uint32_t max_offset;
    size_t count = 0u;

    if (offsets == NULL || region_size == 0u || sample_len == 0u) {
        return 0u;
    }

    max_offset = (region_size > (uint32_t)sample_len) ? (region_size - (uint32_t)sample_len) : 0u;
    candidates[0] = 0u;
    candidates[1] = (uint32_t)(((uint64_t)max_offset * 30u) / 100u);
    candidates[2] = (uint32_t)(((uint64_t)max_offset * 70u) / 100u);
    candidates[3] = max_offset;

    for (size_t i = 0u; i < BURN_BLANK_SAMPLE_POINTS; ++i) {
        uint32_t offset = candidates[i];

        if (align_mask != 0u) {
            offset &= ~align_mask;
        }
        if (!burner_blank_sample_offset_seen(offsets, count, offset)) {
            offsets[count++] = offset;
        }
    }

    return count;
}

static esp_err_t burner_mbc5_region_is_blank_sampled(
    uint32_t region_addr,
    uint32_t region_size,
    bool *blank_out)
{
    uint8_t sample_buf[BURN_BLANK_SAMPLE_BYTES];
    uint32_t sample_offsets[BURN_BLANK_SAMPLE_POINTS];
    size_t sample_len;
    size_t sample_count;
    esp_err_t err;

    if (blank_out == NULL || region_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    sample_len = (region_size < BURN_BLANK_SAMPLE_BYTES) ? (size_t)region_size : (size_t)BURN_BLANK_SAMPLE_BYTES;
    sample_count = burner_build_blank_sample_offsets(region_size, sample_len, 0u, sample_offsets);
    if (sample_count == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    *blank_out = true;
    for (size_t i = 0u; i < sample_count; ++i) {
        bool chunk_blank = false;

        err = burner_bacon_mbc5_read_block_program_window(
            sample_buf,
            sample_len,
            region_addr + sample_offsets[i]);
        if (err != ESP_OK) {
            return err;
        }
        err = burner_buffer_all_ff(sample_buf, sample_len, &chunk_blank);
        if (err != ESP_OK) {
            return err;
        }
        if (!chunk_blank) {
            *blank_out = false;
            return ESP_OK;
        }
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

static esp_err_t burner_mbc5_sector_is_blank(
    uint32_t sector_addr,
    uint32_t sector_size,
    bool *blank_out)
{
    return burner_mbc5_region_is_blank_sampled(sector_addr, sector_size, blank_out);
}

static esp_err_t burner_bacon_mbc5_erase_range(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool sample_blank_sectors,
    bool erase_always)
{
    const burner_nor_geometry_t *geometry = &s_cart_ctx.geometry;
    burner_nor_region_cursor_t cursor = {0};
    uint32_t sector_addr = 0u;
    uint32_t stop_sector_addr = 0u;
    uint32_t skipped_blank = 0u;
    uint32_t erased = 0u;
    uint32_t erase_bytes;
    uint32_t timeout_ms;
    int64_t erase_deadline_us;
    esp_err_t err = ESP_OK;

    (void)sector_size;
    if (!burner_nor_geometry_is_valid(geometry) || addr_end < addr_begin) {
        return ESP_ERR_INVALID_ARG;
    }
    if (burner_nor_geometry_region_cursor_begin(geometry, addr_begin, &cursor) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    err = burner_nor_geometry_sector_bounds_in_cursor(&cursor, addr_begin, &stop_sector_addr, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_nor_geometry_region_cursor_begin(geometry, addr_end, &cursor);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    err = burner_nor_geometry_sector_bounds_in_cursor(&cursor, addr_end, &sector_addr, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    erase_bytes = burner_nor_geometry_erase_bytes_from_range(geometry, addr_begin, addr_end);
    if (erase_bytes == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }
    timeout_ms = burner_erase_timeout_ms_for_bytes(erase_bytes);
    erase_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    {
        uint16_t first_bank = 0u;
        uint16_t first_cart_addr = 0u;

        burner_mbc5_addr_to_program_window(sector_addr, &first_bank, &first_cart_addr, NULL);
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 erase plan: range=0x%08" PRIX32 "-0x%08" PRIX32
            " first_sector=0x%08" PRIX32 " first_bank=%u first_cart=0x%04X bytes=%" PRIu32
            " timeout=%" PRIu32 "ms geom=%s largest=%" PRIu32 " regions=%u",
            addr_begin,
            addr_end,
            sector_addr,
            (unsigned)first_bank,
            (unsigned)first_cart_addr,
            erase_bytes,
            timeout_ms,
            burner_nor_geometry_is_uniform(geometry) ? "uniform" : "mixed",
            burner_nor_geometry_largest_sector_size(geometry),
            (unsigned)geometry->region_count);
    }

    while (true) {
        uint32_t current_sector_size = 0u;
        uint16_t sector_bank = 0u;
        uint16_t sector_cart_addr = 0u;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            goto erase_range_out;
        }
        err = burner_nor_geometry_region_cursor_seek_forward(geometry, sector_addr, &cursor);
        if (err != ESP_OK) {
            goto erase_range_out;
        }
        err = burner_nor_geometry_sector_bounds_in_cursor(
            &cursor,
            sector_addr,
            &sector_addr,
            NULL,
            &current_sector_size);
        if (err != ESP_OK || current_sector_size == 0u) {
            err = (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
            goto erase_range_out;
        }

        burner_mbc5_addr_to_program_window(sector_addr, &sector_bank, &sector_cart_addr, NULL);
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 erase sector: flash=0x%08" PRIX32 " size=%" PRIu32 " bank=%u cart=0x%04X",
            sector_addr,
            current_sector_size,
            (unsigned)sector_bank,
            (unsigned)sector_cart_addr);
        if (sample_blank_sectors && !erase_always) {
            bool blank = false;

            err = burner_mbc5_sector_is_blank(
                sector_addr,
                current_sector_size,
                &blank);
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            if (blank) {
                skipped_blank++;
                burner_status_advance_erase_phase(1u, current_sector_size);
            } else {
                err = burner_bacon_mbc5_erase_sector(
                    sector_addr,
                    burner_erase_remaining_timeout_ms(erase_deadline_us));
                if (err != ESP_OK) {
                    goto erase_range_out;
                }
                erased++;
                burner_status_advance_erase_phase(1u, current_sector_size);
            }
        } else {
            err = burner_bacon_mbc5_erase_sector(
                sector_addr,
                burner_erase_remaining_timeout_ms(erase_deadline_us));
            if (err != ESP_OK) {
                goto erase_range_out;
            }
            erased++;
            burner_status_advance_erase_phase(1u, current_sector_size);
        }
        if (sector_addr == stop_sector_addr) {
            break;
        }
        if (sector_addr == 0u) {
            err = ESP_ERR_INVALID_SIZE;
            goto erase_range_out;
        }
        err = burner_nor_geometry_region_cursor_begin(geometry, sector_addr - 1u, &cursor);
        if (err != ESP_OK) {
            goto erase_range_out;
        }
        err = burner_nor_geometry_sector_bounds_in_cursor(&cursor, sector_addr - 1u, &sector_addr, NULL, NULL);
        if (err != ESP_OK) {
            goto erase_range_out;
        }
    }

erase_range_out:
    if (err == ESP_OK && sample_blank_sectors && !erase_always &&
        (erased > 0u || skipped_blank > 0u)) {
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 erase sector-sample: 4x2B erased=%" PRIu32 " skipped_blank=%" PRIu32,
            erased,
            skipped_blank);
    }
    return err;
}

static esp_err_t burner_bacon_mbc5_chip_erase(void)
{
    uint8_t cmd;
    uint8_t read_back = 0u;
    int64_t deadline_us;
    esp_err_t err;

    if (burner_gbc_gbx_is_active()) {
        return burner_gbc_gbx_chip_erase_once();
    }

    err = burner_bacon_mbc5_switch_bank(0u);
    if (err != ESP_OK) {
        return err;
    }
    s_cart_ctx.current_bank = 0u;

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x80u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    cmd = 0x10u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    deadline_us = esp_timer_get_time() + ((int64_t)BURNER_ROM_CHIP_ERASE_TIMEOUT_MS * 1000);
    while (esp_timer_get_time() < deadline_us) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_mbc5_amd_wait_erase_complete(
            0x0000u,
            burner_erase_remaining_timeout_ms(deadline_us),
            &read_back);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_TIMEOUT) {
            return err;
        }
        if ((read_back & 0x20u) != 0u) {
            ESP_LOGW(
                BURNER_TAG,
                "MBC5 chip erase timeout status=0x%02X timeout=%ums",
                (unsigned)read_back,
                (unsigned)BURNER_ROM_CHIP_ERASE_TIMEOUT_MS);
            return err;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t burner_bacon_gbc_rom_program(
    uint16_t cart_addr,
    const uint8_t *buf,
    size_t len,
    uint16_t buffer_write_bytes)
{
    size_t i = 0;
    esp_err_t err;

    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (i < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint16_t start_addr = (uint16_t)(cart_addr + (uint16_t)i);

        if (buffer_write_bytes == 0u) {
            uint8_t seq[24];

            seq[0] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[1] = 0xAAu;
            seq[2] = 0x0Au;
            seq[3] = 0xAAu;
            seq[4] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[5] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[6] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[7] = 0x55u;
            seq[8] = 0x05u;
            seq[9] = 0x55u;
            seq[10] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[11] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[12] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[13] = 0xAAu;
            seq[14] = 0x0Au;
            seq[15] = 0xA0u;
            seq[16] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[17] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
            seq[18] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
            seq[19] = (uint8_t)(start_addr & 0xFFu);
            seq[20] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[21] = buf[i];
            seq[22] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
            seq[23] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

            err = burner_spi_transfer(seq, NULL, sizeof(seq));
            if (err != ESP_OK) {
                return err;
            }
            err = burner_bacon_mbc5_amd_wait_program_complete(
                start_addr,
                buf[i],
                BURNER_ROM_POLL_TIMEOUT_MS,
                NULL);
            if (err != ESP_OK) {
                return err;
            }
            i += 1u;
        } else {
            size_t wr_buf_cnt;
            size_t write_len = len - i;
            size_t seq_len;
            uint8_t *seq;
            uint8_t last_expected;
            uint16_t last_addr;
            bool fallback_to_single = false;

            if (write_len > buffer_write_bytes) {
                write_len = buffer_write_bytes;
            }

            seq_len = 25u + 3u * write_len;
            if (seq_len > BURNER_SPI_MAX_XFER) {
                return ESP_ERR_INVALID_SIZE;
            }

            seq = (uint8_t *)malloc(seq_len);
            if (seq == NULL) {
                return ESP_ERR_NO_MEM;
            }

            seq[0] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[1] = 0xAAu;
            seq[2] = 0xAAu;
            seq[3] = 0x0Au;
            seq[4] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[5] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[6] = 0x55u;
            seq[7] = 0x55u;
            seq[8] = 0x05u;
            seq[9] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[10] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[11] = 0x25u;
            seq[12] = (uint8_t)(start_addr & 0xFFu);
            seq[13] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[14] = burner_bacon_option_byte2(0, true, true, false, true, true, true);
            seq[15] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[16] = (uint8_t)(write_len - 1u);
            seq[17] = (uint8_t)(start_addr & 0xFFu);
            seq[18] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[19] = burner_bacon_option_byte2(0, true, true, false, true, true, true);

            for (wr_buf_cnt = 0; wr_buf_cnt < write_len; ++wr_buf_cnt) {
                size_t base = 20u + 3u * wr_buf_cnt;

                seq[base + 0u] = burner_bacon_option_byte2(1, true, true, false, false, true, false);
                seq[base + 1u] = buf[i + wr_buf_cnt];
                seq[base + 2u] = burner_bacon_option_byte2(0, true, true, true, false, true, true);
            }

            seq[20u + 3u * write_len + 0u] = burner_bacon_option_byte2(3, true, true, false, false, true, false);
            seq[20u + 3u * write_len + 1u] = 0x29u;
            seq[20u + 3u * write_len + 2u] = (uint8_t)(start_addr & 0xFFu);
            seq[20u + 3u * write_len + 3u] = (uint8_t)((start_addr >> 8) & 0xFFu);
            seq[20u + 3u * write_len + 4u] = burner_bacon_option_byte2(0, true, true, false, true, true, true);

            err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, seq, NULL, seq_len);
            free(seq);
            if (err != ESP_OK) {
                fallback_to_single = true;
            }

            if (!fallback_to_single) {
                last_addr = (uint16_t)(start_addr + (uint16_t)write_len - 1u);
                last_expected = buf[i + write_len - 1u];
                err = burner_bacon_mbc5_amd_wait_program_complete(
                    last_addr,
                    last_expected,
                    BURNER_ROM_POLL_TIMEOUT_MS,
                    NULL);
                if (err != ESP_OK) {
                    fallback_to_single = true;
                }
            }

            if (fallback_to_single) {
                size_t single_i;
                burner_status_record_mbc5_buffer_write(true);
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 buffer write fallback -> single-byte at 0x%04X len=%u",
                    (unsigned)start_addr,
                    (unsigned)write_len);
                for (single_i = 0u; single_i < write_len; ++single_i) {
                    uint16_t single_addr = (uint16_t)(start_addr + (uint16_t)single_i);
                    uint8_t seq_single[24];

                    seq_single[0] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[1] = 0xAAu;
                    seq_single[2] = 0x0Au;
                    seq_single[3] = 0xAAu;
                    seq_single[4] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[5] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[6] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[7] = 0x55u;
                    seq_single[8] = 0x05u;
                    seq_single[9] = 0x55u;
                    seq_single[10] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[11] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[12] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[13] = 0xAAu;
                    seq_single[14] = 0x0Au;
                    seq_single[15] = 0xA0u;
                    seq_single[16] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[17] = burner_bacon_option_byte0(0, true, true, true, true, true, true);
                    seq_single[18] = burner_bacon_option_byte0(3, true, true, true, false, true, true);
                    seq_single[19] = (uint8_t)(single_addr & 0xFFu);
                    seq_single[20] = (uint8_t)((single_addr >> 8) & 0xFFu);
                    seq_single[21] = buf[i + single_i];
                    seq_single[22] = burner_bacon_option_byte0(0, true, true, true, false, true, false);
                    seq_single[23] = burner_bacon_option_byte0(0, true, true, true, true, true, true);

                    err = burner_spi_transfer(seq_single, NULL, sizeof(seq_single));
                    if (err != ESP_OK) {
                        return err;
                    }
                    err = burner_bacon_mbc5_amd_wait_program_complete(
                        single_addr,
                        buf[i + single_i],
                        BURNER_ROM_POLL_TIMEOUT_MS,
                        NULL);
                    if (err != ESP_OK) {
                        return err;
                    }
                    burner_task_yield_if_due();
                }
            } else {
                burner_status_record_mbc5_buffer_write(false);
            }

            i += write_len;
            burner_task_yield_if_due();
        }
    }

    return ESP_OK;
}


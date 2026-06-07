/* GBA GBX command path helpers and probe cache. */

#include "esp_attr.h"

typedef struct {
    uint32_t sector_addr;
    uint32_t program_addr;
    uint32_t bank_base;
    uint16_t program_data;
    uint16_t last_program_data;
    uint16_t buffer_size_value;
} burner_gba_gbx_exec_ctx_t;

typedef struct {
    burner_nor_cmdset_t cmdset;
    uint32_t flash_size;
    uint32_t sector_size;
    const burner_nor_geometry_t *geometry;
    bool cfi_ok;
    bool d0d1_known;
    bool d0d1_swapped;
    bool found;
    bool ambiguous;
    size_t best_match_len;
    uint32_t best_method_specificity;
    uint32_t method_results;
    burner_gbx_profile_t *profile_out;
    burner_gbx_profile_t *candidate_profile;
    uint8_t id[BURNER_GBX_FLASH_ID_LEN_MAX];
} burner_gba_gbx_probe_ctx_t;

typedef struct {
    bool valid;
    uint64_t stored_at_us;
    bool gbx_profile_matched;
    uint8_t id[8];
    uint32_t device_size;
    uint32_t sector_size;
    uint16_t buffer_write_bytes;
    bool cfi_ok;
    burner_cart_ctx_t cart_ctx;
} burner_gba_gbx_cached_probe_t;

#define BURNER_GBA_GBX_CACHED_PROBE_ID_LEN 8u
#define BURNER_GBA_GBX_CACHED_PROBE_MAX_AGE_US (10ULL * 60ULL * 1000ULL * 1000ULL)

static EXT_RAM_BSS_ATTR burner_gba_gbx_cached_probe_t s_gba_gbx_cached_probe;

void burner_gba_gbx_cache_probe_result(
    const uint8_t id[8],
    uint32_t device_size,
    uint32_t sector_size,
    uint16_t buffer_write_bytes,
    bool cfi_ok,
    bool gbx_profile_matched);
void burner_gba_gbx_clear_cached_probe(void);
bool burner_gba_gbx_take_cached_probe(
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out,
    bool *gbx_profile_matched_out);
esp_err_t burner_gba_gbx_reset_to_read_mode(bool full_reset, bool is_multi_card, uint32_t max_address);

#define BURNER_GBA_GBX_SWAP_D0D1_U16(data) \
    (((data) & 0xFFFCu) | (((data) & 0x0001u) << 1) | (((data) & 0x0002u) >> 1))

static inline uint16_t burner_gba_gbx_apply_d0d1_swap_on_read(uint16_t data, bool is_swapped)
{
    return is_swapped ? BURNER_GBA_GBX_SWAP_D0D1_U16(data) : data;
}

bool burner_gba_gbx_is_active(void)
{
    return s_cart_ctx.gbx.active && s_cart_ctx.gbx.runtime_commands_enabled;
}

static bool burner_gba_gbx_runtime_supported(const burner_gbx_profile_t *profile)
{
    if (profile == NULL) {
        return false;
    }
    if (profile->type[0] != '\0' && strcasecmp(profile->type, "AGB") != 0) {
        return false;
    }
    if (profile->has_flash_bank_select_type && profile->flash_bank_select_type > 1u) {
        return false;
    }
    return profile->single_write.count > 0u || profile->buffer_write.count > 0u;
}

void burner_gba_gbx_cache_probe_result(
    const uint8_t id[8],
    uint32_t device_size,
    uint32_t sector_size,
    uint16_t buffer_write_bytes,
    bool cfi_ok,
    bool gbx_profile_matched)
{
    if (id == NULL) {
        burner_gba_gbx_clear_cached_probe();
        return;
    }

    memset(&s_gba_gbx_cached_probe, 0, sizeof(s_gba_gbx_cached_probe));
    s_gba_gbx_cached_probe.valid = true;
    s_gba_gbx_cached_probe.stored_at_us = (uint64_t)esp_timer_get_time();
    s_gba_gbx_cached_probe.gbx_profile_matched = gbx_profile_matched;
    memcpy(s_gba_gbx_cached_probe.id, id, BURNER_GBA_GBX_CACHED_PROBE_ID_LEN);
    s_gba_gbx_cached_probe.device_size = device_size;
    s_gba_gbx_cached_probe.sector_size = sector_size;
    s_gba_gbx_cached_probe.buffer_write_bytes = buffer_write_bytes;
    s_gba_gbx_cached_probe.cfi_ok = cfi_ok;
    s_gba_gbx_cached_probe.cart_ctx = s_cart_ctx;
    s_gba_gbx_cached_probe.cart_ctx.prepared = false;
    s_gba_gbx_cached_probe.cart_ctx.current_bank = UINT16_MAX;
    s_gba_gbx_cached_probe.cart_ctx.program_buffer_write_bytes = 0u;

    ESP_LOGI(
        BURNER_TAG,
        "GBA GBX cached probe stored: matched=%u file=%s flash=%" PRIu32 " sector=%" PRIu32
        " buf=%u cfi=%s id=%02X %02X %02X %02X %02X %02X %02X %02X",
        gbx_profile_matched ? 1u : 0u,
        s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown",
        device_size,
        sector_size,
        (unsigned)buffer_write_bytes,
        cfi_ok ? "ok" : "unavailable",
        id[0],
        id[1],
        id[2],
        id[3],
        id[4],
        id[5],
        id[6],
        id[7]);
}

void burner_gba_gbx_clear_cached_probe(void)
{
    memset(&s_gba_gbx_cached_probe, 0, sizeof(s_gba_gbx_cached_probe));
}

bool burner_gba_gbx_take_cached_probe(
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out,
    bool *gbx_profile_matched_out)
{
    burner_gba_gbx_cached_probe_t cached_probe;
    uint64_t now_us;

    if (id_out == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || cfi_ok_out == NULL || gbx_profile_matched_out == NULL ||
        !s_gba_gbx_cached_probe.valid) {
        return false;
    }
    now_us = (uint64_t)esp_timer_get_time();
    if (now_us < s_gba_gbx_cached_probe.stored_at_us ||
        (now_us - s_gba_gbx_cached_probe.stored_at_us) > BURNER_GBA_GBX_CACHED_PROBE_MAX_AGE_US) {
        uint64_t age_us = (now_us >= s_gba_gbx_cached_probe.stored_at_us)
                              ? (now_us - s_gba_gbx_cached_probe.stored_at_us)
                              : 0u;
        ESP_LOGW(
            BURNER_TAG,
            "GBA GBX cached probe expired: age=%" PRIu64 "ms max=%" PRIu64 "ms",
            age_us / 1000u,
            (uint64_t)(BURNER_GBA_GBX_CACHED_PROBE_MAX_AGE_US / 1000u));
        burner_gba_gbx_clear_cached_probe();
        return false;
    }

    cached_probe = s_gba_gbx_cached_probe;
    burner_gba_gbx_clear_cached_probe();

    s_cart_ctx = cached_probe.cart_ctx;
    memcpy(id_out, cached_probe.id, BURNER_GBA_GBX_CACHED_PROBE_ID_LEN);
    *device_size = cached_probe.device_size;
    *sector_size = cached_probe.sector_size;
    *buffer_write_bytes = cached_probe.buffer_write_bytes;
    *cfi_ok_out = cached_probe.cfi_ok;
    *gbx_profile_matched_out = cached_probe.gbx_profile_matched;

    ESP_LOGI(
        BURNER_TAG,
        "GBA GBX prepare reused cached probe: matched=%u file=%s flash=%" PRIu32 " sector=%" PRIu32
        " buf=%u cfi=%s id=%02X %02X %02X %02X %02X %02X %02X %02X",
        *gbx_profile_matched_out ? 1u : 0u,
        s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown",
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        *cfi_ok_out ? "ok" : "unavailable",
        id_out[0],
        id_out[1],
        id_out[2],
        id_out[3],
        id_out[4],
        id_out[5],
        id_out[6],
        id_out[7]);
    return true;
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

static uint32_t burner_gba_gbx_bank_base_for_addr(uint32_t byte_addr)
{
    return byte_addr - (byte_addr % BURN_GBA_BANK_BYTES);
}

static esp_err_t burner_gba_gbx_switch_bank_if_needed(uint32_t bank)
{
    esp_err_t err;
    uint64_t bank_start_us;

    if (bank > UINT8_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (bank == (uint32_t)s_cart_ctx.current_bank) {
        return ESP_OK;
    }

    bank_start_us = burner_gba_diag_now_us();
    err = burner_gba_gbx_reset_to_read_mode(true, false, BURN_GBA_BANK_BYTES);
    if (err != ESP_OK) {
        burner_gba_chis_diag_add_bank_switch(burner_gba_diag_now_us() - bank_start_us);
        return err;
    }
    err = burner_bacon_gba_rom_switch_bank((uint8_t)bank);
    if (err != ESP_OK) {
        burner_gba_chis_diag_add_bank_switch(burner_gba_diag_now_us() - bank_start_us);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BURNER_GBA_BANK_SWITCH_SETTLE_MS));
    s_cart_ctx.current_bank = (uint16_t)bank;
    burner_gba_chis_diag_add_bank_switch(burner_gba_diag_now_us() - bank_start_us);
    return ESP_OK;
}

static size_t burner_gba_gbx_first_flash_id_len(const burner_gbx_profile_t *profile)
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
        esp_err_t err = burner_gba_gbx_switch_bank_if_needed(bank);
        if (err != ESP_OK) {
            return err;
        }
        local_byte_addr = abs_byte_addr % BURN_GBA_BANK_BYTES;
    }

    *local_byte_addr_out = local_byte_addr;
    return ESP_OK;
}

static esp_err_t burner_gba_gbx_read_chip_bytes(uint32_t byte_addr, uint8_t *out, size_t len, bool is_multi_card)
{
    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0u; i < len;) {
        uint32_t abs_byte_addr = byte_addr + (uint32_t)i;
        uint32_t aligned_abs_byte_addr = abs_byte_addr & ~1u;
        uint32_t local_byte_addr = 0u;
        uint16_t word = 0u;
        esp_err_t err = burner_gba_gbx_prepare_bank_addr(aligned_abs_byte_addr, is_multi_card, &local_byte_addr);

        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(local_byte_addr >> 1, &word);
        if (err != ESP_OK) {
            return err;
        }
        word = burner_gba_gbx_apply_d0d1_swap_on_read(word, s_cart_ctx.d0d1_swapped);
        if ((abs_byte_addr & 0x1u) == 0u) {
            out[i++] = (uint8_t)(word & 0xFFu);
            if (i < len) {
                out[i++] = (uint8_t)((word >> 8) & 0xFFu);
            }
        } else {
            out[i++] = (uint8_t)((word >> 8) & 0xFFu);
        }
    }
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
        *abs_byte_addr_out = ctx->bank_base + base_offset + addr_value;
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
    if (timeout_ms == 0u) {
        return ESP_ERR_TIMEOUT;
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
        err = burner_bacon_rom_read_u16(local_byte_addr >> 1, &read_back);
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

static esp_err_t burner_gba_gbx_reset_profile_to_read_mode(const burner_gbx_profile_t *profile)
{
    burner_gba_gbx_exec_ctx_t ctx = {0};
    burner_nor_cmdset_t cmdset = BURNER_NOR_CMDSET_UNKNOWN;

    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (profile->reset.count > 0u) {
        return burner_gba_gbx_execute_plain_sequence(
            &profile->reset,
            NULL,
            &ctx,
            false,
            0u,
            BURNER_ROM_POLL_TIMEOUT_MS);
    }
    if (profile->base_cmdset != BURNER_NOR_CMDSET_UNKNOWN) {
        return burner_bacon_gba_reset_to_read_mode_for_cmdset(profile->base_cmdset);
    }
    cmdset = (s_cart_ctx.gba_cmdset != BURNER_NOR_CMDSET_UNKNOWN) ? s_cart_ctx.gba_cmdset : profile->base_cmdset;
    return burner_bacon_gba_reset_to_read_mode_for_cmdset(cmdset);
}

static esp_err_t burner_gba_gbx_enter_cfi_with_profile(const burner_gbx_profile_t *profile)
{
    burner_gba_gbx_exec_ctx_t ctx = {0};
    esp_err_t err;

    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_gba_gbx_reset_profile_to_read_mode(profile);
    if (err != ESP_OK) {
        return err;
    }

    if (profile->read_cfi.count > 0u) {
        err = burner_gba_gbx_execute_plain_sequence(
            &profile->read_cfi,
            NULL,
            &ctx,
            false,
            0u,
            BURNER_ROM_POLL_TIMEOUT_MS);
    } else {
        switch (profile->base_cmdset) {
        case BURNER_NOR_CMDSET_INTEL:
            err = burner_bacon_rom_write_u16(0x000u, 0x0098u);
            break;
        case BURNER_NOR_CMDSET_AMD:
            err = burner_bacon_gba_command_write_u16(burner_gba_cfi_enter_addr(), 0x0098u);
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (err == ESP_OK) {
        esp_rom_delay_us(100000u);
    }
    return err;
}

static esp_err_t burner_gba_gbx_read_cfi_with_profile(
    const burner_gbx_profile_t *profile,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry,
    burner_nor_cmdset_t *cmdset_out)
{
    esp_err_t err;
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    uint16_t w10 = 0u;
    uint16_t w11 = 0u;
    uint16_t w12 = 0u;
    uint8_t cmdset_lo = 0u;
    uint8_t cmdset_hi = 0u;
    uint8_t cfi27 = 0u;
    uint8_t cfi2a_lo = 0u;
    uint8_t cfi2b_hi = 0u;
    uint8_t cfi2c = 0u;
    bool detected_swapped = false;
    bool high_byte_lane = false;
    uint16_t buffer_size_field = 0u;
    uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    bool reverse_sector_region = false;
    uint32_t region_count = 0u;

    if (profile == NULL || device_size == NULL || sector_size == NULL ||
        buffer_write_bytes == NULL || geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }
    burner_nor_geometry_clear(geometry);

    err = burner_gba_gbx_enter_cfi_with_profile(profile);
    if (err != ESP_OK) {
        goto cfi_reset;
    }

    err = burner_bacon_rom_read_u16(0x010u, &w10);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_rom_read_u16(0x011u, &w11);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_rom_read_u16(0x012u, &w12);
    if (err != ESP_OK) {
        goto cfi_reset;
    }

    if (!burner_gba_detect_qry_words(
            burner_apply_d0d1_swap_on_read(w10, s_cart_ctx.d0d1_swapped),
            burner_apply_d0d1_swap_on_read(w11, s_cart_ctx.d0d1_swapped),
            burner_apply_d0d1_swap_on_read(w12, s_cart_ctx.d0d1_swapped),
            &detected_swapped,
            &high_byte_lane)) {
        ESP_LOGW(
            BURNER_TAG,
            "GBX CFI signature mismatch: profile=%s [010]=%04X [011]=%04X [012]=%04X",
            profile->file_name[0] != '\0' ? profile->file_name : "unknown",
            w10,
            w11,
            w12);
        err = ESP_FAIL;
        goto cfi_reset;
    }

    s_cart_ctx.d0d1_known = true;
    s_cart_ctx.d0d1_swapped = detected_swapped;
    s_cart_ctx.gba_cmd_data_lane = high_byte_lane ? BURNER_GBA_CMD_DATA_HIGH : BURNER_GBA_CMD_DATA_LOW;

    err = burner_bacon_gba_cfi_read_u8(0x013u, high_byte_lane, &cmdset_lo);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x014u, high_byte_lane, &cmdset_hi);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    cfi_cmdset = burner_nor_cmdset_from_cfi_primary_id((uint16_t)(((uint16_t)cmdset_hi << 8) | (uint16_t)cmdset_lo));
    if (cmdset_out != NULL) {
        *cmdset_out = cfi_cmdset;
    }
    if (cfi_cmdset != BURNER_NOR_CMDSET_UNKNOWN) {
        s_cart_ctx.gba_cmdset = cfi_cmdset;
    }

    err = burner_bacon_gba_cfi_read_u8(0x027u, high_byte_lane, &cfi27);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x02Au, high_byte_lane, &cfi2a_lo);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x02Bu, high_byte_lane, &cfi2b_hi);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    err = burner_bacon_gba_cfi_read_u8(0x02Cu, high_byte_lane, &cfi2c);
    if (err != ESP_OK) {
        goto cfi_reset;
    }

    if (cfi27 >= 31u) {
        err = ESP_FAIL;
        goto cfi_reset;
    }
    *device_size = (1u << cfi27);

    buffer_size_field = (uint16_t)(((uint16_t)cfi2b_hi << 8) | (uint16_t)cfi2a_lo);
    if (buffer_size_field <= 1u) {
        *buffer_write_bytes = 0u;
    } else {
        uint32_t buffer_bytes = 0u;

        if (buffer_size_field >= 32u) {
            err = ESP_ERR_INVALID_SIZE;
            goto cfi_reset;
        }
        buffer_bytes = 1u << buffer_size_field;
        if (buffer_bytes > UINT16_MAX) {
            err = ESP_ERR_INVALID_SIZE;
            goto cfi_reset;
        }
        *buffer_write_bytes = (uint16_t)buffer_bytes;
    }

    region_count = (uint32_t)cfi2c;
    if (region_count == 0u || region_count > BURNER_NOR_GEOMETRY_REGION_MAX) {
        err = ESP_ERR_INVALID_SIZE;
        goto cfi_reset;
    }

    for (uint32_t i = 0u; i < region_count; ++i) {
        uint8_t count_lo = 0u;
        uint8_t count_hi = 0u;
        uint8_t size_lo = 0u;
        uint8_t size_hi = 0u;
        uint32_t base = 0x02Du + (i * 4u);

        err = burner_bacon_gba_cfi_read_u8(base + 0u, high_byte_lane, &count_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 1u, high_byte_lane, &count_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 2u, high_byte_lane, &size_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(base + 3u, high_byte_lane, &size_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }

        sector_counts[i] = (((uint32_t)count_hi << 8) | (uint32_t)count_lo) + 1u;
        sector_sizes[i] = ((((uint32_t)size_hi << 8) | (uint32_t)size_lo) * 256u);
        if (sector_counts[i] == 0u || sector_sizes[i] == 0u) {
            err = ESP_ERR_INVALID_SIZE;
            goto cfi_reset;
        }
    }

    {
        uint8_t pri_lo = 0u;
        uint8_t pri_hi = 0u;
        uint32_t pri_word_addr = 0u;

        err = burner_bacon_gba_cfi_read_u8(0x015u, high_byte_lane, &pri_lo);
        if (err != ESP_OK) {
            goto cfi_reset;
        }
        err = burner_bacon_gba_cfi_read_u8(0x016u, high_byte_lane, &pri_hi);
        if (err != ESP_OK) {
            goto cfi_reset;
        }

        pri_word_addr = ((uint32_t)pri_hi << 8) | (uint32_t)pri_lo;
        if (pri_word_addr + 0x1Eu >= 0x200u) {
            pri_word_addr = 0x040u;
        }

        if (pri_word_addr + 0x1Eu < 0x200u) {
            uint8_t pri_p = 0u;
            uint8_t pri_r = 0u;
            uint8_t pri_i = 0u;

            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 0u, high_byte_lane, &pri_p);
            if (err != ESP_OK) {
                goto cfi_reset;
            }
            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 1u, high_byte_lane, &pri_r);
            if (err != ESP_OK) {
                goto cfi_reset;
            }
            err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 2u, high_byte_lane, &pri_i);
            if (err != ESP_OK) {
                goto cfi_reset;
            }

            if (pri_p == 'P' && pri_r == 'R' && pri_i == 'I') {
                uint8_t tb_boot_sector_raw = 0u;

                err = burner_bacon_gba_cfi_read_u8(pri_word_addr + 0x1Eu, high_byte_lane, &tb_boot_sector_raw);
                if (err != ESP_OK) {
                    goto cfi_reset;
                }
                reverse_sector_region = (tb_boot_sector_raw == 0x03u);
            }
        }
    }

    err = burner_nor_geometry_build(
        geometry,
        *device_size,
        sector_counts,
        sector_sizes,
        region_count,
        reverse_sector_region);
    if (err != ESP_OK) {
        goto cfi_reset;
    }
    *sector_size = burner_nor_geometry_report_sector_size(geometry);
    err = ESP_OK;

cfi_reset:
    {
        esp_err_t reset_err = burner_gba_gbx_reset_profile_to_read_mode(profile);
        if (err == ESP_OK && reset_err != ESP_OK) {
            err = reset_err;
        }
    }
    return err;
}

static esp_err_t burner_gba_gbx_run_unlock_read_for_profile(
    const burner_gbx_profile_t *profile,
    bool is_multi_card)
{
    uint16_t discard = 0u;

    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    {
        uint32_t local_byte_addr = 0u;
        esp_err_t err = burner_gba_gbx_prepare_bank_addr(0u, is_multi_card, &local_byte_addr);

        if (err != ESP_OK) {
            return err;
        }
        err = burner_bacon_rom_read_u16(local_byte_addr >> 1, &discard);
        if (err != ESP_OK) {
            return err;
        }
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

    if (profile->unlock_read_count > 0u) {
        esp_rom_delay_us(1000u);
    }
    return ESP_OK;
}

static esp_err_t burner_gba_gbx_run_unlock_read(bool is_multi_card)
{
    return burner_gba_gbx_run_unlock_read_for_profile(&s_cart_ctx.gbx, is_multi_card);
}

static esp_err_t burner_gba_gbx_read_id_with_profile(
    const burner_gbx_profile_t *profile,
    uint8_t id_out[BURNER_GBX_FLASH_ID_LEN_MAX],
    size_t *id_len_out,
    bool *changed_out)
{
    burner_gba_gbx_exec_ctx_t ctx = {0};
    uint8_t baseline[BURNER_GBX_FLASH_ID_LEN_MAX] = {0};
    uint32_t read_addr = 0u;
    size_t id_len;
    esp_err_t err;

    if (profile == NULL || id_out == NULL || id_len_out == NULL || changed_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (profile->read_identifier.count == 0u) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    id_len = burner_gba_gbx_first_flash_id_len(profile);
    if (id_len == 0u || id_len > BURNER_GBX_FLASH_ID_LEN_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    read_addr = profile->has_read_identifier_at ? profile->read_identifier_at : 0u;

    memset(id_out, 0, BURNER_GBX_FLASH_ID_LEN_MAX);
    *id_len_out = id_len;
    *changed_out = false;

    if (profile->power_cycle) {
        err = burner_bacon_gba_power_cycle_3v3_locked();
        if (err != ESP_OK) {
            return err;
        }
        esp_rom_delay_us(1000u);
    }

    err = burner_gba_gbx_execute_plain_sequence(
        &profile->reset,
        NULL,
        &ctx,
        false,
        0u,
        BURNER_ROM_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gba_gbx_read_chip_bytes(read_addr, baseline, id_len, false);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gba_gbx_run_unlock_read_for_profile(profile, false);
    if (err != ESP_OK) {
        return err;
    }
    if (profile->unlock.count > 0u) {
        err = burner_gba_gbx_execute_plain_sequence(
            &profile->unlock,
            NULL,
            &ctx,
            false,
            0u,
            BURNER_ROM_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        esp_rom_delay_us(1000u);
    }
    err = burner_gba_gbx_execute_plain_sequence(
        &profile->read_identifier,
        NULL,
        &ctx,
        false,
        0u,
        BURNER_ROM_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(1000u);
    err = burner_gba_gbx_read_chip_bytes(read_addr, id_out, id_len, false);
    (void)burner_gba_gbx_execute_plain_sequence(
        &profile->reset,
        NULL,
        &ctx,
        false,
        0u,
        BURNER_ROM_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    *changed_out = (memcmp(baseline, id_out, id_len) != 0);
    return ESP_OK;
}

static bool burner_gba_gbx_cmd_step_is_abs_value(
    const burner_gbx_cmd_step_t *step,
    uint32_t addr,
    uint16_t value)
{
    return step != NULL &&
           step->addr_kind == BURNER_GBX_ADDR_ABS &&
           step->addr_value == addr &&
           step->data_kind == BURNER_GBX_DATA_VALUE &&
           step->data_value == value;
}

static uint32_t burner_gba_gbx_method_specificity(const burner_gbx_cmd_list_t *method)
{
    if (method == NULL || method->count == 0u) {
        return 0u;
    }
    if (method->count == 1u &&
        burner_gba_gbx_cmd_step_is_abs_value(&method->steps[0], 0u, 0x0090u)) {
        return 10u;
    }
    if (method->count == 3u &&
        burner_gba_gbx_cmd_step_is_abs_value(&method->steps[0], 0x0AAAu, 0x00AAu) &&
        burner_gba_gbx_cmd_step_is_abs_value(&method->steps[1], 0x0555u, 0x0055u) &&
        burner_gba_gbx_cmd_step_is_abs_value(&method->steps[2], 0x0AAAu, 0x0090u)) {
        return 20u;
    }
    if (method->count == 3u &&
        burner_gba_gbx_cmd_step_is_abs_value(&method->steps[0], 0x0AAAu, 0x00A9u) &&
        burner_gba_gbx_cmd_step_is_abs_value(&method->steps[1], 0x0555u, 0x0056u) &&
        burner_gba_gbx_cmd_step_is_abs_value(&method->steps[2], 0x0AAAu, 0x0090u)) {
        return 20u;
    }
    return 100u + method->count;
}

static void burner_gba_gbx_probe_lookup_consider(
    burner_gba_gbx_probe_ctx_t *ctx,
    const burner_gbx_profile_t *profile,
    const uint8_t *id,
    size_t id_len,
    size_t match_len,
    bool candidate_ambiguous)
{
    uint32_t method_specificity;

    if (ctx == NULL || profile == NULL || id == NULL || ctx->profile_out == NULL) {
        return;
    }
    if (match_len == 0u) {
        return;
    }

    method_specificity = burner_gba_gbx_method_specificity(&profile->read_identifier);

    if (!ctx->found ||
        method_specificity > ctx->best_method_specificity ||
        (method_specificity == ctx->best_method_specificity && match_len > ctx->best_match_len)) {
        *ctx->profile_out = *profile;
        memset(ctx->id, 0, sizeof(ctx->id));
        memcpy(ctx->id, id, id_len < sizeof(ctx->id) ? id_len : sizeof(ctx->id));
        ctx->best_match_len = match_len;
        ctx->best_method_specificity = method_specificity;
        ctx->found = true;
        ctx->ambiguous = candidate_ambiguous;
        return;
    }

    if (method_specificity == ctx->best_method_specificity &&
        match_len == ctx->best_match_len &&
        candidate_ambiguous) {
        ctx->ambiguous = true;
    }
}

static esp_err_t burner_gba_gbx_probe_method_visitor(
    const burner_gbx_profile_t *profile,
    void *user,
    bool *stop_out)
{
    burner_gba_gbx_probe_ctx_t *ctx = (burner_gba_gbx_probe_ctx_t *)user;
    burner_gbx_profile_t *candidate = NULL;
    uint8_t id[BURNER_GBX_FLASH_ID_LEN_MAX] = {0};
    size_t id_len = 0u;
    size_t match_len = 0u;
    bool changed = false;
    bool candidate_ambiguous = false;
    esp_err_t err;

    if (profile == NULL || ctx == NULL || ctx->profile_out == NULL ||
        ctx->candidate_profile == NULL || stop_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    candidate = ctx->candidate_profile;
    *stop_out = false;
    if (profile->read_identifier.count == 0u) {
        return ESP_OK;
    }

    err = burner_cancel_poll();
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gba_gbx_read_id_with_profile(profile, id, &id_len, &changed);
    if (err != ESP_OK || !changed) {
        return ESP_OK;
    }
    ctx->method_results++;

    burner_gbx_profile_clear(candidate);
    err = burner_gbx_find_agb_profile_for_method_id(
        &profile->read_identifier,
        id,
        id_len,
        candidate,
        &match_len,
        &candidate_ambiguous);
    if (err != ESP_OK) {
        return (err == ESP_ERR_NOT_FOUND) ? ESP_OK : err;
    }

    burner_gba_gbx_probe_lookup_consider(ctx, candidate, id, id_len, match_len, candidate_ambiguous);
    return ESP_OK;
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
    burner_gba_gbx_probe_ctx_t probe_ctx = {0};
    burner_gbx_profile_t *matched_profile = NULL;
    burner_gbx_profile_t *candidate_profile = NULL;
    bool cfi_probe_used = false;
    bool need_geometry_probe = false;
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

    s_cart_ctx.gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD;
    s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_gba_active_nor_flags = 0u;
    s_gba_active_intel_generic_cfi = false;
    s_gba_active_intel_e9_entry = false;
    s_cart_ctx.d0d1_known = false;
    s_cart_ctx.d0d1_swapped = false;
    s_cart_ctx.gba_likely_read_only = false;
    burner_nor_geometry_clear(&s_cart_ctx.geometry);

    err = burner_gba_detect_d0d1_swap(&s_cart_ctx.d0d1_swapped, &s_cart_ctx.gba_cmd_data_lane);
    if (err == ESP_OK) {
        s_cart_ctx.d0d1_known = true;
    } else {
        s_cart_ctx.d0d1_swapped = false;
        s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    }

    matched_profile = (burner_gbx_profile_t *)calloc(1u, sizeof(*matched_profile));
    if (matched_profile == NULL) {
        return ESP_ERR_NO_MEM;
    }
    candidate_profile = (burner_gbx_profile_t *)calloc(1u, sizeof(*candidate_profile));
    if (candidate_profile == NULL) {
        free(matched_profile);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(BURNER_TAG, "GBX probe: FlashGBX AGB method scan starting");

    probe_ctx.cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    probe_ctx.flash_size = 0u;
    probe_ctx.sector_size = 0u;
    probe_ctx.geometry = NULL;
    probe_ctx.cfi_ok = false;
    probe_ctx.d0d1_known = s_cart_ctx.d0d1_known;
    probe_ctx.d0d1_swapped = s_cart_ctx.d0d1_swapped;
    probe_ctx.found = false;
    probe_ctx.ambiguous = false;
    probe_ctx.best_match_len = 0u;
    probe_ctx.best_method_specificity = 0u;
    probe_ctx.method_results = 0u;
    probe_ctx.profile_out = matched_profile;
    probe_ctx.candidate_profile = candidate_profile;

    err = burner_gbx_visit_cached_methods_by_type("AGB", burner_gba_gbx_probe_method_visitor, &probe_ctx);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(BURNER_TAG, "GBX AGB method cache unavailable; falling back to full AGB profile scan");
        err = burner_gbx_visit_agb_profiles(burner_gba_gbx_probe_method_visitor, &probe_ctx);
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        goto cleanup;
    }
    if (!probe_ctx.found || probe_ctx.ambiguous) {
        ESP_LOGE(
            BURNER_TAG,
            "GBX probe failed: no unique FlashGBX AGB profile matched method scan (ambiguous=%u methods=%" PRIu32 ")",
            probe_ctx.ambiguous ? 1u : 0u,
            probe_ctx.method_results);
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    memcpy(id_out, probe_ctx.id, 8u);

    err = burner_gba_gbx_apply_profile_geometry(
        matched_profile,
        device_size,
        sector_size,
        buffer_write_bytes,
        *cfi_ok_out);
    if (err != ESP_OK) {
        goto cleanup;
    }
    need_geometry_probe =
        (*device_size == 0u) ||
        (*sector_size == 0u) ||
        (*buffer_write_bytes == 0u && !matched_profile->has_buffer_size) ||
        (matched_profile->base_cmdset == BURNER_NOR_CMDSET_INTEL && !*cfi_ok_out) ||
        (matched_profile->sector_size_from_cfi && !*cfi_ok_out);
    if (need_geometry_probe) {
        burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

        ESP_LOGI(
            BURNER_TAG,
            "GBX probe CFI helper: profile=%s needs FlashGBX CFI fill flash=%" PRIu32
            " sector=%" PRIu32 " buf=%u cfi=%s",
            matched_profile->file_name,
            *device_size,
            *sector_size,
            (unsigned)*buffer_write_bytes,
            *cfi_ok_out ? "ok" : "unavailable");
        err = burner_gba_gbx_read_cfi_with_profile(
            matched_profile,
            device_size,
            sector_size,
            buffer_write_bytes,
            &s_cart_ctx.geometry,
            &cfi_cmdset);
        if (err == ESP_OK) {
            cfi_probe_used = true;
            *cfi_ok_out = true;
            if (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
                s_cart_ctx.gba_cmdset = cfi_cmdset;
            }
            err = burner_gba_gbx_apply_profile_geometry(
                matched_profile,
                device_size,
                sector_size,
                buffer_write_bytes,
                *cfi_ok_out);
            if (err != ESP_OK) {
                goto cleanup;
            }
        } else {
            ESP_LOGW(
                BURNER_TAG,
                "GBX probe CFI helper failed: profile=%s err=%s; using profile metadata only",
                matched_profile->file_name,
                esp_err_to_name(err));
            *device_size = 0u;
            *sector_size = 0u;
            *buffer_write_bytes = 0u;
            *cfi_ok_out = false;
            burner_nor_geometry_clear(&s_cart_ctx.geometry);
            err = burner_gba_gbx_apply_profile_geometry(
                matched_profile,
                device_size,
                sector_size,
                buffer_write_bytes,
                false);
            if (err != ESP_OK) {
                goto cleanup;
            }
        }
    }
    if (*device_size == 0u || *sector_size == 0u) {
        ESP_LOGE(BURNER_TAG, "GBX probe incomplete geometry: profile=%s", matched_profile->file_name);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    s_cart_ctx.gbx = *matched_profile;
    if (profile_out != NULL) {
        *profile_out = *matched_profile;
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBX probe ok: file=%s chip=%s flash=%" PRIu32 " sector=%" PRIu32
        " buf=%u cfi=%s cmdset=%s geom_helper=%u match_len=%u id=%02X %02X %02X %02X %02X %02X %02X %02X",
        matched_profile->file_name,
        matched_profile->display_name[0] != '\0' ? matched_profile->display_name : "unknown",
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        *cfi_ok_out ? "ok" : "unavailable",
        burner_nor_cmdset_name(s_cart_ctx.gba_cmdset),
        cfi_probe_used ? 1u : 0u,
        (unsigned)probe_ctx.best_match_len,
        id_out[0],
        id_out[1],
        id_out[2],
        id_out[3],
        id_out[4],
        id_out[5],
        id_out[6],
        id_out[7]);
    err = ESP_OK;

cleanup:
    free(candidate_profile);
    free(matched_profile);
    return err;
}

esp_err_t burner_gba_gbx_prepare_manual_profile(
    const char *file_name,
    uint8_t id_out[8],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out)
{
    burner_gbx_profile_t manual_profile = {0};
    uint8_t raw_id[BURNER_GBX_FLASH_ID_LEN_MAX] = {0};
    size_t raw_id_len = 0u;
    size_t match_len = 0u;
    bool changed = false;
    bool need_geometry_probe = false;
    esp_err_t err;

    if (file_name == NULL || file_name[0] == '\0' || id_out == NULL || device_size == NULL ||
        sector_size == NULL || buffer_write_bytes == NULL || cfi_ok_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(id_out, 0, 8u);
    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    *cfi_ok_out = false;

    burner_gbx_profile_clear(&s_cart_ctx.gbx);
    burner_nor_geometry_clear(&s_cart_ctx.geometry);
    s_cart_ctx.gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD;
    s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    s_cart_ctx.gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    s_cart_ctx.d0d1_known = false;
    s_cart_ctx.d0d1_swapped = false;
    s_cart_ctx.gba_likely_read_only = false;
    s_gba_active_nor_flags = 0u;
    s_gba_active_intel_generic_cfi = false;
    s_gba_active_intel_e9_entry = false;

    err = burner_gbx_load_profile_by_file_name("AGB", file_name, &manual_profile);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_gba_detect_d0d1_swap(&s_cart_ctx.d0d1_swapped, &s_cart_ctx.gba_cmd_data_lane);
    if (err == ESP_OK) {
        s_cart_ctx.d0d1_known = true;
    } else {
        s_cart_ctx.d0d1_swapped = false;
        s_cart_ctx.gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW;
    }
    if (manual_profile.d0d1_known) {
        s_cart_ctx.d0d1_known = true;
        s_cart_ctx.d0d1_swapped = manual_profile.d0d1_swapped;
    }

    err = burner_gba_gbx_read_id_with_profile(&manual_profile, raw_id, &raw_id_len, &changed);
    if (err != ESP_OK) {
        return err;
    }
    match_len = burner_gbx_profile_match_id_ex(&manual_profile, raw_id, raw_id_len, NULL, NULL);
    if (!changed || match_len == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBX manual profile ID mismatch: file=%s changed=%u match_len=%u",
            manual_profile.file_name,
            changed ? 1u : 0u,
            (unsigned)match_len);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(id_out, raw_id, (raw_id_len < 8u) ? raw_id_len : 8u);

    err = burner_gba_gbx_apply_profile_geometry(
        &manual_profile,
        device_size,
        sector_size,
        buffer_write_bytes,
        *cfi_ok_out);
    if (err != ESP_OK) {
        return err;
    }
    need_geometry_probe =
        (*device_size == 0u) ||
        (*sector_size == 0u) ||
        (*buffer_write_bytes == 0u && !manual_profile.has_buffer_size) ||
        (manual_profile.base_cmdset == BURNER_NOR_CMDSET_INTEL && !*cfi_ok_out) ||
        (manual_profile.sector_size_from_cfi && !*cfi_ok_out);
    if (need_geometry_probe) {
        burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;

        err = burner_gba_gbx_read_cfi_with_profile(
            &manual_profile,
            device_size,
            sector_size,
            buffer_write_bytes,
            &s_cart_ctx.geometry,
            &cfi_cmdset);
        if (err == ESP_OK) {
            *cfi_ok_out = true;
            if (s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
                s_cart_ctx.gba_cmdset = cfi_cmdset;
            }
            err = burner_gba_gbx_apply_profile_geometry(
                &manual_profile,
                device_size,
                sector_size,
                buffer_write_bytes,
                *cfi_ok_out);
            if (err != ESP_OK) {
                return err;
            }
        } else {
            *device_size = 0u;
            *sector_size = 0u;
            *buffer_write_bytes = 0u;
            *cfi_ok_out = false;
            burner_nor_geometry_clear(&s_cart_ctx.geometry);
            err = burner_gba_gbx_apply_profile_geometry(
                &manual_profile,
                device_size,
                sector_size,
                buffer_write_bytes,
                false);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    if (*device_size == 0u || *sector_size == 0u) {
        ESP_LOGE(BURNER_TAG, "GBX manual profile incomplete geometry: file=%s", manual_profile.file_name);
        return ESP_ERR_INVALID_SIZE;
    }

    s_cart_ctx.gbx = manual_profile;
    ESP_LOGI(
        BURNER_TAG,
        "GBX manual profile prepared: file=%s chip=%s flash=%" PRIu32 " sector=%" PRIu32
        " buf=%u cfi=%s id=%02X %02X %02X %02X %02X %02X %02X %02X",
        s_cart_ctx.gbx.file_name,
        s_cart_ctx.gbx.display_name[0] != '\0' ? s_cart_ctx.gbx.display_name : "unknown",
        *device_size,
        *sector_size,
        (unsigned)*buffer_write_bytes,
        *cfi_ok_out ? "ok" : "unavailable",
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
        esp_rom_delay_us(1000u);
        err = burner_gba_gbx_run_unlock_read(is_multi_card);
        if (err != ESP_OK) {
            return err;
        }
        if (s_cart_ctx.gbx.unlock.count > 0u) {
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
            esp_rom_delay_us(1000u);
        }
        if (is_multi_card) {
            err = burner_bacon_gba_rom_switch_bank(0u);
            if (err != ESP_OK) {
                return err;
            }
            vTaskDelay(pdMS_TO_TICKS(BURNER_GBA_BANK_SWITCH_SETTLE_MS));
            s_cart_ctx.current_bank = 0u;
        }
        return ESP_OK;
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
        err = burner_bacon_gba_rom_switch_bank(0u);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(BURNER_GBA_BANK_SWITCH_SETTLE_MS));
        s_cart_ctx.current_bank = 0u;
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
        .bank_base = burner_gba_gbx_bank_base_for_addr(byte_addr),
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
    ctx.bank_base = burner_gba_gbx_bank_base_for_addr(starting_address);
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
    (void)prepare_sectors;

    if (data == NULL || len == 0u || (offset & 0x1u) != 0u || (len & 0x1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_gba_gbx_is_active() || !s_cart_ctx.prepared) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_ctx.gbx.single_write.count == 0u && s_cart_ctx.gbx.buffer_write.count == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBA GBX profile missing write commands: file=%s",
            s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
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
            err = burner_gba_gbx_switch_bank_if_needed(bank);
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

        programmed += chunk;
        burner_task_yield_if_due();
    }

    return ESP_OK;
}

esp_err_t burner_gba_gbx_erase_sector(uint32_t flash_addr, bool is_multi_card, uint32_t timeout_ms)
{
    uint32_t bank = 0u;
    burner_gba_gbx_exec_ctx_t ctx = {
        .sector_addr = flash_addr,
        .program_addr = flash_addr,
        .bank_base = burner_gba_gbx_bank_base_for_addr(flash_addr),
    };
    esp_err_t err;

    if (!burner_gba_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_ctx.gbx.sector_erase.count == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBA GBX profile missing sector erase commands: file=%s",
            s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (is_multi_card) {
        bank = flash_addr / BURN_GBA_BANK_BYTES;
        err = burner_gba_gbx_switch_bank_if_needed(bank);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = burner_gba_gbx_reset_to_read_mode(false, false, 0u);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_gba_gbx_execute_plain_sequence(
        &s_cart_ctx.gbx.sector_erase,
        &s_cart_ctx.gbx.sector_erase_wait_for,
        &ctx,
        is_multi_card,
        0u,
        timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    return burner_gba_gbx_reset_to_read_mode(false, false, 0u);
}

esp_err_t burner_gba_gbx_chip_erase_once(void)
{
    burner_gba_gbx_exec_ctx_t ctx = {0};
    uint32_t timeout_ms = s_cart_ctx.gbx.has_chip_erase_timeout ? s_cart_ctx.gbx.chip_erase_timeout_ms
                                                                : BURNER_ROM_CHIP_ERASE_TIMEOUT_MS;
    esp_err_t err;

    if (!burner_gba_gbx_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cart_ctx.gbx.chip_erase.count == 0u) {
        ESP_LOGE(
            BURNER_TAG,
            "GBA GBX profile missing chip erase commands: file=%s",
            s_cart_ctx.gbx.file_name[0] != '\0' ? s_cart_ctx.gbx.file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = burner_gba_gbx_reset_to_read_mode(true, false, s_cart_ctx.device_size);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_gba_gbx_execute_plain_sequence(
        &s_cart_ctx.gbx.chip_erase,
        &s_cart_ctx.gbx.chip_erase_wait_for,
        &ctx,
        false,
        0u,
        timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    return burner_gba_gbx_reset_to_read_mode(true, false, s_cart_ctx.device_size);
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
            "GBX profile uses custom command_set=%s, GBX runtime may not support it",
            profile->command_set_name);
    }

    if (!burner_gba_gbx_runtime_supported(profile)) {
        ESP_LOGE(
            BURNER_TAG,
            "GBX profile matched but GBA runtime write commands are unavailable: file=%s",
            profile->file_name[0] != '\0' ? profile->file_name : "unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }
    profile->runtime_commands_enabled = true;
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

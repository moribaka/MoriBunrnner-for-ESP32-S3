/* Included from ws_server.c before some late static helper definitions. */
static void burner_readid_trace_begin(const char *name, int64_t *start_us, int64_t *last_us);
static void burner_readid_trace_log_u8(
    const char *name,
    const char *op,
    uint32_t addr,
    uint8_t data,
    int64_t *last_us);
static void burner_readid_trace_end(const char *name, int64_t start_us, esp_err_t err);
static esp_err_t burner_bacon_gbc_write(uint16_t addr, const uint8_t *buf, size_t len);
static esp_err_t burner_bacon_gbc_read(uint16_t addr, uint8_t *buf, size_t len);
static esp_err_t burner_bacon_gbc_read_u8(uint16_t addr, uint8_t *value);
static bool burner_buffer_all_equal(const uint8_t *left, const uint8_t *right, size_t len);
static esp_err_t burner_bacon_mbc5_switch_bank(uint16_t bank);
static esp_err_t burner_bacon_gb_probe_write_low(uint8_t bank);
static esp_err_t burner_bacon_gb_probe_read_switch_sample(uint8_t *sample, size_t len);
static bool burner_mbc5_probe_load_entry_geometry(
    const uint8_t id[4],
    const burner_nor_entry_t *entry,
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry);
static bool burner_mbc5_geometry_should_prefer_id(
    const burner_nor_geometry_t *cfi_geometry,
    uint32_t cfi_device_size,
    uint32_t cfi_sector_size,
    const burner_nor_geometry_t *id_geometry,
    uint32_t id_device_size,
    uint32_t id_sector_size);

enum {
    GB_FIXED_SAMPLE_ADDR = 0x0000u,
    GB_SWITCH_SAMPLE_ADDR = 0x4000u,
    GB_MAPPER_SAMPLE_LEN = 64u,
};

void burner_build_output_timestamp(char *buf, size_t buf_len)
{
    struct tm tm_now = {0};

    if (buf == NULL || buf_len < 2u) {
        return;
    }

    buf[0] = '\0';
    if (burner_get_wallclock_time(NULL, &tm_now) &&
        strftime(buf, buf_len, "%Y%m%d_%H%M%S", &tm_now) > 0u) {
        return;
    }

    (void)snprintf(buf, buf_len, "ts%lu", (unsigned long)esp_log_timestamp());
}

bool burner_build_mbc5_verify_log_rel_path(
    const uint8_t id[4],
    char *rel_path,
    size_t rel_path_len)
{
    char chip_name_safe[48] = {0};
    char timestamp_text[32] = {0};
    const char *chip_name;
    int n;

    if (rel_path == NULL || rel_path_len < 16u) {
        return false;
    }

    chip_name = burner_mbc5_chip_name(id);
    if (!burner_sanitize_filename(chip_name, chip_name_safe, sizeof(chip_name_safe))) {
        (void)snprintf(chip_name_safe, sizeof(chip_name_safe), "unknown");
    }
    burner_build_output_timestamp(timestamp_text, sizeof(timestamp_text));
    if (timestamp_text[0] == '\0') {
        return false;
    }

    n = snprintf(
        rel_path,
        rel_path_len,
        VERIFY_LOG_DIR_REL "/MBC5_%s_%s.log",
        chip_name_safe,
        timestamp_text);
    return n > 0 && n < (int)rel_path_len;
}

FILE *burner_open_mbc5_verify_log(
    const burner_task_param_t *job,
    char *log_rel,
    size_t log_rel_len)
{
    char log_full[TF_PATH_LEN_MAX + 64] = {0};
    FILE *fp = NULL;
    const char *chip_name;

    if (job == NULL || log_rel == NULL || log_rel_len < 16u) {
        return NULL;
    }

    if (burner_mkdirs_rel(VERIFY_LOG_DIR_REL) != ESP_OK) {
        return NULL;
    }
    if (!burner_build_mbc5_verify_log_rel_path(s_cart_ctx.mbc5_id, log_rel, log_rel_len) ||
        !burner_build_full_path(log_rel, log_full, sizeof(log_full))) {
        return NULL;
    }

    fp = fopen(log_full, "wb");
    if (fp == NULL) {
        return NULL;
    }

    chip_name = burner_mbc5_chip_name(s_cart_ctx.mbc5_id);
    (void)fprintf(
        fp,
        "mode=MBC5\nchip=%s\nrom=%s\npath=%s\naddr_begin=0x%08" PRIX32 "\ntotal=%" PRIu32
        "\nid=%02X %02X %02X %02X\n\n",
        chip_name,
        job->rom_name,
        job->rom_path,
        job->addr_begin,
        job->total_bytes,
        s_cart_ctx.mbc5_id[0],
        s_cart_ctx.mbc5_id[1],
        s_cart_ctx.mbc5_id[2],
        s_cart_ctx.mbc5_id[3]);
    return fp;
}

static esp_err_t burner_bacon_gb_detect_mapper(burner_gb_mapper_t *mapper_out)
{
    uint8_t fixed_sample[GB_MAPPER_SAMPLE_LEN] = {0};
    uint8_t switch_sample[GB_MAPPER_SAMPLE_LEN] = {0};
    bool fixed_blank = false;
    bool switch_blank = false;
    bool eq = false;
    esp_err_t err = ESP_OK;

    if (mapper_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *mapper_out = BURNER_GB_MAPPER_MBC5;

    err = burner_bacon_gb_probe_write_low(0u);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gbc_read(GB_FIXED_SAMPLE_ADDR, fixed_sample, sizeof(fixed_sample));
    if (err != ESP_OK) {
        return err;
    }
    err = burner_bacon_gb_probe_read_switch_sample(switch_sample, sizeof(switch_sample));
    if (err != ESP_OK) {
        return err;
    }

    err = burner_buffer_all_ff(fixed_sample, sizeof(fixed_sample), &fixed_blank);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_buffer_all_ff(switch_sample, sizeof(switch_sample), &switch_blank);
    if (err != ESP_OK) {
        return err;
    }

    eq = burner_buffer_all_equal(fixed_sample, switch_sample, sizeof(fixed_sample));
    if (eq) {
        *mapper_out = BURNER_GB_MAPPER_MBC5;
    } else {
        *mapper_out = BURNER_GB_MAPPER_MBC3;
    }

    ESP_LOGI(
        BURNER_TAG,
        "GB mapper detect: bank0 fixed=%04X switch=%04X len=%u fixed=%02X-%02X-%02X-%02X switch=%02X-%02X-%02X-%02X eq=%d blank=%d/%d result=%s",
        GB_FIXED_SAMPLE_ADDR,
        GB_SWITCH_SAMPLE_ADDR,
        (unsigned)GB_MAPPER_SAMPLE_LEN,
        fixed_sample[0],
        fixed_sample[1],
        fixed_sample[2],
        fixed_sample[3],
        switch_sample[0],
        switch_sample[1],
        switch_sample[2],
        switch_sample[3],
        eq ? 1 : 0,
        fixed_blank ? 1 : 0,
        switch_blank ? 1 : 0,
        burner_gb_mapper_name(*mapper_out));
    return ESP_OK;
}

static esp_err_t burner_bacon_gb_probe_write_low(uint8_t bank)
{
    return burner_bacon_gbc_write(0x2000u, &bank, 1u);
}

static esp_err_t burner_bacon_gb_probe_read_switch_sample(uint8_t *sample, size_t len)
{
    if (sample == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return burner_bacon_gbc_read(GB_SWITCH_SAMPLE_ADDR, sample, len);
}

esp_err_t burner_bacon_mbc5_get_id(uint8_t id_out[4])
{
    uint8_t cmd;
    esp_err_t err;
    int64_t trace_start_us = 0;
    int64_t trace_last_us = 0;

    if (id_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_readid_trace_begin("MBC5 ReadID trace", &trace_start_us, &trace_last_us);

    cmd = 0xAAu;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0AAA data=0xAA failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0AAAu, 0xAAu, &trace_last_us);
    cmd = 0x55u;
    err = burner_bacon_gbc_write(0x0555u, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0555 data=0x55 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0555u, 0x55u, &trace_last_us);
    cmd = 0x90u;
    err = burner_bacon_gbc_write(0x0AAAu, &cmd, 1);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0AAA data=0x90 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0AAAu, 0x90u, &trace_last_us);

    err = burner_bacon_gbc_read_u8(0x0000u, &id_out[0]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x0000 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x0000u, id_out[0], &trace_last_us);
    err = burner_bacon_gbc_read_u8(0x0002u, &id_out[1]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x0002 failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x0002u, id_out[1], &trace_last_us);
    err = burner_bacon_gbc_read_u8(0x001Cu, &id_out[2]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x001C failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x001Cu, id_out[2], &trace_last_us);
    err = burner_bacon_gbc_read_u8(0x001Eu, &id_out[3]);
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace read addr=0x001E failed: %s", esp_err_to_name(err));
        burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
        return err;
    }
    burner_readid_trace_log_u8("MBC5 ReadID trace", "read", 0x001Eu, id_out[3], &trace_last_us);

    cmd = 0xF0u;
    err = burner_bacon_gbc_write(0x0000u, &cmd, 1);
    if (err == ESP_OK) {
        burner_readid_trace_log_u8("MBC5 ReadID trace", "write", 0x0000u, 0xF0u, &trace_last_us);
    } else {
        ESP_LOGW(BURNER_TAG, "MBC5 ReadID trace write addr=0x0000 data=0xF0 failed: %s", esp_err_to_name(err));
    }
    burner_readid_trace_end("MBC5 ReadID trace", trace_start_us, err);
    return err;
}

esp_err_t burner_bacon_mbc5_get_cfi(
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    burner_nor_geometry_t *geometry,
    burner_nor_cmdset_t *cmdset_out)
{
    uint8_t cfi = 0;
    uint8_t hi = 0;
    uint8_t lo = 0;
    uint8_t cmd = 0x98u;
    uint8_t reset_cmd = 0xF0u;
    uint32_t tmp32;
    uint16_t tmp16;
    uint16_t primary_cmdset_id = 0u;
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    const uint16_t enter_addrs[] = {0x00AAu, 0x0000u};
    const uint8_t reset_cmds[] = {0xF0u, 0xFFu};
    uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
    bool entered_cfi = false;
    bool cfi_matched = false;
    bool reverse_sector_region = false;
    uint32_t region_count = 0u;
    size_t enter_idx;
    esp_err_t err;

    if (device_size == NULL || sector_size == NULL || buffer_write_bytes == NULL || geometry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *device_size = 0u;
    *sector_size = 0u;
    *buffer_write_bytes = 0u;
    if (cmdset_out != NULL) {
        *cmdset_out = BURNER_NOR_CMDSET_UNKNOWN;
    }
    burner_nor_geometry_clear(geometry);

    for (enter_idx = 0u; enter_idx < (sizeof(enter_addrs) / sizeof(enter_addrs[0])); ++enter_idx) {
        reset_cmd = reset_cmds[enter_idx];
        err = burner_bacon_gbc_write(enter_addrs[enter_idx], &cmd, 1);
        if (err != ESP_OK) {
            return err;
        }
        entered_cfi = true;

        err = burner_bacon_gbc_read_u8(0x0020u, &cfi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (cfi != 0x51u) {
            goto cfi_retry;
        }
        err = burner_bacon_gbc_read_u8(0x0022u, &cfi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (cfi != 0x52u) {
            goto cfi_retry;
        }
        err = burner_bacon_gbc_read_u8(0x0024u, &cfi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (cfi != 0x59u) {
            goto cfi_retry;
        }
        cfi_matched = true;
        break;

cfi_retry:
        if (entered_cfi) {
            esp_err_t reset_err = burner_bacon_gbc_write(0x0000u, &reset_cmd, 1);
            if (reset_err != ESP_OK) {
                return reset_err;
            }
            entered_cfi = false;
        }
    }

    if (!cfi_matched) {
        err = ESP_FAIL;
        goto cfi_out;
    }

    err = burner_bacon_gbc_read_u8(0x0026u, &lo);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    err = burner_bacon_gbc_read_u8(0x0028u, &hi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    primary_cmdset_id = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
    cfi_cmdset = burner_nor_cmdset_from_cfi_primary_id(primary_cmdset_id);
    if (cfi_cmdset == BURNER_NOR_CMDSET_UNKNOWN) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto cfi_out;
    }
    if (cmdset_out != NULL) {
        *cmdset_out = cfi_cmdset;
    }

    err = burner_bacon_gbc_read_u8(0x004Eu, &cfi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    if (cfi >= 31u) {
        err = ESP_FAIL;
        goto cfi_out;
    }
    *device_size = (1u << cfi);

    err = burner_bacon_gbc_read_u8(0x0056u, &hi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    err = burner_bacon_gbc_read_u8(0x0054u, &lo);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    tmp16 = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
    if (tmp16 == 0u) {
        *buffer_write_bytes = 0u;
    } else {
        if (hi != 0u) {
            ESP_LOGW(
                BURNER_TAG,
                "MBC5 CFI buffer size exponent high byte non-zero (0x%02X), using low byte 0x%02X",
                hi,
                lo);
        }
        if (lo >= 16u) {
            err = ESP_FAIL;
            goto cfi_out;
        }
        *buffer_write_bytes = (uint16_t)(1u << lo);
    }

    err = burner_bacon_gbc_read_u8(0x0058u, &cfi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    region_count = (uint32_t)cfi;
    if (region_count == 0u || region_count > BURNER_NOR_GEOMETRY_REGION_MAX) {
        err = ESP_ERR_INVALID_SIZE;
        goto cfi_out;
    }

    for (uint32_t i = 0u; i < region_count; ++i) {
        uint32_t base = 0x005Au + (i * 8u);

        err = burner_bacon_gbc_read_u8(base + 0u, &lo);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(base + 2u, &hi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        sector_counts[i] = (((uint32_t)hi << 8) | (uint32_t)lo) + 1u;

        err = burner_bacon_gbc_read_u8(base + 4u, &lo);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(base + 6u, &hi);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        sector_sizes[i] = ((((uint32_t)hi << 8) | (uint32_t)lo) * 256u);
        if (sector_counts[i] == 0u || sector_sizes[i] == 0u) {
            err = ESP_ERR_INVALID_SIZE;
            goto cfi_out;
        }
    }

    err = burner_bacon_gbc_read_u8(0x002Au, &lo);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    err = burner_bacon_gbc_read_u8(0x002Cu, &hi);
    if (err != ESP_OK) {
        goto cfi_out;
    }
    tmp32 = ((((uint32_t)hi << 8) | (uint32_t)lo) * 2u);
    if (tmp32 + 0x3Cu >= 0x400u) {
        tmp32 = 0x80u;
    }
    if (tmp32 + 0x1Eu < 0x400u) {
        uint8_t pri_p = 0u;
        uint8_t pri_r = 0u;
        uint8_t pri_i = 0u;

        err = burner_bacon_gbc_read_u8(tmp32 + 0u, &pri_p);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(tmp32 + 2u, &pri_r);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        err = burner_bacon_gbc_read_u8(tmp32 + 4u, &pri_i);
        if (err != ESP_OK) {
            goto cfi_out;
        }
        if (pri_p == 'P' && pri_r == 'R' && pri_i == 'I') {
            uint8_t tb_boot_sector_raw = 0u;

            err = burner_bacon_gbc_read_u8(tmp32 + 0x1Eu, &tb_boot_sector_raw);
            if (err != ESP_OK) {
                goto cfi_out;
            }
            reverse_sector_region = (tb_boot_sector_raw == 0x03u);
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
        goto cfi_out;
    }
    *sector_size = burner_nor_geometry_report_sector_size(geometry);

cfi_out:
    if (entered_cfi) {
        esp_err_t reset_err = burner_bacon_gbc_write(0x0000u, &reset_cmd, 1);
        if (err == ESP_OK && reset_err != ESP_OK) {
            err = reset_err;
        }
    }
    return err;
}

static esp_err_t burner_bacon_mbc5_probe_locked(
    uint8_t id_out[4],
    uint32_t *device_size,
    uint32_t *sector_size,
    uint16_t *buffer_write_bytes,
    bool *cfi_ok_out,
    burner_nor_cmdset_t *cmdset_out)
{
    uint32_t cfi_device_size = 0u;
    uint32_t cfi_sector_size = 0u;
    uint16_t cfi_buffer_write_bytes = 0u;
    burner_nor_geometry_t cfi_geometry = {0};
    burner_nor_cmdset_t cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    const burner_nor_entry_t *id_entry = NULL;
    burner_nor_geometry_t id_geometry = {0};
    uint32_t id_device_size = 0u;
    uint32_t id_sector_size = 0u;
    uint16_t id_buffer_write_bytes = 0u;
    burner_nor_cmdset_t id_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    bool id_geometry_ok = false;
    uint32_t cfi_try;
    esp_err_t err = ESP_FAIL;

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
    burner_nor_geometry_clear(&s_cart_ctx.geometry);

    err = burner_bacon_mbc5_get_id(id_out);
    if (err == ESP_OK) {
        id_entry = burner_nor_db_lookup_mbc5(id_out);
        id_cmdset = burner_nor_entry_cmdset(id_entry);
        id_geometry_ok = burner_mbc5_probe_load_entry_geometry(
            id_out,
            id_entry,
            &id_device_size,
            &id_sector_size,
            &id_buffer_write_bytes,
            &id_geometry);
        ESP_LOGI(
            BURNER_TAG,
            "MBC5 ID probe: id=%02X %02X %02X %02X chip=%s flash=%" PRIu32
            " sector=%" PRIu32 " geom=%s largest=%" PRIu32 " regions=%u buf=%u cmdset=%s",
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3],
            burner_nor_entry_name(id_entry),
            id_device_size,
            id_sector_size,
            burner_nor_geometry_is_uniform(&id_geometry) ? "uniform" : "mixed",
            burner_nor_geometry_largest_sector_size(&id_geometry),
            (unsigned)id_geometry.region_count,
            (unsigned)id_buffer_write_bytes,
            burner_nor_cmdset_name(id_cmdset));
    } else {
        ESP_LOGW(BURNER_TAG, "MBC5 ID probe failed before CFI: %s", esp_err_to_name(err));
        memset(id_out, 0, 4u);
    }

    err = ESP_FAIL;
    for (cfi_try = 0u; cfi_try < 3u; ++cfi_try) {
        burner_nor_geometry_clear(&cfi_geometry);
        cfi_device_size = 0u;
        cfi_sector_size = 0u;
        cfi_buffer_write_bytes = 0u;
        cfi_cmdset = BURNER_NOR_CMDSET_UNKNOWN;
        err = burner_bacon_mbc5_get_cfi(
            &cfi_device_size,
            &cfi_sector_size,
            &cfi_buffer_write_bytes,
            &cfi_geometry,
            &cfi_cmdset);
        if (err == ESP_OK) {
            if (!id_geometry_ok) {
                esp_err_t id_retry_err = burner_bacon_mbc5_get_id(id_out);

                if (id_retry_err == ESP_OK) {
                    id_entry = burner_nor_db_lookup_mbc5(id_out);
                    id_cmdset = burner_nor_entry_cmdset(id_entry);
                    id_geometry_ok = burner_mbc5_probe_load_entry_geometry(
                        id_out,
                        id_entry,
                        &id_device_size,
                        &id_sector_size,
                        &id_buffer_write_bytes,
                        &id_geometry);
                    ESP_LOGI(
                        BURNER_TAG,
                        "MBC5 ID retry after CFI: id=%02X %02X %02X %02X chip=%s flash=%" PRIu32
                        " sector=%" PRIu32 " geom=%s largest=%" PRIu32 " regions=%u buf=%u cmdset=%s",
                        id_out[0],
                        id_out[1],
                        id_out[2],
                        id_out[3],
                        burner_nor_entry_name(id_entry),
                        id_device_size,
                        id_sector_size,
                        burner_nor_geometry_is_uniform(&id_geometry) ? "uniform" : "mixed",
                        burner_nor_geometry_largest_sector_size(&id_geometry),
                        (unsigned)id_geometry.region_count,
                        (unsigned)id_buffer_write_bytes,
                        burner_nor_cmdset_name(id_cmdset));
                } else {
                    ESP_LOGW(BURNER_TAG, "MBC5 ID retry after CFI failed: %s", esp_err_to_name(id_retry_err));
                    memset(id_out, 0, 4u);
                }
            }
            if (id_geometry_ok &&
                burner_mbc5_geometry_should_prefer_id(
                    &cfi_geometry,
                    cfi_device_size,
                    cfi_sector_size,
                    &id_geometry,
                    id_device_size,
                    id_sector_size)) {
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 CFI geometry conflicts with ID, using ID geometry: cfi_flash=%" PRIu32
                    " cfi_sector=%" PRIu32 " cfi_geom=%s cfi_largest=%" PRIu32
                    " id_flash=%" PRIu32 " id_sector=%" PRIu32 " chip=%s",
                    cfi_device_size,
                    cfi_sector_size,
                    burner_nor_geometry_is_uniform(&cfi_geometry) ? "uniform" : "mixed",
                    burner_nor_geometry_largest_sector_size(&cfi_geometry),
                    id_device_size,
                    id_sector_size,
                    burner_nor_entry_name(id_entry));
                cfi_device_size = id_device_size;
                cfi_sector_size = id_sector_size;
                if (cfi_buffer_write_bytes == 0u || id_buffer_write_bytes < cfi_buffer_write_bytes) {
                    cfi_buffer_write_bytes = id_buffer_write_bytes;
                }
                cfi_geometry = id_geometry;
            } else if (id_geometry_ok) {
                if (cfi_device_size == 0u) {
                    cfi_device_size = id_device_size;
                }
                if (cfi_sector_size == 0u) {
                    cfi_sector_size = id_sector_size;
                }
                if (cfi_buffer_write_bytes == 0u) {
                    cfi_buffer_write_bytes = id_buffer_write_bytes;
                }
                if (!burner_nor_geometry_is_valid(&cfi_geometry)) {
                    cfi_geometry = id_geometry;
                }
            }
            if (!burner_nor_geometry_is_valid(&cfi_geometry) ||
                cfi_device_size == 0u || cfi_sector_size == 0u) {
                err = ESP_ERR_INVALID_SIZE;
                continue;
            }
            if (id_cmdset != BURNER_NOR_CMDSET_UNKNOWN &&
                cfi_cmdset != BURNER_NOR_CMDSET_UNKNOWN &&
                cfi_cmdset != id_cmdset) {
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 CFI cmdset conflicts with ID, using ID cmdset: cfi=%s id=%s chip=%s",
                    burner_nor_cmdset_name(cfi_cmdset),
                    burner_nor_cmdset_name(id_cmdset),
                    burner_nor_entry_name(id_entry));
            }
            if (id_buffer_write_bytes > 0u && cfi_buffer_write_bytes > id_buffer_write_bytes) {
                ESP_LOGW(
                    BURNER_TAG,
                    "MBC5 CFI buffer larger than ID limit, clamping: cfi_buf=%u id_buf=%u chip=%s",
                    (unsigned)cfi_buffer_write_bytes,
                    (unsigned)id_buffer_write_bytes,
                    burner_nor_entry_name(id_entry));
                cfi_buffer_write_bytes = id_buffer_write_bytes;
            }
            *device_size = cfi_device_size;
            *sector_size = cfi_sector_size;
            *buffer_write_bytes = cfi_buffer_write_bytes;
            *cfi_ok_out = true;
            if (cmdset_out != NULL) {
                *cmdset_out = (id_cmdset != BURNER_NOR_CMDSET_UNKNOWN) ? id_cmdset : cfi_cmdset;
            }
            s_cart_ctx.geometry = cfi_geometry;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (id_geometry_ok) {
        *device_size = id_device_size;
        *sector_size = id_sector_size;
        *buffer_write_bytes = id_buffer_write_bytes;
        *cfi_ok_out = false;
        if (cmdset_out != NULL) {
            *cmdset_out = id_cmdset;
        }
        s_cart_ctx.geometry = id_geometry;
        ESP_LOGW(
            BURNER_TAG,
            "MBC5 CFI read failed after retries, fallback by ID geometry: flash=%" PRIu32
            " sector=%" PRIu32 " buf=%u chip=%s id=%02X %02X %02X %02X",
            *device_size,
            *sector_size,
            (unsigned)*buffer_write_bytes,
            burner_nor_entry_name(id_entry),
            id_out[0],
            id_out[1],
            id_out[2],
            id_out[3]);
        return ESP_OK;
    }

    burner_nor_geometry_clear(&s_cart_ctx.geometry);
    return err;
}

esp_err_t burner_bacon_mbc5_prepare_probe_info_locked(
    uint8_t id_out[4],
    uint32_t total_bytes,
    uint32_t *device_size_out,
    uint32_t *sector_size_out,
    uint16_t *buffer_write_bytes_out,
    bool *cfi_ok_out,
    burner_nor_cmdset_t *cmdset_out,
    const char **mapper_name_out)
{
    char chip_name[48] = {0};
    uint8_t local_id[4] = {0};
    uint32_t device_size = 0u;
    uint32_t probed_device_size = 0u;
    uint32_t sector_size = 0u;
    uint16_t buffer_write_bytes = 0u;
    bool cfi_ok = false;
    burner_nor_cmdset_t cmdset = BURNER_NOR_CMDSET_UNKNOWN;
    burner_gb_mapper_t mapper = BURNER_GB_MAPPER_MBC5;
    esp_err_t err;

    if (id_out == NULL || device_size_out == NULL || sector_size_out == NULL ||
        buffer_write_bytes_out == NULL || cfi_ok_out == NULL || total_bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_bacon_mbc5_probe_locked(
        local_id,
        &device_size,
        &sector_size,
        &buffer_write_bytes,
        &cfi_ok,
        &cmdset);
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "MBC5 probe failed: %s", esp_err_to_name(err));
        return err;
    }
    if (cmdset != BURNER_NOR_CMDSET_AMD) {
        ESP_LOGE(
            BURNER_TAG,
            "MBC5 cmdset unsupported for write path: %s",
            burner_nor_cmdset_name(cmdset));
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!cfi_ok) {
        ESP_LOGW(BURNER_TAG, "MBC5 CFI unavailable; continuing with ID geometry");
    }
    if (burner_mbc5_nor_has_flag(local_id, BURNER_NOR_FLAG_LIMIT_BUFFER_TO_ID)) {
        uint32_t id_device_size = 0u;
        uint32_t id_sector_size = 0u;
        uint16_t id_buffer_write_bytes = 0u;

        if (burner_mbc5_geometry_from_id(
                local_id,
                &id_device_size,
                &id_sector_size,
                &id_buffer_write_bytes) &&
            id_buffer_write_bytes > 0u) {
            buffer_write_bytes = id_buffer_write_bytes;
        }
    }
    if (buffer_write_bytes > 512u) {
        /* Keep a conservative upper bound for GBC flash families currently supported. */
        buffer_write_bytes = 512u;
    }
    if (buffer_write_bytes > ((BURNER_SPI_MAX_XFER - 30u) / 6u)) {
        buffer_write_bytes = (uint16_t)((BURNER_SPI_MAX_XFER - 30u) / 6u);
    }

    probed_device_size = device_size;
    if (s_gb_mapper_override_kind != BURNER_GB_MAPPER_UNKNOWN) {
        mapper = s_gb_mapper_override_kind;
        ESP_LOGI(BURNER_TAG, "GB mapper override: using %s", burner_gb_mapper_name(mapper));
    } else {
        err = burner_bacon_gb_detect_mapper(&mapper);
        if (err != ESP_OK) {
            ESP_LOGW(BURNER_TAG, "GB mapper detect failed, defaulting to MBC5: %s", esp_err_to_name(err));
            mapper = BURNER_GB_MAPPER_MBC5;
            err = ESP_OK;
        }
    }
    s_gb_mapper_kind = mapper;
    if (burner_gb_mapper_device_size_limit(mapper) > 0u &&
        device_size > burner_gb_mapper_device_size_limit(mapper)) {
        device_size = burner_gb_mapper_device_size_limit(mapper);
        if (burner_nor_geometry_limit_prefix(&s_cart_ctx.geometry, device_size) != ESP_OK) {
            ESP_LOGE(BURNER_TAG, "GB mapper geometry clamp failed");
            return ESP_ERR_INVALID_SIZE;
        }
        sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    }

    if (total_bytes > device_size) {
        ESP_LOGE(
            BURNER_TAG,
            "ROM larger than flash: rom=%" PRIu32 " flash=%" PRIu32,
            total_bytes,
            device_size);
        return ESP_ERR_INVALID_SIZE;
    }

    s_cart_ctx.prepared = true;
    s_cart_ctx.current_bank = UINT16_MAX;
    s_cart_ctx.buffer_write_bytes = buffer_write_bytes;
    s_cart_ctx.sector_size = sector_size;
    s_cart_ctx.device_size = device_size;
    memcpy(s_cart_ctx.mbc5_id, local_id, sizeof(s_cart_ctx.mbc5_id));

    burner_nor_format_chip_name(
        chip_name,
        sizeof(chip_name),
        burner_mbc5_chip_name(local_id),
        cmdset,
        probed_device_size);
    ESP_LOGI(
        BURNER_TAG,
        "GB prepared: mapper=%s flash=%" PRIu32 " usable=%" PRIu32 " sector=%" PRIu32
        " geom=%s largest=%" PRIu32 " regions=%u buf=%u nor=%s cfi=%s id=%02X %02X %02X %02X",
        burner_gb_mapper_name(mapper),
        probed_device_size,
        device_size,
        sector_size,
        burner_nor_geometry_is_uniform(&s_cart_ctx.geometry) ? "uniform" : "mixed",
        burner_nor_geometry_largest_sector_size(&s_cart_ctx.geometry),
        (unsigned)s_cart_ctx.geometry.region_count,
        (unsigned)buffer_write_bytes,
        burner_nor_cmdset_name(cmdset),
        cfi_ok ? "ok" : "id-fallback",
        local_id[0],
        local_id[1],
        local_id[2],
        local_id[3]);

    burner_status_set_probe_info(
        BURNER_CART_MODE_MBC5,
        local_id,
        4u,
        device_size,
        sector_size,
        buffer_write_bytes,
        cfi_ok,
        false,
        false,
        false,
        false,
        chip_name,
        burner_gb_mapper_name(mapper));

    memcpy(id_out, local_id, sizeof(local_id));
    *device_size_out = device_size;
    *sector_size_out = sector_size;
    *buffer_write_bytes_out = buffer_write_bytes;
    *cfi_ok_out = cfi_ok;
    if (cmdset_out != NULL) {
        *cmdset_out = cmdset;
    }
    if (mapper_name_out != NULL) {
        *mapper_name_out = burner_gb_mapper_name(mapper);
    }
    return ESP_OK;
}

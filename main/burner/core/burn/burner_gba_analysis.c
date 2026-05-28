/* GBA header game code lives at 0xAC..0xAF. */
#define BURNER_GBA_GAME_CODE_OFFSET 0xACu
#define BURNER_GBA_GAME_CODE_LEN 4u
#define BURNER_GBA_GAME_CODE_HEADER_BYTES (BURNER_GBA_GAME_CODE_OFFSET + BURNER_GBA_GAME_CODE_LEN)

typedef struct {
    const char *game_code;
    burner_gba_save_type_t save_type;
    uint32_t save_size;
} burner_gba_save_header_hint_t;

/*
 * Retail carts often don't expose writable NOR IDs/CFI and some titles don't
 * leave the usual SRAM/EEPROM/FLASH strings in easy-to-scan ROM regions.
 * Keep this table tiny and focused for known exceptions we want to handle.
 */
static const burner_gba_save_header_hint_t s_gba_save_header_hints[] = {
    {"RZWE", BURNER_GBA_SAVE_TYPE_SRAM, 32u * 1024u}, /* WarioWare: Twisted! */
    {"RZWJ", BURNER_GBA_SAVE_TYPE_SRAM, 32u * 1024u}, /* Mawaru Made in Wario */
};

bool burner_extract_ascii_cart_title(
    const uint8_t *raw,
    size_t raw_len,
    char *title,
    size_t title_len)
{
    size_t out_idx = 0u;
    size_t trim_len = 0u;
    size_t i;

    if (raw == NULL || title == NULL || title_len < 2u) {
        return false;
    }

    title[0] = '\0';
    for (i = 0u; i < raw_len && out_idx + 1u < title_len; ++i) {
        uint8_t ch = raw[i];

        if (ch == 0x00u || ch == 0xFFu) {
            break;
        }
        if (ch < 0x20u || ch > 0x7Eu) {
            break;
        }

        title[out_idx++] = (char)ch;
        if (ch != ' ') {
            trim_len = out_idx;
        }
    }

    if (trim_len == 0u) {
        return false;
    }

    title[trim_len] = '\0';
    return true;
}

bool burner_try_probe_cart_title(
    burner_cart_mode_t cart_mode,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool gba_force_multi,
    char *title,
    size_t title_len)
{
    burner_task_param_t probe_job = {0};
    uint8_t header[BURNER_MBC5_TITLE_LEN] = {0};
    size_t header_len = 0u;
    esp_err_t err = ESP_OK;

    if (title == NULL || title_len < 2u || total_bytes == 0u) {
        return false;
    }
    if (burner_task_is_running_snapshot()) {
        return false;
    }

    title[0] = '\0';
    err = burner_spi_init();
    if (err != ESP_OK) {
        ESP_LOGW(BURNER_TAG, "dump title probe init failed: %s", esp_err_to_name(err));
        return false;
    }

    probe_job.mode = BURNER_JOB_READ_ROM;
    probe_job.cart_mode = cart_mode;
    probe_job.addr_begin = addr_begin;
    probe_job.total_bytes = total_bytes;
    probe_job.gba_force_multi = gba_force_multi;

    burner_spi_lock_take();
    if (cart_mode == BURNER_CART_MODE_GBA) {
        header_len = BURNER_GBA_TITLE_LEN;
        err = burner_spi_prepare_burn_gba(&probe_job);
        if (err == ESP_OK) {
            err = burner_bacon_gba_read_block(
                header,
                header_len,
                addr_begin + BURNER_GBA_TITLE_OFFSET,
                burner_is_gba_multi_card(&probe_job));
        }
    } else {
        header_len = BURNER_MBC5_TITLE_LEN;
        err = burner_spi_prepare_burn_mbc5(&probe_job);
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_read_block(
                header,
                header_len,
                addr_begin + BURNER_MBC5_TITLE_OFFSET);
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "dump title probe failed mode=%s addr=0x%08" PRIX32 ": %s",
            (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
            addr_begin,
            esp_err_to_name(err));
        return false;
    }

    return burner_extract_ascii_cart_title(header, header_len, title, title_len);
}

static bool burner_extract_gba_game_code(
    const uint8_t *raw,
    size_t raw_len,
    char *game_code,
    size_t game_code_len)
{
    size_t i;

    if (raw == NULL || game_code == NULL || game_code_len < (BURNER_GBA_GAME_CODE_LEN + 1u) ||
        raw_len < BURNER_GBA_GAME_CODE_HEADER_BYTES) {
        return false;
    }

    for (i = 0u; i < BURNER_GBA_GAME_CODE_LEN; ++i) {
        uint8_t ch = raw[BURNER_GBA_GAME_CODE_OFFSET + i];

        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'))) {
            return false;
        }
        game_code[i] = (char)ch;
    }
    game_code[BURNER_GBA_GAME_CODE_LEN] = '\0';
    return true;
}

static bool burner_lookup_gba_save_type_by_game_code(
    const char *game_code,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out)
{
    size_t i;

    if (game_code == NULL || save_type_out == NULL || save_size_out == NULL) {
        return false;
    }

    for (i = 0u; i < sizeof(s_gba_save_header_hints) / sizeof(s_gba_save_header_hints[0]); ++i) {
        if (strcmp(game_code, s_gba_save_header_hints[i].game_code) == 0) {
            *save_type_out = s_gba_save_header_hints[i].save_type;
            *save_size_out = s_gba_save_header_hints[i].save_size;
            return true;
        }
    }

    return false;
}

static bool burner_detect_gba_save_type_from_header_game_code(
    const uint8_t *raw,
    size_t raw_len,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out)
{
    char game_code[BURNER_GBA_GAME_CODE_LEN + 1u];

    if (!burner_extract_gba_game_code(raw, raw_len, game_code, sizeof(game_code))) {
        return false;
    }
    if (!burner_lookup_gba_save_type_by_game_code(game_code, save_type_out, save_size_out)) {
        return false;
    }

    ESP_LOGI(
        BURNER_TAG,
        "GBA save type matched by game code %s: type=%u size=%" PRIu32,
        game_code,
        (unsigned)*save_type_out,
        *save_size_out);
    return true;
}

static bool burner_probe_gba_save_type_from_header_locked(
    const burner_task_param_t *job,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out)
{
    uint8_t header[BURNER_GBA_GAME_CODE_HEADER_BYTES] = {0};

    if (job == NULL || save_type_out == NULL || save_size_out == NULL ||
        job->total_bytes < BURNER_GBA_GAME_CODE_HEADER_BYTES) {
        return false;
    }

    if (burner_bacon_gba_read_block(
            header,
            sizeof(header),
            job->addr_begin,
            burner_is_gba_multi_card(job)) != ESP_OK) {
        return false;
    }

    return burner_detect_gba_save_type_from_header_game_code(
        header,
        sizeof(header),
        save_type_out,
        save_size_out);
}

static bool burner_memmem_ascii(
    const uint8_t *buf,
    size_t buf_len,
    const char *needle)
{
    size_t needle_len;
    size_t i;

    if (buf == NULL || needle == NULL) {
        return false;
    }
    needle_len = strlen(needle);
    if (needle_len == 0u || buf_len < needle_len) {
        return false;
    }

    for (i = 0u; i + needle_len <= buf_len; ++i) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static void burner_probe_scan_yield_if_needed(uint64_t *last_yield_us)
{
    uint64_t now_us;

    if (last_yield_us == NULL) {
        return;
    }

    now_us = esp_timer_get_time();
    if (*last_yield_us != 0u && (now_us - *last_yield_us) < BURNER_CPU_YIELD_INTERVAL_US) {
        return;
    }

    *last_yield_us = now_us;
    vTaskDelay(1);
}

static bool burner_memmem_ascii_windowed(
    const uint8_t *buf,
    size_t buf_len,
    const char *needle)
{
    size_t needle_len;
    size_t offset = 0u;
    uint64_t last_yield_us = 0u;

    if (buf == NULL || needle == NULL) {
        return false;
    }
    needle_len = strlen(needle);
    if (needle_len == 0u || buf_len < needle_len) {
        return false;
    }

    while (offset < buf_len) {
        size_t scan_start = (offset > (needle_len - 1u)) ? (offset - (needle_len - 1u)) : 0u;
        size_t covered = offset - scan_start;
        size_t scan_len = buf_len - scan_start;
        size_t window_len = BURNER_PROBE_SCAN_WINDOW_BYTES + covered;

        if (scan_len > window_len) {
            scan_len = window_len;
        }
        if (burner_memmem_ascii(buf + scan_start, scan_len, needle)) {
            return true;
        }
        offset += BURNER_PROBE_SCAN_WINDOW_BYTES;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    return false;
}

static bool burner_detect_gba_save_type_in_span(
    const uint8_t *buf,
    size_t buf_len,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out)
{
    if (save_type_out == NULL || save_size_out == NULL) {
        return false;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;

    if (buf == NULL || buf_len == 0u) {
        return false;
    }

    if (burner_memmem_ascii_windowed(buf, buf_len, "FLASH1M_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_FLASH;
        *save_size_out = 128u * 1024u;
        return true;
    }
    if (burner_memmem_ascii_windowed(buf, buf_len, "FLASH512_V") ||
        burner_memmem_ascii_windowed(buf, buf_len, "FLASH_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_FLASH;
        *save_size_out = 64u * 1024u;
        return true;
    }
    if (burner_memmem_ascii_windowed(buf, buf_len, "EEPROM_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_EEPROM;
        *save_size_out = 8u * 1024u;
        return true;
    }
    if (burner_memmem_ascii_windowed(buf, buf_len, "SRAM_V")) {
        *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
        *save_size_out = 32u * 1024u;
        return true;
    }

    return false;
}

static bool burner_detect_gba_save_type_from_rom_locked(
    const burner_task_param_t *job,
    const uint8_t *prefix,
    size_t prefix_len,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out)
{
    static const char *const s_save_signatures[] = {
        "FLASH1M_V",
        "FLASH512_V",
        "FLASH_V",
        "EEPROM_V",
        "SRAM_V",
    };
    uint8_t *scan_buf = NULL;
    uint32_t scan_total = 0u;
    uint32_t offset = 0u;
    size_t carry_len = 0u;
    size_t overlap = 0u;
    bool detected = false;
    uint64_t last_yield_us = 0u;
    size_t i;

    if (job == NULL || save_type_out == NULL || save_size_out == NULL) {
        return false;
    }

    scan_total = job->total_bytes;
    if (scan_total == 0u) {
        return false;
    }

    for (i = 0u; i < sizeof(s_save_signatures) / sizeof(s_save_signatures[0]); ++i) {
        size_t needle_len = strlen(s_save_signatures[i]);
        if (needle_len > 0u && overlap < needle_len - 1u) {
            overlap = needle_len - 1u;
        }
    }

    if (prefix != NULL && prefix_len > overlap) {
        prefix += prefix_len - overlap;
        prefix_len = overlap;
    }

    scan_buf = (uint8_t *)malloc(BURNER_GBA_SAVE_SCAN_STEP_BYTES + overlap);
    if (scan_buf == NULL) {
        return false;
    }
    if (prefix != NULL && prefix_len > 0u) {
        memcpy(scan_buf, prefix, prefix_len);
        carry_len = prefix_len;
    }

    while (offset < scan_total) {
        size_t chunk = scan_total - offset;
        size_t scan_len;
        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }

        if (burner_bacon_gba_read_block(
                scan_buf + carry_len,
                chunk,
                job->addr_begin + offset,
                burner_is_gba_multi_card(job)) != ESP_OK) {
            break;
        }

        scan_len = carry_len + chunk;
        if (burner_detect_gba_save_type_in_span(scan_buf, scan_len, save_type_out, save_size_out)) {
            detected = true;
            break;
        }

        if (scan_len <= overlap) {
            carry_len = scan_len;
        } else {
            carry_len = overlap;
            memmove(scan_buf, scan_buf + scan_len - carry_len, carry_len);
        }
        offset += (uint32_t)chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    free(scan_buf);
    return detected;
}

static bool burner_memmem_binary(const uint8_t *haystack, size_t haystack_len, const uint8_t *needle, size_t needle_len)
{
    size_t i;

    if (haystack == NULL || needle == NULL || needle_len == 0u || haystack_len < needle_len) {
        return false;
    }
    for (i = 0u; i + needle_len <= haystack_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool burner_memmem_binary_windowed(
    const uint8_t *haystack,
    size_t haystack_len,
    const uint8_t *needle,
    size_t needle_len)
{
    size_t offset = 0u;
    uint64_t last_yield_us = 0u;

    if (haystack == NULL || needle == NULL || needle_len == 0u || haystack_len < needle_len) {
        return false;
    }

    while (offset < haystack_len) {
        size_t scan_start = (offset > (needle_len - 1u)) ? (offset - (needle_len - 1u)) : 0u;
        size_t covered = offset - scan_start;
        size_t scan_len = haystack_len - scan_start;
        size_t window_len = BURNER_PROBE_SCAN_WINDOW_BYTES + covered;

        if (scan_len > window_len) {
            scan_len = window_len;
        }
        if (burner_memmem_binary(haystack + scan_start, scan_len, needle, needle_len)) {
            return true;
        }
        offset += BURNER_PROBE_SCAN_WINDOW_BYTES;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    return false;
}

static bool burner_detect_gbata_sram_patch_in_span(const uint8_t *buf, size_t buf_len)
{
    static const uint8_t s_gbata_flash512_stub1[] = {
        0x70, 0xB5, 0xA0, 0xB0, 0x00, 0x03, 0x40, 0x18,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x08, 0x78,
        0x10, 0x70, 0x01, 0x3B, 0x01, 0x32, 0x01, 0x31,
        0x00, 0x2B, 0xF8, 0xD1, 0x00, 0x20, 0x20, 0xB0,
    };
    static const uint8_t s_gbata_flash_v121_stub[] = {
        0x70, 0xB5, 0x90, 0xB0, 0x00, 0x03, 0x0A, 0x1C,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x01, 0x23,
        0x1B, 0x03, 0x10, 0x78, 0x08, 0x70, 0x01, 0x3B,
        0x01, 0x32, 0x01, 0x31, 0x00, 0x2B, 0xF8, 0xD1,
    };
    static const uint8_t s_gbata_flash512_ret_stub[] = {
        0x00, 0x03, 0x40, 0x18, 0xE0, 0x21, 0x09, 0x05,
        0x09, 0x18, 0x08, 0x78, 0x10, 0x70, 0x01, 0x3B,
        0x01, 0x32, 0x01, 0x31, 0x00, 0x2B, 0xF8, 0xD1,
        0x00, 0x20, 0x20, 0xB0, 0x70, 0xBC, 0x02, 0xBC,
        0x08, 0x47,
    };
    static const uint8_t s_gbata_flash_v121_ret_stub[] = {
        0x00, 0x03, 0x0A, 0x1C, 0xE0, 0x21, 0x09, 0x05,
        0x09, 0x18, 0x01, 0x23, 0x1B, 0x03, 0x10, 0x78,
        0x08, 0x70, 0x01, 0x3B, 0x01, 0x32, 0x01, 0x31,
        0x00, 0x2B, 0xF8, 0xD1, 0x00, 0x20, 0x10, 0xB0,
        0x7C, 0xBC, 0x02, 0xBC, 0x08, 0x47,
    };

    if (buf == NULL || buf_len < sizeof(s_gbata_flash_v121_stub)) {
        return false;
    }

    return burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash512_stub1, sizeof(s_gbata_flash512_stub1)) ||
           burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash_v121_stub, sizeof(s_gbata_flash_v121_stub)) ||
           burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash512_ret_stub, sizeof(s_gbata_flash512_ret_stub)) ||
           burner_memmem_binary_windowed(buf, buf_len, s_gbata_flash_v121_ret_stub, sizeof(s_gbata_flash_v121_ret_stub));
}

static bool burner_detect_gbata_sram_patch_from_rom_locked(const burner_task_param_t *job)
{
    static const uint8_t s_gbata_flash512_stub1[] = {
        0x70, 0xB5, 0xA0, 0xB0, 0x00, 0x03, 0x40, 0x18,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x08, 0x78,
        0x10, 0x70, 0x01, 0x3B, 0x01, 0x32, 0x01, 0x31,
        0x00, 0x2B, 0xF8, 0xD1, 0x00, 0x20, 0x20, 0xB0,
    };
    static const uint8_t s_gbata_flash_v121_stub[] = {
        0x70, 0xB5, 0x90, 0xB0, 0x00, 0x03, 0x0A, 0x1C,
        0xE0, 0x21, 0x09, 0x05, 0x09, 0x18, 0x01, 0x23,
        0x1B, 0x03, 0x10, 0x78, 0x08, 0x70, 0x01, 0x3B,
        0x01, 0x32, 0x01, 0x31, 0x00, 0x2B, 0xF8, 0xD1,
    };
    uint8_t *buf = NULL;
    uint32_t scan_total;
    uint32_t offset = 0u;
    size_t carry_len = 0u;
    size_t overlap;
    bool detected = false;
    uint64_t last_yield_us = 0u;

    if (job == NULL || job->total_bytes < sizeof(s_gbata_flash512_stub1)) {
        return false;
    }

    scan_total = job->total_bytes;
    overlap = sizeof(s_gbata_flash512_stub1) - 1u;
    if (overlap < sizeof(s_gbata_flash_v121_stub) - 1u) {
        overlap = sizeof(s_gbata_flash_v121_stub) - 1u;
    }
    buf = (uint8_t *)malloc(BURNER_GBA_SAVE_SCAN_STEP_BYTES + overlap);
    if (buf == NULL) {
        return false;
    }

    while (offset < scan_total) {
        size_t chunk = scan_total - offset;
        size_t scan_len;
        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }
        if (burner_bacon_gba_read_block(buf + carry_len, chunk, job->addr_begin + offset, burner_is_gba_multi_card(job)) != ESP_OK) {
            break;
        }
        scan_len = carry_len + chunk;
        if (burner_detect_gbata_sram_patch_in_span(buf, scan_len)) {
            detected = true;
            break;
        }
        if (scan_len <= overlap) {
            carry_len = scan_len;
        } else {
            carry_len = overlap;
            memmove(buf, buf + scan_len - carry_len, carry_len);
        }
        offset += (uint32_t)chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    free(buf);
    return detected;
}

static bool burner_detect_flash1m_repro_sram_patch_in_span(const uint8_t *buf, size_t buf_len)
{
    static const uint8_t s_flash1m_repro_patch_a[] = {
        0x80, 0x21, 0x09, 0x02, 0x09, 0x22, 0x12, 0x06,
        0x9F, 0x44, 0x90, 0x21, 0x09, 0x05, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x70, 0x70, 0x47, 0xFE, 0xFF,
        0xFF, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_b[] = {
        0x0E, 0x21, 0x09, 0x06, 0xFF, 0x24, 0x80, 0x22,
        0x13, 0x4B, 0x52, 0x02, 0x01, 0x3A, 0x8C, 0x54,
        0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_c[] = {
        0x08, 0x22, 0x00, 0x00, 0x52, 0x02, 0x01, 0x3A,
        0xA5, 0x54, 0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const char *const s_flash1m_repro_source_signatures[] = {
        "FLASH1M_V",
        "FLASH512_V",
    };
    size_t i;
    bool found_source_signature = false;

    if (buf == NULL || buf_len < sizeof(s_flash1m_repro_patch_a)) {
        return false;
    }

    for (i = 0u; i < sizeof(s_flash1m_repro_source_signatures) / sizeof(s_flash1m_repro_source_signatures[0]); ++i) {
        if (burner_memmem_ascii_windowed(buf, buf_len, s_flash1m_repro_source_signatures[i])) {
            found_source_signature = true;
            break;
        }
    }
    if (!found_source_signature) {
        return false;
    }

    return burner_memmem_binary_windowed(buf, buf_len, s_flash1m_repro_patch_a, sizeof(s_flash1m_repro_patch_a)) &&
           burner_memmem_binary_windowed(buf, buf_len, s_flash1m_repro_patch_b, sizeof(s_flash1m_repro_patch_b)) &&
           burner_memmem_binary_windowed(buf, buf_len, s_flash1m_repro_patch_c, sizeof(s_flash1m_repro_patch_c));
}

static bool burner_detect_flash1m_repro_sram_patch_from_rom_locked(const burner_task_param_t *job)
{
    static const uint8_t s_flash1m_repro_patch_a[] = {
        0x80, 0x21, 0x09, 0x02, 0x09, 0x22, 0x12, 0x06,
        0x9F, 0x44, 0x90, 0x21, 0x09, 0x05, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x70, 0x70, 0x47, 0xFE, 0xFF,
        0xFF, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_b[] = {
        0x0E, 0x21, 0x09, 0x06, 0xFF, 0x24, 0x80, 0x22,
        0x13, 0x4B, 0x52, 0x02, 0x01, 0x3A, 0x8C, 0x54,
        0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t s_flash1m_repro_patch_c[] = {
        0x08, 0x22, 0x00, 0x00, 0x52, 0x02, 0x01, 0x3A,
        0xA5, 0x54, 0xFC, 0xD1, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const char *const s_flash1m_repro_source_signatures[] = {
        "FLASH1M_V",
        "FLASH512_V",
    };
    uint8_t *buf = NULL;
    uint32_t scan_total;
    uint32_t offset = 0u;
    size_t carry_len = 0u;
    size_t overlap = sizeof(s_flash1m_repro_patch_a) - 1u;
    size_t i;
    uint64_t last_yield_us = 0u;

    if (job == NULL || job->total_bytes < sizeof(s_flash1m_repro_patch_a)) {
        return false;
    }

    if (overlap < sizeof(s_flash1m_repro_patch_b) - 1u) {
        overlap = sizeof(s_flash1m_repro_patch_b) - 1u;
    }
    if (overlap < sizeof(s_flash1m_repro_patch_c) - 1u) {
        overlap = sizeof(s_flash1m_repro_patch_c) - 1u;
    }
    for (i = 0u; i < sizeof(s_flash1m_repro_source_signatures) / sizeof(s_flash1m_repro_source_signatures[0]); ++i) {
        size_t needle_len = strlen(s_flash1m_repro_source_signatures[i]);
        if (overlap + 1u < needle_len) {
            overlap = needle_len - 1u;
        }
    }

    scan_total = job->total_bytes;
    buf = (uint8_t *)malloc(BURNER_GBA_SAVE_SCAN_STEP_BYTES + overlap);
    if (buf == NULL) {
        return false;
    }

    while (offset < scan_total) {
        size_t chunk = scan_total - offset;
        size_t scan_len;

        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }
        if (burner_bacon_gba_read_block(buf + carry_len, chunk, job->addr_begin + offset, burner_is_gba_multi_card(job)) != ESP_OK) {
            break;
        }

        scan_len = carry_len + chunk;
        if (burner_detect_flash1m_repro_sram_patch_in_span(buf, scan_len)) {
            free(buf);
            return true;
        }

        if (scan_len <= overlap) {
            carry_len = scan_len;
        } else {
            carry_len = overlap;
            memmove(buf, buf + scan_len - carry_len, carry_len);
        }
        offset += (uint32_t)chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    free(buf);
    return false;
}

static bool burner_detect_gba_sram_patch_in_span(
    const uint8_t *buf,
    size_t buf_len,
    burner_gba_sram_patch_kind_t *patch_kind_out)
{
    if (patch_kind_out == NULL) {
        return false;
    }

    *patch_kind_out = BURNER_GBA_SRAM_PATCH_NONE;
    if (burner_detect_gbata_sram_patch_in_span(buf, buf_len)) {
        *patch_kind_out = BURNER_GBA_SRAM_PATCH_GBATA;
        return true;
    }
    if (burner_detect_flash1m_repro_sram_patch_in_span(buf, buf_len)) {
        *patch_kind_out = BURNER_GBA_SRAM_PATCH_FLASH1M_REPRO;
        return true;
    }
    return false;
}

static esp_err_t burner_read_gba_rom_to_buffer_locked(
    const burner_task_param_t *job,
    uint8_t *dst,
    uint32_t offset,
    size_t total_len)
{
    size_t done = 0u;
    uint64_t last_yield_us = 0u;

    if (job == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (done < total_len) {
        size_t chunk = total_len - done;
        if (chunk > BURNER_GBA_SAVE_SCAN_STEP_BYTES) {
            chunk = BURNER_GBA_SAVE_SCAN_STEP_BYTES;
        }
        if (burner_bacon_gba_read_block(
                dst + done,
                chunk,
                job->addr_begin + offset + (uint32_t)done,
                burner_is_gba_multi_card(job)) != ESP_OK) {
            return ESP_FAIL;
        }
        done += chunk;
        burner_probe_scan_yield_if_needed(&last_yield_us);
    }

    return ESP_OK;
}

esp_err_t burner_probe_gba_rom_analysis_locked(
    uint32_t device_size,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *save_detected_out,
    burner_gba_sram_patch_kind_t *patch_kind_out,
    bool *patch_detected_out)
{
    uint8_t *head_cache = NULL;
    uint32_t head_scan_bytes = 0u;
    burner_gba_save_type_t save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t save_size = 0u;
    bool save_detected = false;
    burner_gba_sram_patch_kind_t patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
    bool patch_detected = false;
    esp_err_t err;

    if (save_type_out == NULL || save_size_out == NULL || save_detected_out == NULL ||
        patch_kind_out == NULL || patch_detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;
    *save_detected_out = false;
    *patch_kind_out = BURNER_GBA_SRAM_PATCH_NONE;
    *patch_detected_out = false;

    if (device_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    head_scan_bytes = device_size;
    if (head_scan_bytes > BURNER_GBA_ANALYSIS_HEAD_BYTES) {
        head_scan_bytes = BURNER_GBA_ANALYSIS_HEAD_BYTES;
    }

    err = ESP_OK;
    if (head_scan_bytes > 0u) {
        head_cache = (uint8_t *)heap_caps_malloc(head_scan_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (head_cache != NULL) {
            err = burner_read_gba_rom_to_buffer_locked(
                &(burner_task_param_t){
                    .cart_mode = BURNER_CART_MODE_GBA,
                    .addr_begin = 0u,
                    .total_bytes = device_size,
                },
                head_cache,
                0u,
                head_scan_bytes);
            if (err == ESP_OK) {
                save_detected = burner_detect_gba_save_type_in_span(head_cache, head_scan_bytes, &save_type, &save_size);
                patch_detected = burner_detect_gba_sram_patch_in_span(head_cache, head_scan_bytes, &patch_kind);
                if (!save_detected) {
                    save_detected = burner_detect_gba_save_type_from_header_game_code(
                        head_cache,
                        head_scan_bytes,
                        &save_type,
                        &save_size);
                }
            }
        } else {
            burner_task_param_t head_job = {
                .cart_mode = BURNER_CART_MODE_GBA,
                .addr_begin = 0u,
                .total_bytes = device_size,
            };

            head_job.total_bytes = head_scan_bytes;
            save_detected = burner_detect_gba_save_type_from_rom_locked(&head_job, NULL, 0u, &save_type, &save_size);
            if (!save_detected) {
                save_detected = burner_probe_gba_save_type_from_header_locked(&head_job, &save_type, &save_size);
            }
            patch_detected = burner_detect_gbata_sram_patch_from_rom_locked(&head_job);
            if (patch_detected) {
                patch_kind = BURNER_GBA_SRAM_PATCH_GBATA;
            } else if (burner_detect_flash1m_repro_sram_patch_from_rom_locked(&head_job)) {
                patch_detected = true;
                patch_kind = BURNER_GBA_SRAM_PATCH_FLASH1M_REPRO;
            }
        }
    }
    if (head_cache != NULL) {
        free(head_cache);
    }
    if (err != ESP_OK) {
        return err;
    }

    *save_type_out = save_type;
    *save_size_out = save_size;
    *save_detected_out = save_detected;
    *patch_kind_out = patch_detected ? patch_kind : BURNER_GBA_SRAM_PATCH_NONE;
    *patch_detected_out = patch_detected;
    return ESP_OK;
}

esp_err_t burner_probe_gba_rom_analysis(
    uint32_t device_size,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *save_detected_out,
    burner_gba_sram_patch_kind_t *patch_kind_out,
    bool *patch_detected_out)
{
    burner_task_param_t probe_job = {0};
    esp_err_t err;

    if (device_size == 0u) {
        err = burner_probe_cart_capacity_bytes(BURNER_CART_MODE_GBA, &device_size);
        if (err != ESP_OK) {
            return err;
        }
    }

    probe_job.cart_mode = BURNER_CART_MODE_GBA;
    probe_job.mode = BURNER_JOB_READ_ROM;
    probe_job.addr_begin = 0u;
    probe_job.total_bytes = device_size;

    burner_spi_lock_take();
    err = burner_spi_prepare_burn_gba(&probe_job);
    if (err == ESP_OK) {
        err = burner_probe_gba_rom_analysis_locked(
            device_size,
            save_type_out,
            save_size_out,
            save_detected_out,
            patch_kind_out,
            patch_detected_out);
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();
    return err;
}

esp_err_t burner_probe_gba_save_type_head_locked(
    uint32_t device_size,
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *detected_out)
{
    uint8_t *head_cache = NULL;
    uint32_t head_scan_bytes = 0u;
    burner_gba_save_type_t save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t save_size = 0u;
    bool detected = false;
    esp_err_t err = ESP_OK;

    if (save_type_out == NULL || save_size_out == NULL || detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;
    *detected_out = false;

    if (device_size == 0u) {
        return ESP_ERR_INVALID_SIZE;
    }

    head_scan_bytes = device_size;
    if (head_scan_bytes > BURNER_GBA_SAVE_HEADER_SCAN_BYTES) {
        head_scan_bytes = BURNER_GBA_SAVE_HEADER_SCAN_BYTES;
    }

    if (head_scan_bytes == 0u) {
        return ESP_OK;
    }

    head_cache = (uint8_t *)heap_caps_malloc(head_scan_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (head_cache != NULL) {
        err = burner_read_gba_rom_to_buffer_locked(
            &(burner_task_param_t){
                .cart_mode = BURNER_CART_MODE_GBA,
                .addr_begin = 0u,
                .total_bytes = device_size,
            },
            head_cache,
            0u,
            head_scan_bytes);
        if (err == ESP_OK) {
            detected = burner_detect_gba_save_type_in_span(head_cache, head_scan_bytes, &save_type, &save_size);
            if (!detected) {
                detected = burner_detect_gba_save_type_from_header_game_code(
                    head_cache,
                    head_scan_bytes,
                    &save_type,
                    &save_size);
            }
        }
        free(head_cache);
    } else {
        burner_task_param_t head_job = {
            .cart_mode = BURNER_CART_MODE_GBA,
            .addr_begin = 0u,
            .total_bytes = head_scan_bytes,
        };

        detected = burner_detect_gba_save_type_from_rom_locked(&head_job, NULL, 0u, &save_type, &save_size);
        if (!detected) {
            detected = burner_probe_gba_save_type_from_header_locked(&head_job, &save_type, &save_size);
        }
    }

    if (err != ESP_OK) {
        return err;
    }

    *save_type_out = save_type;
    *save_size_out = save_size;
    *detected_out = detected;
    return ESP_OK;
}

esp_err_t burner_probe_gba_save_type(
    burner_gba_save_type_t *save_type_out,
    uint32_t *save_size_out,
    bool *detected_out)
{
    burner_gba_sram_patch_kind_t patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
    bool patch_detected = false;
    esp_err_t err;

    if (save_type_out == NULL || save_size_out == NULL || detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *save_type_out = BURNER_GBA_SAVE_TYPE_SRAM;
    *save_size_out = 0u;
    *detected_out = false;

    err = burner_probe_gba_rom_analysis(
        0u,
        save_type_out,
        save_size_out,
        detected_out,
        &patch_kind,
        &patch_detected);
    return err;
}

esp_err_t burner_probe_gba_sram_patch(
    burner_gba_sram_patch_kind_t *patch_kind_out,
    bool *detected_out)
{
    burner_gba_save_type_t save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    uint32_t save_size = 0u;
    bool save_detected = false;
    esp_err_t err;

    if (patch_kind_out == NULL || detected_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *patch_kind_out = BURNER_GBA_SRAM_PATCH_NONE;
    *detected_out = false;

    err = burner_probe_gba_rom_analysis(
        0u,
        &save_type,
        &save_size,
        &save_detected,
        patch_kind_out,
        detected_out);
    return err;
}

void burner_build_default_dump_name(
    burner_cart_mode_t cart_mode,
    uint32_t addr_begin,
    uint32_t total_bytes,
    bool gba_force_multi,
    char *raw_name,
    size_t raw_name_len)
{
    char timestamp_text[32] = {0};
    char title[32] = {0};
    char title_safe[32] = {0};
    const char *ext = burner_rom_dump_ext_for_mode(cart_mode);

    if (raw_name == NULL || raw_name_len < 2u) {
        return;
    }

    raw_name[0] = '\0';
    burner_build_output_timestamp(timestamp_text, sizeof(timestamp_text));
    if (burner_try_probe_cart_title(
            cart_mode,
            addr_begin,
            total_bytes,
            gba_force_multi,
            title,
            sizeof(title)) &&
        burner_sanitize_filename(title, title_safe, sizeof(title_safe))) {
        (void)snprintf(raw_name, raw_name_len, "%s_%s%s", title_safe, timestamp_text, ext);
        return;
    }

    (void)snprintf(raw_name, raw_name_len, "dump_%s%s", timestamp_text, ext);
}

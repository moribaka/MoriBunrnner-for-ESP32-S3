/* Serial-only, explicitly requested capacity spot test. The caller owns
 * s_spi_lock and the performance lock for the entire backup/test/restore.
 * The 88B0 backup contains 16 consecutive 256 KiB sectors (bank first/last).
 * M36 backups concatenate the first/last sector of each CFI region; logged
 * addresses and sizes define that layout. Never overwrite an existing file. */
#define GBA_SPOT_SECTOR_BYTES (256u * 1024u)
#define GBA_SPOT_COUNT 16u
#define GBA_SPOT_MARKER_BYTES 1024u
#define GBA_SPOT_READ_BYTES 65536u

static uint32_t gba_spot_address(unsigned index)
{
    return (index / 2u) * BURN_GBA_BANK_BYTES +
        ((index & 1u) ? BURN_GBA_BANK_BYTES - GBA_SPOT_SECTOR_BYTES : 0u);
}

static void gba_spot_pattern(unsigned index, uint8_t *buffer)
{
    uint32_t value = 0x88B02026u ^ gba_spot_address(index);
    for (unsigned i = 0; i < GBA_SPOT_MARKER_BYTES; ++i) {
        value ^= value << 13; value ^= value >> 17; value ^= value << 5;
        buffer[i] = (uint8_t)value;
    }
}

/* Replay the raw bus sequence recovered from GBABF V60B3cn ARM9
 * 0x02003160..0x02003254. This lab path deliberately has no ready poll
 * between EA and count. Keep a bounded completion wait and status errors;
 * never import the reference program's silent timeout/partial-page bugs. */
static esp_err_t gba_spot_gbabf_marker(uint32_t address, const uint8_t *data)
{
    if (!data || address >= BURNER_GBA_88B0_SUPPORTED_BYTES || (address & 1023u) ||
        !s_gba_probe_88b0_window || !s_cart_ctx.d0d1_swapped)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = burner_gba_switch_bank_if_needed(address / BURN_GBA_BANK_BYTES);
    uint32_t word = (address % BURN_GBA_BANK_BYTES) >> 1;
    const uint16_t prefix[] = {0x0050u, 0x00FFu, 0x00EAu, 0x01FFu};
    for (unsigned i = 0; err == ESP_OK && i < 4u; ++i)
        err = burner_bacon_rom_write_u16(word, prefix[i]);
    for (unsigned i = 0; err == ESP_OK && i < 512u; ++i)
        err = burner_bacon_rom_write_u16(word + i, (uint16_t)(data[2*i] | ((uint16_t)data[2*i+1] << 8)));
    if (err == ESP_OK) err = burner_bacon_rom_write_u16(word, 0x00D0u);
    uint16_t raw = 0xFFFFu;
    int64_t deadline = esp_timer_get_time() + (int64_t)BURNER_ROM_POLL_TIMEOUT_MS * 1000;
    while (err == ESP_OK) {
        err = burner_bacon_rom_read_u16(word, &raw);
        if (err != ESP_OK) break;
        if (raw != 0xFFFFu && (raw & 0x80u)) {
            uint16_t status = burner_apply_d0d1_swap_on_read(raw, true);
            if (burner_gba_intel_status_has_error(status)) err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        err = burner_cancel_poll();
        esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
        burner_task_yield_if_due();
    }
    esp_err_t reset = burner_bacon_rom_write_u16(word, 0x00FFu);
    return err == ESP_OK ? reset : err;
}

/* GBABF Type 10: single-halfword programming for the live M36L0R806.
 * These command bytes are invariant under D0/D1 swap; payload is raw. */
static esp_err_t gba_spot_gbabf_words(uint32_t address, const uint8_t *data, size_t bytes)
{
    if (!data || (address & 1u) || (bytes & 1u) ||
        (uint64_t)address + bytes > s_cart_ctx.device_size) return ESP_ERR_INVALID_ARG;
    for (size_t off = 0; off < bytes; off += 2u) {
        uint16_t value = (uint16_t)(data[off] | ((uint16_t)data[off+1] << 8));
        if (value == 0xFFFFu) continue; // callers have erased the whole sector
        uint32_t word = (address + off) >> 1;
        esp_err_t err = burner_bacon_rom_write_u16(word, 0x50u);
        if (err == ESP_OK) err = burner_bacon_rom_write_u16(word, 0xFFu);
        if (err == ESP_OK) err = burner_bacon_rom_write_u16(word, 0x40u);
        if (err == ESP_OK) err = burner_bacon_rom_write_u16(word, value);
        uint16_t raw = 0xFFFFu;
        int64_t deadline = esp_timer_get_time() + (int64_t)BURNER_ROM_POLL_TIMEOUT_MS * 1000;
        while (err == ESP_OK) {
            err = burner_bacon_rom_read_u16(word, &raw);
            if (err != ESP_OK) break;
            if (raw != 0xFFFFu && (raw & 0x80u)) {
                if (burner_gba_intel_status_has_error(burner_apply_d0d1_swap_on_read(raw, s_cart_ctx.d0d1_swapped)))
                    err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
            err = burner_cancel_poll();
            esp_rom_delay_us(BURNER_ROM_POLL_INTERVAL_US);
            burner_task_yield_if_due();
        }
        esp_err_t reset = burner_bacon_rom_write_u16(word, 0xFFu);
        if (err == ESP_OK) err = reset;
        if (err != ESP_OK) return err;
        burner_task_yield_if_due();
    }
    return ESP_OK;
}

static esp_err_t gba_spot_test_locked(const char *backup_path, burner_gba_spot_report_t *report, bool gbabf, bool native_buffer)
{
    if (!backup_path || !report) return ESP_ERR_INVALID_ARG;
    memset(report, 0, sizeof(*report));
    report->mismatch_address = UINT32_MAX;
    uint32_t addresses[GBA_SPOT_COUNT], sizes[GBA_SPOT_COUNT];
    unsigned points = GBA_SPOT_COUNT;
    bool word_recipe = false, multi = true;
    uint16_t saved_program_buffer = 0;
    for (unsigned i = 0; i < points; ++i) {
        addresses[i] = gba_spot_address(i); sizes[i] = GBA_SPOT_SECTOR_BYTES;
    }
    uint8_t *original = heap_caps_malloc(GBA_SPOT_COUNT * GBA_SPOT_SECTOR_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *actual = heap_caps_malloc(GBA_SPOT_READ_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t marker[GBA_SPOT_MARKER_BYTES];
    FILE *file = NULL;
    esp_err_t err = original && actual ? ESP_OK : ESP_ERR_NO_MEM;
    bool prepared = false;
    burner_task_param_t job = {0};
    job.mode = BURNER_JOB_READ_ROM;
    job.cart_mode = BURNER_CART_MODE_GBA;
    job.recipe_mode = native_buffer ? s_burn_recipe_mode_default : BURNER_RECIPE_MODE_CHIS;
    job.total_bytes = gbabf ? 2u : 256u * 1024u * 1024u;
    if (err == ESP_OK) err = burner_spi_prepare_burn_gba(&job);
    if (err == ESP_OK) {
        prepared = true;
        saved_program_buffer = s_cart_ctx.program_buffer_write_bytes;
        if (gbabf && s_cart_ctx.probe_cfi_ok && s_cart_ctx.gba_cmdset == BURNER_NOR_CMDSET_INTEL &&
            s_cart_ctx.device_size == 33554432u && s_cart_ctx.buffer_write_bytes == 64u) {
            uint8_t id[8];
            err = burner_bacon_gba_read_id(id, s_cart_ctx.d0d1_swapped);
            if (err == ESP_OK && !(id[0]==0x20 && id[1]==0 && (id[2]==0x0D || id[2]==0x0E) && id[3]==0x88))
                err = ESP_ERR_NOT_SUPPORTED;
            if (err == ESP_OK && (!burner_nor_geometry_is_valid(&s_cart_ctx.geometry) || s_cart_ctx.geometry.region_count > 2u))
                err = ESP_ERR_INVALID_SIZE;
            if (err == ESP_OK) {
                points = 0; word_recipe = true; multi = false;
                for (unsigned r = 0; r < s_cart_ctx.geometry.region_count; ++r) {
                    const burner_nor_region_t *region = &s_cart_ctx.geometry.regions[r];
                    uint32_t candidates[] = {region->addr_begin, region->addr_end - region->sector_size};
                    if (region->sector_size > GBA_SPOT_SECTOR_BYTES || region->sector_size < GBA_SPOT_MARKER_BYTES) {
                        err = ESP_ERR_INVALID_SIZE; break;
                    }
                    for (unsigned j = 0; j < 2u; ++j) {
                        if (j && candidates[j] == candidates[0]) continue;
                        addresses[points] = candidates[j]; sizes[points++] = region->sector_size;
                    }
                }
            }
        } else if (!s_gba_probe_88b0_window || s_cart_ctx.gba_cmdset != BURNER_NOR_CMDSET_INTEL ||
            s_cart_ctx.sector_size != GBA_SPOT_SECTOR_BYTES || s_cart_ctx.program_buffer_write_bytes != 1024u)
            err = ESP_ERR_NOT_SUPPORTED;
        if (gbabf && !word_recipe && !s_cart_ctx.d0d1_swapped) err = ESP_ERR_NOT_SUPPORTED;
    }
    report->points_total = points;
    report->recipe_id = native_buffer ? 4u : word_recipe ? 3u : gbabf ? 2u : 1u;
    if (native_buffer) {
        if (!word_recipe || s_cart_ctx.buffer_write_bytes != 64u) err = ESP_ERR_NOT_SUPPORTED;
        else if (err == ESP_OK) {
            /* Explicit lab qualification of the EXISTING native writer.
             * Keep the database flag until hardware validation passes. */
            s_cart_ctx.program_buffer_write_bytes = 64u;
            ESP_LOGI(BURNER_TAG, "Native M36 qualification: existing Intel 64B writer, detected source=%s",
                     burner_recipe_mode_to_str(job.recipe_mode));
        }
    }
    if (err == ESP_OK) {
        int fd = open(backup_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0) err = ESP_FAIL;
        else {
            file = fdopen(fd, "wb");
            if (!file) { close(fd); err = ESP_FAIL; }
        }
    }
    for (unsigned i = 0; err == ESP_OK && i < points; ++i) {
        uint8_t *saved = original + i * GBA_SPOT_SECTOR_BYTES;
        err = burner_bacon_gba_verify_read_block_hoststyle(saved, sizes[i], addresses[i], multi);
        if (err == ESP_OK && fwrite(saved, 1, sizes[i], file) != sizes[i])
            err = ESP_FAIL;
        ESP_LOGI(BURNER_TAG, "GBA spot backup %u/%u addr=0x%08" PRIX32 " size=%" PRIu32 " recipe=%u err=%s",
                 i + 1u, points, addresses[i], sizes[i], (unsigned)report->recipe_id, esp_err_to_name(err));
    }
    if (file) {
        if (fflush(file) != 0 && err == ESP_OK) err = ESP_FAIL;
        if (fsync(fileno(file)) != 0 && err == ESP_OK) err = ESP_FAIL;
        if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
        file = NULL;
    }
    /* Verify the persisted backup AND a second cartridge read before the
     * first erase. No reliance on first-word samples for preservation. */
    if (err == ESP_OK) { file = fopen(backup_path, "rb"); if (!file) err = ESP_FAIL; }
    for (unsigned i = 0; err == ESP_OK && i < points; ++i) {
        for (uint32_t off = 0; err == ESP_OK && off < sizes[i]; off += GBA_SPOT_READ_BYTES) {
            size_t count = sizes[i] - off;
            if (count > GBA_SPOT_READ_BYTES) count = GBA_SPOT_READ_BYTES;
            const uint8_t *saved = original + i * GBA_SPOT_SECTOR_BYTES + off;
            if (fread(actual, 1, count, file) != count || memcmp(actual, saved, count)) { err = ESP_FAIL; break; }
            err = burner_bacon_gba_verify_read_block_hoststyle(actual, count, addresses[i] + off, multi);
            if (err == ESP_OK && memcmp(actual, saved, count)) {
                unsigned different = 0;
                while (different < count && actual[different] == saved[different]) ++different;
                report->mismatch_address = addresses[i] + off + different;
                ESP_LOGE(BURNER_TAG, "GBA spot backup reread mismatch addr=0x%08" PRIX32 " saved=%02X read=%02X",
                         report->mismatch_address, saved[different], actual[different]);
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (file) { fclose(file); file = NULL; }
    report->backup_verified = err == ESP_OK;
    for (unsigned i = 0; err == ESP_OK && i < points; ++i) {
        report->destructive_started = true; // includes partially failed erase/program
        err = burner_bacon_gba_erase_sector(addresses[i], multi, 10000u);
        gba_spot_pattern(i, marker);
        if (err == ESP_OK) err = native_buffer ?
            burner_bacon_gba_program_block(marker, sizeof(marker), addresses[i], multi, false) :
            word_recipe ? gba_spot_gbabf_words(addresses[i], marker, sizeof(marker)) :
            gbabf ? gba_spot_gbabf_marker(addresses[i], marker) :
            burner_bacon_gba_program_block(marker, sizeof(marker), addresses[i], multi, false);
        /* Keep all earlier markers simultaneously and inspect untouched
         * targets too. Detect 32/64/128 MiB aliases as soon as they occur. */
        for (unsigned j = 0; err == ESP_OK && j < points; ++j) {
            const uint8_t *expected = original + j * GBA_SPOT_SECTOR_BYTES;
            if (j <= i) { gba_spot_pattern(j, marker); expected = marker; }
            err = burner_bacon_gba_verify_read_block_hoststyle(actual, GBA_SPOT_MARKER_BYTES, addresses[j], multi);
            if (err == ESP_OK && memcmp(actual, expected, GBA_SPOT_MARKER_BYTES)) {
                report->mismatch_address = addresses[j];
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        if (err == ESP_OK) ++report->markers_verified;
        ESP_LOGI(BURNER_TAG, "GBA spot marker %u/%u addr=0x%08" PRIX32 " err=%s",
                 i + 1u, points, addresses[i], esp_err_to_name(err));
    }
    report->test_error = err;
    /* Recovery uses saved originals even after a failed test. Restore all
     * targets because a broken address line may have modified an alias. */
    if (report->destructive_started) {
        for (unsigned i = 0; i < points; ++i) {
            esp_err_t restore = burner_bacon_gba_erase_sector(addresses[i], multi, 10000u);
            if (restore == ESP_OK) restore = word_recipe ?
                gba_spot_gbabf_words(addresses[i], original + i * GBA_SPOT_SECTOR_BYTES, sizes[i]) :
                burner_bacon_gba_program_block(original + i * GBA_SPOT_SECTOR_BYTES, sizes[i], addresses[i], multi, false);
            if (restore == ESP_OK) ++report->sectors_restored;
            else if (report->restore_error == ESP_OK) report->restore_error = restore;
            ESP_LOGI(BURNER_TAG, "GBA spot restore %u/%u addr=0x%08" PRIX32 " err=%s",
                     i + 1u, points, addresses[i], esp_err_to_name(restore));
        }
        /* Check every byte after ALL restores to catch cross-bank aliases. */
        for (unsigned i = 0; i < points; ++i) {
            esp_err_t verify = ESP_OK;
            for (uint32_t off = 0; verify == ESP_OK && off < sizes[i]; off += GBA_SPOT_READ_BYTES) {
                size_t count = sizes[i] - off;
                if (count > GBA_SPOT_READ_BYTES) count = GBA_SPOT_READ_BYTES;
                verify = burner_bacon_gba_verify_read_block_hoststyle(actual, count, addresses[i] + off, multi);
                if (verify == ESP_OK && memcmp(actual, original + i * GBA_SPOT_SECTOR_BYTES + off, count))
                    verify = ESP_ERR_INVALID_RESPONSE;
            }
            if (verify == ESP_OK) ++report->sectors_verified;
            else if (report->restore_error == ESP_OK) report->restore_error = verify;
        }
    }
    if (prepared) {
        s_cart_ctx.program_buffer_write_bytes = saved_program_buffer;
        esp_err_t finish = multi ? burner_gba_switch_bank_if_needed(0u) : burner_bacon_gba_intel_reset();
        if (finish == ESP_OK) finish = burner_bacon_finish_cart_access();
        if (err == ESP_OK) err = finish;
    }
    free(actual); free(original);
    if (report->restore_error != ESP_OK) return report->restore_error;
    return err;
}

esp_err_t burner_gba_88b0_spot_test_locked(const char *path, burner_gba_spot_report_t *report)
{
    return gba_spot_test_locked(path, report, false, false);
}

esp_err_t burner_gba_88b0_gbabf_test_locked(const char *path, burner_gba_spot_report_t *report)
{
    return gba_spot_test_locked(path, report, true, false);
}

esp_err_t burner_gba_m36_native_test_locked(const char *path, burner_gba_spot_report_t *report)
{
    return gba_spot_test_locked(path, report, true, true);
}

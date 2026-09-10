#include "../burner_sector_plan.h"

typedef struct {
    uint8_t *data;
    uint32_t address, size, first, bytes, file_offset;
} burner_write_slot_t;

static esp_err_t burner_write_slot_prepare(const burner_task_param_t *job,
    uint32_t offset, burner_write_slot_t *slot)
{
    burner_nor_region_cursor_t cursor = {0};
    uint32_t address = job->addr_begin + offset;
    esp_err_t err = burner_nor_geometry_region_cursor_begin(&s_cart_ctx.geometry, address, &cursor);
    if (err != ESP_OK) return err;
    err = burner_nor_geometry_sector_bounds_in_cursor(&cursor, address, &slot->address, NULL, &slot->size);
    if (err != ESP_OK) return err;
    slot->first = address - slot->address;
    slot->bytes = slot->size - slot->first;
    if (slot->bytes > job->total_bytes - offset) slot->bytes = job->total_bytes - offset;
    slot->file_offset = offset;
    return ESP_OK;
}

static esp_err_t burner_write_read_sector(void *context, uint8_t *data, size_t bytes, uint32_t address)
{
    const burner_task_param_t *job = context;
    burner_spi_lock_take();
    esp_err_t err = job->cart_mode == BURNER_CART_MODE_GBA ?
        burner_bacon_gba_verify_read_block_hoststyle(data, bytes, address, burner_is_gba_multi_card(job)) :
        (burner_gbc_gbx_is_active() ? burner_gbc_blank_read(data, bytes, address) :
                                    burner_bacon_mbc5_read_block_hoststyle(data, bytes, address));
    burner_spi_lock_give();
    return err;
}

static esp_err_t burner_write_erase_sector(const burner_task_param_t *job, const burner_write_slot_t *slot)
{
    burner_spi_lock_take();
    burner_status_mark_erase_begin();
    uint64_t started = esp_timer_get_time();
    uint32_t timeout = burner_erase_timeout_ms_for_bytes(slot->size);
    esp_err_t err = job->cart_mode == BURNER_CART_MODE_GBA ?
        burner_bacon_gba_erase_sector(slot->address, burner_is_gba_multi_card(job), timeout) :
        burner_bacon_mbc5_erase_sector(slot->address, timeout);
    burner_status_mark_erase_end();
    if (job->cart_mode == BURNER_CART_MODE_GBA) burner_gba_chis_diag_add_erase(esp_timer_get_time() - started);
    burner_spi_lock_give();
    if (err == ESP_OK) burner_status_record_erase_sectors(1, slot->size);
    return err;
}

static esp_err_t burner_write_program_sector(const burner_task_param_t *job, const burner_write_slot_t *slot)
{
    uint16_t page = job->cart_mode == BURNER_CART_MODE_GBA ?
        s_cart_ctx.program_buffer_write_bytes : s_cart_ctx.buffer_write_bytes;
    if (!page) page = job->cart_mode == BURNER_CART_MODE_GBA ? 2u : 1u;
    for (size_t done = 0; done < slot->size;) {
        if (burner_cancel_poll() != ESP_OK) return ESP_ERR_INVALID_STATE;
        size_t count = slot->size - done;
        size_t limit = job->cart_mode == BURNER_CART_MODE_GBA ? BURN_GBA_PROGRAM_CHUNK_BYTES : job->mbc5_program_chunk_bytes;
        if (!limit) return ESP_ERR_INVALID_ARG;
        if (count > limit) count = limit;
        burner_gbc_program_span_t span = burner_gbc_program_span(
            slot->data + done, count, slot->address + done, page);
        if (span.blank) {
            size_t lo = done > slot->first ? done : slot->first;
            size_t hi = done + span.bytes < slot->first + slot->bytes ? done + span.bytes : slot->first + slot->bytes;
            if (lo < hi) {
                burner_status_record_write_skipped(hi - lo);
                if (job->cart_mode == BURNER_CART_MODE_GBA) s_gba_chis_diag.skipped_ff_bytes += hi - lo;
            }
        } else {
            uint32_t written = span.bytes;
            burner_spi_lock_take();
            burner_status_mark_write_begin();
            esp_err_t err = job->cart_mode == BURNER_CART_MODE_GBA ?
                burner_bacon_gba_program_block(slot->data + done, span.bytes, slot->address + done,
                                              burner_is_gba_multi_card(job), false) :
                burner_bacon_mbc5_program_erased_block(slot->data + done, span.bytes, slot->address + done, &written);
            uint64_t elapsed = burner_status_mark_write_end();
            burner_spi_lock_give();
            if (err != ESP_OK) return err;
            burner_status_record_write_sample(written, elapsed);
        }
        done += span.bytes;
        burner_task_yield_if_due();
    }
    return ESP_OK;
}

/* PSRAM/pipeline use one persistent reader and two whole-sector slots.
 * Direct uses one slot and synchronous input. NOR operations stay ordered;
 * only the next TF read overlaps work on the current sector. */
static esp_err_t burner_write_sector_pipeline(const burner_task_param_t *job)
{
    burner_write_slot_t slots[2] = {0};
    burner_tf_reader_ctx_t reader = {0};
    uint8_t *old = NULL;
    FILE *fp = NULL;
    uint32_t processed = 0, largest = 0;
    unsigned current = 0;
    bool overlap = job->write_path != BURNER_WRITE_PATH_DIRECT;
    esp_err_t err = ESP_OK;
    const char *failure = "write preparation failed";
    if (!burner_nor_geometry_is_valid(&s_cart_ctx.geometry)) return ESP_ERR_INVALID_SIZE;
    err = burner_nor_geometry_largest_sector_size_in_range(&s_cart_ctx.geometry, job->addr_begin, job->total_bytes, &largest);
    if (err != ESP_OK || !largest) return ESP_ERR_INVALID_SIZE;
    slots[0].data = heap_caps_malloc(largest, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (overlap) slots[1].data = heap_caps_malloc(largest, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    old = heap_caps_malloc(largest, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!slots[0].data || (overlap && !slots[1].data) || !old) {
        err = ESP_ERR_NO_MEM; failure = "no memory for sector buffers"; goto done;
    }
    fp = burner_file_open_read(job->rom_path);
    if (!fp) { err = ESP_FAIL; failure = "open ROM failed"; goto done; }
    burner_tf_reader_set_source_size(job->cart_mode == BURNER_CART_MODE_GBA ? job->source_size : 0);
    burner_status_mark_write_manual_begin();
    burner_status_plan_erase_phase(
        burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, job->addr_begin, job->addr_begin + job->total_bytes - 1),
        burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, job->addr_begin, job->addr_begin + job->total_bytes - 1),
        burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry));
    if (overlap) {
        err = burner_tf_reader_start(&reader, fp);
        if (err != ESP_OK) { failure = "create TF reader failed"; goto done; }
    }
    err = burner_write_slot_prepare(job, 0, &slots[0]);
    if (err != ESP_OK) goto done;
    if (overlap) {
        err = burner_tf_reader_submit(&reader, slots[0].data + slots[0].first, slots[0].bytes);
        if (err != ESP_OK) goto done;
    }
    while (processed < job->total_bytes) {
        burner_write_slot_t *slot = &slots[current];
        uint64_t started = esp_timer_get_time();
        err = overlap ? burner_tf_reader_wait(&reader) :
            burner_tf_read_exact(fp, slot->data + slot->first, slot->bytes);
        uint64_t read_us = overlap ? reader.read_us : esp_timer_get_time() - started;
        if (err != ESP_OK) { failure = "read source ROM failed"; goto done; }
        if (job->cart_mode == BURNER_CART_MODE_GBA) {
            burner_gba_chis_diag_add_tf_read(read_us);
            if (overlap) burner_gba_chis_diag_add_prefetch_wait(esp_timer_get_time() - started);
        }
        burner_status_record_tf_to_psram_copy(slot->bytes, read_us);
        burner_apply_write_transform(job, slot->data + slot->first, slot->bytes, slot->file_offset);

        uint32_t next = processed + slot->bytes;
        if (overlap && next < job->total_bytes) {
            burner_write_slot_t *following = &slots[current ^ 1u];
            err = burner_write_slot_prepare(job, next, following);
            if (err == ESP_OK) err = burner_tf_reader_submit(&reader, following->data + following->first, following->bytes);
            if (err != ESP_OK) goto done;
        }
        if (burner_cancel_poll() != ESP_OK) { err = ESP_ERR_INVALID_STATE; goto done; }
        bool same = false, blank = false;
        bool partial = slot->first != 0 || slot->bytes != slot->size;
        if (!job->erase_always || partial) {
            burner_status_update(BURNER_STATE_BURNING, burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed, job->total_bytes, "comparing target sector", job->rom_name, job->rom_path);
            err = burner_sector_compare(slot->data, old, slot->address, slot->size,
                slot->first, slot->bytes, burner_write_read_sector, (void *)job, &same, &blank);
            if (err != ESP_OK) { failure = "read sector before erase failed"; goto done; }
        }
        if (same && !job->erase_always) {
            burner_status_record_write_matched(slot->bytes);
        } else {
            if (job->erase_always || !blank) {
                burner_status_update(BURNER_STATE_BURNING, burner_calc_progress_percent_u64(processed, job->total_bytes),
                    processed, job->total_bytes, "erasing changed sector", job->rom_name, job->rom_path);
                err = burner_write_erase_sector(job, slot);
                if (err != ESP_OK) { failure = "erase sector failed"; goto done; }
            }
            burner_status_update(BURNER_STATE_BURNING, burner_calc_progress_percent_u64(processed, job->total_bytes),
                processed, job->total_bytes, "programming sector", job->rom_name, job->rom_path);
            err = burner_write_program_sector(job, slot);
            if (err != ESP_OK) { failure = "program sector failed"; goto done; }
        }
        burner_status_advance_erase_phase(1, slot->size);
        processed = next;
        int progress = burner_calc_progress_percent_u64(processed, job->total_bytes);
        burner_status_update(BURNER_STATE_BURNING, progress, processed, job->total_bytes,
            "sector write complete", job->rom_name, job->rom_path);
        burner_emit_progress_cb(progress, processed);
        if (overlap) current ^= 1u;
        else if (processed < job->total_bytes) {
            err = burner_write_slot_prepare(job, processed, slot);
            if (err != ESP_OK) goto done;
        }
    }
    burner_spi_lock_take();
    if (job->cart_mode == BURNER_CART_MODE_GBA)
        err = burner_bacon_gba_finalize_write(burner_is_gba_multi_card(job));
    else if (burner_gbc_gbx_is_active())
        err = burner_gbc_gbx_reset_to_read_mode(false, s_cart_ctx.device_size);
    burner_spi_lock_give();
    failure = "final flash read mode failed";
done:
    burner_tf_reader_stop(&reader); // Join before releasing either slot or fp.
    burner_tf_reader_set_source_size(0);
    if (fp) fclose(fp);
    free(old); free(slots[0].data); free(slots[1].data);
    if (err != ESP_OK) burner_status_update(BURNER_STATE_ERROR,
        burner_calc_progress_percent_u64(processed, job->total_bytes), processed, job->total_bytes,
        failure, job->rom_name, job->rom_path);
    return err;
}

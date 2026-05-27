#include "ws_server_internal.h"

bool burner_status_tracks_speed(burner_state_t state)
{
    return (state == BURNER_STATE_RECEIVING || state == BURNER_STATE_BURNING);
}

bool burner_status_is_operation_active_state(burner_state_t state)
{
    return (state == BURNER_STATE_RECEIVING || state == BURNER_STATE_BURNING);
}

bool burner_status_is_operation_active_locked(void)
{
    return burner_status_is_operation_active_state(s_status.state);
}

void burner_cancel_reset_locked(void)
{
    s_status.cancel_requested = false;
}

void burner_cancel_reset(void)
{
    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        burner_cancel_reset_locked();
        xSemaphoreGive(s_status_lock);
    } else {
        s_status.cancel_requested = false;
    }
}

bool burner_cancel_is_requested(void)
{
    bool requested;

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        requested = s_status.cancel_requested;
        xSemaphoreGive(s_status_lock);
    } else {
        requested = s_status.cancel_requested;
    }

    return requested;
}

esp_err_t burner_cancel_poll(void)
{
    return burner_cancel_is_requested() ? ESP_ERR_INVALID_STATE : ESP_OK;
}

bool burner_cancel_request(void)
{
    burner_status_t snap = {0};
    bool active = false;

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        active = burner_status_is_operation_active_locked();
        if (active) {
            s_status.cancel_requested = true;
            snap = s_status;
        }
        xSemaphoreGive(s_status_lock);
    } else {
        active = burner_status_is_operation_active_state(s_status.state);
        if (active) {
            s_status.cancel_requested = true;
            snap = s_status;
        }
    }

    if (active) {
        burner_status_update(
            snap.state,
            snap.progress,
            snap.processed_bytes,
            snap.total_bytes,
            "cancel requested",
            snap.rom_name,
            snap.rom_path);
    }

    return active;
}

uint32_t burner_us_to_ms_clamped(uint64_t us)
{
    uint64_t ms = us / 1000ULL;
    if (ms > (uint64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)ms;
}

void burner_status_speed_reset_locked(void)
{
    s_status.speed_start_us = 0u;
    s_status.speed_warmup_until_us = 0u;
    s_status.speed_last_us = 0u;
    s_status.speed_start_bytes = 0u;
    s_status.speed_last_bytes = 0u;
    s_status.speed_current_bps = 0u;
    s_status.speed_avg_bps = 0u;
    s_status.speed_min_bps = 0u;
    s_status.speed_max_bps = 0u;
}

void burner_status_verify_sample_reset_locked(void)
{
    s_status.verify_sample_addr = 0u;
    s_status.verify_sample_file_byte = 0u;
    s_status.verify_sample_cart_byte = 0u;
    s_status.verify_sample_valid = false;
    s_status.verify_sample_equal = false;
}

void burner_status_phase_reset_locked(void)
{
    s_status.task_start_us = 0u;
    s_status.task_elapsed_us = 0u;
    s_status.erase_sector_count = 0u;
    s_status.erase_sector_size = 0u;
    s_status.erase_phase_total_sectors = 0u;
    s_status.erase_phase_done_sectors = 0u;
    s_status.erase_phase_total_bytes = 0u;
    s_status.erase_phase_done_bytes = 0u;
    s_status.erase_phase_planned = false;
    s_status.erase_phase_active = false;
    s_status.erase_start_us = 0u;
    s_status.erase_elapsed_us = 0u;
    s_status.write_start_us = 0u;
    s_status.write_elapsed_us = 0u;
    s_status.tf_to_psram_speed_current_bps = 0u;
    s_status.tf_to_psram_speed_avg_bps = 0u;
    s_status.tf_to_psram_speed_min_bps = 0u;
    s_status.tf_to_psram_speed_max_bps = 0u;
    s_status.dump_read_speed_current_bps = 0u;
    s_status.dump_read_speed_avg_bps = 0u;
    s_status.dump_read_speed_min_bps = 0u;
    s_status.dump_read_speed_max_bps = 0u;
    s_status.dump_write_speed_current_bps = 0u;
    s_status.dump_write_speed_avg_bps = 0u;
    s_status.dump_write_speed_min_bps = 0u;
    s_status.dump_write_speed_max_bps = 0u;
    s_status.mbc5_buffer_write_ok_count = 0u;
    s_status.mbc5_buffer_fallback_count = 0u;
    s_status.tf_to_psram_total_bytes = 0u;
    s_status.tf_to_psram_total_us = 0u;
    s_status.dump_read_total_bytes = 0u;
    s_status.dump_read_total_us = 0u;
    s_status.dump_write_total_bytes = 0u;
    s_status.dump_write_total_us = 0u;
    s_status.dump_wait_total_us = 0u;
    s_status.dump_finalize_total_us = 0u;
    burner_status_verify_sample_reset_locked();
}

void burner_status_probe_reset_locked(void)
{
    s_status.probe_cart_mode = BURNER_CART_MODE_MBC5;
    s_status.probe_valid = false;
    s_status.probe_cfi_ok = false;
    s_status.probe_gba_multi = false;
    s_status.probe_gba_force_multi = false;
    s_status.probe_gba_d0d1_known = false;
    s_status.probe_gba_d0d1_swapped = false;
    s_status.probe_gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
    s_status.probe_gba_save_size = 0u;
    s_status.probe_gba_save_detected = false;
    s_status.probe_gba_sram_patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
    s_status.probe_gba_sram_patch_scanned = false;
    s_status.probe_gba_sram_patch_detected = false;
    s_status.probe_device_size = 0u;
    s_status.probe_sector_size = 0u;
    s_status.probe_buffer_write_bytes = 0u;
    memset(s_status.probe_id, 0, sizeof(s_status.probe_id));
    s_status.probe_chip_name[0] = '\0';
}

void burner_status_set_probe_info(
    burner_cart_mode_t cart_mode,
    const uint8_t *id,
    size_t id_len,
    uint32_t device_size,
    uint32_t sector_size,
    uint16_t buffer_write_bytes,
    bool cfi_ok,
    bool gba_multi,
    bool gba_force_multi,
    bool gba_d0d1_known,
    bool gba_d0d1_swapped,
    const char *chip_name)
{
    size_t copy_len;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.probe_cart_mode = cart_mode;
    s_status.probe_valid = true;
    s_status.probe_cfi_ok = cfi_ok;
    s_status.probe_gba_multi = gba_multi;
    s_status.probe_gba_force_multi = gba_force_multi;
    s_status.probe_gba_d0d1_known = (cart_mode == BURNER_CART_MODE_GBA) ? gba_d0d1_known : false;
    s_status.probe_gba_d0d1_swapped = (cart_mode == BURNER_CART_MODE_GBA && gba_d0d1_known) ? gba_d0d1_swapped : false;
    if (cart_mode != BURNER_CART_MODE_GBA) {
        s_status.probe_gba_save_type = BURNER_GBA_SAVE_TYPE_SRAM;
        s_status.probe_gba_save_size = 0u;
        s_status.probe_gba_save_detected = false;
        s_status.probe_gba_sram_patch_kind = BURNER_GBA_SRAM_PATCH_NONE;
        s_status.probe_gba_sram_patch_scanned = false;
        s_status.probe_gba_sram_patch_detected = false;
    }
    s_status.probe_device_size = device_size;
    s_status.probe_sector_size = sector_size;
    s_status.probe_buffer_write_bytes = buffer_write_bytes;
    memset(s_status.probe_id, 0, sizeof(s_status.probe_id));
    if (id != NULL && id_len > 0u) {
        copy_len = (id_len < sizeof(s_status.probe_id)) ? id_len : sizeof(s_status.probe_id);
        memcpy(s_status.probe_id, id, copy_len);
    }
    snprintf(
        s_status.probe_chip_name,
        sizeof(s_status.probe_chip_name),
        "%s",
        (chip_name != NULL && chip_name[0] != '\0') ? chip_name : "unknown");
    xSemaphoreGive(s_status_lock);
}

void burner_status_set_gba_save_probe(
    burner_gba_save_type_t save_type,
    uint32_t save_size,
    bool detected)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.probe_gba_save_type = save_type;
    s_status.probe_gba_save_size = save_size;
    s_status.probe_gba_save_detected = detected;
    xSemaphoreGive(s_status_lock);
}

void burner_status_set_gba_sram_patch_probe(
    burner_gba_sram_patch_kind_t patch_kind,
    bool scanned,
    bool detected)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.probe_gba_sram_patch_scanned = scanned;
    s_status.probe_gba_sram_patch_kind = detected ? patch_kind : BURNER_GBA_SRAM_PATCH_NONE;
    s_status.probe_gba_sram_patch_detected = detected;
    xSemaphoreGive(s_status_lock);
}

void burner_status_set_verify_sample(uint32_t addr, uint8_t file_byte, uint8_t cart_byte, bool equal)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.verify_sample_addr = addr;
    s_status.verify_sample_file_byte = file_byte;
    s_status.verify_sample_cart_byte = cart_byte;
    s_status.verify_sample_valid = true;
    s_status.verify_sample_equal = equal;
    xSemaphoreGive(s_status_lock);
}

void burner_status_begin_erase_phase(uint32_t total_sectors, uint32_t total_bytes, uint32_t sector_size)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (!s_status.erase_phase_planned) {
        s_status.erase_phase_total_sectors = total_sectors;
        s_status.erase_phase_done_sectors = 0u;
        s_status.erase_phase_total_bytes = total_bytes;
        s_status.erase_phase_done_bytes = 0u;
    } else if (s_status.erase_phase_total_sectors == 0u) {
        s_status.erase_phase_total_sectors = total_sectors;
        s_status.erase_phase_total_bytes = total_bytes;
    } else if (s_status.erase_phase_total_bytes == 0u) {
        s_status.erase_phase_total_bytes = total_bytes;
    }
    s_status.erase_sector_size = sector_size;
    s_status.erase_phase_active = false;
    xSemaphoreGive(s_status_lock);
}

void burner_status_plan_erase_phase(uint32_t total_sectors, uint32_t total_bytes, uint32_t sector_size)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.erase_phase_total_sectors = total_sectors;
    s_status.erase_phase_done_sectors = 0u;
    s_status.erase_phase_total_bytes = total_bytes;
    s_status.erase_phase_done_bytes = 0u;
    s_status.erase_phase_planned = (total_sectors > 0u) || (total_bytes > 0u);
    s_status.erase_sector_size = sector_size;
    s_status.erase_phase_active = false;
    xSemaphoreGive(s_status_lock);
}

void burner_status_advance_erase_phase(uint32_t sectors_done, uint32_t bytes_done)
{
    uint64_t total_done;
    uint64_t total_done_bytes;

    if (s_status_lock == NULL || (sectors_done == 0u && bytes_done == 0u)) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    total_done = (uint64_t)s_status.erase_phase_done_sectors + (uint64_t)sectors_done;
    if (s_status.erase_phase_total_sectors > 0u && total_done > (uint64_t)s_status.erase_phase_total_sectors) {
        s_status.erase_phase_done_sectors = s_status.erase_phase_total_sectors;
    } else if (total_done > UINT32_MAX) {
        s_status.erase_phase_done_sectors = UINT32_MAX;
    } else {
        s_status.erase_phase_done_sectors = (uint32_t)total_done;
    }
    total_done_bytes = (uint64_t)s_status.erase_phase_done_bytes + (uint64_t)bytes_done;
    if (s_status.erase_phase_total_bytes > 0u &&
        total_done_bytes > (uint64_t)s_status.erase_phase_total_bytes) {
        s_status.erase_phase_done_bytes = s_status.erase_phase_total_bytes;
    } else if (total_done_bytes > UINT32_MAX) {
        s_status.erase_phase_done_bytes = UINT32_MAX;
    } else {
        s_status.erase_phase_done_bytes = (uint32_t)total_done_bytes;
    }
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_erase_begin(void)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.erase_start_us = (uint64_t)esp_timer_get_time();
    s_status.erase_phase_active = true;
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_erase_end(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (s_status.erase_start_us > 0u) {
        now_us = (uint64_t)esp_timer_get_time();
        if (now_us > s_status.erase_start_us) {
            s_status.erase_elapsed_us += now_us - s_status.erase_start_us;
        }
        s_status.erase_start_us = 0u;
    }
    if (!s_status.erase_phase_planned &&
        s_status.erase_phase_total_sectors > 0u &&
        s_status.erase_phase_done_sectors < s_status.erase_phase_total_sectors) {
        s_status.erase_phase_done_sectors = s_status.erase_phase_total_sectors;
    }
    if (!s_status.erase_phase_planned &&
        s_status.erase_phase_total_bytes > 0u &&
        s_status.erase_phase_done_bytes < s_status.erase_phase_total_bytes) {
        s_status.erase_phase_done_bytes = s_status.erase_phase_total_bytes;
    }
    s_status.erase_phase_active = false;
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_write_begin(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.write_start_us = now_us;
    burner_status_speed_reset_locked();
    s_status.speed_warmup_until_us = now_us + BURNER_SPEED_WARMUP_US;
    s_status.speed_last_bytes = s_status.processed_bytes;
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_write_end(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (s_status.write_start_us > 0u) {
        now_us = (uint64_t)esp_timer_get_time();
        if (now_us > s_status.write_start_us) {
            s_status.write_elapsed_us += now_us - s_status.write_start_us;
        }
        s_status.write_start_us = 0u;
    }
    xSemaphoreGive(s_status_lock);
}

void burner_status_mark_task_begin(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_phase_reset_locked();
    burner_status_speed_reset_locked();
    s_status.task_start_us = now_us;
    xSemaphoreGive(s_status_lock);
    s_burn_task_last_yield_us = now_us;
}

void burner_status_mark_task_end(void)
{
    uint64_t now_us;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (s_status.task_start_us > 0u) {
        now_us = (uint64_t)esp_timer_get_time();
        if (now_us > s_status.task_start_us) {
            s_status.task_elapsed_us += now_us - s_status.task_start_us;
        }
        s_status.task_start_us = 0u;
    }
    xSemaphoreGive(s_status_lock);
}

void burner_task_yield_if_due(void)
{
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    if (s_burn_task_last_yield_us == 0u) {
        s_burn_task_last_yield_us = now_us;
        return;
    }
    if (now_us - s_burn_task_last_yield_us < BURNER_CPU_YIELD_INTERVAL_US) {
        return;
    }

    s_burn_task_last_yield_us = now_us;
    vTaskDelay(1);
}

uint32_t burner_erase_sector_count_from_bytes(uint64_t bytes, uint32_t sector_size)
{
    uint64_t sectors;

    if (bytes == 0u || sector_size == 0u) {
        return 0u;
    }

    sectors = (bytes + (uint64_t)sector_size - 1u) / (uint64_t)sector_size;
    if (sectors > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)sectors;
}

uint32_t burner_erase_sector_count_from_range(uint32_t addr_begin, uint32_t addr_end, uint32_t sector_size)
{
    uint32_t sector_mask;
    uint32_t aligned_begin;
    uint32_t aligned_end;

    if (addr_end < addr_begin) {
        return 0u;
    }
    if (sector_size == 0u) {
        return 0u;
    }
    if ((sector_size & (sector_size - 1u)) != 0u) {
        return burner_erase_sector_count_from_bytes(
            (uint64_t)addr_end - (uint64_t)addr_begin + 1u,
            sector_size);
    }
    sector_mask = sector_size - 1u;
    aligned_begin = addr_begin & ~sector_mask;
    aligned_end = addr_end & ~sector_mask;
    if (aligned_end < aligned_begin) {
        return 0u;
    }
    return burner_erase_sector_count_from_bytes(
        (uint64_t)aligned_end - (uint64_t)aligned_begin + (uint64_t)sector_size,
        sector_size);
}

void burner_status_record_erase_sectors(uint32_t sector_count, uint32_t sector_size)
{
    uint64_t total;

    if (s_status_lock == NULL || sector_count == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    total = (uint64_t)s_status.erase_sector_count + (uint64_t)sector_count;
    if (total > UINT32_MAX) {
        s_status.erase_sector_count = UINT32_MAX;
    } else {
        s_status.erase_sector_count = (uint32_t)total;
    }
    s_status.erase_sector_size = sector_size;
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_speed_sample_locked(
    uint32_t bytes,
    uint64_t elapsed_us,
    uint32_t *current_bps,
    uint32_t *avg_bps,
    uint32_t *min_bps,
    uint32_t *max_bps,
    uint32_t *total_bytes,
    uint64_t *total_us)
{
    uint32_t instant_bps;
    uint64_t total_bytes64;
    uint64_t total_us64;

    if (bytes == 0u || elapsed_us == 0u || current_bps == NULL || avg_bps == NULL || min_bps == NULL ||
        max_bps == NULL || total_bytes == NULL || total_us == NULL) {
        return;
    }

    instant_bps = (uint32_t)(((uint64_t)bytes * 1000000ULL) / elapsed_us);

    *current_bps = instant_bps;
    if (*min_bps == 0u || instant_bps < *min_bps) {
        *min_bps = instant_bps;
    }
    if (instant_bps > *max_bps) {
        *max_bps = instant_bps;
    }

    total_bytes64 = (uint64_t)(*total_bytes) + (uint64_t)bytes;
    total_us64 = (*total_us) + elapsed_us;
    if (total_bytes64 > (uint64_t)UINT32_MAX) {
        *total_bytes = UINT32_MAX;
    } else {
        *total_bytes = (uint32_t)total_bytes64;
    }
    *total_us = total_us64;

    if (total_us64 > 0u) {
        uint64_t avg_bps64 = (total_bytes64 * 1000000ULL) / total_us64;
        if (avg_bps64 > (uint64_t)UINT32_MAX) {
            *avg_bps = UINT32_MAX;
        } else {
            *avg_bps = (uint32_t)avg_bps64;
        }
    }
}

void burner_status_record_tf_to_psram_copy(uint32_t bytes, uint64_t elapsed_us)
{
    if (s_status_lock == NULL || bytes == 0u || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_speed_sample_locked(
        bytes,
        elapsed_us,
        &s_status.tf_to_psram_speed_current_bps,
        &s_status.tf_to_psram_speed_avg_bps,
        &s_status.tf_to_psram_speed_min_bps,
        &s_status.tf_to_psram_speed_max_bps,
        &s_status.tf_to_psram_total_bytes,
        &s_status.tf_to_psram_total_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_dump_read(uint32_t bytes, uint64_t elapsed_us)
{
    if (s_status_lock == NULL || bytes == 0u || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_speed_sample_locked(
        bytes,
        elapsed_us,
        &s_status.dump_read_speed_current_bps,
        &s_status.dump_read_speed_avg_bps,
        &s_status.dump_read_speed_min_bps,
        &s_status.dump_read_speed_max_bps,
        &s_status.dump_read_total_bytes,
        &s_status.dump_read_total_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_dump_write(uint32_t bytes, uint64_t elapsed_us)
{
    if (s_status_lock == NULL || bytes == 0u || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_speed_sample_locked(
        bytes,
        elapsed_us,
        &s_status.dump_write_speed_current_bps,
        &s_status.dump_write_speed_avg_bps,
        &s_status.dump_write_speed_min_bps,
        &s_status.dump_write_speed_max_bps,
        &s_status.dump_write_total_bytes,
        &s_status.dump_write_total_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_elapsed_total(uint64_t *total_us, uint64_t elapsed_us)
{
    if (total_us == NULL || elapsed_us == 0u) {
        return;
    }

    if (*total_us > UINT64_MAX - elapsed_us) {
        *total_us = UINT64_MAX;
    } else {
        *total_us += elapsed_us;
    }
}

void burner_status_record_dump_wait(uint64_t elapsed_us)
{
    if (s_status_lock == NULL || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_elapsed_total(&s_status.dump_wait_total_us, elapsed_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_dump_finalize(uint64_t elapsed_us)
{
    if (s_status_lock == NULL || elapsed_us == 0u) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    burner_status_record_elapsed_total(&s_status.dump_finalize_total_us, elapsed_us);
    xSemaphoreGive(s_status_lock);
}

void burner_status_record_mbc5_buffer_write(bool fallback)
{
    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (fallback) {
        if (s_status.mbc5_buffer_fallback_count < UINT32_MAX) {
            s_status.mbc5_buffer_fallback_count++;
        }
    } else {
        if (s_status.mbc5_buffer_write_ok_count < UINT32_MAX) {
            s_status.mbc5_buffer_write_ok_count++;
        }
    }
    xSemaphoreGive(s_status_lock);
}

void burner_status_speed_update_locked(
    burner_state_t prev_state,
    uint32_t prev_total,
    uint32_t processed,
    uint32_t total)
{
    bool prev_tracks = burner_status_tracks_speed(prev_state);
    bool now_tracks = burner_status_tracks_speed(s_status.state);
    uint64_t now_us;

    if (!now_tracks) {
        if (s_status.state == BURNER_STATE_IDLE) {
            burner_status_speed_reset_locked();
        }
        return;
    }

    now_us = (uint64_t)esp_timer_get_time();
    if (!prev_tracks || processed < s_status.speed_last_bytes || (processed == 0u && total != prev_total)) {
        burner_status_speed_reset_locked();
        if (s_status.write_start_us > 0u) {
            s_status.speed_warmup_until_us = s_status.write_start_us + BURNER_SPEED_WARMUP_US;
        }
        if (s_status.speed_warmup_until_us == 0u || now_us >= s_status.speed_warmup_until_us) {
            s_status.speed_start_us = now_us;
            s_status.speed_start_bytes = processed;
            s_status.speed_last_us = now_us;
        }
        s_status.speed_last_bytes = processed;
        return;
    }

    if (s_status.speed_warmup_until_us > 0u && now_us < s_status.speed_warmup_until_us) {
        s_status.speed_current_bps = 0u;
        s_status.speed_last_bytes = processed;
        return;
    }
    if (s_status.speed_start_us == 0u || s_status.speed_last_us == 0u) {
        s_status.speed_start_us = now_us;
        s_status.speed_start_bytes = processed;
        s_status.speed_last_us = now_us;
        s_status.speed_last_bytes = processed;
        s_status.speed_current_bps = 0u;
        return;
    }

    if (processed >= s_status.speed_last_bytes && now_us > s_status.speed_last_us) {
        uint32_t delta_bytes = processed - s_status.speed_last_bytes;
        uint64_t delta_us = now_us - s_status.speed_last_us;

        if (delta_bytes > 0u && delta_us > 0u) {
            uint32_t instant_bps = (uint32_t)(((uint64_t)delta_bytes * 1000000ULL) / delta_us);
            s_status.speed_current_bps = instant_bps;
            if (s_status.speed_min_bps == 0u || instant_bps < s_status.speed_min_bps) {
                s_status.speed_min_bps = instant_bps;
            }
            if (instant_bps > s_status.speed_max_bps) {
                s_status.speed_max_bps = instant_bps;
            }
        }
        s_status.speed_last_us = now_us;
        s_status.speed_last_bytes = processed;
    }

    if (processed > s_status.speed_start_bytes && s_status.speed_start_us > 0u && now_us > s_status.speed_start_us) {
        uint32_t measured_bytes = processed - s_status.speed_start_bytes;
        uint64_t total_us = now_us - s_status.speed_start_us;
        s_status.speed_avg_bps = (uint32_t)(((uint64_t)measured_bytes * 1000000ULL) / total_us);
        if (s_status.speed_max_bps == 0u && s_status.speed_avg_bps > 0u) {
            s_status.speed_max_bps = s_status.speed_avg_bps;
        }
        if (s_status.speed_min_bps == 0u && s_status.speed_avg_bps > 0u) {
            s_status.speed_min_bps = s_status.speed_avg_bps;
        }
    }
}

void burner_status_reset(void)
{
    s_status.state = BURNER_STATE_IDLE;
    s_status.progress = 0;
    s_status.total_bytes = 0;
    s_status.processed_bytes = 0;
    burner_status_phase_reset_locked();
    burner_status_speed_reset_locked();
    burner_status_probe_reset_locked();
    burner_cancel_reset_locked();
    s_status.rom_name[0] = '\0';
    s_status.rom_path[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message), "%s", "idle");
}

int burner_calc_progress_percent_u64(uint64_t processed, uint64_t total)
{
    uint64_t progress = 0u;

    if (total == 0u) {
        return 0;
    }
    if (processed >= total) {
        return 100;
    }

    progress = (processed * 100ULL) / total;
    if (progress > 100ULL) {
        progress = 100ULL;
    }
    return (int)progress;
}

void burner_status_update(
    burner_state_t state,
    int progress,
    uint32_t processed,
    uint32_t total,
    const char *message,
    const char *rom_name,
    const char *rom_path)
{
    char ui_message[96];
    burner_state_t prev_state;
    uint32_t prev_total;

    if (s_status_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    prev_state = s_status.state;
    prev_total = s_status.total_bytes;
    s_status.state = state;
    s_status.progress = progress;
    s_status.processed_bytes = processed;
    s_status.total_bytes = total;
    burner_status_speed_update_locked(prev_state, prev_total, processed, total);

    if (message != NULL) {
        snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    }
    if (rom_name != NULL) {
        snprintf(s_status.rom_name, sizeof(s_status.rom_name), "%s", rom_name);
    }
    if (rom_path != NULL) {
        snprintf(s_status.rom_path, sizeof(s_status.rom_path), "%s", rom_path);
    }
    xSemaphoreGive(s_status_lock);

    ui_set_burn_progress(progress, processed, total);
    if (message != NULL && message[0] != '\0') {
        snprintf(ui_message, sizeof(ui_message), "burner %s: %s", burner_state_to_str(state), message);
    } else {
        snprintf(ui_message, sizeof(ui_message), "burner state: %s", burner_state_to_str(state));
    }
    ui_set_status_text(ui_message);
}

void burner_status_snapshot(burner_status_t *out)
{
    uint64_t now_us;

    if (out == NULL) {
        return;
    }

    if (s_status_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);

    now_us = (uint64_t)esp_timer_get_time();
    if (out->task_start_us > 0u && now_us > out->task_start_us) {
        out->task_elapsed_us += now_us - out->task_start_us;
    }
    if (out->erase_start_us > 0u && now_us > out->erase_start_us) {
        out->erase_elapsed_us += now_us - out->erase_start_us;
    }
    if (out->write_start_us > 0u && now_us > out->write_start_us) {
        out->write_elapsed_us += now_us - out->write_start_us;
    }
}

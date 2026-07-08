#include "ws_server_internal.h"
#include "lvgl_port.h"
#include "power_manager.h"

static void burner_task(void *param)
{
    burner_task_param_t *job = (burner_task_param_t *)param;
    esp_err_t err = ESP_OK;
    const char *done_msg = "task finished";
    const char *start_msg = "task started";
    bool restore_power = false;
    bool task_with_caps = false;
    bool idle_dim_suspended = false;
    bool perf_lock_acquired = false;

    if (job == NULL) {
        vTaskDelete(NULL);
        return;
    }
    task_with_caps = job->task_with_caps;
    if (power_manager_perf_lock_acquire("burn_task") == ESP_OK) {
        perf_lock_acquired = true;
    }
    lvgl_port_set_idle_dim_suspended(true);
    idle_dim_suspended = true;

    burner_cancel_reset();

    switch (job->mode) {
    case BURNER_JOB_WRITE_ROM:
        done_msg = "burn finished";
        if (job->write_path == BURNER_WRITE_PATH_PIPELINE) {
            start_msg = "burn task started (pipeline erase/write)";
        } else if (job->write_path == BURNER_WRITE_PATH_PSRAM) {
            start_msg = "burn task started (psram staging)";
        } else {
            start_msg = "burn task started";
        }
        break;
    case BURNER_JOB_READ_ROM:
        done_msg = "dump finished";
        start_msg = "dump task started (direct)";
        break;
    case BURNER_JOB_VERIFY_ROM:
        done_msg = "verify finished";
        start_msg = "verify task started";
        break;
    case BURNER_JOB_ERASE_ROM:
        done_msg = "chip erase finished";
        start_msg = "chip erase task started";
        break;
    case BURNER_JOB_WRITE_RAM:
        done_msg = "ram write finished";
        start_msg = "ram write task started";
        break;
    case BURNER_JOB_READ_RAM:
        done_msg = "ram dump finished";
        start_msg = "ram dump task started";
        break;
    case BURNER_JOB_VERIFY_RAM:
        done_msg = "ram verify finished";
        start_msg = "ram verify task started";
        break;
    case BURNER_JOB_WRITE_GBA_SAVE_NEW:
        done_msg = "gba save write finished";
        start_msg = "gba save write task started";
        break;
    case BURNER_JOB_READ_GBA_SAVE_NEW:
        done_msg = "gba save dump finished";
        start_msg = "gba save dump task started";
        break;
    case BURNER_JOB_VERIFY_GBA_SAVE_NEW:
        done_msg = "gba save verify finished";
        start_msg = "gba save verify task started";
        break;
    default:
        break;
    }

    burner_status_mark_task_begin();
    burner_status_update(
        BURNER_STATE_BURNING,
        0,
        0,
        job->total_bytes,
        start_msg,
        job->rom_name,
        job->rom_path);

    err = burner_spi_init();
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            job->total_bytes,
            "spi init failed",
            job->rom_name,
            job->rom_path);
        goto task_done;
    }
    restore_power = true;

    if (job->mode == BURNER_JOB_READ_ROM) {
        err = burner_run_read_job(job);
    } else if (job->mode == BURNER_JOB_WRITE_ROM) {
        err = burner_run_write_job(job);
    } else if (job->mode == BURNER_JOB_VERIFY_ROM) {
        err = burner_run_verify_rom_job(job);
    } else if (job->mode == BURNER_JOB_ERASE_ROM) {
        err = burner_run_erase_rom_job(job);
    } else if (job->mode == BURNER_JOB_WRITE_RAM) {
        err = burner_run_write_ram_job(job);
    } else if (job->mode == BURNER_JOB_READ_RAM) {
        err = burner_run_read_ram_job(job);
    } else if (job->mode == BURNER_JOB_VERIFY_RAM) {
        err = burner_run_verify_ram_job(job);
    } else if (job->mode == BURNER_JOB_WRITE_GBA_SAVE_NEW) {
        err = burner_run_write_gba_save_job_new(job);
    } else if (job->mode == BURNER_JOB_READ_GBA_SAVE_NEW) {
        err = burner_run_read_gba_save_job_new(job);
    } else if (job->mode == BURNER_JOB_VERIFY_GBA_SAVE_NEW) {
        err = burner_run_verify_gba_save_job_new(job);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }

    if (restore_power) {
        burner_spi_lock_take();
        burner_bacon_restore_3v3_power();
        burner_spi_lock_give();
    }

    burner_status_mark_task_end();

    if (err == ESP_OK) {
        burner_status_t snap;
        uint32_t done_total = job->total_bytes;
        uint32_t done_processed = job->total_bytes;

        burner_status_snapshot(&snap);
        if (snap.total_bytes != 0u) {
            done_total = snap.total_bytes;
            done_processed = snap.processed_bytes;
        }
        burner_status_update(
            BURNER_STATE_DONE,
            100,
            done_processed,
            done_total,
            done_msg,
            job->rom_name,
            job->rom_path);
    } else {
        burner_status_t snap;
        burner_status_snapshot(&snap);
        if (snap.state != BURNER_STATE_ERROR && snap.state != BURNER_STATE_CANCELLED) {
            if (burner_cancel_is_requested()) {
                burner_status_update(
                    BURNER_STATE_CANCELLED,
                    snap.progress,
                    snap.processed_bytes,
                    snap.total_bytes,
                    "task cancelled",
                    job->rom_name,
                    job->rom_path);
            } else {
                burner_status_update(
                    BURNER_STATE_ERROR,
                    snap.progress,
                    snap.processed_bytes,
                    snap.total_bytes,
                    "task failed",
                    job->rom_name,
                    job->rom_path);
            }
        }
    }

    {
        burner_status_t final_snap;

        burner_status_snapshot(&final_snap);
        if (err == ESP_OK) {
            ESP_LOGI(
                BURNER_TAG,
                "burn_task result: err=%s state=%s processed=%" PRIu32 "/%" PRIu32 " msg=%s",
                esp_err_to_name(err),
                burner_state_to_str(final_snap.state),
                final_snap.processed_bytes,
                final_snap.total_bytes,
                final_snap.message);
        } else {
            ESP_LOGW(
                BURNER_TAG,
                "burn_task result: err=%s state=%s processed=%" PRIu32 "/%" PRIu32 " msg=%s",
                esp_err_to_name(err),
                burner_state_to_str(final_snap.state),
                final_snap.processed_bytes,
                final_snap.total_bytes,
                final_snap.message);
        }
    }

task_done:
    burner_status_set_chip_erase_ui_active(false);
    burner_cancel_reset();
    burner_bacon_mark_activity_locked();
    if (idle_dim_suspended) {
        ui_mark_activity();
        lvgl_port_set_idle_dim_suspended(false);
        idle_dim_suspended = false;
    }
    if (perf_lock_acquired) {
        power_manager_perf_lock_release("burn_task");
        perf_lock_acquired = false;
    }
    ESP_LOGI(
        BURNER_TAG,
        "burn_task stack free min=%u bytes",
        (unsigned)uxTaskGetStackHighWaterMark2(NULL));
    free(job);
    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        s_burn_task = NULL;
        xSemaphoreGive(s_status_lock);
    } else {
        s_burn_task = NULL;
    }
    if (task_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

esp_err_t burner_start_task_ex(
    burner_job_mode_t mode,
    burner_cart_mode_t cart_mode,
    burner_write_path_t write_path,
    burner_recipe_mode_t recipe_mode,
    bool erase_always,
    bool gba_force_multi,
    bool gba_force_no_cfi,
    uint32_t mbc5_program_chunk_bytes,
    uint32_t read_chunk_bytes,
    uint32_t psram_window_bytes,
    const char *rom_name,
    const char *rom_path,
    uint32_t addr_begin,
    uint32_t total_bytes,
    burner_gba_save_type_t gba_save_type,
    const char *gbx_profile_file,
    bool ram_fram,
    uint8_t ram_latency)
{
    BaseType_t ret;
    burner_task_param_t *job = NULL;
    burner_core_affinity_t burn_task_affinity = BURNER_CORE_AFFINITY_AUTO;
    bool is_busy = false;

    if (rom_name == NULL || rom_path == NULL || total_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode != BURNER_JOB_ERASE_ROM && usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        is_busy = (s_burn_task != NULL);
        xSemaphoreGive(s_status_lock);
    } else {
        is_busy = (s_burn_task != NULL);
    }

    if (is_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    burner_cancel_reset();

    job = (burner_task_param_t *)calloc(1, sizeof(*job));
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }

    job->mode = mode;
    job->cart_mode = cart_mode;
    job->write_path = write_path;
    job->recipe_mode = recipe_mode;
    job->erase_always = erase_always;
    job->mbc5_program_chunk_bytes = burner_clamp_mbc5_program_chunk_bytes(mbc5_program_chunk_bytes);
    job->read_chunk_bytes = burner_dump_chunk_kb_to_bytes(
        burner_dump_chunk_bytes_to_kb(read_chunk_bytes));
    if (write_path == BURNER_WRITE_PATH_PIPELINE) {
        job->psram_window_bytes = 0u;
    } else {
        job->psram_window_bytes = burner_psram_window_mb_to_bytes(
            burner_psram_window_bytes_to_mb(psram_window_bytes));
    }
    snprintf(job->rom_name, sizeof(job->rom_name), "%s", rom_name);
    snprintf(job->rom_path, sizeof(job->rom_path), "%s", rom_path);
    job->addr_begin = addr_begin;
    job->total_bytes = total_bytes;
    job->gba_save_type = gba_save_type;
    snprintf(
        job->gbx_profile_file,
        sizeof(job->gbx_profile_file),
        "%s",
        (gbx_profile_file != NULL) ? gbx_profile_file : "");
    job->ram_fram = ram_fram;
    job->ram_latency = ram_latency;
    job->gba_force_multi = gba_force_multi;
    job->gba_force_no_cfi = gba_force_no_cfi;

    if (mode == BURNER_JOB_WRITE_ROM) {
        burn_task_affinity = s_burn_core_cfg.psram_core;
    } else if (mode == BURNER_JOB_ERASE_ROM) {
        burn_task_affinity = s_burn_core_cfg.erase_core;
    } else {
        burn_task_affinity = BURNER_CORE_AFFINITY_CPU1;
    }

    job->task_with_caps = true;
    ret = xTaskCreatePinnedToCoreWithCaps(
        burner_task,
        "burn_task",
        BURN_TASK_STACK_BYTES,
        job,
        5,
        &s_burn_task,
        burner_core_affinity_to_task_core_id(burn_task_affinity),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        job->task_with_caps = false;
        ret = burner_create_task_with_affinity(
            burner_task,
            "burn_task",
            BURN_TASK_STACK_BYTES,
            job,
            5,
            &s_burn_task,
            burn_task_affinity);
    }
    if (ret != pdPASS) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGW(
            BURNER_TAG,
            "burn_task create failed: ret=%d stack=%u affinity=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
            (int)ret,
            (unsigned)BURN_TASK_STACK_BYTES,
            burner_core_affinity_to_str(burn_task_affinity),
            (unsigned)internal_free,
            (unsigned)internal_largest,
            (unsigned)psram_free,
            (unsigned)psram_largest);
        free(job);
        s_burn_task = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t burner_start_task(
    burner_job_mode_t mode,
    const char *rom_name,
    const char *rom_path,
    uint32_t addr_begin,
    uint32_t total_bytes,
    burner_gba_save_type_t gba_save_type,
    bool ram_fram,
    uint8_t ram_latency)
{
    return burner_start_task_ex(
        mode,
        BURNER_CART_MODE_MBC5,
        BURNER_WRITE_PATH_PSRAM,
        s_burn_recipe_mode_default,
        false,
        false,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        rom_name,
        rom_path,
        addr_begin,
        total_bytes,
        gba_save_type,
        "",
        ram_fram,
        ram_latency);
}

esp_err_t burner_start_gba_save_write_from_tf_new(
    const char *raw_name,
    burner_gba_save_type_t save_type,
    uint32_t save_size,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    (void)raw_name;
    (void)save_type;
    (void)save_size;
    (void)result;
    if (error_msg != NULL && error_msg_len > 0u) {
        snprintf(error_msg, error_msg_len, "%s", "new GBA save write not implemented");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t burner_start_gba_save_verify_from_tf_new(
    const char *raw_name,
    burner_gba_save_type_t save_type,
    uint32_t save_size,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    (void)raw_name;
    (void)save_type;
    (void)save_size;
    (void)result;
    if (error_msg != NULL && error_msg_len > 0u) {
        snprintf(error_msg, error_msg_len, "%s", "new GBA save verify not implemented");
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t burner_start_gba_save_dump_new(
    const char *safe_name,
    const char *full_path,
    burner_gba_save_type_t save_type,
    uint32_t save_size)
{
    return burner_start_task_ex(
        BURNER_JOB_READ_GBA_SAVE_NEW,
        BURNER_CART_MODE_GBA,
        BURNER_WRITE_PATH_DIRECT,
        s_burn_recipe_mode_default,
        false,
        false,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        safe_name,
        full_path,
        0u,
        save_size,
        save_type,
        "",
        false,
        0u);
}

#include "ag32_batch_programmer.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag32_batch_format.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mcu_debug.h"
#include "usb_msc_tf.h"
#include "burner/core/ws_server_internal.h"

#define AG32_PROGRAM_TAG "ag32_program"
#define AG32_DEVICE_ID_ADDRESS 0x03000100u
#define AG32_FLASH_REG_BASE 0x40001000u
#define AG32_FLASH_KEYR (AG32_FLASH_REG_BASE + 0x04u)
#define AG32_FLASH_OPTKEYR (AG32_FLASH_REG_BASE + 0x08u)
#define AG32_FLASH_SR (AG32_FLASH_REG_BASE + 0x0cu)
#define AG32_FLASH_CR (AG32_FLASH_REG_BASE + 0x10u)
#define AG32_FLASH_AR (AG32_FLASH_REG_BASE + 0x14u)
#define AG32_FLASH_KEY1 0x45670123u
#define AG32_FLASH_KEY2 0xcdef89abu
#define AG32_FLASH_SR_BSY (1u << 0)
#define AG32_FLASH_SR_PGERR (1u << 2)
#define AG32_FLASH_SR_WRPRTERR (1u << 4)
#define AG32_FLASH_SR_EOP (1u << 5)
#define AG32_FLASH_CR_PG (1u << 0)
#define AG32_FLASH_CR_SER (1u << 1)
#define AG32_FLASH_CR_MER (1u << 2)
#define AG32_FLASH_CR_BER (1u << 3)
#define AG32_FLASH_CR_OPTPG (1u << 4)
#define AG32_FLASH_CR_OPTER (1u << 5)
#define AG32_FLASH_CR_STRT (1u << 6)
#define AG32_FLASH_CR_LOCK (1u << 7)
#define AG32_FLASH_CR_OPTWRE (1u << 9)
#define AG32_FLASH_CR_ERASE_MASK \
    (AG32_FLASH_CR_SER | AG32_FLASH_CR_MER | AG32_FLASH_CR_BER | AG32_FLASH_CR_OPTER)
#define AG32_FLASH_ERROR_MASK (AG32_FLASH_SR_PGERR | AG32_FLASH_SR_WRPRTERR)
#define AG32_FLASH_SECTOR_SIZE 0x1000u
#define AG32_IO_CHUNK 1024u
#define AG32_FLASH_TIMEOUT_US (60LL * 1000LL * 1000LL)

typedef struct {
    char path[304];
} ag32_batch_job_t;

static SemaphoreHandle_t s_job_lock;
static TaskHandle_t s_job_task;
static bool s_job_running;
static ag32_batch_job_status_t s_job_status;

static esp_err_t set_error(
    esp_err_t err,
    char *error,
    size_t error_size,
    const char *format,
    ...)
{
    if (error != NULL && error_size > 0u) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return err;
}

static void report_progress(
    ag32_batch_progress_cb_t callback,
    void *context,
    const char *phase,
    uint32_t processed,
    uint32_t total)
{
    if (callback != NULL) {
        callback(phase, processed, total, context);
    }
}

static esp_err_t flash_read(uint32_t address, uint32_t *value)
{
    return mcu_debug_read_memory32(address, value);
}

static esp_err_t flash_write(uint32_t address, uint32_t value)
{
    return mcu_debug_write_memory32(address, value);
}

static esp_err_t flash_wait_ready(char *error, size_t error_size)
{
    int64_t deadline = esp_timer_get_time() + AG32_FLASH_TIMEOUT_US;
    while (esp_timer_get_time() < deadline) {
        uint32_t status;
        esp_err_t err = flash_read(AG32_FLASH_SR, &status);
        if (err != ESP_OK) {
            return set_error(err, error, error_size, "cannot read AG32 flash status");
        }
        if ((status & AG32_FLASH_SR_BSY) == 0u) {
            if ((status & AG32_FLASH_ERROR_MASK) != 0u) {
                return set_error(ESP_ERR_INVALID_RESPONSE, error, error_size,
                    "AG32 flash status error 0x%08" PRIx32, status);
            }
            return ESP_OK;
        }
        vTaskDelay(1);
    }
    return set_error(ESP_ERR_TIMEOUT, error, error_size, "AG32 flash operation timed out");
}

static esp_err_t flash_clear_status(char *error, size_t error_size)
{
    esp_err_t err = flash_write(
        AG32_FLASH_SR,
        AG32_FLASH_ERROR_MASK | AG32_FLASH_SR_EOP);
    return err == ESP_OK ? ESP_OK
        : set_error(err, error, error_size, "cannot clear AG32 flash status");
}

static esp_err_t flash_unlock(bool option, char *error, size_t error_size)
{
    uint32_t control;
    esp_err_t err = flash_read(AG32_FLASH_CR, &control);
    if (err != ESP_OK) {
        return set_error(err, error, error_size, "cannot read AG32 flash control");
    }
    if ((control & AG32_FLASH_CR_LOCK) != 0u) {
        if (flash_write(AG32_FLASH_KEYR, AG32_FLASH_KEY1) != ESP_OK
            || flash_write(AG32_FLASH_KEYR, AG32_FLASH_KEY2) != ESP_OK) {
            return set_error(ESP_FAIL, error, error_size, "cannot unlock AG32 flash");
        }
        if (flash_read(AG32_FLASH_CR, &control) != ESP_OK
            || (control & AG32_FLASH_CR_LOCK) != 0u) {
            return set_error(ESP_ERR_INVALID_STATE, error, error_size,
                "AG32 flash remains locked");
        }
    }
    if (option) {
        if (flash_write(AG32_FLASH_OPTKEYR, AG32_FLASH_KEY1) != ESP_OK
            || flash_write(AG32_FLASH_OPTKEYR, AG32_FLASH_KEY2) != ESP_OK) {
            return set_error(ESP_FAIL, error, error_size, "cannot unlock AG32 option bytes");
        }
        if (flash_read(AG32_FLASH_CR, &control) != ESP_OK
            || (control & AG32_FLASH_CR_OPTWRE) == 0u) {
            return set_error(ESP_ERR_INVALID_STATE, error, error_size,
                "AG32 option bytes remain locked");
        }
    }
    return ESP_OK;
}

static esp_err_t flash_set_control_bits(
    uint32_t clear_mask,
    uint32_t set_mask,
    char *error,
    size_t error_size)
{
    uint32_t control;
    esp_err_t err = flash_read(AG32_FLASH_CR, &control);
    if (err == ESP_OK) {
        control = (control & ~clear_mask) | set_mask;
        err = flash_write(AG32_FLASH_CR, control);
    }
    return err == ESP_OK ? ESP_OK
        : set_error(err, error, error_size, "cannot update AG32 flash control");
}

static esp_err_t flash_erase_operation(
    uint32_t mode,
    uint32_t address,
    bool *destructive_started,
    char *error,
    size_t error_size)
{
    esp_err_t err = flash_clear_status(error, error_size);
    if (err == ESP_OK && mode != AG32_FLASH_CR_MER && mode != AG32_FLASH_CR_OPTER) {
        err = flash_write(AG32_FLASH_AR, address);
    }
    if (err == ESP_OK) {
        err = flash_set_control_bits(
            AG32_FLASH_CR_ERASE_MASK | AG32_FLASH_CR_PG | AG32_FLASH_CR_OPTPG,
            mode,
            error,
            error_size);
    }
    if (err == ESP_OK) {
        burner_spi_block_swd_restore();
        if (destructive_started != NULL) *destructive_started = true;
        esp_rom_delay_us(10u);
        err = flash_set_control_bits(0u, AG32_FLASH_CR_STRT, error, error_size);
    }
    if (err == ESP_OK) {
        err = flash_wait_ready(error, error_size);
    }
    esp_err_t clear_err = flash_set_control_bits(
        AG32_FLASH_CR_ERASE_MASK | AG32_FLASH_CR_STRT,
        0u,
        error,
        error_size);
    return err != ESP_OK ? err : clear_err;
}

static esp_err_t flash_erase_range(
    uint32_t address,
    uint32_t size,
    bool *destructive_started,
    ag32_batch_progress_cb_t progress,
    void *context,
    char *error,
    size_t error_size)
{
    uint32_t first = address & ~(AG32_FLASH_SECTOR_SIZE - 1u);
    uint32_t end = (address + size + AG32_FLASH_SECTOR_SIZE - 1u)
        & ~(AG32_FLASH_SECTOR_SIZE - 1u);
    for (uint32_t current = first; current < end; current += AG32_FLASH_SECTOR_SIZE) {
        esp_err_t err = flash_erase_operation(
            AG32_FLASH_CR_SER, current, destructive_started, error, error_size);
        if (err != ESP_OK) {
            return err;
        }
        report_progress(progress, context, "erase", current - first + AG32_FLASH_SECTOR_SIZE, end - first);
    }
    return ESP_OK;
}

static esp_err_t read_payload(FILE *file, uint32_t offset, void *data, size_t size)
{
    return fseek(file, (long)offset, SEEK_SET) == 0
        && fread(data, 1u, size, file) == size ? ESP_OK : ESP_FAIL;
}

static esp_err_t validate_payload_reads(
    FILE *file,
    const ag32_batch_manifest_t *manifest,
    uint8_t *buffer,
    char *error,
    size_t error_size)
{
    for (uint32_t index = 0u; index < manifest->record_count; ++index) {
        const ag32_batch_record_t *record = &manifest->records[index];
        uint32_t processed = 0u;
        while (processed < record->payload_size) {
            size_t chunk = record->payload_size - processed;
            if (chunk > AG32_IO_CHUNK) chunk = AG32_IO_CHUNK;
            if (read_payload(file, record->payload_offset + processed, buffer, chunk) != ESP_OK) {
                return set_error(ESP_FAIL, error, error_size,
                    "cannot pre-read record %" PRIu32 " payload", index);
            }
            processed += (uint32_t)chunk;
        }
    }
    return ESP_OK;
}

static esp_err_t program_option_record(
    FILE *file,
    const ag32_batch_record_t *record,
    char *error,
    size_t error_size)
{
    uint8_t payload[AG32_OPTION_SIZE];
    esp_err_t err = read_payload(file, record->payload_offset, payload, sizeof(payload));
    if (err != ESP_OK) {
        return set_error(err, error, error_size, "cannot read option-byte payload");
    }
    err = flash_set_control_bits(
        AG32_FLASH_CR_ERASE_MASK | AG32_FLASH_CR_PG,
        AG32_FLASH_CR_PG | AG32_FLASH_CR_OPTPG,
        error,
        error_size);
    for (uint32_t offset = 0u; err == ESP_OK && offset < sizeof(payload); offset += 2u) {
        uint16_t value = (uint16_t)payload[offset] | ((uint16_t)payload[offset + 1u] << 8);
        if (value != UINT16_MAX) {
            err = mcu_debug_write_memory16(record->address + offset, value);
            if (err == ESP_OK) err = flash_wait_ready(error, error_size);
        }
    }
    esp_err_t clear_err = flash_set_control_bits(
        AG32_FLASH_CR_PG | AG32_FLASH_CR_OPTPG,
        0u,
        error,
        error_size);
    return err != ESP_OK ? err : clear_err;
}

static esp_err_t program_flash_record(
    FILE *file,
    const ag32_batch_record_t *record,
    uint8_t *buffer,
    ag32_batch_progress_cb_t progress,
    void *context,
    ag32_batch_program_report_t *report,
    uint32_t total_bytes,
    char *error,
    size_t error_size)
{
    uint32_t processed = 0u;
    esp_err_t err = flash_set_control_bits(
        AG32_FLASH_CR_ERASE_MASK | AG32_FLASH_CR_OPTPG,
        AG32_FLASH_CR_PG,
        error,
        error_size);
    while (err == ESP_OK && processed < record->payload_size) {
        size_t chunk = record->payload_size - processed;
        if (chunk > AG32_IO_CHUNK) chunk = AG32_IO_CHUNK;
        err = read_payload(file, record->payload_offset + processed, buffer, chunk);
        if (err != ESP_OK) {
            err = set_error(err, error, error_size, "cannot read batch payload");
            break;
        }
        for (size_t offset = 0u; err == ESP_OK && offset < chunk; offset += 4u) {
            uint32_t value = UINT32_MAX;
            size_t remain = chunk - offset;
            if (remain > 4u) remain = 4u;
            memcpy(&value, buffer + offset, remain);
            if (value != UINT32_MAX) {
                err = mcu_debug_write_memory32(record->address + processed + (uint32_t)offset, value);
                if (err == ESP_OK) err = flash_wait_ready(error, error_size);
            }
        }
        if (err == ESP_OK) {
            processed += (uint32_t)chunk;
            report->programmed_bytes += (uint32_t)chunk;
            report_progress(progress, context, "program", report->programmed_bytes, total_bytes);
            vTaskDelay(1);
        }
    }
    esp_err_t clear_err = flash_set_control_bits(AG32_FLASH_CR_PG, 0u, error, error_size);
    return err != ESP_OK ? err : clear_err;
}

static esp_err_t verify_record(
    FILE *file,
    const ag32_batch_record_t *record,
    uint8_t *buffer,
    ag32_batch_progress_cb_t progress,
    void *context,
    ag32_batch_program_report_t *report,
    uint32_t total_bytes,
    char *error,
    size_t error_size)
{
    uint32_t processed = 0u;
    while (processed < record->payload_size) {
        size_t chunk = record->payload_size - processed;
        if (chunk > AG32_IO_CHUNK) chunk = AG32_IO_CHUNK;
        esp_err_t err = read_payload(file, record->payload_offset + processed, buffer, chunk);
        if (err != ESP_OK) {
            return set_error(err, error, error_size, "cannot reread batch payload");
        }
        for (size_t offset = 0u; offset < chunk; offset += 4u) {
            uint32_t actual;
            size_t remain = chunk - offset;
            if (remain > 4u) remain = 4u;
            err = mcu_debug_read_memory32(
                (record->address + processed + (uint32_t)offset) & ~3u,
                &actual);
            if (err != ESP_OK || memcmp(&actual, buffer + offset, remain) != 0) {
                return set_error(ESP_ERR_INVALID_CRC, error, error_size,
                    "verify failed at 0x%08" PRIx32,
                    record->address + processed + (uint32_t)offset);
            }
        }
        processed += (uint32_t)chunk;
        report->verified_bytes += (uint32_t)chunk;
        report_progress(progress, context, "verify", report->verified_bytes, total_bytes);
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t ag32_batch_program_file(
    const char *path,
    ag32_batch_progress_cb_t progress,
    void *progress_context,
    ag32_batch_program_report_t *report,
    char *error,
    size_t error_size)
{
    ag32_batch_manifest_t manifest;
    ag32_batch_program_report_t local_report = {0};
    uint8_t *buffer = NULL;
    FILE *file = NULL;
    bool spi_mode_entered = false;
    bool swd_session_open = false;
    bool restore_spi = false;
    esp_err_t err = ESP_OK;

    if (path == NULL || report == NULL) {
        return set_error(ESP_ERR_INVALID_ARG, error, error_size, "invalid batch programming arguments");
    }
    *report = local_report;
    file = fopen(path, "rb");
    if (file == NULL) {
        return set_error(ESP_ERR_NOT_FOUND, error, error_size, "cannot open batch file");
    }
    report_progress(progress, progress_context, "validate", 0u, 0u);
    if (ag32_batch_parse(file, &manifest, error, error_size) != 0) {
        err = ESP_ERR_INVALID_ARG;
        goto out;
    }
    local_report.record_count = manifest.record_count;
    buffer = malloc(AG32_IO_CHUNK);
    if (buffer == NULL) {
        err = set_error(ESP_ERR_NO_MEM, error, error_size, "cannot allocate batch I/O buffer");
        goto out;
    }
    err = validate_payload_reads(file, &manifest, buffer, error, error_size);
    if (err != ESP_OK) {
        goto out;
    }

    err = burner_spi_enter_swd_mode();
    if (err != ESP_OK) {
        err = set_error(err, error, error_size, "cannot suspend cartridge SPI");
        goto out;
    }
    spi_mode_entered = true;
    report_progress(progress, progress_context, "connect", 0u, manifest.payload_bytes);
    err = mcu_debug_session_begin(&local_report.dp_idcode);
    if (err != ESP_OK) {
        err = set_error(err, error, error_size, "AG32 SWD connection failed");
        goto out;
    }
    swd_session_open = true;
    err = mcu_debug_halt();
    if (err != ESP_OK) {
        err = set_error(err, error, error_size, "cannot halt AG32 MCU");
        goto out;
    }
    err = mcu_debug_read_memory32(AG32_DEVICE_ID_ADDRESS, &local_report.device_id);
    if (err != ESP_OK || local_report.device_id != AG32_DEVICE_ID) {
        err = set_error(ESP_ERR_INVALID_RESPONSE, error, error_size,
            "unexpected AG32 device ID 0x%08" PRIx32, local_report.device_id);
        goto out;
    }
    err = flash_unlock(true, error, error_size);
    if (err != ESP_OK) goto out;

    for (uint32_t index = 0u; index < manifest.record_count; ++index) {
        const ag32_batch_record_t *record = &manifest.records[index];
        if (record->kind == AG32_BATCH_RECORD_OPTION) {
            if (record->erase_options != 0u) {
                report_progress(progress, progress_context, "option_erase", 0u, manifest.payload_bytes);
                err = flash_erase_operation(
                    AG32_FLASH_CR_OPTER, 0u, &local_report.destructive_started,
                    error, error_size);
            }
            if (err == ESP_OK) {
                err = program_option_record(file, record, error, error_size);
                if (err == ESP_OK) {
                    local_report.programmed_bytes += record->payload_size;
                }
            }
        } else {
            if (record->erase_options != 0u) {
                err = flash_erase_range(record->address, record->payload_size,
                    &local_report.destructive_started,
                    progress, progress_context, error, error_size);
            }
            if (err == ESP_OK) {
                err = program_flash_record(file, record, buffer,
                    progress, progress_context, &local_report, manifest.payload_bytes,
                    error, error_size);
            }
        }
        if (err != ESP_OK) goto out;
        err = verify_record(file, record, buffer,
            progress, progress_context, &local_report, manifest.payload_bytes,
            error, error_size);
        if (err != ESP_OK) goto out;
    }

    report_progress(progress, progress_context, "reset", local_report.verified_bytes, local_report.verified_bytes);
    err = mcu_debug_system_reset();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        burner_spi_allow_swd_restore();
    }
    restore_spi = err == ESP_OK;

out:
    if (swd_session_open) {
        mcu_debug_session_end();
    }
    if (spi_mode_entered) {
        esp_err_t restore_err = burner_spi_leave_swd_mode(
            restore_spi || !local_report.destructive_started);
        if (err == ESP_OK && restore_err != ESP_OK) {
            err = set_error(restore_err, error, error_size, "AG32 updated but cartridge SPI restore failed");
        }
    }
    free(buffer);
    if (file != NULL) fclose(file);
    *report = local_report;
    ESP_LOGI(AG32_PROGRAM_TAG,
        "batch result=%s records=%" PRIu32 " programmed=%" PRIu32 " verified=%" PRIu32,
        esp_err_to_name(err), local_report.record_count,
        local_report.programmed_bytes, local_report.verified_bytes);
    return err;
}

static void job_progress(
    const char *phase,
    uint32_t processed,
    uint32_t total,
    void *context)
{
    (void)context;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    snprintf(s_job_status.phase, sizeof(s_job_status.phase), "%s", phase);
    s_job_status.processed = processed;
    s_job_status.total = total;
    xSemaphoreGive(s_job_lock);
}

static void ag32_batch_job_task(void *arg)
{
    ag32_batch_job_t *job = arg;
    ag32_batch_program_report_t report;
    char error[sizeof(s_job_status.message)] = {0};
    esp_err_t result = ag32_batch_program_file(
        job->path, job_progress, NULL, &report, error, sizeof(error));

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_job_status.report = report;
    s_job_status.state = result == ESP_OK ? AG32_BATCH_JOB_SUCCESS : AG32_BATCH_JOB_FAILED;
    snprintf(s_job_status.phase, sizeof(s_job_status.phase), "%s",
        result == ESP_OK ? "complete" : "failed");
    snprintf(s_job_status.message, sizeof(s_job_status.message), "%s",
        result == ESP_OK ? "AG32 batch programmed and verified" : error);
    s_job_running = false;
    s_job_task = NULL;
    xSemaphoreGive(s_job_lock);

    free(job);
    vTaskDelete(NULL);
}

esp_err_t ag32_batch_program_start(const char *path)
{
    ag32_batch_job_t *job;
    if (path == NULL || strncmp(path, "/sdcard/", 8u) != 0 || strstr(path, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t init_err = burner_backend_init();
    if (init_err != ESP_OK) return init_err;
    if (s_job_lock == NULL) {
        s_job_lock = xSemaphoreCreateMutex();
        if (s_job_lock == NULL) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    if (s_job_running) {
        xSemaphoreGive(s_job_lock);
        xSemaphoreGive(s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_job_running = true;
    if (usb_msc_tf_in_use_by_host() || s_burn_starting || s_burn_task != NULL) {
        s_job_running = false;
        xSemaphoreGive(s_job_lock);
        xSemaphoreGive(s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    job = calloc(1u, sizeof(*job));
    if (job == NULL) {
        s_job_running = false;
        xSemaphoreGive(s_job_lock);
        xSemaphoreGive(s_status_lock);
        return ESP_ERR_NO_MEM;
    }
    snprintf(job->path, sizeof(job->path), "%s", path);
    memset(&s_job_status, 0, sizeof(s_job_status));
    s_job_status.state = AG32_BATCH_JOB_RUNNING;
    snprintf(s_job_status.path, sizeof(s_job_status.path), "%s", path);
    snprintf(s_job_status.phase, sizeof(s_job_status.phase), "queued");
    snprintf(s_job_status.message, sizeof(s_job_status.message), "AG32 batch update queued");
    if (xTaskCreatePinnedToCore(
            ag32_batch_job_task,
            "ag32_batch",
            8192,
            job,
            4,
            &s_job_task,
            1) != pdPASS) {
        free(job);
        s_job_running = false;
        s_job_status.state = AG32_BATCH_JOB_FAILED;
        snprintf(s_job_status.message, sizeof(s_job_status.message), "cannot create AG32 update task");
        xSemaphoreGive(s_job_lock);
        xSemaphoreGive(s_status_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_job_lock);
    xSemaphoreGive(s_status_lock);
    return ESP_OK;
}

bool ag32_batch_program_is_running(void)
{
    bool running;
    if (s_job_lock == NULL) return false;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    running = s_job_running;
    xSemaphoreGive(s_job_lock);
    return running;
}

void ag32_batch_program_status(ag32_batch_job_status_t *status)
{
    if (status == NULL) return;
    if (s_job_lock == NULL) {
        memset(status, 0, sizeof(*status));
        return;
    }
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    *status = s_job_status;
    xSemaphoreGive(s_job_lock);
}

const char *ag32_batch_job_state_name(ag32_batch_job_state_t state)
{
    static const char *const names[] = {"idle", "running", "success", "failed"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

esp_err_t ag32_batch_validate_path(
    const char *path,
    uint32_t *record_count,
    uint32_t *payload_bytes,
    char *error,
    size_t error_size)
{
    ag32_batch_manifest_t manifest;
    if (path == NULL || strncmp(path, "/sdcard/", 8u) != 0 || strstr(path, "..") != NULL) {
        return set_error(ESP_ERR_INVALID_ARG, error, error_size, "invalid batch path");
    }
    if (ag32_batch_program_is_running() || usb_msc_tf_in_use_by_host()) {
        return set_error(ESP_ERR_INVALID_STATE, error, error_size, "TF storage is busy");
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return set_error(ESP_ERR_NOT_FOUND, error, error_size, "cannot open batch file");
    }
    int parsed = ag32_batch_parse(file, &manifest, error, error_size);
    fclose(file);
    if (parsed != 0) return ESP_ERR_INVALID_ARG;
    if (record_count != NULL) *record_count = manifest.record_count;
    if (payload_bytes != NULL) *payload_bytes = manifest.payload_bytes;
    return ESP_OK;
}

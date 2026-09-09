#ifndef AG32_BATCH_PROGRAMMER_H
#define AG32_BATCH_PROGRAMMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*ag32_batch_progress_cb_t)(
    const char *phase,
    uint32_t processed,
    uint32_t total,
    void *context);

typedef struct {
    uint32_t dp_idcode;
    uint32_t device_id;
    uint32_t record_count;
    uint32_t programmed_bytes;
    uint32_t verified_bytes;
    bool destructive_started;
} ag32_batch_program_report_t;

typedef enum {
    AG32_BATCH_JOB_IDLE = 0,
    AG32_BATCH_JOB_RUNNING,
    AG32_BATCH_JOB_SUCCESS,
    AG32_BATCH_JOB_FAILED,
} ag32_batch_job_state_t;

typedef struct {
    ag32_batch_job_state_t state;
    char path[304];
    char phase[24];
    char message[160];
    uint32_t processed;
    uint32_t total;
    ag32_batch_program_report_t report;
} ag32_batch_job_status_t;

esp_err_t ag32_batch_program_file(
    const char *path,
    ag32_batch_progress_cb_t progress,
    void *progress_context,
    ag32_batch_program_report_t *report,
    char *error,
    size_t error_size);

esp_err_t ag32_batch_program_start(const char *path);
bool ag32_batch_program_is_running(void);
esp_err_t ag32_batch_validate_path(
    const char *path,
    uint32_t *record_count,
    uint32_t *payload_bytes,
    char *error,
    size_t error_size);
void ag32_batch_program_status(ag32_batch_job_status_t *status);
const char *ag32_batch_job_state_name(ag32_batch_job_state_t state);

#endif

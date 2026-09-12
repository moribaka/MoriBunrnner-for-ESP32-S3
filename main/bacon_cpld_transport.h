#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "bacon_cpld_protocol.h"

void bacon_cpld_link_invalidate(void);
typedef struct {
    uint32_t first_ready_us, elapsed_us, packets, status_polls;
} bacon_cpld_stats_t;
void bacon_cpld_get_stats(bacon_cpld_stats_t *out);
// Diagnostic-only GBA reader. Caller owns the burner SPI lock; never used by jobs.
typedef struct {
    uint32_t elapsed_us, ready_us, start_us, finish_us, crc_us, copy_us;
    uint32_t packets, status_polls;
} bacon_cpld_read_profile_t;
esp_err_t bacon_cpld_read_experiment_locked(uint32_t address, void *data,
    size_t size, bool pipeline, bacon_cpld_read_profile_t *profile);
// RAM-only opt-in: 0=off, 1=BSC1 measurement, 2=BSC2 write experiment.
esp_err_t bacon_cpld_write_experiment_configure_locked(unsigned mode);
typedef struct {
    unsigned mode;
    uint64_t bytes, streams, packets, status_polls, credits;
    uint64_t elapsed_us, ready_us, prepare_us, tx_us, done_us;
} bacon_cpld_write_profile_t;
void bacon_cpld_write_experiment_profile_locked(bacon_cpld_write_profile_t *out);
esp_err_t bacon_cpld_probe_locked(void);
esp_err_t bacon_cpld_try_transfer_locked(uint8_t operation, uint32_t byte_address,
    void *data, size_t size, uint16_t page_bytes, uint32_t timeout_ms, bool *used);

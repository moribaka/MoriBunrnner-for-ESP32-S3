#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

static inline bool burner_gbc_bytes_are_ff(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) if (data[i] != 0xffu) return false;
    return true;
}

typedef struct { size_t bytes; bool blank; } burner_gbc_program_span_t;

/* Group adjacent pages of the same kind within the caller's MBC window.
 * Blank spans may be omitted only after erase or a complete blank check. */
static inline burner_gbc_program_span_t burner_gbc_program_span(
    const uint8_t *data, size_t size, uint32_t address, uint16_t page_bytes)
{
    burner_gbc_program_span_t span = {0};
    if (page_bytes == 0u) page_bytes = 1u; // byte-programming devices
    while (span.bytes < size) {
        size_t count = page_bytes - ((address + span.bytes) % page_bytes);
        if (count > size - span.bytes) count = size - span.bytes;
        bool blank = burner_gbc_bytes_are_ff(data + span.bytes, count);
        if (span.bytes == 0) span.blank = blank;
        else if (span.blank != blank) break;
        span.bytes += count;
    }
    return span;
}

typedef esp_err_t (*burner_gbc_blank_read_t)(uint8_t *, size_t, uint32_t);

static inline esp_err_t burner_gbc_verify_blank_range(uint32_t address, uint32_t size,
    uint8_t *buffer, size_t capacity, burner_gbc_blank_read_t read, bool *blank)
{
    if (!size || !capacity || !buffer || !read || !blank) return ESP_ERR_INVALID_ARG;
    *blank = false;
    for (uint32_t done = 0; done < size;) {
        size_t count = size - done;
        if (count > capacity) count = capacity;
        esp_err_t err = read(buffer, count, address + done);
        if (err != ESP_OK) return err;
        if (!burner_gbc_bytes_are_ff(buffer, count)) return ESP_OK;
        done += (uint32_t)count;
    }
    *blank = true;
    return ESP_OK;
}

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#include "burner_gbc_program_spans.h"

typedef esp_err_t (*burner_sector_read_t)(void *, uint8_t *, size_t, uint32_t);

/* Full equality and full blankness require every byte. A dirty mismatch can
 * stop early only for a whole-sector replacement. Partial sectors preserve
 * everything outside the requested range before an erase is allowed. */
static inline esp_err_t burner_sector_compare(
    uint8_t *expected, uint8_t *old, uint32_t address, size_t size,
    size_t first, size_t bytes, burner_sector_read_t read, void *context,
    bool *same, bool *blank)
{
    if (!expected || !old || !read || !same || !blank || !bytes || first > size || bytes > size-first)
        return ESP_ERR_INVALID_ARG;
    *same = true; *blank = true;
    bool partial = first != 0 || bytes != size;
    for (size_t offset = 0; offset < size;) {
        size_t count = size - offset;
        if (count > 16384u) count = 16384u;
        esp_err_t err = read(context, old + offset, count, address + offset);
        if (err != ESP_OK) { *same = *blank = false; return err; }
        if (!burner_gbc_bytes_are_ff(old + offset, count)) *blank = false;
        size_t lo = offset > first ? offset : first;
        size_t hi = offset + count < first + bytes ? offset + count : first + bytes;
        if (lo < hi && memcmp(expected + lo, old + lo, hi - lo) != 0) *same = false;
        if (!partial && !*same && !*blank) return ESP_OK;
        offset += count;
    }
    if (partial) {
        memcpy(expected, old, first);
        memcpy(expected + first + bytes, old + first + bytes, size - first - bytes);
    }
    return ESP_OK;
}

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"

/* source_size==0 is strict reading. Otherwise FF padding is permitted only
 * after the declared source EOF; truncation inside the source is an error. */
static inline esp_err_t burner_source_read_exact(FILE *fp, uint8_t *dst, size_t bytes, uint32_t source_size)
{
    if (fp == NULL || dst == NULL || bytes == 0) return ESP_ERR_INVALID_ARG;
    size_t from_file = bytes;
    if (source_size != 0) {
        long position = ftell(fp);
        if (position < 0) return ESP_FAIL;
        if ((uint64_t)position >= source_size) {
            // A prior seek may land past EOF; verify that the actual file has
            // not shrunk before treating the request as expansion padding.
            if (fseek(fp, 0, SEEK_END) != 0) return ESP_FAIL;
            long actual_size = ftell(fp);
            if (actual_size < 0 || (uint64_t)actual_size < source_size) return ESP_FAIL;
            if (fseek(fp, position, SEEK_SET) != 0) return ESP_FAIL;
            from_file = 0;
        } else if (from_file > source_size - (uint32_t)position) {
            from_file = source_size - (uint32_t)position;
        }
    }
    if (from_file && fread(dst, 1, from_file, fp) != from_file) return ESP_FAIL;
    if (ferror(fp)) return ESP_FAIL;
    if (from_file < bytes) memset(dst + from_file, 0xFF, bytes - from_file);
    return ESP_OK;
}

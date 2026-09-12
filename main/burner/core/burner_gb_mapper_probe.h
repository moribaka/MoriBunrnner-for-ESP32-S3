#pragma once
#include <string.h>
#include "esp_err.h"

/* Only the low register is touched: 0x3000 is not a high-bank register on
 * MBC3. Compare complete banks; equal/blank contents alone prove no mapper. */
static inline esp_err_t burner_gb_probe_mapper(
    uint8_t *fixed, uint8_t *window, size_t bytes,
    esp_err_t (*select_low)(uint8_t),
    esp_err_t (*read_bus)(uint16_t, uint8_t *, size_t),
    burner_gb_mapper_t *detected)
{
    *detected = BURNER_GB_MAPPER_UNKNOWN;
    esp_err_t err = select_low(0);
    if (err == ESP_OK) err = read_bus(0, fixed, bytes);
    if (err == ESP_OK) err = read_bus(0x4000, window, bytes);
    if (err != ESP_OK) return err;
    if (memcmp(fixed, window, bytes) != 0) {
        *detected = BURNER_GB_MAPPER_MBC3;
        return ESP_OK;
    }
    err = select_low(1);
    if (err == ESP_OK) err = read_bus(0x4000, window, bytes);
    if (err == ESP_OK && memcmp(fixed, window, bytes) != 0)
        *detected = BURNER_GB_MAPPER_MBC5;
    return err;
}

static inline esp_err_t burner_gb_resolve_mapper(
    burner_gb_mapper_t requested, burner_gb_mapper_t detected,
    burner_gb_mapper_t *selected)
{
    *selected = BURNER_GB_MAPPER_UNKNOWN;
    if (requested != BURNER_GB_MAPPER_UNKNOWN &&
        detected != BURNER_GB_MAPPER_UNKNOWN && requested != detected)
        return ESP_ERR_INVALID_STATE;
    *selected = requested != BURNER_GB_MAPPER_UNKNOWN ? requested : detected;
    return *selected == BURNER_GB_MAPPER_UNKNOWN ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
typedef enum { BURNER_GB_MAPPER_UNKNOWN, BURNER_GB_MAPPER_MBC3,
               BURNER_GB_MAPPER_MBC5 } burner_gb_mapper_t;
#include "../../main/burner/core/burner_gb_mapper_probe.h"
static uint8_t rom[2][16384], fixed[16384], window[16384], bank;
static burner_gb_mapper_t hardware;
static int fail_read;
static esp_err_t select_low(uint8_t value) {
    bank = (hardware == BURNER_GB_MAPPER_MBC3 && !value) ? 1 : value;
    return ESP_OK;
}
static esp_err_t read_bus(uint16_t address, uint8_t *out, size_t size) {
    if (fail_read) return ESP_FAIL;
    assert(size == 16384 && (address == 0 || address == 0x4000));
    memcpy(out, rom[address ? bank : 0], size);
    return ESP_OK;
}
int main(void) {
    burner_gb_mapper_t detected, selected;
    /* Identical first 64 bytes hid bank differences in the old probe. */
    memset(rom, 0xff, sizeof(rom)); rom[1][4096] = 0x37;
    for (hardware = BURNER_GB_MAPPER_MBC3; hardware <= BURNER_GB_MAPPER_MBC5; ++hardware) {
        assert(burner_gb_probe_mapper(fixed, window, sizeof(fixed), select_low, read_bus, &detected) == ESP_OK);
        assert(detected == hardware);
        assert(burner_gb_resolve_mapper(BURNER_GB_MAPPER_UNKNOWN, detected, &selected) == ESP_OK && selected == hardware);
        assert(burner_gb_resolve_mapper(hardware, detected, &selected) == ESP_OK);
        burner_gb_mapper_t wrong = hardware == BURNER_GB_MAPPER_MBC3 ? BURNER_GB_MAPPER_MBC5 : BURNER_GB_MAPPER_MBC3;
        assert(burner_gb_resolve_mapper(wrong, detected, &selected) == ESP_ERR_INVALID_STATE);
        assert(selected == BURNER_GB_MAPPER_UNKNOWN);
    }
    memset(rom, 0xff, sizeof(rom));
    assert(burner_gb_probe_mapper(fixed, window, sizeof(fixed), select_low, read_bus, &detected) == ESP_OK);
    assert(detected == BURNER_GB_MAPPER_UNKNOWN);
    assert(burner_gb_resolve_mapper(BURNER_GB_MAPPER_UNKNOWN, detected, &selected) == ESP_ERR_NOT_SUPPORTED);
    assert(burner_gb_resolve_mapper(BURNER_GB_MAPPER_MBC3, detected, &selected) == ESP_OK);
    fail_read = 1;
    assert(burner_gb_probe_mapper(fixed, window, sizeof(fixed), select_low, read_bus, &detected) == ESP_FAIL);
    puts("Mapper probe: MBC3/MBC5, equal prefixes, blank ambiguity, conflicting selection and read failures passed");
}

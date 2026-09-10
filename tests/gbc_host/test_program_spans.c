#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/burner/core/burner_gbc_program_spans.h"

static uint8_t original[32768], restored[32768];
static uint32_t read_end;
static bool fail_read;
static esp_err_t read_region(uint8_t *out, size_t count, uint32_t address)
{
    assert(address == read_end && address + count <= sizeof(original));
    read_end += (uint32_t)count;
    if (fail_read) return ESP_FAIL;
    memcpy(out, original + address, count);
    return ESP_OK;
}

static void roundtrip(size_t offset, size_t length, uint16_t page)
{
    size_t done = 0, skipped = 0;
    memset(restored, 0xff, sizeof(restored));
    while (done < length) {
        size_t window = 16384 - ((offset + done) % 16384);
        if (window > length - done) window = length - done;
        burner_gbc_program_span_t span = burner_gbc_program_span(
            original + offset + done, window, offset + done, page);
        assert(span.bytes > 0 && span.bytes <= window);
        if (span.blank) {
            assert(burner_gbc_bytes_are_ff(original + offset + done, span.bytes));
            skipped += span.bytes;
        } else {
            memcpy(restored + offset + done, original + offset + done, span.bytes);
        }
        done += span.bytes;
    }
    assert(done == length && skipped <= length);
    assert(memcmp(original + offset, restored + offset, length) == 0);
}

int main(void)
{
    memset(original, 0xff, sizeof(original));
    const uint16_t pages[] = {0, 2, 32, 64, 256, 512};
    for (size_t p = 0; p < sizeof(pages)/sizeof(pages[0]); ++p) roundtrip(0, sizeof(original), pages[p]);
    // Dirty bytes in partial pages and immediately across the 16 KiB MBC boundary.
    const size_t dirty[] = {1, 63, 64, 130, 16383, 16384, 16385, 32766};
    for (size_t i = 0; i < sizeof(dirty)/sizeof(dirty[0]); ++i) original[dirty[i]] = (uint8_t)i;
    for (size_t p = 0; p < sizeof(pages)/sizeof(pages[0]); ++p) {
        roundtrip(0, sizeof(original), pages[p]);
        roundtrip(61, 20000, pages[p]);
        roundtrip(16380, 11, pages[p]);
    }
    uint32_t random = 0x318ed4u;
    for (unsigned trial = 0; trial < 100; ++trial) {
        for (size_t i = 0; i < sizeof(original); ++i) {
            random = random * 1664525u + 1013904223u;
            original[i] = random & 0x80 ? 0xff : (uint8_t)random;
        }
        roundtrip(trial, sizeof(original) - trial, pages[trial % 6]);
    }
    uint8_t buffer[4096]; bool blank;
    memset(original, 0xff, sizeof(original));
    original[20003] = 0; // Not one of the old 4x2-byte sample positions.
    read_end = 0;
    assert(burner_gbc_verify_blank_range(0, sizeof(original), buffer, sizeof(buffer), read_region, &blank) == ESP_OK);
    assert(!blank && read_end >= 20004);
    original[20003] = 0xff; read_end = 0;
    assert(burner_gbc_verify_blank_range(0, sizeof(original), buffer, sizeof(buffer), read_region, &blank) == ESP_OK);
    assert(blank && read_end == sizeof(original));
    fail_read = true; read_end = 0;
    assert(burner_gbc_verify_blank_range(0, sizeof(original), buffer, sizeof(buffer), read_region, &blank) == ESP_FAIL && !blank);
    puts("GBC spans: dense/sparse data, partial pages, MBC boundaries and complete blank validation passed");
}

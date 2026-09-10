#include <assert.h>
#include <stdio.h>
#include "../../main/burner/core/burner_sector_plan.h"
static uint8_t flash[32768], expected[32768], old[32768];
static uint32_t read_bytes;
static bool fail;
static esp_err_t read_flash(void *context, uint8_t *dst, size_t bytes, uint32_t address) {
    (void)context;
    if (fail && address >= 16384) return ESP_FAIL;
    memcpy(dst, flash + address, bytes); read_bytes += bytes; return ESP_OK;
}
static void compare(size_t first, size_t bytes, bool want_same, bool want_blank) {
    bool same, blank; read_bytes = 0;
    assert(burner_sector_compare(expected, old, 0, sizeof(flash), first, bytes, read_flash, NULL, &same, &blank) == ESP_OK);
    assert(same == want_same && blank == want_blank);
}
int main(void) {
    memset(flash, 0xff, sizeof(flash)); memcpy(expected, flash, sizeof(flash));
    compare(0, sizeof(flash), true, true); assert(read_bytes == sizeof(flash));
    flash[20003] = 0; // Old 4x2-byte sampling misses this byte.
    compare(0, sizeof(flash), false, false);
    memset(flash, 0xff, sizeof(flash)); expected[20003] = 0;
    compare(0, sizeof(flash), false, true); assert(read_bytes == sizeof(flash));
    flash[0] = 0; expected[0] = 1;
    compare(0, sizeof(flash), false, false); assert(read_bytes == 16384);
    for (size_t i=0; i<sizeof(flash); ++i) flash[i] = (i * 137) ^ (i >> 8);
    memset(expected, 0xcc, sizeof(expected));
    memcpy(expected + 100, flash + 100, 100);
    compare(100, 100, true, false); assert(memcmp(expected, flash, sizeof(flash)) == 0);
    expected[145] ^= 1;
    compare(100, 100, false, false);
    assert(memcmp(expected, flash, 100) == 0 && memcmp(expected+200, flash+200, sizeof(flash)-200) == 0);
    memcpy(expected, flash, sizeof(flash)); fail = true;
    bool same = true, blank = true;
    assert(burner_sector_compare(expected, old, 0, sizeof(flash), 0, sizeof(flash), read_flash, NULL, &same, &blank) == ESP_FAIL);
    assert(!same && !blank);
    puts("Sector decisions: complete equality/blank checks, dirty mismatch, partial preservation and read failure passed");
}

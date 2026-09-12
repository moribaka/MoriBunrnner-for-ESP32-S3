// Host failure injection against the actual diagnostic reader, not a rewrite.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bacon_cpld_transport.h"

#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define portMAX_DELAY -1
#define BURNER_SPI_CS_MODE_0 0
#define BURNER_SPI_CS_MODE_1 1
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
static void *s_mcu_spi = (void *)1;
static uint8_t s_read_clocks[BACON_CPLD_BLOCK_BYTES + 8];
static struct { uint32_t status_polls; } s_stats;
static bool acquired, inflight, selected;
static unsigned starts, ends, exits, frees, cancels, fail_start, fail_crc;
static bool fail_alloc, fail_enter, fail_ready, fail_exit, fail_cancel;
static size_t remaining, position;
static spi_transaction_t *active;
static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return ++clock_us; }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return fail_alloc ? NULL : malloc(n); }
static void checked_free(void *p) { assert(!inflight && !acquired && !selected); ++frees; free(p); }
static esp_err_t burner_spi_init(void) { return ESP_OK; }
static esp_err_t spi_device_acquire_bus(void *d, int t) { (void)d; (void)t; assert(!acquired); acquired = true; return ESP_OK; }
static void spi_device_release_bus(void *d) { (void)d; assert(acquired && !inflight && !selected); acquired = false; }
static esp_err_t enter(void) { assert(acquired); return fail_enter ? ESP_FAIL : ESP_OK; }
static esp_err_t mode_key(const char *key) { assert(!inflight && !selected); assert(!strcmp(key, BACON_CPLD_EXIT_MAGIC)); ++exits; return fail_exit ? ESP_FAIL : ESP_OK; }
static esp_err_t burner_spi_transfer_cs(int mode, const uint8_t *tx, uint8_t *rx, size_t n)
{
    assert(mode == 0 && !rx && n == 20 && !inflight);
    assert(tx[4] == BACON_CPLD_GBA_READ);
    position = ag32_mcu_read_le32(tx + 8); remaining = ag32_mcu_read_le32(tx + 12);
    return ESP_OK;
}
static esp_err_t wait_flag(uint8_t flag, uint32_t total, int64_t deadline)
{
    (void)total; (void)deadline; assert(!inflight); ++s_stats.status_polls;
    if (flag == BACON_CPLD_DONE) assert(remaining == 0);
    return flag == BACON_CPLD_TX_READY && fail_ready ? ESP_ERR_TIMEOUT : ESP_OK;
}
static esp_err_t burner_spi_begin_cs(int mode) { assert(mode == 1 && !selected); selected = true; return ESP_OK; }
static void burner_spi_end_cs(int mode) { assert(mode == 1 && selected && !inflight); selected = false; }
static uint8_t sample(size_t offset) { return (uint8_t)((offset * 37u) ^ (offset >> 8)); }
static uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *data, size_t n) { assert(seed == 0); return ag32_mcu_crc32(data, n); }
static esp_err_t spi_device_polling_start(void *d, spi_transaction_t *t, int timeout)
{
    (void)d; (void)timeout; assert(selected && acquired && !inflight);
    assert(t->tx_buffer != t->rx_buffer && (((uintptr_t)t->rx_buffer | (t->length / 8)) & 3u) == 0);
    ++starts;
    if (starts == fail_start) return ESP_FAIL;
    active = t; inflight = true; return ESP_OK;
}
static esp_err_t spi_device_polling_end(void *d, int timeout)
{
    (void)d; (void)timeout; assert(inflight && selected); ++ends;
    size_t count = remaining > 1020 ? 1020 : remaining;
    size_t padded = (count + 3) & ~(size_t)3;
    assert(active->length == (padded + 8) * 8);
    uint8_t *rx = active->rx_buffer;
    memset(rx, 0xa5, padded + 8);
    for (size_t i = 0; i < count; ++i) rx[i + 4] = sample(position + i);
    ag32_mcu_write_le32(rx + 4 + padded, ag32_mcu_crc32(rx + 4, count) ^ (ends == fail_crc));
    position += count; remaining -= count; inflight = false; return ESP_OK;
}
static esp_err_t burner_cancel_poll(void) { assert(!inflight); ++cancels; return fail_cancel ? ESP_FAIL : ESP_OK; }
static void burner_task_yield_if_due(void) { assert(!inflight); }
#define free checked_free
#include "../../main/bacon_cpld_read_experiment.inc"
#undef free

static void reset(void)
{
    assert(!acquired && !inflight && !selected);
    starts = ends = exits = frees = cancels = fail_start = fail_crc = 0;
    fail_alloc = fail_enter = fail_ready = fail_exit = fail_cancel = false;
}
int main(void)
{
    static uint8_t out[65540];
    const size_t sizes[] = {2, 4, 6, 1018, 1020, 1022, 2040, 2042, 65536};
    bacon_cpld_read_profile_t profile;
    for (unsigned pipeline = 0; pipeline < 2; ++pipeline) {
        for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
            reset(); memset(out, 0xcc, sizeof(out));
            assert(bacon_cpld_read_experiment_locked(18, out + 1, sizes[i], pipeline, &profile) == ESP_OK);
            assert(out[0] == 0xcc && out[sizes[i] + 1] == 0xcc);
            for (size_t j = 0; j < sizes[i]; ++j) assert(out[j + 1] == sample(18 + j));
            assert(starts == ends && ends == (sizes[i] + 1019)/1020 && exits == 1 && frees == 1);
        }
        // First and final packet CRC, pending-DMA cleanup, failed start, READY,
        // mode entry/exit, cancellation and allocation. All must release ownership.
        for (unsigned fault = 0; fault < 9; ++fault) {
            reset(); memset(out, 0xcc, sizeof(out));
            if (fault == 0) fail_crc = 1;
            if (fault == 1) fail_crc = 3;
            if (fault == 2) fail_start = 2;
            if (fault == 3) fail_ready = true;
            if (fault == 4) fail_enter = true;
            if (fault == 5) fail_exit = true;
            if (fault == 6) fail_cancel = true;
            if (fault == 7) fail_alloc = true;
            if (fault == 8) { fail_crc = 1; fail_exit = true; }
            esp_err_t err = bacon_cpld_read_experiment_locked(0, out, 2042, pipeline, &profile);
            assert(err != ESP_OK && !acquired && !inflight && !selected);
            assert(starts == ends + (fault == 2));
            if (fault == 0 || fault == 8) { assert(err == ESP_ERR_INVALID_CRC); assert(out[0] == 0xcc); }
            if (fault == 1) assert(out[2040] == 0xcc);
            assert(frees == (fault != 7));
        }
    }
    reset();
    assert(bacon_cpld_read_experiment_locked(1, out, 2, true, &profile) == ESP_ERR_INVALID_ARG);
    assert(bacon_cpld_read_experiment_locked(33554430, out, 4, true, &profile) == ESP_ERR_INVALID_ARG);
    assert(!starts && !frees);
    puts("PASS: CPLD diagnostic read boundaries, data, CRC and DMA/CS/bus failure cleanup");
    return 0;
}

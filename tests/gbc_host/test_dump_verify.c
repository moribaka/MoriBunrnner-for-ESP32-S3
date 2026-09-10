/* Execute the production dump loop with a delayed writer: submit retains
 * its input until wait, so premature slot reuse is detected byte-for-byte. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_err.h"
#define TF_PATH_LEN_MAX 240
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define BURNER_STATE_BURNING 1
#define BURNER_STATE_ERROR 2
#define BURNER_CART_MODE_MBC5 1
#define ESP_LOGI(...) ((void)0)
typedef struct { uint32_t addr_begin, total_bytes; const char *rom_name, *rom_path; int cart_mode; uint32_t source_size; } burner_task_param_t;
static void burner_apply_write_transform(const burner_task_param_t *job, uint8_t *data, size_t bytes, uint32_t offset) {
    (void)job; (void)data; (void)bytes; (void)offset;
}
static uint32_t verified_count;
static void burner_status_record_verified(uint32_t bytes) { verified_count = bytes; }
static FILE *burner_open_mbc5_verify_log(const burner_task_param_t *job, char *path, size_t size) {
    (void)job; (void)path; (void)size; return NULL;
}
typedef struct { int fd; const uint8_t *src; size_t bytes; bool inflight, running; } burner_tf_writer_ctx_t;
typedef esp_err_t (*burner_dump_read_block_fn_t)(uint8_t *, size_t, uint32_t, const burner_task_param_t *);
static uint32_t recorded, mismatch_addr;
static bool sample_equal, cancelled, fail_start, fail_write;
static int fail_allocation, allocations;
static uint32_t fail_read_at, corrupt_at;
static uint8_t snapshot[262144];
static burner_tf_writer_ctx_t *owner;
static void *heap_caps_malloc(size_t size, int caps) {
    (void)caps;
    if (++allocations == fail_allocation) return NULL;
    return malloc(size);
}
static uint64_t esp_timer_get_time(void) { static uint64_t t; return ++t; }
static int burner_calc_progress_percent_u64(uint32_t done, uint32_t total) { return 100ull * done / total; }
static void burner_status_update(int state, int progress, uint32_t done, uint32_t total,
                                const char *message, const char *name, const char *path) {
    (void)state; (void)progress; (void)total; (void)message; (void)name; (void)path; recorded = done;
}
static void burner_status_set_verify_sample(uint32_t addr, uint8_t a, uint8_t b, bool equal) {
    (void)a; (void)b; mismatch_addr = addr; sample_equal = equal;
}
static void burner_emit_progress_cb(int progress, uint32_t done) { (void)progress; (void)done; }
static void burner_status_record_dump_read(uint32_t bytes, uint64_t us) { (void)bytes; (void)us; }
static void burner_status_record_dump_finalize(uint64_t us) { (void)us; }
static bool usb_msc_tf_in_use_by_host(void) { return false; }
static esp_err_t burner_cancel_poll(void) { return cancelled ? ESP_ERR_INVALID_STATE : ESP_OK; }
static esp_err_t burner_replace_file(const char *a, const char *b) { return rename(a,b) == 0 ? ESP_OK : ESP_FAIL; }
static esp_err_t burner_tf_writer_start(burner_tf_writer_ctx_t *ctx, int fd) {
    if (fail_start) return ESP_ERR_NO_MEM;
    ctx->fd = fd; ctx->running = true; owner = ctx; return ESP_OK;
}
static esp_err_t burner_tf_writer_submit(burner_tf_writer_ctx_t *ctx, const uint8_t *src, size_t bytes) {
    assert(!ctx->inflight && bytes <= sizeof(snapshot));
    ctx->src = src; ctx->bytes = bytes; ctx->inflight = true;
    memcpy(snapshot, src, bytes); return ESP_OK;
}
static esp_err_t burner_tf_writer_wait(burner_tf_writer_ctx_t *ctx) {
    assert(ctx->inflight);
    assert(memcmp(ctx->src, snapshot, ctx->bytes) == 0);
    ctx->inflight = false;
    if (fail_write) return ESP_FAIL;
    assert(write(ctx->fd, ctx->src, ctx->bytes) == (ssize_t)ctx->bytes);
    return ESP_OK;
}
static void burner_tf_writer_stop(burner_tf_writer_ctx_t *ctx) {
    if (ctx->inflight) (void)burner_tf_writer_wait(ctx);
    ctx->running = false; owner = NULL;
}
static uint8_t pattern(uint32_t address) { return (address * 131u) ^ (address >> 8) ^ (address >> 16); }
static esp_err_t cart_read(uint8_t *out, size_t count, uint32_t address, const burner_task_param_t *job) {
    (void)job;
    if (address >= fail_read_at) return ESP_FAIL;
    if (owner && owner->inflight) {
        assert((uintptr_t)out + count <= (uintptr_t)owner->src ||
               (uintptr_t)out >= (uintptr_t)owner->src + owner->bytes);
    }
    for (size_t i = 0; i < count; ++i) out[i] = pattern(address + i) ^ ((address + i == corrupt_at) ? 1 : 0);
    return ESP_OK;
}
#include "../../main/burner/core/burn/burner_dump_stream.c"
#include "../../main/burner/core/burn/burner_verify_stream.c"
static void reset(void) {
    allocations = fail_allocation = 0; recorded = 0;
    cancelled = fail_start = fail_write = false;
    fail_read_at = corrupt_at = UINT32_MAX;
    unlink("dump_fixture.bin"); unlink("dump_fixture.bin.dump_tmp");
}
static esp_err_t dump(burner_task_param_t *job, uint32_t chunk) {
    return burner_run_read_job_direct(job, job->total_bytes, chunk, cart_read,
                                     "dump", "allocation", "read", "write");
}
int main(void) {
    _set_fmode(_O_BINARY);
    const uint32_t lengths[] = {1, 511, 512, 8191, 8192, 8193, 16385, 65536, 65537, 1048593};
    const uint32_t chunks[] = {32768, 65536, 131072, 262144};
    for (size_t c = 0; c < 4; ++c) for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l) {
        reset();
        burner_task_param_t job = {0x1fff0, lengths[l], "fixture", "dump_fixture.bin", l % 2, lengths[l]};
        assert(dump(&job, chunks[c]) == ESP_OK && recorded == job.total_bytes);
        assert(burner_verify_stream(&job, cart_read) == ESP_OK && recorded == job.total_bytes);
        verified_count = 0;
        assert(burner_verify_stream_expected(&job, cart_read, true) == ESP_OK && verified_count == job.total_bytes);
        // A bad header/checksum byte and a byte at the last boundary must fail exactly.
        corrupt_at = job.addr_begin + (job.total_bytes > 0xbd ? 0xbd : 0);
        assert(burner_verify_stream(&job, cart_read) == ESP_FAIL && !sample_equal && mismatch_addr == corrupt_at);
        verified_count = 0;
        assert(burner_verify_stream_expected(&job, cart_read, true) == ESP_FAIL && verified_count < job.total_bytes);
        corrupt_at = job.addr_begin + job.total_bytes - 1;
        assert(burner_verify_stream(&job, cart_read) == ESP_FAIL && mismatch_addr == corrupt_at);
        corrupt_at = UINT32_MAX;
        job.total_bytes++; // Truncation is not silently padded.
        assert(burner_verify_stream(&job, cart_read) == ESP_FAIL);
    }
    for (int fault = 0; fault < 6; ++fault) {
        reset();
        burner_task_param_t job = {0, 200000, "fixture", "dump_fixture.bin", 1, 200000};
        if (fault < 2) fail_allocation = fault + 1;
        if (fault == 2) fail_start = true;
        if (fault == 3) fail_write = true;
        if (fault == 4) fail_read_at = 131072; // Fail while another slot is owned by writer.
        if (fault == 5) cancelled = true;
        assert(dump(&job, 65536) != ESP_OK);
        assert(access(job.rom_path, F_OK) != 0 && access("dump_fixture.bin.dump_tmp", F_OK) != 0);
        assert(owner == NULL);
    }
    reset();
    puts("Production dump/verify: delayed slot ownership, boundaries, odd tails, exact mismatch and failure cleanup passed");
}

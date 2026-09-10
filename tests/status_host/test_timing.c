/* Deterministic clock tests of the production status accumulator. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define WS_SERVER_INTERNAL_H
typedef int burner_state_t;
typedef int burner_cart_mode_t;
typedef int burner_gba_save_type_t;
typedef int burner_gba_sram_patch_kind_t;
enum { BURNER_STATE_IDLE, BURNER_STATE_RECEIVING, BURNER_STATE_BURNING,
       BURNER_STATE_DONE, BURNER_STATE_ERROR, BURNER_STATE_CANCELLED };
enum { BURNER_CART_MODE_MBC5, BURNER_CART_MODE_GBA };
#define BURNER_GBA_SAVE_TYPE_SRAM 0
#define BURNER_GBA_SRAM_PATCH_NONE 0
#define BURNER_FILE_NAME_LEN 240
#define BURNER_FILE_PATH_LEN 304
#define BURNER_SPEED_WARMUP_US 1000000ULL
#define BURNER_CPU_YIELD_INTERVAL_US 20000ULL
#define BURNER_UI_NOTIFY_INTERVAL_US 100000ULL
#include "../../main/burner/core/burner_status_types.h"
static burner_status_t s_status;
static SemaphoreHandle_t s_status_lock = (void *)1;
static uint64_t s_burn_task_last_yield_us, clock_us = 1000000;
static uint64_t esp_timer_get_time(void) { return clock_us; }
static const char *burner_state_to_str(burner_state_t state) { (void)state; return "test"; }
static void ui_set_burn_progress(int p, uint32_t a, uint32_t b) { (void)p; (void)a; (void)b; }
static void ui_set_status_text(const char *s) { (void)s; }
void burner_status_update(burner_state_t, int, uint32_t, uint32_t, const char *, const char *, const char *);
#include "../../main/burner/core/burner_status.c"

static void check(uint64_t write, uint64_t erase)
{
    burner_status_t snapshot;
    burner_status_t stored = s_status;
    burner_status_snapshot(&snapshot);
    assert(snapshot.write_elapsed_us == write);
    assert(snapshot.erase_elapsed_us == erase);
    assert(memcmp(&stored, &s_status, sizeof(stored)) == 0);
}

int main(void)
{
    burner_status_mark_task_begin();
    burner_status_mark_write_manual_begin();
    /* TF reads neither start nor advance the cartridge write timer. */
    clock_us += 2000000;
    burner_status_record_tf_to_psram_copy(2000, 2000000);
    check(0, 0);
    burner_status_mark_erase_begin();
    clock_us += 3000000;
    burner_status_mark_erase_end();
    check(0, 3000000);

    burner_status_mark_write_begin();
    clock_us += 1000000;
    check(1000000, 3000000);
    /* A pipeline write invokes a synchronous erase midway through the call. */
    burner_status_mark_erase_begin();
    clock_us += 4000000;
    burner_status_record_tf_to_psram_copy(9000, 9000000); // concurrent prefetch accounting
    check(1000000, 7000000);
    burner_status_mark_erase_end();
    clock_us += 2000000;
    uint64_t elapsed = burner_status_mark_write_end();
    assert(elapsed == 3000000);
    burner_status_record_write_sample(3000, elapsed);
    check(3000000, 7000000);
    assert(s_status.speed_avg_bps == 1000 && s_status.tf_to_psram_total_us == 11000000);

    /* Idle gaps, TF activity and repeated cleanup cannot inflate write time. */
    clock_us += 5000000;
    assert(burner_status_mark_write_end() == 0);
    check(3000000, 7000000);
    burner_status_mark_write_begin();
    clock_us += 2000000;
    elapsed = burner_status_mark_write_end();
    assert(elapsed == 2000000);
    // A failed/cancelled block has elapsed time, but no completed-byte speed sample.
    check(5000000, 7000000);
    burner_status_mark_task_end();
    uint64_t total = s_status.task_elapsed_us;
    clock_us += 9000000;
    burner_status_t snapshot;
    burner_status_snapshot(&snapshot);
    assert(snapshot.task_elapsed_us == total);
    check(5000000, 7000000);

    burner_status_mark_task_begin();
    burner_status_mark_write_manual_begin();
    check(0, 0);
    assert(s_status.tf_to_psram_total_us == 0 && s_status.write_speed_total_us == 0);
    // An already-active erase contributes only its overlap with a write sample.
    burner_status_mark_erase_begin();
    clock_us += 1000000;
    burner_status_mark_write_begin();
    clock_us += 2000000;
    check(0, 3000000);
    burner_status_mark_erase_end();
    clock_us += 4000000;
    assert(burner_status_mark_write_end() == 4000000);
    check(4000000, 3000000);
    burner_status_mark_write_begin();
    clock_us += 5000000000ULL;
    assert(burner_status_mark_write_end() == 5000000000ULL);
    check(5004000000ULL, 3000000);
    burner_status_mark_task_begin();
    burner_status_plan_write_verify();
    burner_status_update(BURNER_STATE_BURNING, 100, 1024, 1024, "written", "test", "test");
    assert(s_status.progress == 80);
    burner_status_mark_verify_begin();
    clock_us += 500000;
    burner_status_record_verified(512);
    burner_status_update(BURNER_STATE_BURNING, 50, 512, 1024, "verify", "test", "test");
    burner_status_t verified;
    burner_status_snapshot(&verified);
    assert(verified.progress == 90 && verified.verify_elapsed_us == 500000);
    assert(verified.write_verified_bytes == 512 && verified.write_elapsed_us == 0);
    burner_status_mark_verify_end();
    clock_us += 1000000;
    burner_status_snapshot(&verified);
    assert(verified.verify_elapsed_us == 500000 && !verified.write_verify_active);
    burner_status_record_write_matched(1024);
    burner_status_mark_task_begin();
    assert(!s_status.write_verification_planned && !s_status.write_matched_bytes && !s_status.write_verified_bytes);
    puts("Burn timing and write/verify phase separation passed");
}

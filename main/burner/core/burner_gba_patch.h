#ifndef BURNER_GBA_PATCH_H
#define BURNER_GBA_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool running;
    bool cancel_requested;
    char phase[32];
    char detail[64];
    uint32_t offset, total, passes;
    uint64_t read_bytes;
    int64_t read_us, elapsed_ms;
    int result;
} burner_gba_patch_debug_t;

void burner_gba_patch_debug_snapshot(burner_gba_patch_debug_t *out);
bool burner_gba_patch_debug_cancel(void);

typedef struct {
    bool created_copy;
    bool sram_patched;
    bool waitcnt_patched;
    bool batteryless_patched;
    uint32_t waitcnt_count;
    uint32_t batteryless_save_size;
    char patch_name[32];
    uint32_t output_size;
} burner_gba_patch_report_t;

typedef enum {
    BURNER_GBA_PATCH_PROGRESS_SRAM = 0,
    BURNER_GBA_PATCH_PROGRESS_BATTERYLESS,
    BURNER_GBA_PATCH_PROGRESS_WAITCNT,
} burner_gba_patch_progress_kind_t;

typedef void (*burner_gba_patch_progress_cb_t)(
    burner_gba_patch_progress_kind_t kind,
    int progress,
    const char *message,
    void *user_ctx);

#define BURNER_GBA_PATCH_MAX_SRAM_OPS 16U
#define BURNER_GBA_PATCH_MAX_WAITCNT_OPS 128U
#define BURNER_GBA_PATCH_MAX_REPLACEMENT 128U
#define BURNER_GBA_PATCH_MAX_IRQ_OPS 128U
#define BURNER_GBA_PATCH_MAX_BATTERYLESS_WRITES 4U
#define BURNER_GBA_PATCH_MAX_PAYLOAD 4096U

typedef struct {
    uint32_t offset;
    uint16_t length;
    uint8_t data[BURNER_GBA_PATCH_MAX_REPLACEMENT];
} burner_gba_patch_write_t;

/* A compact, immutable plan applied to each ROM staging window at burn time. */
typedef struct {
    uint32_t source_size;
    uint32_t output_size;
    size_t sram_count;
    burner_gba_patch_write_t sram[BURNER_GBA_PATCH_MAX_SRAM_OPS];
    size_t waitcnt_count;
    uint32_t waitcnt_offsets[BURNER_GBA_PATCH_MAX_WAITCNT_OPS];
    size_t batteryless_irq_count;
    uint32_t batteryless_irq_offsets[BURNER_GBA_PATCH_MAX_IRQ_OPS];
    size_t batteryless_write_count;
    burner_gba_patch_write_t batteryless_writes[BURNER_GBA_PATCH_MAX_BATTERYLESS_WRITES];
    uint32_t batteryless_save_size;
    uint32_t payload_offset;
    uint16_t payload_length;
    uint8_t payload[BURNER_GBA_PATCH_MAX_PAYLOAD];
} burner_gba_patch_plan_t;

/* Returns true when a ROM contains a known non-SRAM save implementation. */
bool burner_gba_rom_has_sram_patch_target(const char *input_path);

int burner_build_gba_patch_plan(
    const char *input_path,
    bool apply_sram_patch,
    bool apply_waitcnt,
    bool apply_batteryless,
    burner_gba_patch_plan_t *plan,
    burner_gba_patch_report_t *report,
    char *error_msg,
    size_t error_msg_len,
    burner_gba_patch_progress_cb_t progress_cb,
    void *progress_ctx);

void burner_apply_gba_patch_plan(
    uint8_t *buffer,
    size_t buffer_size,
    uint32_t base_offset,
    const burner_gba_patch_plan_t *plan);

/* Return false to cancel writing. The callback runs in the caller's worker. */
typedef bool (*burner_gba_patch_save_progress_cb_t)(uint32_t done, uint32_t total, void *ctx);

/* Save selected patches to a new sibling *.patched[-N].gba file. Existing
 * files are never overwritten; partial output is removed on failure/cancel. */
int burner_save_gba_patch_file(
    const char *input_path, bool apply_sram_patch, bool apply_waitcnt, bool apply_batteryless,
    char *output_path, size_t output_path_len, burner_gba_patch_report_t *report,
    char *error_msg, size_t error_msg_len, burner_gba_patch_progress_cb_t progress_cb,
    burner_gba_patch_save_progress_cb_t save_progress_cb, void *progress_ctx);

#endif

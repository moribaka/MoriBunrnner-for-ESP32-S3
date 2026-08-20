#ifndef BURNER_GBA_PATCH_H
#define BURNER_GBA_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool created_copy;
    bool sram_patched;
    bool waitcnt_patched;
    uint32_t waitcnt_count;
    char patch_name[32];
    uint32_t output_size;
} burner_gba_patch_report_t;

/* Prepare a patched ROM without modifying input_path. */
int burner_prepare_gba_patch_file(
    const char *input_path,
    bool apply_waitcnt,
    char *output_path,
    size_t output_path_len,
    burner_gba_patch_report_t *report,
    char *error_msg,
    size_t error_msg_len);

#endif

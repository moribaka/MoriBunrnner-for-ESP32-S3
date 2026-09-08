/* Compile the production patch implementation with host-only RTOS/heap stubs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/burner/core/burner_gba_patch.c"

static FILE *rom_file(size_t size)
{
    FILE *fp = tmpfile();
    assert(fp);
    unsigned char zeros[4096] = {0};
    while (size) {
        size_t n = size < sizeof(zeros) ? size : sizeof(zeros);
        assert(fwrite(zeros, 1, n, fp) == n);
        size -= n;
    }
    rewind(fp);
    return fp;
}

static void put(FILE *fp, uint32_t offset, const void *data, size_t size)
{
    assert(fseek(fp, offset, SEEK_SET) == 0);
    assert(fwrite(data, 1, size, fp) == size);
    assert(fflush(fp) == 0);
}

int main(void)
{
    /* Pattern overlap and wildcard masks must work across read boundaries. */
    FILE *fp = rom_file(3 * PATCH_SCAN_BYTES);
    const unsigned char pattern[] = {0x91, 0x82, 0x73, 0x64, 0x55};
    uint32_t offset = 0;
    put(fp, PATCH_SCAN_BYTES - 2, pattern, sizeof(pattern));
    assert(find_pattern(fp, 3 * PATCH_SCAN_BYTES, pattern, sizeof(pattern), NULL, 0,
                        &offset, NULL, BURNER_GBA_PATCH_PROGRESS_SRAM, 0, 100) == 0);
    assert(offset == PATCH_SCAN_BYTES - 2);
    unsigned char masked[] = {0x00, 0x82, 0x73, 0x00, 0x55};
    unsigned char mask[] = {1, 0, 0, 1, 0};
    assert(find_pattern(fp, 3 * PATCH_SCAN_BYTES, masked, sizeof(masked), mask, sizeof(mask),
                        &offset, NULL, BURNER_GBA_PATCH_PROGRESS_SRAM, 0, 100) == 0);
    assert(offset == PATCH_SCAN_BYTES - 2);
    fclose(fp);

    /* All identifiers are found in one read pass, including cross-block names.
     * A higher-priority type late in the ROM must not lose to an earlier one. */
    fp = rom_file(3 * PATCH_ANALYSIS_CHUNK_BYTES);
    const sram_patch_set_t *first = &s_generated_patch_sets[0];
    const sram_patch_set_t *later = &s_generated_patch_sets[10];
    put(fp, PATCH_ANALYSIS_CHUNK_BYTES - 3, later->identifier, later->identifier_len);
    put(fp, 2 * PATCH_ANALYSIS_CHUNK_BYTES + 64, first->identifier, first->identifier_len);
    bool found[sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0])];
    assert(scan_patch_identifiers(fp, 3 * PATCH_ANALYSIS_CHUNK_BYTES, found, true, NULL, NULL) == 0);
    assert(found[0] && found[10]);
    assert(scan_patch_identifiers(fp, 3 * PATCH_ANALYSIS_CHUNK_BYTES, found, false, NULL, NULL) == 0);
    assert(!found[0] && found[10]);
    fclose(fp);

    /* A Thumb literal reference straddles the heap-backed WAITCNT read window. */
    fp = rom_file(2 * PATCH_SCAN_BYTES);
    const unsigned char thumb_ldr[] = {0x00, 0x48};
    const unsigned char waitcnt[] = {0x04, 0x02, 0x00, 0x04};
    put(fp, PATCH_SCAN_BYTES - 2, thumb_ldr, sizeof(thumb_ldr));
    put(fp, PATCH_SCAN_BYTES, waitcnt, sizeof(waitcnt));
    put(fp, PATCH_SCAN_BYTES + 8192, waitcnt, sizeof(waitcnt)); /* Unreferenced literal. */
    uint32_t offsets[4];
    size_t count;
    assert(collect_waitcnt_offsets(fp, 2 * PATCH_SCAN_BYTES, offsets, 4, &count, NULL, NULL) == 0);
    assert(count == 1 && offsets[0] == PATCH_SCAN_BYTES);
    assert(collect_waitcnt_offsets(fp, 2 * PATCH_SCAN_BYTES, offsets, 0, &count, NULL, NULL) == -2);
    fclose(fp);

    /* Word-aligned burn windows must equal one full apply, including a split SRAM patch. */
    burner_gba_patch_plan_t plan = {0};
    unsigned char whole[128] = {0}, windows[128] = {0};
    plan.sram_count = 1;
    plan.sram[0].offset = 15;
    plan.sram[0].length = sizeof(pattern);
    memcpy(plan.sram[0].data, pattern, sizeof(pattern));
    plan.waitcnt_count = 1;
    plan.waitcnt_offsets[0] = 60;
    memset(whole + 60, 0xFF, 4);
    memcpy(windows, whole, sizeof(whole));
    burner_apply_gba_patch_plan(whole, sizeof(whole), 0, &plan);
    for (unsigned i = 0; i < sizeof(windows); i += 16)
        burner_apply_gba_patch_plan(windows + i, 16, i, &plan);
    assert(memcmp(whole, windows, sizeof(whole)) == 0);

    /* Failure must release the single-plan guard for the next request. */
    char error[128];
    burner_gba_patch_report_t report;
    assert(burner_build_gba_patch_plan("missing-test-rom.gba", true, false, false,
               &plan, &report, error, sizeof(error), NULL, NULL) != ESP_OK);
    burner_gba_patch_debug_t snapshot;
    burner_gba_patch_debug_snapshot(&snapshot);
    assert(!snapshot.running);
    assert(!burner_gba_patch_debug_cancel());
    puts("patch host tests passed");
    return 0;
}

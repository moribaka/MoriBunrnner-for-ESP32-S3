/* Compare a device-exported sbw ROM against the production plan on the host.
 * Usage: verify_export original.gba exported.gba */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/burner/core/burner_gba_patch.c"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    burner_gba_patch_plan_t plan;
    burner_gba_patch_report_t report;
    char error[128];
    if (burner_build_gba_patch_plan(argv[1], true, true, true, &plan, &report,
                                  error, sizeof(error), NULL, NULL) != ESP_OK) {
        fprintf(stderr, "Plan failed: %s\n", error);
        return 1;
    }
    FILE *source = fopen(argv[1], "rb"), *exported = fopen(argv[2], "rb");
    if (source == NULL || exported == NULL) return 1;
    unsigned char expected[PATCH_SCAN_BYTES], actual[PATCH_SCAN_BYTES];
    for (uint32_t offset = 0; offset < plan.output_size;) {
        size_t count = plan.output_size - offset;
        if (count > sizeof(expected)) count = sizeof(expected);
        size_t available = offset < plan.source_size ? plan.source_size - offset : 0;
        if (available > count) available = count;
        memset(expected, 0xFF, count);
        if (fread(expected, 1, available, source) != available || fread(actual, 1, count, exported) != count)
            return 1;
        burner_apply_gba_patch_plan(expected, count, offset, &plan);
        for (size_t i = 0; i < count; ++i) {
            if (expected[i] != actual[i]) {
                fprintf(stderr, "Mismatch at 0x%08lx: expected %02x got %02x\n",
                        (unsigned long)(offset + i), expected[i], actual[i]);
                return 1;
            }
        }
        offset += (uint32_t)count;
    }
    if (fgetc(exported) != EOF) return 1;
    fclose(source);
    fclose(exported);
    printf("Verified %lu bytes: SRAM=%lu IRQ=%lu WAITCNT=%lu payload=0x%08lx\n",
           (unsigned long)plan.output_size, (unsigned long)plan.sram_count,
           (unsigned long)plan.batteryless_irq_count, (unsigned long)plan.waitcnt_count,
           (unsigned long)plan.payload_offset);
    return 0;
}

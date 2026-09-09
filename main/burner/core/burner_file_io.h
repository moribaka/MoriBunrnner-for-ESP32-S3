#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

/* S3 SDMMC cannot DMA into PSRAM: a direct fread there falls back to one
 * 512-byte command per sector. An internal buffer keeps multi-sector reads.
 * Keep allocation local: patch scans and burner readers can run on either core. */
static inline size_t burner_file_read(void *dst, size_t size, size_t count, FILE *fp)
{
    if (size == 0 || count == 0) return 0;
    if (count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (bytes < 4096 || (esp_ptr_dma_capable(dst) && ((uintptr_t)dst & 3u) == 0))
        return fread(dst, size, count, fp);

    const size_t capacity = 16u * 1024u;
    unsigned char *dma = heap_caps_malloc(capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (dma == NULL) return fread(dst, size, count, fp);
    size_t copied = 0;
    while (copied < bytes) {
        size_t chunk = bytes - copied;
        if (chunk > capacity) chunk = capacity;
        size_t got = fread(dma, 1, chunk, fp);
        memcpy((unsigned char *)dst + copied, dma, got);
        copied += got;
        if (got != chunk) break;
    }
    heap_caps_free(dma);
    return copied / size;
}

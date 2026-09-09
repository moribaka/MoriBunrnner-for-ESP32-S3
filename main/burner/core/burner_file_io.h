#pragma once

#include <stdio.h>

/* Newlib's small default FILE buffer splits sequential ROM reads into tiny
 * FatFS requests. A 16 KiB buffer allows SDMMC multi-sector transfers; stdio
 * owns and frees it on fclose. Allocation failure keeps the default buffer. */
static inline FILE *burner_file_open_read(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp != NULL && setvbuf(fp, NULL, _IOFBF, 16u * 1024u) != 0) {
        fclose(fp);
        fp = fopen(path, "rb");
    }
    return fp;
}

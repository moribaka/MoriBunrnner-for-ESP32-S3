#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/burner/core/burner_source_reader.h"

int main(void)
{
    FILE *fp = tmpfile();
    assert(fp && fwrite("abcd", 1, 4, fp) == 4);
    rewind(fp);
    unsigned char buf[8];
    assert(burner_source_read_exact(fp, buf, 2, 4) == ESP_OK && memcmp(buf, "ab", 2) == 0);
    assert(burner_source_read_exact(fp, buf, 4, 4) == ESP_OK);
    assert(buf[0] == 'c' && buf[1] == 'd' && buf[2] == 255 && buf[3] == 255);
    assert(burner_source_read_exact(fp, buf, 8, 4) == ESP_OK);
    for (unsigned i = 0; i < 8; ++i) assert(buf[i] == 255);
    assert(fseek(fp, 12, SEEK_SET) == 0);
    assert(burner_source_read_exact(fp, buf, 8, 4) == ESP_OK);
    for (unsigned i = 0; i < 8; ++i) assert(buf[i] == 255);
    assert(fseek(fp, 8, SEEK_SET) == 0);
    assert(burner_source_read_exact(fp, buf, 8, 8) == ESP_FAIL); /* seek cannot hide truncation */
    rewind(fp);
    assert(burner_source_read_exact(fp, buf, 8, 8) == ESP_FAIL); /* source truncated */
    rewind(fp);
    assert(burner_source_read_exact(fp, buf, 8, 0) == ESP_FAIL); /* strict read */
    rewind(fp);
    assert(burner_source_read_exact(fp, buf, 4, 0) == ESP_OK);
    assert(burner_source_read_exact(fp, buf, 1, 0) == ESP_FAIL);
    assert(burner_source_read_exact(NULL, buf, 1, 0) == ESP_ERR_INVALID_ARG);
    fclose(fp);
    puts("Source bounds, truncation, strict EOF and expanded tail passed");
    return 0;
}

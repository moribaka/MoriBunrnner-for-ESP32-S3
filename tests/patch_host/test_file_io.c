#include <assert.h>
#include <stdlib.h>
static int fail_alloc;
#define heap_caps_malloc(size, caps) (fail_alloc ? NULL : malloc(size))
#include "../../main/burner/core/burner_source_reader.h"

int main(void)
{
    const size_t length = 40003;
    unsigned char *data = malloc(length), *out = malloc(length + 100);
    assert(data && out);
    for (size_t i = 0; i < length; ++i) data[i] = (unsigned char)(i * 71 + i / 512);
    FILE *fp = tmpfile();
    assert(fp && fwrite(data, 1, length, fp) == length);
    for (fail_alloc = 0; fail_alloc <= 1; ++fail_alloc) {
        rewind(fp);
        assert(burner_file_read(out, 1, length, fp) == length);
        assert(memcmp(data, out, length) == 0);
        assert(ftell(fp) == (long)length);
        assert(fseek(fp, 3, SEEK_SET) == 0);
        assert(burner_file_read(out + 1, 7, (length + 7) / 7, fp) == (length - 3) / 7);
        assert(memcmp(data + 3, out + 1, length - 3) == 0 && feof(fp));
        rewind(fp);
        assert(burner_source_read_exact(fp, out, length + 100, length) == ESP_OK);
        assert(memcmp(data, out, length) == 0);
        for (size_t i = length; i < length + 100; ++i) assert(out[i] == 255);
        rewind(fp);
        assert(burner_source_read_exact(fp, out, length + 100, length + 1) == ESP_FAIL);
    }
    fclose(fp); free(data); free(out);
    puts("DMA staging reads: content, offset, partial elements, EOF, padding and allocation fallback passed");
}

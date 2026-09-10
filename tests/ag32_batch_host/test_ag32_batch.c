#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ag32_batch_format.h"

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    while (size-- > 0u) {
        crc ^= *data++;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void update_header_crc(uint8_t *header)
{
    write_le32(header + AG32_BATCH_HEADER_SIZE - 4u,
        crc32_update(0u, header, AG32_BATCH_HEADER_SIZE - 4u));
}

static int parse_memory(const uint8_t *data, size_t size, char *error, size_t error_size)
{
    ag32_batch_manifest_t manifest;
    FILE *file = tmpfile();
    assert(file != NULL);
    assert(fwrite(data, 1u, size, file) == size);
    rewind(file);
    int result = ag32_batch_parse(file, &manifest, error, error_size);
    fclose(file);
    return result;
}

static void assert_payload_matches(
    const char *batch_path,
    const ag32_batch_record_t *record,
    const char *payload_path)
{
    FILE *batch = fopen(batch_path, "rb");
    FILE *payload = fopen(payload_path, "rb");
    uint8_t batch_buf[4096];
    uint8_t payload_buf[4096];
    uint32_t compared = 0u;
    assert(batch != NULL && payload != NULL);
    assert(fseek(batch, (long)record->payload_offset, SEEK_SET) == 0);
    while (compared < record->payload_size) {
        size_t chunk = record->payload_size - compared;
        if (chunk > sizeof(batch_buf)) chunk = sizeof(batch_buf);
        assert(fread(batch_buf, 1u, chunk, batch) == chunk);
        assert(fread(payload_buf, 1u, chunk, payload) == chunk);
        assert(memcmp(batch_buf, payload_buf, chunk) == 0);
        compared += (uint32_t)chunk;
    }
    assert(fgetc(payload) == EOF);
    fclose(payload);
    fclose(batch);
}

int main(int argc, char **argv)
{
    ag32_batch_manifest_t manifest;
    char error[160];
    FILE *file;

    assert(argc == 2 || argc == 4);
    file = fopen(argv[1], "rb");
    assert(file != NULL);
    assert(ag32_batch_parse(file, &manifest, error, sizeof(error)) == 0);
    fclose(file);

    assert(manifest.record_count == 3u);
    assert(manifest.flash_record_count == 2u);
    assert(manifest.records[0].kind == AG32_BATCH_RECORD_OPTION);
    assert(manifest.records[0].address == AG32_OPTION_BASE);
    assert(manifest.records[1].kind == AG32_BATCH_RECORD_FLASH);
    assert(manifest.records[2].address == AG32_FLASH_BASE);
    assert(manifest.file_size == manifest.payload_bytes + manifest.record_count * AG32_BATCH_HEADER_SIZE);
    if (argc == 4) {
        assert_payload_matches(argv[1], &manifest.records[1], argv[2]);
        assert_payload_matches(argv[1], &manifest.records[2], argv[3]);
    }

    uint8_t *batch = malloc(manifest.file_size);
    assert(batch != NULL);
    file = fopen(argv[1], "rb");
    assert(file != NULL);
    assert(fread(batch, 1u, manifest.file_size, file) == manifest.file_size);
    fclose(file);

    uint32_t middle_header = manifest.records[1].payload_offset - AG32_BATCH_HEADER_SIZE;
    uint32_t final_header = manifest.records[2].payload_offset - AG32_BATCH_HEADER_SIZE;

    write_le32(batch + middle_header + 8u, manifest.records[1].address + 1u);
    update_header_crc(batch + middle_header);
    assert(parse_memory(batch, manifest.file_size, error, sizeof(error)) != 0);
    write_le32(batch + middle_header + 8u, manifest.records[1].address);
    update_header_crc(batch + middle_header);

    write_le32(batch + 16u, 0u);
    update_header_crc(batch);
    assert(parse_memory(batch, manifest.file_size, error, sizeof(error)) != 0);
    write_le32(batch + 16u, 1u);
    update_header_crc(batch);

    write_le32(batch + middle_header + 24u, 0x10001u);
    update_header_crc(batch + middle_header);
    assert(parse_memory(batch, manifest.file_size, error, sizeof(error)) != 0);
    write_le32(batch + middle_header + 24u, 0x10000u);
    update_header_crc(batch + middle_header);

    write_le32(batch + final_header + 24u, 0x10002u);
    update_header_crc(batch + final_header);
    assert(parse_memory(batch, manifest.file_size, error, sizeof(error)) != 0);
    write_le32(batch + final_header + 24u, 0x10006u);
    update_header_crc(batch + final_header);

    write_le32(batch + final_header + 8u, AG32_FLASH_BASE + 0x1000u);
    update_header_crc(batch + final_header);
    assert(parse_memory(batch, manifest.file_size, error, sizeof(error)) != 0);

    free(batch);

    puts("AG32 batch parser test passed");
    return 0;
}

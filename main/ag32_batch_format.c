#include "ag32_batch_format.h"

#include <stdarg.h>
#include <string.h>

#define AG32_BATCH_VERSION 1u
#define AG32_BATCH_PROGRAM_FLASH 1u
#define AG32_BATCH_ERASE_FLASH 1u
#define AG32_BATCH_RUN_SUPPORTED_FLAGS (AG32_BATCH_RUN_RESET | AG32_BATCH_RUN_STOP)

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    while (size-- > 0u) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static int fail(char *error, size_t error_size, const char *format, ...)
{
    if (error != NULL && error_size > 0u) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return -1;
}

static bool range_fits(uint32_t address, uint32_t size, uint32_t base, uint32_t capacity)
{
    uint64_t start = address;
    uint64_t end = start + size;
    return size > 0u && start >= base && end <= (uint64_t)base + capacity;
}

static bool ranges_overlap(
    uint32_t first_address,
    uint32_t first_size,
    uint32_t second_address,
    uint32_t second_size)
{
    uint64_t first_end = (uint64_t)first_address + first_size;
    uint64_t second_end = (uint64_t)second_address + second_size;
    return first_address < second_end && second_address < first_end;
}

static int get_file_size(FILE *file, uint32_t *size_out)
{
    long size;

    if (fseek(file, 0, SEEK_END) != 0) {
        return -1;
    }
    size = ftell(file);
    if (size <= 0 || (unsigned long)size > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
        return -1;
    }
    *size_out = (uint32_t)size;
    return 0;
}

static int decode_record(
    const uint8_t header[AG32_BATCH_HEADER_SIZE],
    ag32_batch_record_t *record,
    uint32_t record_index,
    char *error,
    size_t error_size)
{
    record->device_id = read_le32(header + 0u);
    record->payload_size = read_le32(header + 4u);
    record->address = read_le32(header + 8u);
    record->type = read_le32(header + 12u);
    record->erase_options = read_le32(header + 16u);
    record->program_options = read_le32(header + 20u);
    record->run_options = read_le32(header + 24u);
    record->encrypt_offset = read_le32(header + 28u);
    record->encrypt_size = read_le32(header + 32u);
    record->version = read_le32(header + AG32_BATCH_HEADER_SIZE - 8u);
    record->header_crc32 = read_le32(header + AG32_BATCH_HEADER_SIZE - 4u);

    if (crc32_update(0u, header, AG32_BATCH_HEADER_SIZE - 4u) != record->header_crc32) {
        return fail(error, error_size, "record %u header CRC mismatch", record_index);
    }
    if (record->version != AG32_BATCH_VERSION) {
        return fail(error, error_size, "record %u unsupported version %u", record_index, record->version);
    }
    if (record->device_id != AG32_DEVICE_ID) {
        return fail(error, error_size, "record %u device ID 0x%08x is not AG32VF407", record_index, record->device_id);
    }
    if (record->type != 0u || record->encrypt_offset != 0u || record->encrypt_size != 0u) {
        return fail(error, error_size, "record %u uses unsupported type or encryption", record_index);
    }
    if (record->erase_options > AG32_BATCH_ERASE_FLASH
        || record->program_options != AG32_BATCH_PROGRAM_FLASH
        || (record->run_options & 0xffffu & ~AG32_BATCH_RUN_SUPPORTED_FLAGS) != 0u) {
        return fail(error, error_size, "record %u has unsupported programming options", record_index);
    }

    if (range_fits(record->address, record->payload_size, AG32_FLASH_BASE, AG32_FLASH_SIZE)) {
        record->kind = AG32_BATCH_RECORD_FLASH;
    } else if (range_fits(record->address, record->payload_size, AG32_OPTION_BASE, AG32_OPTION_SIZE)) {
        record->kind = AG32_BATCH_RECORD_OPTION;
    } else {
        return fail(error, error_size, "record %u address range is outside AG32 flash", record_index);
    }
    return 0;
}

int ag32_batch_parse(
    FILE *file,
    ag32_batch_manifest_t *manifest,
    char *error,
    size_t error_size)
{
    uint8_t header[AG32_BATCH_HEADER_SIZE];
    uint32_t offset = 0u;
    bool has_main_program = false;

    if (file == NULL || manifest == NULL) {
        return fail(error, error_size, "invalid parser arguments");
    }
    memset(manifest, 0, sizeof(*manifest));
    if (get_file_size(file, &manifest->file_size) != 0) {
        return fail(error, error_size, "cannot determine batch file size");
    }

    while (offset < manifest->file_size) {
        ag32_batch_record_t *record;
        uint64_t next_offset;

        if (manifest->record_count >= AG32_BATCH_MAX_RECORDS) {
            return fail(error, error_size, "batch has more than %u records", AG32_BATCH_MAX_RECORDS);
        }
        if (manifest->file_size - offset < AG32_BATCH_HEADER_SIZE
            || fseek(file, (long)offset, SEEK_SET) != 0
            || fread(header, 1u, sizeof(header), file) != sizeof(header)) {
            return fail(error, error_size, "record %u header is truncated", manifest->record_count);
        }

        record = &manifest->records[manifest->record_count];
        if (decode_record(header, record, manifest->record_count, error, error_size) != 0) {
            return -1;
        }
        record->payload_offset = offset + AG32_BATCH_HEADER_SIZE;
        next_offset = (uint64_t)record->payload_offset + record->payload_size;
        if (next_offset > manifest->file_size) {
            return fail(error, error_size, "record %u payload is truncated", manifest->record_count);
        }

        for (uint32_t i = 0u; i < manifest->record_count; ++i) {
            const ag32_batch_record_t *previous = &manifest->records[i];
            if (record->kind == previous->kind
                && ranges_overlap(record->address, record->payload_size,
                    previous->address, previous->payload_size)) {
                return fail(error, error_size, "record %u overlaps record %u", manifest->record_count, i);
            }
        }

        if (record->kind == AG32_BATCH_RECORD_OPTION) {
            uint8_t rdp[2];
            if (manifest->record_count != 0u || record->address != AG32_OPTION_BASE
                || record->payload_size != AG32_OPTION_SIZE
                || record->erase_options != AG32_BATCH_ERASE_FLASH
                || fseek(file, (long)record->payload_offset, SEEK_SET) != 0
                || fread(rdp, 1u, sizeof(rdp), file) != sizeof(rdp)
                || rdp[0] != 0xa5u || rdp[1] != 0x5au) {
                return fail(error, error_size, "full batch must start with valid option bytes");
            }
        } else {
            ++manifest->flash_record_count;
            has_main_program |= record->address == AG32_FLASH_BASE;
        }

        manifest->payload_bytes += record->payload_size;
        ++manifest->record_count;
        offset = (uint32_t)next_offset;
    }

    if (manifest->record_count < 2u
        || manifest->records[0].kind != AG32_BATCH_RECORD_OPTION
        || !has_main_program) {
        return fail(error, error_size, "file is not a complete AG32 batch");
    }
    const ag32_batch_record_t *last = &manifest->records[manifest->record_count - 1u];
    if (last->kind != AG32_BATCH_RECORD_FLASH || last->address != AG32_FLASH_BASE) {
        return fail(error, error_size, "complete batch must end with the MCU program");
    }
    for (uint32_t i = 0u; i + 1u < manifest->record_count; ++i) {
        if ((manifest->records[i].run_options & 0xffffu) != 0u) {
            return fail(error, error_size, "record %u has run flags before the final record", i);
        }
    }
    if ((last->run_options & 0xffffu)
        != (AG32_BATCH_RUN_RESET | AG32_BATCH_RUN_STOP)) {
        return fail(error, error_size, "final MCU record must request reset and stop");
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        return fail(error, error_size, "cannot rewind batch file");
    }
    if (error != NULL && error_size > 0u) {
        error[0] = '\0';
    }
    return 0;
}

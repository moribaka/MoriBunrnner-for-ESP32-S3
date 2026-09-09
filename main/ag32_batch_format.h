#ifndef AG32_BATCH_FORMAT_H
#define AG32_BATCH_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define AG32_BATCH_HEADER_SIZE 256u
#define AG32_BATCH_MAX_RECORDS 16u
#define AG32_DEVICE_ID 0x40200001u
#define AG32_FLASH_BASE 0x80000000u
#define AG32_FLASH_SIZE 0x00100000u
#define AG32_OPTION_BASE 0x81000000u
#define AG32_OPTION_SIZE 128u
#define AG32_BATCH_RUN_RESET (1u << 1)
#define AG32_BATCH_RUN_STOP (1u << 2)

typedef enum {
    AG32_BATCH_RECORD_FLASH = 0,
    AG32_BATCH_RECORD_OPTION,
} ag32_batch_record_kind_t;

typedef struct {
    uint32_t device_id;
    uint32_t payload_size;
    uint32_t address;
    uint32_t type;
    uint32_t erase_options;
    uint32_t program_options;
    uint32_t run_options;
    uint32_t encrypt_offset;
    uint32_t encrypt_size;
    uint32_t version;
    uint32_t header_crc32;
    uint32_t payload_offset;
    ag32_batch_record_kind_t kind;
} ag32_batch_record_t;

typedef struct {
    uint32_t file_size;
    uint32_t payload_bytes;
    uint32_t record_count;
    uint32_t flash_record_count;
    ag32_batch_record_t records[AG32_BATCH_MAX_RECORDS];
} ag32_batch_manifest_t;

int ag32_batch_parse(
    FILE *file,
    ag32_batch_manifest_t *manifest,
    char *error,
    size_t error_size);

#endif

#ifndef AG32_MCU_PROTOCOL_H
#define AG32_MCU_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AG32_MCU_PROTOCOL_VERSION 1u
#define AG32_MCU_FRAME_HEADER_SIZE 32u
#define AG32_MCU_MAX_PAYLOAD_SIZE 8192u
#define AG32_MCU_STREAM_CHUNK_SIZE 4096u

#define AG32_MCU_REQUEST_MAGIC 0x32514741u  /* "AGQ2" */
#define AG32_MCU_RESPONSE_MAGIC 0x32524741u /* "AGR2" */

#define AG32_MCU_MODE_ENTER_MAGIC "MORI2MCU"
#define AG32_MCU_MODE_EXIT_MAGIC "MORI2LEG"
#define AG32_MCU_MODE_MAGIC_SIZE 8u

enum {
    AG32_MCU_CAP_RAW_BACON = 1u << 0,
    AG32_MCU_CAP_GBA_ROM = 1u << 1,
    AG32_MCU_CAP_GBA_RAM = 1u << 2,
    AG32_MCU_CAP_GBC_ROM = 1u << 3,
    AG32_MCU_CAP_GBC_RAM = 1u << 4,
    AG32_MCU_CAP_AMD_PROGRAM = 1u << 5,
    AG32_MCU_CAP_POWER = 1u << 6,
    AG32_MCU_CAP_STREAM = 1u << 7,
};

typedef enum {
    AG32_MCU_CMD_PING = 0x01,
    AG32_MCU_CMD_ECHO = 0x02,
    AG32_MCU_CMD_RAW_BACON_EXEC = 0x10,
    AG32_MCU_CMD_STREAM_BEGIN = 0x20,
    AG32_MCU_CMD_CART_POWER = 0xA0,
    AG32_MCU_CMD_SYS_INFO = 0xA2,
    AG32_MCU_CMD_ROM_READ_ID = 0xF0,
    AG32_MCU_CMD_ROM_ERASE_CHIP = 0xF1,
    AG32_MCU_CMD_ROM_ERASE_BLOCK = 0xF2,
    AG32_MCU_CMD_ROM_ERASE_SECTOR = 0xF3,
    AG32_MCU_CMD_ROM_PROGRAM = 0xF4,
    AG32_MCU_CMD_ROM_WRITE = 0xF5,
    AG32_MCU_CMD_ROM_READ = 0xF6,
    AG32_MCU_CMD_RAM_WRITE = 0xF7,
    AG32_MCU_CMD_RAM_READ = 0xF8,
    AG32_MCU_CMD_RAM_PROGRAM_FLASH = 0xF9,
    AG32_MCU_CMD_GBC_WRITE = 0xFA,
    AG32_MCU_CMD_GBC_READ = 0xFB,
    AG32_MCU_CMD_GBC_ROM_PROGRAM = 0xFC,
} ag32_mcu_command_t;

typedef enum {
    AG32_MCU_STATUS_OK = 0,
    AG32_MCU_STATUS_BAD_MAGIC = 1,
    AG32_MCU_STATUS_BAD_VERSION = 2,
    AG32_MCU_STATUS_BAD_LENGTH = 3,
    AG32_MCU_STATUS_BAD_HEADER_CRC = 4,
    AG32_MCU_STATUS_BAD_PAYLOAD_CRC = 5,
    AG32_MCU_STATUS_UNSUPPORTED = 6,
    AG32_MCU_STATUS_INVALID_ARGUMENT = 7,
    AG32_MCU_STATUS_IO_ERROR = 8,
    AG32_MCU_STATUS_TIMEOUT = 9,
    AG32_MCU_STATUS_VERIFY_FAILED = 10,
} ag32_mcu_status_t;

typedef struct {
    uint8_t opcode;
    uint16_t flags;
    uint32_t sequence;
    uint32_t payload_size;
    uint32_t response_capacity;
    uint32_t timeout_ms;
    uint32_t payload_crc32;
} ag32_mcu_request_t;

typedef struct {
    uint8_t status;
    uint8_t opcode;
    uint8_t flags;
    uint32_t sequence;
    uint32_t payload_size;
    uint32_t result;
    uint32_t payload_crc32;
} ag32_mcu_response_t;

static inline uint16_t ag32_mcu_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static inline uint32_t ag32_mcu_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
}

static inline void ag32_mcu_write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static inline void ag32_mcu_write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static inline uint32_t ag32_mcu_crc32(const void *data_ptr, size_t size)
{
    const uint8_t *data = (const uint8_t *)data_ptr;
    uint32_t crc = UINT32_MAX;
    while (size-- > 0u) {
        crc ^= *data++;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static inline size_t ag32_mcu_frame_wire_size(uint32_t payload_size)
{
    return (AG32_MCU_FRAME_HEADER_SIZE + payload_size + 3u) & ~(size_t)3u;
}

static inline bool ag32_mcu_encode_request(
    uint8_t header[AG32_MCU_FRAME_HEADER_SIZE],
    const ag32_mcu_request_t *request)
{
    if (header == NULL || request == NULL
        || request->payload_size > AG32_MCU_MAX_PAYLOAD_SIZE
        || request->response_capacity > AG32_MCU_MAX_PAYLOAD_SIZE) {
        return false;
    }
    memset(header, 0, AG32_MCU_FRAME_HEADER_SIZE);
    ag32_mcu_write_le32(header + 0u, AG32_MCU_REQUEST_MAGIC);
    header[4] = AG32_MCU_PROTOCOL_VERSION;
    header[5] = request->opcode;
    ag32_mcu_write_le16(header + 6u, request->flags);
    ag32_mcu_write_le32(header + 8u, request->sequence);
    ag32_mcu_write_le32(header + 12u, request->payload_size);
    ag32_mcu_write_le32(header + 16u, request->response_capacity);
    ag32_mcu_write_le32(header + 20u, request->timeout_ms);
    ag32_mcu_write_le32(header + 24u, request->payload_crc32);
    ag32_mcu_write_le32(header + 28u, ag32_mcu_crc32(header, 28u));
    return true;
}

static inline ag32_mcu_status_t ag32_mcu_decode_request(
    const uint8_t *frame,
    size_t frame_size,
    ag32_mcu_request_t *request)
{
    if (frame == NULL || request == NULL || frame_size < AG32_MCU_FRAME_HEADER_SIZE) {
        return AG32_MCU_STATUS_BAD_LENGTH;
    }
    if (ag32_mcu_read_le32(frame) != AG32_MCU_REQUEST_MAGIC) {
        return AG32_MCU_STATUS_BAD_MAGIC;
    }
    if (frame[4] != AG32_MCU_PROTOCOL_VERSION) {
        return AG32_MCU_STATUS_BAD_VERSION;
    }
    if (ag32_mcu_read_le32(frame + 28u) != ag32_mcu_crc32(frame, 28u)) {
        return AG32_MCU_STATUS_BAD_HEADER_CRC;
    }
    request->opcode = frame[5];
    request->flags = ag32_mcu_read_le16(frame + 6u);
    request->sequence = ag32_mcu_read_le32(frame + 8u);
    request->payload_size = ag32_mcu_read_le32(frame + 12u);
    request->response_capacity = ag32_mcu_read_le32(frame + 16u);
    request->timeout_ms = ag32_mcu_read_le32(frame + 20u);
    request->payload_crc32 = ag32_mcu_read_le32(frame + 24u);
    if (request->payload_size > AG32_MCU_MAX_PAYLOAD_SIZE
        || request->response_capacity > AG32_MCU_MAX_PAYLOAD_SIZE
        || ag32_mcu_frame_wire_size(request->payload_size) != frame_size) {
        return AG32_MCU_STATUS_BAD_LENGTH;
    }
    if (request->payload_crc32
        != ag32_mcu_crc32(frame + AG32_MCU_FRAME_HEADER_SIZE, request->payload_size)) {
        return AG32_MCU_STATUS_BAD_PAYLOAD_CRC;
    }
    return AG32_MCU_STATUS_OK;
}

static inline bool ag32_mcu_encode_response(
    uint8_t header[AG32_MCU_FRAME_HEADER_SIZE],
    const ag32_mcu_response_t *response)
{
    if (header == NULL || response == NULL
        || response->payload_size > AG32_MCU_MAX_PAYLOAD_SIZE) {
        return false;
    }
    memset(header, 0, AG32_MCU_FRAME_HEADER_SIZE);
    ag32_mcu_write_le32(header + 0u, AG32_MCU_RESPONSE_MAGIC);
    header[4] = AG32_MCU_PROTOCOL_VERSION;
    header[5] = response->status;
    header[6] = response->opcode;
    header[7] = response->flags;
    ag32_mcu_write_le32(header + 8u, response->sequence);
    ag32_mcu_write_le32(header + 12u, response->payload_size);
    ag32_mcu_write_le32(header + 16u, response->result);
    ag32_mcu_write_le32(header + 20u, response->payload_crc32);
    ag32_mcu_write_le32(header + 28u, ag32_mcu_crc32(header, 28u));
    return true;
}

static inline ag32_mcu_status_t ag32_mcu_decode_response(
    const uint8_t *frame,
    size_t frame_size,
    ag32_mcu_response_t *response)
{
    if (frame == NULL || response == NULL || frame_size < AG32_MCU_FRAME_HEADER_SIZE) {
        return AG32_MCU_STATUS_BAD_LENGTH;
    }
    if (ag32_mcu_read_le32(frame) != AG32_MCU_RESPONSE_MAGIC) {
        return AG32_MCU_STATUS_BAD_MAGIC;
    }
    if (frame[4] != AG32_MCU_PROTOCOL_VERSION) {
        return AG32_MCU_STATUS_BAD_VERSION;
    }
    if (ag32_mcu_read_le32(frame + 28u) != ag32_mcu_crc32(frame, 28u)) {
        return AG32_MCU_STATUS_BAD_HEADER_CRC;
    }
    response->status = frame[5];
    response->opcode = frame[6];
    response->flags = frame[7];
    response->sequence = ag32_mcu_read_le32(frame + 8u);
    response->payload_size = ag32_mcu_read_le32(frame + 12u);
    response->result = ag32_mcu_read_le32(frame + 16u);
    response->payload_crc32 = ag32_mcu_read_le32(frame + 20u);
    if (response->payload_size > AG32_MCU_MAX_PAYLOAD_SIZE
        || ag32_mcu_frame_wire_size(response->payload_size) > frame_size) {
        return AG32_MCU_STATUS_BAD_LENGTH;
    }
    if (response->payload_crc32
        != ag32_mcu_crc32(frame + AG32_MCU_FRAME_HEADER_SIZE, response->payload_size)) {
        return AG32_MCU_STATUS_BAD_PAYLOAD_CRC;
    }
    return AG32_MCU_STATUS_OK;
}

#endif

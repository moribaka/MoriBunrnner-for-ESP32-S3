#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ag32_mcu_protocol.h"

static void test_request(void)
{
    uint8_t frame[AG32_MCU_FRAME_HEADER_SIZE + 8u] = {0};
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    ag32_mcu_request_t encoded = {
        .opcode = AG32_MCU_CMD_ROM_READ,
        .flags = 0x1234u,
        .sequence = 0x89abcdefu,
        .payload_size = sizeof(payload),
        .response_capacity = 4096u,
        .timeout_ms = 2000u,
        .payload_crc32 = ag32_mcu_crc32(payload, sizeof(payload)),
    };
    ag32_mcu_request_t decoded = {0};
    assert(ag32_mcu_encode_request(frame, &encoded));
    memcpy(frame + AG32_MCU_FRAME_HEADER_SIZE, payload, sizeof(payload));
    assert(ag32_mcu_decode_request(frame, sizeof(frame), &decoded) == AG32_MCU_STATUS_OK);
    assert(decoded.opcode == encoded.opcode);
    assert(decoded.flags == encoded.flags);
    assert(decoded.sequence == encoded.sequence);
    assert(decoded.payload_size == encoded.payload_size);
    assert(decoded.response_capacity == encoded.response_capacity);
    assert(decoded.timeout_ms == encoded.timeout_ms);
    assert(decoded.payload_crc32 == encoded.payload_crc32);

    frame[4]++;
    assert(ag32_mcu_decode_request(frame, sizeof(frame), &decoded) == AG32_MCU_STATUS_BAD_VERSION);
    frame[4]--;

    frame[8] ^= 1u;
    assert(ag32_mcu_decode_request(frame, sizeof(frame), &decoded) == AG32_MCU_STATUS_BAD_HEADER_CRC);
    frame[8] ^= 1u;

    frame[AG32_MCU_FRAME_HEADER_SIZE + 2u] ^= 1u;
    assert(ag32_mcu_decode_request(frame, sizeof(frame), &decoded) == AG32_MCU_STATUS_BAD_PAYLOAD_CRC);
    frame[AG32_MCU_FRAME_HEADER_SIZE + 2u] ^= 1u;

    assert(ag32_mcu_decode_request(frame, sizeof(frame) - 4u, &decoded) == AG32_MCU_STATUS_BAD_LENGTH);
    assert(ag32_mcu_decode_request(frame, sizeof(frame) + 4u, &decoded) == AG32_MCU_STATUS_BAD_LENGTH);
}

static void test_response(void)
{
    uint8_t frame[AG32_MCU_FRAME_HEADER_SIZE + 16u] = {0};
    const uint8_t payload[] = {0xaa, 0x55, 0x12, 0x34};
    ag32_mcu_response_t encoded = {
        .status = AG32_MCU_STATUS_OK,
        .opcode = AG32_MCU_CMD_PING,
        .flags = 0x5au,
        .sequence = 17u,
        .payload_size = sizeof(payload),
        .result = AG32_MCU_CAP_RAW_BACON | AG32_MCU_CAP_GBA_ROM,
        .payload_crc32 = ag32_mcu_crc32(payload, sizeof(payload)),
    };
    ag32_mcu_response_t decoded = {0};
    assert(ag32_mcu_encode_response(frame, &encoded));
    memcpy(frame + AG32_MCU_FRAME_HEADER_SIZE, payload, sizeof(payload));
    assert(ag32_mcu_decode_response(frame, sizeof(frame), &decoded) == AG32_MCU_STATUS_OK);
    assert(decoded.status == encoded.status);
    assert(decoded.opcode == encoded.opcode);
    assert(decoded.flags == encoded.flags);
    assert(decoded.sequence == encoded.sequence);
    assert(decoded.payload_size == encoded.payload_size);
    assert(decoded.result == encoded.result);
    assert(decoded.payload_crc32 == encoded.payload_crc32);

    assert(ag32_mcu_decode_response(
        frame, AG32_MCU_FRAME_HEADER_SIZE + 3u, &decoded) == AG32_MCU_STATUS_BAD_LENGTH);

    frame[AG32_MCU_FRAME_HEADER_SIZE + 1u] ^= 1u;
    assert(ag32_mcu_decode_response(frame, sizeof(frame), &decoded)
        == AG32_MCU_STATUS_BAD_PAYLOAD_CRC);
    frame[AG32_MCU_FRAME_HEADER_SIZE + 1u] ^= 1u;

    frame[0] = 0u;
    assert(ag32_mcu_decode_response(frame, sizeof(frame), &decoded) == AG32_MCU_STATUS_BAD_MAGIC);
}

int main(void)
{
    assert(ag32_mcu_crc32(NULL, 0) == 0);
    assert(ag32_mcu_crc32("123456789", 9) == 0xcbf43926u);
    uint8_t bulk[8192];
    for (size_t i = 0; i < sizeof(bulk); ++i) bulk[i] = (uint8_t)(i * 73 + i / 7);
    assert(ag32_mcu_crc32(bulk, sizeof(bulk)) == 0x7838d009u); /* Python zlib oracle */
    assert(ag32_mcu_frame_wire_size(0u) == 32u);
    assert(ag32_mcu_frame_wire_size(1u) == 36u);
    assert(ag32_mcu_frame_wire_size(4u) == 36u);
    assert(ag32_mcu_frame_wire_size(8192u) == 8224u);
    assert(strlen(AG32_MCU_MODE_ENTER_MAGIC) == AG32_MCU_MODE_MAGIC_SIZE);
    assert(strlen(AG32_MCU_MODE_EXIT_MAGIC) == AG32_MCU_MODE_MAGIC_SIZE);
    test_request();
    test_response();
    puts("AG32 MCU protocol tests passed");
    return 0;
}

#pragma once

#include <stdint.h>

/* Independent ROM-playback stream; Bacon programming opcodes are unchanged. */
#define ENTITY_CART_MAGIC 0x54524143u /* CART, little endian */
#define ENTITY_CART_VERSION 1u

typedef enum {
    ENTITY_CART_OPEN = 1,
    ENTITY_CART_ROM_BANK = 2,
    ENTITY_CART_READ = 3,
    ENTITY_CART_READ_DATA = 4,
    ENTITY_CART_CLOSE = 5,
} entity_cart_opcode_t;

typedef enum {
    ENTITY_CART_OK = 0,
    ENTITY_CART_ERR_MAPPER_MISMATCH = 1,
    ENTITY_CART_ERR_BANK_READ_TIMEOUT = 2,
    ENTITY_CART_ERR_CACHE_CRC = 3,
    ENTITY_CART_ERR_VOLTAGE_UNAVAILABLE = 4,
    ENTITY_CART_ERR_UNSUPPORTED_MAPPER = 5,
} entity_cart_status_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t opcode;
    uint16_t flags;
    uint32_t request_id;
    uint32_t bank;
    uint32_t offset;
    uint32_t length;
} entity_cart_frame_t;


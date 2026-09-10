#ifndef BACON_CPLD_PROTOCOL_H
#define BACON_CPLD_PROTOCOL_H

#include "ag32_mcu_protocol.h"

#define BACON_CPLD_ENTER_MAGIC "MORI2CPL"
#define BACON_CPLD_EXIT_MAGIC "MORI2LEG"
#define BACON_CPLD_DESCRIPTOR_MAGIC 0x31435342u /* BSC1 */
#define BACON_CPLD_DESCRIPTOR_BYTES 20u
#define BACON_CPLD_BLOCK_BYTES 1024u
#define BACON_CPLD_READ_BYTES 1020u /* Four bytes of each physical buffer hold CRC. */
#define BACON_CPLD_STATUS_BYTES 16u
#define BACON_CPLD_READ_MIN_BYTES 2048u

enum {
    BACON_CPLD_GBA_READ = 1,
    BACON_CPLD_GBA_WRITE = 2,
    BACON_CPLD_GBA_PROGRAM = 3,
    BACON_CPLD_GB_READ = 4,
    BACON_CPLD_GB_WRITE = 5,
    BACON_CPLD_GB_PROGRAM = 6,
};
enum {
    BACON_CPLD_MODE = 1u,
    BACON_CPLD_CONFIGURED = 2u,
    BACON_CPLD_DONE = 4u,
    BACON_CPLD_ERROR = 8u,
    BACON_CPLD_RX_READY = 16u,
    BACON_CPLD_TX_READY = 32u,
    BACON_CPLD_BUSY = 64u,
};

static inline void bacon_cpld_descriptor(uint8_t *out, uint8_t op,
    uint32_t byte_address, uint32_t size, uint16_t page_bytes)
{
    memset(out, 0, BACON_CPLD_DESCRIPTOR_BYTES);
    ag32_mcu_write_le32(out, BACON_CPLD_DESCRIPTOR_MAGIC);
    out[4] = op;
    ag32_mcu_write_le16(out + 6, page_bytes ? page_bytes - 1u : 0u);
    ag32_mcu_write_le32(out + 8, byte_address);
    ag32_mcu_write_le32(out + 12, size);
    ag32_mcu_write_le32(out + 16, ag32_mcu_crc32(out, 16));
}

#endif

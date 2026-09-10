#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "bacon_cpld_protocol.h"
int main(void)
{
    uint8_t wire[BACON_CPLD_DESCRIPTOR_BYTES];
    const uint8_t expected[16] = {
        'B','S','C','1',3,0,0xff,3,0x34,0x12,0,0,0,0,1,0
    };
    bacon_cpld_descriptor(wire,BACON_CPLD_GBA_PROGRAM,0x1234,65536,1024);
    assert(!memcmp(wire,expected,sizeof(expected)));
    assert(ag32_mcu_read_le32(wire+16) == ag32_mcu_crc32(expected,sizeof(expected)));
    assert(BACON_CPLD_READ_BYTES+4 == BACON_CPLD_BLOCK_BYTES);
    assert(ag32_mcu_crc32("123456789",9) == 0xcbf43926u);
    puts("CPLD descriptor byte order and CRC passed");
    return 0;
}

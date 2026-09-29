#ifndef BURNER_GBABF_PROBE_H
#define BURNER_GBABF_PROBE_H
#include <stdbool.h>
#include <stdint.h>
/* Detection constants recovered from GBABF V60B3cn (shn, GBABF/Burn2slot).
 * Addresses are Bacon halfwords, converted from NDS byte offsets.
 * These tables intentionally contain NO program or erase commands. */
static const uint32_t gbabf_unlock_words[3][2] = {
    {0x555u,0x2AAu}, {0xAAAu,0x555u}, {0x5555u,0x2AAAu}
};
/* unlock1, unlock2, ID, reset, CFI */
static const uint16_t gbabf_probe_commands[4][5] = {
    {0xAA,0x55,0x90,0xF0,0x98},
    {0xA9,0x56,0x90,0xF0,0x98},
    {0xAAA9,0x5556,0x9090,0xF0F0,0x9898},
    {0xAAAA,0x5555,0x9090,0xF0F0,0x9898}
};
typedef struct {
    bool active, amd_valid;
    uint8_t address_index, command_index;
} burner_gbabf_probe_state_t;
#endif

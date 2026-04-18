#ifndef MCU_DEBUG_H
#define MCU_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * SWD debug path switch.
 * 0: disabled (current hardware uses GPIO1 as SPI CS1 for dual-CS Bacon mode)
 * 1: enable SWD probe implementation
 */
#ifndef MORI_SWD_ENABLE
#define MORI_SWD_ENABLE 0
#endif

typedef enum {
    MCU_DEBUG_PROBE_OK = 0,
    MCU_DEBUG_PROBE_ACK_WAIT,
    MCU_DEBUG_PROBE_ACK_FAULT,
    MCU_DEBUG_PROBE_ACK_PROTOCOL,
    MCU_DEBUG_PROBE_PARITY_ERROR,
    MCU_DEBUG_PROBE_TIMEOUT,
    MCU_DEBUG_PROBE_IO_ERROR,
} mcu_debug_probe_status_t;

typedef struct {
    mcu_debug_probe_status_t status;
    uint8_t ack;
    uint32_t idcode;
    bool parity_ok;
    uint8_t seq_used;
    uint8_t attempt_count;
} mcu_debug_probe_result_t;

typedef enum {
    MCU_DEBUG_SEQ_AUTO = 0,
    MCU_DEBUG_SEQ_STD,
    MCU_DEBUG_SEQ_REV,
    MCU_DEBUG_SEQ_NONE,
} mcu_debug_seq_mode_t;

typedef enum {
    MCU_DEBUG_PULL_UP = 0,
    MCU_DEBUG_PULL_DOWN,
    MCU_DEBUG_PULL_NONE,
} mcu_debug_pull_mode_t;

typedef struct {
    bool do_reset;
    uint32_t delay_us;
    mcu_debug_seq_mode_t seq_mode;
    bool swap_clk_dio;
    uint8_t turnaround_cycles;
    mcu_debug_pull_mode_t swdio_pull_mode;
} mcu_debug_probe_options_t;

esp_err_t mcu_debug_init(void);
void mcu_debug_get_default_probe_options(mcu_debug_probe_options_t *out);
esp_err_t mcu_debug_probe_idcode(mcu_debug_probe_result_t *out);
esp_err_t mcu_debug_probe_idcode_with_opts(
    const mcu_debug_probe_options_t *opts,
    mcu_debug_probe_result_t *out);
const char *mcu_debug_probe_status_str(mcu_debug_probe_status_t status);
const char *mcu_debug_seq_mode_str(mcu_debug_seq_mode_t mode);
const char *mcu_debug_pull_mode_str(mcu_debug_pull_mode_t mode);

#endif /* MCU_DEBUG_H */

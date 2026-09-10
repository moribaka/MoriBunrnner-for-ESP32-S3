#ifndef AG32_MCU_TRANSPORT_H
#define AG32_MCU_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AG32_LINK_PREFERENCE_AUTO = 0,
    AG32_LINK_PREFERENCE_LEGACY,
    AG32_LINK_PREFERENCE_MCU,
    AG32_LINK_PREFERENCE_CPLD,
} ag32_link_preference_t;

typedef enum {
    AG32_LINK_ACTIVE_UNKNOWN = 0,
    AG32_LINK_ACTIVE_LEGACY,
    AG32_LINK_ACTIVE_MCU,
    AG32_LINK_ACTIVE_CPLD,
} ag32_link_active_t;

void ag32_mcu_link_set_preference(ag32_link_preference_t preference);
ag32_link_preference_t ag32_mcu_link_get_preference(void);
ag32_link_active_t ag32_mcu_link_get_active(void);
bool ag32_mcu_link_parse_preference(
    const char *text,
    ag32_link_preference_t *preference_out);
const char *ag32_mcu_link_preference_name(ag32_link_preference_t preference);
const char *ag32_mcu_link_active_name(ag32_link_active_t active);
uint32_t ag32_mcu_link_capabilities(void);
bool ag32_mcu_link_capabilities_known(void);
void ag32_mcu_link_invalidate(void);
void ag32_link_mark_cpld_active(void);

esp_err_t ag32_mcu_try_command_locked(
    uint32_t required_capability,
    uint8_t opcode,
    uint16_t flags,
    const void *request_payload,
    size_t request_size,
    void *response_payload,
    size_t response_capacity,
    size_t *response_size,
    uint32_t timeout_ms,
    uint32_t *result,
    bool *used_mcu);

esp_err_t ag32_mcu_try_read_locked(
    uint32_t capability,
    uint8_t opcode,
    uint32_t address,
    void *data,
    size_t size,
    bool *used_mcu);
esp_err_t ag32_mcu_try_stream_locked(
    uint32_t capability, uint8_t opcode, uint32_t address,
    uint16_t buffer_bytes, void *data, size_t size, uint32_t timeout_ms,
    bool *used_mcu);
esp_err_t ag32_mcu_try_write_locked(
    uint32_t capability,
    uint8_t opcode,
    uint32_t address,
    const void *data,
    size_t size,
    bool *used_mcu);
esp_err_t ag32_mcu_try_program_locked(
    uint8_t opcode,
    uint32_t address,
    uint16_t buffer_bytes,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    bool *used_mcu);

#endif

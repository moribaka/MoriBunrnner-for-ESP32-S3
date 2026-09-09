#include "ag32_mcu_transport.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ag32_mcu_protocol.h"
#include "burner/core/ws_server_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AG32_MCU_TAG "ag32_mcu_link"
#define AG32_MCU_STATUS_PREFIX0 0xa7u
#define AG32_MCU_STATUS_PREFIX1 0x32u
#define AG32_MCU_STATUS_VERSION 0x01u
#define AG32_MCU_STATUS_MODE (1u << 0)
#define AG32_MCU_STATUS_RESPONSE_READY (1u << 1)
#define AG32_MCU_STATUS_ERROR (1u << 3)
#define AG32_MCU_NEGOTIATE_TIMEOUT_MS 100u
#define AG32_MCU_DEFAULT_TIMEOUT_MS 2000u

static ag32_link_preference_t s_preference = AG32_LINK_PREFERENCE_AUTO;
static ag32_link_active_t s_active = AG32_LINK_ACTIVE_UNKNOWN;
static uint32_t s_capabilities;
static uint32_t s_sequence;
static bool s_capabilities_known;

static esp_err_t transfer_no_cs(const void *tx, void *rx, size_t size)
{
    burner_spi_release_cs();
    esp_rom_delay_us(2u);
    esp_err_t err = burner_spi_transfer_active(tx, rx, size);
    burner_spi_release_cs();
    esp_rom_delay_us(2u);
    return err;
}

static esp_err_t read_transport_status(uint8_t *flags)
{
    static const uint8_t safe_poll[4] = {0x07u, 0u, 0u, 0u};
    uint8_t response[sizeof(safe_poll)] = {0};
    esp_err_t err = burner_spi_transfer_cs(
        BURNER_SPI_CS_MODE_2, safe_poll, response, sizeof(response));
    if (err != ESP_OK) return err;
    if (response[0] != AG32_MCU_STATUS_PREFIX0
        || response[1] != AG32_MCU_STATUS_PREFIX1
        || response[2] != AG32_MCU_STATUS_VERSION) {
        return ESP_ERR_NOT_FOUND;
    }
    if (flags != NULL) *flags = response[3];
    return (response[3] & AG32_MCU_STATUS_MODE) != 0u
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t enter_mcu_mode(void)
{
    static const uint8_t release_bus = 0x0fu;
    static const uint8_t enter_magic[AG32_MCU_MODE_MAGIC_SIZE] = AG32_MCU_MODE_ENTER_MAGIC;
    esp_err_t err = burner_spi_transfer_cs(
        BURNER_SPI_CS_MODE_0, &release_bus, NULL, sizeof(release_bus));
    if (err == ESP_OK) err = transfer_no_cs(enter_magic, NULL, sizeof(enter_magic));
    if (err == ESP_OK) {
        esp_rom_delay_us(50u);
        err = read_transport_status(NULL);
    }
    return err;
}

static esp_err_t exit_mcu_mode(void)
{
    static const uint8_t exit_magic[AG32_MCU_MODE_MAGIC_SIZE] = AG32_MCU_MODE_EXIT_MAGIC;
    return transfer_no_cs(exit_magic, NULL, sizeof(exit_magic));
}

static esp_err_t wait_response_ready(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    do {
        uint8_t flags = 0u;
        esp_err_t err = read_transport_status(&flags);
        if (err != ESP_OK) return err;
        if ((flags & AG32_MCU_STATUS_ERROR) != 0u) return ESP_ERR_INVALID_RESPONSE;
        if ((flags & AG32_MCU_STATUS_RESPONSE_READY) != 0u) return ESP_OK;
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t status_to_esp(uint8_t status)
{
    switch ((ag32_mcu_status_t)status) {
    case AG32_MCU_STATUS_OK: return ESP_OK;
    case AG32_MCU_STATUS_BAD_LENGTH: return ESP_ERR_INVALID_SIZE;
    case AG32_MCU_STATUS_UNSUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    case AG32_MCU_STATUS_INVALID_ARGUMENT: return ESP_ERR_INVALID_ARG;
    case AG32_MCU_STATUS_TIMEOUT: return ESP_ERR_TIMEOUT;
    case AG32_MCU_STATUS_VERIFY_FAILED:
    case AG32_MCU_STATUS_BAD_HEADER_CRC:
    case AG32_MCU_STATUS_BAD_PAYLOAD_CRC: return ESP_ERR_INVALID_CRC;
    default: return ESP_ERR_INVALID_RESPONSE;
    }
}

static esp_err_t command_in_active_mode(
    uint8_t opcode,
    uint16_t flags,
    const void *request_payload,
    size_t request_size,
    void *response_payload,
    size_t response_capacity,
    size_t *response_size,
    uint32_t timeout_ms,
    uint32_t *result)
{
    uint8_t *request_frame = NULL;
    uint8_t *response_wire = NULL;
    size_t request_wire_size;
    size_t response_wire_size;
    esp_err_t err = ESP_OK;
    ag32_mcu_request_t request = {0};
    ag32_mcu_response_t response = {0};

    if (request_size > AG32_MCU_MAX_PAYLOAD_SIZE
        || response_capacity > AG32_MCU_MAX_PAYLOAD_SIZE
        || (request_size != 0u && request_payload == NULL)
        || (response_capacity != 0u && response_payload == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    request_wire_size = ag32_mcu_frame_wire_size((uint32_t)request_size);
    response_wire_size = 4u + ag32_mcu_frame_wire_size((uint32_t)response_capacity);
    request_frame = calloc(1u, request_wire_size);
    response_wire = calloc(1u, response_wire_size);
    if (request_frame == NULL || response_wire == NULL) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    if (++s_sequence == 0u) ++s_sequence;
    request.opcode = opcode;
    request.flags = flags;
    request.sequence = s_sequence;
    request.payload_size = (uint32_t)request_size;
    request.response_capacity = (uint32_t)response_capacity;
    request.timeout_ms = timeout_ms;
    request.payload_crc32 = ag32_mcu_crc32(request_payload, request_size);
    if (!ag32_mcu_encode_request(request_frame, &request)) {
        err = ESP_ERR_INVALID_ARG;
        goto out;
    }
    if (request_size != 0u)
        memcpy(request_frame + AG32_MCU_FRAME_HEADER_SIZE, request_payload, request_size);

    err = burner_spi_transfer_cs(
        BURNER_SPI_CS_MODE_0, request_frame, NULL, request_wire_size);
    if (err != ESP_OK) goto out;
    err = wait_response_ready(timeout_ms != 0u ? timeout_ms : AG32_MCU_DEFAULT_TIMEOUT_MS);
    if (err != ESP_OK) goto out;
    err = burner_spi_transfer_cs(
        BURNER_SPI_CS_MODE_1, response_wire, response_wire, response_wire_size);
    if (err != ESP_OK) goto out;
    err = status_to_esp(ag32_mcu_decode_response(
        response_wire + 4u, response_wire_size - 4u, &response));
    if (err != ESP_OK) goto out;
    if (response.sequence != request.sequence || response.opcode != request.opcode) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    err = status_to_esp(response.status);
    if (err != ESP_OK) goto out;
    if (response.payload_size > response_capacity) {
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    if (response.payload_size != 0u)
        memcpy(response_payload,
            response_wire + 4u + AG32_MCU_FRAME_HEADER_SIZE,
            response.payload_size);
    if (response_size != NULL) *response_size = response.payload_size;
    if (result != NULL) *result = response.result;

out:
    free(response_wire);
    free(request_frame);
    return err;
}

static esp_err_t negotiate_capabilities(void)
{
    uint8_t ping[16] = {0};
    size_t ping_size = 0u;
    uint32_t capabilities = 0u;
    esp_err_t err = enter_mcu_mode();
    if (err == ESP_OK) {
        s_active = AG32_LINK_ACTIVE_MCU;
        err = command_in_active_mode(
            AG32_MCU_CMD_PING, 0u, NULL, 0u,
            ping, sizeof(ping), &ping_size,
            AG32_MCU_NEGOTIATE_TIMEOUT_MS, &capabilities);
    }
    esp_err_t exit_err = exit_mcu_mode();
    if (err == ESP_OK) err = exit_err;
    s_capabilities_known = true;
    if (err == ESP_OK && ping_size == sizeof(ping)
        && ag32_mcu_read_le32(ping) == AG32_MCU_PROTOCOL_VERSION
        && ag32_mcu_read_le32(ping + 4u) == AG32_MCU_MAX_PAYLOAD_SIZE) {
        s_capabilities = capabilities;
        ESP_LOGI(AG32_MCU_TAG, "MCU protocol v%u capabilities=0x%08" PRIx32,
            AG32_MCU_PROTOCOL_VERSION, capabilities);
        return ESP_OK;
    }
    s_capabilities = 0u;
    ESP_LOGW(AG32_MCU_TAG, "MCU protocol unavailable: %s", esp_err_to_name(err));
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
}

void ag32_mcu_link_set_preference(ag32_link_preference_t preference)
{
    if ((unsigned)preference > AG32_LINK_PREFERENCE_MCU) return;
    if (s_preference != preference) {
        s_preference = preference;
        ag32_mcu_link_invalidate();
    }
}

ag32_link_preference_t ag32_mcu_link_get_preference(void) { return s_preference; }
ag32_link_active_t ag32_mcu_link_get_active(void) { return s_active; }
uint32_t ag32_mcu_link_capabilities(void) { return s_capabilities; }
bool ag32_mcu_link_capabilities_known(void) { return s_capabilities_known; }

bool ag32_mcu_link_parse_preference(
    const char *text,
    ag32_link_preference_t *preference_out)
{
    if (text == NULL || preference_out == NULL) return false;
    if (strcasecmp(text, "auto") == 0) {
        *preference_out = AG32_LINK_PREFERENCE_AUTO;
        return true;
    }
    if (strcasecmp(text, "legacy") == 0) {
        *preference_out = AG32_LINK_PREFERENCE_LEGACY;
        return true;
    }
    if (strcasecmp(text, "mcu") == 0) {
        *preference_out = AG32_LINK_PREFERENCE_MCU;
        return true;
    }
    return false;
}

const char *ag32_mcu_link_preference_name(ag32_link_preference_t preference)
{
    static const char *const names[] = {"auto", "legacy", "mcu"};
    return (unsigned)preference < sizeof(names) / sizeof(names[0])
        ? names[preference] : "unknown";
}

const char *ag32_mcu_link_active_name(ag32_link_active_t active)
{
    static const char *const names[] = {"unknown", "legacy", "mcu"};
    return (unsigned)active < sizeof(names) / sizeof(names[0])
        ? names[active] : "unknown";
}

void ag32_mcu_link_invalidate(void)
{
    s_active = AG32_LINK_ACTIVE_UNKNOWN;
    s_capabilities = 0u;
    s_capabilities_known = false;
}

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
    bool *used_mcu)
{
    esp_err_t err;
    if (used_mcu == NULL) return ESP_ERR_INVALID_ARG;
    *used_mcu = false;
    if (response_size != NULL) *response_size = 0u;
    if (result != NULL) *result = 0u;
    if (s_preference == AG32_LINK_PREFERENCE_LEGACY) {
        s_active = AG32_LINK_ACTIVE_LEGACY;
        return ESP_OK;
    }
    if (s_preference == AG32_LINK_PREFERENCE_AUTO
        && s_capabilities_known
        && s_active == AG32_LINK_ACTIVE_LEGACY) {
        return ESP_OK;
    }
    if (!s_capabilities_known) {
        err = negotiate_capabilities();
        if (err != ESP_OK) {
            if (s_preference == AG32_LINK_PREFERENCE_AUTO) {
                s_active = AG32_LINK_ACTIVE_LEGACY;
                return ESP_OK;
            }
            *used_mcu = true;
            return err;
        }
    }
    if ((s_capabilities & required_capability) != required_capability) {
        *used_mcu = true;
        return ESP_ERR_NOT_SUPPORTED;
    }

    *used_mcu = true;
    err = enter_mcu_mode();
    if (err == ESP_OK) {
        s_active = AG32_LINK_ACTIVE_MCU;
        err = command_in_active_mode(opcode, flags,
            request_payload, request_size,
            response_payload, response_capacity, response_size,
            timeout_ms, result);
    }
    esp_err_t exit_err = exit_mcu_mode();
    if (exit_err != ESP_OK) {
        s_active = AG32_LINK_ACTIVE_UNKNOWN;
        if (err == ESP_OK) err = exit_err;
    }
    return err;
}

esp_err_t ag32_mcu_try_read_locked(
    uint32_t capability,
    uint8_t opcode,
    uint32_t address,
    void *data,
    size_t size,
    bool *used_mcu)
{
    if (used_mcu == NULL) return ESP_ERR_INVALID_ARG;
    *used_mcu = false;
    if (data == NULL || size == 0u) {
        if (s_preference == AG32_LINK_PREFERENCE_MCU) *used_mcu = true;
        return ESP_ERR_INVALID_ARG;
    }
    size_t copied = 0u;
    while (copied < size) {
        uint8_t request[6];
        size_t chunk = size - copied;
        size_t response_size = 0u;
        bool one_used = false;
        if (chunk > AG32_MCU_MAX_PAYLOAD_SIZE) chunk = AG32_MCU_MAX_PAYLOAD_SIZE;
        ag32_mcu_write_le32(request, address);
        ag32_mcu_write_le16(request + 4u, (uint16_t)chunk);
        esp_err_t err = ag32_mcu_try_command_locked(
            capability, opcode, 0u,
            request, sizeof(request), (uint8_t *)data + copied, chunk, &response_size,
            AG32_MCU_DEFAULT_TIMEOUT_MS, NULL, &one_used);
        if (!one_used) return ESP_OK;
        *used_mcu = true;
        if (err != ESP_OK) return err;
        if (response_size != chunk) return ESP_ERR_INVALID_SIZE;
        copied += chunk;
        address += opcode == AG32_MCU_CMD_ROM_READ ? (uint32_t)(chunk / 2u) : (uint32_t)chunk;
    }
    return ESP_OK;
}

esp_err_t ag32_mcu_try_write_locked(
    uint32_t capability,
    uint8_t opcode,
    uint32_t address,
    const void *data,
    size_t size,
    bool *used_mcu)
{
    if (used_mcu == NULL) return ESP_ERR_INVALID_ARG;
    *used_mcu = false;
    if (data == NULL || size == 0u) {
        if (s_preference == AG32_LINK_PREFERENCE_MCU) *used_mcu = true;
        return ESP_ERR_INVALID_ARG;
    }
    size_t written = 0u;
    while (written < size) {
        size_t chunk = size - written;
        bool one_used = false;
        uint32_t result = 0u;
        if (chunk > AG32_MCU_MAX_PAYLOAD_SIZE - 4u)
            chunk = AG32_MCU_MAX_PAYLOAD_SIZE - 4u;
        uint8_t *request = malloc(chunk + 4u);
        if (request == NULL) return ESP_ERR_NO_MEM;
        ag32_mcu_write_le32(request, address);
        memcpy(request + 4u, (const uint8_t *)data + written, chunk);
        esp_err_t err = ag32_mcu_try_command_locked(
            capability, opcode, 0u,
            request, chunk + 4u, NULL, 0u, NULL,
            AG32_MCU_DEFAULT_TIMEOUT_MS, &result, &one_used);
        free(request);
        if (!one_used) return ESP_OK;
        *used_mcu = true;
        if (err != ESP_OK) return err;
        if (result != chunk) return ESP_ERR_INVALID_SIZE;
        written += chunk;
        address += opcode == AG32_MCU_CMD_ROM_WRITE ? (uint32_t)(chunk / 2u) : (uint32_t)chunk;
    }
    return ESP_OK;
}

esp_err_t ag32_mcu_try_program_locked(
    uint8_t opcode,
    uint32_t address,
    uint16_t buffer_bytes,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    bool *used_mcu)
{
    if (used_mcu == NULL) return ESP_ERR_INVALID_ARG;
    *used_mcu = false;
    if (data == NULL || size == 0u) {
        if (s_preference == AG32_LINK_PREFERENCE_MCU) *used_mcu = true;
        return ESP_ERR_INVALID_ARG;
    }
    if (size + 6u > AG32_MCU_MAX_PAYLOAD_SIZE) {
        if (s_preference == AG32_LINK_PREFERENCE_MCU) *used_mcu = true;
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *request = malloc(size + 6u);
    if (request == NULL) return ESP_ERR_NO_MEM;
    ag32_mcu_write_le32(request, address);
    ag32_mcu_write_le16(request + 4u, buffer_bytes);
    memcpy(request + 6u, data, size);
    esp_err_t err = ag32_mcu_try_command_locked(
        AG32_MCU_CAP_AMD_PROGRAM, opcode, 0u,
        request, size + 6u, NULL, 0u, NULL,
        timeout_ms, NULL, used_mcu);
    free(request);
    return err;
}

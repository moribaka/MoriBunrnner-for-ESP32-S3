#include "board.h"
#include "ag32_mcu_protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define REG32(address) (*(volatile uint32_t *)(address))

#define MCU_REG_CTRL       0x60020000u
#define MCU_REG_RX_ADDR    0x60020004u
#define MCU_REG_RX_LIMIT   0x60020008u
#define MCU_REG_RX_WORDS   0x6002000cu
#define MCU_REG_TX_ADDR    0x60020010u
#define MCU_REG_TX_WORDS   0x60020014u
#define MCU_REG_IDENTITY   0x60020018u
#define MCU_REG_CART_OUT   0x60020100u
#define MCU_REG_CART_CTRL  0x60020104u
#define MCU_REG_CART_IN    0x60020108u

#define MCU_CTRL_ARM_RX       (1u << 0)
#define MCU_CTRL_CLEAR_RX     (1u << 1)
#define MCU_CTRL_PUBLISH_TX   (1u << 2)
#define MCU_CTRL_CLEAR_TX     (1u << 3)
#define MCU_CTRL_BUSY_SET     (1u << 4)
#define MCU_CTRL_BUSY_CLEAR   (1u << 5)
#define MCU_CTRL_ERROR_SET    (1u << 6)
#define MCU_CTRL_ERROR_CLEAR  (1u << 7)
#define MCU_STATE_MODE        (1u << 5)
#define MCU_STATE_RX_ARMED    (1u << 6)
#define MCU_STATE_RX_DONE     (1u << 7)
#define MCU_STATE_TX_READY    (1u << 8)
#define MCU_STATE_ERROR       (1u << 10)

#define CART_CTRL_WR       (1u << 0)
#define CART_CTRL_RD       (1u << 1)
#define CART_CTRL_CS1      (1u << 2)
#define CART_CTRL_CS2      (1u << 3)
#define CART_CTRL_DIR_AD   (1u << 4)
#define CART_CTRL_DIR_A    (1u << 5)
#define CART_CTRL_POWER_3V (1u << 6)
#define CART_CTRL_POWER_5V (1u << 7)
#define CART_CTRL_PHI_SHIFT 8u
#define CART_CTRL_IDLE_LINES \
    (CART_CTRL_WR | CART_CTRL_RD | CART_CTRL_CS1 | CART_CTRL_CS2)

#define CART_BUS_DELAY_CYCLES 24u
#define REQUEST_WORD_CAPACITY \
    ((AG32_MCU_FRAME_HEADER_SIZE + AG32_MCU_MAX_PAYLOAD_SIZE + 3u) / 4u)
#define RESPONSE_WORD_CAPACITY (REQUEST_WORD_CAPACITY + 1u)

static uint32_t s_request_words[REQUEST_WORD_CAPACITY] __attribute__((aligned(4)));
static uint32_t s_response_words[RESPONSE_WORD_CAPACITY] __attribute__((aligned(4)));
static uint16_t s_gba_words[AG32_MCU_MAX_PAYLOAD_SIZE / 2u] __attribute__((aligned(4)));
static uint32_t s_sequence;
static uint32_t s_cart_control = CART_CTRL_IDLE_LINES | CART_CTRL_POWER_3V;
static uint32_t s_cart_output;
static uint8_t s_stream_opcode;
static uint16_t s_stream_buffer_bytes;
static uint32_t s_stream_address, s_stream_remaining, s_stream_timeout;

static inline __attribute__((always_inline)) void cart_delay(void)
{
    // Preserve the minimum 24-cycle bus guard without repeatedly reading
    // and comparing a 64-bit mcycle counter on the RV32 hot path.
    __asm__ volatile (".rept %c0\n\tnop\n\t.endr" :: "i" (CART_BUS_DELAY_CYCLES) : "memory");
}

static void cart_write_control(uint32_t control)
{
    s_cart_control = control;
    REG32(MCU_REG_CART_CTRL) = control;
}

static void cart_write_output(uint32_t output)
{
    s_cart_output = output & 0x00ffffffu;
    REG32(MCU_REG_CART_OUT) = s_cart_output;
}

static void cart_release(void)
{
    cart_write_control((s_cart_control &
        (CART_CTRL_POWER_3V | CART_CTRL_POWER_5V | (3u << CART_CTRL_PHI_SHIFT))) |
        CART_CTRL_IDLE_LINES);
    cart_delay();
}

static void cart_power(uint8_t mode, uint8_t phi_div)
{
    uint32_t control = CART_CTRL_IDLE_LINES |
        (((uint32_t)phi_div & 3u) << CART_CTRL_PHI_SHIFT);
    if (mode == 1u) control |= CART_CTRL_POWER_3V;
    if (mode == 2u) control |= CART_CTRL_POWER_5V;
    cart_write_control(control);
}

static void gba_latch_address(uint32_t word_address, bool data_output)
{
    uint32_t control = (s_cart_control &
        (CART_CTRL_POWER_3V | CART_CTRL_POWER_5V | (3u << CART_CTRL_PHI_SHIFT))) |
        CART_CTRL_IDLE_LINES | CART_CTRL_DIR_A | CART_CTRL_DIR_AD;
    cart_write_control(control);
    cart_write_output(((word_address >> 16u) << 16u) | (word_address & 0xffffu));
    cart_delay();
    control &= ~CART_CTRL_CS1;
    cart_write_control(control);
    cart_delay();
    if (!data_output) {
        control &= ~CART_CTRL_DIR_AD;
        cart_write_control(control);
        cart_delay();
    }
}

static void gba_read_words(uint32_t word_address, uint16_t *data, uint32_t word_count)
{
    gba_latch_address(word_address, false);
    for (uint32_t i = 0u; i < word_count; ++i) {
        cart_write_control(s_cart_control & ~CART_CTRL_RD);
        cart_delay();
        data[i] = (uint16_t)REG32(MCU_REG_CART_IN);
        cart_write_control(s_cart_control | CART_CTRL_RD);
        cart_delay();
    }
    cart_release();
}

static void gba_write_words(uint32_t word_address, const uint16_t *data, uint32_t word_count)
{
    gba_latch_address(word_address, true);
    for (uint32_t i = 0u; i < word_count; ++i) {
        cart_write_output((s_cart_output & 0x00ff0000u) | data[i]);
        cart_delay();
        cart_write_control(s_cart_control & ~CART_CTRL_WR);
        cart_delay();
        cart_write_control(s_cart_control | CART_CTRL_WR);
        cart_delay();
    }
    cart_release();
}

static void gbc_begin(uint16_t address, bool write)
{
    uint32_t control = (s_cart_control &
        (CART_CTRL_POWER_3V | CART_CTRL_POWER_5V | (3u << CART_CTRL_PHI_SHIFT))) |
        CART_CTRL_IDLE_LINES | CART_CTRL_DIR_AD;
    if (write) control |= CART_CTRL_DIR_A;
    control &= ~CART_CTRL_CS1;
    cart_write_output(address);
    cart_write_control(control);
    cart_delay();
}

static void gbc_read_bytes(uint16_t address, uint8_t *data, uint32_t size)
{
    gbc_begin(address, false);
    for (uint32_t i = 0u; i < size; ++i) {
        cart_write_output((uint16_t)(address + i));
        cart_write_control(s_cart_control & ~CART_CTRL_RD);
        cart_delay();
        data[i] = (uint8_t)(REG32(MCU_REG_CART_IN) >> 16u);
        cart_write_control(s_cart_control | CART_CTRL_RD);
        cart_delay();
    }
    cart_release();
}

static void gbc_write_bytes(uint16_t address, const uint8_t *data, uint32_t size)
{
    gbc_begin(address, true);
    for (uint32_t i = 0u; i < size; ++i) {
        cart_write_output(((uint32_t)data[i] << 16u) | (uint16_t)(address + i));
        cart_delay();
        cart_write_control(s_cart_control & ~CART_CTRL_WR);
        cart_delay();
        cart_write_control(s_cart_control | CART_CTRL_WR);
        cart_delay();
    }
    cart_release();
}

static void ram_begin(uint16_t address, bool write)
{
    uint32_t control = (s_cart_control &
        (CART_CTRL_POWER_3V | CART_CTRL_POWER_5V | (3u << CART_CTRL_PHI_SHIFT))) |
        CART_CTRL_IDLE_LINES | CART_CTRL_DIR_AD;
    if (write) control |= CART_CTRL_DIR_A;
    control &= ~CART_CTRL_CS2;
    cart_write_output(address);
    cart_write_control(control);
    cart_delay();
}

static void ram_read_bytes(uint16_t address, uint8_t *data, uint32_t size)
{
    ram_begin(address, false);
    for (uint32_t i = 0u; i < size; ++i) {
        cart_write_output((uint16_t)(address + i));
        cart_write_control(s_cart_control & ~CART_CTRL_RD);
        cart_delay();
        data[i] = (uint8_t)(REG32(MCU_REG_CART_IN) >> 16u);
        cart_write_control(s_cart_control | CART_CTRL_RD);
        cart_delay();
    }
    cart_release();
}

static void ram_write_bytes(uint16_t address, const uint8_t *data, uint32_t size)
{
    ram_begin(address, true);
    for (uint32_t i = 0u; i < size; ++i) {
        cart_write_output(((uint32_t)data[i] << 16u) | (uint16_t)(address + i));
        cart_delay();
        cart_write_control(s_cart_control & ~CART_CTRL_WR);
        cart_delay();
        cart_write_control(s_cart_control | CART_CTRL_WR);
        cart_delay();
    }
    cart_release();
}

static bool gba_wait_dq7(uint32_t word_address, uint16_t expected, uint32_t timeout_ms)
{
    uint32_t start_us = UTIL_GetUSec();
    uint32_t timeout_us = timeout_ms * 1000u;
    while (1) {
        uint16_t observed;
        gba_read_words(word_address, &observed, 1u);
        if ((observed & 0x0080u) == (expected & 0x0080u)) return true;
        if (timeout_ms != 0u && (uint32_t)(UTIL_GetUSec() - start_us) >= timeout_us)
            return false;
    }
}

static bool gbc_wait_dq7(uint16_t address, uint8_t expected, uint32_t timeout_ms)
{
    uint32_t start_us = UTIL_GetUSec();
    uint32_t timeout_us = timeout_ms * 1000u;
    while (1) {
        uint8_t observed;
        gbc_read_bytes(address, &observed, 1u);
        if ((observed & 0x80u) == (expected & 0x80u)) return true;
        if (timeout_ms != 0u && (uint32_t)(UTIL_GetUSec() - start_us) >= timeout_us)
            return false;
    }
}

static ag32_mcu_status_t gba_program_amd(
    uint32_t word_address,
    const uint8_t *data,
    uint32_t size,
    uint16_t buffer_bytes,
    uint32_t timeout_ms)
{
    if ((size & 1u) != 0u || size == 0u || buffer_bytes == 1u ||
        (buffer_bytes & 1u) != 0u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
    uint32_t words = size / 2u;
    for (uint32_t i = 0u; i < words; ++i)
        s_gba_words[i] = ag32_mcu_read_le16(data + i * 2u);
    uint32_t done = 0u;
    while (done < words) {
        uint32_t chunk_words = buffer_bytes == 0u ? 1u : buffer_bytes / 2u;
        if (chunk_words > words - done) chunk_words = words - done;
        if (buffer_bytes != 0u) {
            uint32_t buffer_words = buffer_bytes / 2u;
            uint32_t boundary_words = buffer_words - ((word_address + done) % buffer_words);
            if (chunk_words > boundary_words) chunk_words = boundary_words;
        }
        uint16_t command;
        command = 0x00aau; gba_write_words(0x555u, &command, 1u);
        command = 0x0055u; gba_write_words(0x2aau, &command, 1u);
        if (buffer_bytes == 0u) {
            command = 0x00a0u; gba_write_words(0x555u, &command, 1u);
            gba_write_words(word_address + done, &s_gba_words[done], 1u);
        } else {
            command = 0x0025u; gba_write_words(word_address + done, &command, 1u);
            command = (uint16_t)(chunk_words - 1u);
            gba_write_words(word_address + done, &command, 1u);
            gba_write_words(word_address + done, &s_gba_words[done], chunk_words);
            command = 0x0029u; gba_write_words(word_address + done, &command, 1u);
        }
        if (!gba_wait_dq7(word_address + done + chunk_words - 1u,
                s_gba_words[done + chunk_words - 1u], timeout_ms))
            return AG32_MCU_STATUS_TIMEOUT;
        done += chunk_words;
    }
    return AG32_MCU_STATUS_OK;
}

static ag32_mcu_status_t gbc_program_amd(
    uint16_t address,
    const uint8_t *data,
    uint32_t size,
    uint16_t buffer_bytes,
    uint32_t timeout_ms)
{
    if (size == 0u || buffer_bytes == 1u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
    uint32_t done = 0u;
    while (done < size) {
        uint32_t chunk = buffer_bytes == 0u ? 1u : buffer_bytes;
        if (chunk > size - done) chunk = size - done;
        if (buffer_bytes != 0u) {
            uint32_t boundary = buffer_bytes - (((uint32_t)address + done) % buffer_bytes);
            if (chunk > boundary) chunk = boundary;
        }
        uint8_t command;
        command = 0xaau; gbc_write_bytes(0x0aaau, &command, 1u);
        command = 0x55u; gbc_write_bytes(0x0555u, &command, 1u);
        if (buffer_bytes == 0u) {
            command = 0xa0u; gbc_write_bytes(0x0aaau, &command, 1u);
            gbc_write_bytes((uint16_t)(address + done), data + done, 1u);
        } else {
            command = 0x25u; gbc_write_bytes((uint16_t)(address + done), &command, 1u);
            command = (uint8_t)(chunk - 1u);
            gbc_write_bytes((uint16_t)(address + done), &command, 1u);
            gbc_write_bytes((uint16_t)(address + done), data + done, chunk);
            command = 0x29u; gbc_write_bytes((uint16_t)(address + done), &command, 1u);
        }
        if (!gbc_wait_dq7((uint16_t)(address + done + chunk - 1u),
                data[done + chunk - 1u], timeout_ms))
            return AG32_MCU_STATUS_TIMEOUT;
        done += chunk;
    }
    return AG32_MCU_STATUS_OK;
}

static ag32_mcu_status_t process_command(
    const ag32_mcu_request_t *request,
    const uint8_t *payload,
    uint8_t *response_payload,
    uint32_t *response_size,
    uint32_t *result)
{
    uint32_t address;
    uint16_t size16;
    *response_size = 0u;
    *result = 0u;

    switch (request->opcode) {
    case AG32_MCU_CMD_ECHO:
        if (request->payload_size > request->response_capacity)
            return AG32_MCU_STATUS_BAD_LENGTH;
        memcpy(response_payload, payload, request->payload_size);
        *response_size = request->payload_size;
        return AG32_MCU_STATUS_OK;

    case AG32_MCU_CMD_PING:
        if (request->payload_size != 0u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
        ag32_mcu_write_le32(response_payload + 0u, AG32_MCU_PROTOCOL_VERSION);
        ag32_mcu_write_le32(response_payload + 4u, AG32_MCU_MAX_PAYLOAD_SIZE);
        ag32_mcu_write_le32(response_payload + 8u, 150000000u);
        ag32_mcu_write_le32(response_payload + 12u, REG32(MCU_REG_IDENTITY));
        *response_size = 16u;
        *result = AG32_MCU_CAP_GBA_ROM | AG32_MCU_CAP_GBA_RAM |
            AG32_MCU_CAP_GBC_ROM | AG32_MCU_CAP_GBC_RAM |
            AG32_MCU_CAP_AMD_PROGRAM | AG32_MCU_CAP_POWER | AG32_MCU_CAP_STREAM;
        return AG32_MCU_STATUS_OK;

    case AG32_MCU_CMD_STREAM_BEGIN: {
        if (request->payload_size != 12u) return AG32_MCU_STATUS_BAD_LENGTH;
        uint8_t op = payload[0];
        uint32_t address = ag32_mcu_read_le32(payload + 4);
        uint32_t total = ag32_mcu_read_le32(payload + 8);
        if (payload[1] != 0 || total == 0) return AG32_MCU_STATUS_INVALID_ARGUMENT;
        if (op != AG32_MCU_CMD_ECHO && op != AG32_MCU_CMD_ROM_READ &&
            op != AG32_MCU_CMD_ROM_WRITE && op != AG32_MCU_CMD_ROM_PROGRAM &&
            op != AG32_MCU_CMD_GBC_READ && op != AG32_MCU_CMD_GBC_WRITE &&
            op != AG32_MCU_CMD_GBC_ROM_PROGRAM && op != AG32_MCU_CMD_RAM_READ &&
            op != AG32_MCU_CMD_RAM_WRITE) return AG32_MCU_STATUS_UNSUPPORTED;
        if (op == AG32_MCU_CMD_ROM_READ || op == AG32_MCU_CMD_ROM_WRITE ||
            op == AG32_MCU_CMD_ROM_PROGRAM) {
            if ((total & 1) || address >= 0x1000000u || total / 2 > 0x1000000u - address)
                return AG32_MCU_STATUS_INVALID_ARGUMENT;
        } else if (op != AG32_MCU_CMD_ECHO &&
                   (address >= 0x10000u || total > 0x10000u - address)) {
            return AG32_MCU_STATUS_INVALID_ARGUMENT;
        }
        s_stream_opcode = op;
        s_stream_buffer_bytes = ag32_mcu_read_le16(payload + 2);
        s_stream_address = address;
        s_stream_remaining = total;
        s_stream_timeout = request->timeout_ms;
        *result = AG32_MCU_STREAM_CHUNK_SIZE;
        return AG32_MCU_STATUS_OK;
    }

    case AG32_MCU_CMD_CART_POWER:
        if (request->payload_size != 2u || payload[0] > 2u || payload[1] > 3u)
            return AG32_MCU_STATUS_INVALID_ARGUMENT;
        cart_power(payload[0], payload[1]);
        return AG32_MCU_STATUS_OK;

    case AG32_MCU_CMD_ROM_READ:
    case AG32_MCU_CMD_GBC_READ:
    case AG32_MCU_CMD_RAM_READ:
        if (request->payload_size != 6u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
        address = ag32_mcu_read_le32(payload);
        size16 = ag32_mcu_read_le16(payload + 4u);
        if (size16 == 0u || size16 > request->response_capacity ||
            size16 > AG32_MCU_MAX_PAYLOAD_SIZE)
            return AG32_MCU_STATUS_BAD_LENGTH;
        if (request->opcode == AG32_MCU_CMD_ROM_READ) {
            if ((size16 & 1u) != 0u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
            gba_read_words(address, s_gba_words, size16 / 2u);
            for (uint32_t i = 0u; i < size16 / 2u; ++i)
                ag32_mcu_write_le16(response_payload + i * 2u, s_gba_words[i]);
        } else if (request->opcode == AG32_MCU_CMD_GBC_READ) {
            gbc_read_bytes((uint16_t)address, response_payload, size16);
        } else {
            ram_read_bytes((uint16_t)address, response_payload, size16);
        }
        *response_size = size16;
        return AG32_MCU_STATUS_OK;

    case AG32_MCU_CMD_ROM_WRITE:
    case AG32_MCU_CMD_GBC_WRITE:
    case AG32_MCU_CMD_RAM_WRITE:
        if (request->payload_size <= 4u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
        address = ag32_mcu_read_le32(payload);
        if (request->opcode == AG32_MCU_CMD_ROM_WRITE) {
            uint32_t bytes = request->payload_size - 4u;
            if ((bytes & 1u) != 0u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
            for (uint32_t i = 0u; i < bytes / 2u; ++i)
                s_gba_words[i] = ag32_mcu_read_le16(payload + 4u + i * 2u);
            gba_write_words(address, s_gba_words, bytes / 2u);
        } else if (request->opcode == AG32_MCU_CMD_GBC_WRITE) {
            gbc_write_bytes((uint16_t)address, payload + 4u, request->payload_size - 4u);
        } else {
            ram_write_bytes((uint16_t)address, payload + 4u, request->payload_size - 4u);
        }
        *result = request->payload_size - 4u;
        return AG32_MCU_STATUS_OK;

    case AG32_MCU_CMD_ROM_PROGRAM:
    case AG32_MCU_CMD_GBC_ROM_PROGRAM:
        if (request->payload_size <= 6u) return AG32_MCU_STATUS_INVALID_ARGUMENT;
        address = ag32_mcu_read_le32(payload);
        size16 = ag32_mcu_read_le16(payload + 4u);
        if (request->opcode == AG32_MCU_CMD_ROM_PROGRAM)
            return gba_program_amd(address, payload + 6u,
                request->payload_size - 6u, size16, request->timeout_ms);
        return gbc_program_amd((uint16_t)address, payload + 6u,
            request->payload_size - 6u, size16, request->timeout_ms);

    default:
        return AG32_MCU_STATUS_UNSUPPORTED;
    }
}

static void publish_response(
    const ag32_mcu_request_t *request,
    ag32_mcu_status_t status,
    uint32_t payload_size,
    uint32_t result)
{
    uint8_t *frame = (uint8_t *)&s_response_words[1];
    uint8_t *payload = frame + AG32_MCU_FRAME_HEADER_SIZE;
    ag32_mcu_response_t response = {
        .status = (uint8_t)status,
        .opcode = request != NULL ? request->opcode : 0u,
        .flags = 0u,
        .sequence = request != NULL ? request->sequence : 0u,
        .payload_size = payload_size,
        .result = result,
        .payload_crc32 = ag32_mcu_crc32(payload, payload_size),
    };
    s_response_words[0] = 0u;
    ag32_mcu_encode_response(frame, &response);
    size_t wire_size = ag32_mcu_frame_wire_size(payload_size);
    if (wire_size > AG32_MCU_FRAME_HEADER_SIZE + payload_size)
        memset(frame + AG32_MCU_FRAME_HEADER_SIZE + payload_size, 0,
            wire_size - AG32_MCU_FRAME_HEADER_SIZE - payload_size);
    REG32(MCU_REG_TX_ADDR) = (uint32_t)s_response_words;
    REG32(MCU_REG_TX_WORDS) = 1u + (uint32_t)(wire_size / 4u);
    REG32(MCU_REG_CTRL) = MCU_CTRL_PUBLISH_TX | MCU_CTRL_BUSY_CLEAR;
}

static void arm_request(void)
{
    REG32(MCU_REG_RX_ADDR) = (uint32_t)s_request_words;
    REG32(MCU_REG_RX_LIMIT) = REQUEST_WORD_CAPACITY;
    REG32(MCU_REG_CTRL) = MCU_CTRL_ARM_RX | MCU_CTRL_ERROR_CLEAR;
}

static bool wait_tx_consumed(void)
{
    while ((REG32(MCU_REG_CTRL) & (MCU_STATE_TX_READY | MCU_STATE_MODE)) ==
           (MCU_STATE_TX_READY | MCU_STATE_MODE)) { }
    return (REG32(MCU_REG_CTRL) & MCU_STATE_MODE) != 0;
}

/* The descriptor defines all block lengths. Data transactions have no
 * command, address or length header; CS delimits one bounded DMA block. */
static void run_stream(void)
{
    uint8_t *input = (uint8_t *)s_request_words;
    uint8_t *output = (uint8_t *)s_response_words + 8;
    bool reading = s_stream_opcode == AG32_MCU_CMD_ROM_READ ||
        s_stream_opcode == AG32_MCU_CMD_GBC_READ || s_stream_opcode == AG32_MCU_CMD_RAM_READ;
    bool programming = s_stream_opcode == AG32_MCU_CMD_ROM_PROGRAM ||
        s_stream_opcode == AG32_MCU_CMD_GBC_ROM_PROGRAM;
    while (s_stream_remaining && (REG32(MCU_REG_CTRL) & MCU_STATE_MODE)) {
        uint32_t size = s_stream_remaining < AG32_MCU_STREAM_CHUNK_SIZE ?
            s_stream_remaining : AG32_MCU_STREAM_CHUNK_SIZE;
        uint32_t padded = (size + 3u) & ~3u;
        ag32_mcu_status_t status = AG32_MCU_STATUS_OK;
        if (!reading) {
            REG32(MCU_REG_RX_ADDR) = (uint32_t)input;
            REG32(MCU_REG_RX_LIMIT) = (padded + 4u) / 4u;
            REG32(MCU_REG_CTRL) = MCU_CTRL_ARM_RX | MCU_CTRL_ERROR_CLEAR;
            while ((REG32(MCU_REG_CTRL) & (MCU_STATE_RX_DONE | MCU_STATE_MODE)) == MCU_STATE_MODE) { }
            if (!(REG32(MCU_REG_CTRL) & MCU_STATE_MODE)) break;
            if ((REG32(MCU_REG_CTRL) & MCU_STATE_ERROR) ||
                REG32(MCU_REG_RX_WORDS) != (padded + 4u) / 4u)
                status = AG32_MCU_STATUS_BAD_LENGTH;
            else if (ag32_mcu_crc32(input, size) != ag32_mcu_read_le32(input + padded))
                status = AG32_MCU_STATUS_BAD_PAYLOAD_CRC;
            REG32(MCU_REG_CTRL) = MCU_CTRL_CLEAR_RX;
        }
        uint32_t output_size = 0, result = 0;
        if (status == AG32_MCU_STATUS_OK) {
            uint32_t prefix = programming || reading ? 6u : 4u;
            if (s_stream_opcode == AG32_MCU_CMD_ECHO) prefix = 0;
            if (!reading) memmove(input + prefix, input, size);
            if (prefix) ag32_mcu_write_le32(input, s_stream_address);
            if (prefix == 6) ag32_mcu_write_le16(input + 4, reading ? size : s_stream_buffer_bytes);
            ag32_mcu_request_t request = {
                .opcode = s_stream_opcode,
                .payload_size = reading ? 6u : prefix + size,
                .response_capacity = AG32_MCU_STREAM_CHUNK_SIZE,
                .timeout_ms = s_stream_timeout,
            };
            status = process_command(&request, input, output, &output_size, &result);
        }
        // Fixed response length remains known even on a failed block.
        uint32_t wire_payload = reading || s_stream_opcode == AG32_MCU_CMD_ECHO ? padded : 0;
        if (status != AG32_MCU_STATUS_OK) memset(output, 0, wire_payload);
        else if (wire_payload > output_size) memset(output + output_size, 0, wire_payload - output_size);
        s_response_words[0] = 0;
        s_response_words[1] = status;
        ag32_mcu_write_le32(output + wire_payload,
            ag32_mcu_crc32((uint8_t *)s_response_words + 4, 4 + wire_payload));
        REG32(MCU_REG_TX_ADDR) = (uint32_t)s_response_words;
        REG32(MCU_REG_TX_WORDS) = 3u + wire_payload / 4u;
        REG32(MCU_REG_CTRL) = MCU_CTRL_PUBLISH_TX | MCU_CTRL_BUSY_CLEAR;
        if (!wait_tx_consumed() || status != AG32_MCU_STATUS_OK) break;
        s_stream_remaining -= size;
        s_stream_address += (s_stream_opcode == AG32_MCU_CMD_ROM_READ ||
            s_stream_opcode == AG32_MCU_CMD_ROM_WRITE || s_stream_opcode == AG32_MCU_CMD_ROM_PROGRAM) ? size / 2u : size;
    }
    s_stream_remaining = 0;
}

int main(void)
{
    board_init();
    SYS_DisableNJTRST();
    SYS_DisableJTDI();
    SYS_DisableJTDO();
    cart_release();
    if (REG32(MCU_REG_IDENTITY) != 0x3155434du) {
        while (1) { }
    }
    arm_request();
    bool was_mcu = false;

    while (1) {
        uint32_t control = REG32(MCU_REG_CTRL);
        if ((control & MCU_STATE_MODE) == 0u) {
            if (was_mcu) arm_request();
            was_mcu = false;
            s_sequence = 0u;
            s_cart_control = REG32(MCU_REG_CART_CTRL);
            continue;
        }
        was_mcu = true;
        if ((control & MCU_STATE_RX_DONE) == 0u) continue;

        REG32(MCU_REG_CTRL) = MCU_CTRL_BUSY_SET;
        uint32_t frame_size = REG32(MCU_REG_RX_WORDS) * 4u;
        ag32_mcu_request_t request = {0};
        ag32_mcu_status_t status = ag32_mcu_decode_request(
            (const uint8_t *)s_request_words, frame_size, &request);
        uint32_t response_size = 0u;
        uint32_t result = 0u;
        uint8_t *response_payload = (uint8_t *)&s_response_words[1] +
            AG32_MCU_FRAME_HEADER_SIZE;

        if (status == AG32_MCU_STATUS_OK) {
            if (request.sequence == s_sequence) {
                status = AG32_MCU_STATUS_INVALID_ARGUMENT;
            } else {
                s_sequence = request.sequence;
                status = process_command(&request,
                    (const uint8_t *)s_request_words + AG32_MCU_FRAME_HEADER_SIZE,
                    response_payload, &response_size, &result);
                if (response_size > request.response_capacity) {
                    response_size = 0u;
                    status = AG32_MCU_STATUS_BAD_LENGTH;
                }
            }
        }
        publish_response(&request, status, response_size, result);
        REG32(MCU_REG_CTRL) = MCU_CTRL_CLEAR_RX;

        if (wait_tx_consumed() && status == AG32_MCU_STATUS_OK &&
            request.opcode == AG32_MCU_CMD_STREAM_BEGIN) run_stream();
        arm_request();
    }
}

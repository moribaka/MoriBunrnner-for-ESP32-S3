#include "mcu_debug.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pin_map.h"

#if MORI_SWD_ENABLE

#define MCU_DEBUG_TAG "mcu_debug"

#define SWD_DELAY_US 1
#define SWD_DELAY_US_MIN 1
#define SWD_DELAY_US_MAX 30
#define SWD_LINE_RESET_CYCLES 60
#define SWD_IDLE_CYCLES 8
#define SWD_RESET_ASSERT_MS 5
#define SWD_RESET_RELEASE_SETTLE_MS 2

/*
 * Stable SWD defaults, aligned with the parameter profile that proved
 * reliable on this board family.
 */
#define SWD_DEFAULT_DO_RESET true
#define SWD_DEFAULT_DELAY_US 1
#define SWD_DEFAULT_SEQ_MODE MCU_DEBUG_SEQ_AUTO
#define SWD_DEFAULT_SWAP_CLK_DIO false
#define SWD_DEFAULT_TURNAROUND_CYCLES 1
#define SWD_DEFAULT_PULL_MODE MCU_DEBUG_PULL_UP

/* SWD request for DP IDCODE read (LSB first: 0xA5). */
#define SWD_REQ_DP_IDCODE 0xA5
#define SWD_JTAG_TO_SWD_STD 0xE79E
#define SWD_JTAG_TO_SWD_REV 0x79E7

/* SWD acknowledge values (LSB first). */
#define SWD_ACK_OK 0x1
#define SWD_ACK_WAIT 0x2
#define SWD_ACK_FAULT 0x4

static bool s_inited = false;
static SemaphoreHandle_t s_lock = NULL;
static uint32_t s_delay_us = SWD_DELAY_US;
static gpio_num_t s_pin_swclk = MORI_PIN_MCU_SWCLK;
static gpio_num_t s_pin_swdio = MORI_PIN_MCU_SWDIO;
static gpio_num_t s_pin_reset = MORI_PIN_MCU_RESET;
static mcu_debug_pull_mode_t s_swdio_pull_mode = MCU_DEBUG_PULL_UP;

static void mcu_debug_release_bus(void);

static inline void swd_delay(void)
{
    esp_rom_delay_us(s_delay_us);
}

static inline void swclk_cycle(void)
{
    gpio_set_level(s_pin_swclk, 0);
    swd_delay();
    gpio_set_level(s_pin_swclk, 1);
    swd_delay();
}

static inline void swdio_output(void)
{
    gpio_set_direction(s_pin_swdio, GPIO_MODE_OUTPUT);
}

static inline void swdio_input(void)
{
    gpio_set_direction(s_pin_swdio, GPIO_MODE_INPUT);
    switch (s_swdio_pull_mode) {
        case MCU_DEBUG_PULL_UP:
            gpio_set_pull_mode(s_pin_swdio, GPIO_PULLUP_ONLY);
            break;
        case MCU_DEBUG_PULL_DOWN:
            gpio_set_pull_mode(s_pin_swdio, GPIO_PULLDOWN_ONLY);
            break;
        case MCU_DEBUG_PULL_NONE:
        default:
            gpio_set_pull_mode(s_pin_swdio, GPIO_FLOATING);
            break;
    }
}

static inline void swdio_write_level(int level)
{
    gpio_set_level(s_pin_swdio, level ? 1 : 0);
}

static inline int swdio_read_level(void)
{
    return gpio_get_level(s_pin_swdio);
}

static uint8_t parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value &= 0xF;
    return (uint8_t)((0x6996U >> value) & 0x1U);
}

static void swd_write_bits(uint32_t value, int bit_count)
{
    for (int i = 0; i < bit_count; i++) {
        swdio_write_level((value >> i) & 0x1);
        swclk_cycle();
    }
}

static uint32_t swd_read_bits(int bit_count)
{
    uint32_t value = 0;
    for (int i = 0; i < bit_count; i++) {
        swclk_cycle();
        if (swdio_read_level() != 0) {
            value |= (1UL << i);
        }
    }
    return value;
}

static void swd_line_reset(void)
{
    swdio_output();
    swdio_write_level(1);
    for (int i = 0; i < SWD_LINE_RESET_CYCLES; i++) {
        swclk_cycle();
    }
}

static void swd_jtag_to_swd_sequence(void)
{
    /*
     * 16-bit JTAG-to-SWD sequence.
     * Sent LSB first using 0xE79E.
     */
    swdio_output();
    swd_write_bits(SWD_JTAG_TO_SWD_STD, 16);
}

static void swd_jtag_to_swd_sequence_rev(void)
{
    /*
     * Reversed-bit variant used for compatibility checks.
     * Sent LSB first using 0x79E7.
     */
    swdio_output();
    swd_write_bits(SWD_JTAG_TO_SWD_REV, 16);
}

static void swd_idle_cycles(void)
{
    swdio_output();
    swdio_write_level(1);
    for (int i = 0; i < SWD_IDLE_CYCLES; i++) {
        swclk_cycle();
    }
}

static void mcu_reset_assert(void)
{
    gpio_set_level(s_pin_reset, 0);
}

static void mcu_reset_release(void)
{
    gpio_set_level(s_pin_reset, 1);
}

static void mcu_debug_select_pins(bool swap_clk_dio)
{
    if (swap_clk_dio) {
        s_pin_swclk = MORI_PIN_MCU_SWDIO;
        s_pin_swdio = MORI_PIN_MCU_SWCLK;
    } else {
        s_pin_swclk = MORI_PIN_MCU_SWCLK;
        s_pin_swdio = MORI_PIN_MCU_SWDIO;
    }
    s_pin_reset = MORI_PIN_MCU_RESET;

    gpio_set_direction(s_pin_swclk, GPIO_MODE_OUTPUT);
    gpio_set_level(s_pin_swclk, 1);
    gpio_set_direction(s_pin_swdio, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(s_pin_swdio, GPIO_PULLUP_ONLY);
    gpio_set_level(s_pin_swdio, 1);
}

static void mcu_debug_release_bus(void)
{
    /*
     * Keep SWD lines high-Z when idle to avoid fighting an external CMSIS-DAP
     * or other debugger connected to the same target.
     */
    gpio_set_direction(MORI_PIN_MCU_SWCLK, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MORI_PIN_MCU_SWCLK, GPIO_FLOATING);
    gpio_set_direction(MORI_PIN_MCU_SWDIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MORI_PIN_MCU_SWDIO, GPIO_FLOATING);

    /* Open-drain reset stays released (high). */
    gpio_set_level(MORI_PIN_MCU_RESET, 1);
}

static void swd_apply_switch_sequence(mcu_debug_seq_mode_t seq_mode)
{
    switch (seq_mode) {
        case MCU_DEBUG_SEQ_STD:
            swd_jtag_to_swd_sequence();
            break;
        case MCU_DEBUG_SEQ_REV:
            swd_jtag_to_swd_sequence_rev();
            break;
        case MCU_DEBUG_SEQ_NONE:
        case MCU_DEBUG_SEQ_AUTO:
        default:
            break;
    }
}

static bool swd_ack_is_valid(uint8_t ack)
{
    return (ack == SWD_ACK_OK || ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT);
}

static void swd_probe_idcode_once(
    mcu_debug_seq_mode_t seq_mode,
    bool do_reset,
    uint8_t turnaround_cycles,
    mcu_debug_probe_result_t *out)
{
    uint8_t ack;
    uint32_t idcode;
    uint8_t parity_bit;
    uint8_t calc_parity;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->status = MCU_DEBUG_PROBE_TIMEOUT;

    /*
     * Bring target to a known state before probing SWD.
     * If target does not support SWD, ACK is expected to be non-OK.
     */
    if (do_reset) {
        /*
         * Match OpenOCD's "connect under reset" idea:
         * hold RESET# low while we push SWD line/switch sequences.
         */
        mcu_reset_assert();
        vTaskDelay(pdMS_TO_TICKS(SWD_RESET_ASSERT_MS));
    }
    swd_line_reset();
    swd_apply_switch_sequence(seq_mode);
    swd_line_reset();
    if (do_reset) {
        mcu_reset_release();
        vTaskDelay(pdMS_TO_TICKS(SWD_RESET_RELEASE_SETTLE_MS));
    }
    swd_idle_cycles();

    /* Request DP IDCODE read. */
    swdio_output();
    swd_write_bits(SWD_REQ_DP_IDCODE, 8);

    /* Turnaround: host releases SWDIO, then target responds ACK(3). */
    swdio_input();
    for (uint8_t i = 0; i < turnaround_cycles; i++) {
        swclk_cycle();
    }
    ack = (uint8_t)swd_read_bits(3);
    out->ack = ack;

    if (ack == SWD_ACK_OK) {
        idcode = swd_read_bits(32);
        parity_bit = (uint8_t)swd_read_bits(1);
        calc_parity = parity32(idcode);

        out->idcode = idcode;
        out->parity_ok = (parity_bit == calc_parity);
        out->status = out->parity_ok ? MCU_DEBUG_PROBE_OK : MCU_DEBUG_PROBE_PARITY_ERROR;
    } else if (ack == SWD_ACK_WAIT) {
        out->status = MCU_DEBUG_PROBE_ACK_WAIT;
    } else if (ack == SWD_ACK_FAULT) {
        out->status = MCU_DEBUG_PROBE_ACK_FAULT;
    } else {
        out->status = MCU_DEBUG_PROBE_ACK_PROTOCOL;
    }

    /* Turnaround back to host drive and keep line idle high. */
    swclk_cycle();
    swdio_output();
    swdio_write_level(1);
    swd_idle_cycles();
}

static esp_err_t mcu_debug_setup_gpio(void)
{
    gpio_config_t cfg = {0};
    esp_err_t err;

    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.pin_bit_mask = (1ULL << MORI_PIN_MCU_SWCLK) | (1ULL << MORI_PIN_MCU_SWDIO);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cfg.pin_bit_mask = (1ULL << MORI_PIN_MCU_RESET);
    cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(MORI_PIN_MCU_RESET, 1);
    mcu_debug_release_bus();
    return ESP_OK;
}

esp_err_t mcu_debug_init(void)
{
    esp_err_t err;

    if (s_inited) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = mcu_debug_setup_gpio();
    if (err != ESP_OK) {
        ESP_LOGE(MCU_DEBUG_TAG, "gpio setup failed: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(
        MCU_DEBUG_TAG,
        "SWD pins configured (idle high-Z): SWCLK=%d SWDIO=%d RESET#=%d",
        MORI_PIN_MCU_SWCLK,
        MORI_PIN_MCU_SWDIO,
        MORI_PIN_MCU_RESET);
    return ESP_OK;
}

void mcu_debug_get_default_probe_options(mcu_debug_probe_options_t *out)
{
    if (out == NULL) {
        return;
    }

    out->do_reset = SWD_DEFAULT_DO_RESET;
    out->delay_us = SWD_DEFAULT_DELAY_US;
    out->seq_mode = SWD_DEFAULT_SEQ_MODE;
    out->swap_clk_dio = SWD_DEFAULT_SWAP_CLK_DIO;
    out->turnaround_cycles = SWD_DEFAULT_TURNAROUND_CYCLES;
    out->swdio_pull_mode = SWD_DEFAULT_PULL_MODE;
}

esp_err_t mcu_debug_probe_idcode_with_opts(
    const mcu_debug_probe_options_t *opts,
    mcu_debug_probe_result_t *out)
{
    mcu_debug_probe_options_t local_opts;
    esp_err_t err;
    mcu_debug_seq_mode_t seq_list[3];
    int seq_count = 0;
    int i;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mcu_debug_get_default_probe_options(&local_opts);
    if (opts != NULL) {
        local_opts = *opts;
    }
    if (local_opts.delay_us < SWD_DELAY_US_MIN) {
        local_opts.delay_us = SWD_DELAY_US_MIN;
    }
    if (local_opts.delay_us > SWD_DELAY_US_MAX) {
        local_opts.delay_us = SWD_DELAY_US_MAX;
    }
    if (local_opts.seq_mode < MCU_DEBUG_SEQ_AUTO || local_opts.seq_mode > MCU_DEBUG_SEQ_NONE) {
        local_opts.seq_mode = MCU_DEBUG_SEQ_AUTO;
    }
    if (local_opts.turnaround_cycles > 2) {
        local_opts.turnaround_cycles = 2;
    }
    if (local_opts.swdio_pull_mode < MCU_DEBUG_PULL_UP ||
        local_opts.swdio_pull_mode > MCU_DEBUG_PULL_NONE) {
        local_opts.swdio_pull_mode = MCU_DEBUG_PULL_UP;
    }

    memset(out, 0, sizeof(*out));
    out->status = MCU_DEBUG_PROBE_TIMEOUT;
    out->seq_used = (uint8_t)local_opts.seq_mode;
    out->attempt_count = 0;

    err = mcu_debug_init();
    if (err != ESP_OK) {
        out->status = MCU_DEBUG_PROBE_IO_ERROR;
        return err;
    }

    if (s_lock == NULL) {
        out->status = MCU_DEBUG_PROBE_IO_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_delay_us = local_opts.delay_us;
    s_swdio_pull_mode = local_opts.swdio_pull_mode;
    mcu_debug_select_pins(local_opts.swap_clk_dio);

    if (local_opts.seq_mode == MCU_DEBUG_SEQ_AUTO) {
        seq_list[0] = MCU_DEBUG_SEQ_STD;
        seq_list[1] = MCU_DEBUG_SEQ_REV;
        seq_list[2] = MCU_DEBUG_SEQ_NONE;
        seq_count = 3;
    } else {
        seq_list[0] = local_opts.seq_mode;
        seq_count = 1;
    }

    for (i = 0; i < seq_count; i++) {
        mcu_debug_probe_result_t one_try;
        bool do_reset_this_try = (i == 0) ? local_opts.do_reset : false;

        swd_probe_idcode_once(
            seq_list[i],
            do_reset_this_try,
            local_opts.turnaround_cycles,
            &one_try);
        one_try.seq_used = (uint8_t)seq_list[i];
        one_try.attempt_count = (uint8_t)(i + 1);
        *out = one_try;

        ESP_LOGI(
            MCU_DEBUG_TAG,
            "probe try=%d seq=%s swap=%d ta=%u pull=%s ack=%u status=%s idcode=0x%08" PRIx32,
            i + 1,
            mcu_debug_seq_mode_str(seq_list[i]),
            local_opts.swap_clk_dio ? 1 : 0,
            local_opts.turnaround_cycles,
            mcu_debug_pull_mode_str(local_opts.swdio_pull_mode),
            out->ack,
            mcu_debug_probe_status_str(out->status),
            out->idcode);

        if (swd_ack_is_valid(out->ack)) {
            break;
        }
    }

    mcu_debug_release_bus();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t mcu_debug_probe_idcode(mcu_debug_probe_result_t *out)
{
    return mcu_debug_probe_idcode_with_opts(NULL, out);
}

const char *mcu_debug_probe_status_str(mcu_debug_probe_status_t status)
{
    switch (status) {
        case MCU_DEBUG_PROBE_OK:
            return "ok";
        case MCU_DEBUG_PROBE_ACK_WAIT:
            return "ack_wait";
        case MCU_DEBUG_PROBE_ACK_FAULT:
            return "ack_fault";
        case MCU_DEBUG_PROBE_ACK_PROTOCOL:
            return "ack_protocol";
        case MCU_DEBUG_PROBE_PARITY_ERROR:
            return "parity_error";
        case MCU_DEBUG_PROBE_TIMEOUT:
            return "timeout";
        case MCU_DEBUG_PROBE_IO_ERROR:
            return "io_error";
        default:
            return "unknown";
    }
}

const char *mcu_debug_seq_mode_str(mcu_debug_seq_mode_t mode)
{
    switch (mode) {
        case MCU_DEBUG_SEQ_AUTO:
            return "auto";
        case MCU_DEBUG_SEQ_STD:
            return "std";
        case MCU_DEBUG_SEQ_REV:
            return "rev";
        case MCU_DEBUG_SEQ_NONE:
            return "none";
        default:
            return "unknown";
    }
}

const char *mcu_debug_pull_mode_str(mcu_debug_pull_mode_t mode)
{
    switch (mode) {
        case MCU_DEBUG_PULL_UP:
            return "up";
        case MCU_DEBUG_PULL_DOWN:
            return "down";
        case MCU_DEBUG_PULL_NONE:
            return "none";
        default:
            return "unknown";
    }
}

#else

esp_err_t mcu_debug_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void mcu_debug_get_default_probe_options(mcu_debug_probe_options_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->do_reset = false;
    out->delay_us = 0;
    out->seq_mode = MCU_DEBUG_SEQ_NONE;
    out->swap_clk_dio = false;
    out->turnaround_cycles = 0;
    out->swdio_pull_mode = MCU_DEBUG_PULL_NONE;
}

esp_err_t mcu_debug_probe_idcode_with_opts(
    const mcu_debug_probe_options_t *opts,
    mcu_debug_probe_result_t *out)
{
    (void)opts;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->status = MCU_DEBUG_PROBE_IO_ERROR;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mcu_debug_probe_idcode(mcu_debug_probe_result_t *out)
{
    return mcu_debug_probe_idcode_with_opts(NULL, out);
}

const char *mcu_debug_probe_status_str(mcu_debug_probe_status_t status)
{
    switch (status) {
        case MCU_DEBUG_PROBE_OK:
            return "ok";
        case MCU_DEBUG_PROBE_ACK_WAIT:
            return "ack_wait";
        case MCU_DEBUG_PROBE_ACK_FAULT:
            return "ack_fault";
        case MCU_DEBUG_PROBE_ACK_PROTOCOL:
            return "ack_protocol";
        case MCU_DEBUG_PROBE_PARITY_ERROR:
            return "parity_error";
        case MCU_DEBUG_PROBE_TIMEOUT:
            return "timeout";
        case MCU_DEBUG_PROBE_IO_ERROR:
            return "disabled";
        default:
            return "unknown";
    }
}

const char *mcu_debug_seq_mode_str(mcu_debug_seq_mode_t mode)
{
    switch (mode) {
        case MCU_DEBUG_SEQ_AUTO:
            return "auto";
        case MCU_DEBUG_SEQ_STD:
            return "std";
        case MCU_DEBUG_SEQ_REV:
            return "rev";
        case MCU_DEBUG_SEQ_NONE:
            return "none";
        default:
            return "unknown";
    }
}

const char *mcu_debug_pull_mode_str(mcu_debug_pull_mode_t mode)
{
    switch (mode) {
        case MCU_DEBUG_PULL_UP:
            return "up";
        case MCU_DEBUG_PULL_DOWN:
            return "down";
        case MCU_DEBUG_PULL_NONE:
            return "none";
        default:
            return "unknown";
    }
}

#endif

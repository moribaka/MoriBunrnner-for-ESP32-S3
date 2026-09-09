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
#define SWD_DELAY_US 1u
#define SWD_LINE_RESET_CYCLES 60u
#define SWD_IDLE_CYCLES 8u
#define SWD_ACK_OK 0x1u
#define SWD_ACK_WAIT 0x2u
#define SWD_WAIT_RETRIES 64u
#define SWD_READ_TURNAROUND 1u
#define SWD_WRITE_TURNAROUND 2u

#define DP_REG_ABORT 0x00u
#define DP_REG_IDCODE 0x00u
#define DP_REG_CTRL_STAT 0x04u
#define DP_REG_SELECT 0x08u
#define DP_REG_RDBUFF 0x0cu
#define DP_ABORT_CLEAR_ALL 0x1fu
#define DP_CTRL_ORUNDETECT (1u << 0)
#define DP_CTRL_CDBGPWRUPREQ (1u << 28)
#define DP_CTRL_CDBGPWRUPACK (1u << 29)
#define DP_CTRL_CSYSPWRUPREQ (1u << 30)
#define DP_CTRL_CSYSPWRUPACK (1u << 31)

#define AP_REG_CSW 0x00u
#define AP_REG_TAR 0x04u
#define AP_REG_DRW 0x0cu
#define AP_REG_IDR 0xfcu
#define AP_CSW_SIZE_MASK 0x7u
#define AP_CSW_ADDRINC_MASK (3u << 4)
#define AP_CSW_SIZE32 0x2u

#define DMI_DMCONTROL 0x10u
#define DMI_DMSTATUS 0x11u
#define DMI_SBCS 0x38u
#define DMI_SBADDRESS0 0x39u
#define DMI_SBDATA0 0x3cu

#define DMCONTROL_DMACTIVE (1u << 0)
#define DMCONTROL_NDMRESET (1u << 1)
#define DMCONTROL_RESUMEREQ (1u << 30)
#define DMCONTROL_HALTREQ (1u << 31)
#define DMSTATUS_ANYHALTED (1u << 8)
#define DMSTATUS_ALLHALTED (1u << 9)
#define DMSTATUS_ANYRESUMEACK (1u << 16)
#define DMSTATUS_ALLRESUMEACK (1u << 17)

#define SBCS_SBERROR_MASK (7u << 12)
#define SBCS_SBACCESS16 (1u << 17)
#define SBCS_SBACCESS32 (2u << 17)
#define SBCS_SBREADONADDR (1u << 20)
#define SBCS_SBBUSY (1u << 21)
#define SBCS_SBBUSYERROR (1u << 22)
#define SBCS_ERROR_CLEAR (SBCS_SBERROR_MASK | SBCS_SBBUSYERROR)

static bool s_inited;
static bool s_session_active;
static SemaphoreHandle_t s_lock;
static uint32_t s_delay_us = SWD_DELAY_US;
static uint32_t s_ap_csw_base;
static uint32_t s_ap_tar = UINT32_MAX;
static uint32_t s_dp_select = UINT32_MAX;
static uint8_t s_last_ack;

static inline void swd_delay(void)
{
    esp_rom_delay_us(s_delay_us);
}

static inline void swclk_cycle(void)
{
    gpio_set_level(MORI_PIN_MCU_SWCLK, 0);
    swd_delay();
    gpio_set_level(MORI_PIN_MCU_SWCLK, 1);
    swd_delay();
}

static inline void swdio_output(void)
{
    gpio_set_direction(MORI_PIN_MCU_SWDIO, GPIO_MODE_OUTPUT);
}

static inline void swdio_input(void)
{
    gpio_set_direction(MORI_PIN_MCU_SWDIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MORI_PIN_MCU_SWDIO, GPIO_PULLUP_ONLY);
}

static void swd_write_bits(uint32_t value, unsigned bit_count)
{
    for (unsigned bit = 0u; bit < bit_count; ++bit) {
        gpio_set_level(MORI_PIN_MCU_SWDIO, (value >> bit) & 1u);
        swclk_cycle();
    }
}

static uint32_t swd_read_bits(unsigned bit_count)
{
    uint32_t value = 0u;
    for (unsigned bit = 0u; bit < bit_count; ++bit) {
        gpio_set_level(MORI_PIN_MCU_SWCLK, 0);
        swd_delay();
        gpio_set_level(MORI_PIN_MCU_SWCLK, 1);
        swd_delay();
        value |= (uint32_t)gpio_get_level(MORI_PIN_MCU_SWDIO) << bit;
    }
    return value;
}

static uint8_t parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    return (uint8_t)((0x6996u >> (value & 0x0fu)) & 1u);
}

static uint8_t swd_request(bool ap, bool read, uint8_t address)
{
    uint8_t fields = (uint8_t)((ap ? 1u : 0u)
        | ((read ? 1u : 0u) << 1)
        | (address & 0x0cu));
    return (uint8_t)(0x81u | (fields << 1) | (parity32(fields) << 5));
}

static esp_err_t swd_transfer_once(bool ap, bool read, uint8_t address, uint32_t *value)
{
    uint8_t ack;

    swdio_output();
    swd_write_bits(swd_request(ap, read, address), 8u);
    swdio_input();
    ack = (uint8_t)swd_read_bits(3u);
    s_last_ack = ack;

    if (ack != SWD_ACK_OK) {
        swdio_output();
        gpio_set_level(MORI_PIN_MCU_SWDIO, 1);
        swclk_cycle();
        return ack == SWD_ACK_WAIT ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }

    if (read) {
        uint32_t data = swd_read_bits(32u);
        uint8_t parity = (uint8_t)swd_read_bits(1u);
        for (unsigned cycle = 0u; cycle < SWD_READ_TURNAROUND; ++cycle) {
            swclk_cycle();
        }
        swdio_output();
        gpio_set_level(MORI_PIN_MCU_SWDIO, 1);
        swclk_cycle();
        if (parity != parity32(data)) {
            return ESP_ERR_INVALID_CRC;
        }
        if (value != NULL) {
            *value = data;
        }
    } else {
        uint32_t data = value != NULL ? *value : 0u;
        for (unsigned cycle = 0u; cycle < SWD_WRITE_TURNAROUND; ++cycle) {
            swclk_cycle();
        }
        swdio_output();
        swd_write_bits(data, 32u);
        swd_write_bits(parity32(data), 1u);
        gpio_set_level(MORI_PIN_MCU_SWDIO, 0);
        for (unsigned cycle = 0u; cycle < SWD_IDLE_CYCLES; ++cycle) {
            swclk_cycle();
        }
        gpio_set_level(MORI_PIN_MCU_SWDIO, 1);
    }
    return ESP_OK;
}

static esp_err_t swd_transfer(bool ap, bool read, uint8_t address, uint32_t *value)
{
    for (unsigned attempt = 0u; attempt < SWD_WAIT_RETRIES; ++attempt) {
        esp_err_t err = swd_transfer_once(ap, read, address, value);
        if (err != ESP_ERR_TIMEOUT) {
            return err;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t dp_read(uint8_t address, uint32_t *value)
{
    return swd_transfer(false, true, address, value);
}

static esp_err_t dp_write(uint8_t address, uint32_t value)
{
    return swd_transfer(false, false, address, &value);
}

static esp_err_t ap_select(uint16_t address)
{
    uint32_t select = address & 0xf0u;
    if (select == s_dp_select) {
        return ESP_OK;
    }
    esp_err_t err = dp_write(DP_REG_SELECT, select);
    if (err == ESP_OK) {
        s_dp_select = select;
    }
    return err;
}

static esp_err_t ap_write(uint16_t address, uint32_t value)
{
    esp_err_t err = ap_select(address);
    if (err == ESP_OK) {
        err = swd_transfer(true, false, (uint8_t)(address & 0x0cu), &value);
    }
    if (err == ESP_OK) {
        err = dp_read(DP_REG_RDBUFF, &value);
    }
    return err;
}

static esp_err_t ap_read(uint16_t address, uint32_t *value)
{
    uint32_t posted;
    esp_err_t err = ap_select(address);
    if (err == ESP_OK) {
        err = swd_transfer(true, true, (uint8_t)(address & 0x0cu), &posted);
    }
    return err == ESP_OK ? dp_read(DP_REG_RDBUFF, value) : err;
}

static void swd_line_reset(void)
{
    swdio_output();
    gpio_set_level(MORI_PIN_MCU_SWDIO, 1);
    for (unsigned cycle = 0u; cycle < SWD_LINE_RESET_CYCLES; ++cycle) {
        swclk_cycle();
    }
}

static void swd_idle(void)
{
    swdio_output();
    gpio_set_level(MORI_PIN_MCU_SWDIO, 0);
    for (unsigned cycle = 0u; cycle < SWD_IDLE_CYCLES; ++cycle) {
        swclk_cycle();
    }
}

static esp_err_t swd_connect(uint32_t *dp_idcode)
{
    uint32_t value;
    esp_err_t err;

    swd_line_reset();
    swd_write_bits(0xe79eu, 16u);
    swd_line_reset();
    swd_idle();
    if (dp_read(DP_REG_IDCODE, &value) != ESP_OK) {
        ESP_LOGE(MCU_DEBUG_TAG, "SWD DP IDCODE read failed, ack=0x%x", s_last_ack);
        return ESP_ERR_NOT_FOUND;
    }
    if (dp_idcode != NULL) {
        *dp_idcode = value;
    }
    ESP_LOGI(MCU_DEBUG_TAG, "SWD DP IDCODE=0x%08" PRIx32, value);
    err = dp_write(DP_REG_ABORT, DP_ABORT_CLEAR_ALL);
    if (err != ESP_OK) {
        ESP_LOGE(MCU_DEBUG_TAG, "SWD DP ABORT failed: %s ack=0x%x", esp_err_to_name(err), s_last_ack);
        return err;
    }
    err = dp_read(DP_REG_CTRL_STAT, &value);
    if (err != ESP_OK) {
        ESP_LOGE(MCU_DEBUG_TAG, "SWD DP status after ABORT failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(MCU_DEBUG_TAG, "SWD DP status after ABORT=0x%08" PRIx32, value);
    err = dp_write(DP_REG_SELECT, 0u);
    if (err != ESP_OK) {
        ESP_LOGE(MCU_DEBUG_TAG, "SWD DP SELECT failed: %s ack=0x%x", esp_err_to_name(err), s_last_ack);
        return err;
    }
    s_dp_select = 0u;
    err = dp_read(DP_REG_CTRL_STAT, &value);
    if (err != ESP_OK) {
        ESP_LOGE(MCU_DEBUG_TAG, "SWD DP power status read failed: %s ack=0x%x",
            esp_err_to_name(err), s_last_ack);
        return err;
    }
    ESP_LOGI(MCU_DEBUG_TAG, "SWD DP CTRL/STAT=0x%08" PRIx32, value);
    if ((value & (DP_CTRL_CDBGPWRUPACK | DP_CTRL_CSYSPWRUPACK))
        == (DP_CTRL_CDBGPWRUPACK | DP_CTRL_CSYSPWRUPACK)) {
        return ESP_OK;
    }
    err = dp_write(
        DP_REG_CTRL_STAT,
        DP_CTRL_CDBGPWRUPREQ | DP_CTRL_CSYSPWRUPREQ | DP_CTRL_ORUNDETECT);
    if (err != ESP_OK) {
        ESP_LOGE(MCU_DEBUG_TAG, "SWD DP power request failed: %s ack=0x%x", esp_err_to_name(err), s_last_ack);
        return err;
    }
    for (unsigned attempt = 0u; attempt < 100u; ++attempt) {
        if (dp_read(DP_REG_CTRL_STAT, &value) != ESP_OK) {
            ESP_LOGE(MCU_DEBUG_TAG, "SWD DP power status read failed, ack=0x%x", s_last_ack);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if ((value & (DP_CTRL_CDBGPWRUPACK | DP_CTRL_CSYSPWRUPACK))
            == (DP_CTRL_CDBGPWRUPACK | DP_CTRL_CSYSPWRUPACK)) {
            return ESP_OK;
        }
        esp_rom_delay_us(100u);
    }
    ESP_LOGW(MCU_DEBUG_TAG, "SWD DP power ACK not implemented, CTRL/STAT=0x%08" PRIx32, value);
    return ESP_OK;
}

static esp_err_t ap_prepare_dmi(void)
{
    uint32_t csw;
    uint32_t idr = 0u;
    esp_err_t err = ap_read(AP_REG_IDR, &idr);
    if (err != ESP_OK || idr == 0u || idr == UINT32_MAX) {
        ESP_LOGE(MCU_DEBUG_TAG, "SWD AP IDR failed: %s value=0x%08" PRIx32,
            esp_err_to_name(err), idr);
        return ESP_ERR_NOT_FOUND;
    }
    err = ap_read(AP_REG_CSW, &csw);
    if (err != ESP_OK) {
        return err;
    }
    s_ap_csw_base = csw & ~(AP_CSW_SIZE_MASK | AP_CSW_ADDRINC_MASK);
    ESP_LOGI(MCU_DEBUG_TAG, "SWD AP IDR=0x%08" PRIx32 " CSW=0x%08" PRIx32, idr, csw);
    s_ap_tar = UINT32_MAX;
    return ap_write(AP_REG_CSW, s_ap_csw_base | AP_CSW_SIZE32);
}

static esp_err_t dmi_set_address(uint8_t address)
{
    uint32_t tar = (uint32_t)address * 4u;
    if (tar == s_ap_tar) {
        return ESP_OK;
    }
    esp_err_t err = ap_write(AP_REG_TAR, tar);
    if (err == ESP_OK) {
        s_ap_tar = tar;
    }
    return err;
}

static esp_err_t dmi_read(uint8_t address, uint32_t *value)
{
    esp_err_t err = dmi_set_address(address);
    return err == ESP_OK ? ap_read(AP_REG_DRW, value) : err;
}

static esp_err_t dmi_write(uint8_t address, uint32_t value)
{
    esp_err_t err = dmi_set_address(address);
    return err == ESP_OK ? ap_write(AP_REG_DRW, value) : err;
}

static esp_err_t wait_dmstatus(uint32_t mask)
{
    uint32_t last_status = 0u;
    for (unsigned attempt = 0u; attempt < 1000u; ++attempt) {
        uint32_t status;
        esp_err_t err = dmi_read(DMI_DMSTATUS, &status);
        if (err != ESP_OK) {
            return err;
        }
        if ((status & mask) == mask) {
            return ESP_OK;
        }
        last_status = status;
        esp_rom_delay_us(100u);
    }
    ESP_LOGE(MCU_DEBUG_TAG, "DMI status timeout: wanted=0x%08" PRIx32 " got=0x%08" PRIx32,
        mask, last_status);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t sb_wait(uint32_t *status_out)
{
    for (unsigned attempt = 0u; attempt < 10000u; ++attempt) {
        uint32_t status;
        esp_err_t err = dmi_read(DMI_SBCS, &status);
        if (err != ESP_OK) {
            return err;
        }
        if ((status & SBCS_SBBUSY) == 0u) {
            if (status_out != NULL) *status_out = status;
            return ESP_OK;
        }
        esp_rom_delay_us(10u);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t sb_configure(bool read, uint32_t access)
{
    uint32_t config = SBCS_ERROR_CLEAR | access;
    return dmi_write(DMI_SBCS, read ? config | SBCS_SBREADONADDR : config);
}

static esp_err_t sb_finish(void)
{
    uint32_t status;
    esp_err_t err = sb_wait(&status);
    if (err != ESP_OK) return err;
    return (status & (SBCS_SBERROR_MASK | SBCS_SBBUSYERROR)) == 0u
        ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t mcu_debug_setup_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << MORI_PIN_MCU_SWCLK) | (1ULL << MORI_PIN_MCU_SWDIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static void mcu_debug_release_bus(void)
{
    gpio_set_direction(MORI_PIN_MCU_SWCLK, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MORI_PIN_MCU_SWCLK, GPIO_FLOATING);
    gpio_set_direction(MORI_PIN_MCU_SWDIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MORI_PIN_MCU_SWDIO, GPIO_FLOATING);
}

esp_err_t mcu_debug_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t mcu_debug_session_begin(uint32_t *dp_idcode)
{
    esp_err_t err = mcu_debug_init();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_session_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    err = mcu_debug_setup_gpio();
    if (err == ESP_OK) {
        gpio_set_direction(MORI_PIN_MCU_SWCLK, GPIO_MODE_OUTPUT);
        gpio_set_level(MORI_PIN_MCU_SWCLK, 1);
        gpio_set_direction(MORI_PIN_MCU_SWDIO, GPIO_MODE_OUTPUT);
        gpio_set_level(MORI_PIN_MCU_SWDIO, 1);
        s_dp_select = UINT32_MAX;
        err = swd_connect(dp_idcode);
    }
    if (err == ESP_OK) {
        err = ap_prepare_dmi();
    }
    if (err != ESP_OK) {
        mcu_debug_release_bus();
        xSemaphoreGive(s_lock);
        return err;
    }
    s_session_active = true;
    return ESP_OK;
}

void mcu_debug_session_end(void)
{
    if (s_session_active) {
        mcu_debug_release_bus();
        s_session_active = false;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t mcu_debug_halt(void)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t before = 0u;
    (void)dmi_read(DMI_DMSTATUS, &before);
    ESP_LOGI(MCU_DEBUG_TAG, "DMI DMSTATUS before halt=0x%08" PRIx32, before);
    esp_err_t err = dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_HALTREQ);
    return err == ESP_OK ? wait_dmstatus(DMSTATUS_ANYHALTED | DMSTATUS_ALLHALTED) : err;
}

esp_err_t mcu_debug_resume(void)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_RESUMEREQ);
    if (err == ESP_OK) {
        err = wait_dmstatus(DMSTATUS_ANYRESUMEACK | DMSTATUS_ALLRESUMEACK);
    }
    if (err == ESP_OK) {
        err = dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);
    }
    return err;
}

esp_err_t mcu_debug_system_reset(void)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE | DMCONTROL_NDMRESET);
    if (err == ESP_OK) {
        esp_rom_delay_us(100u);
        err = dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);
    }
    return err;
}

esp_err_t mcu_debug_read_memory32(uint32_t address, uint32_t *value)
{
    if (!s_session_active || value == NULL || (address & 3u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sb_wait(NULL);
    if (err == ESP_OK) err = sb_configure(true, SBCS_SBACCESS32);
    if (err == ESP_OK) err = dmi_write(DMI_SBADDRESS0, address);
    if (err == ESP_OK) err = sb_finish();
    if (err == ESP_OK) err = dmi_read(DMI_SBDATA0, value);
    return err;
}

esp_err_t mcu_debug_write_memory32(uint32_t address, uint32_t value)
{
    if (!s_session_active || (address & 3u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sb_wait(NULL);
    if (err == ESP_OK) err = sb_configure(false, SBCS_SBACCESS32);
    if (err == ESP_OK) err = dmi_write(DMI_SBADDRESS0, address);
    if (err == ESP_OK) err = dmi_write(DMI_SBDATA0, value);
    return err == ESP_OK ? sb_finish() : err;
}

esp_err_t mcu_debug_read_memory16(uint32_t address, uint16_t *value)
{
    uint32_t data;
    if (!s_session_active || value == NULL || (address & 1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sb_wait(NULL);
    if (err == ESP_OK) err = sb_configure(true, SBCS_SBACCESS16);
    if (err == ESP_OK) err = dmi_write(DMI_SBADDRESS0, address);
    if (err == ESP_OK) err = sb_finish();
    if (err == ESP_OK) err = dmi_read(DMI_SBDATA0, &data);
    if (err == ESP_OK) *value = (uint16_t)data;
    return err;
}

esp_err_t mcu_debug_write_memory16(uint32_t address, uint16_t value)
{
    if (!s_session_active || (address & 1u) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sb_wait(NULL);
    if (err == ESP_OK) err = sb_configure(false, SBCS_SBACCESS16);
    if (err == ESP_OK) err = dmi_write(DMI_SBADDRESS0, address);
    if (err == ESP_OK) err = dmi_write(DMI_SBDATA0, value);
    return err == ESP_OK ? sb_finish() : err;
}

void mcu_debug_get_default_probe_options(mcu_debug_probe_options_t *out)
{
    if (out != NULL) {
        *out = (mcu_debug_probe_options_t){
            .do_reset = false,
            .delay_us = SWD_DELAY_US,
            .seq_mode = MCU_DEBUG_SEQ_STD,
            .turnaround_cycles = 1u,
            .swdio_pull_mode = MCU_DEBUG_PULL_UP,
        };
    }
}

esp_err_t mcu_debug_probe_idcode_with_opts(
    const mcu_debug_probe_options_t *opts,
    mcu_debug_probe_result_t *out)
{
    uint32_t idcode = 0u;
    (void)opts;
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    esp_err_t err = mcu_debug_session_begin(&idcode);
    out->idcode = idcode;
    if (err == ESP_OK) {
        out->status = MCU_DEBUG_PROBE_OK;
        out->ack = SWD_ACK_OK;
        out->idcode = idcode;
        out->parity_ok = true;
        out->seq_used = MCU_DEBUG_SEQ_STD;
        out->attempt_count = 1u;
        mcu_debug_session_end();
    } else {
        out->status = err == ESP_ERR_TIMEOUT
            ? MCU_DEBUG_PROBE_TIMEOUT : MCU_DEBUG_PROBE_ACK_PROTOCOL;
    }
    return err;
}

esp_err_t mcu_debug_probe_idcode(mcu_debug_probe_result_t *out)
{
    return mcu_debug_probe_idcode_with_opts(NULL, out);
}

const char *mcu_debug_probe_status_str(mcu_debug_probe_status_t status)
{
    static const char *const names[] = {
        "ok", "ack_wait", "ack_fault", "ack_protocol",
        "parity_error", "timeout", "io_error",
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

const char *mcu_debug_seq_mode_str(mcu_debug_seq_mode_t mode)
{
    static const char *const names[] = {"auto", "std", "rev", "none"};
    return (unsigned)mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "unknown";
}

const char *mcu_debug_pull_mode_str(mcu_debug_pull_mode_t mode)
{
    static const char *const names[] = {"up", "down", "none"};
    return (unsigned)mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "unknown";
}

#else

esp_err_t mcu_debug_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_session_begin(uint32_t *id) { (void)id; return ESP_ERR_NOT_SUPPORTED; }
void mcu_debug_session_end(void) { }
esp_err_t mcu_debug_halt(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_resume(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_system_reset(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_read_memory16(uint32_t address, uint16_t *value)
{ (void)address; (void)value; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_write_memory16(uint32_t address, uint16_t value)
{ (void)address; (void)value; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_read_memory32(uint32_t address, uint32_t *value)
{ (void)address; (void)value; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_write_memory32(uint32_t address, uint32_t value)
{ (void)address; (void)value; return ESP_ERR_NOT_SUPPORTED; }
void mcu_debug_get_default_probe_options(mcu_debug_probe_options_t *out)
{ if (out != NULL) memset(out, 0, sizeof(*out)); }
esp_err_t mcu_debug_probe_idcode_with_opts(const mcu_debug_probe_options_t *opts, mcu_debug_probe_result_t *out)
{ (void)opts; if (out != NULL) memset(out, 0, sizeof(*out)); return ESP_ERR_NOT_SUPPORTED; }
esp_err_t mcu_debug_probe_idcode(mcu_debug_probe_result_t *out)
{ return mcu_debug_probe_idcode_with_opts(NULL, out); }
const char *mcu_debug_probe_status_str(mcu_debug_probe_status_t status)
{ (void)status; return "disabled"; }
const char *mcu_debug_seq_mode_str(mcu_debug_seq_mode_t mode)
{ (void)mode; return "none"; }
const char *mcu_debug_pull_mode_str(mcu_debug_pull_mode_t mode)
{ (void)mode; return "none"; }

#endif

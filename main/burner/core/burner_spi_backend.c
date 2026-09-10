#include "ws_server_internal.h"

#include "esp_memory_utils.h"

static bool s_swd_mode;
static volatile bool s_swd_restore_blocked;

uint8_t burner_bacon_option_byte0(
    uint8_t batch_size,
    bool dir_a,
    bool dir_ad,
    bool cs2,
    bool cs1,
    bool rd,
    bool wr)
{
    uint8_t ret = (uint8_t)((batch_size & 0x03u) << 6);

    if (dir_a) {
        ret |= 0x20u;
    }
    if (dir_ad) {
        ret |= 0x10u;
    }
    if (cs2) {
        ret |= 0x08u;
    }
    if (cs1) {
        ret |= 0x04u;
    }
    if (rd) {
        ret |= 0x02u;
    }
    if (wr) {
        ret |= 0x01u;
    }

    return ret;
}

void burner_spi_apply_cs_mode(burner_spi_cs_mode_t mode)
{
#if BURNER_SPI_ENABLE
    switch (mode) {
    case BURNER_SPI_CS_MODE_0:
        gpio_set_level(MORI_PIN_MCU_SPI_CS, 0);
        gpio_set_level(MORI_PIN_MCU_SPI_CS1, 1);
        break;
    case BURNER_SPI_CS_MODE_1:
        gpio_set_level(MORI_PIN_MCU_SPI_CS, 1);
        gpio_set_level(MORI_PIN_MCU_SPI_CS1, 0);
        break;
    case BURNER_SPI_CS_MODE_2:
    default:
        gpio_set_level(MORI_PIN_MCU_SPI_CS, 0);
        gpio_set_level(MORI_PIN_MCU_SPI_CS1, 0);
        break;
    }
#else
    (void)mode;
#endif
}

void burner_spi_release_cs(void)
{
#if BURNER_SPI_ENABLE
    gpio_set_level(MORI_PIN_MCU_SPI_CS, 1);
    gpio_set_level(MORI_PIN_MCU_SPI_CS1, 1);
#endif
}

uint32_t burner_spi_cs_setup_delay_us(burner_spi_cs_mode_t mode)
{
    switch (mode) {
    case BURNER_SPI_CS_MODE_0:
    case BURNER_SPI_CS_MODE_1:
        return BURNER_GBA_SPI_CS_SETUP_US;
    case BURNER_SPI_CS_MODE_2:
    default:
        return 0u;
    }
}

static uint32_t burner_spi_cs_hold_delay_us(burner_spi_cs_mode_t mode)
{
    switch (mode) {
    case BURNER_SPI_CS_MODE_0:
    case BURNER_SPI_CS_MODE_1:
        return BURNER_GBA_SPI_CS_HOLD_US;
    case BURNER_SPI_CS_MODE_2:
    default:
        return 0u;
    }
}

esp_err_t burner_spi_begin_cs(burner_spi_cs_mode_t mode)
{
#if BURNER_SPI_ENABLE
    uint32_t cs_setup_delay_us;

    if (s_swd_mode || !s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    burner_bacon_mark_activity_locked();
    burner_spi_apply_cs_mode(mode);
    cs_setup_delay_us = burner_spi_cs_setup_delay_us(mode);
    if (cs_setup_delay_us > 0u) {
        esp_rom_delay_us(cs_setup_delay_us);
    }
    return ESP_OK;
#else
    (void)mode;
    return ESP_OK;
#endif
}

void burner_spi_end_cs(burner_spi_cs_mode_t mode)
{
#if BURNER_SPI_ENABLE
    if (s_swd_mode) return;
    uint32_t cs_hold_delay_us = burner_spi_cs_hold_delay_us(mode);

    if (cs_hold_delay_us > 0u) {
        esp_rom_delay_us(cs_hold_delay_us);
    }
    burner_spi_release_cs();
#else
    (void)mode;
#endif
}

esp_err_t burner_spi_transfer_cs(
    burner_spi_cs_mode_t mode,
    const uint8_t *tx,
    uint8_t *rx,
    size_t len)
{
#if BURNER_SPI_ENABLE
    size_t offset;
    size_t chunk_limit;
    esp_err_t err;

    if (tx == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_swd_mode || !s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    chunk_limit = BURNER_SPI_CS_XFER_CHUNK_BYTES;
    if (chunk_limit == 0u || chunk_limit > BURNER_SPI_MAX_XFER) {
        chunk_limit = BURNER_SPI_MAX_XFER;
    }

    err = burner_spi_begin_cs(mode);
    if (err != ESP_OK) {
        return err;
    }
    for (offset = 0u; offset < len; offset += chunk_limit) {
        size_t chunk_len = len - offset;

        if (chunk_len > chunk_limit) {
            chunk_len = chunk_limit;
        }

        err = burner_spi_transfer_active(tx + offset, (rx != NULL) ? (rx + offset) : NULL, chunk_len);
        if (err != ESP_OK) {
            break;
        }
    }

    burner_spi_end_cs(mode);
    return err;
#else
    (void)mode;
    (void)tx;
    (void)rx;
    (void)len;
    return ESP_OK;
#endif
}

esp_err_t burner_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    return burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, tx, rx, len);
}

/*
 * GBA command path regression guard:
 * use a direct single-transaction transfer that matches the old stable path.
 * Keep this scoped to GBA command read/write sequences only.
 */
esp_err_t burner_spi_transfer_cs_legacy(
    burner_spi_cs_mode_t mode,
    const uint8_t *tx,
    uint8_t *rx,
    size_t len)
{
#if BURNER_SPI_ENABLE
    esp_err_t err = ESP_OK;
    spi_transaction_t trans = {0};
    const uint8_t *tx_buf = tx;
    uint8_t *rx_buf = rx;
    bool use_tx_shadow = false;
    bool use_rx_shadow = false;

    if (tx == NULL || len == 0u || len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_swd_mode || !s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mcu_spi_tx_shadow != NULL && len <= BURNER_SPI_SHADOW_CHUNK_BYTES) {
        memcpy(s_mcu_spi_tx_shadow, tx, len);
        tx_buf = s_mcu_spi_tx_shadow;
        use_tx_shadow = true;
    }
    if (rx != NULL && s_mcu_spi_rw_shadow != NULL && len <= BURNER_SPI_SHADOW_CHUNK_BYTES) {
        rx_buf = s_mcu_spi_rw_shadow;
        use_rx_shadow = true;
    }

    trans.length = (uint32_t)(len * 8u);
    trans.tx_buffer = tx_buf;
    trans.rx_buffer = rx_buf;

    burner_bacon_mark_activity_locked();
    burner_spi_apply_cs_mode(mode);
    err = spi_device_polling_transmit(s_mcu_spi, &trans);
    burner_spi_release_cs();
    if (err == ESP_OK && use_rx_shadow) {
        memcpy(rx, s_mcu_spi_rw_shadow, len);
    }
    (void)use_tx_shadow;
    return err;
#else
    (void)mode;
    (void)tx;
    (void)rx;
    (void)len;
    return ESP_OK;
#endif
}

esp_err_t burner_spi_transfer_active(const uint8_t *tx, uint8_t *rx, size_t len)
{
#if BURNER_SPI_ENABLE
    esp_err_t err;
    spi_transaction_t trans = {0};
    uint8_t *tx_buf = NULL;
    uint8_t *rx_buf = NULL;
    bool free_tx_buf = false;
    bool free_rx_buf = false;

    if (tx == NULL || len == 0u || len > BURNER_SPI_MAX_XFER) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_swd_mode || !s_mcu_spi_ready || s_mcu_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    trans.length = (uint32_t)(len * 8u);
    if (len <= sizeof(trans.tx_data)) {
        trans.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(trans.tx_data, tx, len);
        if (rx != NULL) {
            trans.flags |= SPI_TRANS_USE_RXDATA;
        }
    } else {
        if (esp_ptr_dma_capable(tx)) {
            trans.tx_buffer = tx;
        } else {
            tx_buf = burner_spi_alloc_tx_buffer(len, &free_tx_buf);
            if (tx_buf == NULL) {
                return ESP_ERR_NO_MEM;
            }
            memcpy(tx_buf, tx, len);
            trans.tx_buffer = tx_buf;
        }
        if (rx != NULL) {
            // RX DMA writes complete words. Non-overlapping aligned internal
            // buffers can be filled directly, without a second CPU copy.
            bool overlaps_tx = (uintptr_t)rx < (uintptr_t)tx + len &&
                               (uintptr_t)tx < (uintptr_t)rx + len;
            if (esp_ptr_dma_capable(rx) && !overlaps_tx &&
                (((uintptr_t)rx | len) & 3u) == 0u) {
                trans.rx_buffer = rx;
            } else {
                rx_buf = burner_spi_alloc_rw_buffer(len, &free_rx_buf);
                if (rx_buf == NULL) {
                    if (free_tx_buf) {
                        free(tx_buf);
                    }
                    return ESP_ERR_NO_MEM;
                }
                trans.rx_buffer = rx_buf;
            }
        }
    }

    burner_bacon_mark_activity_locked();
    err = spi_device_polling_transmit(s_mcu_spi, &trans);
    if (err == ESP_OK && rx != NULL) {
        if (trans.flags & SPI_TRANS_USE_RXDATA) {
            memcpy(rx, trans.rx_data, len);
        } else if (rx_buf != NULL && rx != rx_buf) {
            memcpy(rx, rx_buf, len);
        }
    }
    if (free_tx_buf) {
        free(tx_buf);
    }
    if (free_rx_buf) {
        free(rx_buf);
    }
    return err;
#else
    (void)tx;
    (void)rx;
    (void)len;
    return ESP_OK;
#endif
}

uint8_t *burner_spi_alloc_rw_buffer(size_t len, bool *needs_free)
{
    uint8_t *buf = NULL;

    if (needs_free != NULL) {
        *needs_free = false;
    }
    if (len == 0u || len > BURNER_SPI_MAX_XFER) {
        return NULL;
    }

    if (s_mcu_spi_rw_shadow != NULL && len <= BURNER_SPI_SHADOW_CHUNK_BYTES) {
        return s_mcu_spi_rw_shadow;
    }

    buf = (uint8_t *)heap_caps_malloc(
        len,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf != NULL && needs_free != NULL) {
        *needs_free = true;
    }
    return buf;
}

uint8_t *burner_spi_alloc_tx_buffer(size_t len, bool *needs_free)
{
    uint8_t *buf = NULL;

    if (needs_free != NULL) {
        *needs_free = false;
    }
    if (len == 0u || len > BURNER_SPI_MAX_XFER) {
        return NULL;
    }

    if (s_mcu_spi_tx_shadow != NULL && len <= BURNER_SPI_SHADOW_CHUNK_BYTES) {
        return s_mcu_spi_tx_shadow;
    }

    buf = (uint8_t *)heap_caps_malloc(
        len,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf != NULL && needs_free != NULL) {
        *needs_free = true;
    }
    return buf;
}

uint8_t burner_bacon_option_byte2(
    uint8_t batch_size,
    bool dir_a,
    bool dir_ad,
    bool ad_incr,
    bool cs1,
    bool rd,
    bool wr)
{
    uint8_t ret = (uint8_t)((batch_size & 0x03u) << 6);

    if (dir_a) {
        ret |= 0x20u;
    }
    if (dir_ad) {
        ret |= 0x10u;
    }
    if (ad_incr) {
        ret |= 0x08u;
    }
    if (cs1) {
        ret |= 0x04u;
    }
    if (rd) {
        ret |= 0x02u;
    }
    if (wr) {
        ret |= 0x01u;
    }

    return ret;
}

#include "burner_cart_power.inc"

static esp_err_t burner_spi_restore_cs_pins(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << MORI_PIN_MCU_SPI_CS) | (1ULL << MORI_PIN_MCU_SPI_CS1),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* Set the output latch before enabling the drivers. */
    burner_spi_release_cs();
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) err = gpio_set_drive_capability(MORI_PIN_MCU_SPI_CS1, GPIO_DRIVE_CAP_3);
    if (err == ESP_OK) err = gpio_pullup_en(MORI_PIN_MCU_SPI_CS1);
    return err;
}

esp_err_t burner_spi_init(void)
{
#if BURNER_SPI_ENABLE
    esp_err_t err;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = MORI_PIN_MCU_SPI_MOSI,
        .miso_io_num = MORI_PIN_MCU_SPI_MISO,
        .sclk_io_num = MORI_PIN_MCU_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = BURNER_SPI_MAX_XFER,
        .flags = 0,
        .intr_flags = 0,
    };
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0, /* Host CH347 is configured as SPI mode0 */
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = (int)s_mcu_spi_clock_hz,
        .input_delay_ns = 0,
        .spics_io_num = -1, /* manual dual-CS control for bacon protocol */
        .flags = 0,
        .queue_size = 1,
        .pre_cb = NULL,
        .post_cb = NULL,
    };

    if (s_swd_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mcu_spi_ready && s_mcu_spi != NULL) {
        burner_bacon_mark_activity_locked();
        return ESP_OK;
    }

    ESP_LOGI(
        BURNER_TAG,
        "MCU SPI init: host=%d cs0=%d cs1=%d clk=%d miso=%d mosi=%d freq=%" PRIu32 "Hz",
        (int)BURNER_SPI_HOST,
        MORI_PIN_MCU_SPI_CS,
        MORI_PIN_MCU_SPI_CS1,
        MORI_PIN_MCU_SPI_CLK,
        MORI_PIN_MCU_SPI_MISO,
        MORI_PIN_MCU_SPI_MOSI,
        s_mcu_spi_clock_hz);

    err = burner_spi_restore_cs_pins();
    if (err != ESP_OK) return err;

    err = spi_bus_initialize(BURNER_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    err = spi_bus_add_device(BURNER_SPI_HOST, &dev_cfg, &s_mcu_spi);
    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        (void)spi_bus_free(BURNER_SPI_HOST);
        return err;
    }

    burner_reset_cart_probe_state();
    ag32_mcu_link_invalidate();
    s_mcu_spi_ready = true;
    if (s_mcu_spi != NULL) {
        int actual_khz = 0;
        if (spi_device_get_actual_freq(s_mcu_spi, &actual_khz) == ESP_OK && actual_khz > 0) {
            s_mcu_spi_actual_hz = (uint32_t)actual_khz * 1000u;
        } else {
            s_mcu_spi_actual_hz = s_mcu_spi_clock_hz;
        }
    } else {
        s_mcu_spi_actual_hz = s_mcu_spi_clock_hz;
    }
    ESP_LOGI(
        BURNER_TAG,
        "MCU SPI ready: actual=%" PRIu32 "Hz dma_scratch_tx=%u dma_scratch_rw=%u",
        s_mcu_spi_actual_hz,
        (unsigned)BURNER_SPI_SHADOW_CHUNK_BYTES,
        (unsigned)BURNER_SPI_SHADOW_CHUNK_BYTES);
    burner_bacon_mark_activity_locked();
    return ESP_OK;
#else
    static bool warned = false;
    if (!warned) {
        ESP_LOGW(BURNER_TAG, "SPI skeleton running in mock mode (BURNER_SPI_ENABLE=0)");
        ESP_LOGI(
            BURNER_TAG,
            "SPI2 pins reserved: CS0=%d CS1=%d CLK=%d MISO=%d MOSI=%d",
            MORI_PIN_MCU_SPI_CS,
            MORI_PIN_MCU_SPI_CS1,
            MORI_PIN_MCU_SPI_CLK,
            MORI_PIN_MCU_SPI_MISO,
            MORI_PIN_MCU_SPI_MOSI);
        warned = true;
    }
    return ESP_OK;
#endif
}

#include "burner_spi_swd.inc"

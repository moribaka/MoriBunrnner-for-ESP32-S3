#include "bacon_cpld_transport.h"
#include "ag32_mcu_transport.h"
#include "burner/core/ws_server_internal.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

static int s_available = -1;
static const char *TAG = "bacon_cpld";
static bacon_cpld_stats_t s_stats;
static bacon_cpld_write_profile_t s_write_profile;
static uint8_t s_enter_flags;
static esp_err_t bsc2_program_locked(uint32_t address, const void *data,
    size_t size, uint16_t page_bytes, uint32_t timeout_ms);
// Separate TX clocks from RX storage so DMA never reads a buffer it is writing.
static DMA_ATTR uint8_t s_read_clocks[BACON_CPLD_BLOCK_BYTES + 8];
void bacon_cpld_get_stats(bacon_cpld_stats_t *out) { if (out) *out = s_stats; }

typedef struct { uint8_t flags; uint32_t completed, error; } cpld_status_t;

void bacon_cpld_link_invalidate(void) { s_available = -1; }

static esp_err_t mode_key(const char *key)
{
    burner_spi_release_cs();
    esp_err_t err = burner_spi_transfer_active((const uint8_t *)key, NULL, 8);
    burner_spi_release_cs();
    return err;
}

static esp_err_t read_status(cpld_status_t *status)
{
    ++s_stats.status_polls;
    uint8_t tx[BACON_CPLD_STATUS_BYTES];
    _Alignas(4) uint8_t rx[BACON_CPLD_STATUS_BYTES];
    memset(tx, 0x0f, sizeof(tx));
    esp_err_t err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_2, tx, rx, sizeof(rx));
    if (err != ESP_OK) return err;
    if (rx[4] != 0xba || rx[5] != 0xce || rx[6] != 1 || !(rx[7] & BACON_CPLD_MODE)) {
        ESP_LOGW(TAG, "status identity=%02x%02x%02x flags=%02x",rx[4],rx[5],rx[6],rx[7]);
        return ESP_ERR_NOT_SUPPORTED;
    }
    status->flags = rx[7];
    status->completed = ag32_mcu_read_le32(rx + 8);
    status->error = ag32_mcu_read_le32(rx + 12);
    if (status->flags & BACON_CPLD_ERROR) {
        ESP_LOGE(TAG, "hardware error=%" PRIu32 " completed=%" PRIu32,
            status->error, status->completed);
        return status->error == 8 ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t enter(void)
{
    // Re-establish ownership once per stream, including after an ESP reset.
    esp_err_t err = mode_key(BACON_CPLD_EXIT_MAGIC);
    static const uint8_t release = 0x0f;
    if (err == ESP_OK) err = burner_spi_transfer_cs_legacy(BURNER_SPI_CS_MODE_0, &release, NULL, 1);
    if (err == ESP_OK) err = mode_key(BACON_CPLD_ENTER_MAGIC);
    cpld_status_t status;
    if (err == ESP_OK) err = read_status(&status);
    if (err == ESP_OK) s_enter_flags = status.flags;
    return err;
}

esp_err_t bacon_cpld_probe_locked(void)
{
    esp_err_t err = enter();
    if (err == ESP_OK) s_available = 1;
    else if (err == ESP_ERR_NOT_SUPPORTED) s_available = 0;
    // Missing optional BSC2 support must not poison the BSC1 availability cache.
    if (err == ESP_OK && s_write_profile.mode == 2 &&
        !(s_enter_flags & BACON_CPLD_V2_CAP_OR_CREDIT)) err = ESP_ERR_NOT_SUPPORTED;
    esp_err_t exit_err = mode_key(BACON_CPLD_EXIT_MAGIC);
    return err == ESP_OK ? exit_err : err;
}

static esp_err_t wait_flag(uint8_t flag, uint32_t total, int64_t deadline)
{
    do {
        cpld_status_t status;
        esp_err_t err = read_status(&status);
        if (err != ESP_OK) return err;
        if (status.completed > total) return ESP_ERR_INVALID_RESPONSE;
        if (status.flags & flag) {
            if (flag == BACON_CPLD_DONE && status.completed != total)
                return ESP_ERR_INVALID_SIZE;
            return ESP_OK;
        }
        err = burner_cancel_poll();
        if (err != ESP_OK) return err;
        // Readiness is hardware-driven. Yield for scheduler fairness using
        // the existing burner policy, not a fixed per-packet millisecond wait.
        burner_task_yield_if_due();
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t bacon_cpld_try_transfer_locked(uint8_t operation, uint32_t byte_address,
    void *data, size_t size, uint16_t page_bytes, uint32_t timeout_ms, bool *used)
{
    if (!used) return ESP_ERR_INVALID_ARG;
    *used = false;
    ag32_link_preference_t preference = ag32_mcu_link_get_preference();
    if (preference != AG32_LINK_PREFERENCE_CPLD && preference != AG32_LINK_PREFERENCE_AUTO)
        return ESP_OK;
    if (s_available == 0 && preference == AG32_LINK_PREFERENCE_AUTO) return ESP_OK;
    *used = true;
    bool reading = operation == BACON_CPLD_GBA_READ || operation == BACON_CPLD_GB_READ;
    bool programming = operation == BACON_CPLD_GBA_PROGRAM || operation == BACON_CPLD_GB_PROGRAM;
    bool gb = operation >= BACON_CPLD_GB_READ;
    uint32_t window = gb ? 65536u : 33554432u;
    if (operation < BACON_CPLD_GBA_READ || operation > BACON_CPLD_GB_PROGRAM ||
        !data || !size || byte_address >= window || size > window - byte_address ||
        (!gb && ((byte_address | size) & 1)) ||
        (programming && (page_bytes < 2 || page_bytes > (gb ? 256 : 1024) ||
                        (page_bytes & (page_bytes - 1))))) return ESP_ERR_INVALID_ARG;

    if (operation == BACON_CPLD_GBA_PROGRAM && s_write_profile.mode == 2)
        return bsc2_program_locked(byte_address, data, size, page_bytes, timeout_ms);
    bool profile_write = operation == BACON_CPLD_GBA_PROGRAM && s_write_profile.mode == 1;

    memset(&s_stats, 0, sizeof(s_stats));
    int64_t started = esp_timer_get_time();
    esp_err_t err = enter();
    if (err != ESP_OK) {
        (void)mode_key(BACON_CPLD_EXIT_MAGIC);
        if (err == ESP_ERR_NOT_SUPPORTED && preference == AG32_LINK_PREFERENCE_AUTO) {
            s_available = 0; *used = false; return ESP_OK;
        }
        return err;
    }
    s_available = 1;
    ag32_link_mark_cpld_active();
    uint8_t descriptor[BACON_CPLD_DESCRIPTOR_BYTES];
    _Alignas(4) uint8_t wire[BACON_CPLD_BLOCK_BYTES + 8];
    bacon_cpld_descriptor(descriptor, operation, byte_address, size, programming ? page_bytes : 0);
    int64_t deadline = esp_timer_get_time() + (int64_t)(timeout_ms ? timeout_ms : 2000) * 1000;
    err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, descriptor, NULL, sizeof(descriptor));
    if (err == ESP_OK) err = wait_flag(BACON_CPLD_CONFIGURED, size, deadline);
    size_t transferred = 0;
    while (err == ESP_OK && transferred < size) {
        deadline = esp_timer_get_time() + (int64_t)(timeout_ms ? timeout_ms : 2000) * 1000;
        size_t count = size - transferred;
        size_t limit = programming ? page_bytes - ((byte_address + transferred) & (page_bytes - 1)) :
            (reading ? BACON_CPLD_READ_BYTES : BACON_CPLD_BLOCK_BYTES);
        if (count > limit) count = limit;
        size_t padded = (count + 3u) & ~(size_t)3u;
        int64_t stage = profile_write ? esp_timer_get_time() : 0;
        err = wait_flag(reading ? BACON_CPLD_TX_READY : BACON_CPLD_RX_READY, size, deadline);
        if (profile_write) s_write_profile.ready_us += esp_timer_get_time() - stage;
        if (err != ESP_OK) break;
        if (transferred == 0) s_stats.first_ready_us = esp_timer_get_time() - started;
        if (reading) {
            err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_1, s_read_clocks, wire, padded + 8);
            if (err == ESP_OK) {
                uint32_t computed = esp_rom_crc32_le(0, wire + 4, count);
                uint32_t received = ag32_mcu_read_le32(wire + 4 + padded);
                if (computed != received) {
                    ESP_LOGE(TAG, "read CRC @0x%08" PRIx32 " bytes=%u computed=%08" PRIx32 " received=%08" PRIx32,
                        byte_address + (uint32_t)transferred, (unsigned)count, computed, received);
                    err = ESP_ERR_INVALID_CRC;
                }
            }
            if (err == ESP_OK) memcpy((uint8_t *)data + transferred, wire + 4, count);
        } else {
            stage = profile_write ? esp_timer_get_time() : 0;
            memcpy(wire, (const uint8_t *)data + transferred, count);
            memset(wire + count, 0, padded - count);
            ag32_mcu_write_le32(wire + padded, esp_rom_crc32_le(0, wire, count));
            if (profile_write) { s_write_profile.prepare_us += esp_timer_get_time() - stage; stage = esp_timer_get_time(); }
            err = burner_spi_transfer_cs(BURNER_SPI_CS_MODE_0, wire, NULL, padded + 4);
            if (profile_write) s_write_profile.tx_us += esp_timer_get_time() - stage;
        }
        if (err == ESP_OK) { transferred += count; ++s_stats.packets; }
        if (err == ESP_OK) err = burner_cancel_poll();
    }
    deadline = esp_timer_get_time() + (int64_t)(timeout_ms ? timeout_ms : 2000) * 1000;
    int64_t done_start = profile_write ? esp_timer_get_time() : 0;
    if (err == ESP_OK) err = wait_flag(BACON_CPLD_DONE, size, deadline);
    if (profile_write) s_write_profile.done_us += esp_timer_get_time() - done_start;
    esp_err_t exit_err = mode_key(BACON_CPLD_EXIT_MAGIC);
    s_stats.elapsed_us = esp_timer_get_time() - started;
    if (profile_write) {
        ++s_write_profile.streams;
        s_write_profile.elapsed_us += s_stats.elapsed_us;
        s_write_profile.packets += s_stats.packets;
        s_write_profile.status_polls += s_stats.status_polls;
        if (err == ESP_OK && exit_err == ESP_OK) s_write_profile.bytes += size;
    }
    return err == ESP_OK ? exit_err : err;
}

#include "bacon_cpld_read_experiment.inc"
#include "bacon_cpld_write_experiment.inc"

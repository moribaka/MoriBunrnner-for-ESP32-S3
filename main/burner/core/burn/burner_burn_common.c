#include "../burner_source_reader.h"

/* Burn pipeline helpers shared by MBC5/GBC and GBA job paths. */

static void burner_emit_progress_cb(int progress, uint32_t processed)
{
    if (s_receive_cb != NULL) {
        uint8_t cb_payload[4];
        cb_payload[0] = (uint8_t)progress;
        cb_payload[1] = (uint8_t)((processed >> 8) & 0xFF);
        cb_payload[2] = (uint8_t)(processed & 0xFF);
        cb_payload[3] = 0;
        s_receive_cb(cb_payload, sizeof(cb_payload));
    }
}

esp_err_t burner_bacon_mbc5_read_block(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0;
    esp_err_t err;

    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = (uint16_t)(rom_addr >> 14);
        uint16_t bank_off = (uint16_t)(rom_addr & 0x3FFFu);
        uint16_t cart_addr;
        size_t remain = len - copied;
        size_t bank_remain = 0x4000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        if (bank == 0u) {
            cart_addr = bank_off;
        } else {
            cart_addr = (uint16_t)(0x4000u + bank_off);
        }

        err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }
        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_read_block_program_window(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0u;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = 0u;
        uint16_t cart_addr = 0u;
        uint32_t bank_off = 0u;
        size_t remain = len - copied;
        size_t bank_remain;
        size_t chunk;

        burner_mbc5_addr_to_program_window(rom_addr, &bank, &cart_addr, &bank_off);
        bank_remain = BURN_MBC5_ROM_BANK_BYTES - bank_off;
        chunk = (remain < bank_remain) ? remain : bank_remain;
        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }
        copied += chunk;
    }

    return ESP_OK;
}

esp_err_t burner_bacon_mbc5_read_block_hoststyle(uint8_t *out, size_t len, uint32_t offset)
{
    size_t copied = 0u;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        uint32_t rom_addr = offset + (uint32_t)copied;
        uint16_t bank = (uint16_t)(rom_addr >> 14);
        uint16_t bank_off = (uint16_t)(rom_addr & 0x3FFFu);
        uint16_t cart_addr;
        size_t remain = len - copied;
        size_t bank_remain = 0x4000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }

        if (bank != s_cart_ctx.current_bank) {
            err = burner_bacon_mbc5_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            s_cart_ctx.current_bank = bank;
        }

        if (bank == 0u) {
            cart_addr = bank_off;
        } else {
            cart_addr = (uint16_t)(0x4000u + bank_off);
        }

        err = burner_bacon_gbc_read_stream_hoststyle(cart_addr, out + copied, chunk);
        if (err != ESP_OK) {
            return err;
        }

        copied += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_ram_write_block(
    const uint8_t *data,
    size_t len,
    uint32_t offset,
    bool fram_mode,
    uint8_t fram_latency)
{
    size_t written = 0;
    uint8_t current_bank = 0xFFu;
    esp_err_t err;

    if (data == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (written < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t ram_addr = offset + (uint32_t)written;
        uint8_t bank = (uint8_t)(ram_addr >> 13);
        uint16_t bank_off = (uint16_t)(ram_addr & 0x1FFFu);
        uint16_t cart_addr = (uint16_t)(0xA000u + bank_off);
        size_t remain = len - written;
        size_t bank_remain = 0x2000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_WRITE_MAX_BYTES) {
            chunk = BURN_CART_WRITE_MAX_BYTES;
        }

        if (bank != current_bank) {
            err = burner_bacon_mbc5_ram_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        if (fram_mode) {
            err = burner_bacon_gbc_write_for_fram(cart_addr, data + written, chunk, fram_latency);
        } else {
            err = burner_bacon_gbc_write(cart_addr, data + written, chunk);
        }
        if (err != ESP_OK) {
            return err;
        }

        written += chunk;
    }

    return ESP_OK;
}

static esp_err_t burner_bacon_mbc5_ram_read_block(
    uint8_t *out,
    size_t len,
    uint32_t offset,
    bool fram_mode,
    uint8_t fram_latency)
{
    size_t copied = 0;
    uint8_t current_bank = 0xFFu;
    esp_err_t err;

    if (out == NULL || len == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    while (copied < len) {
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            return err;
        }
        uint32_t ram_addr = offset + (uint32_t)copied;
        uint8_t bank = (uint8_t)(ram_addr >> 13);
        uint16_t bank_off = (uint16_t)(ram_addr & 0x1FFFu);
        uint16_t cart_addr = (uint16_t)(0xA000u + bank_off);
        size_t remain = len - copied;
        size_t bank_remain = 0x2000u - bank_off;
        size_t chunk = (remain < bank_remain) ? remain : bank_remain;

        if (chunk > BURN_CART_READ_MAX_BYTES) {
            chunk = BURN_CART_READ_MAX_BYTES;
        }

        if (bank != current_bank) {
            err = burner_bacon_mbc5_ram_switch_bank(bank);
            if (err != ESP_OK) {
                return err;
            }
            current_bank = bank;
        }

        if (fram_mode) {
            err = burner_bacon_gbc_read_for_fram(cart_addr, out + copied, chunk, fram_latency);
        } else {
            err = burner_bacon_gbc_read(cart_addr, out + copied, chunk);
        }
        if (err != ESP_OK) {
            return err;
        }

        copied += chunk;
    }

    return ESP_OK;
}

esp_err_t burner_ensure_dump_dir(void)
{
    struct stat st;

    if (stat(DUMP_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(DUMP_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t burner_ensure_rom_output_dir(void)
{
    struct stat st;

    if (stat(ROM_OUTPUT_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(ROM_OUTPUT_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}


static esp_err_t burner_replace_file(const char *tmp_path, const char *target_path)
{
    if (tmp_path == NULL || target_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (rename(tmp_path, target_path) == 0) {
        (void)burner_apply_current_file_mtime(target_path, NULL);
        return ESP_OK;
    }
    if (errno == EEXIST) {
        if (unlink(target_path) == 0 && rename(tmp_path, target_path) == 0) {
            (void)burner_apply_current_file_mtime(target_path, NULL);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static uint8_t *burner_attach_stdio_buffer(FILE *fp, size_t preferred_size)
{
    uint8_t *buf;

    if (fp == NULL || preferred_size == 0u) {
        return NULL;
    }

    buf = (uint8_t *)heap_caps_malloc(preferred_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)malloc(preferred_size);
    }
    if (buf == NULL) {
        buf = (uint8_t *)heap_caps_malloc(preferred_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        return NULL;
    }

    if (setvbuf(fp, (char *)buf, _IOFBF, preferred_size) != 0) {
        free(buf);
        return NULL;
    }

    return buf;
}


typedef esp_err_t (*burner_dump_read_block_fn_t)(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job);

static esp_err_t burner_dump_read_block_mbc5(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job)
{
    esp_err_t err;

    (void)job;

    burner_spi_lock_take();
    /* Use a dedicated Bacon-style streaming ROM read for MBC5 dump/export. */
    err = burner_bacon_mbc5_read_block_hoststyle(dst, len, addr);
    burner_spi_lock_give();

    return err;
}

static esp_err_t burner_dump_read_block_gba(
    uint8_t *dst,
    size_t len,
    uint32_t addr,
    const burner_task_param_t *job)
{
    esp_err_t err;

    burner_spi_lock_take();
    /* Use Bacon hoststyle ROM reads for GBA dump/export. */
    err = burner_bacon_gba_verify_read_block_hoststyle(dst, len, addr, burner_is_gba_multi_card(job));
    burner_spi_lock_give();

    return err;
}

#include "burner_dump_stream.c"
#include "burner_verify_stream.c"

typedef struct {
    FILE *fp;
    uint8_t *dst;
    size_t bytes;
    size_t read_len;
    esp_err_t err;
    SemaphoreHandle_t done;
} burner_tf_prefetch_ctx_t;

typedef enum {
    BURNER_ERASE_OP_MBC5_RANGE = 0,
    BURNER_ERASE_OP_GBA_RANGE,
    BURNER_ERASE_OP_MBC5_CHIP,
    BURNER_ERASE_OP_GBA_CHIP,
} burner_erase_op_t;

typedef struct {
    burner_erase_op_t op;
    uint32_t addr_begin;
    uint32_t addr_end;
    uint32_t sector_size;
    bool gba_multi;
    bool sample_blank_sectors;
    bool erase_always;
    esp_err_t err;
    SemaphoreHandle_t done;
} burner_erase_task_ctx_t;

typedef struct {
    FILE *fp;
    uint8_t *dst;
    size_t bytes;
    size_t read_len;
    esp_err_t err;
    bool stop;
    bool running;
    TaskHandle_t task;
    SemaphoreHandle_t request;
    SemaphoreHandle_t done;
} burner_tf_reader_ctx_t;

static uint32_t s_tf_reader_source_size;

void burner_tf_reader_set_source_size(uint32_t source_size)
{
    s_tf_reader_source_size = source_size;
}

static esp_err_t burner_tf_read_exact(FILE *fp, uint8_t *dst, size_t bytes)
{
    if (fp == NULL || dst == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    return burner_source_read_exact(fp, dst, bytes, s_tf_reader_source_size);
}

static esp_err_t burner_tf_write_exact(burner_tf_writer_ctx_t *ctx)
{
    int fd = ctx->fd;
    const uint8_t *src = ctx->src;
    size_t bytes = ctx->bytes;
    size_t offset = 0u;
    uint64_t write_start_us;
    uint64_t write_elapsed_us;

    if (fd < 0 || src == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }

    write_start_us = (uint64_t)esp_timer_get_time();
    while (offset < bytes) {
        size_t count = bytes - offset;
        if (count > 16384u) count = 16384u;
        /* SDMMC on S3 cannot DMA from PSRAM. An internal 16 KiB block
         * keeps FatFS writes multi-sector instead of 512-byte bounces. */
        memcpy(ctx->dma_buf, src + offset, count);
        size_t done = 0;
        while (done < count) {
            ssize_t written = write(fd, ctx->dma_buf + done, count - done);
            if (written <= 0) return ESP_FAIL;
            done += (size_t)written;
        }
        offset += count;
        if (offset < bytes) {
            if (burner_cancel_is_requested()) {
                return ESP_ERR_INVALID_STATE;
            }
            if (usb_msc_tf_in_use_by_host()) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    write_elapsed_us = (uint64_t)esp_timer_get_time();
    if (offset == bytes && write_elapsed_us > 0u) {
        burner_status_record_dump_write((uint32_t)bytes, write_elapsed_us - write_start_us);
    }

    return ESP_OK;
}

static void burner_tf_prefetch_task(void *arg)
{
    burner_tf_prefetch_ctx_t *ctx = (burner_tf_prefetch_ctx_t *)arg;
    uint64_t read_start_us = 0u;
    uint64_t read_elapsed_us = 0u;

    if (ctx == NULL || ctx->fp == NULL || ctx->dst == NULL || ctx->bytes == 0u || ctx->done == NULL) {
        if (ctx != NULL) {
            ctx->err = ESP_ERR_INVALID_ARG;
        }
        vTaskDelete(NULL);
        return;
    }

    if (usb_msc_tf_in_use_by_host()) {
        ctx->err = ESP_ERR_INVALID_STATE;
    } else {
        read_start_us = (uint64_t)esp_timer_get_time();
        ctx->err = burner_tf_read_exact(ctx->fp, ctx->dst, ctx->bytes);
        ctx->read_len = ctx->err == ESP_OK ? ctx->bytes : 0;
        read_elapsed_us = (uint64_t)esp_timer_get_time() - read_start_us;
        if (ctx->err == ESP_OK && ctx->read_len > 0u && read_elapsed_us > 0u) {
            burner_status_record_tf_to_psram_copy((uint32_t)ctx->read_len, read_elapsed_us);
            burner_gba_chis_diag_add_tf_read(read_elapsed_us);
        }
    }

    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

void burner_tf_writer_task(void *arg)
{
    burner_tf_writer_ctx_t *ctx = (burner_tf_writer_ctx_t *)arg;

    if (ctx == NULL || ctx->fd < 0 || ctx->request == NULL || ctx->done == NULL) {
        if (ctx != NULL) {
            ctx->err = ESP_ERR_INVALID_ARG;
        }
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        xSemaphoreTake(ctx->request, portMAX_DELAY);
        if (ctx->stop) {
            ctx->err = ESP_OK;
            xSemaphoreGive(ctx->done);
            break;
        }

        ctx->written = 0u;
        if (ctx->src == NULL || ctx->bytes == 0u) {
            ctx->err = ESP_ERR_INVALID_ARG;
        } else {
            ctx->err = burner_tf_write_exact(ctx);
            if (ctx->err == ESP_OK) {
                ctx->written = ctx->bytes;
            }
        }
        xSemaphoreGive(ctx->done);
    }

    vTaskDelete(NULL);
}

static void burner_erase_task(void *arg)
{
    burner_erase_task_ctx_t *ctx = (burner_erase_task_ctx_t *)arg;

    if (ctx == NULL || ctx->done == NULL) {
        vTaskDelete(NULL);
        return;
    }

    burner_spi_lock_take();
    switch (ctx->op) {
    case BURNER_ERASE_OP_MBC5_RANGE:
        ctx->err = burner_bacon_mbc5_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_GBA_RANGE:
        ctx->err = burner_bacon_gba_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->gba_multi,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_MBC5_CHIP:
        ctx->err = burner_bacon_mbc5_chip_erase();
        break;
    case BURNER_ERASE_OP_GBA_CHIP:
        ctx->err = burner_bacon_gba_chip_erase();
        break;
    default:
        ctx->err = ESP_ERR_INVALID_ARG;
        break;
    }
    burner_spi_lock_give();

    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static esp_err_t burner_erase_exec_in_current_task(burner_erase_task_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    burner_spi_lock_take();
    switch (ctx->op) {
    case BURNER_ERASE_OP_MBC5_RANGE:
        ctx->err = burner_bacon_mbc5_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_GBA_RANGE:
        ctx->err = burner_bacon_gba_erase_range(
            ctx->addr_begin,
            ctx->addr_end,
            ctx->sector_size,
            ctx->gba_multi,
            ctx->sample_blank_sectors,
            ctx->erase_always);
        break;
    case BURNER_ERASE_OP_MBC5_CHIP:
        ctx->err = burner_bacon_mbc5_chip_erase();
        break;
    case BURNER_ERASE_OP_GBA_CHIP:
        ctx->err = burner_bacon_gba_chip_erase();
        break;
    default:
        ctx->err = ESP_ERR_INVALID_ARG;
        break;
    }
    burner_spi_lock_give();
    return ctx->err;
}

static esp_err_t burner_run_erase_task(burner_erase_task_ctx_t *ctx)
{
    BaseType_t create_ret;

    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_burn_core_cfg.erase_core == BURNER_CORE_AFFINITY_AUTO) {
        return burner_erase_exec_in_current_task(ctx);
    }

    ctx->done = xSemaphoreCreateBinary();
    if (ctx->done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    create_ret = burner_create_task_with_affinity(
        burner_erase_task,
        "burn_erase",
        4096,
        ctx,
        4,
        NULL,
        s_burn_core_cfg.erase_core);
    if (create_ret != pdPASS) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(ctx->done, portMAX_DELAY);
    vSemaphoreDelete(ctx->done);
    ctx->done = NULL;
    return ctx->err;
}

static esp_err_t burner_run_mbc5_range_erase(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool sample_blank_sectors,
    bool erase_always)
{
    uint32_t planned_sectors = burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t planned_bytes = burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_MBC5_RANGE,
        .addr_begin = addr_begin,
        .addr_end = addr_end,
        .sector_size = sector_size,
        .gba_multi = false,
        .sample_blank_sectors = sample_blank_sectors,
        .erase_always = erase_always,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_gba_range_erase(
    uint32_t addr_begin,
    uint32_t addr_end,
    uint32_t sector_size,
    bool gba_multi,
    bool sample_blank_sectors,
    bool erase_always)
{
    uint32_t planned_sectors = burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t planned_bytes = burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, addr_begin, addr_end);
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_GBA_RANGE,
        .addr_begin = addr_begin,
        .addr_end = addr_end,
        .sector_size = sector_size,
        .gba_multi = gba_multi,
        .sample_blank_sectors = sample_blank_sectors,
        .erase_always = erase_always,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_mbc5_chip_erase(void)
{
    uint32_t planned_sectors =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : burner_erase_sector_count_from_bytes(s_cart_ctx.device_size, s_cart_ctx.sector_size);
    uint32_t planned_bytes =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : s_cart_ctx.device_size;
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_MBC5_CHIP,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static esp_err_t burner_run_gba_chip_erase(void)
{
    uint32_t planned_sectors =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_sector_count_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : burner_erase_sector_count_from_bytes(s_cart_ctx.device_size, s_cart_ctx.sector_size);
    uint32_t planned_bytes =
        (burner_nor_geometry_is_valid(&s_cart_ctx.geometry) && s_cart_ctx.device_size > 0u)
            ? burner_nor_geometry_erase_bytes_from_range(&s_cart_ctx.geometry, 0u, s_cart_ctx.device_size - 1u)
            : s_cart_ctx.device_size;
    uint32_t tracked_sector_size = burner_nor_geometry_report_sector_size(&s_cart_ctx.geometry);
    burner_erase_task_ctx_t ctx = {
        .op = BURNER_ERASE_OP_GBA_CHIP,
        .err = ESP_FAIL,
        .done = NULL,
    };
    burner_status_begin_erase_phase(planned_sectors, planned_bytes, tracked_sector_size);
    esp_err_t err = burner_run_erase_task(&ctx);

    if (err == ESP_OK) {
        burner_status_record_erase_sectors(planned_sectors, tracked_sector_size);
    }
    return err;
}

static void burner_tf_reader_task(void *arg)
{
    burner_tf_reader_ctx_t *ctx = (burner_tf_reader_ctx_t *)arg;

    if (ctx == NULL || ctx->request == NULL || ctx->done == NULL || ctx->fp == NULL) {
        if (ctx != NULL) {
            ctx->running = false;
        }
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        xSemaphoreTake(ctx->request, portMAX_DELAY);
        if (ctx->stop) {
            ctx->running = false;
            xSemaphoreGive(ctx->done);
            break;
        }

        if (ctx->dst == NULL || ctx->bytes == 0u) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_ARG;
            xSemaphoreGive(ctx->done);
            continue;
        }

        if (burner_cancel_is_requested()) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(ctx->done);
            continue;
        }

        if (usb_msc_tf_in_use_by_host()) {
            ctx->read_len = 0u;
            ctx->err = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(ctx->done);
            continue;
        }

        ctx->err = burner_tf_read_exact(ctx->fp, ctx->dst, ctx->bytes);
        ctx->read_len = ctx->err == ESP_OK ? ctx->bytes : 0;
        xSemaphoreGive(ctx->done);
    }

    vTaskDelete(NULL);
}

static esp_err_t burner_tf_reader_start(burner_tf_reader_ctx_t *ctx, FILE *fp)
{
    BaseType_t create_ret;

    if (ctx == NULL || fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fp = fp;

    if (s_burn_core_cfg.tf_core == BURNER_CORE_AFFINITY_AUTO) {
        return ESP_OK;
    }

    ctx->request = xSemaphoreCreateBinary();
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->request == NULL || ctx->done == NULL) {
        if (ctx->request != NULL) {
            vSemaphoreDelete(ctx->request);
            ctx->request = NULL;
        }
        if (ctx->done != NULL) {
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ctx->running = true;
    create_ret = burner_create_task_with_affinity(
        burner_tf_reader_task,
        "tf_reader",
        4096,
        ctx,
        4,
        &ctx->task,
        s_burn_core_cfg.tf_core);
    if (create_ret != pdPASS) {
        ctx->running = false;
        vSemaphoreDelete(ctx->request);
        vSemaphoreDelete(ctx->done);
        ctx->request = NULL;
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t burner_tf_reader_read(burner_tf_reader_ctx_t *ctx, uint8_t *dst, size_t bytes)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_burn_core_cfg.tf_core == BURNER_CORE_AFFINITY_AUTO || !ctx->running || ctx->request == NULL ||
        ctx->done == NULL) {
        return burner_tf_read_exact(ctx->fp, dst, bytes);
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx->dst = dst;
    ctx->bytes = bytes;
    ctx->read_len = 0u;
    ctx->err = ESP_FAIL;
    xSemaphoreGive(ctx->request);
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    if (ctx->err != ESP_OK) {
        return ctx->err;
    }
    return (ctx->read_len == bytes) ? ESP_OK : ESP_FAIL;
}

static void burner_tf_reader_stop(burner_tf_reader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->running && ctx->request != NULL && ctx->done != NULL) {
        ctx->stop = true;
        xSemaphoreGive(ctx->request);
        xSemaphoreTake(ctx->done, portMAX_DELAY);
    }

    if (ctx->request != NULL) {
        vSemaphoreDelete(ctx->request);
        ctx->request = NULL;
    }
    if (ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    ctx->running = false;
}

esp_err_t burner_tf_writer_start(burner_tf_writer_ctx_t *ctx, int fd)
{
    BaseType_t create_ret;

    if (ctx == NULL || fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = fd;

    ctx->dma_buf = heap_caps_malloc(16384u, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (ctx->dma_buf == NULL) return ESP_ERR_NO_MEM;

    ctx->request = xSemaphoreCreateBinary();
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->request == NULL || ctx->done == NULL) {
        free(ctx->dma_buf);
        ctx->dma_buf = NULL;
        if (ctx->request != NULL) {
            vSemaphoreDelete(ctx->request);
            ctx->request = NULL;
        }
        if (ctx->done != NULL) {
            vSemaphoreDelete(ctx->done);
            ctx->done = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ctx->running = true;
    create_ret = burner_create_task_with_affinity(
        burner_tf_writer_task,
        "tf_writer",
        4096,
        ctx,
        4,
        &ctx->task,
        s_burn_core_cfg.tf_core);
    if (create_ret != pdPASS) {
        ctx->running = false;
        free(ctx->dma_buf);
        ctx->dma_buf = NULL;
        vSemaphoreDelete(ctx->request);
        vSemaphoreDelete(ctx->done);
        ctx->request = NULL;
        ctx->done = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t burner_tf_writer_submit(burner_tf_writer_ctx_t *ctx, const uint8_t *src, size_t bytes)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->running || ctx->request == NULL || ctx->done == NULL || ctx->inflight) {
        return ESP_ERR_INVALID_STATE;
    }

    if (src == NULL || bytes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->src = src;
    ctx->bytes = bytes;
    ctx->written = 0u;
    ctx->err = ESP_FAIL;
    ctx->inflight = true;
    xSemaphoreGive(ctx->request);
    return ESP_OK;
}

esp_err_t burner_tf_writer_wait(burner_tf_writer_ctx_t *ctx)
{
    uint64_t wait_start_us;
    uint64_t wait_end_us;

    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->running || ctx->request == NULL || ctx->done == NULL || !ctx->inflight) {
        return ESP_ERR_INVALID_STATE;
    }

    wait_start_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    ctx->inflight = false;
    wait_end_us = (uint64_t)esp_timer_get_time();
    if (wait_end_us > wait_start_us) {
        burner_status_record_dump_wait(wait_end_us - wait_start_us);
    }
    if (ctx->err != ESP_OK) {
        return ctx->err;
    }
    return (ctx->written == ctx->bytes) ? ESP_OK : ESP_FAIL;
}

void burner_tf_writer_stop(burner_tf_writer_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->running && ctx->request != NULL && ctx->done != NULL) {
        if (ctx->inflight) (void)burner_tf_writer_wait(ctx);
        ctx->stop = true;
        xSemaphoreGive(ctx->request);
        xSemaphoreTake(ctx->done, portMAX_DELAY);
    }

    if (ctx->request != NULL) {
        vSemaphoreDelete(ctx->request);
        ctx->request = NULL;
    }
    if (ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    ctx->running = false;
    free(ctx->dma_buf);
    ctx->dma_buf = NULL;
}

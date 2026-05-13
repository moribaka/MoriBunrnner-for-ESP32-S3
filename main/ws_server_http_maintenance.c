#include "ws_server_http_maintenance.h"
#include "ws_server_http_device.h"

esp_err_t burner_web_main_upload_handler(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len > WEB_MAIN_UPLOAD_MAX_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "main.html too large");
    }
    return burner_web_upload_file(req, "main.html", false);
}

esp_err_t burner_web_upload_handler(httpd_req_t *req)
{
    return burner_web_upload_file(req, NULL, true);
}

static void burner_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

void burner_schedule_restart(void)
{
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCore(burner_restart_task, "web_reboot", 2048, NULL, 3, &task, 0) != pdPASS) {
        ESP_LOGW(BURNER_TAG, "schedule reboot task failed");
    }
}

esp_err_t burner_fw_upgrade_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition = NULL;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    uint8_t *buf = NULL;
    int remaining = 0;
    uint32_t written_total = 0;
    esp_err_t err;
    char resp[240];
    int n;

    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty firmware body");
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }
    if ((size_t)req->content_len > update_partition->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware too large for OTA slot");
    }

    err = esp_ota_begin(update_partition, (size_t)req->content_len, &ota_handle);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }
    ota_started = true;

    buf = (uint8_t *)malloc(FW_UPLOAD_CHUNK_SIZE);
    if (buf == NULL) {
        esp_ota_end(ota_handle);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    burner_cancel_reset();
    burner_status_update(
        BURNER_STATE_RECEIVING,
        0,
        0,
        (uint32_t)req->content_len,
        "firmware upload started",
        "moriburnner.bin",
        "/ota");

    remaining = req->content_len;
    while (remaining > 0) {
        int to_recv = remaining > FW_UPLOAD_CHUNK_SIZE ? FW_UPLOAD_CHUNK_SIZE : remaining;
        int recv_len;

        if (burner_cancel_is_requested()) {
            free(buf);
            esp_ota_end(ota_handle);
            burner_status_update(
                BURNER_STATE_CANCELLED,
                burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
                written_total,
                (uint32_t)req->content_len,
                "firmware upload cancelled",
                "moriburnner.bin",
                "/ota");
            burner_cancel_reset();
            return httpd_resp_send_custom_err(req, "409 Conflict", "firmware upload cancelled");
        }

        recv_len = httpd_req_recv(req, (char *)buf, to_recv);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            free(buf);
            esp_ota_end(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware upload interrupted");
        }

        err = esp_ota_write(ota_handle, buf, (size_t)recv_len);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_end(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
        }

        written_total += (uint32_t)recv_len;
        remaining -= recv_len;
        burner_status_update(
            BURNER_STATE_RECEIVING,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "firmware uploading",
            "moriburnner.bin",
            "/ota");
    }

    if (burner_cancel_is_requested()) {
        free(buf);
        esp_ota_end(ota_handle);
        burner_status_update(
            BURNER_STATE_CANCELLED,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "firmware upload cancelled",
            "moriburnner.bin",
            "/ota");
        burner_cancel_reset();
        return httpd_resp_send_custom_err(req, "409 Conflict", "firmware upload cancelled");
    }

    free(buf);

    if (ota_started) {
        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
        }
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"written\":%" PRIu32 ",\"partition\":\"%s\",\"rebooting\":true}",
        written_total,
        update_partition->label);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    burner_schedule_restart();
    burner_status_update(
        BURNER_STATE_DONE,
        100,
        written_total,
        written_total,
        "firmware upload complete, rebooting",
        "moriburnner.bin",
        "/ota");
    burner_cancel_reset();
    return burner_send_json(req, resp);
}

esp_err_t burner_mcu_probe_handler(httpd_req_t *req)
{
#if !MORI_SWD_ENABLE
    return burner_send_json(
        req,
        "{\"ok\":false,\"status\":\"disabled\",\"message\":\"SWD disabled on this hardware\"}");
#else
    mcu_debug_probe_result_t probe;
    mcu_debug_probe_options_t opts = {0};
    char resp[520];
    int n;
    esp_err_t err;

    (void)req;
    mcu_debug_get_default_probe_options(&opts);

    err = mcu_debug_probe_idcode_with_opts(&opts, &probe);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "probe execution failed");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":%s,\"status\":\"%s\",\"ack\":%u,\"idcode\":\"0x%08" PRIx32
        "\",\"parity_ok\":%s,\"do_reset\":%s,\"delay_us\":%" PRIu32
        ",\"seq_mode\":\"%s\",\"seq_used\":\"%s\",\"attempt_count\":%u"
        ",\"swap_clk_dio\":%s,\"turnaround_cycles\":%u,\"swdio_pull_mode\":\"%s\"}",
        (probe.status == MCU_DEBUG_PROBE_OK) ? "true" : "false",
        mcu_debug_probe_status_str(probe.status),
        probe.ack,
        probe.idcode,
        probe.parity_ok ? "true" : "false",
        opts.do_reset ? "true" : "false",
        opts.delay_us,
        mcu_debug_seq_mode_str(opts.seq_mode),
        mcu_debug_seq_mode_str((mcu_debug_seq_mode_t)probe.seq_used),
        probe.attempt_count,
        opts.swap_clk_dio ? "true" : "false",
        opts.turnaround_cycles,
        mcu_debug_pull_mode_str(opts.swdio_pull_mode));
    if (n < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
#endif
}

esp_err_t burner_cart_unlock_ppb_handler(httpd_req_t *req)
{
    char mode_arg[16] = {0};
    burner_cart_mode_t cart_mode = BURNER_CART_MODE_GBA;
    burner_ppb_unlock_report_t report;
    char id_hex[32] = {0};
    char resp[640];
    bool is_busy = false;
    esp_err_t err;
    int n;

    if (!burner_get_query_arg(req, "mode", mode_arg, sizeof(mode_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode query");
    }
    if (mode_arg[0] != '\0' && !burner_parse_cart_mode_text(mode_arg, &cart_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        is_busy = (s_burn_task != NULL);
        xSemaphoreGive(s_status_lock);
    } else {
        is_busy = (s_burn_task != NULL);
    }
    if (is_busy) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "burn task is running");
    }

    err = burner_spi_init();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spi init failed");
    }

    memset(&report, 0, sizeof(report));
    burner_spi_lock_take();
    err = burner_cart_unlock_ppb_locked(cart_mode, &report);
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (cart_mode == BURNER_CART_MODE_GBA) {
        n = snprintf(
            id_hex,
            sizeof(id_hex),
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            report.gba_id[0],
            report.gba_id[1],
            report.gba_id[2],
            report.gba_id[3],
            report.gba_id[4],
            report.gba_id[5],
            report.gba_id[6],
            report.gba_id[7]);
        if (n <= 0 || n >= (int)sizeof(id_hex)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "id encode failed");
        }

        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":%s,\"mode\":\"gba\",\"power\":{\"v5\":false,\"v3\":true},"
            "\"id\":\"%s\",\"chip\":\"%s\",\"cfi_ok\":%s,"
            "\"device_size\":%" PRIu32 ",\"sector_size\":%" PRIu32 ",\"buffer_write\":%u,"
            "\"sector_count\":%" PRIu32 ",\"ppb_lock_status\":\"0x%04X\","
            "\"ppb_needs_unlock_before\":%" PRIu32 ",\"ppb_needs_unlock_after\":%" PRIu32 ","
            "\"message\":\"%s\"}",
            (err == ESP_OK) ? "true" : "false",
            id_hex,
            burner_gba_chip_name(report.gba_id),
            report.cfi_ok ? "true" : "false",
            report.device_size,
            report.sector_size,
            (unsigned)report.buffer_write_bytes,
            report.sector_count,
            (unsigned)report.gba_lock_status,
            report.ppb_needs_unlock_before,
            report.ppb_needs_unlock_after,
            (err == ESP_OK)
                ? ((report.ppb_needs_unlock_before == 0u && report.ppb_needs_unlock_after == 0u)
                       ? "PPB already unlocked"
                       : "PPB unlock completed")
                : ((err == ESP_ERR_NOT_SUPPORTED)
                       ? "PPB lock status does not allow erase"
                       : esp_err_to_name(err)));
    } else {
        n = snprintf(
            id_hex,
            sizeof(id_hex),
            "%02X %02X %02X %02X",
            report.mbc5_id[0],
            report.mbc5_id[1],
            report.mbc5_id[2],
            report.mbc5_id[3]);
        if (n <= 0 || n >= (int)sizeof(id_hex)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "id encode failed");
        }

        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":%s,\"mode\":\"mbc5\",\"power\":{\"v5\":true,\"v3\":false},"
            "\"id\":\"%s\",\"chip\":\"%s\",\"cfi_ok\":%s,"
            "\"device_size\":%" PRIu32 ",\"sector_size\":%" PRIu32 ",\"buffer_write\":%u,"
            "\"sector_count\":%" PRIu32 ",\"ppb_lock_status\":\"0x%02X\","
            "\"ppb_needs_unlock_before\":%" PRIu32 ",\"ppb_needs_unlock_after\":%" PRIu32 ","
            "\"message\":\"%s\"}",
            (err == ESP_OK) ? "true" : "false",
            id_hex,
            burner_mbc5_chip_name(report.mbc5_id),
            report.cfi_ok ? "true" : "false",
            report.device_size,
            report.sector_size,
            (unsigned)report.buffer_write_bytes,
            report.sector_count,
            (unsigned)report.mbc5_lock_status,
            report.ppb_needs_unlock_before,
            report.ppb_needs_unlock_after,
            (err == ESP_OK)
                ? ((report.ppb_needs_unlock_before == 0u && report.ppb_needs_unlock_after == 0u)
                       ? "PPB already unlocked"
                       : "PPB unlock completed")
                : ((err == ESP_ERR_NOT_SUPPORTED)
                       ? "PPB lock status does not allow erase"
                       : esp_err_to_name(err)));
    }
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

static esp_err_t burner_cart_debug_read_sample_locked(
    burner_cart_mode_t cart_mode,
    uint32_t device_size,
    uint32_t sample_addr,
    uint32_t requested_len,
    uint8_t *sample_buf,
    size_t sample_buf_size,
    uint32_t *actual_len_out,
    const char **error_out)
{
    size_t actual_len = 0u;
    esp_err_t err;
    bool is_multi = false;

    if (actual_len_out != NULL) {
        *actual_len_out = 0u;
    }
    if (error_out != NULL) {
        *error_out = NULL;
    }

    if (sample_buf == NULL || sample_buf_size == 0u || actual_len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (requested_len == 0u) {
        if (error_out != NULL) {
            *error_out = "sample length is zero";
        }
        return ESP_ERR_INVALID_ARG;
    }

    actual_len = requested_len;
    if (actual_len > sample_buf_size) {
        actual_len = sample_buf_size;
    }

    if (device_size > 0u) {
        if (sample_addr >= device_size) {
            if (error_out != NULL) {
                *error_out = "sample address out of range";
            }
            return ESP_ERR_INVALID_ARG;
        }
        if (((uint64_t)sample_addr + (uint64_t)actual_len) > (uint64_t)device_size) {
            actual_len = (size_t)(device_size - sample_addr);
        }
    }

    if (actual_len == 0u) {
        if (error_out != NULL) {
            *error_out = "sample length clipped to zero";
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (cart_mode == BURNER_CART_MODE_GBA) {
        is_multi = (device_size > BURN_GBA_BANK_BYTES) || (sample_addr >= BURN_GBA_BANK_BYTES);
        /* Match the dump/export path so sampled bytes reflect the real ROM contents. */
        err = burner_bacon_gba_verify_read_block_hoststyle(sample_buf, actual_len, sample_addr, is_multi);
    } else {
        /* Match the dump/export path so sampled bytes reflect the real ROM contents. */
        err = burner_bacon_mbc5_read_block_hoststyle(sample_buf, actual_len, sample_addr);
    }
    if (err != ESP_OK) {
        if (error_out != NULL) {
            *error_out = esp_err_to_name(err);
        }
        return err;
    }

    *actual_len_out = (uint32_t)actual_len;
    return ESP_OK;
}

esp_err_t burner_cart_id_debug_handler(httpd_req_t *req)
{
    char mode_arg[16] = {0};
    char sample_addr_arg[24] = {0};
    char sample_len_arg[16] = {0};
    burner_cart_mode_t cart_mode = BURNER_CART_MODE_GBA;
    uint8_t gba_id[8] = {0};
    uint8_t mbc5_id[4] = {0};
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    uint32_t sample_addr = 0u;
    uint32_t sample_len = BURNER_CART_ID_DEBUG_SAMPLE_DEFAULT;
    uint32_t sample_actual_len = 0u;
    uint8_t sample_buf[BURNER_CART_ID_DEBUG_SAMPLE_MAX] = {0};
    char id_hex[32];
    char sample_hex[BURNER_CART_ID_DEBUG_SAMPLE_MAX * 3 + 4];
    char resp[1024];
    const char *gba_cmd_mode = "word";
    const char *gba_cmd_data_lane = "low";
    const char *sample_error = "";
    bool cfi_ok = false;
    bool gba_id_looks_like_rom_header = false;
    bool is_busy = false;
    bool sample_ok = false;
    esp_err_t err;
    esp_err_t sample_err = ESP_OK;
    int n;

    if (!burner_get_query_arg(req, "mode", mode_arg, sizeof(mode_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode query");
    }
    if (mode_arg[0] != '\0' && !burner_parse_cart_mode_text(mode_arg, &cart_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }
    if (!burner_get_query_arg(req, "sample_addr", sample_addr_arg, sizeof(sample_addr_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid sample_addr query");
    }
    if (sample_addr_arg[0] != '\0' && !burner_parse_u32_text(sample_addr_arg, &sample_addr)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sample_addr must be uint32");
    }
    if (!burner_get_query_arg(req, "sample_len", sample_len_arg, sizeof(sample_len_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid sample_len query");
    }
    if (sample_len_arg[0] != '\0') {
        if (!burner_parse_u32_text(sample_len_arg, &sample_len) ||
            sample_len == 0u || sample_len > BURNER_CART_ID_DEBUG_SAMPLE_MAX) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sample_len out of range (1..64)");
        }
    }

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        is_busy = (s_burn_task != NULL);
        xSemaphoreGive(s_status_lock);
    } else {
        is_busy = (s_burn_task != NULL);
    }
    if (is_busy) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "burn task is running");
    }

    err = burner_spi_init();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spi init failed");
    }

    sample_hex[0] = '\0';
    burner_spi_lock_take();
    if (cart_mode == BURNER_CART_MODE_MBC5) {
        err = burner_bacon_mbc5_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_get_id(mbc5_id);
        }
        if (err == ESP_OK) {
            uint32_t cfi_device_size = 0;
            uint32_t cfi_sector_size = 0;
            uint16_t cfi_buffer_write_bytes = 0;

            if (burner_bacon_mbc5_get_cfi(&cfi_device_size, &cfi_sector_size, &cfi_buffer_write_bytes) == ESP_OK) {
                device_size = cfi_device_size;
                sector_size = cfi_sector_size;
                buffer_write_bytes = cfi_buffer_write_bytes;
                cfi_ok = true;
            } else {
                (void)burner_mbc5_geometry_from_id(mbc5_id, &device_size, &sector_size, &buffer_write_bytes);
            }
        }
    } else {
        err = burner_bacon_gba_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_gba_probe_locked(
                gba_id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok);
            if (err == ESP_OK) {
                gba_cmd_mode = burner_gba_cmd_addr_mode_name(s_cart_ctx.gba_cmd_addr_mode);
                gba_cmd_data_lane = burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane);
                gba_id_looks_like_rom_header = burner_gba_id_looks_like_rom_header(gba_id);
            }
        }
    }

    if (err == ESP_OK) {
        sample_err = burner_cart_debug_read_sample_locked(
            cart_mode,
            device_size,
            sample_addr,
            sample_len,
            sample_buf,
            sizeof(sample_buf),
            &sample_actual_len,
            &sample_error);
        sample_ok = (sample_err == ESP_OK);
        if (sample_ok) {
            burner_format_hex_bytes(sample_buf, sample_actual_len, sample_hex, sizeof(sample_hex));
        } else {
            ESP_LOGW(
                BURNER_TAG,
                "cart id debug sample failed mode=%s addr=0x%08" PRIX32 " len=%" PRIu32 ": %s",
                (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
                sample_addr,
                sample_len,
                (sample_error != NULL && sample_error[0] != '\0') ? sample_error : esp_err_to_name(sample_err));
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "cart id debug read failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cart id debug read failed");
    }

    if (cart_mode == BURNER_CART_MODE_MBC5) {
        n = snprintf(
            id_hex,
            sizeof(id_hex),
            "%02X %02X %02X %02X",
            mbc5_id[0],
            mbc5_id[1],
            mbc5_id[2],
            mbc5_id[3]);
        if (n <= 0 || n >= (int)sizeof(id_hex)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "id encode failed");
        }

        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":true,\"mode\":\"mbc5\",\"power\":{\"v5\":true,\"v3\":false},"
            "\"id\":\"%s\",\"chip\":\"%s\","
            "\"cfi_ok\":%s,\"device_size\":%" PRIu32 ",\"sector_size\":%" PRIu32 ",\"buffer_write\":%u,"
            "\"sample_ok\":%s,\"sample_addr\":%" PRIu32 ",\"sample_len\":%" PRIu32 ","
            "\"sample_hex\":\"%s\",\"sample_error\":\"%s\"}",
            id_hex,
            burner_mbc5_chip_name(mbc5_id),
            cfi_ok ? "true" : "false",
            device_size,
            sector_size,
            (unsigned)buffer_write_bytes,
            sample_ok ? "true" : "false",
            sample_addr,
            sample_actual_len,
            sample_ok ? sample_hex : "",
            sample_ok ? "" : ((sample_error != NULL) ? sample_error : "sample read failed"));
    } else {
        n = snprintf(
            id_hex,
            sizeof(id_hex),
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            gba_id[0],
            gba_id[1],
            gba_id[2],
            gba_id[3],
            gba_id[4],
            gba_id[5],
            gba_id[6],
            gba_id[7]);
        if (n <= 0 || n >= (int)sizeof(id_hex)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "id encode failed");
        }

        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":true,\"mode\":\"gba\",\"power\":{\"v5\":false,\"v3\":true},"
            "\"id\":\"%s\",\"chip\":\"%s\",\"cmd_mode\":\"%s\",\"cmd_data_lane\":\"%s\","
            "\"id_looks_like_rom_header\":%s,"
            "\"cfi_ok\":%s,\"device_size\":%" PRIu32 ",\"sector_size\":%" PRIu32 ",\"buffer_write\":%u,"
            "\"sample_ok\":%s,\"sample_addr\":%" PRIu32 ",\"sample_len\":%" PRIu32 ","
            "\"sample_hex\":\"%s\",\"sample_error\":\"%s\"}",
            id_hex,
            burner_gba_chip_name(gba_id),
            gba_cmd_mode,
            gba_cmd_data_lane,
            gba_id_looks_like_rom_header ? "true" : "false",
            cfi_ok ? "true" : "false",
            device_size,
            sector_size,
            (unsigned)buffer_write_bytes,
            sample_ok ? "true" : "false",
            sample_addr,
            sample_actual_len,
            sample_ok ? sample_hex : "",
            sample_ok ? "" : ((sample_error != NULL) ? sample_error : "sample read failed"));
    }
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_cart_id_handler(httpd_req_t *req)
{
    char mode_arg[16] = {0};
    const char *mode = NULL;
    uint8_t gba_id[8] = {0};
    uint8_t mbc5_id[4] = {0};
    uint32_t device_size = 0;
    uint32_t sector_size = 0;
    uint16_t buffer_write_bytes = 0;
    char id_hex[32];
    char resp[480];
    const char *gba_cmd_mode = "word";
    const char *gba_cmd_data_lane = "low";
    bool cfi_ok = false;
    bool gba_id_looks_like_rom_header = false;
    bool is_busy = false;
    esp_err_t err;
    int n;

    if (!burner_get_query_arg(req, "mode", mode_arg, sizeof(mode_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode query");
    }
    mode = (mode_arg[0] == '\0') ? "gba" : mode_arg;
    if (strcasecmp(mode, "gba") != 0 && strcasecmp(mode, "mbc5") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        is_busy = (s_burn_task != NULL);
        xSemaphoreGive(s_status_lock);
    } else {
        is_busy = (s_burn_task != NULL);
    }
    if (is_busy) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "burn task is running");
    }

    err = burner_spi_init();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spi init failed");
    }

    burner_spi_lock_take();
    if (strcasecmp(mode, "mbc5") == 0) {
        err = burner_bacon_mbc5_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_mbc5_get_id(mbc5_id);
        }
        if (err == ESP_OK) {
            uint32_t cfi_device_size = 0;
            uint32_t cfi_sector_size = 0;
            uint16_t cfi_buffer_write_bytes = 0;

            if (burner_bacon_mbc5_get_cfi(&cfi_device_size, &cfi_sector_size, &cfi_buffer_write_bytes) == ESP_OK) {
                device_size = cfi_device_size;
                sector_size = cfi_sector_size;
                buffer_write_bytes = cfi_buffer_write_bytes;
                cfi_ok = true;
            } else {
                (void)burner_mbc5_geometry_from_id(mbc5_id, &device_size, &sector_size, &buffer_write_bytes);
            }
        }
    } else {
        err = burner_bacon_gba_prepare_power();
        if (err == ESP_OK) {
            err = burner_bacon_gba_probe_locked(
                gba_id,
                &device_size,
                &sector_size,
                &buffer_write_bytes,
                &cfi_ok);
            if (err == ESP_OK) {
                gba_cmd_mode = burner_gba_cmd_addr_mode_name(s_cart_ctx.gba_cmd_addr_mode);
                gba_cmd_data_lane = burner_gba_cmd_data_lane_name(s_cart_ctx.gba_cmd_data_lane);
                gba_id_looks_like_rom_header = burner_gba_id_looks_like_rom_header(gba_id);
            }
        }
    }
    burner_bacon_restore_3v3_power();
    burner_spi_lock_give();

    if (err != ESP_OK) {
        ESP_LOGE(BURNER_TAG, "cart id read failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cart id read failed");
    }

    if (strcasecmp(mode, "mbc5") == 0) {
        n = snprintf(
            id_hex,
            sizeof(id_hex),
            "%02X %02X %02X %02X",
            mbc5_id[0],
            mbc5_id[1],
            mbc5_id[2],
            mbc5_id[3]);
        if (n <= 0 || n >= (int)sizeof(id_hex)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "id encode failed");
        }

        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":true,\"mode\":\"mbc5\",\"power\":{\"v5\":true,\"v3\":false},"
            "\"id\":\"%s\",\"chip\":\"%s\","
            "\"cfi_ok\":%s,\"device_size\":%" PRIu32 ",\"sector_size\":%" PRIu32 ",\"buffer_write\":%u}",
            id_hex,
            burner_mbc5_chip_name(mbc5_id),
            cfi_ok ? "true" : "false",
            device_size,
            sector_size,
            (unsigned)buffer_write_bytes);
    } else {
        n = snprintf(
            id_hex,
            sizeof(id_hex),
            "%02X %02X %02X %02X %02X %02X %02X %02X",
            gba_id[0],
            gba_id[1],
            gba_id[2],
            gba_id[3],
            gba_id[4],
            gba_id[5],
            gba_id[6],
            gba_id[7]);
        if (n <= 0 || n >= (int)sizeof(id_hex)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "id encode failed");
        }

        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":true,\"mode\":\"gba\",\"power\":{\"v5\":false,\"v3\":true},"
            "\"id\":\"%s\",\"chip\":\"%s\",\"cmd_mode\":\"%s\",\"cmd_data_lane\":\"%s\","
            "\"id_looks_like_rom_header\":%s,"
            "\"cfi_ok\":%s,\"device_size\":%" PRIu32 ",\"sector_size\":%" PRIu32 ",\"buffer_write\":%u}",
            id_hex,
            burner_gba_chip_name(gba_id),
            gba_cmd_mode,
            gba_cmd_data_lane,
            gba_id_looks_like_rom_header ? "true" : "false",
            cfi_ok ? "true" : "false",
            device_size,
            sector_size,
            (unsigned)buffer_write_bytes);
    }
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_status_handler(httpd_req_t *req)
{
    burner_status_t snap;
    char resp[2304];
    uint32_t task_ms;
    uint32_t erase_ms;
    uint32_t write_ms;
    uint32_t dump_read_ms;
    uint32_t dump_write_ms;
    uint32_t dump_wait_ms;
    uint32_t dump_finalize_ms;
    bool cart_sleeping;
    int n;

    burner_status_snapshot(&snap);
    cart_sleeping = s_bacon_idle_powered_down;
    task_ms = burner_us_to_ms_clamped(snap.task_elapsed_us);
    erase_ms = burner_us_to_ms_clamped(snap.erase_elapsed_us);
    write_ms = burner_us_to_ms_clamped(snap.write_elapsed_us);
    dump_read_ms = burner_us_to_ms_clamped(snap.dump_read_total_us);
    dump_write_ms = burner_us_to_ms_clamped(snap.dump_write_total_us);
    dump_wait_ms = burner_us_to_ms_clamped(snap.dump_wait_total_us);
    dump_finalize_ms = burner_us_to_ms_clamped(snap.dump_finalize_total_us);
    n = snprintf(
        resp,
        sizeof(resp),
        "{\"state\":\"%s\",\"progress\":%d,\"processed\":%" PRIu32 ",\"total\":%" PRIu32
        ",\"speed_current_bps\":%" PRIu32 ",\"speed_avg_bps\":%" PRIu32
        ",\"speed_min_bps\":%" PRIu32 ",\"speed_max_bps\":%" PRIu32 ",\"speed_warmup_ms\":1000"
        ",\"tf_to_psram_speed_current_bps\":%" PRIu32 ",\"tf_to_psram_speed_avg_bps\":%" PRIu32
        ",\"tf_to_psram_speed_min_bps\":%" PRIu32 ",\"tf_to_psram_speed_max_bps\":%" PRIu32
        ",\"dump_read_speed_current_bps\":%" PRIu32 ",\"dump_read_speed_avg_bps\":%" PRIu32
        ",\"dump_read_speed_min_bps\":%" PRIu32 ",\"dump_read_speed_max_bps\":%" PRIu32
        ",\"dump_write_speed_current_bps\":%" PRIu32 ",\"dump_write_speed_avg_bps\":%" PRIu32
        ",\"dump_write_speed_min_bps\":%" PRIu32 ",\"dump_write_speed_max_bps\":%" PRIu32
        ",\"mbc5_buffer_write_ok_count\":%" PRIu32 ",\"mbc5_buffer_fallback_count\":%" PRIu32
        ",\"erase_sector_count\":%" PRIu32 ",\"erase_sector_size\":%" PRIu32
        ",\"erase_active\":%s,\"erase_phase_done_sectors\":%" PRIu32 ",\"erase_phase_total_sectors\":%" PRIu32
        ",\"task_time_ms\":%" PRIu32 ",\"erase_time_ms\":%" PRIu32 ",\"write_time_ms\":%" PRIu32
        ",\"dump_read_time_ms\":%" PRIu32 ",\"dump_write_time_ms\":%" PRIu32
        ",\"dump_wait_time_ms\":%" PRIu32 ",\"dump_finalize_time_ms\":%" PRIu32
        ",\"spi_configured_hz\":%" PRIu32 ",\"spi_actual_hz\":%" PRIu32
        ",\"cart_auto_sleep_ms\":%u,\"cart_sleeping\":%s,\"cancel_requested\":%s"
        ",\"verify_sample_valid\":%s,\"verify_sample_equal\":%s"
        ",\"verify_sample_addr\":%" PRIu32 ",\"verify_sample_file_byte\":%u,\"verify_sample_cart_byte\":%u"
        ",\"rom\":\"%s\",\"message\":\"%s\"}",
        burner_state_to_str(snap.state),
        snap.progress,
        snap.processed_bytes,
        snap.total_bytes,
        snap.speed_current_bps,
        snap.speed_avg_bps,
        snap.speed_min_bps,
        snap.speed_max_bps,
        snap.tf_to_psram_speed_current_bps,
        snap.tf_to_psram_speed_avg_bps,
        snap.tf_to_psram_speed_min_bps,
        snap.tf_to_psram_speed_max_bps,
        snap.dump_read_speed_current_bps,
        snap.dump_read_speed_avg_bps,
        snap.dump_read_speed_min_bps,
        snap.dump_read_speed_max_bps,
        snap.dump_write_speed_current_bps,
        snap.dump_write_speed_avg_bps,
        snap.dump_write_speed_min_bps,
        snap.dump_write_speed_max_bps,
        snap.mbc5_buffer_write_ok_count,
        snap.mbc5_buffer_fallback_count,
        snap.erase_sector_count,
        snap.erase_sector_size,
        snap.erase_phase_active ? "true" : "false",
        snap.erase_phase_done_sectors,
        snap.erase_phase_total_sectors,
        task_ms,
        erase_ms,
        write_ms,
        dump_read_ms,
        dump_write_ms,
        dump_wait_ms,
        dump_finalize_ms,
        s_mcu_spi_clock_hz,
        s_mcu_spi_actual_hz,
        (unsigned)BURNER_IDLE_POWER_TIMEOUT_MS,
        cart_sleeping ? "true" : "false",
        snap.cancel_requested ? "true" : "false",
        snap.verify_sample_valid ? "true" : "false",
        snap.verify_sample_equal ? "true" : "false",
        snap.verify_sample_addr,
        (unsigned)snap.verify_sample_file_byte,
        (unsigned)snap.verify_sample_cart_byte,
        snap.rom_name,
        snap.message);
    if (n < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_cancel_handler(httpd_req_t *req)
{
    burner_status_t snap;
    char resp[256];
    int n;

    burner_status_snapshot(&snap);
    if (!burner_status_is_operation_active_state(snap.state)) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "no cancellable task is running");
    }

    if (!burner_cancel_request()) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "no cancellable task is running");
    }

    burner_status_snapshot(&snap);
    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"state\":\"%s\",\"message\":\"cancel requested\"}",
        burner_state_to_str(snap.state));
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}


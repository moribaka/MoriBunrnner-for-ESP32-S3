#include "ws_server_http_burn.h"

static esp_err_t burner_resolve_input_file(
    const char *raw_name,
    char *safe_name,
    size_t safe_name_len,
    char *full_path,
    size_t full_path_len,
    uint32_t *file_size);
static esp_err_t burner_send_start_error(httpd_req_t *req, esp_err_t err, const char *error_msg);
static bool burner_parse_pipeline_erase_text(const char *text, bool *erase_always_out);

static bool burner_parse_pipeline_erase_text(const char *text, bool *erase_always_out)
{
    if (text == NULL || erase_always_out == NULL) {
        return false;
    }
    if (strcasecmp(text, "smart") == 0 || strcasecmp(text, "skip") == 0) {
        *erase_always_out = false;
        return true;
    }
    if (strcasecmp(text, "force") == 0 || strcasecmp(text, "always") == 0) {
        *erase_always_out = true;
        return true;
    }
    return false;
}

static bool burner_parse_psram_mb_or_auto_text(const char *text, uint32_t *psram_mb_out)
{
    uint32_t value = 0;

    if (text == NULL || psram_mb_out == NULL) {
        return false;
    }
    if (text[0] == '\0' || strcasecmp(text, "auto") == 0 || strcmp(text, "0") == 0) {
        *psram_mb_out = BURN_PSRAM_WINDOW_AUTO_MB;
        return true;
    }
    if (!burner_parse_u32_text(text, &value)) {
        return false;
    }
    if (value < BURN_PSRAM_WINDOW_MIN_MB || value > BURN_PSRAM_WINDOW_MAX_MB) {
        return false;
    }
    *psram_mb_out = value;
    return true;
}

static bool burner_rom_dump_name_is_placeholder(const char *name)
{
    char stem[96] = {0};
    const char *base = NULL;
    const char *dot = NULL;
    size_t stem_len = 0u;

    if (name == NULL || name[0] == '\0') {
        return true;
    }

    base = burner_basename(name);
    if (base == NULL || base[0] == '\0') {
        return true;
    }

    dot = strrchr(base, '.');
    stem_len = (dot != NULL) ? (size_t)(dot - base) : strlen(base);
    if (stem_len == 0u) {
        return true;
    }
    if (stem_len >= sizeof(stem)) {
        stem_len = sizeof(stem) - 1u;
    }

    memcpy(stem, base, stem_len);
    stem[stem_len] = '\0';

    if (strcasecmp(stem, "cart") == 0 || strcasecmp(stem, "dump") == 0) {
        return true;
    }
    if (strncasecmp(stem, "cart_", 5) == 0 || strncasecmp(stem, "cart-", 5) == 0) {
        return true;
    }
    if (strncasecmp(stem, "dump_", 5) == 0 || strncasecmp(stem, "dump-", 5) == 0) {
        return true;
    }

    return false;
}

esp_err_t burner_upload_handler(httpd_req_t *req)
{
    char query[TF_QUERY_LEN_MAX] = {0};
    char raw_name[TF_PATH_LEN_MAX] = {0};
    char mode_arg[16] = {0};
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    char resp[BURNER_JSON_RESP_LEN] = {0};
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    int remaining;
    uint32_t written_total = 0;
    burner_cart_mode_t cart_mode = BURNER_CART_MODE_MBC5;
    esp_err_t err;
    bool failed = false;
    bool canceled = false;
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty upload body");
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "name", raw_name, sizeof(raw_name));
        (void)httpd_query_key_value(query, "mode", mode_arg, sizeof(mode_arg));
    }
    if (!burner_parse_cart_mode_text(mode_arg, &cart_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }
    if (raw_name[0] == '\0') {
        snprintf(raw_name, sizeof(raw_name), "rom_%lu.gba", (unsigned long)esp_log_timestamp());
    }

    if (!burner_sanitize_filename(raw_name, safe_name, sizeof(safe_name))) {
        snprintf(safe_name, sizeof(safe_name), "upload.gba");
    }

    if (snprintf(full_path, sizeof(full_path), ROM_DIR_PATH "/%s", safe_name) >= (int)sizeof(full_path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename too long");
    }

    err = burner_ensure_rom_dir();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "create rom dir failed");
    }

    fp = fopen(full_path, "wb");
    if (fp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open rom file failed");
    }

    buf = (uint8_t *)malloc(UPLOAD_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(fp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    burner_cancel_reset();

    burner_status_update(
        BURNER_STATE_RECEIVING,
        0,
        0,
        (uint32_t)req->content_len,
        "upload started",
        safe_name,
        full_path);

    remaining = req->content_len;
    while (remaining > 0) {
        int to_recv = remaining > UPLOAD_CHUNK_SIZE ? UPLOAD_CHUNK_SIZE : remaining;
        int recv_len;
        int progress;

        if (burner_cancel_is_requested()) {
            canceled = true;
            burner_status_update(
                BURNER_STATE_CANCELLED,
                burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
                written_total,
                (uint32_t)req->content_len,
                "upload cancelled",
                safe_name,
                full_path);
            break;
        }

        recv_len = httpd_req_recv(req, (char *)buf, to_recv);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            failed = true;
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                written_total,
                (uint32_t)req->content_len,
                "upload interrupted",
                safe_name,
                full_path);
            break;
        }

        if (fwrite(buf, 1, (size_t)recv_len, fp) != (size_t)recv_len) {
            failed = true;
            burner_status_update(
                BURNER_STATE_ERROR,
                0,
                written_total,
                (uint32_t)req->content_len,
                "write tf failed",
                safe_name,
                full_path);
            break;
        }

        remaining -= recv_len;
        written_total += (uint32_t)recv_len;
        progress = burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len);
        if (progress > 100) {
            progress = 100;
        }
        burner_status_update(
            BURNER_STATE_RECEIVING,
            progress,
            written_total,
            (uint32_t)req->content_len,
            "uploading",
            safe_name,
            full_path);
    }

    free(buf);
    fclose(fp);

    if (!canceled && burner_cancel_is_requested()) {
        canceled = true;
        burner_status_update(
            BURNER_STATE_CANCELLED,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "upload cancelled",
            safe_name,
            full_path);
    }

    if (failed || canceled) {
        unlink(full_path);
        burner_cancel_reset();
        if (canceled) {
            return httpd_resp_send_custom_err(req, "409 Conflict", "upload cancelled");
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }

    if (cart_mode == BURNER_CART_MODE_GBA && (written_total & 0x1u) != 0u) {
        FILE *pad_fp = fopen(full_path, "ab");
        if (pad_fp == NULL) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "append gba padding failed");
        }
        if (fputc(0x00, pad_fp) == EOF) {
            fclose(pad_fp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "append gba padding failed");
        }
        fclose(pad_fp);
        written_total += 1u;
    }

    (void)burner_apply_current_file_mtime(full_path, NULL);

    burner_status_update(
        BURNER_STATE_DONE,
        100,
        written_total,
        written_total,
        "upload to tf complete",
        safe_name,
        full_path);
    burner_cancel_reset();

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"mode\":\"%s\",\"message\":\"upload complete\",\"path\":\"%s\",\"size\":%" PRIu32 "}",
        (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
        full_path,
        written_total);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_write_handler(httpd_req_t *req)
{
    char raw_name[TF_PATH_LEN_MAX] = {0};
    char slot_arg[16] = {0};
    char mode_arg[16] = {0};
    char write_path_arg[16] = {0};
    char pipeline_erase_arg[16] = {0};
    char psram_mb_arg[16] = {0};
    char mbc5_chunk_kb_arg[16] = {0};
    char force_no_cfi_arg[16] = {0};
    char resp[BURNER_JSON_RESP_LEN] = {0};
    char error_msg[160] = {0};
    uint32_t slot = 0;
    uint32_t mbc5_chunk_kb = s_burn_mbc5_chunk_kb;
    uint32_t psram_mb = s_burn_psram_window_mb;
    bool gba_force_no_cfi = false;
    bool erase_always = (s_burn_erase_always != 0u);
    burner_cart_mode_t cart_mode = BURNER_CART_MODE_MBC5;
    burner_write_path_t write_path = s_burn_write_path_default;
    burner_task_start_result_t result = {0};
    esp_err_t err;
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }
    if (!burner_get_query_arg(req, "name", raw_name, sizeof(raw_name), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name query");
    }
    if (!burner_get_query_arg(req, "slot", slot_arg, sizeof(slot_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (!burner_get_query_arg(req, "mode", mode_arg, sizeof(mode_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode query");
    }
    if (!burner_get_query_arg(req, "write_path", write_path_arg, sizeof(write_path_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid write_path query");
    }
    if (!burner_get_query_arg(req, "pipeline_erase", pipeline_erase_arg, sizeof(pipeline_erase_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid pipeline_erase query");
    }
    if (!burner_get_query_arg(req, "psram_mb", psram_mb_arg, sizeof(psram_mb_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid psram_mb query");
    }
    if (!burner_get_query_arg(req, "mbc5_chunk_kb", mbc5_chunk_kb_arg, sizeof(mbc5_chunk_kb_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mbc5_chunk_kb query");
    }
    if (!burner_get_query_arg(req, "force_no_cfi", force_no_cfi_arg, sizeof(force_no_cfi_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid force_no_cfi query");
    }
    if (!burner_parse_cart_mode_text(mode_arg, &cart_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }
    if (write_path_arg[0] != '\0' &&
        !burner_parse_write_path_text(write_path_arg, &write_path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "write_path must be direct, psram, or pipeline");
    }
    if (pipeline_erase_arg[0] != '\0' &&
        !burner_parse_pipeline_erase_text(pipeline_erase_arg, &erase_always)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pipeline_erase must be smart or force");
    }
    if (psram_mb_arg[0] != '\0') {
        if (!burner_parse_psram_mb_or_auto_text(psram_mb_arg, &psram_mb)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "psram_mb must be auto or integer 1..8");
        }
    }
    if (mbc5_chunk_kb_arg[0] != '\0') {
        if (!burner_parse_u32_text(mbc5_chunk_kb_arg, &mbc5_chunk_kb)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mbc5_chunk_kb must be integer");
        }
    }
    if (slot_arg[0] != '\0' && !burner_parse_u32_text(slot_arg, &slot)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (force_no_cfi_arg[0] != '\0' && !burner_parse_bool_text(force_no_cfi_arg, &gba_force_no_cfi)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "force_no_cfi must be true/false/1/0");
    }
    if (cart_mode != BURNER_CART_MODE_GBA) {
        gba_force_no_cfi = false;
    }

    err = burner_start_write_from_tf(
        raw_name,
        cart_mode,
        slot,
        write_path,
        erase_always,
        psram_mb,
        mbc5_chunk_kb,
        gba_force_no_cfi,
        &result,
        error_msg,
        sizeof(error_msg));
    if (err != ESP_OK) {
        return burner_send_start_error(req, err, error_msg);
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"mode\":\"%s\",\"write_path\":\"%s\",\"mbc5_chunk_kb\":%" PRIu32 ",\"psram_mb\":%" PRIu32 ",\"force_no_cfi\":%s,\"message\":\"burn started\",\"path\":\"%s\",\"size\":%" PRIu32 "}",
        (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
        burner_write_path_to_str(write_path),
        result.mbc5_chunk_kb,
        result.psram_mb,
        result.gba_force_no_cfi ? "true" : "false",
        result.full_path,
        result.effective_size);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_read_handler(httpd_req_t *req)
{
    char query[TF_QUERY_LEN_MAX] = {0};
    char raw_name[TF_PATH_LEN_MAX] = {0};
    char slot_arg[16] = {0};
    char mode_arg[16] = {0};
    char read_path_arg[16] = {0};
    char dump_chunk_kb_arg[16] = {0};
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char size_arg[32] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    char resp[BURNER_JSON_RESP_LEN] = {0};
    uint32_t read_size = 0;
    uint32_t slot = 0;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    uint32_t dump_chunk_kb = s_burn_dump_chunk_kb;
    uint32_t dump_chunk_bytes = burner_dump_chunk_kb_to_bytes(s_burn_dump_chunk_kb);
    bool gba_force_multi = false;
    burner_cart_mode_t cart_mode = BURNER_CART_MODE_MBC5;
    burner_write_path_t read_path = BURNER_WRITE_PATH_DIRECT;
    esp_err_t err;
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "name", raw_name, sizeof(raw_name));
        (void)httpd_query_key_value(query, "size", size_arg, sizeof(size_arg));
        (void)httpd_query_key_value(query, "slot", slot_arg, sizeof(slot_arg));
        (void)httpd_query_key_value(query, "mode", mode_arg, sizeof(mode_arg));
        (void)httpd_query_key_value(query, "read_path", read_path_arg, sizeof(read_path_arg));
        (void)httpd_query_key_value(query, "dump_chunk_kb", dump_chunk_kb_arg, sizeof(dump_chunk_kb_arg));
    }
    if (!burner_parse_cart_mode_text(mode_arg, &cart_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }
    if (read_path_arg[0] != '\0' && !burner_parse_write_path_text(read_path_arg, &read_path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid read_path");
    }
    read_path = BURNER_WRITE_PATH_DIRECT;
    if (slot_arg[0] != '\0' && !burner_parse_u32_text(slot_arg, &slot)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (dump_chunk_kb_arg[0] != '\0') {
        if (!burner_parse_u32_text(dump_chunk_kb_arg, &dump_chunk_kb)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dump_chunk_kb must be integer");
        }
        dump_chunk_bytes = burner_dump_chunk_kb_to_bytes(dump_chunk_kb);
        if (burner_dump_chunk_bytes_to_kb(dump_chunk_bytes) != dump_chunk_kb) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dump_chunk_kb must be 32/64/128/256");
        }
    }

    if (!burner_parse_size_text(size_arg, &read_size)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size, use bytes/KB/MB suffix");
    }
    if (cart_mode == BURNER_CART_MODE_GBA && (read_size & 0x1u) != 0u) {
        if (read_size == UINT32_MAX) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
        }
        read_size += 1u;
    }

    if (cart_mode == BURNER_CART_MODE_GBA) {
        err = burner_apply_gba_slot_limit(slot, read_size, &addr_begin, &effective_size, &gba_force_multi);
    } else {
        err = burner_apply_mbc5_slot_limit(false, slot, read_size, &addr_begin, &effective_size);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read size exceeds selected slot range");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }

    if (raw_name[0] == '\0' || burner_rom_dump_name_is_placeholder(raw_name)) {
        burner_build_default_dump_name(
            cart_mode,
            addr_begin,
            effective_size,
            gba_force_multi,
            raw_name,
            sizeof(raw_name));
    }
    if (!burner_sanitize_filename(raw_name, safe_name, sizeof(safe_name))) {
        snprintf(safe_name, sizeof(safe_name), "dump%s", burner_rom_dump_ext_for_mode(cart_mode));
    }
    if (!burner_force_file_extension(
            safe_name,
            burner_rom_dump_ext_for_mode(cart_mode),
            safe_name,
            sizeof(safe_name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename too long");
    }

    err = burner_ensure_rom_output_dir();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "create rom output dir failed");
    }

    err = burner_resolve_unique_output_path(
        ROM_OUTPUT_DIR_PATH,
        safe_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename too long");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "resolve output filename failed");
    }

    err = burner_start_task_ex(
        BURNER_JOB_READ_ROM,
        cart_mode,
        read_path,
        false,
        gba_force_multi,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        dump_chunk_bytes,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        false,
        0u);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            effective_size,
            "dump task busy or start failed",
            safe_name,
            full_path);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dump task is already running");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"mode\":\"%s\",\"read_path\":\"%s\",\"dump_chunk_kb\":%" PRIu32 ",\"message\":\"dump started\",\"path\":\"%s\",\"size\":%" PRIu32 "}",
        (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
        burner_write_path_to_str(read_path),
        burner_dump_chunk_bytes_to_kb(dump_chunk_bytes),
        full_path,
        effective_size);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

static esp_err_t burner_resolve_input_file(
    const char *raw_name,
    char *safe_name,
    size_t safe_name_len,
    char *full_path,
    size_t full_path_len,
    uint32_t *file_size)
{
    struct stat st;
    char path_try[BURNER_FILE_PATH_LEN] = {0};
    const char *bases[] = {ROM_DIR_PATH, ROM_OUTPUT_DIR_PATH, DUMP_DIR_PATH};
    char validated_name[BURNER_FILE_NAME_LEN] = {0};
    char fallback_name[BURNER_FILE_NAME_LEN] = {0};
    const char *candidates[2] = {0};
    size_t candidate_count = 0;
    size_t i;
    size_t j;

    if (raw_name == NULL || safe_name == NULL || full_path == NULL || file_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* New firmware path mode: allow using any normalized TF relative file path directly. */
    {
        char rel_path[TF_PATH_LEN_MAX] = {0};
        char direct_full[TF_PATH_LEN_MAX + 64] = {0};
        const char *name_only = NULL;

        if (burner_normalize_rel_path(raw_name, rel_path, sizeof(rel_path), false) &&
            burner_build_full_path(rel_path, direct_full, sizeof(direct_full)) &&
            stat(direct_full, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st.st_size <= 0 || (unsigned long long)st.st_size > UINT32_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (snprintf(full_path, full_path_len, "%s", direct_full) >= (int)full_path_len) {
                return ESP_ERR_INVALID_SIZE;
            }

            name_only = strrchr(rel_path, '/');
            name_only = (name_only != NULL) ? (name_only + 1) : rel_path;
            if (snprintf(safe_name, safe_name_len, "%s", name_only) >= (int)safe_name_len) {
                return ESP_ERR_INVALID_SIZE;
            }

            *file_size = (uint32_t)st.st_size;
            return ESP_OK;
        }
    }

    /* Prefer exact validated name (keeps UTF-8 file names), fallback to legacy sanitized name. */
    if (burner_validate_file_name(raw_name, validated_name, sizeof(validated_name))) {
        candidates[candidate_count++] = validated_name;
    }
    if (burner_sanitize_filename(raw_name, fallback_name, sizeof(fallback_name))) {
        if (candidate_count == 0 || strcmp(fallback_name, candidates[0]) != 0) {
            candidates[candidate_count++] = fallback_name;
        }
    }
    if (candidate_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (j = 0; j < candidate_count; ++j) {
        for (i = 0; i < (sizeof(bases) / sizeof(bases[0])); ++i) {
            if (snprintf(path_try, sizeof(path_try), "%s/%s", bases[i], candidates[j]) >= (int)sizeof(path_try)) {
                continue;
            }
            if (stat(path_try, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            if (st.st_size <= 0 || (unsigned long long)st.st_size > UINT32_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (snprintf(full_path, full_path_len, "%s", path_try) >= (int)full_path_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (snprintf(safe_name, safe_name_len, "%s", candidates[j]) >= (int)safe_name_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            *file_size = (uint32_t)st.st_size;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t burner_start_error(
    esp_err_t err,
    const char *message,
    char *error_msg,
    size_t error_msg_len)
{
    if (error_msg != NULL && error_msg_len > 0u) {
        snprintf(error_msg, error_msg_len, "%s", (message != NULL) ? message : esp_err_to_name(err));
    }
    return err;
}

static void burner_start_result_fill(
    burner_task_start_result_t *result,
    const char *safe_name,
    const char *full_path,
    uint32_t effective_size,
    uint32_t psram_mb,
    uint32_t mbc5_chunk_kb,
    bool gba_force_no_cfi)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    snprintf(result->safe_name, sizeof(result->safe_name), "%s", (safe_name != NULL) ? safe_name : "");
    snprintf(result->full_path, sizeof(result->full_path), "%s", (full_path != NULL) ? full_path : "");
    result->effective_size = effective_size;
    result->psram_mb = psram_mb;
    result->mbc5_chunk_kb = mbc5_chunk_kb;
    result->gba_force_no_cfi = gba_force_no_cfi;
}

static esp_err_t burner_send_start_error(httpd_req_t *req, esp_err_t err, const char *error_msg)
{
    const char *msg = (error_msg != NULL && error_msg[0] != '\0') ? error_msg : esp_err_to_name(err);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, msg);
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NOT_SUPPORTED ||
        err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
}

esp_err_t burner_start_write_from_tf(
    const char *raw_name,
    burner_cart_mode_t cart_mode,
    uint32_t slot,
    burner_write_path_t write_path,
    bool erase_always,
    uint32_t psram_mb,
    uint32_t mbc5_chunk_kb,
    bool gba_force_no_cfi,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    uint32_t write_size = 0;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    uint32_t device_size = 0;
    uint32_t available_size = 0;
    uint32_t psram_window_bytes = BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES;
    uint32_t mbc5_program_chunk_bytes = BURN_MBC5_PROGRAM_CHUNK_BYTES;
    char probe_err[96] = {0};
    bool gba_force_multi = false;
    esp_err_t err;

    if (card == NULL) {
        return burner_start_error(ESP_ERR_INVALID_STATE, "TF card not ready", error_msg, error_msg_len);
    }
    if (usb_msc_tf_in_use_by_host()) {
        return burner_start_error(ESP_ERR_INVALID_STATE, "tf busy by usb host", error_msg, error_msg_len);
    }

    if (write_path == BURNER_WRITE_PATH_PIPELINE) {
        psram_window_bytes = 0u;
        psram_mb = BURN_PSRAM_WINDOW_AUTO_MB;
    } else {
        psram_window_bytes = burner_psram_window_mb_to_bytes(psram_mb);
        psram_mb = burner_psram_window_bytes_to_mb(psram_window_bytes);
    }
    mbc5_program_chunk_bytes = burner_mbc5_program_chunk_kb_to_bytes(mbc5_chunk_kb);
    if (cart_mode != BURNER_CART_MODE_GBA) {
        gba_force_no_cfi = false;
    }

    err = burner_resolve_input_file(
        raw_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path),
        &write_size);
    if (err == ESP_ERR_NOT_FOUND) {
        return burner_start_error(err, "rom file not found", error_msg, error_msg_len);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return burner_start_error(err, "rom file invalid size", error_msg, error_msg_len);
    }
    if (err != ESP_OK) {
        return burner_start_error(err, "invalid rom file", error_msg, error_msg_len);
    }

    if (cart_mode == BURNER_CART_MODE_GBA && (write_size & 0x1u) != 0u) {
        FILE *pad_fp;

        if (write_size == UINT32_MAX) {
            return burner_start_error(ESP_ERR_INVALID_SIZE, "rom file too large", error_msg, error_msg_len);
        }
        pad_fp = fopen(full_path, "ab");
        if (pad_fp == NULL) {
            return burner_start_error(ESP_FAIL, "append gba padding failed", error_msg, error_msg_len);
        }
        if (fputc(0x00, pad_fp) == EOF) {
            fclose(pad_fp);
            return burner_start_error(ESP_FAIL, "append gba padding failed", error_msg, error_msg_len);
        }
        fclose(pad_fp);
        write_size += 1u;
    }

    if (cart_mode == BURNER_CART_MODE_GBA) {
        err = burner_apply_gba_slot_limit(slot, write_size, &addr_begin, &effective_size, &gba_force_multi);
    } else {
        err = burner_apply_mbc5_slot_limit(false, slot, write_size, &addr_begin, &effective_size);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return burner_start_error(err, "rom file exceeds selected slot range", error_msg, error_msg_len);
    }
    if (err != ESP_OK) {
        return burner_start_error(err, "invalid slot query", error_msg, error_msg_len);
    }

    if (((uint64_t)addr_begin + (uint64_t)effective_size) > UINT32_MAX) {
        return burner_start_error(ESP_ERR_INVALID_SIZE, "requested write range too large", error_msg, error_msg_len);
    }

    err = burner_probe_cart_capacity_bytes(cart_mode, &device_size);
    if (err != ESP_OK) {
        if (cart_mode == BURNER_CART_MODE_GBA && err == ESP_ERR_NOT_SUPPORTED) {
            return burner_start_error(
                err,
                "gba cfi probe failed: command mode unavailable; cartridge may be read-only or unsupported",
                error_msg,
                error_msg_len);
        }
        (void)snprintf(
            probe_err,
            sizeof(probe_err),
            "read %s nor size failed: %s",
            (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
            esp_err_to_name(err));
        return burner_start_error(err, probe_err, error_msg, error_msg_len);
    }
    available_size = (addr_begin < device_size) ? (device_size - addr_begin) : 0u;
    if (((uint64_t)addr_begin + (uint64_t)effective_size) > (uint64_t)device_size) {
        char size_err[160] = {0};

        (void)snprintf(
            size_err,
            sizeof(size_err),
            "rom size exceeds nor size: rom=%" PRIu32 "B available=%" PRIu32 "B nor=%" PRIu32 "B",
            effective_size,
            available_size,
            device_size);
        return burner_start_error(ESP_ERR_INVALID_SIZE, size_err, error_msg, error_msg_len);
    }

    err = burner_start_task_ex(
        BURNER_JOB_WRITE_ROM,
        cart_mode,
        write_path,
        erase_always,
        gba_force_multi,
        gba_force_no_cfi,
        mbc5_program_chunk_bytes,
        BURN_GBA_DUMP_CHUNK_BYTES,
        psram_window_bytes,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        false,
        0u);
    if (err != ESP_OK) {
        const char *task_start_error =
            (err == ESP_ERR_INVALID_STATE) ? "burn task is already running" : "burn task start failed";
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            effective_size,
            task_start_error,
            safe_name,
            full_path);
        return burner_start_error(err, task_start_error, error_msg, error_msg_len);
    }

    burner_start_result_fill(
        result,
        safe_name,
        full_path,
        effective_size,
        psram_mb,
        (uint32_t)(mbc5_program_chunk_bytes / 1024u),
        gba_force_no_cfi);
    return ESP_OK;
}

esp_err_t burner_start_verify_from_tf(
    const char *raw_name,
    burner_cart_mode_t cart_mode,
    uint32_t slot,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    uint32_t verify_size = 0;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    bool gba_force_multi = false;
    esp_err_t err;

    if (card == NULL) {
        return burner_start_error(ESP_ERR_INVALID_STATE, "TF card not ready", error_msg, error_msg_len);
    }
    if (usb_msc_tf_in_use_by_host()) {
        return burner_start_error(ESP_ERR_INVALID_STATE, "tf busy by usb host", error_msg, error_msg_len);
    }

    err = burner_resolve_input_file(
        raw_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path),
        &verify_size);
    if (err == ESP_ERR_NOT_FOUND) {
        return burner_start_error(err, "verify file not found", error_msg, error_msg_len);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return burner_start_error(err, "verify file invalid size", error_msg, error_msg_len);
    }
    if (err != ESP_OK) {
        return burner_start_error(err, "invalid verify file", error_msg, error_msg_len);
    }

    if (cart_mode == BURNER_CART_MODE_GBA && (verify_size & 0x1u) != 0u) {
        if (verify_size == UINT32_MAX) {
            return burner_start_error(ESP_ERR_INVALID_SIZE, "verify file too large", error_msg, error_msg_len);
        }
        verify_size += 1u;
    }

    if (cart_mode == BURNER_CART_MODE_GBA) {
        err = burner_apply_gba_slot_limit(slot, verify_size, &addr_begin, &effective_size, &gba_force_multi);
    } else {
        err = burner_apply_mbc5_slot_limit(false, slot, verify_size, &addr_begin, &effective_size);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return burner_start_error(err, "verify file exceeds selected slot range", error_msg, error_msg_len);
    }
    if (err != ESP_OK) {
        return burner_start_error(err, "invalid slot query", error_msg, error_msg_len);
    }

    err = burner_start_task_ex(
        BURNER_JOB_VERIFY_ROM,
        cart_mode,
        BURNER_WRITE_PATH_DIRECT,
        false,
        gba_force_multi,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_VERIFY_PSRAM_WINDOW_BYTES,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        false,
        0u);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            effective_size,
            "verify task busy or start failed",
            safe_name,
            full_path);
        return burner_start_error(err, "verify task is already running", error_msg, error_msg_len);
    }

    burner_start_result_fill(result, safe_name, full_path, effective_size, 0u, 0u, false);
    return ESP_OK;
}

static esp_err_t burner_start_ram_file_from_tf(
    burner_job_mode_t mode,
    const char *raw_name,
    uint32_t slot,
    bool fram_mode,
    uint8_t ram_latency,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    uint32_t file_size = 0;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    const bool write_mode = (mode == BURNER_JOB_WRITE_RAM);
    esp_err_t err;

    if (card == NULL) {
        return burner_start_error(ESP_ERR_INVALID_STATE, "TF card not ready", error_msg, error_msg_len);
    }
    if (usb_msc_tf_in_use_by_host()) {
        return burner_start_error(ESP_ERR_INVALID_STATE, "tf busy by usb host", error_msg, error_msg_len);
    }

    err = burner_resolve_input_file(
        raw_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path),
        &file_size);
    if (err == ESP_ERR_NOT_FOUND) {
        return burner_start_error(err, write_mode ? "sav file not found" : "sav verify file not found", error_msg, error_msg_len);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return burner_start_error(err, write_mode ? "sav file invalid size" : "sav verify file invalid size", error_msg, error_msg_len);
    }
    if (err != ESP_OK) {
        return burner_start_error(err, write_mode ? "invalid sav file" : "invalid sav verify file", error_msg, error_msg_len);
    }

    err = burner_apply_mbc5_slot_limit(true, slot, file_size, &addr_begin, &effective_size);
    if (err == ESP_ERR_INVALID_SIZE) {
        return burner_start_error(
            err,
            write_mode ? "sav file exceeds selected slot range" : "sav verify file exceeds selected slot range",
            error_msg,
            error_msg_len);
    }
    if (err != ESP_OK) {
        return burner_start_error(err, "invalid slot query", error_msg, error_msg_len);
    }

    err = burner_start_task(
        mode,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        fram_mode,
        ram_latency);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            effective_size,
            write_mode ? "ram write task busy or start failed" : "ram verify task busy or start failed",
            safe_name,
            full_path);
        return burner_start_error(
            err,
            write_mode ? "ram write task is already running" : "ram verify task is already running",
            error_msg,
            error_msg_len);
    }

    burner_start_result_fill(result, safe_name, full_path, effective_size, 0u, 0u, false);
    return ESP_OK;
}

esp_err_t burner_start_ram_write_from_tf(
    const char *raw_name,
    uint32_t slot,
    bool fram_mode,
    uint8_t ram_latency,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    return burner_start_ram_file_from_tf(
        BURNER_JOB_WRITE_RAM,
        raw_name,
        slot,
        fram_mode,
        ram_latency,
        result,
        error_msg,
        error_msg_len);
}

esp_err_t burner_start_ram_verify_from_tf(
    const char *raw_name,
    uint32_t slot,
    bool fram_mode,
    uint8_t ram_latency,
    burner_task_start_result_t *result,
    char *error_msg,
    size_t error_msg_len)
{
    return burner_start_ram_file_from_tf(
        BURNER_JOB_VERIFY_RAM,
        raw_name,
        slot,
        fram_mode,
        ram_latency,
        result,
        error_msg,
        error_msg_len);
}

esp_err_t burner_verify_handler(httpd_req_t *req)
{
    char raw_name[TF_PATH_LEN_MAX] = {0};
    char slot_arg[16] = {0};
    char mode_arg[16] = {0};
    char resp[BURNER_JSON_RESP_LEN] = {0};
    char error_msg[160] = {0};
    uint32_t slot = 0;
    burner_cart_mode_t cart_mode = BURNER_CART_MODE_MBC5;
    burner_task_start_result_t result = {0};
    esp_err_t err;
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }
    if (!burner_get_query_arg(req, "name", raw_name, sizeof(raw_name), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name query");
    }
    if (!burner_get_query_arg(req, "slot", slot_arg, sizeof(slot_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (!burner_get_query_arg(req, "mode", mode_arg, sizeof(mode_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode query");
    }
    if (!burner_parse_cart_mode_text(mode_arg, &cart_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }
    if (slot_arg[0] != '\0' && !burner_parse_u32_text(slot_arg, &slot)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }

    err = burner_start_verify_from_tf(
        raw_name,
        cart_mode,
        slot,
        &result,
        error_msg,
        sizeof(error_msg));
    if (err != ESP_OK) {
        return burner_send_start_error(req, err, error_msg);
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"mode\":\"%s\",\"message\":\"verify started\",\"path\":\"%s\",\"size\":%" PRIu32 "}",
        (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5",
        result.full_path,
        result.effective_size);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_ram_write_handler(httpd_req_t *req)
{
    char raw_name[TF_PATH_LEN_MAX] = {0};
    char slot_arg[16] = {0};
    char ram_type_arg[16] = {0};
    char ram_latency_arg[16] = {0};
    char resp[BURNER_JSON_RESP_LEN] = {0};
    char error_msg[160] = {0};
    uint32_t slot = 0;
    uint32_t ram_latency = 10u;
    bool fram_mode = false;
    burner_task_start_result_t result = {0};
    esp_err_t err;
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }
    if (!burner_get_query_arg(req, "name", raw_name, sizeof(raw_name), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name query");
    }
    if (!burner_get_query_arg(req, "slot", slot_arg, sizeof(slot_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (slot_arg[0] != '\0' && !burner_parse_u32_text(slot_arg, &slot)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (!burner_get_query_arg(req, "ram_type", ram_type_arg, sizeof(ram_type_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ram_type query");
    }
    if (!burner_parse_ram_mode(ram_type_arg, &fram_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ram_type must be sram or fram");
    }
    if (!burner_get_query_arg(req, "ram_latency", ram_latency_arg, sizeof(ram_latency_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ram_latency query");
    }
    if (ram_latency_arg[0] != '\0' &&
        (!burner_parse_u32_text(ram_latency_arg, &ram_latency) || ram_latency > 255u)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ram_latency must be 0..255");
    }

    err = burner_start_ram_write_from_tf(
        raw_name,
        slot,
        fram_mode,
        (uint8_t)ram_latency,
        &result,
        error_msg,
        sizeof(error_msg));
    if (err != ESP_OK) {
        return burner_send_start_error(req, err, error_msg);
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"message\":\"ram write started\",\"path\":\"%s\",\"size\":%" PRIu32 "}",
        result.full_path,
        result.effective_size);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_ram_read_handler(httpd_req_t *req)
{
    char query[TF_QUERY_LEN_MAX] = {0};
    char raw_name[TF_PATH_LEN_MAX] = {0};
    char slot_arg[16] = {0};
    char ram_type_arg[16] = {0};
    char ram_latency_arg[16] = {0};
    char safe_name[BURNER_FILE_NAME_LEN] = {0};
    char size_arg[32] = {0};
    char full_path[BURNER_FILE_PATH_LEN] = {0};
    char resp[BURNER_JSON_RESP_LEN] = {0};
    uint32_t read_size = 0;
    uint32_t slot = 0;
    uint32_t addr_begin = 0;
    uint32_t effective_size = 0;
    uint32_t ram_latency = 10u;
    bool fram_mode = false;
    esp_err_t err;
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "name", raw_name, sizeof(raw_name));
        (void)httpd_query_key_value(query, "size", size_arg, sizeof(size_arg));
        (void)httpd_query_key_value(query, "slot", slot_arg, sizeof(slot_arg));
        (void)httpd_query_key_value(query, "ram_type", ram_type_arg, sizeof(ram_type_arg));
        (void)httpd_query_key_value(query, "ram_latency", ram_latency_arg, sizeof(ram_latency_arg));
    }
    if (slot_arg[0] != '\0' && !burner_parse_u32_text(slot_arg, &slot)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (!burner_parse_ram_mode(ram_type_arg, &fram_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ram_type must be sram or fram");
    }
    if (ram_latency_arg[0] != '\0' &&
        (!burner_parse_u32_text(ram_latency_arg, &ram_latency) || ram_latency > 255u)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ram_latency must be 0..255");
    }

    if (!burner_parse_size_text(size_arg, &read_size)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size, use bytes/KB/MB suffix");
    }

    if (raw_name[0] == '\0') {
        char timestamp_text[32] = {0};
        burner_build_output_timestamp(timestamp_text, sizeof(timestamp_text));
        snprintf(raw_name, sizeof(raw_name), "dump_ram_%s.sav", timestamp_text);
    }
    if (!burner_sanitize_filename(raw_name, safe_name, sizeof(safe_name))) {
        snprintf(safe_name, sizeof(safe_name), "dump_ram.sav");
    }

    err = burner_ensure_dump_dir();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "create dump dir failed");
    }

    err = burner_resolve_unique_output_path(
        DUMP_DIR_PATH,
        safe_name,
        safe_name,
        sizeof(safe_name),
        full_path,
        sizeof(full_path));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename too long");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "resolve output filename failed");
    }

    err = burner_apply_mbc5_slot_limit(true, slot, read_size, &addr_begin, &effective_size);
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read size exceeds selected slot range");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }

    err = burner_start_task(
        BURNER_JOB_READ_RAM,
        safe_name,
        full_path,
        addr_begin,
        effective_size,
        BURNER_GBA_SAVE_TYPE_SRAM,
        fram_mode,
        (uint8_t)ram_latency);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            effective_size,
            "ram dump task busy or start failed",
            safe_name,
            full_path);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ram dump task is already running");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"message\":\"ram dump started\",\"path\":\"%s\",\"size\":%" PRIu32 "}",
        full_path,
        effective_size);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_ram_verify_handler(httpd_req_t *req)
{
    char raw_name[TF_PATH_LEN_MAX] = {0};
    char slot_arg[16] = {0};
    char ram_type_arg[16] = {0};
    char ram_latency_arg[16] = {0};
    char resp[BURNER_JSON_RESP_LEN] = {0};
    char error_msg[160] = {0};
    uint32_t slot = 0;
    uint32_t ram_latency = 10u;
    bool fram_mode = false;
    burner_task_start_result_t result = {0};
    esp_err_t err;
    int n;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }
    if (!burner_get_query_arg(req, "name", raw_name, sizeof(raw_name), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name query");
    }
    if (!burner_get_query_arg(req, "slot", slot_arg, sizeof(slot_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (slot_arg[0] != '\0' && !burner_parse_u32_text(slot_arg, &slot)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot query");
    }
    if (!burner_get_query_arg(req, "ram_type", ram_type_arg, sizeof(ram_type_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ram_type query");
    }
    if (!burner_parse_ram_mode(ram_type_arg, &fram_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ram_type must be sram or fram");
    }
    if (!burner_get_query_arg(req, "ram_latency", ram_latency_arg, sizeof(ram_latency_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ram_latency query");
    }
    if (ram_latency_arg[0] != '\0' &&
        (!burner_parse_u32_text(ram_latency_arg, &ram_latency) || ram_latency > 255u)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ram_latency must be 0..255");
    }

    err = burner_start_ram_verify_from_tf(
        raw_name,
        slot,
        fram_mode,
        (uint8_t)ram_latency,
        &result,
        error_msg,
        sizeof(error_msg));
    if (err != ESP_OK) {
        return burner_send_start_error(req, err, error_msg);
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"message\":\"ram verify started\",\"path\":\"%s\",\"size\":%" PRIu32 "}",
        result.full_path,
        result.effective_size);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_cart_erase_handler(httpd_req_t *req)
{
    char mode_arg[16] = {0};
    burner_cart_mode_t cart_mode = BURNER_CART_MODE_MBC5;
    char resp[128] = {0};
    esp_err_t err;
    int n;

    if (!burner_get_query_arg(req, "mode", mode_arg, sizeof(mode_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode query");
    }
    if (!burner_parse_cart_mode_text(mode_arg, &cart_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be gba or mbc5");
    }

    err = burner_start_task_ex(
        BURNER_JOB_ERASE_ROM,
        cart_mode,
        BURNER_WRITE_PATH_DIRECT,
        false,
        false,
        false,
        BURN_MBC5_PROGRAM_CHUNK_BYTES,
        BURN_GBA_DUMP_CHUNK_BYTES,
        BURN_WRITE_PSRAM_DEFAULT_WINDOW_BYTES,
        "chip_erase",
        "/cart",
        0u,
        1u,
        BURNER_GBA_SAVE_TYPE_SRAM,
        false,
        0u);
    if (err != ESP_OK) {
        burner_status_update(
            BURNER_STATE_ERROR,
            0,
            0,
            1u,
            "chip erase task busy or start failed",
            "chip_erase",
            "/cart");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "chip erase task is already running");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"mode\":\"%s\",\"message\":\"chip erase started\"}",
        (cart_mode == BURNER_CART_MODE_GBA) ? "gba" : "mbc5");
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}


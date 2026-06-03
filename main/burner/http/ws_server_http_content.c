#include "ws_server_http_content.h"

static esp_err_t burner_send_builtin_html(httpd_req_t *req, const char *html)
{
    if (req == NULL || html == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_ws_mark_activity();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_static_handler(httpd_req_t *req)
{
    char rel_path[WEB_FILE_PATH_LEN_MAX] = {0};

    if (!burner_uri_to_web_rel_path(req->uri, rel_path, sizeof(rel_path))) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "resource not found");
    }

    web_ws_mark_activity();
    return burner_send_static_file(req, rel_path);
}

esp_err_t burner_root_handler(httpd_req_t *req)
{
    return burner_send_builtin_html(req, s_base_settings_html);
}

esp_err_t burner_sys_page_handler(httpd_req_t *req)
{
    return burner_root_handler(req);
}

esp_err_t burner_business_page_handler(httpd_req_t *req)
{
    web_ws_mark_activity();
    return burner_send_static_file(req, WEB_MAIN_FILE_REL);
}

esp_err_t burner_tf_page_handler(httpd_req_t *req)
{
    return burner_business_page_handler(req);
}

esp_err_t burner_settings_page_handler(httpd_req_t *req)
{
    return burner_business_page_handler(req);
}

typedef struct {
    char name[TF_PATH_LEN_MAX];
    char path[TF_PATH_LEN_MAX];
    bool is_dir;
    long long size;
    long long mtime;
    uint8_t sort_rank;
} burner_tf_list_entry_t;

typedef struct {
    burner_tf_list_entry_t *items;
    size_t count;
    size_t cap;
} burner_tf_list_entries_t;

static const char *burner_tf_file_ext(const char *name)
{
    const char *dot = NULL;

    if (name == NULL) {
        return "";
    }
    dot = strrchr(name, '.');
    if (dot == NULL || dot[1] == '\0') {
        return "";
    }
    return dot + 1;
}

static uint8_t burner_tf_sort_rank(bool is_dir, const char *name)
{
    const char *ext = burner_tf_file_ext(name);

    if (is_dir) {
        return 0U;
    }
    if (strcasecmp(ext, "gba") == 0) {
        return 1U;
    }
    if (strcasecmp(ext, "gb") == 0 || strcasecmp(ext, "gbc") == 0) {
        return 2U;
    }
    if (strcasecmp(ext, "sav") == 0 || strcasecmp(ext, "srm") == 0) {
        return 3U;
    }
    return 4U;
}

static int burner_tf_list_entry_compare(const void *lhs, const void *rhs)
{
    const burner_tf_list_entry_t *a = (const burner_tf_list_entry_t *)lhs;
    const burner_tf_list_entry_t *b = (const burner_tf_list_entry_t *)rhs;
    int cmp;

    if (a->sort_rank != b->sort_rank) {
        return (a->sort_rank < b->sort_rank) ? -1 : 1;
    }
    cmp = strcasecmp(a->name, b->name);
    if (cmp != 0) {
        return cmp;
    }
    cmp = strcmp(a->name, b->name);
    if (cmp != 0) {
        return cmp;
    }
    return strcmp(a->path, b->path);
}

static bool burner_tf_list_entries_append(
    burner_tf_list_entries_t *list,
    const char *name,
    const char *path,
    bool is_dir,
    long long size,
    long long mtime)
{
    burner_tf_list_entry_t *item = NULL;

    if (list == NULL || name == NULL || path == NULL) {
        return false;
    }
    if (list->count >= list->cap) {
        size_t next_cap = (list->cap == 0U) ? 32U : (list->cap * 2U);
        burner_tf_list_entry_t *new_items =
            (burner_tf_list_entry_t *)realloc(list->items, next_cap * sizeof(*list->items));
        if (new_items == NULL) {
            return false;
        }
        list->items = new_items;
        list->cap = next_cap;
    }

    item = &list->items[list->count];
    memset(item, 0, sizeof(*item));
    if (snprintf(item->name, sizeof(item->name), "%s", name) >= (int)sizeof(item->name) ||
        snprintf(item->path, sizeof(item->path), "%s", path) >= (int)sizeof(item->path)) {
        return false;
    }
    item->is_dir = is_dir;
    item->size = size;
    item->mtime = mtime;
    item->sort_rank = burner_tf_sort_rank(is_dir, name);
    list->count++;
    return true;
}

esp_err_t burner_tf_list_handler(httpd_req_t *req)
{
    char path_arg[TF_PATH_LEN_MAX] = {0};
    char rel_path[TF_PATH_LEN_MAX] = {0};
    char dir_path[TF_PATH_LEN_MAX + 64] = {0};
    burner_tf_list_buf_t *bufs = NULL;
    burner_tf_list_entries_t entries = {0};
    DIR *dir = NULL;
    struct dirent *entry;
    bool first = true;
    time_t server_now = 0;
    long long server_time = 0;
    esp_err_t send_err = ESP_OK;
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

    if (!burner_get_query_arg(req, "path", path_arg, sizeof(path_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query path");
    }

    if (!burner_normalize_rel_path(path_arg, rel_path, sizeof(rel_path), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
    }

    if (!burner_build_full_path(rel_path, dir_path, sizeof(dir_path))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }

    bufs = (burner_tf_list_buf_t *)calloc(1, sizeof(*bufs));
    if (bufs == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    dir = opendir(dir_path);
    if (dir == NULL) {
        free(bufs);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "path not found");
    }

    if (!burner_json_escape(rel_path, bufs->esc_path, sizeof(bufs->esc_path))) {
        closedir(dir);
        free(bufs);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    if (burner_get_wallclock_time(&server_now, NULL)) {
        server_time = (long long)server_now;
    }

    n = snprintf(
        bufs->head,
        sizeof(bufs->head),
        "{\"ok\":true,\"path\":\"%s\",\"server_time\":%lld,\"entries\":[",
        bufs->esc_path,
        server_time);
    if (n < 0 || n >= (int)sizeof(bufs->head)) {
        closedir(dir);
        free(bufs);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        bool is_dir = false;
        long long size = 0;
        long long mtime = 0;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (rel_path[0] == '\0') {
            n = snprintf(bufs->child_rel, sizeof(bufs->child_rel), "%s", entry->d_name);
        } else {
            n = snprintf(bufs->child_rel, sizeof(bufs->child_rel), "%s/%s", rel_path, entry->d_name);
        }
        if (n <= 0 || n >= (int)sizeof(bufs->child_rel)) {
            continue;
        }

        if (!burner_build_full_path(bufs->child_rel, bufs->child_full, sizeof(bufs->child_full))) {
            continue;
        }

        if (stat(bufs->child_full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long long)st.st_size;
            mtime = burner_fixup_file_mtime_for_api(bufs->child_full, &st);
        }

        if (!burner_tf_list_entries_append(&entries, entry->d_name, bufs->child_rel, is_dir, size, mtime)) {
            closedir(dir);
            free(entries.items);
            free(bufs);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        }
    }
    closedir(dir);
    dir = NULL;

    if (entries.count > 1U) {
        qsort(entries.items, entries.count, sizeof(entries.items[0]), burner_tf_list_entry_compare);
    }

    send_err = httpd_resp_sendstr_chunk(req, bufs->head);
    for (size_t i = 0; send_err == ESP_OK && i < entries.count; ++i) {
        const burner_tf_list_entry_t *item = &entries.items[i];

        if (!burner_json_escape(item->name, bufs->esc_name, sizeof(bufs->esc_name))) {
            continue;
        }
        if (!burner_json_escape(item->path, bufs->esc_child, sizeof(bufs->esc_child))) {
            continue;
        }

        n = snprintf(
            bufs->line,
            sizeof(bufs->line),
            "%s{\"name\":\"%s\",\"path\":\"%s\",\"is_dir\":%s,\"size\":%lld,\"mtime\":%lld,\"sort_rank\":%u}",
            first ? "" : ",",
            bufs->esc_name,
            bufs->esc_child,
            item->is_dir ? "true" : "false",
            item->size,
            item->mtime,
            (unsigned)item->sort_rank);
        if (n <= 0 || n >= (int)sizeof(bufs->line)) {
            continue;
        }

        send_err = httpd_resp_sendstr_chunk(req, bufs->line);
        first = false;
    }

    free(entries.items);
    free(bufs);
    if (send_err == ESP_OK) {
        send_err = httpd_resp_sendstr_chunk(req, "]}");
    }
    if (send_err == ESP_OK) {
        send_err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return send_err;
}

esp_err_t burner_tf_upload_handler(httpd_req_t *req)
{
    char dir_arg[TF_PATH_LEN_MAX] = {0};
    char dir_rel[TF_PATH_LEN_MAX] = {0};
    char raw_name[128] = {0};
    char file_name[96] = {0};
    char dir_full[TF_PATH_LEN_MAX + 64] = {0};
    char file_rel[TF_PATH_LEN_MAX] = {0};
    char file_full[TF_PATH_LEN_MAX + 64] = {0};
    char esc_rel[TF_PATH_LEN_MAX * 2 + 8] = {0};
    char resp[TF_PATH_LEN_MAX * 2 + 64] = {0};
    struct stat st;
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    int remaining = 0;
    uint32_t written_total = 0;
    bool cancelled = false;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_get_query_arg(req, "dir", dir_arg, sizeof(dir_arg), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid dir query");
    }
    if (!burner_normalize_rel_path(dir_arg, dir_rel, sizeof(dir_rel), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid dir path");
    }

    if (!burner_get_query_arg(req, "name", raw_name, sizeof(raw_name), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing file name");
    }
    if (!burner_validate_file_name(raw_name, file_name, sizeof(file_name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid file name");
    }

    if (!burner_build_full_path(dir_rel, dir_full, sizeof(dir_full))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dir path too long");
    }

    if (stat(dir_full, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "target dir not found");
    }

    if (dir_rel[0] == '\0') {
        if (snprintf(file_rel, sizeof(file_rel), "%s", file_name) >= (int)sizeof(file_rel)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        }
    } else {
        if (snprintf(file_rel, sizeof(file_rel), "%s/%s", dir_rel, file_name) >= (int)sizeof(file_rel)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        }
    }

    if (!burner_build_full_path(file_rel, file_full, sizeof(file_full))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }

    fp = fopen(file_full, "wb");
    if (fp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open file failed");
    }

    burner_cancel_reset();
    burner_status_update(
        BURNER_STATE_RECEIVING,
        0,
        0,
        (req->content_len > 0) ? (uint32_t)req->content_len : 0u,
        "tf upload started",
        file_name,
        file_full);

    remaining = req->content_len;
    if (remaining > 0) {
        buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
        if (buf == NULL) {
            fclose(fp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        }
    }

    while (remaining > 0) {
        int recv_len;
        int to_recv = remaining > TF_IO_CHUNK_SIZE ? TF_IO_CHUNK_SIZE : remaining;

        if (burner_cancel_is_requested()) {
            cancelled = true;
            burner_status_update(
                BURNER_STATE_CANCELLED,
                burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
                written_total,
                (uint32_t)req->content_len,
                "tf upload cancelled",
                file_name,
                file_full);
            break;
        }

        recv_len = httpd_req_recv(req, (char *)buf, to_recv);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            free(buf);
            fclose(fp);
            unlink(file_full);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
        }

        if (fwrite(buf, 1, (size_t)recv_len, fp) != (size_t)recv_len) {
            free(buf);
            fclose(fp);
            unlink(file_full);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write file failed");
        }

        remaining -= recv_len;
        written_total += (uint32_t)recv_len;
        burner_status_update(
            BURNER_STATE_RECEIVING,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "tf uploading",
            file_name,
            file_full);
    }

    if (buf != NULL) {
        free(buf);
    }
    fclose(fp);

    if (!cancelled && burner_cancel_is_requested()) {
        cancelled = true;
        burner_status_update(
            BURNER_STATE_CANCELLED,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "tf upload cancelled",
            file_name,
            file_full);
    }

    if (cancelled) {
        unlink(file_full);
        return httpd_resp_send_custom_err(req, "409 Conflict", "upload cancelled");
    }

    burner_status_update(
        BURNER_STATE_DONE,
        100,
        written_total,
        written_total,
        "tf upload complete",
        file_name,
        file_full);
    burner_cancel_reset();
    (void)burner_apply_current_file_mtime(file_full, NULL);

    if (!burner_json_escape(file_rel, esc_rel, sizeof(esc_rel))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    if (snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":true,\"path\":\"%s\",\"written\":%" PRIu32 "}",
            esc_rel,
            written_total) >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_tf_download_handler(httpd_req_t *req)
{
    char path_arg[TF_PATH_LEN_MAX] = {0};
    char rel_path[TF_PATH_LEN_MAX] = {0};
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    char disp[128] = {0};
    uint8_t *buf = NULL;
    FILE *fp = NULL;
    struct stat st;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_get_query_arg(req, "path", path_arg, sizeof(path_arg), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
    }
    if (!burner_normalize_rel_path(path_arg, rel_path, sizeof(rel_path), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
    }

    if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }

    if (stat(full_path, &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
    }
    if (S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path is a directory");
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open file failed");
    }

    buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(fp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    {
        int n = snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", burner_basename(rel_path));
        if (n < 0 || n >= (int)sizeof(disp)) {
            free(buf);
            fclose(fp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "header build failed");
        }
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    while (true) {
        size_t read_len = fread(buf, 1, TF_IO_CHUNK_SIZE, fp);
        if (read_len == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, read_len) != ESP_OK) {
            free(buf);
            fclose(fp);
            return ESP_FAIL;
        }
    }

    if (ferror(fp)) {
        free(buf);
        fclose(fp);
        return ESP_FAIL;
    }

    free(buf);
    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void burner_zip_put_le16(uint8_t *buf, uint16_t value)
{
    if (buf == NULL) {
        return;
    }
    buf[0] = (uint8_t)(value & 0xFFU);
    buf[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static uint16_t burner_zip_get_le16(const uint8_t *buf)
{
    if (buf == NULL) {
        return 0U;
    }
    return (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
}

static void burner_zip_put_le32(uint8_t *buf, uint32_t value)
{
    if (buf == NULL) {
        return;
    }
    buf[0] = (uint8_t)(value & 0xFFU);
    buf[1] = (uint8_t)((value >> 8) & 0xFFU);
    buf[2] = (uint8_t)((value >> 16) & 0xFFU);
    buf[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint32_t burner_zip_get_le32(const uint8_t *buf)
{
    if (buf == NULL) {
        return 0U;
    }
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static uint32_t burner_zip_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return crc;
    }

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static char *burner_dup_string(const char *src)
{
    size_t len = 0;
    char *dst = NULL;

    if (src == NULL) {
        return NULL;
    }
    len = strlen(src);
    dst = (char *)malloc(len + 1U);
    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, len + 1U);
    return dst;
}

static void burner_zip_item_list_free(burner_zip_item_list_t *list)
{
    if (list == NULL) {
        return;
    }
    if (list->items != NULL) {
        for (size_t i = 0; i < list->count; i++) {
            free(list->items[i].full_path);
            free(list->items[i].zip_path);
        }
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static void burner_zip_central_list_free(burner_zip_central_list_t *list)
{
    if (list == NULL) {
        return;
    }
    if (list->items != NULL) {
        free(list->items);
    }
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static esp_err_t burner_zip_central_list_add(
    burner_zip_central_list_t *list,
    const burner_zip_central_entry_t *entry)
{
    burner_zip_central_entry_t *new_items = NULL;

    if (list == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (list->count >= 0xFFFFU) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (list->count == list->cap) {
        size_t next_cap = (list->cap == 0U) ? 16U : (list->cap * 2U);
        new_items = (burner_zip_central_entry_t *)realloc(list->items, next_cap * sizeof(*list->items));
        if (new_items == NULL) {
            return ESP_ERR_NO_MEM;
        }
        list->items = new_items;
        list->cap = next_cap;
    }

    list->items[list->count] = *entry;
    list->count++;
    return ESP_OK;
}

static esp_err_t burner_zip_item_list_add(
    burner_zip_item_list_t *list,
    const char *full_path,
    const char *zip_path,
    uint32_t file_size)
{
    burner_zip_item_t *new_items = NULL;
    char *full_copy = NULL;
    char *zip_copy = NULL;

    if (list == NULL || full_path == NULL || zip_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(zip_path) > 0xFFFFU) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (list->count >= 0xFFFFU) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (list->count == list->cap) {
        size_t next_cap = (list->cap == 0U) ? 16U : (list->cap * 2U);
        new_items = (burner_zip_item_t *)realloc(list->items, next_cap * sizeof(*list->items));
        if (new_items == NULL) {
            return ESP_ERR_NO_MEM;
        }
        list->items = new_items;
        list->cap = next_cap;
    }

    full_copy = burner_dup_string(full_path);
    zip_copy = burner_dup_string(zip_path);
    if (full_copy == NULL || zip_copy == NULL) {
        free(full_copy);
        free(zip_copy);
        return ESP_ERR_NO_MEM;
    }

    list->items[list->count].full_path = full_copy;
    list->items[list->count].zip_path = zip_copy;
    list->items[list->count].size = file_size;
    list->items[list->count].crc32 = 0U;
    list->items[list->count].local_offset = 0U;
    list->count++;
    return ESP_OK;
}

static esp_err_t burner_zip_collect_recursive(
    const char *dir_full,
    const char *dir_zip,
    burner_zip_item_list_t *list)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    esp_err_t err = ESP_OK;

    if (dir_full == NULL || dir_zip == NULL || list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dir = opendir(dir_full);
    if (dir == NULL) {
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL) {
        char child_full[WEB_FILE_PATH_LEN_MAX] = {0};
        char child_zip[TF_PATH_LEN_MAX] = {0};
        struct stat st;
        int n;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        n = snprintf(child_full, sizeof(child_full), "%s/%s", dir_full, entry->d_name);
        if (n < 0 || n >= (int)sizeof(child_full)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        n = snprintf(child_zip, sizeof(child_zip), "%s/%s", dir_zip, entry->d_name);
        if (n < 0 || n >= (int)sizeof(child_zip)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        if (stat(child_full, &st) != 0) {
            err = ESP_FAIL;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            err = burner_zip_collect_recursive(child_full, child_zip, list);
            if (err != ESP_OK) {
                break;
            }
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            err = burner_zip_item_list_add(list, child_full, child_zip, (uint32_t)st.st_size);
            if (err != ESP_OK) {
                break;
            }
        }
    }

    closedir(dir);
    return err;
}

static esp_err_t burner_zip_collect_system_dirs(burner_zip_item_list_t *list)
{
    esp_err_t err = ESP_OK;
    bool found_any = false;

    if (list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < BURNER_SYSTEM_MIGRATE_REL_DIR_COUNT; i++) {
        const char *rel_dir = s_system_migrate_rel_dirs[i];
        char full_dir[WEB_FILE_PATH_LEN_MAX] = {0};
        struct stat st;

        if (!burner_build_full_path(rel_dir, full_dir, sizeof(full_dir))) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(full_dir, &st) != 0) {
            continue;
        }

        found_any = true;
        if (S_ISDIR(st.st_mode)) {
            err = burner_zip_collect_recursive(full_dir, rel_dir, list);
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            err = burner_zip_item_list_add(list, full_dir, rel_dir, (uint32_t)st.st_size);
        } else {
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!found_any || list->count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t burner_zip_stream_send(burner_zip_stream_t *stream, const void *data, size_t len)
{
    if (stream == NULL || stream->req == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0U) {
        return ESP_OK;
    }
    if (len > (size_t)(UINT32_MAX - stream->offset)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (httpd_resp_send_chunk(stream->req, (const char *)data, len) != ESP_OK) {
        return ESP_FAIL;
    }
    stream->offset += (uint32_t)len;
    return ESP_OK;
}

static esp_err_t burner_zip_write_file(burner_zip_stream_t *stream, burner_zip_item_t *item)
{
    uint8_t local_header[30] = {0};
    uint8_t data_descriptor[16] = {0};
    uint8_t *buf = NULL;
    FILE *fp = NULL;
    size_t zip_name_len = 0;
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t total = 0U;
    esp_err_t err = ESP_OK;

    if (stream == NULL || item == NULL || item->full_path == NULL || item->zip_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    zip_name_len = strlen(item->zip_path);
    if (zip_name_len == 0U || zip_name_len > 0xFFFFU) {
        return ESP_ERR_INVALID_SIZE;
    }

    fp = fopen(item->full_path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    item->local_offset = stream->offset;

    burner_zip_put_le32(&local_header[0], 0x04034B50U);
    burner_zip_put_le16(&local_header[4], ZIP_VERSION_NEEDED);
    burner_zip_put_le16(&local_header[6], ZIP_GP_FLAG_DATA_DESCRIPTOR);
    burner_zip_put_le16(&local_header[8], ZIP_METHOD_STORE);
    burner_zip_put_le16(&local_header[10], 0U);
    burner_zip_put_le16(&local_header[12], ZIP_DOS_DATE_MIN);
    burner_zip_put_le32(&local_header[14], 0U);
    burner_zip_put_le32(&local_header[18], 0U);
    burner_zip_put_le32(&local_header[22], 0U);
    burner_zip_put_le16(&local_header[26], (uint16_t)zip_name_len);
    burner_zip_put_le16(&local_header[28], 0U);

    err = burner_zip_stream_send(stream, local_header, sizeof(local_header));
    if (err == ESP_OK) {
        err = burner_zip_stream_send(stream, item->zip_path, zip_name_len);
    }

    while (err == ESP_OK) {
        size_t read_len = fread(buf, 1, TF_IO_CHUNK_SIZE, fp);
        if (read_len == 0U) {
            break;
        }
        if (read_len > (size_t)(UINT32_MAX - total)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        crc = burner_zip_crc32_update(crc, buf, read_len);
        total += (uint32_t)read_len;
        err = burner_zip_stream_send(stream, buf, read_len);
    }

    if (err == ESP_OK && ferror(fp)) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        item->size = total;
        item->crc32 = crc ^ 0xFFFFFFFFU;
        burner_zip_put_le32(&data_descriptor[0], 0x08074B50U);
        burner_zip_put_le32(&data_descriptor[4], item->crc32);
        burner_zip_put_le32(&data_descriptor[8], item->size);
        burner_zip_put_le32(&data_descriptor[12], item->size);
        err = burner_zip_stream_send(stream, data_descriptor, sizeof(data_descriptor));
    }

    free(buf);
    fclose(fp);
    return err;
}

static esp_err_t burner_zip_write_central_entry(
    burner_zip_stream_t *stream,
    const burner_zip_item_t *item)
{
    uint8_t central_header[46] = {0};
    size_t zip_name_len = 0;
    esp_err_t err;

    if (stream == NULL || item == NULL || item->zip_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    zip_name_len = strlen(item->zip_path);
    if (zip_name_len == 0U || zip_name_len > 0xFFFFU) {
        return ESP_ERR_INVALID_SIZE;
    }

    burner_zip_put_le32(&central_header[0], 0x02014B50U);
    burner_zip_put_le16(&central_header[4], ZIP_VERSION_NEEDED);
    burner_zip_put_le16(&central_header[6], ZIP_VERSION_NEEDED);
    burner_zip_put_le16(&central_header[8], ZIP_GP_FLAG_DATA_DESCRIPTOR);
    burner_zip_put_le16(&central_header[10], ZIP_METHOD_STORE);
    burner_zip_put_le16(&central_header[12], 0U);
    burner_zip_put_le16(&central_header[14], ZIP_DOS_DATE_MIN);
    burner_zip_put_le32(&central_header[16], item->crc32);
    burner_zip_put_le32(&central_header[20], item->size);
    burner_zip_put_le32(&central_header[24], item->size);
    burner_zip_put_le16(&central_header[28], (uint16_t)zip_name_len);
    burner_zip_put_le16(&central_header[30], 0U);
    burner_zip_put_le16(&central_header[32], 0U);
    burner_zip_put_le16(&central_header[34], 0U);
    burner_zip_put_le16(&central_header[36], 0U);
    burner_zip_put_le32(&central_header[38], 0U);
    burner_zip_put_le32(&central_header[42], item->local_offset);

    err = burner_zip_stream_send(stream, central_header, sizeof(central_header));
    if (err != ESP_OK) {
        return err;
    }
    return burner_zip_stream_send(stream, item->zip_path, zip_name_len);
}

static esp_err_t burner_zip_write_end_record(
    burner_zip_stream_t *stream,
    uint16_t entry_count,
    uint32_t central_size,
    uint32_t central_offset)
{
    uint8_t end_record[22] = {0};

    if (stream == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_zip_put_le32(&end_record[0], 0x06054B50U);
    burner_zip_put_le16(&end_record[4], 0U);
    burner_zip_put_le16(&end_record[6], 0U);
    burner_zip_put_le16(&end_record[8], entry_count);
    burner_zip_put_le16(&end_record[10], entry_count);
    burner_zip_put_le32(&end_record[12], central_size);
    burner_zip_put_le32(&end_record[16], central_offset);
    burner_zip_put_le16(&end_record[20], 0U);
    return burner_zip_stream_send(stream, end_record, sizeof(end_record));
}

esp_err_t burner_system_migrate_zip_handler(httpd_req_t *req)
{
    burner_zip_item_list_t files = {0};
    burner_zip_stream_t stream = {
        .req = req,
        .offset = 0U,
    };
    char disp[128] = {0};
    uint32_t central_offset = 0U;
    uint32_t central_size = 0U;
    esp_err_t err;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    err = burner_zip_collect_system_dirs(&files);
    if (err == ESP_ERR_NOT_FOUND) {
        burner_zip_item_list_free(&files);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "system folders not found");
    }
    if (err != ESP_OK) {
        burner_zip_item_list_free(&files);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "collect system folders failed");
    }
    if (files.count == 0U || files.count > 0xFFFFU) {
        burner_zip_item_list_free(&files);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "invalid migration zip entry count");
    }

    if (snprintf(
            disp,
            sizeof(disp),
            "attachment; filename=\"%s\"",
            SYSTEM_MIGRATE_ZIP_NAME) >= (int)sizeof(disp)) {
        burner_zip_item_list_free(&files);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "header build failed");
    }

    httpd_resp_set_type(req, "application/zip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    for (size_t i = 0; i < files.count; i++) {
        err = burner_zip_write_file(&stream, &files.items[i]);
        if (err != ESP_OK) {
            burner_zip_item_list_free(&files);
            return err;
        }
    }

    central_offset = stream.offset;
    for (size_t i = 0; i < files.count; i++) {
        err = burner_zip_write_central_entry(&stream, &files.items[i]);
        if (err != ESP_OK) {
            burner_zip_item_list_free(&files);
            return err;
        }
    }
    central_size = stream.offset - central_offset;

    err = burner_zip_write_end_record(&stream, (uint16_t)files.count, central_size, central_offset);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }

    burner_zip_item_list_free(&files);
    return err;
}

static esp_err_t burner_file_get_size_u32(FILE *fp, uint32_t *size_out)
{
    long file_size = 0;

    if (fp == NULL || size_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    file_size = ftell(fp);
    if (file_size < 0 || (uint64_t)file_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    *size_out = (uint32_t)file_size;
    return ESP_OK;
}

static esp_err_t burner_zip_find_eocd(
    FILE *fp,
    uint32_t file_size,
    uint16_t *entry_count_out,
    uint32_t *central_offset_out,
    uint32_t *central_size_out)
{
    uint8_t *tail = NULL;
    size_t search_len = 0;
    esp_err_t err = ESP_FAIL;

    if (fp == NULL || entry_count_out == NULL || central_offset_out == NULL || central_size_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (file_size < ZIP_EOCD_MIN_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    search_len = (file_size < ZIP_EOCD_MAX_SEARCH) ? (size_t)file_size : (size_t)ZIP_EOCD_MAX_SEARCH;
    tail = (uint8_t *)malloc(search_len);
    if (tail == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (fseek(fp, (long)(file_size - (uint32_t)search_len), SEEK_SET) != 0) {
        free(tail);
        return ESP_FAIL;
    }
    if (fread(tail, 1, search_len, fp) != search_len) {
        free(tail);
        return ESP_FAIL;
    }

    for (size_t pos = search_len - ZIP_EOCD_MIN_SIZE + 1U; pos-- > 0U;) {
        uint16_t disk_no;
        uint16_t start_disk;
        uint16_t entries_this_disk;
        uint16_t entries_total;
        uint16_t comment_len;
        uint32_t central_size;
        uint32_t central_offset;

        if (burner_zip_get_le32(&tail[pos]) != 0x06054B50U) {
            continue;
        }

        comment_len = burner_zip_get_le16(&tail[pos + 20U]);
        if (pos + ZIP_EOCD_MIN_SIZE + (size_t)comment_len != search_len) {
            continue;
        }

        disk_no = burner_zip_get_le16(&tail[pos + 4U]);
        start_disk = burner_zip_get_le16(&tail[pos + 6U]);
        entries_this_disk = burner_zip_get_le16(&tail[pos + 8U]);
        entries_total = burner_zip_get_le16(&tail[pos + 10U]);
        central_size = burner_zip_get_le32(&tail[pos + 12U]);
        central_offset = burner_zip_get_le32(&tail[pos + 16U]);

        if (disk_no != 0U || start_disk != 0U || entries_this_disk != entries_total) {
            err = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        if (entries_total == 0U) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }
        if (central_offset > file_size || central_size > (file_size - central_offset)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        *entry_count_out = entries_total;
        *central_offset_out = central_offset;
        *central_size_out = central_size;
        err = ESP_OK;
        break;
    }

    free(tail);
    return err;
}

static esp_err_t burner_zip_read_central_directory(FILE *fp, burner_zip_central_list_t *list)
{
    uint8_t header[46];
    uint32_t file_size = 0;
    uint16_t entry_count = 0;
    uint32_t central_offset = 0;
    uint32_t central_size = 0;
    esp_err_t err;

    if (fp == NULL || list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_file_get_size_u32(fp, &file_size);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_zip_find_eocd(fp, file_size, &entry_count, &central_offset, &central_size);
    if (err != ESP_OK) {
        return err;
    }

    if (central_offset + central_size > file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fp, (long)central_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    for (uint16_t i = 0; i < entry_count; i++) {
        uint16_t gp_flags;
        uint16_t method;
        uint32_t crc32;
        uint32_t compressed_size;
        uint32_t uncompressed_size;
        uint16_t name_len;
        uint16_t extra_len;
        uint16_t comment_len;
        uint32_t local_offset;
        burner_zip_central_entry_t item = {0};

        if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
            return ESP_FAIL;
        }
        if (burner_zip_get_le32(&header[0]) != 0x02014B50U) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        gp_flags = burner_zip_get_le16(&header[8]);
        method = burner_zip_get_le16(&header[10]);
        crc32 = burner_zip_get_le32(&header[16]);
        compressed_size = burner_zip_get_le32(&header[20]);
        uncompressed_size = burner_zip_get_le32(&header[24]);
        name_len = burner_zip_get_le16(&header[28]);
        extra_len = burner_zip_get_le16(&header[30]);
        comment_len = burner_zip_get_le16(&header[32]);
        local_offset = burner_zip_get_le32(&header[42]);

        if (name_len == 0U || name_len >= ZIP_ENTRY_NAME_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (fread(item.zip_name, 1, name_len, fp) != name_len) {
            return ESP_FAIL;
        }
        item.zip_name[name_len] = '\0';
        if ((extra_len > 0U || comment_len > 0U) &&
            fseek(fp, (long)((uint32_t)extra_len + (uint32_t)comment_len), SEEK_CUR) != 0) {
            return ESP_FAIL;
        }

        item.gp_flags = gp_flags;
        item.method = method;
        item.crc32 = crc32;
        item.compressed_size = compressed_size;
        item.uncompressed_size = uncompressed_size;
        item.local_offset = local_offset;
        item.is_dir = (item.zip_name[name_len - 1U] == '/');

        err = burner_zip_central_list_add(list, &item);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static bool burner_system_deploy_map_stage_rel(
    const char *normalized_rel,
    char *stage_rel,
    size_t stage_rel_len,
    bool *is_web_out,
    bool *is_setting_out)
{
    const char *suffix = NULL;

    if (normalized_rel == NULL || stage_rel == NULL || stage_rel_len < 2U) {
        return false;
    }
    if (strlen(normalized_rel) == 0U || strlen(normalized_rel) > SYSTEM_DEPLOY_REL_MAX_LEN) {
        return false;
    }

    if (strcmp(normalized_rel, WEB_ROOT_DIR_REL) == 0 ||
        strncmp(normalized_rel, WEB_ROOT_DIR_REL "/", strlen(WEB_ROOT_DIR_REL) + 1U) == 0) {
        suffix = normalized_rel + strlen(WEB_ROOT_DIR_REL);
        if (snprintf(stage_rel, stage_rel_len, "%s%s", SYSTEM_DEPLOY_STAGE_WEB_REL, suffix) >= (int)stage_rel_len) {
            return false;
        }
        if (is_web_out != NULL) {
            *is_web_out = true;
        }
        if (is_setting_out != NULL) {
            *is_setting_out = false;
        }
        return true;
    }

    if (strcmp(normalized_rel, WEB_LANG_DIR_REL) == 0 ||
        strncmp(normalized_rel, WEB_LANG_DIR_REL "/", strlen(WEB_LANG_DIR_REL) + 1U) == 0) {
        suffix = normalized_rel + strlen(WEB_LANG_DIR_REL);
        if (snprintf(stage_rel, stage_rel_len, "%s%s", SYSTEM_DEPLOY_STAGE_SETTING_REL, suffix) >= (int)stage_rel_len) {
            return false;
        }
        if (is_web_out != NULL) {
            *is_web_out = false;
        }
        if (is_setting_out != NULL) {
            *is_setting_out = true;
        }
        return true;
    }

    return false;
}

static bool burner_rel_parent(const char *rel_path, char *parent, size_t parent_len)
{
    const char *slash = NULL;
    size_t len = 0;

    if (rel_path == NULL || parent == NULL || parent_len == 0U) {
        return false;
    }

    parent[0] = '\0';
    slash = strrchr(rel_path, '/');
    if (slash == NULL) {
        return true;
    }
    len = (size_t)(slash - rel_path);
    if (len + 1U > parent_len) {
        return false;
    }
    memcpy(parent, rel_path, len);
    parent[len] = '\0';
    return true;
}

static esp_err_t burner_remove_rel_if_exists(const char *rel_path)
{
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    struct stat st;

    if (rel_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(full_path, &st) != 0) {
        return ESP_OK;
    }
    return burner_remove_recursive(full_path);
}

static esp_err_t burner_system_deploy_prepare_workspace(void)
{
    esp_err_t err;

    err = burner_remove_rel_if_exists(SYSTEM_DEPLOY_TMP_ROOT_REL);
    if (err != ESP_OK) {
        return err;
    }

    err = burner_mkdirs_rel(SYSTEM_DEPLOY_STAGE_WEB_REL);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_mkdirs_rel(SYSTEM_DEPLOY_STAGE_SETTING_REL);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t burner_system_deploy_save_upload_zip(httpd_req_t *req)
{
    char zip_full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    int remaining;
    uint32_t written_total = 0u;
    bool cancelled = false;

    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0 || req->content_len > (int)SYSTEM_DEPLOY_ZIP_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!burner_build_full_path(SYSTEM_DEPLOY_TMP_ZIP_REL, zip_full_path, sizeof(zip_full_path))) {
        return ESP_ERR_INVALID_SIZE;
    }

    fp = fopen(zip_full_path, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    burner_status_update(
        BURNER_STATE_RECEIVING,
        0,
        0,
        (uint32_t)req->content_len,
        "deploy zip upload started",
        "system_deploy.zip",
        zip_full_path);

    remaining = req->content_len;
    while (remaining > 0) {
        int recv_len;
        int to_recv = (remaining > TF_IO_CHUNK_SIZE) ? TF_IO_CHUNK_SIZE : remaining;

        if (burner_cancel_is_requested()) {
            cancelled = true;
            burner_status_update(
                BURNER_STATE_CANCELLED,
                burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
                written_total,
                (uint32_t)req->content_len,
                "deploy zip upload cancelled",
                "system_deploy.zip",
                zip_full_path);
            break;
        }

        recv_len = httpd_req_recv(req, (char *)buf, to_recv);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            free(buf);
            fclose(fp);
            unlink(zip_full_path);
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, (size_t)recv_len, fp) != (size_t)recv_len) {
            free(buf);
            fclose(fp);
            unlink(zip_full_path);
            return ESP_FAIL;
        }
        written_total += (uint32_t)recv_len;
        burner_status_update(
            BURNER_STATE_RECEIVING,
            burner_calc_progress_percent_u64(written_total, (uint64_t)(uint32_t)req->content_len),
            written_total,
            (uint32_t)req->content_len,
            "deploy zip uploading",
            "system_deploy.zip",
            zip_full_path);
        remaining -= recv_len;
    }

    free(buf);
    fclose(fp);
    if (cancelled) {
        unlink(zip_full_path);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t burner_system_deploy_extract_one_entry(
    FILE *zip_fp,
    uint32_t zip_file_size,
    const burner_zip_central_entry_t *entry,
    bool *has_web,
    bool *has_setting,
    size_t *file_count,
    uint64_t *total_bytes)
{
    char normalized_rel[TF_PATH_LEN_MAX] = {0};
    char stage_rel[TF_PATH_LEN_MAX] = {0};
    char parent_rel[TF_PATH_LEN_MAX] = {0};
    char stage_full[WEB_FILE_PATH_LEN_MAX] = {0};
    bool is_web = false;
    bool is_setting = false;
    uint8_t local_header[30] = {0};
    uint16_t local_name_len = 0U;
    uint16_t local_extra_len = 0U;
    uint16_t local_method = 0U;
    uint32_t data_offset = 0U;
    uint32_t remaining = 0U;
    uint8_t *buf = NULL;
    FILE *out_fp = NULL;
    uint32_t crc = 0xFFFFFFFFU;
    uint64_t written = 0U;
    esp_err_t err = ESP_OK;

    if (zip_fp == NULL || entry == NULL || has_web == NULL || has_setting == NULL ||
        file_count == NULL || total_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!burner_normalize_rel_path(entry->zip_name, normalized_rel, sizeof(normalized_rel), false)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!burner_system_deploy_map_stage_rel(
            normalized_rel,
            stage_rel,
            sizeof(stage_rel),
            &is_web,
            &is_setting)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (entry->is_dir) {
        if (burner_mkdirs_rel(stage_rel) != ESP_OK) {
            return ESP_FAIL;
        }
        *has_web = *has_web || is_web;
        *has_setting = *has_setting || is_setting;
        return ESP_OK;
    }

    if ((entry->gp_flags & 0x0001U) != 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (entry->method != ZIP_METHOD_STORE || entry->compressed_size != entry->uncompressed_size) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (entry->compressed_size > zip_file_size || entry->local_offset > zip_file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(normalized_rel, WEB_ROOT_DIR_REL) == 0 || strcmp(normalized_rel, WEB_LANG_DIR_REL) == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (fseek(zip_fp, (long)entry->local_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(local_header, 1, sizeof(local_header), zip_fp) != sizeof(local_header)) {
        return ESP_FAIL;
    }
    if (burner_zip_get_le32(&local_header[0]) != 0x04034B50U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    local_method = burner_zip_get_le16(&local_header[8]);
    local_name_len = burner_zip_get_le16(&local_header[26]);
    local_extra_len = burner_zip_get_le16(&local_header[28]);
    if (local_method != entry->method) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    data_offset = entry->local_offset + (uint32_t)sizeof(local_header) + (uint32_t)local_name_len +
                  (uint32_t)local_extra_len;
    if (data_offset > zip_file_size || entry->compressed_size > (zip_file_size - data_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!burner_rel_parent(stage_rel, parent_rel, sizeof(parent_rel))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (parent_rel[0] != '\0' && burner_mkdirs_rel(parent_rel) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!burner_build_full_path(stage_rel, stage_full, sizeof(stage_full))) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_fp = fopen(stage_full, "wb");
    if (out_fp == NULL) {
        return ESP_FAIL;
    }

    buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(out_fp);
        return ESP_ERR_NO_MEM;
    }

    if (fseek(zip_fp, (long)data_offset, SEEK_SET) != 0) {
        err = ESP_FAIL;
        goto extract_done;
    }

    remaining = entry->compressed_size;
    while (remaining > 0U) {
        size_t chunk = (remaining > TF_IO_CHUNK_SIZE) ? TF_IO_CHUNK_SIZE : (size_t)remaining;
        size_t read_len = fread(buf, 1, chunk, zip_fp);
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            goto extract_done;
        }
        if (read_len != chunk) {
            err = ESP_FAIL;
            goto extract_done;
        }
        if (fwrite(buf, 1, read_len, out_fp) != read_len) {
            err = ESP_FAIL;
            goto extract_done;
        }
        crc = burner_zip_crc32_update(crc, buf, read_len);
        written += read_len;
        remaining -= (uint32_t)read_len;
    }

    crc ^= 0xFFFFFFFFU;
    if (written != entry->uncompressed_size || crc != entry->crc32) {
        err = ESP_ERR_INVALID_CRC;
        goto extract_done;
    }

    *file_count += 1U;
    *total_bytes += written;
    *has_web = *has_web || is_web;
    *has_setting = *has_setting || is_setting;

extract_done:
    free(buf);
    if (out_fp != NULL) {
        fclose(out_fp);
        if (err != ESP_OK) {
            unlink(stage_full);
        }
    }
    return err;
}

static esp_err_t burner_system_deploy_extract_zip(
    bool *has_web,
    bool *has_setting,
    size_t *file_count,
    uint64_t *total_bytes)
{
    char zip_full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    FILE *zip_fp = NULL;
    uint32_t zip_file_size = 0;
    burner_zip_central_list_t list = {0};
    esp_err_t err = ESP_OK;

    if (has_web == NULL || has_setting == NULL || file_count == NULL || total_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_build_full_path(SYSTEM_DEPLOY_TMP_ZIP_REL, zip_full_path, sizeof(zip_full_path))) {
        return ESP_ERR_INVALID_SIZE;
    }

    zip_fp = fopen(zip_full_path, "rb");
    if (zip_fp == NULL) {
        return ESP_FAIL;
    }

    err = burner_file_get_size_u32(zip_fp, &zip_file_size);
    if (err == ESP_OK) {
        err = burner_zip_read_central_directory(zip_fp, &list);
    }
    if (err != ESP_OK) {
        goto extract_zip_done;
    }

    for (size_t i = 0; i < list.count; i++) {
        burner_status_update(
            BURNER_STATE_BURNING,
            burner_calc_progress_percent_u64(i, list.count),
            (uint32_t)i,
            (uint32_t)list.count,
            "deploy zip extracting",
            "system_deploy.zip",
            zip_full_path);
        err = burner_cancel_poll();
        if (err != ESP_OK) {
            break;
        }
        err = burner_system_deploy_extract_one_entry(
            zip_fp,
            zip_file_size,
            &list.items[i],
            has_web,
            has_setting,
            file_count,
            total_bytes);
        if (err != ESP_OK) {
            break;
        }
    }

extract_zip_done:
    burner_zip_central_list_free(&list);
    if (zip_fp != NULL) {
        fclose(zip_fp);
    }
    return err;
}

static esp_err_t burner_system_deploy_commit(bool has_web, bool has_setting)
{
    char stage_full[WEB_FILE_PATH_LEN_MAX] = {0};
    char target_full[WEB_FILE_PATH_LEN_MAX] = {0};
    struct stat st;
    bool deployed = false;

    if (!has_web && !has_setting) {
        return ESP_ERR_NOT_FOUND;
    }

    if (burner_cancel_is_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (has_web) {
        if (!burner_build_full_path(SYSTEM_DEPLOY_STAGE_WEB_REL, stage_full, sizeof(stage_full)) ||
            !burner_build_full_path(WEB_ROOT_DIR_REL, target_full, sizeof(target_full))) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(stage_full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return ESP_ERR_NOT_FOUND;
        }
        if (burner_remove_rel_if_exists(WEB_ROOT_DIR_REL) != ESP_OK) {
            return ESP_FAIL;
        }
        if (rename(stage_full, target_full) != 0) {
            return ESP_FAIL;
        }
        deployed = true;
    }

    if (has_setting) {
        if (!burner_build_full_path(SYSTEM_DEPLOY_STAGE_SETTING_REL, stage_full, sizeof(stage_full)) ||
            !burner_build_full_path(WEB_LANG_DIR_REL, target_full, sizeof(target_full))) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(stage_full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return ESP_ERR_NOT_FOUND;
        }
        if (burner_remove_rel_if_exists(WEB_LANG_DIR_REL) != ESP_OK) {
            return ESP_FAIL;
        }
        if (rename(stage_full, target_full) != 0) {
            return ESP_FAIL;
        }
        deployed = true;
    }

    return deployed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void burner_system_deploy_cleanup_workspace(void)
{
    (void)burner_remove_rel_if_exists(SYSTEM_DEPLOY_TMP_ROOT_REL);
}

esp_err_t burner_system_deploy_zip_handler(httpd_req_t *req)
{
    bool has_web = false;
    bool has_setting = false;
    size_t file_count = 0U;
    uint64_t total_bytes = 0U;
    esp_err_t err = ESP_OK;
    const char *err_msg = "deploy failed";
    char resp[192];

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
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing zip body");
    }
    if (req->content_len > (int)SYSTEM_DEPLOY_ZIP_MAX_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zip file too large");
    }

    burner_cancel_reset();

    err = burner_system_deploy_prepare_workspace();
    if (err != ESP_OK) {
        err_msg = "prepare deploy workspace failed";
        goto deploy_fail;
    }

    err = burner_system_deploy_save_upload_zip(req);
    if (err != ESP_OK) {
        err_msg = "save uploaded zip failed";
        goto deploy_fail;
    }

    err = burner_system_deploy_extract_zip(&has_web, &has_setting, &file_count, &total_bytes);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        err_msg = "zip contains unsupported compression or encryption";
        goto deploy_fail;
    }
    if (err != ESP_OK) {
        err_msg = "invalid deploy zip";
        goto deploy_fail;
    }
    if (file_count == 0U) {
        err = ESP_ERR_NOT_FOUND;
        err_msg = "deploy zip contains no files";
        goto deploy_fail;
    }

    burner_status_update(
        BURNER_STATE_BURNING,
        100,
        (uint32_t)file_count,
        (uint32_t)file_count,
        "deploy zip applying files",
        "system_deploy.zip",
        SYSTEM_DEPLOY_TMP_ZIP_REL);

    err = burner_system_deploy_commit(has_web, has_setting);
    if (err != ESP_OK) {
        err_msg = "apply deployed files failed";
        goto deploy_fail;
    }

    burner_system_deploy_cleanup_workspace();
    burner_status_update(
        BURNER_STATE_DONE,
        100,
        (uint32_t)file_count,
        (uint32_t)file_count,
        "deploy zip complete",
        "system_deploy.zip",
        SYSTEM_DEPLOY_TMP_ZIP_REL);
    burner_cancel_reset();

    if (snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":true,\"web\":%s,\"setting\":%s,\"files\":%u,\"bytes\":%" PRIu64 "}",
            has_web ? "true" : "false",
            has_setting ? "true" : "false",
            (unsigned)file_count,
            total_bytes) >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);

deploy_fail:
    burner_system_deploy_cleanup_workspace();
    if (burner_cancel_is_requested()) {
        burner_status_t snap;
        burner_status_snapshot(&snap);
        if (snap.state != BURNER_STATE_CANCELLED) {
            burner_status_update(
                BURNER_STATE_CANCELLED,
                snap.progress,
                snap.processed_bytes,
                snap.total_bytes,
                "deploy cancelled",
                "system_deploy.zip",
                SYSTEM_DEPLOY_TMP_ZIP_REL);
        }
        burner_cancel_reset();
        return httpd_resp_send_custom_err(req, "409 Conflict", "deploy cancelled");
    }
    burner_cancel_reset();
    if (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC ||
        err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
    }
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
}

esp_err_t burner_tf_delete_handler(httpd_req_t *req)
{
    char path_arg[TF_PATH_LEN_MAX] = {0};
    char rel_path[TF_PATH_LEN_MAX] = {0};
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    char esc_rel[TF_PATH_LEN_MAX * 2 + 8] = {0};
    char resp[TF_PATH_LEN_MAX * 2 + 48] = {0};

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_get_query_arg(req, "path", path_arg, sizeof(path_arg), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
    }
    if (!burner_normalize_rel_path(path_arg, rel_path, sizeof(rel_path), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
    }

    if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }

    if (burner_remove_recursive(full_path) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }

    if (!burner_json_escape(rel_path, esc_rel, sizeof(esc_rel))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    if (snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\"}", esc_rel) >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_tf_mkdir_handler(httpd_req_t *req)
{
    char path_arg[TF_PATH_LEN_MAX] = {0};
    char rel_path[TF_PATH_LEN_MAX] = {0};
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    char esc_rel[TF_PATH_LEN_MAX * 2 + 8] = {0};
    char resp[TF_PATH_LEN_MAX * 2 + 48] = {0};

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_get_query_arg(req, "path", path_arg, sizeof(path_arg), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
    }
    if (!burner_normalize_rel_path(path_arg, rel_path, sizeof(rel_path), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
    }

    if (burner_mkdirs_rel(rel_path) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
    }
    if (burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        (void)burner_apply_current_file_mtime(full_path, NULL);
    }

    if (!burner_json_escape(rel_path, esc_rel, sizeof(esc_rel))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    if (snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\"}", esc_rel) >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_tf_rename_handler(httpd_req_t *req)
{
    char from_arg[TF_PATH_LEN_MAX] = {0};
    char to_arg[TF_PATH_LEN_MAX] = {0};
    char from_rel[TF_PATH_LEN_MAX] = {0};
    char to_rel[TF_PATH_LEN_MAX] = {0};
    char from_full[TF_PATH_LEN_MAX + 64] = {0};
    char to_full[TF_PATH_LEN_MAX + 64] = {0};
    char to_parent_rel[TF_PATH_LEN_MAX] = {0};
    char to_parent_full[TF_PATH_LEN_MAX + 64] = {0};
    char esc_from[TF_PATH_LEN_MAX * 2 + 8] = {0};
    char esc_to[TF_PATH_LEN_MAX * 2 + 8] = {0};
    char resp[TF_PATH_LEN_MAX * 4 + 96] = {0};
    struct stat from_st;
    struct stat parent_st;
    const char *slash = NULL;
    size_t from_len = 0;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_get_query_arg(req, "from", from_arg, sizeof(from_arg), true) ||
        !burner_get_query_arg(req, "to", to_arg, sizeof(to_arg), true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing from/to");
    }

    if (!burner_normalize_rel_path(from_arg, from_rel, sizeof(from_rel), false) ||
        !burner_normalize_rel_path(to_arg, to_rel, sizeof(to_rel), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
    }

    if (!burner_build_full_path(from_rel, from_full, sizeof(from_full)) ||
        !burner_build_full_path(to_rel, to_full, sizeof(to_full))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }

    if (stat(from_full, &from_st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "source not found");
    }

    from_len = strlen(from_rel);
    if (S_ISDIR(from_st.st_mode) && strncmp(to_rel, from_rel, from_len) == 0 &&
        to_rel[from_len] == '/') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cannot move folder into itself");
    }

    slash = strrchr(to_rel, '/');
    if (slash != NULL) {
        size_t parent_len = (size_t)(slash - to_rel);
        if (parent_len >= sizeof(to_parent_rel)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target parent path too long");
        }
        memcpy(to_parent_rel, to_rel, parent_len);
        to_parent_rel[parent_len] = '\0';

        if (!burner_build_full_path(to_parent_rel, to_parent_full, sizeof(to_parent_full))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target parent path too long");
        }
        if (stat(to_parent_full, &parent_st) != 0 || !S_ISDIR(parent_st.st_mode)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target parent not found");
        }
    }

    if (rename(from_full, to_full) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    (void)burner_apply_current_file_mtime(to_full, NULL);

    if (!burner_json_escape(from_rel, esc_from, sizeof(esc_from)) ||
        !burner_json_escape(to_rel, esc_to, sizeof(esc_to))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    if (snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":true,\"from\":\"%s\",\"to\":\"%s\"}",
            esc_from,
            esc_to) >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

const char *burner_json_bool(bool value)
{
    return value ? "true" : "false";
}

uint16_t burner_ip5306_charge_current_cfg_ma(uint8_t chg_dig_ctl0)
{
    uint8_t bits = (uint8_t)(chg_dig_ctl0 & 0x1F);
    uint16_t ma = 50;

    if ((bits & 0x01U) != 0U) {
        ma += 100;
    }
    if ((bits & 0x02U) != 0U) {
        ma += 200;
    }
    if ((bits & 0x04U) != 0U) {
        ma += 400;
    }
    if ((bits & 0x08U) != 0U) {
        ma += 800;
    }
    if ((bits & 0x10U) != 0U) {
        ma += 1600;
    }

    return ma;
}

bool burner_ip5306_battery_level_code_known(uint8_t bat_level_raw)
{
    switch (bat_level_raw & 0xF0U) {
        case 0xE0:
        case 0xC0:
        case 0x80:
        case 0x00:
            return true;
        default:
            return false;
    }
}


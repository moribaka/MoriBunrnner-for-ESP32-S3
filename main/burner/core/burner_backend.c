#include "ws_server_internal.h"

#define BURNER_TASK_CORE_ID 1

static char *burner_trim_inplace(char *text)
{
    char *start = text;
    char *end = NULL;

    if (text == NULL) {
        return NULL;
    }

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (*start == '\0') {
        return start;
    }

    end = start + strlen(start) - 1;
    while (end >= start && isspace((unsigned char)*end)) {
        *end = '\0';
        if (end == start) {
            break;
        }
        end--;
    }

    return start;
}

static bool burner_ini_split_line(char *line, char **key, char **value)
{
    char *eq = NULL;
    char *trimmed = NULL;

    if (line == NULL || key == NULL || value == NULL) {
        return false;
    }

    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
        memmove(line, line + 3, strlen(line + 3) + 1);
    }

    trimmed = burner_trim_inplace(line);
    if (trimmed == NULL || trimmed[0] == '\0') {
        return false;
    }
    if (trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
        return false;
    }

    eq = strchr(trimmed, '=');
    if (eq == NULL) {
        return false;
    }
    *eq = '\0';

    *key = burner_trim_inplace(trimmed);
    *value = burner_trim_inplace(eq + 1);
    if (*key == NULL || *value == NULL || (*key)[0] == '\0') {
        return false;
    }

    return true;
}

static const char *burner_erase_mode_to_str(bool erase_always)
{
    return erase_always ? "force" : "smart";
}

static bool burner_parse_erase_mode_text(const char *text, bool *erase_always_out)
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

static const char *burner_gbc_voltage_to_str(bool use_5v)
{
    return use_5v ? "5v" : "3v3";
}

static bool burner_parse_gbc_voltage_text(const char *text, bool *use_5v_out)
{
    if (text == NULL || use_5v_out == NULL) {
        return false;
    }
    if (strcasecmp(text, "5v") == 0 || strcmp(text, "5") == 0) {
        *use_5v_out = true;
        return true;
    }
    if (strcasecmp(text, "3v3") == 0 || strcasecmp(text, "3.3v") == 0 ||
        strcasecmp(text, "3.3") == 0 || strcmp(text, "3") == 0) {
        *use_5v_out = false;
        return true;
    }
    return false;
}

static bool burner_parse_power_settle_ms_text(const char *text, uint32_t *ms_out)
{
    uint32_t value = 0;

    if (text == NULL || ms_out == NULL) {
        return false;
    }
    if (!burner_parse_u32_text(text, &value)) {
        return false;
    }
    if (value != 100u && value != 200u && value != 400u && value != 800u && value != 1000u) {
        return false;
    }
    *ms_out = value;
    return true;
}

static bool burner_parse_psram_window_mb_text(const char *text, uint32_t *mb_out)
{
    uint32_t value = 0;

    if (text == NULL || mb_out == NULL) {
        return false;
    }
    if (text[0] == '\0' || strcasecmp(text, "auto") == 0 || strcmp(text, "0") == 0) {
        *mb_out = BURN_PSRAM_WINDOW_AUTO_MB;
        return true;
    }
    if (!burner_parse_u32_text(text, &value)) {
        return false;
    }
    if (value < BURN_PSRAM_WINDOW_MIN_MB || value > BURN_PSRAM_WINDOW_MAX_MB) {
        return false;
    }
    *mb_out = value;
    return true;
}

static const char *burner_psram_window_mb_to_text(uint32_t mb, char *out, size_t out_len)
{
    if (mb == BURN_PSRAM_WINDOW_AUTO_MB) {
        return "auto";
    }
    if (out == NULL || out_len == 0u) {
        return "";
    }
    snprintf(out, out_len, "%" PRIu32, mb);
    return out;
}

static uint32_t burner_mbc5_program_chunk_bytes_to_kb(uint32_t bytes)
{
    bytes = burner_clamp_mbc5_program_chunk_bytes(bytes);
    return bytes / 1024U;
}

static void burner_burn_config_apply_defaults(void)
{
    s_burn_core_cfg.erase_core = BURNER_CORE_AFFINITY_CPU1;
    s_burn_core_cfg.tf_core = BURNER_CORE_AFFINITY_CPU1;
    s_burn_core_cfg.psram_core = BURNER_CORE_AFFINITY_CPU1;
    s_burn_erase_always = BURN_ERASE_ALWAYS_DEFAULT;
    s_gba_fixed_erase_window_enabled = BURN_GBA_FIXED_ERASE_WINDOW_ENABLED_DEFAULT;
    s_mbc5_power_5v_enabled = 0u;
    s_bacon_power_settle_ms = BURNER_POWER_SETTLE_MS;
    s_burn_write_path_default = BURNER_WRITE_PATH_PSRAM;
    s_burn_recipe_mode_default = BURNER_RECIPE_MODE_CHIS;
    s_burn_psram_window_mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    s_burn_mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
    s_burn_dump_chunk_kb = BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
}

esp_err_t burner_load_burn_config(void)
{
    char path[WEB_FILE_PATH_LEN_MAX] = {0};
    FILE *fp = NULL;
    char line[WEB_LANG_LINE_MAX];
    bool erase_always = false;
    bool gba_fixed_erase_window = (BURN_GBA_FIXED_ERASE_WINDOW_ENABLED_DEFAULT != 0u);
    bool use_5v = false;
    uint32_t power_settle_ms = BURNER_POWER_SETTLE_MS;
    burner_write_path_t write_path = BURNER_WRITE_PATH_PSRAM;
    burner_recipe_mode_t recipe_mode = BURNER_RECIPE_MODE_CHIS;
    uint32_t psram_window_mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    uint32_t mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
    uint32_t dump_chunk_kb = BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
    burner_core_affinity_t erase_core = BURNER_CORE_AFFINITY_CPU1;
    burner_core_affinity_t tf_core = BURNER_CORE_AFFINITY_CPU1;
    burner_core_affinity_t psram_core = BURNER_CORE_AFFINITY_CPU1;

    burner_burn_config_apply_defaults();

    if (!burner_build_full_path(BURN_CONFIG_INI_REL, path, sizeof(path))) {
        return ESP_ERR_INVALID_SIZE;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0u && len == sizeof(line) - 1u && line[len - 1u] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) {
            line[--len] = '\0';
        }

        if (burner_ini_split_line(line, &key, &value)) {
            if (strcmp(key, "erase_mode") == 0) {
                (void)burner_parse_erase_mode_text(value, &erase_always);
            } else if (strcmp(key, "gbc_voltage") == 0) {
                (void)burner_parse_gbc_voltage_text(value, &use_5v);
            } else if (strcmp(key, "power_settle_ms") == 0) {
                (void)burner_parse_power_settle_ms_text(value, &power_settle_ms);
            } else if (strcmp(key, "write_path") == 0) {
                (void)burner_parse_write_path_text(value, &write_path);
            } else if (strcmp(key, "recipe_mode") == 0) {
                (void)burner_parse_recipe_mode_text(value, &recipe_mode);
            } else if (strcmp(key, "psram_window_mb") == 0) {
                (void)burner_parse_psram_window_mb_text(value, &psram_window_mb);
            } else if (strcmp(key, "mbc5_chunk_kb") == 0) {
                uint32_t parsed = 0;
                if (burner_parse_u32_text(value, &parsed)) {
                    mbc5_chunk_kb = burner_mbc5_program_chunk_bytes_to_kb(
                        burner_mbc5_program_chunk_kb_to_bytes(parsed));
                }
            } else if (strcmp(key, "dump_chunk_kb") == 0) {
                uint32_t parsed = 0;
                if (burner_parse_u32_text(value, &parsed)) {
                    dump_chunk_kb = burner_dump_chunk_bytes_to_kb(burner_dump_chunk_kb_to_bytes(parsed));
                }
            } else if (strcmp(key, "gba_fixed_erase_window") == 0) {
                (void)burner_parse_bool_text(value, &gba_fixed_erase_window);
            } else if (strcmp(key, "erase_core") == 0) {
                (void)burner_parse_core_affinity_text(value, &erase_core);
            } else if (strcmp(key, "tf_core") == 0) {
                (void)burner_parse_core_affinity_text(value, &tf_core);
            } else if (strcmp(key, "psram_core") == 0) {
                (void)burner_parse_core_affinity_text(value, &psram_core);
            }
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    fclose(fp);

    s_burn_erase_always = erase_always ? 1u : 0u;
    s_gba_fixed_erase_window_enabled = gba_fixed_erase_window ? 1u : 0u;
    s_mbc5_power_5v_enabled = use_5v ? 1u : 0u;
    s_bacon_power_settle_ms = power_settle_ms;
    s_burn_write_path_default = write_path;
    s_burn_recipe_mode_default = recipe_mode;
    s_burn_psram_window_mb = psram_window_mb;
    s_burn_mbc5_chunk_kb = mbc5_chunk_kb;
    s_burn_dump_chunk_kb = dump_chunk_kb;
    s_burn_core_cfg.erase_core = erase_core;
    s_burn_core_cfg.tf_core = tf_core;
    s_burn_core_cfg.psram_core = psram_core;

    {
        char psram_text[16] = {0};

        ESP_LOGI(
            BURNER_TAG,
            "burn config loaded: erase=%s write_path=%s recipe=%s psram_mb=%s mbc5_chunk=%" PRIu32
            " dump_chunk=%" PRIu32 " voltage=%s settle=%" PRIu32 "ms cores=%s/%s/%s",
            burner_erase_mode_to_str(s_burn_erase_always != 0u),
            burner_write_path_to_str(s_burn_write_path_default),
            burner_recipe_mode_to_str(s_burn_recipe_mode_default),
            burner_psram_window_mb_to_text(s_burn_psram_window_mb, psram_text, sizeof(psram_text)),
            s_burn_mbc5_chunk_kb,
            s_burn_dump_chunk_kb,
            burner_gbc_voltage_to_str(s_mbc5_power_5v_enabled != 0u),
            s_bacon_power_settle_ms,
            burner_core_affinity_to_str(s_burn_core_cfg.erase_core),
            burner_core_affinity_to_str(s_burn_core_cfg.tf_core),
            burner_core_affinity_to_str(s_burn_core_cfg.psram_core));
    }
    return ESP_OK;
}

esp_err_t burner_save_burn_config(void)
{
    char dir_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char path[WEB_FILE_PATH_LEN_MAX] = {0};
    char tmp_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char psram_text[16] = {0};
    FILE *fp = NULL;
    bool write_ok = false;

    if (!burner_build_full_path(BURN_CONFIG_DIR_REL, dir_path, sizeof(dir_path)) ||
        !burner_build_full_path(BURN_CONFIG_INI_REL, path, sizeof(path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mkdir(dir_path, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    if (fprintf(
            fp,
            "# MORI burn config\n"
            "erase_mode=%s\n"
            "gbc_voltage=%s\n"
            "power_settle_ms=%" PRIu32 "\n"
            "write_path=%s\n"
            "recipe_mode=%s\n"
            "psram_window_mb=%s\n"
            "mbc5_chunk_kb=%" PRIu32 "\n"
            "dump_chunk_kb=%" PRIu32 "\n"
            "gba_fixed_erase_window=%u\n"
            "erase_core=%s\n"
            "tf_core=%s\n"
            "psram_core=%s\n",
            burner_erase_mode_to_str(s_burn_erase_always != 0u),
            burner_gbc_voltage_to_str(s_mbc5_power_5v_enabled != 0u),
            s_bacon_power_settle_ms,
            burner_write_path_to_str(s_burn_write_path_default),
            burner_recipe_mode_to_str(s_burn_recipe_mode_default),
            burner_psram_window_mb_to_text(s_burn_psram_window_mb, psram_text, sizeof(psram_text)),
            s_burn_mbc5_chunk_kb,
            s_burn_dump_chunk_kb,
            (unsigned)s_gba_fixed_erase_window_enabled,
            burner_core_affinity_to_str(s_burn_core_cfg.erase_core),
            burner_core_affinity_to_str(s_burn_core_cfg.tf_core),
            burner_core_affinity_to_str(s_burn_core_cfg.psram_core)) > 0 &&
        fflush(fp) == 0) {
        write_ok = true;
    }

    if (fclose(fp) != 0) {
        write_ok = false;
    }
    fp = NULL;

    if (!write_ok) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void burner_spi_lock_take(void)
{
    if (s_spi_lock != NULL) {
        xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    }
}

void burner_spi_lock_give(void)
{
    if (s_spi_lock != NULL) {
        xSemaphoreGive(s_spi_lock);
    }
}

void burner_bacon_mark_activity_locked(void)
{
    s_bacon_last_active_tick = xTaskGetTickCount();
    s_bacon_idle_powered_down = false;
}

bool burner_task_is_running_snapshot(void)
{
    bool is_busy = false;

    if (s_status_lock != NULL) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        is_busy = (s_burn_task != NULL);
        xSemaphoreGive(s_status_lock);
    } else {
        is_busy = (s_burn_task != NULL);
    }
    return is_busy;
}

esp_err_t burner_backend_init(void)
{
    if (s_status_lock == NULL) {
        s_status_lock = xSemaphoreCreateMutex();
        if (s_status_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_spi_lock == NULL) {
        s_spi_lock = xSemaphoreCreateMutex();
        if (s_spi_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_bacon_last_active_tick = xTaskGetTickCount();
    s_bacon_idle_powered_down = false;

    if (s_bacon_idle_task == NULL) {
        if (xTaskCreatePinnedToCore(
                burner_bacon_idle_task_entry,
                "bacon_idle",
                3072,
                NULL,
                2,
                &s_bacon_idle_task,
                BURNER_TASK_CORE_ID) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t burner_spi_config_get_handler(httpd_req_t *req)
{
    char resp[240];
    int n;

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"configured_hz\":%" PRIu32 ",\"actual_hz\":%" PRIu32
        ",\"min_hz\":%u,\"max_hz\":%u,\"fixed\":true}",
        s_mcu_spi_clock_hz,
        s_mcu_spi_actual_hz,
        (unsigned)BURNER_SPI_CLOCK_HZ,
        (unsigned)BURNER_SPI_CLOCK_HZ);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

static int burner_build_core_config_json(char *resp, size_t resp_len)
{
    char psram_text[16] = {0};

    return snprintf(
        resp,
        resp_len,
        "{\"ok\":true,\"erase\":\"%s\",\"tf\":\"%s\",\"psram\":\"%s\","
        "\"power_settle_ms\":%" PRIu32 ",\"power_settle_options\":[100,200,400,800,1000],"
        "\"write_path\":\"%s\",\"recipe_mode\":\"%s\",\"pipeline_erase\":\"%s\",\"psram_mb\":\"%s\","
        "\"mbc5_chunk_kb\":%" PRIu32 ",\"dump_chunk_kb\":%" PRIu32 ","
        "\"gbc_voltage\":\"%s\",\"gba_fixed_erase_window\":%s,"
        "\"options\":[\"auto\",\"cpu0\",\"cpu1\"]}",
        burner_core_affinity_to_str(s_burn_core_cfg.erase_core),
        burner_core_affinity_to_str(s_burn_core_cfg.tf_core),
        burner_core_affinity_to_str(s_burn_core_cfg.psram_core),
        s_bacon_power_settle_ms,
        burner_write_path_to_str(s_burn_write_path_default),
        burner_recipe_mode_to_str(s_burn_recipe_mode_default),
        burner_erase_mode_to_str(s_burn_erase_always != 0u),
        burner_psram_window_mb_to_text(s_burn_psram_window_mb, psram_text, sizeof(psram_text)),
        s_burn_mbc5_chunk_kb,
        s_burn_dump_chunk_kb,
        burner_gbc_voltage_to_str(s_mbc5_power_5v_enabled != 0u),
        (s_gba_fixed_erase_window_enabled != 0u) ? "true" : "false");
}

esp_err_t burner_core_config_get_handler(httpd_req_t *req)
{
    char resp[512];
    int n;

    n = burner_build_core_config_json(resp, sizeof(resp));
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

esp_err_t burner_core_config_post_handler(httpd_req_t *req)
{
    char erase_arg[16] = {0};
    char tf_arg[16] = {0};
    char psram_arg[16] = {0};
    char power_settle_arg[16] = {0};
    char write_path_arg[16] = {0};
    char recipe_mode_arg[16] = {0};
    char erase_mode_arg[16] = {0};
    char pipeline_erase_arg[16] = {0};
    char psram_mb_arg[16] = {0};
    char mbc5_chunk_kb_arg[16] = {0};
    char dump_chunk_kb_arg[16] = {0};
    char gbc_voltage_arg[16] = {0};
    char gba_fixed_erase_window_arg[16] = {0};
    burner_core_affinity_t erase_val = s_burn_core_cfg.erase_core;
    burner_core_affinity_t tf_val = s_burn_core_cfg.tf_core;
    burner_core_affinity_t psram_val = s_burn_core_cfg.psram_core;
    uint32_t power_settle_val = s_bacon_power_settle_ms;
    burner_write_path_t write_path_val = s_burn_write_path_default;
    burner_recipe_mode_t recipe_mode_val = s_burn_recipe_mode_default;
    bool erase_mode_val = (s_burn_erase_always != 0u);
    uint32_t psram_mb_val = s_burn_psram_window_mb;
    uint32_t mbc5_chunk_kb_val = s_burn_mbc5_chunk_kb;
    uint32_t dump_chunk_kb_val = s_burn_dump_chunk_kb;
    bool use_5v_val = (s_mbc5_power_5v_enabled != 0u);
    bool gba_fixed_erase_window_val = (s_gba_fixed_erase_window_enabled != 0u);
    bool update_erase = false;
    bool update_tf = false;
    bool update_psram = false;
    bool update_power_settle = false;
    bool update_write_path = false;
    bool update_recipe_mode = false;
    bool update_erase_mode = false;
    bool update_psram_mb = false;
    bool update_mbc5_chunk_kb = false;
    bool update_dump_chunk_kb = false;
    bool update_gbc_voltage = false;
    bool update_gba_fixed_erase_window = false;
    char resp[512];
    int n;

    if (!burner_get_query_arg(req, "erase", erase_arg, sizeof(erase_arg), false) ||
        !burner_get_query_arg(req, "tf", tf_arg, sizeof(tf_arg), false) ||
        !burner_get_query_arg(req, "psram", psram_arg, sizeof(psram_arg), false) ||
        !burner_get_query_arg(req, "power_settle_ms", power_settle_arg, sizeof(power_settle_arg), false) ||
        !burner_get_query_arg(req, "write_path", write_path_arg, sizeof(write_path_arg), false) ||
        !burner_get_query_arg(req, "recipe_mode", recipe_mode_arg, sizeof(recipe_mode_arg), false) ||
        !burner_get_query_arg(req, "erase_mode", erase_mode_arg, sizeof(erase_mode_arg), false) ||
        !burner_get_query_arg(req, "pipeline_erase", pipeline_erase_arg, sizeof(pipeline_erase_arg), false) ||
        !burner_get_query_arg(req, "psram_mb", psram_mb_arg, sizeof(psram_mb_arg), false) ||
        !burner_get_query_arg(req, "mbc5_chunk_kb", mbc5_chunk_kb_arg, sizeof(mbc5_chunk_kb_arg), false) ||
        !burner_get_query_arg(req, "dump_chunk_kb", dump_chunk_kb_arg, sizeof(dump_chunk_kb_arg), false) ||
        !burner_get_query_arg(req, "gbc_voltage", gbc_voltage_arg, sizeof(gbc_voltage_arg), false) ||
        !burner_get_query_arg(
            req,
            "gba_fixed_erase_window",
            gba_fixed_erase_window_arg,
            sizeof(gba_fixed_erase_window_arg),
            false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query");
    }
    if (erase_arg[0] != '\0') {
        if (!burner_parse_core_affinity_text(erase_arg, &erase_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "erase must be auto/cpu0/cpu1");
        }
        update_erase = true;
    }
    if (tf_arg[0] != '\0') {
        if (!burner_parse_core_affinity_text(tf_arg, &tf_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tf must be auto/cpu0/cpu1");
        }
        update_tf = true;
    }
    if (psram_arg[0] != '\0') {
        if (!burner_parse_core_affinity_text(psram_arg, &psram_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "psram must be auto/cpu0/cpu1");
        }
        update_psram = true;
    }
    if (power_settle_arg[0] != '\0') {
        if (!burner_parse_power_settle_ms_text(power_settle_arg, &power_settle_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "power_settle_ms must be 100/200/400/800/1000");
        }
        update_power_settle = true;
    }
    if (write_path_arg[0] != '\0') {
        if (!burner_parse_write_path_text(write_path_arg, &write_path_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "write_path must be direct/psram/pipeline");
        }
        update_write_path = true;
    }
    if (recipe_mode_arg[0] != '\0') {
        if (!burner_parse_recipe_mode_text(recipe_mode_arg, &recipe_mode_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recipe_mode must be chis or gbx");
        }
        update_recipe_mode = true;
    }
    if (erase_mode_arg[0] != '\0') {
        if (!burner_parse_erase_mode_text(erase_mode_arg, &erase_mode_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "erase_mode must be smart or force");
        }
        update_erase_mode = true;
    }
    if (pipeline_erase_arg[0] != '\0') {
        if (!burner_parse_erase_mode_text(pipeline_erase_arg, &erase_mode_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pipeline_erase must be smart or force");
        }
        update_erase_mode = true;
    }
    if (psram_mb_arg[0] != '\0') {
        if (!burner_parse_psram_window_mb_text(psram_mb_arg, &psram_mb_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "psram_mb must be auto or integer 1..8");
        }
        update_psram_mb = true;
    }
    if (mbc5_chunk_kb_arg[0] != '\0') {
        uint32_t parsed = 0;
        if (!burner_parse_u32_text(mbc5_chunk_kb_arg, &parsed)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mbc5_chunk_kb must be integer");
        }
        mbc5_chunk_kb_val = burner_mbc5_program_chunk_bytes_to_kb(
            burner_mbc5_program_chunk_kb_to_bytes(parsed));
        update_mbc5_chunk_kb = true;
    }
    if (dump_chunk_kb_arg[0] != '\0') {
        uint32_t parsed = 0;
        if (!burner_parse_u32_text(dump_chunk_kb_arg, &parsed)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dump_chunk_kb must be integer");
        }
        dump_chunk_kb_val = burner_dump_chunk_bytes_to_kb(burner_dump_chunk_kb_to_bytes(parsed));
        if (dump_chunk_kb_val != parsed) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dump_chunk_kb must be 32/64/128/256");
        }
        update_dump_chunk_kb = true;
    }
    if (gbc_voltage_arg[0] != '\0') {
        if (!burner_parse_gbc_voltage_text(gbc_voltage_arg, &use_5v_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gbc_voltage must be 3v3 or 5v");
        }
        update_gbc_voltage = true;
    }
    if (gba_fixed_erase_window_arg[0] != '\0') {
        if (!burner_parse_bool_text(gba_fixed_erase_window_arg, &gba_fixed_erase_window_val)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gba_fixed_erase_window must be true/false/1/0");
        }
        update_gba_fixed_erase_window = true;
    }
    if (!update_erase && !update_tf && !update_psram && !update_power_settle &&
        !update_write_path && !update_recipe_mode && !update_erase_mode && !update_psram_mb &&
        !update_mbc5_chunk_kb && !update_dump_chunk_kb && !update_gbc_voltage &&
        !update_gba_fixed_erase_window) {
        return httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "at least one burn config query is required");
    }

    if (burner_task_is_running_snapshot()) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "burn task is running");
    }

    if (update_erase) {
        s_burn_core_cfg.erase_core = erase_val;
    }
    if (update_tf) {
        s_burn_core_cfg.tf_core = tf_val;
    }
    if (update_psram) {
        s_burn_core_cfg.psram_core = psram_val;
    }
    if (update_power_settle) {
        s_bacon_power_settle_ms = power_settle_val;
    }
    if (update_write_path) {
        s_burn_write_path_default = write_path_val;
    }
    if (update_recipe_mode) {
        s_burn_recipe_mode_default = recipe_mode_val;
    }
    if (update_erase_mode) {
        s_burn_erase_always = erase_mode_val ? 1u : 0u;
    }
    if (update_psram_mb) {
        s_burn_psram_window_mb = psram_mb_val;
    }
    if (update_mbc5_chunk_kb) {
        s_burn_mbc5_chunk_kb = mbc5_chunk_kb_val;
    }
    if (update_dump_chunk_kb) {
        s_burn_dump_chunk_kb = dump_chunk_kb_val;
    }
    if (update_gbc_voltage) {
        s_mbc5_power_5v_enabled = use_5v_val ? 1u : 0u;
    }
    if (update_gba_fixed_erase_window) {
        s_gba_fixed_erase_window_enabled = gba_fixed_erase_window_val ? 1u : 0u;
    }

    if (burner_save_burn_config() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save burn_config.ini failed");
    }

    n = burner_build_core_config_json(resp, sizeof(resp));
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    return burner_send_json(req, resp);
}

void burner_bacon_idle_task_entry(void *param)
{
    const TickType_t interval_ticks = pdMS_TO_TICKS(BURNER_IDLE_MONITOR_INTERVAL_MS);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(BURNER_IDLE_POWER_TIMEOUT_MS);

    (void)param;

    for (;;) {
        TickType_t now_tick;
        TickType_t last_tick;
        esp_err_t err;
        bool should_sleep = false;

        vTaskDelay((interval_ticks == 0) ? 1 : interval_ticks);

        if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
            continue;
        }
        if (burner_task_is_running_snapshot()) {
            continue;
        }

        now_tick = xTaskGetTickCount();
        last_tick = s_bacon_last_active_tick;
        if ((TickType_t)(now_tick - last_tick) < timeout_ticks) {
            continue;
        }
        if (s_bacon_idle_powered_down) {
            continue;
        }

        if (s_spi_lock == NULL) {
            continue;
        }
        if (xSemaphoreTake(s_spi_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }

        do {
            now_tick = xTaskGetTickCount();
            last_tick = s_bacon_last_active_tick;
            if (!s_mcu_spi_ready || s_mcu_spi == NULL) {
                break;
            }
            if (s_bacon_idle_powered_down) {
                break;
            }
            if (s_burn_task != NULL) {
                break;
            }
            if ((TickType_t)(now_tick - last_tick) < timeout_ticks) {
                break;
            }
            should_sleep = true;
        } while (0);

        if (should_sleep) {
            err = burner_bacon_gba_power_cmd(false, false);
            if (err == ESP_OK) {
                s_bacon_idle_powered_down = true;
                burner_reset_cart_probe_state();
                ESP_LOGI(BURNER_TAG, "bacon idle timeout reached, cartridge power rails off");
            } else {
                /* Avoid retry storms when transport is unstable. */
                burner_bacon_mark_activity_locked();
                ESP_LOGW(BURNER_TAG, "idle power off failed: %s", esp_err_to_name(err));
            }
        }

        xSemaphoreGive(s_spi_lock);
    }
}

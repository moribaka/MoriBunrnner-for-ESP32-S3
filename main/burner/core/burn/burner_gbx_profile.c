#include "cJSON.h"

#define BURNER_GBX_PROFILE_MAX_BYTES (64U * 1024U)

static char *burner_gbx_read_text_file_alloc(const char *path, size_t max_bytes)
{
    FILE *fp = NULL;
    long file_size = 0;
    char *buffer = NULL;

    if (path == NULL) {
        return NULL;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    file_size = ftell(fp);
    if (file_size <= 0 || (size_t)file_size > max_bytes) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    buffer = (char *)calloc((size_t)file_size + 1u, 1u);
    if (buffer == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buffer, 1u, (size_t)file_size, fp) != (size_t)file_size) {
        free(buffer);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buffer[file_size] = '\0';
    return buffer;
}

static char *burner_gbx_normalize_json_numbers(const char *raw_text)
{
    size_t src_len;
    size_t dst_cap;
    char *dst = NULL;
    size_t out_pos = 0u;
    bool in_string = false;
    bool escaped = false;

    if (raw_text == NULL) {
        return NULL;
    }

    src_len = strlen(raw_text);
    dst_cap = (src_len * 3u) + 64u;
    dst = (char *)calloc(dst_cap, 1u);
    if (dst == NULL) {
        return NULL;
    }

    for (size_t i = 0u; i < src_len; ++i) {
        char ch = raw_text[i];

        if (in_string) {
            dst[out_pos++] = ch;
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            dst[out_pos++] = ch;
            continue;
        }

        if (ch == '0' &&
            (i + 2u) < src_len &&
            (raw_text[i + 1u] == 'x' || raw_text[i + 1u] == 'X') &&
            isxdigit((unsigned char)raw_text[i + 2u])) {
            uint64_t value = 0u;
            char temp[32] = {0};
            int n = 0;

            ++i;
            while ((i + 1u) < src_len && isxdigit((unsigned char)raw_text[i + 1u])) {
                char hex = raw_text[i + 1u];

                value <<= 4u;
                if (hex >= '0' && hex <= '9') {
                    value |= (uint64_t)(hex - '0');
                } else if (hex >= 'a' && hex <= 'f') {
                    value |= (uint64_t)(10 + (hex - 'a'));
                } else {
                    value |= (uint64_t)(10 + (hex - 'A'));
                }
                ++i;
            }

            n = snprintf(temp, sizeof(temp), "%" PRIu64, value);
            if (n <= 0 || (size_t)n >= sizeof(temp)) {
                free(dst);
                return NULL;
            }
            memcpy(dst + out_pos, temp, (size_t)n);
            out_pos += (size_t)n;
            continue;
        }

        dst[out_pos++] = ch;
    }

    dst[out_pos] = '\0';
    return dst;
}

void burner_gbx_profile_clear(burner_gbx_profile_t *profile)
{
    if (profile != NULL) {
        memset(profile, 0, sizeof(*profile));
    }
}

static burner_nor_cmdset_t burner_gbx_cmdset_from_text(const char *text)
{
    if (text == NULL) {
        return BURNER_NOR_CMDSET_UNKNOWN;
    }
    if (strcasecmp(text, "AMD") == 0) {
        return BURNER_NOR_CMDSET_AMD;
    }
    if (strcasecmp(text, "INTEL") == 0) {
        return BURNER_NOR_CMDSET_INTEL;
    }
    return BURNER_NOR_CMDSET_UNKNOWN;
}

static bool burner_gbx_parse_addr_token(
    const cJSON *item,
    burner_gbx_addr_kind_t *addr_kind_out,
    uint32_t *addr_value_out)
{
    if (addr_kind_out == NULL || addr_value_out == NULL) {
        return false;
    }

    *addr_kind_out = BURNER_GBX_ADDR_NONE;
    *addr_value_out = 0u;

    if (item == NULL || cJSON_IsNull(item)) {
        return true;
    }
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0.0 || item->valuedouble > (double)UINT32_MAX) {
            return false;
        }
        *addr_kind_out = BURNER_GBX_ADDR_ABS;
        *addr_value_out = (uint32_t)item->valuedouble;
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    if (strcasecmp(item->valuestring, "SA") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_SA;
        return true;
    }
    if (strcasecmp(item->valuestring, "SA+1") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_SA_PLUS_1;
        return true;
    }
    if (strcasecmp(item->valuestring, "SA+2") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_SA_PLUS_2;
        return true;
    }
    if (strcasecmp(item->valuestring, "SA+66") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_SA_PLUS_66;
        return true;
    }
    if (strcasecmp(item->valuestring, "SA+132") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_SA_PLUS_132;
        return true;
    }
    if (strcasecmp(item->valuestring, "SA+16384") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_SA_PLUS_16384;
        return true;
    }
    if (strcasecmp(item->valuestring, "SA+28672") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_SA_PLUS_28672;
        return true;
    }
    if (strcasecmp(item->valuestring, "PA") == 0) {
        *addr_kind_out = BURNER_GBX_ADDR_PA;
        return true;
    }

    return false;
}

static bool burner_gbx_parse_data_token(
    const cJSON *item,
    burner_gbx_data_kind_t *data_kind_out,
    uint16_t *data_value_out)
{
    if (data_kind_out == NULL || data_value_out == NULL) {
        return false;
    }

    *data_kind_out = BURNER_GBX_DATA_NONE;
    *data_value_out = 0u;

    if (item == NULL || cJSON_IsNull(item)) {
        return true;
    }
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0.0 || item->valuedouble > 65535.0) {
            return false;
        }
        *data_kind_out = BURNER_GBX_DATA_VALUE;
        *data_value_out = (uint16_t)item->valuedouble;
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    if (strcasecmp(item->valuestring, "PD") == 0) {
        *data_kind_out = BURNER_GBX_DATA_PD;
        return true;
    }
    if (strcasecmp(item->valuestring, "BS") == 0) {
        *data_kind_out = BURNER_GBX_DATA_BS;
        return true;
    }

    return false;
}

static esp_err_t burner_gbx_parse_cmd_step(const cJSON *item, burner_gbx_cmd_step_t *step_out)
{
    const cJSON *addr_item;
    const cJSON *data_item;

    if (!cJSON_IsArray(item) || step_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(step_out, 0, sizeof(*step_out));
    addr_item = cJSON_GetArrayItem((cJSON *)item, 0);
    data_item = cJSON_GetArrayItem((cJSON *)item, 1);
    if (!burner_gbx_parse_addr_token(addr_item, &step_out->addr_kind, &step_out->addr_value) ||
        !burner_gbx_parse_data_token(data_item, &step_out->data_kind, &step_out->data_value)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t burner_gbx_parse_wait_step(const cJSON *item, burner_gbx_wait_step_t *step_out)
{
    const cJSON *addr_item;
    const cJSON *expect_item;
    const cJSON *mask_item;

    if (!cJSON_IsArray(item) || step_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(step_out, 0, sizeof(*step_out));
    addr_item = cJSON_GetArrayItem((cJSON *)item, 0);
    expect_item = cJSON_GetArrayItem((cJSON *)item, 1);
    mask_item = cJSON_GetArrayItem((cJSON *)item, 2);

    if ((addr_item == NULL || cJSON_IsNull(addr_item)) &&
        (expect_item == NULL || cJSON_IsNull(expect_item)) &&
        (mask_item == NULL || cJSON_IsNull(mask_item))) {
        return ESP_OK;
    }

    if (!burner_gbx_parse_addr_token(addr_item, &step_out->addr_kind, &step_out->addr_value) ||
        !burner_gbx_parse_data_token(expect_item, &step_out->expect_kind, &step_out->expect_value) ||
        !cJSON_IsNumber(mask_item) ||
        mask_item->valuedouble < 0.0 ||
        mask_item->valuedouble > 65535.0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    step_out->enabled = true;
    step_out->mask = (uint16_t)mask_item->valuedouble;
    return ESP_OK;
}

static esp_err_t burner_gbx_parse_cmd_list(const cJSON *item, burner_gbx_cmd_list_t *list_out)
{
    int count = 0;

    if (list_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(list_out, 0, sizeof(*list_out));
    if (item == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(item)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    count = cJSON_GetArraySize((cJSON *)item);
    if (count < 0 || count > (int)BURNER_GBX_CMD_STEP_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    list_out->count = (uint8_t)count;
    for (int i = 0; i < count; ++i) {
        esp_err_t err = burner_gbx_parse_cmd_step(cJSON_GetArrayItem((cJSON *)item, i), &list_out->steps[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t burner_gbx_parse_wait_list(const cJSON *item, burner_gbx_wait_list_t *list_out)
{
    int count = 0;

    if (list_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(list_out, 0, sizeof(*list_out));
    if (item == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(item)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    count = cJSON_GetArraySize((cJSON *)item);
    if (count < 0 || count > (int)BURNER_GBX_CMD_STEP_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    list_out->count = (uint8_t)count;
    for (int i = 0; i < count; ++i) {
        esp_err_t err = burner_gbx_parse_wait_step(cJSON_GetArrayItem((cJSON *)item, i), &list_out->steps[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t burner_gbx_parse_unlock_read_list(
    const cJSON *item,
    burner_gbx_unlock_read_step_t out_steps[BURNER_GBX_UNLOCK_READ_MAX],
    uint8_t *count_out)
{
    int count = 0;

    if (out_steps == NULL || count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count_out = 0u;
    if (item == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(item)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    count = cJSON_GetArraySize((cJSON *)item);
    if (count < 0 || count > (int)BURNER_GBX_UNLOCK_READ_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (int i = 0; i < count; ++i) {
        const cJSON *step = cJSON_GetArrayItem((cJSON *)item, i);
        const cJSON *addr_item;
        const cJSON *len_item;
        const cJSON *repeat_item;
        burner_gbx_addr_kind_t addr_kind = BURNER_GBX_ADDR_NONE;
        uint32_t addr_value = 0u;

        if (!cJSON_IsArray(step)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        addr_item = cJSON_GetArrayItem((cJSON *)step, 0);
        len_item = cJSON_GetArrayItem((cJSON *)step, 1);
        repeat_item = cJSON_GetArrayItem((cJSON *)step, 2);
        if (!burner_gbx_parse_addr_token(addr_item, &addr_kind, &addr_value) ||
            addr_kind != BURNER_GBX_ADDR_ABS ||
            !cJSON_IsNumber(len_item) ||
            !cJSON_IsNumber(repeat_item) ||
            len_item->valuedouble < 0.0 ||
            len_item->valuedouble > 65535.0 ||
            repeat_item->valuedouble < 0.0 ||
            repeat_item->valuedouble > 65535.0) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        out_steps[i].addr = addr_value;
        out_steps[i].len = (uint16_t)len_item->valuedouble;
        out_steps[i].repeat_count = (uint16_t)repeat_item->valuedouble;
    }

    *count_out = (uint8_t)count;
    return ESP_OK;
}

static esp_err_t burner_gbx_parse_flash_id_list(
    const cJSON *id_list,
    uint8_t ids[BURNER_GBX_FLASH_ID_MAX][BURNER_GBX_FLASH_ID_LEN_MAX],
    uint8_t lens[BURNER_GBX_FLASH_ID_MAX],
    uint8_t *count_out)
{
    const cJSON *candidate = NULL;
    uint8_t count = 0u;

    if (ids == NULL || lens == NULL || count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ids, 0, BURNER_GBX_FLASH_ID_MAX * BURNER_GBX_FLASH_ID_LEN_MAX);
    memset(lens, 0, BURNER_GBX_FLASH_ID_MAX);
    *count_out = 0u;

    if (id_list == NULL) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(id_list)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON_ArrayForEach(candidate, id_list) {
        int id_count = 0;

        if (!cJSON_IsArray(candidate)) {
            continue;
        }
        id_count = cJSON_GetArraySize((cJSON *)candidate);
        if (id_count <= 0 || id_count > (int)BURNER_GBX_FLASH_ID_LEN_MAX) {
            continue;
        }
        if (count >= BURNER_GBX_FLASH_ID_MAX) {
            ESP_LOGW(BURNER_TAG, "GBX profile has too many flash IDs; extra entries ignored");
            break;
        }
        for (int i = 0; i < id_count; ++i) {
            const cJSON *id_item = cJSON_GetArrayItem((cJSON *)candidate, i);

            if (!cJSON_IsNumber(id_item) ||
                id_item->valuedouble < 0.0 ||
                id_item->valuedouble > 255.0) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            ids[count][i] = (uint8_t)id_item->valuedouble;
        }
        lens[count] = (uint8_t)id_count;
        ++count;
    }

    *count_out = count;
    return ESP_OK;
}

static size_t burner_gbx_match_parsed_id_list(
    const uint8_t ids[BURNER_GBX_FLASH_ID_MAX][BURNER_GBX_FLASH_ID_LEN_MAX],
    const uint8_t lens[BURNER_GBX_FLASH_ID_MAX],
    uint8_t count,
    const uint8_t *gba_id,
    size_t gba_id_len)
{
    size_t best_len = 0u;

    if (ids == NULL || lens == NULL || gba_id == NULL || gba_id_len == 0u) {
        return 0u;
    }

    for (uint32_t index = 0u; index < count && index < BURNER_GBX_FLASH_ID_MAX; ++index) {
        size_t candidate_len = lens[index];

        if (candidate_len == 0u ||
            candidate_len > BURNER_GBX_FLASH_ID_LEN_MAX ||
            candidate_len > gba_id_len ||
            candidate_len <= best_len) {
            continue;
        }
        if (memcmp(ids[index], gba_id, candidate_len) == 0) {
            best_len = candidate_len;
        }
    }

    return best_len;
}

size_t burner_gbx_profile_match_id(
    const burner_gbx_profile_t *profile,
    const uint8_t *gba_id,
    size_t gba_id_len)
{
    size_t best_len = 0u;
    size_t bank_len = 0u;

    if (profile == NULL || gba_id == NULL || gba_id_len == 0u) {
        return 0u;
    }

    best_len = burner_gbx_match_parsed_id_list(
        profile->flash_ids,
        profile->flash_id_len,
        profile->flash_id_count,
        gba_id,
        gba_id_len);
    bank_len = burner_gbx_match_parsed_id_list(
        profile->flash_ids_banks,
        profile->flash_id_bank_len,
        profile->flash_id_bank_count,
        gba_id,
        gba_id_len);
    return bank_len > best_len ? bank_len : best_len;
}

static esp_err_t burner_gbx_parse_sector_geometry_from_json(
    const cJSON *item,
    uint32_t flash_size,
    burner_gbx_profile_t *profile)
{
    if (item == NULL || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0.0 || item->valuedouble > (double)UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        profile->sector_size = (uint32_t)item->valuedouble;
        profile->has_sector_size = true;
        if (flash_size != 0u && profile->sector_size != 0u &&
            burner_nor_geometry_set_uniform(&profile->sector_geometry, flash_size, profile->sector_size) == ESP_OK) {
            profile->has_sector_geometry = true;
        }
        return ESP_OK;
    }

    if (cJSON_IsArray(item)) {
        uint32_t sector_counts[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
        uint32_t sector_sizes[BURNER_NOR_GEOMETRY_REGION_MAX] = {0};
        int region_count = cJSON_GetArraySize((cJSON *)item);

        if (region_count <= 0 || region_count > (int)BURNER_NOR_GEOMETRY_REGION_MAX) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        for (int i = 0; i < region_count; ++i) {
            const cJSON *region = cJSON_GetArrayItem((cJSON *)item, i);
            const cJSON *size_item;
            const cJSON *count_item;

            if (!cJSON_IsArray(region) || cJSON_GetArraySize((cJSON *)region) != 2) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            size_item = cJSON_GetArrayItem((cJSON *)region, 0);
            count_item = cJSON_GetArrayItem((cJSON *)region, 1);
            if (!cJSON_IsNumber(size_item) || !cJSON_IsNumber(count_item) ||
                size_item->valuedouble < 0.0 || size_item->valuedouble > (double)UINT32_MAX ||
                count_item->valuedouble < 0.0 || count_item->valuedouble > (double)UINT32_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            sector_sizes[i] = (uint32_t)size_item->valuedouble;
            sector_counts[i] = (uint32_t)count_item->valuedouble;
        }

        if (burner_nor_geometry_build(
                &profile->sector_geometry,
                flash_size,
                sector_counts,
                sector_sizes,
                (uint32_t)region_count,
                false) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }

        profile->sector_size = burner_nor_geometry_report_sector_size(&profile->sector_geometry);
        profile->has_sector_size = (profile->sector_size != 0u);
        profile->has_sector_geometry = true;
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t burner_gbx_parse_profile_root(
    const cJSON *root,
    const char *entry_name,
    burner_gbx_profile_t *profile_out)
{
    const cJSON *item = NULL;
    const cJSON *names_item = NULL;
    const cJSON *commands = NULL;
    esp_err_t err = ESP_OK;

    if (root == NULL || entry_name == NULL || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_gbx_profile_clear(profile_out);
    snprintf(
        profile_out->file_name,
        sizeof(profile_out->file_name),
        "%.*s",
        (int)(sizeof(profile_out->file_name) - 1u),
        entry_name);

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "type");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(
            profile_out->type,
            sizeof(profile_out->type),
            "%.*s",
            (int)(sizeof(profile_out->type) - 1u),
            item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "command_set");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(
            profile_out->command_set_name,
            sizeof(profile_out->command_set_name),
            "%.*s",
            (int)(sizeof(profile_out->command_set_name) - 1u),
            item->valuestring);
        profile_out->base_cmdset = burner_gbx_cmdset_from_text(item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "flash_size");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= (double)UINT32_MAX) {
        profile_out->flash_size = (uint32_t)item->valuedouble;
        profile_out->has_flash_size = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "read_identifier_at");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= (double)UINT32_MAX) {
        profile_out->read_identifier_at = (uint32_t)item->valuedouble;
        profile_out->has_read_identifier_at = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "start_addr");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= (double)UINT32_MAX) {
        profile_out->start_addr = (uint32_t)item->valuedouble;
        profile_out->has_start_addr = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "first_bank");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= (double)UINT32_MAX) {
        profile_out->first_bank = (uint32_t)item->valuedouble;
        profile_out->has_first_bank = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "buffer_size");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 65535.0) {
        profile_out->buffer_size = (uint16_t)item->valuedouble;
        profile_out->has_buffer_size = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "reset_every");
    if (cJSON_IsNumber(item) && item->valuedouble > 0.0 && item->valuedouble <= (double)UINT32_MAX) {
        profile_out->reset_every = (uint32_t)item->valuedouble;
        profile_out->has_reset_every = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "chip_erase_timeout");
    if (cJSON_IsNumber(item) && item->valuedouble > 0.0 && item->valuedouble <= (double)UINT32_MAX) {
        profile_out->chip_erase_timeout_ms = (uint32_t)item->valuedouble * 1000u;
        profile_out->has_chip_erase_timeout = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "sector_size_from_cfi");
    if (cJSON_IsBool(item)) {
        profile_out->sector_size_from_cfi = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "wait_read_status_register");
    if (cJSON_IsBool(item)) {
        profile_out->wait_read_status_register = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "power_cycle");
    if (cJSON_IsBool(item)) {
        profile_out->power_cycle = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "flash_bank_select_type");
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0 && item->valuedouble <= 255.0) {
        profile_out->flash_bank_select_type = (uint8_t)item->valuedouble;
        profile_out->has_flash_bank_select_type = true;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "write_pin");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(
            profile_out->write_pin,
            sizeof(profile_out->write_pin),
            "%.*s",
            (int)(sizeof(profile_out->write_pin) - 1u),
            item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "mbc");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(
            profile_out->mbc_name,
            sizeof(profile_out->mbc_name),
            "%.*s",
            (int)(sizeof(profile_out->mbc_name) - 1u),
            item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        snprintf(
            profile_out->mbc_name,
            sizeof(profile_out->mbc_name),
            "0x%X",
            (unsigned)(uint32_t)item->valuedouble);
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "d0d1_swapped");
    if (!cJSON_IsBool(item)) {
        item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "d0_d1_swapped");
    }
    if (!cJSON_IsBool(item)) {
        item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "d_swap");
    }
    if (cJSON_IsBool(item)) {
        profile_out->d0d1_known = true;
        profile_out->d0d1_swapped = cJSON_IsTrue(item);
    }

    names_item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "names");
    if (cJSON_IsArray(names_item)) {
        item = cJSON_GetArrayItem((cJSON *)names_item, 0);
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            snprintf(
                profile_out->display_name,
                sizeof(profile_out->display_name),
                "%.*s",
                (int)(sizeof(profile_out->display_name) - 1u),
                item->valuestring);
        }
    }

    err = burner_gbx_parse_flash_id_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)root, "flash_ids"),
        profile_out->flash_ids,
        profile_out->flash_id_len,
        &profile_out->flash_id_count);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_flash_id_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)root, "flash_ids_banks"),
        profile_out->flash_ids_banks,
        profile_out->flash_id_bank_len,
        &profile_out->flash_id_bank_count);
    if (err != ESP_OK) {
        return err;
    }

    item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "sector_size");
    if (item != NULL) {
        err = burner_gbx_parse_sector_geometry_from_json(item, profile_out->flash_size, profile_out);
        if (err != ESP_OK) {
            return err;
        }
    }

    commands = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "commands");
    if (commands == NULL || !cJSON_IsObject(commands)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "unlock"),
        &profile_out->unlock);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_unlock_read_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "unlock_read"),
        profile_out->unlock_read,
        &profile_out->unlock_read_count);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "reset"),
        &profile_out->reset);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "read_status_register"),
        &profile_out->read_status_register);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "read_identifier"),
        &profile_out->read_identifier);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "read_cfi"),
        &profile_out->read_cfi);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "single_write"),
        &profile_out->single_write);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_wait_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "single_write_wait_for"),
        &profile_out->single_write_wait_for);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "buffer_write"),
        &profile_out->buffer_write);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_wait_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "buffer_write_wait_for"),
        &profile_out->buffer_write_wait_for);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "sector_erase"),
        &profile_out->sector_erase);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_wait_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "sector_erase_wait_for"),
        &profile_out->sector_erase_wait_for);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_cmd_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "chip_erase"),
        &profile_out->chip_erase);
    if (err != ESP_OK) {
        return err;
    }
    err = burner_gbx_parse_wait_list(
        cJSON_GetObjectItemCaseSensitive((cJSON *)commands, "chip_erase_wait_for"),
        &profile_out->chip_erase_wait_for);
    if (err != ESP_OK) {
        return err;
    }

    profile_out->active = true;
    return ESP_OK;
}

static bool burner_gbx_profile_entry_is_candidate(const char *entry_name)
{
    const char *ext = NULL;

    if (entry_name == NULL || strncasecmp(entry_name, "fc_", 3) != 0) {
        return false;
    }
    ext = strrchr(entry_name, '.');
    return ext != NULL && (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".json") == 0);
}

esp_err_t burner_gbx_visit_profiles_by_type(
    const char *type,
    burner_gbx_profile_visitor_t visitor,
    void *user)
{
    static const char *const s_dirs[] = {
        ".gbx",
        ".gbx/flashcart",
    };
    bool any_profile = false;

    if (type == NULL || visitor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t dir_index = 0u; dir_index < (sizeof(s_dirs) / sizeof(s_dirs[0])); ++dir_index) {
        char dir_path[WEB_FILE_PATH_LEN_MAX] = {0};
        DIR *dir = NULL;

        if (!burner_build_full_path(s_dirs[dir_index], dir_path, sizeof(dir_path))) {
            continue;
        }
        dir = opendir(dir_path);
        if (dir == NULL) {
            continue;
        }

        for (;;) {
            struct dirent *entry = readdir(dir);
            char full_path[WEB_FILE_PATH_LEN_MAX] = {0};
            char *raw_text = NULL;
            char *json_text = NULL;
            cJSON *root = NULL;
            const cJSON *type_item = NULL;
            burner_gbx_profile_t *candidate = NULL;
            esp_err_t parse_err;
            bool stop = false;

            if (entry == NULL) {
                break;
            }
            if (!burner_gbx_profile_entry_is_candidate(entry->d_name)) {
                continue;
            }
            if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(full_path)) {
                continue;
            }

            raw_text = burner_gbx_read_text_file_alloc(full_path, BURNER_GBX_PROFILE_MAX_BYTES);
            if (raw_text == NULL) {
                continue;
            }
            json_text = burner_gbx_normalize_json_numbers(raw_text);
            free(raw_text);
            raw_text = NULL;
            if (json_text == NULL) {
                continue;
            }

            root = cJSON_Parse(json_text);
            free(json_text);
            json_text = NULL;
            if (root == NULL) {
                continue;
            }

            type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
            if (!cJSON_IsString(type_item) ||
                type_item->valuestring == NULL ||
                strcasecmp(type_item->valuestring, type) != 0) {
                cJSON_Delete(root);
                continue;
            }

            candidate = (burner_gbx_profile_t *)calloc(1u, sizeof(*candidate));
            if (candidate == NULL) {
                cJSON_Delete(root);
                closedir(dir);
                return ESP_ERR_NO_MEM;
            }

            parse_err = burner_gbx_parse_profile_root(root, entry->d_name, candidate);
            cJSON_Delete(root);
            if (parse_err != ESP_OK) {
                free(candidate);
                continue;
            }
            any_profile = true;

            parse_err = visitor(candidate, user, &stop);
            free(candidate);
            if (parse_err != ESP_OK) {
                closedir(dir);
                return parse_err;
            }
            if (stop) {
                closedir(dir);
                return ESP_OK;
            }
        }

        closedir(dir);
    }

    return any_profile ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t burner_gbx_visit_agb_profiles(burner_gbx_profile_visitor_t visitor, void *user)
{
    return burner_gbx_visit_profiles_by_type("AGB", visitor, user);
}

esp_err_t burner_gbx_visit_dmg_profiles(burner_gbx_profile_visitor_t visitor, void *user)
{
    return burner_gbx_visit_profiles_by_type("DMG", visitor, user);
}

typedef struct {
    const uint8_t *id;
    size_t id_len;
    size_t best_match_len;
    burner_gbx_profile_t *profile_out;
} burner_gbx_lookup_ctx_t;

static esp_err_t burner_gbx_lookup_profile_visitor(
    const burner_gbx_profile_t *profile,
    void *user,
    bool *stop_out)
{
    burner_gbx_lookup_ctx_t *ctx = (burner_gbx_lookup_ctx_t *)user;
    size_t match_len;

    if (profile == NULL || ctx == NULL || stop_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *stop_out = false;
    match_len = burner_gbx_profile_match_id(profile, ctx->id, ctx->id_len);
    if (match_len > ctx->best_match_len) {
        ctx->best_match_len = match_len;
        *ctx->profile_out = *profile;
        if (match_len >= ctx->id_len) {
            *stop_out = true;
        }
    }
    return ESP_OK;
}

esp_err_t burner_gbx_lookup_profile_from_id(const uint8_t gba_id[8], burner_gbx_profile_t *profile_out)
{
    burner_gbx_lookup_ctx_t ctx = {
        .id = gba_id,
        .id_len = 8u,
        .best_match_len = 0u,
        .profile_out = profile_out,
    };
    esp_err_t err;

    if (gba_id == NULL || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_gbx_profile_clear(profile_out);
    err = burner_gbx_visit_agb_profiles(burner_gbx_lookup_profile_visitor, &ctx);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    return (ctx.best_match_len != 0u) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

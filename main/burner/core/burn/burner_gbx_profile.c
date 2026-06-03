#include "cJSON.h"

#define BURNER_GBX_PROFILE_MAX_BYTES (64U * 1024U)
#define BURNER_GBX_CACHE_DIR_REL ".gbx"
#define BURNER_GBX_CACHE_FILE_REL BURNER_GBX_CACHE_DIR_REL "/gbx_cache.bin"
#define BURNER_GBX_CACHE_TMP_REL BURNER_GBX_CACHE_DIR_REL "/gbx_cache.tmp"
#define BURNER_GBX_CACHE_MAGIC 0x5842474Du /* MGBX */
#define BURNER_GBX_CACHE_VERSION 4U
#define BURNER_GBX_CACHE_PATH_LEN 128U
#define BURNER_GBX_CACHE_MAX_BYTES (2U * 1024U * 1024U)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t entry_count;
    uint32_t profile_count;
} burner_gbx_cache_header_t;

typedef struct {
    char type[BURNER_GBX_PROFILE_TYPE_LEN];
    char rel_path[BURNER_GBX_CACHE_PATH_LEN];
    char file_name[BURNER_GBX_PROFILE_NAME_LEN];
    char display_name[BURNER_GBX_PROFILE_NAME_LEN];
    char command_set_name[BURNER_GBX_PROFILE_CMDSET_LEN];
    char write_pin[BURNER_GBX_PROFILE_WRITE_PIN_LEN];
    char mbc_name[BURNER_GBX_PROFILE_MBC_LEN];
    burner_gbx_cmd_list_t reset;
    burner_gbx_cmd_list_t unlock;
    burner_gbx_cmd_list_t read_identifier;
    uint8_t unlock_read_count;
    burner_gbx_unlock_read_step_t unlock_read[BURNER_GBX_UNLOCK_READ_MAX];
    burner_nor_geometry_t sector_geometry;
    uint32_t method_hash;
    uint32_t flash_size;
    uint32_t sector_size;
    uint32_t buffer_size;
    uint32_t reset_every;
    uint32_t read_identifier_at;
    uint32_t start_addr;
    uint32_t first_bank;
    uint8_t cmdset;
    uint8_t id_len;
    uint8_t match_index;
    uint8_t bank_id;
    uint8_t power_cycle;
    uint8_t sector_size_from_cfi;
    uint8_t has_flash_size;
    uint8_t has_sector_size;
    uint8_t has_sector_geometry;
    uint8_t has_buffer_size;
    uint8_t has_reset_every;
    uint8_t has_read_identifier_at;
    uint8_t has_start_addr;
    uint8_t has_first_bank;
    uint8_t has_flash_bank_select_type;
    uint8_t flash_bank_select_type;
    uint8_t id[BURNER_GBX_FLASH_ID_LEN_MAX];
} burner_gbx_cache_entry_t;

static burner_gbx_cache_entry_t *s_gbx_cache_entries = NULL;
static uint32_t s_gbx_cache_entry_count = 0u;
static uint32_t s_gbx_cache_profile_count = 0u;

static uint32_t burner_gbx_hash_u32(uint32_t hash, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i) {
        hash ^= (uint8_t)((value >> (i * 8u)) & 0xFFu);
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t burner_gbx_cmd_list_hash(const burner_gbx_cmd_list_t *list)
{
    uint32_t hash = 2166136261u;

    if (list == NULL) {
        return 0u;
    }
    hash = burner_gbx_hash_u32(hash, list->count);
    for (uint32_t i = 0u; i < list->count; ++i) {
        const burner_gbx_cmd_step_t *step = &list->steps[i];

        hash = burner_gbx_hash_u32(hash, (uint32_t)step->addr_kind);
        hash = burner_gbx_hash_u32(hash, step->addr_value);
        hash = burner_gbx_hash_u32(hash, (uint32_t)step->data_kind);
        hash = burner_gbx_hash_u32(hash, step->data_value);
    }
    return hash;
}

static bool burner_gbx_cmd_list_equal(const burner_gbx_cmd_list_t *left, const burner_gbx_cmd_list_t *right)
{
    if (left == NULL || right == NULL || left->count != right->count) {
        return false;
    }

    for (uint32_t i = 0u; i < left->count; ++i) {
        const burner_gbx_cmd_step_t *a = &left->steps[i];
        const burner_gbx_cmd_step_t *b = &right->steps[i];

        if (a->addr_kind != b->addr_kind ||
            a->addr_value != b->addr_value ||
            a->data_kind != b->data_kind ||
            a->data_value != b->data_value) {
            return false;
        }
    }
    return true;
}

static void burner_gbx_cache_entry_to_profile(
    const burner_gbx_cache_entry_t *entry,
    uint8_t method_max_id_len,
    burner_gbx_profile_t *profile)
{
    if (entry == NULL || profile == NULL) {
        return;
    }

    burner_gbx_profile_clear(profile);
    profile->active = true;
    profile->runtime_commands_enabled = false;
    profile->power_cycle = entry->power_cycle != 0u;
    profile->sector_size_from_cfi = entry->sector_size_from_cfi != 0u;
    profile->has_flash_size = entry->has_flash_size != 0u;
    profile->has_sector_size = entry->has_sector_size != 0u;
    profile->has_sector_geometry = entry->has_sector_geometry != 0u;
    profile->has_buffer_size = entry->has_buffer_size != 0u;
    profile->has_reset_every = entry->has_reset_every != 0u;
    profile->has_flash_bank_select_type = entry->has_flash_bank_select_type != 0u;
    profile->has_read_identifier_at = entry->has_read_identifier_at != 0u;
    profile->has_start_addr = entry->has_start_addr != 0u;
    profile->has_first_bank = entry->has_first_bank != 0u;
    profile->flash_bank_select_type = entry->flash_bank_select_type;
    profile->buffer_size = (uint16_t)entry->buffer_size;
    profile->flash_size = entry->flash_size;
    profile->sector_size = entry->sector_size;
    profile->reset_every = entry->reset_every;
    profile->read_identifier_at = entry->read_identifier_at;
    profile->start_addr = entry->start_addr;
    profile->first_bank = entry->first_bank;
    profile->base_cmdset = (burner_nor_cmdset_t)entry->cmdset;
    profile->sector_geometry = entry->sector_geometry;
    snprintf(profile->type, sizeof(profile->type), "%.*s", (int)(sizeof(profile->type) - 1u), entry->type);
    snprintf(
        profile->display_name,
        sizeof(profile->display_name),
        "%.*s",
        (int)(sizeof(profile->display_name) - 1u),
        entry->display_name);
    snprintf(
        profile->display_names[0],
        sizeof(profile->display_names[0]),
        "%.*s",
        (int)(sizeof(profile->display_names[0]) - 1u),
        entry->display_name);
    profile->display_name_count = (entry->display_name[0] != '\0') ? 1u : 0u;
    snprintf(
        profile->file_name,
        sizeof(profile->file_name),
        "%.*s",
        (int)(sizeof(profile->file_name) - 1u),
        entry->file_name);
    snprintf(
        profile->command_set_name,
        sizeof(profile->command_set_name),
        "%.*s",
        (int)(sizeof(profile->command_set_name) - 1u),
        entry->command_set_name);
    snprintf(
        profile->write_pin,
        sizeof(profile->write_pin),
        "%.*s",
        (int)(sizeof(profile->write_pin) - 1u),
        entry->write_pin);
    snprintf(
        profile->mbc_name,
        sizeof(profile->mbc_name),
        "%.*s",
        (int)(sizeof(profile->mbc_name) - 1u),
        entry->mbc_name);
    profile->reset = entry->reset;
    profile->unlock = entry->unlock;
    profile->read_identifier = entry->read_identifier;
    profile->unlock_read_count = entry->unlock_read_count;
    memcpy(profile->unlock_read, entry->unlock_read, sizeof(profile->unlock_read));
    profile->flash_id_count = 1u;
    profile->flash_id_len[0] = method_max_id_len;
    memcpy(profile->flash_ids[0], entry->id, entry->id_len);
}

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

static void burner_gbx_parse_names(
    const cJSON *names_item,
    burner_gbx_profile_t *profile)
{
    const cJSON *name_item = NULL;
    uint8_t count = 0u;

    if (profile == NULL || !cJSON_IsArray(names_item)) {
        return;
    }

    cJSON_ArrayForEach(name_item, names_item) {
        if (!cJSON_IsString(name_item) || name_item->valuestring == NULL) {
            continue;
        }
        if (count >= BURNER_GBX_FLASH_ID_MAX) {
            break;
        }
        snprintf(
            profile->display_names[count],
            sizeof(profile->display_names[count]),
            "%.*s",
            (int)(sizeof(profile->display_names[count]) - 1u),
            name_item->valuestring);
        if (count == 0u) {
            snprintf(
                profile->display_name,
                sizeof(profile->display_name),
                "%.*s",
                (int)(sizeof(profile->display_name) - 1u),
                name_item->valuestring);
        }
        ++count;
    }

    profile->display_name_count = count;
}

static bool burner_gbx_id_tail_is_blank(const uint8_t *id, size_t from, size_t id_len)
{
    if (id == NULL || from > id_len) {
        return false;
    }
    for (size_t i = from; i < id_len; ++i) {
        if (id[i] != 0x00u && id[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static bool burner_gbx_id_prefix_matches(
    const uint8_t *candidate,
    size_t candidate_len,
    const uint8_t *id,
    size_t id_len,
    bool allow_nonblank_tail)
{
    if (candidate == NULL || id == NULL || candidate_len == 0u || candidate_len > id_len) {
        return false;
    }
    if (!allow_nonblank_tail &&
        candidate_len < id_len &&
        !burner_gbx_id_tail_is_blank(id, candidate_len, id_len)) {
        return false;
    }
    return memcmp(candidate, id, candidate_len) == 0;
}

static size_t burner_gbx_match_parsed_id_list_ex(
    const uint8_t ids[BURNER_GBX_FLASH_ID_MAX][BURNER_GBX_FLASH_ID_LEN_MAX],
    const uint8_t lens[BURNER_GBX_FLASH_ID_MAX],
    uint8_t count,
    const uint8_t *gba_id,
    size_t gba_id_len,
    bool allow_nonblank_tail,
    uint8_t *match_index_out)
{
    size_t best_len = 0u;
    uint8_t best_index = 0u;

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
        if (burner_gbx_id_prefix_matches(
                ids[index],
                candidate_len,
                gba_id,
                gba_id_len,
                allow_nonblank_tail)) {
            best_len = candidate_len;
            best_index = (uint8_t)index;
        }
    }

    if (match_index_out != NULL && best_len != 0u) {
        *match_index_out = best_index;
    }
    return best_len;
}

static size_t burner_gbx_profile_match_id_mode_ex(
    const burner_gbx_profile_t *profile,
    const uint8_t *gba_id,
    size_t gba_id_len,
    bool allow_nonblank_tail,
    uint8_t *match_index_out,
    bool *bank_match_out)
{
    size_t best_len = 0u;
    size_t bank_len = 0u;
    uint8_t best_index = 0u;
    uint8_t bank_index = 0u;

    if (profile == NULL || gba_id == NULL || gba_id_len == 0u) {
        return 0u;
    }

    if (match_index_out != NULL) {
        *match_index_out = 0u;
    }
    if (bank_match_out != NULL) {
        *bank_match_out = false;
    }

    best_len = burner_gbx_match_parsed_id_list_ex(
        profile->flash_ids,
        profile->flash_id_len,
        profile->flash_id_count,
        gba_id,
        gba_id_len,
        allow_nonblank_tail,
        &best_index);
    bank_len = burner_gbx_match_parsed_id_list_ex(
        profile->flash_ids_banks,
        profile->flash_id_bank_len,
        profile->flash_id_bank_count,
        gba_id,
        gba_id_len,
        allow_nonblank_tail,
        &bank_index);
    if (bank_len > best_len) {
        if (match_index_out != NULL) {
            *match_index_out = bank_index;
        }
        if (bank_match_out != NULL) {
            *bank_match_out = true;
        }
        return bank_len;
    }
    if (best_len != 0u && match_index_out != NULL) {
        *match_index_out = best_index;
    }
    return best_len;
}

size_t burner_gbx_profile_match_id_ex(
    const burner_gbx_profile_t *profile,
    const uint8_t *gba_id,
    size_t gba_id_len,
    uint8_t *match_index_out,
    bool *bank_match_out)
{
    return burner_gbx_profile_match_id_mode_ex(
        profile,
        gba_id,
        gba_id_len,
        false,
        match_index_out,
        bank_match_out);
}

size_t burner_gbx_profile_match_id(
    const burner_gbx_profile_t *profile,
    const uint8_t *gba_id,
    size_t gba_id_len)
{
    return burner_gbx_profile_match_id_ex(profile, gba_id, gba_id_len, NULL, NULL);
}

void burner_gbx_profile_apply_match_name(
    burner_gbx_profile_t *profile,
    uint8_t match_index,
    bool bank_match)
{
    (void)bank_match;

    if (profile == NULL) {
        return;
    }
    if (match_index >= profile->display_name_count ||
        match_index >= BURNER_GBX_FLASH_ID_MAX ||
        profile->display_names[match_index][0] == '\0') {
        return;
    }

    snprintf(
        profile->display_name,
        sizeof(profile->display_name),
        "%.*s",
        (int)(sizeof(profile->display_name) - 1u),
        profile->display_names[match_index]);
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
    burner_gbx_parse_names(names_item, profile_out);

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

static const char *burner_gbx_basename(const char *path)
{
    const char *slash;

    if (path == NULL) {
        return "";
    }
    slash = strrchr(path, '/');
    return (slash != NULL) ? (slash + 1) : path;
}

static esp_err_t burner_gbx_parse_profile_file_rel(const char *rel_path, burner_gbx_profile_t *profile_out)
{
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char *raw_text = NULL;
    char *json_text = NULL;
    cJSON *root = NULL;
    esp_err_t err;

    if (rel_path == NULL || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        return ESP_ERR_INVALID_SIZE;
    }

    raw_text = burner_gbx_read_text_file_alloc(full_path, BURNER_GBX_PROFILE_MAX_BYTES);
    if (raw_text == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    json_text = burner_gbx_normalize_json_numbers(raw_text);
    free(raw_text);
    if (json_text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    root = cJSON_Parse(json_text);
    free(json_text);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = burner_gbx_parse_profile_root(root, burner_gbx_basename(rel_path), profile_out);
    cJSON_Delete(root);
    return err;
}

static void burner_gbx_cache_unload(void)
{
    if (s_gbx_cache_entries != NULL) {
        free(s_gbx_cache_entries);
    }
    s_gbx_cache_entries = NULL;
    s_gbx_cache_entry_count = 0u;
    s_gbx_cache_profile_count = 0u;
}

static esp_err_t burner_gbx_cache_validate_file(void)
{
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    struct stat st;
    FILE *fp = NULL;
    burner_gbx_cache_header_t header = {0};
    size_t entries_bytes;

    if (!burner_build_full_path(BURNER_GBX_CACHE_FILE_REL, full_path, sizeof(full_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(full_path, &st) != 0 || st.st_size < (off_t)sizeof(header) ||
        st.st_size > (off_t)BURNER_GBX_CACHE_MAX_BYTES) {
        return ESP_ERR_NOT_FOUND;
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fread(&header, 1u, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    fclose(fp);

    if (header.magic != BURNER_GBX_CACHE_MAGIC ||
        header.version != BURNER_GBX_CACHE_VERSION ||
        header.entry_size != sizeof(burner_gbx_cache_entry_t) ||
        header.entry_count == 0u) {
        return ESP_ERR_INVALID_VERSION;
    }

    entries_bytes = (size_t)header.entry_count * sizeof(burner_gbx_cache_entry_t);
    if (entries_bytes / sizeof(burner_gbx_cache_entry_t) != header.entry_count ||
        entries_bytes + sizeof(header) > (size_t)st.st_size ||
        entries_bytes + sizeof(header) > BURNER_GBX_CACHE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t burner_gbx_cache_load(void)
{
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    struct stat st;
    FILE *fp = NULL;
    burner_gbx_cache_header_t header = {0};
    burner_gbx_cache_entry_t *entries = NULL;
    size_t entries_bytes;
    size_t read_count;

    if (s_gbx_cache_entries != NULL) {
        return ESP_OK;
    }
    if (!burner_build_full_path(BURNER_GBX_CACHE_FILE_REL, full_path, sizeof(full_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(full_path, &st) != 0 || st.st_size < (off_t)sizeof(header) ||
        st.st_size > (off_t)BURNER_GBX_CACHE_MAX_BYTES) {
        return ESP_ERR_NOT_FOUND;
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fread(&header, 1u, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (header.magic != BURNER_GBX_CACHE_MAGIC ||
        header.version != BURNER_GBX_CACHE_VERSION ||
        header.entry_size != sizeof(burner_gbx_cache_entry_t) ||
        header.entry_count == 0u) {
        fclose(fp);
        return ESP_ERR_INVALID_VERSION;
    }

    entries_bytes = (size_t)header.entry_count * sizeof(burner_gbx_cache_entry_t);
    if (entries_bytes / sizeof(burner_gbx_cache_entry_t) != header.entry_count ||
        entries_bytes + sizeof(header) > (size_t)st.st_size ||
        entries_bytes + sizeof(header) > BURNER_GBX_CACHE_MAX_BYTES) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    entries = (burner_gbx_cache_entry_t *)heap_caps_malloc(entries_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (entries == NULL) {
        entries = (burner_gbx_cache_entry_t *)malloc(entries_bytes);
    }
    if (entries == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    read_count = fread(entries, sizeof(burner_gbx_cache_entry_t), header.entry_count, fp);
    fclose(fp);
    if (read_count != header.entry_count) {
        free(entries);
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_gbx_cache_entries = entries;
    s_gbx_cache_entry_count = header.entry_count;
    s_gbx_cache_profile_count = header.profile_count;
    ESP_LOGI(
        BURNER_TAG,
        "GBX cache loaded: profiles=%" PRIu32 " entries=%" PRIu32,
        s_gbx_cache_profile_count,
        s_gbx_cache_entry_count);
    return ESP_OK;
}

static esp_err_t burner_gbx_cache_load_full_profile(
    const burner_gbx_cache_entry_t *entry,
    burner_gbx_profile_t *profile_out)
{
    esp_err_t err;

    if (entry == NULL || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_gbx_parse_profile_file_rel(entry->rel_path, profile_out);
    if (err != ESP_OK) {
        ESP_LOGW(
            BURNER_TAG,
            "GBX cache matched stale/missing profile: file=%s rel=%s err=%s",
            entry->file_name,
            entry->rel_path,
            esp_err_to_name(err));
        burner_gbx_profile_clear(profile_out);
        return err;
    }
    burner_gbx_profile_apply_match_name(
        profile_out,
        entry->match_index,
        entry->bank_id != 0u);
    return ESP_OK;
}

static esp_err_t burner_gbx_cache_write_entry(
    FILE *fp,
    const char *rel_path,
    const burner_gbx_profile_t *profile,
    const uint8_t id[BURNER_GBX_FLASH_ID_LEN_MAX],
    uint8_t id_len,
    uint8_t match_index,
    bool bank_id)
{
    burner_gbx_cache_entry_t entry = {0};

    if (fp == NULL || rel_path == NULL || profile == NULL || id == NULL || id_len == 0u ||
        id_len > BURNER_GBX_FLASH_ID_LEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(entry.type, sizeof(entry.type), "%.*s", (int)(sizeof(entry.type) - 1u), profile->type);
    snprintf(entry.rel_path, sizeof(entry.rel_path), "%.*s", (int)(sizeof(entry.rel_path) - 1u), rel_path);
    snprintf(entry.file_name, sizeof(entry.file_name), "%.*s", (int)(sizeof(entry.file_name) - 1u), profile->file_name);
    if (match_index < profile->display_name_count &&
        match_index < BURNER_GBX_FLASH_ID_MAX &&
        profile->display_names[match_index][0] != '\0') {
        snprintf(
            entry.display_name,
            sizeof(entry.display_name),
            "%.*s",
            (int)(sizeof(entry.display_name) - 1u),
            profile->display_names[match_index]);
    } else {
        snprintf(
            entry.display_name,
            sizeof(entry.display_name),
            "%.*s",
            (int)(sizeof(entry.display_name) - 1u),
            profile->display_name);
    }
    snprintf(
        entry.command_set_name,
        sizeof(entry.command_set_name),
        "%.*s",
        (int)(sizeof(entry.command_set_name) - 1u),
        profile->command_set_name);
    snprintf(
        entry.write_pin,
        sizeof(entry.write_pin),
        "%.*s",
        (int)(sizeof(entry.write_pin) - 1u),
        profile->write_pin);
    snprintf(
        entry.mbc_name,
        sizeof(entry.mbc_name),
        "%.*s",
        (int)(sizeof(entry.mbc_name) - 1u),
        profile->mbc_name);
    entry.reset = profile->reset;
    entry.unlock = profile->unlock;
    entry.read_identifier = profile->read_identifier;
    entry.unlock_read_count = profile->unlock_read_count;
    memcpy(entry.unlock_read, profile->unlock_read, sizeof(entry.unlock_read));
    entry.sector_geometry = profile->sector_geometry;
    entry.method_hash = burner_gbx_cmd_list_hash(&profile->read_identifier);
    entry.flash_size = profile->flash_size;
    entry.sector_size = profile->sector_size;
    entry.buffer_size = profile->buffer_size;
    entry.reset_every = profile->reset_every;
    entry.read_identifier_at = profile->has_read_identifier_at ? profile->read_identifier_at : 0u;
    entry.start_addr = profile->has_start_addr ? profile->start_addr : 0u;
    entry.first_bank = profile->has_first_bank ? profile->first_bank : 0u;
    entry.cmdset = (uint8_t)profile->base_cmdset;
    entry.id_len = id_len;
    entry.match_index = match_index;
    entry.bank_id = bank_id ? 1u : 0u;
    entry.power_cycle = profile->power_cycle ? 1u : 0u;
    entry.sector_size_from_cfi = profile->sector_size_from_cfi ? 1u : 0u;
    entry.has_flash_size = profile->has_flash_size ? 1u : 0u;
    entry.has_sector_size = profile->has_sector_size ? 1u : 0u;
    entry.has_sector_geometry = profile->has_sector_geometry ? 1u : 0u;
    entry.has_buffer_size = profile->has_buffer_size ? 1u : 0u;
    entry.has_reset_every = profile->has_reset_every ? 1u : 0u;
    entry.has_read_identifier_at = profile->has_read_identifier_at ? 1u : 0u;
    entry.has_start_addr = profile->has_start_addr ? 1u : 0u;
    entry.has_first_bank = profile->has_first_bank ? 1u : 0u;
    entry.has_flash_bank_select_type = profile->has_flash_bank_select_type ? 1u : 0u;
    entry.flash_bank_select_type = profile->flash_bank_select_type;
    memcpy(entry.id, id, id_len);

    return (fwrite(&entry, 1u, sizeof(entry), fp) == sizeof(entry)) ? ESP_OK : ESP_FAIL;
}

esp_err_t burner_gbx_rebuild_cache(uint32_t *profile_count_out, uint32_t *entry_count_out)
{
    static const char *const s_dirs[] = {
        ".gbx",
        ".gbx/flashcart",
    };
    char tmp_full[WEB_FILE_PATH_LEN_MAX] = {0};
    char cache_full[WEB_FILE_PATH_LEN_MAX] = {0};
    FILE *fp = NULL;
    burner_gbx_cache_header_t header = {
        .magic = BURNER_GBX_CACHE_MAGIC,
        .version = BURNER_GBX_CACHE_VERSION,
        .entry_size = sizeof(burner_gbx_cache_entry_t),
        .entry_count = 0u,
        .profile_count = 0u,
    };
    esp_err_t err = ESP_OK;

    if (profile_count_out != NULL) {
        *profile_count_out = 0u;
    }
    if (entry_count_out != NULL) {
        *entry_count_out = 0u;
    }
    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = burner_mkdirs_rel(BURNER_GBX_CACHE_DIR_REL);
    if (err != ESP_OK) {
        return err;
    }
    if (!burner_build_full_path(BURNER_GBX_CACHE_TMP_REL, tmp_full, sizeof(tmp_full)) ||
        !burner_build_full_path(BURNER_GBX_CACHE_FILE_REL, cache_full, sizeof(cache_full))) {
        return ESP_ERR_INVALID_SIZE;
    }

    fp = fopen(tmp_full, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }
    if (fwrite(&header, 1u, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        unlink(tmp_full);
        return ESP_FAIL;
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
            char rel_path[BURNER_GBX_CACHE_PATH_LEN] = {0};
            burner_gbx_profile_t *profile = NULL;
            esp_err_t parse_err;

            if (entry == NULL) {
                break;
            }
            if (!burner_gbx_profile_entry_is_candidate(entry->d_name)) {
                continue;
            }
            if (snprintf(rel_path, sizeof(rel_path), "%s/%s", s_dirs[dir_index], entry->d_name) >=
                (int)sizeof(rel_path)) {
                continue;
            }

            profile = (burner_gbx_profile_t *)calloc(1u, sizeof(*profile));
            if (profile == NULL) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            parse_err = burner_gbx_parse_profile_file_rel(rel_path, profile);
            if (parse_err != ESP_OK) {
                free(profile);
                continue;
            }

            header.profile_count++;
            for (uint32_t i = 0u; i < profile->flash_id_count && i < BURNER_GBX_FLASH_ID_MAX; ++i) {
                err = burner_gbx_cache_write_entry(
                    fp,
                    rel_path,
                    profile,
                    profile->flash_ids[i],
                    profile->flash_id_len[i],
                    (uint8_t)i,
                    false);
                if (err != ESP_OK) {
                    break;
                }
                header.entry_count++;
            }
            for (uint32_t i = 0u; err == ESP_OK && i < profile->flash_id_bank_count && i < BURNER_GBX_FLASH_ID_MAX; ++i) {
                err = burner_gbx_cache_write_entry(
                    fp,
                    rel_path,
                    profile,
                    profile->flash_ids_banks[i],
                    profile->flash_id_bank_len[i],
                    (uint8_t)i,
                    true);
                if (err != ESP_OK) {
                    break;
                }
                header.entry_count++;
            }
            free(profile);
            if (err != ESP_OK) {
                break;
            }
            burner_task_yield_if_due();
        }

        closedir(dir);
        if (err != ESP_OK) {
            break;
        }
    }

    if (err == ESP_OK) {
        if (header.entry_count == 0u) {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    if (err == ESP_OK) {
        if (fseek(fp, 0, SEEK_SET) != 0 ||
            fwrite(&header, 1u, sizeof(header), fp) != sizeof(header)) {
            err = ESP_FAIL;
        }
    }
    if (fclose(fp) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        unlink(cache_full);
        if (rename(tmp_full, cache_full) != 0) {
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        unlink(tmp_full);
        return err;
    }

    burner_gbx_cache_unload();
    if (profile_count_out != NULL) {
        *profile_count_out = header.profile_count;
    }
    if (entry_count_out != NULL) {
        *entry_count_out = header.entry_count;
    }
    ESP_LOGI(
        BURNER_TAG,
        "GBX cache rebuilt: profiles=%" PRIu32 " entries=%" PRIu32 " file=%s",
        header.profile_count,
        header.entry_count,
        BURNER_GBX_CACHE_FILE_REL);
    return ESP_OK;
}

esp_err_t burner_gbx_ensure_cache(void)
{
    uint32_t profile_count = 0u;
    uint32_t entry_count = 0u;
    esp_err_t err;

    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = burner_gbx_cache_validate_file();
    if (err == ESP_OK) {
        ESP_LOGI(BURNER_TAG, "GBX cache exists, keep existing file: %s", BURNER_GBX_CACHE_FILE_REL);
        return ESP_OK;
    }
    burner_gbx_cache_unload();
    if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(
            BURNER_TAG,
            "GBX cache invalid, rebuilding: file=%s err=%s",
            BURNER_GBX_CACHE_FILE_REL,
            esp_err_to_name(err));
    } else {
        ESP_LOGI(BURNER_TAG, "GBX cache missing, auto rebuild: %s", BURNER_GBX_CACHE_FILE_REL);
    }

    err = burner_gbx_rebuild_cache(&profile_count, &entry_count);
    if (err == ESP_OK) {
        ESP_LOGI(
            BURNER_TAG,
            "GBX cache auto rebuilt: profiles=%" PRIu32 " entries=%" PRIu32,
            profile_count,
            entry_count);
    } else {
        ESP_LOGW(BURNER_TAG, "GBX cache auto rebuild skipped/failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t burner_gbx_find_cached_profile(
    const char *type,
    const burner_gbx_cmd_list_t *method,
    const uint8_t *id,
    size_t id_len,
    burner_gbx_profile_t *profile_out,
    size_t *match_len_out)
{
    uint32_t method_hash;
    const burner_gbx_cache_entry_t *best = NULL;
    size_t best_len = 0u;
    bool ambiguous = false;
    esp_err_t err;

    if (type == NULL || method == NULL || id == NULL || id_len == 0u || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (match_len_out != NULL) {
        *match_len_out = 0u;
    }

    err = burner_gbx_cache_load();
    if (err != ESP_OK) {
        return err;
    }
    method_hash = burner_gbx_cmd_list_hash(method);
    for (uint32_t i = 0u; i < s_gbx_cache_entry_count; ++i) {
        const burner_gbx_cache_entry_t *entry = &s_gbx_cache_entries[i];
        size_t candidate_len = entry->id_len;

        if (entry->method_hash != method_hash ||
            !burner_gbx_cmd_list_equal(&entry->read_identifier, method) ||
            strcasecmp(entry->type, type) != 0 ||
            candidate_len == 0u ||
            candidate_len > BURNER_GBX_FLASH_ID_LEN_MAX ||
            candidate_len > id_len ||
            (candidate_len < id_len &&
             !burner_gbx_id_tail_is_blank(id, candidate_len, id_len)) ||
            candidate_len < best_len) {
            continue;
        }
        if (memcmp(entry->id, id, candidate_len) != 0) {
            continue;
        }
        if (candidate_len > best_len || best == NULL) {
            best = entry;
            best_len = candidate_len;
            ambiguous = false;
        } else if (candidate_len == best_len &&
                   best != NULL &&
                   (strncmp(entry->rel_path, best->rel_path, sizeof(entry->rel_path)) != 0 ||
                    entry->flash_size != best->flash_size ||
                    entry->sector_size != best->sector_size ||
                    entry->cmdset != best->cmdset)) {
            ambiguous = true;
        }
    }
    if (best == NULL || ambiguous) {
        if (ambiguous) {
            ESP_LOGW(
                BURNER_TAG,
                "GBX cache match ambiguous: type=%s id_len=%u best_len=%u first=%s",
                type,
                (unsigned)id_len,
                (unsigned)best_len,
                best != NULL ? best->file_name : "none");
        }
        return ESP_ERR_NOT_FOUND;
    }

    err = burner_gbx_cache_load_full_profile(best, profile_out);
    if (err != ESP_OK) {
        return err;
    }
    if (match_len_out != NULL) {
        *match_len_out = best_len;
    }
    return ESP_OK;
}

esp_err_t burner_gbx_find_cached_profile_by_id(
    const char *type,
    const uint8_t *id,
    size_t id_len,
    burner_gbx_profile_t *profile_out,
    size_t *match_len_out)
{
    const burner_gbx_cache_entry_t *best = NULL;
    size_t best_len = 0u;
    bool ambiguous = false;
    esp_err_t err;

    if (type == NULL || id == NULL || id_len == 0u || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (match_len_out != NULL) {
        *match_len_out = 0u;
    }
    burner_gbx_profile_clear(profile_out);

    err = burner_gbx_cache_load();
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t i = 0u; i < s_gbx_cache_entry_count; ++i) {
        const burner_gbx_cache_entry_t *entry = &s_gbx_cache_entries[i];
        size_t candidate_len = entry->id_len;

        if (strcasecmp(entry->type, type) != 0 ||
            candidate_len == 0u ||
            candidate_len > BURNER_GBX_FLASH_ID_LEN_MAX ||
            candidate_len > id_len ||
            (candidate_len < id_len &&
             !burner_gbx_id_tail_is_blank(id, candidate_len, id_len)) ||
            candidate_len < best_len) {
            continue;
        }
        if (memcmp(entry->id, id, candidate_len) != 0) {
            continue;
        }
        if (candidate_len > best_len || best == NULL) {
            best = entry;
            best_len = candidate_len;
            ambiguous = false;
        } else if (candidate_len == best_len &&
                   best != NULL &&
                   strncmp(entry->rel_path, best->rel_path, sizeof(entry->rel_path)) != 0) {
            ambiguous = true;
        }
    }
    if (best == NULL || ambiguous) {
        if (ambiguous) {
            ESP_LOGW(
                BURNER_TAG,
                "GBX cache ID match ambiguous: type=%s id_len=%u best_len=%u first=%s",
                type,
                (unsigned)id_len,
                (unsigned)best_len,
                best != NULL ? best->file_name : "none");
        }
        return ESP_ERR_NOT_FOUND;
    }

    err = burner_gbx_cache_load_full_profile(best, profile_out);
    if (err != ESP_OK) {
        return err;
    }
    if (match_len_out != NULL) {
        *match_len_out = best_len;
    }
    return ESP_OK;
}

esp_err_t burner_gbx_visit_cached_methods_by_type(
    const char *type,
    burner_gbx_profile_visitor_t visitor,
    void *user)
{
    bool any_method = false;
    esp_err_t err;

    if (type == NULL || visitor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_gbx_cache_load();
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t i = 0u; i < s_gbx_cache_entry_count; ++i) {
        const burner_gbx_cache_entry_t *entry = &s_gbx_cache_entries[i];
        burner_gbx_profile_t profile = {0};
        bool duplicate_method = false;
        bool stop = false;
        uint8_t method_max_id_len = entry->id_len;

        if (strcasecmp(entry->type, type) != 0 || entry->method_hash == 0u) {
            continue;
        }
        for (uint32_t prev = 0u; prev < i; ++prev) {
            const burner_gbx_cache_entry_t *prev_entry = &s_gbx_cache_entries[prev];

            if (prev_entry->method_hash == entry->method_hash &&
                burner_gbx_cmd_list_equal(&prev_entry->read_identifier, &entry->read_identifier) &&
                strcasecmp(prev_entry->type, type) == 0) {
                duplicate_method = true;
                break;
            }
        }
        if (duplicate_method) {
            continue;
        }
        for (uint32_t next = i + 1u; next < s_gbx_cache_entry_count; ++next) {
            const burner_gbx_cache_entry_t *next_entry = &s_gbx_cache_entries[next];

            if (next_entry->method_hash == entry->method_hash &&
                burner_gbx_cmd_list_equal(&next_entry->read_identifier, &entry->read_identifier) &&
                strcasecmp(next_entry->type, type) == 0 &&
                next_entry->id_len > method_max_id_len) {
                method_max_id_len = next_entry->id_len;
            }
        }

        burner_gbx_cache_entry_to_profile(entry, method_max_id_len, &profile);
        any_method = true;
        err = visitor(&profile, user, &stop);
        if (err != ESP_OK) {
            return err;
        }
        if (stop) {
            return ESP_OK;
        }
    }

    return any_method ? ESP_OK : ESP_ERR_NOT_FOUND;
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
    const char *type;
    const burner_gbx_cmd_list_t *method;
    const uint8_t *id;
    size_t id_len;
    burner_nor_cmdset_t cmdset;
    bool d0d1_known;
    bool d0d1_swapped;
    uint32_t flash_size;
    uint32_t sector_size;
    const burner_nor_geometry_t *geometry;
    bool geometry_valid;
    bool cfi_ok;
    size_t best_match_len;
    int32_t best_score;
    uint8_t best_match_index;
    bool best_bank_match;
    bool allow_nonblank_tail;
    bool ambiguous;
    burner_gbx_profile_t *profile_out;
} burner_gbx_probe_lookup_ctx_t;

static bool burner_gbx_geometry_equal(
    const burner_nor_geometry_t *left,
    const burner_nor_geometry_t *right)
{
    if (left == NULL || right == NULL ||
        !burner_nor_geometry_is_valid(left) ||
        !burner_nor_geometry_is_valid(right) ||
        left->region_count != right->region_count ||
        left->uniform_sector_size != right->uniform_sector_size ||
        left->smallest_sector_size != right->smallest_sector_size ||
        left->largest_sector_size != right->largest_sector_size) {
        return false;
    }

    for (uint32_t i = 0u; i < left->region_count && i < BURNER_NOR_GEOMETRY_REGION_MAX; ++i) {
        if (left->regions[i].addr_begin != right->regions[i].addr_begin ||
            left->regions[i].addr_end != right->regions[i].addr_end ||
            left->regions[i].sector_size != right->regions[i].sector_size) {
            return false;
        }
    }
    return true;
}

static int32_t burner_gbx_profile_probe_score(
    const burner_gbx_profile_t *profile,
    const burner_gbx_probe_lookup_ctx_t *ctx,
    size_t *match_len_out,
    uint8_t *match_index_out,
    bool *bank_match_out)
{
    size_t match_len = 0u;
    uint8_t match_index = 0u;
    bool bank_match = false;
    int32_t score = 0;

    if (profile == NULL || ctx == NULL || ctx->id == NULL || ctx->id_len == 0u) {
        return INT32_MIN;
    }
    if (match_len_out != NULL) {
        *match_len_out = 0u;
    }
    if (match_index_out != NULL) {
        *match_index_out = 0u;
    }
    if (bank_match_out != NULL) {
        *bank_match_out = false;
    }
    if (ctx->type != NULL &&
        profile->type[0] != '\0' &&
        strcasecmp(profile->type, ctx->type) != 0) {
        return INT32_MIN;
    }
    if (ctx->method != NULL &&
        !burner_gbx_cmd_list_equal(&profile->read_identifier, ctx->method)) {
        return INT32_MIN;
    }

    match_len = burner_gbx_profile_match_id_ex(
        profile,
        ctx->id,
        ctx->id_len,
        &match_index,
        &bank_match);
    if (ctx->allow_nonblank_tail) {
        match_len = burner_gbx_profile_match_id_mode_ex(
            profile,
            ctx->id,
            ctx->id_len,
            true,
            &match_index,
            &bank_match);
    }
    if (match_len == 0u) {
        return INT32_MIN;
    }
    if (ctx->cmdset != BURNER_NOR_CMDSET_UNKNOWN &&
        profile->base_cmdset != BURNER_NOR_CMDSET_UNKNOWN &&
        profile->base_cmdset != ctx->cmdset) {
        return INT32_MIN;
    }
    if (ctx->d0d1_known &&
        profile->d0d1_known &&
        profile->d0d1_swapped != ctx->d0d1_swapped) {
        return INT32_MIN;
    }

    score = (int32_t)(match_len * 1000u);
    if (ctx->method != NULL) {
        score += 600;
    }
    if (ctx->cmdset != BURNER_NOR_CMDSET_UNKNOWN &&
        profile->base_cmdset == ctx->cmdset) {
        score += 500;
    }
    if (ctx->d0d1_known &&
        profile->d0d1_known &&
        profile->d0d1_swapped == ctx->d0d1_swapped) {
        score += 120;
    }
    if (ctx->flash_size != 0u &&
        profile->has_flash_size &&
        profile->flash_size == ctx->flash_size) {
        score += ctx->cfi_ok ? 400 : 100;
    }
    if (ctx->sector_size != 0u &&
        profile->has_sector_size &&
        profile->sector_size == ctx->sector_size) {
        score += ctx->cfi_ok ? 250 : 60;
    }
    if (ctx->geometry_valid && profile->has_sector_geometry) {
        if (burner_gbx_geometry_equal(&profile->sector_geometry, ctx->geometry)) {
            score += ctx->cfi_ok ? 350 : 90;
        } else if (ctx->sector_size != 0u &&
                   burner_nor_geometry_report_sector_size(&profile->sector_geometry) == ctx->sector_size) {
            score += 40;
        }
    }
    if (profile->has_flash_size) {
        score += 15;
    }
    if (profile->has_sector_geometry) {
        score += 20;
    } else if (profile->has_sector_size) {
        score += 10;
    }
    if (profile->has_buffer_size) {
        score += 5;
    }
    if (profile->has_flash_bank_select_type) {
        score += 8;
    }

    if (match_len_out != NULL) {
        *match_len_out = match_len;
    }
    if (match_index_out != NULL) {
        *match_index_out = match_index;
    }
    if (bank_match_out != NULL) {
        *bank_match_out = bank_match;
    }
    return score;
}

static void burner_gbx_probe_lookup_consider(
    burner_gbx_probe_lookup_ctx_t *ctx,
    const burner_gbx_profile_t *profile,
    int32_t score,
    size_t match_len,
    uint8_t match_index,
    bool bank_match)
{
    if (ctx == NULL || profile == NULL || ctx->profile_out == NULL || score == INT32_MIN) {
        return;
    }

    if (ctx->best_match_len == 0u ||
        score > ctx->best_score ||
        (score == ctx->best_score && match_len > ctx->best_match_len)) {
        *ctx->profile_out = *profile;
        ctx->best_score = score;
        ctx->best_match_len = match_len;
        ctx->best_match_index = match_index;
        ctx->best_bank_match = bank_match;
        ctx->ambiguous = false;
        return;
    }

    if (score == ctx->best_score &&
        match_len == ctx->best_match_len &&
        strncmp(profile->file_name, ctx->profile_out->file_name, sizeof(profile->file_name)) != 0) {
        ctx->ambiguous = true;
    }
}

static esp_err_t burner_gbx_probe_lookup_visitor(
    const burner_gbx_profile_t *profile,
    void *user,
    bool *stop_out)
{
    burner_gbx_probe_lookup_ctx_t *ctx = (burner_gbx_probe_lookup_ctx_t *)user;
    size_t match_len = 0u;
    uint8_t match_index = 0u;
    bool bank_match = false;
    int32_t score;

    if (profile == NULL || ctx == NULL || stop_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *stop_out = false;
    score = burner_gbx_profile_probe_score(
        profile,
        ctx,
        &match_len,
        &match_index,
        &bank_match);
    if (score != INT32_MIN) {
        burner_gbx_probe_lookup_consider(
            ctx,
            profile,
            score,
            match_len,
            match_index,
            bank_match);
    }
    return ESP_OK;
}

static bool burner_gbx_cache_entry_probe_candidate(
    const burner_gbx_cache_entry_t *entry,
    const burner_gbx_probe_lookup_ctx_t *ctx)
{
    size_t candidate_len;

    if (entry == NULL || ctx == NULL || ctx->type == NULL || ctx->id == NULL || ctx->id_len == 0u) {
        return false;
    }
    if (strcasecmp(entry->type, ctx->type) != 0) {
        return false;
    }
    if (ctx->method != NULL &&
        (entry->method_hash == 0u ||
         !burner_gbx_cmd_list_equal(&entry->read_identifier, ctx->method))) {
        return false;
    }

    candidate_len = entry->id_len;
    if (candidate_len == 0u ||
        candidate_len > BURNER_GBX_FLASH_ID_LEN_MAX ||
        !burner_gbx_id_prefix_matches(
            entry->id,
            candidate_len,
            ctx->id,
            ctx->id_len,
            ctx->allow_nonblank_tail)) {
        return false;
    }
    return true;
}

static esp_err_t burner_gbx_find_probe_profile_from_cache(
    burner_gbx_probe_lookup_ctx_t *ctx)
{
    char last_rel_path[BURNER_GBX_CACHE_PATH_LEN] = {0};
    esp_err_t err;

    if (ctx == NULL || ctx->profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_gbx_cache_load();
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t i = 0u; i < s_gbx_cache_entry_count; ++i) {
        const burner_gbx_cache_entry_t *entry = &s_gbx_cache_entries[i];
        burner_gbx_profile_t profile = {0};
        size_t match_len = 0u;
        uint8_t match_index = 0u;
        bool bank_match = false;
        int32_t score;

        if (!burner_gbx_cache_entry_probe_candidate(entry, ctx)) {
            continue;
        }
        if (strncmp(last_rel_path, entry->rel_path, sizeof(last_rel_path)) == 0) {
            continue;
        }
        snprintf(last_rel_path, sizeof(last_rel_path), "%.*s", (int)(sizeof(last_rel_path) - 1u), entry->rel_path);

        err = burner_gbx_parse_profile_file_rel(entry->rel_path, &profile);
        if (err != ESP_OK) {
            continue;
        }

        score = burner_gbx_profile_probe_score(
            &profile,
            ctx,
            &match_len,
            &match_index,
            &bank_match);
        if (score == INT32_MIN) {
            continue;
        }
        burner_gbx_probe_lookup_consider(
            ctx,
            &profile,
            score,
            match_len,
            match_index,
            bank_match);
    }

    if (ctx->best_match_len == 0u) {
        return ESP_ERR_NOT_FOUND;
    }

    burner_gbx_profile_apply_match_name(
        ctx->profile_out,
        ctx->best_match_index,
        ctx->best_bank_match);
    return ESP_OK;
}

esp_err_t burner_gbx_find_agb_profile_for_probe(
    const burner_gbx_cmd_list_t *method,
    const uint8_t *id,
    size_t id_len,
    burner_nor_cmdset_t cmdset,
    bool d0d1_known,
    bool d0d1_swapped,
    uint32_t flash_size,
    uint32_t sector_size,
    const burner_nor_geometry_t *geometry,
    bool cfi_ok,
    burner_gbx_profile_t *profile_out,
    size_t *match_len_out,
    int32_t *score_out,
    bool *ambiguous_out)
{
    burner_gbx_probe_lookup_ctx_t ctx = {
        .type = "AGB",
        .method = method,
        .id = id,
        .id_len = id_len,
        .cmdset = cmdset,
        .d0d1_known = d0d1_known,
        .d0d1_swapped = d0d1_swapped,
        .flash_size = flash_size,
        .sector_size = sector_size,
        .geometry = geometry,
        .geometry_valid = (geometry != NULL) && burner_nor_geometry_is_valid(geometry),
        .cfi_ok = cfi_ok,
        .best_match_len = 0u,
        .best_score = INT32_MIN,
        .best_match_index = 0u,
        .best_bank_match = false,
        .allow_nonblank_tail = true,
        .ambiguous = false,
        .profile_out = profile_out,
    };
    esp_err_t err;

    if (id == NULL || id_len == 0u || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (match_len_out != NULL) {
        *match_len_out = 0u;
    }
    if (score_out != NULL) {
        *score_out = INT32_MIN;
    }
    if (ambiguous_out != NULL) {
        *ambiguous_out = false;
    }
    burner_gbx_profile_clear(profile_out);

    err = burner_gbx_find_probe_profile_from_cache(&ctx);
    if (err != ESP_OK) {
        ctx.best_match_len = 0u;
        ctx.best_score = INT32_MIN;
        ctx.best_match_index = 0u;
        ctx.best_bank_match = false;
        ctx.allow_nonblank_tail = true;
        ctx.ambiguous = false;
        burner_gbx_profile_clear(profile_out);
        err = burner_gbx_visit_agb_profiles(burner_gbx_probe_lookup_visitor, &ctx);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            return err;
        }
        if (ctx.best_match_len == 0u) {
            return ESP_ERR_NOT_FOUND;
        }
        burner_gbx_profile_apply_match_name(
            profile_out,
            ctx.best_match_index,
            ctx.best_bank_match);
    }

    if (match_len_out != NULL) {
        *match_len_out = ctx.best_match_len;
    }
    if (score_out != NULL) {
        *score_out = ctx.best_score;
    }
    if (ambiguous_out != NULL) {
        *ambiguous_out = ctx.ambiguous;
    }
    return ESP_OK;
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
    esp_err_t err;

    if (gba_id == NULL || profile_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = burner_gbx_find_cached_profile_by_id("AGB", gba_id, 8u, profile_out, NULL);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_VERSION) {
        burner_gbx_lookup_ctx_t ctx = {
            .id = gba_id,
            .id_len = 8u,
            .best_match_len = 0u,
            .profile_out = profile_out,
        };

        burner_gbx_profile_clear(profile_out);
        err = burner_gbx_visit_agb_profiles(burner_gbx_lookup_profile_visitor, &ctx);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            return err;
        }
        return (ctx.best_match_len != 0u) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    return err;
}

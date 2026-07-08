#include "app_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#define APP_CONFIG_TAG "app_config"
#define APP_CONFIG_SETTING_DIR mount_point "/.setting"

static char *app_config_trim(char *text)
{
    char *end = NULL;

    if (text == NULL) {
        return NULL;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return text;
}

bool app_config_contains(const app_config_t *config, app_id_t id)
{
    if (config == NULL) {
        return false;
    }
    for (size_t i = 0; i < config->count; ++i) {
        if (config->apps[i] == id) {
            return true;
        }
    }
    return false;
}

static bool app_config_remove(app_config_t *config, app_id_t id)
{
    if (config == NULL) {
        return false;
    }
    for (size_t i = 0; i < config->count; ++i) {
        if (config->apps[i] == id) {
            for (size_t j = i + 1; j < config->count; ++j) {
                config->apps[j - 1] = config->apps[j];
            }
            config->count--;
            if (config->count < APP_REGISTRY_HOME_APP_MAX) {
                config->apps[config->count] = APP_ID_INVALID;
            }
            return true;
        }
    }
    return false;
}

esp_err_t app_config_set_enabled(app_config_t *config, app_id_t id, bool enabled)
{
    if (config == NULL || app_registry_get(id) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!enabled) {
        (void)app_config_remove(config, id);
        return ESP_OK;
    }
    if (app_config_contains(config, id)) {
        return ESP_OK;
    }
    if (config->count >= APP_REGISTRY_HOME_APP_MAX) {
        return ESP_ERR_NO_MEM;
    }
    config->apps[config->count++] = id;
    return ESP_OK;
}

bool app_config_move(app_config_t *config, app_id_t id, int delta)
{
    size_t index;
    size_t target;
    app_id_t tmp;

    if (config == NULL || delta == 0) {
        return false;
    }
    for (index = 0; index < config->count; ++index) {
        if (config->apps[index] == id) {
            break;
        }
    }
    if (index >= config->count) {
        return false;
    }
    if (delta < 0) {
        if (index == 0U) {
            return false;
        }
        target = index - 1U;
    } else {
        if (index + 1U >= config->count) {
            return false;
        }
        target = index + 1U;
    }
    tmp = config->apps[index];
    config->apps[index] = config->apps[target];
    config->apps[target] = tmp;
    return true;
}

static void app_config_add_default(app_config_t *config, app_id_t id)
{
    if (config == NULL || config->count >= APP_REGISTRY_HOME_APP_MAX || app_registry_get(id) == NULL) {
        return;
    }
    config->apps[config->count++] = id;
}

void app_config_defaults(app_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    for (size_t i = 0; i < app_registry_default_count(); ++i) {
        app_config_add_default(config, app_registry_default_app(i));
    }
}

static esp_err_t app_config_ensure_dir(void)
{
    if (mkdir(APP_CONFIG_SETTING_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(APP_CONFIG_TAG, "mkdir %s failed: errno=%d", APP_CONFIG_SETTING_DIR, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *config)
{
    FILE *file = NULL;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = app_config_ensure_dir();
    if (err != ESP_OK) {
        return err;
    }
    file = fopen(APP_CONFIG_PATH, "w");
    if (file == NULL) {
        ESP_LOGW(APP_CONFIG_TAG, "open %s for write failed: errno=%d", APP_CONFIG_PATH, errno);
        return ESP_FAIL;
    }
    fprintf(file, "# MORI app launcher\n");
    for (size_t i = 0; i < config->count && i < APP_REGISTRY_HOME_APP_MAX; ++i) {
        const app_descriptor_t *desc = app_registry_get(config->apps[i]);

        if (desc == NULL) {
            fclose(file);
            return ESP_ERR_INVALID_ARG;
        }
        fprintf(file, "app=%s\n", desc->key);
    }
    fclose(file);
    return ESP_OK;
}

esp_err_t app_config_load(app_config_t *config)
{
    FILE *file = NULL;
    char line[96];
    uint32_t line_no = 0;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    file = fopen(APP_CONFIG_PATH, "r");
    if (file == NULL) {
        esp_err_t save_err;

        app_config_defaults(config);
        save_err = app_config_save(config);
        if (save_err != ESP_OK) {
            return save_err;
        }
        return ESP_OK;
    }

    config->loaded_from_file = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *cursor;
        char *value;
        const app_descriptor_t *desc;

        line_no++;
        cursor = app_config_trim(line);
        if (cursor == NULL || cursor[0] == '\0' || cursor[0] == '#' || cursor[0] == ';') {
            continue;
        }
        if (strncmp(cursor, "app=", 4) != 0) {
            config->had_error = true;
            ESP_LOGW(APP_CONFIG_TAG, "invalid line %lu in %s", (unsigned long)line_no, APP_CONFIG_PATH);
            continue;
        }
        value = app_config_trim(cursor + 4);
        desc = app_registry_find_key(value);
        if (desc == NULL) {
            config->had_error = true;
            config->had_unknown = true;
            ESP_LOGW(APP_CONFIG_TAG, "unknown app '%s' in %s", value, APP_CONFIG_PATH);
            continue;
        }
        if (app_config_contains(config, desc->id)) {
            config->had_error = true;
            config->had_duplicate = true;
            ESP_LOGW(APP_CONFIG_TAG, "duplicate app '%s' in %s", value, APP_CONFIG_PATH);
            continue;
        }
        if (config->count >= APP_REGISTRY_HOME_APP_MAX) {
            config->had_error = true;
            config->had_overflow = true;
            ESP_LOGW(APP_CONFIG_TAG, "too many apps in %s", APP_CONFIG_PATH);
            continue;
        }
        config->apps[config->count++] = desc->id;
    }
    fclose(file);
    return config->had_error ? ESP_ERR_INVALID_STATE : ESP_OK;
}

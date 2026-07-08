#include "app_registry.h"

#include <string.h>

static const app_descriptor_t s_app_descriptors[] = {
    {
        .id = APP_ID_READER,
        .key = "reader",
        .title = "Reader",
        .hint = "read TF files",
        .symbol = "READ",
        .default_enabled = true,
        .user_visible = true,
    },
    {
        .id = APP_ID_MUSIC,
        .key = "music",
        .title = "Music",
        .hint = "audio player",
        .symbol = "MUS",
        .default_enabled = true,
        .user_visible = true,
    },
};

static const app_id_t s_default_apps[] = {
    APP_ID_READER,
    APP_ID_MUSIC,
};

const app_descriptor_t *app_registry_get(app_id_t id)
{
    for (size_t i = 0; i < app_registry_count(); ++i) {
        if (s_app_descriptors[i].id == id) {
            return &s_app_descriptors[i];
        }
    }
    return NULL;
}

const app_descriptor_t *app_registry_at(size_t index)
{
    if (index >= app_registry_count()) {
        return NULL;
    }
    return &s_app_descriptors[index];
}

const app_descriptor_t *app_registry_find_key(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < app_registry_count(); ++i) {
        if (strcmp(s_app_descriptors[i].key, key) == 0) {
            return &s_app_descriptors[i];
        }
    }
    return NULL;
}

size_t app_registry_count(void)
{
    return sizeof(s_app_descriptors) / sizeof(s_app_descriptors[0]);
}

app_id_t app_registry_default_app(size_t index)
{
    if (index >= app_registry_default_count()) {
        return APP_ID_INVALID;
    }
    return s_default_apps[index];
}

size_t app_registry_default_count(void)
{
    return sizeof(s_default_apps) / sizeof(s_default_apps[0]);
}

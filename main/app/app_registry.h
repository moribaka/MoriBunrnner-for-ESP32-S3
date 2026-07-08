#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_REGISTRY_HOME_FIXED_COUNT 3U
#define APP_REGISTRY_HOME_APP_MAX 5U
#define APP_REGISTRY_HOME_ICON_MAX (APP_REGISTRY_HOME_FIXED_COUNT + APP_REGISTRY_HOME_APP_MAX)

typedef enum {
    APP_ID_READER = 0,
    APP_ID_MUSIC,
    APP_ID_COUNT,
    APP_ID_INVALID = 0xFF,
} app_id_t;

typedef struct {
    app_id_t id;
    const char *key;
    const char *title;
    const char *hint;
    const char *symbol;
    bool default_enabled;
    bool user_visible;
} app_descriptor_t;

const app_descriptor_t *app_registry_get(app_id_t id);
const app_descriptor_t *app_registry_at(size_t index);
const app_descriptor_t *app_registry_find_key(const char *key);
size_t app_registry_count(void);
app_id_t app_registry_default_app(size_t index);
size_t app_registry_default_count(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_REGISTRY_H */

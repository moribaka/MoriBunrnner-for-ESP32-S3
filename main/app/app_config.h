#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "app_registry.h"
#include "esp_err.h"
#include "file_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_PATH mount_point "/.setting/apps.ini"

typedef struct {
    app_id_t apps[APP_REGISTRY_HOME_APP_MAX];
    size_t count;
    bool loaded_from_file;
    bool had_error;
    bool had_unknown;
    bool had_duplicate;
    bool had_overflow;
} app_config_t;

void app_config_defaults(app_config_t *config);
esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
bool app_config_contains(const app_config_t *config, app_id_t id);
esp_err_t app_config_set_enabled(app_config_t *config, app_id_t id, bool enabled);
bool app_config_move(app_config_t *config, app_id_t id, int delta);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */

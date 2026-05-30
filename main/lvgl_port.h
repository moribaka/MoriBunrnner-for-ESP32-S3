#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t lvgl_port_init(void);
void lvgl_port_mark_activity(void);
bool lvgl_port_is_idle_dimmed(void);
void lvgl_port_set_idle_dim_suspended(bool suspended);

#endif /* LVGL_PORT_H */

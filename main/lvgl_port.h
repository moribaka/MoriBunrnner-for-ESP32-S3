#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t lvgl_port_init(void);
void lvgl_port_mark_activity(void);
bool lvgl_port_is_idle_dimmed(void);
void lvgl_port_set_idle_dim_suspended(bool suspended);
void lvgl_port_set_idle_dim_timeout_minutes(uint16_t minutes);

#endif /* LVGL_PORT_H */
